/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file BL_StaticBatch.h
 *  \ingroup bgeconv
 *
 * Static batching: at scene conversion, the meshes flagged "Static Batch"
 * (Object.game.use_static_batch) are merged into one object per material set and per cell of a
 * world grid (Scene.eevee.game_batch_cell_size). Rendering cost is per object, not per triangle,
 * so thousands of static props collapse into a few draws; the cells keep them cullable.
 *
 * The flagged objects stay game objects, with their logic and physics, but are not drawn: the
 * batches draw them. Their transform and visibility are not followed once merged, and their
 * texture coordinates in object or generated space become those of the batch.
 */

#pragma once

#include <vector>

namespace blender {
struct Depsgraph;
struct Main;
struct Object;
struct Scene;
}  // namespace blender

struct BL_StaticBatchResult {
  /** Created batch objects, owned by the scene until it is freed. */
  std::vector<blender::Object *> batches;
  /** Merged objects, hidden once converted. */
  std::vector<blender::Object *> merged;
};

/** Builds the batches of the flagged objects of the scene and links them to its master
 * collection. Groups of a single object are left as they are. */
BL_StaticBatchResult BL_BuildStaticBatches(blender::Main *bmain,
                                           blender::Scene *scene,
                                           blender::Depsgraph *depsgraph);

/** Unlinks and frees the batch objects and their meshes. */
void BL_FreeStaticBatches(blender::Main *bmain,
                          blender::Scene *scene,
                          std::vector<blender::Object *> &batches);
