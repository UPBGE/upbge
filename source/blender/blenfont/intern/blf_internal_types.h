/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2008 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup blf
 */

#pragma once

#include "GPU_texture.h"
#include "GPU_vertex_buffer.h"

#include FT_MULTIPLE_MASTERS_H /* Variable font support. */

#define BLF_VARIATIONS_MAX 16 /* Maximum variation axes per font. */

#define MAKE_DVAR_TAG(a, b, c, d) \
  (((uint32_t)a << 24u) | ((uint32_t)b << 16u) | ((uint32_t)c << 8u) | ((uint32_t)d))

#define blf_variation_axis_weight MAKE_DVAR_TAG('w', 'g', 'h', 't')  /* 'wght' weight axis. */
#define blf_variation_axis_slant MAKE_DVAR_TAG('s', 'l', 'n', 't')   /* 'slnt' slant axis. */
#define blf_variation_axis_width MAKE_DVAR_TAG('w', 'd', 't', 'h')   /* 'wdth' width axis. */
#define blf_variation_axis_spacing MAKE_DVAR_TAG('s', 'p', 'a', 'c') /* 'spac' spacing axis. */
#define blf_variation_axis_optsize MAKE_DVAR_TAG('o', 'p', 's', 'z') /* 'opsz' optical size. */

/* -------------------------------------------------------------------- */
/** \name Sub-Pixel Offset & Utilities
 *
 * Free-type uses fixed point precision for sub-pixel offsets.
 * Utility functions here avoid exposing the details in the BLF API.
 * \{ */

/**
 * This is an internal type that represents sub-pixel positioning,
 * users of this type are to use `ft_pix_*` functions to keep scaling/rounding in one place.
 */
typedef int32_t ft_pix;

/* Macros copied from `include/freetype/internal/ftobjs.h`. */

/* FIXME(@campbellbarton): Follow rounding from Blender 3.1x and older.
 * This is what users will expect and changing this creates wider spaced text.
 * Use this macro to communicate that rounding should be used, using floor is to avoid
 * user visible changes, which can be reviewed and handled separately. */
#define USE_LEGACY_SPACING

#define FT_PIX_FLOOR(x) ((x) & ~63)
#define FT_PIX_ROUND(x) FT_PIX_FLOOR((x) + 32)
#define FT_PIX_CEIL(x) ((x) + 63)

#ifdef USE_LEGACY_SPACING
#  define FT_PIX_DEFAULT_ROUNDING(x) FT_PIX_FLOOR(x)
#else
#  define FT_PIX_DEFAULT_ROUNDING(x) FT_PIX_ROUND(x)
#endif

BLI_INLINE int ft_pix_to_int(ft_pix v)
{
#ifdef USE_LEGACY_SPACING
  return (int)(v >> 6);
#else
  return (int)(FT_PIX_DEFAULT_ROUNDING(v) >> 6);
#endif
}

BLI_INLINE int ft_pix_to_int_floor(ft_pix v)
{
  return (int)(v >> 6); /* No need for explicit floor as the bits are removed when shifting. */
}

BLI_INLINE int ft_pix_to_int_ceil(ft_pix v)
{
  return (int)(FT_PIX_CEIL(v) >> 6);
}

BLI_INLINE ft_pix ft_pix_from_int(int v)
{
  return v * 64;
}

BLI_INLINE ft_pix ft_pix_from_float(float v)
{
  return lroundf(v * 64.0f);
}

BLI_INLINE ft_pix ft_pix_round_advance(ft_pix v, ft_pix step)
{
  /* See #USE_LEGACY_SPACING, rounding logic could change here. */
  return FT_PIX_DEFAULT_ROUNDING(v) + FT_PIX_DEFAULT_ROUNDING(step);
}

#undef FT_PIX_ROUND
#undef FT_PIX_CEIL
#undef FT_PIX_DEFAULT_ROUNDING

/** \} */

#define BLF_BATCH_DRAW_LEN_MAX 2048 /* in glyph */

/* Number of characters in GlyphCacheBLF.glyph_ascii_table. */
#define GLYPH_ASCII_TABLE_SIZE 128

/* Number of characters in KerningCacheBLF.table. */
#define KERNING_CACHE_TABLE_SIZE 128

/* A value in the kerning cache that indicates it is not yet set. */
#define KERNING_ENTRY_UNSET INT_MAX

typedef struct BatchBLF {
  struct FontBLF *font; /* can only batch glyph from the same font */
  struct GPUBatch *batch;
  struct GPUVertBuf *verts;
  struct GPUVertBufRaw pos_step, col_step, offset_step, glyph_size_step;
  unsigned int pos_loc, col_loc, offset_loc, glyph_size_loc;
  unsigned int glyph_len;
  int ofs[2];      /* copy of font->pos */
  float mat[4][4]; /* previous call modelmatrix. */
  bool enabled, active, simple_shader;
  struct GlyphCacheBLF *glyph_cache;
} BatchBLF;

extern BatchBLF g_batch;

typedef struct KerningCacheBLF {
  /**
   * Cache a ascii glyph pairs. Only store the x offset we are interested in,
   * instead of the full #FT_Vector since it's not used for drawing at the moment.
   */
  int ascii_table[KERNING_CACHE_TABLE_SIZE][KERNING_CACHE_TABLE_SIZE];
} KerningCacheBLF;

typedef struct GlyphCacheBLF {
  struct GlyphCacheBLF *next;
  struct GlyphCacheBLF *prev;

  /* font size. */
  float size;

  /* and DPI. */
  unsigned int dpi;
  float char_weight;
  float char_slant;
  float char_width;
  float char_spacing;

  bool bold;
  bool italic;

  /* Column width when printing monospaced. */
  int fixed_width;

  /* and the glyphs. */
  ListBase bucket[257];

  /* fast ascii lookup */
  struct GlyphBLF *glyph_ascii_table[GLYPH_ASCII_TABLE_SIZE];

  /* texture array, to draw the glyphs. */
  GPUTexture *texture;
  char *bitmap_result;
  int bitmap_len;
  int bitmap_len_landed;
  int bitmap_len_alloc;

} GlyphCacheBLF;

typedef struct GlyphBLF {
  struct GlyphBLF *next;
  struct GlyphBLF *prev;

  /* and the character, as UTF-32 */
  unsigned int c;

  /* freetype2 index, to speed-up the search. */
  FT_UInt idx;

  /* glyph box. */
  ft_pix box_xmin;
  ft_pix box_xmax;
  ft_pix box_ymin;
  ft_pix box_ymax;

  ft_pix advance_x;

  /* The difference in bearings when hinting is active, zero otherwise. */
  ft_pix lsb_delta;
  ft_pix rsb_delta;

  /* position inside the texture where this glyph is store. */
  int offset;

  /* Bitmap data, from freetype. Take care that this
   * can be NULL.
   */
  unsigned char *bitmap;

  /* Glyph width and height. */
  int dims[2];
  int pitch;

  /**
   * X and Y bearing of the glyph.
   * The X bearing is from the origin to the glyph left bbox edge.
   * The Y bearing is from the baseline to the top of the glyph edge.
   */
  int pos[2];

  struct GlyphCacheBLF *glyph_cache;
} GlyphBLF;

typedef struct FontBufInfoBLF {
  /* for draw to buffer, always set this to NULL after finish! */
  float *fbuf;

  /* the same but unsigned char */
  unsigned char *cbuf;

  /** Buffer size, keep signed so comparisons with negative values work. */
  int dims[2];

  /* number of channels. */
  int ch;

  /* display device used for color management */
  struct ColorManagedDisplay *display;

  /* and the color, the alphas is get from the glyph!
   * color is sRGB space */
  float col_init[4];
  /* cached conversion from 'col_init' */
  unsigned char col_char[4];
  float col_float[4];

} FontBufInfoBLF;

typedef struct FontBLF {
  /* font name. */
  char *name;

  /* # of times this font was loaded */
  unsigned int reference_count;

  /* Full path to font file or NULL if from memory. */
  char *filepath;

  /* Pointer to in-memory font, or NULL if from file. */
  void *mem;
  size_t mem_size;

  /* Copied from the SFNT OS/2 table. Bit flags for unicode blocks and ranges
   * considered "functional". Cached here because face might not always exist.
   * See: https://docs.microsoft.com/en-us/typography/opentype/spec/os2#ur */
  uint UnicodeRanges[4];

  /* aspect ratio or scale. */
  float aspect[3];

  /* initial position for draw the text. */
  int pos[3];

  /* angle in radians. */
  float angle;

#if 0 /* BLF_BLUR_ENABLE */
  /* blur: 3 or 5 large kernel */
  int blur;
#endif

  /* shadow level. */
  int shadow;

  /* and shadow offset. */
  int shadow_x;
  int shadow_y;

  /* shadow color. */
  unsigned char shadow_color[4];

  /* main text color. */
  unsigned char color[4];

  /* Multiplied this matrix with the current one before
   * draw the text! see blf_draw__start.
   */
  float m[16];

  /* clipping rectangle. */
  rcti clip_rec;

  /* the width to wrap the text, see BLF_WORD_WRAP */
  int wrap_width;

  /* Font DPI (default 72). */
  unsigned int dpi;

  /* font size. */
  float size;

  /* Axes data for Adobe MM, TrueType GX, or OpenType variation fonts.  */
  FT_MM_Var *variations;

  /* Character variation; 0=default, -1=min, +1=max. */
  float char_weight;
  float char_slant;
  float char_width;
  float char_spacing;

  /* max texture size. */
  int tex_size_max;

  /* font options. */
  int flags;

  /**
   * List of glyph caches (#GlyphCacheBLF) for this font for size, DPI, bold, italic.
   * Use blf_glyph_cache_acquire(font) and blf_glyph_cache_release(font) to access cache!
   */
  ListBase cache;

  /* Cache of unscaled kerning values. Will be NULL if font does not have kerning. */
  KerningCacheBLF *kerning_cache;

  /* freetype2 lib handle. */
  FT_Library ft_lib;

  /* freetype2 face. */
  FT_Face face;

  /* Point to face->size or to cache's size. */
  FT_Size ft_size;

  /* Copy of the font->face->face_flags, in case we don't have a face loaded. */
  FT_Long face_flags;

  /* data for buffer usage (drawing into a texture buffer) */
  FontBufInfoBLF buf_info;

  /* Mutex lock for glyph cache. */
  ThreadMutex glyph_cache_mutex;
} FontBLF;

typedef struct DirBLF {
  struct DirBLF *next;
  struct DirBLF *prev;

  /* full path where search fonts. */
  char *path;
} DirBLF;
