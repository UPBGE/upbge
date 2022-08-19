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

#include "DRW_render.h"

#include "BKE_camera.h"
#include "DNA_camera_types.h"

#include "GPU_platform.h"
#include "GPU_texture.h"
#include "GPU_uniform_buffer.h"

#include "eevee_camera.hh"
#include "eevee_instance.hh"
#include "eevee_sampling.hh"
#include "eevee_shader.hh"
#include "eevee_shader_shared.hh"

#include "eevee_depth_of_field.hh"

namespace blender::eevee {

/* -------------------------------------------------------------------- */
/** \name Depth of field
 * \{ */

void DepthOfField::init()
{
  const SceneEEVEE &sce_eevee = inst_.scene->eevee;
  const Object *camera_object_eval = inst_.camera_eval_object;
  const ::Camera *camera = (camera_object_eval) ?
                               reinterpret_cast<const ::Camera *>(camera_object_eval->data) :
                               nullptr;
  if (camera == nullptr) {
    /* Set to invalid value for update detection */
    data_.scatter_color_threshold = -1.0f;
    return;
  }
  /* Reminder: These are parameters not interpolated by motion blur. */
  int update = 0;
  int sce_flag = sce_eevee.flag;
  update += assign_if_different(do_jitter_, (sce_flag & SCE_EEVEE_DOF_JITTER) != 0);
  update += assign_if_different(user_overblur_, sce_eevee.bokeh_overblur / 100.0f);
  update += assign_if_different(fx_max_coc_, sce_eevee.bokeh_max_size);
  update += assign_if_different(data_.scatter_color_threshold, sce_eevee.bokeh_threshold);
  update += assign_if_different(data_.scatter_neighbor_max_color, sce_eevee.bokeh_neighbor_max);
  update += assign_if_different(data_.bokeh_blades, float(camera->dof.aperture_blades));
  if (update > 0) {
    inst_.sampling.reset();
  }
}

void DepthOfField::sync()
{
  const Camera &camera = inst_.camera;
  const Object *camera_object_eval = inst_.camera_eval_object;
  const ::Camera *camera_data = (camera_object_eval) ?
                                    reinterpret_cast<const ::Camera *>(camera_object_eval->data) :
                                    nullptr;

  int update = 0;

  if (camera_data == nullptr || (camera_data->dof.flag & CAM_DOF_ENABLED) == 0) {
    update += assign_if_different(jitter_radius_, 0.0f);
    update += assign_if_different(fx_radius_, 0.0f);
    if (update > 0) {
      inst_.sampling.reset();
    }
    return;
  }

  float2 anisotropic_scale = {clamp_f(1.0f / camera_data->dof.aperture_ratio, 1e-5f, 1.0f),
                              clamp_f(camera_data->dof.aperture_ratio, 1e-5f, 1.0f)};
  update += assign_if_different(data_.bokeh_anisotropic_scale, anisotropic_scale);
  update += assign_if_different(data_.bokeh_rotation, camera_data->dof.aperture_rotation);
  update += assign_if_different(focus_distance_,
                                BKE_camera_object_dof_distance(camera_object_eval));
  data_.bokeh_anisotropic_scale_inv = 1.0f / data_.bokeh_anisotropic_scale;

  float fstop = max_ff(camera_data->dof.aperture_fstop, 1e-5f);

  if (update) {
    inst_.sampling.reset();
  }

  float aperture = 1.0f / (2.0f * fstop);
  if (camera.is_perspective()) {
    aperture *= camera_data->lens * 1e-3f;
  }

  if (camera.is_orthographic()) {
    /* FIXME: Why is this needed? Some kind of implicit unit conversion? */
    aperture *= 0.04f;
    /* Really strange behavior from Cycles but replicating. */
    focus_distance_ += camera.data_get().clip_near;
  }

  if (camera.is_panoramic()) {
    /* FIXME: Eyeballed. */
    aperture *= 0.185f;
  }

  if (camera_data->dof.aperture_ratio < 1.0) {
    /* If ratio is scaling the bokeh outwards, we scale the aperture so that
     * the gather kernel size will encompass the maximum axis. */
    aperture /= max_ff(camera_data->dof.aperture_ratio, 1e-5f);
  }

  float jitter_radius, fx_radius;

  /* Balance blur radius between fx dof and jitter dof. */
  if (do_jitter_ && (inst_.sampling.dof_ring_count_get() > 0) && !camera.is_panoramic() &&
      !inst_.is_viewport()) {
    /* Compute a minimal overblur radius to fill the gaps between the samples.
     * This is just the simplified form of dividing the area of the bokeh by
     * the number of samples. */
    float minimal_overblur = 1.0f / sqrtf(inst_.sampling.dof_sample_count_get());

    fx_radius = (minimal_overblur + user_overblur_) * aperture;
    /* Avoid dilating the shape. Over-blur only soften. */
    jitter_radius = max_ff(0.0f, aperture - fx_radius);
  }
  else {
    jitter_radius = 0.0f;
    fx_radius = aperture;
  }

  /* Disable post fx if result wouldn't be noticeable. */
  if (fx_max_coc_ <= 0.5f) {
    fx_radius = 0.0f;
  }

  update += assign_if_different(jitter_radius_, jitter_radius);
  update += assign_if_different(fx_radius_, fx_radius);
  if (update > 0) {
    inst_.sampling.reset();
  }

  if (fx_radius_ == 0.0f) {
    return;
  }

  /* TODO(fclem): Once we render into multiple view, we will need to use the maximum resolution. */
  int2 max_render_res = inst_.film.render_extent_get();
  int2 half_res = math::divide_ceil(max_render_res, int2(2));
  int2 reduce_size = math::ceil_to_multiple(half_res, int2(DOF_REDUCE_GROUP_SIZE));

  data_.gather_uv_fac = 1.0f / float2(reduce_size);

  /* Now that we know the maximum render resolution of every view, using depth of field, allocate
   * the reduced buffers. Color needs to be signed format here. See note in shader for
   * explanation. Do not use texture pool because of needs mipmaps. */
  reduced_color_tx_.ensure_2d(GPU_RGBA16F, reduce_size, nullptr, DOF_MIP_COUNT);
  reduced_coc_tx_.ensure_2d(GPU_R16F, reduce_size, nullptr, DOF_MIP_COUNT);
  reduced_color_tx_.ensure_mip_views();
  reduced_coc_tx_.ensure_mip_views();

  /* Resize the scatter list to contain enough entry to cover half the screen with sprites (which
   * is unlikely due to local contrast test). */
  data_.scatter_max_rect = (reduced_color_tx_.pixel_count() / 4) / 2;
  scatter_fg_list_buf_.resize(data_.scatter_max_rect);
  scatter_bg_list_buf_.resize(data_.scatter_max_rect);

  bokeh_lut_pass_sync();
  setup_pass_sync();
  stabilize_pass_sync();
  downsample_pass_sync();
  reduce_pass_sync();
  tiles_flatten_pass_sync();
  tiles_dilate_pass_sync();
  gather_pass_sync();
  filter_pass_sync();
  scatter_pass_sync();
  hole_fill_pass_sync();
  resolve_pass_sync();
}

void DepthOfField::jitter_apply(float4x4 &winmat, float4x4 &viewmat)
{
  if (jitter_radius_ == 0.0f) {
    return;
  }

  float radius, theta;
  inst_.sampling.dof_disk_sample_get(&radius, &theta);

  if (data_.bokeh_blades >= 3.0f) {
    theta = circle_to_polygon_angle(data_.bokeh_blades, theta);
    radius *= circle_to_polygon_radius(data_.bokeh_blades, theta);
  }
  radius *= jitter_radius_;
  theta += data_.bokeh_rotation;

  /* Sample in View Space. */
  float2 sample = float2(radius * cosf(theta), radius * sinf(theta));
  sample *= data_.bokeh_anisotropic_scale;
  /* Convert to NDC Space. */
  float3 jitter = float3(UNPACK2(sample), -focus_distance_);
  float3 center = float3(0.0f, 0.0f, -focus_distance_);
  mul_project_m4_v3(winmat.ptr(), jitter);
  mul_project_m4_v3(winmat.ptr(), center);

  const bool is_ortho = (winmat[2][3] != -1.0f);
  if (is_ortho) {
    sample *= focus_distance_;
  }
  /* Translate origin. */
  sub_v2_v2(viewmat[3], sample);
  /* Skew winmat Z axis. */
  add_v2_v2(winmat[2], center - jitter);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Passes setup.
 * \{ */

void DepthOfField::bokeh_lut_pass_sync()
{
  const bool has_anisotropy = data_.bokeh_anisotropic_scale != float2(1.0f);
  if (!has_anisotropy && (data_.bokeh_blades == 0.0)) {
    /* No need for LUTs in these cases. */
    bokeh_lut_ps_ = nullptr;
    return;
  }

  /* Precompute bokeh texture. */
  bokeh_lut_ps_ = DRW_pass_create("Dof.bokeh_lut_ps_", DRW_STATE_NO_DRAW);
  GPUShader *sh = inst_.shaders.static_shader_get(DOF_BOKEH_LUT);
  DRWShadingGroup *grp = DRW_shgroup_create(sh, bokeh_lut_ps_);
  DRW_shgroup_uniform_block(grp, "dof_buf", data_);
  DRW_shgroup_uniform_image_ref(grp, "out_gather_lut_img", &bokeh_gather_lut_tx_);
  DRW_shgroup_uniform_image_ref(grp, "out_scatter_lut_img", &bokeh_scatter_lut_tx_);
  DRW_shgroup_uniform_image_ref(grp, "out_resolve_lut_img", &bokeh_resolve_lut_tx_);
  DRW_shgroup_call_compute(grp, 1, 1, 1);
}

void DepthOfField::setup_pass_sync()
{
  RenderBuffers &render_buffers = inst_.render_buffers;

  setup_ps_ = DRW_pass_create("Dof.setup_ps_", DRW_STATE_NO_DRAW);
  GPUShader *sh = inst_.shaders.static_shader_get(DOF_SETUP);
  DRWShadingGroup *grp = DRW_shgroup_create(sh, setup_ps_);
  DRW_shgroup_uniform_texture_ref_ex(grp, "color_tx", &input_color_tx_, no_filter);
  DRW_shgroup_uniform_texture_ref_ex(grp, "depth_tx", &render_buffers.depth_tx, no_filter);
  DRW_shgroup_uniform_block(grp, "dof_buf", data_);
  DRW_shgroup_uniform_image_ref(grp, "out_color_img", &setup_color_tx_);
  DRW_shgroup_uniform_image_ref(grp, "out_coc_img", &setup_coc_tx_);
  DRW_shgroup_call_compute_ref(grp, dispatch_setup_size_);
  DRW_shgroup_barrier(grp, GPU_BARRIER_TEXTURE_FETCH);
}

void DepthOfField::stabilize_pass_sync()
{
  RenderBuffers &render_buffers = inst_.render_buffers;
  VelocityModule &velocity = inst_.velocity;

  stabilize_ps_ = DRW_pass_create("Dof.stabilize_ps_", DRW_STATE_NO_DRAW);
  GPUShader *sh = inst_.shaders.static_shader_get(DOF_STABILIZE);
  DRWShadingGroup *grp = DRW_shgroup_create(sh, stabilize_ps_);
  DRW_shgroup_uniform_block_ref(grp, "camera_prev", &(*velocity.camera_steps[STEP_PREVIOUS]));
  DRW_shgroup_uniform_block_ref(grp, "camera_curr", &(*velocity.camera_steps[STEP_CURRENT]));
  /* This is only for temporal stability. The next step is not needed. */
  DRW_shgroup_uniform_block_ref(grp, "camera_next", &(*velocity.camera_steps[STEP_PREVIOUS]));
  DRW_shgroup_uniform_texture_ref_ex(grp, "coc_tx", &setup_coc_tx_, no_filter);
  DRW_shgroup_uniform_texture_ref_ex(grp, "color_tx", &setup_color_tx_, no_filter);
  DRW_shgroup_uniform_texture_ref_ex(grp, "velocity_tx", &render_buffers.vector_tx, no_filter);
  DRW_shgroup_uniform_texture_ref_ex(grp, "in_history_tx", &stabilize_input_, with_filter);
  DRW_shgroup_uniform_texture_ref_ex(grp, "depth_tx", &render_buffers.depth_tx, no_filter);
  DRW_shgroup_uniform_bool(grp, "use_history", &stabilize_valid_history_, 1);
  DRW_shgroup_uniform_block(grp, "dof_buf", data_);
  DRW_shgroup_uniform_image(grp, "out_coc_img", reduced_coc_tx_.mip_view(0));
  DRW_shgroup_uniform_image(grp, "out_color_img", reduced_color_tx_.mip_view(0));
  DRW_shgroup_uniform_image_ref(grp, "out_history_img", &stabilize_output_tx_);
  DRW_shgroup_call_compute_ref(grp, dispatch_stabilize_size_);
  DRW_shgroup_barrier(grp, GPU_BARRIER_TEXTURE_FETCH | GPU_BARRIER_SHADER_IMAGE_ACCESS);
}

void DepthOfField::downsample_pass_sync()
{
  downsample_ps_ = DRW_pass_create("Dof.downsample_ps_", DRW_STATE_NO_DRAW);
  GPUShader *sh = inst_.shaders.static_shader_get(DOF_DOWNSAMPLE);
  DRWShadingGroup *grp = DRW_shgroup_create(sh, downsample_ps_);
  DRW_shgroup_uniform_texture_ex(grp, "color_tx", reduced_color_tx_.mip_view(0), no_filter);
  DRW_shgroup_uniform_texture_ex(grp, "coc_tx", reduced_coc_tx_.mip_view(0), no_filter);
  DRW_shgroup_uniform_image_ref(grp, "out_color_img", &downsample_tx_);
  DRW_shgroup_call_compute_ref(grp, dispatch_downsample_size_);
  DRW_shgroup_barrier(grp, GPU_BARRIER_TEXTURE_FETCH);
}

void DepthOfField::reduce_pass_sync()
{
  reduce_ps_ = DRW_pass_create("Dof.reduce_ps_", DRW_STATE_NO_DRAW);
  GPUShader *sh = inst_.shaders.static_shader_get(DOF_REDUCE);
  DRWShadingGroup *grp = DRW_shgroup_create(sh, reduce_ps_);
  DRW_shgroup_uniform_block(grp, "dof_buf", data_);
  DRW_shgroup_uniform_texture_ref_ex(grp, "downsample_tx", &downsample_tx_, no_filter);
  DRW_shgroup_storage_block(grp, "scatter_fg_list_buf", scatter_fg_list_buf_);
  DRW_shgroup_storage_block(grp, "scatter_bg_list_buf", scatter_bg_list_buf_);
  DRW_shgroup_storage_block(grp, "scatter_fg_indirect_buf", scatter_fg_indirect_buf_);
  DRW_shgroup_storage_block(grp, "scatter_bg_indirect_buf", scatter_bg_indirect_buf_);
  DRW_shgroup_uniform_image(grp, "inout_color_lod0_img", reduced_color_tx_.mip_view(0));
  DRW_shgroup_uniform_image(grp, "out_color_lod1_img", reduced_color_tx_.mip_view(1));
  DRW_shgroup_uniform_image(grp, "out_color_lod2_img", reduced_color_tx_.mip_view(2));
  DRW_shgroup_uniform_image(grp, "out_color_lod3_img", reduced_color_tx_.mip_view(3));
  DRW_shgroup_uniform_image(grp, "in_coc_lod0_img", reduced_coc_tx_.mip_view(0));
  DRW_shgroup_uniform_image(grp, "out_coc_lod1_img", reduced_coc_tx_.mip_view(1));
  DRW_shgroup_uniform_image(grp, "out_coc_lod2_img", reduced_coc_tx_.mip_view(2));
  DRW_shgroup_uniform_image(grp, "out_coc_lod3_img", reduced_coc_tx_.mip_view(3));
  DRW_shgroup_call_compute_ref(grp, dispatch_reduce_size_);
  /* NOTE: Command buffer barrier is done automatically by the GPU backend. */
  DRW_shgroup_barrier(grp, GPU_BARRIER_TEXTURE_FETCH | GPU_BARRIER_SHADER_STORAGE);
}

void DepthOfField::tiles_flatten_pass_sync()
{
  tiles_flatten_ps_ = DRW_pass_create("Dof.tiles_flatten_ps_", DRW_STATE_NO_DRAW);
  GPUShader *sh = inst_.shaders.static_shader_get(DOF_TILES_FLATTEN);
  DRWShadingGroup *grp = DRW_shgroup_create(sh, tiles_flatten_ps_);
  /* NOTE(fclem): We should use the reduced_coc_tx_ as it is stable, but we need the slight focus
   * flag from the setup pass. A better way would be to do the brute-force in focus gather without
   * this. */
  DRW_shgroup_uniform_texture_ref_ex(grp, "coc_tx", &setup_coc_tx_, no_filter);
  DRW_shgroup_uniform_image_ref(grp, "out_tiles_fg_img", &tiles_fg_tx_.current());
  DRW_shgroup_uniform_image_ref(grp, "out_tiles_bg_img", &tiles_bg_tx_.current());
  DRW_shgroup_call_compute_ref(grp, dispatch_tiles_flatten_size_);
  DRW_shgroup_barrier(grp, GPU_BARRIER_SHADER_IMAGE_ACCESS);
}

void DepthOfField::tiles_dilate_pass_sync()
{
  tiles_dilate_minmax_ps_ = DRW_pass_create("Dof.tiles_dilate_minmax_ps_", DRW_STATE_NO_DRAW);
  tiles_dilate_minabs_ps_ = DRW_pass_create("Dof.tiles_dilate_minabs_ps_", DRW_STATE_NO_DRAW);
  for (int pass = 0; pass < 2; pass++) {
    DRWPass *drw_pass = (pass == 0) ? tiles_dilate_minmax_ps_ : tiles_dilate_minabs_ps_;
    GPUShader *sh = inst_.shaders.static_shader_get((pass == 0) ? DOF_TILES_DILATE_MINMAX :
                                                                  DOF_TILES_DILATE_MINABS);
    DRWShadingGroup *grp = DRW_shgroup_create(sh, drw_pass);
    DRW_shgroup_uniform_image_ref(grp, "in_tiles_fg_img", &tiles_fg_tx_.previous());
    DRW_shgroup_uniform_image_ref(grp, "in_tiles_bg_img", &tiles_bg_tx_.previous());
    DRW_shgroup_uniform_image_ref(grp, "out_tiles_fg_img", &tiles_fg_tx_.current());
    DRW_shgroup_uniform_image_ref(grp, "out_tiles_bg_img", &tiles_bg_tx_.current());
    DRW_shgroup_uniform_int(grp, "ring_count", &tiles_dilate_ring_count_, 1);
    DRW_shgroup_uniform_int(grp, "ring_width_multiplier", &tiles_dilate_ring_width_mul_, 1);
    DRW_shgroup_call_compute_ref(grp, dispatch_tiles_dilate_size_);
    DRW_shgroup_barrier(grp, GPU_BARRIER_SHADER_IMAGE_ACCESS);
  }
}

void DepthOfField::gather_pass_sync()
{
  gather_fg_ps_ = DRW_pass_create("Dof.gather_fg_ps_", DRW_STATE_NO_DRAW);
  gather_bg_ps_ = DRW_pass_create("Dof.gather_bg_ps_", DRW_STATE_NO_DRAW);
  for (int pass = 0; pass < 2; pass++) {
    SwapChain<TextureFromPool, 2> &color_chain = (pass == 0) ? color_fg_tx_ : color_bg_tx_;
    SwapChain<TextureFromPool, 2> &weight_chain = (pass == 0) ? weight_fg_tx_ : weight_bg_tx_;
    bool use_lut = bokeh_lut_ps_ != nullptr;
    eShaderType sh_type = (pass == 0) ?
                              (use_lut ? DOF_GATHER_FOREGROUND_LUT : DOF_GATHER_FOREGROUND) :
                              (use_lut ? DOF_GATHER_BACKGROUND_LUT : DOF_GATHER_BACKGROUND);
    GPUShader *sh = inst_.shaders.static_shader_get(sh_type);
    DRWShadingGroup *grp = DRW_shgroup_create(sh, (pass == 0) ? gather_fg_ps_ : gather_bg_ps_);
    inst_.sampling.bind_resources(grp);
    DRW_shgroup_uniform_block(grp, "dof_buf", data_);
    DRW_shgroup_uniform_texture_ex(grp, "color_bilinear_tx", reduced_color_tx_, gather_bilinear);
    DRW_shgroup_uniform_texture_ex(grp, "color_tx", reduced_color_tx_, gather_nearest);
    DRW_shgroup_uniform_texture_ex(grp, "coc_tx", reduced_coc_tx_, gather_nearest);
    DRW_shgroup_uniform_image_ref(grp, "in_tiles_fg_img", &tiles_fg_tx_.current());
    DRW_shgroup_uniform_image_ref(grp, "in_tiles_bg_img", &tiles_bg_tx_.current());
    DRW_shgroup_uniform_image_ref(grp, "out_color_img", &color_chain.current());
    DRW_shgroup_uniform_image_ref(grp, "out_weight_img", &weight_chain.current());
    DRW_shgroup_uniform_image_ref(grp, "out_occlusion_img", &occlusion_tx_);
    DRW_shgroup_uniform_texture_ref(grp, "bokeh_lut_tx", &bokeh_gather_lut_tx_);
    DRW_shgroup_call_compute_ref(grp, dispatch_gather_size_);
    DRW_shgroup_barrier(grp, GPU_BARRIER_TEXTURE_FETCH);
  }
}

void DepthOfField::filter_pass_sync()
{
  filter_fg_ps_ = DRW_pass_create("Dof.filter_fg_ps_", DRW_STATE_NO_DRAW);
  filter_bg_ps_ = DRW_pass_create("Dof.filter_bg_ps_", DRW_STATE_NO_DRAW);
  for (int pass = 0; pass < 2; pass++) {
    SwapChain<TextureFromPool, 2> &color_chain = (pass == 0) ? color_fg_tx_ : color_bg_tx_;
    SwapChain<TextureFromPool, 2> &weight_chain = (pass == 0) ? weight_fg_tx_ : weight_bg_tx_;
    GPUShader *sh = inst_.shaders.static_shader_get(DOF_FILTER);
    DRWShadingGroup *grp = DRW_shgroup_create(sh, (pass == 0) ? filter_fg_ps_ : filter_bg_ps_);
    DRW_shgroup_uniform_texture_ref(grp, "color_tx", &color_chain.previous());
    DRW_shgroup_uniform_texture_ref(grp, "weight_tx", &weight_chain.previous());
    DRW_shgroup_uniform_image_ref(grp, "out_color_img", &color_chain.current());
    DRW_shgroup_uniform_image_ref(grp, "out_weight_img", &weight_chain.current());
    DRW_shgroup_call_compute_ref(grp, dispatch_filter_size_);
    DRW_shgroup_barrier(grp, GPU_BARRIER_TEXTURE_FETCH);
  }
}

void DepthOfField::scatter_pass_sync()
{
  DRWState state = DRW_STATE_WRITE_COLOR | DRW_STATE_BLEND_ADD_FULL;
  scatter_fg_ps_ = DRW_pass_create("Dof.scatter_fg_ps_", state);
  scatter_bg_ps_ = DRW_pass_create("Dof.scatter_bg_ps_", state);
  for (int pass = 0; pass < 2; pass++) {
    GPUStorageBuf *scatter_buf = (pass == 0) ? scatter_fg_indirect_buf_ : scatter_bg_indirect_buf_;
    GPUStorageBuf *rect_list_buf = (pass == 0) ? scatter_fg_list_buf_ : scatter_bg_list_buf_;

    GPUShader *sh = inst_.shaders.static_shader_get(DOF_SCATTER);
    DRWShadingGroup *grp = DRW_shgroup_create(sh, (pass == 0) ? scatter_fg_ps_ : scatter_bg_ps_);
    DRW_shgroup_uniform_bool_copy(grp, "use_bokeh_lut", bokeh_lut_ps_ != nullptr);
    DRW_shgroup_storage_block(grp, "scatter_list_buf", rect_list_buf);
    DRW_shgroup_uniform_texture_ref(grp, "bokeh_lut_tx", &bokeh_scatter_lut_tx_);
    DRW_shgroup_uniform_texture_ref(grp, "occlusion_tx", &occlusion_tx_);
    DRW_shgroup_call_procedural_indirect(grp, GPU_PRIM_TRI_STRIP, nullptr, scatter_buf);
    if (pass == 0) {
      /* Avoid background gather pass writing to the occlusion_tx mid pass. */
      DRW_shgroup_barrier(grp, GPU_BARRIER_SHADER_IMAGE_ACCESS);
    }
  }
}

void DepthOfField::hole_fill_pass_sync()
{
  hole_fill_ps_ = DRW_pass_create("Dof.hole_fill_ps_", DRW_STATE_NO_DRAW);
  GPUShader *sh = inst_.shaders.static_shader_get(DOF_GATHER_HOLE_FILL);
  DRWShadingGroup *grp = DRW_shgroup_create(sh, hole_fill_ps_);
  inst_.sampling.bind_resources(grp);
  DRW_shgroup_uniform_block(grp, "dof_buf", data_);
  DRW_shgroup_uniform_texture_ex(grp, "color_bilinear_tx", reduced_color_tx_, gather_bilinear);
  DRW_shgroup_uniform_texture_ex(grp, "color_tx", reduced_color_tx_, gather_nearest);
  DRW_shgroup_uniform_texture_ex(grp, "coc_tx", reduced_coc_tx_, gather_nearest);
  DRW_shgroup_uniform_image_ref(grp, "in_tiles_fg_img", &tiles_fg_tx_.current());
  DRW_shgroup_uniform_image_ref(grp, "in_tiles_bg_img", &tiles_bg_tx_.current());
  DRW_shgroup_uniform_image_ref(grp, "out_color_img", &hole_fill_color_tx_);
  DRW_shgroup_uniform_image_ref(grp, "out_weight_img", &hole_fill_weight_tx_);
  DRW_shgroup_call_compute_ref(grp, dispatch_gather_size_);
  DRW_shgroup_barrier(grp, GPU_BARRIER_TEXTURE_FETCH);
}

void DepthOfField::resolve_pass_sync()
{
  eGPUSamplerState with_filter = GPU_SAMPLER_FILTER;
  RenderBuffers &render_buffers = inst_.render_buffers;

  resolve_ps_ = DRW_pass_create("Dof.resolve_ps_", DRW_STATE_NO_DRAW);
  bool use_lut = bokeh_lut_ps_ != nullptr;
  eShaderType sh_type = use_lut ? DOF_RESOLVE_LUT : DOF_RESOLVE;
  GPUShader *sh = inst_.shaders.static_shader_get(sh_type);
  DRWShadingGroup *grp = DRW_shgroup_create(sh, resolve_ps_);
  inst_.sampling.bind_resources(grp);
  DRW_shgroup_uniform_block(grp, "dof_buf", data_);
  DRW_shgroup_uniform_texture_ref_ex(grp, "depth_tx", &render_buffers.depth_tx, no_filter);
  DRW_shgroup_uniform_texture_ref_ex(grp, "color_tx", &input_color_tx_, no_filter);
  DRW_shgroup_uniform_texture_ref_ex(grp, "stable_color_tx", &resolve_stable_color_tx_, no_filter);
  DRW_shgroup_uniform_texture_ref_ex(grp, "color_bg_tx", &color_bg_tx_.current(), with_filter);
  DRW_shgroup_uniform_texture_ref_ex(grp, "color_fg_tx", &color_fg_tx_.current(), with_filter);
  DRW_shgroup_uniform_image_ref(grp, "in_tiles_fg_img", &tiles_fg_tx_.current());
  DRW_shgroup_uniform_image_ref(grp, "in_tiles_bg_img", &tiles_bg_tx_.current());
  DRW_shgroup_uniform_texture_ref(grp, "weight_bg_tx", &weight_bg_tx_.current());
  DRW_shgroup_uniform_texture_ref(grp, "weight_fg_tx", &weight_fg_tx_.current());
  DRW_shgroup_uniform_texture_ref(grp, "color_hole_fill_tx", &hole_fill_color_tx_);
  DRW_shgroup_uniform_texture_ref(grp, "weight_hole_fill_tx", &hole_fill_weight_tx_);
  DRW_shgroup_uniform_texture_ref(grp, "bokeh_lut_tx", &bokeh_resolve_lut_tx_);
  DRW_shgroup_uniform_image_ref(grp, "out_color_img", &output_color_tx_);
  DRW_shgroup_barrier(grp, GPU_BARRIER_TEXTURE_FETCH);
  DRW_shgroup_call_compute_ref(grp, dispatch_resolve_size_);
  DRW_shgroup_barrier(grp, GPU_BARRIER_TEXTURE_FETCH);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Post-FX Rendering.
 * \{ */

/* Similar to Film::update_sample_table() but with constant filter radius and constant sample
 * count. */
void DepthOfField::update_sample_table()
{
  float2 subpixel_offset = inst_.film.pixel_jitter_get();
  /* Since the film jitter is in full-screen res, divide by 2 to get the jitter in half res. */
  subpixel_offset *= 0.5;

  /* Same offsets as in dof_spatial_filtering(). */
  const std::array<int2, 4> plus_offsets = {int2(-1, 0), int2(0, -1), int2(1, 0), int2(0, 1)};

  const float radius = 1.5f;
  int i = 0;
  for (int2 offset : plus_offsets) {
    float2 pixel_ofs = float2(offset) - subpixel_offset;
    data_.filter_samples_weight[i++] = film_filter_weight(radius, math::length_squared(pixel_ofs));
  }
  data_.filter_center_weight = film_filter_weight(radius, math::length_squared(subpixel_offset));
}

void DepthOfField::render(GPUTexture **input_tx,
                          GPUTexture **output_tx,
                          DepthOfFieldBuffer &dof_buffer)
{
  if (fx_radius_ == 0.0f) {
    return;
  }

  input_color_tx_ = *input_tx;
  output_color_tx_ = *output_tx;
  extent_ = {GPU_texture_width(input_color_tx_), GPU_texture_height(input_color_tx_)};

  {
    const CameraData &cam_data = inst_.camera.data_get();
    data_.camera_type = cam_data.type;
    /* OPTI(fclem) Could be optimized. */
    float3 jitter = float3(fx_radius_, 0.0f, -focus_distance_);
    float3 center = float3(0.0f, 0.0f, -focus_distance_);
    mul_project_m4_v3(cam_data.winmat.ptr(), jitter);
    mul_project_m4_v3(cam_data.winmat.ptr(), center);
    /* Simplify CoC calculation to a simple MADD. */
    if (inst_.camera.is_orthographic()) {
      data_.coc_mul = (center[0] - jitter[0]) * 0.5f * extent_[0];
      data_.coc_bias = focus_distance_ * data_.coc_mul;
    }
    else {
      data_.coc_bias = -(center[0] - jitter[0]) * 0.5f * extent_[0];
      data_.coc_mul = focus_distance_ * data_.coc_bias;
    }

    float min_fg_coc = coc_radius_from_camera_depth(data_, -cam_data.clip_near);
    float max_bg_coc = coc_radius_from_camera_depth(data_, -cam_data.clip_far);
    if (data_.camera_type != CAMERA_ORTHO) {
      /* Background is at infinity so maximum CoC is the limit of coc_radius_from_camera_depth
       * at -inf. We only do this for perspective camera since orthographic coc limit is inf. */
      max_bg_coc = data_.coc_bias;
    }
    /* Clamp with user defined max. */
    data_.coc_abs_max = min_ff(max_ff(fabsf(min_fg_coc), fabsf(max_bg_coc)), fx_max_coc_);
    /* TODO(fclem): Make this dependent of the quality of the gather pass. */
    data_.scatter_coc_threshold = 4.0f;

    update_sample_table();

    data_.push_update();
  }

  int2 half_res = math::divide_ceil(extent_, int2(2));
  int2 quarter_res = math::divide_ceil(extent_, int2(4));
  int2 tile_res = math::divide_ceil(half_res, int2(DOF_TILES_SIZE));

  dispatch_setup_size_ = int3(math::divide_ceil(half_res, int2(DOF_DEFAULT_GROUP_SIZE)), 1);
  dispatch_stabilize_size_ = int3(math::divide_ceil(half_res, int2(DOF_STABILIZE_GROUP_SIZE)), 1);
  dispatch_downsample_size_ = int3(math::divide_ceil(quarter_res, int2(DOF_DEFAULT_GROUP_SIZE)),
                                   1);
  dispatch_reduce_size_ = int3(math::divide_ceil(half_res, int2(DOF_REDUCE_GROUP_SIZE)), 1);
  dispatch_tiles_flatten_size_ = int3(math::divide_ceil(half_res, int2(DOF_TILES_SIZE)), 1);
  dispatch_tiles_dilate_size_ = int3(
      math::divide_ceil(tile_res, int2(DOF_TILES_DILATE_GROUP_SIZE)), 1);
  dispatch_gather_size_ = int3(math::divide_ceil(half_res, int2(DOF_GATHER_GROUP_SIZE)), 1);
  dispatch_filter_size_ = int3(math::divide_ceil(half_res, int2(DOF_FILTER_GROUP_SIZE)), 1);
  dispatch_resolve_size_ = int3(math::divide_ceil(extent_, int2(DOF_RESOLVE_GROUP_SIZE)), 1);

  if (GPU_type_matches_ex(GPU_DEVICE_ATI, GPU_OS_UNIX, GPU_DRIVER_ANY, GPU_BACKEND_OPENGL)) {
    /* On Mesa, there is a sync bug which can make a portion of the main pass (usually one shader)
     * leave blocks of un-initialized memory. Doing a flush seems to alleviate the issue. */
    GPU_flush();
  }

  DRW_stats_group_start("Depth of Field");

  {
    DRW_stats_group_start("Setup");
    {
      bokeh_gather_lut_tx_.acquire(int2(DOF_BOKEH_LUT_SIZE), GPU_RG16F);
      bokeh_scatter_lut_tx_.acquire(int2(DOF_BOKEH_LUT_SIZE), GPU_R16F);
      bokeh_resolve_lut_tx_.acquire(int2(DOF_MAX_SLIGHT_FOCUS_RADIUS * 2 + 1), GPU_R16F);

      DRW_draw_pass(bokeh_lut_ps_);
    }
    {
      setup_color_tx_.acquire(half_res, GPU_RGBA16F);
      setup_coc_tx_.acquire(half_res, GPU_R16F);

      DRW_draw_pass(setup_ps_);
    }
    {
      stabilize_output_tx_.acquire(half_res, GPU_RGBA16F);
      stabilize_valid_history_ = !dof_buffer.stabilize_history_tx_.ensure_2d(GPU_RGBA16F,
                                                                             half_res);

      if (stabilize_valid_history_ == false) {
        /* Avoid uninitialized memory that can contain NaNs. */
        dof_buffer.stabilize_history_tx_.clear(float4(0.0f));
      }

      stabilize_input_ = dof_buffer.stabilize_history_tx_;
      /* Outputs to reduced_*_tx_ mip 0. */
      DRW_draw_pass(stabilize_ps_);

      /* WATCH(fclem): Swap Texture an TextureFromPool internal GPUTexture in order to reuse
       * the one that we just consumed. */
      TextureFromPool::swap(stabilize_output_tx_, dof_buffer.stabilize_history_tx_);

      /* Used by stabilize pass. */
      stabilize_output_tx_.release();
      setup_color_tx_.release();
    }
    {
      DRW_stats_group_start("Tile Prepare");

      /* WARNING: If format changes, make sure dof_tile_* GLSL constants are properly encoded. */
      tiles_fg_tx_.previous().acquire(tile_res, GPU_R11F_G11F_B10F);
      tiles_bg_tx_.previous().acquire(tile_res, GPU_R11F_G11F_B10F);
      tiles_fg_tx_.current().acquire(tile_res, GPU_R11F_G11F_B10F);
      tiles_bg_tx_.current().acquire(tile_res, GPU_R11F_G11F_B10F);

      DRW_draw_pass(tiles_flatten_ps_);

      /* Used by tile_flatten and stabilize_ps pass. */
      setup_coc_tx_.release();

      /* Error introduced by gather center jittering. */
      const float error_multiplier = 1.0f + 1.0f / (DOF_GATHER_RING_COUNT + 0.5f);
      int dilation_end_radius = ceilf((fx_max_coc_ * error_multiplier) / (DOF_TILES_SIZE * 2));

      /* Run dilation twice. One for minmax and one for minabs. */
      for (int pass = 0; pass < 2; pass++) {
        /* This algorithm produce the exact dilation radius by dividing it in multiple passes. */
        int dilation_radius = 0;
        while (dilation_radius < dilation_end_radius) {
          int remainder = dilation_end_radius - dilation_radius;
          /* Do not step over any unvisited tile. */
          int max_multiplier = dilation_radius + 1;

          int ring_count = min_ii(DOF_DILATE_RING_COUNT, ceilf(remainder / (float)max_multiplier));
          int multiplier = min_ii(max_multiplier, floorf(remainder / (float)ring_count));

          dilation_radius += ring_count * multiplier;

          tiles_dilate_ring_count_ = ring_count;
          tiles_dilate_ring_width_mul_ = multiplier;

          tiles_fg_tx_.swap();
          tiles_bg_tx_.swap();

          DRW_draw_pass((pass == 0) ? tiles_dilate_minmax_ps_ : tiles_dilate_minabs_ps_);
        }
      }

      tiles_fg_tx_.previous().release();
      tiles_bg_tx_.previous().release();

      DRW_stats_group_end();
    }

    downsample_tx_.acquire(quarter_res, GPU_RGBA16F);

    DRW_draw_pass(downsample_ps_);

    scatter_fg_indirect_buf_.clear_to_zero();
    scatter_bg_indirect_buf_.clear_to_zero();

    DRW_draw_pass(reduce_ps_);

    /* Used by reduce pass. */
    downsample_tx_.release();

    DRW_stats_group_end();
  }

  for (int is_background = 0; is_background < 2; is_background++) {
    DRW_stats_group_start(is_background ? "Background Convolution" : "Foreground Convolution");

    SwapChain<TextureFromPool, 2> &color_tx = is_background ? color_bg_tx_ : color_fg_tx_;
    SwapChain<TextureFromPool, 2> &weight_tx = is_background ? weight_bg_tx_ : weight_fg_tx_;
    Framebuffer &scatter_fb = is_background ? scatter_bg_fb_ : scatter_fg_fb_;
    DRWPass *gather_ps = is_background ? gather_bg_ps_ : gather_fg_ps_;
    DRWPass *filter_ps = is_background ? filter_bg_ps_ : filter_fg_ps_;
    DRWPass *scatter_ps = is_background ? scatter_bg_ps_ : scatter_fg_ps_;

    color_tx.current().acquire(half_res, GPU_RGBA16F);
    weight_tx.current().acquire(half_res, GPU_R16F);
    occlusion_tx_.acquire(half_res, GPU_RG16F);

    DRW_draw_pass(gather_ps);

    {
      /* Filtering pass. */
      color_tx.swap();
      weight_tx.swap();

      color_tx.current().acquire(half_res, GPU_RGBA16F);
      weight_tx.current().acquire(half_res, GPU_R16F);

      DRW_draw_pass(filter_ps);

      color_tx.previous().release();
      weight_tx.previous().release();
    }

    GPU_memory_barrier(GPU_BARRIER_FRAMEBUFFER);

    scatter_fb.ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(color_tx.current()));

    GPU_framebuffer_bind(scatter_fb);
    DRW_draw_pass(scatter_ps);

    /* Used by scatter pass. */
    occlusion_tx_.release();

    DRW_stats_group_end();
  }
  {
    DRW_stats_group_start("Hole Fill");

    bokeh_gather_lut_tx_.release();
    bokeh_scatter_lut_tx_.release();

    hole_fill_color_tx_.acquire(half_res, GPU_RGBA16F);
    hole_fill_weight_tx_.acquire(half_res, GPU_R16F);

    DRW_draw_pass(hole_fill_ps_);

    /* NOTE: We do not filter the hole-fill pass as effect is likely to not be noticeable. */

    DRW_stats_group_end();
  }
  {
    DRW_stats_group_start("Resolve");

    resolve_stable_color_tx_ = dof_buffer.stabilize_history_tx_;

    DRW_draw_pass(resolve_ps_);

    color_bg_tx_.current().release();
    color_fg_tx_.current().release();
    weight_bg_tx_.current().release();
    weight_fg_tx_.current().release();
    tiles_fg_tx_.current().release();
    tiles_bg_tx_.current().release();
    hole_fill_color_tx_.release();
    hole_fill_weight_tx_.release();
    bokeh_resolve_lut_tx_.release();

    DRW_stats_group_end();
  }

  DRW_stats_group_end();

  /* Swap buffers so that next effect has the right input. */
  SWAP(GPUTexture *, *input_tx, *output_tx);
}

/** \} */

}  // namespace blender::eevee
