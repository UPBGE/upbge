/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup DNA
 */

#pragma once

/* Struct members on own line. */
/* clang-format off */

/* -------------------------------------------------------------------- */
/** \name Curve Struct
 * \{ */

#define _DNA_DEFAULT_Curve \
  { \
    .size = {1, 1, 1}, \
    .flag = CU_DEFORM_BOUNDS_OFF | CU_PATH_RADIUS, \
    .pathlen = 100, \
    .resolu = 12, \
    .resolv = 12, \
    .offset = 1.0, \
    .wordspace = 1.0, \
    .spacing = 1.0f, \
    .linedist = 1.0, \
    .fsize = 1.0, \
    .ulheight = 0.05, \
    .texflag = CU_AUTOSPACE, \
    .smallcaps_scale = 0.75f, \
    /* This one seems to be the best one in most cases, at least for curve deform. */ \
    .twist_mode = CU_TWIST_MINIMUM, \
    .bevfac1 = 0.0f, \
    .bevfac2 = 1.0f, \
    .bevfac1_mapping = CU_BEVFAC_MAP_RESOLU, \
    .bevfac2_mapping = CU_BEVFAC_MAP_RESOLU, \
    .bevresol = 4, \
    .bevel_mode = CU_BEV_MODE_ROUND, \
    .taper_radius_mode = CU_TAPER_RADIUS_OVERRIDE, \
  }

/** \} */

/* clang-format on */
