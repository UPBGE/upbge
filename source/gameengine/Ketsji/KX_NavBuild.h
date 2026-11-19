/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file KX_NavBuild.h
 *  \ingroup ketsji
 *  The scene's own navmeshes, built while the game runs from the static collision shapes
 *  (Scene > Navigation Mesh's settings), one for each agent size, kept up to date: obstacles
 *  carved while they stand still, tiles rebuilt where level pieces change, and (Around NPCs) tiles
 *  made only near the agents. See KX_NavBuild.cpp.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "KX_Navigation.h"
#include "MT_Vector3.h"

class KX_GameObject;
class KX_NavMesh;
class KX_Scene;
class LN_JobPool;
class PHY_IPhysicsEnvironment;

/** Triangles of the level in the game's space (Z up), with an area id each (0: ground). */
struct KX_NavGeometry {
  std::vector<float> verts;
  std::vector<int> tris;
  std::vector<uint8_t> areas;
};

/** Where the level's triangles come from: the physics' static bodies (KX_Scene), or a test. */
using KX_NavGeometrySource = std::function<void(
    const MT_Vector3 &aabb_min, const MT_Vector3 &aabb_max, KX_NavGeometry &r_geometry)>;

/** An obstacle for carving: an oriented box in the game's space. */
struct KX_NavCarveObstacle {
  const KX_GameObject *object = nullptr;
  MT_Vector3 center;
  MT_Vector3 half_size;
  /** Turn around the up axis (radians). */
  float yaw = 0.0f;
};

/** One agent size's navmesh: its build settings and tiles. */
struct KX_NavBuildState;

KX_NavBuildState *KX_NavBuildCreate(const KX_Navigation::Settings &settings,
                                     KX_NavGeometrySource source);
void KX_NavBuildFree(KX_NavBuildState *state);
/** Each navmesh built so far. */
void KX_NavBuildForEachNavMesh(const KX_NavBuildState &state,
                               const std::function<void(const KX_NavMesh &)> &navmesh);

/** The level's triangles in a box: the static, solid bodies of the scene's physics, each with
 * its object's area (the `nav_area` game property, through `area_id`); objects that are
 * obstacles left out (they are carved instead). */
void KX_NavCollectSceneGeometry(KX_Scene *scene,
                                const MT_Vector3 &aabb_min,
                                const MT_Vector3 &aabb_max,
                                const std::function<int(const std::string &)> &area_id,
                                const std::function<bool(const KX_GameObject *)> &is_obstacle,
                                KX_NavGeometry &r_geometry);
/** The same from a physics environment (the scene's; tests give a world of their own). */
void KX_NavCollectPhysicsGeometry(PHY_IPhysicsEnvironment *physics,
                                  const MT_Vector3 &aabb_min,
                                  const MT_Vector3 &aabb_max,
                                  const std::function<int(const std::string &)> &area_id,
                                  const std::function<bool(const KX_GameObject *)> &is_obstacle,
                                  KX_NavGeometry &r_geometry);
/** The scene's navigation links: objects with a `nav_link` game property naming the object at
 * the link's other end (`nav_link_kind`, `nav_link_both_ways`, `nav_link_radius`,
 * `nav_link_duration` optional). */
std::vector<KX_NavLink> KX_NavCollectLinks(KX_Scene *scene);

/** The navmesh for agents of this radius (the size class it falls in), made on first use; null
 * when building is off. */
KX_NavMesh *KX_NavBuildNavMesh(KX_NavBuildState &state, float agent_radius);
/** The navmesh of that size class if it was made (any thread: nothing is made). */
const KX_NavMesh *KX_NavBuildFindNavMesh(const KX_NavBuildState &state, float agent_radius);

/** Once a step: builds what is due (whole level at the first step, tiles near `agents` in Around
 * NPCs mode, a fixed number a step), rebuilds dirty tiles, carves `obstacles` (those standing
 * still). Returns the navmeshes whose tiles changed. */
void KX_NavBuildUpdate(KX_NavBuildState &state,
                       const std::vector<MT_Vector3> &agent_positions,
                       const std::vector<float> &agent_radii,
                       const std::vector<KX_NavCarveObstacle> &obstacles,
                       const std::vector<KX_NavLink> &links,
                       LN_JobPool *pool);

/** Marks the tiles touching a box for rebuilding. */
void KX_NavBuildMarkDirty(KX_NavBuildState &state,
                          const MT_Vector3 &aabb_min,
                          const MT_Vector3 &aabb_max);

/** Tests: tiles built so far, tiles alive, obstacles carved. */
struct KX_NavBuildStats {
  uint64_t tiles_built = 0;
  uint64_t tiles_removed = 0;
  uint64_t tiles_restored = 0;
  size_t tiles_alive = 0;
  size_t obstacles_carved = 0;
  size_t size_classes = 0;
};
KX_NavBuildStats KX_NavBuildGetStats(const KX_NavBuildState &state);
