/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2021 Blender Foundation.
 */

/** \file
 * \ingroup eevee
 *
 * Depth of field post process effect.
 *
 * There are 2 methods to achieve this effect.
 * - The first uses projection matrix offsetting and sample accumulation to give
 * reference quality depth of field. But this needs many samples to hide the
 * under-sampling.
 * - The second one is a post-processing based one. It follows the
 * implementation described in the presentation
 * "Life of a Bokeh - Siggraph 2018" from Guillaume Abadie.
 * There are some difference with our actual implementation that prioritize quality.
 */

#pragma once

#include "eevee_shader_shared.hh"

namespace blender::eevee {

class Instance;

/* -------------------------------------------------------------------- */
/** \name Depth of field
 * \{ */

struct DepthOfFieldBuffer {
  /**
   * Per view history texture for stabilize pass.
   * Swapped with stabilize_output_tx_ in order to reuse the previous history during DoF
   * processing.
   * Note this should be private as its inner working only concerns the Depth Of Field
   * implementation. The view itself should not touch it.
   */
  Texture stabilize_history_tx_ = {"dof_taa"};
};

class DepthOfField {
 private:
  class Instance &inst_;

  /** Samplers */
  static constexpr eGPUSamplerState gather_bilinear = GPU_SAMPLER_MIPMAP | GPU_SAMPLER_FILTER;
  static constexpr eGPUSamplerState gather_nearest = GPU_SAMPLER_MIPMAP;

  /** Input/Output texture references. */
  GPUTexture *input_color_tx_ = nullptr;
  GPUTexture *output_color_tx_ = nullptr;

  /** Bokeh LUT precompute pass. */
  TextureFromPool bokeh_gather_lut_tx_ = {"dof_bokeh_gather_lut"};
  TextureFromPool bokeh_resolve_lut_tx_ = {"dof_bokeh_resolve_lut"};
  TextureFromPool bokeh_scatter_lut_tx_ = {"dof_bokeh_scatter_lut"};
  DRWPass *bokeh_lut_ps_ = nullptr;

  /** Outputs half-resolution color and Circle Of Confusion. */
  TextureFromPool setup_coc_tx_ = {"dof_setup_coc"};
  TextureFromPool setup_color_tx_ = {"dof_setup_color"};
  int3 dispatch_setup_size_ = int3(-1);
  DRWPass *setup_ps_ = nullptr;

  /** Allocated because we need mip chain. Which isn't supported by TextureFromPool. */
  Texture reduced_coc_tx_ = {"dof_reduced_coc"};
  Texture reduced_color_tx_ = {"dof_reduced_color"};

  /** Stabilization (flicker attenuation) of Color and CoC output of the setup pass. */
  TextureFromPool stabilize_output_tx_ = {"dof_taa"};
  GPUTexture *stabilize_input_ = nullptr;
  bool1 stabilize_valid_history_ = false;
  int3 dispatch_stabilize_size_ = int3(-1);
  DRWPass *stabilize_ps_ = nullptr;

  /** 1/4th res color buffer used to speedup the local contrast test in the first reduce pass. */
  TextureFromPool downsample_tx_ = {"dof_downsample"};
  int3 dispatch_downsample_size_ = int3(-1);
  DRWPass *downsample_ps_ = nullptr;

  /** Create mip-mapped color & COC textures for gather passes as well as scatter rect list. */
  DepthOfFieldScatterListBuf scatter_fg_list_buf_;
  DepthOfFieldScatterListBuf scatter_bg_list_buf_;
  DrawIndirectBuf scatter_fg_indirect_buf_;
  DrawIndirectBuf scatter_bg_indirect_buf_;
  int3 dispatch_reduce_size_ = int3(-1);
  DRWPass *reduce_ps_ = nullptr;

  /** Outputs min & max COC in each 8x8 half res pixel tiles (so 1/16th of full resolution). */
  SwapChain<TextureFromPool, 2> tiles_fg_tx_;
  SwapChain<TextureFromPool, 2> tiles_bg_tx_;
  int3 dispatch_tiles_flatten_size_ = int3(-1);
  DRWPass *tiles_flatten_ps_ = nullptr;

  /** Dilates the min & max CoCs to cover maximum COC values. */
  int tiles_dilate_ring_count_ = -1;
  int tiles_dilate_ring_width_mul_ = -1;
  int3 dispatch_tiles_dilate_size_ = int3(-1);
  DRWPass *tiles_dilate_minmax_ps_ = nullptr;
  DRWPass *tiles_dilate_minabs_ps_ = nullptr;

  /** Gather convolution for low intensity pixels and low contrast areas. */
  SwapChain<TextureFromPool, 2> color_bg_tx_;
  SwapChain<TextureFromPool, 2> color_fg_tx_;
  SwapChain<TextureFromPool, 2> weight_bg_tx_;
  SwapChain<TextureFromPool, 2> weight_fg_tx_;
  TextureFromPool occlusion_tx_ = {"dof_occlusion"};
  int3 dispatch_gather_size_ = int3(-1);
  DRWPass *gather_fg_ps_ = nullptr;
  DRWPass *gather_bg_ps_ = nullptr;

  /** Hole-fill convolution: Gather pass meant to fill areas of foreground dis-occlusion. */
  TextureFromPool hole_fill_color_tx_ = {"dof_color_hole_fill"};
  TextureFromPool hole_fill_weight_tx_ = {"dof_weight_hole_fill"};
  DRWPass *hole_fill_ps_ = nullptr;

  /** Small Filter pass to reduce noise out of gather passes. */
  int3 dispatch_filter_size_ = int3(-1);
  DRWPass *filter_fg_ps_ = nullptr;
  DRWPass *filter_bg_ps_ = nullptr;

  /** Scatter convolution: A quad is emitted for every 4 bright enough half pixels. */
  Framebuffer scatter_fg_fb_ = {"dof_scatter_fg"};
  Framebuffer scatter_bg_fb_ = {"dof_scatter_bg"};
  DRWPass *scatter_fg_ps_ = nullptr;
  DRWPass *scatter_bg_ps_ = nullptr;

  /** Recombine the results and also perform a slight out of focus gather. */
  GPUTexture *resolve_stable_color_tx_ = nullptr;
  int3 dispatch_resolve_size_ = int3(-1);
  DRWPass *resolve_ps_ = nullptr;

  DepthOfFieldDataBuf data_;

  /** Scene settings that are immutable. */
  float user_overblur_;
  float fx_max_coc_;
  /** Use jittered depth of field where we randomize camera location. */
  bool do_jitter_;

  /** Circle of Confusion radius for FX DoF passes. Is in view X direction in [0..1] range. */
  float fx_radius_;
  /** Circle of Confusion radius for jittered DoF. Is in view X direction in [0..1] range. */
  float jitter_radius_;
  /** Focus distance in view space. */
  float focus_distance_;
  /** Extent of the input buffer. */
  int2 extent_;

 public:
  DepthOfField(Instance &inst) : inst_(inst){};
  ~DepthOfField(){};

  void init();

  void sync();

  /**
   * Apply Depth Of Field jittering to the view and projection matrices..
   */
  void jitter_apply(float4x4 &winmat, float4x4 &viewmat);

  /**
   * Will swap input and output texture if rendering happens. The actual output of this function
   * is in input_tx.
   */
  void render(GPUTexture **input_tx, GPUTexture **output_tx, DepthOfFieldBuffer &dof_buffer);

  bool postfx_enabled() const
  {
    return fx_radius_ > 0.0f;
  }

 private:
  void bokeh_lut_pass_sync();
  void setup_pass_sync();
  void stabilize_pass_sync();
  void downsample_pass_sync();
  void reduce_pass_sync();
  void tiles_flatten_pass_sync();
  void tiles_dilate_pass_sync();
  void gather_pass_sync();
  void filter_pass_sync();
  void scatter_pass_sync();
  void hole_fill_pass_sync();
  void resolve_pass_sync();

  void update_sample_table();
};

/** \} */

}  // namespace blender::eevee
