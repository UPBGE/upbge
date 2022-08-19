/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2009 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup edinterface
 */

#include "DNA_screen_types.h"
#include "DNA_space_types.h"

#include "BLI_math_color.h"
#include "BLI_math_vector.h"

#include "BKE_context.h"
#include "BKE_screen.h"

#include "UI_interface.h"

#include "WM_api.h"
#include "WM_types.h"

#include "interface_intern.h"

#include "eyedropper_intern.h" /* own include */

/* -------------------------------------------------------------------- */
/* Keymap
 */
/** \name Modal Keymap
 * \{ */

wmKeyMap *eyedropper_modal_keymap(wmKeyConfig *keyconf)
{
  static const EnumPropertyItem modal_items[] = {
      {EYE_MODAL_CANCEL, "CANCEL", 0, "Cancel", ""},
      {EYE_MODAL_SAMPLE_CONFIRM, "SAMPLE_CONFIRM", 0, "Confirm Sampling", ""},
      {EYE_MODAL_SAMPLE_BEGIN, "SAMPLE_BEGIN", 0, "Start Sampling", ""},
      {EYE_MODAL_SAMPLE_RESET, "SAMPLE_RESET", 0, "Reset Sampling", ""},
      {0, NULL, 0, NULL, NULL},
  };

  wmKeyMap *keymap = WM_modalkeymap_find(keyconf, "Eyedropper Modal Map");

  /* this function is called for each spacetype, only needs to add map once */
  if (keymap && keymap->modal_items) {
    return NULL;
  }

  keymap = WM_modalkeymap_ensure(keyconf, "Eyedropper Modal Map", modal_items);

  /* assign to operators */
  WM_modalkeymap_assign(keymap, "UI_OT_eyedropper_colorramp");
  WM_modalkeymap_assign(keymap, "UI_OT_eyedropper_color");
  WM_modalkeymap_assign(keymap, "UI_OT_eyedropper_id");
  WM_modalkeymap_assign(keymap, "UI_OT_eyedropper_depth");
  WM_modalkeymap_assign(keymap, "UI_OT_eyedropper_driver");
  WM_modalkeymap_assign(keymap, "UI_OT_eyedropper_gpencil_color");

  return keymap;
}

wmKeyMap *eyedropper_colorband_modal_keymap(wmKeyConfig *keyconf)
{
  static const EnumPropertyItem modal_items_point[] = {
      {EYE_MODAL_POINT_CANCEL, "CANCEL", 0, "Cancel", ""},
      {EYE_MODAL_POINT_SAMPLE, "SAMPLE_SAMPLE", 0, "Sample a Point", ""},
      {EYE_MODAL_POINT_CONFIRM, "SAMPLE_CONFIRM", 0, "Confirm Sampling", ""},
      {EYE_MODAL_POINT_RESET, "SAMPLE_RESET", 0, "Reset Sampling", ""},
      {0, NULL, 0, NULL, NULL},
  };

  wmKeyMap *keymap = WM_modalkeymap_find(keyconf, "Eyedropper ColorRamp PointSampling Map");
  if (keymap && keymap->modal_items) {
    return keymap;
  }

  keymap = WM_modalkeymap_ensure(
      keyconf, "Eyedropper ColorRamp PointSampling Map", modal_items_point);

  /* assign to operators */
  WM_modalkeymap_assign(keymap, "UI_OT_eyedropper_colorramp_point");

  return keymap;
}

/** \} */

/* -------------------------------------------------------------------- */
/* Utility Functions
 */

/** \name Generic Shared Functions
 * \{ */

static void eyedropper_draw_cursor_text_ex(const int xy[2], const char *name)
{
  const uiFontStyle *fstyle = UI_FSTYLE_WIDGET;

  /* Use the theme settings from tooltips. */
  const bTheme *btheme = UI_GetTheme();
  const uiWidgetColors *wcol = &btheme->tui.wcol_tooltip;

  float col_fg[4], col_bg[4];
  rgba_uchar_to_float(col_fg, wcol->text);
  rgba_uchar_to_float(col_bg, wcol->inner);

  UI_fontstyle_draw_simple_backdrop(fstyle, xy[0], xy[1] + U.widget_unit, name, col_fg, col_bg);
}

void eyedropper_draw_cursor_text_window(const struct wmWindow *window, const char *name)
{
  if (name[0] == '\0') {
    return;
  }

  eyedropper_draw_cursor_text_ex(window->eventstate->xy, name);
}

void eyedropper_draw_cursor_text_region(const int xy[2], const char *name)
{
  if (name[0] == '\0') {
    return;
  }

  eyedropper_draw_cursor_text_ex(xy, name);
}

uiBut *eyedropper_get_property_button_under_mouse(bContext *C, const wmEvent *event)
{
  bScreen *screen = CTX_wm_screen(C);
  ScrArea *area = BKE_screen_find_area_xy(screen, SPACE_TYPE_ANY, event->xy);
  const ARegion *region = BKE_area_find_region_xy(area, RGN_TYPE_ANY, event->xy);

  uiBut *but = ui_but_find_mouse_over(region, event);

  if (ELEM(NULL, but, but->rnapoin.data, but->rnaprop)) {
    return NULL;
  }
  return but;
}

void datadropper_win_area_find(
    const bContext *C, const int mval[2], int r_mval[2], wmWindow **r_win, ScrArea **r_area)
{
  bScreen *screen = CTX_wm_screen(C);

  *r_win = CTX_wm_window(C);
  *r_area = BKE_screen_find_area_xy(screen, SPACE_TYPE_ANY, mval);
  if (*r_area == NULL) {
    *r_win = WM_window_find_under_cursor(*r_win, mval, r_mval);
    if (*r_win) {
      screen = WM_window_get_active_screen(*r_win);
      *r_area = BKE_screen_find_area_xy(screen, SPACE_TYPE_ANY, r_mval);
    }
  }
  else if (mval != r_mval) {
    copy_v2_v2_int(r_mval, mval);
  }
}

/** \} */
