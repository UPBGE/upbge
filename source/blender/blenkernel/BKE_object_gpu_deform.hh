/* SPDX-FileCopyrightText: 2025 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

namespace blender {

struct bContext;
struct Object; /* blender::Object via DNA_object_types.h */

/**
 * Advance the depsgraph update counter for GPU modifiers if any of the
 * object's modifiers needs evaluated cage/object data that is updated by
 * actions (e.g. Mesh Deform cage animated directly, not via armature).
 *
 * This is called from the game engine animation loop when an action is
 * evaluated on `ob`.  It is intentionally lightweight: it only calls
 * DEG_bump_update_count if a GPU Mesh Deform modifier depends on a cage
 * that is not driven by an armature.
 */
void BKE_object_gpu_deform_bump_update_if_needed(Object *ob, bContext *C);

}  // namespace blender