/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup wm
 *
 * Tool-system used to define tools in Blender's toolbar.
 * See: `./scripts/startup/bl_ui/space_toolsystem_common.py`, `ToolDef` for a detailed
 * description of tool definitions.
 *
 * \note Tools are stored per workspace.
 * Notice many functions take #Main & #WorkSpace and *not* window/screen/scene data.
 * This is intentional as changing tools must account for all scenes using that workspace.
 * Functions that refreshes on tool change are responsible for updating all windows using
 * this workspace.
 */

#include <cstring>

#include "CLG_log.h"

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_utildefines.h"

#include "DNA_ID.h"
#include "DNA_brush_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_space_types.h"
#include "DNA_windowmanager_types.h"
#include "DNA_workspace_types.h"

#include "BKE_asset_edit.hh"
#include "BKE_brush.hh"
#include "BKE_context.hh"
#include "BKE_global.hh"
#include "BKE_idprop.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_paint.hh"
#include "BKE_paint_types.hh"
#include "BKE_workspace.hh"

#include "RNA_access.hh"
#include "RNA_enum_types.hh"

#include "WM_api.hh"
#include "WM_message.hh"
#include "WM_toolsystem.hh" /* Own include. */
#include "WM_types.hh"

static void toolsystem_reinit_with_toolref(bContext *C, WorkSpace * /*workspace*/, bToolRef *tref);
static bToolRef *toolsystem_reinit_ensure_toolref(bContext *C,
                                                  WorkSpace *workspace,
                                                  const bToolKey *tkey,
                                                  const char *default_tool);
static void toolsystem_refresh_screen_from_active_tool(Main *bmain,
                                                       WorkSpace *workspace,
                                                       bToolRef *tref);
static void toolsystem_ref_set_by_brush_type(bContext *C, const char *brush_type);

static void toolsystem_ref_set_by_id_pending(Main *bmain,
                                             bToolRef *tref,
                                             const char *idname_pending);

/* -------------------------------------------------------------------- */
/** \name Tool Reference API
 * \{ */

bToolRef *WM_toolsystem_ref_from_context(const bContext *C)
{
  WorkSpace *workspace = CTX_wm_workspace(C);
  if (workspace == nullptr) {
    return nullptr;
  }
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  ScrArea *area = CTX_wm_area(C);
  if ((area == nullptr) || ((1 << area->spacetype) & WM_TOOLSYSTEM_SPACE_MASK) == 0) {
    return nullptr;
  }
  bToolKey tkey{};
  tkey.space_type = area->spacetype;
  tkey.mode = WM_toolsystem_mode_from_spacetype(scene, view_layer, area, area->spacetype);
  bToolRef *tref = WM_toolsystem_ref_find(workspace, &tkey);
  /* We could return 'area->runtime.tool' in this case. */
  if (area->runtime.is_tool_set) {
    BLI_assert(tref == area->runtime.tool);
  }
  return tref;
}

bToolRef_Runtime *WM_toolsystem_runtime_from_context(const bContext *C)
{
  bToolRef *tref = WM_toolsystem_ref_from_context(C);
  return tref ? tref->runtime : nullptr;
}

bToolRef *WM_toolsystem_ref_find(WorkSpace *workspace, const bToolKey *tkey)
{
  BLI_assert((1 << tkey->space_type) & WM_TOOLSYSTEM_SPACE_MASK);
  LISTBASE_FOREACH (bToolRef *, tref, &workspace->tools) {
    if ((tref->space_type == tkey->space_type) && (tref->mode == tkey->mode)) {
      return tref;
    }
  }
  return nullptr;
}

bToolRef_Runtime *WM_toolsystem_runtime_find(WorkSpace *workspace, const bToolKey *tkey)
{
  bToolRef *tref = WM_toolsystem_ref_find(workspace, tkey);
  return tref ? tref->runtime : nullptr;
}

bool WM_toolsystem_ref_ensure(WorkSpace *workspace, const bToolKey *tkey, bToolRef **r_tref)
{
  bToolRef *tref = WM_toolsystem_ref_find(workspace, tkey);
  if (tref) {
    *r_tref = tref;
    return false;
  }
  tref = MEM_callocN<bToolRef>(__func__);
  BLI_addhead(&workspace->tools, tref);
  tref->space_type = tkey->space_type;
  tref->mode = tkey->mode;
  *r_tref = tref;
  return true;
}

/**
 * Similar to #toolsystem_active_tool_from_context_or_view3d(), but returns the tool key only.
 */
static bToolKey toolsystem_key_from_context_or_view3d(const Scene *scene,
                                                      ViewLayer *view_layer,
                                                      ScrArea *area)
{
  bToolKey tkey{};

  if (area && ((1 << area->spacetype) & WM_TOOLSYSTEM_SPACE_MASK)) {
    tkey.space_type = area->spacetype;
    tkey.mode = WM_toolsystem_mode_from_spacetype(scene, view_layer, area, area->spacetype);
    return tkey;
  }

  /* Otherwise: Fallback to getting the active tool for 3D views. */
  tkey.space_type = SPACE_VIEW3D;
  tkey.mode = WM_toolsystem_mode_from_spacetype(scene, view_layer, nullptr, SPACE_VIEW3D);
  return tkey;
}

/**
 * Get the active tool for the current context (space and mode) if the current space supports tools
 * or, fallback to the active tool of the 3D View in the current mode.
 *
 * Use this instead of #WM_toolsystem_ref_from_context() when usage from properties editors should
 * be possible, which shows tool settings of the 3D View.
 */
static const bToolRef *toolsystem_active_tool_from_context_or_view3d(const bContext *C)
{
  /* Current space & mode has its own active tool, use that. */
  const ScrArea *area = CTX_wm_area(C);
  if (area && ((1 << area->spacetype) & WM_TOOLSYSTEM_SPACE_MASK)) {
    return WM_toolsystem_ref_from_context(C);
  }

  /* Otherwise: Fallback to getting the active tool for 3D views. */
  WorkSpace *workspace = CTX_wm_workspace(C);
  if (workspace == nullptr) {
    return nullptr;
  }
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  bToolKey tkey{};
  tkey.space_type = SPACE_VIEW3D;
  tkey.mode = WM_toolsystem_mode_from_spacetype(scene, view_layer, nullptr, SPACE_VIEW3D);
  return WM_toolsystem_ref_find(workspace, &tkey);
}

/** \} */

static void toolsystem_unlink_ref(bContext *C, WorkSpace * /*workspace*/, bToolRef *tref)
{
  bToolRef_Runtime *tref_rt = tref->runtime;

  if (tref_rt->gizmo_group[0]) {
    wmGizmoGroupType *gzgt = WM_gizmogrouptype_find(tref_rt->gizmo_group, false);
    if (gzgt != nullptr) {
      Main *bmain = CTX_data_main(C);
      WM_gizmo_group_remove_by_tool(C, bmain, gzgt, tref);
    }
  }
}
void WM_toolsystem_unlink(bContext *C, WorkSpace *workspace, const bToolKey *tkey)
{
  bToolRef *tref = WM_toolsystem_ref_find(workspace, tkey);
  if (tref && tref->runtime) {
    toolsystem_unlink_ref(C, workspace, tref);
  }
}

/* -------------------------------------------------------------------- */
/** \name Brush Tools
 * \{ */

static const char *brush_type_identifier_get(const int brush_type, const PaintMode paint_mode)
{
  const EnumPropertyItem *type_enum = BKE_paint_get_tool_enum_from_paintmode(paint_mode);
  const int item_idx = RNA_enum_from_value(type_enum, brush_type);
  if (item_idx == -1) {
    return "";
  }
  return type_enum[item_idx].identifier;
}

static bool brush_type_matches_active_tool(bContext *C, const int brush_type)
{
  const bToolRef *active_tool = toolsystem_active_tool_from_context_or_view3d(C);

  if (active_tool->runtime == nullptr) {
    /* Should only ever be null in background mode. */
    BLI_assert(G.background);
    return false;
  }

  if (!(active_tool->runtime->flag & TOOLREF_FLAG_USE_BRUSHES)) {
    return false;
  }

  BLI_assert(BKE_paintmode_get_active_from_context(C) == BKE_paintmode_get_from_tool(active_tool));
  return active_tool->runtime->brush_type == brush_type;
}

static NamedBrushAssetReference *toolsystem_brush_type_binding_lookup(const Paint *paint,
                                                                      const char *brush_type_name)
{
  return static_cast<NamedBrushAssetReference *>(
      BLI_findstring_ptr(&paint->tool_brush_bindings.active_brush_per_brush_type,
                         brush_type_name,
                         offsetof(NamedBrushAssetReference, name)));
}

/**
 * Update the bindings so the main brush reference matches the currently active brush.
 */
static void toolsystem_main_brush_binding_update_from_active(Paint *paint)
{
  MEM_delete(paint->tool_brush_bindings.main_brush_asset_reference);
  paint->tool_brush_bindings.main_brush_asset_reference = nullptr;

  if (paint->brush != nullptr) {
    if (std::optional<AssetWeakReference> brush_asset_reference =
            blender::bke::asset_edit_weak_reference_from_id(paint->brush->id))
    {
      paint->tool_brush_bindings.main_brush_asset_reference = MEM_new<AssetWeakReference>(
          __func__, *brush_asset_reference);
    }
  }
}

static void toolsystem_brush_type_binding_update(Paint *paint,
                                                 const PaintMode paint_mode,
                                                 const int brush_type)
{
  if (paint->brush == nullptr) {
    return;
  }
  const char *brush_type_name = brush_type_identifier_get(brush_type, paint_mode);
  if (!brush_type_name || !brush_type_name[0]) {
    return;
  }

  /* Update existing reference. */
  if (NamedBrushAssetReference *existing_brush_ref = toolsystem_brush_type_binding_lookup(
          paint, brush_type_name))
  {
    MEM_delete(existing_brush_ref->brush_asset_reference);
    existing_brush_ref->brush_asset_reference = MEM_new<AssetWeakReference>(
        __func__, *paint->brush_asset_reference);
  }
  /* Add new reference. */
  else {
    NamedBrushAssetReference *new_brush_ref = MEM_callocN<NamedBrushAssetReference>(__func__);

    new_brush_ref->name = BLI_strdup(brush_type_name);
    new_brush_ref->brush_asset_reference = MEM_new<AssetWeakReference>(
        __func__, *paint->brush_asset_reference);
    BLI_addhead(&paint->tool_brush_bindings.active_brush_per_brush_type, new_brush_ref);
  }
}

bool WM_toolsystem_activate_brush_and_tool(bContext *C, Paint *paint, Brush *brush)
{
  const bToolRef *active_tool = toolsystem_active_tool_from_context_or_view3d(C);
  const PaintMode paint_mode = BKE_paintmode_get_active_from_context(C);

  if (!BKE_paint_brush_poll(paint, brush)) {
    /* Avoid switching tool when brush isn't valid for this mode anyway. */
    return false;
  }

  /* If necessary, find a compatible tool to switch to. */
  {
    std::optional<int> brush_type = BKE_paint_get_brush_type_from_paintmode(brush, paint_mode);
    if (!brush_type) {
      BLI_assert_unreachable();
      WM_toolsystem_ref_set_by_id(C, "builtin.brush");
    }
    else if (!brush_type_matches_active_tool(C, *brush_type)) {
      const char *brush_type_name = brush_type_identifier_get(*brush_type, paint_mode);
      /* Calls into .py to query available tools. */
      toolsystem_ref_set_by_brush_type(C, brush_type_name);
    }
  }

  /* Do after switching tool, since switching tool will attempt to restore the last used brush of
   * that tool (in #toolsystem_brush_activate_from_toolref_for_object_paint()). */
  if (!BKE_paint_brush_set(paint, brush)) {
    return false;
  }

  if (active_tool->runtime->brush_type == -1) {
    /* Only update the main brush binding to reference the newly active brush. */
    toolsystem_main_brush_binding_update_from_active(paint);
  }
  else {
    toolsystem_brush_type_binding_update(paint, paint_mode, active_tool->runtime->brush_type);
  }

  return true;
}

static void toolsystem_brush_activate_from_toolref_for_object_particle(const Main *bmain,
                                                                       const WorkSpace *workspace,
                                                                       const bToolRef *tref)
{
  const bToolRef_Runtime *tref_rt = tref->runtime;

  if (!tref_rt->data_block[0]) {
    return;
  }

  const EnumPropertyItem *items = rna_enum_particle_edit_hair_brush_items;
  const int i = RNA_enum_from_identifier(items, tref_rt->data_block);
  if (i == -1) {
    return;
  }

  const wmWindowManager *wm = static_cast<wmWindowManager *>(bmain->wm.first);
  LISTBASE_FOREACH (wmWindow *, win, &wm->windows) {
    if (workspace == WM_window_get_active_workspace(win)) {
      Scene *scene = WM_window_get_active_scene(win);
      ToolSettings *ts = scene->toolsettings;
      ts->particle.brushtype = items[i].value;
    }
  }
}

static void toolsystem_brush_activate_from_toolref_for_object_paint(Main *bmain,
                                                                    const WorkSpace *workspace,
                                                                    const bToolRef *tref)
{
  bToolRef_Runtime *tref_rt = tref->runtime;

  const PaintMode paint_mode = BKE_paintmode_get_from_tool(tref);
  BLI_assert(paint_mode != PaintMode::Invalid);

  wmWindowManager *wm = static_cast<wmWindowManager *>(bmain->wm.first);
  LISTBASE_FOREACH (wmWindow *, win, &wm->windows) {
    if (workspace != WM_window_get_active_workspace(win)) {
      continue;
    }
    Scene *scene = WM_window_get_active_scene(win);
    BKE_paint_ensure_from_paintmode(scene, paint_mode);
    Paint *paint = BKE_paint_get_active_from_paintmode(scene, paint_mode);

    /* Attempt to re-activate a brush remembered for this brush type, as stored in a brush
     * binding. */
    if (tref_rt->brush_type != -1) {
      std::optional<AssetWeakReference> brush_asset_reference =
          [&]() -> std::optional<AssetWeakReference> {
        const char *brush_type_name = brush_type_identifier_get(tref_rt->brush_type, paint_mode);
        const NamedBrushAssetReference *brush_ref = toolsystem_brush_type_binding_lookup(
            paint, brush_type_name);

        if (brush_ref && brush_ref->brush_asset_reference) {
          return *brush_ref->brush_asset_reference;
        }
        /* No remembered brush found for this type, use a default for the type. */
        return BKE_paint_brush_type_default_reference(eObjectMode(paint->runtime->ob_mode),
                                                      tref_rt->brush_type);
      }();

      if (brush_asset_reference) {
        BKE_paint_brush_set(bmain, paint, &*brush_asset_reference);
      }
    }
    /* Re-activate the main brush, regardless of the brush type. */
    else {
      if (paint->tool_brush_bindings.main_brush_asset_reference) {
        BKE_paint_brush_set(bmain, paint, paint->tool_brush_bindings.main_brush_asset_reference);
        toolsystem_main_brush_binding_update_from_active(paint);
      }
      else {
        std::optional<AssetWeakReference> main_brush_asset_reference =
            [&]() -> std::optional<AssetWeakReference> {
          if (paint->tool_brush_bindings.main_brush_asset_reference) {
            return *paint->tool_brush_bindings.main_brush_asset_reference;
          }
          return BKE_paint_brush_type_default_reference(eObjectMode(paint->runtime->ob_mode),
                                                        std::nullopt);
        }();

        if (main_brush_asset_reference) {
          BKE_paint_brush_set(bmain, paint, &*main_brush_asset_reference);
          toolsystem_main_brush_binding_update_from_active(paint);
        }
      }
    }
  }
}

/**
 * Activate a brush compatible with \a tref, call when the active tool changes.
 */
static void toolsystem_brush_activate_from_toolref(Main *bmain,
                                                   const WorkSpace *workspace,
                                                   const bToolRef *tref)
{
  BLI_assert(tref->runtime->flag & TOOLREF_FLAG_USE_BRUSHES);

  if (tref->space_type == SPACE_VIEW3D) {
    if (tref->mode == CTX_MODE_PARTICLE) {
      toolsystem_brush_activate_from_toolref_for_object_particle(bmain, workspace, tref);
    }
    else {
      toolsystem_brush_activate_from_toolref_for_object_paint(bmain, workspace, tref);
    }
  }
  else if (tref->space_type == SPACE_IMAGE) {
    if (tref->mode == SI_MODE_PAINT) {
      toolsystem_brush_activate_from_toolref_for_object_paint(bmain, workspace, tref);
    }
  }
}

/**
 * Special case, the active brush data-block for the image & 3D viewport are shared.
 * This means changing the active brush tool in one space must change the tool
 * for the other space as well, see: #131062.
 */
static void toolsystem_brush_sync_for_texture_paint(Main *bmain,
                                                    WorkSpace *workspace,
                                                    bToolRef *tref)
{
  if (tref->space_type == SPACE_VIEW3D) {
    if (tref->mode == CTX_MODE_PAINT_TEXTURE) {
      bToolKey tkey{};
      tkey.space_type = SPACE_IMAGE;
      tkey.mode = SI_MODE_PAINT;
      bToolRef *tref_other = WM_toolsystem_ref_find(workspace, &tkey);
      if (tref_other) {
        toolsystem_ref_set_by_id_pending(bmain, tref_other, tref->idname);
      }
    }
  }
  else if (tref->space_type == SPACE_IMAGE) {
    if (tref->mode == SI_MODE_PAINT) {
      bToolKey tkey{};
      tkey.space_type = SPACE_VIEW3D;
      tkey.mode = CTX_MODE_PAINT_TEXTURE;
      bToolRef *tref_other = WM_toolsystem_ref_find(workspace, &tkey);
      if (tref_other) {
        toolsystem_ref_set_by_id_pending(bmain, tref_other, tref->idname);
      }
    }
  }
}
static void toolsystem_brush_clear_paint_reference(Main *bmain,
                                                   WorkSpace *workspace,
                                                   bToolRef *tref)
{
  const PaintMode paint_mode = BKE_paintmode_get_from_tool(tref);

  wmWindowManager *wm = static_cast<wmWindowManager *>(bmain->wm.first);
  LISTBASE_FOREACH (wmWindow *, win, &wm->windows) {
    if (workspace != WM_window_get_active_workspace(win)) {
      continue;
    }
    Scene *scene = WM_window_get_active_scene(win);
    if (Paint *paint = BKE_paint_get_active_from_paintmode(scene, paint_mode)) {
      BKE_paint_previous_asset_reference_clear(paint);
    }
  }
}

/** \} */

static void toolsystem_ref_link(Main *bmain, WorkSpace *workspace, bToolRef *tref)
{
  bToolRef_Runtime *tref_rt = tref->runtime;
  if (tref_rt->gizmo_group[0]) {
    const char *idname = tref_rt->gizmo_group;
    wmGizmoGroupType *gzgt = WM_gizmogrouptype_find(idname, false);
    if (gzgt != nullptr) {
      if ((gzgt->flag & WM_GIZMOGROUPTYPE_TOOL_INIT) == 0) {
        if (!WM_gizmo_group_type_ensure_ptr(gzgt)) {
          /* Even if the group-type has been linked, it's possible the space types
           * were not previously using it. (happens with multiple windows). */
          wmGizmoMapType *gzmap_type = WM_gizmomaptype_ensure(&gzgt->gzmap_params);
          WM_gizmoconfig_update_tag_group_type_init(gzmap_type, gzgt);
        }
      }
    }
    else {
      CLOG_WARN(WM_LOG_TOOL_GIZMO, "'%s' widget not found", idname);
    }
  }

  if (tref_rt->flag & TOOLREF_FLAG_USE_BRUSHES) {
    toolsystem_brush_activate_from_toolref(bmain, workspace, tref);
    toolsystem_brush_sync_for_texture_paint(bmain, workspace, tref);
  }
  else {
    toolsystem_brush_clear_paint_reference(bmain, workspace, tref);
  }
}

static void toolsystem_refresh_ref(const bContext *C, WorkSpace *workspace, bToolRef *tref)
{
  if (tref->runtime == nullptr) {
    return;
  }
  /* Currently same operation. */
  toolsystem_ref_link(CTX_data_main(C), workspace, tref);
}
void WM_toolsystem_refresh(const bContext *C, WorkSpace *workspace, const bToolKey *tkey)
{
  bToolRef *tref = WM_toolsystem_ref_find(workspace, tkey);
  if (tref) {
    toolsystem_refresh_ref(C, workspace, tref);
  }
}

static void toolsystem_reinit_ref(bContext *C, WorkSpace *workspace, bToolRef *tref)
{
  toolsystem_reinit_with_toolref(C, workspace, tref);
}
void WM_toolsystem_reinit(bContext *C, WorkSpace *workspace, const bToolKey *tkey)
{
  bToolRef *tref = WM_toolsystem_ref_find(workspace, tkey);
  if (tref) {
    toolsystem_reinit_ref(C, workspace, tref);
  }
}

void WM_toolsystem_unlink_all(bContext *C, WorkSpace *workspace)
{
  LISTBASE_FOREACH (bToolRef *, tref, &workspace->tools) {
    tref->tag = 0;
  }

  LISTBASE_FOREACH (bToolRef *, tref, &workspace->tools) {
    if (tref->runtime) {
      if (tref->tag == 0) {
        toolsystem_unlink_ref(C, workspace, tref);
        tref->tag = 1;
      }
    }
  }
}

void WM_toolsystem_refresh_all(const bContext *C, WorkSpace *workspace)
{
  BLI_assert(0);
  LISTBASE_FOREACH (bToolRef *, tref, &workspace->tools) {
    toolsystem_refresh_ref(C, workspace, tref);
  }
}
void WM_toolsystem_reinit_all(bContext *C, wmWindow *win)
{
  bScreen *screen = WM_window_get_active_screen(win);
  const Scene *scene = WM_window_get_active_scene(win);
  ViewLayer *view_layer = WM_window_get_active_view_layer(win);
  LISTBASE_FOREACH (ScrArea *, area, &screen->areabase) {
    if (((1 << area->spacetype) & WM_TOOLSYSTEM_SPACE_MASK) == 0) {
      continue;
    }

    WorkSpace *workspace = WM_window_get_active_workspace(win);
    bToolKey tkey{};
    tkey.space_type = area->spacetype;
    tkey.mode = WM_toolsystem_mode_from_spacetype(scene, view_layer, area, area->spacetype);
    bToolRef *tref = WM_toolsystem_ref_find(workspace, &tkey);
    if (tref) {
      if (tref->tag == 0) {
        toolsystem_reinit_ref(C, workspace, tref);
        tref->tag = 1;
      }
    }
  }
}

void WM_toolsystem_ref_set_from_runtime(bContext *C,
                                        WorkSpace *workspace,
                                        bToolRef *tref,
                                        const bToolRef_Runtime *tref_rt,
                                        const char *idname)
{
  Main *bmain = CTX_data_main(C);

  if (tref->runtime) {
    toolsystem_unlink_ref(C, workspace, tref);
  }

  STRNCPY_UTF8(tref->idname, idname);

  /* This immediate request supersedes any unhandled pending requests. */
  tref->idname_pending[0] = '\0';

  if (tref->runtime == nullptr) {
    tref->runtime = MEM_callocN<bToolRef_Runtime>(__func__);
  }

  if (tref_rt != tref->runtime) {
    *tref->runtime = *tref_rt;
  }

  /* Ideally Python could check this gizmo group flag and not
   * pass in the argument to begin with. */
  bool use_fallback_keymap = false;

  if (tref->idname_fallback[0] || tref->runtime->keymap_fallback[0]) {
    if (tref_rt->flag & TOOLREF_FLAG_FALLBACK_KEYMAP) {
      use_fallback_keymap = true;
    }
    else if (tref_rt->gizmo_group[0]) {
      wmGizmoGroupType *gzgt = WM_gizmogrouptype_find(tref_rt->gizmo_group, false);
      if (gzgt) {
        if (gzgt->flag & WM_GIZMOGROUPTYPE_TOOL_FALLBACK_KEYMAP) {
          use_fallback_keymap = true;
        }
      }
    }
  }
  if (use_fallback_keymap == false) {
    tref->idname_fallback[0] = '\0';
    tref->runtime->keymap_fallback[0] = '\0';
  }

  toolsystem_ref_link(bmain, workspace, tref);

  toolsystem_refresh_screen_from_active_tool(bmain, workspace, tref);

  /* Set the cursor if possible, if not - it's fine as entering the region will refresh it. */
  {
    wmWindow *win = CTX_wm_window(C);
    if (win != nullptr) {
      win->addmousemove = true;
      win->tag_cursor_refresh = true;
    }
  }

  {
    wmMsgBus *mbus = CTX_wm_message_bus(C);
    WM_msg_publish_rna_prop(mbus, &workspace->id, workspace, WorkSpace, tools);
  }
}

void WM_toolsystem_ref_sync_from_context(Main *bmain, WorkSpace *workspace, bToolRef *tref)
{
  bToolRef_Runtime *tref_rt = tref->runtime;
  if ((tref_rt == nullptr) || (tref_rt->data_block[0] == '\0')) {
    return;
  }
  wmWindowManager *wm = static_cast<wmWindowManager *>(bmain->wm.first);
  LISTBASE_FOREACH (wmWindow *, win, &wm->windows) {
    if (workspace != WM_window_get_active_workspace(win)) {
      continue;
    }

    Scene *scene = WM_window_get_active_scene(win);
    ToolSettings *ts = scene->toolsettings;
    ViewLayer *view_layer = WM_window_get_active_view_layer(win);
    BKE_view_layer_synced_ensure(scene, view_layer);
    const Object *ob = BKE_view_layer_active_object_get(view_layer);
    if (ob == nullptr) {
      /* Pass. */
    }
    if ((tref->space_type == SPACE_VIEW3D) && (tref->mode == CTX_MODE_PARTICLE)) {
      if (ob->mode & OB_MODE_PARTICLE_EDIT) {
        const EnumPropertyItem *items = rna_enum_particle_edit_hair_brush_items;
        const int i = RNA_enum_from_value(items, ts->particle.brushtype);
        const EnumPropertyItem *item = &items[i];
        if (!STREQ(tref_rt->data_block, item->identifier)) {
          STRNCPY_UTF8(tref_rt->data_block, item->identifier);
          SNPRINTF_UTF8(tref->idname, "builtin_brush.%s", item->name);
        }
      }
    }
  }
}

void WM_toolsystem_init(const bContext *C)
{
  Main *bmain = CTX_data_main(C);

  BLI_assert(CTX_wm_window(C) == nullptr);

  LISTBASE_FOREACH (WorkSpace *, workspace, &bmain->workspaces) {
    LISTBASE_FOREACH (bToolRef *, tref, &workspace->tools) {
      MEM_SAFE_FREE(tref->runtime);
    }
  }

  /* Rely on screen initialization for gizmos. */
}

static bool toolsystem_key_ensure_check(const bToolKey *tkey)
{
  switch (tkey->space_type) {
    case SPACE_VIEW3D:
      return true;
    case SPACE_IMAGE:
      if (ELEM(tkey->mode, SI_MODE_PAINT, SI_MODE_UV, SI_MODE_VIEW)) {
        return true;
      }
      break;
    case SPACE_NODE:
      return true;
    case SPACE_SEQ:
      return true;
  }
  return false;
}

int WM_toolsystem_mode_from_spacetype(const Scene *scene,
                                      ViewLayer *view_layer,
                                      ScrArea *area,
                                      int space_type)
{
  int mode = -1;
  switch (space_type) {
    case SPACE_VIEW3D: {
      /* 'area' may be nullptr in this case. */
      BKE_view_layer_synced_ensure(scene, view_layer);
      Object *obact = BKE_view_layer_active_object_get(view_layer);
      if (obact != nullptr) {
        Object *obedit = OBEDIT_FROM_OBACT(obact);
        mode = CTX_data_mode_enum_ex(obedit, obact, eObjectMode(obact->mode));
      }
      else {
        mode = CTX_MODE_OBJECT;
      }
      break;
    }
    case SPACE_IMAGE: {
      SpaceImage *sima = static_cast<SpaceImage *>(area->spacedata.first);
      mode = sima->mode;
      break;
    }
    case SPACE_NODE: {
      mode = 0;
      break;
    }
    case SPACE_SEQ: {
      SpaceSeq *sseq = static_cast<SpaceSeq *>(area->spacedata.first);
      mode = sseq->view;
      break;
    }
  }
  return mode;
}

bool WM_toolsystem_key_from_context(const Scene *scene,
                                    ViewLayer *view_layer,
                                    ScrArea *area,
                                    bToolKey *tkey)
{
  int space_type = SPACE_EMPTY;
  int mode = -1;

  if (area != nullptr) {
    space_type = area->spacetype;
    mode = WM_toolsystem_mode_from_spacetype(scene, view_layer, area, space_type);
  }

  if (mode != -1) {
    tkey->space_type = space_type;
    tkey->mode = mode;
    return true;
  }
  return false;
}

void WM_toolsystem_refresh_active(bContext *C)
{
  Main *bmain = CTX_data_main(C);

  struct {
    wmWindow *win;
    ScrArea *area;
    ARegion *region;
    bool is_set;
  } context_prev = {nullptr};

  for (wmWindowManager *wm = static_cast<wmWindowManager *>(bmain->wm.first); wm;
       wm = static_cast<wmWindowManager *>(wm->id.next))
  {
    LISTBASE_FOREACH (wmWindow *, win, &wm->windows) {
      WorkSpace *workspace = WM_window_get_active_workspace(win);
      bScreen *screen = WM_window_get_active_screen(win);
      const Scene *scene = WM_window_get_active_scene(win);
      ViewLayer *view_layer = WM_window_get_active_view_layer(win);
      /* Could skip loop for modes that don't depend on space type. */
      int space_type_mask_handled = 0;
      LISTBASE_FOREACH (ScrArea *, area, &screen->areabase) {
        /* Don't change the space type of the active tool, only update its mode. */
        const int space_type_mask = (1 << area->spacetype);
        if ((space_type_mask & WM_TOOLSYSTEM_SPACE_MASK) &&
            ((space_type_mask_handled & space_type_mask) == 0))
        {
          space_type_mask_handled |= space_type_mask;
          bToolKey tkey{};
          tkey.space_type = area->spacetype;
          tkey.mode = WM_toolsystem_mode_from_spacetype(scene, view_layer, area, area->spacetype);
          bToolRef *tref = WM_toolsystem_ref_find(workspace, &tkey);
          if (tref != area->runtime.tool) {
            if (context_prev.is_set == false) {
              context_prev.win = CTX_wm_window(C);
              context_prev.area = CTX_wm_area(C);
              context_prev.region = CTX_wm_region(C);
              context_prev.is_set = true;
            }

            CTX_wm_window_set(C, win);
            CTX_wm_area_set(C, area);

            toolsystem_reinit_ensure_toolref(C, workspace, &tkey, nullptr);
          }
        }
      }
    }
  }

  if (context_prev.is_set) {
    CTX_wm_window_set(C, context_prev.win);
    CTX_wm_area_set(C, context_prev.area);
    CTX_wm_region_set(C, context_prev.region);
  }

  BKE_workspace_id_tag_all_visible(bmain, ID_TAG_DOIT);

  LISTBASE_FOREACH (WorkSpace *, workspace, &bmain->workspaces) {
    if (workspace->id.tag & ID_TAG_DOIT) {
      workspace->id.tag &= ~ID_TAG_DOIT;
      /* Refresh to ensure data is initialized.
       * This is needed because undo can load a state which no longer has the underlying DNA data
       * needed for the tool (un-initialized paint-slots for eg), see: #64339. */
      LISTBASE_FOREACH (bToolRef *, tref, &workspace->tools) {
        toolsystem_refresh_ref(C, workspace, tref);
      }
    }
  }
}

bool WM_toolsystem_refresh_screen_area(WorkSpace *workspace,
                                       const Scene *scene,
                                       ViewLayer *view_layer,
                                       ScrArea *area)
{
  const bool is_tool_set_prev = area->runtime.is_tool_set;
  const bToolRef *tref_prev = area->runtime.tool;

  area->runtime.tool = nullptr;
  area->runtime.is_tool_set = true;
  const int mode = WM_toolsystem_mode_from_spacetype(scene, view_layer, area, area->spacetype);
  LISTBASE_FOREACH (bToolRef *, tref, &workspace->tools) {
    if (tref->space_type == area->spacetype) {
      if (tref->mode == mode) {
        area->runtime.tool = tref;
        break;
      }
    }
  }
  return !(is_tool_set_prev && (tref_prev == area->runtime.tool));
}

void WM_toolsystem_refresh_screen_window(wmWindow *win)
{
  WorkSpace *workspace = WM_window_get_active_workspace(win);
  bool space_type_has_tools[SPACE_TYPE_NUM] = {false};
  LISTBASE_FOREACH (bToolRef *, tref, &workspace->tools) {
    space_type_has_tools[tref->space_type] = true;
  }
  bScreen *screen = WM_window_get_active_screen(win);
  const Scene *scene = WM_window_get_active_scene(win);
  ViewLayer *view_layer = WM_window_get_active_view_layer(win);
  LISTBASE_FOREACH (ScrArea *, area, &screen->areabase) {
    area->runtime.tool = nullptr;
    area->runtime.is_tool_set = true;
    if (space_type_has_tools[area->spacetype]) {
      WM_toolsystem_refresh_screen_area(workspace, scene, view_layer, area);
    }
  }
}

void WM_toolsystem_refresh_screen_all(Main *bmain)
{
  /* Update all ScrArea's tools. */
  for (wmWindowManager *wm = static_cast<wmWindowManager *>(bmain->wm.first); wm;
       wm = static_cast<wmWindowManager *>(wm->id.next))
  {
    LISTBASE_FOREACH (wmWindow *, win, &wm->windows) {
      WM_toolsystem_refresh_screen_window(win);
    }
  }
}

static void toolsystem_refresh_screen_from_active_tool(Main *bmain,
                                                       WorkSpace *workspace,
                                                       bToolRef *tref)
{
  /* Update all ScrArea's tools. */
  for (wmWindowManager *wm = static_cast<wmWindowManager *>(bmain->wm.first); wm;
       wm = static_cast<wmWindowManager *>(wm->id.next))
  {
    LISTBASE_FOREACH (wmWindow *, win, &wm->windows) {
      if (workspace == WM_window_get_active_workspace(win)) {
        bScreen *screen = WM_window_get_active_screen(win);
        const Scene *scene = WM_window_get_active_scene(win);
        ViewLayer *view_layer = WM_window_get_active_view_layer(win);
        LISTBASE_FOREACH (ScrArea *, area, &screen->areabase) {
          if (area->spacetype == tref->space_type) {
            int mode = WM_toolsystem_mode_from_spacetype(scene, view_layer, area, area->spacetype);
            if (mode == tref->mode) {
              area->runtime.tool = tref;
              area->runtime.is_tool_set = true;
            }
          }
        }
      }
    }
  }
}

bToolRef *WM_toolsystem_ref_set_by_id_ex(
    bContext *C, WorkSpace *workspace, const bToolKey *tkey, const char *name, bool cycle)
{
  wmOperatorType *ot = WM_operatortype_find("WM_OT_tool_set_by_id", false);
  /* On startup, Python operators are not yet loaded. */
  if (ot == nullptr) {
    return nullptr;
  }

/* Some contexts use the current space type (e.g. image editor),
 * ensure this is set correctly or there is no area. */
#ifndef NDEBUG
  /* Exclude this check for some space types where the space type isn't used. */
  if ((1 << tkey->space_type) & WM_TOOLSYSTEM_SPACE_MASK_MODE_FROM_SPACE) {
    ScrArea *area = CTX_wm_area(C);
    BLI_assert(area == nullptr || area->spacetype == tkey->space_type);
  }
#endif

  PointerRNA op_props;
  WM_operator_properties_create_ptr(&op_props, ot);
  RNA_string_set(&op_props, "name", name);

  BLI_assert((1 << tkey->space_type) & WM_TOOLSYSTEM_SPACE_MASK);

  RNA_enum_set(&op_props, "space_type", tkey->space_type);
  RNA_boolean_set(&op_props, "cycle", cycle);

  WM_operator_name_call_ptr(C, ot, blender::wm::OpCallContext::ExecDefault, &op_props, nullptr);
  WM_operator_properties_free(&op_props);

  bToolRef *tref = WM_toolsystem_ref_find(workspace, tkey);

  if (tref) {
    Main *bmain = CTX_data_main(C);
    toolsystem_refresh_screen_from_active_tool(bmain, workspace, tref);
  }

  return (tref && STREQ(tref->idname, name)) ? tref : nullptr;
}

bToolRef *WM_toolsystem_ref_set_by_id(bContext *C, const char *name)
{
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  ScrArea *area = CTX_wm_area(C);
  bToolKey tkey;
  if (WM_toolsystem_key_from_context(scene, view_layer, area, &tkey)) {
    WorkSpace *workspace = CTX_wm_workspace(C);
    return WM_toolsystem_ref_set_by_id_ex(C, workspace, &tkey, name, false);
  }
  return nullptr;
}

static void toolsystem_ref_set_by_brush_type(bContext *C, const char *brush_type)
{
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  ScrArea *area = CTX_wm_area(C);
  const bToolKey tkey = toolsystem_key_from_context_or_view3d(scene, view_layer, area);
  WorkSpace *workspace = CTX_wm_workspace(C);

  wmOperatorType *ot = WM_operatortype_find("WM_OT_tool_set_by_brush_type", false);
  /* On startup, Python operators are not yet loaded. */
  if (ot == nullptr) {
    return;
  }

/* Some contexts use the current space type (e.g. image editor),
 * ensure this is set correctly or there is no area. */
#ifndef NDEBUG
  /* Exclude this check for some space types where the space type isn't used. */
  if ((1 << tkey.space_type) & WM_TOOLSYSTEM_SPACE_MASK_MODE_FROM_SPACE) {
    ScrArea *area = CTX_wm_area(C);
    BLI_assert(area == nullptr || area->spacetype == tkey.space_type);
  }
#endif

  PointerRNA op_props;
  WM_operator_properties_create_ptr(&op_props, ot);
  RNA_string_set(&op_props, "brush_type", brush_type);

  BLI_assert((1 << tkey.space_type) & WM_TOOLSYSTEM_SPACE_MASK);

  RNA_enum_set(&op_props, "space_type", tkey.space_type);

  WM_operator_name_call_ptr(C, ot, blender::wm::OpCallContext::ExecDefault, &op_props, nullptr);
  WM_operator_properties_free(&op_props);

  bToolRef *tref = WM_toolsystem_ref_find(workspace, &tkey);

  if (tref) {
    Main *bmain = CTX_data_main(C);
    toolsystem_refresh_screen_from_active_tool(bmain, workspace, tref);
  }
}

/**
 * Request a tool ID be activated in a context where it's not known if the tool exists,
 * when the areas using this tool are not visible.
 * In this case, set the `idname` as pending and flag tools area for updating.
 *
 * If the tool doesn't exist then the current tool is to be left as-is.
 */
static void toolsystem_ref_set_by_id_pending(Main *bmain,
                                             bToolRef *tref,
                                             const char *idname_pending)
{
  BLI_assert(idname_pending[0]);

  /* Check if the pending or current tool is already set to the requested value. */
  const bool this_match = STREQ(idname_pending, tref->idname);
  if (tref->idname_pending[0]) {
    const bool next_match = STREQ(idname_pending, tref->idname_pending);
    if (next_match) {
      return;
    }
    /* Highly unlikely but possible the current active tool matches the name.
     * In this case clear pending as there is nothing to do. */
    if (this_match) {
      tref->idname_pending[0] = '\0';
      return;
    }
  }
  else {
    if (this_match) {
      return;
    }
  }

  STRNCPY_UTF8(tref->idname_pending, idname_pending);

  /* If there would be a convenient way to know which screens used which work-spaces,
   * that could be used here. */
  LISTBASE_FOREACH (bScreen *, screen, &bmain->screens) {
    LISTBASE_FOREACH (ScrArea *, area, &screen->areabase) {
      if (area->runtime.tool == tref) {
        area->runtime.tool = nullptr;
        area->runtime.is_tool_set = false;
        area->flag |= AREA_FLAG_ACTIVE_TOOL_UPDATE;
      }
    }
  }
}

static void toolsystem_reinit_with_toolref(bContext *C, WorkSpace *workspace, bToolRef *tref)
{
  bToolKey tkey{};
  tkey.space_type = tref->space_type;
  tkey.mode = tref->mode;

  const char *idname = tref->idname_pending[0] ? tref->idname_pending : tref->idname;

  WM_toolsystem_ref_set_by_id_ex(C, workspace, &tkey, idname, false);

  /* Never attempt the pending name again, if it's not found, no need to keep trying. */
  tref->idname_pending[0] = '\0';
}

static const char *toolsystem_default_tool(const bToolKey *tkey)
{
  switch (tkey->space_type) {
    case SPACE_VIEW3D:
      switch (tkey->mode) {
        case CTX_MODE_SCULPT:
        case CTX_MODE_PAINT_VERTEX:
        case CTX_MODE_PAINT_WEIGHT:
        case CTX_MODE_PAINT_TEXTURE:
        case CTX_MODE_PAINT_GPENCIL_LEGACY:
        case CTX_MODE_PAINT_GREASE_PENCIL:
        case CTX_MODE_SCULPT_GPENCIL_LEGACY:
        case CTX_MODE_SCULPT_GREASE_PENCIL:
        case CTX_MODE_WEIGHT_GPENCIL_LEGACY:
        case CTX_MODE_WEIGHT_GREASE_PENCIL:
        case CTX_MODE_VERTEX_GPENCIL_LEGACY:
        case CTX_MODE_VERTEX_GREASE_PENCIL:
        case CTX_MODE_SCULPT_CURVES:
          return "builtin.brush";
        case CTX_MODE_PARTICLE:
          return "builtin_brush.Comb";
        case CTX_MODE_EDIT_TEXT:
          return "builtin.select_text";
      }
      break;
    case SPACE_IMAGE:
      switch (tkey->mode) {
        case SI_MODE_PAINT:
          return "builtin.brush";
        case SI_MODE_VIEW:
          return "builtin.sample";
      }
      break;
    case SPACE_NODE: {
      return "builtin.select_box";
    }
    case SPACE_SEQ: {
      return "builtin.select_box";
    }
  }

  return "builtin.select_box";
}

/**
 * Run after changing modes.
 */
static bToolRef *toolsystem_reinit_ensure_toolref(bContext *C,
                                                  WorkSpace *workspace,
                                                  const bToolKey *tkey,
                                                  const char *default_tool)
{
  bToolRef *tref;
  if (WM_toolsystem_ref_ensure(workspace, tkey, &tref)) {
    if (default_tool == nullptr) {
      default_tool = toolsystem_default_tool(tkey);
    }
    STRNCPY_UTF8(tref->idname, default_tool);
  }
  toolsystem_reinit_with_toolref(C, workspace, tref);
  return tref;
}

static void wm_toolsystem_update_from_context_view3d_impl(bContext *C, WorkSpace *workspace)
{
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  int space_type = SPACE_VIEW3D;
  bToolKey tkey{};
  tkey.space_type = space_type;
  tkey.mode = WM_toolsystem_mode_from_spacetype(scene, view_layer, nullptr, space_type);
  toolsystem_reinit_ensure_toolref(C, workspace, &tkey, nullptr);
}

void WM_toolsystem_update_from_context_view3d(bContext *C)
{
  WorkSpace *workspace = CTX_wm_workspace(C);
  if (workspace) {
    wm_toolsystem_update_from_context_view3d_impl(C, workspace);
  }

  /* Multi window support. */
  Main *bmain = CTX_data_main(C);
  wmWindowManager *wm = static_cast<wmWindowManager *>(bmain->wm.first);
  if (!BLI_listbase_is_single(&wm->windows)) {
    wmWindow *win_prev = CTX_wm_window(C);
    ScrArea *area_prev = CTX_wm_area(C);
    ARegion *region_prev = CTX_wm_region(C);

    LISTBASE_FOREACH (wmWindow *, win, &wm->windows) {
      if (win != win_prev) {
        WorkSpace *workspace_iter = WM_window_get_active_workspace(win);
        if (workspace_iter != workspace) {

          CTX_wm_window_set(C, win);

          wm_toolsystem_update_from_context_view3d_impl(C, workspace_iter);

          CTX_wm_window_set(C, win_prev);
          CTX_wm_area_set(C, area_prev);
          CTX_wm_region_set(C, region_prev);
        }
      }
    }
  }
}

void WM_toolsystem_update_from_context(
    bContext *C, WorkSpace *workspace, const Scene *scene, ViewLayer *view_layer, ScrArea *area)
{
  bToolKey tkey{};
  tkey.space_type = area->spacetype;
  tkey.mode = WM_toolsystem_mode_from_spacetype(scene, view_layer, area, area->spacetype);
  if (toolsystem_key_ensure_check(&tkey)) {
    toolsystem_reinit_ensure_toolref(C, workspace, &tkey, nullptr);
  }
}

bool WM_toolsystem_active_tool_is_brush(const bContext *C)
{
  const bToolRef_Runtime *tref_rt = WM_toolsystem_runtime_from_context((bContext *)C);
  return tref_rt && (tref_rt->flag & TOOLREF_FLAG_USE_BRUSHES);
}

bool WM_toolsystem_active_tool_has_custom_cursor(const bContext *C)
{
  const bToolRef_Runtime *tref_rt = WM_toolsystem_runtime_from_context((bContext *)C);
  return tref_rt && (tref_rt->cursor != WM_CURSOR_DEFAULT);
}

void WM_toolsystem_do_msg_notify_tag_refresh(bContext *C,
                                             wmMsgSubscribeKey * /*msg_key*/,
                                             wmMsgSubscribeValue *msg_val)
{
  ScrArea *area = static_cast<ScrArea *>(msg_val->user_data);
  Main *bmain = CTX_data_main(C);
  wmWindow *win = static_cast<wmWindow *>(((wmWindowManager *)bmain->wm.first)->windows.first);
  if (win->next != nullptr) {
    do {
      bScreen *screen = WM_window_get_active_screen(win);
      if (BLI_findindex(&screen->areabase, area) != -1) {
        break;
      }
    } while ((win = win->next));
  }

  WorkSpace *workspace = WM_window_get_active_workspace(win);
  const Scene *scene = WM_window_get_active_scene(win);
  ViewLayer *view_layer = WM_window_get_active_view_layer(win);

  bToolKey tkey{};
  tkey.space_type = area->spacetype;
  tkey.mode = WM_toolsystem_mode_from_spacetype(scene, view_layer, area, area->spacetype);
  WM_toolsystem_refresh(C, workspace, &tkey);
  WM_toolsystem_refresh_screen_area(workspace, scene, view_layer, area);
}

static IDProperty *idprops_ensure_named_group(IDProperty *group, const char *idname)
{
  IDProperty *prop = IDP_GetPropertyFromGroup(group, idname);
  if ((prop == nullptr) || (prop->type != IDP_GROUP)) {
    prop = blender::bke::idprop::create_group(__func__).release();
    STRNCPY_UTF8(prop->name, idname);
    IDP_ReplaceInGroup_ex(group, prop, nullptr, 0);
  }
  return prop;
}

IDProperty *WM_toolsystem_ref_properties_get_idprops(bToolRef *tref)
{
  IDProperty *group = tref->properties;
  if (group == nullptr) {
    return nullptr;
  }
  return IDP_GetPropertyFromGroup(group, tref->idname);
}

IDProperty *WM_toolsystem_ref_properties_ensure_idprops(bToolRef *tref)
{
  if (tref->properties == nullptr) {
    tref->properties = blender::bke::idprop::create_group(__func__).release();
  }
  return idprops_ensure_named_group(tref->properties, tref->idname);
}

bool WM_toolsystem_ref_properties_get_ex(bToolRef *tref,
                                         const char *idname,
                                         StructRNA *type,
                                         PointerRNA *r_ptr)
{
  IDProperty *group = WM_toolsystem_ref_properties_get_idprops(tref);
  IDProperty *prop = group ? IDP_GetPropertyFromGroup(group, idname) : nullptr;
  *r_ptr = RNA_pointer_create_discrete(nullptr, type, prop);
  return (prop != nullptr);
}

void WM_toolsystem_ref_properties_ensure_ex(bToolRef *tref,
                                            const char *idname,
                                            StructRNA *type,
                                            PointerRNA *r_ptr)
{
  IDProperty *group = WM_toolsystem_ref_properties_ensure_idprops(tref);
  IDProperty *prop = idprops_ensure_named_group(group, idname);
  *r_ptr = RNA_pointer_create_discrete(nullptr, type, prop);
}

void WM_toolsystem_ref_properties_init_for_keymap(bToolRef *tref,
                                                  PointerRNA *dst_ptr,
                                                  PointerRNA *src_ptr,
                                                  wmOperatorType *ot)
{
  *dst_ptr = *src_ptr;
  if (dst_ptr->data) {
    dst_ptr->data = IDP_CopyProperty(static_cast<const IDProperty *>(dst_ptr->data));
  }
  else {
    dst_ptr->data = blender::bke::idprop::create_group("wmOpItemProp").release();
  }
  IDProperty *group = WM_toolsystem_ref_properties_get_idprops(tref);
  if (group != nullptr) {
    IDProperty *prop = IDP_GetPropertyFromGroup(group, ot->idname);
    if (prop) {
      /* Important key-map items properties don't get overwritten by the tools.
       * - When a key-map item doesn't set a property, the tool-systems is used.
       * - When it does, it overrides the tool-system.
       *
       * This way the default action can be to follow the top-bar tool-settings &
       * modifier keys can be used to perform different actions that aren't clobbered here.
       */
      IDP_MergeGroup(static_cast<IDProperty *>(dst_ptr->data), prop, false);
    }
  }
}
