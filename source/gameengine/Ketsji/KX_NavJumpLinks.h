/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file KX_NavJumpLinks.h
 *  \ingroup ketsji
 *  Jump links found from a navmesh (2026-11-19, Scene > Navigation Mesh > Jump Links
 *  Automatic): gaps to jump across, ledges to drop from and ledges to jump up to. As Havok's
 *  traversal analysis and recast4j's JumpLinkBuilder: the walkable area's open edges are sampled,
 *  a landing is looked for outward within the limits, the jump's arc is tested with the agent's
 *  body against the level's triangles, a link is kept only where walking there is clearly
 *  longer, and links along one ledge are thinned to one per Spacing. Each link goes one way (a
 *  jump back is found from its own side), so each NPC's limits judge each direction.
 */

#pragma once

#include <cstdint>
#include <vector>

#include "MT_Vector3.h"

class dtNavMesh;
class dtNavMeshQuery;
struct KX_NavGeometry;

/** What the finder looks for (one agent size class's body and the scene's limits). */
struct KX_NavJumpLinkSpec {
  float radius = 0.5f;
  float height = 2.0f;
  /** The highest step the class walks up (a height difference below it is no jump). */
  float climb = 0.4f;
  /** The scene's Jump Gap (the space between the two ledges, on the plan: the jump itself is
   * longer, the navmesh keeping a body's width from each), Jump Up (above the climb), Drop
   * Down. */
  float gap = 3.0f;
  float up = 1.5f;
  float drop = 4.0f;
  /** One link per this much of a ledge. */
  float spacing = 2.0f;
  /** The navmesh's tile width: Detour joins a link's end only in the start's tile or a
   * neighbour, so no landing is looked for farther. */
  float tile_width = 1e9f;
};

/** How far beyond a tile (on the plan) the finder reads the level for that tile's links. */
float KX_NavJumpLinkReach(const KX_NavJumpLinkSpec &spec);

/** A link found (the game's space, Z up): from `start` (on the walkable area, near its edge) to
 * `end` (on the landing, near its edge); a drop goes down by more than the climb. `gap`: the
 * space between the level's two ledges on the plan (what the limits judge), less than the jump
 * from `start` to `end`. */
struct KX_NavFoundLink {
  MT_Vector3 start = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 end = MT_Vector3(0.0f, 0.0f, 0.0f);
  bool drop = false;
  float gap = 0.0f;
};

/** The links starting in the navmesh tiles at (`tile_x`, `tile_y`) (every layer), `geometry` the
 * level's triangles around them (the game's space: at least the tiles' box widened by
 * KX_NavJumpLinkReach), only those listed in `triangles` when given. `query` is the caller's
 * (its node pool bounds the "walking is about as short" search; 512 nodes is enough). Any
 * thread, while nothing changes the navmesh; the same links on every run (in the tiles' polygon
 * and edge order). */
void KX_NavFindJumpLinks(const dtNavMesh &mesh,
                         dtNavMeshQuery &query,
                         int tile_x,
                         int tile_y,
                         const KX_NavJumpLinkSpec &spec,
                         const KX_NavGeometry &geometry,
                         const std::vector<int> *triangles,
                         std::vector<KX_NavFoundLink> &r_links);

/** A jump's way through the air (the game's space): a throw's parabola from `start` to `end`
 * whose top is `clearance` above the higher end, at `t` (0..1) of the way. Shared by the finder's
 * test, the carried crossing and the physics launch (KX_NavJumpLaunch). */
MT_Vector3 KX_NavJumpArc(const MT_Vector3 &start,
                         const MT_Vector3 &end,
                         float clearance,
                         float t);
/** How high a jump's top rises above its higher end: more for a longer jump. */
float KX_NavJumpClearance(const MT_Vector3 &start, const MT_Vector3 &end);

/** A physics jump from `start` to `end` under `gravity` (m/s2, > 0) whose top is `clearance`
 * above the higher end: the launch's upward speed, the speed along the plan, and the time in the
 * air. False when it can't (a zero gravity). */
bool KX_NavJumpLaunch(const MT_Vector3 &start,
                      const MT_Vector3 &end,
                      float clearance,
                      float gravity,
                      float &r_up_speed,
                      float &r_plan_speed,
                      float &r_time);

/** Whether an agent whose upward jump speed is `jump_speed` (m/s), whose speed along the plan is
 * at most `plan_speed`, under `gravity`, makes a jump `gap` across (on the plan) and `rise` up
 * (negative: down), climbing `climb` by itself. */
bool KX_NavJumpReachable(float jump_speed,
                         float plan_speed,
                         float gravity,
                         float climb,
                         float gap,
                         float rise);
