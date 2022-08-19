/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2020 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup bke
 */

#pragma once

#include <optional>
#include <string>

#include "BKE_cryptomatte.h"

#include "BLI_map.hh"
#include "BLI_string_ref.hh"

#include "BKE_cryptomatte.h"

struct ID;

namespace blender::bke::cryptomatte {

/**
 * Format to a cryptomatte meta data key.
 *
 * Cryptomatte stores meta data. The keys are formatted containing a hash that
 * is generated from its layer name.
 *
 * The output of this function is:
 * 'cryptomatte/{hash of layer_name}/{key_name}'.
 */
std::string BKE_cryptomatte_meta_data_key(const StringRef layer_name,
                                          const StringRefNull key_name);

/**
 * Extract the cryptomatte layer name from the given `render_pass_name`.
 *
 * Cryptomatte passes are formatted with a trailing number for storing multiple samples that belong
 * to the same cryptomatte layer. This function would remove the trailing numbers to determine the
 * cryptomatte layer name.
 *
 * # Example
 *
 * A render_pass_name could be 'View Layer.CryptoMaterial02'. The cryptomatte layer would be 'View
 * Layer.CryptoMaterial'.
 *
 * \note The return type is a sub-string of `render_pass_name` and therefore cannot outlive the
 * `render_pass_name` internal data.
 */
StringRef BKE_cryptomatte_extract_layer_name(const StringRef render_pass_name);

struct CryptomatteHash {
  uint32_t hash;

  CryptomatteHash(uint32_t hash);
  CryptomatteHash(const char *name, int name_len);
  static CryptomatteHash from_hex_encoded(blender::StringRef hex_encoded);

  std::string hex_encoded() const;
  /**
   * Convert a cryptomatte hash to a float.
   *
   * Cryptomatte hashes are stored in float textures and images. The conversion is taken from the
   * cryptomatte specification. See Floating point conversion section in
   * https://github.com/Psyop/Cryptomatte/blob/master/specification/cryptomatte_specification.pdf.
   *
   * The conversion uses as many 32 bit floating point values as possible to minimize hash
   * collisions. Unfortunately not all 32 bits can be used as NaN and Inf can be problematic.
   *
   * Note that this conversion assumes to be running on a L-endian system.
   */
  float float_encoded() const;
};

struct CryptomatteLayer {
  blender::Map<std::string, CryptomatteHash> hashes;

#ifdef WITH_CXX_GUARDEDALLOC
  MEM_CXX_CLASS_ALLOC_FUNCS("cryptomatte:CryptomatteLayer")
#endif

  static std::unique_ptr<CryptomatteLayer> read_from_manifest(blender::StringRefNull manifest);
  uint32_t add_ID(const struct ID &id);
  void add_hash(blender::StringRef name, CryptomatteHash cryptomatte_hash);
  std::string manifest() const;

  std::optional<std::string> operator[](float encoded_hash) const;
};

struct CryptomatteStampDataCallbackData {
  struct CryptomatteSession *session;
  blender::Map<std::string, std::string> hash_to_layer_name;

  /**
   * Extract the hash from a stamp data key.
   *
   * Cryptomatte keys are formatted as "cryptomatte/{layer_hash}/{attribute}".
   */
  static blender::StringRef extract_layer_hash(blender::StringRefNull key);

  /* C type callback function (StampCallback). */
  static void extract_layer_names(void *_data, const char *propname, char *propvalue, int len);
  /* C type callback function (StampCallback). */
  static void extract_layer_manifest(void *_data, const char *propname, char *propvalue, int len);
};

const blender::Vector<std::string> &BKE_cryptomatte_layer_names_get(
    const CryptomatteSession &session);

struct CryptomatteSessionDeleter {
  void operator()(CryptomatteSession *session)
  {
    BKE_cryptomatte_free(session);
  }
};

using CryptomatteSessionPtr = std::unique_ptr<CryptomatteSession, CryptomatteSessionDeleter>;

}  // namespace blender::bke::cryptomatte
