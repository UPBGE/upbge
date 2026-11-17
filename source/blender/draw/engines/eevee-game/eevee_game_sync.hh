/* SPDX-FileCopyrightText: 2021 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_map.hh"
#include "DRW_render.hh"
#include "DNA_object_types.h"
#include "draw_handle.hh"

namespace blender::eevee_game {

using namespace draw;

class GameInstance;

/* -------------------------------------------------------------------- */
/** \name SyncModule
 *
 * Iterates the Depsgraph each frame and registers objects into the
 * appropriate eevee_game modules (culling, lights, velocity, shadows).
 *
 * Adapted from blender::eevee::SyncModule. The main difference is that
 * eevee_game has no material system yet — object geometry is submitted
 * directly to the GPU-Driven Culling module rather than through material
 * sub-passes.
 * \{ */

struct ObjectHandle {
  ObjectKey object_key;
  unsigned int recalc = 0;
};

class SyncModule {
 public:
  SyncModule(GameInstance &inst) : inst_(inst) {}

  /**
   * Called once per object by the Depsgraph iterator.
   * Routes the object to the correct sync helper based on its type.
   */
  void object_sync(const ObjectRef &ob_ref);

  /**
   * Retrieve (or create) a stable handle for this object.
   * The handle carries the recalc flags needed by VelocityModule.
   */
  ObjectHandle &sync_object(const ObjectRef &ob_ref);

  /** Clear per-frame handle data without discarding the persistent map. */
  void begin_sync();

  /** Called after all objects have been synced. Nothing to do here yet. */
  void end_sync() {}

 private:
  /** Register a mesh object into culling + velocity + shadows. */
  void sync_mesh(Object *ob, ObjectHandle &handle, const ObjectRef &ob_ref);

  /** Hair curves and point clouds: same passes as meshes, other geometry. */
  void sync_curves(Object *ob, ObjectHandle &handle, const ObjectRef &ob_ref);
  void sync_pointcloud(Object *ob, ObjectHandle &handle, const ObjectRef &ob_ref);
  /** Particle hair of a mesh, drawn with the object's resource handle. */
  void sync_hair_particles(Object *ob, const ObjectRef &ob_ref, ResourceHandleRange res_handle);

  /** Register a light object into LightModule. */
  void sync_light(Object *ob, ObjectHandle &handle);

  GameInstance &inst_;

  /** Persistent map: stable object identity → per-frame handle. */
  Map<ObjectKey, ObjectHandle> ob_handles_;
};

/** \} */

} // namespace blender::eevee_game
