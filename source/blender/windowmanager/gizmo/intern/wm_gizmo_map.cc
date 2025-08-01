/* SPDX-FileCopyrightText: 2014 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup wm
 */

#include <cstring>

#include "BLI_ghash.h"
#include "BLI_listbase.h"
#include "BLI_math_vector.h"
#include "BLI_math_vector_types.hh"
#include "BLI_rect.h"
#include "BLI_vector.hh"

#include "BKE_context.hh"
#include "BKE_global.hh"
#include "BKE_main.hh"
#include "BKE_screen.hh"

#include "ED_screen.hh"
#include "ED_select_utils.hh"
#include "ED_view3d.hh"

#include "GPU_framebuffer.hh"
#include "GPU_matrix.hh"
#include "GPU_select.hh"
#include "GPU_state.hh"
#include "GPU_viewport.hh"

#include "MEM_guardedalloc.h"

#include "WM_api.hh"
#include "WM_types.hh"
#include "wm_event_system.hh"

/* For tool-tips. */
#include "UI_interface.hh"

#include "DEG_depsgraph.hh"

/* Own includes. */
#include "wm_gizmo_intern.hh"
#include "wm_gizmo_wmapi.hh"

/**
 * Store all gizmo-maps here. Anyone who wants to register a gizmo for a certain
 * area type can query the gizmo-map to do so.
 */
static ListBase gizmomaptypes = {nullptr, nullptr};

/**
 * Update when gizmo-map types change.
 */
/* So operator removal can trigger update. */
enum eWM_GizmoFlagGroupTypeGlobalFlag {
  /** Initialize by #wmGroupType.type_update_flag. */
  WM_GIZMOMAPTYPE_GLOBAL_UPDATE_INIT = (1 << 0),
  /** Remove by #wmGroupType.type_update_flag. */
  WM_GIZMOMAPTYPE_GLOBAL_UPDATE_REMOVE = (1 << 1),

  /** Remove by #wmGroup.tag_remove. */
  WM_GIZMOTYPE_GLOBAL_UPDATE_REMOVE = (1 << 2),

  /**
   * Re-create all gizmos.
   *
   * This is a heavy operation as it clears and re-initializes all gizmos.
   * It should only be performed when internals have been manipulated
   * (such as reloading Python scripts).
   */
  WM_GIZMOTYPE_GLOBAL_REINIT_ALL = (1 << 3),

};
ENUM_OPERATORS(eWM_GizmoFlagGroupTypeGlobalFlag, WM_GIZMOTYPE_GLOBAL_REINIT_ALL)

static eWM_GizmoFlagGroupTypeGlobalFlag wm_gzmap_type_update_flag =
    eWM_GizmoFlagGroupTypeGlobalFlag(0);

/**
 * Gizmo-map update tagging.
 */
enum {
  /** #gizmomap_prepare_drawing has run. */
  GIZMOMAP_IS_PREPARE_DRAW = (1 << 0),
  GIZMOMAP_IS_REFRESH_CALLBACK = (1 << 1),
};

/* -------------------------------------------------------------------- */
/** \name wmGizmoMap Selection Array API
 *
 * Just handle `wm_gizmomap_select_array_*`, not flags or callbacks.
 *
 * \{ */

static void wm_gizmomap_select_array_ensure_len_alloc(wmGizmoMap *gzmap, int len)
{
  wmGizmoMapSelectState *msel = &gzmap->gzmap_context.select;
  if (len <= msel->len_alloc) {
    return;
  }
  msel->items = static_cast<wmGizmo **>(MEM_reallocN(msel->items, sizeof(*msel->items) * len));
  msel->len_alloc = len;
}

void wm_gizmomap_select_array_clear(wmGizmoMap *gzmap)
{
  wmGizmoMapSelectState *msel = &gzmap->gzmap_context.select;
  MEM_SAFE_FREE(msel->items);
  msel->len = 0;
  msel->len_alloc = 0;
}

void wm_gizmomap_select_array_shrink(wmGizmoMap *gzmap, int len_subtract)
{
  wmGizmoMapSelectState *msel = &gzmap->gzmap_context.select;
  msel->len -= len_subtract;
  if (msel->len <= 0) {
    wm_gizmomap_select_array_clear(gzmap);
  }
  else {
    if (msel->len < msel->len_alloc / 2) {
      msel->items = static_cast<wmGizmo **>(
          MEM_reallocN(msel->items, sizeof(*msel->items) * msel->len));
      msel->len_alloc = msel->len;
    }
  }
}

void wm_gizmomap_select_array_push_back(wmGizmoMap *gzmap, wmGizmo *gz)
{
  wmGizmoMapSelectState *msel = &gzmap->gzmap_context.select;
  BLI_assert(msel->len <= msel->len_alloc);
  if (msel->len == msel->len_alloc) {
    msel->len_alloc = (msel->len + 1) * 2;
    msel->items = static_cast<wmGizmo **>(
        MEM_reallocN(msel->items, sizeof(*msel->items) * msel->len_alloc));
  }
  msel->items[msel->len++] = gz;
}

void wm_gizmomap_select_array_remove(wmGizmoMap *gzmap, wmGizmo *gz)
{
  wmGizmoMapSelectState *msel = &gzmap->gzmap_context.select;
  /* Remove gizmo from selected_gizmos array. */
  for (int i = 0; i < msel->len; i++) {
    if (msel->items[i] == gz) {
      for (int j = i; j < (msel->len - 1); j++) {
        msel->items[j] = msel->items[j + 1];
      }
      wm_gizmomap_select_array_shrink(gzmap, 1);
      break;
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name wmGizmoMap
 * \{ */

static wmGizmoMap *wm_gizmomap_new_from_type_ex(wmGizmoMapType *gzmap_type, wmGizmoMap *gzmap)
{
  gzmap->type = gzmap_type;
  gzmap->is_init = true;
  WM_gizmomap_tag_refresh(gzmap);

  /* Create all gizmo-groups for this gizmo-map. We may create an empty one
   * too in anticipation of gizmos from operators etc. */
  LISTBASE_FOREACH (wmGizmoGroupTypeRef *, gzgt_ref, &gzmap_type->grouptype_refs) {
    wm_gizmogroup_new_from_type(gzmap, gzgt_ref->type);
  }

  return gzmap;
}

wmGizmoMap *WM_gizmomap_new_from_type(const wmGizmoMapType_Params *gzmap_params)
{
  wmGizmoMapType *gzmap_type = WM_gizmomaptype_ensure(gzmap_params);
  wmGizmoMap *gzmap = MEM_callocN<wmGizmoMap>("GizmoMap");
  wm_gizmomap_new_from_type_ex(gzmap_type, gzmap);
  return gzmap;
}

static void wm_gizmomap_free_data(wmGizmoMap *gzmap)
{
  /* Clear first so further calls don't waste time trying to maintain correct array state. */
  wm_gizmomap_select_array_clear(gzmap);

  for (wmGizmoGroup *gzgroup = static_cast<wmGizmoGroup *>(gzmap->groups.first), *gzgroup_next;
       gzgroup;
       gzgroup = gzgroup_next)
  {
    gzgroup_next = gzgroup->next;
    BLI_assert(gzgroup->parent_gzmap == gzmap);
    wm_gizmogroup_free(nullptr, gzgroup);
  }
  BLI_assert(BLI_listbase_is_empty(&gzmap->groups));
}

void wm_gizmomap_remove(wmGizmoMap *gzmap)
{
  wm_gizmomap_free_data(gzmap);
  MEM_freeN(gzmap);
}

void WM_gizmomap_reinit(wmGizmoMap *gzmap)
{
  wmGizmoMapType *gzmap_type = gzmap->type;
  wm_gizmomap_free_data(gzmap);
  memset(gzmap, 0x0, sizeof(*gzmap));
  wm_gizmomap_new_from_type_ex(gzmap_type, gzmap);
}

wmGizmoGroup *WM_gizmomap_group_find(wmGizmoMap *gzmap, const char *idname)
{
  wmGizmoGroupType *gzgt = WM_gizmogrouptype_find(idname, false);
  if (gzgt) {
    return WM_gizmomap_group_find_ptr(gzmap, gzgt);
  }
  return nullptr;
}

wmGizmoGroup *WM_gizmomap_group_find_ptr(wmGizmoMap *gzmap, const wmGizmoGroupType *gzgt)
{
  LISTBASE_FOREACH (wmGizmoGroup *, gzgroup, &gzmap->groups) {
    if (gzgroup->type == gzgt) {
      return gzgroup;
    }
  }
  return nullptr;
}

const ListBase *WM_gizmomap_group_list(wmGizmoMap *gzmap)
{
  return &gzmap->groups;
}

bool WM_gizmomap_is_any_selected(const wmGizmoMap *gzmap)
{
  return gzmap->gzmap_context.select.len != 0;
}

wmGizmo *WM_gizmomap_get_modal(const wmGizmoMap *gzmap)
{
  return gzmap->gzmap_context.modal;
}

bool WM_gizmomap_minmax(
    const wmGizmoMap *gzmap, bool /*use_hidden*/, bool use_select, float r_min[3], float r_max[3])
{
  if (use_select) {
    int i;
    for (i = 0; i < gzmap->gzmap_context.select.len; i++) {
      minmax_v3v3_v3(r_min, r_max, gzmap->gzmap_context.select.items[i]->matrix_basis[3]);
    }
    return i != 0;
  }

  bool ok = false;
  BLI_assert_msg(0, "TODO");
  return ok;
}

/**
 * Creates and returns idname hash table for (visible) gizmos in \a gzmap
 *
 * \param poll: Polling function for excluding gizmos.
 * \param data: Custom data passed to \a poll
 *
 * TODO(@ideasman42): this uses unreliable order,
 * best we use an iterator function instead of a hash.
 */
static GHash *WM_gizmomap_gizmo_hash_new(const bContext *C,
                                         wmGizmoMap *gzmap,
                                         bool (*poll)(const wmGizmo *, void *),
                                         void *data,
                                         const eWM_GizmoFlag flag_exclude)
{
  GHash *hash = BLI_ghash_ptr_new(__func__);

  /* Collect gizmos. */
  LISTBASE_FOREACH (wmGizmoGroup *, gzgroup, &gzmap->groups) {
    if (WM_gizmo_group_type_poll(C, gzgroup->type)) {
      LISTBASE_FOREACH (wmGizmo *, gz, &gzgroup->gizmos) {
        if (((flag_exclude == 0) || ((gz->flag & flag_exclude) == 0)) && (!poll || poll(gz, data)))
        {
          BLI_ghash_insert(hash, gz, gz);
        }
      }
    }
  }

  return hash;
}

eWM_GizmoFlagMapDrawStep WM_gizmomap_drawstep_from_gizmo_group(const wmGizmoGroup *gzgroup)
{
  eWM_GizmoFlagMapDrawStep step;
  if (gzgroup->type->flag & WM_GIZMOGROUPTYPE_3D) {
    step = WM_GIZMOMAP_DRAWSTEP_3D;
  }
  else {
    step = WM_GIZMOMAP_DRAWSTEP_2D;
  }
  return step;
}

void WM_gizmomap_tag_refresh_drawstep(wmGizmoMap *gzmap, const eWM_GizmoFlagMapDrawStep drawstep)
{
  BLI_assert(uint(drawstep) < WM_GIZMOMAP_DRAWSTEP_MAX);
  if (gzmap) {
    gzmap->update_flag[drawstep] |= (GIZMOMAP_IS_PREPARE_DRAW | GIZMOMAP_IS_REFRESH_CALLBACK);
    /* This could be split out into a separate tagging function,
     * in practice both when refreshing the highlight should also be updated. */
    gzmap->tag_highlight_pending = true;
  }
}

void WM_gizmomap_tag_refresh(wmGizmoMap *gzmap)
{
  if (gzmap) {
    for (int i = 0; i < WM_GIZMOMAP_DRAWSTEP_MAX; i++) {
      gzmap->update_flag[i] |= (GIZMOMAP_IS_PREPARE_DRAW | GIZMOMAP_IS_REFRESH_CALLBACK);
    }
    /* See code-comment for #WM_gizmomap_tag_refresh_drawstep. */
    gzmap->tag_highlight_pending = true;
  }
}

bool WM_gizmomap_tag_delay_refresh_for_tweak_check(wmGizmoMap *gzmap)
{
  LISTBASE_FOREACH (wmGizmoGroup *, gzgroup, &gzmap->groups) {
    if (gzgroup->hide.delay_refresh_for_tweak) {
      return true;
    }
  }
  return false;
}

static bool gizmo_prepare_drawing(wmGizmoMap *gzmap,
                                  wmGizmo *gz,
                                  const bContext *C,
                                  ListBase *draw_gizmos,
                                  const eWM_GizmoFlagMapDrawStep drawstep)
{
  int do_draw = wm_gizmo_is_visible(gz);
  if (do_draw == 0) {
    /* Skip. */
  }
  else {
    /* Ensure we get RNA updates. */
    if (do_draw & WM_GIZMO_IS_VISIBLE_UPDATE) {
      /* Hover gizmos need updating, even if we don't draw them. */
      wm_gizmo_update(gz, C, (gzmap->update_flag[drawstep] & GIZMOMAP_IS_PREPARE_DRAW) != 0);
    }
    if (do_draw & WM_GIZMO_IS_VISIBLE_DRAW) {
      BLI_addhead(draw_gizmos, BLI_genericNodeN(gz));
    }
    return true;
  }

  return false;
}

/**
 * Update gizmos of \a gzmap to prepare for drawing. Adds all gizmos that
 * should be drawn to list \a draw_gizmos, note that added items need freeing.
 */
static void gizmomap_prepare_drawing(wmGizmoMap *gzmap,
                                     const bContext *C,
                                     ListBase *draw_gizmos,
                                     const eWM_GizmoFlagMapDrawStep drawstep)
{
  if (!gzmap || BLI_listbase_is_empty(&gzmap->groups)) {
    return;
  }

  gzmap->is_init = false;

  wmGizmo *gz_modal = gzmap->gzmap_context.modal;

  /* Allow refresh functions to ask to be refreshed again, clear before the loop below. */
  const bool do_refresh = gzmap->update_flag[drawstep] & GIZMOMAP_IS_REFRESH_CALLBACK;
  gzmap->update_flag[drawstep] &= ~GIZMOMAP_IS_REFRESH_CALLBACK;

  LISTBASE_FOREACH (wmGizmoGroup *, gzgroup, &gzmap->groups) {
    /* Check group visibility - drawstep first to avoid unnecessary call of group poll callback. */
    if (!wm_gizmogroup_is_visible_in_drawstep(gzgroup, drawstep)) {
      continue;
    }

    if (gz_modal && (gzgroup == gz_modal->parent_gzgroup)) {
      if (gzgroup->type->flag & WM_GIZMOGROUPTYPE_DRAW_MODAL_EXCLUDE) {
        continue;
      }
    }
    else { /* Don't poll modal gizmo since some poll functions unlink. */
      if (!WM_gizmo_group_type_poll(C, gzgroup->type)) {
        continue;
      }
      /* When modal only show other gizmo groups tagged with #WM_GIZMOGROUPTYPE_DRAW_MODAL_ALL. */
      if (gz_modal && ((gzgroup->type->flag & WM_GIZMOGROUPTYPE_DRAW_MODAL_ALL) == 0)) {
        continue;
      }
    }

    /* Needs to be initialized on first draw. */
    /* XXX weak: Gizmo-group may skip refreshing if it's invisible
     * (map gets untagged nevertheless). */
    if (do_refresh) {
      /* Force refresh again. */
      gzgroup->init_flag &= ~WM_GIZMOGROUP_INIT_REFRESH;
    }
    /* Calls `setup`, `setup_keymap` and `refresh` if they're defined. */
    WM_gizmogroup_ensure_init(C, gzgroup);

    /* Check after ensure which can run refresh and update this value. */
    if (gzgroup->hide.any != 0) {
      continue;
    }

    /* Prepare drawing. */
    if (gzgroup->type->draw_prepare) {
      gzgroup->type->draw_prepare(C, gzgroup);
    }

    LISTBASE_FOREACH (wmGizmo *, gz, &gzgroup->gizmos) {
      gizmo_prepare_drawing(gzmap, gz, C, draw_gizmos, drawstep);
    }
  }

  gzmap->update_flag[drawstep] &= ~GIZMOMAP_IS_PREPARE_DRAW;
}

/**
 * Draw all visible gizmos in \a gzmap.
 * Uses global draw_gizmos listbase.
 */
static void gizmos_draw_list(const wmGizmoMap *gzmap, const bContext *C, ListBase *draw_gizmos)
{
  /* Can be empty if we're dynamically added and removed. */
  if ((gzmap == nullptr) || BLI_listbase_is_empty(&gzmap->groups)) {
    return;
  }

  /* TODO(@ideasman42): This will need its own shader probably?
   * Don't think it can be handled from that point though. */
  // const bool use_lighting = (U.gizmo_flag & V3D_GIZMO_SHADED) != 0;

  bool is_depth_prev = false;

  /* `draw_gizmos` contains all visible gizmos - draw them. */
  for (LinkData *link = static_cast<LinkData *>(draw_gizmos->first), *link_next; link;
       link = link_next)
  {
    wmGizmo *gz = static_cast<wmGizmo *>(link->data);
    link_next = link->next;

    bool is_depth = (gz->parent_gzgroup->type->flag & WM_GIZMOGROUPTYPE_DEPTH_3D) != 0;

    /* Weak! since we don't 100% support depth yet (select ignores depth)
     * always show highlighted. */
    if (is_depth && (gz->state & WM_GIZMO_STATE_HIGHLIGHT)) {
      is_depth = false;
    }

    if (is_depth == is_depth_prev) {
      /* Pass. */
    }
    else {
      if (is_depth) {
        GPU_depth_test(GPU_DEPTH_LESS_EQUAL);
      }
      else {
        GPU_depth_test(GPU_DEPTH_NONE);
      }
      is_depth_prev = is_depth;
    }

    /* XXX force AntiAlias Gizmos. */
    GPU_line_smooth(true);
    GPU_polygon_smooth(true);

    gz->type->draw(C, gz);

    GPU_line_smooth(false);
    GPU_polygon_smooth(false);

    /* Free/remove gizmo link after drawing. */
    BLI_freelinkN(draw_gizmos, link);
  }

  if (is_depth_prev) {
    GPU_depth_test(GPU_DEPTH_NONE);
  }
}

void WM_gizmomap_draw(wmGizmoMap *gzmap,
                      const bContext *C,
                      const eWM_GizmoFlagMapDrawStep drawstep)
{
  if (!WM_gizmo_context_check_drawstep(C, drawstep)) {
    return;
  }

  ListBase draw_gizmos = {nullptr};

  gizmomap_prepare_drawing(gzmap, C, &draw_gizmos, drawstep);
  gizmos_draw_list(gzmap, C, &draw_gizmos);
  BLI_assert(BLI_listbase_is_empty(&draw_gizmos));
}

static void gizmo_draw_select_3d_loop(const bContext *C,
                                      wmGizmo **visible_gizmos,
                                      const int visible_gizmos_len,
                                      bool *r_use_select_bias)
{
  /* WORKAROUND(#132196): `GPU_DEPTH_NONE` leads to issues with Intel GPU drivers on Windows
   * where camera gizmos cannot be shifted. `glGetQueryObjectuiv` for `GL_SAMPLES_PASSED`
   * seems to return zero in all cases. This might be due to undefined behavior of OpenGL
   * when the depth test is disabled and rendering to a depth render target-only framebuffer.
   * Using `GPU_DEPTH_ALWAYS` fixes the issue. */
  const bool use_intel_gpu_workaround = true;

  /* Set default depth state. */
  GPU_depth_test(use_intel_gpu_workaround ? GPU_DEPTH_ALWAYS : GPU_DEPTH_NONE);
  GPU_depth_mask(true);
  bool is_depth_prev = false;
  bool is_depth_skip_prev = false;

  for (int select_id = 0; select_id < visible_gizmos_len; select_id++) {
    wmGizmo *gz = visible_gizmos[select_id];
    if (gz->type->draw_select == nullptr) {
      continue;
    }

    bool is_depth = (gz->parent_gzgroup->type->flag & WM_GIZMOGROUPTYPE_DEPTH_3D) != 0;
    if (is_depth == is_depth_prev) {
      /* Pass. */
    }
    else {
      if (is_depth) {
        GPU_depth_test(GPU_DEPTH_LESS_EQUAL);
      }
      else {
        GPU_depth_test(use_intel_gpu_workaround ? GPU_DEPTH_ALWAYS : GPU_DEPTH_NONE);
      }
      is_depth_prev = is_depth;
    }
    bool is_depth_skip = (gz->flag & WM_GIZMO_SELECT_BACKGROUND) != 0;
    if (is_depth_skip == is_depth_skip_prev) {
      /* Pass. */
    }
    else {
      GPU_depth_mask(!is_depth_skip);
      is_depth_skip_prev = is_depth_skip;
    }

    if (gz->select_bias != 0.0) {
      *r_use_select_bias = true;
    }

    /* Pass the selection id shifted by 8 bits. Last 8 bits are used for selected gizmo part id. */

    gz->type->draw_select(C, gz, select_id << 8);
  }

  /* Reset depth state. */

  if (is_depth_prev || use_intel_gpu_workaround) {
    GPU_depth_test(GPU_DEPTH_NONE);
  }
  if (is_depth_skip_prev) {
    GPU_depth_mask(true);
  }
}

static int gizmo_find_intersected_3d_intern(wmGizmo **visible_gizmos,
                                            const int visible_gizmos_len,
                                            const bContext *C,
                                            const int co[2],
                                            const int hotspot)
{
  const wmWindowManager *wm = CTX_wm_manager(C);
  ScrArea *area = CTX_wm_area(C);
  ARegion *region = CTX_wm_region(C);
  View3D *v3d = static_cast<View3D *>(area->spacedata.first);
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  rcti rect;
  /* Almost certainly overkill, but allow for many custom gizmos. */
  GPUSelectBuffer buffer;
  short hits;

  BLI_rcti_init_pt_radius(&rect, co, hotspot);

  ED_view3d_draw_setup_view(
      wm, CTX_wm_window(C), depsgraph, CTX_data_scene(C), region, v3d, nullptr, nullptr, &rect);

  bool use_select_bias = false;

  /* TODO: waiting for the GPU in the middle of the event loop for every
   * mouse move is bad for performance, we need to find a solution to not
   * use the GPU or draw something once, see #61474. */
  GPU_select_begin(&buffer, &rect, GPU_SELECT_NEAREST_FIRST_PASS, 0);
  /* Do the drawing. */
  gizmo_draw_select_3d_loop(C, visible_gizmos, visible_gizmos_len, &use_select_bias);

  hits = GPU_select_end();

  if (hits > 0) {
    GPU_select_begin(&buffer, &rect, GPU_SELECT_NEAREST_SECOND_PASS, hits);
    gizmo_draw_select_3d_loop(C, visible_gizmos, visible_gizmos_len, &use_select_bias);
    GPU_select_end();
  }

  ED_view3d_draw_setup_view(
      wm, CTX_wm_window(C), depsgraph, CTX_data_scene(C), region, v3d, nullptr, nullptr, nullptr);

  const blender::Span<GPUSelectResult> hit_results = buffer.storage.as_span().take_front(hits);
  if (use_select_bias && (hits > 1)) {
    float co_direction[3];
    float co_screen[3] = {float(co[0]), float(co[1]), 0.0f};
    ED_view3d_win_to_vector(region, blender::float2{float(co[0]), float(co[1])}, co_direction);

    RegionView3D *rv3d = static_cast<RegionView3D *>(region->regiondata);
    const int viewport[4] = {0, 0, region->winx, region->winy};
    float co_3d_origin[3];

    GPU_matrix_unproject_3fv(co_screen, rv3d->viewinv, rv3d->winmat, viewport, co_3d_origin);

    int hit_found = -1;
    float dot_best = FLT_MAX;

    for (const GPUSelectResult &hit_result : hit_results) {
      BLI_assert(hit_result.id != -1);
      wmGizmo *gz = visible_gizmos[hit_result.id >> 8];
      float co_3d[3];
      co_screen[2] = float(double(hit_result.depth) / double(UINT_MAX));
      GPU_matrix_unproject_3fv(co_screen, rv3d->viewinv, rv3d->winmat, viewport, co_3d);
      float select_bias = gz->select_bias;
      if ((gz->flag & WM_GIZMO_DRAW_NO_SCALE) == 0) {
        select_bias *= gz->scale_final;
      }
      sub_v3_v3(co_3d, co_3d_origin);
      const float dot_test = dot_v3v3(co_3d, co_direction) - select_bias;
      if (dot_best > dot_test) {
        dot_best = dot_test;
        hit_found = hit_result.id;
      }
    }
    return hit_found;
  }

  const GPUSelectResult *hit_near = GPU_select_buffer_near(hit_results);
  return hit_near ? hit_near->id : -1;
}

/**
 * Try to find a 3D gizmo at screen-space coordinate \a co. Uses OpenGL picking.
 */
static wmGizmo *gizmo_find_intersected_3d(bContext *C,
                                          const int co[2],
                                          wmGizmo **visible_gizmos,
                                          const int visible_gizmos_len,
                                          int *r_part)
{
  wmGizmo *result = nullptr;
  int visible_gizmos_len_trim = visible_gizmos_len;
  int hit = -1;

  *r_part = 0;

  /* Set up view matrices. */
  view3d_operator_needs_gpu(C);

  /* Search for 3D gizmo's that use the 2D callback for checking intersections. */
  bool has_3d = false;
  {
    for (int select_id = 0; select_id < visible_gizmos_len; select_id++) {
      wmGizmo *gz = visible_gizmos[select_id];
      /* With both defined, favor the 3D, in case the gizmo can be used in 2D or 3D views. */
      if (gz->type->test_select && (gz->type->draw_select == nullptr)) {
        if ((*r_part = gz->type->test_select(C, gz, co)) != -1) {
          hit = select_id;
          result = gz;
          /* Don't search past this when checking intersections. */
          visible_gizmos_len_trim = select_id;
          break;
        }
      }
      else if (gz->type->draw_select != nullptr) {
        has_3d = true;
      }
    }
  }

  /* Search for 3D intersections if they're before 2D that have been found (if any).
   * This way we always use the first hit. */
  if (has_3d) {

    /* The depth buffer is needed for gizmos to obscure each other. */
    GPUViewport *viewport = WM_draw_region_get_viewport(CTX_wm_region(C));

    /* When switching between modes and the mouse pointer is over a gizmo, the highlight test is
     * performed before the viewport is fully initialized (region->runtime->draw_buffer = nullptr).
     * When this is the case we should not use depth testing. */
    if (viewport == nullptr) {
      return nullptr;
    }
    blender::gpu::Texture *depth_tx = GPU_viewport_depth_texture(viewport);
    GPUFrameBuffer *depth_read_fb = nullptr;
    GPU_framebuffer_ensure_config(&depth_read_fb,
                                  {
                                      GPU_ATTACHMENT_TEXTURE(depth_tx),
                                      GPU_ATTACHMENT_NONE,
                                  });
    GPU_framebuffer_bind(depth_read_fb);

    /* NOTE(@ideasman42): Regarding the hit-radius:
     *
     * - These must remain constant for all event types
     *   since changing the radius per event types means non-motion events
     *   can cause the gizmo not to be highlighted.
     * - A single large radius would result in gizmos that are further away from the cursor
     *   with a nearer Z-depth being highlighted.
     *   So only use the larger radius when the first (smaller) pass has no hits.
     * - As this runs on cursor-motion, avoid doing too many tests (currently 2x).
     */
    const int hotspot_radii[] = {
        /* Use a small value so it's possible to accurately pick a gizmo
         * when multiple are overlapping. */
        int(3.0f * UI_SCALE_FAC),
        /* Use a larger value as a fallback so wire gizmos aren't difficult to click on. */
        int(10.0f * UI_SCALE_FAC),
    };
    for (int i = 0; i < ARRAY_SIZE(hotspot_radii); i++) {
      hit = gizmo_find_intersected_3d_intern(
          visible_gizmos, visible_gizmos_len_trim, C, co, hotspot_radii[i]);
      if (hit != -1) {
        break;
      }
    }

    GPU_framebuffer_restore();
    GPU_framebuffer_free(depth_read_fb);

    if (hit != -1) {
      const int select_id = hit >> 8;
      const int select_part = hit & 0xff;
      BLI_assert(select_id < visible_gizmos_len);
      *r_part = select_part;
      result = visible_gizmos[select_id];
    }
  }

  return result;
}

bool wm_gizmomap_highlight_pending(const wmGizmoMap *gzmap)
{
  return gzmap->tag_highlight_pending;
}
bool wm_gizmomap_highlight_handled(wmGizmoMap *gzmap)
{
  return gzmap->tag_highlight_pending = false;
}

wmGizmo *wm_gizmomap_highlight_find(wmGizmoMap *gzmap,
                                    bContext *C,
                                    const wmEvent *event,
                                    int *r_part)
{
  wmWindowManager *wm = CTX_wm_manager(C);
  wmGizmo *gz = nullptr;
  blender::Vector<wmGizmo *, 128> visible_3d_gizmos;
  bool do_step[WM_GIZMOMAP_DRAWSTEP_MAX];

  int mval[2];
  if (event->val == KM_PRESS_DRAG) {
    WM_event_drag_start_mval(event, CTX_wm_region(C), mval);
  }
  else {
    copy_v2_v2_int(mval, event->mval);
  }

  for (int i = 0; i < ARRAY_SIZE(do_step); i++) {
    do_step[i] = WM_gizmo_context_check_drawstep(C, eWM_GizmoFlagMapDrawStep(i));
  }

  LISTBASE_FOREACH (wmGizmoGroup *, gzgroup, &gzmap->groups) {

    /* If it were important we could initialize here,
     * but this only happens when events are handled before drawing,
     * just skip to keep code-path for initializing gizmos simple. */
    if ((gzgroup->hide.any != 0) || ((gzgroup->init_flag & WM_GIZMOGROUP_INIT_SETUP) == 0)) {
      continue;
    }

    if (WM_gizmo_group_type_poll(C, gzgroup->type)) {
      const eWM_GizmoFlagMapDrawStep step = WM_gizmomap_drawstep_from_gizmo_group(gzgroup);
      if (do_step[step]) {
        if (gzmap->update_flag[step] & GIZMOMAP_IS_REFRESH_CALLBACK) {
          WM_gizmo_group_refresh(C, gzgroup);
          /* Cleared below. */
        }
        if (step == WM_GIZMOMAP_DRAWSTEP_3D) {
          wm_gizmogroup_intersectable_gizmos_to_list(
              wm, gzgroup, event->modifier, &visible_3d_gizmos);
        }
        else if (step == WM_GIZMOMAP_DRAWSTEP_2D) {
          if ((gz = wm_gizmogroup_find_intersected_gizmo(
                   wm, gzgroup, C, event->modifier, mval, r_part)))
          {
            break;
          }
        }
      }
    }
  }

  if (!visible_3d_gizmos.is_empty()) {
    /* 2D gizmos get priority. */
    if (gz == nullptr) {
      gz = gizmo_find_intersected_3d(
          C, mval, visible_3d_gizmos.data(), visible_3d_gizmos.size(), r_part);
    }
  }

  gzmap->update_flag[WM_GIZMOMAP_DRAWSTEP_3D] &= ~GIZMOMAP_IS_REFRESH_CALLBACK;
  gzmap->update_flag[WM_GIZMOMAP_DRAWSTEP_2D] &= ~GIZMOMAP_IS_REFRESH_CALLBACK;

  return gz;
}

void WM_gizmomap_add_handlers(ARegion *region, wmGizmoMap *gzmap)
{
  LISTBASE_FOREACH (wmEventHandler *, handler_base, &region->runtime->handlers) {
    if (handler_base->type == WM_HANDLER_TYPE_GIZMO) {
      wmEventHandler_Gizmo *handler = (wmEventHandler_Gizmo *)handler_base;
      if (handler->gizmo_map == gzmap) {
        return;
      }
    }
  }

  wmEventHandler_Gizmo *handler = MEM_callocN<wmEventHandler_Gizmo>(__func__);
  handler->head.type = WM_HANDLER_TYPE_GIZMO;
  BLI_assert(gzmap == region->runtime->gizmo_map);
  handler->gizmo_map = gzmap;
  BLI_addtail(&region->runtime->handlers, handler);
}

void wm_gizmomaps_handled_modal_update(bContext *C, wmEvent *event, wmEventHandler_Op *handler)
{
  const bool modal_running = (handler->op != nullptr);

  /* Happens on render or when joining areas. */
  if (!handler->context.region || !handler->context.region->runtime->gizmo_map) {
    return;
  }

  wmGizmoMap *gzmap = handler->context.region->runtime->gizmo_map;
  wmGizmo *gz = wm_gizmomap_modal_get(gzmap);
  ScrArea *area = CTX_wm_area(C);
  ARegion *region = CTX_wm_region(C);

  wm_gizmomap_handler_context_op(C, handler);

  /* Regular update for running operator. */
  if (modal_running) {
    wmGizmoOpElem *gzop = gz ? WM_gizmo_operator_get(gz, gz->highlight_part) : nullptr;
    if (gz && gzop && (gzop->type != nullptr) && (gzop->type == handler->op->type)) {
      wmGizmoFnModal modal_fn = gz->custom_modal ? gz->custom_modal : gz->type->modal;
      if (modal_fn != nullptr) {
        const wmOperatorStatus retval = modal_fn(C, gz, event, eWM_GizmoFlagTweak(0));
        OPERATOR_RETVAL_CHECK(retval);
        /* The gizmo is tried to the operator, we can't choose when to exit. */
        BLI_assert(retval & OPERATOR_RUNNING_MODAL);
        UNUSED_VARS_NDEBUG(retval);
      }
    }
  }
  /* Operator not running anymore. */
  else {
    wm_gizmomap_highlight_set(gzmap, C, nullptr, 0);
    if (gz) {
      /* This isn't defined if it ends because of success of cancel, we may want to change. */
      bool cancel = true;
      if (gz->type->exit) {
        gz->type->exit(C, gz, cancel);
      }
      wm_gizmomap_modal_set(gzmap, C, gz, nullptr, false);
    }
  }

  /* Restore the area. */
  CTX_wm_area_set(C, area);
  CTX_wm_region_set(C, region);
}

bool wm_gizmomap_deselect_all(wmGizmoMap *gzmap)
{
  wmGizmoMapSelectState *msel = &gzmap->gzmap_context.select;

  if (msel->items == nullptr || msel->len == 0) {
    return false;
  }

  for (int i = 0; i < msel->len; i++) {
    wm_gizmo_select_set_ex(gzmap, msel->items[i], false, false, true);
  }

  wm_gizmomap_select_array_clear(gzmap);

  /* Always return true, we already checked
   * if there's anything to deselect. */
  return true;
}

static bool gizmo_selectable_poll(const wmGizmo *gz, void * /*data*/)
{
  return (gz->parent_gzgroup->type->flag & WM_GIZMOGROUPTYPE_SELECT);
}

/**
 * Select all selectable gizmos in \a gzmap.
 * \return if selection has changed.
 */
static bool wm_gizmomap_select_all_intern(bContext *C, wmGizmoMap *gzmap)
{
  wmGizmoMapSelectState *msel = &gzmap->gzmap_context.select;
  /* GHash is used here to avoid having to loop over all gizmos twice (once to
   * get tot_sel for allocating, once for actually selecting). Instead we collect
   * selectable gizmos in hash table and use this to get tot_sel and do selection. */

  GHash *hash = WM_gizmomap_gizmo_hash_new(
      C, gzmap, gizmo_selectable_poll, nullptr, WM_GIZMO_HIDDEN | WM_GIZMO_HIDDEN_SELECT);
  GHashIterator gh_iter;
  int i;
  bool changed = false;

  wm_gizmomap_select_array_ensure_len_alloc(gzmap, BLI_ghash_len(hash));

  GHASH_ITER_INDEX (gh_iter, hash, i) {
    wmGizmo *gz_iter = static_cast<wmGizmo *>(BLI_ghashIterator_getValue(&gh_iter));
    WM_gizmo_select_set(gzmap, gz_iter, true);
  }
  /* Highlight first gizmo. */
  wm_gizmomap_highlight_set(gzmap, C, msel->items[0], msel->items[0]->highlight_part);

  BLI_assert(BLI_ghash_len(hash) == msel->len);

  BLI_ghash_free(hash, nullptr, nullptr);
  return changed;
}

bool WM_gizmomap_select_all(bContext *C, wmGizmoMap *gzmap, const int action)
{
  bool changed = false;

  switch (action) {
    case SEL_SELECT:
      changed = wm_gizmomap_select_all_intern(C, gzmap);
      break;
    case SEL_DESELECT:
      changed = wm_gizmomap_deselect_all(gzmap);
      break;
    default:
      BLI_assert_unreachable();
      break;
  }

  if (changed) {
    WM_event_add_mousemove(CTX_wm_window(C));
  }

  return changed;
}

void wm_gizmomap_handler_context_op(bContext *C, wmEventHandler_Op *handler)
{
  bScreen *screen = CTX_wm_screen(C);

  if (screen) {
    ScrArea *area;

    for (area = static_cast<ScrArea *>(screen->areabase.first); area; area = area->next) {
      if (area == handler->context.area) {
        break;
      }
    }
    if (area == nullptr) {
      /* When changing screen layouts with running modal handlers (like render display),
       * this is not an error to print. */
    }
    else {
      ARegion *region;
      CTX_wm_area_set(C, area);
      for (region = static_cast<ARegion *>(area->regionbase.first); region; region = region->next)
      {
        if (region == handler->context.region) {
          break;
        }
      }
      /* XXX no warning print here, after full-area and back regions are remade. */
      if (region) {
        CTX_wm_region_set(C, region);
      }
    }
  }
}

void wm_gizmomap_handler_context_gizmo(bContext * /*C*/, wmEventHandler_Gizmo * /*handler*/)
{
  /* Pass. */
}

bool WM_gizmomap_cursor_set(const wmGizmoMap *gzmap, wmWindow *win)
{
  wmGizmo *gz = gzmap->gzmap_context.highlight;
  if (gz && gz->type->cursor_get) {
    WM_cursor_set(win, gz->type->cursor_get(gz));
    return true;
  }

  return false;
}

bool wm_gizmomap_highlight_set(wmGizmoMap *gzmap, const bContext *C, wmGizmo *gz, int part)
{
  if ((gz != gzmap->gzmap_context.highlight) || (gz && part != gz->highlight_part)) {
    const bool init_last_cursor = !(gzmap->gzmap_context.highlight &&
                                    gzmap->gzmap_context.last_cursor != -1);
    if (gzmap->gzmap_context.highlight) {
      gzmap->gzmap_context.highlight->state &= ~WM_GIZMO_STATE_HIGHLIGHT;
      gzmap->gzmap_context.highlight->highlight_part = -1;
    }

    gzmap->gzmap_context.highlight = gz;

    if (gz) {
      gz->state |= WM_GIZMO_STATE_HIGHLIGHT;
      gz->highlight_part = part;
      if (init_last_cursor) {
        gzmap->gzmap_context.last_cursor = -1;
      }

      if (C && gz->type->cursor_get) {
        wmWindow *win = CTX_wm_window(C);
        if (init_last_cursor) {
          gzmap->gzmap_context.last_cursor = win->cursor;
        }
        WM_cursor_set(win, gz->type->cursor_get(gz));
      }
    }
    else {
      if (C && gzmap->gzmap_context.last_cursor != -1) {
        wmWindow *win = CTX_wm_window(C);
        WM_cursor_set(win, gzmap->gzmap_context.last_cursor);
      }
      gzmap->gzmap_context.last_cursor = -1;
    }

    /* Tag the region for redraw. */
    if (C) {
      ARegion *region = CTX_wm_region(C);
      ED_region_tag_redraw_editor_overlays(region);
    }

    return true;
  }

  return false;
}

wmGizmo *wm_gizmomap_highlight_get(wmGizmoMap *gzmap)
{
  return gzmap->gzmap_context.highlight;
}

void wm_gizmomap_modal_set(
    wmGizmoMap *gzmap, bContext *C, wmGizmo *gz, const wmEvent *event, bool enable)
{
  bool do_refresh = false;

  if (enable) {
    BLI_assert(gzmap->gzmap_context.modal == nullptr);
    wmWindow *win = CTX_wm_window(C);

    WM_tooltip_clear(C, win);

    /* Use even if we don't have invoke, so we can setup data before an operator runs. */
    if (gz->parent_gzgroup->type->invoke_prepare) {
      gz->parent_gzgroup->type->invoke_prepare(C, gz->parent_gzgroup, gz, event);
    }

    if (gz->type->invoke && (gz->type->modal || gz->custom_modal)) {
      const wmOperatorStatus retval = gz->type->invoke(C, gz, event);
      OPERATOR_RETVAL_CHECK(retval);

      if ((retval & OPERATOR_RUNNING_MODAL) == 0) {
        return;
      }
    }

    if (gzmap->gzmap_context.modal != gz) {
      do_refresh = true;
    }
    gz->state |= WM_GIZMO_STATE_MODAL;
    gzmap->gzmap_context.modal = gz;

    if ((gz->flag & WM_GIZMO_MOVE_CURSOR) && (event->tablet.is_motion_absolute == false)) {
      WM_cursor_grab_enable(win, WM_CURSOR_WRAP_XY, nullptr, true);
      copy_v2_v2_int(gzmap->gzmap_context.event_xy, event->xy);
      gzmap->gzmap_context.event_grabcursor = win->grabcursor;
    }
    else {
      gzmap->gzmap_context.event_xy[0] = INT_MAX;
    }

    wmGizmoOpElem *gzop = WM_gizmo_operator_get(gz, gz->highlight_part);
    if (gzop && gzop->type) {
      const wmOperatorStatus retval = WM_gizmo_operator_invoke(C, gz, gzop, event);
      OPERATOR_RETVAL_CHECK(retval);

      if ((retval & OPERATOR_RUNNING_MODAL) == 0) {
        wm_gizmomap_modal_set(gzmap, C, gz, event, false);
      }

      /* We failed to hook the gizmo to the operator handler or operator was canceled, return. */
      if (!gzmap->gzmap_context.modal) {
        gz->state &= ~WM_GIZMO_STATE_MODAL;
        MEM_SAFE_FREE(gz->interaction_data);
      }
    }
  }
  else {
    BLI_assert(ELEM(gzmap->gzmap_context.modal, nullptr, gz));

    /* Deactivate, gizmo but first take care of some stuff. */
    if (gz) {
      gz->state &= ~WM_GIZMO_STATE_MODAL;
      MEM_SAFE_FREE(gz->interaction_data);
    }

    if (gzmap->gzmap_context.modal != nullptr) {
      do_refresh = true;
    }
    gzmap->gzmap_context.modal = nullptr;

    if (C) {
      wmWindow *win = CTX_wm_window(C);
      if (gzmap->gzmap_context.event_xy[0] != INT_MAX) {
        /* Check if some other part of Blender (typically operators)
         * have adjusted the grab mode since it was set.
         * If so: warp, so we have a predictable outcome. */
        if (gzmap->gzmap_context.event_grabcursor == win->grabcursor) {
          WM_cursor_grab_disable(win, gzmap->gzmap_context.event_xy);
        }
        else {
          WM_cursor_warp(win, UNPACK2(gzmap->gzmap_context.event_xy));
        }
      }
      ED_region_tag_redraw_editor_overlays(CTX_wm_region(C));
      WM_event_add_mousemove(win);
    }

    gzmap->gzmap_context.event_xy[0] = INT_MAX;
  }

  if (do_refresh) {
    const int update_flag = GIZMOMAP_IS_REFRESH_CALLBACK;
    const eWM_GizmoFlagMapDrawStep step = WM_gizmomap_drawstep_from_gizmo_group(
        gz->parent_gzgroup);
    gzmap->update_flag[step] |= update_flag;

    /* Ensure the update flag is set for gizmos that were hidden while modal, see #104817. */
    for (int i = 0; i < WM_GIZMOMAP_DRAWSTEP_MAX; i++) {
      const eWM_GizmoFlagMapDrawStep step_iter = (eWM_GizmoFlagMapDrawStep)i;
      if (step_iter == step) {
        continue;
      }
      if ((gzmap->update_flag[i] & update_flag) == update_flag) {
        continue;
      }
      LISTBASE_FOREACH (wmGizmoGroup *, gzgroup, &gzmap->groups) {
        if (((gzgroup->type->flag & WM_GIZMOGROUPTYPE_DRAW_MODAL_ALL) == 0) &&
            wm_gizmogroup_is_visible_in_drawstep(gzgroup, step_iter))
        {
          gzmap->update_flag[i] |= update_flag;
          break;
        }
      }
    }
  }
}

wmGizmo *wm_gizmomap_modal_get(wmGizmoMap *gzmap)
{
  return gzmap->gzmap_context.modal;
}

wmGizmo **wm_gizmomap_selected_get(wmGizmoMap *gzmap, int *r_selected_len)
{
  *r_selected_len = gzmap->gzmap_context.select.len;
  return gzmap->gzmap_context.select.items;
}

ListBase *wm_gizmomap_groups_get(wmGizmoMap *gzmap)
{
  return &gzmap->groups;
}

void WM_gizmomap_message_subscribe(const bContext *C,
                                   wmGizmoMap *gzmap,
                                   ARegion *region,
                                   wmMsgBus *mbus)
{
  LISTBASE_FOREACH (wmGizmoGroup *, gzgroup, &gzmap->groups) {
    if ((gzgroup->hide.any != 0) || (gzgroup->init_flag & WM_GIZMOGROUP_INIT_SETUP) == 0 ||
        !WM_gizmo_group_type_poll(C, gzgroup->type))
    {
      continue;
    }
    LISTBASE_FOREACH (wmGizmo *, gz, &gzgroup->gizmos) {
      if (gz->flag & WM_GIZMO_HIDDEN) {
        continue;
      }
      WM_gizmo_target_property_subscribe_all(gz, mbus, region);
    }
    if (gzgroup->type->message_subscribe != nullptr) {
      gzgroup->type->message_subscribe(C, gzgroup, mbus);
    }
  }
}

/** \} */ /* #wmGizmoMap. */

/* -------------------------------------------------------------------- */
/** \name Tooltip Handling
 * \{ */

ARegion *WM_gizmomap_tooltip_init(
    bContext *C, ARegion *region, int * /*r_pass*/, double * /*pass_delay*/, bool *r_exit_on_event)
{
  wmGizmoMap *gzmap = region->runtime->gizmo_map;
  *r_exit_on_event = false;
  if (gzmap) {
    wmGizmo *gz = gzmap->gzmap_context.highlight;
    if (gz) {
      wmGizmoGroup *gzgroup = gz->parent_gzgroup;
      if ((gzgroup->type->flag & WM_GIZMOGROUPTYPE_3D) != 0) {
        /* On screen area of 3D gizmos may be large, exit on cursor motion. */
        *r_exit_on_event = true;
      }
      return UI_tooltip_create_from_gizmo(C, gz);
    }
  }
  return nullptr;
}

/** \} */ /* #wmGizmoMapType. */

/* -------------------------------------------------------------------- */
/** \name wmGizmoMapType
 * \{ */

wmGizmoMapType *WM_gizmomaptype_find(const wmGizmoMapType_Params *gzmap_params)
{
  LISTBASE_FOREACH (wmGizmoMapType *, gzmap_type, &gizmomaptypes) {
    if (gzmap_type->spaceid == gzmap_params->spaceid &&
        gzmap_type->regionid == gzmap_params->regionid)
    {
      return gzmap_type;
    }
  }

  return nullptr;
}

wmGizmoMapType *WM_gizmomaptype_ensure(const wmGizmoMapType_Params *gzmap_params)
{
  wmGizmoMapType *gzmap_type = WM_gizmomaptype_find(gzmap_params);

  if (gzmap_type) {
    return gzmap_type;
  }

  gzmap_type = MEM_callocN<wmGizmoMapType>("gizmotype list");
  gzmap_type->spaceid = gzmap_params->spaceid;
  gzmap_type->regionid = gzmap_params->regionid;
  BLI_addhead(&gizmomaptypes, gzmap_type);

  return gzmap_type;
}

void wm_gizmomaptypes_free()
{
  for (wmGizmoMapType *gzmap_type = static_cast<wmGizmoMapType *>(gizmomaptypes.first),
                      *gzmap_type_next;
       gzmap_type;
       gzmap_type = gzmap_type_next)
  {
    gzmap_type_next = gzmap_type->next;
    for (wmGizmoGroupTypeRef *
             gzgt_ref = static_cast<wmGizmoGroupTypeRef *>(gzmap_type->grouptype_refs.first),
            *gzgt_next;
         gzgt_ref;
         gzgt_ref = gzgt_next)
    {
      gzgt_next = gzgt_ref->next;
      WM_gizmomaptype_group_free(gzgt_ref);
    }
    MEM_freeN(gzmap_type);
  }
}

void wm_gizmos_keymap(wmKeyConfig *keyconf)
{
  LISTBASE_FOREACH (wmGizmoMapType *, gzmap_type, &gizmomaptypes) {
    LISTBASE_FOREACH (wmGizmoGroupTypeRef *, gzgt_ref, &gzmap_type->grouptype_refs) {
      wm_gizmogrouptype_setup_keymap(gzgt_ref->type, keyconf);
    }
  }

  wm_gizmogroup_tweak_modal_keymap(keyconf);
}

/** \} */ /* #wmGizmoMapType. */

/* -------------------------------------------------------------------- */
/** \name Updates for Dynamic Type Registration
 * \{ */

void WM_gizmoconfig_update_tag_group_type_init(wmGizmoMapType *gzmap_type, wmGizmoGroupType *gzgt)
{
  /* Tag for update on next use. */
  gzmap_type->type_update_flag |= (WM_GIZMOMAPTYPE_UPDATE_INIT | WM_GIZMOMAPTYPE_KEYMAP_INIT);
  gzgt->type_update_flag |= (WM_GIZMOMAPTYPE_UPDATE_INIT | WM_GIZMOMAPTYPE_KEYMAP_INIT);

  wm_gzmap_type_update_flag |= WM_GIZMOMAPTYPE_GLOBAL_UPDATE_INIT;
}

void WM_gizmoconfig_update_tag_group_type_remove(wmGizmoMapType *gzmap_type,
                                                 wmGizmoGroupType *gzgt)
{
  /* Tag for update on next use. */
  gzmap_type->type_update_flag |= WM_GIZMOMAPTYPE_UPDATE_REMOVE;
  gzgt->type_update_flag |= WM_GIZMOMAPTYPE_UPDATE_REMOVE;

  wm_gzmap_type_update_flag |= WM_GIZMOMAPTYPE_GLOBAL_UPDATE_REMOVE;
}

void WM_gizmoconfig_update_tag_group_remove(wmGizmoMap *gzmap)
{
  gzmap->tag_remove_group = true;

  wm_gzmap_type_update_flag |= WM_GIZMOTYPE_GLOBAL_UPDATE_REMOVE;
}

void WM_gizmoconfig_update_tag_reinit_all()
{
  wm_gzmap_type_update_flag |= WM_GIZMOTYPE_GLOBAL_REINIT_ALL;
}

void WM_gizmoconfig_update(Main *bmain)
{
  if (G.background) {
    return;
  }

  if (wm_gzmap_type_update_flag == 0) {
    return;
  }

  if (wm_gzmap_type_update_flag & WM_GIZMOMAPTYPE_GLOBAL_UPDATE_REMOVE) {
    LISTBASE_FOREACH (wmGizmoMapType *, gzmap_type, &gizmomaptypes) {
      if (gzmap_type->type_update_flag & WM_GIZMOMAPTYPE_GLOBAL_UPDATE_REMOVE) {
        gzmap_type->type_update_flag &= ~WM_GIZMOMAPTYPE_UPDATE_REMOVE;
        for (wmGizmoGroupTypeRef *
                 gzgt_ref = static_cast<wmGizmoGroupTypeRef *>(gzmap_type->grouptype_refs.first),
                *gzgt_ref_next;
             gzgt_ref;
             gzgt_ref = gzgt_ref_next)
        {
          gzgt_ref_next = gzgt_ref->next;
          if (gzgt_ref->type->type_update_flag & WM_GIZMOMAPTYPE_UPDATE_REMOVE) {
            gzgt_ref->type->type_update_flag &= ~WM_GIZMOMAPTYPE_UPDATE_REMOVE;
            WM_gizmomaptype_group_unlink(nullptr, bmain, gzmap_type, gzgt_ref->type);
          }
        }
      }
    }

    wm_gzmap_type_update_flag &= ~WM_GIZMOMAPTYPE_GLOBAL_UPDATE_REMOVE;
  }

  if (wm_gzmap_type_update_flag & WM_GIZMOMAPTYPE_GLOBAL_UPDATE_INIT) {
    LISTBASE_FOREACH (wmGizmoMapType *, gzmap_type, &gizmomaptypes) {
      const uchar type_update_all = WM_GIZMOMAPTYPE_UPDATE_INIT | WM_GIZMOMAPTYPE_KEYMAP_INIT;
      if (gzmap_type->type_update_flag & type_update_all) {
        gzmap_type->type_update_flag &= ~eWM_GizmoFlagMapTypeUpdateFlag(type_update_all);
        LISTBASE_FOREACH (wmGizmoGroupTypeRef *, gzgt_ref, &gzmap_type->grouptype_refs) {
          if (gzgt_ref->type->type_update_flag & WM_GIZMOMAPTYPE_KEYMAP_INIT) {
            WM_gizmomaptype_group_init_runtime_keymap(bmain, gzgt_ref->type);
            gzgt_ref->type->type_update_flag &= ~WM_GIZMOMAPTYPE_KEYMAP_INIT;
          }

          if (gzgt_ref->type->type_update_flag & WM_GIZMOMAPTYPE_UPDATE_INIT) {
            WM_gizmomaptype_group_init_runtime(bmain, gzmap_type, gzgt_ref->type);
            gzgt_ref->type->type_update_flag &= ~WM_GIZMOMAPTYPE_UPDATE_INIT;
          }
        }
      }
    }

    wm_gzmap_type_update_flag &= ~WM_GIZMOMAPTYPE_GLOBAL_UPDATE_INIT;
  }

  if (wm_gzmap_type_update_flag & WM_GIZMOTYPE_GLOBAL_UPDATE_REMOVE) {
    for (bScreen *screen = static_cast<bScreen *>(bmain->screens.first); screen;
         screen = static_cast<bScreen *>(screen->id.next))
    {
      LISTBASE_FOREACH (ScrArea *, area, &screen->areabase) {
        LISTBASE_FOREACH (SpaceLink *, sl, &area->spacedata) {
          ListBase *regionbase = (sl == area->spacedata.first) ? &area->regionbase :
                                                                 &sl->regionbase;
          LISTBASE_FOREACH (ARegion *, region, regionbase) {
            wmGizmoMap *gzmap = region->runtime->gizmo_map;
            if (gzmap != nullptr && gzmap->tag_remove_group) {
              gzmap->tag_remove_group = false;

              for (wmGizmoGroup *gzgroup = static_cast<wmGizmoGroup *>(gzmap->groups.first),
                                *gzgroup_next;
                   gzgroup;
                   gzgroup = gzgroup_next)
              {
                gzgroup_next = gzgroup->next;
                if (gzgroup->tag_remove) {
                  wm_gizmogroup_free(nullptr, gzgroup);
                  ED_region_tag_redraw_editor_overlays(region);
                }
              }
            }
          }
        }
      }
    }
    wm_gzmap_type_update_flag &= ~WM_GIZMOTYPE_GLOBAL_UPDATE_REMOVE;
  }

  if (wm_gzmap_type_update_flag & WM_GIZMOTYPE_GLOBAL_REINIT_ALL) {
    WM_reinit_gizmomap_all(bmain);
    wm_gzmap_type_update_flag &= ~WM_GIZMOTYPE_GLOBAL_REINIT_ALL;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Recreate All Gizmos
 *
 * Use when adjusting themes.
 *
 * \{ */

void WM_reinit_gizmomap_all(Main *bmain)
{
  for (bScreen *screen = static_cast<bScreen *>(bmain->screens.first); screen;
       screen = static_cast<bScreen *>(screen->id.next))
  {
    LISTBASE_FOREACH (ScrArea *, area, &screen->areabase) {
      LISTBASE_FOREACH (SpaceLink *, sl, &area->spacedata) {
        ListBase *regionbase = (sl == area->spacedata.first) ? &area->regionbase : &sl->regionbase;
        LISTBASE_FOREACH (ARegion *, region, regionbase) {
          wmGizmoMap *gzmap = region->runtime->gizmo_map;
          if ((gzmap != nullptr) && (gzmap->is_init == false)) {
            WM_gizmomap_reinit(gzmap);

            /* Without a redraw elements can fail to activate
             * (such as the 2D viewport buttons). */
            ED_region_tag_redraw(region);
          }
        }
      }
    }
  }
}

/** \} */
