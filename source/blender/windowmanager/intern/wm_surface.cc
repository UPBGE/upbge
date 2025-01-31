/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup wm
 */

#include "BLI_listbase.h"
#ifndef NDEBUG
#  include "BLI_threads.h"
#endif

#include "BKE_context.hh"

#include "DNA_scene_types.h"

#include "GPU_context.hh"
#include "GPU_framebuffer.hh"

#include "MEM_guardedalloc.h"

#include "WM_api.hh"
#include "WM_types.hh"

#include "wm_surface.hh"

static ListBase global_surface_list = {nullptr, nullptr};
static wmSurface *g_drawable = nullptr;

void wm_surfaces_iter(bContext *C, void (*cb)(bContext *C, wmSurface *))
{
  /* Mutable iterator in case a surface is freed. */
  LISTBASE_FOREACH_MUTABLE (wmSurface *, surf, &global_surface_list) {
    cb(C, surf);
  }
}

static void wm_surface_do_depsgraph_fn(bContext *C, wmSurface *surface)
{
  if (surface->do_depsgraph) {
    surface->do_depsgraph(C);
  }
}

void wm_surfaces_do_depsgraph(bContext *C)
{
  wm_surfaces_iter(C, wm_surface_do_depsgraph_fn);
}

void wm_surface_clear_drawable(bContext *C)
{
  if (g_drawable) {
    bool is_game_xr_session = false;
    if (C != nullptr) {
      Scene *scene = CTX_data_scene(C);
      if (scene->flag & SCE_IS_GAME_XR_SESSION) {
        is_game_xr_session = true;
      }
    }
    if (!is_game_xr_session) {
      WM_system_gpu_context_release(g_drawable->system_gpu_context);
      GPU_context_active_set(nullptr);

      if (g_drawable->deactivate) {
        g_drawable->deactivate();
      }
    }

    g_drawable = nullptr;
  }
}

void wm_surface_set_drawable(wmSurface *surface, bContext *C, bool activate)
{
  BLI_assert(ELEM(g_drawable, nullptr, surface));

  bool is_game_xr_session = false;
  if (C != nullptr) {
    Scene *scene = CTX_data_scene(C);
    if (scene->flag & SCE_IS_GAME_XR_SESSION) {
      is_game_xr_session = true;
    }
  }

  g_drawable = surface;
  if (activate) {
    if (surface->activate && !is_game_xr_session) {
      surface->activate();
    }
    WM_system_gpu_context_activate(surface->system_gpu_context);
  }

  GPU_context_active_set(surface->blender_gpu_context);
}

void wm_surface_make_drawable(wmSurface *surface, bContext *C)
{
  BLI_assert(GPU_framebuffer_active_get() == GPU_framebuffer_back_get());

  if (surface != g_drawable) {
    wm_surface_clear_drawable(C);
    wm_surface_set_drawable(surface, C, true);
  }
}

void wm_surface_reset_drawable(bContext *C)
{
  BLI_assert(BLI_thread_is_main());
  BLI_assert(GPU_framebuffer_active_get() == GPU_framebuffer_back_get());

  if (g_drawable) {
    wm_surface_clear_drawable(C);
    wm_surface_set_drawable(g_drawable, C, true);
  }
}

void wm_surface_add(wmSurface *surface)
{
  BLI_addtail(&global_surface_list, surface);
}

void wm_surface_remove(wmSurface *surface, bContext *C)
{
  BLI_remlink(&global_surface_list, surface);
  /* Ensure GPU context is bound to free GPU resources. */
  wm_surface_make_drawable(surface, C);
  surface->free_data(surface);
  wm_surface_clear_drawable(C);
  MEM_freeN(surface);
}

void wm_surfaces_free()
{
  LISTBASE_FOREACH_MUTABLE (wmSurface *, surf, &global_surface_list) {
    wm_surface_remove(surf, nullptr);
  }

  BLI_assert(BLI_listbase_is_empty(&global_surface_list));
}
