/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bli
 */

#include "BLI_math_inline.h"

/* -------------------------------------------------------------------- */
/** \name Defines
 * \{ */

/* YCbCr */
#define BLI_YCC_ITU_BT601 0
#define BLI_YCC_ITU_BT709 1
#define BLI_YCC_JFIF_0_255 2

/* YUV */
#define BLI_YUV_ITU_BT601 0
#define BLI_YUV_ITU_BT709 1

/** \} */

/* -------------------------------------------------------------------- */
/** \name Conversion to RGB
 * \{ */

void hsv_to_rgb(float h, float s, float v, float *r_r, float *r_g, float *r_b);
void hsv_to_rgb_v(const float hsv[3], float r_rgb[3]);
void hsl_to_rgb(float h, float s, float l, float *r_r, float *r_g, float *r_b);
void hsl_to_rgb_v(const float hsl[3], float r_rgb[3]);
void hex_to_rgb(const char *hexcol, float *r_r, float *r_g, float *r_b);
void yuv_to_rgb(float y, float u, float v, float *r_r, float *r_g, float *r_b, int colorspace);
void ycc_to_rgb(float y, float cb, float cr, float *r_r, float *r_g, float *r_b, int colorspace);
void cpack_to_rgb(unsigned int col, float *r_r, float *r_g, float *r_b);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Conversion to RGBA
 * \{ */

void hex_to_rgba(const char *hexcol, float *r_r, float *r_g, float *r_b, float *r_a);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Conversion from RGB
 * \{ */

void rgb_to_yuv(float r, float g, float b, float *r_y, float *r_u, float *r_v, int colorspace);
/**
 * The RGB inputs are supposed gamma corrected and in the range 0 - 1.0f
 *
 * Output YCC have a range of 16-235 and 16-240 except with JFIF_0_255 where the range is 0-255.
 */
void rgb_to_ycc(float r, float g, float b, float *r_y, float *r_cb, float *r_cr, int colorspace);
void rgb_to_hsv(float r, float g, float b, float *r_h, float *r_s, float *r_v);
void rgb_to_hsv_v(const float rgb[3], float r_hsv[3]);
void rgb_to_hsl(float r, float g, float b, float *r_h, float *r_s, float *r_l);
void rgb_to_hsl_v(const float rgb[3], float r_hsl[3]);
void rgb_to_hsl_compat(float r, float g, float b, float *r_h, float *r_s, float *r_l);
void rgb_to_hsl_compat_v(const float rgb[3], float r_hsl[3]);
void rgb_to_hsv_compat(float r, float g, float b, float *r_h, float *r_s, float *r_v);
void rgb_to_hsv_compat_v(const float rgb[3], float r_hsv[3]);
unsigned int rgb_to_cpack(float r, float g, float b);
/**
 * We define a 'cpack' here as a (3 byte color code)
 * number that can be expressed like 0xFFAA66 or so.
 * For that reason it is sensitive for endianness... with this function it works correctly.
 * \see #imm_cpack
 */
unsigned int hsv_to_cpack(float h, float s, float v);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Profile Transformations
 * \{ */

float srgb_to_linearrgb(float c);
float linearrgb_to_srgb(float c);

void srgb_to_linearrgb_v3_v3(float linear[3], const float srgb[3]);
void linearrgb_to_srgb_v3_v3(float srgb[3], const float linear[3]);

MINLINE void srgb_to_linearrgb_v4(float linear[4], const float srgb[4]);
MINLINE void linearrgb_to_srgb_v4(float srgb[4], const float linear[4]);

MINLINE void srgb_to_linearrgb_predivide_v4(float linear[4], const float srgb[4]);
MINLINE void linearrgb_to_srgb_predivide_v4(float srgb[4], const float linear[4]);

MINLINE unsigned short to_srgb_table_lookup(float f);
MINLINE void linearrgb_to_srgb_ushort4(unsigned short srgb[4], const float linear[4]);
MINLINE void srgb_to_linearrgb_uchar4(float linear[4], const unsigned char srgb[4]);
MINLINE void srgb_to_linearrgb_uchar4_predivide(float linear[4], const unsigned char srgb[4]);

MINLINE void linearrgb_to_srgb_uchar3(unsigned char srgb[3], const float linear[3]);
MINLINE void linearrgb_to_srgb_uchar4(unsigned char srgb[4], const float linear[4]);

void BLI_init_srgb_conversion(void);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Alpha Transformations
 * \{ */

MINLINE void premul_to_straight_v4_v4(float straight[4], const float premul[4]);
MINLINE void premul_to_straight_v4(float color[4]);
MINLINE void straight_to_premul_v4_v4(float premul[4], const float straight[4]);
MINLINE void straight_to_premul_v4(float color[4]);
MINLINE void straight_uchar_to_premul_float(float result[4], const unsigned char color[4]);
MINLINE void premul_float_to_straight_uchar(unsigned char *result, const float color[4]);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Other
 * \{ */

/**
 * Clamp `hsv` to usable values.
 */
void hsv_clamp_v(float hsv[3], float v_max);

/**
 * Applies an HUE offset to a float RGB color.
 */
void rgb_float_set_hue_float_offset(float rgb[3], float hue_offset);
/**
 * Applies an HUE offset to a byte RGB color.
 */
void rgb_byte_set_hue_float_offset(unsigned char rgb[3], float hue_offset);

MINLINE void rgb_uchar_to_float(float r_col[3], const unsigned char col_ub[3]);
MINLINE void rgba_uchar_to_float(float r_col[4], const unsigned char col_ub[4]);
MINLINE void rgb_float_to_uchar(unsigned char r_col[3], const float col_f[3]);
MINLINE void rgba_float_to_uchar(unsigned char r_col[4], const float col_f[4]);

/**
 * Compute luminance using Rec.709 primaries, for sRGB and linear Rec.709.
 *
 * Only use for colors known to be in sRGB space, like user interface and themes.
 * Scene colors should use #IMB_colormanagement_get_luminance instead.
 */
MINLINE float srgb_to_grayscale(const float rgb[3]);
MINLINE unsigned char srgb_to_grayscale_byte(const unsigned char rgb[3]);

MINLINE int compare_rgb_uchar(const unsigned char col_a[3],
                              const unsigned char col_b[3],
                              int limit);

/**
 * Returns triangle noise in [-1..+1) range, given integer pixel coordinates.
 * Triangle distribution which gives a more final uniform noise,
 * see "Banding in Games: A Noisy Rant" by Mikkel Gjoel (slide 27)
 * https://loopit.dk/banding_in_games.pdf
 */
MINLINE float dither_random_value(int x, int y);
MINLINE void float_to_byte_dither_v3(
    unsigned char b[3], const float f[3], float dither, int x, int y);

#define rgba_char_args_set_fl(col, r, g, b, a) \
  rgba_char_args_set(col, (r) * 255, (g) * 255, (b) * 255, (a) * 255)

#define rgba_float_args_set_ch(col, r, g, b, a) \
  rgba_float_args_set(col, (r) / 255.0f, (g) / 255.0f, (b) / 255.0f, (a) / 255.0f)

MINLINE void rgba_uchar_args_set(
    unsigned char col[4], unsigned char r, unsigned char g, unsigned char b, unsigned char a);
MINLINE void rgba_float_args_set(float col[4], float r, float g, float b, float a);
MINLINE void rgba_uchar_args_test_set(
    unsigned char col[4], unsigned char r, unsigned char g, unsigned char b, unsigned char a);
MINLINE void cpack_cpy_3ub(unsigned char r_col[3], unsigned int pack);

/** \} */

#if BLI_MATH_DO_INLINE
#  include "intern/math_color_inline.cc"
#endif
