/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "eevee_game_defines.hh"
#include "GPU_texture.hh"

#include "../eevee/eevee_gbuffer.hh"

namespace blender::eevee_game {

/**
 * GameRenderBuffers handles the allocation and lifetime of all 
 * intermediate textures used during the frame.
 */
class RenderBuffers {
 public:
  RenderBuffers(class GameInstance &inst) : inst_(inst) {}

  /* (Re)allocate every target for the given render resolution. Cheap when
   * nothing changed: Texture::ensure_*() only compares size and format.
   *
   * All targets are persistent, not pooled. They are needed every frame for
   * the whole frame, combined_tx must survive into the next frame (SSGI reads
   * it as last frame's radiance), and framebuffers that reference them stay
   * valid without being rebuilt. */
  /* Returns true when the targets were reallocated: their previous content,
   * including last frame's radiance, is gone. */
  bool acquire(int2 extent);

  /* Core render targets. */
  Texture depth_tx = {"depth_tx"}; /* D32F + S8, reversed (1 = near) */
  /* HDR color, RGBA16F. Alpha follows EEVEE's convention because EEVEE's
   * material shaders write it: transmittance, 1 = nothing drawn, 0 = opaque. */
  Texture combined_tx = {"combined_tx"};
  /* Last frame's combined_tx, the radiance source of SSGI. The two are
   * swapped at the start of each frame: the G-Buffer pass writes emission
   * into combined_tx before SSGI runs, so the previous frame cannot be read
   * from the same texture, and swapping costs nothing where a copy would
   * move a full RGBA16F target every frame. */
  Texture combined_prev_tx = {"combined_prev_tx"};
  Texture vector_tx = {"vector_tx"}; /* RG16F motion vectors, UV units, previous - current */

  /* G-Buffer, in the layout EEVEE's material shaders write and
   * `eevee_gbuffer_read_lib.glsl` decodes: header (closure types), packed
   * closures, octahedral normals. Layer counts depend on which closures the
   * synced materials use, so it is allocated in PipelineModule::sync(). */
  eevee::GBuffer gbuffer;

  /* Shadow mask: one float per pixel, written by the PCF 3x3 compute shader
   * and read by the deferred lighting pass.
   * R16F is enough for a [0,1] factor and halves the cost of R32F. */
  Texture shadow_mask_tx = {"shadow_mask_tx"};

  /* FSR 3 inputs. */
  Texture reactive_mask_tx = {"reactive_mask_tx"}; /* R8, protects smoke / alpha */
  Texture transp_mask_tx = {"transp_mask_tx"};     /* R8, protects glass / composition */
  Texture ui_color_tx = {"ui_color_tx"};           /* RGBA8 HUD at display resolution */

  /* Render pass and cryptomatte outputs of EEVEE's deferred surface shader.
   * A game has none, but the shader declares the images, so something valid
   * must be bound. 1x1: every pass index in the uniform data is -1, and the
   * shader skips cryptomatte when the image is one pixel wide. */
  Texture rp_color_tx = {"rp_color_dummy"};
  Texture rp_value_tx = {"rp_value_dummy"};
  Texture cryptomatte_tx = {"cryptomatte_dummy"};

 private:
  class GameInstance &inst_;
};

} // namespace blender::eevee_game
