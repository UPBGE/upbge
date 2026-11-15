/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_TreeCompiler_ray_outputs.cc
 *  \ingroup logicnodes
 *
 * The raycast recommendations (2026-11-11): the queries of Projectile Ray, Line of Sight and
 * Sweep Object, and the outputs of the ray family that are not in the per-kind builders: every
 * output of Mouse Ray, Camera Ray, Projectile Ray, Line of Sight, Sweep Object, Aim for Arc and
 * Ray to Plane, and Hit Part / Hit Material (Hit Parts / Hit Materials) of Raycast, Shape Cast
 * and Raycast from Mesh. TryCompileExpressionOutputByKind asks TryCompileRayFamilyOutput first.
 */

#include "../LN_TreeCompiler_internal.hh"

#include <algorithm>

#include "DNA_node_types.h"

namespace ln_compiler {

namespace {

/** The typed caches a query builder needs besides the float and bool ones it is given. */
struct InputCaches {
  IntExpressionCache ints;
  StringExpressionCache strings;
  VectorExpressionCache vectors;
  ColorExpressionCache colors;
  ValueExpressionCache values;
};

/** A node's inputs, read through the shared builders; a missing socket (a node made by hand in a
 * test) or an unlinked one whose constant cannot be read reads as "unset" (LN_INVALID_INDEX: the
 * query's own fallback), a linked one that fails to compile as an error (as the Projectile Ray's
 * compile was before 2026-11-11). */
class InputReader {
 public:
  InputReader(LN_Program &program,
              const blender::bNode &node,
              const NodeDefinitionMap &node_definitions,
              const InputLinkMap &input_links,
              ValueCache &value_cache,
              FloatExpressionCache &float_cache,
              BoolExpressionCache &bool_cache)
      : program_(program),
        node_(node),
        node_definitions_(node_definitions),
        input_links_(input_links),
        value_cache_(value_cache),
        float_cache_(float_cache),
        bool_cache_(bool_cache)
  {
  }

  bool failed() const
  {
    return failed_;
  }

  uint32_t Condition()
  {
    return Check("Flow", BuildOptionalExecutionCondition(program_,
                                                 node_,
                                                 "Flow",
                                                 node_definitions_,
                                                 input_links_,
                                                 value_cache_,
                                                 float_cache_,
                                                 bool_cache_));
  }

  /** An Object input that is the owner (or nothing) when empty: unset then. */
  uint32_t OptionalObject(const char *name)
  {
    if (FindInputSocket(node_, name) == nullptr) {
      return LN_INVALID_INDEX;
    }
    const std::optional<uint32_t> result = BuildOptionalObjectTargetExpression(program_,
                                                                               node_,
                                                                               name,
                                                                               node_definitions_,
                                                                               input_links_,
                                                                               value_cache_,
                                                                               bool_cache_,
                                                                               caches_.ints,
                                                                               float_cache_,
                                                                               caches_.strings,
                                                                               caches_.vectors,
                                                                               caches_.colors,
                                                                               caches_.values);
    if (!result && IsInputSocketLinked(node_, name, input_links_)) {
      failed_ = true;
    }
    return result.value_or(LN_INVALID_INDEX);
  }

  uint32_t Value(const char *name)
  {
    if (FindInputSocket(node_, name) == nullptr) {
      return LN_INVALID_INDEX;
    }
    return Check(name, BuildInputValueExpression(program_,
                                           node_,
                                           name,
                                           node_definitions_,
                                           input_links_,
                                           value_cache_,
                                           bool_cache_,
                                           caches_.ints,
                                           float_cache_,
                                           caches_.strings,
                                           caches_.vectors,
                                           caches_.colors,
                                           caches_.values));
  }

  uint32_t Vector(const char *name)
  {
    if (FindInputSocket(node_, name) == nullptr) {
      return LN_INVALID_INDEX;
    }
    return Check(name, BuildInputVectorExpression(program_,
                                            node_,
                                            name,
                                            node_definitions_,
                                            input_links_,
                                            value_cache_,
                                            float_cache_,
                                            caches_.vectors));
  }

  uint32_t Rotation(const char *name)
  {
    if (FindInputSocket(node_, name) == nullptr) {
      return LN_INVALID_INDEX;
    }
    return Check(name, BuildInputRotationVectorExpression(program_,
                                                    node_,
                                                    name,
                                                    node_definitions_,
                                                    input_links_,
                                                    value_cache_,
                                                    float_cache_,
                                                    caches_.vectors));
  }

  uint32_t Float(const char *name)
  {
    if (FindInputSocket(node_, name) == nullptr) {
      return LN_INVALID_INDEX;
    }
    return Check(name, BuildInputFloatExpression(
        program_, node_, name, node_definitions_, input_links_, value_cache_, float_cache_));
  }

  /** A Bool input; a missing one is constant false. */
  uint32_t Bool(const char *name)
  {
    if (FindInputSocket(node_, name) == nullptr) {
      return AddConstantBoolExpression(program_, false);
    }
    return Check(name, BuildInputBoolExpression(program_,
                                          node_,
                                          name,
                                          node_definitions_,
                                          input_links_,
                                          value_cache_,
                                          float_cache_,
                                          bool_cache_));
  }

  uint32_t Int(const char *name)
  {
    if (FindInputSocket(node_, name) == nullptr) {
      return LN_INVALID_INDEX;
    }
    return Check(name, BuildInputIntExpression(
        program_, node_, name, node_definitions_, input_links_, value_cache_, caches_.ints));
  }

  uint32_t String(const char *name)
  {
    if (FindInputSocket(node_, name) == nullptr) {
      return LN_INVALID_INDEX;
    }
    return Check(name, BuildInputStringExpression(
        program_, node_, name, node_definitions_, input_links_, value_cache_, caches_.strings));
  }

 private:
  uint32_t Check(const char *name, const std::optional<uint32_t> &result)
  {
    if (!result) {
      if (IsInputSocketLinked(node_, name, input_links_)) {
        failed_ = true;
      }
      return LN_INVALID_INDEX;
    }
    return *result;
  }

  LN_Program &program_;
  const blender::bNode &node_;
  const NodeDefinitionMap &node_definitions_;
  const InputLinkMap &input_links_;
  ValueCache &value_cache_;
  FloatExpressionCache &float_cache_;
  BoolExpressionCache &bool_cache_;
  InputCaches caches_;
  bool failed_ = false;
};

MT_Vector3 VectorFallback(const LN_Program &program, const uint32_t index)
{
  return index != LN_INVALID_INDEX ? VectorExpressionConstantFallback(program, index) :
                                     MT_Vector3(0.0f, 0.0f, 0.0f);
}

float FloatFallback(const LN_Program &program, const uint32_t index, const float fallback)
{
  return index != LN_INVALID_INDEX ? FloatExpressionConstantFallback(program, index) : fallback;
}

}  // namespace

std::optional<uint32_t> BuildProjectileRayQueryExpression(
    LN_Program &program,
    const blender::bNode &node,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
    FloatExpressionCache &float_expression_cache,
    BoolExpressionCache &bool_expression_cache)
{
  InputReader in(program,
                 node,
                 node_definitions,
                 input_links,
                 value_cache,
                 float_expression_cache,
                 bool_expression_cache);
  LN_QueryExpression query;
  query.kind = LN_QueryExpressionKind::ProjectileRay;
  query.condition_bool_expr_index = in.Condition();
  /* Caster is optional, as for Raycast (empty: no object ignored; in Local space the owner). */
  query.input0 = in.OptionalObject("Caster");
  query.input1 = in.Vector("Origin");
  query.input2 = in.Vector("Aim");
  query.input3 = in.Value("Ignore Object");
  query.ignore_list_expr_index = in.Value("Ignore Objects");
  query.bool_expr_index = in.Bool("Local");
  query.float_expr_index = in.Float("Power");
  query.secondary_float_expr_index = in.Float("Distance");
  query.tertiary_float_expr_index = in.Float("Resolution");
  query.input4 = in.Float("Radius");
  query.input5 = in.Float("Max Time");
  /* Gravity: Custom (custom1 1) reads the socket; Scene leaves input6 unset. */
  query.input6 = node.custom1 == 1 ? in.Vector("Gravity") : LN_INVALID_INDEX;
  query.int_expr_index = in.Int("Mask");
  query.string_expr_index = in.String("Property");
  query.secondary_bool_expr_index = in.Bool("X-Ray");
  query.tertiary_bool_expr_index = in.Bool("Visualize");
  query.quaternary_bool_expr_index = in.Bool("Include Sensors");
  query.quinary_bool_expr_index = in.Bool("Hit Backfaces");
  if (in.failed() || query.input1 == LN_INVALID_INDEX || query.input2 == LN_INVALID_INDEX) {
    return std::nullopt;
  }
  query.vector_value = VectorFallback(program, query.input1);
  query.secondary_vector_value = VectorFallback(program, query.input2);
  query.float_value = FloatFallback(program, query.float_expr_index, 10.0f);
  query.int_value = query.int_expr_index != LN_INVALID_INDEX ?
                        IntExpressionConstantFallback(program, query.int_expr_index) :
                        0;
  query.cache_key = node.identifier;
  AddVisualizeStyleInputs(program, node, node_definitions, input_links, value_cache, query);
  return program.AddQueryExpression(query);
}

std::optional<uint32_t> BuildLineOfSightQueryExpression(
    LN_Program &program,
    const blender::bNode &node,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
    FloatExpressionCache &float_expression_cache,
    BoolExpressionCache &bool_expression_cache)
{
  InputReader in(program,
                 node,
                 node_definitions,
                 input_links,
                 value_cache,
                 float_expression_cache,
                 bool_expression_cache);
  const bool eye_object = node.custom1 != 1;
  const bool target_object = node.custom2 != 1;
  LN_QueryExpression query;
  query.kind = LN_QueryExpressionKind::LineOfSight;
  query.distance_endpoint_a = eye_object ? LN_DistanceEndpointMode::Object :
                                           LN_DistanceEndpointMode::Position;
  query.distance_endpoint_b = target_object ? LN_DistanceEndpointMode::Object :
                                              LN_DistanceEndpointMode::Position;
  query.condition_bool_expr_index = in.Condition();
  /* Eye: empty is the owner (input0 unset). Target: empty is no target (input3 unset). */
  query.input0 = eye_object ? in.OptionalObject("Eye") : LN_INVALID_INDEX;
  query.input1 = in.Vector(eye_object ? "Eye Offset" : "Eye Position");
  query.input2 = in.Vector("Forward");
  query.input3 = target_object ? in.OptionalObject("Target") : LN_INVALID_INDEX;
  query.input4 = in.Vector(target_object ? "Target Offset" : "Target Position");
  query.float_expr_index = in.Float("Max Distance");
  query.secondary_float_expr_index = in.Float("View Angle");
  query.ignore_list_expr_index = in.Value("Ignore Objects");
  query.int_expr_index = in.Int("Mask");
  query.quaternary_bool_expr_index = in.Bool("Include Sensors");
  query.quinary_bool_expr_index = in.Bool("Hit Backfaces");
  query.tertiary_bool_expr_index = in.Bool("Visualize");
  if (in.failed()) {
    return std::nullopt;
  }
  query.vector_value = VectorFallback(program, query.input1);
  query.secondary_vector_value = query.input2 != LN_INVALID_INDEX ?
                                     VectorFallback(program, query.input2) :
                                     MT_Vector3(0.0f, 1.0f, 0.0f);
  query.tertiary_vector_value = VectorFallback(program, query.input4);
  query.float_value = FloatFallback(program, query.float_expr_index, 50.0f);
  query.secondary_float_value = FloatFallback(
      program, query.secondary_float_expr_index, float(M_PI_2));
  query.int_value = query.int_expr_index != LN_INVALID_INDEX ?
                        IntExpressionConstantFallback(program, query.int_expr_index) :
                        1023;
  query.cache_key = node.identifier;
  AddVisualizeStyleInputs(program,
                          node,
                          node_definitions,
                          input_links,
                          value_cache,
                          query,
                          "Seen Color",
                          "Hidden Color");
  return program.AddQueryExpression(query);
}

std::optional<uint32_t> BuildSweepObjectQueryExpression(
    LN_Program &program,
    const blender::bNode &node,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
    FloatExpressionCache &float_expression_cache,
    BoolExpressionCache &bool_expression_cache)
{
  InputReader in(program,
                 node,
                 node_definitions,
                 input_links,
                 value_cache,
                 float_expression_cache,
                 bool_expression_cache);
  LN_QueryExpression query;
  query.kind = LN_QueryExpressionKind::SweepObject;
  query.condition_bool_expr_index = in.Condition();
  query.input0 = in.OptionalObject("Object");
  query.input1 = in.Vector("Destination");
  /* Rotation: Custom (custom1 1) reads the socket; Current leaves input2 unset. */
  query.input2 = node.custom1 == 1 ? in.Rotation("Rotation") : LN_INVALID_INDEX;
  query.bool_expr_index = in.Bool("Local");
  query.ignore_list_expr_index = in.Value("Ignore Objects");
  query.string_expr_index = in.String("Property");
  query.int_expr_index = in.Int("Mask");
  query.secondary_bool_expr_index = in.Bool("X-Ray");
  query.quaternary_bool_expr_index = in.Bool("Include Sensors");
  query.quinary_bool_expr_index = in.Bool("Hit Backfaces");
  query.tertiary_bool_expr_index = in.Bool("Visualize");
  if (in.failed() || query.input1 == LN_INVALID_INDEX) {
    return std::nullopt;
  }
  query.vector_value = VectorFallback(program, query.input1);
  query.bool_value = BoolExpressionConstantFallback(program, query.bool_expr_index);
  query.int_value = query.int_expr_index != LN_INVALID_INDEX ?
                        IntExpressionConstantFallback(program, query.int_expr_index) :
                        1023;
  query.cache_key = node.identifier;
  AddVisualizeStyleInputs(program, node, node_definitions, input_links, value_cache, query);
  return program.AddQueryExpression(query);
}

/** Keep Distance's outputs: one query reading what the node's last answer left in the tree (its
 * state slot, LN_Program::AcquireKeepDistanceNode: the instruction's too). No physics query: the
 * flush asked it. */
static std::optional<uint32_t> BuildKeepDistanceQueryExpression(LN_Program &program,
                                                                const blender::bNode &node)
{
  LN_QueryExpression query;
  query.kind = LN_QueryExpressionKind::KeepDistance;
  query.int_value = int32_t(program.AcquireKeepDistanceNode(&node));
  query.cache_key = node.identifier;
  return program.AddQueryExpression(query);
}

/** Velocity at Point (2026-11-14): one query its outputs share (LN_QueryExpressionKind::
 * PointVelocity). Point From (custom1) Position reads Position, Vertex Group reads Vertex Group
 * and Vertices (custom2). */
static std::optional<uint32_t> BuildVelocityAtPointQueryExpression(
    LN_Program &program,
    const blender::bNode &node,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
    FloatExpressionCache &float_expression_cache,
    BoolExpressionCache &bool_expression_cache)
{
  InputReader in(program,
                 node,
                 node_definitions,
                 input_links,
                 value_cache,
                 float_expression_cache,
                 bool_expression_cache);
  const bool group = node.custom1 == int16_t(LN_PointVelocitySource::VertexGroup);
  LN_QueryExpression query;
  query.kind = LN_QueryExpressionKind::PointVelocity;
  query.input0 = in.OptionalObject("Object");
  query.int_value = int32_t(group ? LN_PointVelocitySource::VertexGroup :
                                    LN_PointVelocitySource::Position);
  if (group) {
    query.string_expr_index = in.String("Vertex Group");
    query.secondary_int_value = std::clamp(int32_t(node.custom2),
                                           int32_t(LN_PointVelocityPick::Center),
                                           int32_t(LN_PointVelocityPick::EachVertex));
  }
  else {
    query.input1 = in.Vector("Position");
  }
  if (in.failed()) {
    return std::nullopt;
  }
  query.vector_value = VectorFallback(program, query.input1);
  query.cache_key = node.identifier;
  return program.AddQueryExpression(query);
}

namespace {

/** The query of a ray family node (the one its other outputs share: same cache key). */
std::optional<uint32_t> BuildFamilyQuery(const LN_NodeKind kind,
                                         LN_Program &program,
                                         const blender::bNode &node,
                                         const NodeDefinitionMap &node_definitions,
                                         const InputLinkMap &input_links,
                                         ValueCache &value_cache,
                                         FloatExpressionCache &float_cache,
                                         BoolExpressionCache &bool_cache)
{
  switch (kind) {
    case LN_NodeKind::Raycast:
      return BuildRaycastQueryExpression(
          program, node, node_definitions, input_links, value_cache, float_cache, bool_cache);
    case LN_NodeKind::RaycastAll:
      return BuildRaycastAllQueryExpression(
          program, node, node_definitions, input_links, value_cache, float_cache, bool_cache);
    case LN_NodeKind::RaycastFromMesh:
      return BuildRaycastFromMeshQueryExpression(
          program, node, node_definitions, input_links, value_cache, float_cache, bool_cache);
    case LN_NodeKind::ShapeCast:
      return BuildShapeCastQueryExpression(
          program, node, node_definitions, input_links, value_cache, float_cache, bool_cache);
    case LN_NodeKind::ShapeCastAll:
      return BuildShapeCastAllQueryExpression(
          program, node, node_definitions, input_links, value_cache, float_cache, bool_cache);
    case LN_NodeKind::MouseRay:
      return BuildMouseRayQueryExpression(
          program, node, node_definitions, input_links, value_cache, float_cache, bool_cache);
    case LN_NodeKind::CameraRay:
      return BuildCameraRayQueryExpression(
          program, node, node_definitions, input_links, value_cache, float_cache, bool_cache);
    case LN_NodeKind::ProjectileRay:
      return BuildProjectileRayQueryExpression(
          program, node, node_definitions, input_links, value_cache, float_cache, bool_cache);
    case LN_NodeKind::LineOfSight:
      return BuildLineOfSightQueryExpression(
          program, node, node_definitions, input_links, value_cache, float_cache, bool_cache);
    case LN_NodeKind::SweepObject:
      return BuildSweepObjectQueryExpression(
          program, node, node_definitions, input_links, value_cache, float_cache, bool_cache);
    case LN_NodeKind::KeepDistance:
      return BuildKeepDistanceQueryExpression(program, node);
    case LN_NodeKind::VelocityAtPoint:
      return BuildVelocityAtPointQueryExpression(
          program, node, node_definitions, input_links, value_cache, float_cache, bool_cache);
    default:
      return std::nullopt;
  }
}

/** Which outputs of a kind this file builds: every one of the newer nodes, the Hit Part / Hit
 * Material ones of the older casts (their other outputs stay in the per-kind builders). */
bool HandlesOutput(const LN_NodeKind kind, const blender::bNodeSocket &socket)
{
  const auto is = [&](const char *name) {
    return NamesMatch(socket.name, socket.identifier, name);
  };
  switch (kind) {
    case LN_NodeKind::Raycast:
    case LN_NodeKind::ShapeCast:
      return is("Hit Part") || is("Hit Material");
    case LN_NodeKind::RaycastAll:
    case LN_NodeKind::ShapeCastAll:
    case LN_NodeKind::RaycastFromMesh:
      return is("Hit Parts") || is("Hit Materials");
    case LN_NodeKind::MouseRay:
    case LN_NodeKind::CameraRay:
    case LN_NodeKind::ProjectileRay:
    case LN_NodeKind::LineOfSight:
    case LN_NodeKind::SweepObject:
    case LN_NodeKind::AimForArc:
    case LN_NodeKind::RayToPlane:
      return true;
    case LN_NodeKind::KeepDistance:
      return !is("Done");
    case LN_NodeKind::VelocityAtPoint:
      return true;
    default:
      return false;
  }
}

/** Aim for Arc: its three PURE kinds over Start, Target, Gravity (scene's or the node's) and
 * Speed or Arc Height. */
std::optional<uint32_t> BuildAimForArcOutput(LN_Program &program,
                                             const blender::bNode &node,
                                             const blender::bNodeSocket &socket,
                                             const NodeDefinitionMap &node_definitions,
                                             const InputLinkMap &input_links,
                                             ValueCache &value_cache,
                                             FloatExpressionCache &float_cache,
                                             BoolExpressionCache &bool_cache)
{
  InputReader in(
      program, node, node_definitions, input_links, value_cache, float_cache, bool_cache);
  const bool by_speed = node.custom1 != 1;
  const uint32_t start = in.Vector("Start");
  const uint32_t target = in.Vector("Target");
  uint32_t gravity = LN_INVALID_INDEX;
  if (node.custom2 == 1) {
    gravity = in.Vector("Gravity");
  }
  else {
    LN_VectorExpression scene_gravity;
    scene_gravity.kind = LN_VectorExpressionKind::SnapshotGravity;
    gravity = program.AddVectorExpression(scene_gravity);
  }
  const uint32_t amount = in.Float(by_speed ? "Speed" : "Arc Height");
  const uint32_t high_arc = by_speed ? in.Bool("High Arc") :
                                       AddConstantBoolExpression(program, false);
  if (in.failed() || start == LN_INVALID_INDEX || target == LN_INVALID_INDEX ||
      gravity == LN_INVALID_INDEX || amount == LN_INVALID_INDEX)
  {
    return std::nullopt;
  }
  const int32_t mode = by_speed ? 0 : 1;
  const auto is = [&](const char *name) {
    return NamesMatch(socket.name, socket.identifier, name);
  };
  if (is("Launch Velocity")) {
    LN_VectorExpression expression;
    expression.kind = LN_VectorExpressionKind::ArcLaunchVelocity;
    expression.input0 = start;
    expression.input1 = target;
    expression.input2 = gravity;
    expression.float_expr_index = amount;
    expression.bool_expr_index = high_arc;
    expression.int_value = mode;
    return program.AddVectorExpression(expression);
  }
  if (is("Can Reach")) {
    LN_BoolExpression expression;
    expression.kind = LN_BoolExpressionKind::ArcCanReach;
    expression.input0 = start;
    expression.input1 = target;
    expression.input2 = gravity;
    expression.float_expr_index = amount;
    expression.int_value = mode;
    return program.AddBoolExpression(expression);
  }
  if (is("Flight Time")) {
    LN_FloatExpression expression;
    expression.kind = LN_FloatExpressionKind::ArcFlightTime;
    expression.input0 = start;
    expression.input1 = target;
    expression.input2 = gravity;
    expression.input_indices = {amount};
    expression.bool_expr_index = high_arc;
    expression.int_value = mode;
    return program.AddFloatExpression(expression);
  }
  return std::nullopt;
}

/** Ray to Plane: t = Float::RayPlaneDistance(Origin - Plane Point, Direction, Normal) (-1: none);
 * Has Result t >= 0, Distance max(t, 0), Point and Normal from t. */
std::optional<uint32_t> BuildRayToPlaneOutput(LN_Program &program,
                                              const blender::bNode &node,
                                              const blender::bNodeSocket &socket,
                                              const NodeDefinitionMap &node_definitions,
                                              const InputLinkMap &input_links,
                                              ValueCache &value_cache,
                                              FloatExpressionCache &float_cache,
                                              BoolExpressionCache &bool_cache)
{
  InputReader in(
      program, node, node_definitions, input_links, value_cache, float_cache, bool_cache);
  const uint32_t origin = in.Vector("Ray Origin");
  const uint32_t direction = in.Vector("Ray Direction");
  const uint32_t plane_point = in.Vector("Plane Point");
  const uint32_t plane_normal = in.Vector("Plane Normal");
  if (in.failed() || origin == LN_INVALID_INDEX || direction == LN_INVALID_INDEX ||
      plane_point == LN_INVALID_INDEX || plane_normal == LN_INVALID_INDEX)
  {
    return std::nullopt;
  }
  LN_VectorExpression relative;
  relative.kind = LN_VectorExpressionKind::Subtract;
  relative.input0 = origin;
  relative.input1 = plane_point;
  LN_FloatExpression distance;
  distance.kind = LN_FloatExpressionKind::RayPlaneDistance;
  distance.input0 = program.AddVectorExpression(relative);
  distance.input1 = direction;
  distance.input2 = plane_normal;
  const uint32_t t = program.AddFloatExpression(distance);

  const auto is = [&](const char *name) {
    return NamesMatch(socket.name, socket.identifier, name);
  };
  if (is("Has Result")) {
    return AddFloatCompareBoolExpression(program,
                                         t,
                                         AddConstantFloatExpression(program, 0.0f),
                                         LN_FloatCompareOperation::GreaterEqual);
  }
  if (is("Distance")) {
    LN_FloatExpression clamped;
    clamped.kind = LN_FloatExpressionKind::Maximum;
    clamped.input0 = t;
    clamped.input1 = AddConstantFloatExpression(program, 0.0f);
    return program.AddFloatExpression(clamped);
  }
  if (is("Point")) {
    LN_VectorExpression point;
    point.kind = LN_VectorExpressionKind::RayPlanePoint;
    point.input0 = origin;
    point.input1 = direction;
    point.float_expr_index = t;
    return program.AddVectorExpression(point);
  }
  if (is("Normal")) {
    LN_VectorExpression normal;
    normal.kind = LN_VectorExpressionKind::RayPlaneNormal;
    normal.input0 = direction;
    normal.input1 = plane_normal;
    normal.float_expr_index = t;
    return program.AddVectorExpression(normal);
  }
  return std::nullopt;
}

}  // namespace

bool TryCompileRayFamilyOutput(LN_Program &program,
                               const blender::bNode &node,
                               const blender::bNodeSocket &output_socket,
                               const LN_NodeDefinition &definition,
                               const NodeDefinitionMap &node_definitions,
                               const InputLinkMap &input_links,
                               ValueCache &value_cache,
                               FloatExpressionCache &float_expression_cache,
                               BoolExpressionCache &bool_expression_cache,
                               IntExpressionCache &int_expression_cache,
                               StringExpressionCache &string_expression_cache,
                               VectorExpressionCache &vector_expression_cache,
                               ColorExpressionCache & /*color_expression_cache*/,
                               ValueExpressionCache &value_expression_cache,
                               std::optional<uint32_t> &r_result)
{
  const LN_NodeKind kind = definition.kind;
  if (!HandlesOutput(kind, output_socket)) {
    return false;
  }
  const blender::bNodeSocket *socket = &output_socket;
  /* Built before: the per-type caches hold it (as the per-kind builders keep theirs). */
  const auto cached = [&](auto &cache) -> bool {
    const auto found = cache.find(socket);
    if (found == cache.end()) {
      return false;
    }
    r_result = found->second;
    return true;
  };
  switch (output_socket.type) {
    case blender::SOCK_BOOLEAN:
      if (cached(bool_expression_cache)) {
        return true;
      }
      break;
    case blender::SOCK_INT:
      if (cached(int_expression_cache)) {
        return true;
      }
      break;
    case blender::SOCK_FLOAT:
      if (cached(float_expression_cache)) {
        return true;
      }
      break;
    case blender::SOCK_STRING:
      if (cached(string_expression_cache)) {
        return true;
      }
      break;
    case blender::SOCK_VECTOR:
    case blender::SOCK_ROTATION:
      if (cached(vector_expression_cache)) {
        return true;
      }
      break;
    default:
      if (cached(value_expression_cache)) {
        return true;
      }
      break;
  }

  std::optional<uint32_t> result;
  const auto is = [&](const char *name) {
    return NamesMatch(output_socket.name, output_socket.identifier, name);
  };
  if (kind == LN_NodeKind::AimForArc) {
    result = BuildAimForArcOutput(program,
                                  node,
                                  output_socket,
                                  node_definitions,
                                  input_links,
                                  value_cache,
                                  float_expression_cache,
                                  bool_expression_cache);
  }
  else if (kind == LN_NodeKind::RayToPlane) {
    result = BuildRayToPlaneOutput(program,
                                   node,
                                   output_socket,
                                   node_definitions,
                                   input_links,
                                   value_cache,
                                   float_expression_cache,
                                   bool_expression_cache);
  }
  else if (kind == LN_NodeKind::VelocityAtPoint) {
    /* Velocity and Point read the query's result, Speed is the length of that Velocity, the lists
     * read each vertex's result. */
    if (const std::optional<uint32_t> query = BuildFamilyQuery(kind,
                                                               program,
                                                               node,
                                                               node_definitions,
                                                               input_links,
                                                               value_cache,
                                                               float_expression_cache,
                                                               bool_expression_cache))
    {
      const auto velocity_of_query = [&]() {
        LN_VectorExpression velocity;
        velocity.kind = LN_VectorExpressionKind::PhysicsQueryImpactVelocity;
        velocity.input0 = *query;
        return program.AddVectorExpression(velocity);
      };
      if (is("Velocity")) {
        result = velocity_of_query();
      }
      else if (is("Point")) {
        LN_VectorExpression point;
        point.kind = LN_VectorExpressionKind::PhysicsQueryPoint;
        point.input0 = *query;
        result = program.AddVectorExpression(point);
      }
      else if (is("Speed")) {
        LN_FloatExpression speed;
        speed.kind = LN_FloatExpressionKind::VectorLength;
        speed.input0 = velocity_of_query();
        result = program.AddFloatExpression(speed);
      }
      else if (is("Velocities") || is("Points")) {
        LN_ValueExpression list;
        list.kind = is("Velocities") ? LN_ValueExpressionKind::PhysicsQueryImpactVelocities :
                                       LN_ValueExpressionKind::PhysicsQueryPoints;
        list.input0 = *query;
        result = program.AddValueExpression(list);
      }
    }
  }
  else if (const std::optional<uint32_t> query = BuildFamilyQuery(kind,
                                                                  program,
                                                                  node,
                                                                  node_definitions,
                                                                  input_links,
                                                                  value_cache,
                                                                  float_expression_cache,
                                                                  bool_expression_cache))
  {
    /* The hit details this output needs from the physics (kept per query and per scene). */
    uint8_t detail = LN_RAY_QUERY_DETAIL_NONE;
    if (is("Hit Part") || is("Hit Parts")) {
      detail = LN_RAY_QUERY_DETAIL_PART;
    }
    else if (is("Hit Material") || is("Hit Materials")) {
      detail = LN_RAY_QUERY_DETAIL_MATERIAL;
    }
    else if (is("Face Index")) {
      detail = LN_RAY_QUERY_DETAIL_FACE_INDEX;
    }
    else if (is("Has UV") || is("UV")) {
      detail = LN_RAY_QUERY_DETAIL_FACE_INDEX | LN_RAY_QUERY_DETAIL_UV;
    }
    program.AddRayQueryDetailRequirement(node.identifier, detail);

    switch (output_socket.type) {
      case blender::SOCK_BOOLEAN: {
        LN_BoolExpression expression;
        expression.input0 = *query;
        if (is("Done")) {
          expression.kind = LN_BoolExpressionKind::PhysicsQueryDone;
        }
        else if (is("Has Result") || is("Visible") || is("Near")) {
          expression.kind = LN_BoolExpressionKind::PhysicsQueryHit;
        }
        else if (is("Blocked")) {
          expression.kind = LN_BoolExpressionKind::PhysicsQueryBlocked;
        }
        else if (is("Has UV")) {
          expression.kind = LN_BoolExpressionKind::PhysicsQueryHasUV;
        }
        else if (is("Started Overlapping")) {
          expression.kind = LN_BoolExpressionKind::PhysicsQueryStartedOverlapping;
        }
        else {
          break;
        }
        result = program.AddBoolExpression(expression);
        break;
      }
      case blender::SOCK_INT: {
        if (is("Face Index")) {
          LN_IntExpression expression;
          expression.kind = LN_IntExpressionKind::PhysicsQueryFaceIndex;
          expression.input0 = *query;
          result = program.AddIntExpression(expression);
        }
        break;
      }
      case blender::SOCK_FLOAT: {
        LN_FloatExpression expression;
        expression.input0 = *query;
        if (is("Distance") || is("Nearest Distance")) {
          expression.kind = LN_FloatExpressionKind::PhysicsQueryDistance;
        }
        else if (is("Fraction") || is("Safe Fraction")) {
          expression.kind = LN_FloatExpressionKind::PhysicsQueryFraction;
        }
        else if (is("Flight Time")) {
          expression.kind = LN_FloatExpressionKind::PhysicsQueryFlightTime;
        }
        else if (is("Angle")) {
          expression.kind = LN_FloatExpressionKind::PhysicsQueryAngle;
        }
        else {
          break;
        }
        result = program.AddFloatExpression(expression);
        break;
      }
      case blender::SOCK_STRING: {
        if (is("Hit Material")) {
          LN_StringExpression expression;
          expression.kind = LN_StringExpressionKind::PhysicsQueryMaterial;
          expression.input0 = *query;
          result = program.AddStringExpression(expression);
        }
        break;
      }
      case blender::SOCK_VECTOR:
      case blender::SOCK_ROTATION: {
        LN_VectorExpression expression;
        expression.input0 = *query;
        if (is("Hit Point") || is("Nearest Point")) {
          expression.kind = LN_VectorExpressionKind::PhysicsQueryPoint;
        }
        else if (is("Hit Normal") || is("Nearest Normal")) {
          expression.kind = LN_VectorExpressionKind::PhysicsQueryNormal;
        }
        else if (is("Force")) {
          /* Keep Distance keeps its force (push or pull) in the result's impact velocity. */
          expression.kind = LN_VectorExpressionKind::PhysicsQueryImpactVelocity;
        }
        else if (is("Ray Origin")) {
          expression.kind = LN_VectorExpressionKind::PhysicsQueryRayOrigin;
        }
        else if (is("Ray Direction")) {
          expression.kind = LN_VectorExpressionKind::PhysicsQueryDirection;
        }
        else if (is("UV")) {
          expression.kind = LN_VectorExpressionKind::PhysicsQueryUV;
        }
        else if (is("Impact Velocity")) {
          expression.kind = LN_VectorExpressionKind::PhysicsQueryImpactVelocity;
        }
        else if (is("Safe Position")) {
          expression.kind = LN_VectorExpressionKind::PhysicsQueryCastPosition;
        }
        else {
          break;
        }
        result = program.AddVectorExpression(expression);
        break;
      }
      default: {
        LN_ValueExpression expression;
        expression.input0 = *query;
        if (is("Hit Object") || is("Nearest Object")) {
          expression.kind = LN_ValueExpressionKind::PhysicsQueryObject;
        }
        else if (is("Hit Part")) {
          expression.kind = LN_ValueExpressionKind::PhysicsQueryPart;
        }
        else if (is("Blocker")) {
          expression.kind = LN_ValueExpressionKind::PhysicsQueryBlocker;
        }
        else if (is("Parabola")) {
          expression.kind = LN_ValueExpressionKind::ProjectileParabola;
        }
        else if (is("Hit Parts")) {
          expression.kind = LN_ValueExpressionKind::PhysicsQueryParts;
        }
        else if (is("Hit Materials")) {
          expression.kind = LN_ValueExpressionKind::PhysicsQueryMaterials;
        }
        else {
          break;
        }
        result = program.AddValueExpression(expression);
        break;
      }
    }
  }

  switch (output_socket.type) {
    case blender::SOCK_BOOLEAN:
      bool_expression_cache[socket] = result;
      break;
    case blender::SOCK_INT:
      int_expression_cache[socket] = result;
      break;
    case blender::SOCK_FLOAT:
      float_expression_cache[socket] = result;
      break;
    case blender::SOCK_STRING:
      string_expression_cache[socket] = result;
      break;
    case blender::SOCK_VECTOR:
    case blender::SOCK_ROTATION:
      vector_expression_cache[socket] = result;
      break;
    default:
      value_expression_cache[socket] = result;
      break;
  }
  r_result = result;
  return true;
}

}  // namespace ln_compiler
