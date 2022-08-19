/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2016 Blender Foundation. */

/** \file
 * \ingroup draw_engine
 *
 * Temporal super sampling technique
 */

#include "DRW_render.h"

#include "ED_screen.h"

#include "BLI_rand.h"

#include "DEG_depsgraph_query.h"

#include "GPU_texture.h"
#include "eevee_private.h"

#define FILTER_CDF_TABLE_SIZE 512

static struct {
  /* Pixel filter table: Only blackman-harris for now. */
  bool inited;
  float inverted_cdf[FILTER_CDF_TABLE_SIZE];
} e_data = {false}; /* Engine data */

static float UNUSED_FUNCTION(filter_box)(float UNUSED(x))
{
  return 1.0f;
}

static float filter_blackman_harris(float x)
{
  /* Hardcoded 1px footprint [-0.5..0.5]. We resize later. */
  const float width = 1.0f;
  x = 2.0f * M_PI * (x / width + 0.5f);
  return 0.35875f - 0.48829f * cosf(x) + 0.14128f * cosf(2.0f * x) - 0.01168f * cosf(3.0f * x);
}

/* Compute cumulative distribution function of a discrete function. */
static void compute_cdf(float (*func)(float x), float cdf[FILTER_CDF_TABLE_SIZE])
{
  cdf[0] = 0.0f;
  /* Actual CDF evaluation. */
  for (int u = 0; u < FILTER_CDF_TABLE_SIZE - 1; u++) {
    float x = (float)(u + 1) / (float)(FILTER_CDF_TABLE_SIZE - 1);
    cdf[u + 1] = cdf[u] + func(x - 0.5f); /* [-0.5..0.5]. We resize later. */
  }
  /* Normalize the CDF. */
  for (int u = 0; u < FILTER_CDF_TABLE_SIZE - 1; u++) {
    cdf[u] /= cdf[FILTER_CDF_TABLE_SIZE - 1];
  }
  /* Just to make sure. */
  cdf[FILTER_CDF_TABLE_SIZE - 1] = 1.0f;
}

static void invert_cdf(const float cdf[FILTER_CDF_TABLE_SIZE],
                       float invert_cdf[FILTER_CDF_TABLE_SIZE])
{
  for (int u = 0; u < FILTER_CDF_TABLE_SIZE; u++) {
    float x = (float)u / (float)(FILTER_CDF_TABLE_SIZE - 1);
    for (int i = 0; i < FILTER_CDF_TABLE_SIZE; i++) {
      if (cdf[i] >= x) {
        if (i == FILTER_CDF_TABLE_SIZE - 1) {
          invert_cdf[u] = 1.0f;
        }
        else {
          float t = (x - cdf[i]) / (cdf[i + 1] - cdf[i]);
          invert_cdf[u] = ((float)i + t) / (float)(FILTER_CDF_TABLE_SIZE - 1);
        }
        break;
      }
    }
  }
}

/* Evaluate a discrete function table with linear interpolation. */
static float eval_table(const float *table, float x)
{
  CLAMP(x, 0.0f, 1.0f);
  x = x * (FILTER_CDF_TABLE_SIZE - 1);

  int index = min_ii((int)(x), FILTER_CDF_TABLE_SIZE - 1);
  int nindex = min_ii(index + 1, FILTER_CDF_TABLE_SIZE - 1);
  float t = x - index;

  return (1.0f - t) * table[index] + t * table[nindex];
}

static void eevee_create_cdf_table_temporal_sampling(void)
{
  float *cdf_table = MEM_mallocN(sizeof(float) * FILTER_CDF_TABLE_SIZE, "Eevee Filter CDF table");

  float filter_width = 2.0f; /* Use a 2 pixel footprint by default. */

  {
    /* Use blackman-harris filter. */
    filter_width *= 2.0f;
    compute_cdf(filter_blackman_harris, cdf_table);
  }

  invert_cdf(cdf_table, e_data.inverted_cdf);

  /* Scale and offset table. */
  for (int i = 0; i < FILTER_CDF_TABLE_SIZE; i++) {
    e_data.inverted_cdf[i] = (e_data.inverted_cdf[i] - 0.5f) * filter_width;
  }

  MEM_freeN(cdf_table);
  e_data.inited = true;
}

void EEVEE_temporal_sampling_offset_calc(const double ht_point[2],
                                         const float filter_size,
                                         float r_offset[2])
{
  r_offset[0] = eval_table(e_data.inverted_cdf, (float)(ht_point[0])) * filter_size;
  r_offset[1] = eval_table(e_data.inverted_cdf, (float)(ht_point[1])) * filter_size;
}

void EEVEE_temporal_sampling_matrices_calc(EEVEE_EffectsInfo *effects, const double ht_point[2])
{
  const float *viewport_size = DRW_viewport_size_get();
  const DRWContextState *draw_ctx = DRW_context_state_get();
  Scene *scene = draw_ctx->scene;
  RenderData *rd = &scene->r;

  float persmat[4][4], viewmat[4][4], winmat[4][4], wininv[4][4];
  DRW_view_persmat_get(NULL, persmat, false);
  DRW_view_viewmat_get(NULL, viewmat, false);
  DRW_view_winmat_get(NULL, winmat, false);
  DRW_view_winmat_get(NULL, wininv, true);

  float ofs[2];
  EEVEE_temporal_sampling_offset_calc(ht_point, rd->gauss, ofs);

  if (effects->taa_current_sample > 1) {
    window_translate_m4(winmat, persmat, ofs[0] / viewport_size[0], ofs[1] / viewport_size[1]);
  }

  /* Jitter is in pixel space. Focus distance in world space units. */
  float dof_jitter[2], focus_distance;
  if (EEVEE_depth_of_field_jitter_get(effects, dof_jitter, &focus_distance)) {
    /* Convert to NDC space [-1..1]. */
    dof_jitter[0] /= viewport_size[0] * 0.5f;
    dof_jitter[1] /= viewport_size[1] * 0.5f;

    /* Skew the projection matrix in the ray direction and offset it to ray origin.
     * Make it focus at focus_distance. */
    if (winmat[2][3] != -1.0f) {
      /* Orthographic */
      add_v2_v2(winmat[2], dof_jitter);

      window_translate_m4(
          winmat, persmat, dof_jitter[0] * focus_distance, dof_jitter[1] * focus_distance);
    }
    else {
      /* Get focus distance in NDC. */
      float focus_pt[3] = {0.0f, 0.0f, -focus_distance};
      mul_project_m4_v3(winmat, focus_pt);
      /* Get pixel footprint in view-space. */
      float jitter_scaled[3] = {dof_jitter[0], dof_jitter[1], focus_pt[2]};
      float center[3] = {0.0f, 0.0f, focus_pt[2]};
      mul_project_m4_v3(wininv, jitter_scaled);
      mul_project_m4_v3(wininv, center);

      /* FIXME(fclem): The offset is noticeably large and the culling might make object pop out
       * of the blurring radius. To fix this, use custom enlarged culling matrix. */
      sub_v2_v2v2(jitter_scaled, jitter_scaled, center);
      add_v2_v2(viewmat[3], jitter_scaled);

      window_translate_m4(winmat, persmat, -dof_jitter[0], -dof_jitter[1]);
    }
  }

  BLI_assert(effects->taa_view != NULL);

  /* When rendering just update the view. This avoids recomputing the culling. */
  DRW_view_update_sub(effects->taa_view, viewmat, winmat);
}

void EEVEE_temporal_sampling_update_matrices(EEVEE_Data *vedata)
{
  EEVEE_StorageList *stl = ((EEVEE_Data *)vedata)->stl;
  EEVEE_EffectsInfo *effects = stl->effects;

  double ht_point[2];
  double ht_offset[2] = {0.0, 0.0};
  const uint ht_primes[2] = {2, 3};

  BLI_halton_2d(ht_primes, ht_offset, effects->taa_current_sample - 1, ht_point);

  EEVEE_temporal_sampling_matrices_calc(effects, ht_point);

  DRW_view_set_active(effects->taa_view);
}

void EEVEE_temporal_sampling_reset(EEVEE_Data *vedata)
{
  vedata->stl->effects->taa_render_sample = 1;
  vedata->stl->effects->taa_current_sample = 1;
}

void EEVEE_temporal_sampling_create_view(EEVEE_Data *vedata)
{
  EEVEE_EffectsInfo *effects = vedata->stl->effects;
  /* Create a sub view to disable clipping planes (if any). */
  const DRWView *default_view = DRW_view_default_get();
  float viewmat[4][4], winmat[4][4];
  DRW_view_viewmat_get(default_view, viewmat, false);
  DRW_view_winmat_get(default_view, winmat, false);
  effects->taa_view = DRW_view_create_sub(default_view, viewmat, winmat);
  DRW_view_clip_planes_set(effects->taa_view, NULL, 0);
}

int EEVEE_temporal_sampling_sample_count_get(const Scene *scene, const EEVEE_StorageList *stl)
{
  const bool is_render = DRW_state_is_image_render();
  int sample_count = is_render ? scene->eevee.taa_render_samples : scene->eevee.taa_samples;
  int timesteps = is_render ? stl->g_data->render_timesteps : 1;

  sample_count = max_ii(0, sample_count);
  sample_count = (sample_count == 0) ? TAA_MAX_SAMPLE : sample_count;
  sample_count = divide_ceil_u(sample_count, timesteps);

  int dof_sample_count = EEVEE_depth_of_field_sample_count_get(stl->effects, sample_count, NULL);
  sample_count = dof_sample_count * divide_ceil_u(sample_count, dof_sample_count);
  return sample_count;
}

int EEVEE_temporal_sampling_init(EEVEE_ViewLayerData *UNUSED(sldata), EEVEE_Data *vedata)
{
  EEVEE_StorageList *stl = vedata->stl;
  EEVEE_EffectsInfo *effects = stl->effects;
  int repro_flag = 0;

  if (!e_data.inited) {
    eevee_create_cdf_table_temporal_sampling();
  }

  /**
   * Reset for each "redraw". When rendering using ogl render,
   * we accumulate the redraw inside the drawing loop in eevee_draw_scene().
   */
  if (DRW_state_is_opengl_render()) {
    effects->taa_render_sample = 1;
  }
  effects->bypass_drawing = false;

  EEVEE_temporal_sampling_create_view(vedata);

  const DRWContextState *draw_ctx = DRW_context_state_get();
  const Scene *scene_eval = DEG_get_evaluated_scene(draw_ctx->depsgraph);

  if ((scene_eval->eevee.taa_samples != 1) || DRW_state_is_image_render()) {
    float persmat[4][4];

    if (!DRW_state_is_image_render() && (scene_eval->eevee.flag & SCE_EEVEE_TAA_REPROJECTION)) {
      repro_flag = EFFECT_TAA_REPROJECT | EFFECT_VELOCITY_BUFFER | EFFECT_DEPTH_DOUBLE_BUFFER |
                   EFFECT_DOUBLE_BUFFER | EFFECT_POST_BUFFER;
      effects->taa_reproject_sample = ((effects->taa_reproject_sample + 1) % 16);
    }

    /* Until we support reprojection, we need to make sure
     * that the history buffer contains correct information. */
    bool view_is_valid = stl->g_data->valid_double_buffer;

    view_is_valid = view_is_valid && (stl->g_data->view_updated == false);

    if (draw_ctx->evil_C != NULL) {
      struct wmWindowManager *wm = CTX_wm_manager(draw_ctx->evil_C);
      view_is_valid = view_is_valid && (ED_screen_animation_no_scrub(wm) == NULL);
    }

    effects->taa_total_sample = EEVEE_temporal_sampling_sample_count_get(scene_eval, stl);

    if (EEVEE_renderpasses_only_first_sample_pass_active(vedata)) {
      view_is_valid = false;
      effects->taa_total_sample = 1;
    }

    /* Motion blur steps could reset the sampling when camera is animated (see T79970). */
    if (!DRW_state_is_scene_render()) {
      DRW_view_persmat_get(NULL, persmat, false);
      view_is_valid = view_is_valid && compare_m4m4(persmat, effects->prev_drw_persmat, FLT_MIN);
    }

    /* Prevent ghosting from probe data. */
    view_is_valid = view_is_valid && (effects->prev_drw_support == DRW_state_draw_support()) &&
                    (effects->prev_is_navigating == DRW_state_is_navigating());
    effects->prev_drw_support = DRW_state_draw_support();
    effects->prev_is_navigating = DRW_state_is_navigating();

    if (((effects->taa_total_sample == 0) ||
         (effects->taa_current_sample < effects->taa_total_sample)) ||
        (!view_is_valid) || DRW_state_is_image_render()) {
      if (view_is_valid) {
        /* Viewport rendering updates the matrices in `eevee_draw_scene` */
        if (!DRW_state_is_image_render()) {
          effects->taa_current_sample += 1;
          repro_flag = 0;
        }
      }
      else {
        effects->taa_current_sample = 1;
      }
    }
    else {
      const bool all_shaders_compiled = stl->g_data->queued_shaders_count_prev == 0;
      /* Fix Texture painting (see T79370) and shader compilation (see T78520). */
      if (DRW_state_is_navigating() || !all_shaders_compiled) {
        effects->taa_current_sample = 1;
      }
      else {
        effects->bypass_drawing = true;
      }
    }

    return repro_flag | EFFECT_TAA | EFFECT_DOUBLE_BUFFER | EFFECT_DEPTH_DOUBLE_BUFFER |
           EFFECT_POST_BUFFER;
  }

  effects->taa_current_sample = 1;

  return repro_flag;
}

void EEVEE_temporal_sampling_cache_init(EEVEE_ViewLayerData *sldata, EEVEE_Data *vedata)
{
  EEVEE_PassList *psl = vedata->psl;
  EEVEE_StorageList *stl = vedata->stl;
  EEVEE_TextureList *txl = vedata->txl;
  EEVEE_EffectsInfo *effects = stl->effects;

  if (effects->enabled_effects & EFFECT_TAA) {
    struct GPUShader *sh = EEVEE_shaders_taa_resolve_sh_get(effects->enabled_effects);

    DRW_PASS_CREATE(psl->taa_resolve, DRW_STATE_WRITE_COLOR);
    DRWShadingGroup *grp = DRW_shgroup_create(sh, psl->taa_resolve);

    DRW_shgroup_uniform_texture_ref(grp, "colorHistoryBuffer", &txl->taa_history);
    DRW_shgroup_uniform_texture_ref(grp, "colorBuffer", &effects->source_buffer);
    DRW_shgroup_uniform_block(grp, "common_block", sldata->common_ubo);
    DRW_shgroup_uniform_block(grp, "renderpass_block", sldata->renderpass_ubo.combined);

    if (effects->enabled_effects & EFFECT_TAA_REPROJECT) {
      DefaultTextureList *dtxl = DRW_viewport_texture_list_get();
      DRW_shgroup_uniform_texture_ref(grp, "depthBuffer", &dtxl->depth);
      DRW_shgroup_uniform_mat4(grp, "prevViewProjectionMatrix", effects->prev_drw_persmat);
    }
    else {
      DRW_shgroup_uniform_float(grp, "alpha", &effects->taa_alpha, 1);
    }
    DRW_shgroup_call(grp, DRW_cache_fullscreen_quad_get(), NULL);
  }
}

void EEVEE_temporal_sampling_draw(EEVEE_Data *vedata)
{
  EEVEE_PassList *psl = vedata->psl;
  EEVEE_TextureList *txl = vedata->txl;
  EEVEE_FramebufferList *fbl = vedata->fbl;
  EEVEE_StorageList *stl = vedata->stl;
  EEVEE_EffectsInfo *effects = stl->effects;

  if ((effects->enabled_effects & (EFFECT_TAA | EFFECT_TAA_REPROJECT)) != 0) {
    if ((effects->enabled_effects & EFFECT_TAA) != 0 && effects->taa_current_sample != 1) {
      if (DRW_state_is_image_render()) {
        /* See EEVEE_temporal_sampling_init() for more details. */
        effects->taa_alpha = 1.0f / (float)(effects->taa_render_sample);
      }
      else {
        effects->taa_alpha = 1.0f / (float)(effects->taa_current_sample);
      }

      GPU_framebuffer_bind(effects->target_buffer);
      DRW_draw_pass(psl->taa_resolve);

      /* Restore the depth from sample 1. */
      GPU_framebuffer_blit(fbl->double_buffer_depth_fb, 0, fbl->main_fb, 0, GPU_DEPTH_BIT);

      SWAP_BUFFERS_TAA();
    }
    else {
      /* Save the depth buffer for the next frame.
       * This saves us from doing anything special
       * in the other mode engines. */
      GPU_framebuffer_blit(fbl->main_fb, 0, fbl->double_buffer_depth_fb, 0, GPU_DEPTH_BIT);

      /* Do reprojection for noise reduction */
      /* TODO: do AA jitter if in only render view. */
      if (!DRW_state_is_image_render() && (effects->enabled_effects & EFFECT_TAA_REPROJECT) != 0 &&
          stl->g_data->valid_taa_history) {
        GPU_framebuffer_bind(effects->target_buffer);
        DRW_draw_pass(psl->taa_resolve);
        SWAP_BUFFERS_TAA();
      }
      else {
        struct GPUFrameBuffer *source_fb = (effects->target_buffer == fbl->main_color_fb) ?
                                               fbl->effect_color_fb :
                                               fbl->main_color_fb;
        GPU_framebuffer_blit(source_fb, 0, fbl->taa_history_color_fb, 0, GPU_COLOR_BIT);
      }
    }

    /* Make each loop count when doing a render. */
    if (DRW_state_is_image_render()) {
      effects->taa_render_sample += 1;
      effects->taa_current_sample += 1;
    }
    else {
      if (!DRW_state_is_playback() &&
          ((effects->taa_total_sample == 0) ||
           (effects->taa_current_sample < effects->taa_total_sample))) {
        DRW_viewport_request_redraw();
      }
    }

    DRW_view_persmat_get(NULL, effects->prev_drw_persmat, false);
  }
}
