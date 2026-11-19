/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup logicnodes
 *
 * Keep Distance's push and its end-of-flush answer (LN_KeepDistance.h).
 */

#include "LN_KeepDistance.h"

#include <algorithm>
#include <cmath>

#include "KX_ClientObjectInfo.h"
#include "KX_GameObject.h"
#include "KX_Globals.h"
#include "LN_RuntimeTree.h"
#include "PHY_IPhysicsController.h"
#include "PHY_IPhysicsEnvironment.h"
#include "SG_Node.h"

namespace {

float keep_distance_weight(const float gap, const LN_KeepDistanceSettings &settings)
{
  if (!(settings.distance > 0.0f)) {
    return gap <= 0.0f ? 1.0f : 0.0f;
  }
  const float t = std::clamp(gap / settings.distance, 0.0f, 1.0f);
  const float linear = 1.0f - t;
  return settings.smooth ? linear * linear : linear;
}

bool vector_finite(const MT_Vector3 &v)
{
  return std::isfinite(v.x()) && std::isfinite(v.y()) && std::isfinite(v.z());
}

}  // namespace

MT_Vector3 LN_KeepDistanceAcceleration(const std::vector<LN_KeepDistancePatch> &patches,
                                       const MT_Vector3 &movement,
                                       const LN_KeepDistanceSettings &settings,
                                       const float strength,
                                       const MT_Vector3 &velocity,
                                       const float dt,
                                       int *r_nearest)
{
  if (r_nearest != nullptr) {
    *r_nearest = -1;
  }
  const bool pull = settings.mode == LN_KeepDistanceMode::Pull;
  const bool hold = settings.mode == LN_KeepDistanceMode::Hold;
  MT_Vector3 sum(0.0f, 0.0f, 0.0f);
  float total = 0.0f;
  float nearest_weight = 0.0f;
  float nearest_gap = FLT_MAX;
  int nearest = -1;
  for (size_t index = 0; index < patches.size(); index++) {
    const LN_KeepDistancePatch &patch = patches[index];
    const float gap = std::max(0.0f, patch.gap + float(movement.dot(patch.away)));
    if (gap < nearest_gap) {
      nearest_gap = gap;
      nearest = int(index);
    }
    if (pull) {
      continue;
    }
    const float weight = gap < settings.distance || settings.distance <= 0.0f ?
                             keep_distance_weight(gap, settings) :
                             0.0f;
    if (weight > 0.0f) {
      sum += patch.away * weight;
      total += weight;
      nearest_weight = std::max(nearest_weight, weight);
    }
  }
  if (r_nearest != nullptr) {
    *r_nearest = nearest;
  }
  if (!(strength > 0.0f) || !std::isfinite(strength)) {
    return MT_Vector3(0.0f, 0.0f, 0.0f);
  }
  /* The spring Damping 1 brakes critically: strength / Distance per meter; Hold's two sides
   * (Distance inward, Reach - Distance outward) as one, strength / (Reach / 2). */
  const float hold_stiffness = strength / std::max(0.5f * LN_KeepDistanceRange(settings), 0.01f);
  MT_Vector3 acceleration;
  MT_Vector3 direction;
  float damping_weight = 0.0f;
  float stiffness = 0.0f;
  if (total > 0.0f) {
    /* Push (and Hold within Distance): along the weighted average of the away directions; as
     * strong as the nearest surface asks, times how one-sided the surfaces are (|sum| / total). */
    const MT_Vector3 push = sum * (strength * nearest_weight / total);
    const float push_length = push.length();
    if (!(push_length > 1.0e-6f) || !std::isfinite(push_length)) {
      return MT_Vector3(0.0f, 0.0f, 0.0f);
    }
    acceleration = push;
    direction = push / push_length;
    /* Faded in with the nearest weight: no jolt where the cushion starts. Hold settles at
     * Distance, the cushion's edge: braked fully there. */
    damping_weight = hold ? 1.0f : nearest_weight;
    stiffness = hold ? hold_stiffness : strength / std::max(settings.distance, 0.01f);
  }
  else if ((pull || hold) && nearest >= 0) {
    float weight = 0.0f;
    if (pull) {
      /* The nearest surface only: harder the closer, as the push. */
      weight = nearest_gap < settings.distance || settings.distance <= 0.0f ?
                   keep_distance_weight(nearest_gap, settings) :
                   0.0f;
      damping_weight = weight;
      stiffness = strength / std::max(settings.distance, 0.01f);
    }
    else {
      /* Hold, nothing within Distance: back toward the nearest surface, from nothing at
       * Distance to all of it at Reach (the falloff mirrored); nothing past Reach. */
      const float distance = std::max(settings.distance, 0.0f);
      const float range = LN_KeepDistanceRange(settings);
      if (!(nearest_gap < range) || !(range > distance)) {
        return MT_Vector3(0.0f, 0.0f, 0.0f);
      }
      const float t = std::clamp((nearest_gap - distance) / (range - distance), 0.0f, 1.0f);
      weight = settings.smooth ? t * t : t;
      /* Fully braked around Distance, where it settles; faded toward Reach (no jolt where the
       * pull starts). */
      damping_weight = 1.0f - t;
      stiffness = hold_stiffness;
    }
    if (!(weight > 0.0f) && !(damping_weight > 0.0f)) {
      return MT_Vector3(0.0f, 0.0f, 0.0f);
    }
    direction = -patches[size_t(nearest)].away;
    acceleration = direction * (strength * weight);
  }
  else {
    return MT_Vector3(0.0f, 0.0f, 0.0f);
  }
  if (settings.damping > 0.0f && damping_weight > 0.0f && vector_finite(velocity)) {
    /* Along the push or pull only (sideways movement is the object's own). */
    const float along = float(velocity.dot(direction));
    float brake = 2.0f * settings.damping * std::sqrt(stiffness) * along * damping_weight;
    /* Never more than stops the movement along it within the step. */
    if (dt > 0.0f) {
      const float most = std::abs(along) / dt;
      brake = std::clamp(brake, -most, most);
    }
    acceleration -= direction * brake;
  }
  return vector_finite(acceleration) ? acceleration : MT_Vector3(0.0f, 0.0f, 0.0f);
}

namespace {

/** The bodies of `object`'s children (they move with it: never pushed away from). */
void append_child_controllers(KX_GameObject &object, std::vector<PHY_IPhysicsController *> &out)
{
  SG_Node *node = object.GetSGNode();
  if (node == nullptr || node->GetSGChildren().empty()) {
    return;
  }
  std::vector<const SG_Node *> stack(node->GetSGChildren().begin(),
                                     node->GetSGChildren().end());
  for (size_t guard = 0; !stack.empty() && guard < 4096; guard++) {
    const SG_Node *child_node = stack.back();
    stack.pop_back();
    if (child_node == nullptr) {
      continue;
    }
    KX_GameObject *child = static_cast<KX_GameObject *>(child_node->GetSGClientObject());
    if (child != nullptr && child->GetPhysicsController() != nullptr) {
      out.push_back(child->GetPhysicsController());
    }
    stack.insert(stack.end(), child_node->GetSGChildren().begin(), child_node->GetSGChildren().end());
  }
}

KX_GameObject *patch_object(const PHY_ProximityPatch &patch)
{
  KX_ClientObjectInfo *info = patch.controller ? static_cast<KX_ClientObjectInfo *>(
                                                     patch.controller->GetNewClientInfo()) :
                                                 nullptr;
  return info != nullptr ? info->m_gameobject : nullptr;
}

/** A line to each surface found: red pushing, magenta pulling, yellow found but not acting; the
 * acceleration a green arrow (the viewport preview draws Distance and Reach in these colours).
 * `nearest`: LN_KeepDistanceAcceleration's. */
void draw_keep_distance(const MT_Vector3 &position,
                        const LN_KeepDistanceState &state,
                        const MT_Vector3 &movement,
                        const LN_KeepDistanceSettings &settings,
                        const MT_Vector3 &acceleration,
                        const int nearest)
{
  const MT_Vector4 pushing(1.0f, 0.1f, 0.05f, 1.0f);
  const MT_Vector4 pulling(0.9f, 0.2f, 0.9f, 1.0f);
  const MT_Vector4 in_reach(1.0f, 0.85f, 0.1f, 1.0f);
  bool any_within_distance = false;
  for (const LN_KeepDistancePatch &patch : state.patches) {
    any_within_distance |= std::max(0.0f, patch.gap + float(movement.dot(patch.away))) <
                           settings.distance;
  }
  const float range = LN_KeepDistanceRange(settings);
  for (size_t index = 0; index < state.patches.size(); index++) {
    const LN_KeepDistancePatch &patch = state.patches[index];
    const float gap = std::max(0.0f, patch.gap + float(movement.dot(patch.away)));
    const bool is_nearest = int(index) == nearest;
    const MT_Vector4 *color = &in_reach;
    switch (settings.mode) {
      case LN_KeepDistanceMode::Push:
        color = gap < settings.distance ? &pushing : &in_reach;
        break;
      case LN_KeepDistanceMode::Pull:
        color = is_nearest && gap < settings.distance ? &pulling : &in_reach;
        break;
      case LN_KeepDistanceMode::Hold:
        color = gap < settings.distance                              ? &pushing :
                !any_within_distance && is_nearest && gap < range ? &pulling :
                                                                       &in_reach;
        break;
    }
    KX_RasterizerDrawDebugLine(position, patch.point, *color);
  }
  /* The acceleration: 1 m for 10 m/s^2. */
  if (acceleration.length2() > 1.0e-8f) {
    KX_RasterizerDrawDebugLine(
        position, position + acceleration * 0.1f, MT_Vector4(0.1f, 1.0f, 0.2f, 1.0f));
  }
}

}  // namespace

void LN_ProcessKeepDistanceRequests(std::vector<LN_KeepDistanceRequest> &requests,
                                    const std::unordered_set<KX_GameObject *> &removed_objects,
                                    const LN_KeepDistanceApplyForce apply_force)
{
  if (requests.empty()) {
    return;
  }
  /* In the flush's order: by tree, then as recorded (they come in that order but for Run Logic
   * Tree passes and appended chunks). */
  const auto by_tree = [](const LN_KeepDistanceRequest &a, const LN_KeepDistanceRequest &b) {
    return a.sort_key < b.sort_key;
  };
  if (!std::is_sorted(requests.begin(), requests.end(), by_tree)) {
    std::stable_sort(requests.begin(), requests.end(), by_tree);
  }

  struct Pending {
    LN_KeepDistanceState *state = nullptr;
    MT_Vector3 position;
    MT_Vector3 velocity;
    PHY_IPhysicsController *controller = nullptr;
    PHY_IPhysicsEnvironment *environment = nullptr;
    /* Its query in `queries` (LN_INVALID_INDEX: the remembered surfaces serve). */
    uint32_t query = LN_INVALID_INDEX;
    float reach = 0.0f;
    bool skip = true;
  };
  thread_local std::vector<Pending> pending;
  thread_local std::vector<PHY_ProximityQuery> queries;
  thread_local std::vector<std::vector<PHY_ProximityPatch>> results;
  pending.assign(requests.size(), Pending());
  queries.clear();

  /* 1. Which requests check this step, and their queries (main thread: game objects). */
  PHY_IPhysicsEnvironment *batch_environment = nullptr;
  for (size_t index = 0; index < requests.size(); index++) {
    LN_KeepDistanceRequest &request = requests[index];
    Pending &entry = pending[index];
    KX_GameObject *object = request.object;
    if (request.tree == nullptr || object == nullptr ||
        removed_objects.find(object) != removed_objects.end())
    {
      continue;
    }
    entry.skip = false;
    entry.state = &request.tree->KeepDistanceState(request.slot);
    entry.position = object->NodeGetWorldPosition();
    entry.controller = object->GetPhysicsController();
    entry.velocity = entry.controller != nullptr ? entry.controller->GetLinearVelocity() :
                                                   MT_Vector3(0.0f, 0.0f, 0.0f);
    if (!vector_finite(entry.velocity)) {
      entry.velocity = MT_Vector3(0.0f, 0.0f, 0.0f);
    }
    const LN_KeepDistanceSettings &settings = request.settings;
    const int32_t every = std::max(settings.check_every, 1);
    /* How far surfaces count: Distance, or Reach in Hold. With Check Every N, a check reaches as
     * far past that as the object may move until the next one, so a surface it heads for is
     * known in time. */
    const float range = std::max(LN_KeepDistanceRange(settings), 0.0f);
    const float lookahead = every > 1 ?
                                std::min(float(entry.velocity.length()) * request.dt * float(every) +
                                             0.05f,
                                         4.0f * std::max(range, 0.25f)) :
                                0.0f;
    entry.reach = range + lookahead;
    const LN_KeepDistanceState &state = *entry.state;
    const float moved = float((entry.position - state.check_position).length());
    const bool due = every <= 1 || state.check_step < 0 || state.check_object != object ||
                     (uint64_t(request.step) + request.spread) % uint64_t(every) == 0 ||
                     request.step - state.check_step >= every || range > state.check_reach ||
                     moved > state.check_reach - range;
    if (!due) {
      continue;
    }
    entry.environment = LN_QueryPhysicsEnvironment(object);
    if (entry.environment == nullptr) {
      continue;
    }
    if (batch_environment == nullptr) {
      batch_environment = entry.environment;
    }
    PHY_ProximityQuery query;
    query.controller = entry.controller;
    query.use_body_shape = !settings.sphere && entry.controller != nullptr;
    query.center = entry.position;
    query.radius = settings.radius;
    query.reach = entry.reach;
    query.collision_mask = settings.mask;
    query.include_sensors = settings.include_sensors;
    /* (More before the Property filter thins them.) */
    query.max_patches = settings.property.empty() ? LN_KEEP_DISTANCE_MAX_PATCHES :
                                                    4 * LN_KEEP_DISTANCE_MAX_PATCHES;
    append_child_controllers(*object, query.ignore);
    for (KX_GameObject *ignored : settings.ignore) {
      if (ignored != nullptr && ignored->GetPhysicsController() != nullptr) {
        query.ignore.push_back(ignored->GetPhysicsController());
      }
    }
    entry.query = uint32_t(queries.size());
    queries.push_back(std::move(query));
  }

  /* 2. The physics answers them at once (Jolt: on its job system). A scene has one physics
   * world; requests of another (a merged scene's) ask it on their own. */
  results.resize(queries.size());
  if (!queries.empty()) {
    bool one_world = true;
    for (const Pending &entry : pending) {
      if (entry.query != LN_INVALID_INDEX && entry.environment != batch_environment) {
        one_world = false;
        break;
      }
    }
    if (one_world) {
      if (!batch_environment->ProximityBatch(queries, results)) {
        results.assign(queries.size(), {});
      }
    }
    else {
      for (const Pending &entry : pending) {
        if (entry.query == LN_INVALID_INDEX) {
          continue;
        }
        thread_local std::vector<PHY_ProximityQuery> one;
        thread_local std::vector<std::vector<PHY_ProximityPatch>> one_result;
        one.assign(1, queries[entry.query]);
        if (entry.environment->ProximityBatch(one, one_result) && !one_result.empty()) {
          results[entry.query].swap(one_result[0]);
        }
        else {
          results[entry.query].clear();
        }
      }
    }
  }

  /* 3. In order: the surfaces, the push, the outputs, the force. */
  for (size_t index = 0; index < requests.size(); index++) {
    const LN_KeepDistanceRequest &request = requests[index];
    Pending &entry = pending[index];
    if (entry.skip) {
      continue;
    }
    LN_KeepDistanceState &state = *entry.state;
    const LN_KeepDistanceSettings &settings = request.settings;
    KX_GameObject *object = request.object;
    if (entry.query != LN_INVALID_INDEX) {
      const std::vector<PHY_ProximityPatch> &found = results[entry.query];
      state.patches.clear();
      KX_GameObject *nearest = nullptr;
      for (const PHY_ProximityPatch &patch : found) {
        if (state.patches.size() >= LN_KEEP_DISTANCE_MAX_PATCHES) {
          break;
        }
        KX_GameObject *owner = patch_object(patch);
        if (owner == nullptr || !vector_finite(patch.point) || !vector_finite(patch.away) ||
            !std::isfinite(patch.gap) ||
            (!settings.property.empty() && owner->GetProperty(settings.property) == nullptr))
        {
          continue;
        }
        if (nearest == nullptr) {
          nearest = owner;
        }
        LN_KeepDistancePatch kept;
        kept.point = patch.point;
        kept.away = patch.away;
        kept.gap = patch.gap;
        state.patches.push_back(kept);
      }
      state.check_position = entry.position;
      state.check_reach = entry.reach;
      state.check_object = object;
      state.check_step = request.step;
      /* The nearest object's reference: made again only when it is another object (or the
       * one it stood for went). */
      if (nearest == nullptr) {
        state.nearest_object = LN_RuntimeRef();
      }
      else if (request.tree->ResolveObjectRef(state.nearest_object) != nearest) {
        state.nearest_object = request.tree->MakeObjectRef(nearest, nearest->GetName());
      }
      /* The body's mass, read once a check (a Character's too: forces move it, 2026-11-17). */
      state.dynamic = entry.controller != nullptr &&
                      (entry.controller->IsDynamic() || entry.controller->IsCharacter());
      state.mass = state.dynamic ? float(entry.controller->GetMass()) : 1.0f;
    }

    /* Strength as an acceleration: Ignore Mass, or the force over the body's mass. */
    const bool dynamic = state.dynamic && entry.controller != nullptr;
    const float mass = dynamic ? state.mass : 1.0f;
    const float strength = settings.ignore_mass || !(mass > 0.0f) ? settings.strength :
                                                                    settings.strength / mass;
    const MT_Vector3 movement = entry.position - state.check_position;
    int nearest = -1;
    const MT_Vector3 acceleration = LN_KeepDistanceAcceleration(
        state.patches, movement, settings, strength, entry.velocity, request.dt, &nearest);
    const MT_Vector3 force = acceleration * (mass > 0.0f ? mass : 1.0f);

    /* The outputs tell of the node's range only (a surface within Distance, Hold's Reach): the
     * same whatever Check Every looks ahead. */
    float nearest_gap = 0.0f;
    if (nearest >= 0) {
      const LN_KeepDistancePatch &patch = state.patches[size_t(nearest)];
      nearest_gap = std::max(0.0f, patch.gap + float(movement.dot(patch.away)));
    }
    if (nearest >= 0 && nearest_gap < LN_KeepDistanceRange(settings)) {
      const LN_KeepDistancePatch &patch = state.patches[size_t(nearest)];
      state.nearest_distance = nearest_gap;
      state.is_near = true;
      state.nearest_point = patch.point;
      state.nearest_normal = patch.away;
    }
    else {
      state.nearest_distance = -1.0f;
      state.is_near = false;
      state.nearest_point = MT_Vector3(0.0f, 0.0f, 0.0f);
      state.nearest_normal = MT_Vector3(0.0f, 0.0f, 0.0f);
    }
    state.force = force;
    if (settings.apply && dynamic && force.length2() > 0.0f && apply_force != nullptr) {
      apply_force(object, force);
    }
    if (settings.visualize) {
      draw_keep_distance(entry.position, state, movement, settings, acceleration, nearest);
    }
  }
}
