/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2010 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup spinfo
 */

#include <limits.h>
#include <string.h>

#include "BLI_utildefines.h"

#include "DNA_screen_types.h"
#include "DNA_space_types.h"

#include "BKE_report.h"

#include "UI_interface.h"
#include "UI_resources.h"
#include "UI_view2d.h"

#include "info_intern.h"
#include "textview.h"

static enum eTextViewContext_LineFlag report_line_data(TextViewContext *tvc,
                                                       uchar fg[4],
                                                       uchar bg[4],
                                                       int *r_icon,
                                                       uchar r_icon_fg[4],
                                                       uchar r_icon_bg[4])
{
  const Report *report = tvc->iter;

  /* Same text color no matter what type of report. */
  UI_GetThemeColor4ubv((report->flag & SELECT) ? TH_INFO_SELECTED_TEXT : TH_TEXT, fg);

  /* Zebra striping for background. */
  int bg_id = (report->flag & SELECT) ? TH_INFO_SELECTED : TH_BACK;
  int shade = (tvc->iter_tmp % 2) ? 4 : -4;
  UI_GetThemeColorShade4ubv(bg_id, shade, bg);

  /* Don't show icon on subsequent rows of multi-row report. */
  *r_icon = (tvc->iter_char_begin != 0) ? ICON_NONE : UI_icon_from_report_type(report->type);

  int icon_fg_id = UI_text_colorid_from_report_type(report->type);
  int icon_bg_id = UI_icon_colorid_from_report_type(report->type);

  if (report->flag & SELECT) {
    icon_fg_id = TH_INFO_SELECTED;
    icon_bg_id = TH_INFO_SELECTED_TEXT;
  }

  if (*r_icon != ICON_NONE) {
    UI_GetThemeColor4ubv(icon_fg_id, r_icon_fg);
    /* This theme color is RGB only, so set alpha. */
    r_icon_fg[3] = 255;
    UI_GetThemeColor4ubv(icon_bg_id, r_icon_bg);
    return TVC_LINE_FG | TVC_LINE_BG | TVC_LINE_ICON | TVC_LINE_ICON_FG | TVC_LINE_ICON_BG;
  }

  return TVC_LINE_FG | TVC_LINE_BG;
}

/* reports! */
static void report_textview_init__internal(TextViewContext *tvc)
{
  const Report *report = tvc->iter;
  const char *str = report->message;
  for (int i = tvc->iter_char_end - 1; i >= 0; i -= 1) {
    if (str[i] == '\n') {
      tvc->iter_char_begin = i + 1;
      return;
    }
  }
  tvc->iter_char_begin = 0;
}

static int report_textview_skip__internal(TextViewContext *tvc)
{
  const SpaceInfo *sinfo = tvc->arg1;
  const int report_mask = info_report_mask(sinfo);
  while (tvc->iter && (((const Report *)tvc->iter)->type & report_mask) == 0) {
    tvc->iter = (void *)((Link *)tvc->iter)->prev;
  }
  return (tvc->iter != NULL);
}

static int report_textview_begin(TextViewContext *tvc)
{
  const ReportList *reports = tvc->arg2;

  tvc->sel_start = 0;
  tvc->sel_end = 0;

  /* iterator */
  tvc->iter = reports->list.last;

  UI_ThemeClearColor(TH_BACK);

  tvc->iter_tmp = 0;
  if (tvc->iter && report_textview_skip__internal(tvc)) {
    /* init the newline iterator */
    const Report *report = tvc->iter;
    tvc->iter_char_end = report->len;
    report_textview_init__internal(tvc);

    return true;
  }

  return false;
}

static void report_textview_end(TextViewContext *UNUSED(tvc))
{
  /* pass */
}

static int report_textview_step(TextViewContext *tvc)
{
  /* simple case, but no newline support */
  if (tvc->iter_char_begin <= 0) {
    tvc->iter = (void *)((Link *)tvc->iter)->prev;
    if (tvc->iter && report_textview_skip__internal(tvc)) {
      tvc->iter_tmp++;

      const Report *report = tvc->iter;
      tvc->iter_char_end = report->len; /* reset start */
      report_textview_init__internal(tvc);

      return true;
    }
    return false;
  }

  /* step to the next newline */
  tvc->iter_char_end = tvc->iter_char_begin - 1;
  report_textview_init__internal(tvc);

  return true;
}

static void report_textview_line_get(TextViewContext *tvc, const char **r_line, int *r_len)
{
  const Report *report = tvc->iter;
  *r_line = report->message + tvc->iter_char_begin;
  *r_len = tvc->iter_char_end - tvc->iter_char_begin;
}

static void info_textview_draw_rect_calc(const ARegion *region,
                                         rcti *r_draw_rect,
                                         rcti *r_draw_rect_outer)
{
  const int margin = 0.45f * U.widget_unit;
  r_draw_rect->xmin = margin + UI_UNIT_X;
  r_draw_rect->xmax = region->winx - V2D_SCROLL_WIDTH;
  r_draw_rect->ymin = margin;
  r_draw_rect->ymax = region->winy;
  /* No margin at the top (allow text to scroll off the window). */

  r_draw_rect_outer->xmin = 0;
  r_draw_rect_outer->xmax = region->winx;
  r_draw_rect_outer->ymin = 0;
  r_draw_rect_outer->ymax = region->winy;
}

static int info_textview_main__internal(const SpaceInfo *sinfo,
                                        const ARegion *region,
                                        const ReportList *reports,
                                        const bool do_draw,
                                        const int mval[2],
                                        void **r_mval_pick_item,
                                        int *r_mval_pick_offset)
{
  int ret = 0;

  const View2D *v2d = &region->v2d;

  TextViewContext tvc = {0};
  tvc.begin = report_textview_begin;
  tvc.end = report_textview_end;

  tvc.step = report_textview_step;
  tvc.line_get = report_textview_line_get;
  tvc.line_data = report_line_data;
  tvc.const_colors = NULL;

  tvc.arg1 = sinfo;
  tvc.arg2 = reports;

  /* view */
  tvc.sel_start = 0;
  tvc.sel_end = 0;
  tvc.lheight = 17 * UI_DPI_FAC;
  tvc.row_vpadding = 0.4 * tvc.lheight;
  tvc.scroll_ymin = v2d->cur.ymin;
  tvc.scroll_ymax = v2d->cur.ymax;

  info_textview_draw_rect_calc(region, &tvc.draw_rect, &tvc.draw_rect_outer);

  ret = textview_draw(&tvc, do_draw, mval, r_mval_pick_item, r_mval_pick_offset);

  return ret;
}

void *info_text_pick(const SpaceInfo *sinfo,
                     const ARegion *region,
                     const ReportList *reports,
                     int mouse_y)
{
  void *mval_pick_item = NULL;
  const int mval[2] = {0, mouse_y};

  info_textview_main__internal(sinfo, region, reports, false, mval, &mval_pick_item, NULL);
  return (void *)mval_pick_item;
}

int info_textview_height(const SpaceInfo *sinfo, const ARegion *region, const ReportList *reports)
{
  const int mval[2] = {INT_MAX, INT_MAX};
  return info_textview_main__internal(sinfo, region, reports, false, mval, NULL, NULL);
}

void info_textview_main(const SpaceInfo *sinfo, const ARegion *region, const ReportList *reports)
{
  const int mval[2] = {INT_MAX, INT_MAX};
  info_textview_main__internal(sinfo, region, reports, true, mval, NULL, NULL);
}
