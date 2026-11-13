/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup intern_binarycrypt
 *
 * Internal access to the runtime key state, shared by the reader.
 */

#pragma once

#include <cstdint>

#include "binarycrypt_format.h"

/** The handed over key whose #bc_key_id is \a key_id, or null. */
const uint8_t *bc_runtime_find_key(const uint8_t key_id[BC_KEY_ID_SIZE]);

/** Record that decrypted data now lives in this process. */
void bc_runtime_mark_tainted();
