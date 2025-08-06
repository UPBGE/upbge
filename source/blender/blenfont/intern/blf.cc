/* SPDX-FileCopyrightText: 2009-2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup blf
 *
 * Main BlenFont (BLF) API, public functions for font handling.
 *
 * Wraps GPU display and FreeType.
 */

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <ft2build.h>

#include FT_FREETYPE_H
#include FT_GLYPH_H

#include "BLI_fileops.h"
#include "BLI_math_rotation.h"
#include "BLI_path_utils.hh"
#include "BLI_string.h"

#include "BLF_api.hh"

#include "IMB_colormanagement.hh"

#include "GPU_matrix.hh"
#include "GPU_state.hh"

#include "blf_internal.hh"
#include "blf_internal_types.hh"

#define BLF_RESULT_CHECK_INIT(r_info) \
  if (r_info) { \
    memset(r_info, 0, sizeof(*(r_info))); \
  } \
  ((void)0)

FontBLF *global_font[BLF_MAX_FONT] = {nullptr};

/* XXX: should these be made into global_font_'s too? */

int blf_mono_font = -1;
int blf_mono_font_render = -1;

static blender::Mutex g_blf_load_mutex;

static FontBLF *blf_get(int fontid)
{
  if (fontid >= 0 && fontid < BLF_MAX_FONT) {
    return global_font[fontid];
  }
  return nullptr;
}

int BLF_init()
{
  for (int i = 0; i < BLF_MAX_FONT; i++) {
    global_font[i] = nullptr;
  }

  return blf_font_init();
}

void BLF_exit()
{
  for (int i = 0; i < BLF_MAX_FONT; i++) {
    FontBLF *font = global_font[i];
    if (font) {
      blf_font_free(font);
      global_font[i] = nullptr;
    }
  }

  blf_font_exit();
}

void BLF_reset_fonts()
{
  const int def_font = BLF_default();
  for (int i = 0; i < BLF_MAX_FONT; i++) {
    FontBLF *font = global_font[i];
    if (font && !ELEM(i, def_font, blf_mono_font, blf_mono_font_render) &&
        !(font->flags & BLF_DEFAULT))
    {
      /* Remove fonts that are not used in the UI or part of the stack. */
      blf_font_free(font);
      global_font[i] = nullptr;
    }
  }
}

void BLF_cache_clear()
{
  for (int i = 0; i < BLF_MAX_FONT; i++) {
    FontBLF *font = global_font[i];
    if (font) {
      blf_glyph_cache_clear(font);
    }
  }
}

static int blf_search_by_mem_name(const char *mem_name)
{
  std::lock_guard lock(g_blf_load_mutex);
  for (int i = 0; i < BLF_MAX_FONT; i++) {
    const FontBLF *font = global_font[i];
    if ((font == nullptr) || (font->mem_name == nullptr)) {
      continue;
    }
    if (STREQ(font->mem_name, mem_name)) {
      return i;
    }
  }

  return -1;
}

static int blf_search_by_filepath(const char *filepath)
{
  std::lock_guard lock(g_blf_load_mutex);
  for (int i = 0; i < BLF_MAX_FONT; i++) {
    const FontBLF *font = global_font[i];
    if ((font == nullptr) || (font->filepath == nullptr)) {
      continue;
    }
    if (BLI_path_cmp(font->filepath, filepath) == 0) {
      return i;
    }
  }

  return -1;
}

static int blf_search_available()
{
  for (int i = 0; i < BLF_MAX_FONT; i++) {
    if (!global_font[i]) {
      return i;
    }
  }

  return -1;
}

bool BLF_has_glyph(int fontid, uint unicode)
{
  FontBLF *font = blf_get(fontid);
  if (font) {
    return blf_get_char_index(font, unicode) != FT_Err_Ok;
  }
  return false;
}

bool BLF_is_loaded(const char *filepath)
{
  return blf_search_by_filepath(filepath) >= 0;
}

bool BLF_is_loaded_mem(const char *name)
{
  return blf_search_by_mem_name(name) >= 0;
}

bool BLF_is_loaded_id(int fontid)
{
  return blf_get(fontid) != nullptr;
}

int BLF_load(const char *filepath)
{
  /* check if we already load this font. */
  int i = blf_search_by_filepath(filepath);
  if (i >= 0) {
    FontBLF *font = global_font[i];
    font->reference_count++;
    return i;
  }

  return BLF_load_unique(filepath);
}

int BLF_load_unique(const char *filepath)
{
  std::lock_guard lock(g_blf_load_mutex);
  int i = blf_search_available();
  if (i == -1) {
    printf("Too many fonts!!!\n");
    return -1;
  }

  /* This isn't essential, it will just cause confusing behavior to load a font
   * that appears to succeed, then doesn't show up. */
  if (!BLI_exists(filepath)) {
    printf("Can't find font: %s\n", filepath);
    return -1;
  }

  FontBLF *font = blf_font_new_from_filepath(filepath);

  if (!font) {
    printf("Can't load font: %s\n", filepath);
    return -1;
  }

  font->reference_count = 1;
  global_font[i] = font;
  return i;
}

void BLF_metrics_attach(const int fontid, const uchar *mem, const int mem_size)
{
  FontBLF *font = blf_get(fontid);

  if (font) {
    blf_font_attach_from_mem(font, mem, mem_size);
  }
}

int BLF_load_mem(const char *name, const uchar *mem, int mem_size)
{
  int i = blf_search_by_mem_name(name);
  if (i >= 0) {
    return i;
  }
  return BLF_load_mem_unique(name, mem, mem_size);
}

int BLF_load_mem_unique(const char *name, const uchar *mem, int mem_size)
{
  std::lock_guard lock(g_blf_load_mutex);
  int i = blf_search_available();
  if (i == -1) {
    printf("Too many fonts!!!\n");
    return -1;
  }

  if (!mem_size) {
    printf("Can't load font: %s from memory!!\n", name);
    return -1;
  }

  FontBLF *font = blf_font_new_from_mem(name, mem, mem_size);
  if (!font) {
    printf("Can't load font: %s from memory!!\n", name);
    return -1;
  }

  font->reference_count = 1;
  global_font[i] = font;
  return i;
}

void BLF_unload(const char *filepath)
{
  std::lock_guard lock(g_blf_load_mutex);
  for (int i = 0; i < BLF_MAX_FONT; i++) {
    FontBLF *font = global_font[i];
    if (font == nullptr || font->filepath == nullptr) {
      continue;
    }

    if (BLI_path_cmp(font->filepath, filepath) == 0) {
      BLI_assert(font->reference_count > 0);
      font->reference_count--;

      if (font->reference_count == 0) {
        blf_font_free(font);
        global_font[i] = nullptr;
      }
    }
  }
}

bool BLF_unload_id(int fontid)
{
  std::lock_guard lock(g_blf_load_mutex);
  FontBLF *font = blf_get(fontid);
  if (font) {
    BLI_assert(font->reference_count > 0);
    font->reference_count--;

    if (font->reference_count == 0) {
      blf_font_free(font);
      global_font[fontid] = nullptr;
      return true;
    }
  }
  return false;
}

void BLF_unload_all()
{
  for (int i = 0; i < BLF_MAX_FONT; i++) {
    FontBLF *font = global_font[i];
    if (font) {
      blf_font_free(font);
      global_font[i] = nullptr;
    }
  }
  blf_mono_font = -1;
  blf_mono_font_render = -1;
  BLF_default_set(-1);
}

void BLF_addref_id(int fontid)
{
  std::lock_guard lock(g_blf_load_mutex);
  FontBLF *font = blf_get(fontid);
  if (font) {
    font->reference_count++;
  }
}

void BLF_enable(int fontid, FontFlags flag)
{
  FontBLF *font = blf_get(fontid);

  if (font) {
    font->flags |= flag;
  }
}

void BLF_disable(int fontid, FontFlags flag)
{
  FontBLF *font = blf_get(fontid);

  if (font) {
    font->flags &= ~flag;
  }
}

bool BLF_is_builtin(int fontid)
{
  FontBLF *font = blf_get(fontid);
  if (font) {
    return font->flags & BLF_DEFAULT;
  }
  return false;
}

void BLF_character_weight(int fontid, int weight)
{
  FontBLF *font = blf_get(fontid);
  if (font) {
    font->char_weight = weight;
  }
}

int BLF_default_weight(int fontid)
{
  FontBLF *font = blf_get(fontid);
  if (font) {
    return font->metrics.weight;
  }
  return 400;
}

bool BLF_has_variable_weight(int fontid)
{
  const FontBLF *font = blf_get(fontid);
  if (font && font->variations) {
    for (int i = 0; i < int(font->variations->num_axis); i++) {
      if (font->variations->axis[i].tag == BLF_VARIATION_AXIS_WEIGHT) {
        return true;
      }
    }
  }
  return false;
}

void BLF_aspect(int fontid, float x, float y, float z)
{
  FontBLF *font = blf_get(fontid);

  if (font) {
    font->aspect[0] = x;
    font->aspect[1] = y;
    font->aspect[2] = z;
  }
}

void BLF_position(int fontid, float x, float y, float z)
{
  FontBLF *font = blf_get(fontid);

  if (font) {
    float xa, ya, za;
    float remainder;

    if (font->flags & BLF_ASPECT) {
      xa = font->aspect[0];
      ya = font->aspect[1];
      za = font->aspect[2];
    }
    else {
      xa = 1.0f;
      ya = 1.0f;
      za = 1.0f;
    }

    remainder = x - floorf(x);
    if (remainder > 0.4f && remainder < 0.6f) {
      if (remainder < 0.5f) {
        x -= 0.1f * xa;
      }
      else {
        x += 0.1f * xa;
      }
    }

    remainder = y - floorf(y);
    if (remainder > 0.4f && remainder < 0.6f) {
      if (remainder < 0.5f) {
        y -= 0.1f * ya;
      }
      else {
        y += 0.1f * ya;
      }
    }

    remainder = z - floorf(z);
    if (remainder > 0.4f && remainder < 0.6f) {
      if (remainder < 0.5f) {
        z -= 0.1f * za;
      }
      else {
        z += 0.1f * za;
      }
    }

    font->pos[0] = round_fl_to_int(x);
    font->pos[1] = round_fl_to_int(y);
    font->pos[2] = round_fl_to_int(z);
  }
}

void BLF_size(int fontid, float size)
{
  FontBLF *font = blf_get(fontid);

  if (font) {
    blf_font_size(font, size);
  }
}

void BLF_color4ubv(int fontid, const uchar rgba[4])
{
  FontBLF *font = blf_get(fontid);

  if (font) {
    font->color[0] = rgba[0];
    font->color[1] = rgba[1];
    font->color[2] = rgba[2];
    font->color[3] = rgba[3];
  }
}

void BLF_color3ubv_alpha(int fontid, const uchar rgb[3], uchar alpha)
{
  FontBLF *font = blf_get(fontid);

  if (font) {
    font->color[0] = rgb[0];
    font->color[1] = rgb[1];
    font->color[2] = rgb[2];
    font->color[3] = alpha;
  }
}

void BLF_color3ubv(int fontid, const uchar rgb[3])
{
  BLF_color3ubv_alpha(fontid, rgb, 255);
}

void BLF_color4ub(int fontid, uchar r, uchar g, uchar b, uchar alpha)
{
  FontBLF *font = blf_get(fontid);

  if (font) {
    font->color[0] = r;
    font->color[1] = g;
    font->color[2] = b;
    font->color[3] = alpha;
  }
}

void BLF_color3ub(int fontid, uchar r, uchar g, uchar b)
{
  FontBLF *font = blf_get(fontid);

  if (font) {
    font->color[0] = r;
    font->color[1] = g;
    font->color[2] = b;
    font->color[3] = 255;
  }
}

void BLF_color4fv(int fontid, const float rgba[4])
{
  FontBLF *font = blf_get(fontid);

  if (font) {
    rgba_float_to_uchar(font->color, rgba);
  }
}

void BLF_color4f(int fontid, float r, float g, float b, float a)
{
  const float rgba[4] = {r, g, b, a};
  BLF_color4fv(fontid, rgba);
}

void BLF_color3fv_alpha(int fontid, const float rgb[3], float alpha)
{
  float rgba[4];
  copy_v3_v3(rgba, rgb);
  rgba[3] = alpha;
  BLF_color4fv(fontid, rgba);
}

void BLF_color3f(int fontid, float r, float g, float b)
{
  const float rgba[4] = {r, g, b, 1.0f};
  BLF_color4fv(fontid, rgba);
}

void BLF_batch_draw_begin()
{
  BLI_assert(g_batch.enabled == false);
  g_batch.enabled = true;
}

void BLF_batch_draw_flush()
{
  if (g_batch.enabled) {
    blf_batch_draw();
  }
}

void BLF_batch_draw_end()
{
  BLI_assert(g_batch.enabled == true);
  blf_batch_draw(); /* Draw remaining glyphs */
  g_batch.enabled = false;
}

static void blf_draw_gpu__start(const FontBLF *font)
{
  /*
   * The pixmap alignment hack is handle
   * in BLF_position (old ui_rasterpos_safe).
   */

  if ((font->flags & (BLF_ROTATION | BLF_ASPECT)) == 0) {
    return; /* glyphs will be translated individually and batched. */
  }

  GPU_matrix_push();

  GPU_matrix_translate_3f(font->pos[0], font->pos[1], font->pos[2]);

  if (font->flags & BLF_ASPECT) {
    GPU_matrix_scale_3fv(font->aspect);
  }

  if (font->flags & BLF_ROTATION) {
    GPU_matrix_rotate_2d(RAD2DEG(font->angle));
  }
}

static void blf_draw_gpu__end(const FontBLF *font)
{
  if ((font->flags & (BLF_ROTATION | BLF_ASPECT)) != 0) {
    GPU_matrix_pop();
  }
}

void BLF_draw(int fontid, const char *str, const size_t str_len, ResultBLF *r_info)
{
  BLF_RESULT_CHECK_INIT(r_info);

  if (str_len == 0 || str[0] == '\0') {
    return;
  }

  FontBLF *font = blf_get(fontid);

  if (font) {
    blf_draw_gpu__start(font);
    if (font->flags & BLF_WORD_WRAP) {
      blf_font_draw__wrap(font, str, str_len, r_info);
    }
    else {
      blf_font_draw(font, str, str_len, r_info);
    }
    blf_draw_gpu__end(font);
  }
}

int BLF_draw_mono(int fontid, const char *str, const size_t str_len, int cwidth, int tab_columns)
{
  if (str_len == 0 || str[0] == '\0') {
    return 0;
  }

  FontBLF *font = blf_get(fontid);
  int columns = 0;

  if (font) {
    blf_draw_gpu__start(font);
    columns = blf_font_draw_mono(font, str, str_len, cwidth, tab_columns);
    blf_draw_gpu__end(font);
  }

  return columns;
}

void BLF_draw_svg_icon(uint icon_id,
                       float x,
                       float y,
                       float size,
                       const float color[4],
                       float outline_alpha,
                       bool multicolor,
                       blender::FunctionRef<void(std::string &)> edit_source_cb)
{
#ifndef WITH_HEADLESS
  FontBLF *font = global_font[0];
  if (font) {
    blf_draw_gpu__start(font);
    blf_draw_svg_icon(font, icon_id, x, y, size, color, outline_alpha, multicolor, edit_source_cb);
    blf_draw_gpu__end(font);
  }
#else
  UNUSED_VARS(icon_id, x, y, size, color, outline_alpha, multicolor, edit_source_cb);
#endif /* WITH_HEADLESS */
}

blender::Array<uchar> BLF_svg_icon_bitmap(uint icon_id,
                                          float size,
                                          int *r_width,
                                          int *r_height,
                                          bool multicolor,
                                          blender::FunctionRef<void(std::string &)> edit_source_cb)
{
#ifndef WITH_HEADLESS
  FontBLF *font = global_font[0];
  if (font) {
    return blf_svg_icon_bitmap(font, icon_id, size, r_width, r_height, multicolor, edit_source_cb);
  }
#else
  UNUSED_VARS(icon_id, size, r_width, r_height, multicolor, edit_source_cb);
#endif /* WITH_HEADLESS */
  return {};
}

void BLF_boundbox_foreach_glyph(
    int fontid, const char *str, size_t str_len, BLF_GlyphBoundsFn user_fn, void *user_data)
{
  FontBLF *font = blf_get(fontid);

  if (font) {
    if (font->flags & BLF_WORD_WRAP) {
      /* TODO: word-wrap support. */
      BLI_assert(0);
    }
    else {
      blf_font_boundbox_foreach_glyph(font, str, str_len, user_fn, user_data);
    }
  }
}

size_t BLF_str_offset_from_cursor_position(int fontid,
                                           const char *str,
                                           size_t str_len,
                                           int location_x)
{
  FontBLF *font = blf_get(fontid);
  if (font) {
    return blf_str_offset_from_cursor_position(font, str, str_len, location_x);
  }
  return 0;
}

bool BLF_str_offset_to_glyph_bounds(int fontid,
                                    const char *str,
                                    size_t str_offset,
                                    rcti *r_glyph_bounds)
{
  FontBLF *font = blf_get(fontid);
  if (font) {
    blf_str_offset_to_glyph_bounds(font, str, str_offset, r_glyph_bounds);
    return true;
  }
  return false;
}

int BLF_str_offset_to_cursor(int fontid,
                             const char *str,
                             const size_t str_len,
                             const size_t str_offset,
                             const int cursor_width)
{
  FontBLF *font = blf_get(fontid);
  if (font) {
    return blf_str_offset_to_cursor(font, str, str_len, str_offset, cursor_width);
  }
  return 0;
}

blender::Vector<blender::Bounds<int>> BLF_str_selection_boxes(
    int fontid, const char *str, size_t str_len, size_t sel_start, size_t sel_length)
{
  FontBLF *font = blf_get(fontid);
  if (font) {
    return blf_str_selection_boxes(font, str, str_len, sel_start, sel_length);
  }
  return {};
}

size_t BLF_width_to_strlen(
    int fontid, const char *str, const size_t str_len, float width, float *r_width)
{
  FontBLF *font = blf_get(fontid);

  if (font) {
    const float xa = (font->flags & BLF_ASPECT) ? font->aspect[0] : 1.0f;
    size_t ret;
    int width_result;
    ret = blf_font_width_to_strlen(font, str, str_len, width / xa, &width_result);
    if (r_width) {
      *r_width = float(width_result) * xa;
    }
    return ret;
  }

  if (r_width) {
    *r_width = 0.0f;
  }
  return 0;
}

size_t BLF_width_to_rstrlen(
    int fontid, const char *str, const size_t str_len, float width, float *r_width)
{
  FontBLF *font = blf_get(fontid);

  if (font) {
    const float xa = (font->flags & BLF_ASPECT) ? font->aspect[0] : 1.0f;
    size_t ret;
    int width_result;
    ret = blf_font_width_to_rstrlen(font, str, str_len, width / xa, &width_result);
    if (r_width) {
      *r_width = float(width_result) * xa;
    }
    return ret;
  }

  if (r_width) {
    *r_width = 0.0f;
  }
  return 0;
}

void BLF_boundbox(
    int fontid, const char *str, const size_t str_len, rcti *r_box, ResultBLF *r_info)
{
  FontBLF *font = blf_get(fontid);

  BLF_RESULT_CHECK_INIT(r_info);

  if (font) {
    if (font->flags & BLF_WORD_WRAP) {
      blf_font_boundbox__wrap(font, str, str_len, r_box, r_info);
    }
    else {
      blf_font_boundbox(font, str, str_len, r_box, r_info);
    }
  }
}

void BLF_width_and_height(
    int fontid, const char *str, const size_t str_len, float *r_width, float *r_height)
{
  FontBLF *font = blf_get(fontid);

  if (font) {
    blf_font_width_and_height(font, str, str_len, r_width, r_height, nullptr);
  }
  else {
    *r_width = *r_height = 0.0f;
  }
}

float BLF_width(int fontid, const char *str, const size_t str_len, ResultBLF *r_info)
{
  FontBLF *font = blf_get(fontid);

  BLF_RESULT_CHECK_INIT(r_info);

  if (font) {
    return blf_font_width(font, str, str_len, r_info);
  }

  return 0.0f;
}

float BLF_fixed_width(int fontid)
{
  FontBLF *font = blf_get(fontid);

  if (font) {
    return blf_font_fixed_width(font);
  }

  return 0.0f;
}

int BLF_glyph_advance(int fontid, const char *str)
{
  FontBLF *font = blf_get(fontid);

  if (font) {
    return blf_font_glyph_advance(font, str);
  }

  return 0;
}

float BLF_height(int fontid, const char *str, const size_t str_len, ResultBLF *r_info)
{
  FontBLF *font = blf_get(fontid);

  BLF_RESULT_CHECK_INIT(r_info);

  if (font) {
    return blf_font_height(font, str, str_len, r_info);
  }

  return 0.0f;
}

int BLF_height_max(int fontid)
{
  FontBLF *font = blf_get(fontid);

  if (font) {
    return blf_font_height_max(font);
  }

  return 0;
}

int BLF_width_max(int fontid)
{
  FontBLF *font = blf_get(fontid);

  if (font) {
    return blf_font_width_max(font);
  }

  return 0;
}

int BLF_descender(int fontid)
{
  FontBLF *font = blf_get(fontid);

  if (font) {
    return blf_font_descender(font);
  }

  return 0;
}

int BLF_ascender(int fontid)
{
  FontBLF *font = blf_get(fontid);

  if (font) {
    return blf_font_ascender(font);
  }

  return 0.0f;
}

void BLF_rotation(int fontid, float angle)
{
  FontBLF *font = blf_get(fontid);

  if (font) {
    font->angle = angle;
  }
}

void BLF_clipping(int fontid, int xmin, int ymin, int xmax, int ymax)
{
  FontBLF *font = blf_get(fontid);

  if (font) {
    font->clip_rec.xmin = xmin;
    font->clip_rec.ymin = ymin;
    font->clip_rec.xmax = xmax;
    font->clip_rec.ymax = ymax;
  }
}

void BLF_wordwrap(int fontid, int wrap_width, BLFWrapMode mode)
{
  FontBLF *font = blf_get(fontid);

  if (font) {
    font->wrap_width = wrap_width;
    font->wrap_mode = mode;
  }
}

void BLF_shadow(int fontid, FontShadowType type, const float rgba[4])
{
  FontBLF *font = blf_get(fontid);

  if (font) {
    font->shadow = type;
    if (rgba) {
      rgba_float_to_uchar(font->shadow_color, rgba);
    }
  }
}

void BLF_shadow_offset(int fontid, int x, int y)
{
  FontBLF *font = blf_get(fontid);

  if (font) {
    font->shadow_x = x;
    font->shadow_y = y;
  }
}

void BLF_buffer(
    int fontid, float *fbuf, uchar *cbuf, int w, int h, const ColorManagedDisplay *display)
{
  FontBLF *font = blf_get(fontid);

  if (font) {
    font->buf_info.fbuf = fbuf;
    font->buf_info.cbuf = cbuf;
    font->buf_info.dims[0] = w;
    font->buf_info.dims[1] = h;
    font->buf_info.display = display;
  }
}

struct BLFBufferState {
  int fontid;
  /**
   * This only exists to validate the font has not been freed since the state was created.
   * Needed because the state can be used from Python.
   */
  const FontBLF *font;

  FontBufInfoBLF buf_info;
};

BLFBufferState *BLF_buffer_state_push(int fontid)
{
  FontBLF *font = blf_get(fontid);
  if (font) {
    BLFBufferState *buffer_state = MEM_new<BLFBufferState>(__func__);
    buffer_state->fontid = fontid;
    buffer_state->font = font;
    buffer_state->buf_info = font->buf_info;
    return buffer_state;
  }
  return nullptr;
}

void BLF_buffer_state_pop(BLFBufferState *buffer_state)
{
  FontBLF *font = blf_get(buffer_state->fontid);
  /* It's possible the font has been removed as this is called from Python. */
  if (font == buffer_state->font) {
    /* From the callers perspective, don't consider the color part of the buffer info.
     *
     * NOTE(@ideasman42) This is done because the color is not logically part of the image binding.
     * It looks like we can refactor color out of #FontBufInfoBLF::col_init,
     * and use #FontBLF::color instead. */
    copy_v4_v4(buffer_state->buf_info.col_init, font->buf_info.col_init);

    font->buf_info = buffer_state->buf_info;
  }
  BLF_buffer_state_free(buffer_state);
}

void BLF_buffer_state_free(BLFBufferState *buffer_state)
{
  MEM_delete(buffer_state);
}

void BLF_buffer_col(int fontid, const float rgba[4])
{
  FontBLF *font = blf_get(fontid);

  if (font) {
    copy_v4_v4(font->buf_info.col_init, rgba);
  }
}

void blf_draw_buffer__start(FontBLF *font)
{
  FontBufInfoBLF *buf_info = &font->buf_info;

  rgba_float_to_uchar(buf_info->col_char, buf_info->col_init);

  if (buf_info->display) {
    copy_v4_v4(buf_info->col_float, buf_info->col_init);
    IMB_colormanagement_display_to_scene_linear_v3(buf_info->col_float, buf_info->display);
  }
  else {
    srgb_to_linearrgb_v4(buf_info->col_float, buf_info->col_init);
  }
}
void blf_draw_buffer__end() {}

void BLF_draw_buffer(int fontid, const char *str, const size_t str_len, ResultBLF *r_info)
{
  FontBLF *font = blf_get(fontid);

  if (font && (font->buf_info.fbuf || font->buf_info.cbuf)) {
    blf_draw_buffer__start(font);
    if (font->flags & BLF_WORD_WRAP) {
      blf_font_draw_buffer__wrap(font, str, str_len, r_info);
    }
    else {
      blf_font_draw_buffer(font, str, str_len, r_info);
    }
    blf_draw_buffer__end();
  }
}

blender::Vector<blender::StringRef> BLF_string_wrap(int fontid,
                                                    blender::StringRef str,
                                                    const int max_pixel_width,
                                                    BLFWrapMode mode)
{
  FontBLF *font = blf_get(fontid);
  if (!font) {
    return {};
  }
  return blf_font_string_wrap(font, str, max_pixel_width, mode);
}

char *BLF_display_name_from_file(const char *filepath)
{
  /* While listing font directories this function can be called simultaneously from a greater
   * number of threads than we want the FreeType cache to keep open at a time. Therefore open
   * with a separate FT_Library object and use FreeType calls directly to avoid any contention. */
  char *name = nullptr;
  FT_Library ft_library;
  if (FT_Init_FreeType(&ft_library) == FT_Err_Ok) {
    FT_Face face;
    if (FT_New_Face(ft_library, filepath, 0, &face) == FT_Err_Ok) {
      if (face->family_name) {
        name = BLI_sprintfN("%s %s", face->family_name, face->style_name);
      }
      FT_Done_Face(face);
    }
    FT_Done_FreeType(ft_library);
  }
  return name;
}

char *BLF_display_name_from_id(int fontid)
{
  FontBLF *font = blf_get(fontid);
  if (!font) {
    return nullptr;
  }

  return blf_display_name(font);
}

bool BLF_get_vfont_metrics(int fontid, float *ascend_ratio, float *em_ratio, float *scale)
{
  FontBLF *font = blf_get(fontid);
  if (!font) {
    return false;
  }

  if (!blf_ensure_face(font)) {
    return false;
  }

  /* Copied without change from vfontdata_freetype.cc to ensure consistent sizing. */

  /* Blender default BFont is not "complete". */
  const bool complete_font = (font->face->ascender != 0) && (font->face->descender != 0) &&
                             (font->face->ascender != font->face->descender);

  if (complete_font) {
    /* We can get descender as well, but we simple store descender in relation to the ascender.
     * Also note that descender is stored as a negative number. */
    *ascend_ratio = float(font->face->ascender) / (font->face->ascender - font->face->descender);
  }
  else {
    *ascend_ratio = BLF_VFONT_METRICS_ASCEND_RATIO_DEFAULT;
    *em_ratio = BLF_VFONT_METRICS_EM_RATIO_DEFAULT;
  }

  /* Adjust font size */
  if (font->face->bbox.yMax != font->face->bbox.yMin) {
    *scale = float(1.0 / double(font->face->bbox.yMax - font->face->bbox.yMin));

    if (complete_font) {
      *em_ratio = float(font->face->ascender - font->face->descender) /
                  (font->face->bbox.yMax - font->face->bbox.yMin);
    }
  }
  else {
    *scale = BLF_VFONT_METRICS_SCALE_DEFAULT;
  }

  return true;
}

bool BLF_character_to_curves(int fontid,
                             uint unicode,
                             ListBase *nurbsbase,
                             const float scale,
                             bool use_fallback,
                             float *r_advance)
{
  FontBLF *font = blf_get(fontid);
  if (!font) {
    return false;
  }
  return blf_character_to_curves(font, unicode, nurbsbase, scale, use_fallback, r_advance);
}

#ifndef NDEBUG
void BLF_state_print(int fontid)
{
  FontBLF *font = blf_get(fontid);
  if (font) {
    printf("fontid %d %p\n", fontid, (void *)font);
    printf("  mem_name:    '%s'\n", font->mem_name ? font->mem_name : "<none>");
    printf("  filepath:    '%s'\n", font->filepath ? font->filepath : "<none>");
    printf("  size:     %f\n", font->size);
    printf("  pos:      %d %d %d\n", UNPACK3(font->pos));
    printf("  aspect:   (%d) %.6f %.6f %.6f\n",
           (font->flags & BLF_ROTATION) != 0,
           UNPACK3(font->aspect));
    printf("  angle:    (%d) %.6f\n", (font->flags & BLF_ASPECT) != 0, font->angle);
    printf("  flag:     %d\n", font->flags);
  }
  else {
    printf("fontid %d (nullptr)\n", fontid);
  }
  fflush(stdout);
}
#endif
