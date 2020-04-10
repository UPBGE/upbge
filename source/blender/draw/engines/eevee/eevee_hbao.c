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
 * Copyright 2016, Blender Foundation.
 */

/** \file
 * \ingroup draw_engine
 *
 * Implementation of simple hbao from https://github.com/scanberg/hbao/tree/master/resources/shaders.
 */

#include "DRW_render.h"

#include "DNA_camera_types.h"

#include "BKE_camera.h"

#include "DEG_depsgraph_query.h"

#include "eevee_private.h"

#include "GPU_shader.h"

static struct {
  /* HBAO */
  struct GPUShader *hbao_sh;
  struct GPUShader *hbao_blurx_sh;
  struct GPUShader *hbao_blury_sh;
  struct GPUShader *hbao_composite_sh;

  struct GPUTexture *hbao_tx;
} e_data = {NULL}; /* Engine data */

extern char datatoc_effect_hbao_frag_glsl[];
extern char datatoc_effect_hbao_blurx_frag_glsl[];
extern char datatoc_effect_hbao_blury_frag_glsl[];
extern char datatoc_effect_hbao_composite_frag_glsl[];

static void eevee_create_shader_hbao(void)
{
  if (!e_data.hbao_sh) {
    e_data.hbao_sh = DRW_shader_create_fullscreen(datatoc_effect_hbao_frag_glsl, NULL);
    e_data.hbao_blurx_sh = DRW_shader_create_fullscreen(datatoc_effect_hbao_blurx_frag_glsl, NULL);
    e_data.hbao_blury_sh = DRW_shader_create_fullscreen(datatoc_effect_hbao_blury_frag_glsl, NULL);
    e_data.hbao_composite_sh = DRW_shader_create_fullscreen(
        datatoc_effect_hbao_composite_frag_glsl, NULL);
  }
}

int EEVEE_hbao_init(EEVEE_ViewLayerData *sldata, EEVEE_Data *vedata)
{
  EEVEE_FramebufferList *fbl = vedata->fbl;

  const DRWContextState *draw_ctx = DRW_context_state_get();
  const Scene *scene_eval = DEG_get_evaluated_scene(draw_ctx->depsgraph);

  View3D *v3d = draw_ctx->v3d;
  Object *obcam = v3d->camera;

  if (!obcam) {
    return 0;
  }

  if (scene_eval->eevee.flag & SCE_EEVEE_HBAO_ENABLED) {

    /* Shaders */
    eevee_create_shader_hbao();

    DRW_texture_ensure_fullscreen_2d(
        &e_data.hbao_tx, GPU_RGBA16F, DRW_TEX_FILTER | DRW_TEX_MIPMAP);
    GPU_framebuffer_ensure_config(
        &fbl->hbao_fb, {GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(e_data.hbao_tx)});
    GPU_framebuffer_ensure_config(&fbl->hbao_blurx_fb,
                                  {GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(e_data.hbao_tx)});
    GPU_framebuffer_ensure_config(&fbl->hbao_blury_fb,
                                  {GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(e_data.hbao_tx)});

    float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    GPU_texture_clear(e_data.hbao_tx, GPU_DATA_FLOAT, clear);

    return EFFECT_HBAO;
  }

  /* Cleanup */
  GPU_FRAMEBUFFER_FREE_SAFE(fbl->hbao_fb);
  GPU_FRAMEBUFFER_FREE_SAFE(fbl->hbao_blurx_fb);
  GPU_FRAMEBUFFER_FREE_SAFE(fbl->hbao_blury_fb);

  return 0;
}

void EEVEE_hbao_cache_init(EEVEE_ViewLayerData *sldata, EEVEE_Data *vedata)
{
  EEVEE_PassList *psl = vedata->psl;
  EEVEE_EffectsInfo *effects = vedata->stl->effects;
  DefaultTextureList *dtxl = DRW_viewport_texture_list_get();

  const DRWContextState *draw_ctx = DRW_context_state_get();
  View3D *v3d = draw_ctx->v3d;
  Object *obcam = v3d->camera;
  Scene *scene_eval = draw_ctx->scene;

  if (!obcam) {
    return;
  }

  if ((effects->enabled_effects & EFFECT_HBAO) != 0) {

    Camera *cam = (Camera *)obcam->data;

    const float *size = DRW_viewport_size_get();
    DRW_PASS_CREATE(psl->hbao_ps, DRW_STATE_WRITE_COLOR);
    DRWShadingGroup *grp = DRW_shgroup_create(e_data.hbao_sh, psl->hbao_ps);
    DRW_shgroup_uniform_texture(grp, "bgl_NoiseTex", EEVEE_materials_get_noise_tex());
    DRW_shgroup_uniform_texture_ref(grp, "bgl_DepthTexture", &dtxl->depth);
    DRW_shgroup_uniform_float(grp, "bgl_RenderedTextureWidth", (float *)&size[0], 1);
    DRW_shgroup_uniform_float(grp, "bgl_RenderedTextureHeight", (float *)&size[1], 1);
    DRW_shgroup_uniform_float(grp, "near", (float *)&cam->clip_start, 1);
    DRW_shgroup_uniform_float(grp, "far", (float *)&cam->clip_end, 1);
    DRW_shgroup_uniform_float(grp, "flen", (float *)&cam->lens, 1);
    DRW_shgroup_uniform_float(grp, "AOStrength", (float *)&scene_eval->eevee.hbao_strength, 1);
    DRW_shgroup_call_procedural_triangles(grp, NULL, 1);

    DRW_PASS_CREATE(psl->hbao_blurx_ps, DRW_STATE_WRITE_COLOR);
    grp = DRW_shgroup_create(e_data.hbao_blurx_sh, psl->hbao_blurx_ps);
    DRW_shgroup_uniform_texture_ref(grp, "bufA", &e_data.hbao_tx);
    DRW_shgroup_uniform_float(grp, "bgl_RenderedTextureWidth", (float *)&size[0], 1);
    DRW_shgroup_uniform_float(grp, "bgl_RenderedTextureHeight", (float *)&size[1], 1);
    DRW_shgroup_call_procedural_triangles(grp, NULL, 1);

    DRW_PASS_CREATE(psl->hbao_blury_ps, DRW_STATE_WRITE_COLOR);
    grp = DRW_shgroup_create(e_data.hbao_blury_sh, psl->hbao_blury_ps);
    DRW_shgroup_uniform_texture_ref(grp, "bufB", &e_data.hbao_tx);
    DRW_shgroup_uniform_float(grp, "bgl_RenderedTextureWidth", (float *)&size[0], 1);
    DRW_shgroup_uniform_float(grp, "bgl_RenderedTextureHeight", (float *)&size[1], 1);
    DRW_shgroup_call_procedural_triangles(grp, NULL, 1);

    DRW_PASS_CREATE(psl->hbao_composite_ps, DRW_STATE_WRITE_COLOR);
    grp = DRW_shgroup_create(e_data.hbao_composite_sh, psl->hbao_composite_ps);
    DRW_shgroup_uniform_texture_ref(grp, "bufC", &e_data.hbao_tx);
    DRW_shgroup_uniform_texture_ref(grp, "bgl_RenderedTexture", &effects->source_buffer);
    DRW_shgroup_call_procedural_triangles(grp, NULL, 1);
  }
}

void EEVEE_hbao_compute(EEVEE_ViewLayerData *UNUSED(sldata), EEVEE_Data *vedata)
{
  EEVEE_PassList *psl = vedata->psl;
  EEVEE_FramebufferList *fbl = vedata->fbl;
  EEVEE_EffectsInfo *effects = vedata->stl->effects;
  EEVEE_TextureList *txl = vedata->txl;

  const DRWContextState *draw_ctx = DRW_context_state_get();
  View3D *v3d = draw_ctx->v3d;
  Object *obcam = v3d->camera;

  if (!obcam) {
    return;
  }

  if ((effects->enabled_effects & EFFECT_HBAO) != 0) {

    GPU_framebuffer_bind(fbl->hbao_fb);
    DRW_draw_pass(psl->hbao_ps);
    GPU_framebuffer_bind(fbl->hbao_blurx_fb);
    DRW_draw_pass(psl->hbao_blurx_ps);
    GPU_framebuffer_bind(fbl->hbao_blury_fb);
    DRW_draw_pass(psl->hbao_blury_ps);
    GPU_framebuffer_bind(effects->target_buffer);
    DRW_draw_pass(psl->hbao_composite_ps);

    SWAP_BUFFERS();

    GPU_framebuffer_texture_detach(fbl->hbao_fb, e_data.hbao_tx);
    GPU_framebuffer_texture_detach(fbl->hbao_blurx_fb, e_data.hbao_tx);
    GPU_framebuffer_texture_detach(fbl->hbao_blury_fb, e_data.hbao_tx);
  }
}

void EEVEE_hbao_free(void)
{
  DRW_SHADER_FREE_SAFE(e_data.hbao_sh);
  DRW_SHADER_FREE_SAFE(e_data.hbao_blurx_sh);
  DRW_SHADER_FREE_SAFE(e_data.hbao_blury_sh);
  DRW_SHADER_FREE_SAFE(e_data.hbao_composite_sh);

  DRW_TEXTURE_FREE_SAFE(e_data.hbao_tx);
}
