/* SPDX-FileCopyrightText: 2008 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edutil
 */

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_math_vector.h"
#include "BLI_rect.h"
#include "BLI_string_utf8.h"
#include "BLI_utildefines.h"

#include "BLT_translation.hh"

#include "BKE_context.hh"
#include "BKE_global.hh"
#include "BKE_image.hh"
#include "BKE_screen.hh"

#include "BLF_api.hh"

#include "IMB_imbuf_types.hh"
#include "IMB_metadata.hh"

#include "ED_screen.hh"
#include "ED_space_api.hh"
#include "ED_util.hh"

#include "GPU_immediate.hh"
#include "GPU_matrix.hh"
#include "GPU_state.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"

#include "WM_api.hh"
#include "WM_types.hh"

/* -------------------------------------------------------------------- */
/** \name Generic Slider
 *
 * The generic slider is supposed to be called during modal operations. It calculates a factor
 * value based on mouse position and draws a visual representation. In order to use it, you need to
 * store a reference to a #tSlider in your operator which you get by calling #ED_slider_create.
 * Then you need to update it during modal operations by calling #ED_slider_modal", which will
 * update #tSlider.factor for you to use. To remove drawing and free the memory, call
 * #ED_slider_destroy.
 * \{ */

#define SLIDE_PIXEL_DISTANCE (300.0f * UI_SCALE_FAC)
#define OVERSHOOT_RANGE_DELTA 0.2f
#define SLIDER_UNIT_STRING_SIZE 64

struct tSlider {
  Scene *scene;
  ScrArea *area;

  /** Header of the region used for drawing the slider. */
  ARegion *region_header;

  /** Draw callback handler. */
  void *draw_handle;

  /** Accumulative factor (not clamped or rounded). */
  float raw_factor;

  /** Current value for determining the influence of whatever is relevant. */
  float factor;

  /** Last mouse cursor position used for mouse movement delta calculation. */
  float last_cursor[2];

  /** Range of the slider without overshoot. */
  float factor_bounds[2];

  /** Change if the slider range is so large/small that a 0.1 increment is meaningless. */
  float increment_step;

  /* How the factor number is drawn. When drawing percent it is factor*100. */
  SliderMode slider_mode;

  /* Optional string that will display next to the slider to indicate which property is modified
   * right now. */
  std::string property_label;

  /* What unit to add to the slider. */
  char unit_string[SLIDER_UNIT_STRING_SIZE];

  /** Enable range beyond factor_bounds.
   * This is set by the code that uses the slider, as not all operations support
   * extrapolation. */
  bool allow_overshoot_lower;
  bool allow_overshoot_upper;

  /** Allow overshoot or clamp between factor_bounds.
   * This is set by the artist while using the slider. */
  bool overshoot;

  /** Whether keeping CTRL pressed will snap to multiples of `increment_step`.
   * Default is true. Set to false if the CTRL key is needed for other means. */
  bool allow_increments;

  /** Move factor in multiples of `increment_step`. */
  bool increments;

  /** Reduces factor delta from mouse movement. */
  bool precision;
};

static void draw_overshoot_triangle(const uint8_t color[4],
                                    const bool facing_right,
                                    const float x,
                                    const float y)
{
  const uint shdr_pos_2d = GPU_vertformat_attr_add(
      immVertexFormat(), "pos", blender::gpu::VertAttrType::SFLOAT_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
  GPU_blend(GPU_BLEND_ALPHA);
  GPU_polygon_smooth(true);
  immUniformColor3ubvAlpha(color, 225);
  const float triangle_side_length = facing_right ? 6 * U.pixelsize : -6 * U.pixelsize;
  const float triangle_offset = facing_right ? 2 * U.pixelsize : -2 * U.pixelsize;

  immBegin(GPU_PRIM_TRIS, 3);
  immVertex2f(shdr_pos_2d, x + triangle_offset + triangle_side_length, y);
  immVertex2f(shdr_pos_2d, x + triangle_offset, y + triangle_side_length / 2);
  immVertex2f(shdr_pos_2d, x + triangle_offset, y - triangle_side_length / 2);
  immEnd();

  GPU_polygon_smooth(false);
  GPU_blend(GPU_BLEND_NONE);
  immUnbindProgram();
}

static void draw_ticks(const float start_factor,
                       const float end_factor,
                       const float line_start[2],
                       const float base_tick_height,
                       const float line_width,
                       const uint8_t color_overshoot[4],
                       const uint8_t color_line[4])
{
  /* Use factor represented as 0-100 int to avoid floating point precision problems. */
  const int tick_increment = 10;

  /* Round initial_tick_factor up to the next tick_increment. */
  int tick_percentage = ceil((start_factor * 100) / tick_increment) * tick_increment;

  while (tick_percentage <= int(end_factor * 100)) {
    float tick_height;
    /* Different ticks have different heights. Multiples of 100% are the tallest, 50% is a bit
     * smaller and the rest is the minimum size. */
    if (tick_percentage % 100 == 0) {
      tick_height = base_tick_height;
    }
    else if (tick_percentage % 50 == 0) {
      tick_height = base_tick_height * 0.8;
    }
    else {
      tick_height = base_tick_height * 0.5;
    }

    const float x = line_start[0] +
                    ((float(tick_percentage) / 100) - start_factor) * SLIDE_PIXEL_DISTANCE;
    rctf tick_rect{};
    tick_rect.xmin = x - (line_width / 2);
    tick_rect.xmax = x + (line_width / 2);
    tick_rect.ymin = line_start[1] - (tick_height / 2);
    tick_rect.ymax = line_start[1] + (tick_height / 2);

    if (tick_percentage < 0 || tick_percentage > 100) {
      UI_draw_roundbox_3ub_alpha(&tick_rect, true, 1, color_overshoot, 255);
    }
    else {
      UI_draw_roundbox_3ub_alpha(&tick_rect, true, 1, color_line, 255);
    }
    tick_percentage += tick_increment;
  }
}

static void draw_main_line(const rctf *main_line_rect,
                           const float factor,
                           const bool overshoot,
                           const uint8_t color_overshoot[4],
                           const uint8_t color_line[4])
{
  if (overshoot) {
    /* In overshoot mode, draw the 0-100% range differently to provide a visual reference. */
    const float line_zero_percent = main_line_rect->xmin -
                                    ((factor - 0.5f - OVERSHOOT_RANGE_DELTA) *
                                     SLIDE_PIXEL_DISTANCE);

    const float clamped_line_zero_percent = clamp_f(
        line_zero_percent, main_line_rect->xmin, main_line_rect->xmax);
    const float clamped_line_hundred_percent = clamp_f(
        line_zero_percent + SLIDE_PIXEL_DISTANCE, main_line_rect->xmin, main_line_rect->xmax);

    rctf left_overshoot_line_rect{};
    left_overshoot_line_rect.xmin = main_line_rect->xmin;
    left_overshoot_line_rect.xmax = clamped_line_zero_percent;
    left_overshoot_line_rect.ymin = main_line_rect->ymin;
    left_overshoot_line_rect.ymax = main_line_rect->ymax;

    rctf right_overshoot_line_rect{};
    right_overshoot_line_rect.xmin = clamped_line_hundred_percent;
    right_overshoot_line_rect.xmax = main_line_rect->xmax;
    right_overshoot_line_rect.ymin = main_line_rect->ymin;
    right_overshoot_line_rect.ymax = main_line_rect->ymax;

    UI_draw_roundbox_3ub_alpha(&left_overshoot_line_rect, true, 0, color_overshoot, 255);
    UI_draw_roundbox_3ub_alpha(&right_overshoot_line_rect, true, 0, color_overshoot, 255);

    rctf non_overshoot_line_rect{};
    non_overshoot_line_rect.xmin = clamped_line_zero_percent;
    non_overshoot_line_rect.xmax = clamped_line_hundred_percent;
    non_overshoot_line_rect.ymin = main_line_rect->ymin;
    non_overshoot_line_rect.ymax = main_line_rect->ymax;
    UI_draw_roundbox_3ub_alpha(&non_overshoot_line_rect, true, 0, color_line, 255);
  }
  else {
    UI_draw_roundbox_3ub_alpha(main_line_rect, true, 0, color_line, 255);
  }
}

static void draw_backdrop(const int fontid,
                          const rctf *main_line_rect,
                          const uint8_t color_bg[4],
                          const short region_y_size,
                          const float base_tick_height,
                          const std::string &property_label)
{
  float percent_string_pixel_size[2];
  const char *percentage_string_placeholder = "000%%";
  BLF_width_and_height(fontid,
                       percentage_string_placeholder,
                       sizeof(percentage_string_placeholder),
                       &percent_string_pixel_size[0],
                       &percent_string_pixel_size[1]);

  float property_name_pixel_size[2];
  BLF_width_and_height(fontid,
                       property_label.c_str(),
                       property_label.size(),
                       &property_name_pixel_size[0],
                       &property_name_pixel_size[1]);
  const float pad[2] = {(region_y_size - base_tick_height) / 2 + 12.0f * U.pixelsize,
                        2.0f * U.pixelsize};
  rctf backdrop_rect{};
  backdrop_rect.xmin = main_line_rect->xmin - property_name_pixel_size[0] - pad[0];
  backdrop_rect.xmax = main_line_rect->xmax + percent_string_pixel_size[0] + pad[0];
  backdrop_rect.ymin = pad[1];
  backdrop_rect.ymax = region_y_size - pad[1];
  UI_draw_roundbox_3ub_alpha(&backdrop_rect, true, 4.0f, color_bg, color_bg[3]);
}

/**
 * Draw an on screen Slider for a Pose Slide Operator.
 */
static void slider_draw(const bContext * /*C*/, ARegion *region, void *arg)
{
  tSlider *slider = static_cast<tSlider *>(arg);

  /* Only draw in region from which the Operator was started. */
  if (region != slider->region_header) {
    return;
  }

  uint8_t color_text[4];
  uint8_t color_line[4];
  uint8_t color_handle[4];
  uint8_t color_overshoot[4];
  uint8_t color_bg[4];

  /* Get theme colors. */
  UI_GetThemeColor4ubv(TH_HEADER_TEXT_HI, color_handle);
  UI_GetThemeColor4ubv(TH_HEADER_TEXT, color_text);
  UI_GetThemeColor4ubv(TH_HEADER_TEXT, color_line);
  UI_GetThemeColor4ubv(TH_HEADER_TEXT, color_overshoot);
  UI_GetThemeColor4ubv(TH_HEADER, color_bg);

  color_overshoot[0] = color_overshoot[0] * 0.8;
  color_overshoot[1] = color_overshoot[1] * 0.8;
  color_overshoot[2] = color_overshoot[2] * 0.8;
  color_bg[3] = 160;

  /* Get the default font. */
  const uiStyle *style = UI_style_get();
  const uiFontStyle *fstyle = &style->widget;
  const int fontid = fstyle->uifont_id;
  BLF_color3ubv(fontid, color_text);
  BLF_rotation(fontid, 0.0f);

  const float line_width = 1.5 * U.pixelsize;
  const float base_tick_height = 12.0 * U.pixelsize;
  const float line_y = region->winy / 2;

  rctf main_line_rect{};
  main_line_rect.xmin = (region->winx / 2) - (SLIDE_PIXEL_DISTANCE / 2);
  main_line_rect.xmax = (region->winx / 2) + (SLIDE_PIXEL_DISTANCE / 2);
  main_line_rect.ymin = line_y - line_width / 2;
  main_line_rect.ymax = line_y + line_width / 2;

  float line_start_factor = 0;
  int handle_pos_x;
  if (slider->overshoot) {
    main_line_rect.xmin = main_line_rect.xmin - SLIDE_PIXEL_DISTANCE * OVERSHOOT_RANGE_DELTA;
    main_line_rect.xmax = main_line_rect.xmax + SLIDE_PIXEL_DISTANCE * OVERSHOOT_RANGE_DELTA;
    line_start_factor = slider->factor - 0.5f - OVERSHOOT_RANGE_DELTA;
    handle_pos_x = region->winx / 2;
  }
  else {
    const float total_range = slider->factor_bounds[1] - slider->factor_bounds[0];
    /* 0-1 value of the representing the position of the slider in the allowed range. */
    const float range_factor = (slider->factor - slider->factor_bounds[0]) / total_range;
    handle_pos_x = main_line_rect.xmin + SLIDE_PIXEL_DISTANCE * range_factor;
  }

  draw_backdrop(fontid,
                &main_line_rect,
                color_bg,
                slider->region_header->winy,
                base_tick_height,
                slider->property_label);

  draw_main_line(&main_line_rect, slider->factor, slider->overshoot, color_overshoot, color_line);

  const float factor_range = slider->overshoot ? 1 + OVERSHOOT_RANGE_DELTA * 2 : 1;
  const float line_start_position[2] = {main_line_rect.xmin, line_y};
  draw_ticks(line_start_factor,
             line_start_factor + factor_range,
             line_start_position,
             base_tick_height,
             line_width,
             color_overshoot,
             color_line);

  /* Draw triangles at the ends of the line in overshoot mode to indicate direction of 0-100%
   * range. */
  if (slider->overshoot) {
    if (slider->factor > 1 + OVERSHOOT_RANGE_DELTA + 0.5) {
      draw_overshoot_triangle(color_line, false, main_line_rect.xmin, line_y);
    }
    if (slider->factor < 0 - OVERSHOOT_RANGE_DELTA - 0.5) {
      draw_overshoot_triangle(color_line, true, main_line_rect.xmax, line_y);
    }
  }

  /* Draw handle indicating current factor. */
  rctf handle_rect{};
  handle_rect.xmin = handle_pos_x - (line_width);
  handle_rect.xmax = handle_pos_x + (line_width);
  handle_rect.ymin = line_y - (base_tick_height / 2);
  handle_rect.ymax = line_y + (base_tick_height / 2);

  UI_draw_roundbox_3ub_alpha(&handle_rect, true, 1, color_handle, 255);

  char factor_string[256];
  switch (slider->slider_mode) {
    case SLIDER_MODE_PERCENT:
      SNPRINTF_UTF8(factor_string, "%.0f %s", slider->factor * 100, slider->unit_string);
      break;
    case SLIDER_MODE_FLOAT:
      SNPRINTF_UTF8(factor_string, "%.1f %s", slider->factor, slider->unit_string);
      break;
  }

  /* Draw factor string. */
  float factor_string_pixel_size[2];
  BLF_width_and_height(fontid,
                       factor_string,
                       sizeof(factor_string),
                       &factor_string_pixel_size[0],
                       &factor_string_pixel_size[1]);

  const float text_padding = 12.0 * U.pixelsize;
  const float factor_string_pos_x = main_line_rect.xmax + text_padding;
  BLF_position(
      fontid, factor_string_pos_x, (region->winy / 2) - factor_string_pixel_size[1] / 2, 0.0f);
  BLF_draw(fontid, factor_string, sizeof(factor_string));

  if (!slider->property_label.empty()) {
    float property_name_pixel_size[2];
    BLF_width_and_height(fontid,
                         slider->property_label.c_str(),
                         slider->property_label.length(),
                         &property_name_pixel_size[0],
                         &property_name_pixel_size[1]);
    BLF_position(fontid,
                 main_line_rect.xmin - text_padding - property_name_pixel_size[0],
                 (region->winy / 2) - property_name_pixel_size[1] / 2,
                 0.0f);
    BLF_draw(fontid, slider->property_label.c_str(), slider->property_label.length());
  }
}

static void slider_update_factor(tSlider *slider, const wmEvent *event)
{
  /* Normalize so no matter the factor bounds, the mouse distance traveled from min to max is
   * constant. */
  const float slider_range = slider->factor_bounds[1] - slider->factor_bounds[0];
  const float factor_delta = (event->xy[0] - slider->last_cursor[0]) /
                             (SLIDE_PIXEL_DISTANCE / slider_range);
  /* Reduced factor delta in precision mode (shift held). */
  slider->raw_factor += slider->precision ? (factor_delta / 8) : factor_delta;
  slider->factor = slider->raw_factor;
  copy_v2fl_v2i(slider->last_cursor, event->xy);

  if (slider->increments) {
    slider->factor = round(slider->factor / slider->increment_step) * slider->increment_step;
  }

  if (!slider->overshoot) {
    slider->factor = clamp_f(slider->factor, slider->factor_bounds[0], slider->factor_bounds[1]);
  }
  else {
    if (!slider->allow_overshoot_lower) {
      slider->factor = max_ff(slider->factor, slider->factor_bounds[0]);
    }
    if (!slider->allow_overshoot_upper) {
      slider->factor = min_ff(slider->factor, slider->factor_bounds[1]);
    }
  }
}

tSlider *ED_slider_create(bContext *C)
{
  tSlider *slider = MEM_new<tSlider>(__func__);
  slider->scene = CTX_data_scene(C);
  slider->area = CTX_wm_area(C);
  slider->region_header = CTX_wm_region(C);

  /* Default is true, caller needs to manually set to false. */
  slider->allow_overshoot_lower = true;
  slider->allow_overshoot_upper = true;
  slider->allow_increments = true;

  slider->factor_bounds[0] = 0;
  slider->factor_bounds[1] = 1;

  slider->unit_string[0] = '%';

  slider->slider_mode = SLIDER_MODE_PERCENT;

  /* Set initial factor. */
  slider->raw_factor = 0.5f;
  slider->factor = 0.5;

  slider->increment_step = 0.1f;

  /* Add draw callback. Always in header. */
  if (slider->area) {
    LISTBASE_FOREACH (ARegion *, region, &slider->area->regionbase) {
      if (region->regiontype == RGN_TYPE_HEADER) {
        slider->region_header = region;
        if (!G.background) {
          slider->draw_handle = ED_region_draw_cb_activate(
              region->runtime->type, slider_draw, slider, REGION_DRAW_POST_PIXEL);
        }
      }
    }
  }

  /* Hide the area menu bar contents, as the slider will be drawn on top. */
  ED_area_status_text(slider->area, "");

  return slider;
}

void ED_slider_init(tSlider *slider, const wmEvent *event)
{
  copy_v2fl_v2i(slider->last_cursor, event->xy);
}

bool ED_slider_modal(tSlider *slider, const wmEvent *event)
{
  bool event_handled = true;
  /* Handle key presses. */
  switch (event->type) {
    case EVT_EKEY:
      if (slider->allow_overshoot_lower || slider->allow_overshoot_upper) {
        slider->overshoot = event->val == KM_PRESS ? !slider->overshoot : slider->overshoot;
        slider_update_factor(slider, event);
      }
      break;
    case EVT_LEFTSHIFTKEY:
    case EVT_RIGHTSHIFTKEY:
      slider->precision = event->val == KM_PRESS;
      break;
    case EVT_LEFTCTRLKEY:
    case EVT_RIGHTCTRLKEY:
      slider->increments = slider->allow_increments && event->val == KM_PRESS;
      break;
    case MOUSEMOVE:;
      /* Update factor. */
      slider_update_factor(slider, event);
      break;
    default:
      event_handled = false;
      break;
  }

  ED_region_tag_redraw(slider->region_header);

  return event_handled;
}

void ED_slider_status_string_get(const tSlider *slider,
                                 char *status_string,
                                 const size_t size_of_status_string)
{
  /* 50 characters is enough to fit the individual setting strings. Extend if message is longer. */
  char overshoot_str[50];
  char precision_str[50];
  char increments_str[50];

  if (slider->allow_overshoot_lower || slider->allow_overshoot_upper) {
    if (slider->overshoot) {
      STRNCPY_UTF8(overshoot_str, IFACE_("[E] - Disable overshoot"));
    }
    else {
      STRNCPY_UTF8(overshoot_str, IFACE_("[E] - Enable overshoot"));
    }
  }
  else {
    STRNCPY_UTF8(overshoot_str, IFACE_("Overshoot disabled"));
  }

  if (slider->precision) {
    STRNCPY_UTF8(precision_str, IFACE_("[Shift] - Precision active"));
  }
  else {
    STRNCPY_UTF8(precision_str, IFACE_("Shift - Hold for precision"));
  }

  if (slider->allow_increments) {
    if (slider->increments) {
      STRNCPY_UTF8(increments_str, IFACE_(" | [Ctrl] - Increments active"));
    }
    else {
      STRNCPY_UTF8(increments_str, IFACE_(" | Ctrl - Hold for increments"));
    }
  }
  else {
    increments_str[0] = '\0';
  }

  BLI_snprintf_utf8(status_string,
                    size_of_status_string,
                    "%s | %s%s",
                    overshoot_str,
                    precision_str,
                    increments_str);
}

void ED_slider_status_get(const tSlider *slider, WorkspaceStatus &status)
{
  if (slider->allow_overshoot_lower || slider->allow_overshoot_upper) {
    status.item_bool(IFACE_("Overshoot"), slider->overshoot, ICON_EVENT_E);
  }
  else {
    status.item(IFACE_("Overshoot Disabled"), ICON_INFO);
  }

  status.item_bool(IFACE_("Precision"), slider->precision, ICON_EVENT_SHIFT);

  if (slider->allow_increments) {
    status.item_bool(IFACE_("Snap"), slider->increments, ICON_EVENT_CTRL);
  }
}

void ED_slider_destroy(bContext *C, tSlider *slider)
{
  /* Remove draw callback. */
  if (slider->draw_handle) {
    ED_region_draw_cb_exit(slider->region_header->runtime->type, slider->draw_handle);
  }
  ED_area_status_text(slider->area, nullptr);
  ED_workspace_status_text(C, nullptr);
  MEM_delete(slider);
}

/* Setters & Getters */

float ED_slider_factor_get(const tSlider *slider)
{
  return slider->factor;
}

void ED_slider_factor_set(tSlider *slider, const float factor)
{
  slider->raw_factor = factor;
  slider->factor = factor;
  if (!slider->overshoot) {
    slider->factor = clamp_f(slider->factor, slider->factor_bounds[0], slider->factor_bounds[1]);
  }
}

void ED_slider_increment_step_set(tSlider *slider, const float increment_step)
{
  if (increment_step == 0) {
    /* Because this value is used as a divisor, it cannot be 0. */
    BLI_assert_unreachable();
    return;
  }
  slider->increment_step = increment_step;
}

void ED_slider_allow_overshoot_set(tSlider *slider, const bool lower, const bool upper)
{
  slider->allow_overshoot_lower = lower;
  slider->allow_overshoot_upper = upper;
}

bool ED_slider_allow_increments_get(const tSlider *slider)
{
  return slider->allow_increments;
}

void ED_slider_allow_increments_set(tSlider *slider, const bool value)
{
  slider->allow_increments = value;
}

void ED_slider_factor_bounds_set(tSlider *slider,
                                 float factor_bound_lower,
                                 float factor_bound_upper)
{
  slider->factor_bounds[0] = factor_bound_lower;
  slider->factor_bounds[1] = factor_bound_upper;
}

void ED_slider_mode_set(tSlider *slider, SliderMode mode)
{
  slider->slider_mode = mode;
}

SliderMode ED_slider_mode_get(const tSlider *slider)
{
  return slider->slider_mode;
}

void ED_slider_unit_set(tSlider *slider, const char *unit)
{
  STRNCPY_UTF8(slider->unit_string, unit);
}

void ED_slider_property_label_set(tSlider *slider, const char *property_label)
{
  slider->property_label.assign(property_label);
}

/** \} */

void ED_region_draw_mouse_line_cb(const bContext *C, ARegion *region, void *arg_info)
{
  wmWindow *win = CTX_wm_window(C);
  const float *mval_src = (float *)arg_info;
  const float mval_dst[2] = {
      float(win->eventstate->xy[0] - region->winrct.xmin),
      float(win->eventstate->xy[1] - region->winrct.ymin),
  };

  const uint shdr_pos = GPU_vertformat_attr_add(
      immVertexFormat(), "pos", blender::gpu::VertAttrType::SFLOAT_32_32);

  GPU_line_width(1.0f);

  immBindBuiltinProgram(GPU_SHADER_3D_LINE_DASHED_UNIFORM_COLOR);

  float viewport_size[4];
  GPU_viewport_size_get_f(viewport_size);
  immUniform2f("viewport_size", viewport_size[2] / UI_SCALE_FAC, viewport_size[3] / UI_SCALE_FAC);

  immUniform1i("colors_len", 0); /* "simple" mode */
  immUniformThemeColor3(TH_VIEW_OVERLAY);
  immUniform1f("dash_width", 6.0f);
  immUniform1f("udash_factor", 0.5f);

  immBegin(GPU_PRIM_LINES, 2);
  immVertex2fv(shdr_pos, mval_src);
  immVertex2fv(shdr_pos, mval_dst);
  immEnd();

  immUnbindProgram();
}

#define MAX_METADATA_STR 1024

static const char *meta_data_list[] = {
    "File",
    "Strip",
    "Date",
    "RenderTime",
    "Note",
    "Marker",
    "Time",
    "Frame",
    "Camera",
    "Scene",
};

BLI_INLINE bool metadata_is_valid(const ImBuf *ibuf, char *r_str, short index, int offset)
{
  return (IMB_metadata_get_field(
              ibuf->metadata, meta_data_list[index], r_str + offset, MAX_METADATA_STR - offset) &&
          r_str[0]);
}

BLI_INLINE bool metadata_is_custom_drawable(const char *field)
{
  /* Metadata field stored by Blender for multi-layer EXR images. Is rather
   * useless to be viewed all the time. Can still be seen in the Metadata
   * panel. */
  if (STREQ(field, "BlenderMultiChannel")) {
    return false;
  }
  /* Is almost always has value "scanlineimage", also useless to be seen
   * all the time. */
  if (STREQ(field, "type")) {
    return false;
  }
  return !BKE_stamp_is_known_field(field);
}

struct MetadataCustomDrawContext {
  int fontid;
  int xmin, ymin;
  int vertical_offset;
  int current_y;
};

static void metadata_custom_draw_fields(const char *field, const char *value, void *ctx_v)
{
  if (!metadata_is_custom_drawable(field)) {
    return;
  }
  MetadataCustomDrawContext *ctx = (MetadataCustomDrawContext *)ctx_v;
  char temp_str[MAX_METADATA_STR];
  SNPRINTF_UTF8(temp_str, "%s: %s", field, value);
  BLF_position(ctx->fontid, ctx->xmin, ctx->ymin + ctx->current_y, 0.0f);
  BLF_draw(ctx->fontid, temp_str, sizeof(temp_str));
  ctx->current_y += ctx->vertical_offset;
}

static void metadata_draw_imbuf(const ImBuf *ibuf, const rctf *rect, int fontid, const bool is_top)
{
  char temp_str[MAX_METADATA_STR];
  int ofs_y = 0;
  const float height = BLF_height_max(fontid);
  const float margin = height / 8;
  const float vertical_offset = (height + margin);

  /* values taking margins into account */
  const float descender = BLF_descender(fontid);
  const float xmin = (rect->xmin + margin);
  const float xmax = (rect->xmax - margin);
  const float ymin = (rect->ymin + margin) - descender;
  const float ymax = (rect->ymax - margin) - descender;

  if (is_top) {
    for (int i = 0; i < 4; i++) {
      /* first line */
      if (i == 0) {
        bool do_newline = false;
        int len = SNPRINTF_UTF8_RLEN(temp_str, "%s: ", meta_data_list[0]);
        if (metadata_is_valid(ibuf, temp_str, 0, len)) {
          BLF_position(fontid, xmin, ymax - vertical_offset, 0.0f);
          BLF_draw(fontid, temp_str, sizeof(temp_str));
          do_newline = true;
        }

        len = SNPRINTF_UTF8_RLEN(temp_str, "%s: ", meta_data_list[1]);
        if (metadata_is_valid(ibuf, temp_str, 1, len)) {
          int line_width = BLF_width(fontid, temp_str, sizeof(temp_str));
          BLF_position(fontid, xmax - line_width, ymax - vertical_offset, 0.0f);
          BLF_draw(fontid, temp_str, sizeof(temp_str));
          do_newline = true;
        }

        if (do_newline) {
          ofs_y += vertical_offset;
        }
      } /* Strip */
      else if (ELEM(i, 1, 2)) {
        int len = SNPRINTF_UTF8_RLEN(temp_str, "%s: ", meta_data_list[i + 1]);
        if (metadata_is_valid(ibuf, temp_str, i + 1, len)) {
          BLF_position(fontid, xmin, ymax - vertical_offset - ofs_y, 0.0f);
          BLF_draw(fontid, temp_str, sizeof(temp_str));
          ofs_y += vertical_offset;
        }
      } /* Note (wrapped) */
      else if (i == 3) {
        int len = SNPRINTF_UTF8_RLEN(temp_str, "%s: ", meta_data_list[i + 1]);
        if (metadata_is_valid(ibuf, temp_str, i + 1, len)) {
          ResultBLF info;
          BLF_enable(fontid, BLF_WORD_WRAP);
          BLF_wordwrap(fontid, ibuf->x - (margin * 2));
          BLF_position(fontid, xmin, ymax - vertical_offset - ofs_y, 0.0f);
          BLF_draw(fontid, temp_str, sizeof(temp_str), &info);
          BLF_wordwrap(fontid, 0);
          BLF_disable(fontid, BLF_WORD_WRAP);
          ofs_y += vertical_offset * info.lines;
        }
      }
      else {
        int len = SNPRINTF_UTF8_RLEN(temp_str, "%s: ", meta_data_list[i + 1]);
        if (metadata_is_valid(ibuf, temp_str, i + 1, len)) {
          int line_width = BLF_width(fontid, temp_str, sizeof(temp_str));
          BLF_position(fontid, xmax - line_width, ymax - vertical_offset - ofs_y, 0.0f);
          BLF_draw(fontid, temp_str, sizeof(temp_str));
          ofs_y += vertical_offset;
        }
      }
    }
  }
  else {
    MetadataCustomDrawContext ctx;
    ctx.fontid = fontid;
    ctx.xmin = xmin;
    ctx.ymin = ymin;
    ctx.current_y = ofs_y;
    ctx.vertical_offset = vertical_offset;
    IMB_metadata_foreach(ibuf, metadata_custom_draw_fields, &ctx);
    int ofs_x = 0;
    ofs_y = ctx.current_y;
    for (int i = 5; i < 10; i++) {
      int len = SNPRINTF_UTF8_RLEN(temp_str, "%s: ", meta_data_list[i]);
      if (metadata_is_valid(ibuf, temp_str, i, len)) {
        BLF_position(fontid, xmin + ofs_x, ymin + ofs_y, 0.0f);
        BLF_draw(fontid, temp_str, sizeof(temp_str));

        ofs_x += BLF_width(fontid, temp_str, sizeof(temp_str)) + UI_UNIT_X;
      }
    }
  }
}

struct MetadataCustomCountContext {
  int count;
};

static void metadata_custom_count_fields(const char *field, const char * /*value*/, void *ctx_v)
{
  if (!metadata_is_custom_drawable(field)) {
    return;
  }
  MetadataCustomCountContext *ctx = (MetadataCustomCountContext *)ctx_v;
  ctx->count++;
}

static float metadata_box_height_get(const ImBuf *ibuf, int fontid, const bool is_top)
{
  const float height = BLF_height_max(fontid);
  const float margin = (height / 8);
  char str[MAX_METADATA_STR] = "";
  short count = 0;

  if (is_top) {
    if (metadata_is_valid(ibuf, str, 0, 0) || metadata_is_valid(ibuf, str, 1, 0)) {
      count++;
    }
    for (int i = 2; i < 5; i++) {
      if (metadata_is_valid(ibuf, str, i, 0)) {
        if (i == 4) {
          struct {
            ResultBLF info;
            rcti rect;
          } wrap;

          BLF_enable(fontid, BLF_WORD_WRAP);
          BLF_wordwrap(fontid, ibuf->x - (margin * 2));
          BLF_boundbox(fontid, str, sizeof(str), &wrap.rect, &wrap.info);
          BLF_wordwrap(fontid, 0);
          BLF_disable(fontid, BLF_WORD_WRAP);

          count += wrap.info.lines;
        }
        else {
          count++;
        }
      }
    }
  }
  else {
    for (int i = 5; i < 10; i++) {
      if (metadata_is_valid(ibuf, str, i, 0)) {
        count = 1;
        break;
      }
    }
    MetadataCustomCountContext ctx;
    ctx.count = 0;
    IMB_metadata_foreach(ibuf, metadata_custom_count_fields, &ctx);
    count += ctx.count;
  }

  if (count) {
    return (height + margin) * count;
  }

  return 0;
}

static void text_info_row(const char *text,
                          const int text_len,
                          int col1,
                          int col2,
                          int row,
                          const int size_x,
                          const int size_y)
{
  const int font_id = BLF_default();
  float text_color[4];

  UI_GetThemeColor4fv(TH_TEXT_HI, text_color);
  BLF_color4fv(font_id, text_color);

  /* Ensure text is visible against bright background. */
  const float shadow_color[4] = {0.0f, 0.0f, 0.0f, 0.8f};
  BLF_enable(font_id, BLF_SHADOW);
  BLF_shadow_offset(font_id, 0, 0);
  BLF_shadow(font_id, FontShadowType::Outline, shadow_color);

  BLF_position(font_id, col1, row, 0.0f);
  BLF_draw(font_id, IFACE_(text), text_len);
  BLF_position(font_id, col2, row, 0.0f);
  char draw_text[MAX_NAME];
  SNPRINTF_UTF8(draw_text, "%d x %d", size_x, size_y);
  BLF_draw(font_id, draw_text, sizeof(draw_text));

  BLF_disable(font_id, BLF_SHADOW);
}

void ED_region_image_overlay_info_text_draw(const int render_size_x,
                                            const int render_size_y,

                                            const int viewer_size_x,
                                            const int viewer_size_y,

                                            const int draw_offset_x,
                                            const int draw_offset_y)
{
  BLF_set_default();
  const int font_id = BLF_default();
  int overlay_lineheight = (UI_style_get()->widget.points * UI_SCALE_FAC * 1.6f);

  const char render_size_name[MAX_NAME] = "Render Size";
  const char viewer_size_name[MAX_NAME] = "Image Size";

  const int render_size_width = BLF_width(font_id, render_size_name, sizeof(render_size_name));
  const int viewer_size_width = BLF_width(font_id, viewer_size_name, sizeof(viewer_size_name));
  int longest_label = max_ii(render_size_width, viewer_size_width);

  int col1 = draw_offset_x;
  int col2 = draw_offset_x + longest_label + (0.5 * U.widget_unit);

  text_info_row(render_size_name,
                sizeof(render_size_name),
                col1,
                col2,
                draw_offset_y - overlay_lineheight,
                render_size_x,
                render_size_y);

  text_info_row(viewer_size_name,
                sizeof(viewer_size_name),
                col1,
                col2,
                draw_offset_y - overlay_lineheight * 2,
                viewer_size_x,
                viewer_size_y);
}

void ED_region_image_render_region_draw(
    int x, int y, const rcti *frame, float zoomx, float zoomy, float passepartout_alpha)
{
  GPU_matrix_push();

  /* Offset and zoom using GPU viewport. */
  const auto frame_width = BLI_rcti_size_x(frame);
  const auto frame_height = BLI_rcti_size_y(frame);
  GPU_matrix_translate_2f(x, y);
  GPU_matrix_scale_2f(zoomx, zoomy);

  GPUVertFormat *format = immVertexFormat();
  uint pos = GPU_vertformat_attr_add(format, "pos", blender::gpu::VertAttrType::SFLOAT_32_32);

  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
  GPU_blend(GPU_BLEND_ALPHA);

  const float x1 = frame->xmin - frame_width / 2;
  const float x2 = frame->xmax - frame_width / 2;
  const float y1 = frame->ymin - frame_height / 2;
  const float y2 = frame->ymax - frame_height / 2;

  /* Darken the area outside the frame. */
  if (passepartout_alpha > 0) {
    /* Using a sufficiently large number instead of numeric_limits::infinity(), to avoid comparison
     * issues and different behavior around large numbers on different platforms. */
    constexpr float inf = 10e5;
    immUniformColor4f(0.0f, 0.0f, 0.0f, passepartout_alpha);
    immRectf(pos, -inf, y2, inf, inf);
    immRectf(pos, -inf, y1, inf, -inf);
    immRectf(pos, -inf, y1, x1, y2);
    immRectf(pos, x2, y1, inf, y2);
  }

  float wire_color[3];
  UI_GetThemeColor3fv(TH_WIRE_EDIT, wire_color);
  immUniformColor4f(wire_color[0], wire_color[1], wire_color[2], 1);

  /* The bounding box must be drawn last to ensure it remains visible
   * when passepartout_alpha > 0. */
  imm_draw_box_wire_2d(pos, x1, y1, x2, y2);

  immUnbindProgram();
  GPU_blend(GPU_BLEND_NONE);

  GPU_matrix_pop();
}

void ED_region_image_metadata_draw(
    int x, int y, const ImBuf *ibuf, const rctf *frame, float zoomx, float zoomy)
{
  const uiStyle *style = UI_style_get_dpi();

  if (!ibuf->metadata) {
    return;
  }

  /* find window pixel coordinates of origin */
  GPU_matrix_push();

  /* Offset and zoom using GPU viewport. */
  GPU_matrix_translate_2f(x, y);
  GPU_matrix_scale_2f(zoomx, zoomy);

  BLF_size(blf_mono_font, style->widget.points * UI_SCALE_FAC);

  /* *** upper box*** */

  /* get needed box height */
  float box_y = metadata_box_height_get(ibuf, blf_mono_font, true);

  if (box_y) {
    /* set up rect */
    rctf rect;
    BLI_rctf_init(&rect, frame->xmin, frame->xmax, frame->ymax, frame->ymax + box_y);
    /* draw top box */
    GPUVertFormat *format = immVertexFormat();
    uint pos = GPU_vertformat_attr_add(format, "pos", blender::gpu::VertAttrType::SFLOAT_32_32);
    immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
    immUniformThemeColorAlpha(TH_METADATA_BG, 1.0f);
    immRectf(pos, rect.xmin, rect.ymin, rect.xmax, rect.ymax);
    immUnbindProgram();

    BLF_clipping(blf_mono_font, rect.xmin, rect.ymin, rect.xmax, rect.ymax);
    BLF_enable(blf_mono_font, BLF_CLIPPING);

    UI_FontThemeColor(blf_mono_font, TH_METADATA_TEXT);
    metadata_draw_imbuf(ibuf, &rect, blf_mono_font, true);

    BLF_disable(blf_mono_font, BLF_CLIPPING);
  }

  /* *** lower box*** */

  box_y = metadata_box_height_get(ibuf, blf_mono_font, false);

  if (box_y) {
    /* set up box rect */
    rctf rect;
    BLI_rctf_init(&rect, frame->xmin, frame->xmax, frame->ymin - box_y, frame->ymin);
    /* draw top box */
    GPUVertFormat *format = immVertexFormat();
    uint pos = GPU_vertformat_attr_add(format, "pos", blender::gpu::VertAttrType::SFLOAT_32_32);
    immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
    immUniformThemeColorAlpha(TH_METADATA_BG, 1.0f);
    immRectf(pos, rect.xmin, rect.ymin, rect.xmax, rect.ymax);
    immUnbindProgram();

    BLF_clipping(blf_mono_font, rect.xmin, rect.ymin, rect.xmax, rect.ymax);
    BLF_enable(blf_mono_font, BLF_CLIPPING);

    UI_FontThemeColor(blf_mono_font, TH_METADATA_TEXT);
    metadata_draw_imbuf(ibuf, &rect, blf_mono_font, false);

    BLF_disable(blf_mono_font, BLF_CLIPPING);
  }

  GPU_matrix_pop();
}

#undef MAX_METADATA_STR
