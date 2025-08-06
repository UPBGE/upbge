/* SPDX-FileCopyrightText: 2019 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edanimation
 */

#include "BKE_context.hh"
#include "BKE_scene.hh"

#include "GPU_immediate.hh"
#include "GPU_matrix.hh"
#include "GPU_state.hh"

#include "ED_time_scrub_ui.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "UI_interface.hh"
#include "UI_interface_icons.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"
#include "UI_view2d.hh"

#include "DNA_scene_types.h"

#include "BLI_math_base.h"
#include "BLI_rect.h"
#include "BLI_string_utf8.h"
#include "BLI_timecode.h"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

void ED_time_scrub_region_rect_get(const ARegion *region, rcti *r_rect)
{
  r_rect->xmin = 0;
  r_rect->xmax = region->winx;
  r_rect->ymax = region->winy;
  r_rect->ymin = r_rect->ymax - UI_TIME_SCRUB_MARGIN_Y;
}

static int get_centered_text_y(const rcti *rect)
{
  return BLI_rcti_cent_y(rect) - UI_SCALE_FAC * 4;
}

static void draw_background(const rcti *rect)
{
  uint pos = GPU_vertformat_attr_add(
      immVertexFormat(), "pos", blender::gpu::VertAttrType::SFLOAT_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

  immUniformThemeColor(TH_TIME_SCRUB_BACKGROUND);

  GPU_blend(GPU_BLEND_ALPHA);

  immRectf(pos, rect->xmin, rect->ymin, rect->xmax, rect->ymax);

  GPU_blend(GPU_BLEND_NONE);

  immUnbindProgram();
}

static void get_current_time_str(
    const Scene *scene, bool display_seconds, int frame, char *r_str, uint str_maxncpy)
{
  if (display_seconds) {
    BLI_timecode_string_from_time(r_str, str_maxncpy, -1, FRA2TIME(frame), FPS, U.timecode_style);
  }
  else {
    BLI_snprintf_utf8(r_str, str_maxncpy, "%d", frame);
  }
}

static void draw_current_frame(const Scene *scene,
                               bool display_seconds,
                               const View2D *v2d,
                               const rcti *scrub_region_rect,
                               int current_frame,
                               bool display_stalk = true)
{
  const uiFontStyle *fstyle = UI_FSTYLE_WIDGET;
  int frame_x = UI_view2d_view_to_region_x(v2d, current_frame);

  char frame_str[64];
  get_current_time_str(scene, display_seconds, current_frame, frame_str, sizeof(frame_str));
  float text_width = UI_fontstyle_string_width(fstyle, frame_str);
  float box_width = std::max(text_width + 8 * UI_SCALE_FAC, 24 * UI_SCALE_FAC);
  float box_padding = 3 * UI_SCALE_FAC;

  float bg_color[4];
  UI_GetThemeColorShade4fv(TH_CFRAME, -5, bg_color);

  if (display_stalk) {
    /* Draw vertical line from the bottom of the current frame box to the bottom of the screen. */
    const float subframe_x = UI_view2d_view_to_region_x(v2d, BKE_scene_ctime_get(scene));
    GPUVertFormat *format = immVertexFormat();
    uint pos = GPU_vertformat_attr_add(format, "pos", blender::gpu::VertAttrType::SFLOAT_32_32);
    GPU_blend(GPU_BLEND_ALPHA);
    immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

    /* There are two usages of "1.0f" below that are not scaled. This is
     * used to force an odd width (but still pixel-aligned) to better
     * line up with the odd widths of the keyframe icons. #98089. */

    /* Outline. */
    immUniformThemeColorShadeAlpha(TH_BACK, -25, -100);
    immRectf(pos,
             floor(subframe_x - U.pixelsize - U.pixelsize),
             scrub_region_rect->ymax - box_padding - U.pixelsize,
             floor(subframe_x + U.pixelsize + 1.0f + U.pixelsize),
             0.0f);

    /* Line. */
    immUniformThemeColor(TH_CFRAME);
    immRectf(pos,
             floor(subframe_x - U.pixelsize),
             scrub_region_rect->ymax - box_padding - U.pixelsize,
             floor(subframe_x + U.pixelsize + 1.0f),
             0.0f);
    immUnbindProgram();
    GPU_blend(GPU_BLEND_NONE);
  }

  UI_draw_roundbox_corner_set(UI_CNR_ALL);

  float outline_color[4];
  UI_GetThemeColorShade4fv(TH_CFRAME, 5, outline_color);

  rctf rect{};
  rect.xmin = floor(frame_x - (box_width / 2.0f) + U.pixelsize + 1.0f);
  rect.xmax = ceil(frame_x + (box_width / 2.0f));
  rect.ymin = floor(scrub_region_rect->ymin + box_padding);
  rect.ymax = ceil(scrub_region_rect->ymax - box_padding);
  UI_draw_roundbox_4fv_ex(
      &rect, bg_color, nullptr, 1.0f, outline_color, U.pixelsize, 4 * UI_SCALE_FAC);

  uchar text_color[4];
  UI_GetThemeColor4ubv(TH_HEADER_TEXT_HI, text_color);

  const int y = BLI_rcti_cent_y(scrub_region_rect) - int(fstyle->points * UI_SCALE_FAC * 0.38f);

  UI_fontstyle_draw_simple(
      fstyle, ceil(frame_x - (text_width / 2.0f) + 1.0f), y, frame_str, text_color);
}

void ED_time_scrub_draw_current_frame(const ARegion *region,
                                      const Scene *scene,
                                      bool display_seconds,
                                      bool display_stalk)
{
  const View2D *v2d = &region->v2d;
  GPU_matrix_push_projection();
  wmOrtho2_region_pixelspace(region);

  rcti scrub_region_rect;
  ED_time_scrub_region_rect_get(region, &scrub_region_rect);

  draw_current_frame(
      scene, display_seconds, v2d, &scrub_region_rect, scene->r.cfra, display_stalk);
  GPU_matrix_pop_projection();
}

void ED_time_scrub_draw(const ARegion *region,
                        const Scene *scene,
                        bool display_seconds,
                        bool discrete_frames)
{
  const View2D *v2d = &region->v2d;

  GPU_matrix_push_projection();
  wmOrtho2_region_pixelspace(region);

  rcti scrub_region_rect;
  ED_time_scrub_region_rect_get(region, &scrub_region_rect);

  draw_background(&scrub_region_rect);

  rcti numbers_rect = scrub_region_rect;
  numbers_rect.ymin = get_centered_text_y(&scrub_region_rect) - 4 * UI_SCALE_FAC;
  if (discrete_frames) {
    UI_view2d_draw_scale_x__discrete_frames_or_seconds(
        region, v2d, &numbers_rect, scene, display_seconds, TH_TEXT);
  }
  else {
    UI_view2d_draw_scale_x__frames_or_seconds(
        region, v2d, &numbers_rect, scene, display_seconds, TH_TEXT);
  }

  GPU_matrix_pop_projection();
}

rcti ED_time_scrub_clamp_scroller_mask(const rcti &scroller_mask)
{
  rcti clamped_mask = scroller_mask;
  clamped_mask.ymax -= UI_TIME_SCRUB_MARGIN_Y;
  return clamped_mask;
}

bool ED_time_scrub_event_in_region(const ARegion *region, const wmEvent *event)
{
  rcti rect = region->winrct;
  rect.ymin = rect.ymax - UI_TIME_SCRUB_MARGIN_Y;
  return BLI_rcti_isect_pt_v(&rect, event->xy);
}

bool ED_time_scrub_event_in_region_poll(const wmWindow * /*win*/,
                                        const ScrArea * /*area*/,
                                        const ARegion *region,
                                        const wmEvent *event)
{
  return ED_time_scrub_event_in_region(region, event);
}

void ED_time_scrub_channel_search_draw(const bContext *C, ARegion *region, bDopeSheet *dopesheet)
{
  GPU_matrix_push_projection();
  wmOrtho2_region_pixelspace(region);

  rcti rect;
  rect.xmin = 0;
  rect.xmax = region->winx;
  rect.ymin = region->winy - UI_TIME_SCRUB_MARGIN_Y;
  rect.ymax = region->winy;

  uint pos = GPU_vertformat_attr_add(
      immVertexFormat(), "pos", blender::gpu::VertAttrType::SFLOAT_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
  immUniformThemeColor(TH_BACK);
  immRectf(pos, rect.xmin, rect.ymin, rect.xmax, rect.ymax);
  immUnbindProgram();

  PointerRNA ptr = RNA_pointer_create_discrete(&CTX_wm_screen(C)->id, &RNA_DopeSheet, dopesheet);

  const uiStyle *style = UI_style_get_dpi();
  const float padding_x = 2 * UI_SCALE_FAC;
  const float padding_y = UI_SCALE_FAC;

  uiBlock *block = UI_block_begin(C, region, __func__, blender::ui::EmbossType::Emboss);
  uiLayout &layout = blender::ui::block_layout(block,
                                               blender::ui::LayoutDirection::Vertical,
                                               blender::ui::LayoutType::Header,
                                               rect.xmin + padding_x,
                                               rect.ymin + UI_UNIT_Y + padding_y,
                                               BLI_rcti_size_x(&rect) - 2 * padding_x,
                                               1,
                                               0,
                                               style);
  layout.scale_y_set((UI_UNIT_Y - padding_y) / UI_UNIT_Y);
  blender::ui::block_layout_set_current(block, &layout);
  UI_block_align_begin(block);
  layout.prop(&ptr, "filter_text", UI_ITEM_NONE, "", ICON_NONE);
  layout.prop(&ptr, "use_filter_invert", UI_ITEM_NONE, "", ICON_ARROW_LEFTRIGHT);
  UI_block_align_end(block);
  blender::ui::block_layout_resolve(block);

  /* Make sure the events are consumed from the search and don't reach other UI blocks since this
   * is drawn on top of animation-channels. */
  UI_block_flag_enable(block, UI_BLOCK_CLIP_EVENTS);
  UI_block_bounds_set_normal(block, 0);
  UI_block_end(C, block);
  UI_block_draw(C, block);

  GPU_matrix_pop_projection();
}
