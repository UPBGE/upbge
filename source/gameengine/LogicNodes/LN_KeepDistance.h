/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup logicnodes
 *
 * Keep Distance (2026-11-12; LN_OpCode::KeepDistance): a physics object is pushed away from every
 * surface within Distance of it, harder the closer, like a cushion (Mode Push); or pulled toward
 * the nearest surface within Distance, harder the closer, like a magnet (Pull); or kept at
 * Distance: pushed as Push from anything closer, pulled back toward the nearest surface when it
 * drifts farther, out to Reach (Hold).
 *
 * A run of the node records a request (LN_CommandBuffer::AppendKeepDistance). At the end of the
 * step's flush (every write of the step applied: the object is where the physics will start
 * from) LN_ProcessKeepDistanceRequests answers all of them: the ones due a check ask the physics
 * at once (PHY_IPhysicsEnvironment::ProximityBatch: one volume query each, Jolt runs them on its
 * job system), then in record order each works out its push from the surfaces found and applies
 * it as a force. The surfaces (patches: one per body and side, PHY_ProximityPatch) are kept in
 * the tree (LN_KeepDistanceState) and, with Check Every N, measured again each step from the
 * object's movement; the node's outputs read that state (LN_QueryExpressionKind::KeepDistance).
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include "LN_Types.h"
#include "MT_Vector3.h"

class KX_GameObject;
class LN_RuntimeTree;

/** One surface a check found: its nearest point, the direction pushing away from it and the gap
 * then. Between checks the gap is `gap + (position - check position) . away`. */
struct LN_KeepDistancePatch {
  MT_Vector3 point = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 away = MT_Vector3(0.0f, 0.0f, 1.0f);
  float gap = 0.0f;
};

/** Keep Distance's Mode (the node's Mode input, LN_KEEP_DISTANCE_MODE_*: 0, 1, 2). */
enum class LN_KeepDistanceMode : uint8_t { Push, Pull, Hold };

/** A Keep Distance node's inputs, read in the tree's run. */
struct LN_KeepDistanceSettings {
  LN_KeepDistanceMode mode = LN_KeepDistanceMode::Push;
  float distance = 1.0f;
  /** Hold: how far the pull back reaches (at least Distance). */
  float reach = 2.0f;
  float strength = 20.0f;
  float damping = 1.0f;
  float radius = 0.5f;
  int32_t check_every = 1;
  bool ignore_mass = true;
  bool apply = true;
  bool visualize = false;
  bool smooth = false;
  bool sphere = false;
  bool include_sensors = false;
  uint32_t mask = 1023;
  std::string property;
  std::vector<KX_GameObject *> ignore;
};

/** A Keep Distance node's state in one tree (LN_RuntimeTree::KeepDistanceState). */
struct LN_KeepDistanceState {
  /** The surfaces of the last check, nearest first. */
  std::vector<LN_KeepDistancePatch> patches;
  /** Where the object was at the last check, and how far that check reached past it. */
  MT_Vector3 check_position = MT_Vector3(0.0f, 0.0f, 0.0f);
  float check_reach = 0.0f;
  /** The object measured (compared only) and the step of the last check (-1: none yet). */
  const KX_GameObject *check_object = nullptr;
  int64_t check_step = -1;
  /** Its body's mass and whether it is dynamic, read at the check. */
  float mass = 1.0f;
  bool dynamic = false;
  /** The outputs: what the last answer gave, of the surfaces within the node's range
   * (LN_KeepDistanceRange; Nearest Object: the last check's nearest). */
  bool near = false;
  float nearest_distance = -1.0f;
  MT_Vector3 nearest_point = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 nearest_normal = MT_Vector3(0.0f, 0.0f, 0.0f);
  LN_RuntimeRef nearest_object;
  /** The Force output: the force applied (or, Apply off, that would be), push or pull. */
  MT_Vector3 force = MT_Vector3(0.0f, 0.0f, 0.0f);
};

/** One run of the node, answered at the end of the flush. The tree outlives the flush (trees go
 * after it: LN_Manager::RemoveDetachedRuntimeTrees). */
struct LN_KeepDistanceRequest {
  LN_RuntimeTree *tree = nullptr;
  uint32_t slot = 0;
  KX_GameObject *object = nullptr;
  LN_KeepDistanceSettings settings;
  uint64_t sort_key = 0;
  /** The step (the tick's phase step) and its length in seconds. */
  int64_t step = 0;
  float dt = 1.0f / 60.0f;
  /** Spreads Check Every's checks of many trees over the steps. */
  uint32_t spread = 0;
};

/** How far a node's surfaces count: Distance, or Reach in Hold (at least Distance). */
inline float LN_KeepDistanceRange(const LN_KeepDistanceSettings &settings)
{
  return settings.mode == LN_KeepDistanceMode::Hold ? std::max(settings.reach, settings.distance) :
                                                      settings.distance;
}

/** What the push or pull of the surfaces `patches` is now, as an acceleration (m/s^2):
 * `movement` the object's move since the check (the gaps are measured again from it), `strength`
 * the push at a touching surface in m/s^2 (Strength, or Strength / mass), `velocity` the
 * object's. Pure.
 *
 * Push (and Hold while a surface is within Distance): each surface within Distance weighs
 * falloff(gap / Distance) (Linear 1 - t, Smooth (1 - t)^2); the push points along the weighted
 * average of the away directions and is strength x the nearest surface's weight x how one-sided
 * they are (|sum| / total weight: 1 with one surface, 0 between two equal opposite walls), so a
 * dense mesh pushes as hard as a coarse one.
 * Pull: toward the nearest surface only (an average would aim between two surfaces, at nothing),
 * strength x its weight: harder the closer, as a magnet; it ends touching.
 * Hold, nothing within Distance: toward the nearest surface while it is within Reach, strength x
 * the mirrored falloff of t = (gap - Distance) / (Reach - Distance) (Linear t, Smooth t^2): nothing
 * at Distance, all of it at Reach, nothing past Reach.
 * Damping brakes the velocity along the push or pull, 2 x Damping x sqrt(k) (1: critical for a
 * spring of stiffness k: strength / Distance; Hold strength / (Reach / 2), the mean of its two
 * sides at the default Reach = 2 x Distance), faded with the weight (no jolt where the effect
 * starts) but, in Hold, full around Distance (where it settles) and faded only toward Reach;
 * never more than stops the movement in `dt`.
 * `r_nearest`: the index of the nearest surface now (-1: none found). */
MT_Vector3 LN_KeepDistanceAcceleration(const std::vector<LN_KeepDistancePatch> &patches,
                                       const MT_Vector3 &movement,
                                       const LN_KeepDistanceSettings &settings,
                                       float strength,
                                       const MT_Vector3 &velocity,
                                       float dt,
                                       int *r_nearest);

/** The flush's hooks: applies a force (world) to an object, the tests' observer included. */
using LN_KeepDistanceApplyForce = void (*)(KX_GameObject *object, const MT_Vector3 &force);

/** Answers the step's requests (the end of LN_CommandBuffer::Flush), in sort-key order;
 * `removed_objects`: objects the flush removed (their requests are dropped). Main thread. */
void LN_ProcessKeepDistanceRequests(std::vector<LN_KeepDistanceRequest> &requests,
                                    const std::unordered_set<KX_GameObject *> &removed_objects,
                                    LN_KeepDistanceApplyForce apply_force);
