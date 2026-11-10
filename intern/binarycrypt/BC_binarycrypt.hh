/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup intern_binarycrypt
 *
 * Player side of BinaryCrypt: receives the key from the launcher and exposes encrypted containers
 * (.blend files, Python modules, data files) as ordinary #FileReader streams. Decryption only;
 * containers are sealed by the external UPBGE-BinaryCrypt tool.
 */

#pragma once

#include <cstddef>

namespace blender {
struct FileReader;
}

/** What a container holds (#BC_KIND_BLEND... in binarycrypt_format.h). Each reader opens its own
 * kind only, so no script API can be handed a scene or a module's source. */
enum class BCKind {
  Blend = 0,
  Python = 1,
  Data = 2,
};

/**
 * Receive the key from the launcher through the inherited pipe named by `UPBGE_BC_KEY_PIPE`
 * (see binarycrypt_format.h). Call once at player startup, before Python starts and before any
 * file is read. Without the variable this is a no-op: the game is simply not protected.
 * \return true when a key was received.
 */
bool BC_runtime_init_from_launcher();

/** True when a key is loaded, i.e. this player process runs a protected game. */
bool BC_runtime_has_key();

/** True once any encrypted container has been opened by this process. Thread-safe. */
bool BC_runtime_is_tainted();

/** True when \a first_bytes (at least 7 bytes) starts a BinaryCrypt container. */
bool BC_file_magic_is_container(const char *first_bytes);

/**
 * Wrap the raw stream of a container into a reader that yields the decrypted .blend bytes.
 * Takes ownership of \a base in every case (it is closed on failure).
 * \return nullptr when no key is loaded, the file was sealed with another key, holds another kind
 * of content or the container is malformed. The reason is printed to stderr.
 */
blender::FileReader *BC_filereader_new(blender::FileReader *base);

/**
 * #BC_filereader_new for a container of \a kind, silent: on failure \a r_error receives the reason
 * (a static string). Takes ownership of \a base in every case.
 */
blender::FileReader *BC_filereader_open(blender::FileReader *base, BCKind kind, const char **r_error);

/** Overwrite \a size bytes at \a data with zeros (decrypted data a caller is done with), in a way
 * the compiler cannot remove. */
void BC_wipe(void *data, size_t size);
