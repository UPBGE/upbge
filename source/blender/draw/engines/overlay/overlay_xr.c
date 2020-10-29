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
 * Copyright 2019, Blender Foundation.
 */

/** \file
 * \ingroup draw_engine
 */

#include "DRW_render.h"

#include "GPU_batch.h"

#include "overlay_private.h"

void OVERLAY_xr_cache_init(OVERLAY_Data *vedata)
{
  OVERLAY_PassList *psl = vedata->psl;
  OVERLAY_PrivateData *pd = vedata->stl->pd;
  DRWShadingGroup *grp;
  const float color[4] = {0.211f, 0.219f, 0.223f, 0.4f};

  DRWState state = DRW_STATE_WRITE_DEPTH | DRW_STATE_WRITE_COLOR | DRW_STATE_DEPTH_LESS_EQUAL |
                   DRW_STATE_BLEND_ALPHA;
  DRW_PASS_CREATE(psl->xr_controllers_ps, state | pd->clipping_state);

  GPUShader *sh = OVERLAY_shader_uniform_color();
  pd->xr_controllers_grp = grp = DRW_shgroup_create(sh, psl->xr_controllers_ps);
  DRW_shgroup_uniform_vec4_copy(grp, "color", color);
}

void OVERLAY_xr_cache_populate(OVERLAY_Data *vedata, Object *ob)
{
  OVERLAY_PrivateData *pd = vedata->stl->pd;
  GPUBatch *xr_controllers = DRW_cache_mesh_all_verts_get(ob);

  if (xr_controllers && xr_controllers->verts[0] &&
      (GPU_vertbuf_get_vertex_len(xr_controllers->verts[0]) > 0)) {
    DRW_shgroup_call_obmat(pd->xr_controllers_grp, xr_controllers, ob->obmat);
  }
  else {
    /* Fallback to primitive sphere. */
    const float scale[3] = {0.05f, 0.05f, 0.05f};
    float obmat[4][4];
    copy_m4_m4(obmat, ob->obmat);
    rescale_m4(obmat, scale);
    DRW_shgroup_call_obmat(pd->xr_controllers_grp, DRW_cache_sphere_get(), obmat);
  }
}

void OVERLAY_xr_draw(OVERLAY_Data *vedata)
{
  const DRWContextState *draw_ctx = DRW_context_state_get();
  if ((draw_ctx->v3d->flag2 & V3D_XR_SHOW_CONTROLLERS) == 0) {
    return;
  }

  OVERLAY_PassList *psl = vedata->psl;
  DefaultFramebufferList *dfbl = DRW_viewport_framebuffer_list_get();

  if (DRW_state_is_fbo()) {
    GPU_framebuffer_bind(dfbl->overlay_fb);
  }

  DRW_draw_pass(psl->xr_controllers_ps);
}
