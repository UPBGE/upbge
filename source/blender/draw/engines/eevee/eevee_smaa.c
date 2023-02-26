/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Copyright 2020, Blender Foundation.
 */

/** \file
 * \ingroup draw_engine
 *
 * Anti-aliasing:
 *
 * We use SMAA (Smart Morphological Anti-Aliasing) as a fast antialiasing solution.
 */

#include "smaa_textures.h"

#include "eevee_private.h"

int EEVEE_antialiasing_engine_init(EEVEE_Data *vedata)
{
  EEVEE_FramebufferList *fbl = vedata->fbl;
  EEVEE_TextureList *txl = vedata->txl;
  EEVEE_PrivateData *g_data = vedata->stl->g_data;
  const DRWContextState *draw_ctx = DRW_context_state_get();
  Scene *scene_eval = draw_ctx->scene;

  if (!(scene_eval->eevee.gameflag & SCE_EEVEE_SMAA) ||
      (scene_eval->gm.samples_per_frame != 1 &&
       !(scene_eval->gm.flag & GAME_USE_VIEWPORT_RENDER))) {
    /* Cleanup */
    DRW_TEXTURE_FREE_SAFE(txl->history_buffer_tx);
    DRW_TEXTURE_FREE_SAFE(txl->depth_buffer_tx);
    DRW_TEXTURE_FREE_SAFE(txl->smaa_search_tx);
    DRW_TEXTURE_FREE_SAFE(txl->smaa_area_tx);
    return 0;
  }

  DrawEngineType *owner = (DrawEngineType *)&EEVEE_antialiasing_engine_init;

  DRW_texture_ensure_fullscreen_2d(&txl->history_buffer_tx, GPU_RGBA16F, DRW_TEX_FILTER);
  DRW_texture_ensure_fullscreen_2d(&txl->depth_buffer_tx, GPU_DEPTH24_STENCIL8, 0);

  g_data->smaa_edge_tx = DRW_texture_pool_query_fullscreen(GPU_RG8, owner);
  g_data->smaa_weight_tx = DRW_texture_pool_query_fullscreen(GPU_RGBA8, owner);

  GPU_framebuffer_ensure_config(&fbl->smaa_fb,
                                {
                                    GPU_ATTACHMENT_TEXTURE(txl->depth_buffer_tx),
                                    GPU_ATTACHMENT_TEXTURE(txl->history_buffer_tx),
                                });

  GPU_framebuffer_ensure_config(&fbl->smaa_edge_fb,
                                {
                                    GPU_ATTACHMENT_NONE,
                                    GPU_ATTACHMENT_TEXTURE(g_data->smaa_edge_tx),
                                });

  GPU_framebuffer_ensure_config(&fbl->smaa_weight_fb,
                                {
                                    GPU_ATTACHMENT_NONE,
                                    GPU_ATTACHMENT_TEXTURE(g_data->smaa_weight_tx),
                                });

  /* TODO could be shared for all viewports. */
  if (txl->smaa_search_tx == NULL) {
    eGPUTextureUsage usage = GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_ATTACHMENT;
    txl->smaa_search_tx = GPU_texture_create_2d(
        "smaa_search", SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, 1, GPU_R8, usage, NULL);
    GPU_texture_update(txl->smaa_search_tx, GPU_DATA_UBYTE, searchTexBytes);
    txl->smaa_area_tx = GPU_texture_create_2d(
        "smaa_area", AREATEX_WIDTH, AREATEX_HEIGHT, 1, GPU_RG8, usage, NULL);
    GPU_texture_update(txl->smaa_area_tx, GPU_DATA_UBYTE, areaTexBytes);

    GPU_texture_filter_mode(txl->smaa_search_tx, true);
    GPU_texture_filter_mode(txl->smaa_area_tx, true);
  }
  return EFFECT_SMAA | EFFECT_DOUBLE_BUFFER;
}

static int prev_smaa_quality = 2;
static float prev_smaa_scale = 0.01f;

void EEVEE_antialiasing_cache_init(EEVEE_Data *vedata)
{
  EEVEE_TextureList *txl = vedata->txl;
  EEVEE_PrivateData *g_data = vedata->stl->g_data;
  EEVEE_PassList *psl = vedata->psl;
  DRWShadingGroup *grp = NULL;

  EEVEE_EffectsInfo *effects = vedata->stl->effects;
  if (!(effects->enabled_effects & EFFECT_SMAA)) {
    return;
  }

  const float *size = DRW_viewport_size_get();
  const float *sizeinv = DRW_viewport_invert_size_get();
  float metrics[4] = {sizeinv[0], sizeinv[1], size[0], size[1]};
  const DRWContextState *draw_ctx = DRW_context_state_get();
  Scene *scene_eval = draw_ctx->scene;

  bool shader_changed = false;
  shader_changed = scene_eval->eevee.smaa_quality != prev_smaa_quality;
  shader_changed = shader_changed || scene_eval->eevee.smaa_predication_scale != prev_smaa_scale;

  if (shader_changed) {
    DRW_viewport_request_redraw();
  }

  prev_smaa_quality = scene_eval->eevee.smaa_quality;
  prev_smaa_scale = scene_eval->eevee.smaa_predication_scale;

  {
    /* Stage 1: Edge detection. */
    DRW_PASS_CREATE(psl->smaa_edge_ps, DRW_STATE_WRITE_COLOR);

    GPUShader *sh = eevee_shader_antialiasing_get(0,
                                                  scene_eval->eevee.smaa_quality,
                                                  scene_eval->eevee.smaa_predication_scale,
                                                  shader_changed);
    grp = DRW_shgroup_create(sh, psl->smaa_edge_ps);
    DRW_shgroup_uniform_texture(grp, "colorTex", txl->history_buffer_tx);
    DRW_shgroup_uniform_texture(grp, "depthTex", txl->depth_buffer_tx);
    DRW_shgroup_uniform_vec4_copy(grp, "viewportMetrics", metrics);

    DRW_shgroup_clear_framebuffer(grp, GPU_COLOR_BIT, 0, 0, 0, 0, 0.0f, 0x0);
    DRW_shgroup_call_procedural_triangles(grp, NULL, 1);
  }
  {
    /* Stage 2: Blend Weight/Coord. */
    DRW_PASS_CREATE(psl->smaa_weight_ps, DRW_STATE_WRITE_COLOR);

    GPUShader *sh = eevee_shader_antialiasing_get(1,
                                                  scene_eval->eevee.smaa_quality,
                                                  scene_eval->eevee.smaa_predication_scale,
                                                  shader_changed);
    grp = DRW_shgroup_create(sh, psl->smaa_weight_ps);
    DRW_shgroup_uniform_texture(grp, "edgesTex", g_data->smaa_edge_tx);
    DRW_shgroup_uniform_texture(grp, "areaTex", txl->smaa_area_tx);
    DRW_shgroup_uniform_texture(grp, "searchTex", txl->smaa_search_tx);
    DRW_shgroup_uniform_vec4_copy(grp, "viewportMetrics", metrics);

    DRW_shgroup_clear_framebuffer(grp, GPU_COLOR_BIT, 0, 0, 0, 0, 0.0f, 0x0);
    DRW_shgroup_call_procedural_triangles(grp, NULL, 1);
  }
  {
    /* Stage 3: Resolve. */
    DRW_PASS_CREATE(psl->smaa_resolve_ps, DRW_STATE_WRITE_COLOR);

    GPUShader *sh = eevee_shader_antialiasing_get(2,
                                                  scene_eval->eevee.smaa_quality,
                                                  scene_eval->eevee.smaa_predication_scale,
                                                  shader_changed);
    grp = DRW_shgroup_create(sh, psl->smaa_resolve_ps);
    DRW_shgroup_uniform_texture(grp, "blendTex", g_data->smaa_weight_tx);
    DRW_shgroup_uniform_texture(grp, "colorTex", txl->history_buffer_tx);
    DRW_shgroup_uniform_vec4_copy(grp, "viewportMetrics", metrics);

    DRW_shgroup_call_procedural_triangles(grp, NULL, 1);
  }
}

void EEVEE_antialiasing_draw_pass(EEVEE_Data *vedata)
{
  EEVEE_FramebufferList *fbl = vedata->fbl;
  EEVEE_PassList *psl = vedata->psl;
  DefaultFramebufferList *dfbl = DRW_viewport_framebuffer_list_get();

  EEVEE_EffectsInfo *effects = vedata->stl->effects;
  if (!(effects->enabled_effects & EFFECT_SMAA) || !vedata->stl->g_data->valid_double_buffer) {
    return;
  }

  /* In playback mode, we are sure the next redraw will not use the same viewmatrix.
   * In this case no need to save the depth buffer. */
  eGPUFrameBufferBits bits = vedata->stl->effects->taa_current_sample == 1 ?
                                 GPU_COLOR_BIT :
                                 GPU_COLOR_BIT | GPU_DEPTH_BIT;
  GPU_framebuffer_blit(dfbl->default_fb, 0, fbl->smaa_fb, 0, bits);

  GPU_framebuffer_bind(fbl->smaa_edge_fb);
  DRW_draw_pass(psl->smaa_edge_ps);

  GPU_framebuffer_bind(fbl->smaa_weight_fb);
  DRW_draw_pass(psl->smaa_weight_ps);

  GPU_framebuffer_bind(dfbl->default_fb);
  DRW_draw_pass(psl->smaa_resolve_ps);
}
