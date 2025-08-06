/* SPDX-FileCopyrightText: 2008 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Floating Persistent Region
 */

#include <cstring>

#include "MEM_guardedalloc.h"

#include "DNA_screen_types.h"
#include "DNA_userdef_types.h"

#include "BLI_listbase.h"
#include "BLI_rect.h"
#include "BLI_string_utf8.h"
#include "BLI_utildefines.h"

#include "BKE_context.hh"
#include "BKE_screen.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "UI_interface_layout.hh"
#include "UI_view2d.hh"

#include "BLT_translation.hh"

#include "ED_screen.hh"

#include "GPU_framebuffer.hh"
#include "interface_intern.hh"

/* -------------------------------------------------------------------- */
/** \name Utilities
 * \{ */

/**
 * Return the index of `region` of other regions in the area (of the same type).
 */
static int area_calc_region_type_index(const ScrArea *area, const ARegion *region)
{
  const int region_type = region->regiontype;
  int index = 0;
  LISTBASE_FOREACH (const ARegion *, region_iter, &area->regionbase) {
    if (region_iter->regiontype != region_type) {
      continue;
    }
    if (region_iter == region) {
      return index;
    }
    index += 1;
  }

  /* Bad input as the `region` was not found in the `area`,
   * -1 causes the first to be returned. */
  BLI_assert_unreachable();
  return -1;
}

/**
 * Find the areas region by type and index, or just by type (if the index isn't found).
 */
static ARegion *area_find_region_by_type_and_index_hint(const ScrArea *area,
                                                        const short region_type,
                                                        int index_hint)
{
  ARegion *region_match_type = nullptr;
  /* Any negative values can return the first match. */
  index_hint = std::max(0, index_hint);
  int index = 0;
  LISTBASE_FOREACH (ARegion *, region, &area->regionbase) {
    if (region->regiontype != region_type) {
      continue;
    }
    if (index == index_hint) {
      region_match_type = region;
      break;
    }
    if (region_match_type == nullptr) {
      region_match_type = region;
    }
    index += 1;
  }
  return region_match_type;
}

struct HudRegionData {
  short regionid;
  /**
   * The region index of this region type in the `area`.
   * When this cannot be resolved, use the first region of `regionid`.
   *
   * This is needed because it's possible the index is no longer available
   * if exiting quad-view in the 3D viewport after performing an operation for example.
   * so in this case use the first region.
   */
  int region_index_hint;
};

static bool last_redo_poll(const bContext *C, short region_type, int region_index_hint)
{
  wmOperator *op = WM_operator_last_redo(C);
  if (op == nullptr) {
    return false;
  }

  bool success = false;
  {
    /* Make sure that we are using the same region type as the original
     * operator call. Otherwise we would be polling the operator with the
     * wrong context.
     */
    ScrArea *area = CTX_wm_area(C);
    ARegion *region_op = (region_type != -1) ? area_find_region_by_type_and_index_hint(
                                                   area, region_type, region_index_hint) :
                                               nullptr;
    ARegion *region_prev = CTX_wm_region(C);
    CTX_wm_region_set((bContext *)C, region_op);

    if (WM_operator_repeat_check(C, op) && WM_operator_ui_poll(op->type, op->ptr)) {
      success = WM_operator_poll((bContext *)C, op->type);
    }
    CTX_wm_region_set((bContext *)C, region_prev);
  }
  return success;
}

static void hud_region_hide(ARegion *region)
{
  region->flag |= RGN_FLAG_HIDDEN;
  /* Avoids setting 'AREA_FLAG_REGION_SIZE_UPDATE'
   * since other regions don't depend on this. */
  BLI_rcti_init(&region->winrct, 0, 0, 0, 0);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Redo Panel
 * \{ */

static bool hud_panel_operator_redo_poll(const bContext *C, PanelType * /*pt*/)
{
  ScrArea *area = CTX_wm_area(C);
  ARegion *region = BKE_area_find_region_type(area, RGN_TYPE_HUD);
  if (region != nullptr) {
    HudRegionData *hrd = static_cast<HudRegionData *>(region->regiondata);
    if (hrd != nullptr) {
      return last_redo_poll(C, hrd->regionid, hrd->region_index_hint);
    }
  }
  return false;
}

static void hud_panel_operator_redo_draw_header(const bContext *C, Panel *panel)
{
  wmOperator *op = WM_operator_last_redo(C);
  const std::string opname = WM_operatortype_name(op->type, op->ptr);
  UI_panel_drawname_set(panel, opname);
}

static void hud_panel_operator_redo_draw(const bContext *C, Panel *panel)
{
  wmOperator *op = WM_operator_last_redo(C);
  if (op == nullptr) {
    return;
  }
  if (!WM_operator_check_ui_enabled(C, op->type->name)) {
    panel->layout->enabled_set(false);
  }
  uiLayout *col = &panel->layout->column(false);
  uiTemplateOperatorRedoProperties(col, C);
}

static void hud_panels_register(ARegionType *art, int space_type, int region_type)
{
  PanelType *pt = MEM_callocN<PanelType>(__func__);
  STRNCPY_UTF8(pt->idname, "OPERATOR_PT_redo");
  STRNCPY_UTF8(pt->label, N_("Redo"));
  STRNCPY_UTF8(pt->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
  pt->draw_header = hud_panel_operator_redo_draw_header;
  pt->draw = hud_panel_operator_redo_draw;
  pt->poll = hud_panel_operator_redo_poll;
  pt->space_type = space_type;
  pt->region_type = region_type;
  pt->flag |= PANEL_TYPE_DEFAULT_CLOSED;
  BLI_addtail(&art->paneltypes, pt);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Callbacks for Floating Region
 * \{ */

static void hud_region_init(wmWindowManager *wm, ARegion *region)
{
  ED_region_panels_init(wm, region);

  /* Reset zoom from panels init because we don't want zoom allowed for redo panel. */
  region->v2d.maxzoom = 1.0f;
  region->v2d.minzoom = 1.0f;

  UI_region_handlers_add(&region->runtime->handlers);
  region->flag |= RGN_FLAG_TEMP_REGIONDATA;
}

static void hud_region_free(ARegion *region)
{
  MEM_SAFE_FREE(region->regiondata);
}

static void hud_region_layout(const bContext *C, ARegion *region)
{
  HudRegionData *hrd = static_cast<HudRegionData *>(region->regiondata);
  if (hrd == nullptr || !last_redo_poll(C, hrd->regionid, hrd->region_index_hint)) {
    ED_region_tag_redraw(region);
    hud_region_hide(region);
    return;
  }

  ScrArea *area = CTX_wm_area(C);
  const int size_y = region->sizey;

  ED_region_panels_layout(C, region);

  if (region->panels.first &&
      ((area->flag & AREA_FLAG_REGION_SIZE_UPDATE) || (region->sizey != size_y)))
  {
    int winx_new = UI_SCALE_FAC * (region->sizex + 0.5f);
    int winy_new = UI_SCALE_FAC * (region->sizey + 0.5f);
    View2D *v2d = &region->v2d;

    if (region->flag & RGN_FLAG_SIZE_CLAMP_X) {
      CLAMP_MAX(winx_new, region->winx);
    }
    if (region->flag & RGN_FLAG_SIZE_CLAMP_Y) {
      CLAMP_MAX(winy_new, region->winy);
    }

    region->winx = winx_new;
    region->winy = winy_new;

    region->winrct.xmax = (region->winrct.xmin + region->winx) - 1;
    region->winrct.ymax = (region->winrct.ymin + region->winy) - 1;

    UI_view2d_region_reinit(v2d, V2D_COMMONVIEW_LIST, region->winx, region->winy);

    /* Weak, but needed to avoid glitches, especially with hi-dpi
     * (where resizing the view glitches often).
     * Fortunately this only happens occasionally. */
    ED_region_panels_layout(C, region);
  }

  /* restore view matrix */
  UI_view2d_view_restore(C);
}

static void hud_region_draw(const bContext *C, ARegion *region)
{
  UI_view2d_view_ortho(&region->v2d);
  wmOrtho2_region_pixelspace(region);
  GPU_clear_color(0.0f, 0.0f, 0.0f, 0.0f);

  if ((region->flag & RGN_FLAG_HIDDEN) == 0) {
    rcti reset_rect = {};
    reset_rect.xmax = region->winx;
    reset_rect.ymax = region->winy;
    ui_draw_menu_back(nullptr, nullptr, &reset_rect);
    ED_region_panels_draw(C, region);
  }
}

static void hud_region_listener(const wmRegionListenerParams *params)
{
  ARegion *region = params->region;
  const wmNotifier *wmn = params->notifier;

  switch (wmn->category) {
    case NC_WM:
      if (wmn->data == ND_HISTORY) {
        ED_region_tag_redraw(region);
      }
      break;
  }
}

ARegionType *ED_area_type_hud(int space_type)
{
  ARegionType *art = MEM_callocN<ARegionType>(__func__);
  art->regionid = RGN_TYPE_HUD;
  art->keymapflag = ED_KEYMAP_UI | ED_KEYMAP_VIEW2D;
  art->listener = hud_region_listener;
  art->layout = hud_region_layout;
  art->draw = hud_region_draw;
  art->init = hud_region_init;
  art->free = hud_region_free;

  /* We need to indicate a preferred size to avoid false `RGN_FLAG_TOO_SMALL`
   * the first time the region is created. */
  art->prefsizex = AREAMINX;
  art->prefsizey = HEADERY;

  hud_panels_register(art, space_type, art->regionid);

  art->lock = REGION_DRAW_LOCK_ALL;
  return art;
}

static ARegion *hud_region_add(ScrArea *area)
{
  ARegion *region = BKE_area_region_new();
  ARegion *region_win = BKE_area_find_region_type(area, RGN_TYPE_WINDOW);
  if (region_win) {
    BLI_insertlinkbefore(&area->regionbase, region_win, region);
  }
  else {
    BLI_addtail(&area->regionbase, region);
  }
  region->regiontype = RGN_TYPE_HUD;
  region->alignment = RGN_ALIGN_FLOAT;
  region->overlap = true;
  region->flag |= RGN_FLAG_DYNAMIC_SIZE;

  return region;
}

void ED_area_type_hud_clear(wmWindowManager *wm, ScrArea *area_keep)
{
  LISTBASE_FOREACH (wmWindow *, win, &wm->windows) {
    bScreen *screen = WM_window_get_active_screen(win);
    LISTBASE_FOREACH (ScrArea *, area, &screen->areabase) {
      if (area != area_keep) {
        LISTBASE_FOREACH (ARegion *, region, &area->regionbase) {
          if (region->regiontype == RGN_TYPE_HUD) {
            if ((region->flag & RGN_FLAG_HIDDEN) == 0) {
              hud_region_hide(region);
              ED_region_tag_redraw(region);
              ED_area_tag_redraw(area);
            }
          }
        }
      }
    }
  }
}

void ED_area_type_hud_ensure(bContext *C, ScrArea *area)
{
  wmWindowManager *wm = CTX_wm_manager(C);
  ED_area_type_hud_clear(wm, area);

  ARegionType *art = BKE_regiontype_from_id(area->type, RGN_TYPE_HUD);
  if (art == nullptr) {
    return;
  }

  ARegion *region = BKE_area_find_region_type(area, RGN_TYPE_HUD);

  if (region && (region->flag & RGN_FLAG_HIDDEN_BY_USER)) {
    /* The region is intentionally hidden by the user, don't show it. */
    hud_region_hide(region);
    return;
  }

  bool init = false;
  const bool was_hidden = region == nullptr || region->runtime->visible == false;
  ARegion *region_op = CTX_wm_region(C);
  BLI_assert((region_op == nullptr) || (region_op->regiontype != RGN_TYPE_HUD));
  const int region_index_hint = region_op ? area_calc_region_type_index(area, region_op) : -1;
  if (!last_redo_poll(C, region_op ? region_op->regiontype : -1, region_index_hint)) {
    if (region) {
      ED_region_tag_redraw(region);
      hud_region_hide(region);
    }
    return;
  }

  if (region == nullptr) {
    init = true;
    region = hud_region_add(area);
    region->runtime->type = art;
  }

  /* Let 'ED_area_update_region_sizes' do the work of placing the region.
   * Otherwise we could set the 'region->winrct' & 'region->winx/winy' here. */
  if (init) {
    ED_area_tag_region_size_update(area, region);
  }
  else {
    if (region->flag & RGN_FLAG_HIDDEN) {
      /* Also forces recalculating HUD size in hud_region_layout(). */
      ED_area_tag_region_size_update(area, region);
    }
    region->flag &= ~RGN_FLAG_HIDDEN;
  }

  {
    HudRegionData *hrd = static_cast<HudRegionData *>(region->regiondata);
    if (hrd == nullptr) {
      hrd = MEM_callocN<HudRegionData>(__func__);
      region->regiondata = hrd;
    }
    if (region_op) {
      hrd->regionid = region_op->regiontype;
      hrd->region_index_hint = region_index_hint;
    }
    else {
      hrd->regionid = -1;
      hrd->region_index_hint = -1;
    }
  }

  if (init) {
    /* This is needed or 'winrct' will be invalid. */
    wmWindow *win = CTX_wm_window(C);
    ED_area_update_region_sizes(wm, win, area);
  }

  ED_region_floating_init(region);
  ED_region_tag_redraw(region);

  /* We need to update/initialize the runtime offsets. */
  ARegion *region_win = BKE_area_find_region_type(area, RGN_TYPE_WINDOW);
  if (region_win) {
    float x, y;

    UI_view2d_scroller_size_get(&region_win->v2d, true, &x, &y);
    region->runtime->offset_x = x;
    region->runtime->offset_y = y;
  }

  /* Reset zoom level (not well supported). */
  rctf reset_rect = {};
  reset_rect.xmax = region->winx;
  reset_rect.ymax = region->winy;
  region->v2d.cur = region->v2d.tot = reset_rect;
  region->v2d.minzoom = 1.0f;
  region->v2d.maxzoom = 1.0f;

  region->runtime->visible = !(region->flag & RGN_FLAG_HIDDEN);

  /* We shouldn't need to do this every time :S */
  /* XXX, this is evil! - it also makes the menu show on first draw. :( */
  if (region->runtime->visible) {
    ARegion *region_prev = CTX_wm_region(C);
    CTX_wm_region_set(C, region);
    hud_region_layout(C, region);
    if (was_hidden) {
      region->winx = region->v2d.winx;
      region->winy = region->v2d.winy;
      region->v2d.cur = region->v2d.tot = reset_rect;
    }
    CTX_wm_region_set(C, region_prev);
  }

  region->runtime->visible = !((region->flag & RGN_FLAG_HIDDEN) ||
                               (region->flag & RGN_FLAG_TOO_SMALL));
}

ARegion *ED_area_type_hud_redo_region_find(const ScrArea *area, const ARegion *hud_region)
{
  BLI_assert(hud_region->regiontype == RGN_TYPE_HUD);
  HudRegionData *hrd = static_cast<HudRegionData *>(hud_region->regiondata);

  if (hrd->regionid == -1) {
    return nullptr;
  }

  return area_find_region_by_type_and_index_hint(area, hrd->regionid, hrd->region_index_hint);
}

/** \} */
