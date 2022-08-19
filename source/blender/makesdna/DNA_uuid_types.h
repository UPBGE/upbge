/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup DNA
 */

#pragma once

#include "DNA_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \brief Universally Unique Identifier according to RFC4122.
 *
 * Cannot be named simply `UUID`, because Windows already defines that type.
 */
typedef struct bUUID {
  uint32_t time_low;
  uint16_t time_mid;
  uint16_t time_hi_and_version;
  uint8_t clock_seq_hi_and_reserved;
  uint8_t clock_seq_low;
  uint8_t node[6];
} bUUID;

/**
 * Memory required for a string representation of a UUID according to RFC4122.
 * This is 36 characters for the string + a trailing zero byte.
 */
#define UUID_STRING_LEN 37

#ifdef __cplusplus
}
#endif
