/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "eevee_game_culling.hh"
#include "eevee_game_instance.hh"

#include "BLI_math_base.h"

#include "GPU_compute.hh"
#include "GPU_debug.hh"
#include "GPU_shader.hh"
#include "GPU_state.hh"
#include "GPU_storage_buffer.hh"
#include "GPU_texture.hh"
#include "GPU_uniform_buffer.hh"

#include "draw_defines.hh"

namespace blender::eevee_game {

void GameView::compute_visibility(draw::ObjectBoundsBuf &bounds,
                                  draw::ObjectInfosBuf & /*infos*/,
                                  uint resource_len,
                                  bool /*debug_freeze*/)
{
  GPU_debug_group_begin("GameView.compute_visibility");

  /* Single view: one bit per resource, tightly packed. This is the layout
   * `draw_command_generate` expects when `view_len == 1`. */
  uint words_len = divide_ceil_u(resource_len, 32);
  words_len = ceil_to_multiple_u(max_ii(1, words_len), 4);
  visibility_buf_.resize(words_len);

  /* Everything starts visible; the shader only clears bits. */
  const uint32_t data = 0xFFFFFFFFu;
  GPU_storagebuf_clear(visibility_buf_, data);

  if (do_visibility_) {
    HiZBufferPair &hiz = inst_.hiz_buffer;

    /* The occlusion test answers "was this box hidden from last frame's
     * camera". That only predicts this frame if the camera moved coherently,
     * so it is skipped after a cut and while the pyramid holds no data. */
    const bool use_occlusion = hiz.valid && inst_.frame_state.history_valid;

    gpu::Shader *shader = inst_.shaders.static_shader_get(SH_CULLING_COMPUTE);
    GPU_shader_bind(shader);
    GPU_shader_uniform_1i(shader, "resource_len", resource_len);
    GPU_shader_uniform_1b(shader, "use_occlusion", use_occlusion);
    /* Matrix the pyramid was rasterized with, not the current one. Depth and
     * projection must come from the same frame or the test is meaningless. */
    GPU_shader_uniform_mat4(shader, "hiz_persmat", hiz.persmat.ptr());
    GPU_shader_uniform_2fv(shader, "hiz_uv_scale", hiz.uv_scale);
    GPU_texture_bind(hiz.front.ref_tx_, GPU_shader_get_sampler_binding(shader, "hiz_tx"));
    GPU_storagebuf_bind(bounds, GPU_shader_get_ssbo_binding(shader, "bounds_buf"));
    GPU_storagebuf_bind(visibility_buf_, GPU_shader_get_ssbo_binding(shader, "visibility_buf"));
    GPU_uniformbuf_bind(data_, DRW_VIEW_UBO_SLOT);
    GPU_uniformbuf_bind(culling_, DRW_VIEW_CULLING_UBO_SLOT);
    GPU_compute_dispatch(shader, divide_ceil_u(resource_len, DRW_VISIBILITY_GROUP_SIZE), 1, 1);
    GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);
  }

  GPU_debug_group_end();
}

}  // namespace blender::eevee_game
