/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup DNA
 */

#pragma once

/* Struct members on own line. */
/* clang-format off */

/* -------------------------------------------------------------------- */
/** \name Light Struct
 * \{ */

#define _DNA_DEFAULT_Light \
  { \
    .r = 1.0f, \
    .g = 1.0f, \
    .b = 1.0f, \
    .k = 1.0f, \
    .energy = 10.0f, \
    .dist = 25.0f, \
    .spotsize = DEG2RADF(45.0f), \
    .spotblend = 0.15f, \
    .att2 = 1.0f, \
    .mode = LA_SHADOW | LA_SOFT_SHADOWS, \
    .bufsize = 512, \
    .clipsta = 0.05f, \
    .clipend = 40.0f, \
    .samp = 3, \
    .bias = 1.0f, \
    .area_size = 0.25f, \
    .area_sizey = 0.25f, \
    .area_sizez = 0.25f, \
    .buffers = 1, \
    .preview = NULL, \
    .falloff_type = LA_FALLOFF_INVSQUARE, \
    .coeff_const = 1.0f, \
    .coeff_lin = 0.0f, \
    .coeff_quad = 0.0f, \
    .cascade_max_dist = 200.0f, \
    .cascade_count = 4, \
    .cascade_exponent = 0.8f, \
    .cascade_fade = 0.1f, \
    .contact_dist = 0.2f, \
    .contact_bias = 0.03f, \
    .contact_thickness = 0.2f, \
    .diff_fac = 1.0f, \
    .spec_fac = 1.0f, \
    .volume_fac = 1.0f, \
    .att_dist = 40.0f, \
    .sun_angle = DEG2RADF(0.526f), \
    .area_spread = DEG2RADF(180.0f), \
  }

/** \} */

/* clang-format on */
