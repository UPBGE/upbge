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
#include <unordered_map>
#include <vector>

#include "KX_Navigation.h"
#include "MT_Vector3.h"

class KX_GameObject;
class KX_NavMesh;
class KX_Scene;
class LN_JobPool;
class PHY_IPhysicsEnvironment;
struct KX_NavChangedPlace;

/** Triangles of the level in the game's space (Z up), with an area id each (0: ground). */
struct KX_NavGeometry {
  std::vector<float> verts;
  std::vector<int> tris;
  std::vector<uint8_t> areas;
  /** Per triangle: the closed convex piece it belongs to (filled solid in the build: the floor
   * inside a box is no floor), -1 for none; empty: none at all. */
  std::vector<int32_t> pieces;
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
  /** A cylinder (2026-11-20, plan item 5: a round shape standing upright): radius half_size[0],
   * half height half_size[2], no turn. */
  bool round = false;
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
 * its object's area (Physics > Navigation > Area, through `area_id`); objects that are
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
/** The scene's navigation links: objects whose Physics > Navigation > Link goes To another object
 * (KX_GameObject::GetNavLinkEnd; kind, both ways, open, when closed, radius, duration, reach
 * from its settings). Closed links are listed too (KX_NavLink::enabled). */
std::vector<KX_NavLink> KX_NavCollectLinks(KX_Scene *scene);

/** The navmesh for agents of this size (the class it falls in), made on first use; null when
 * building is off. */
KX_NavMesh *KX_NavBuildNavMesh(KX_NavBuildState &state, const KX_NavAgentSize &size);
KX_NavMesh *KX_NavBuildNavMesh(KX_NavBuildState &state, float agent_radius);
/** The navmesh of that class if it was made (any thread: nothing is made). */
const KX_NavMesh *KX_NavBuildFindNavMesh(const KX_NavBuildState &state, float agent_radius);

/** Once a step: builds what is due (whole level at the first step, tiles near `agents` in Around
 * NPCs mode, a fixed number a step; `agent_ids` the agents' own lasting numbers), rebuilds dirty tiles, makes again the tiles links changed
 * in (from their layers), carves `obstacles` (those standing still). Every navmesh whose tiles
 * changed gets the changed places (KX_NavMesh::NoteTilesChanged). */
void KX_NavBuildUpdate(KX_NavBuildState &state,
                       const std::vector<MT_Vector3> &agent_positions,
                       const std::vector<KX_NavAgentSize> &agent_sizes,
                       const std::vector<uint64_t> &agent_ids,
                       const std::vector<KX_NavCarveObstacle> &obstacles,
                       const std::vector<KX_NavLink> &links,
                       LN_JobPool *pool);

/** Marks the tiles touching a box for rebuilding. */
void KX_NavBuildMarkDirty(KX_NavBuildState &state,
                          const MT_Vector3 &aabb_min,
                          const MT_Vector3 &aabb_max);
/** Marks the tile under `point` to be made again from its layers (a link starting there came,
 * went or moved: Detour keeps a link in the tile its start lies in). */
void KX_NavBuildRelinkAt(KX_NavBuildState &state, const MT_Vector3 &point);
/** Whether Detour can hold a link from `start` to `end` (2026-11-20): it joins a link's end only
 * in its start's tile or a neighbouring one. Longer links (a teleporter across the level) are
 * routed by the navigation itself (KX_NavFrameLink). */
bool KX_NavBuildLinkFits(const KX_NavBuildState &state,
                         const MT_Vector3 &start,
                         const MT_Vector3 &end);
/** Ways through `places` changed though no tile did (2026-11-20: a link opened or closed): each
 * navmesh is told (KX_NavMesh::NoteTilesChanged), so NPCs whose ways may pass there check them. */
void KX_NavBuildNoteWaysChanged(KX_NavBuildState &state,
                                const std::vector<KX_NavChangedPlace> &places);

/** Jump links (2026-11-19). Ids of the links the build finds start at kNavFoundLinkBase (a
 * hand-placed link's own number stays below); a hand-placed jump link both ways is two one-way
 * links, its way back's id with kNavLinkReverse (each NPC's jump limits judge each way). */
constexpr uint32_t kNavFoundLinkBase = 0x20000000u;
constexpr uint32_t kNavLinkReverse = 0x40000000u;

/** A link the build found (Jump Links Automatic): its ends in the game's space, a drop or a
 * jump (across or up), the space between its ledges (KX_NavFoundLink::gap). */
struct KX_NavLinkInfo {
  MT_Vector3 start = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 end = MT_Vector3(0.0f, 0.0f, 0.0f);
  bool drop = false;
  float gap = 0.0f;
};

/** The areas the links' polygons get (KX_Navigation::AreaId of "jump" and "drop": NPCs' area
 * costs price them, Area Closed forbids them). */
void KX_NavBuildSetLinkAreas(KX_NavBuildState &state, uint8_t jump_area, uint8_t drop_area);
/** A found link by its id (null: none; any thread while no update runs). */
const KX_NavLinkInfo *KX_NavBuildFindLink(const KX_NavBuildState &state, uint32_t id);
/** Every found link (Visualize, tests). */
void KX_NavBuildForEachFoundLink(
    const KX_NavBuildState &state,
    const std::function<void(uint32_t id, const KX_NavLinkInfo &link)> &link);
/** Whether a hand-placed link's kind is a jump the NPCs' limits judge ("jump" or "drop"). */
bool KX_NavLinkKindIsJump(const std::string &kind);
/** Tests: links in the navmeshes whose start is joined and whose end is not, though its tile is
 * there (Detour found no room or no polygon for it). */
size_t KX_NavBuildCountUnjoinedLinks(const KX_NavBuildState &state);
/** Whether an NPC of this size has a navmesh of its own (else the six classes were full and it
 * shares the nearest: plan item 9's warning). */
bool KX_NavBuildHasOwnClass(const KX_NavBuildState &state, const KX_NavAgentSize &agent);

/** Tests: tiles built so far, tiles alive, obstacles carved. */
struct KX_NavBuildStats {
  uint64_t tiles_built = 0;
  uint64_t tiles_removed = 0;
  uint64_t tiles_restored = 0;
  /** Tiles made again from their layers (links). */
  uint64_t tiles_relinked = 0;
  size_t tiles_alive = 0;
  size_t obstacles_carved = 0;
  size_t size_classes = 0;
  /** Jump links found, alive now; tiles whose links were looked for (each time). */
  size_t found_links = 0;
  uint64_t tiles_jump_searched = 0;
};
KX_NavBuildStats KX_NavBuildGetStats(const KX_NavBuildState &state);
