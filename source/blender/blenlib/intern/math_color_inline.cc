/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 */

#include "BLI_math_base.h"
#include "BLI_math_color.h"

#include <cmath>

#ifndef __MATH_COLOR_INLINE_C__
#  define __MATH_COLOR_INLINE_C__

/******************************** Color Space ********************************/

MINLINE void srgb_to_linearrgb_v4(float linear[4], const float srgb[4])
{
  srgb_to_linearrgb_v3_v3(linear, srgb);
  linear[3] = srgb[3];
}

MINLINE void linearrgb_to_srgb_v4(float srgb[4], const float linear[4])
{
  linearrgb_to_srgb_v3_v3(srgb, linear);
  srgb[3] = linear[3];
}

MINLINE void linearrgb_to_srgb_uchar3(unsigned char srgb[3], const float linear[3])
{
  float srgb_f[3];

  linearrgb_to_srgb_v3_v3(srgb_f, linear);
  unit_float_to_uchar_clamp_v3(srgb, srgb_f);
}

MINLINE void linearrgb_to_srgb_uchar4(unsigned char srgb[4], const float linear[4])
{
  float srgb_f[4];

  linearrgb_to_srgb_v4(srgb_f, linear);
  unit_float_to_uchar_clamp_v4(srgb, srgb_f);
}

/* predivide versions to work on associated/pre-multiplied alpha. if this should
 * be done or not depends on the background the image will be composited over,
 * ideally you would never do color space conversion on an image with alpha
 * because it is ill defined */

MINLINE void srgb_to_linearrgb_predivide_v4(float linear[4], const float srgb[4])
{
  float alpha, inv_alpha;

  if (srgb[3] == 1.0f || srgb[3] == 0.0f) {
    alpha = 1.0f;
    inv_alpha = 1.0f;
  }
  else {
    alpha = srgb[3];
    inv_alpha = 1.0f / alpha;
  }

  linear[0] = srgb[0] * inv_alpha;
  linear[1] = srgb[1] * inv_alpha;
  linear[2] = srgb[2] * inv_alpha;
  linear[3] = srgb[3];
  srgb_to_linearrgb_v3_v3(linear, linear);
  linear[0] *= alpha;
  linear[1] *= alpha;
  linear[2] *= alpha;
}

MINLINE void linearrgb_to_srgb_predivide_v4(float srgb[4], const float linear[4])
{
  float alpha, inv_alpha;

  if (linear[3] == 1.0f || linear[3] == 0.0f) {
    alpha = 1.0f;
    inv_alpha = 1.0f;
  }
  else {
    alpha = linear[3];
    inv_alpha = 1.0f / alpha;
  }

  srgb[0] = linear[0] * inv_alpha;
  srgb[1] = linear[1] * inv_alpha;
  srgb[2] = linear[2] * inv_alpha;
  srgb[3] = linear[3];
  linearrgb_to_srgb_v3_v3(srgb, srgb);
  srgb[0] *= alpha;
  srgb[1] *= alpha;
  srgb[2] *= alpha;
}

/* LUT accelerated conversions */

extern float BLI_color_from_srgb_table[256];
extern unsigned short BLI_color_to_srgb_table[0x10000];

MINLINE unsigned short to_srgb_table_lookup(const float f)
{

  union {
    float f;
    unsigned short us[2];
  } tmp;
  tmp.f = f;
  /* NOTE: this is endianness-sensitive. */
  return BLI_color_to_srgb_table[tmp.us[1]];
}

MINLINE void linearrgb_to_srgb_ushort4(unsigned short srgb[4], const float linear[4])
{
  srgb[0] = to_srgb_table_lookup(linear[0]);
  srgb[1] = to_srgb_table_lookup(linear[1]);
  srgb[2] = to_srgb_table_lookup(linear[2]);
  srgb[3] = unit_float_to_ushort_clamp(linear[3]);
}

MINLINE void srgb_to_linearrgb_uchar4(float linear[4], const unsigned char srgb[4])
{
  linear[0] = BLI_color_from_srgb_table[srgb[0]];
  linear[1] = BLI_color_from_srgb_table[srgb[1]];
  linear[2] = BLI_color_from_srgb_table[srgb[2]];
  linear[3] = srgb[3] * (1.0f / 255.0f);
}

MINLINE void srgb_to_linearrgb_uchar4_predivide(float linear[4], const unsigned char srgb[4])
{
  float fsrgb[4];
  int i;

  if (srgb[3] == 255 || srgb[3] == 0) {
    srgb_to_linearrgb_uchar4(linear, srgb);
    return;
  }

  for (i = 0; i < 4; i++) {
    fsrgb[i] = srgb[i] * (1.0f / 255.0f);
  }

  srgb_to_linearrgb_predivide_v4(linear, fsrgb);
}

MINLINE void rgb_uchar_to_float(float r_col[3], const uchar col_ub[3])
{
  r_col[0] = float(col_ub[0]) * (1.0f / 255.0f);
  r_col[1] = float(col_ub[1]) * (1.0f / 255.0f);
  r_col[2] = float(col_ub[2]) * (1.0f / 255.0f);
}

MINLINE void rgba_uchar_to_float(float r_col[4], const uchar col_ub[4])
{
  r_col[0] = float(col_ub[0]) * (1.0f / 255.0f);
  r_col[1] = float(col_ub[1]) * (1.0f / 255.0f);
  r_col[2] = float(col_ub[2]) * (1.0f / 255.0f);
  r_col[3] = float(col_ub[3]) * (1.0f / 255.0f);
}

MINLINE void rgb_float_to_uchar(uchar r_col[3], const float col_f[3])
{
  unit_float_to_uchar_clamp_v3(r_col, col_f);
}

MINLINE void rgba_float_to_uchar(uchar r_col[4], const float col_f[4])
{
  unit_float_to_uchar_clamp_v4(r_col, col_f);
}

MINLINE void rgba_uchar_args_set(
    uchar col[4], const uchar r, const uchar g, const uchar b, const uchar a)
{
  col[0] = r;
  col[1] = g;
  col[2] = b;
  col[3] = a;
}

MINLINE void rgba_float_args_set(
    float col[4], const float r, const float g, const float b, const float a)
{
  col[0] = r;
  col[1] = g;
  col[2] = b;
  col[3] = a;
}

MINLINE void rgba_uchar_args_test_set(
    uchar col[4], const uchar r, const uchar g, const uchar b, const uchar a)
{
  if (col[3] == 0) {
    col[0] = r;
    col[1] = g;
    col[2] = b;
    col[3] = a;
  }
}

MINLINE void cpack_cpy_3ub(unsigned char r_col[3], const unsigned int pack)
{
  r_col[0] = ((pack) >> 0) & 0xFF;
  r_col[1] = ((pack) >> 8) & 0xFF;
  r_col[2] = ((pack) >> 16) & 0xFF;
}

/* -------------------------------------------------------------------- */
/** \name sRGB/Gray-Scale Functions
 *
 * \warning
 * Only use for colors known to be in sRGB space, like user interface and themes.
 * Scene color should use #IMB_colormanagement_get_luminance instead.
 *
 * \{ */

MINLINE float srgb_to_grayscale(const float rgb[3])
{
  /* Real values are:
   * `Y = 0.2126390059(R) + 0.7151686788(G) + 0.0721923154(B)`
   * according to: "Derivation of Basic Television Color Equations", RP 177-1993
   *
   * As this sums slightly above 1.0, the document recommends to use:
   * `0.2126(R) + 0.7152(G) + 0.0722(B)`, as used here. */
  return (0.2126f * rgb[0]) + (0.7152f * rgb[1]) + (0.0722f * rgb[2]);
}

MINLINE unsigned char srgb_to_grayscale_byte(const unsigned char rgb[3])
{
  /* The high precision values are used to calculate the rounded byte weights so they add up to
   * 255: `54(R) + 182(G) + 19(B)` */
  return (unsigned char)(((54 * (unsigned short)rgb[0]) + (182 * (unsigned short)rgb[1]) +
                          (19 * (unsigned short)rgb[2])) /
                         255);
}

/** \} */

MINLINE int compare_rgb_uchar(const unsigned char col_a[3],
                              const unsigned char col_b[3],
                              const int limit)
{
  const int r = (int)col_a[0] - (int)col_b[0];
  if (abs(r) < limit) {
    const int g = (int)col_a[1] - (int)col_b[1];
    if (abs(g) < limit) {
      const int b = (int)col_a[2] - (int)col_b[2];
      if (abs(b) < limit) {
        return 1;
      }
    }
  }

  return 0;
}

/* 2D hash (iqint3) recommended from "Hash Functions for GPU Rendering" JCGT Vol. 9, No. 3, 2020
 * https://jcgt.org/published/0009/03/02/ */
MINLINE float hash_iqint3_f(const uint32_t x, const uint32_t y)
{
  const uint32_t qx = 1103515245u * ((x >> 1u) ^ (y));
  const uint32_t qy = 1103515245u * ((y >> 1u) ^ (x));
  const uint32_t n = 1103515245u * ((qx) ^ (qy >> 3u));
  return float(n) * (1.0f / float(0xffffffffu));
}

MINLINE float dither_random_value(int x, int y)
{
  float v = hash_iqint3_f(x, y);
  /* Convert uniform distribution into triangle-shaped distribution. Based on
   * "remap_pdf_tri_unity" from https://www.shadertoy.com/view/WldSRf */
  v = v * 2.0f - 1.0f;
  v = signf(v) * (1.0f - sqrtf(1.0f - fabsf(v)));
  return v;
}

MINLINE void float_to_byte_dither_v3(
    unsigned char b[3], const float f[3], float dither, int x, int y)
{
  float dither_value = dither_random_value(x, y) * 0.0033f * dither;

  b[0] = unit_float_to_uchar_clamp(dither_value + f[0]);
  b[1] = unit_float_to_uchar_clamp(dither_value + f[1]);
  b[2] = unit_float_to_uchar_clamp(dither_value + f[2]);
}

/**************** Alpha Transformations *****************/

MINLINE void premul_to_straight_v4_v4(float straight[4], const float premul[4])
{
  if (premul[3] == 0.0f || premul[3] == 1.0f) {
    straight[0] = premul[0];
    straight[1] = premul[1];
    straight[2] = premul[2];
    straight[3] = premul[3];
  }
  else {
    const float alpha_inv = 1.0f / premul[3];
    straight[0] = premul[0] * alpha_inv;
    straight[1] = premul[1] * alpha_inv;
    straight[2] = premul[2] * alpha_inv;
    straight[3] = premul[3];
  }
}

MINLINE void premul_to_straight_v4(float color[4])
{
  premul_to_straight_v4_v4(color, color);
}

MINLINE void straight_to_premul_v4_v4(float premul[4], const float straight[4])
{
  const float alpha = straight[3];
  premul[0] = straight[0] * alpha;
  premul[1] = straight[1] * alpha;
  premul[2] = straight[2] * alpha;
  premul[3] = straight[3];
}

MINLINE void straight_to_premul_v4(float color[4])
{
  straight_to_premul_v4_v4(color, color);
}

MINLINE void straight_uchar_to_premul_float(float result[4], const unsigned char color[4])
{
  const float alpha = color[3] * (1.0f / 255.0f);
  const float fac = alpha * (1.0f / 255.0f);

  result[0] = color[0] * fac;
  result[1] = color[1] * fac;
  result[2] = color[2] * fac;
  result[3] = alpha;
}

MINLINE void premul_float_to_straight_uchar(unsigned char *result, const float color[4])
{
  if (color[3] == 0.0f || color[3] == 1.0f) {
    result[0] = unit_float_to_uchar_clamp(color[0]);
    result[1] = unit_float_to_uchar_clamp(color[1]);
    result[2] = unit_float_to_uchar_clamp(color[2]);
    result[3] = unit_float_to_uchar_clamp(color[3]);
  }
  else {
    const float alpha_inv = 1.0f / color[3];

    /* hopefully this would be optimized */
    result[0] = unit_float_to_uchar_clamp(color[0] * alpha_inv);
    result[1] = unit_float_to_uchar_clamp(color[1] * alpha_inv);
    result[2] = unit_float_to_uchar_clamp(color[2] * alpha_inv);
    result[3] = unit_float_to_uchar_clamp(color[3]);
  }
}

#endif /* !__MATH_COLOR_INLINE_C__ */
