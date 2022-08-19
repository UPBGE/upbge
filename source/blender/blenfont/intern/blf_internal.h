/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2009 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup blf
 */

#pragma once

struct FontBLF;
struct GlyphBLF;
struct GlyphCacheBLF;
struct ResultBLF;
struct rctf;
struct rcti;

/* Max number of FontBLFs in memory. Take care that every font has a glyph cache per size/dpi,
 * so we don't need load the same font with different size, just load one and call BLF_size. */
#define BLF_MAX_FONT 64

/* Maximum number of opened FT_Face objects managed by cache. 0 is default of 2. */
#define BLF_CACHE_MAX_FACES 4
/* Maximum number of opened FT_Size objects managed by cache. 0 is default of 4 */
#define BLF_CACHE_MAX_SIZES 8
/* Maximum number of bytes to use for cached data nodes. 0 is default of 200,000. */
#define BLF_CACHE_BYTES 400000

extern struct FontBLF *global_font[BLF_MAX_FONT];

void blf_batch_draw_begin(struct FontBLF *font);
void blf_batch_draw(void);

unsigned int blf_next_p2(unsigned int x);
unsigned int blf_hash(unsigned int val);

char *blf_dir_search(const char *file);
/**
 * Some font have additional file with metrics information,
 * in general, the extension of the file is: `.afm` or `.pfm`
 */
char *blf_dir_metrics_search(const char *filepath);
/* int blf_dir_split(const char *str, char *file, int *size); */ /* UNUSED */

int blf_font_init(void);
void blf_font_exit(void);

bool blf_font_id_is_valid(int fontid);

uint blf_get_char_index(struct FontBLF *font, uint charcode);

bool blf_ensure_face(struct FontBLF *font);
void blf_ensure_size(struct FontBLF *font);

void blf_draw_buffer__start(struct FontBLF *font);
void blf_draw_buffer__end(void);

struct FontBLF *blf_font_new_ex(const char *name,
                                const char *filepath,
                                const unsigned char *mem,
                                size_t mem_size,
                                void *ft_library);

struct FontBLF *blf_font_new(const char *name, const char *filepath);
struct FontBLF *blf_font_new_from_mem(const char *name, const unsigned char *mem, size_t mem_size);
void blf_font_attach_from_mem(struct FontBLF *font, const unsigned char *mem, size_t mem_size);

/**
 * Change font's output size. Returns true if successful in changing the size.
 */
bool blf_font_size(struct FontBLF *font, float size, unsigned int dpi);

void blf_font_draw(struct FontBLF *font,
                   const char *str,
                   size_t str_len,
                   struct ResultBLF *r_info);
void blf_font_draw__wrap(struct FontBLF *font,
                         const char *str,
                         size_t str_len,
                         struct ResultBLF *r_info);

/**
 * Use fixed column width, but an utf8 character may occupy multiple columns.
 */
int blf_font_draw_mono(struct FontBLF *font, const char *str, size_t str_len, int cwidth);
void blf_font_draw_buffer(struct FontBLF *font,
                          const char *str,
                          size_t str_len,
                          struct ResultBLF *r_info);
void blf_font_draw_buffer__wrap(struct FontBLF *font,
                                const char *str,
                                size_t str_len,
                                struct ResultBLF *r_info);
size_t blf_font_width_to_strlen(
    struct FontBLF *font, const char *str, size_t str_len, int width, int *r_width);
size_t blf_font_width_to_rstrlen(
    struct FontBLF *font, const char *str, size_t str_len, int width, int *r_width);
void blf_font_boundbox(struct FontBLF *font,
                       const char *str,
                       size_t str_len,
                       struct rcti *r_box,
                       struct ResultBLF *r_info);
void blf_font_boundbox__wrap(struct FontBLF *font,
                             const char *str,
                             size_t str_len,
                             struct rcti *r_box,
                             struct ResultBLF *r_info);
void blf_font_width_and_height(struct FontBLF *font,
                               const char *str,
                               size_t str_len,
                               float *r_width,
                               float *r_height,
                               struct ResultBLF *r_info);
float blf_font_width(struct FontBLF *font,
                     const char *str,
                     size_t str_len,
                     struct ResultBLF *r_info);
float blf_font_height(struct FontBLF *font,
                      const char *str,
                      size_t str_len,
                      struct ResultBLF *r_info);
float blf_font_fixed_width(struct FontBLF *font);
int blf_font_height_max(struct FontBLF *font);
int blf_font_width_max(struct FontBLF *font);
int blf_font_descender(struct FontBLF *font);
int blf_font_ascender(struct FontBLF *font);

char *blf_display_name(struct FontBLF *font);

void blf_font_boundbox_foreach_glyph(struct FontBLF *font,
                                     const char *str,
                                     size_t str_len,
                                     bool (*user_fn)(const char *str,
                                                     size_t str_step_ofs,
                                                     const struct rcti *glyph_step_bounds,
                                                     int glyph_advance_x,
                                                     const struct rcti *glyph_bounds,
                                                     const int glyph_bearing[2],
                                                     void *user_data),
                                     void *user_data,
                                     struct ResultBLF *r_info);

int blf_font_count_missing_chars(struct FontBLF *font,
                                 const char *str,
                                 size_t str_len,
                                 int *r_tot_chars);

void blf_font_free(struct FontBLF *font);

struct GlyphCacheBLF *blf_glyph_cache_acquire(struct FontBLF *font);
void blf_glyph_cache_release(struct FontBLF *font);
void blf_glyph_cache_clear(struct FontBLF *font);

/**
 * Create (or load from cache) a fully-rendered bitmap glyph.
 */
struct GlyphBLF *blf_glyph_ensure(struct FontBLF *font, struct GlyphCacheBLF *gc, uint charcode);

void blf_glyph_free(struct GlyphBLF *g);
void blf_glyph_draw(
    struct FontBLF *font, struct GlyphCacheBLF *gc, struct GlyphBLF *g, int x, int y);

#ifdef WIN32
/* blf_font_win32_compat.c */

#  ifdef FT_FREETYPE_H
extern FT_Error FT_New_Face__win32_compat(FT_Library library,
                                          const char *pathname,
                                          FT_Long face_index,
                                          FT_Face *aface);
#  endif
#endif
