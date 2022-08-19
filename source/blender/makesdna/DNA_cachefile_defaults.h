/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup DNA
 */

#pragma once

/* Struct members on own line. */
/* clang-format off */

/* -------------------------------------------------------------------- */
/** \name CacheFile Struct
 * \{ */

#define _DNA_DEFAULT_CacheFile \
  { \
    .filepath[0] = '\0', \
    .override_frame = false, \
    .frame = 0.0f, \
    .is_sequence = false, \
    .scale = 1.0f, \
    .object_paths ={NULL, NULL}, \
 \
    .type = 0, \
    .handle = NULL, \
    .handle_filepath[0] = '\0', \
    .handle_readers = NULL, \
    .use_prefetch = 1, \
    .prefetch_cache_size = 4096, \
  }

/** \} */

/* clang-format on */
