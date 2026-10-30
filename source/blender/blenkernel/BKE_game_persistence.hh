/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <cstdint>

#include "DNA_uuid_types.h"

namespace blender {
struct ID;
struct Main;
struct Object;
struct Scene;
}

enum class BKE_GamePersistenceIdentityState : uint8_t {
  Missing = 0,
  Valid,
  Invalid,
};

/**
 * Read a durable game-persistence identity from an ID's hidden system properties.
 *
 * Missing metadata is distinct from malformed metadata so callers can fail closed instead of
 * silently replacing an identity that may already be referenced by Save Game files. `r_uuid` is
 * always set to nil unless a valid canonical non-nil UUID is returned.
 */
BKE_GamePersistenceIdentityState BKE_game_persistence_identity_read(
    const blender::ID &id, blender::bUUID &r_uuid);

/** Replace an editable local ID's durable identity with a newly generated random UUID. */
bool BKE_game_persistence_identity_renew(blender::ID &id);

/**
 * Restore the source identity on an internal/runtime copy after Blender's regular copy policy.
 */
bool BKE_game_persistence_identity_copy(const blender::ID &source, blender::ID &destination);

/**
 * Apply the authored-vs-internal identity policy after an ID's system properties were copied.
 *
 * Regular local datablock duplication receives a new identity. Evaluated, localized, linked, and
 * library-override transfer copies preserve the copied source identity. This is intentionally
 * called only by Object and Scene copy paths.
 */
void BKE_game_persistence_identity_handle_copy(const blender::ID &source,
                                               blender::ID &destination,
                                               int copy_flags);

/** Renew durable identities on new local Object/Scene IDs while clipboard pre-existing tags live. */
void BKE_game_persistence_identities_on_copybuffer_paste(blender::Main &bmain);

/**
 * Ensure durable checkpoint identities for one explicit persistence authoring edit.
 *
 * Linked data is intentionally read-only. A local object must belong to the selected scene; when
 * the selected scene is null or does not contain it, the first local owning scene is used.
 */
bool BKE_game_persistence_ensure(blender::Main &bmain,
                                 blender::Scene *selected_scene,
                                 blender::Object &object);
