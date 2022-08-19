/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2009 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup blf
 *
 * Deals with drawing text to OpenGL or bitmap buffers.
 *
 * Also low level functions for managing \a FontBLF.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ft2build.h>

#include FT_FREETYPE_H
#include FT_CACHE_H /* FreeType Cache. */
#include FT_GLYPH_H
#include FT_MULTIPLE_MASTERS_H /* Variable font support. */
#include FT_TRUETYPE_IDS_H     /* Codepoint coverage constants. */
#include FT_TRUETYPE_TABLES_H  /* For TT_OS2 */

#include "MEM_guardedalloc.h"

#include "DNA_vec_types.h"

#include "BLI_listbase.h"
#include "BLI_math.h"
#include "BLI_math_color_blend.h"
#include "BLI_path_util.h"
#include "BLI_rect.h"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_threads.h"

#include "BLF_api.h"

#include "GPU_batch.h"
#include "GPU_matrix.h"

#include "blf_internal.h"
#include "blf_internal_types.h"

#include "BLI_strict_flags.h"

#ifdef WIN32
#  define FT_New_Face FT_New_Face__win32_compat
#endif

/* Batching buffer for drawing. */

BatchBLF g_batch;

/* freetype2 handle ONLY for this file! */
static FT_Library ft_lib = NULL;
static FTC_Manager ftc_manager = NULL;
static FTC_CMapCache ftc_charmap_cache = NULL;

/* Lock for FreeType library, used around face creation and deletion.  */
static ThreadMutex ft_lib_mutex;

/* May be set to #UI_widgetbase_draw_cache_flush. */
static void (*blf_draw_cache_flush)(void) = NULL;

static ft_pix blf_font_height_max_ft_pix(struct FontBLF *font);
static ft_pix blf_font_width_max_ft_pix(struct FontBLF *font);

/* -------------------------------------------------------------------- */

/** \name FreeType Caching
 * \{ */

/**
 * Called when a face is removed by the cache. FreeType will call #FT_Done_Face.
 */
static void blf_face_finalizer(void *object)
{
  FT_Face face = object;
  FontBLF *font = (FontBLF *)face->generic.data;
  font->face = NULL;
}

/**
 * Called in response to #FTC_Manager_LookupFace. Now add a face to our font.
 *
 * \note Unused arguments are kept to match #FTC_Face_Requester function signature.
 */
static FT_Error blf_cache_face_requester(FTC_FaceID faceID,
                                         FT_Library lib,
                                         FT_Pointer UNUSED(reqData),
                                         FT_Face *face)
{
  FontBLF *font = (FontBLF *)faceID;
  int err = FT_Err_Cannot_Open_Resource;

  BLI_mutex_lock(&ft_lib_mutex);
  if (font->filepath) {
    err = FT_New_Face(lib, font->filepath, 0, face);
  }
  else if (font->mem) {
    err = FT_New_Memory_Face(lib, font->mem, (FT_Long)font->mem_size, 0, face);
  }
  BLI_mutex_unlock(&ft_lib_mutex);

  if (err == FT_Err_Ok) {
    font->face = *face;
    font->face->generic.data = font;
    font->face->generic.finalizer = blf_face_finalizer;
  }

  return err;
}

/**
 * Called when the FreeType cache is removing a font size.
 */
static void blf_size_finalizer(void *object)
{
  FT_Size size = object;
  FontBLF *font = (FontBLF *)size->generic.data;
  font->ft_size = NULL;
}

/* -------------------------------------------------------------------- */
/** \name FreeType Utilities (Internal)
 * \{ */

/* Return glyph id from charcode. */
uint blf_get_char_index(struct FontBLF *font, uint charcode)
{
  if (font->flags & BLF_CACHED) {
    /* Use charmap cache for much faster lookup. */
    return FTC_CMapCache_Lookup(ftc_charmap_cache, font, -1, charcode);
  }
  /* Fonts that are not cached need to use the regular lookup function. */
  return blf_ensure_face(font) ? FT_Get_Char_Index(font->face, charcode) : 0;
}

/* Convert a FreeType 26.6 value representing an unscaled design size to fractional pixels. */
static ft_pix blf_unscaled_F26Dot6_to_pixels(FontBLF *font, FT_Pos value)
{
  /* Make sure we have a valid font->ft_size. */
  blf_ensure_size(font);

  /* Scale value by font size using integer-optimized multiplication. */
  FT_Long scaled = FT_MulFix(value, font->ft_size->metrics.x_scale);

  /* Copied from FreeType's FT_Get_Kerning (with FT_KERNING_DEFAULT), scaling down */
  /* kerning distances at small ppem values so that they don't become too big. */
  if (font->ft_size->metrics.x_ppem < 25) {
    scaled = FT_MulDiv(scaled, font->ft_size->metrics.x_ppem, 25);
  }

  return (ft_pix)scaled;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Glyph Batching
 * \{ */

/**
 * Draw-calls are precious! make them count!
 * Since most of the Text elements are not covered by other UI elements, we can
 * group some strings together and render them in one draw-call. This behavior
 * is on demand only, between #BLF_batch_draw_begin() and #BLF_batch_draw_end().
 */
static void blf_batch_draw_init(void)
{
  GPUVertFormat format = {0};
  g_batch.pos_loc = GPU_vertformat_attr_add(&format, "pos", GPU_COMP_F32, 4, GPU_FETCH_FLOAT);
  g_batch.col_loc = GPU_vertformat_attr_add(
      &format, "col", GPU_COMP_U8, 4, GPU_FETCH_INT_TO_FLOAT_UNIT);
  g_batch.offset_loc = GPU_vertformat_attr_add(&format, "offset", GPU_COMP_I32, 1, GPU_FETCH_INT);
  g_batch.glyph_size_loc = GPU_vertformat_attr_add(
      &format, "glyph_size", GPU_COMP_I32, 2, GPU_FETCH_INT);

  g_batch.verts = GPU_vertbuf_create_with_format_ex(&format, GPU_USAGE_STREAM);
  GPU_vertbuf_data_alloc(g_batch.verts, BLF_BATCH_DRAW_LEN_MAX);

  GPU_vertbuf_attr_get_raw_data(g_batch.verts, g_batch.pos_loc, &g_batch.pos_step);
  GPU_vertbuf_attr_get_raw_data(g_batch.verts, g_batch.col_loc, &g_batch.col_step);
  GPU_vertbuf_attr_get_raw_data(g_batch.verts, g_batch.offset_loc, &g_batch.offset_step);
  GPU_vertbuf_attr_get_raw_data(g_batch.verts, g_batch.glyph_size_loc, &g_batch.glyph_size_step);
  g_batch.glyph_len = 0;

  /* A dummy VBO containing 4 points, attributes are not used. */
  GPUVertBuf *vbo = GPU_vertbuf_create_with_format(&format);
  GPU_vertbuf_data_alloc(vbo, 4);

  /* We render a quad as a triangle strip and instance it for each glyph. */
  g_batch.batch = GPU_batch_create_ex(GPU_PRIM_TRI_STRIP, vbo, NULL, GPU_BATCH_OWNS_VBO);
  GPU_batch_instbuf_set(g_batch.batch, g_batch.verts, true);
}

static void blf_batch_draw_exit(void)
{
  GPU_BATCH_DISCARD_SAFE(g_batch.batch);
}

void blf_batch_draw_begin(FontBLF *font)
{
  if (g_batch.batch == NULL) {
    blf_batch_draw_init();
  }

  const bool font_changed = (g_batch.font != font);
  const bool simple_shader = ((font->flags & (BLF_ROTATION | BLF_MATRIX | BLF_ASPECT)) == 0);
  const bool shader_changed = (simple_shader != g_batch.simple_shader);

  g_batch.active = g_batch.enabled && simple_shader;

  if (simple_shader) {
    /* Offset is applied to each glyph. */
    g_batch.ofs[0] = font->pos[0];
    g_batch.ofs[1] = font->pos[1];
  }
  else {
    /* Offset is baked in modelview mat. */
    zero_v2_int(g_batch.ofs);
  }

  if (g_batch.active) {
    float gpumat[4][4];
    GPU_matrix_model_view_get(gpumat);

    bool mat_changed = (memcmp(gpumat, g_batch.mat, sizeof(g_batch.mat)) != 0);

    if (mat_changed) {
      /* Modelviewmat is no longer the same.
       * Flush cache but with the previous mat. */
      GPU_matrix_push();
      GPU_matrix_set(g_batch.mat);
    }

    /* flush cache if config is not the same. */
    if (mat_changed || font_changed || shader_changed) {
      blf_batch_draw();
      g_batch.simple_shader = simple_shader;
      g_batch.font = font;
    }
    else {
      /* Nothing changed continue batching. */
      return;
    }

    if (mat_changed) {
      GPU_matrix_pop();
      /* Save for next memcmp. */
      memcpy(g_batch.mat, gpumat, sizeof(g_batch.mat));
    }
  }
  else {
    /* flush cache */
    blf_batch_draw();
    g_batch.font = font;
    g_batch.simple_shader = simple_shader;
  }
}

static GPUTexture *blf_batch_cache_texture_load(void)
{
  GlyphCacheBLF *gc = g_batch.glyph_cache;
  BLI_assert(gc);
  BLI_assert(gc->bitmap_len > 0);

  if (gc->bitmap_len > gc->bitmap_len_landed) {
    const int tex_width = GPU_texture_width(gc->texture);

    int bitmap_len_landed = gc->bitmap_len_landed;
    int remain = gc->bitmap_len - bitmap_len_landed;
    int offset_x = bitmap_len_landed % tex_width;
    int offset_y = bitmap_len_landed / tex_width;

    /* TODO(germano): Update more than one row in a single call. */
    while (remain) {
      int remain_row = tex_width - offset_x;
      int width = remain > remain_row ? remain_row : remain;
      GPU_texture_update_sub(gc->texture,
                             GPU_DATA_UBYTE,
                             &gc->bitmap_result[bitmap_len_landed],
                             offset_x,
                             offset_y,
                             0,
                             width,
                             1,
                             0);

      bitmap_len_landed += width;
      remain -= width;
      offset_x = 0;
      offset_y += 1;
    }

    gc->bitmap_len_landed = bitmap_len_landed;
  }

  return gc->texture;
}

void blf_batch_draw(void)
{
  if (g_batch.glyph_len == 0) {
    return;
  }

  GPU_blend(GPU_BLEND_ALPHA);

  /* We need to flush widget base first to ensure correct ordering. */
  if (blf_draw_cache_flush != NULL) {
    blf_draw_cache_flush();
  }

  GPUTexture *texture = blf_batch_cache_texture_load();
  GPU_vertbuf_data_len_set(g_batch.verts, g_batch.glyph_len);
  GPU_vertbuf_use(g_batch.verts); /* send data */

  GPU_batch_program_set_builtin(g_batch.batch, GPU_SHADER_TEXT);
  GPU_batch_texture_bind(g_batch.batch, "glyph", texture);
  GPU_batch_draw(g_batch.batch);

  GPU_blend(GPU_BLEND_NONE);

  GPU_texture_unbind(texture);

  /* restart to 1st vertex data pointers */
  GPU_vertbuf_attr_get_raw_data(g_batch.verts, g_batch.pos_loc, &g_batch.pos_step);
  GPU_vertbuf_attr_get_raw_data(g_batch.verts, g_batch.col_loc, &g_batch.col_step);
  GPU_vertbuf_attr_get_raw_data(g_batch.verts, g_batch.offset_loc, &g_batch.offset_step);
  GPU_vertbuf_attr_get_raw_data(g_batch.verts, g_batch.glyph_size_loc, &g_batch.glyph_size_step);
  g_batch.glyph_len = 0;
}

static void blf_batch_draw_end(void)
{
  if (!g_batch.active) {
    blf_batch_draw();
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Glyph Stepping Utilities (Internal)
 * \{ */

/* Fast path for runs of ASCII characters. Given that common UTF-8
 * input will consist of an overwhelming majority of ASCII
 * characters.
 */

BLI_INLINE GlyphBLF *blf_glyph_from_utf8_and_step(
    FontBLF *font, GlyphCacheBLF *gc, const char *str, size_t str_len, size_t *i_p)
{
  uint charcode = BLI_str_utf8_as_unicode_step(str, str_len, i_p);
  /* Invalid unicode sequences return the byte value, stepping forward one.
   * This allows `latin1` to display (which is sometimes used for file-paths). */
  BLI_assert(charcode != BLI_UTF8_ERR);
  return blf_glyph_ensure(font, gc, charcode);
}

BLI_INLINE ft_pix blf_kerning(FontBLF *font, const GlyphBLF *g_prev, const GlyphBLF *g)
{
  ft_pix adjustment = 0;

  /* Small adjust if there is hinting. */
  adjustment += g->lsb_delta - ((g_prev) ? g_prev->rsb_delta : 0);

  if (FT_HAS_KERNING(font) && g_prev) {
    FT_Vector delta = {KERNING_ENTRY_UNSET};

    /* Get unscaled kerning value from our cache if ASCII. */
    if ((g_prev->c < KERNING_CACHE_TABLE_SIZE) && (g->c < GLYPH_ASCII_TABLE_SIZE)) {
      delta.x = font->kerning_cache->ascii_table[g->c][g_prev->c];
    }

    /* If not ASCII or not found in cache, ask FreeType for kerning. */
    if (UNLIKELY(font->face && delta.x == KERNING_ENTRY_UNSET)) {
      /* Note that this function sets delta values to zero on any error. */
      FT_Get_Kerning(font->face, g_prev->idx, g->idx, FT_KERNING_UNSCALED, &delta);
    }

    /* If ASCII we save this value to our cache for quicker access next time. */
    if ((g_prev->c < KERNING_CACHE_TABLE_SIZE) && (g->c < GLYPH_ASCII_TABLE_SIZE)) {
      font->kerning_cache->ascii_table[g->c][g_prev->c] = (int)delta.x;
    }

    if (delta.x != 0) {
      /* Convert unscaled design units to pixels and move pen. */
      adjustment += blf_unscaled_F26Dot6_to_pixels(font, delta.x);
    }
  }

  return adjustment;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Text Drawing: GPU
 * \{ */

static void blf_font_draw_ex(FontBLF *font,
                             GlyphCacheBLF *gc,
                             const char *str,
                             const size_t str_len,
                             struct ResultBLF *r_info,
                             ft_pix pen_y)
{
  GlyphBLF *g, *g_prev = NULL;
  ft_pix pen_x = 0;
  size_t i = 0;

  if (str_len == 0) {
    /* early output, don't do any IMM OpenGL. */
    return;
  }

  blf_batch_draw_begin(font);

  while ((i < str_len) && str[i]) {
    g = blf_glyph_from_utf8_and_step(font, gc, str, str_len, &i);

    if (UNLIKELY(g == NULL)) {
      continue;
    }
    pen_x += blf_kerning(font, g_prev, g);

    /* do not return this loop if clipped, we want every character tested */
    blf_glyph_draw(font, gc, g, ft_pix_to_int_floor(pen_x), ft_pix_to_int_floor(pen_y));

    pen_x = ft_pix_round_advance(pen_x, g->advance_x);
    g_prev = g;
  }

  blf_batch_draw_end();

  if (r_info) {
    r_info->lines = 1;
    r_info->width = ft_pix_to_int(pen_x);
  }
}
void blf_font_draw(FontBLF *font, const char *str, const size_t str_len, struct ResultBLF *r_info)
{
  GlyphCacheBLF *gc = blf_glyph_cache_acquire(font);
  blf_font_draw_ex(font, gc, str, str_len, r_info, 0);
  blf_glyph_cache_release(font);
}

int blf_font_draw_mono(FontBLF *font, const char *str, const size_t str_len, int cwidth)
{
  GlyphBLF *g;
  int col, columns = 0;
  ft_pix pen_x = 0, pen_y = 0;
  ft_pix cwidth_fpx = ft_pix_from_int(cwidth);

  size_t i = 0;

  GlyphCacheBLF *gc = blf_glyph_cache_acquire(font);

  blf_batch_draw_begin(font);

  while ((i < str_len) && str[i]) {
    g = blf_glyph_from_utf8_and_step(font, gc, str, str_len, &i);

    if (UNLIKELY(g == NULL)) {
      continue;
    }
    /* do not return this loop if clipped, we want every character tested */
    blf_glyph_draw(font, gc, g, ft_pix_to_int_floor(pen_x), ft_pix_to_int_floor(pen_y));

    col = BLI_wcwidth((char32_t)g->c);
    if (col < 0) {
      col = 1;
    }

    columns += col;
    pen_x += cwidth_fpx * col;
  }

  blf_batch_draw_end();

  blf_glyph_cache_release(font);
  return columns;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Text Drawing: Buffer
 * \{ */

/* Sanity checks are done by BLF_draw_buffer() */
static void blf_font_draw_buffer_ex(FontBLF *font,
                                    GlyphCacheBLF *gc,
                                    const char *str,
                                    const size_t str_len,
                                    struct ResultBLF *r_info,
                                    ft_pix pen_y)
{
  GlyphBLF *g, *g_prev = NULL;
  ft_pix pen_x = ft_pix_from_int(font->pos[0]);
  ft_pix pen_y_basis = ft_pix_from_int(font->pos[1]) + pen_y;
  size_t i = 0;

  /* buffer specific vars */
  FontBufInfoBLF *buf_info = &font->buf_info;
  const float *b_col_float = buf_info->col_float;
  const unsigned char *b_col_char = buf_info->col_char;
  int chx, chy;
  int y, x;

  /* another buffer specific call for color conversion */

  while ((i < str_len) && str[i]) {
    g = blf_glyph_from_utf8_and_step(font, gc, str, str_len, &i);

    if (UNLIKELY(g == NULL)) {
      continue;
    }
    pen_x += blf_kerning(font, g_prev, g);

    chx = ft_pix_to_int(pen_x + ft_pix_from_int(g->pos[0]));
    chy = ft_pix_to_int(pen_y_basis + ft_pix_from_int(g->dims[1]));

    if (g->pitch < 0) {
      pen_y = pen_y_basis + ft_pix_from_int(g->dims[1] - g->pos[1]);
    }
    else {
      pen_y = pen_y_basis - ft_pix_from_int(g->dims[1] - g->pos[1]);
    }

    if ((chx + g->dims[0]) >= 0 && chx < buf_info->dims[0] &&
        (ft_pix_to_int(pen_y) + g->dims[1]) >= 0 && ft_pix_to_int(pen_y) < buf_info->dims[1]) {
      /* don't draw beyond the buffer bounds */
      int width_clip = g->dims[0];
      int height_clip = g->dims[1];
      int yb_start = g->pitch < 0 ? 0 : g->dims[1] - 1;

      if (width_clip + chx > buf_info->dims[0]) {
        width_clip -= chx + width_clip - buf_info->dims[0];
      }
      if (height_clip + ft_pix_to_int(pen_y) > buf_info->dims[1]) {
        height_clip -= ft_pix_to_int(pen_y) + height_clip - buf_info->dims[1];
      }

      /* drawing below the image? */
      if (pen_y < 0) {
        yb_start += (g->pitch < 0) ? -ft_pix_to_int(pen_y) : ft_pix_to_int(pen_y);
        height_clip += ft_pix_to_int(pen_y);
        pen_y = 0;
      }

      /* Avoid conversions in the pixel writing loop. */
      const int pen_y_px = ft_pix_to_int(pen_y);

      if (buf_info->fbuf) {
        int yb = yb_start;
        for (y = ((chy >= 0) ? 0 : -chy); y < height_clip; y++) {
          for (x = ((chx >= 0) ? 0 : -chx); x < width_clip; x++) {
            const char a_byte = *(g->bitmap + x + (yb * g->pitch));
            if (a_byte) {
              const float a = (a_byte / 255.0f) * b_col_float[3];
              const size_t buf_ofs = (((size_t)(chx + x) +
                                       ((size_t)(pen_y_px + y) * (size_t)buf_info->dims[0])) *
                                      (size_t)buf_info->ch);
              float *fbuf = buf_info->fbuf + buf_ofs;

              float font_pixel[4];
              font_pixel[0] = b_col_float[0] * a;
              font_pixel[1] = b_col_float[1] * a;
              font_pixel[2] = b_col_float[2] * a;
              font_pixel[3] = a;
              blend_color_mix_float(fbuf, fbuf, font_pixel);
            }
          }

          if (g->pitch < 0) {
            yb++;
          }
          else {
            yb--;
          }
        }
      }

      if (buf_info->cbuf) {
        int yb = yb_start;
        for (y = ((chy >= 0) ? 0 : -chy); y < height_clip; y++) {
          for (x = ((chx >= 0) ? 0 : -chx); x < width_clip; x++) {
            const char a_byte = *(g->bitmap + x + (yb * g->pitch));

            if (a_byte) {
              const float a = (a_byte / 255.0f) * b_col_float[3];
              const size_t buf_ofs = (((size_t)(chx + x) +
                                       ((size_t)(pen_y_px + y) * (size_t)buf_info->dims[0])) *
                                      (size_t)buf_info->ch);
              unsigned char *cbuf = buf_info->cbuf + buf_ofs;

              uchar font_pixel[4];
              font_pixel[0] = b_col_char[0];
              font_pixel[1] = b_col_char[1];
              font_pixel[2] = b_col_char[2];
              font_pixel[3] = unit_float_to_uchar_clamp(a);
              blend_color_mix_byte(cbuf, cbuf, font_pixel);
            }
          }

          if (g->pitch < 0) {
            yb++;
          }
          else {
            yb--;
          }
        }
      }
    }

    pen_x = ft_pix_round_advance(pen_x, g->advance_x);
    g_prev = g;
  }

  if (r_info) {
    r_info->lines = 1;
    r_info->width = ft_pix_to_int(pen_x);
  }
}

void blf_font_draw_buffer(FontBLF *font,
                          const char *str,
                          const size_t str_len,
                          struct ResultBLF *r_info)
{
  GlyphCacheBLF *gc = blf_glyph_cache_acquire(font);
  blf_font_draw_buffer_ex(font, gc, str, str_len, r_info, 0);
  blf_glyph_cache_release(font);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Text Evaluation: Width to String Length
 *
 * Use to implement exported functions:
 * - #BLF_width_to_strlen
 * - #BLF_width_to_rstrlen
 * \{ */

static bool blf_font_width_to_strlen_glyph_process(
    FontBLF *font, GlyphBLF *g_prev, GlyphBLF *g, ft_pix *pen_x, const int width_i)
{
  if (UNLIKELY(g == NULL)) {
    return false; /* continue the calling loop. */
  }
  *pen_x += blf_kerning(font, g_prev, g);
  *pen_x = ft_pix_round_advance(*pen_x, g->advance_x);

  /* When true, break the calling loop. */
  return (ft_pix_to_int(*pen_x) >= width_i);
}

size_t blf_font_width_to_strlen(
    FontBLF *font, const char *str, const size_t str_len, int width, int *r_width)
{
  GlyphBLF *g, *g_prev;
  ft_pix pen_x;
  ft_pix width_new;
  size_t i, i_prev;

  GlyphCacheBLF *gc = blf_glyph_cache_acquire(font);
  const int width_i = (int)width;

  for (i_prev = i = 0, width_new = pen_x = 0, g_prev = NULL; (i < str_len) && str[i];
       i_prev = i, width_new = pen_x, g_prev = g) {
    g = blf_glyph_from_utf8_and_step(font, gc, str, str_len, &i);

    if (blf_font_width_to_strlen_glyph_process(font, g_prev, g, &pen_x, width_i)) {
      break;
    }
  }

  if (r_width) {
    *r_width = ft_pix_to_int(width_new);
  }

  blf_glyph_cache_release(font);
  return i_prev;
}

size_t blf_font_width_to_rstrlen(
    FontBLF *font, const char *str, const size_t str_len, int width, int *r_width)
{
  GlyphBLF *g, *g_prev;
  ft_pix pen_x, width_new;
  size_t i, i_prev, i_tmp;
  const char *s, *s_prev;

  GlyphCacheBLF *gc = blf_glyph_cache_acquire(font);

  i = BLI_strnlen(str, str_len);
  s = BLI_str_find_prev_char_utf8(&str[i], str);
  i = (size_t)(s - str);
  s_prev = BLI_str_find_prev_char_utf8(s, str);
  i_prev = (size_t)(s_prev - str);

  i_tmp = i;
  g = blf_glyph_from_utf8_and_step(font, gc, str, str_len, &i_tmp);
  for (width_new = pen_x = 0; (s != NULL);
       i = i_prev, s = s_prev, g = g_prev, g_prev = NULL, width_new = pen_x) {
    s_prev = BLI_str_find_prev_char_utf8(s, str);
    i_prev = (size_t)(s_prev - str);

    if (s_prev != NULL) {
      i_tmp = i_prev;
      g_prev = blf_glyph_from_utf8_and_step(font, gc, str, str_len, &i_tmp);
      BLI_assert(i_tmp == i);
    }

    if (blf_font_width_to_strlen_glyph_process(font, g_prev, g, &pen_x, width)) {
      break;
    }
  }

  if (r_width) {
    *r_width = ft_pix_to_int(width_new);
  }

  blf_glyph_cache_release(font);
  return i;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Text Evaluation: Glyph Bound Box with Callback
 * \{ */

static void blf_font_boundbox_ex(FontBLF *font,
                                 GlyphCacheBLF *gc,
                                 const char *str,
                                 const size_t str_len,
                                 rcti *box,
                                 struct ResultBLF *r_info,
                                 ft_pix pen_y)
{
  GlyphBLF *g, *g_prev = NULL;
  ft_pix pen_x = 0;
  size_t i = 0;

  ft_pix box_xmin = ft_pix_from_int(32000);
  ft_pix box_xmax = ft_pix_from_int(-32000);
  ft_pix box_ymin = ft_pix_from_int(32000);
  ft_pix box_ymax = ft_pix_from_int(-32000);

  while ((i < str_len) && str[i]) {
    g = blf_glyph_from_utf8_and_step(font, gc, str, str_len, &i);

    if (UNLIKELY(g == NULL)) {
      continue;
    }
    pen_x += blf_kerning(font, g_prev, g);
    const ft_pix pen_x_next = ft_pix_round_advance(pen_x, g->advance_x);

    const ft_pix gbox_xmin = pen_x;
    const ft_pix gbox_xmax = pen_x_next;
    const ft_pix gbox_ymin = g->box_ymin + pen_y;
    const ft_pix gbox_ymax = g->box_ymax + pen_y;

    if (gbox_xmin < box_xmin) {
      box_xmin = gbox_xmin;
    }
    if (gbox_ymin < box_ymin) {
      box_ymin = gbox_ymin;
    }

    if (gbox_xmax > box_xmax) {
      box_xmax = gbox_xmax;
    }
    if (gbox_ymax > box_ymax) {
      box_ymax = gbox_ymax;
    }

    pen_x = pen_x_next;
    g_prev = g;
  }

  if (box_xmin > box_xmax) {
    box_xmin = 0;
    box_ymin = 0;
    box_xmax = 0;
    box_ymax = 0;
  }

  box->xmin = ft_pix_to_int_floor(box_xmin);
  box->xmax = ft_pix_to_int_ceil(box_xmax);
  box->ymin = ft_pix_to_int_floor(box_ymin);
  box->ymax = ft_pix_to_int_ceil(box_ymax);

  if (r_info) {
    r_info->lines = 1;
    r_info->width = ft_pix_to_int(pen_x);
  }
}
void blf_font_boundbox(
    FontBLF *font, const char *str, const size_t str_len, rcti *r_box, struct ResultBLF *r_info)
{
  GlyphCacheBLF *gc = blf_glyph_cache_acquire(font);
  blf_font_boundbox_ex(font, gc, str, str_len, r_box, r_info, 0);
  blf_glyph_cache_release(font);
}

void blf_font_width_and_height(FontBLF *font,
                               const char *str,
                               const size_t str_len,
                               float *r_width,
                               float *r_height,
                               struct ResultBLF *r_info)
{
  float xa, ya;
  rcti box;

  if (font->flags & BLF_ASPECT) {
    xa = font->aspect[0];
    ya = font->aspect[1];
  }
  else {
    xa = 1.0f;
    ya = 1.0f;
  }

  if (font->flags & BLF_WORD_WRAP) {
    blf_font_boundbox__wrap(font, str, str_len, &box, r_info);
  }
  else {
    blf_font_boundbox(font, str, str_len, &box, r_info);
  }
  *r_width = ((float)BLI_rcti_size_x(&box) * xa);
  *r_height = ((float)BLI_rcti_size_y(&box) * ya);
}

float blf_font_width(FontBLF *font,
                     const char *str,
                     const size_t str_len,
                     struct ResultBLF *r_info)
{
  float xa;
  rcti box;

  if (font->flags & BLF_ASPECT) {
    xa = font->aspect[0];
  }
  else {
    xa = 1.0f;
  }

  if (font->flags & BLF_WORD_WRAP) {
    blf_font_boundbox__wrap(font, str, str_len, &box, r_info);
  }
  else {
    blf_font_boundbox(font, str, str_len, &box, r_info);
  }
  return (float)BLI_rcti_size_x(&box) * xa;
}

float blf_font_height(FontBLF *font,
                      const char *str,
                      const size_t str_len,
                      struct ResultBLF *r_info)
{
  float ya;
  rcti box;

  if (font->flags & BLF_ASPECT) {
    ya = font->aspect[1];
  }
  else {
    ya = 1.0f;
  }

  if (font->flags & BLF_WORD_WRAP) {
    blf_font_boundbox__wrap(font, str, str_len, &box, r_info);
  }
  else {
    blf_font_boundbox(font, str, str_len, &box, r_info);
  }
  return (float)BLI_rcti_size_y(&box) * ya;
}

float blf_font_fixed_width(FontBLF *font)
{
  GlyphCacheBLF *gc = blf_glyph_cache_acquire(font);
  float width = (gc) ? (float)gc->fixed_width : font->size / 2.0f;
  blf_glyph_cache_release(font);
  return width;
}

static void blf_font_boundbox_foreach_glyph_ex(FontBLF *font,
                                               GlyphCacheBLF *gc,
                                               const char *str,
                                               const size_t str_len,
                                               BLF_GlyphBoundsFn user_fn,
                                               void *user_data,
                                               struct ResultBLF *r_info,
                                               ft_pix pen_y)
{
  GlyphBLF *g, *g_prev = NULL;
  ft_pix pen_x = 0;
  size_t i = 0, i_curr;
  rcti gbox_px;

  if (str_len == 0 || str[0] == 0) {
    /* early output. */
    return;
  }

  while ((i < str_len) && str[i]) {
    i_curr = i;
    g = blf_glyph_from_utf8_and_step(font, gc, str, str_len, &i);

    if (UNLIKELY(g == NULL)) {
      continue;
    }
    pen_x += blf_kerning(font, g_prev, g);
    const ft_pix pen_x_next = ft_pix_round_advance(pen_x, g->advance_x);

    gbox_px.xmin = ft_pix_to_int_floor(pen_x);
    gbox_px.xmax = ft_pix_to_int_ceil(pen_x_next);
    gbox_px.ymin = ft_pix_to_int_floor(pen_y);
    gbox_px.ymax = gbox_px.ymin - g->dims[1];
    const int advance_x_px = gbox_px.xmax - gbox_px.xmin;

    pen_x = pen_x_next;

    rcti box_px;
    box_px.xmin = ft_pix_to_int_floor(g->box_xmin);
    box_px.xmax = ft_pix_to_int_ceil(g->box_xmax);
    box_px.ymin = ft_pix_to_int_floor(g->box_ymin);
    box_px.ymax = ft_pix_to_int_ceil(g->box_ymax);

    if (user_fn(str, i_curr, &gbox_px, advance_x_px, &box_px, g->pos, user_data) == false) {
      break;
    }

    g_prev = g;
  }

  if (r_info) {
    r_info->lines = 1;
    r_info->width = ft_pix_to_int(pen_x);
  }
}
void blf_font_boundbox_foreach_glyph(FontBLF *font,
                                     const char *str,
                                     const size_t str_len,
                                     BLF_GlyphBoundsFn user_fn,
                                     void *user_data,
                                     struct ResultBLF *r_info)
{
  GlyphCacheBLF *gc = blf_glyph_cache_acquire(font);
  blf_font_boundbox_foreach_glyph_ex(font, gc, str, str_len, user_fn, user_data, r_info, 0);
  blf_glyph_cache_release(font);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Text Evaluation: Word-Wrap with Callback
 * \{ */

/**
 * Generic function to add word-wrap support for other existing functions.
 *
 * Wraps on spaces and respects newlines.
 * Intentionally ignores non-unix newlines, tabs and more advanced text formatting.
 *
 * \note If we want rich text - we better have a higher level API to handle that
 * (color, bold, switching fonts... etc).
 */
static void blf_font_wrap_apply(FontBLF *font,
                                const char *str,
                                const size_t str_len,
                                struct ResultBLF *r_info,
                                void (*callback)(FontBLF *font,
                                                 GlyphCacheBLF *gc,
                                                 const char *str,
                                                 const size_t str_len,
                                                 ft_pix pen_y,
                                                 void *userdata),
                                void *userdata)
{
  GlyphBLF *g, *g_prev = NULL;
  ft_pix pen_x = 0;
  ft_pix pen_y = 0;
  size_t i = 0;
  int lines = 0;
  ft_pix pen_x_next = 0;

  ft_pix line_height = blf_font_height_max_ft_pix(font);

  GlyphCacheBLF *gc = blf_glyph_cache_acquire(font);

  struct WordWrapVars {
    ft_pix wrap_width;
    size_t start, last[2];
  } wrap = {font->wrap_width != -1 ? ft_pix_from_int(font->wrap_width) : INT_MAX, 0, {0, 0}};

  // printf("%s wrapping (%d, %d) `%s`:\n", __func__, str_len, strlen(str), str);
  while ((i < str_len) && str[i]) {

    /* wrap vars */
    size_t i_curr = i;
    bool do_draw = false;

    g = blf_glyph_from_utf8_and_step(font, gc, str, str_len, &i);

    if (UNLIKELY(g == NULL)) {
      continue;
    }
    pen_x += blf_kerning(font, g_prev, g);

    /**
     * Implementation Detail (utf8).
     *
     * Take care with single byte offsets here,
     * since this is utf8 we can't be sure a single byte is a single character.
     *
     * This is _only_ done when we know for sure the character is ascii (newline or a space).
     */
    pen_x_next = ft_pix_round_advance(pen_x, g->advance_x);
    if (UNLIKELY((pen_x_next >= wrap.wrap_width) && (wrap.start != wrap.last[0]))) {
      do_draw = true;
    }
    else if (UNLIKELY(((i < str_len) && str[i]) == 0)) {
      /* need check here for trailing newline, else we draw it */
      wrap.last[0] = i + ((g->c != '\n') ? 1 : 0);
      wrap.last[1] = i;
      do_draw = true;
    }
    else if (UNLIKELY(g->c == '\n')) {
      wrap.last[0] = i_curr + 1;
      wrap.last[1] = i;
      do_draw = true;
    }
    else if (UNLIKELY(g->c != ' ' && (g_prev ? g_prev->c == ' ' : false))) {
      wrap.last[0] = i_curr;
      wrap.last[1] = i_curr;
    }

    if (UNLIKELY(do_draw)) {
      // printf("(%03d..%03d)  `%.*s`\n",
      //        wrap.start, wrap.last[0], (wrap.last[0] - wrap.start) - 1, &str[wrap.start]);

      callback(font, gc, &str[wrap.start], (wrap.last[0] - wrap.start) - 1, pen_y, userdata);
      wrap.start = wrap.last[0];
      i = wrap.last[1];
      pen_x = 0;
      pen_y -= line_height;
      g_prev = NULL;
      lines += 1;
      continue;
    }

    pen_x = pen_x_next;
    g_prev = g;
  }

  // printf("done! lines: %d, width, %d\n", lines, pen_x_next);

  if (r_info) {
    r_info->lines = lines;
    /* width of last line only (with wrapped lines) */
    r_info->width = ft_pix_to_int(pen_x_next);
  }

  blf_glyph_cache_release(font);
}

/* blf_font_draw__wrap */
static void blf_font_draw__wrap_cb(FontBLF *font,
                                   GlyphCacheBLF *gc,
                                   const char *str,
                                   const size_t str_len,
                                   ft_pix pen_y,
                                   void *UNUSED(userdata))
{
  blf_font_draw_ex(font, gc, str, str_len, NULL, pen_y);
}
void blf_font_draw__wrap(FontBLF *font,
                         const char *str,
                         const size_t str_len,
                         struct ResultBLF *r_info)
{
  blf_font_wrap_apply(font, str, str_len, r_info, blf_font_draw__wrap_cb, NULL);
}

/* blf_font_boundbox__wrap */
static void blf_font_boundbox_wrap_cb(FontBLF *font,
                                      GlyphCacheBLF *gc,
                                      const char *str,
                                      const size_t str_len,
                                      ft_pix pen_y,
                                      void *userdata)
{
  rcti *box = userdata;
  rcti box_single;

  blf_font_boundbox_ex(font, gc, str, str_len, &box_single, NULL, pen_y);
  BLI_rcti_union(box, &box_single);
}
void blf_font_boundbox__wrap(
    FontBLF *font, const char *str, const size_t str_len, rcti *box, struct ResultBLF *r_info)
{
  box->xmin = 32000;
  box->xmax = -32000;
  box->ymin = 32000;
  box->ymax = -32000;

  blf_font_wrap_apply(font, str, str_len, r_info, blf_font_boundbox_wrap_cb, box);
}

/* blf_font_draw_buffer__wrap */
static void blf_font_draw_buffer__wrap_cb(FontBLF *font,
                                          GlyphCacheBLF *gc,
                                          const char *str,
                                          const size_t str_len,
                                          ft_pix pen_y,
                                          void *UNUSED(userdata))
{
  blf_font_draw_buffer_ex(font, gc, str, str_len, NULL, pen_y);
}
void blf_font_draw_buffer__wrap(FontBLF *font,
                                const char *str,
                                const size_t str_len,
                                struct ResultBLF *r_info)
{
  blf_font_wrap_apply(font, str, str_len, r_info, blf_font_draw_buffer__wrap_cb, NULL);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Text Evaluation: Count Missing Characters
 * \{ */

int blf_font_count_missing_chars(FontBLF *font,
                                 const char *str,
                                 const size_t str_len,
                                 int *r_tot_chars)
{
  int missing = 0;
  size_t i = 0;

  *r_tot_chars = 0;
  while (i < str_len) {
    unsigned int c;

    if ((c = str[i]) < GLYPH_ASCII_TABLE_SIZE) {
      i++;
    }
    else {
      c = BLI_str_utf8_as_unicode_step(str, str_len, &i);
      if (blf_get_char_index(font, c) == 0) {
        missing++;
      }
    }
    (*r_tot_chars)++;
  }
  return missing;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Font Query: Attributes
 * \{ */

static ft_pix blf_font_height_max_ft_pix(FontBLF *font)
{
  blf_ensure_size(font);
  /* Metrics.height is rounded to pixel. Force minimum of one pixel. */
  return MAX2((ft_pix)font->ft_size->metrics.height, ft_pix_from_int(1));
}

int blf_font_height_max(FontBLF *font)
{
  return ft_pix_to_int(blf_font_height_max_ft_pix(font));
}

static ft_pix blf_font_width_max_ft_pix(FontBLF *font)
{
  blf_ensure_size(font);
  /* Metrics.max_advance is rounded to pixel. Force minimum of one pixel. */
  return MAX2((ft_pix)font->ft_size->metrics.max_advance, ft_pix_from_int(1));
}

int blf_font_width_max(FontBLF *font)
{
  return ft_pix_to_int(blf_font_width_max_ft_pix(font));
}

int blf_font_descender(FontBLF *font)
{
  blf_ensure_size(font);
  return ft_pix_to_int((ft_pix)font->ft_size->metrics.descender);
}

int blf_font_ascender(FontBLF *font)
{
  blf_ensure_size(font);
  return ft_pix_to_int((ft_pix)font->ft_size->metrics.ascender);
}

char *blf_display_name(FontBLF *font)
{
  if (!blf_ensure_face(font) || !font->face->family_name) {
    return NULL;
  }
  return BLI_sprintfN("%s %s", font->face->family_name, font->face->style_name);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Font Subsystem Init/Exit
 * \{ */

int blf_font_init(void)
{
  memset(&g_batch, 0, sizeof(g_batch));
  BLI_mutex_init(&ft_lib_mutex);
  int err = FT_Init_FreeType(&ft_lib);
  if (err == FT_Err_Ok) {
    /* Create a FreeType cache manager. */
    err = FTC_Manager_New(ft_lib,
                          BLF_CACHE_MAX_FACES,
                          BLF_CACHE_MAX_SIZES,
                          BLF_CACHE_BYTES,
                          blf_cache_face_requester,
                          NULL,
                          &ftc_manager);
    if (err == FT_Err_Ok) {
      /* Create a charmap cache to speed up glyph index lookups. */
      err = FTC_CMapCache_New(ftc_manager, &ftc_charmap_cache);
    }
  }
  return err;
}

void blf_font_exit(void)
{
  BLI_mutex_end(&ft_lib_mutex);
  if (ftc_manager) {
    FTC_Manager_Done(ftc_manager);
  }
  if (ft_lib) {
    FT_Done_FreeType(ft_lib);
  }
  blf_batch_draw_exit();
}

void BLF_cache_flush_set_fn(void (*cache_flush_fn)(void))
{
  blf_draw_cache_flush = cache_flush_fn;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Font New/Free
 * \{ */

static void blf_font_fill(FontBLF *font)
{
  font->aspect[0] = 1.0f;
  font->aspect[1] = 1.0f;
  font->aspect[2] = 1.0f;
  font->pos[0] = 0;
  font->pos[1] = 0;
  font->angle = 0.0f;

  for (int i = 0; i < 16; i++) {
    font->m[i] = 0;
  }

  /* annoying bright color so we can see where to add BLF_color calls */
  font->color[0] = 255;
  font->color[1] = 255;
  font->color[2] = 0;
  font->color[3] = 255;

  font->clip_rec.xmin = 0;
  font->clip_rec.xmax = 0;
  font->clip_rec.ymin = 0;
  font->clip_rec.ymax = 0;
  font->flags = 0;
  font->dpi = 0;
  font->size = 0;
  BLI_listbase_clear(&font->cache);
  font->kerning_cache = NULL;
#if BLF_BLUR_ENABLE
  font->blur = 0;
#endif
  font->tex_size_max = -1;

  font->buf_info.fbuf = NULL;
  font->buf_info.cbuf = NULL;
  font->buf_info.dims[0] = 0;
  font->buf_info.dims[1] = 0;
  font->buf_info.ch = 0;
  font->buf_info.col_init[0] = 0;
  font->buf_info.col_init[1] = 0;
  font->buf_info.col_init[2] = 0;
  font->buf_info.col_init[3] = 0;
}

/**
 * Create an FT_Face for this font if not already existing.
 */
bool blf_ensure_face(FontBLF *font)
{
  if (font->face) {
    return true;
  }

  if (font->flags & BLF_BAD_FONT) {
    return false;
  }

  FT_Error err;

  if (font->flags & BLF_CACHED) {
    err = FTC_Manager_LookupFace(ftc_manager, font, &font->face);
  }
  else {
    BLI_mutex_lock(&ft_lib_mutex);
    if (font->filepath) {
      err = FT_New_Face(font->ft_lib, font->filepath, 0, &font->face);
    }
    if (font->mem) {
      err = FT_New_Memory_Face(font->ft_lib, font->mem, (FT_Long)font->mem_size, 0, &font->face);
    }
    font->face->generic.data = font;
    BLI_mutex_unlock(&ft_lib_mutex);
  }

  if (err) {
    if (ELEM(err, FT_Err_Unknown_File_Format, FT_Err_Unimplemented_Feature)) {
      printf("Format of this font file is not supported\n");
    }
    else {
      printf("Error encountered while opening font file\n");
    }
    font->flags |= BLF_BAD_FONT;
    return false;
  }

  err = FT_Select_Charmap(font->face, FT_ENCODING_UNICODE);
  if (err) {
    err = FT_Select_Charmap(font->face, FT_ENCODING_APPLE_ROMAN);
  }
  if (err && font->face->num_charmaps > 0) {
    err = FT_Select_Charmap(font->face, font->face->charmaps[0]->encoding);
  }
  if (err) {
    printf("Can't set a character map!\n");
    font->flags |= BLF_BAD_FONT;
    return false;
  }

  if (font->filepath) {
    char *mfile = blf_dir_metrics_search(font->filepath);
    if (mfile) {
      err = FT_Attach_File(font->face, mfile);
      if (err) {
        fprintf(stderr,
                "FT_Attach_File failed to load '%s' with error %d\n",
                font->filepath,
                (int)err);
      }
      MEM_freeN(mfile);
    }
  }

  if (!(font->flags & BLF_CACHED)) {
    /* Not cached so point at the face's size for convenience. */
    font->ft_size = font->face->size;
  }

  font->face_flags = font->face->face_flags;

  if (FT_HAS_MULTIPLE_MASTERS(font)) {
    FT_Get_MM_Var(font->face, &(font->variations));
  }

  /* Save TrueType table with bits to quickly test most unicode block coverage. */
  TT_OS2 *os2_table = (TT_OS2 *)FT_Get_Sfnt_Table(font->face, FT_SFNT_OS2);
  if (os2_table) {
    font->UnicodeRanges[0] = (uint)os2_table->ulUnicodeRange1;
    font->UnicodeRanges[1] = (uint)os2_table->ulUnicodeRange2;
    font->UnicodeRanges[2] = (uint)os2_table->ulUnicodeRange3;
    font->UnicodeRanges[3] = (uint)os2_table->ulUnicodeRange4;
  }

  if (FT_IS_FIXED_WIDTH(font)) {
    font->flags |= BLF_MONOSPACED;
  }

  if (FT_HAS_KERNING(font) && !font->kerning_cache) {
    /* Create kerning cache table and fill with value indicating "unset". */
    font->kerning_cache = MEM_mallocN(sizeof(KerningCacheBLF), __func__);
    for (uint i = 0; i < KERNING_CACHE_TABLE_SIZE; i++) {
      for (uint j = 0; j < KERNING_CACHE_TABLE_SIZE; j++) {
        font->kerning_cache->ascii_table[i][j] = KERNING_ENTRY_UNSET;
      }
    }
  }

  return true;
}

typedef struct eFaceDetails {
  char name[50];
  unsigned int coverage1;
  unsigned int coverage2;
  unsigned int coverage3;
  unsigned int coverage4;
} eFaceDetails;

/* Details about the fallback fonts we ship, so that we can load only when needed. */
static const eFaceDetails static_face_details[] = {
    {"lastresort.woff2", UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX},
    {"Noto Sans CJK Regular.woff2", 0x30000083L, 0x2BDF3C10L, 0x16L, 0},
    {"NotoEmoji-VariableFont_wght.woff2", 0x80000003L, 0x241E4ACL, 0x14000000L, 0x4000000L},
    {"NotoSansArabic-VariableFont_wdth,wght.woff2",
     TT_UCR_ARABIC,
     (uint)TT_UCR_ARABIC_PRESENTATION_FORMS_A,
     TT_UCR_ARABIC_PRESENTATION_FORMS_B,
     0},
    {"NotoSansArmenian-VariableFont_wdth,wght.woff2", TT_UCR_ARMENIAN, 0, 0, 0},
    {"NotoSansBengali-VariableFont_wdth,wght.woff2", TT_UCR_BENGALI, 0, 0, 0},
    {"NotoSansDevanagari-Regular.woff2", TT_UCR_DEVANAGARI, 0, 0, 0},
    {"NotoSansEthiopic-Regular.woff2", 0, 0, TT_UCR_ETHIOPIC, 0},
    {"NotoSansGeorgian-VariableFont_wdth,wght.woff2", TT_UCR_GEORGIAN, 0, 0, 0},
    {"NotoSansGujarati-Regular.woff2", TT_UCR_GUJARATI, 0, 0, 0},
    {"NotoSansGurmukhi-VariableFont_wdth,wght.woff2", TT_UCR_GURMUKHI, 0, 0, 0},
    {"NotoSansHebrew-VariableFont_wdth,wght.woff2", TT_UCR_HEBREW, 0, 0, 0},
    {"NotoSansJavanese-Regular.woff2", 0x80000003L, 0x2000L, 0, 0},
    {"NotoSansKannada-VariableFont_wdth,wght.woff2", TT_UCR_KANNADA, 0, 0, 0},
    {"NotoSansMalayalam-VariableFont_wdth,wght.woff2", TT_UCR_MALAYALAM, 0, 0, 0},
    {"NotoSansMath-Regular.woff2", 0, TT_UCR_MATHEMATICAL_OPERATORS, 0, 0},
    {"NotoSansMyanmar-Regular.woff2", 0, 0, TT_UCR_MYANMAR, 0},
    {"NotoSansSymbols-VariableFont_wght.woff2", 0x3L, 0x200E4B4L, 0, 0},
    {"NotoSansSymbols2-Regular.woff2", 0x80000003L, 0x200E3E4L, 0x40020L, 0x580A048L},
    {"NotoSansTamil-VariableFont_wdth,wght.woff2", TT_UCR_TAMIL, 0, 0, 0},
    {"NotoSansTelugu-VariableFont_wdth,wght.woff2", TT_UCR_TELUGU, 0, 0, 0},
    {"NotoSansThai-VariableFont_wdth,wght.woff2", TT_UCR_THAI, 0, 0, 0},
};

/**
 * Create a new font from filename OR memory pointer.
 * For normal operation pass NULL as FT_Library object. Pass a custom FT_Library if you
 * want to use the font without its lifetime being managed by the FreeType cache subsystem.
 */
FontBLF *blf_font_new_ex(const char *name,
                         const char *filepath,
                         const unsigned char *mem,
                         const size_t mem_size,
                         void *ft_library)
{
  FontBLF *font = (FontBLF *)MEM_callocN(sizeof(FontBLF), "blf_font_new");

  font->name = BLI_strdup(name);
  font->filepath = filepath ? BLI_strdup(filepath) : NULL;
  if (mem) {
    font->mem = (void *)mem;
    font->mem_size = mem_size;
  }
  blf_font_fill(font);

  if (ft_library && ((FT_Library)ft_library != ft_lib)) {
    font->ft_lib = (FT_Library)ft_library;
  }
  else {
    font->ft_lib = ft_lib;
    font->flags |= BLF_CACHED;
  }

  font->ft_lib = ft_library ? (FT_Library)ft_library : ft_lib;

  BLI_mutex_init(&font->glyph_cache_mutex);

  /* If we have static details about this font file, we don't have to load the Face yet. */
  bool face_needed = true;

  if (font->filepath) {
    const eFaceDetails *static_details = NULL;
    char filename[256];
    for (int i = 0; i < (int)ARRAY_SIZE(static_face_details); i++) {
      BLI_split_file_part(font->filepath, filename, sizeof(filename));
      if (STREQ(static_face_details[i].name, filename)) {
        static_details = &static_face_details[i];
        font->UnicodeRanges[0] = static_details->coverage1;
        font->UnicodeRanges[1] = static_details->coverage2;
        font->UnicodeRanges[2] = static_details->coverage3;
        font->UnicodeRanges[3] = static_details->coverage4;
        face_needed = false;
        break;
      }
    }
  }

  if (face_needed) {
    if (!blf_ensure_face(font)) {
      blf_font_free(font);
      return NULL;
    }
  }

  /* Detect "Last resort" fonts. They have everything. Usually except last 5 bits.  */
  if (font->UnicodeRanges[0] == 0xffffffffU && font->UnicodeRanges[1] == 0xffffffffU &&
      font->UnicodeRanges[2] == 0xffffffffU && font->UnicodeRanges[3] >= 0x7FFFFFFU) {
    font->flags |= BLF_LAST_RESORT;
  }

  return font;
}

FontBLF *blf_font_new(const char *name, const char *filepath)
{
  return blf_font_new_ex(name, filepath, NULL, 0, NULL);
}

FontBLF *blf_font_new_from_mem(const char *name, const unsigned char *mem, const size_t mem_size)
{
  return blf_font_new_ex(name, NULL, mem, mem_size, NULL);
}

void blf_font_attach_from_mem(FontBLF *font, const unsigned char *mem, const size_t mem_size)
{
  FT_Open_Args open;

  open.flags = FT_OPEN_MEMORY;
  open.memory_base = (const FT_Byte *)mem;
  open.memory_size = (FT_Long)mem_size;
  if (blf_ensure_face(font)) {
    FT_Attach_Stream(font->face, &open);
  }
}

void blf_font_free(FontBLF *font)
{
  blf_glyph_cache_clear(font);

  if (font->kerning_cache) {
    MEM_freeN(font->kerning_cache);
  }

  if (font->variations) {
    FT_Done_MM_Var(font->ft_lib, font->variations);
  }

  if (font->face) {
    BLI_mutex_lock(&ft_lib_mutex);
    if (font->flags & BLF_CACHED) {
      FTC_Manager_RemoveFaceID(ftc_manager, font);
    }
    else {
      FT_Done_Face(font->face);
    }
    BLI_mutex_unlock(&ft_lib_mutex);
    font->face = NULL;
  }
  if (font->filepath) {
    MEM_freeN(font->filepath);
  }
  if (font->name) {
    MEM_freeN(font->name);
  }

  BLI_mutex_end(&font->glyph_cache_mutex);

  MEM_freeN(font);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Font Configure
 * \{ */

void blf_ensure_size(FontBLF *font)
{
  if (font->ft_size || !(font->flags & BLF_CACHED)) {
    return;
  }

  FTC_ScalerRec scaler = {0};
  scaler.face_id = font;
  scaler.width = 0;
  scaler.height = round_fl_to_uint(font->size * 64.0f);
  scaler.pixel = 0;
  scaler.x_res = font->dpi;
  scaler.y_res = font->dpi;
  if (FTC_Manager_LookupSize(ftc_manager, &scaler, &font->ft_size) == FT_Err_Ok) {
    font->ft_size->generic.data = (void *)font;
    font->ft_size->generic.finalizer = blf_size_finalizer;
    return;
  }

  BLI_assert_unreachable();
}

bool blf_font_size(FontBLF *font, float size, unsigned int dpi)
{
  if (!blf_ensure_face(font)) {
    return false;
  }

  /* FreeType uses fixed-point integers in 64ths. */
  FT_UInt ft_size = round_fl_to_uint(size * 64.0f);
  /* Adjust our new size to be on even 64ths. */
  size = (float)ft_size / 64.0f;

  if (font->size != size || font->dpi != dpi) {
    if (font->flags & BLF_CACHED) {
      FTC_ScalerRec scaler = {0};
      scaler.face_id = font;
      scaler.width = 0;
      scaler.height = ft_size;
      scaler.pixel = 0;
      scaler.x_res = dpi;
      scaler.y_res = dpi;
      if (FTC_Manager_LookupSize(ftc_manager, &scaler, &font->ft_size) != FT_Err_Ok) {
        return false;
      }
      font->ft_size->generic.data = (void *)font;
      font->ft_size->generic.finalizer = blf_size_finalizer;
    }
    else {
      if (FT_Set_Char_Size(font->face, 0, ft_size, dpi, dpi) != FT_Err_Ok) {
        return false;
      }
      font->ft_size = font->face->size;
    }
  }

  font->size = size;
  font->dpi = dpi;
  return true;
}

/** \} */
