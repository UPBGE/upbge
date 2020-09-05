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
 *
 * If the viewport stays static, the engine ask for multiple redraw and will progressively
 * converge to a much more accurate image without aliasing.
 * We call this one TAA (Temporal Anti-Aliasing).
 *
 * This is done using an accumulation buffer and a final pass that will output the final color
 * to the scene buffer. We softly blend between SMAA and TAA to avoid really harsh transitions.
 */

#include "ED_screen.h"

#include "BLI_jitter_2d.h"

#include "smaa_textures.h"

#include "eevee_private.h"

#define SMAA_THRESHOLD 16

static struct {
  bool init;
  float jitter_5[5][2];
  float jitter_8[8][2];
  float jitter_11[11][2];
  float jitter_16[16][2];
  float jitter_32[32][2];
} e_data = {false};

static void eevee_taa_jitter_init_order(float (*table)[2], int num)
{
  BLI_jitter_init(table, num);

  /* find closest element to center */
  int closest_index = 0;
  float closest_squared_distance = 1.0f;

  for (int index = 0; index < num; index++) {
    const float squared_dist = square_f(table[index][0]) + square_f(table[index][1]);
    if (squared_dist < closest_squared_distance) {
      closest_squared_distance = squared_dist;
      closest_index = index;
    }
  }

  /* move jitter table so that closest sample is in center */
  for (int index = 0; index < num; index++) {
    sub_v2_v2(table[index], table[closest_index]);
    mul_v2_fl(table[index], 2.0f);
  }

  /* swap center sample to the start of the table */
  if (closest_index != 0) {
    swap_v2_v2(table[0], table[closest_index]);
  }

  /* sort list based on furtest distance with previous */
  for (int i = 0; i < num - 2; i++) {
    float f_squared_dist = 0.0;
    int f_index = i;
    for (int j = i + 1; j < num; j++) {
      const float squared_dist = square_f(table[i][0] - table[j][0]) +
                                 square_f(table[i][1] - table[j][1]);
      if (squared_dist > f_squared_dist) {
        f_squared_dist = squared_dist;
        f_index = j;
      }
    }
    swap_v2_v2(table[i + 1], table[f_index]);
  }
}

static void eevee_taa_jitter_init(void)
{
  if (e_data.init == false) {
    e_data.init = true;
    eevee_taa_jitter_init_order(e_data.jitter_5, 5);
    eevee_taa_jitter_init_order(e_data.jitter_8, 8);
    eevee_taa_jitter_init_order(e_data.jitter_11, 11);
    eevee_taa_jitter_init_order(e_data.jitter_16, 16);
    eevee_taa_jitter_init_order(e_data.jitter_32, 32);
  }
}

int EEVEE_antialiasing_engine_init(EEVEE_Data *vedata)
{
  EEVEE_FramebufferList *fbl = vedata->fbl;
  EEVEE_TextureList *txl = vedata->txl;
  EEVEE_PrivateData *g_data = vedata->stl->g_data;
  const DRWContextState *draw_ctx = DRW_context_state_get();
  Scene *scene_eval = draw_ctx->scene;

  if (!(scene_eval->eevee.flag & SCE_EEVEE_SMAA)) {
    /* Cleanup */
    DRW_TEXTURE_FREE_SAFE(txl->history_buffer_tx);
    DRW_TEXTURE_FREE_SAFE(txl->depth_buffer_tx);
    DRW_TEXTURE_FREE_SAFE(txl->smaa_search_tx);
    DRW_TEXTURE_FREE_SAFE(txl->smaa_area_tx);
    return 0;
  }

  DrawEngineType *owner = (DrawEngineType *)&EEVEE_antialiasing_engine_init;
  g_data->view = NULL;

  eevee_taa_jitter_init();

  DRW_texture_ensure_fullscreen_2d(&txl->history_buffer_tx, GPU_RGBA16F, DRW_TEX_FILTER);
  DRW_texture_ensure_fullscreen_2d(&txl->depth_buffer_tx, GPU_DEPTH24_STENCIL8, 0);

  g_data->smaa_edge_tx = DRW_texture_pool_query_fullscreen(GPU_RG8, owner);
  g_data->smaa_weight_tx = DRW_texture_pool_query_fullscreen(GPU_RGBA8, owner);

  GPU_framebuffer_ensure_config(&fbl->antialiasing_fb,
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
    txl->smaa_search_tx = GPU_texture_create_2d(
        "smaa_search", SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, 1, GPU_R8, NULL);
    GPU_texture_update(txl->smaa_search_tx, GPU_DATA_UNSIGNED_BYTE, searchTexBytes);
    txl->smaa_area_tx = GPU_texture_create_2d(
        "smaa_area", AREATEX_WIDTH, AREATEX_HEIGHT, 1, GPU_RG8, NULL);
    GPU_texture_update(txl->smaa_area_tx, GPU_DATA_UNSIGNED_BYTE, areaTexBytes);

    GPU_texture_filter_mode(txl->smaa_search_tx, true);
    GPU_texture_filter_mode(txl->smaa_area_tx, true);
  }
  return EFFECT_SMAA;
}

void EEVEE_antialiasing_cache_init(EEVEE_Data *vedata)
{
  EEVEE_TextureList *txl = vedata->txl;
  EEVEE_PrivateData *g_data = vedata->stl->g_data;
  EEVEE_PassList *psl = vedata->psl;
  DefaultTextureList *dtxl = DRW_viewport_texture_list_get();
  DRWShadingGroup *grp = NULL;

  EEVEE_EffectsInfo *effects = vedata->stl->effects;
  if (!(effects->enabled_effects & EFFECT_SMAA)) {
    return;
  }

  {
    DRW_PASS_CREATE(psl->aa_accum_ps, DRW_STATE_WRITE_COLOR | DRW_STATE_BLEND_ADD_FULL);

    GPUShader *shader = eevee_shader_antialiasing_accumulation_get();
    grp = DRW_shgroup_create(shader, psl->aa_accum_ps);
    DRW_shgroup_uniform_texture(grp, "colorBuffer", dtxl->color);
    DRW_shgroup_call_procedural_triangles(grp, NULL, 1);
  }

  const float *size = DRW_viewport_size_get();
  const float *sizeinv = DRW_viewport_invert_size_get();
  float metrics[4] = {sizeinv[0], sizeinv[1], size[0], size[1]};

  {
    /* Stage 1: Edge detection. */
    DRW_PASS_CREATE(psl->aa_edge_ps, DRW_STATE_WRITE_COLOR);

    GPUShader *sh = eevee_shader_antialiasing_get(0);
    grp = DRW_shgroup_create(sh, psl->aa_edge_ps);
    DRW_shgroup_uniform_texture(grp, "colorTex", txl->history_buffer_tx);
    DRW_shgroup_uniform_vec4_copy(grp, "viewportMetrics", metrics);

    DRW_shgroup_clear_framebuffer(grp, GPU_COLOR_BIT, 0, 0, 0, 0, 0.0f, 0x0);
    DRW_shgroup_call_procedural_triangles(grp, NULL, 1);
  }
  {
    /* Stage 2: Blend Weight/Coord. */
    DRW_PASS_CREATE(psl->aa_weight_ps, DRW_STATE_WRITE_COLOR);

    GPUShader *sh = eevee_shader_antialiasing_get(1);
    grp = DRW_shgroup_create(sh, psl->aa_weight_ps);
    DRW_shgroup_uniform_texture(grp, "edgesTex", g_data->smaa_edge_tx);
    DRW_shgroup_uniform_texture(grp, "areaTex", txl->smaa_area_tx);
    DRW_shgroup_uniform_texture(grp, "searchTex", txl->smaa_search_tx);
    DRW_shgroup_uniform_vec4_copy(grp, "viewportMetrics", metrics);

    DRW_shgroup_clear_framebuffer(grp, GPU_COLOR_BIT, 0, 0, 0, 0, 0.0f, 0x0);
    DRW_shgroup_call_procedural_triangles(grp, NULL, 1);
  }
  {
    /* Stage 3: Resolve. */
    DRW_PASS_CREATE(psl->aa_resolve_ps, DRW_STATE_WRITE_COLOR);

    GPUShader *sh = eevee_shader_antialiasing_get(2);
    grp = DRW_shgroup_create(sh, psl->aa_resolve_ps);
    DRW_shgroup_uniform_texture(grp, "blendTex", g_data->smaa_weight_tx);
    DRW_shgroup_uniform_texture(grp, "colorTex", txl->history_buffer_tx);
    DRW_shgroup_uniform_vec4_copy(grp, "viewportMetrics", metrics);
    DRW_shgroup_uniform_float(grp, "mixFactor", &g_data->smaa_mix_factor, 1);
    DRW_shgroup_uniform_float(grp, "taaSampleCountInv", &g_data->taa_sample_inv, 1);

    DRW_shgroup_call_procedural_triangles(grp, NULL, 1);
  }
}

/* Return true if render is not cached. */
void eevee_antialiasing_setup(EEVEE_Data *vedata)
{
  EEVEE_PrivateData *g_data = vedata->stl->g_data;

  EEVEE_EffectsInfo *effects = vedata->stl->effects;
  if (!(effects->enabled_effects & EFFECT_SMAA)) {
    return;
  }

  if (vedata->stl->effects->taa_current_sample >= SMAA_THRESHOLD) {
    /* TAA accumulation has finish. Just copy the result back */
    return;
  }
  else {
    const float *viewport_size = DRW_viewport_size_get();
    const DRWView *default_view = DRW_view_default_get();
    float *transform_offset;

    transform_offset = e_data.jitter_32[min_ii(vedata->stl->effects->taa_current_sample, 32)];

    /* construct new matrices from transform delta */
    float winmat[4][4], viewmat[4][4], persmat[4][4];
    DRW_view_winmat_get(default_view, winmat, false);
    DRW_view_viewmat_get(default_view, viewmat, false);
    DRW_view_persmat_get(default_view, persmat, false);

    window_translate_m4(winmat,
                        persmat,
                        transform_offset[0] / viewport_size[0],
                        transform_offset[1] / viewport_size[1]);

    if (g_data->view) {
      /* When rendering just update the view. This avoids recomputing the culling. */
      DRW_view_update_sub(g_data->view, viewmat, winmat);
    }
    else {
      /* TAA is not making a big change to the matrices.
       * Reuse the main view culling by creating a sub-view. */
      g_data->view = DRW_view_create_sub(default_view, viewmat, winmat);
    }
    DRW_view_set_active(g_data->view);
    return;
  }
}

void EEVEE_antialiasing_draw_pass(EEVEE_Data *vedata)
{
  EEVEE_PrivateData *g_data = vedata->stl->g_data;
  EEVEE_FramebufferList *fbl = vedata->fbl;
  EEVEE_PassList *psl = vedata->psl;
  DefaultFramebufferList *dfbl = DRW_viewport_framebuffer_list_get();

  EEVEE_EffectsInfo *effects = vedata->stl->effects;
  if (!(effects->enabled_effects & EFFECT_SMAA)) {
    return;
  }

  // eevee_antialiasing_setup(vedata);

  /**
   * We always do SMAA on top of TAA accumulation, unless the number of samples of TAA is already
   * high. This ensure a smoother transition.
   * If TAA accumulation is finished, we only blit the result.
   */
  if (vedata->stl->effects->taa_current_sample < SMAA_THRESHOLD) {
    /* In playback mode, we are sure the next redraw will not use the same viewmatrix.
     * In this case no need to save the depth buffer. */
    eGPUFrameBufferBits bits = vedata->stl->effects->taa_current_sample == 1 ?
                                   GPU_COLOR_BIT :
                                   GPU_COLOR_BIT | GPU_DEPTH_BIT;
    GPU_framebuffer_blit(dfbl->default_fb, 0, fbl->antialiasing_fb, 0, bits);

    /* After a certain point SMAA is no longer necessary. */
    g_data->smaa_mix_factor = 1.0f -
                              clamp_f(vedata->stl->effects->taa_current_sample / 4.0f, 0.0f, 1.0f);
    g_data->taa_sample_inv = 1.0f /
                             clamp_f((vedata->stl->effects->taa_current_sample + 1), 0.0f, 1.0f);

    if (g_data->smaa_mix_factor > 0.0f) {
      GPU_framebuffer_bind(fbl->smaa_edge_fb);
      DRW_draw_pass(psl->aa_edge_ps);

      GPU_framebuffer_bind(fbl->smaa_weight_fb);
      DRW_draw_pass(psl->aa_weight_ps);
    }

    GPU_framebuffer_bind(dfbl->default_fb);
    DRW_draw_pass(psl->aa_resolve_ps);
  }
  else {
    /* Accumulate result to the TAA buffer. */
    GPU_framebuffer_bind(fbl->antialiasing_fb);
    DRW_draw_pass(psl->aa_accum_ps);
    /* Copy back the saved depth buffer for correct overlays. */
    GPU_framebuffer_blit(fbl->antialiasing_fb, 0, dfbl->default_fb, 0, GPU_DEPTH_BIT);
  }
}
