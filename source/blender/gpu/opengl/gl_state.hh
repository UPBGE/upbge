/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2020 Blender Foundation. */

/** \file
 * \ingroup gpu
 */

#pragma once

#include "MEM_guardedalloc.h"

#include "BLI_utildefines.h"

#include "gpu_state_private.hh"

#include <epoxy/gl.h>

namespace blender {
namespace gpu {

class GLFrameBuffer;
class GLTexture;

/**
 * State manager keeping track of the draw state and applying it before drawing.
 * Opengl Implementation.
 */
class GLStateManager : public StateManager {
 public:
  /** Another reference to the active frame-buffer. */
  GLFrameBuffer *active_fb = nullptr;

 private:
  /** Current state of the GL implementation. Avoids resetting the whole state for every change. */
  GPUState current_;
  GPUStateMutable current_mutable_;
  /** Limits. */
  float line_width_range_[2];

  /** Texture state:
   * We keep the full stack of textures and sampler bounds to use multi bind, and to be able to
   * edit and restore texture binds on the fly without querying the context.
   * Also this allows us to keep track of textures bounds to many texture units.
   * Keep the targets to know what target to set to 0 for unbinding (legacy).
   * Init first target to GL_TEXTURE_2D for texture_bind_temp to work.
   */
  GLuint targets_[64] = {GL_TEXTURE_2D};
  GLuint textures_[64] = {0};
  GLuint samplers_[64] = {0};
  uint64_t dirty_texture_binds_ = 0;

  GLuint images_[8] = {0};
  GLenum formats_[8] = {0};
  uint8_t dirty_image_binds_ = 0;

 public:
  GLStateManager();

  void apply_state() override;
  /**
   * Will set all the states regardless of the current ones.
   */
  void force_state() override;

  void issue_barrier(eGPUBarrier barrier_bits) override;

  void texture_bind(Texture *tex, eGPUSamplerState sampler, int unit) override;
  /**
   * Bind the texture to slot 0 for editing purpose. Used by legacy pipeline.
   */
  void texture_bind_temp(GLTexture *tex);
  void texture_unbind(Texture *tex) override;
  void texture_unbind_all() override;

  void image_bind(Texture *tex, int unit) override;
  void image_unbind(Texture *tex) override;
  void image_unbind_all() override;

  void texture_unpack_row_length_set(uint len) override;

  uint64_t bound_texture_slots();
  uint8_t bound_image_slots();

 private:
  static void set_write_mask(eGPUWriteMask value);
  static void set_depth_test(eGPUDepthTest value);
  static void set_stencil_test(eGPUStencilTest test, eGPUStencilOp operation);
  static void set_stencil_mask(eGPUStencilTest test, const GPUStateMutable state);
  static void set_clip_distances(int new_dist_len, int old_dist_len);
  static void set_logic_op(bool enable);
  static void set_facing(bool invert);
  static void set_backface_culling(eGPUFaceCullTest test);
  static void set_provoking_vert(eGPUProvokingVertex vert);
  static void set_shadow_bias(bool enable);
  static void set_blend(eGPUBlend value);

  void set_state(const GPUState &state);
  void set_mutable_state(const GPUStateMutable &state);

  void texture_bind_apply();
  void image_bind_apply();

  MEM_CXX_CLASS_ALLOC_FUNCS("GLStateManager")
};

static inline GLbitfield to_gl(eGPUBarrier barrier_bits)
{
  GLbitfield barrier = 0;
  if (barrier_bits & GPU_BARRIER_SHADER_IMAGE_ACCESS) {
    barrier |= GL_SHADER_IMAGE_ACCESS_BARRIER_BIT;
  }
  if (barrier_bits & GPU_BARRIER_SHADER_STORAGE) {
    barrier |= GL_SHADER_STORAGE_BARRIER_BIT;
  }
  if (barrier_bits & GPU_BARRIER_TEXTURE_FETCH) {
    barrier |= GL_TEXTURE_FETCH_BARRIER_BIT;
  }
  if (barrier_bits & GPU_BARRIER_TEXTURE_UPDATE) {
    barrier |= GL_TEXTURE_UPDATE_BARRIER_BIT;
  }
  if (barrier_bits & GPU_BARRIER_COMMAND) {
    barrier |= GL_COMMAND_BARRIER_BIT;
  }
  if (barrier_bits & GPU_BARRIER_FRAMEBUFFER) {
    barrier |= GL_FRAMEBUFFER_BARRIER_BIT;
  }
  if (barrier_bits & GPU_BARRIER_VERTEX_ATTRIB_ARRAY) {
    barrier |= GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT;
  }
  if (barrier_bits & GPU_BARRIER_ELEMENT_ARRAY) {
    barrier |= GL_ELEMENT_ARRAY_BARRIER_BIT;
  }
  return barrier;
}

}  // namespace gpu
}  // namespace blender
