/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_RuntimeTree.cpp
 *  \ingroup logicnodes
 */

#include "LN_RuntimeTree.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(__SSE__) || defined(_M_X64) || defined(_M_IX86_FP)
#  include <xmmintrin.h>
#  define LN_HAS_SSE_REGISTER_SIMD 1
#else
#  define LN_HAS_SSE_REGISTER_SIMD 0
#endif

#include "DNA_ID.h"
#include "DNA_camera_types.h"
#include "DNA_collection_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_scene_types.h"
#include "DNA_sound_types.h"
#include "DNA_speaker_types.h"

#include "../../../blender/depsgraph/DEG_depsgraph.hh"
#include "BKE_action.hh"
#include "BKE_anim_path.h"
#include "BKE_armature.hh"
#include "BKE_context.hh"
#include "BKE_idprop.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_modifier.hh"
#include "BKE_node.hh"
#include "BKE_node_enum.hh"
#include "BKE_node_runtime.hh"
#include "BKE_object.hh"
#include "BLI_bounds.hh"
#include "BLI_hash.h"
#include "BLI_listbase.h"
#include "BLI_math_matrix.h"
#include "BLI_math_rotation.h"
#include "BLI_path_utils.hh"
#include "BLI_serialize.hh"
#include "BLI_string_utf8.h"
#include "DNA_action_types.h"
#include "DNA_armature_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"

#include "../../Converter/BL_Converter.h"
#include "BL_ArmatureObject.h"
#include "EXP_ArrayValue.h"
#include "EXP_DictValue.h"
#include "EXP_Vector2Value.h"
#include "EXP_Vector3Value.h"
#include "CM_Message.h"
#include "EXP_Vector4Value.h"
#include "KX_Camera.h"
#include "KX_ClientObjectInfo.h"
#include "KX_GameObject.h"
#include "KX_Globals.h"
#include "KX_KetsjiEngine.h"
#include "KX_Light.h"
#include "KX_NavMeshObject.h"
#include "KX_ObjectReferenceValue.h"
#include "KX_PhysicsEngineEnums.h"
#include "KX_RayCast.h"
#include "KX_Scene.h"
#include "LN_BonePoseRotation.h"
#include "LN_CommandBuffer.h"
#include "LN_CommandDiagnostics.h"
#include "LN_FormulaEval.hh"
#include "LN_GameStateMaterial.h"
#include "DEV_Joystick.h"
#include "LN_InputMap.h"
#include "LN_Inventory.h"
#include "LN_Manager.h"
#include "LN_Performance.h"
#include "LN_RuntimeSemantics.h"
#include "LN_TypeConversion.h"
#include "LN_UIService.h"
#include "LN_ValueInterpolation.h"
#include "MT_Matrix3x3.h"
#include "MT_Quaternion.h"
#include "MT_Transform.h"
#include "PHY_DynamicTypes.h"
#include "PHY_ICharacter.h"
#include "PHY_IPhysicsController.h"
#include "PHY_IPhysicsEnvironment.h"
#include "PHY_IVehicle.h"
#include "RAS_ICanvas.h"
#include "RAS_IDisplayArray.h"
#include "RAS_IVertex.h"
#include "RAS_MeshObject.h"
#include "RAS_Polygon.h"
#include "RAS_Rasterizer.h"
#include "SCA_IInputDevice.h"

namespace {

static constexpr float LN_PI = 3.14159265358979323846f;
static constexpr float LN_WAYPOINT_RADIUS = 0.25f;
static constexpr double LN_PATH_UPDATE_PERIOD = 0.5;
static constexpr double LN_TIME_FLOW_EPSILON = 1.0e-6;
static constexpr int32_t LN_RAYCAST_MAX_COLLISION_MASK = (1 << 10) - 1;
static constexpr int32_t LN_RAYCAST_FROM_MESH_MAX_RAYS = 256;

static constexpr bool PhaseTickWasSkipped(const uint64_t previous_tick,
                                          const uint64_t current_tick)
{
  return previous_tick != UINT64_MAX && current_tick > previous_tick &&
         current_tick - previous_tick > 1;
}

static constexpr bool IsContinuousPhaseTick(const uint64_t previous_tick,
                                            const uint64_t current_tick)
{
  return previous_tick != UINT64_MAX && !PhaseTickWasSkipped(previous_tick, current_tick);
}

static_assert(!PhaseTickWasSkipped(0, 1));
static_assert(PhaseTickWasSkipped(0, 2));
static_assert(IsContinuousPhaseTick(UINT64_MAX - 1, UINT64_MAX));
static_assert(!IsContinuousPhaseTick(UINT64_MAX, 0));

static bool UsesJoltPhysics(KX_GameObject *game_object)
{
  KX_Scene *scene = game_object ? game_object->GetScene() : nullptr;
  return scene && scene->GetBlenderScene() &&
         static_cast<e_PhysicsEngine>(scene->GetBlenderScene()->gm.physicsEngine) == UseJolt;
}

static LN_RigidBodyConstraintMatchMode ValidRigidBodyConstraintMatchMode(
    const LN_RigidBodyConstraintMatchMode mode)
{
  switch (mode) {
    case LN_RigidBodyConstraintMatchMode::Exact:
    case LN_RigidBodyConstraintMatchMode::Contains:
    case LN_RigidBodyConstraintMatchMode::All:
      return mode;
  }
  return LN_RigidBodyConstraintMatchMode::Exact;
}

static bool RigidBodyConstraintNameMatches(const std::string &constraint_name,
                                           const std::string &query,
                                           const LN_RigidBodyConstraintMatchMode mode)
{
  if (constraint_name.empty()) {
    return false;
  }
  switch (ValidRigidBodyConstraintMatchMode(mode)) {
    case LN_RigidBodyConstraintMatchMode::Exact:
      return !query.empty() && constraint_name == query;
    case LN_RigidBodyConstraintMatchMode::Contains:
      return !query.empty() && constraint_name.find(query) != std::string::npos;
    case LN_RigidBodyConstraintMatchMode::All:
      return true;
  }
  return false;
}

static const KX_GameObject::RigidBodyConstraintData *FindRigidBodyConstraintMatch(
    const KX_GameObject &game_object,
    const std::string &query,
    const LN_RigidBodyConstraintMatchMode mode)
{
  const LN_RigidBodyConstraintMatchMode valid_mode = ValidRigidBodyConstraintMatchMode(mode);
  if (valid_mode == LN_RigidBodyConstraintMatchMode::Exact) {
    return query.empty() ? nullptr : game_object.FindRigidBodyConstraint(query);
  }
  if (valid_mode == LN_RigidBodyConstraintMatchMode::Contains && query.empty()) {
    return nullptr;
  }
  const std::vector<KX_GameObject::RigidBodyConstraintData> &constraints =
      game_object.GetRigidBodyConstraints();
  const auto match = std::find_if(
      constraints.begin(), constraints.end(), [&](const auto &constraint) {
        return RigidBodyConstraintNameMatches(constraint.m_name, query, valid_mode);
      });
  return match != constraints.end() ? &*match : nullptr;
}

static float ReadRigidBodyFloatAttribute(PHY_IPhysicsController &controller,
                                         const LN_RigidBodyAttribute attribute,
                                         const uint8_t component)
{
  switch (attribute) {
    case LN_RigidBodyAttribute::Mass:
      return float(controller.GetMass());
    case LN_RigidBodyAttribute::Friction:
      return float(controller.GetFriction());
    case LN_RigidBodyAttribute::Restitution:
      return float(controller.GetRestitution());
    case LN_RigidBodyAttribute::Damping:
      return component == 1 ? controller.GetAngularDamping() : controller.GetLinearDamping();
    case LN_RigidBodyAttribute::MinLinearVelocity:
      return controller.GetLinVelocityMin();
    case LN_RigidBodyAttribute::MaxLinearVelocity:
      return controller.GetLinVelocityMax();
    case LN_RigidBodyAttribute::MinAngularVelocity:
      return controller.GetAngularVelocityMin();
    case LN_RigidBodyAttribute::MaxAngularVelocity:
      return controller.GetAngularVelocityMax();
    case LN_RigidBodyAttribute::GravityFactor:
      return controller.GetGravityFactor();
    case LN_RigidBodyAttribute::Ccd:
    case LN_RigidBodyAttribute::Sleeping:
    case LN_RigidBodyAttribute::AxisLocks:
    case LN_RigidBodyAttribute::AllowPhysicsRotation:
      break;
  }
  return 0.0f;
}

static bool ReadRigidBodyBoolAttribute(PHY_IPhysicsController &controller,
                                       const LN_RigidBodyAttribute attribute,
                                       const int32_t output_selector)
{
  if (output_selector == 0) {
    return true;
  }
  switch (attribute) {
    case LN_RigidBodyAttribute::Ccd:
      return output_selector == 1 && controller.GetCcdEnabled();
    case LN_RigidBodyAttribute::Sleeping:
      return output_selector == 2 && controller.GetAllowSleeping();
    case LN_RigidBodyAttribute::AllowPhysicsRotation:
      return output_selector == 1 && controller.GetRigidBodyRotationEnabled();
    case LN_RigidBodyAttribute::Mass:
    case LN_RigidBodyAttribute::Friction:
    case LN_RigidBodyAttribute::Restitution:
    case LN_RigidBodyAttribute::Damping:
    case LN_RigidBodyAttribute::MinLinearVelocity:
    case LN_RigidBodyAttribute::MaxLinearVelocity:
    case LN_RigidBodyAttribute::MinAngularVelocity:
    case LN_RigidBodyAttribute::MaxAngularVelocity:
    case LN_RigidBodyAttribute::GravityFactor:
    case LN_RigidBodyAttribute::AxisLocks:
      break;
  }
  return false;
}

static MT_Vector3 ReadRigidBodyAxisLockAttribute(PHY_IPhysicsController &controller,
                                                 const bool rotation_locks)
{
  bool lock_translation_x = false;
  bool lock_translation_y = false;
  bool lock_translation_z = false;
  bool lock_rotation_x = false;
  bool lock_rotation_y = false;
  bool lock_rotation_z = false;
  controller.GetRigidBodyAxisLocks(lock_translation_x,
                                   lock_translation_y,
                                   lock_translation_z,
                                   lock_rotation_x,
                                   lock_rotation_y,
                                   lock_rotation_z);
  return rotation_locks ? MT_Vector3(lock_rotation_x ? 1.0f : 0.0f,
                                     lock_rotation_y ? 1.0f : 0.0f,
                                     lock_rotation_z ? 1.0f : 0.0f) :
                          MT_Vector3(lock_translation_x ? 1.0f : 0.0f,
                                     lock_translation_y ? 1.0f : 0.0f,
                                     lock_translation_z ? 1.0f : 0.0f);
}

static double TimeFlowStep(const LN_TickContext &context, const bool ignore_timescale)
{
  return std::max(0.0, ignore_timescale ? context.unscaled_dt : context.delta_time);
}

static LN_Event TimeFlowContinuationEvent(const LN_Event event)
{
  /* On Init has no recurring phase. Continue initialization-time latent work on fixed Logic +
   * Physics steps, in lockstep with the compiler's ContinuationEvent(). */
  return event == LN_Event::OnUpdate ? LN_Event::OnUpdate : LN_Event::OnFixedUpdate;
}

static LN_Value MakeNoneValue();

static const LN_Phase10OpcodeFallbackDiagnostic *FindOpcodeFallbackDiagnostic(
    const LN_OpCode opcode)
{
  static const std::array<const LN_Phase10OpcodeFallbackDiagnostic *, size_t(LN_OpCode::Count)>
      diagnostics_by_opcode = []() {
        std::array<const LN_Phase10OpcodeFallbackDiagnostic *, size_t(LN_OpCode::Count)> table =
            {};
        for (const LN_Phase10OpcodeFallbackDiagnostic &diagnostic :
             LN_GetPhase10OpcodeFallbackDiagnostics())
        {
          const size_t index = size_t(diagnostic.opcode);
          if (index < table.size()) {
            table[index] = &diagnostic;
          }
        }
        return table;
      }();

  const size_t index = size_t(opcode);
  return index < diagnostics_by_opcode.size() ? diagnostics_by_opcode[index] : nullptr;
}

static const LN_Phase10ExpressionFallbackDiagnostic *FindExpressionFallbackDiagnostic(
    const LN_RuntimeExpressionFamily family, const uint32_t kind)
{
  for (const LN_Phase10ExpressionFallbackDiagnostic &diagnostic :
       LN_GetPhase10ExpressionFallbackDiagnostics())
  {
    if (diagnostic.family == family && diagnostic.kind == kind) {
      return &diagnostic;
    }
  }
  return nullptr;
}

static LN_RuntimeFallbackReason FallbackReasonFromRequirements(
    const uint32_t fallback_requirements)
{
  if ((fallback_requirements & LN_RUNTIME_FALLBACK_MISSING_SNAPSHOT_CHANNEL) != 0u) {
    return LN_RuntimeFallbackReason::MissingSnapshotChannel;
  }
  if ((fallback_requirements & LN_RUNTIME_FALLBACK_STALE_HANDLE) != 0u) {
    return LN_RuntimeFallbackReason::StaleHandle;
  }
  if ((fallback_requirements & LN_RUNTIME_FALLBACK_ENGINE_SERVICE_BOUNDARY) != 0u) {
    return LN_RuntimeFallbackReason::EngineServiceBoundary;
  }
  if ((fallback_requirements & LN_RUNTIME_FALLBACK_DYNAMIC_LOOKUP) != 0u ||
      (fallback_requirements & LN_RUNTIME_FALLBACK_GENERIC_VALUE_CONVERSION) != 0u)
  {
    return LN_RuntimeFallbackReason::DynamicLookup;
  }
  if ((fallback_requirements & LN_RUNTIME_FALLBACK_MAIN_THREAD_LIVE_READ) != 0u) {
    return LN_RuntimeFallbackReason::MainThreadLiveRead;
  }
  return LN_RuntimeFallbackReason::UnsupportedExpression;
}

static bool RegisterExpressionIRHasProductionBenefit(const LN_RegisterExpressionProgram &ir)
{
  if (!ir.valid || ir.ops.empty()) {
    return false;
  }
  if (ir.fallback_expression_count == 0) {
    return true;
  }

  /* Avoid repeated register probes for expression graphs that are mostly scalar fallback. */
  return ir.scalar_op_count >= ir.fallback_expression_count;
}

static bool QueryExpressionNeedsCommandOrderedEvaluation(const LN_QueryExpressionKind kind)
{
  return kind == LN_QueryExpressionKind::Raycast || kind == LN_QueryExpressionKind::RaycastAll ||
         kind == LN_QueryExpressionKind::RaycastFromMesh ||
         kind == LN_QueryExpressionKind::ShapeCast ||
         kind == LN_QueryExpressionKind::ShapeCastAll ||
         kind == LN_QueryExpressionKind::MouseOver || kind == LN_QueryExpressionKind::MouseRay ||
         kind == LN_QueryExpressionKind::CameraRay ||
         kind == LN_QueryExpressionKind::VectorAverage ||
         kind == LN_QueryExpressionKind::DistanceBetween;
}

static bool QueryExpressionCanWarmCache(const LN_QueryExpression &expression)
{
  return !QueryExpressionNeedsCommandOrderedEvaluation(expression.kind);
}

static void FlushPendingRaycastTransforms(KX_GameObject *game_object,
                                          const LN_TickContext &context)
{
  if (game_object == nullptr || context.command_buffer == nullptr ||
      context.command_execution_key == UINT64_MAX ||
      !context.command_buffer->CanFlushPendingObjectTransformCommands())
  {
    return;
  }

  context.command_buffer->FlushPendingObjectHierarchyTransformCommands(game_object);
}

static blender::Main *CurrentMain()
{
  KX_KetsjiEngine *engine = KX_GetActiveEngine();
  if (engine == nullptr || engine->GetConverter() == nullptr) {
    return nullptr;
  }
  return engine->GetConverter()->GetMain();
}

static std::string game_object_id_name(KX_GameObject *game_object)
{
  if (game_object == nullptr) {
    return std::string();
  }
  if (blender::Object *blender_object = game_object->GetBlenderObject()) {
    return blender_object->id.name + 2;
  }
  return game_object->GetName();
}

static bool game_object_matches_name(KX_GameObject *game_object, const std::string &name)
{
  if (game_object == nullptr) {
    return false;
  }
  if (game_object->GetName() == name) {
    return true;
  }
  if (blender::Object *blender_object = game_object->GetBlenderObject()) {
    return name == blender_object->id.name + 2;
  }
  return false;
}

static bool vector_is_finite(const MT_Vector3 &value)
{
  return std::isfinite(value.x()) && std::isfinite(value.y()) && std::isfinite(value.z());
}

static PHY_IPhysicsController *RaycastOwningPhysicsController(KX_GameObject *object)
{
  /* Compound children and transform-only children can be represented by an ancestor's single
   * physics body. Ignore that owning controller as well, otherwise a ray emitted by the child can
   * immediately hit its own hierarchy even though the child has no controller of its own. */
  for (KX_GameObject *candidate = object; candidate; candidate = candidate->GetParent()) {
    if (PHY_IPhysicsController *controller = candidate->GetPhysicsController()) {
      return controller;
    }
  }
  return nullptr;
}

struct LN_MeshRaySample {
  MT_Vector3 position;
  MT_Vector3 normal;
};

static bool NormalizeFinite(MT_Vector3 &vector)
{
  const float length_squared = vector.length2();
  if (!vector_is_finite(vector) || !std::isfinite(length_squared) ||
      length_squared <= MT_EPSILON * MT_EPSILON)
  {
    return false;
  }
  vector /= std::sqrt(length_squared);
  return vector_is_finite(vector);
}

static bool MeshVertexRaySample(const RAS_MeshObject::SharedVertex &first,
                                const std::vector<RAS_MeshObject::SharedVertex> &shared_vertices,
                                LN_MeshRaySample &r_sample)
{
  if (!first.m_darray || first.m_offset < 0 ||
      size_t(first.m_offset) >= first.m_darray->GetVertexCount())
  {
    return false;
  }
  const RAS_IVertex *first_vertex = first.m_darray->GetVertex(uint32_t(first.m_offset));
  if (!first_vertex) {
    return false;
  }
  r_sample.position = first_vertex->xyz();
  if (!vector_is_finite(r_sample.position)) {
    return false;
  }

  r_sample.normal = MT_Vector3(0.0f, 0.0f, 0.0f);
  for (const RAS_MeshObject::SharedVertex &shared : shared_vertices) {
    if (!shared.m_darray || shared.m_offset < 0 ||
        size_t(shared.m_offset) >= shared.m_darray->GetVertexCount())
    {
      continue;
    }
    const RAS_IVertex *vertex = shared.m_darray->GetVertex(uint32_t(shared.m_offset));
    if (vertex) {
      const MT_Vector3 normal(vertex->getNormal());
      if (vector_is_finite(normal)) {
        r_sample.normal += normal;
      }
    }
  }
  return NormalizeFinite(r_sample.normal);
}

static bool MeshFaceRaySample(const RAS_Polygon &polygon, LN_MeshRaySample &r_sample)
{
  const int vertex_count = polygon.VertexCount();
  if (vertex_count < 3) {
    return false;
  }
  std::vector<MT_Vector3> positions;
  positions.reserve(size_t(vertex_count));
  r_sample.position = MT_Vector3(0.0f, 0.0f, 0.0f);
  for (int index = 0; index < vertex_count; index++) {
    const RAS_IVertex *vertex = polygon.GetVertex(index);
    if (!vertex) {
      return false;
    }
    const MT_Vector3 position = vertex->xyz();
    if (!vector_is_finite(position)) {
      return false;
    }
    positions.push_back(position);
    r_sample.position += position;
  }
  r_sample.position /= float(vertex_count);

  r_sample.normal = MT_Vector3(0.0f, 0.0f, 0.0f);
  const MT_Vector3 &base = positions.front();
  for (int index = 1; index + 1 < vertex_count; index++) {
    r_sample.normal +=
        (positions[size_t(index)] - base).cross(positions[size_t(index + 1)] - base);
  }
  return NormalizeFinite(r_sample.normal);
}

struct LN_MeshRayDomainBlock {
  RAS_MeshObject *mesh = nullptr;
  uint64_t begin = 0;
  uint64_t end = 0;
};

static bool MeshRaySampleAt(const std::vector<LN_MeshRayDomainBlock> &blocks,
                            const LN_MeshRayDomain domain,
                            const uint64_t ordinal,
                            LN_MeshRaySample &r_sample)
{
  const auto block = std::upper_bound(
      blocks.begin(), blocks.end(), ordinal, [](const uint64_t value, const auto &candidate) {
        return value < candidate.end;
      });
  if (block == blocks.end() || ordinal < block->begin || !block->mesh) {
    return false;
  }
  const uint64_t local_index = ordinal - block->begin;
  if (domain == LN_MeshRayDomain::Vertices) {
    if (local_index >= block->mesh->m_sharedvertex_map.size()) {
      return false;
    }
    const std::vector<RAS_MeshObject::SharedVertex> &shared =
        block->mesh->m_sharedvertex_map[size_t(local_index)];
    return !shared.empty() && MeshVertexRaySample(shared.front(), shared, r_sample);
  }
  if (local_index >= uint64_t(std::max(0, block->mesh->NumPolygons()))) {
    return false;
  }
  RAS_Polygon *polygon = block->mesh->GetPolygon(int(local_index));
  return polygon && MeshFaceRaySample(*polygon, r_sample);
}

static std::vector<LN_MeshRaySample> SampleMeshRays(KX_GameObject &source_object,
                                                    const LN_MeshRayDomain domain,
                                                    const size_t max_rays)
{
  std::vector<LN_MeshRayDomainBlock> blocks;
  blocks.reserve(source_object.GetMeshCount());
  uint64_t candidate_count = 0;
  for (unsigned int mesh_index = 0; mesh_index < source_object.GetMeshCount(); mesh_index++) {
    RAS_MeshObject *mesh = source_object.GetMesh(mesh_index);
    if (!mesh) {
      continue;
    }
    const uint64_t count = domain == LN_MeshRayDomain::Vertices ?
                               uint64_t(mesh->m_sharedvertex_map.size()) :
                               uint64_t(std::max(0, mesh->NumPolygons()));
    if (count == 0 || count > std::numeric_limits<uint64_t>::max() - candidate_count) {
      continue;
    }
    blocks.push_back({mesh, candidate_count, candidate_count + count});
    candidate_count += count;
  }

  const size_t output_count = size_t(std::min<uint64_t>(candidate_count, max_rays));
  std::vector<LN_MeshRaySample> samples;
  samples.reserve(output_count);
  if (output_count == 0) {
    return samples;
  }

  /* One sample is selected from each even domain bin. Search outwards from its center only when
   * that candidate is degenerate; regular meshes therefore read exactly Max Rays elements even
   * when the source contains millions of vertices or triangles. Bins never overlap, so ordering
   * and uniqueness remain deterministic. */
  const uint64_t bin_stride = candidate_count / uint64_t(output_count);
  const uint64_t bin_remainder = candidate_count % uint64_t(output_count);
  const auto bin_boundary = [&](const size_t index) {
    const uint64_t i = uint64_t(index);
    return bin_stride * i + (bin_remainder * i) / uint64_t(output_count);
  };
  for (size_t output_index = 0; output_index < output_count; output_index++) {
    const uint64_t bin_begin = bin_boundary(output_index);
    const uint64_t bin_end = bin_boundary(output_index + 1);
    const uint64_t center = bin_begin + (bin_end - bin_begin - 1u) / 2u;
    LN_MeshRaySample sample;
    if (MeshRaySampleAt(blocks, domain, center, sample)) {
      samples.push_back(sample);
      continue;
    }
    for (uint64_t offset = 1; offset < bin_end - bin_begin; offset++) {
      const bool has_lower = offset <= center - bin_begin;
      const bool has_upper = offset < bin_end - center;
      if (has_lower && MeshRaySampleAt(blocks, domain, center - offset, sample)) {
        samples.push_back(sample);
        break;
      }
      if (has_upper && MeshRaySampleAt(blocks, domain, center + offset, sample)) {
        samples.push_back(sample);
        break;
      }
      if (!has_lower && !has_upper) {
        break;
      }
    }
  }
  return samples;
}

static bool BuildMeshNormalMatrix(const MT_Matrix3x3 &basis, MT_Matrix3x3 &r_normal_matrix)
{
  double matrix[3][3];
  double column_length_squared[3] = {0.0, 0.0, 0.0};
  for (int row = 0; row < 3; row++) {
    for (int column = 0; column < 3; column++) {
      matrix[row][column] = double(basis[row][column]);
      if (!std::isfinite(matrix[row][column])) {
        return false;
      }
      column_length_squared[column] += matrix[row][column] * matrix[row][column];
    }
  }
  const double column_length_product = std::sqrt(column_length_squared[0]) *
                                       std::sqrt(column_length_squared[1]) *
                                       std::sqrt(column_length_squared[2]);
  if (!std::isfinite(column_length_product) || column_length_product == 0.0) {
    return false;
  }

  double cofactor[3][3];
  cofactor[0][0] = matrix[1][1] * matrix[2][2] - matrix[1][2] * matrix[2][1];
  cofactor[0][1] = matrix[1][2] * matrix[2][0] - matrix[1][0] * matrix[2][2];
  cofactor[0][2] = matrix[1][0] * matrix[2][1] - matrix[1][1] * matrix[2][0];
  cofactor[1][0] = matrix[0][2] * matrix[2][1] - matrix[0][1] * matrix[2][2];
  cofactor[1][1] = matrix[0][0] * matrix[2][2] - matrix[0][2] * matrix[2][0];
  cofactor[1][2] = matrix[0][1] * matrix[2][0] - matrix[0][0] * matrix[2][1];
  cofactor[2][0] = matrix[0][1] * matrix[1][2] - matrix[0][2] * matrix[1][1];
  cofactor[2][1] = matrix[0][2] * matrix[1][0] - matrix[0][0] * matrix[1][2];
  cofactor[2][2] = matrix[0][0] * matrix[1][1] - matrix[0][1] * matrix[1][0];
  const double determinant = matrix[0][0] * cofactor[0][0] + matrix[0][1] * cofactor[0][1] +
                             matrix[0][2] * cofactor[0][2];
  if (!std::isfinite(determinant) || std::fabs(determinant) <= column_length_product * 1.0e-8) {
    return false;
  }

  float normal[3][3];
  for (int row = 0; row < 3; row++) {
    for (int column = 0; column < 3; column++) {
      normal[row][column] = float(cofactor[row][column] / determinant);
      if (!std::isfinite(normal[row][column])) {
        return false;
      }
    }
  }
  r_normal_matrix = MT_Matrix3x3(normal[0][0],
                                 normal[0][1],
                                 normal[0][2],
                                 normal[1][0],
                                 normal[1][1],
                                 normal[1][2],
                                 normal[2][0],
                                 normal[2][1],
                                 normal[2][2]);
  return true;
}

static void accumulate_finite_position(const MT_Vector3 &position, double sum[3], uint64_t &count)
{
  if (!vector_is_finite(position)) {
    return;
  }
  sum[0] += double(position.x());
  sum[1] += double(position.y());
  sum[2] += double(position.z());
  count++;
}

static blender::Material *ResolveMaterialByValue(const LN_RuntimeTree *runtime_tree,
                                                 const LN_Value &value)
{
  if (value.type != LN_ValueType::DatablockRef) {
    return nullptr;
  }

  if (runtime_tree != nullptr && value.runtime_ref.IsValid()) {
    if (blender::ID *id = runtime_tree->ResolveDatablockRef(value.runtime_ref)) {
      if (id->name[0] == 'M' && id->name[1] == 'A') {
        return reinterpret_cast<blender::Material *>(id);
      }
      return nullptr;
    }
  }

  blender::Main *bmain = CurrentMain();
  if (bmain == nullptr || value.reference_name.empty()) {
    return nullptr;
  }
  return static_cast<blender::Material *>(blender::BLI_findstring(
      &bmain->materials, value.reference_name.c_str(), offsetof(blender::ID, name) + 2));
}

static blender::Collection *ResolveCollectionByValue(const LN_RuntimeTree *runtime_tree,
                                                     const LN_Value &value)
{
  if (value.type != LN_ValueType::CollectionRef) {
    return nullptr;
  }

  if (runtime_tree != nullptr && value.runtime_ref.IsValid()) {
    if (blender::Collection *collection = runtime_tree->ResolveCollectionRef(value.runtime_ref)) {
      return collection;
    }
  }

  blender::Main *bmain = CurrentMain();
  if (bmain == nullptr || value.reference_name.empty()) {
    return nullptr;
  }
  return static_cast<blender::Collection *>(blender::BLI_findstring(
      &bmain->collections, value.reference_name.c_str(), offsetof(blender::ID, name) + 2));
}

static blender::Material *ObjectMaterialAtSlot(KX_GameObject *game_object, const int32_t slot)
{
  if (game_object == nullptr || slot < 0) {
    return nullptr;
  }
  blender::Object *object = game_object->GetBlenderObject();
  if (object == nullptr || slot >= object->totcol) {
    return nullptr;
  }
  return BKE_object_material_get(object, short(slot + 1));
}

static std::string MaterialFilterNameFromValue(const LN_RuntimeTree *runtime_tree,
                                               const LN_Value &value)
{
  if (blender::Material *material = ResolveMaterialByValue(runtime_tree, value)) {
    return material->id.name + 2;
  }
  if (value.type == LN_ValueType::DatablockRef && !value.reference_name.empty()) {
    return value.reference_name;
  }
  return std::string();
}

static bool MaterialNameMatchesFilter(const std::string &material_name,
                                      const std::string &filter_name)
{
  if (filter_name.empty()) {
    return true;
  }
  if (material_name == filter_name) {
    return true;
  }
  if (material_name.size() > 2 && material_name[0] == 'M' && material_name[1] == 'A' &&
      material_name.compare(2, std::string::npos, filter_name) == 0)
  {
    return true;
  }
  if (filter_name.size() > 2 && filter_name[0] == 'M' && filter_name[1] == 'A' &&
      filter_name.compare(2, std::string::npos, material_name) == 0)
  {
    return true;
  }
  return false;
}

static bool GameObjectUsesMaterial(KX_GameObject *game_object, const std::string &filter_name)
{
  if (filter_name.empty()) {
    return true;
  }
  if (game_object == nullptr) {
    return false;
  }
  for (unsigned int mesh_index = 0; mesh_index < game_object->GetMeshCount(); mesh_index++) {
    RAS_MeshObject *mesh = game_object->GetMesh(mesh_index);
    if (mesh == nullptr) {
      continue;
    }
    for (int material_index = 0; material_index < mesh->NumMaterials(); material_index++) {
      if (MaterialNameMatchesFilter(mesh->GetMaterialName(material_index), filter_name)) {
        return true;
      }
    }
  }
  return false;
}

static blender::bNode *FindNodeByUserName(blender::bNodeTree *ntree, const std::string &name)
{
  if (ntree == nullptr || name.empty()) {
    return nullptr;
  }
  if (blender::bNode *node = blender::bke::node_find_node_by_name(*ntree, name)) {
    return node;
  }
  for (blender::bNode &node : ntree->nodes) {
    if (name == node.name || (node.label[0] != '\0' && name == node.label)) {
      return &node;
    }
  }
  return nullptr;
}

static blender::bNodeSocket *FindInputSocketByIdentifierOrName(blender::bNode *node,
                                                               const std::string &name)
{
  if (node == nullptr || name.empty()) {
    return nullptr;
  }
  for (blender::bNodeSocket &socket : node->inputs) {
    if (name == socket.identifier || name == socket.name) {
      return &socket;
    }
  }
  return nullptr;
}

enum class EditorNodeValueTarget : uint32_t {
  GeometryModifier = 1,
  CompositorScene = 2,
  CompositorTree = 3,
};

static blender::bNodeTree *ResolveEditorNodeValueTree(const EditorNodeValueTarget target,
                                                      KX_GameObject *game_object,
                                                      const std::string &target_name)
{
  if (target == EditorNodeValueTarget::GeometryModifier) {
    blender::Object *object = game_object != nullptr ? game_object->GetBlenderObject() : nullptr;
    blender::ModifierData *modifier = object != nullptr && !target_name.empty() ?
                                          BKE_modifiers_findby_name(object, target_name.c_str()) :
                                          nullptr;
    if (modifier == nullptr || modifier->type != blender::eModifierType_Nodes) {
      return nullptr;
    }
    blender::bNodeTree *tree =
        reinterpret_cast<blender::NodesModifierData *>(modifier)->node_group;
    return tree != nullptr && tree->type == blender::NTREE_GEOMETRY ? tree : nullptr;
  }

  blender::Main *bmain = CurrentMain();
  if (bmain == nullptr || target_name.empty()) {
    return nullptr;
  }
  if (target == EditorNodeValueTarget::CompositorScene) {
    blender::Scene *scene = static_cast<blender::Scene *>(blender::BLI_findstring(
        &bmain->scenes, target_name.c_str(), offsetof(blender::ID, name) + 2));
    blender::bNodeTree *tree = scene != nullptr ? scene->compositing_node_group : nullptr;
    return tree != nullptr && tree->type == blender::NTREE_COMPOSIT ? tree : nullptr;
  }
  if (target == EditorNodeValueTarget::CompositorTree) {
    blender::bNodeTree *tree = static_cast<blender::bNodeTree *>(blender::BLI_findstring(
        &bmain->nodetrees, target_name.c_str(), offsetof(blender::ID, name) + 2));
    return tree != nullptr && tree->type == blender::NTREE_COMPOSIT ? tree : nullptr;
  }
  return nullptr;
}

static LN_Value MakeDatablockValue(LN_RuntimeTree *runtime_tree, blender::ID *id)
{
  LN_Value value = MakeNoneValue();
  if (id == nullptr) {
    return value;
  }
  value.type = LN_ValueType::DatablockRef;
  value.exists = true;
  value.reference_name = id->name + 2;
  if (runtime_tree != nullptr) {
    value.runtime_ref = runtime_tree->MakeDatablockRef(id, value.reference_name);
  }
  return value;
}

static LN_Value MakeMaterialValue(LN_RuntimeTree *runtime_tree, blender::Material *material)
{
  return MakeDatablockValue(runtime_tree, material != nullptr ? &material->id : nullptr);
}

static LN_Value MakeObjectValue(LN_RuntimeTree *runtime_tree, KX_GameObject *object)
{
  LN_Value value = MakeNoneValue();
  if (object == nullptr || runtime_tree == nullptr) {
    return value;
  }
  value.type = LN_ValueType::ObjectRef;
  value.exists = true;
  value.reference_name = object->GetName();
  value.runtime_ref = runtime_tree->MakeObjectRef(object, value.reference_name);
  return value;
}

static LN_Value MakeVectorValue(const MT_Vector3 &vector)
{
  LN_Value value;
  value.type = LN_ValueType::Vector;
  value.exists = true;
  value.vector_value = vector;
  return value;
}

static LN_Value MakeBoolValue(const bool bool_value)
{
  LN_Value value;
  value.type = LN_ValueType::Bool;
  value.exists = true;
  value.bool_value = bool_value;
  return value;
}

static LN_Value MakeIntValue(const int32_t int_value)
{
  LN_Value value;
  value.type = LN_ValueType::Int;
  value.exists = true;
  value.int_value = int_value;
  return value;
}

static LN_Value MakeFloatValue(const float float_value)
{
  LN_Value value;
  value.type = LN_ValueType::Float;
  value.exists = true;
  value.float_value = float_value;
  return value;
}

static LN_Value MakeStringValue(const std::string &string_value)
{
  LN_Value value;
  value.type = LN_ValueType::String;
  value.exists = true;
  value.string_value = string_value;
  return value;
}

static blender::ID *SocketDefaultId(const blender::bNodeSocket &socket)
{
  switch (socket.type) {
    case blender::SOCK_OBJECT:
      return reinterpret_cast<blender::ID *>(
          static_cast<const blender::bNodeSocketValueObject *>(socket.default_value)->value);
    case blender::SOCK_IMAGE:
      return reinterpret_cast<blender::ID *>(
          static_cast<const blender::bNodeSocketValueImage *>(socket.default_value)->value);
    case blender::SOCK_COLLECTION:
      return reinterpret_cast<blender::ID *>(
          static_cast<const blender::bNodeSocketValueCollection *>(socket.default_value)->value);
    case blender::SOCK_TEXTURE:
      return reinterpret_cast<blender::ID *>(
          static_cast<const blender::bNodeSocketValueTexture *>(socket.default_value)->value);
    case blender::SOCK_MATERIAL:
      return reinterpret_cast<blender::ID *>(
          static_cast<const blender::bNodeSocketValueMaterial *>(socket.default_value)->value);
    case blender::SOCK_FONT:
      return reinterpret_cast<blender::ID *>(
          static_cast<const blender::bNodeSocketValueFont *>(socket.default_value)->value);
    case blender::SOCK_SCENE:
      return reinterpret_cast<blender::ID *>(
          static_cast<const blender::bNodeSocketValueScene *>(socket.default_value)->value);
    case blender::SOCK_TEXT_ID:
      return reinterpret_cast<blender::ID *>(
          static_cast<const blender::bNodeSocketValueText *>(socket.default_value)->value);
    case blender::SOCK_MASK:
      return reinterpret_cast<blender::ID *>(
          static_cast<const blender::bNodeSocketValueMask *>(socket.default_value)->value);
    case blender::SOCK_SOUND:
      return reinterpret_cast<blender::ID *>(
          static_cast<const blender::bNodeSocketValueSound *>(socket.default_value)->value);
    default:
      return nullptr;
  }
}

static LN_Value ReadSocketDefaultValue(const blender::bNodeSocket &socket,
                                       LN_RuntimeTree *runtime_tree = nullptr)
{
  LN_Value value = MakeNoneValue();
  if (socket.default_value == nullptr) {
    return value;
  }
  switch (socket.type) {
    case blender::SOCK_BOOLEAN: {
      const auto *socket_value = static_cast<const blender::bNodeSocketValueBoolean *>(
          socket.default_value);
      value.type = LN_ValueType::Bool;
      value.bool_value = socket_value->value != 0;
      value.exists = true;
      return value;
    }
    case blender::SOCK_INT: {
      const auto *socket_value = static_cast<const blender::bNodeSocketValueInt *>(
          socket.default_value);
      value.type = LN_ValueType::Int;
      value.int_value = socket_value->value;
      value.exists = true;
      return value;
    }
    case blender::SOCK_MENU: {
      const auto *socket_value = static_cast<const blender::bNodeSocketValueMenu *>(
          socket.default_value);
      if (socket_value->enum_items == nullptr || socket_value->has_conflict() ||
          socket_value->enum_items->find_item_by_identifier(socket_value->value) == nullptr)
      {
        return value;
      }
      value.type = LN_ValueType::Int;
      value.int_value = socket_value->value;
      value.exists = true;
      return value;
    }
    case blender::SOCK_INT_VECTOR: {
      const auto *socket_value = static_cast<const blender::bNodeSocketValueIntVector *>(
          socket.default_value);
      const int dimensions = std::clamp(socket_value->dimensions, 1, 3);
      value.type = LN_ValueType::Vector;
      value.vector_value = MT_Vector3(socket_value->value[0],
                                      dimensions >= 2 ? socket_value->value[1] : 0,
                                      dimensions >= 3 ? socket_value->value[2] : 0);
      value.exists = true;
      return value;
    }
    case blender::SOCK_FLOAT: {
      const auto *socket_value = static_cast<const blender::bNodeSocketValueFloat *>(
          socket.default_value);
      value.type = LN_ValueType::Float;
      value.float_value = socket_value->value;
      value.exists = true;
      return value;
    }
    case blender::SOCK_VECTOR: {
      const auto *socket_value = static_cast<const blender::bNodeSocketValueVector *>(
          socket.default_value);
      value.type = LN_ValueType::Vector;
      value.vector_value = MT_Vector3(
          socket_value->value[0], socket_value->value[1], socket_value->value[2]);
      value.exists = true;
      return value;
    }
    case blender::SOCK_ROTATION: {
      const auto *socket_value = static_cast<const blender::bNodeSocketValueRotation *>(
          socket.default_value);
      const MT_Vector3 euler(socket_value->value_euler[0],
                             socket_value->value_euler[1],
                             socket_value->value_euler[2]);
      value.type = LN_ValueType::Rotation;
      value.SetRotation(LN_Rotation::FromEulerXYZ(euler), euler);
      value.exists = true;
      return value;
    }
    case blender::SOCK_RGBA: {
      const auto *socket_value = static_cast<const blender::bNodeSocketValueRGBA *>(
          socket.default_value);
      value.type = LN_ValueType::Color;
      value.color_value = MT_Vector4(socket_value->value[0],
                                     socket_value->value[1],
                                     socket_value->value[2],
                                     socket_value->value[3]);
      value.exists = true;
      return value;
    }
    case blender::SOCK_STRING: {
      const auto *socket_value = static_cast<const blender::bNodeSocketValueString *>(
          socket.default_value);
      value.type = LN_ValueType::String;
      value.string_value = socket_value->value;
      value.exists = true;
      return value;
    }
    case blender::SOCK_OBJECT:
    case blender::SOCK_IMAGE:
    case blender::SOCK_COLLECTION:
    case blender::SOCK_TEXTURE:
    case blender::SOCK_MATERIAL:
    case blender::SOCK_FONT:
    case blender::SOCK_SCENE:
    case blender::SOCK_TEXT_ID:
    case blender::SOCK_MASK:
    case blender::SOCK_SOUND: {
      blender::ID *id = SocketDefaultId(socket);
      if (id == nullptr) {
        return value;
      }
      if (runtime_tree != nullptr && socket.type == blender::SOCK_OBJECT) {
        KX_GameObject *owner = runtime_tree->GetGameObject();
        KX_Scene *scene = owner != nullptr ? owner->GetScene() : nullptr;
        KX_GameObject *game_object = scene != nullptr ?
                                         scene->GetGameObjectFromObject(
                                             reinterpret_cast<blender::Object *>(id)) :
                                         nullptr;
        if (game_object != nullptr) {
          return MakeObjectValue(runtime_tree, game_object);
        }
      }
      if (runtime_tree != nullptr && socket.type == blender::SOCK_COLLECTION) {
        value.type = LN_ValueType::CollectionRef;
        value.exists = true;
        value.reference_name = id->name + 2;
        value.runtime_ref = runtime_tree->MakeCollectionRef(
            reinterpret_cast<blender::Collection *>(id), value.reference_name);
        return value;
      }
      if (runtime_tree != nullptr && socket.type == blender::SOCK_SCENE) {
        KX_GameObject *owner = runtime_tree->GetGameObject();
        KX_Scene *scene = owner != nullptr ? owner->GetScene() : nullptr;
        if (scene != nullptr && scene->GetBlenderScene() == reinterpret_cast<blender::Scene *>(id))
        {
          value.type = LN_ValueType::SceneRef;
          value.exists = true;
          value.reference_name = id->name + 2;
          value.runtime_ref = runtime_tree->MakeSceneRef(scene, value.reference_name);
          return value;
        }
      }
      return MakeDatablockValue(runtime_tree, id);
    }
    default:
      return value;
  }
}

static LN_Value MaterialParameterValueFromFloats(const blender::bNodeSocket &socket,
                                                 const std::array<float, 4> &values)
{
  LN_Value value = MakeNoneValue();
  switch (socket.type) {
    case blender::SOCK_BOOLEAN:
      value.type = LN_ValueType::Bool;
      value.bool_value = values[0] >= 0.5f;
      value.exists = true;
      return value;
    case blender::SOCK_INT:
      value.type = LN_ValueType::Int;
      value.int_value = int(std::lround(values[0]));
      value.exists = true;
      return value;
    case blender::SOCK_FLOAT:
      value.type = LN_ValueType::Float;
      value.float_value = values[0];
      value.exists = true;
      return value;
    case blender::SOCK_VECTOR:
      value.type = LN_ValueType::Vector;
      value.vector_value = MT_Vector3(values[0], values[1], values[2]);
      value.exists = true;
      return value;
    case blender::SOCK_ROTATION: {
      const MT_Vector3 euler(values[0], values[1], values[2]);
      value.type = LN_ValueType::Rotation;
      value.SetRotation(LN_Rotation::FromEulerXYZ(euler), euler);
      value.exists = true;
      return value;
    }
    case blender::SOCK_RGBA:
      value.type = LN_ValueType::Color;
      value.color_value = MT_Vector4(values[0], values[1], values[2], values[3]);
      value.exists = true;
      return value;
    default:
      return value;
  }
}

static LN_Value ReadMaterialParameterValue(blender::Object *object,
                                           blender::Material *material,
                                           blender::bNode &node,
                                           blender::bNodeSocket &socket,
                                           LN_RuntimeTree *runtime_tree)
{
  if (!upbge::logic_nodes::game_state::material_parameter_socket_supports_object_attribute(socket))
  {
    return MakeNoneValue();
  }

  std::array<float, 4> values{};
  const std::string attribute_name =
      upbge::logic_nodes::game_state::material_parameter_attribute_name(material, node, socket);
  if (upbge::logic_nodes::game_state::read_object_material_parameter_values(
          object, attribute_name, values))
  {
    return MaterialParameterValueFromFloats(socket, values);
  }
  return ReadSocketDefaultValue(socket, runtime_tree);
}

static MT_Vector3 PathWaypoint(const float *path, const int index)
{
  return MT_Vector3(&path[3 * index]);
}

static KX_NavMeshObject *ResolveNavMeshObject(KX_GameObject *game_object)
{
  return game_object ? dynamic_cast<KX_NavMeshObject *>(game_object) : nullptr;
}

static MT_Vector3 ProjectilePoint(const MT_Vector3 &velocity,
                                  const MT_Vector3 &origin,
                                  const MT_Vector3 &gravity,
                                  const float time)
{
  return gravity * (0.5f * time * time) + velocity * time + origin;
}

static bool ValueAsWorldVector(const LN_Value &value, MT_Vector3 &r_vector)
{
  if (value.type == LN_ValueType::Vector && value.exists) {
    r_vector = value.vector_value;
    return true;
  }
  return false;
}

static bool CollectPathPoints(const LN_Value &path_value, std::vector<MT_Vector3> &r_points)
{
  r_points.clear();
  if (path_value.type != LN_ValueType::List || !path_value.exists) {
    return false;
  }

  for (const LN_Value &item : path_value.list_value) {
    MT_Vector3 point(0.0f, 0.0f, 0.0f);
    if (ValueAsWorldVector(item, point)) {
      r_points.push_back(point);
    }
  }
  return !r_points.empty();
}

static bool StringStartsWith(const std::string &value, const std::string &prefix)
{
  return prefix.size() <= value.size() && value.compare(0, prefix.size(), prefix) == 0;
}

static bool StringEndsWith(const std::string &value, const std::string &suffix)
{
  return suffix.size() <= value.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

static int32_t CountStringOccurrences(const std::string &value, const std::string &needle)
{
  if (needle.empty()) {
    return 0;
  }

  int32_t count = 0;
  size_t pos = 0;
  while ((pos = value.find(needle, pos)) != std::string::npos) {
    count++;
    pos += needle.size();
  }
  return count;
}

static std::string ReplaceStringOccurrences(const std::string &value,
                                            const std::string &needle,
                                            const std::string &replacement)
{
  if (needle.empty()) {
    return value;
  }

  std::string result = value;
  size_t pos = 0;
  while ((pos = result.find(needle, pos)) != std::string::npos) {
    result.replace(pos, needle.size(), replacement);
    pos += replacement.size();
  }
  return result;
}

static std::string ToCaseString(const std::string &value, const bool uppercase)
{
  std::string result = value;
  for (char &character : result) {
    const unsigned char byte = static_cast<unsigned char>(character);
    character = static_cast<char>(uppercase ? std::toupper(byte) : std::tolower(byte));
  }
  return result;
}

static LN_RuntimeRef MakeInvalidRef(const LN_RuntimeRefKind kind, const std::string &debug_name)
{
  LN_RuntimeRef runtime_ref;
  runtime_ref.kind = kind;
  runtime_ref.debug_name = debug_name;
  return runtime_ref;
}

static LN_Value MakeNoneValue()
{
  LN_Value value;
  value.type = LN_ValueType::None;
  value.exists = false;
  return value;
}

static short EulerOrderFromLogicIndex(const uint32_t order_index)
{
  return short(blender::EULER_ORDER_XYZ + std::min<uint32_t>(order_index, 5));
}

static void MatrixToFloat3x3(const MT_Matrix3x3 &matrix, float r_matrix[3][3])
{
  for (int row = 0; row < 3; row++) {
    for (int column = 0; column < 3; column++) {
      r_matrix[row][column] = float(matrix[row][column]);
    }
  }
}

static MT_Matrix3x3 MatrixFromFloat3x3(const float matrix[3][3])
{
  return MT_Matrix3x3(matrix[0][0],
                      matrix[0][1],
                      matrix[0][2],
                      matrix[1][0],
                      matrix[1][1],
                      matrix[1][2],
                      matrix[2][0],
                      matrix[2][1],
                      matrix[2][2]);
}

static MT_Matrix3x3 MatrixFromEuler(const MT_Vector3 &euler, const uint32_t order_index = 0)
{
  float input[3] = {float(euler.x()), float(euler.y()), float(euler.z())};
  float matrix[3][3];
  blender::eulO_to_mat3(matrix, input, EulerOrderFromLogicIndex(order_index));
  return MatrixFromFloat3x3(matrix);
}

static MT_Vector3 EulerFromMatrix(const MT_Matrix3x3 &matrix, const uint32_t order_index = 0)
{
  float input[3][3];
  MatrixToFloat3x3(matrix, input);
  float euler[3];
  blender::mat3_to_eulO(euler, EulerOrderFromLogicIndex(order_index), input);
  return MT_Vector3(euler[0], euler[1], euler[2]);
}

static MT_Vector3 InterpolateRotationEuler(const MT_Vector3 &from,
                                           const MT_Vector3 &to,
                                           float factor)
{
  if (!std::isfinite(factor)) {
    factor = 0.0f;
  }
  return from + (to - from) * factor;
}

static bool MatrixHasNonZeroElement(const MT_Matrix3x3 &matrix)
{
  for (int row = 0; row < 3; row++) {
    for (int column = 0; column < 3; column++) {
      if (matrix[row][column] != 0.0f) {
        return true;
      }
    }
  }
  return false;
}

using GlobalPropertyStore = std::map<std::string, std::map<std::string, LN_Value>>;

static GlobalPropertyStore &GlobalProperties()
{
  static GlobalPropertyStore store;
  return store;
}

static std::mutex &GlobalPropertiesMutex()
{
  static std::mutex mutex;
  return mutex;
}
static std::unordered_set<std::string> &LoadedGlobalPropertyCategories()
{
  static std::unordered_set<std::string> loaded_categories;
  return loaded_categories;
}

static std::shared_ptr<blender::io::serialize::Value> ValueToJsonValue(const LN_Value &value);

static LN_Value JsonValueToValue(const blender::io::serialize::Value &json_value)
{
  using namespace blender::io::serialize;

  switch (json_value.type()) {
    case eValueType::Boolean: {
      LN_Value value;
      value.type = LN_ValueType::Bool;
      value.exists = true;
      value.bool_value = json_value.as_boolean_value()->value();
      return value;
    }
    case eValueType::Int: {
      LN_Value value;
      value.type = LN_ValueType::Int;
      value.exists = true;
      value.int_value = int32_t(json_value.as_int_value()->value());
      return value;
    }
    case eValueType::Double: {
      LN_Value value;
      value.type = LN_ValueType::Float;
      value.exists = true;
      value.float_value = float(json_value.as_double_value()->value());
      return value;
    }
    case eValueType::String: {
      LN_Value value;
      value.type = LN_ValueType::String;
      value.exists = true;
      value.string_value = json_value.as_string_value()->value();
      return value;
    }
    case eValueType::Array: {
      LN_Value value;
      value.type = LN_ValueType::List;
      value.exists = true;
      for (const std::shared_ptr<Value> &item : json_value.as_array_value()->elements()) {
        if (item) {
          value.list_value.push_back(JsonValueToValue(*item));
        }
      }
      return value;
    }
    case eValueType::Dictionary: {
      LN_Value value;
      value.type = LN_ValueType::Dict;
      value.exists = true;
      for (const DictionaryValue::Item &item : json_value.as_dictionary_value()->elements()) {
        if (item.second) {
          value.dict_value[item.first] = JsonValueToValue(*item.second);
        }
      }
      return value;
    }
    case eValueType::Null:
    case eValueType::Enum:
      return MakeNoneValue();
  }
  return MakeNoneValue();
}

static std::shared_ptr<blender::io::serialize::Value> ValueToJsonValue(const LN_Value &value)
{
  using namespace blender::io::serialize;

  if (!value.exists) {
    return std::make_shared<NullValue>();
  }

  switch (value.type) {
    case LN_ValueType::Bool:
      return std::make_shared<BooleanValue>(value.bool_value);
    case LN_ValueType::Int:
      return std::make_shared<IntValue>(value.int_value);
    case LN_ValueType::Float:
      return std::make_shared<DoubleValue>(double(value.float_value));
    case LN_ValueType::String:
      return std::make_shared<StringValue>(value.string_value);
    case LN_ValueType::List: {
      std::shared_ptr<ArrayValue> array_value = std::make_shared<ArrayValue>();
      for (const LN_Value &item : value.list_value) {
        array_value->append(ValueToJsonValue(item));
      }
      return array_value;
    }
    case LN_ValueType::Dict: {
      std::shared_ptr<DictionaryValue> dict_value = std::make_shared<DictionaryValue>();
      for (const auto &item : value.dict_value) {
        dict_value->append(item.first, ValueToJsonValue(item.second));
      }
      return dict_value;
    }
    case LN_ValueType::Vector: {
      std::shared_ptr<ArrayValue> array_value = std::make_shared<ArrayValue>();
      array_value->append_double(value.vector_value.x());
      array_value->append_double(value.vector_value.y());
      array_value->append_double(value.vector_value.z());
      return array_value;
    }
    case LN_ValueType::Vector4: {
      std::shared_ptr<ArrayValue> array_value = std::make_shared<ArrayValue>();
      array_value->append_double(value.vector4_value.x());
      array_value->append_double(value.vector4_value.y());
      array_value->append_double(value.vector4_value.z());
      array_value->append_double(value.vector4_value.w());
      return array_value;
    }
    case LN_ValueType::Matrix: {
      std::shared_ptr<ArrayValue> rows = std::make_shared<ArrayValue>();
      for (int row = 0; row < 3; row++) {
        std::shared_ptr<ArrayValue> columns = std::make_shared<ArrayValue>();
        for (int column = 0; column < 3; column++) {
          columns->append_double(value.matrix_value[row][column]);
        }
        rows->append(columns);
      }
      return rows;
    }
    case LN_ValueType::Color: {
      std::shared_ptr<ArrayValue> array_value = std::make_shared<ArrayValue>();
      array_value->append_double(value.color_value.x());
      array_value->append_double(value.color_value.y());
      array_value->append_double(value.color_value.z());
      array_value->append_double(value.color_value.w());
      return array_value;
    }
    case LN_ValueType::None:
    case LN_ValueType::Rotation:
    case LN_ValueType::ObjectRef:
    case LN_ValueType::SceneRef:
    case LN_ValueType::CollectionRef:
    case LN_ValueType::DatablockRef:
    case LN_ValueType::Generic:
      return std::make_shared<NullValue>();
  }
  return std::make_shared<NullValue>();
}

static std::string ResolveVariableFilePath(std::string path, std::string file_name)
{
  if (path.empty()) {
    path = "//Data";
  }
  if (file_name.empty()) {
    file_name = "variables";
  }
  if (!StringEndsWith(file_name, ".json")) {
    file_name += ".json";
  }

  char expanded[FILE_MAX];
  std::snprintf(expanded, sizeof(expanded), "%s", path.c_str());
  blender::BLI_path_abs(expanded, KX_GetMainPath().c_str());
  blender::BLI_path_normalize(expanded);
  return (std::filesystem::path(expanded) / file_name).string();
}

/** Variable files as last read. Load Variable can run every frame: a file is parsed again only
 * when it changed on disk (its write time or size), also when something else wrote it. */
struct VariableFileCacheEntry {
  std::filesystem::file_time_type write_time;
  uintmax_t size = 0;
  LN_Value dict;
};

static std::mutex &VariableFileCacheMutex()
{
  static std::mutex mutex;
  return mutex;
}

static std::unordered_map<std::string, VariableFileCacheEntry> &VariableFileCache()
{
  static std::unordered_map<std::string, VariableFileCacheEntry> cache;
  return cache;
}

static LN_Value LoadVariableDictFromFile(const std::string &path, const std::string &file_name)
{
  using namespace blender::io::serialize;

  const std::string filepath = ResolveVariableFilePath(path, file_name);
  LN_Value empty;
  empty.type = LN_ValueType::Dict;
  empty.exists = true;

  /* Locked for the whole read, as saving is for the whole write: a read on another thread never
   * caches the old file while it is being replaced (a quick rewrite can keep the write time). */
  std::lock_guard<std::mutex> lock(VariableFileCacheMutex());
  std::error_code error;
  const std::filesystem::file_time_type write_time = std::filesystem::last_write_time(filepath,
                                                                                      error);
  const uintmax_t size = error ? 0 : std::filesystem::file_size(filepath, error);
  const bool readable = !error;
  auto &cache = VariableFileCache();
  const auto cached = cache.find(filepath);
  if (cached != cache.end()) {
    if (readable && cached->second.write_time == write_time && cached->second.size == size) {
      return cached->second.dict;
    }
    cache.erase(cached);
  }
  if (!readable) {
    return empty;
  }

  std::shared_ptr<Value> root = read_json_file(filepath);
  LN_Value dict = root && root->type() == eValueType::Dictionary ? JsonValueToValue(*root) :
                                                                    empty;
  cache[filepath] = {write_time, size, dict};
  return dict;
}

static void SaveVariableDictToFile(const std::string &path,
                                   const std::string &file_name,
                                   const LN_Value &dict_value)
{
  const std::string filepath = ResolveVariableFilePath(path, file_name);
  /* The next read parses what is written now (exactly as a fresh read would). */
  std::lock_guard<std::mutex> lock(VariableFileCacheMutex());
  VariableFileCache().erase(filepath);
  std::filesystem::create_directories(std::filesystem::path(filepath).parent_path());
  blender::io::serialize::JsonFormatter formatter;
  formatter.indentation_len = 2;
  std::ofstream stream(filepath);
  if (!stream.is_open()) {
    return;
  }
  formatter.serialize(stream, *ValueToJsonValue(dict_value));
}
static std::string ResolveGlobalPropertyFilePath(const std::string &category)
{
  char expanded[FILE_MAX];
  std::snprintf(expanded, sizeof(expanded), "%s", "//Globals");
  blender::BLI_path_abs(expanded, KX_GetMainPath().c_str());
  blender::BLI_path_normalize(expanded);
  const std::string file_name = category.empty() ? "global" : category;
  return (std::filesystem::path(expanded) / (file_name + ".logdb.txt")).string();
}
static void EnsureGlobalCategoryLoadedUnlocked(const std::string &category)
{
  if (category.empty() ||
      LoadedGlobalPropertyCategories().find(category) != LoadedGlobalPropertyCategories().end())
  {
    return;
  }
  LoadedGlobalPropertyCategories().insert(category);

  using namespace blender::io::serialize;
  std::shared_ptr<Value> root = read_json_file(ResolveGlobalPropertyFilePath(category));
  if (!root || root->type() != eValueType::Dictionary) {
    return;
  }

  LN_Value dict_value = JsonValueToValue(*root);
  if (dict_value.type != LN_ValueType::Dict || !dict_value.exists) {
    return;
  }
  std::map<std::string, LN_Value> &category_values = GlobalProperties()[category];
  for (const auto &item : dict_value.dict_value) {
    category_values.emplace(item.first, item.second);
  }
}
static void PersistGlobalCategoryUnlocked(const std::string &category)
{
  if (category.empty()) {
    return;
  }
  LN_Value dict_value;
  dict_value.type = LN_ValueType::Dict;
  dict_value.exists = true;
  dict_value.dict_value = GlobalProperties()[category];

  const std::string filepath = ResolveGlobalPropertyFilePath(category);
  std::filesystem::create_directories(std::filesystem::path(filepath).parent_path());
  blender::io::serialize::JsonFormatter formatter;
  formatter.indentation_len = 2;
  std::ofstream stream(filepath);
  if (stream.is_open()) {
    formatter.serialize(stream, *ValueToJsonValue(dict_value));
  }
}

static bool NumericListItem(const LN_Value &value, const size_t index, float &r_value)
{
  if (value.type != LN_ValueType::List || index >= value.list_value.size()) {
    return false;
  }
  const LN_Value &item = value.list_value[index];
  if (item.type == LN_ValueType::Float) {
    r_value = item.float_value;
    return true;
  }
  if (item.type == LN_ValueType::Int) {
    r_value = float(item.int_value);
    return true;
  }
  return false;
}

static bool ValueAsVector4(const LN_Value &value, MT_Vector4 &r_vector)
{
  if (value.type == LN_ValueType::Vector4) {
    r_vector = value.vector4_value;
    return true;
  }
  if (value.type == LN_ValueType::Color) {
    r_vector = value.color_value;
    return true;
  }
  if (value.type == LN_ValueType::Vector) {
    r_vector = MT_Vector4(
        value.vector_value.x(), value.vector_value.y(), value.vector_value.z(), 0.0f);
    return true;
  }
  if (value.type == LN_ValueType::Rotation) {
    const MT_Vector3 euler = value.RotationEulerRadians();
    r_vector = MT_Vector4(euler.x(), euler.y(), euler.z(), 0.0f);
    return true;
  }
  if (value.type == LN_ValueType::List && value.list_value.size() >= 2) {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;
    if (NumericListItem(value, 0, x) && NumericListItem(value, 1, y)) {
      if (value.list_value.size() >= 3 && !NumericListItem(value, 2, z)) {
        return false;
      }
      if (value.list_value.size() >= 4 && !NumericListItem(value, 3, w)) {
        return false;
      }
      r_vector = MT_Vector4(x, y, z, w);
      return true;
    }
  }
  return false;
}

static bool ValueAsColor(const LN_Value &value, MT_Vector4 &r_color)
{
  if (value.type == LN_ValueType::Color) {
    r_color = value.color_value;
    return true;
  }
  if (value.type == LN_ValueType::Vector4) {
    r_color = value.vector4_value;
    return true;
  }
  if (value.type == LN_ValueType::Vector) {
    r_color = MT_Vector4(
        value.vector_value.x(), value.vector_value.y(), value.vector_value.z(), 1.0f);
    return true;
  }
  if (value.type == LN_ValueType::Rotation) {
    const MT_Vector3 euler = value.RotationEulerRadians();
    r_color = MT_Vector4(euler.x(), euler.y(), euler.z(), 1.0f);
    return true;
  }
  if (value.type == LN_ValueType::List && value.list_value.size() >= 3) {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
    if (!NumericListItem(value, 0, r) || !NumericListItem(value, 1, g) ||
        !NumericListItem(value, 2, b) ||
        (value.list_value.size() >= 4 && !NumericListItem(value, 3, a)))
    {
      return false;
    }
    r_color = MT_Vector4(r, g, b, a);
    return true;
  }
  return false;
}

static bool ValueAsVector(const LN_Value &value, MT_Vector3 &r_vector)
{
  if (value.type == LN_ValueType::Int) {
    const float scalar = float(value.int_value);
    r_vector = MT_Vector3(scalar, scalar, scalar);
    return true;
  }
  if (value.type == LN_ValueType::Float) {
    r_vector = MT_Vector3(value.float_value, value.float_value, value.float_value);
    return true;
  }
  if (value.type == LN_ValueType::Vector) {
    r_vector = value.vector_value;
    return true;
  }
  if (value.type == LN_ValueType::Vector4) {
    r_vector = MT_Vector3(
        value.vector4_value.x(), value.vector4_value.y(), value.vector4_value.z());
    return true;
  }
  if (value.type == LN_ValueType::Color) {
    r_vector = MT_Vector3(value.color_value.x(), value.color_value.y(), value.color_value.z());
    return true;
  }
  if (value.type == LN_ValueType::Rotation) {
    r_vector = value.RotationEulerRadians();
    return true;
  }
  if (value.type == LN_ValueType::List && value.list_value.size() >= 3) {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    if (NumericListItem(value, 0, x) && NumericListItem(value, 1, y) &&
        NumericListItem(value, 2, z))
    {
      r_vector = MT_Vector3(x, y, z);
      return true;
    }
  }
  return false;
}

static bool ValueAsVectorList(const LN_Value &value, std::vector<MT_Vector3> &r_points)
{
  r_points.clear();
  if (value.type != LN_ValueType::List) {
    return false;
  }
  r_points.reserve(value.list_value.size());
  for (const LN_Value &item : value.list_value) {
    MT_Vector3 point(0.0f, 0.0f, 0.0f);
    if (!ValueAsVector(item, point)) {
      return false;
    }
    r_points.push_back(point);
  }
  return r_points.size() >= 2;
}

static bool RasterizerReady()
{
  KX_KetsjiEngine *engine = KX_GetActiveEngine();
  return engine != nullptr && engine->GetRasterizer() != nullptr;
}

static void DrawDebugRaycastHitFrame(const MT_Vector3 &point,
                                     const MT_Vector3 &normal,
                                     const float destination_length)
{
  const MT_Vector3 z_axis = normal.safe_normalized();
  if (z_axis.length2() <= MT_EPSILON * MT_EPSILON) {
    return;
  }

  const MT_Vector3 tangent_seed = std::fabs(z_axis.z()) < 0.9f ? MT_Vector3(0.0f, 0.0f, 1.0f) :
                                                                 MT_Vector3(1.0f, 0.0f, 0.0f);
  const MT_Vector3 x_axis = tangent_seed.cross(z_axis).safe_normalized();
  const MT_Vector3 y_axis = z_axis.cross(x_axis).safe_normalized();
  if (x_axis.length2() <= MT_EPSILON * MT_EPSILON || y_axis.length2() <= MT_EPSILON * MT_EPSILON) {
    return;
  }

  const float z_length = std::max(destination_length, MT_EPSILON);
  const float tangent_length = 0.3f;
  KX_RasterizerDrawDebugLine(
      point, point + x_axis * tangent_length, MT_Vector4(1.0f, 0.12f, 0.04f, 1.0f));
  KX_RasterizerDrawDebugLine(
      point, point + y_axis * tangent_length, MT_Vector4(0.2f, 1.0f, 0.2f, 1.0f));
  KX_RasterizerDrawDebugLine(
      point, point + z_axis * z_length, MT_Vector4(0.2f, 0.55f, 1.0f, 1.0f));
}

static void DrawDebugRaycast(const MT_Vector3 &from,
                             const MT_Vector3 &to,
                             const bool hit,
                             const MT_Vector3 *hit_normal,
                             const float destination_length)
{
  if (!RasterizerReady()) {
    return;
  }

  const MT_Vector4 color = hit ? MT_Vector4(1.0f, 0.05f, 0.02f, 1.0f) :
                                 MT_Vector4(1.0f, 0.92f, 0.05f, 1.0f);
  KX_RasterizerDrawDebugLine(from, to, color);
  if (hit) {
    if (hit_normal != nullptr) {
      DrawDebugRaycastHitFrame(to, *hit_normal, destination_length);
    }
  }
}

static void DrawDebugBox(const MT_Vector3 &origin,
                         const float half_x,
                         const float half_y,
                         const float half_z,
                         const MT_Vector4 &color)
{
  const MT_Vector3 vertices[8] = {
      origin + MT_Vector3(-half_x, -half_y, -half_z),
      origin + MT_Vector3(half_x, -half_y, -half_z),
      origin + MT_Vector3(half_x, half_y, -half_z),
      origin + MT_Vector3(-half_x, half_y, -half_z),
      origin + MT_Vector3(-half_x, -half_y, half_z),
      origin + MT_Vector3(half_x, -half_y, half_z),
      origin + MT_Vector3(half_x, half_y, half_z),
      origin + MT_Vector3(-half_x, half_y, half_z),
  };
  static constexpr int edges[12][2] = {
      {0, 1},
      {1, 2},
      {2, 3},
      {3, 0},
      {4, 5},
      {5, 6},
      {6, 7},
      {7, 4},
      {0, 4},
      {1, 5},
      {2, 6},
      {3, 7},
  };
  for (const int (&edge)[2] : edges) {
    KX_RasterizerDrawDebugLine(vertices[edge[0]], vertices[edge[1]], color);
  }
}

static MT_Vector3 DebugShapePoint(const MT_Vector3 &center,
                                  const MT_Matrix3x3 &orientation,
                                  const MT_Vector3 &local_point)
{
  return center + orientation * local_point;
}

static void DrawDebugOrientedBox(const MT_Vector3 &center,
                                 const MT_Matrix3x3 &orientation,
                                 const MT_Vector3 &half_extents,
                                 const MT_Vector4 &color)
{
  MT_Vector3 vertices[8] = {
      MT_Vector3(-half_extents.x(), -half_extents.y(), -half_extents.z()),
      MT_Vector3(half_extents.x(), -half_extents.y(), -half_extents.z()),
      MT_Vector3(half_extents.x(), half_extents.y(), -half_extents.z()),
      MT_Vector3(-half_extents.x(), half_extents.y(), -half_extents.z()),
      MT_Vector3(-half_extents.x(), -half_extents.y(), half_extents.z()),
      MT_Vector3(half_extents.x(), -half_extents.y(), half_extents.z()),
      MT_Vector3(half_extents.x(), half_extents.y(), half_extents.z()),
      MT_Vector3(-half_extents.x(), half_extents.y(), half_extents.z()),
  };
  for (MT_Vector3 &vertex : vertices) {
    vertex = DebugShapePoint(center, orientation, vertex);
  }
  static constexpr int edges[12][2] = {
      {0, 1},
      {1, 2},
      {2, 3},
      {3, 0},
      {4, 5},
      {5, 6},
      {6, 7},
      {7, 4},
      {0, 4},
      {1, 5},
      {2, 6},
      {3, 7},
  };
  for (const int (&edge)[2] : edges) {
    KX_RasterizerDrawDebugLine(vertices[edge[0]], vertices[edge[1]], color);
  }
}

static void DrawDebugSphere(const MT_Vector3 &center,
                            const MT_Matrix3x3 &orientation,
                            const float radius,
                            const MT_Vector4 &color)
{
  static constexpr int circle_segments = 24;
  const MT_Vector3 axes[3] = {
      orientation * MT_Vector3(1.0f, 0.0f, 0.0f),
      orientation * MT_Vector3(0.0f, 1.0f, 0.0f),
      orientation * MT_Vector3(0.0f, 0.0f, 1.0f),
  };
  static constexpr int circle_axes[3][2] = {{0, 1}, {0, 2}, {1, 2}};
  for (const int (&circle)[2] : circle_axes) {
    MT_Vector3 previous = center + axes[circle[0]] * radius;
    for (int segment = 1; segment <= circle_segments; ++segment) {
      const float angle = (2.0f * LN_PI * float(segment)) / float(circle_segments);
      const MT_Vector3 point = center + axes[circle[0]] * (radius * std::cos(angle)) +
                               axes[circle[1]] * (radius * std::sin(angle));
      KX_RasterizerDrawDebugLine(previous, point, color);
      previous = point;
    }
  }
}

static void DrawDebugRoundedBox(const MT_Vector3 &center,
                                const MT_Matrix3x3 &orientation,
                                const MT_Vector3 &half_extents,
                                const float radius,
                                const MT_Vector4 &color)
{
  if (radius <= MT_EPSILON) {
    DrawDebugOrientedBox(center, orientation, half_extents, color);
    return;
  }

  static constexpr int arc_segments = 4;
  for (int axis = 0; axis < 3; ++axis) {
    const int cross_axis0 = (axis + 1) % 3;
    const int cross_axis1 = (axis + 2) % 3;
    for (const float sign0 : {-1.0f, 1.0f}) {
      for (const float sign1 : {-1.0f, 1.0f}) {
        MT_Vector3 previous_low;
        MT_Vector3 previous_high;
        for (int segment = 0; segment <= arc_segments; ++segment) {
          const float angle = (0.5f * LN_PI * float(segment)) / float(arc_segments);
          MT_Vector3 low(0.0f, 0.0f, 0.0f);
          MT_Vector3 high(0.0f, 0.0f, 0.0f);
          low[axis] = -half_extents[axis];
          high[axis] = half_extents[axis];
          low[cross_axis0] = high[cross_axis0] = sign0 * (half_extents[cross_axis0] +
                                                          radius * std::cos(angle));
          low[cross_axis1] = high[cross_axis1] = sign1 * (half_extents[cross_axis1] +
                                                          radius * std::sin(angle));

          const MT_Vector3 world_low = DebugShapePoint(center, orientation, low);
          const MT_Vector3 world_high = DebugShapePoint(center, orientation, high);
          if (segment == 0 || segment == arc_segments / 2 || segment == arc_segments) {
            KX_RasterizerDrawDebugLine(world_low, world_high, color);
          }
          if (segment > 0) {
            KX_RasterizerDrawDebugLine(
                DebugShapePoint(center, orientation, previous_low), world_low, color);
            KX_RasterizerDrawDebugLine(
                DebugShapePoint(center, orientation, previous_high), world_high, color);
          }
          previous_low = low;
          previous_high = high;
        }
      }
    }
  }
}

static void DrawDebugCapsule(const MT_Vector3 &center,
                             const MT_Matrix3x3 &orientation,
                             const float radius,
                             const float cylinder_half_height,
                             const MT_Vector4 &color)
{
  if (cylinder_half_height <= MT_EPSILON) {
    DrawDebugSphere(center, orientation, radius, color);
    return;
  }

  const MT_Vector3 axis_x = orientation * MT_Vector3(1.0f, 0.0f, 0.0f);
  const MT_Vector3 axis_y = orientation * MT_Vector3(0.0f, 1.0f, 0.0f);
  const MT_Vector3 axis_z = orientation * MT_Vector3(0.0f, 0.0f, 1.0f);
  const MT_Vector3 bottom_center = center - axis_z * cylinder_half_height;
  const MT_Vector3 top_center = center + axis_z * cylinder_half_height;
  static constexpr int circle_segments = 24;
  for (const MT_Vector3 &circle_center : {bottom_center, top_center}) {
    MT_Vector3 previous = circle_center + axis_x * radius;
    for (int segment = 1; segment <= circle_segments; ++segment) {
      const float angle = (2.0f * LN_PI * float(segment)) / float(circle_segments);
      const MT_Vector3 point = circle_center + axis_x * (radius * std::cos(angle)) +
                               axis_y * (radius * std::sin(angle));
      KX_RasterizerDrawDebugLine(previous, point, color);
      previous = point;
    }
  }

  static constexpr int meridians = 4;
  static constexpr int hemisphere_segments = 4;
  for (int meridian = 0; meridian < meridians; ++meridian) {
    const float azimuth = (2.0f * LN_PI * float(meridian)) / float(meridians);
    const MT_Vector3 radial = axis_x * std::cos(azimuth) + axis_y * std::sin(azimuth);
    MT_Vector3 previous = bottom_center - axis_z * radius;
    for (int segment = hemisphere_segments - 1; segment >= 0; --segment) {
      const float angle = (0.5f * LN_PI * float(segment)) / float(hemisphere_segments);
      const MT_Vector3 point = bottom_center + radial * (radius * std::cos(angle)) -
                               axis_z * (radius * std::sin(angle));
      KX_RasterizerDrawDebugLine(previous, point, color);
      previous = point;
    }
    const MT_Vector3 top_equator = top_center + radial * radius;
    KX_RasterizerDrawDebugLine(previous, top_equator, color);
    previous = top_equator;
    for (int segment = 1; segment <= hemisphere_segments; ++segment) {
      const float angle = (0.5f * LN_PI * float(segment)) / float(hemisphere_segments);
      const MT_Vector3 point = top_center + radial * (radius * std::cos(angle)) +
                               axis_z * (radius * std::sin(angle));
      KX_RasterizerDrawDebugLine(previous, point, color);
      previous = point;
    }
  }
}

static void DrawDebugShape(const PHY_ShapeCastSettings &settings,
                           const MT_Vector3 &center,
                           const MT_Vector4 &color)
{
  switch (settings.type) {
    case PHY_ShapeCastType::Sphere:
      DrawDebugSphere(
          center, settings.orientation, settings.radius + settings.extra_radius, color);
      break;
    case PHY_ShapeCastType::Box:
      DrawDebugRoundedBox(
          center, settings.orientation, settings.half_extents, settings.extra_radius, color);
      break;
    case PHY_ShapeCastType::Capsule:
      DrawDebugCapsule(center,
                       settings.orientation,
                       settings.radius + settings.extra_radius,
                       std::max(0.0f, settings.height * 0.5f - settings.radius),
                       color);
      break;
  }
}

static float DebugShapeMarkerLength(const PHY_ShapeCastSettings &settings)
{
  switch (settings.type) {
    case PHY_ShapeCastType::Sphere:
      return settings.radius + settings.extra_radius;
    case PHY_ShapeCastType::Box:
      return std::max({settings.half_extents.x(),
                       settings.half_extents.y(),
                       settings.half_extents.z()}) +
             settings.extra_radius;
    case PHY_ShapeCastType::Capsule:
      return std::max(settings.height * 0.5f, settings.radius) + settings.extra_radius;
  }
  return 0.25f;
}

static void DrawDebugShapeCast(const PHY_ShapeCastSettings &settings,
                               const std::vector<LN_PhysicsQueryResult> &hits,
                               const LN_PhysicsQueryResult *blocker,
                               const bool collect_all)
{
  if (!RasterizerReady()) {
    return;
  }

  static const MT_Vector4 start_color(0.1f, 0.55f, 1.0f, 1.0f);
  static const MT_Vector4 miss_color(1.0f, 0.92f, 0.05f, 1.0f);
  static const MT_Vector4 hit_color(1.0f, 0.05f, 0.02f, 1.0f);
  static const MT_Vector4 blocked_color(1.0f, 0.3f, 0.0f, 1.0f);
  const bool has_hits = !hits.empty();
  const MT_Vector3 terminal = blocker                  ? blocker->cast_position :
                              collect_all || !has_hits ? settings.destination :
                                                         hits.front().cast_position;
  const MT_Vector4 &terminal_color = blocker                  ? blocked_color :
                                     has_hits && !collect_all ? hit_color :
                                                                miss_color;
  const MT_Vector4 &path_color = blocker ? blocked_color : has_hits ? hit_color : miss_color;
  const bool moved = (terminal - settings.origin).length2() > MT_EPSILON * MT_EPSILON;
  if (moved) {
    DrawDebugShape(settings, settings.origin, start_color);
    KX_RasterizerDrawDebugLine(settings.origin, terminal, path_color);
  }
  DrawDebugShape(settings, terminal, terminal_color);

  const float marker_length = std::max(0.25f, DebugShapeMarkerLength(settings));
  const size_t hit_count = collect_all ? hits.size() : std::min<size_t>(hits.size(), 1);
  for (size_t index = 0; index < hit_count; ++index) {
    KX_RasterizerDrawDebugLine(hits[index].cast_position, hits[index].hit_position, hit_color);
    DrawDebugRaycastHitFrame(hits[index].hit_position, hits[index].hit_normal, marker_length);
  }
  if (blocker) {
    KX_RasterizerDrawDebugLine(blocker->cast_position, blocker->hit_position, blocked_color);
    DrawDebugRaycastHitFrame(blocker->hit_position, blocker->hit_normal, marker_length);
  }
}

static void DrawDebugArrow(const MT_Vector3 &from, const MT_Vector3 &to, const MT_Vector4 &color)
{
  KX_RasterizerDrawDebugLine(from, to, color);
  const MT_Vector3 delta = to - from;
  const float length = std::sqrt(delta.length2());
  if (length <= 1.0e-6f) {
    return;
  }
  const MT_Vector3 direction = delta.safe_normalized();
  const MT_Vector3 up_seed = std::fabs(direction.z()) < 0.9f ? MT_Vector3(0.0f, 0.0f, 1.0f) :
                                                               MT_Vector3(0.0f, 1.0f, 0.0f);
  const MT_Vector3 side = direction.cross(up_seed).safe_normalized();
  const MT_Vector3 up = side.cross(direction).safe_normalized();
  const float head_length = std::max(0.05f, length * 0.2f);
  const float head_width = head_length * 0.45f;
  const MT_Vector3 base = to - direction * head_length;
  KX_RasterizerDrawDebugLine(to, base + side * head_width, color);
  KX_RasterizerDrawDebugLine(to, base - side * head_width, color);
  KX_RasterizerDrawDebugLine(to, base + up * head_width, color);
  KX_RasterizerDrawDebugLine(to, base - up * head_width, color);
}

static std::string SoundNameFromSpeakerObject(KX_GameObject *speaker,
                                              float &r_volume,
                                              float &r_pitch)
{
  const blender::Object *blender_object = speaker ? speaker->GetBlenderObject() : nullptr;
  if (blender_object == nullptr || blender_object->type != blender::OB_SPEAKER ||
      blender_object->data == nullptr)
  {
    return std::string();
  }
  const blender::Speaker *speaker_data = reinterpret_cast<const blender::Speaker *>(
      blender_object->data);
  if (speaker_data->sound == nullptr) {
    return std::string();
  }
  r_volume = speaker_data->volume;
  r_pitch = speaker_data->pitch;
  return std::string(speaker_data->sound->id.name + 2);
}

static bool ValueAsMatrix(const LN_Value &value, MT_Matrix3x3 &r_matrix)
{
  if (value.type == LN_ValueType::Matrix) {
    r_matrix = value.matrix_value;
    return true;
  }
  if (value.type == LN_ValueType::Rotation) {
    r_matrix = value.rotation_value.ToMatrix();
    return true;
  }
  if (value.type == LN_ValueType::Vector) {
    r_matrix = MatrixFromEuler(value.vector_value);
    return true;
  }
  if (value.type == LN_ValueType::List && value.list_value.size() == 3) {
    float matrix[3][3];
    for (size_t row = 0; row < 3; row++) {
      const LN_Value &row_value = value.list_value[row];
      if (row_value.type != LN_ValueType::List || row_value.list_value.size() < 3) {
        return false;
      }
      for (size_t column = 0; column < 3; column++) {
        if (!NumericListItem(row_value, column, matrix[row][column])) {
          return false;
        }
      }
    }
    r_matrix = MatrixFromFloat3x3(matrix);
    return true;
  }
  return false;
}

static bool RuntimeRefsEqual(const LN_RuntimeRef &a, const LN_RuntimeRef &b)
{
  if (a.IsValid() != b.IsValid()) {
    return false;
  }
  if (!a.IsValid()) {
    return true;
  }
  return a.kind == b.kind && a.slot == b.slot && a.generation == b.generation &&
         a.shared_domain == b.shared_domain;
}

static bool ValuesEqual(const LN_Value &a, const LN_Value &b, const float tolerance = 0.0001f)
{
  if (!a.exists || a.type == LN_ValueType::None) {
    return !b.exists || b.type == LN_ValueType::None;
  }
  if (!b.exists || b.type == LN_ValueType::None) {
    return false;
  }
  /* A whole number and a decimal one are equal when their values are (2 and 2.0). */
  if ((a.type == LN_ValueType::Int && b.type == LN_ValueType::Float) ||
      (a.type == LN_ValueType::Float && b.type == LN_ValueType::Int))
  {
    const double a_number = a.type == LN_ValueType::Int ? double(a.int_value) :
                                                          double(a.float_value);
    const double b_number = b.type == LN_ValueType::Int ? double(b.int_value) :
                                                          double(b.float_value);
    return a_number == b_number || std::fabs(a_number - b_number) <= double(tolerance);
  }
  if (a.type != b.type) {
    return false;
  }
  switch (a.type) {
    case LN_ValueType::Bool:
      return a.bool_value == b.bool_value;
    case LN_ValueType::Int:
      return a.int_value == b.int_value;
    case LN_ValueType::Float:
      return a.float_value == b.float_value ||
             std::fabs(a.float_value - b.float_value) <= tolerance;
    case LN_ValueType::String:
      return a.string_value == b.string_value;
    case LN_ValueType::Vector:
      return (a.vector_value - b.vector_value).length() <= tolerance;
    case LN_ValueType::Vector4:
      return (a.vector4_value - b.vector4_value).length() <= tolerance;
    case LN_ValueType::Matrix:
      for (int row = 0; row < 3; row++) {
        for (int column = 0; column < 3; column++) {
          if (std::fabs(a.matrix_value[row][column] - b.matrix_value[row][column]) > tolerance) {
            return false;
          }
        }
      }
      return true;
    case LN_ValueType::Color:
      return (a.color_value - b.color_value).length() <= tolerance;
    case LN_ValueType::Rotation:
      return (a.RotationEulerRadians() - b.RotationEulerRadians()).length() <= tolerance;
    case LN_ValueType::ObjectRef:
    case LN_ValueType::SceneRef:
    case LN_ValueType::CollectionRef:
    case LN_ValueType::DatablockRef:
      return RuntimeRefsEqual(a.runtime_ref, b.runtime_ref) &&
             (a.runtime_ref.IsValid() || a.reference_name == b.reference_name);
    case LN_ValueType::List: {
      if (a.list_value.size() != b.list_value.size()) {
        return false;
      }
      for (size_t index = 0; index < a.list_value.size(); index++) {
        if (!ValuesEqual(a.list_value[index], b.list_value[index], tolerance)) {
          return false;
        }
      }
      return true;
    }
    case LN_ValueType::Dict: {
      if (a.dict_value.size() != b.dict_value.size()) {
        return false;
      }
      for (const auto &entry : a.dict_value) {
        const auto other = b.dict_value.find(entry.first);
        if (other == b.dict_value.end() || !ValuesEqual(entry.second, other->second, tolerance)) {
          return false;
        }
      }
      return true;
    }
    default:
      return false;
  }
}

static std::string ValueToPrintString(const LN_Value &value, int depth = 0);

static bool ValueAsBool(const LN_Value &value)
{
  if (!value.exists || value.type == LN_ValueType::None) {
    return false;
  }
  switch (value.type) {
    case LN_ValueType::Bool:
      return value.bool_value;
    case LN_ValueType::Int:
      return value.int_value != 0;
    case LN_ValueType::Float:
      return value.float_value != 0.0f;
    case LN_ValueType::String:
      return !value.string_value.empty();
    case LN_ValueType::Vector:
      return value.vector_value.length2() > 0.0f;
    case LN_ValueType::Vector4:
      return value.vector4_value.length2() > 0.0f;
    case LN_ValueType::Matrix:
      return MatrixHasNonZeroElement(value.matrix_value);
    case LN_ValueType::Color:
      return value.color_value.length2() > 0.0f;
    case LN_ValueType::Rotation:
      return value.RotationEulerRadians().length2() > 0.0f;
    case LN_ValueType::List:
      return !value.list_value.empty();
    case LN_ValueType::Dict:
      return !value.dict_value.empty();
    case LN_ValueType::ObjectRef:
    case LN_ValueType::SceneRef:
    case LN_ValueType::CollectionRef:
    case LN_ValueType::DatablockRef:
      return value.runtime_ref.IsValid() || !value.reference_name.empty();
    default:
      return true;
  }
}

static bool ValueAsFloat(const LN_Value &value, float &r_value)
{
  if (!value.exists || value.type == LN_ValueType::None) {
    return false;
  }
  switch (value.type) {
    case LN_ValueType::Bool:
      r_value = value.bool_value ? 1.0f : 0.0f;
      return true;
    case LN_ValueType::Int:
      r_value = float(value.int_value);
      return true;
    case LN_ValueType::Float:
      r_value = value.float_value;
      return true;
    case LN_ValueType::String: {
      return LN_TryParseTypecastFloat(value.string_value, r_value);
    }
    default:
      return false;
  }
}

static int32_t ValueAsInt(const LN_Value &value, const LN_FloatToIntMode mode)
{
  float float_value = 0.0f;
  if (ValueAsFloat(value, float_value)) {
    return LN_ConvertFloatToInt(float_value, mode);
  }
  return 0;
}

static bool CompareValues(const LN_Value &a,
                          const LN_Value &b,
                          const LN_FloatCompareOperation operation)
{
  switch (operation) {
    case LN_FloatCompareOperation::Equal:
      return ValuesEqual(a, b);
    case LN_FloatCompareOperation::NotEqual:
      return !ValuesEqual(a, b);
    default:
      break;
  }

  float a_float = 0.0f;
  float b_float = 0.0f;
  if (ValueAsFloat(a, a_float) && ValueAsFloat(b, b_float)) {
    switch (operation) {
      case LN_FloatCompareOperation::GreaterThan:
        return a_float > b_float;
      case LN_FloatCompareOperation::LessThan:
        return a_float < b_float;
      case LN_FloatCompareOperation::GreaterEqual:
        return a_float >= b_float;
      case LN_FloatCompareOperation::LessEqual:
        return a_float <= b_float;
      default:
        return false;
    }
  }

  if (a.type == LN_ValueType::String && b.type == LN_ValueType::String) {
    switch (operation) {
      case LN_FloatCompareOperation::GreaterThan:
        return a.string_value > b.string_value;
      case LN_FloatCompareOperation::LessThan:
        return a.string_value < b.string_value;
      case LN_FloatCompareOperation::GreaterEqual:
        return a.string_value >= b.string_value;
      case LN_FloatCompareOperation::LessEqual:
        return a.string_value <= b.string_value;
      default:
        return false;
    }
  }
  return false;
}

static bool IsNumericValueType(const LN_ValueType type)
{
  return type == LN_ValueType::Int || type == LN_ValueType::Float;
}

/** Numbers first, then text, then anything else; a \a key sorts dictionaries by that entry. */
static void SortListValues(std::vector<LN_Value> &items,
                           const std::string &key,
                           const bool descending)
{
  static const LN_Value none;
  const auto sort_value = [&](const LN_Value &item) -> const LN_Value & {
    if (key.empty() || item.type != LN_ValueType::Dict) {
      return item;
    }
    const auto entry = item.dict_value.find(key);
    return entry != item.dict_value.end() ? entry->second : none;
  };
  const auto rank = [](const LN_Value &value) {
    if (!value.exists) {
      return 2;
    }
    if (IsNumericValueType(value.type) || value.type == LN_ValueType::Bool) {
      return 0;
    }
    return value.type == LN_ValueType::String ? 1 : 2;
  };
  std::stable_sort(items.begin(), items.end(), [&](const LN_Value &a_item, const LN_Value &b_item) {
    const LN_Value &a = sort_value(descending ? b_item : a_item);
    const LN_Value &b = sort_value(descending ? a_item : b_item);
    if (rank(a) != rank(b)) {
      return descending ? rank(a) > rank(b) : rank(a) < rank(b);
    }
    if (rank(a) == 0) {
      float a_float = 0.0f;
      float b_float = 0.0f;
      ValueAsFloat(a, a_float);
      ValueAsFloat(b, b_float);
      return a_float < b_float;
    }
    return rank(a) == 1 && a.string_value < b.string_value;
  });
}

static bool ValueChangedToTarget(const LN_Value &old_value,
                                 const LN_Value &new_value,
                                 const LN_Value &target)
{
  if (ValuesEqual(new_value, target)) {
    return true;
  }

  if (!IsNumericValueType(old_value.type) || !IsNumericValueType(new_value.type) ||
      !IsNumericValueType(target.type))
  {
    return false;
  }

  float old_float = 0.0f;
  float new_float = 0.0f;
  float target_float = 0.0f;
  if (!ValueAsFloat(old_value, old_float) || !ValueAsFloat(new_value, new_float) ||
      !ValueAsFloat(target, target_float))
  {
    return false;
  }

  constexpr float tolerance = 0.0001f;
  if (std::fabs(new_float - target_float) <= tolerance) {
    return true;
  }

  return (old_float < target_float && new_float > target_float) ||
         (old_float > target_float && new_float < target_float);
}

static bool NormalizeListIndex(const int32_t index, const size_t size, size_t &r_index)
{
  const int64_t normalized = index < 0 ? int64_t(size) + int64_t(index) : int64_t(index);
  if (normalized < 0 || normalized >= int64_t(size)) {
    return false;
  }
  r_index = size_t(normalized);
  return true;
}

static BL_ArmatureObject *ArmatureFacade(KX_GameObject *game_object)
{
  return game_object != nullptr && game_object->GetGameObjectType() == SCA_IObject::OBJ_ARMATURE ?
             static_cast<BL_ArmatureObject *>(game_object) :
             nullptr;
}

enum class BonePosePoint {
  Head,
  Tail,
  Center,
};

static LN_BonePosePositionSpace NormalizeBonePosePositionSpace(const uint32_t value)
{
  switch (value) {
    case uint32_t(LN_BonePosePositionSpace::Armature):
      return LN_BonePosePositionSpace::Armature;
    case uint32_t(LN_BonePosePositionSpace::World):
    case LN_INVALID_INDEX:
      return LN_BonePosePositionSpace::World;
    default:
      return LN_BonePosePositionSpace::World;
  }
}

static MT_Vector3 ArmaturePointToWorld(KX_GameObject *armature_object, const MT_Vector3 &position)
{
  const MT_Matrix3x3 &rot = armature_object->NodeGetWorldOrientation();
  const MT_Vector3 &scale = armature_object->NodeGetWorldScaling();
  const MT_Vector3 &pos = armature_object->NodeGetWorldPosition();
  return rot * (position * scale) + pos;
}

static bool ResolveBonePosePosition(KX_GameObject *armature_object,
                                    const std::string &bone_name,
                                    const BonePosePoint point,
                                    const LN_BonePosePositionSpace space,
                                    MT_Vector3 &r_position)
{
  if (armature_object == nullptr) {
    return false;
  }
  BL_ArmatureObject *armature = ArmatureFacade(armature_object);
  blender::Object *ob = armature ? armature->GetArmatureObject() : nullptr;
  if (ob == nullptr || ob->type != blender::OB_ARMATURE || ob->pose == nullptr) {
    return false;
  }
  if (bone_name.empty()) {
    return false;
  }
  armature->ApplyPose();
  blender::bPoseChannel *pchan = armature->FindPoseChannel(bone_name);
  if (pchan == nullptr) {
    return false;
  }
  const MT_Vector3 pose_head(pchan->pose_head[0], pchan->pose_head[1], pchan->pose_head[2]);
  const MT_Vector3 pose_tail(pchan->pose_tail[0], pchan->pose_tail[1], pchan->pose_tail[2]);
  MT_Vector3 pose_position = pose_head;
  if (point == BonePosePoint::Tail) {
    pose_position = pose_tail;
  }
  else if (point == BonePosePoint::Center) {
    pose_position = (pose_head + pose_tail) * 0.5f;
  }
  r_position = space == LN_BonePosePositionSpace::World ?
                   ArmaturePointToWorld(armature_object, pose_position) :
                   pose_position;
  return true;
}

static bool ResolveBonePoseLocation(KX_GameObject *armature_object,
                                    const std::string &bone_name,
                                    const LN_BonePoseRotationSpace space,
                                    MT_Vector3 &r_location)
{
  if (armature_object == nullptr) {
    return false;
  }
  BL_ArmatureObject *armature = ArmatureFacade(armature_object);
  blender::Object *ob = armature ? armature->GetArmatureObject() : nullptr;
  if (ob == nullptr || ob->type != blender::OB_ARMATURE || ob->pose == nullptr ||
      bone_name.empty())
  {
    return false;
  }
  blender::bPoseChannel *pchan = armature->FindPoseChannel(bone_name);
  if (pchan == nullptr) {
    return false;
  }
  if (space == LN_BonePoseRotationSpace::PoseChannel) {
    r_location = MT_Vector3(pchan->loc[0], pchan->loc[1], pchan->loc[2]);
    return true;
  }

  armature->ApplyPose();
  const MT_Vector3 pose_head(pchan->pose_head[0], pchan->pose_head[1], pchan->pose_head[2]);
  r_location = space == LN_BonePoseRotationSpace::World ?
                   ArmaturePointToWorld(armature_object, pose_head) :
                   pose_head;
  return true;
}

static bool ResolveBonePoseScale(KX_GameObject *armature_object,
                                 const std::string &bone_name,
                                 MT_Vector3 &r_scale)
{
  if (armature_object == nullptr) {
    return false;
  }
  BL_ArmatureObject *armature = ArmatureFacade(armature_object);
  blender::Object *ob = armature ? armature->GetArmatureObject() : nullptr;
  if (ob == nullptr || ob->type != blender::OB_ARMATURE || ob->pose == nullptr ||
      bone_name.empty())
  {
    return false;
  }
  blender::bPoseChannel *pchan = armature->FindPoseChannel(bone_name);
  if (pchan == nullptr) {
    return false;
  }
  r_scale = MT_Vector3(pchan->scale[0], pchan->scale[1], pchan->scale[2]);
  return true;
}

static LN_BonePoseRotationSpace NormalizeBonePoseRotationSpace(const uint32_t value)
{
  switch (value) {
    case uint32_t(LN_BonePoseRotationSpace::Armature):
      return LN_BonePoseRotationSpace::Armature;
    case uint32_t(LN_BonePoseRotationSpace::World):
      return LN_BonePoseRotationSpace::World;
    case uint32_t(LN_BonePoseRotationSpace::PoseChannel):
    case LN_INVALID_INDEX:
    default:
      return LN_BonePoseRotationSpace::PoseChannel;
  }
}

static LN_Rotation NativePoseChannelRotation(const blender::bPoseChannel &pchan)
{
  const blender::float4 quaternion = blender::BKE_pchan_rot_to_quat(pchan);
  return LN_Rotation(MT_Quaternion(quaternion[1], quaternion[2], quaternion[3], quaternion[0]));
}

static bool ResolveBonePoseRotation(KX_GameObject *armature_object,
                                    const std::string &bone_name,
                                    const LN_BonePoseRotationSpace space,
                                    LN_Rotation &r_rotation)
{
  if (armature_object == nullptr) {
    return false;
  }
  BL_ArmatureObject *armature = ArmatureFacade(armature_object);
  blender::Object *ob = armature ? armature->GetArmatureObject() : nullptr;
  if (ob == nullptr || ob->type != blender::OB_ARMATURE || ob->pose == nullptr) {
    return false;
  }
  if (bone_name.empty()) {
    return false;
  }
  blender::bPoseChannel *pchan = armature->FindPoseChannel(bone_name);
  if (pchan == nullptr) {
    return false;
  }
  if (space == LN_BonePoseRotationSpace::PoseChannel) {
    r_rotation = NativePoseChannelRotation(*pchan);
    return true;
  }

  armature->ApplyPose();
  float pose_mat[3][3];
  blender::copy_m3_m4(pose_mat, pchan->pose_mat);
  LN_Rotation orientation = LN_BlenderMatrixToRotation(pose_mat);
  if (space == LN_BonePoseRotationSpace::World) {
    const LN_Rotation armature_world_rotation = LN_Rotation::FromMatrix(
        armature_object->NodeGetWorldOrientation());
    orientation = LN_Rotation(armature_world_rotation.Quaternion() * orientation.Quaternion());
  }
  r_rotation = orientation;
  return true;
}

static float NormalizeGamepadAxisValue(const int32_t axis_value)
{
  return std::clamp(float(axis_value) / 32767.0f, -1.0f, 1.0f);
}

static KX_KetsjiEngine *ActiveEngine()
{
  return KX_GetActiveEngine();
}

static RAS_ICanvas *ActiveCanvas()
{
  if (KX_KetsjiEngine *engine = ActiveEngine()) {
    return engine->GetCanvas();
  }
  return nullptr;
}

/** Get Player View: where the player's split screen view is in the window; false when the
 * player has no view on screen. */
static bool ResolvePlayerViewArea(KX_GameObject *owner,
                                  const int32_t user,
                                  KX_PlayerViews::Area &r_area,
                                  RAS_Rect *r_window = nullptr)
{
  KX_Scene *scene = owner != nullptr ? owner->GetScene() : nullptr;
  if (scene == nullptr) {
    return false;
  }
  /* The window the views are drawn in; the canvas before the first frame. */
  const KX_PlayerViews &views = scene->GetPlayerViews();
  RAS_Rect window = views.GetWindow();
  if (window.GetWidth() <= 0 || window.GetHeight() <= 0) {
    RAS_ICanvas *canvas = ActiveCanvas();
    window = canvas != nullptr ? RAS_Rect(canvas->GetWidth(), canvas->GetHeight()) : RAS_Rect(1, 1);
  }
  if (r_window != nullptr) {
    *r_window = window;
  }
  return views.GetPlayerArea(user, scene->GetActiveCamera(), window, r_area);
}

static int32_t WindowVSyncModeFromSwapInterval(const int interval)
{
  if (interval < 0) {
    return VSYNC_ADAPTIVE;
  }
  return interval > 0 ? VSYNC_ON : VSYNC_OFF;
}

static bool ActiveCanvasVSyncMode(int32_t &r_mode)
{
  if (RAS_ICanvas *canvas = ActiveCanvas()) {
    int interval = 0;
    if (canvas->GetSwapInterval(interval)) {
      r_mode = WindowVSyncModeFromSwapInterval(interval);
      return true;
    }
  }
  return false;
}

static MT_Vector3 OrientationToEuler(const MT_Matrix3x3 &orientation)
{
  MT_Scalar x = 0.0f;
  MT_Scalar y = 0.0f;
  MT_Scalar z = 0.0f;
  orientation.getEuler(x, y, z);
  return MT_Vector3(x, y, z);
}

static bool NormalizeSafe(MT_Vector3 &vector)
{
  const float length_squared = vector.length2();
  if (length_squared <= 1.0e-20f) {
    return false;
  }
  vector *= 1.0f / std::sqrt(length_squared);
  return true;
}

static MT_Vector3 FallbackUpVector(const MT_Vector3 &axis)
{
  return std::fabs(axis.z()) < 0.9f ? MT_Vector3(0.0f, 0.0f, 1.0f) : MT_Vector3(0.0f, 1.0f, 0.0f);
}

static MT_Vector3 VectorToRotationEuler(MT_Vector3 direction,
                                        MT_Vector3 up,
                                        const int32_t axis_selector)
{
  if (!NormalizeSafe(direction)) {
    return MT_Vector3(0.0f, 0.0f, 0.0f);
  }

  const int32_t selected_axis = std::clamp(axis_selector, 0, 5);
  const int32_t track_axis = selected_axis % 3;
  const float track_sign = selected_axis >= 3 ? -1.0f : 1.0f;
  const int32_t up_axis = track_axis == 2 ? 1 : 2;
  const int32_t missing_axis = 3 - track_axis - up_axis;

  MT_Vector3 columns[3];
  columns[track_axis] = direction * track_sign;

  if (!NormalizeSafe(up)) {
    up = FallbackUpVector(columns[track_axis]);
  }
  columns[up_axis] = up - columns[track_axis] * columns[track_axis].dot(up);
  if (!NormalizeSafe(columns[up_axis])) {
    up = FallbackUpVector(columns[track_axis]);
    columns[up_axis] = up - columns[track_axis] * columns[track_axis].dot(up);
    if (!NormalizeSafe(columns[up_axis])) {
      return MT_Vector3(0.0f, 0.0f, 0.0f);
    }
  }

  switch (missing_axis) {
    case 0:
      columns[0] = columns[1].cross(columns[2]);
      break;
    case 1:
      columns[1] = columns[2].cross(columns[0]);
      break;
    default:
      columns[2] = columns[0].cross(columns[1]);
      break;
  }
  NormalizeSafe(columns[missing_axis]);

  switch (up_axis) {
    case 0:
      columns[0] = columns[1].cross(columns[2]);
      break;
    case 1:
      columns[1] = columns[2].cross(columns[0]);
      break;
    default:
      columns[2] = columns[0].cross(columns[1]);
      break;
  }
  NormalizeSafe(columns[up_axis]);

  MT_Matrix3x3 orientation;
  orientation.setColumn(0, columns[0]);
  orientation.setColumn(1, columns[1]);
  orientation.setColumn(2, columns[2]);
  return OrientationToEuler(orientation);
}

static bool KeyboardInputIsActive(const LN_InputSnapshot &input_snapshot)
{
  for (int32_t input_code = 0; input_code < SCA_IInputDevice::BEGINMOUSE; input_code++) {
    if (input_snapshot.GetStatus(input_code, SCA_InputEvent::ACTIVE) ||
        input_snapshot.GetStatus(input_code, SCA_InputEvent::JUSTACTIVATED))
    {
      return true;
    }
  }
  return false;
}

static LN_PhysicsQueryResult BuildPhysicsQueryResult(LN_RuntimeTree &runtime_tree,
                                                     KX_GameObject *hit_object,
                                                     const MT_Vector3 &from,
                                                     const MT_Vector3 &to,
                                                     const MT_Vector3 &hit_point,
                                                     const MT_Vector3 &hit_normal,
                                                     const MT_Vector2 &hit_uv,
                                                     const int hit_uv_ok,
                                                     const int hit_polygon)
{
  LN_PhysicsQueryResult result;
  result.hit = true;
  result.diagnostic_status = LN_QueryDiagnosticStatus::Hit;
  result.object_ref = runtime_tree.MakeObjectRef(hit_object, hit_object->GetName());
  result.hit_position = hit_point;
  result.hit_normal = hit_normal;
  result.ray_origin = from;
  result.ray_direction = (to - from).safe_normalized();
  result.end_position = to;
  result.hit_uv = hit_uv;
  result.polygon_index = hit_polygon;
  result.has_uv = hit_uv_ok != 0;

  const MT_Vector3 segment = to - from;
  const float length = segment.length();
  if (length > MT_EPSILON) {
    result.hit_distance = (hit_point - from).length();
    result.hit_fraction = result.hit_distance / length;
  }
  return result;
}

static bool GameObjectHasAncestor(KX_GameObject *object, KX_GameObject *ancestor)
{
  if (object == nullptr || ancestor == nullptr) {
    return false;
  }

  int depth = 0;
  for (KX_GameObject *parent = object->GetParent(); parent != nullptr && depth < 128;
       parent = parent->GetParent(), depth++)
  {
    if (parent == ancestor) {
      return true;
    }
  }
  return false;
}

static bool MouseOverHitMatchesTarget(KX_GameObject *hit_object, KX_GameObject *target_object)
{
  if (hit_object == nullptr || target_object == nullptr) {
    return false;
  }
  if (hit_object == target_object) {
    return true;
  }
  if (GameObjectHasAncestor(hit_object, target_object)) {
    return true;
  }
  return target_object->GetPhysicsController() == nullptr &&
         GameObjectHasAncestor(target_object, hit_object);
}

/**
 * Immutable controller lookup shared by every broad-phase callback in one query. Small ignore
 * lists use a cache-friendly linear scan. Larger lists build a hash table once, which keeps a
 * Raycast from Mesh batch from multiplying list-search work by every ray and broad-phase body.
 * Concurrent reads are safe while a physics backend executes a synchronous parallel batch.
 */
class LN_PhysicsControllerIgnoreSet {
 public:
  LN_PhysicsControllerIgnoreSet(LN_RuntimeTree &runtime_tree,
                                const std::vector<LN_RuntimeRef> &object_refs)
  {
    m_controllers.reserve(object_refs.size());
    for (const LN_RuntimeRef &object_ref : object_refs) {
      KX_GameObject *object = runtime_tree.ResolveObjectRef(object_ref);
      if (PHY_IPhysicsController *controller = RaycastOwningPhysicsController(object)) {
        m_controllers.push_back(controller);
      }
    }

    const std::less<PHY_IPhysicsController *> less;
    std::sort(m_controllers.begin(), m_controllers.end(), less);
    m_controllers.erase(std::unique(m_controllers.begin(), m_controllers.end()),
                        m_controllers.end());
    if (m_controllers.size() > kLinearSearchLimit) {
      m_controllerSet.reserve(m_controllers.size());
      m_controllerSet.insert(m_controllers.begin(), m_controllers.end());
    }
  }

  bool Contains(PHY_IPhysicsController *controller) const
  {
    if (!controller) {
      return false;
    }
    if (!m_controllerSet.empty()) {
      return m_controllerSet.find(controller) != m_controllerSet.end();
    }
    return std::find(m_controllers.begin(), m_controllers.end(), controller) !=
           m_controllers.end();
  }

 private:
  static constexpr size_t kLinearSearchLimit = 8;
  std::vector<PHY_IPhysicsController *> m_controllers;
  std::unordered_set<PHY_IPhysicsController *> m_controllerSet;
};

static bool InteractionQueryAllowsObject(KX_Scene *scene,
                                         KX_GameObject *object,
                                         const LN_InteractionDomain domain,
                                         const bool respect_runtime_visibility,
                                         const KX_Camera *view_camera)
{
  if (object == nullptr) {
    return false;
  }
  if (respect_runtime_visibility && !object->GetVisible()) {
    return false;
  }
  /* A pointer ray passes through what its camera does not draw (Set Camera Visibility). */
  if (view_camera != nullptr && scene != nullptr && scene->IsHiddenForCamera(object, view_camera))
  {
    return false;
  }
  if (domain == LN_InteractionDomain::Any) {
    return true;
  }
  const bool is_overlay = scene != nullptr && scene->IsOverlayRuntimeObject(object);
  return domain == LN_InteractionDomain::Overlay ? is_overlay : !is_overlay;
}

class LN_RaycastCallback : public KX_RayCast {
 public:
  LN_RaycastCallback(LN_RuntimeTree &runtime_tree,
                     PHY_IPhysicsController *ignore_controller,
                     const LN_PhysicsControllerIgnoreSet &extra_ignore_controllers,
                     const MT_Vector3 &from,
                     const MT_Vector3 &to,
                     uint32_t collision_mask,
                     std::string property_name = std::string(),
                     bool xray = false,
                     bool collect_all = false,
                     KX_Scene *interaction_scene = nullptr,
                     LN_InteractionDomain interaction_domain = LN_InteractionDomain::Any,
                     bool respect_runtime_visibility = false,
                     const KX_Camera *view_camera = nullptr)
      : KX_RayCast(ignore_controller, true, true),
        m_runtimeTree(runtime_tree),
        m_extraIgnoreControllers(extra_ignore_controllers),
        m_from(from),
        m_to(to),
        m_collisionMask(collision_mask),
        m_propertyName(std::move(property_name)),
        m_xray(xray),
        m_collectAll(collect_all),
        m_interactionScene(interaction_scene),
        m_interactionDomain(interaction_domain),
        m_respectRuntimeVisibility(respect_runtime_visibility),
        m_viewCamera(view_camera)
  {
    m_hitFound = false;
    m_result.ray_origin = m_from;
    m_result.ray_direction = (m_to - m_from).safe_normalized();
    m_result.end_position = m_to;
  }

  bool RayHit(KX_ClientObjectInfo *client) override
  {
    if (client == nullptr || client->m_gameobject == nullptr) {
      return false;
    }

    KX_GameObject *hit_object = client->m_gameobject;
    if (!InteractionQueryAllowsObject(m_interactionScene,
                                      hit_object,
                                      m_interactionDomain,
                                      m_respectRuntimeVisibility,
                                      m_viewCamera))
    {
      return false;
    }
    const bool property_matches = m_propertyName.empty() ||
                                  hit_object->GetProperty(m_propertyName);
    if (!property_matches) {
      if (!m_xray) {
        m_result.blocked = true;
        m_result.ray_origin = m_from;
        m_result.ray_direction = (m_to - m_from).safe_normalized();
        m_result.end_position = m_to;
      }
      return m_xray ? false : true;
    }

    LN_PhysicsQueryResult result = BuildResult(hit_object);
    if (m_collectAll) {
      m_results.push_back(result);
      if (!m_result.hit) {
        m_result = result;
      }
      m_hitFound = true;
      return false;
    }
    m_result = result;
    m_hitFound = true;
    return true;
  }

  bool needBroadphaseRayCast(PHY_IPhysicsController *controller) override
  {
    if (controller == nullptr || controller == m_ignoreController ||
        m_extraIgnoreControllers.Contains(controller))
    {
      return false;
    }
    if (m_collisionMask == 0 && (!m_xray || m_propertyName.empty()) &&
        m_interactionDomain == LN_InteractionDomain::Any && !m_respectRuntimeVisibility &&
        m_viewCamera == nullptr)
    {
      return true;
    }

    KX_ClientObjectInfo *info = static_cast<KX_ClientObjectInfo *>(controller->GetNewClientInfo());
    if (info == nullptr || info->m_gameobject == nullptr) {
      return false;
    }
    if (!InteractionQueryAllowsObject(m_interactionScene,
                                      info->m_gameobject,
                                      m_interactionDomain,
                                      m_respectRuntimeVisibility,
                                      m_viewCamera))
    {
      return false;
    }
    if (m_collisionMask != 0 && (info->m_gameobject->GetCollisionGroup() & m_collisionMask) == 0) {
      return false;
    }
    return !m_xray || m_propertyName.empty() || info->m_gameobject->GetProperty(m_propertyName);
  }

  const LN_PhysicsQueryResult &GetResult() const
  {
    return m_result;
  }

  const std::vector<LN_PhysicsQueryResult> &GetResults() const
  {
    return m_results;
  }

 private:
  LN_PhysicsQueryResult BuildResult(KX_GameObject *hit_object) const
  {
    return BuildPhysicsQueryResult(m_runtimeTree,
                                   hit_object,
                                   m_from,
                                   m_to,
                                   m_hitPoint,
                                   m_hitNormal,
                                   m_hitUV,
                                   m_hitUVOK,
                                   m_hitPolygon);
  }

  LN_RuntimeTree &m_runtimeTree;
  const LN_PhysicsControllerIgnoreSet &m_extraIgnoreControllers;
  MT_Vector3 m_from;
  MT_Vector3 m_to;
  uint32_t m_collisionMask = 0;
  LN_PhysicsQueryResult m_result;
  std::vector<LN_PhysicsQueryResult> m_results;
  std::string m_propertyName;
  bool m_xray = false;
  bool m_collectAll = false;
  KX_Scene *m_interactionScene = nullptr;
  LN_InteractionDomain m_interactionDomain = LN_InteractionDomain::Any;
  bool m_respectRuntimeVisibility = false;
  /** A pointer ray's camera: what it does not draw is not hit. */
  const KX_Camera *m_viewCamera = nullptr;
};

class LN_RayQueryFilterCallback final : public PHY_IRayCastFilterCallback {
 public:
  LN_RayQueryFilterCallback(PHY_IPhysicsController *ignore_controller,
                            const LN_PhysicsControllerIgnoreSet &extra_ignore_controllers,
                            const uint32_t collision_mask,
                            const std::string &property_name,
                            const bool xray)
      : PHY_IRayCastFilterCallback(ignore_controller),
        m_extraIgnoreControllers(extra_ignore_controllers),
        m_collisionMask(collision_mask),
        m_propertyName(property_name),
        m_xray(xray)
  {
  }

  bool needBroadphaseRayCast(PHY_IPhysicsController *controller) override
  {
    if (!controller || controller == m_ignoreController ||
        m_extraIgnoreControllers.Contains(controller))
    {
      return false;
    }
    KX_ClientObjectInfo *info = static_cast<KX_ClientObjectInfo *>(controller->GetNewClientInfo());
    if (!info || !info->m_gameobject) {
      return false;
    }
    if (m_collisionMask != 0 && (info->m_gameobject->GetCollisionGroup() & m_collisionMask) == 0) {
      return false;
    }
    return !m_xray || m_propertyName.empty() || info->m_gameobject->GetProperty(m_propertyName);
  }

  void reportHit(PHY_RayCastResult * /*result*/) override {}

 private:
  const LN_PhysicsControllerIgnoreSet &m_extraIgnoreControllers;
  uint32_t m_collisionMask;
  const std::string &m_propertyName;
  bool m_xray;
};

class LN_ShapeCastFilterCallback final : public PHY_IShapeCastFilterCallback {
 public:
  LN_ShapeCastFilterCallback(PHY_IPhysicsController *ignore_controller,
                             const LN_PhysicsControllerIgnoreSet &extra_ignore_controllers,
                             const uint32_t collision_mask,
                             const std::string &property_name,
                             const bool xray)
      : PHY_IShapeCastFilterCallback(ignore_controller, nullptr),
        m_extraIgnoreControllers(extra_ignore_controllers),
        m_collisionMask(collision_mask),
        m_propertyName(property_name),
        m_xray(xray)
  {
  }

  bool needBroadphaseShapeCast(PHY_IPhysicsController *controller) override
  {
    if (!controller || controller == m_ignoreController ||
        m_extraIgnoreControllers.Contains(controller))
    {
      return false;
    }
    KX_ClientObjectInfo *info = static_cast<KX_ClientObjectInfo *>(controller->GetNewClientInfo());
    if (!info || !info->m_gameobject) {
      return false;
    }
    if (m_collisionMask != 0 && (info->m_gameobject->GetCollisionGroup() & m_collisionMask) == 0) {
      return false;
    }
    return !m_xray || m_propertyName.empty() || info->m_gameobject->GetProperty(m_propertyName);
  }

 private:
  const LN_PhysicsControllerIgnoreSet &m_extraIgnoreControllers;
  uint32_t m_collisionMask;
  const std::string &m_propertyName;
  bool m_xray;
};

static std::string InputCodeToName(const SCA_IInputDevice::SCA_EnumInputs input_code)
{
  if (input_code >= SCA_IInputDevice::AKEY && input_code <= SCA_IInputDevice::ZKEY) {
    return std::string(1, char('A' + (input_code - SCA_IInputDevice::AKEY)));
  }
  if (input_code >= SCA_IInputDevice::ZEROKEY && input_code <= SCA_IInputDevice::NINEKEY) {
    return std::string(1, char('0' + (input_code - SCA_IInputDevice::ZEROKEY)));
  }
  switch (input_code) {
    case SCA_IInputDevice::SPACEKEY:
      return "SPACE";
    case SCA_IInputDevice::RETKEY:
      return "ENTER";
    case SCA_IInputDevice::ESCKEY:
      return "ESC";
    case SCA_IInputDevice::TABKEY:
      return "TAB";
    case SCA_IInputDevice::LEFTARROWKEY:
      return "LEFT";
    case SCA_IInputDevice::RIGHTARROWKEY:
      return "RIGHT";
    case SCA_IInputDevice::UPARROWKEY:
      return "UP";
    case SCA_IInputDevice::DOWNARROWKEY:
      return "DOWN";
    default:
      return std::to_string(int32_t(input_code));
  }
}

static std::string UnicodeToCharacterString(const uint32_t unicode)
{
  if (unicode == 0) {
    return "";
  }
  /* Unicode scalar values exclude UTF-16 surrogate code points. Reject malformed backend input
   * deterministically instead of depending on the process locale or wcrtomb's shared state. */
  if (unicode > 0x10ffffu || (unicode >= 0xd800u && unicode <= 0xdfffu)) {
    return "";
  }
  char buffer[BLI_UTF8_MAX] = {};
  const size_t length = blender::BLI_str_utf8_from_unicode(unicode, buffer, sizeof(buffer));
  return std::string(buffer, length);
}

struct KeyLoggerScanResult {
  bool pressed = false;
  std::string character;
  std::string keycode;
};

static KeyLoggerScanResult ScanKeyLogger(const LN_InputSnapshot &input_snapshot,
                                         const bool only_characters)
{
  KeyLoggerScanResult result;
  if (!input_snapshot.HasDevice()) {
    return result;
  }

  for (int32_t input_code = 0; input_code < SCA_IInputDevice::MAX_KEYS; input_code++) {
    if (!input_snapshot.GetStatus(input_code, SCA_InputEvent::JUSTACTIVATED)) {
      continue;
    }
    const std::string character = UnicodeToCharacterString(input_snapshot.GetUnicode(input_code));
    if (only_characters && character.empty()) {
      continue;
    }
    result.pressed = true;
    result.character = character;
    result.keycode = InputCodeToName(SCA_IInputDevice::SCA_EnumInputs(input_code));
    return result;
  }
  return result;
}

static float MouseLookSmoothingFactor(const float smoothing, const double delta_time)
{
  /* Mouse Look is fixed-only and normally always receives a positive step. Preserve an input
   * impulse with the reference response if a diagnostic/manual caller violates that invariant. */
  const double effective_delta_time = std::isfinite(delta_time) && delta_time > 0.0 ? delta_time :
                                                                                      (1.0 / 60.0);
  const float clamped_smoothing = std::isfinite(smoothing) ? std::clamp(smoothing, 0.0f, 1.0f) :
                                                             0.0f;
  const double retention_at_60_hz = double(clamped_smoothing) * 0.99;
  if (retention_at_60_hz <= 0.0) {
    return 1.0f;
  }
  /* Preserve the existing 60 Hz response while expressing the filter in elapsed time. A 240 Hz
   * fixed simulation therefore performs four smaller filter advances over the same duration
   * instead of making smoothing settle four times faster. */
  const double retention = std::pow(retention_at_60_hz, effective_delta_time * 60.0);
  return float(std::clamp(1.0 - retention, 0.0, 1.0));
}

static constexpr float MOUSE_LOOK_REFERENCE_PIXELS = 1000.0f;
/* After Mouse Look starts (over), it holds the cursor this long without turning: the system may
 * report where its pointer really is a moment later (on Wayland ~0.16 s after a game window opens
 * or hides its cursor again), which would read as a huge mouse move. At most this many runs, so a
 * missing time step can never stop it for good. */
static constexpr float MOUSE_LOOK_SETTLE_SECONDS = 0.2f;
static constexpr int MOUSE_LOOK_SETTLE_MAX_RUNS = 30;

static float MouseLookCenterCoordinate(const int32_t size)
{
  if (size > 0 && (size % 2) != 0) {
    return ((float(size) - 1.0f) * 0.5f) / float(size);
  }
  return 0.5f;
}

static float MouseLookPixelToRotationScale()
{
  return 1.0f / MOUSE_LOOK_REFERENCE_PIXELS;
}

static int32_t MouseLookCenterPixel(const int32_t size)
{
  return size > 0 ? int32_t(MouseLookCenterCoordinate(size) * float(size)) : 0;
}

struct MouseLookInputState {
  bool has_position = false;
  int32_t x = 0;
  int32_t y = 0;
  int32_t delta_x = 0;
  int32_t delta_y = 0;
};

static int32_t DominantMouseInputDelta(const SCA_InputEvent &event)
{
  if (event.m_values.size() <= 1) {
    return 0;
  }

  const int32_t origin = event.m_values.front();
  int32_t dominant_delta = event.m_values.back() - origin;
  for (size_t index = 1; index < event.m_values.size(); index++) {
    const int32_t delta = event.m_values[index] - origin;
    if (std::abs(delta) > std::abs(dominant_delta)) {
      dominant_delta = delta;
    }
  }
  return dominant_delta;
}

static MouseLookInputState ReadMouseLookInputState(const LN_MouseSnapshot &snapshot)
{
  MouseLookInputState state;
  state.has_position = snapshot.has_position;
  state.x = snapshot.x;
  state.y = snapshot.y;
  state.delta_x = snapshot.delta_x;
  state.delta_y = snapshot.delta_y;

  KX_KetsjiEngine *engine = ActiveEngine();
  SCA_IInputDevice *input_device = engine != nullptr ? engine->GetInputDevice() : nullptr;
  if (input_device == nullptr) {
    return state;
  }

  const SCA_InputEvent &xevent = input_device->GetInput(SCA_IInputDevice::MOUSEX);
  const SCA_InputEvent &yevent = input_device->GetInput(SCA_IInputDevice::MOUSEY);
  if (xevent.m_values.empty() || yevent.m_values.empty()) {
    return state;
  }

  state.has_position = true;
  state.x = xevent.m_values.back();
  state.y = yevent.m_values.back();
  const int32_t frame_delta_x = DominantMouseInputDelta(xevent);
  const int32_t frame_delta_y = DominantMouseInputDelta(yevent);
  if (std::abs(frame_delta_x) > std::abs(state.delta_x)) {
    state.delta_x = frame_delta_x;
  }
  if (std::abs(frame_delta_y) > std::abs(state.delta_y)) {
    state.delta_y = frame_delta_y;
  }
  return state;
}

static float ClampMouseLookDelta(float &angle, const float delta, const MT_Vector3 &range)
{
  const float min_angle = std::min(range.x(), range.y());
  const float max_angle = std::max(range.x(), range.y());
  const float clamped_angle = std::clamp(angle + delta, min_angle, max_angle);
  const float clamped_delta = clamped_angle - angle;
  angle = clamped_angle;
  return clamped_delta;
}

static MT_Vector3 MouseLookRotationVector(const int axis, const float value)
{
  MT_Vector3 rotation(0.0f, 0.0f, 0.0f);
  rotation[std::clamp(axis, 0, 2)] = value;
  return rotation;
}

static std::string ZeroFillString(const std::string &value, const int32_t width)
{
  if (width <= int32_t(value.size())) {
    return value;
  }

  const size_t sign_offset = (!value.empty() && (value[0] == '-' || value[0] == '+')) ? 1 : 0;
  std::string result = value;
  result.insert(sign_offset, size_t(width - int32_t(value.size())), '0');
  return result;
}

static float HashToUnitFloat(uint64_t value)
{
  value ^= value >> 33;
  value *= 0xff51afd7ed558ccdull;
  value ^= value >> 33;
  value *= 0xc4ceb9fe1a85ec53ull;
  value ^= value >> 33;
  return float(value & 0x00ffffffu) / float(0x01000000u);
}

static const std::string &FormatInputString(const size_t index,
                                            const std::string &a,
                                            const std::string &b,
                                            const std::string &c,
                                            const std::string &d)
{
  switch (index) {
    case 0:
      return a;
    case 1:
      return b;
    case 2:
      return c;
    default:
      return d;
  }
}

static std::string FormatStringSlots(const std::string &format,
                                     const std::string &a,
                                     const std::string &b,
                                     const std::string &c,
                                     const std::string &d)
{
  std::string result;
  result.reserve(format.size() + a.size() + b.size() + c.size() + d.size());

  size_t input_index = 0;
  for (size_t index = 0; index < format.size(); index++) {
    if (index + 1 >= format.size()) {
      result.push_back(format[index]);
      continue;
    }

    const char current = format[index];
    const char next = format[index + 1];
    if (current == '{' && next == '{') {
      result.push_back('{');
      index++;
    }
    else if (current == '}' && next == '}') {
      result.push_back('}');
      index++;
    }
    else if (current == '{' && next == '}' && input_index < 4) {
      result += FormatInputString(input_index, a, b, c, d);
      input_index++;
      index++;
    }
    else {
      result.push_back(current);
    }
  }

  return result;
}

static std::string FloatToPrintString(const float value)
{
  /* Keep diagnostic text deterministic and human-readable. IEEE-754 signed zero is useful
   * internally, but "-0" is misleading in a printed transform. */
  if (value == 0.0f) {
    return "0";
  }
  std::ostringstream stream;
  stream << value;
  return stream.str();
}

static std::string VectorToPrintString(const MT_Vector3 &value)
{
  return "(" + FloatToPrintString(value.x()) + ", " + FloatToPrintString(value.y()) + ", " +
         FloatToPrintString(value.z()) + ")";
}

static std::string ColorToPrintString(const MT_Vector4 &value)
{
  return "(" + FloatToPrintString(value.x()) + ", " + FloatToPrintString(value.y()) + ", " +
         FloatToPrintString(value.z()) + ", " + FloatToPrintString(value.w()) + ")";
}

static std::string MatrixToPrintString(const MT_Matrix3x3 &value)
{
  std::string result = "(";
  for (int row = 0; row < 3; row++) {
    if (row != 0) {
      result += ", ";
    }
    result += "(" + FloatToPrintString(value[row][0]) + ", " + FloatToPrintString(value[row][1]) +
              ", " + FloatToPrintString(value[row][2]) + ")";
  }
  result += ")";
  return result;
}

static std::string ListToPrintString(const std::vector<LN_Value> &values, const int depth)
{
  if (depth >= 4) {
    return "[...]";
  }

  std::string result = "[";
  for (size_t i = 0; i < values.size(); i++) {
    if (i != 0) {
      result += ", ";
    }
    result += ValueToPrintString(values[i], depth + 1);
  }
  result += "]";
  return result;
}

static std::string DictToPrintString(const std::map<std::string, LN_Value> &values,
                                     const int depth)
{
  if (depth >= 4) {
    return "{...}";
  }

  std::string result = "{";
  size_t index = 0;
  for (const auto &item : values) {
    if (index != 0) {
      result += ", ";
    }
    result += item.first + ": " + ValueToPrintString(item.second, depth + 1);
    index++;
  }
  result += "}";
  return result;
}

/** A value linked into a text input: nothing (no item, an empty object) is empty text, so the
 * text it sets clears; Print keeps "None". */
static std::string ValueToText(const LN_Value &value)
{
  const bool empty_reference = ELEM(value.type,
                                    LN_ValueType::ObjectRef,
                                    LN_ValueType::SceneRef,
                                    LN_ValueType::CollectionRef,
                                    LN_ValueType::DatablockRef) &&
                               value.reference_name.empty();
  return !value.exists || empty_reference ? std::string() : ValueToPrintString(value);
}

static std::string ValueToPrintString(const LN_Value &value, const int depth)
{
  if (!value.exists && value.type == LN_ValueType::None) {
    return "None";
  }

  switch (value.type) {
    case LN_ValueType::Bool:
      return value.bool_value ? "True" : "False";
    case LN_ValueType::Int:
      return std::to_string(value.int_value);
    case LN_ValueType::Float:
      return FloatToPrintString(value.float_value);
    case LN_ValueType::Vector:
      return VectorToPrintString(value.vector_value);
    case LN_ValueType::Vector4:
      return ColorToPrintString(value.vector4_value);
    case LN_ValueType::Matrix:
      return MatrixToPrintString(value.matrix_value);
    case LN_ValueType::Color:
      return ColorToPrintString(value.color_value);
    case LN_ValueType::Rotation:
      return "Rotation(deg)=" + VectorToPrintString(value.RotationEulerDegrees());
    case LN_ValueType::String:
      return value.string_value;
    case LN_ValueType::ObjectRef:
    case LN_ValueType::SceneRef:
    case LN_ValueType::CollectionRef:
    case LN_ValueType::DatablockRef:
      return value.reference_name.empty() ? "None" : value.reference_name;
    case LN_ValueType::List:
      return ListToPrintString(value.list_value, depth);
    case LN_ValueType::Dict:
      return DictToPrintString(value.dict_value, depth);
    case LN_ValueType::Generic:
    case LN_ValueType::None:
      return "None";
  }

  return "None";
}

}  // namespace

bool LN_RuntimeTree::ItemOf(const LN_Value &value, std::string &r_item, int32_t &r_count) const
{
  if (value.exists && value.type == LN_ValueType::ObjectRef) {
    /* A pickup or library object: its item and count properties, else its name and one. An
     * object that is gone is no item (not the library object of its name). */
    KX_GameObject *object = ResolveLiveObjectValue(value, true);
    if (object == nullptr) {
      return false;
    }
    EXP_Value *item = object->GetProperty("item");
    r_item = item != nullptr && !item->GetText().empty() ? item->GetText() : object->GetName();
    r_count = 1;
    if (EXP_Value *count = object->GetProperty("count")) {
      const double number = count->GetNumber();
      if (std::isfinite(number) && number >= 1.0) {
        r_count = int32_t(std::min(std::round(number), 2147483647.0));
      }
    }
    return !r_item.empty();
  }
  return LN_InventoryEntry(value, r_item, r_count);
}

std::string LN_RuntimeTree::ItemNameOf(const LN_Value &value) const
{
  std::string item;
  int32_t count = 0;
  return ItemOf(value, item, count) ? item : std::string();
}

const blender::Collection *LN_RuntimeTree::SceneItemLibrary() const
{
  KX_Scene *scene = m_gameObject != nullptr ? m_gameObject->GetScene() : nullptr;
  const blender::Scene *blender_scene = scene != nullptr ? scene->GetBlenderScene() : nullptr;
  return blender_scene != nullptr ? blender_scene->gm.item_library : nullptr;
}

LN_RuntimeTree::InventoryEditResult LN_RuntimeTree::RunInventoryEdit(
    const LN_ValueExpression &expression,
    std::vector<LN_Value> &slots,
    const LN_TickContext &context,
    std::vector<LN_Value> *other_slots,
    const LN_InventoryRules *list_rules,
    const LN_InventoryRules *other_list_rules,
    KX_GameObject *owner)
{
  const auto input = [&](const LN_InventoryInput index) {
    return ResolveValueExpression(index < expression.input_indices.size() ?
                                      expression.input_indices[index] :
                                      LN_INVALID_INDEX,
                                  MakeNoneValue(),
                                  context);
  };
  const auto int_input = [&](const LN_InventoryInput index) {
    return ValueAsInt(input(index), LN_FloatToIntMode::Round);
  };
  /* Amount 0 means all of it; below 0 nothing. */
  constexpr int32_t all = std::numeric_limits<int32_t>::max();
  /* The node's own rules: with Override, or without a list of an object (a list value). */
  LN_InventoryRules node_rules;
  node_rules.capacity = int_input(LN_INVENTORY_INPUT_CAPACITY);
  node_rules.stack = ValueAsBool(input(LN_INVENTORY_INPUT_STACK));
  node_rules.max_stack = int_input(LN_INVENTORY_INPUT_MAX_STACK);
  /* Keep Places: emptied slots stay empty places (both lists of a Move To Slot). */
  node_rules.keep_places = ValueAsBool(input(LN_INVENTORY_INPUT_KEEP_PLACES));
  if (expression.input_indices.size() > LN_INVENTORY_INPUT_LIBRARY &&
      expression.input_indices[LN_INVENTORY_INPUT_LIBRARY] != LN_INVALID_INDEX)
  {
    node_rules.library = ResolveCollectionByValue(this, input(LN_INVENTORY_INPUT_LIBRARY));
  }
  /* The list's Inventory Rules, unless this call overrides them. Accept Categories are always
   * the list's: what a list may hold is never overridden. */
  const bool use_node_rules = list_rules == nullptr ||
                              ValueAsBool(input(LN_INVENTORY_INPUT_OVERRIDE));
  const auto rules_of = [&](const LN_InventoryRules *list) {
    if (!use_node_rules && list != nullptr) {
      return *list;
    }
    LN_InventoryRules rules = node_rules;
    if (list != nullptr) {
      rules.accept_categories = list->accept_categories;
    }
    return rules;
  };
  const LN_InventoryRules rules = rules_of(list_rules);
  const LN_InventoryRules other_rules = rules_of(other_list_rules);
  /* Empty libraries mean the scene's Item Library. The scene is only needed to find library
   * objects. */
  LN_InventoryLookup lookup;
  if (expression.input1 == LN_INVENTORY_SCENE_LIBRARY) {
    lookup.scene_library = SceneItemLibrary();
  }
  if (m_gameObject != nullptr &&
      (lookup.scene_library != nullptr || rules.library != nullptr ||
       other_rules.library != nullptr))
  {
    lookup.scene = m_gameObject->GetScene();
  }
  const LN_InventoryStackLimits limits = lookup.Limits(rules);
  /* An item's categories: a pickup's own "category" first, else its library object's. */
  const auto accepts = [&](const LN_InventoryRules &list, const LN_Value &value) {
    if (list.accept_categories.empty()) {
      return true;
    }
    if (value.exists && value.type == LN_ValueType::ObjectRef) {
      KX_GameObject *object = ResolveLiveObjectValue(value, true);
      if (EXP_Value *category = object != nullptr ? object->GetProperty("category") : nullptr) {
        return LN_CategoriesOverlap(list.accept_categories,
                                    LN_NormalizedCategories(category->GetText()));
      }
      return lookup.Accepts(list, MakeStringValue(ItemNameOf(value)));
    }
    return lookup.Accepts(list, value);
  };
  InventoryEditResult result;
  const int32_t amount = int_input(LN_INVENTORY_INPUT_AMOUNT);
  switch (LN_InventoryOperation(expression.value.int_value)) {
    case LN_InventoryOperation::AddItem: {
      std::string item;
      int32_t carried = 1;
      const LN_Value given = input(LN_INVENTORY_INPUT_ITEM);
      if (!ItemOf(given, item, carried)) {
        result.amount = std::max(0, amount);
        break;
      }
      result.requested = std::max(0, amount == 0 ? carried : amount);
      if (!accepts(rules, given)) {
        result.amount = result.requested;
        result.outcome = LN_InventoryOutcome::Refused;
        break;
      }
      result.amount = LN_InventoryAdd(slots,
                                      item,
                                      amount == 0 ? carried : amount,
                                      LN_InventoryStackLimit(item, &limits),
                                      rules.stack,
                                      rules.capacity);
      result.outcome = result.amount > 0 ? LN_InventoryOutcome::NoRoom :
                                           LN_InventoryOutcome::None;
      break;
    }
    case LN_InventoryOperation::RemoveItem:
      result.amount = LN_InventoryRemoveItem(slots,
                                             ItemNameOf(input(LN_INVENTORY_INPUT_ITEM)),
                                             amount == 0 ? all : amount,
                                             rules.Layout());
      break;
    case LN_InventoryOperation::RemoveFromSlot:
      result.amount = LN_InventoryRemoveFromSlot(
          slots, int_input(LN_INVENTORY_INPUT_SLOT), amount == 0 ? all : amount, rules.Layout());
      break;
    case LN_InventoryOperation::SwapSlots:
      LN_InventorySwap(
          slots, int_input(LN_INVENTORY_INPUT_SLOT), int_input(LN_INVENTORY_INPUT_OTHER_SLOT));
      break;
    case LN_InventoryOperation::SplitStack:
      LN_InventorySplit(
          slots, int_input(LN_INVENTORY_INPUT_SLOT), amount, rules.Layout(), rules.capacity);
      break;
    case LN_InventoryOperation::Sort:
      LN_InventorySort(slots,
                       ValueAsBool(input(LN_INVENTORY_INPUT_BY_COUNT)) ?
                           LN_InventorySortKey::Count :
                           LN_InventorySortKey::Item,
                       ValueAsBool(input(LN_INVENTORY_INPUT_DESCENDING)),
                       rules.Layout());
      break;
    case LN_InventoryOperation::MoveToSlot: {
      /* The same move a grid makes (LN_InventoryApplyMove), with the node's own slots. */
      std::vector<LN_Value> &target = other_slots != nullptr ? *other_slots : slots;
      LN_InventoryMove move;
      move.from = int_input(LN_INVENTORY_INPUT_SLOT);
      move.to = int_input(LN_INVENTORY_INPUT_OTHER_SLOT);
      if (move.from >= 0 && size_t(move.from) < slots.size()) {
        move.from_item = slots[size_t(move.from)];
      }
      if (move.to >= 0 && size_t(move.to) < target.size()) {
        move.to_item = target[size_t(move.to)];
      }
      move.allow_swap = ValueAsBool(input(LN_INVENTORY_INPUT_SWAP));
      move.into_list_fallback = false;
      /* Recipes combine only inside one list: the node's (Override), else the list's. */
      std::vector<LN_Recipe> recipes;
      if (other_slots == nullptr) {
        if (use_node_rules) {
          LN_ParseRecipes(input(LN_INVENTORY_INPUT_RECIPES), recipes);
        }
        else if (!rules.recipes_property.empty() && owner != nullptr) {
          if (EXP_Value *property = owner->GetProperty(rules.recipes_property)) {
            LN_ParseRecipes(LN_GamePropertyValue(property, m_logicManager), recipes);
          }
        }
      }
      result.outcome = LN_InventoryApplyMove(move,
                                             target,
                                             other_slots != nullptr ? other_rules : rules,
                                             other_slots != nullptr ? &slots : nullptr,
                                             other_slots != nullptr ? &rules : nullptr,
                                             recipes,
                                             lookup,
                                             result.made);
      break;
    }
    case LN_InventoryOperation::Craft: {
      result.craft = true;
      const LN_Value text = input(LN_INVENTORY_INPUT_RECIPE);
      LN_Recipe recipe;
      if (text.exists && text.type == LN_ValueType::String &&
          LN_ParseRecipe(text.string_value, recipe))
      {
        /* What a recipe makes must be what the list takes. */
        for (const LN_RecipePart &made : recipe.results) {
          if (!accepts(rules, MakeStringValue(made.item))) {
            result.outcome = LN_InventoryOutcome::Refused;
            break;
          }
        }
        if (result.outcome == LN_InventoryOutcome::Refused) {
          break;
        }
        /* Amount is how many times; 0 makes as many as the ingredients allow. */
        result.outcome = LN_InventoryCraft(slots,
                                           recipe,
                                           LN_InventoryStackLimit,
                                           &limits,
                                           rules.stack,
                                           rules.capacity,
                                           amount,
                                           result.amount,
                                           rules.Layout());
        if (result.outcome == LN_InventoryOutcome::Combined) {
          result.made = recipe.results.front().item;
        }
      }
      break;
    }
  }
  return result;
}

void LN_ClearGlobalProperties()
{
  std::lock_guard<std::mutex> lock(GlobalPropertiesMutex());
  GlobalProperties().clear();
  LoadedGlobalPropertyCategories().clear();
}

LN_RuntimeTree::LN_RuntimeTree(std::shared_ptr<const LN_Program> program,
                               KX_GameObject *gameobj,
                               uint32_t scene_object_index,
                               uint32_t applied_tree_index)
    : m_program(std::move(program)),
      m_gameObject(gameobj),
      m_sceneObjectIndex(scene_object_index),
      m_appliedTreeIndex(applied_tree_index)
{
  if (m_program != nullptr) {
    const size_t rotation_cache_capacity = m_program->GetVectorExpressions().size();
    m_rotationExpressionValues.reserve(rotation_cache_capacity);
    m_rotationExpressionTicks.reserve(rotation_cache_capacity);
  }
  RefreshOptimizedPathEligibility();
  EnsureInstructionStateSize();
  EnsureBoolExpressionStateSize();
  EnsureFloatExpressionStateSize();
  EnsureTimeFlowStateSize(false);
  EnsureFlowControlStateSize(false);
  EnsureQueryExpressionStateSize();
  EnsureTreePropertyStateSize(false);
}

void LN_RuntimeTree::RefreshOptimizedPathEligibility()
{
  m_registerExpressionIRHasProductionBenefit = false;
  m_snapshotChannelMask = LN_SNAPSHOT_CHANNEL_NONE;
  m_hasTimeFlowRuntimeState = false;
  m_hasQueryExpressions = false;

  if (m_program == nullptr) {
    return;
  }

  m_registerExpressionIRHasProductionBenefit = RegisterExpressionIRHasProductionBenefit(
      m_program->GetRegisterExpressionProgram());
  m_snapshotChannelMask = LN_SnapshotChannelMaskForProgram(m_program.get());
  m_hasTimeFlowRuntimeState = m_program->GetTimeFlowStateCount() != 0u;
  m_hasQueryExpressions = !m_program->GetQueryExpressions().empty();
}

void LN_RuntimeTree::BindDenseIds(LN_DenseIdRegistry &registry)
{
  m_denseRuntimeTreeId = registry.MakeRuntimeTreeId(
      this, m_program ? m_program->GetSourceTreeName() : std::string());
  m_ownerObjectHandle = registry.MakeObjectHandle(
      m_gameObject, m_gameObject ? m_gameObject->GetName() : std::string());

  m_sharedStringIds.clear();
  m_eventSubjectIds.clear();
  m_gamePropertyIds.clear();
  m_treePropertyIds.clear();
  m_uiCommandCompletions.clear();
  m_actionCompletions.clear();

  if (m_program == nullptr) {
    return;
  }

  const std::vector<std::string> &strings = m_program->GetStringTable();
  m_sharedStringIds.reserve(strings.size());
  m_eventSubjectIds.reserve(strings.size());
  for (const std::string &value : strings) {
    m_sharedStringIds.push_back(registry.InternString(value));
    m_eventSubjectIds.push_back(registry.InternEventSubject(value));
  }

  const std::vector<LN_GamePropertyRef> &game_property_refs = m_program->GetGamePropertyRefs();
  m_gamePropertyIds.reserve(game_property_refs.size());
  for (const LN_GamePropertyRef &property_ref : game_property_refs) {
    m_gamePropertyIds.push_back(registry.InternGameProperty(property_ref.name));
  }

  const std::vector<LN_TreePropertyRef> &tree_property_refs = m_program->GetTreePropertyRefs();
  m_treePropertyIds.reserve(tree_property_refs.size());
  for (const LN_TreePropertyRef &property_ref : tree_property_refs) {
    m_treePropertyIds.push_back(registry.InternTreeProperty(property_ref.name));
  }
}

void LN_RuntimeTree::SetLogicManager(LN_Manager *logic_manager)
{
  m_logicManager = logic_manager;
}

LN_EventSubjectId LN_RuntimeTree::GetEventSubjectId(const LN_StringId string_id) const
{
  if (!string_id.IsValid() || string_id.index >= m_eventSubjectIds.size()) {
    return LN_EventSubjectId{};
  }
  return m_eventSubjectIds[string_id.index];
}

LN_GamePropertyId LN_RuntimeTree::GetGamePropertyId(const uint32_t property_ref_index) const
{
  if (property_ref_index >= m_gamePropertyIds.size()) {
    return LN_GamePropertyId{};
  }
  return m_gamePropertyIds[property_ref_index];
}

LN_TreePropertyId LN_RuntimeTree::GetTreePropertyId(const uint32_t property_ref_index) const
{
  if (property_ref_index >= m_treePropertyIds.size()) {
    return LN_TreePropertyId{};
  }
  return m_treePropertyIds[property_ref_index];
}

void LN_RuntimeTree::CaptureSnapshot(const LN_TickReadContext *tick_context,
                                     const float fixed_delta,
                                     const uint64_t tick_index,
                                     const bool force_disabled)
{
  if (!(force_disabled ? ShouldCaptureSnapshotForForcedUpdate() : ShouldCaptureSnapshot())) {
    return;
  }
  if (tick_index != UINT64_MAX && m_snapshotTick == tick_index) {
    return;
  }

  m_snapshot.Capture(m_gameObject, m_program.get(), tick_context, fixed_delta);
  m_snapshotTick = tick_index;
  m_queryCacheWarmTick = UINT64_MAX;
}

void LN_RuntimeTree::InvalidateUICollectionCaches()
{
  /* Collection read UI node inputs before the UI resolved. A UI query read then saw no UI state,
   * and an input read then still held a click or key the UI has since consumed: neither may be
   * reused by this tick's execution. */
  m_registerExpressionState.cached_tick = UINT64_MAX;
  if (m_program == nullptr) {
    return;
  }
  const std::vector<LN_QueryExpression> &queries = m_program->GetQueryExpressions();
  for (uint32_t query_index = 0; query_index < queries.size(); ++query_index) {
    const LN_QueryExpression &expression = queries[query_index];
    switch (expression.kind) {
      case LN_QueryExpressionKind::ObjectScrollView:
      case LN_QueryExpressionKind::ObjectGridView:
      case LN_QueryExpressionKind::UIElementEvent:
        QueryExpressionState(expression.runtime_state_index != LN_INVALID_INDEX ?
                                 expression.runtime_state_index :
                                 query_index)
            .cached_tick = UINT64_MAX;
        break;
      default:
        break;
    }
  }
}

namespace {
LN_Value MakeValueFromEXPValue(EXP_Value *property, LN_RuntimeTree *runtime_tree);
}  // namespace

void LN_RuntimeTree::CollectUIViews(const LN_TickContext &context)
{
  if (!m_runtimeActive || !m_enabled || m_program == nullptr || m_gameObject == nullptr ||
      m_logicManager == nullptr)
  {
    return;
  }
  KX_Scene *scene = m_gameObject->GetScene();
  if (scene == nullptr) {
    return;
  }

  const std::vector<LN_QueryExpression> &queries = m_program->GetQueryExpressions();
  for (uint32_t query_index = 0; query_index < queries.size(); ++query_index) {
    const LN_QueryExpression &expression = queries[query_index];
    if (expression.kind != LN_QueryExpressionKind::ObjectScrollView &&
        expression.kind != LN_QueryExpressionKind::ObjectGridView)
    {
      continue;
    }

    KX_GameObject *viewport =
        expression.input0 == LN_INVALID_INDEX ?
            m_gameObject :
            ResolveObjectValue(ResolveValueExpression(expression.input0, MakeNoneValue(), context),
                               true);
    if (viewport == nullptr || viewport->GetScene() != scene) {
      continue;
    }
    const bool is_overlay = scene->IsOverlayRuntimeObject(viewport);
    KX_Camera *camera = nullptr;
    switch (expression.interaction_view) {
      case LN_InteractionView::Automatic:
        camera = is_overlay ? scene->GetOverlayCamera() : PointerWorldCamera(scene, viewport);
        break;
      case LN_InteractionView::Main:
        if (!is_overlay) {
          camera = PointerWorldCamera(scene, viewport);
        }
        break;
      case LN_InteractionView::Overlay:
        if (is_overlay) {
          camera = scene->GetOverlayCamera();
        }
        break;
      case LN_InteractionView::CustomCamera:
        /* Scroll reserves input1 for Content Object, while Grid uses input1 for Camera. */
        const uint32_t camera_input = expression.kind == LN_QueryExpressionKind::ObjectScrollView ?
                                          expression.input2 :
                                          expression.input1;
        camera = camera_input != LN_INVALID_INDEX ? ResolveCameraValue(ResolveValueExpression(
                                                        camera_input, MakeNoneValue(), context)) :
                                                    nullptr;
        break;
    }
    if (camera == nullptr || camera->GetScene() != scene) {
      continue;
    }

    blender::Object *blender_object = viewport->GetBlenderObject();
    const std::optional<blender::Bounds<blender::float3>> bounds = blender_object != nullptr ?
                                                                       BKE_object_boundbox_get(
                                                                           blender_object) :
                                                                       std::nullopt;
    if (!bounds.has_value()) {
      continue;
    }
    MT_Vector3 ray_from;
    MT_Vector3 ray_to;
    if (!ComputeMouseRay(camera, std::max(camera->GetCameraFar(), 0.0f), ray_from, ray_to)) {
      /* The pointer is outside the view: keep showing and binding the items for keyboard and
       * gamepad players, but let the wheel hit nothing. */
      ray_from = ray_to = camera->NodeGetWorldPosition();
    }

    LN_UIService::ViewRegistration registration;
    registration.listener = {this, query_index};
    registration.kind = expression.kind == LN_QueryExpressionKind::ObjectScrollView ?
                            LN_UIService::ViewKind::Scroll :
                            LN_UIService::ViewKind::Grid;
    registration.viewport = viewport;
    registration.camera = camera;
    registration.ray_from = ray_from;
    registration.ray_to = ray_to;
    registration.bounds_min = MT_Vector3(bounds->min.x, bounds->min.y, bounds->min.z);
    registration.bounds_max = MT_Vector3(bounds->max.x, bounds->max.y, bounds->max.z);
    registration.world_position = viewport->NodeGetWorldPosition();
    registration.world_orientation = viewport->NodeGetWorldOrientation();
    registration.world_scale = viewport->NodeGetWorldScaling();
    registration.enabled = ResolveBoolExpression(
        expression.bool_expr_index, expression.bool_value, context);
    registration.user = std::max(
        0, ResolveIntExpression(expression.int_expr_index, expression.int_value, context));
    /* In a menu: that menu's player (each player's copy of a per-player menu). */
    if (const int32_t menu_user = m_logicManager->GetUIService().MenuUserOfObject(viewport);
        menu_user >= 0)
    {
      registration.user = menu_user;
    }
    registration.priority = ResolveIntExpression(
        expression.secondary_int_expr_index, expression.secondary_int_value, context);
    registration.padding = std::max(
        0.0f,
        ResolveFloatExpression(expression.float_expr_index, expression.float_value, context));

    if (expression.kind == LN_QueryExpressionKind::ObjectScrollView) {
      registration.content = expression.input1 != LN_INVALID_INDEX ?
                                 ResolveObjectValue(ResolveValueExpression(expression.input1,
                                                                           MakeNoneValue(),
                                                                           context),
                                                    true) :
                                 nullptr;
      registration.min_offset = ResolveVectorExpression(
          expression.input3, expression.vector_value, context);
      registration.max_offset = ResolveVectorExpression(
          expression.input4, expression.secondary_vector_value, context);
      registration.wheel_step = ResolveVectorExpression(
          expression.input5, expression.tertiary_vector_value, context);
      registration.horizontal = ResolveBoolExpression(
          expression.secondary_bool_expr_index, false, context);
      registration.vertical = ResolveBoolExpression(
          expression.tertiary_bool_expr_index, true, context);
      registration.cull_outside = ResolveBoolExpression(
          expression.quaternary_bool_expr_index, true, context);
      registration.focus_follow = ResolveBoolExpression(
          expression.quinary_bool_expr_index, true, context);
    }
    else {
      const LN_Value slots = ResolveValueExpression(expression.input2, MakeNoneValue(), context);
      auto append_slot = [&](const LN_Value &value) {
        if (KX_GameObject *object = ResolveObjectValue(value, true)) {
          if (object->GetScene() == scene &&
              std::find(registration.slots.begin(), registration.slots.end(), object) ==
                  registration.slots.end())
          {
            registration.slots.push_back(object);
          }
        }
      };
      if (slots.type == LN_ValueType::List) {
        for (const LN_Value &slot : slots.list_value) {
          append_slot(slot);
        }
      }
      else {
        append_slot(slots);
      }
      /* The list: Property names a game property of Object (empty: whose menu this tree's
       * object is in, else that object), read as it is now, so the grid can write it back;
       * Items gives it while Property is empty. */
      const auto grid_input = [&](const LN_GridViewInput input) {
        return input < expression.input_indices.size() ? expression.input_indices[input] :
                                                         LN_INVALID_INDEX;
      };
      LN_UIService &ui = m_logicManager->GetUIService();
      std::string list_property = ResolveStringExpression(
          grid_input(LN_GRID_INPUT_PROPERTY), std::string(), context);
      if (!list_property.empty()) {
        KX_GameObject *list_object = nullptr;
        if (const uint32_t object_input = grid_input(LN_GRID_INPUT_OBJECT);
            object_input != LN_INVALID_INDEX)
        {
          list_object = ResolveObjectValue(
              ResolveValueExpression(object_input, MakeNoneValue(), context), true);
        }
        else {
          /* A menu whose owner is gone has no list: never the menu panel's own property. */
          list_object = ui.MenuUserOfObject(m_gameObject) >= 0 ?
                            ui.MenuOwnerOfObject(m_gameObject) :
                            m_gameObject;
        }
        if (list_object != nullptr && list_object->GetScene() == scene &&
            !m_logicManager->IsGameObjectQueuedForRemoval(list_object))
        {
          if (EXP_Value *property = list_object->GetProperty(list_property)) {
            LN_Value list = MakeValueFromEXPValue(property, this);
            if (list.exists && list.type == LN_ValueType::List) {
              ShareValue(list);
              registration.items = std::move(list.list_value);
            }
            else {
              ui.WarnGridListNotAList(list_object, list_property);
            }
          }
          registration.list_object = list_object;
          registration.list_property = std::move(list_property);
        }
      }
      else {
        LN_Value items = ResolveValueExpression(expression.input3, MakeNoneValue(), context);
        if (items.type == LN_ValueType::List) {
          ShareValue(items);
          registration.items = std::move(items.list_value);
        }
      }
      LN_UIService::GridInventory &inventory = registration.inventory;
      inventory.handle_moves = registration.list_object != nullptr &&
                               ResolveBoolExpression(
                                   grid_input(LN_GRID_INPUT_HANDLE_MOVES), false, context);
      if (inventory.handle_moves) {
        inventory.split_action = ResolveStringExpression(
            grid_input(LN_GRID_INPUT_SPLIT_ACTION), std::string(), context);
        inventory.split_amount = std::max(
            0, ResolveIntExpression(grid_input(LN_GRID_INPUT_SPLIT_AMOUNT), 0, context));
        inventory.swap = ResolveBoolExpression(grid_input(LN_GRID_INPUT_SWAP), true, context);
        inventory.drop_into_world = ResolveBoolExpression(
            grid_input(LN_GRID_INPUT_DROP_INTO_WORLD), false, context);
        inventory.drop_at_pointer = ResolveBoolExpression(
            grid_input(LN_GRID_INPUT_DROP_AT_POINTER), false, context);
        inventory.drop_in_front = ResolveBoolExpression(
            grid_input(LN_GRID_INPUT_DROP_IN_FRONT), false, context);
        inventory.distance = std::max(
            0.0f, ResolveFloatExpression(grid_input(LN_GRID_INPUT_DISTANCE), 1.5f, context));
        inventory.hover_height = ResolveFloatExpression(
            grid_input(LN_GRID_INPUT_HOVER_HEIGHT), 0.0f, context);
      }
      if (const uint32_t hint_input = grid_input(LN_GRID_INPUT_HINT_TEXT);
          hint_input != LN_INVALID_INDEX)
      {
        KX_GameObject *hint = ResolveObjectValue(
            ResolveValueExpression(hint_input, MakeNoneValue(), context), true);
        if (hint != nullptr && hint->GetScene() == scene) {
          inventory.hint_text = hint;
          inventory.accept_hint = ResolveStringExpression(
              grid_input(LN_GRID_INPUT_ACCEPT_HINT), "Put {name} here", context);
          inventory.refuse_hint = ResolveStringExpression(
              grid_input(LN_GRID_INPUT_REFUSE_HINT), "{name} does not fit here", context);
        }
      }
      registration.show_categories = ResolveStringExpression(
          grid_input(LN_GRID_INPUT_SHOW_CATEGORIES), std::string(), context);
      registration.search = ResolveStringExpression(
          grid_input(LN_GRID_INPUT_SEARCH), std::string(), context);
      registration.use_action = ResolveStringExpression(
          grid_input(LN_GRID_INPUT_USE_ACTION), std::string(), context);
      registration.use_keys = ResolveStringExpression(
          grid_input(LN_GRID_INPUT_USE_KEYS), std::string(), context);
      registration.requested_first_index = ResolveIntExpression(
          expression.input4, expression.int_value, context);
      registration.wheel_rows = std::max(1, ResolveIntExpression(expression.input5, 1, context));
      registration.columns = std::max(
          1, ResolveIntExpression(expression.tertiary_int_expr_index, 1, context));
      registration.cull_outside = ResolveBoolExpression(
          expression.secondary_bool_expr_index, true, context);
      registration.focus_follow = ResolveBoolExpression(
          expression.tertiary_bool_expr_index, true, context);
      /* The list's Inventory Rules (its game property's set): how the grid stacks, keeps places
       * and combines it, and the library its icons come from (none: the scene's Item Library,
       * like the inventory nodes). A list Items gives follows the defaults. */
      registration.rules = m_logicManager->InventoryRulesOf(registration.list_object,
                                                            registration.list_property);
      registration.icon_library = const_cast<blender::Collection *>(
          registration.rules.library != nullptr ? registration.rules.library :
                                                  SceneItemLibrary());
      registration.show_counts = ResolveBoolExpression(
          expression.quaternary_bool_expr_index, true, context);
      registration.show_names = ResolveBoolExpression(
          expression.quinary_bool_expr_index, false, context);
      registration.show_empty = ResolveBoolExpression(
          expression.senary_bool_expr_index, false, context);
      registration.slot_template = expression.input7 != LN_INVALID_INDEX ?
                                       ResolveObjectValue(ResolveValueExpression(expression.input7,
                                                                                 MakeNoneValue(),
                                                                                 context),
                                                          true) :
                                       nullptr;
      registration.rows = std::max(
          0, ResolveIntExpression(expression.quaternary_int_expr_index, 0, context));
      registration.spacing = ResolveVectorExpression(
          expression.input8, MT_Vector3(0.0f, 0.0f, 0.0f), context);
      registration.horizontal = expression.grid_horizontal;
      registration.vertical = !expression.grid_horizontal;
    }
    registration.overlay = is_overlay;
    KX_GameObject *scrollbar = expression.input9 != LN_INVALID_INDEX ?
                                   ResolveObjectValue(ResolveValueExpression(expression.input9,
                                                                             MakeNoneValue(),
                                                                             context),
                                                      true) :
                                   nullptr;
    const auto local_bounds = [](KX_GameObject *object, MT_Vector3 &r_min, MT_Vector3 &r_max) {
      blender::Object *blender_object = object->GetBlenderObject();
      const std::optional<blender::Bounds<blender::float3>> object_bounds =
          blender_object != nullptr ? BKE_object_boundbox_get(blender_object) : std::nullopt;
      if (!object_bounds.has_value()) {
        return false;
      }
      r_min = MT_Vector3(object_bounds->min.x, object_bounds->min.y, object_bounds->min.z);
      r_max = MT_Vector3(object_bounds->max.x, object_bounds->max.y, object_bounds->max.z);
      return true;
    };
    if (scrollbar != nullptr && scrollbar->GetScene() == scene &&
        local_bounds(scrollbar, registration.scrollbar_min, registration.scrollbar_max))
    {
      registration.scrollbar = scrollbar;
      /* The handle is the bar's first mesh child, as a slot's icon is the slot's. */
      for (KX_GameObject *child : scrollbar->GetChildren()) {
        if (child->GetGameObjectType() != SCA_IObject::OBJ_TEXT && child->GetMeshCount() > 0 &&
            local_bounds(child, registration.handle_min, registration.handle_max))
        {
          registration.scrollbar_handle = child;
          break;
        }
      }
    }
    m_logicManager->GetUIService().RegisterView(std::move(registration));
  }
}

size_t LN_RuntimeTree::WarmQueryCache(const LN_TickContext &context)
{
  if (!m_runtimeActive || !m_enabled || m_program == nullptr ||
      m_queryCacheWarmTick == context.tick_index || !m_hasQueryExpressions)
  {
    return 0;
  }

  const std::vector<LN_QueryExpression> &queries = m_program->GetQueryExpressions();
  size_t warmed_count = 0;
  for (uint32_t query_index = 0; query_index < queries.size(); query_index++) {
    if (!QueryExpressionCanWarmCache(queries[query_index])) {
      continue;
    }
    ResolveQueryExpression(query_index, context);
    warmed_count++;
  }
  m_queryCacheWarmTick = context.tick_index;
  return warmed_count;
}

bool LN_RuntimeTree::ShouldCaptureSnapshot() const
{
  return m_enabled && ShouldCaptureSnapshotForForcedUpdate();
}

bool LN_RuntimeTree::ShouldCaptureSnapshotForForcedUpdate() const
{
  const bool needs_snapshot_context = m_snapshotChannelMask != LN_SNAPSHOT_CHANNEL_NONE ||
                                      (m_program != nullptr &&
                                       m_program->GetDependencySummary().input_channels !=
                                           LN_DEP_INPUT_NONE);
  return m_runtimeActive && m_program != nullptr && m_gameObject != nullptr &&
         !m_gameObject->IsLogicSuspended() && needs_snapshot_context;
}

const std::vector<LN_EventEntry> &LN_RuntimeTree::ActiveTickEvents() const
{
  if (m_logicManager != nullptr) {
    return m_logicManager->GetEventBus().TickEvents();
  }
  static const std::vector<LN_EventEntry> empty_events;
  return empty_events;
}

const LN_EventEntry *LN_RuntimeTree::FindFirstActiveEvent(const LN_EventSubjectId subject_id,
                                                          const std::string &subject,
                                                          KX_GameObject *filter_target) const
{
  if (m_logicManager == nullptr) {
    return nullptr;
  }
  const LN_ObjectHandle target_handle = filter_target == m_gameObject ?
                                            m_ownerObjectHandle :
                                            m_logicManager->FindObjectHandle(filter_target);
  return m_logicManager->GetEventBus().FindFirst(subject_id, subject, target_handle);
}

void LN_RuntimeTree::ExecuteForcedUpdate(LN_CommandBuffer &command_buffer,
                                         const LN_TickContext &context,
                                         LN_RuntimeProfileCounters *profile_counters)
{
  ClearRuntimeFallbackReport(context.tick_index);
  if (!m_runtimeActive || m_program == nullptr || m_gameObject == nullptr) {
    return;
  }
  if (m_gameObject->IsLogicSuspended()) {
    return;
  }

  if (context.tick_index == UINT64_MAX || m_loopIterationBudgetTick != context.tick_index) {
    m_remainingLoopIterationBudget = kMaxLoopIterationsPerTick;
    m_loopIterationBudgetTick = context.tick_index;
  }

  LN_RuntimeProfileCounters *previous_profile_counters = m_activeProfileCounters;
  if (profile_counters != nullptr) {
    m_activeProfileCounters = profile_counters;
  }

  if (!m_initExecuted) {
    ExecuteEvent(LN_Event::OnInit,
                 m_program->GetInstructionHeaders(LN_Event::OnInit),
                 command_buffer,
                 context);
    m_initExecuted = true;
  }

  if (m_hasTimeFlowRuntimeState) {
    TickLatentTimeFlowStates(context);
  }

  ExecuteEvent(
      context.event, m_program->GetInstructionHeaders(context.event), command_buffer, context);

  if (profile_counters != nullptr) {
    m_activeProfileCounters = previous_profile_counters;
  }
}

namespace {

/** Without a tree, objects become references of `shared_manager`'s shared domain (any tree and
 * the UI service find the very object), else they are kept by name. */
LN_Value MakeValueFromEXPValue(EXP_Value *property,
                               LN_RuntimeTree *runtime_tree,
                               LN_Manager *shared_manager)
{
  if (property == nullptr) {
    return LN_Value();
  }

  LN_Value value;
  value.exists = true;
  switch (property->GetValueType()) {
    case VALUE_BOOL_TYPE:
      value.type = LN_ValueType::Bool;
      value.bool_value = property->GetNumber() != 0.0;
      break;
    case VALUE_INT_TYPE:
      value.type = LN_ValueType::Int;
      value.int_value = int32_t(property->GetNumber());
      break;
    case VALUE_FLOAT_TYPE:
      value.type = LN_ValueType::Float;
      value.float_value = float(property->GetNumber());
      break;
    case VALUE_STRING_TYPE:
      value.type = LN_ValueType::String;
      value.string_value = property->GetText();
      break;
    case VALUE_VECTOR2_TYPE: {
      value.type = LN_ValueType::Vector;
      const MT_Vector2 &vector = static_cast<const EXP_Vector2Value *>(property)->GetVector2();
      value.vector_value = MT_Vector3(vector.x(), vector.y(), 0.0f);
      break;
    }
    case VALUE_VECTOR3_TYPE:
      value.type = LN_ValueType::Vector;
      value.vector_value = static_cast<const EXP_Vector3Value *>(property)->GetVector3();
      break;
    case VALUE_VECTOR4_TYPE:
      value.type = LN_ValueType::Vector4;
      value.vector4_value = static_cast<const EXP_Vector4Value *>(property)->GetVector4();
      break;
    case VALUE_OBJECT_TYPE: {
      KX_GameObject *object = static_cast<KX_ObjectReferenceValue *>(property)->GetObject();
      if (runtime_tree == nullptr && object != nullptr) {
        /* No tree to hold a reference (LN_GamePropertyValue). */
        value.type = LN_ValueType::ObjectRef;
        value.reference_name = object->GetName();
        if (shared_manager != nullptr) {
          value.runtime_ref = shared_manager->MakeSharedObjectRef(object, value.reference_name);
        }
        return value;
      }
      return MakeObjectValue(runtime_tree, object);
    }
    case VALUE_ARRAY_TYPE:
      value.type = LN_ValueType::List;
      for (EXP_Value *item : static_cast<EXP_ArrayValue *>(property)->GetItems()) {
        value.list_value.push_back(MakeValueFromEXPValue(item, runtime_tree, shared_manager));
      }
      break;
    case VALUE_DICT_TYPE:
      value.type = LN_ValueType::Dict;
      for (const auto &entry : static_cast<EXP_DictValue *>(property)->GetEntries()) {
        value.dict_value.emplace(entry.first,
                                 MakeValueFromEXPValue(entry.second, runtime_tree, shared_manager));
      }
      break;
    default:
      value.type = LN_ValueType::String;
      value.string_value = property->GetText();
      break;
  }
  return value;
}

LN_Value MakeValueFromEXPValue(EXP_Value *property, LN_RuntimeTree *runtime_tree)
{
  return MakeValueFromEXPValue(property, runtime_tree, nullptr);
}

}  // namespace

LN_Value LN_GamePropertyValue(EXP_Value *property, LN_Manager *manager)
{
  return MakeValueFromEXPValue(property, nullptr, manager);
}

void LN_RuntimeTree::ExecuteReady(LN_CommandBuffer &command_buffer,
                                  const LN_TickContext &context,
                                  LN_RuntimeProfileCounters *profile_counters)
{
  ClearRuntimeFallbackReport(context.tick_index);
  if (!m_runtimeActive || !m_enabled || m_program == nullptr || m_gameObject == nullptr) {
    return;
  }
  if (context.tick_index == UINT64_MAX || m_loopIterationBudgetTick != context.tick_index) {
    m_remainingLoopIterationBudget = kMaxLoopIterationsPerTick;
    m_loopIterationBudgetTick = context.tick_index;
  }

  LN_RuntimeProfileCounters *previous_profile_counters = m_activeProfileCounters;
  if (profile_counters != nullptr) {
    m_activeProfileCounters = profile_counters;
  }

  if (!m_initExecuted) {
    /* A tree that starts while paused (a menu added then) initializes fully, so its UI event
     * routes work at once. On Init is setup, not game time passing. */
    const bool ui_event_routes_only = std::exchange(m_uiEventRoutesOnly, false);
    ExecuteEvent(LN_Event::OnInit,
                 m_program->GetInstructionHeaders(LN_Event::OnInit),
                 command_buffer,
                 context);
    m_uiEventRoutesOnly = ui_event_routes_only;
    m_initExecuted = true;
  }

  /* While paused, only states that UI event routes armed are ticked. Game time is frozen then, so
   * of those Wait for Next Frame completes and the rest advance only with Ignore Timescale. */
  if (m_hasTimeFlowRuntimeState) {
    TickLatentTimeFlowStates(context);
  }

  ExecuteEvent(
      context.event, m_program->GetInstructionHeaders(context.event), command_buffer, context);

  if (profile_counters != nullptr) {
    m_activeProfileCounters = previous_profile_counters;
  }
}

bool LN_RuntimeTree::CanTick() const
{
  if (!m_runtimeActive || !m_enabled || m_program == nullptr || m_gameObject == nullptr) {
    return false;
  }

  return !m_gameObject->IsLogicSuspended();
}

bool LN_RuntimeTree::CollectsObjectUI() const
{
  return CanTick() &&
         (m_program->GetDependencySummary().query_channels & LN_DEP_QUERY_OBJECT_UI) != 0u;
}

bool LN_RuntimeTree::CanTick(const LN_Event event) const
{
  if (!CanTick()) {
    return false;
  }

  /* Initialization is deliberately owned by the first recurring phase (normally Update), so
   * On Init also runs for trees that have no instructions in that recurring phase. */
  if (!m_initExecuted) {
    return true;
  }

  if (!m_program->GetInstructionHeaders(event).empty()) {
    return true;
  }

  if (m_hasTimeFlowRuntimeState) {
    const bool has_phase_time_flow = std::any_of(
        m_timeFlowStates.begin(),
        m_timeFlowStates.end(),
        [event](const TimeFlowRuntimeState &state) {
          return state.event == event && (state.active || !state.pending_pulses.empty());
        });
    if (has_phase_time_flow) {
      return true;
    }
  }

  return false;
}

void LN_RuntimeTree::CollectPauseProcessingObjects(std::vector<KX_GameObject *> &r_objects)
{
  if (!CanTick() || m_program == nullptr || !m_program->HasPauseProcessingRequests()) {
    return;
  }

  KX_Scene *scene = m_gameObject->GetScene();
  if (scene == nullptr) {
    return;
  }

  /* The policy-owning object remains active so it can coordinate the selected scope. */
  r_objects.push_back(m_gameObject);

  LN_TickContext static_context;
  static_context.event = LN_Event::OnUpdate;
  static_context.game_paused = true;

  for (const LN_PauseProcessingRequest &request : m_program->GetPauseProcessingRequests()) {
    switch (request.scope) {
      case LN_PauseProcessingScope::Owner:
        break;
      case LN_PauseProcessingScope::ObjectHierarchy: {
        KX_GameObject *root = m_gameObject;
        if (request.target_value_expr_index != LN_INVALID_INDEX) {
          KX_GameObject *resolved = ResolveObjectValue(ResolveValueExpression(
              request.target_value_expr_index, MakeNoneValue(), static_context));
          if (resolved != nullptr) {
            root = resolved;
          }
        }
        r_objects.push_back(root);
        const std::vector<KX_GameObject *> descendants = root->GetChildrenRecursive();
        r_objects.insert(r_objects.end(), descendants.begin(), descendants.end());
        break;
      }
      case LN_PauseProcessingScope::Collection: {
        if (request.target_value_expr_index == LN_INVALID_INDEX) {
          break;
        }
        const LN_Value collection_value = ResolveValueExpression(
            request.target_value_expr_index, MakeNoneValue(), static_context);
        blender::Collection *collection = ResolveCollectionByValue(this, collection_value);
        if (collection == nullptr) {
          break;
        }

        std::vector<blender::Collection *> pending_collections = {collection};
        std::unordered_set<blender::Collection *> visited_collections;
        while (!pending_collections.empty()) {
          blender::Collection *current = pending_collections.back();
          pending_collections.pop_back();
          if (current == nullptr || !visited_collections.insert(current).second) {
            continue;
          }
          for (blender::CollectionObject &collection_object : current->gobject) {
            if (collection_object.ob != nullptr) {
              if (KX_GameObject *game_object = scene->GetGameObjectFromObject(
                      collection_object.ob))
              {
                r_objects.push_back(game_object);
              }
            }
          }
          for (blender::CollectionChild &child : current->children) {
            if (child.collection != nullptr) {
              pending_collections.push_back(child.collection);
            }
          }
        }
        /* Overlay collections can contain runtime-generated hierarchy and collection-instance
         * members whose duplicated Blender objects are not direct collection entries. They are
         * still part of the user's collection scope and must keep their trees and visual
         * animations active while the game is paused. */
        scene->AppendOverlayCollectionRuntimeObjects(collection, r_objects);
        break;
      }
      case LN_PauseProcessingScope::Scene:
        if (scene->GetObjectList() != nullptr) {
          for (KX_GameObject *game_object : *scene->GetObjectList()) {
            if (game_object != nullptr) {
              r_objects.push_back(game_object);
            }
          }
        }
        break;
    }
  }
}

bool LN_RuntimeTree::OwnsGameObject(KX_GameObject *gameobj) const
{
  return gameobj != nullptr && m_gameObject == gameobj;
}

void LN_RuntimeTree::DetachGameObject()
{
  InvalidateObjectRef(m_gameObject);
  m_gameObject = nullptr;
  m_runtimeActive = false;
}

void LN_RuntimeTree::SetProgram(std::shared_ptr<const LN_Program> program)
{
  if (m_program == program) {
    return;
  }

  m_programRevision++;
  m_actionCompletions.clear();
  m_denseRuntimeTreeId = LN_RuntimeTreeId{};
  m_sharedStringIds.clear();
  m_eventSubjectIds.clear();
  m_gamePropertyIds.clear();
  m_treePropertyIds.clear();

  if (m_program != nullptr && program != nullptr &&
      program->CanPreserveRuntimeStateWhenReplacing(*m_program))
  {
    m_program = std::move(program);
    m_rotationExpressionValues.clear();
    m_rotationExpressionTicks.clear();
    const size_t rotation_cache_capacity = m_program->GetVectorExpressions().size();
    m_rotationExpressionValues.reserve(rotation_cache_capacity);
    m_rotationExpressionTicks.reserve(rotation_cache_capacity);
    RefreshOptimizedPathEligibility();
    m_snapshotTick = UINT64_MAX;
    m_queryCacheWarmTick = UINT64_MAX;
    EnsureTimeFlowStateSize(true);
    EnsureFlowControlStateSize(true);
    EnsureQueryExpressionStateSize();
    EnsureTreePropertyStateSize(true);
    return;
  }

  m_program = std::move(program);
  m_rotationExpressionValues.clear();
  m_rotationExpressionTicks.clear();
  if (m_program != nullptr) {
    const size_t rotation_cache_capacity = m_program->GetVectorExpressions().size();
    m_rotationExpressionValues.reserve(rotation_cache_capacity);
    m_rotationExpressionTicks.reserve(rotation_cache_capacity);
  }
  RefreshOptimizedPathEligibility();
  m_initExecuted = false;
  m_snapshotTick = UINT64_MAX;
  m_queryCacheWarmTick = UINT64_MAX;
  m_executionScopeSerial = 0;
  m_activeExecutionScopeSerial = 0;
  EnsureInstructionStateSize();
  EnsureBoolExpressionStateSize();
  EnsureFloatExpressionStateSize();
  EnsureTimeFlowStateSize(false);
  EnsureFlowControlStateSize(false);
  EnsureQueryExpressionStateSize();
  EnsureTreePropertyStateSize(false);
  m_loopStates.clear();
  m_activeLoopBodyFrame = LN_INVALID_INDEX;
  m_stateArena.Clear();
}

LN_RuntimeRef LN_RuntimeTree::MakeObjectRef(KX_GameObject *gameobj, const std::string &debug_name)
{
  if (gameobj == nullptr) {
    LN_RuntimeRef runtime_ref;
    runtime_ref.kind = LN_RuntimeRefKind::Object;
    runtime_ref.debug_name = debug_name;
    return runtime_ref;
  }

  for (uint32_t index = 0; index < m_objectRefs.size(); index++) {
    ObjectRefSlot &slot = m_objectRefs[index];
    if (slot.object == gameobj) {
      LN_RuntimeRef runtime_ref;
      runtime_ref.kind = LN_RuntimeRefKind::Object;
      runtime_ref.slot = index;
      runtime_ref.generation = slot.generation;
      runtime_ref.debug_name = slot.debug_name;
      return runtime_ref;
    }
  }

  for (uint32_t index = 0; index < m_objectRefs.size(); index++) {
    ObjectRefSlot &slot = m_objectRefs[index];
    if (slot.object == nullptr) {
      slot.object = gameobj;
      slot.generation++;
      slot.debug_name = debug_name;
      if (m_logicManager != nullptr) {
        m_logicManager->RegisterRuntimeObjectRef(this, gameobj);
      }

      LN_RuntimeRef runtime_ref;
      runtime_ref.kind = LN_RuntimeRefKind::Object;
      runtime_ref.slot = index;
      runtime_ref.generation = slot.generation;
      runtime_ref.debug_name = slot.debug_name;
      return runtime_ref;
    }
  }

  ObjectRefSlot slot;
  slot.object = gameobj;
  slot.debug_name = debug_name;
  m_objectRefs.push_back(slot);
  if (m_logicManager != nullptr) {
    m_logicManager->RegisterRuntimeObjectRef(this, gameobj);
  }

  LN_RuntimeRef runtime_ref;
  runtime_ref.kind = LN_RuntimeRefKind::Object;
  runtime_ref.slot = uint32_t(m_objectRefs.size() - 1);
  runtime_ref.generation = slot.generation;
  runtime_ref.debug_name = slot.debug_name;
  return runtime_ref;
}

KX_GameObject *LN_RuntimeTree::ResolveObjectRef(const LN_RuntimeRef &runtime_ref) const
{
  if (runtime_ref.shared_domain != 0) {
    return m_logicManager ? m_logicManager->ResolveSharedObjectRef(runtime_ref) : nullptr;
  }
  if (runtime_ref.kind != LN_RuntimeRefKind::Object || runtime_ref.slot >= m_objectRefs.size()) {
    if (runtime_ref.kind == LN_RuntimeRefKind::Object && runtime_ref.IsValid()) {
      RecordRuntimeSystemFallback(
          LN_RuntimeFallbackReason::StaleHandle,
          runtime_ref.debug_name,
          "fallback_paths",
          "Refresh the cached object handle after object deletion, scene reload, or generation "
          "invalidation.");
    }
    return nullptr;
  }

  const ObjectRefSlot &slot = m_objectRefs[runtime_ref.slot];
  if (slot.generation != runtime_ref.generation) {
    RecordRuntimeSystemFallback(
        LN_RuntimeFallbackReason::StaleHandle,
        runtime_ref.debug_name.empty() ? slot.debug_name : runtime_ref.debug_name,
        "fallback_paths",
        "Refresh the cached object handle after object deletion, scene reload, or generation "
        "invalidation.");
    return nullptr;
  }
  return slot.object;
}

KX_GameObject *LN_RuntimeTree::ResolveCollisionPayloadObjectRef(
    const LN_RuntimeRef &object_ref) const
{
  if (object_ref.kind != LN_RuntimeRefKind::Object || object_ref.slot >= m_objectRefs.size()) {
    return nullptr;
  }
  const ObjectRefSlot &slot = m_objectRefs[object_ref.slot];
  return slot.generation == object_ref.generation ? slot.object : nullptr;
}

void LN_RuntimeTree::InvalidateObjectRef(KX_GameObject *gameobj)
{
  if (gameobj == nullptr) {
    return;
  }

  std::erase_if(m_collisionEventPayloads, [&](const CollisionEventPayload &payload) {
    return ResolveCollisionPayloadObjectRef(payload.owner_ref) == gameobj;
  });

  auto scrubCollisionResult = [&](CollisionResult &result) {
    if (result.owner == gameobj) {
      result = CollisionResult{};
      return;
    }
    if (result.hit_object == gameobj) {
      result.hit_object = nullptr;
    }
    std::erase(result.hit_objects, gameobj);
    if (result.hit_object == nullptr && !result.hit_objects.empty()) {
      result.hit_object = result.hit_objects.front();
    }
    if (result.detail != CollisionResult::Detail::StateOnly && result.hit_objects.empty()) {
      result.hit = false;
      result.contact_count = 0;
      result.hit_points.clear();
      result.hit_normals.clear();
    }
  };
  for (CollisionResult &result : m_collisionResults) {
    scrubCollisionResult(result);
  }
  scrubCollisionResult(m_collisionExitPayloadScratch);

  bool invalidated = false;
  for (ObjectRefSlot &slot : m_objectRefs) {
    if (slot.object == gameobj) {
      slot.object = nullptr;
      slot.generation++;
      invalidated = true;
    }
  }
  if (invalidated && m_logicManager != nullptr) {
    m_logicManager->UnregisterRuntimeObjectRef(this, gameobj);
  }
}

LN_RuntimeRef LN_RuntimeTree::MakeSceneRef(KX_Scene *scene, const std::string &debug_name)
{
  if (scene == nullptr) {
    return MakeInvalidRef(LN_RuntimeRefKind::Scene, debug_name);
  }

  for (uint32_t index = 0; index < m_runtimeRefs.size(); index++) {
    RuntimeRefSlot &slot = m_runtimeRefs[index];
    if (slot.kind == LN_RuntimeRefKind::Scene && slot.pointer == scene) {
      LN_RuntimeRef runtime_ref;
      runtime_ref.kind = LN_RuntimeRefKind::Scene;
      runtime_ref.slot = index;
      runtime_ref.generation = slot.generation;
      runtime_ref.debug_name = slot.debug_name;
      return runtime_ref;
    }
  }

  for (uint32_t index = 0; index < m_runtimeRefs.size(); index++) {
    RuntimeRefSlot &slot = m_runtimeRefs[index];
    if (slot.pointer == nullptr) {
      slot.pointer = scene;
      slot.kind = LN_RuntimeRefKind::Scene;
      slot.generation++;
      slot.debug_name = debug_name;
      LN_RuntimeRef runtime_ref;
      runtime_ref.kind = LN_RuntimeRefKind::Scene;
      runtime_ref.slot = index;
      runtime_ref.generation = slot.generation;
      runtime_ref.debug_name = slot.debug_name;
      return runtime_ref;
    }
  }

  RuntimeRefSlot slot;
  slot.pointer = scene;
  slot.kind = LN_RuntimeRefKind::Scene;
  slot.debug_name = debug_name;
  m_runtimeRefs.push_back(slot);

  LN_RuntimeRef runtime_ref;
  runtime_ref.kind = LN_RuntimeRefKind::Scene;
  runtime_ref.slot = uint32_t(m_runtimeRefs.size() - 1);
  runtime_ref.generation = slot.generation;
  runtime_ref.debug_name = slot.debug_name;
  return runtime_ref;
}

KX_Scene *LN_RuntimeTree::ResolveSceneRef(const LN_RuntimeRef &runtime_ref) const
{
  if (runtime_ref.kind == LN_RuntimeRefKind::Scene && runtime_ref.IsValid() &&
      runtime_ref.slot < m_runtimeRefs.size())
  {
    const RuntimeRefSlot &slot = m_runtimeRefs[runtime_ref.slot];
    if (slot.kind == LN_RuntimeRefKind::Scene && slot.generation == runtime_ref.generation) {
      return static_cast<KX_Scene *>(slot.pointer);
    }
  }
  if (runtime_ref.kind == LN_RuntimeRefKind::Scene && runtime_ref.IsValid()) {
    RecordRuntimeSystemFallback(
        LN_RuntimeFallbackReason::StaleHandle,
        runtime_ref.debug_name,
        "fallback_paths",
        "Refresh the cached scene handle after scene unload, reload, or generation invalidation.");
  }
  return nullptr;
}

LN_RuntimeRef LN_RuntimeTree::MakeCollectionRef(blender::Collection *collection,
                                                const std::string &debug_name)
{
  if (collection == nullptr) {
    return MakeInvalidRef(LN_RuntimeRefKind::Collection, debug_name);
  }

  for (uint32_t index = 0; index < m_runtimeRefs.size(); index++) {
    RuntimeRefSlot &slot = m_runtimeRefs[index];
    if (slot.kind == LN_RuntimeRefKind::Collection && slot.pointer == collection) {
      LN_RuntimeRef runtime_ref;
      runtime_ref.kind = LN_RuntimeRefKind::Collection;
      runtime_ref.slot = index;
      runtime_ref.generation = slot.generation;
      runtime_ref.debug_name = slot.debug_name;
      return runtime_ref;
    }
  }

  for (uint32_t index = 0; index < m_runtimeRefs.size(); index++) {
    RuntimeRefSlot &slot = m_runtimeRefs[index];
    if (slot.pointer == nullptr) {
      slot.pointer = collection;
      slot.kind = LN_RuntimeRefKind::Collection;
      slot.generation++;
      slot.debug_name = debug_name;
      LN_RuntimeRef runtime_ref;
      runtime_ref.kind = LN_RuntimeRefKind::Collection;
      runtime_ref.slot = index;
      runtime_ref.generation = slot.generation;
      runtime_ref.debug_name = slot.debug_name;
      return runtime_ref;
    }
  }

  RuntimeRefSlot slot;
  slot.pointer = collection;
  slot.kind = LN_RuntimeRefKind::Collection;
  slot.debug_name = debug_name;
  m_runtimeRefs.push_back(slot);

  LN_RuntimeRef runtime_ref;
  runtime_ref.kind = LN_RuntimeRefKind::Collection;
  runtime_ref.slot = uint32_t(m_runtimeRefs.size() - 1);
  runtime_ref.generation = slot.generation;
  runtime_ref.debug_name = slot.debug_name;
  return runtime_ref;
}

blender::Collection *LN_RuntimeTree::ResolveCollectionRef(const LN_RuntimeRef &runtime_ref) const
{
  if (runtime_ref.kind != LN_RuntimeRefKind::Collection ||
      runtime_ref.slot >= m_runtimeRefs.size())
  {
    if (runtime_ref.kind == LN_RuntimeRefKind::Collection && runtime_ref.IsValid()) {
      RecordRuntimeSystemFallback(
          LN_RuntimeFallbackReason::StaleHandle,
          runtime_ref.debug_name,
          "fallback_paths",
          "Refresh the cached collection handle after datablock mutation or generation "
          "invalidation.");
    }
    return nullptr;
  }
  const RuntimeRefSlot &slot = m_runtimeRefs[runtime_ref.slot];
  if (slot.kind != LN_RuntimeRefKind::Collection || slot.generation != runtime_ref.generation) {
    RecordRuntimeSystemFallback(LN_RuntimeFallbackReason::StaleHandle,
                                runtime_ref.debug_name.empty() ? slot.debug_name :
                                                                 runtime_ref.debug_name,
                                "fallback_paths",
                                "Refresh the cached collection handle after datablock mutation or "
                                "generation invalidation.");
    return nullptr;
  }
  return static_cast<blender::Collection *>(slot.pointer);
}

LN_RuntimeRef LN_RuntimeTree::MakeDatablockRef(blender::ID *id, const std::string &debug_name)
{
  if (id == nullptr) {
    return MakeInvalidRef(LN_RuntimeRefKind::Datablock, debug_name);
  }

  for (uint32_t index = 0; index < m_runtimeRefs.size(); index++) {
    RuntimeRefSlot &slot = m_runtimeRefs[index];
    if (slot.kind == LN_RuntimeRefKind::Datablock && slot.pointer == id) {
      LN_RuntimeRef runtime_ref;
      runtime_ref.kind = LN_RuntimeRefKind::Datablock;
      runtime_ref.slot = index;
      runtime_ref.generation = slot.generation;
      runtime_ref.debug_name = slot.debug_name;
      return runtime_ref;
    }
  }

  for (uint32_t index = 0; index < m_runtimeRefs.size(); index++) {
    RuntimeRefSlot &slot = m_runtimeRefs[index];
    if (slot.pointer == nullptr) {
      slot.pointer = id;
      slot.kind = LN_RuntimeRefKind::Datablock;
      slot.generation++;
      slot.debug_name = debug_name;
      LN_RuntimeRef runtime_ref;
      runtime_ref.kind = LN_RuntimeRefKind::Datablock;
      runtime_ref.slot = index;
      runtime_ref.generation = slot.generation;
      runtime_ref.debug_name = slot.debug_name;
      return runtime_ref;
    }
  }

  RuntimeRefSlot slot;
  slot.pointer = id;
  slot.kind = LN_RuntimeRefKind::Datablock;
  slot.debug_name = debug_name;
  m_runtimeRefs.push_back(slot);

  LN_RuntimeRef runtime_ref;
  runtime_ref.kind = LN_RuntimeRefKind::Datablock;
  runtime_ref.slot = uint32_t(m_runtimeRefs.size() - 1);
  runtime_ref.generation = slot.generation;
  runtime_ref.debug_name = slot.debug_name;
  return runtime_ref;
}

blender::ID *LN_RuntimeTree::ResolveDatablockRef(const LN_RuntimeRef &runtime_ref) const
{
  if (runtime_ref.kind != LN_RuntimeRefKind::Datablock || runtime_ref.slot >= m_runtimeRefs.size())
  {
    if (runtime_ref.kind == LN_RuntimeRefKind::Datablock && runtime_ref.IsValid()) {
      RecordRuntimeSystemFallback(
          LN_RuntimeFallbackReason::StaleHandle,
          runtime_ref.debug_name,
          "fallback_paths",
          "Refresh the cached datablock handle after datablock mutation or generation "
          "invalidation.");
    }
    return nullptr;
  }
  const RuntimeRefSlot &slot = m_runtimeRefs[runtime_ref.slot];
  if (slot.kind != LN_RuntimeRefKind::Datablock || slot.generation != runtime_ref.generation) {
    RecordRuntimeSystemFallback(LN_RuntimeFallbackReason::StaleHandle,
                                runtime_ref.debug_name.empty() ? slot.debug_name :
                                                                 runtime_ref.debug_name,
                                "fallback_paths",
                                "Refresh the cached datablock handle after datablock mutation or "
                                "generation invalidation.");
    return nullptr;
  }
  return static_cast<blender::ID *>(slot.pointer);
}

void LN_RuntimeTree::InvalidateRuntimeRef(const LN_RuntimeRefKind kind, const void *pointer)
{
  if (pointer == nullptr || kind == LN_RuntimeRefKind::Object) {
    return;
  }

  for (RuntimeRefSlot &slot : m_runtimeRefs) {
    if (slot.kind == kind && slot.pointer == pointer) {
      slot.pointer = nullptr;
      slot.generation++;
    }
  }
}

LN_PhysicsControllerHandle LN_RuntimeTree::MakePhysicsControllerHandle(
    const LN_RuntimeRef &object_ref) const
{
  LN_PhysicsControllerHandle handle;
  KX_GameObject *gameobj = ResolveObjectRef(object_ref);
  if (gameobj == nullptr || gameobj->GetPhysicsController() == nullptr) {
    return handle;
  }
  handle.object_ref = object_ref;
  handle.controller_generation = object_ref.generation;
  return handle;
}

PHY_IPhysicsController *LN_RuntimeTree::ResolvePhysicsControllerHandle(
    const LN_PhysicsControllerHandle &handle) const
{
  if (!handle.IsValid() || handle.controller_generation != handle.object_ref.generation) {
    return nullptr;
  }
  KX_GameObject *gameobj = ResolveObjectRef(handle.object_ref);
  return gameobj ? gameobj->GetPhysicsController() : nullptr;
}

PHY_ICharacter *LN_RuntimeTree::ResolveCharacterController(const LN_RuntimeRef &object_ref) const
{
  KX_GameObject *gameobj = ResolveObjectRef(object_ref);
  KX_Scene *scene = gameobj ? gameobj->GetScene() : nullptr;
  PHY_IPhysicsEnvironment *environment = scene ? scene->GetPhysicsEnvironment() : nullptr;
  return environment ? environment->GetCharacterController(gameobj) : nullptr;
}

PHY_IVehicle *LN_RuntimeTree::ResolveVehicleController(const LN_RuntimeRef &object_ref) const
{
  KX_GameObject *gameobj = ResolveObjectRef(object_ref);
  KX_Scene *scene = gameobj ? gameobj->GetScene() : nullptr;
  PHY_IPhysicsEnvironment *environment = scene ? scene->GetPhysicsEnvironment() : nullptr;
  return (environment && gameobj) ?
             environment->GetVehicleConstraint(gameobj->GetPhysicsController()) :
             nullptr;
}

LN_PhysicsQueryResult LN_RuntimeTree::Raycast(
    const MT_Vector3 &from,
    const MT_Vector3 &to,
    const LN_RuntimeRef &ignore_object_ref,
    const uint32_t collision_mask,
    const std::string &property_name,
    const bool xray,
    const std::vector<LN_RuntimeRef> &extra_ignore_object_refs,
    const LN_InteractionDomain interaction_domain,
    const bool respect_runtime_visibility,
    const KX_Camera *view_camera)
{
  LN_PhysicsQueryResult result;
  const MT_Vector3 segment = to - from;
  if (segment.length2() <= MT_EPSILON * MT_EPSILON) {
    result.diagnostic_status = LN_QueryDiagnosticStatus::InvalidTarget;
    return result;
  }
  result.ray_origin = from;
  result.ray_direction = segment.safe_normalized();
  result.end_position = to;

  KX_Scene *scene = m_gameObject ? m_gameObject->GetScene() : nullptr;
  PHY_IPhysicsEnvironment *environment = scene ? scene->GetPhysicsEnvironment() : nullptr;
  if (environment == nullptr) {
    result.diagnostic_status = LN_QueryDiagnosticStatus::MissingPhysicsWorld;
    return result;
  }

  KX_GameObject *ignore_object = ResolveObjectRef(ignore_object_ref);
  PHY_IPhysicsController *ignore_controller = RaycastOwningPhysicsController(ignore_object);
  const LN_PhysicsControllerIgnoreSet extra_ignore_controllers(*this, extra_ignore_object_refs);
  LN_RaycastCallback callback(*this,
                              ignore_controller,
                              extra_ignore_controllers,
                              from,
                              to,
                              collision_mask,
                              property_name,
                              xray,
                              false,
                              scene,
                              interaction_domain,
                              respect_runtime_visibility,
                              view_camera);
  if (!KX_RayCast::RayTest(environment, from, to, callback)) {
    return result;
  }
  return callback.GetResult();
}

LN_PhysicsQueryResult LN_RuntimeTree::RaycastPointer(const MT_Vector3 &from,
                                                     const MT_Vector3 &to,
                                                     KX_Camera *camera,
                                                     const LN_RuntimeRef &ignore_object_ref,
                                                     const uint32_t collision_mask,
                                                     const std::string &property_name,
                                                     const bool xray,
                                                     const LN_InteractionDomain interaction_domain,
                                                     const uint64_t command_execution_key)
{
  KX_GameObject *ignored_object = ResolveObjectRef(ignore_object_ref);
  if (m_logicManager != nullptr) {
    LN_Manager::PointerHitCacheKey key;
    key.camera = camera;
    key.ignored_object = ignored_object;
    key.from = from;
    key.to = to;
    key.property_name = property_name;
    key.command_execution_key = command_execution_key;
    key.collision_mask = collision_mask;
    key.domain = interaction_domain;
    key.xray = xray;
    key.respect_runtime_visibility = true;

    if (const auto cached = m_logicManager->m_pointerHitCache.find(key);
        cached != m_logicManager->m_pointerHitCache.end())
    {
      LN_PhysicsQueryResult result = cached->second.result;
      if (result.hit && cached->second.hit_object != nullptr) {
        result.object_ref = MakeObjectRef(cached->second.hit_object,
                                          cached->second.hit_object->GetName());
      }
      return result;
    }

    LN_PhysicsQueryResult result = Raycast(from,
                                           to,
                                           ignore_object_ref,
                                           collision_mask,
                                           property_name,
                                           xray,
                                           {},
                                           interaction_domain,
                                           true,
                                           camera);
    LN_Manager::PointerHitCacheValue value;
    value.result = result;
    value.hit_object = result.hit ? ResolveObjectRef(result.object_ref) : nullptr;
    value.result.object_ref = LN_RuntimeRef();
    m_logicManager->m_pointerHitCache.emplace(std::move(key), std::move(value));
    return result;
  }

  return Raycast(from,
                 to,
                 ignore_object_ref,
                 collision_mask,
                 property_name,
                 xray,
                 {},
                 interaction_domain,
                 true,
                 camera);
}

LN_PhysicsQueryResult LN_RuntimeTree::RaycastMouseOverTarget(
    const MT_Vector3 &from,
    const MT_Vector3 &to,
    KX_GameObject *target_object,
    KX_Camera *camera,
    const LN_RuntimeRef &ignore_object_ref,
    const LN_InteractionDomain interaction_domain,
    const uint64_t command_execution_key)
{
  LN_PhysicsQueryResult result;
  const MT_Vector3 segment = to - from;
  if (segment.length2() <= MT_EPSILON * MT_EPSILON || target_object == nullptr) {
    result.diagnostic_status = LN_QueryDiagnosticStatus::InvalidTarget;
    return result;
  }
  result.ray_direction = segment.safe_normalized();

  result = RaycastPointer(from,
                          to,
                          camera,
                          ignore_object_ref,
                          0,
                          std::string(),
                          false,
                          interaction_domain,
                          command_execution_key);
  KX_GameObject *hit_object = result.hit ? ResolveObjectRef(result.object_ref) : nullptr;
  if (MouseOverHitMatchesTarget(hit_object, target_object)) {
    return result;
  }

  const bool was_blocked = result.hit;
  result.hit = false;
  result.blocked = was_blocked;
  result.diagnostic_status = LN_QueryDiagnosticStatus::NoHit;
  result.object_ref = LN_RuntimeRef();
  result.hit_position = MT_Vector3(0.0f, 0.0f, 0.0f);
  result.hit_normal = MT_Vector3(0.0f, 0.0f, 0.0f);
  return result;
}

std::vector<LN_PhysicsQueryResult> LN_RuntimeTree::RaycastAll(
    const MT_Vector3 &from,
    const MT_Vector3 &to,
    const LN_RuntimeRef &ignore_object_ref,
    const uint32_t collision_mask,
    const std::string &property_name,
    const bool xray,
    const std::vector<LN_RuntimeRef> &extra_ignore_object_refs)
{
  const MT_Vector3 segment = to - from;
  if (segment.length2() <= MT_EPSILON * MT_EPSILON) {
    return {};
  }

  KX_Scene *scene = m_gameObject ? m_gameObject->GetScene() : nullptr;
  PHY_IPhysicsEnvironment *environment = scene ? scene->GetPhysicsEnvironment() : nullptr;
  if (environment == nullptr) {
    return {};
  }

  KX_GameObject *ignore_object = ResolveObjectRef(ignore_object_ref);
  PHY_IPhysicsController *ignore_controller = RaycastOwningPhysicsController(ignore_object);
  const LN_PhysicsControllerIgnoreSet extra_ignore_controllers(*this, extra_ignore_object_refs);
  LN_RaycastCallback callback(*this,
                              ignore_controller,
                              extra_ignore_controllers,
                              from,
                              to,
                              collision_mask,
                              property_name,
                              xray,
                              true);
  KX_RayCast::RayTest(environment, from, to, callback);
  return callback.GetResults();
}

std::vector<LN_PhysicsQueryResult> LN_RuntimeTree::RayCast(
    const PHY_RayQuerySettings &settings,
    const LN_RuntimeRef &ignore_object_ref,
    const uint32_t collision_mask,
    const std::string &property_name,
    const bool xray,
    const std::vector<LN_RuntimeRef> &extra_ignore_object_refs,
    bool &r_blocked,
    LN_PhysicsQueryResult &r_blocker,
    bool &r_supported)
{
  r_blocked = false;
  r_blocker = LN_PhysicsQueryResult();
  r_supported = false;
  KX_Scene *scene = m_gameObject ? m_gameObject->GetScene() : nullptr;
  PHY_IPhysicsEnvironment *environment = scene ? scene->GetPhysicsEnvironment() : nullptr;
  if (!environment) {
    return {};
  }

  KX_GameObject *ignore_object = ResolveObjectRef(ignore_object_ref);
  const LN_PhysicsControllerIgnoreSet extra_ignore_controllers(*this, extra_ignore_object_refs);
  LN_RayQueryFilterCallback filter(RaycastOwningPhysicsController(ignore_object),
                                   extra_ignore_controllers,
                                   collision_mask,
                                   property_name,
                                   xray);
  std::vector<PHY_RayCastResult> physics_results;
  r_supported = environment->RayCast(settings, filter, physics_results);
  if (!r_supported) {
    return {};
  }

  const MT_Vector3 displacement = settings.destination - settings.origin;
  const float cast_length = displacement.length();
  if (!std::isfinite(cast_length) || cast_length <= MT_EPSILON) {
    return {};
  }
  const MT_Vector3 direction = displacement / cast_length;
  std::vector<LN_PhysicsQueryResult> results;
  results.reserve(physics_results.size());
  for (const PHY_RayCastResult &physics_result : physics_results) {
    KX_ClientObjectInfo *info = physics_result.m_controller ?
                                    static_cast<KX_ClientObjectInfo *>(
                                        physics_result.m_controller->GetNewClientInfo()) :
                                    nullptr;
    KX_GameObject *hit_object = info ? info->m_gameobject : nullptr;
    if (!hit_object) {
      continue;
    }
    if (!std::isfinite(physics_result.m_fraction) || !vector_is_finite(physics_result.m_hitPoint))
    {
      continue;
    }

    LN_PhysicsQueryResult result;
    result.hit = true;
    result.diagnostic_status = LN_QueryDiagnosticStatus::Hit;
    result.object_ref = MakeObjectRef(hit_object, hit_object->GetName());
    result.hit_position = physics_result.m_hitPoint;
    result.hit_normal = vector_is_finite(physics_result.m_hitNormal) ?
                            physics_result.m_hitNormal :
                            MT_Vector3(0.0f, 0.0f, 0.0f);
    result.ray_origin = settings.origin;
    result.ray_direction = direction;
    result.end_position = settings.destination;
    result.hit_fraction = std::clamp(physics_result.m_fraction, 0.0f, 1.0f);
    result.hit_distance = cast_length * result.hit_fraction;
    result.polygon_index = physics_result.m_polygon;
    result.has_uv = physics_result.m_hitUVOK != 0 && std::isfinite(physics_result.m_hitUV.x()) &&
                    std::isfinite(physics_result.m_hitUV.y());
    result.hit_uv = result.has_uv ? physics_result.m_hitUV : MT_Vector2(0.0f, 0.0f);

    const bool property_matches = property_name.empty() || hit_object->GetProperty(property_name);
    if (!property_matches) {
      if (!xray) {
        result.hit = false;
        result.diagnostic_status = LN_QueryDiagnosticStatus::NoHit;
        result.object_ref = LN_RuntimeRef();
        result.hit_normal = MT_Vector3(0.0f, 0.0f, 0.0f);
        result.hit_uv = MT_Vector2(0.0f, 0.0f);
        result.polygon_index = -1;
        result.has_uv = false;
        result.blocked = true;
        r_blocker = result;
        r_blocked = true;
        break;
      }
      continue;
    }
    results.push_back(result);
  }
  return results;
}

std::vector<LN_PhysicsQueryResult> LN_RuntimeTree::ShapeCast(
    const PHY_ShapeCastSettings &settings,
    const LN_RuntimeRef &ignore_object_ref,
    const uint32_t collision_mask,
    const std::string &property_name,
    const bool xray,
    const std::vector<LN_RuntimeRef> &extra_ignore_object_refs,
    bool &r_blocked,
    LN_PhysicsQueryResult &r_blocker,
    bool &r_supported)
{
  r_blocked = false;
  r_blocker = LN_PhysicsQueryResult();
  r_supported = false;
  KX_Scene *scene = m_gameObject ? m_gameObject->GetScene() : nullptr;
  PHY_IPhysicsEnvironment *environment = scene ? scene->GetPhysicsEnvironment() : nullptr;
  if (!environment) {
    return {};
  }

  KX_GameObject *ignore_object = ResolveObjectRef(ignore_object_ref);
  const LN_PhysicsControllerIgnoreSet extra_ignore_controllers(*this, extra_ignore_object_refs);
  LN_ShapeCastFilterCallback filter(RaycastOwningPhysicsController(ignore_object),
                                    extra_ignore_controllers,
                                    collision_mask,
                                    property_name,
                                    xray);
  std::vector<PHY_ShapeCastResult> physics_results;
  r_supported = environment->ShapeCast(settings, filter, physics_results);
  if (!r_supported) {
    return {};
  }

  const MT_Vector3 displacement = settings.destination - settings.origin;
  const float cast_length = displacement.length();
  if (!std::isfinite(cast_length)) {
    return {};
  }
  const MT_Vector3 direction = cast_length > MT_EPSILON ? displacement / cast_length :
                                                          MT_Vector3(0.0f, 0.0f, 0.0f);
  std::vector<LN_PhysicsQueryResult> results;
  results.reserve(physics_results.size());
  for (const PHY_ShapeCastResult &physics_result : physics_results) {
    KX_ClientObjectInfo *info = physics_result.controller ?
                                    static_cast<KX_ClientObjectInfo *>(
                                        physics_result.controller->GetNewClientInfo()) :
                                    nullptr;
    KX_GameObject *hit_object = info ? info->m_gameobject : nullptr;
    if (!hit_object) {
      continue;
    }
    if (!std::isfinite(physics_result.fraction) || !vector_is_finite(physics_result.point) ||
        !vector_is_finite(physics_result.cast_position))
    {
      continue;
    }

    LN_PhysicsQueryResult result;
    result.hit = true;
    result.diagnostic_status = LN_QueryDiagnosticStatus::Hit;
    result.object_ref = MakeObjectRef(hit_object, hit_object->GetName());
    result.hit_position = physics_result.point;
    result.hit_normal = vector_is_finite(physics_result.normal) ? physics_result.normal :
                                                                  MT_Vector3(0.0f, 0.0f, 0.0f);
    result.cast_position = physics_result.cast_position;
    result.ray_direction = direction;
    result.hit_fraction = std::clamp(physics_result.fraction, 0.0f, 1.0f);
    result.hit_distance = cast_length * result.hit_fraction;
    result.penetration_depth = std::isfinite(physics_result.penetration_depth) ?
                                   std::max(physics_result.penetration_depth, 0.0f) :
                                   0.0f;
    result.polygon_index = physics_result.polygon_index;
    result.has_uv = physics_result.has_uv && std::isfinite(physics_result.hit_uv.x()) &&
                    std::isfinite(physics_result.hit_uv.y());
    result.hit_uv = result.has_uv ? physics_result.hit_uv : MT_Vector2(0.0f, 0.0f);
    result.started_overlapping = physics_result.started_overlapping;

    const bool property_matches = property_name.empty() || hit_object->GetProperty(property_name);
    if (!property_matches) {
      if (!xray) {
        result.hit = false;
        result.diagnostic_status = LN_QueryDiagnosticStatus::NoHit;
        result.object_ref = LN_RuntimeRef();
        result.hit_uv = MT_Vector2(0.0f, 0.0f);
        result.polygon_index = -1;
        result.has_uv = false;
        result.blocked = true;
        r_blocker = result;
        r_blocked = true;
        break;
      }
      continue;
    }
    results.push_back(result);
  }
  return results;
}

const LN_Value *LN_RuntimeTree::GetTreePropertyValue(const uint32_t property_ref_index) const
{
  if (m_activeTreePropertySnapshot != nullptr &&
      property_ref_index < m_activeTreePropertySnapshot->size())
  {
    return &(*m_activeTreePropertySnapshot)[property_ref_index];
  }
  if (property_ref_index >= m_treeProperties.size()) {
    return nullptr;
  }
  return &m_treeProperties[property_ref_index];
}

const std::vector<LN_Value> *LN_RuntimeTree::DelayTreePropertySnapshotForGuard(
    const uint32_t bool_expr_index, const uint64_t tick_index) const
{
  if (bool_expr_index == LN_INVALID_INDEX || tick_index == UINT64_MAX || m_program == nullptr) {
    return nullptr;
  }

  const std::vector<LN_BoolExpression> &expressions = m_program->GetBoolExpressions();
  if (bool_expr_index >= expressions.size()) {
    return nullptr;
  }

  const LN_BoolExpression &expression = expressions[bool_expr_index];
  if (expression.kind != LN_BoolExpressionKind::DelayDone || expression.int_value < 0) {
    return nullptr;
  }

  const TimeFlowRuntimeState *state = FindTimeFlowState(uint32_t(expression.int_value));
  if (state == nullptr || state->pulse_tick != tick_index || !state->has_active_tree_properties) {
    return nullptr;
  }
  return &state->active_tree_properties;
}

bool LN_RuntimeTree::SetTreePropertyValue(const uint32_t property_ref_index, const LN_Value &value)
{
  if (property_ref_index >= m_treeProperties.size()) {
    return false;
  }
  m_treeProperties[property_ref_index] = value;
  return true;
}

void LN_RuntimeTree::CompletePersistenceOperation(const uint32_t expression_index,
                                                  const bool success)
{
  if (m_program == nullptr || expression_index >= m_program->GetBoolExpressions().size()) {
    return;
  }
  if (m_program->GetBoolExpressions()[expression_index].kind !=
      LN_BoolExpressionKind::PersistenceCompleted)
  {
    return;
  }
  BoolExpressionRuntimeState &state = BoolExpressionState(expression_index);
  constexpr uint8_t max_pending_results = 64;
  if (state.pending_persistence_count < max_pending_results) {
    if (success) {
      state.pending_persistence_results |= uint64_t(1) << state.pending_persistence_count;
    }
    state.pending_persistence_count++;
  }
  else {
    /* An unobserved output cannot consume pulses. Keep the queue bounded while retaining the
     * latest result for when that output is observed again. */
    const uint64_t newest_bit = uint64_t(1) << (max_pending_results - 1);
    state.pending_persistence_results = success ? state.pending_persistence_results | newest_bit :
                                                  state.pending_persistence_results & ~newest_bit;
  }
}

bool LN_RuntimeTree::CanCompleteUIOperation(const uint32_t expression_index) const
{
  return m_program != nullptr && expression_index < m_program->GetBoolExpressions().size() &&
         m_program->GetBoolExpressions()[expression_index].kind ==
             LN_BoolExpressionKind::UICommandCompleted;
}

bool LN_RuntimeTree::ReadsUIOperation(const uint32_t expression_index) const
{
  /* The compiler marks a completion as observed only when one of its outputs is wired. */
  return CanCompleteUIOperation(expression_index) &&
         m_program->GetBoolExpressions()[expression_index].bool_value;
}

bool LN_RuntimeTree::CompleteUIOperation(const uint32_t expression_index,
                                         const LN_UIResult result,
                                         const int32_t handle)
{
  if (!CanCompleteUIOperation(expression_index)) {
    return false;
  }
  /* An unobserved result is never read, so queueing it would only grow without bound. */
  if (!ReadsUIOperation(expression_index)) {
    return true;
  }
  UICommandCompletionState &state = m_uiCommandCompletions[expression_index];
  /* Results are consumed one per tick. Keep the newest ones if a graph issues commands faster
   * than it reads them, instead of refusing the command itself. */
  constexpr size_t max_pending_results = 64;
  if (state.pending.size() >= max_pending_results) {
    state.pending.pop_front();
  }
  state.pending.push_back(UICommandCompletion{result, handle});
  return true;
}

bool LN_RuntimeTree::CompleteActionOperation(const uint32_t expression_index,
                                             const LN_ActionResult result)
{
  if (m_program == nullptr || expression_index >= m_program->GetBoolExpressions().size() ||
      m_program->GetBoolExpressions()[expression_index].kind !=
          LN_BoolExpressionKind::ActionLifecycleEvent)
  {
    return false;
  }
  ActionCompletionState &state = m_actionCompletions[expression_index];
  constexpr size_t max_pending_results = 64;
  if (state.pending.size() >= max_pending_results) {
    return false;
  }
  state.pending.push_back(result);
  return true;
}

KX_GameObject *LN_RuntimeTree::FindSceneObjectByName(const std::string &name,
                                                     bool include_inactive) const
{
  if (name.empty()) {
    return nullptr;
  }

  KX_Scene *scene = m_gameObject ? m_gameObject->GetScene() : nullptr;
  if (scene == nullptr) {
    return nullptr;
  }

  auto find_inactive = [&]() -> KX_GameObject * {
    if (scene->GetInactiveList() == nullptr) {
      return nullptr;
    }
    for (KX_GameObject *game_object : *scene->GetInactiveList()) {
      if (game_object_matches_name(game_object, name)) {
        return game_object;
      }
    }
    return nullptr;
  };

  if (include_inactive) {
    if (KX_GameObject *object = find_inactive()) {
      return object;
    }
  }

  /* In a player's copy of a menu, the menu's names mean that copy's objects. */
  if (LN_Manager *manager = scene->GetLogicNodeManager()) {
    if (KX_GameObject *object = manager->FindObjectByName(name)) {
      return scene->OverlayCounterpart(object, m_gameObject);
    }
  }

  if (scene->GetObjectList() == nullptr) {
    return nullptr;
  }

  for (KX_GameObject *game_object : *scene->GetObjectList()) {
    if (game_object_matches_name(game_object, name)) {
      return scene->OverlayCounterpart(game_object, m_gameObject);
    }
  }

  if (include_inactive) {
    return find_inactive();
  }
  return nullptr;
}

KX_Camera *LN_RuntimeTree::GetActiveCamera() const
{
  KX_Scene *scene = m_gameObject ? m_gameObject->GetScene() : nullptr;
  return scene ? scene->GetActiveCamera() : nullptr;
}

KX_GameObject *LN_RuntimeTree::ResolveLiveObjectValue(const LN_Value &value,
                                                      const bool include_inactive) const
{
  if (value.type == LN_ValueType::ObjectRef && value.runtime_ref.IsValid()) {
    return ResolveObjectRef(value.runtime_ref);
  }
  return ResolveObjectValue(value, include_inactive);
}

KX_GameObject *LN_RuntimeTree::ResolveObjectValue(const LN_Value &value,
                                                  bool include_inactive) const
{
  if (value.type != LN_ValueType::ObjectRef) {
    return nullptr;
  }

  if (value.runtime_ref.IsValid()) {
    if (KX_GameObject *game_object = ResolveObjectRef(value.runtime_ref)) {
      return game_object;
    }
  }

  if (!value.reference_name.empty()) {
    return FindSceneObjectByName(value.reference_name, include_inactive);
  }
  return nullptr;
}

void LN_RuntimeTree::ShareValue(LN_Value &value)
{
  switch (value.type) {
    case LN_ValueType::ObjectRef:
      if (value.runtime_ref.shared_domain == 0 && m_logicManager != nullptr) {
        KX_GameObject *object = ResolveObjectValue(value);
        value.runtime_ref = object ? m_logicManager->MakeSharedObjectRef(object,
                                                                         value.reference_name) :
                                     LN_RuntimeRef();
      }
      break;
    case LN_ValueType::SceneRef:
    case LN_ValueType::CollectionRef:
    case LN_ValueType::DatablockRef:
      /* Scene and datablock names are unique, so readers resolve these by name. */
      value.runtime_ref = LN_RuntimeRef();
      break;
    case LN_ValueType::List:
      for (LN_Value &item : value.list_value) {
        ShareValue(item);
      }
      break;
    case LN_ValueType::Dict:
      for (auto &item : value.dict_value) {
        ShareValue(item.second);
      }
      break;
    default:
      break;
  }
}

void LN_RuntimeTree::LocalizeValue(LN_Value &value)
{
  switch (value.type) {
    case LN_ValueType::ObjectRef:
      if (value.runtime_ref.shared_domain != 0) {
        KX_GameObject *object = ResolveObjectRef(value.runtime_ref);
        value.runtime_ref = object ? MakeObjectRef(object, value.reference_name) :
                                     LN_RuntimeRef();
      }
      break;
    case LN_ValueType::List:
      for (LN_Value &item : value.list_value) {
        LocalizeValue(item);
      }
      break;
    case LN_ValueType::Dict:
      for (auto &item : value.dict_value) {
        LocalizeValue(item.second);
      }
      break;
    default:
      break;
  }
}

std::vector<LN_RuntimeRef> LN_RuntimeTree::ResolveIgnoreObjectRefs(
    const uint32_t object_expr_index,
    const uint32_t list_expr_index,
    const LN_TickContext &context)
{
  std::vector<LN_RuntimeRef> refs;
  std::unordered_set<KX_GameObject *> seen;
  auto append_object = [&](KX_GameObject *object) {
    if (!object || !seen.insert(object).second) {
      return;
    }
    refs.push_back(MakeObjectRef(object, object->GetName()));
  };

  if (object_expr_index != LN_INVALID_INDEX) {
    append_object(
        ResolveObjectValue(ResolveValueExpression(object_expr_index, MakeNoneValue(), context)));
  }
  if (list_expr_index != LN_INVALID_INDEX) {
    const LN_Value list = ResolveValueExpression(list_expr_index, MakeNoneValue(), context);
    if (list.exists && list.type == LN_ValueType::List) {
      refs.reserve(refs.size() + list.list_value.size());
      for (const LN_Value &item : list.list_value) {
        append_object(ResolveObjectValue(item));
      }
    }
  }
  return refs;
}

PHY_IPhysicsController *LN_RuntimeTree::ResolveRigidBodyAttributeController(
    const uint32_t object_expr_index, const LN_TickContext &context)
{
  KX_GameObject *target_object = m_gameObject;
  if (object_expr_index != LN_INVALID_INDEX) {
    target_object = ResolveObjectValue(
        ResolveValueExpression(object_expr_index, MakeNoneValue(), context));
  }
  if (target_object == nullptr) {
    return nullptr;
  }
  PHY_IPhysicsController *controller = target_object->GetPhysicsController();
  if (controller == nullptr || !controller->IsDynamic()) {
    return nullptr;
  }
  return controller;
}

bool LN_RuntimeTree::ResolveRigidBodyConstraintBreakEventQuery(
    const uint32_t object_expr_index,
    const uint32_t constraint_name_expr_index,
    const uint32_t match_any_expr_index,
    const LN_RigidBodyConstraintMatchMode match_mode,
    const LN_TickContext &context,
    KX_GameObject *&r_owner,
    std::string &r_constraint_name,
    LN_RigidBodyConstraintMatchMode &r_match_mode)
{
  r_owner = nullptr;
  r_constraint_name.clear();
  r_match_mode = ValidRigidBodyConstraintMatchMode(match_mode);
  if (m_logicManager == nullptr) {
    return false;
  }

  r_owner = m_gameObject;
  if (object_expr_index != LN_INVALID_INDEX) {
    r_owner = ResolveObjectValue(
        ResolveValueExpression(object_expr_index, MakeNoneValue(), context));
  }
  if (r_owner == nullptr) {
    return false;
  }

  const bool match_any = match_any_expr_index != LN_INVALID_INDEX &&
                         ResolveBoolExpression(match_any_expr_index, false, context);
  if (match_any) {
    r_match_mode = LN_RigidBodyConstraintMatchMode::All;
    return true;
  }

  r_constraint_name = ResolveStringExpression(constraint_name_expr_index, std::string(), context);
  return !r_constraint_name.empty();
}

const LN_RigidBodyConstraintBreakEvent *LN_RuntimeTree::ResolveRigidBodyConstraintBreakEvent(
    const uint32_t object_expr_index,
    const uint32_t constraint_name_expr_index,
    const uint32_t match_any_expr_index,
    const LN_RigidBodyConstraintMatchMode match_mode,
    const LN_TickContext &context)
{
  KX_GameObject *owner = nullptr;
  std::string constraint_name;
  LN_RigidBodyConstraintMatchMode resolved_match_mode = LN_RigidBodyConstraintMatchMode::Exact;
  if (!ResolveRigidBodyConstraintBreakEventQuery(object_expr_index,
                                                 constraint_name_expr_index,
                                                 match_any_expr_index,
                                                 match_mode,
                                                 context,
                                                 owner,
                                                 constraint_name,
                                                 resolved_match_mode))
  {
    return nullptr;
  }
  return m_logicManager->FindRigidBodyConstraintBreakEvent(
      owner, constraint_name, resolved_match_mode);
}

int32_t LN_RuntimeTree::ResolveRigidBodyConstraintBreakEventCount(
    const uint32_t object_expr_index,
    const uint32_t constraint_name_expr_index,
    const uint32_t match_any_expr_index,
    const LN_RigidBodyConstraintMatchMode match_mode,
    const LN_TickContext &context)
{
  KX_GameObject *owner = nullptr;
  std::string constraint_name;
  LN_RigidBodyConstraintMatchMode resolved_match_mode = LN_RigidBodyConstraintMatchMode::Exact;
  if (!ResolveRigidBodyConstraintBreakEventQuery(object_expr_index,
                                                 constraint_name_expr_index,
                                                 match_any_expr_index,
                                                 match_mode,
                                                 context,
                                                 owner,
                                                 constraint_name,
                                                 resolved_match_mode))
  {
    return 0;
  }

  if (resolved_match_mode == LN_RigidBodyConstraintMatchMode::All) {
    return int32_t(std::min<uint32_t>(m_logicManager->RigidBodyConstraintBreakEventCount(owner),
                                      uint32_t(std::numeric_limits<int32_t>::max())));
  }

  uint32_t count = 0;
  for (uint32_t event_index = m_logicManager->FirstRigidBodyConstraintBreakEventIndex(owner);
       event_index != LN_INVALID_INDEX;
       event_index = m_logicManager->NextRigidBodyConstraintBreakEventIndex(event_index))
  {
    const LN_RigidBodyConstraintBreakEvent *event =
        m_logicManager->RigidBodyConstraintBreakEventAt(event_index);
    if (event != nullptr && RigidBodyConstraintNameMatches(
                                event->constraint_name, constraint_name, resolved_match_mode))
    {
      if (count == uint32_t(std::numeric_limits<int32_t>::max())) {
        return std::numeric_limits<int32_t>::max();
      }
      count++;
    }
  }
  return int32_t(count);
}

LN_Value LN_RuntimeTree::ResolveRigidBodyConstraintBreakEvents(
    const uint32_t object_expr_index,
    const uint32_t constraint_name_expr_index,
    const uint32_t match_any_expr_index,
    const LN_RigidBodyConstraintMatchMode match_mode,
    const LN_TickContext &context)
{
  LN_Value events;
  events.type = LN_ValueType::List;
  events.exists = true;

  KX_GameObject *owner = nullptr;
  std::string constraint_name;
  LN_RigidBodyConstraintMatchMode resolved_match_mode = LN_RigidBodyConstraintMatchMode::Exact;
  if (!ResolveRigidBodyConstraintBreakEventQuery(object_expr_index,
                                                 constraint_name_expr_index,
                                                 match_any_expr_index,
                                                 match_mode,
                                                 context,
                                                 owner,
                                                 constraint_name,
                                                 resolved_match_mode))
  {
    return events;
  }

  /* Exact matching is normally unique. Do not reserve every event on this
   * owner for an exact-name query: a large simultaneous break batch with a
   * single name match should not allocate a large, mostly unused list. */
  if (resolved_match_mode == LN_RigidBodyConstraintMatchMode::All) {
    events.list_value.reserve(m_logicManager->RigidBodyConstraintBreakEventCount(owner));
  }
  else if (resolved_match_mode == LN_RigidBodyConstraintMatchMode::Exact &&
           m_logicManager->FindRigidBodyConstraintBreakEvent(
               owner, constraint_name, resolved_match_mode) != nullptr)
  {
    events.list_value.reserve(1);
  }
  for (uint32_t event_index = m_logicManager->FirstRigidBodyConstraintBreakEventIndex(owner);
       event_index != LN_INVALID_INDEX;
       event_index = m_logicManager->NextRigidBodyConstraintBreakEventIndex(event_index))
  {
    const LN_RigidBodyConstraintBreakEvent *event =
        m_logicManager->RigidBodyConstraintBreakEventAt(event_index);
    if (event == nullptr || !RigidBodyConstraintNameMatches(
                                event->constraint_name, constraint_name, resolved_match_mode))
    {
      continue;
    }

    LN_Value record;
    record.type = LN_ValueType::Dict;
    record.exists = true;
    record.dict_value.emplace("constraint_name", MakeStringValue(event->constraint_name));
    record.dict_value.emplace(
        "target_object",
        MakeObjectValue(this, m_logicManager->ResolveObjectHandle(event->target)));
    record.dict_value.emplace("anchor_position", MakeVectorValue(event->anchor_position));
    record.dict_value.emplace("linear_impulse", MakeFloatValue(event->linear_impulse));
    record.dict_value.emplace("angular_impulse", MakeFloatValue(event->angular_impulse));
    record.dict_value.emplace("applied_impulse", MakeFloatValue(event->applied_impulse));
    events.list_value.push_back(std::move(record));
  }
  return events;
}

KX_Camera *LN_RuntimeTree::ResolveCameraValue(const LN_Value &value) const
{
  return dynamic_cast<KX_Camera *>(ResolveObjectValue(value));
}

bool LN_RuntimeTree::ProjectWorldToScreen(KX_Camera *camera,
                                          const MT_Vector3 &world_position,
                                          MT_Vector3 &r_screen_position) const
{
  return LN_ProjectWorldToScreen(camera, world_position, r_screen_position);
}

bool LN_RuntimeTree::ProjectScreenToWorld(KX_Camera *camera,
                                          const float screen_x,
                                          const float screen_y,
                                          const float depth,
                                          MT_Vector3 &r_world_position) const
{
  if (camera == nullptr) {
    return false;
  }

  const float ndc_x = screen_x * 2.0f - 1.0f;
  const float ndc_y = (1.0f - screen_y) * 2.0f - 1.0f;

  MT_Matrix4x4 inverse_projection = camera->GetProjectionMatrix();
  inverse_projection.invert();
  const MT_Matrix4x4 camera_to_world(camera->GetCameraToWorld());
  const MT_Vector4 near_clip(ndc_x, ndc_y, 0.0f, 1.0f);
  const MT_Vector4 far_clip(ndc_x, ndc_y, 1.0f, 1.0f);
  const MT_Vector4 near_camera = inverse_projection * near_clip;
  const MT_Vector4 far_camera = inverse_projection * far_clip;
  const MT_Vector4 near_world = camera_to_world * near_camera;
  const MT_Vector4 far_world = camera_to_world * far_camera;
  if (std::fabs(near_world[3]) <= MT_EPSILON || std::fabs(far_world[3]) <= MT_EPSILON) {
    return false;
  }

  const MT_Vector3 camera_position = camera->NodeGetWorldPosition();
  const MT_Vector3 near_position(
      near_world[0] / near_world[3], near_world[1] / near_world[3], near_world[2] / near_world[3]);
  const MT_Vector3 far_position(
      far_world[0] / far_world[3], far_world[1] / far_world[3], far_world[2] / far_world[3]);
  if (camera->GetCameraData()->m_perspective) {
    MT_Vector3 ray_direction = camera_position - near_position;
    if (ray_direction.length2() <= (MT_EPSILON * MT_EPSILON)) {
      return false;
    }

    ray_direction.normalize();
    r_world_position = camera_position + (ray_direction * -depth);
  }
  else {
    MT_Vector3 ray_direction = far_position - near_position;
    if (ray_direction.length2() <= (MT_EPSILON * MT_EPSILON)) {
      return false;
    }

    ray_direction.normalize();
    r_world_position = near_position + (ray_direction * depth);
  }
  return true;
}

bool LN_RuntimeTree::ProjectCanvasPixelToWorld(KX_Camera *camera,
                                               const float pixel_x,
                                               const float pixel_y,
                                               const float depth,
                                               MT_Vector3 &r_world_position) const
{
  KX_KetsjiEngine *engine = KX_GetActiveEngine();
  RAS_ICanvas *canvas = engine ? engine->GetCanvas() : nullptr;
  RAS_Rasterizer *rasterizer = engine ? engine->GetRasterizer() : nullptr;
  KX_Scene *scene = camera ? camera->GetScene() : nullptr;
  if (camera == nullptr || canvas == nullptr || rasterizer == nullptr || scene == nullptr ||
      canvas->GetWidth() <= 0 || canvas->GetHeight() <= 0 || !std::isfinite(pixel_x) ||
      !std::isfinite(pixel_y) || !std::isfinite(depth) || depth < 0.0f)
  {
    return false;
  }

  const RAS_Rect display_area = rasterizer->GetRenderArea(canvas,
                                                          RAS_Rasterizer::RAS_STEREO_LEFTEYE);
  RAS_Rect camera_area;
  RAS_Rect viewport;
  engine->GetSceneViewport(scene, camera, display_area, camera_area, viewport);
  if (viewport.GetWidth() <= 0 || viewport.GetHeight() <= 0) {
    return false;
  }

  const float pixel_y_bottom = float(canvas->GetHeight()) - pixel_y;
  const int drawn_extra = LN_CameraViewportDrawnExtra(camera);
  const float ndc_x = 2.0f * ((pixel_x - float(viewport.GetLeft())) /
                              float(viewport.GetWidth() + drawn_extra)) -
                      1.0f;
  const float ndc_y = 2.0f * ((pixel_y_bottom - float(viewport.GetBottom())) /
                              float(viewport.GetHeight() + drawn_extra)) -
                      1.0f;
  const MT_Matrix4x4 projection = engine->GetCameraProjectionMatrix(
      scene, camera, RAS_Rasterizer::RAS_STEREO_LEFTEYE, viewport, camera_area);
  MT_Matrix4x4 inverse_projection = projection;
  inverse_projection.invert();
  const MT_Vector4 near_camera = inverse_projection * MT_Vector4(ndc_x, ndc_y, -1.0f, 1.0f);
  const MT_Vector4 far_camera = inverse_projection * MT_Vector4(ndc_x, ndc_y, 1.0f, 1.0f);
  const MT_Matrix4x4 camera_to_world(camera->GetCameraToWorld());
  const MT_Vector4 near_world = camera_to_world * near_camera;
  const MT_Vector4 far_world = camera_to_world * far_camera;
  if (std::fabs(near_world[3]) <= MT_EPSILON || std::fabs(far_world[3]) <= MT_EPSILON) {
    return false;
  }

  const MT_Vector3 near_position(
      near_world[0] / near_world[3], near_world[1] / near_world[3], near_world[2] / near_world[3]);
  const MT_Vector3 far_position(
      far_world[0] / far_world[3], far_world[1] / far_world[3], far_world[2] / far_world[3]);
  const MT_Vector3 origin = camera->GetCameraData()->m_perspective ?
                                camera->NodeGetWorldPosition() :
                                near_position;
  MT_Vector3 direction = camera->GetCameraData()->m_perspective ? near_position - origin :
                                                                  far_position - near_position;
  if (direction.length2() <= MT_EPSILON * MT_EPSILON) {
    return false;
  }
  direction.normalize();
  r_world_position = origin + direction * depth;
  return std::isfinite(r_world_position.x()) && std::isfinite(r_world_position.y()) &&
         std::isfinite(r_world_position.z());
}

KX_Camera *LN_RuntimeTree::PointerWorldCamera(KX_Scene *scene,
                                              const KX_GameObject *target) const
{
  if (scene == nullptr) {
    return nullptr;
  }
  const LN_MouseSnapshot &mouse = m_snapshot.GetInputSnapshot().GetMouse();
  if (!mouse.has_position || m_logicManager == nullptr) {
    return scene->GetActiveCamera();
  }
  return m_logicManager->GetUIService().WorldPointerCameraFor(
      target, float(mouse.x), float(mouse.y));
}

bool LN_RuntimeTree::ComputeMouseRay(KX_Camera *camera,
                                     const float distance,
                                     MT_Vector3 &r_from,
                                     MT_Vector3 &r_to) const
{
  if (camera == nullptr) {
    return false;
  }

  const LN_MouseSnapshot &mouse = m_snapshot.GetInputSnapshot().GetMouse();
  if (!mouse.has_position) {
    return false;
  }
  /* A drag held in this camera's view goes on past it into another player's view. */
  return LN_ComputeCameraPointerRay(
      camera,
      float(mouse.x),
      float(mouse.y),
      distance,
      r_from,
      r_to,
      m_logicManager != nullptr && m_logicManager->GetUIService().HoldsPointerInView(camera));
}

void LN_RuntimeTree::EnsureInstructionStateSize()
{
  if (m_program == nullptr) {
    m_instructionStates.clear();
    return;
  }

  const size_t instruction_count =
      m_program->GetInstructionHeaders(LN_Event::OnInit).size() +
      m_program->GetInstructionHeaders(LN_Event::OnUpdate).size() +
      m_program->GetInstructionHeaders(LN_Event::OnFixedUpdate).size();
  m_instructionStates.assign(instruction_count, InstructionRuntimeState{});
}

void LN_RuntimeTree::EnsureBoolExpressionStateSize()
{
  if (m_program == nullptr) {
    m_boolExpressionStates.clear();
    return;
  }

  m_boolExpressionStates.assign(m_program->GetBoolExpressions().size(),
                                BoolExpressionRuntimeState{});
}

LN_RuntimeTree::BoolExpressionRuntimeState &LN_RuntimeTree::BoolExpressionState(
    const uint32_t expression_index)
{
  if (expression_index >= m_boolExpressionStates.size()) {
    m_boolExpressionStates.resize(size_t(expression_index) + 1);
  }
  return m_boolExpressionStates[expression_index];
}

void LN_RuntimeTree::EnsureFloatExpressionStateSize()
{
  if (m_program == nullptr) {
    m_floatExpressionStates.clear();
    return;
  }

  m_floatExpressionStates.assign(m_program->GetFloatExpressions().size(),
                                 FloatExpressionRuntimeState{});
}

LN_RuntimeTree::FloatExpressionRuntimeState &LN_RuntimeTree::FloatExpressionState(
    const uint32_t expression_index)
{
  if (expression_index >= m_floatExpressionStates.size()) {
    m_floatExpressionStates.resize(size_t(expression_index) + 1);
  }
  return m_floatExpressionStates[expression_index];
}

void LN_RuntimeTree::EnsureTimeFlowStateSize(const bool preserve_existing)
{
  if (m_program == nullptr) {
    m_timeFlowStates.clear();
    return;
  }

  const size_t state_count = m_program->GetTimeFlowStateCount();
  if (preserve_existing) {
    m_timeFlowStates.resize(state_count);
    return;
  }

  m_timeFlowStates.assign(state_count, TimeFlowRuntimeState{});
}

void LN_RuntimeTree::EnsureFlowControlStateSize(const bool preserve_existing)
{
  if (m_program == nullptr) {
    m_flowControlStates.clear();
    return;
  }

  const size_t state_count = m_program->GetFlowControlStateCount();
  if (preserve_existing) {
    m_flowControlStates.resize(state_count);
    return;
  }

  m_flowControlStates.assign(state_count, FlowControlRuntimeState{});
}

LN_RuntimeTree::TimeFlowRuntimeState *LN_RuntimeTree::FindTimeFlowState(const uint32_t state_index)
{
  if (state_index >= m_timeFlowStates.size()) {
    return nullptr;
  }
  return &m_timeFlowStates[state_index];
}

const LN_RuntimeTree::TimeFlowRuntimeState *LN_RuntimeTree::FindTimeFlowState(
    const uint32_t state_index) const
{
  if (state_index >= m_timeFlowStates.size()) {
    return nullptr;
  }
  return &m_timeFlowStates[state_index];
}

LN_RuntimeTree::FlowControlRuntimeState *LN_RuntimeTree::FindFlowControlState(
    const uint32_t state_index)
{
  if (state_index >= m_flowControlStates.size()) {
    return nullptr;
  }
  return &m_flowControlStates[state_index];
}

const LN_RuntimeTree::FlowControlRuntimeState *LN_RuntimeTree::FindFlowControlState(
    const uint32_t state_index) const
{
  if (state_index >= m_flowControlStates.size()) {
    return nullptr;
  }
  return &m_flowControlStates[state_index];
}

void LN_RuntimeTree::TickLatentTimeFlowStates(const LN_TickContext &context)
{
  if (m_timeFlowStates.empty() || context.tick_index == UINT64_MAX) {
    return;
  }

  for (TimeFlowRuntimeState &state : m_timeFlowStates) {
    if (state.event != context.event || (m_uiEventRoutesOnly && !state.ui_event_route)) {
      continue;
    }

    const double step = TimeFlowStep(context, state.ignore_timescale);
    switch (state.kind) {
      case TimeFlowRuntimeKind::NextFrame:
        if (state.pulse_tick != context.tick_index) {
          state.pulse_tick = UINT64_MAX;
        }
        if (state.active && state.last_update_tick != context.tick_index) {
          state.active = false;
          state.pulse_tick = context.tick_index;
        }
        break;
      case TimeFlowRuntimeKind::Timer:
        if (!state.active || step <= 0.0) {
          break;
        }

        state.remaining_time = std::max(0.0, state.remaining_time - step);
        if (state.remaining_time <= LN_TIME_FLOW_EPSILON) {
          state.active = false;
          state.pulse_tick = context.tick_index;
        }
        break;
      case TimeFlowRuntimeKind::Delay: {
        state.has_active_tree_properties = false;
        if (state.pulse_tick != context.tick_index) {
          state.pulse_tick = UINT64_MAX;
        }
        if (state.pending_pulses.empty() || step <= 0.0) {
          state.active = !state.pending_pulses.empty();
          break;
        }

        for (TimeFlowRuntimeState::PendingPulse &pulse : state.pending_pulses) {
          pulse.remaining_time = std::max(0.0, pulse.remaining_time - step);
        }

        auto due_it = std::find_if(state.pending_pulses.begin(),
                                   state.pending_pulses.end(),
                                   [](const TimeFlowRuntimeState::PendingPulse &pulse) {
                                     return pulse.remaining_time <= LN_TIME_FLOW_EPSILON;
                                   });
        if (due_it != state.pending_pulses.end()) {
          state.active_tree_properties = std::move(due_it->tree_properties);
          state.has_active_tree_properties = true;
          state.pulse_tick = context.tick_index;
          state.pending_pulses.erase(due_it);
        }

        state.active = !state.pending_pulses.empty();
        state.remaining_time = state.active ? state.pending_pulses.front().remaining_time : 0.0;
        for (const TimeFlowRuntimeState::PendingPulse &pulse : state.pending_pulses) {
          state.remaining_time = std::min(state.remaining_time, pulse.remaining_time);
        }
        break;
      }
      case TimeFlowRuntimeKind::Barrier:
        if (state.active &&
            PhaseTickWasSkipped(state.last_update_phase_tick, context.phase_tick_index))
        {
          state.active = false;
          state.accumulated_time = 0.0;
        }
        break;
      case TimeFlowRuntimeKind::Cooldown:
        if (state.pulse_tick != context.tick_index) {
          state.pulse_tick = UINT64_MAX;
        }
        if (!state.active || state.last_update_tick == context.tick_index) {
          break;
        }

        state.last_update_tick = context.tick_index;
        state.last_update_phase_tick = context.phase_tick_index;
        if (step <= 0.0) {
          break;
        }
        state.remaining_time = std::max(0.0, state.remaining_time - step);
        if (state.remaining_time <= LN_TIME_FLOW_EPSILON) {
          state.active = false;
          state.remaining_time = 0.0;
          state.pulse_tick = context.tick_index;
        }
        break;
      case TimeFlowRuntimeKind::Pulsify:
      case TimeFlowRuntimeKind::None:
        break;
    }
  }
}

void LN_RuntimeTree::EnsureQueryExpressionStateSize()
{
  if (m_program == nullptr) {
    m_queryExpressionStates.clear();
    return;
  }

  m_queryExpressionStates.assign(m_program->GetQueryRuntimeStateCount(),
                                 QueryExpressionRuntimeState{});
}

LN_RuntimeTree::QueryExpressionRuntimeState &LN_RuntimeTree::QueryExpressionState(
    const uint32_t state_index)
{
  if (state_index >= m_queryExpressionStates.size()) {
    m_queryExpressionStates.resize(size_t(state_index) + 1);
  }
  return m_queryExpressionStates[state_index];
}

void LN_RuntimeTree::EnsureTreePropertyStateSize(const bool preserve_existing)
{
  if (m_program == nullptr) {
    m_treeProperties.clear();
    return;
  }

  const std::vector<LN_TreePropertyRef> &property_refs = m_program->GetTreePropertyRefs();
  std::vector<LN_Value> previous_properties;
  if (preserve_existing) {
    previous_properties = std::move(m_treeProperties);
  }

  m_treeProperties.clear();
  m_treeProperties.reserve(property_refs.size());
  for (uint32_t index = 0; index < property_refs.size(); index++) {
    const LN_TreePropertyRef &property_ref = property_refs[index];
    if (preserve_existing && index < previous_properties.size()) {
      m_treeProperties.push_back(previous_properties[index]);
      continue;
    }

    LN_Value value = property_ref.default_value;
    value.type = property_ref.value_type;
    value.exists = true;
    m_treeProperties.push_back(value);
  }
}

bool LN_RuntimeTree::ResolvePathMoveTarget(InstructionRuntimeState &state,
                                           KX_GameObject *target,
                                           KX_NavMeshObject *navmesh,
                                           const MT_Vector3 &destination,
                                           const float reach_threshold,
                                           MT_Vector3 &r_move_target,
                                           bool &r_reached) const
{
  r_reached = false;
  if (target == nullptr) {
    return false;
  }

  const MT_Vector3 current = target->NodeGetWorldPosition();
  if ((destination - current).length2() <= reach_threshold * reach_threshold) {
    state.path_len = 0;
    state.waypoint_index = -1;
    state.next_path_point = destination;
    r_reached = true;
    return false;
  }

  if (navmesh == nullptr) {
    r_move_target = destination;
    state.next_path_point = destination;
    return true;
  }

  KX_KetsjiEngine *engine = KX_GetActiveEngine();
  const double current_time = engine ? engine->GetRealTime() : 0.0;
  if (state.path_len <= 0 || state.path_update_time < 0.0 ||
      (engine && current_time - state.path_update_time > LN_PATH_UPDATE_PERIOD))
  {
    state.path_len = navmesh->FindPath(current, destination, state.path, kMaxPathLength);
    state.waypoint_index = state.path_len > 1 ? 1 : -1;
    state.path_update_time = current_time;
  }

  if (state.waypoint_index > 0 && state.waypoint_index < state.path_len) {
    MT_Vector3 waypoint = PathWaypoint(state.path, state.waypoint_index);
    if ((waypoint - current).length2() < LN_WAYPOINT_RADIUS * LN_WAYPOINT_RADIUS) {
      state.waypoint_index++;
      if (state.waypoint_index >= state.path_len) {
        state.waypoint_index = -1;
        r_move_target = destination;
        state.next_path_point = destination;
        return true;
      }
      waypoint = PathWaypoint(state.path, state.waypoint_index);
    }
    r_move_target = waypoint;
    state.next_path_point = waypoint;
    return true;
  }

  r_move_target = destination;
  state.next_path_point = destination;
  return true;
}

void LN_RuntimeTree::DrawNavigationPath(KX_GameObject *target,
                                        const InstructionRuntimeState &state,
                                        const MT_Vector3 &destination) const
{
  if (target == nullptr) {
    return;
  }

  std::vector<MT_Vector3> points;
  points.reserve(size_t(std::max(state.path_len, 0)) + 2);
  points.push_back(target->NodeGetWorldPosition());
  if (state.path_len > 0 && state.waypoint_index >= 0 && state.waypoint_index < state.path_len) {
    for (int index = state.waypoint_index; index < state.path_len; index++) {
      points.push_back(PathWaypoint(state.path, index));
    }
  }
  else {
    points.push_back(state.next_path_point);
  }
  if ((points.back() - destination).length2() > 1.0e-8f) {
    points.push_back(destination);
  }

  const MT_Vector4 color(1.0f, 0.0f, 0.0f, 1.0f);
  for (size_t index = 1; index < points.size(); index++) {
    KX_RasterizerDrawDebugLine(points[index - 1], points[index], color);
  }
}

uint32_t LN_RuntimeTree::InstructionStateIndex(const LN_Event event,
                                               const uint32_t instruction_index) const
{
  if (m_program == nullptr) {
    return LN_INVALID_INDEX;
  }

  uint32_t offset = 0;
  if (event == LN_Event::OnUpdate) {
    offset = uint32_t(m_program->GetInstructionHeaders(LN_Event::OnInit).size());
  }
  else if (event == LN_Event::OnFixedUpdate) {
    offset = uint32_t(m_program->GetInstructionHeaders(LN_Event::OnInit).size() +
                      m_program->GetInstructionHeaders(LN_Event::OnUpdate).size());
  }
  return offset + instruction_index;
}

void LN_RuntimeTree::MarkInstructionExecuted(const LN_Event event,
                                             const uint32_t instruction_index,
                                             const uint64_t tick_index)
{
  const uint32_t state_index = InstructionStateIndex(event, instruction_index);
  if (state_index == LN_INVALID_INDEX || state_index >= m_instructionStates.size()) {
    return;
  }

  m_instructionStates[state_index].last_execution_tick = tick_index;
  m_instructionStates[state_index].last_execution_scope_serial = m_activeExecutionScopeSerial;
}

void LN_RuntimeTree::ProfileInstructionDispatch(const LN_Instruction &instruction)
{
  if (m_activeProfileCounters == nullptr) {
    return;
  }

  m_activeProfileCounters->instruction_dispatch_count++;
  const LN_RuntimeInstructionSemantics *semantics = LN_GetRuntimeInstructionSemantics(
      instruction.opcode);
  if (semantics != nullptr && semantics->fallback_requirements != LN_RUNTIME_FALLBACK_NONE) {
    m_activeProfileCounters->fallback_path_count++;
  }
}

void LN_RuntimeTree::ProfileExpressionEvaluation(const uint32_t fallback_requirements)
{
  if (m_activeProfileCounters == nullptr) {
    return;
  }

  m_activeProfileCounters->expression_evaluation_count++;
  if (fallback_requirements != LN_RUNTIME_FALLBACK_NONE) {
    m_activeProfileCounters->fallback_path_count++;
  }
}

void LN_RuntimeTree::RecordMissingSnapshotChannelsForExpression(
    const LN_RuntimeExpressionSemantics &semantics) const
{
  if ((semantics.reads & LN_RUNTIME_SEMANTIC_READ_SNAPSHOT) == 0u ||
      m_activeProfileCounters == nullptr)
  {
    return;
  }

  const LN_SnapshotCaptureStats &stats = m_snapshot.GetCaptureStats();
  const LN_SnapshotChannelMask declared_channels = stats.declared_channels !=
                                                           LN_SNAPSHOT_CHANNEL_NONE ?
                                                       stats.declared_channels :
                                                       LN_SnapshotChannelMaskForProgram(
                                                           m_program.get());
  const LN_SnapshotChannelMask missing_channels = declared_channels & ~stats.captured_channels;
  if (missing_channels == LN_SNAPSHOT_CHANNEL_NONE) {
    return;
  }

  std::ostringstream debug_name;
  debug_name << semantics.name << " missing_snapshot_channels=0x" << std::hex << missing_channels;
  RecordRuntimeSystemFallback(
      LN_RuntimeFallbackReason::MissingSnapshotChannel,
      debug_name.str(),
      "fallback_paths",
      "Declare and capture the snapshot channel required by this expression before using the "
      "optimized path.");
}

void LN_RuntimeTree::ClearRuntimeFallbackReport(const uint64_t tick_index)
{
  m_runtimeFallbackReport.tick_index = tick_index;
  m_runtimeFallbackReport.fallback_block_count = 0;
  m_runtimeFallbackReport.fallback_instruction_count = 0;
  m_runtimeFallbackReport.expression_fallback_count = 0;
  m_runtimeFallbackReport.system_fallback_count = 0;
  m_runtimeFallbackReport.reason_mask = 0;
  m_runtimeFallbackReport.expression_reason_mask = 0;
  m_runtimeFallbackReport.system_reason_mask = 0;
  m_runtimeFallbackReport.optimized_execution_partial = false;
  m_runtimeFallbackReport.hits.clear();
  m_runtimeFallbackReport.expression_hits.clear();
  m_runtimeFallbackReport.system_hits.clear();
}

void LN_RuntimeTree::RecordRuntimeFallbackInstruction(const LN_Event event,
                                                      const uint32_t instruction_index,
                                                      const LN_Instruction &instruction)
{
  if (m_activeProfileCounters == nullptr) {
    return;
  }

  const LN_Phase10OpcodeFallbackDiagnostic *diagnostic = FindOpcodeFallbackDiagnostic(
      instruction.opcode);
  if (diagnostic == nullptr) {
    static constexpr const char *fallback_profile_counter = "exec_fallback_instruction_count";
    static constexpr const char *fallback_removal_condition =
        "Implement direct exec-block support for the enclosing runtime control-flow block.";

    m_runtimeFallbackReport.optimized_execution_partial = true;
    m_runtimeFallbackReport.reason_mask |= 1u
                                           << uint8_t(LN_RuntimeFallbackReason::UnsupportedOpcode);

    for (RuntimeFallbackHit &hit : m_runtimeFallbackReport.hits) {
      if (hit.opcode == instruction.opcode &&
          hit.reason == LN_RuntimeFallbackReason::UnsupportedOpcode)
      {
        hit.count++;
        return;
      }
    }

    RuntimeFallbackHit hit;
    hit.opcode = instruction.opcode;
    hit.reason = LN_RuntimeFallbackReason::UnsupportedOpcode;
    hit.event = event;
    hit.count = 1;
    hit.first_instruction_index = instruction_index;
    hit.first_source_ref_index = instruction.source_ref_index;
    hit.opcode_name = LN_GetRuntimeInstructionSemantics(instruction.opcode) != nullptr ?
                          LN_GetRuntimeInstructionSemantics(instruction.opcode)->name :
                          "unknown";
    hit.profile_counter_name = fallback_profile_counter;
    hit.removal_condition = fallback_removal_condition;
    hit.source_ref_available = instruction.source_ref_index != LN_INVALID_INDEX;
    hit.hot_path_warning_required = true;
    m_runtimeFallbackReport.hits.push_back(hit);
    return;
  }

  m_runtimeFallbackReport.optimized_execution_partial = true;
  m_runtimeFallbackReport.reason_mask |= 1u << uint8_t(diagnostic->reason);

  for (RuntimeFallbackHit &hit : m_runtimeFallbackReport.hits) {
    if (hit.opcode == instruction.opcode && hit.reason == diagnostic->reason) {
      hit.count++;
      return;
    }
  }

  RuntimeFallbackHit hit;
  hit.opcode = instruction.opcode;
  hit.reason = diagnostic->reason;
  hit.event = event;
  hit.count = 1;
  hit.first_instruction_index = instruction_index;
  hit.first_source_ref_index = instruction.source_ref_index;
  hit.opcode_name = diagnostic->name;
  hit.profile_counter_name = diagnostic->profile_counter_name;
  hit.removal_condition = diagnostic->removal_condition;
  hit.source_ref_available = instruction.source_ref_index != LN_INVALID_INDEX;
  hit.hot_path_warning_required = diagnostic->hot_path_warning_required;
  m_runtimeFallbackReport.hits.push_back(hit);
}

void LN_RuntimeTree::RecordRegisterExpressionFallback(const LN_RuntimeExpressionFamily family,
                                                      const uint32_t expression_index)
{
  if (m_activeProfileCounters == nullptr) {
    return;
  }

  if (m_program == nullptr || expression_index == LN_INVALID_INDEX) {
    return;
  }

  uint32_t kind = 0;
  LN_RuntimeExpressionSemantics semantics;
  switch (family) {
    case LN_RuntimeExpressionFamily::Bool:
      if (expression_index >= m_program->GetBoolExpressions().size()) {
        return;
      }
      kind = uint32_t(m_program->GetBoolExpressions()[expression_index].kind);
      semantics = LN_GetRuntimeExpressionSemantics(
          m_program->GetBoolExpressions()[expression_index].kind);
      break;
    case LN_RuntimeExpressionFamily::Int:
      if (expression_index >= m_program->GetIntExpressions().size()) {
        return;
      }
      kind = uint32_t(m_program->GetIntExpressions()[expression_index].kind);
      semantics = LN_GetRuntimeExpressionSemantics(
          m_program->GetIntExpressions()[expression_index].kind);
      break;
    case LN_RuntimeExpressionFamily::Float:
      if (expression_index >= m_program->GetFloatExpressions().size()) {
        return;
      }
      kind = uint32_t(m_program->GetFloatExpressions()[expression_index].kind);
      semantics = LN_GetRuntimeExpressionSemantics(
          m_program->GetFloatExpressions()[expression_index].kind);
      break;
    case LN_RuntimeExpressionFamily::Vector:
      if (expression_index >= m_program->GetVectorExpressions().size()) {
        return;
      }
      kind = uint32_t(m_program->GetVectorExpressions()[expression_index].kind);
      semantics = LN_GetRuntimeExpressionSemantics(
          m_program->GetVectorExpressions()[expression_index].kind);
      break;
    case LN_RuntimeExpressionFamily::Color:
      if (expression_index >= m_program->GetColorExpressions().size()) {
        return;
      }
      kind = uint32_t(m_program->GetColorExpressions()[expression_index].kind);
      semantics = LN_GetRuntimeExpressionSemantics(
          m_program->GetColorExpressions()[expression_index].kind);
      break;
    case LN_RuntimeExpressionFamily::String:
      if (expression_index >= m_program->GetStringExpressions().size()) {
        return;
      }
      kind = uint32_t(m_program->GetStringExpressions()[expression_index].kind);
      semantics = LN_GetRuntimeExpressionSemantics(
          m_program->GetStringExpressions()[expression_index].kind);
      break;
    case LN_RuntimeExpressionFamily::Value:
      if (expression_index >= m_program->GetValueExpressions().size()) {
        return;
      }
      kind = uint32_t(m_program->GetValueExpressions()[expression_index].kind);
      semantics = LN_GetRuntimeExpressionSemantics(
          m_program->GetValueExpressions()[expression_index].kind);
      break;
    case LN_RuntimeExpressionFamily::Query:
      if (expression_index >= m_program->GetQueryExpressions().size()) {
        return;
      }
      kind = uint32_t(m_program->GetQueryExpressions()[expression_index].kind);
      semantics = LN_GetRuntimeExpressionSemantics(
          m_program->GetQueryExpressions()[expression_index].kind);
      break;
  }

  const LN_Phase10ExpressionFallbackDiagnostic *diagnostic = FindExpressionFallbackDiagnostic(
      family, kind);
  if (diagnostic == nullptr && semantics.fallback_requirements == LN_RUNTIME_FALLBACK_NONE) {
    return;
  }
  const LN_RuntimeFallbackReason reason = diagnostic != nullptr ?
                                              diagnostic->reason :
                                              FallbackReasonFromRequirements(
                                                  semantics.fallback_requirements);
  const char *expression_name = diagnostic != nullptr ? diagnostic->name : semantics.name;
  const char *profile_counter_name = diagnostic != nullptr ? diagnostic->profile_counter_name :
                                                             "register_expression_fallback_count";
  const char *removal_condition =
      diagnostic != nullptr ?
          diagnostic->removal_condition :
          "Keep this runtime fallback until all inputs required by the register expression are "
          "available in typed register storage.";
  const bool source_ref_required = diagnostic != nullptr ? diagnostic->source_ref_required : true;
  const bool hot_path_warning_required = diagnostic != nullptr ?
                                             diagnostic->hot_path_warning_required :
                                             true;

  m_runtimeFallbackReport.optimized_execution_partial = true;
  m_runtimeFallbackReport.expression_fallback_count++;
  m_runtimeFallbackReport.expression_reason_mask |= 1u << uint8_t(reason);

  for (RuntimeExpressionFallbackHit &hit : m_runtimeFallbackReport.expression_hits) {
    if (hit.family == family && hit.kind == kind) {
      hit.count++;
      return;
    }
  }

  RuntimeExpressionFallbackHit hit;
  hit.family = family;
  hit.kind = kind;
  hit.expression_index = expression_index;
  hit.reason = reason;
  hit.count = 1;
  hit.expression_name = expression_name;
  hit.profile_counter_name = profile_counter_name;
  hit.removal_condition = removal_condition;
  hit.source_ref_required = source_ref_required;
  hit.hot_path_warning_required = hot_path_warning_required;
  m_runtimeFallbackReport.expression_hits.push_back(hit);
}

void LN_RuntimeTree::RecordRuntimeSystemFallback(const LN_RuntimeFallbackReason reason,
                                                 const std::string &debug_name,
                                                 const char *profile_counter_name,
                                                 const char *removal_condition) const
{
  if (m_activeProfileCounters == nullptr) {
    return;
  }

  m_runtimeFallbackReport.optimized_execution_partial = true;
  m_runtimeFallbackReport.system_fallback_count++;
  m_runtimeFallbackReport.system_reason_mask |= 1u << uint8_t(reason);
  m_activeProfileCounters->fallback_path_count++;

  for (RuntimeSystemFallbackHit &hit : m_runtimeFallbackReport.system_hits) {
    if (hit.reason == reason && hit.debug_name == debug_name) {
      hit.count++;
      return;
    }
  }

  RuntimeSystemFallbackHit hit;
  hit.reason = reason;
  hit.count = 1;
  hit.debug_name = debug_name;
  hit.profile_counter_name = profile_counter_name;
  hit.removal_condition = removal_condition;
  hit.hot_path_warning_required = true;
  m_runtimeFallbackReport.system_hits.push_back(std::move(hit));
}

void LN_RuntimeTree::ProfileRegisterExpressionHit()
{
  if (m_activeProfileCounters != nullptr) {
    m_activeProfileCounters->register_expression_hit_count++;
  }
}

void LN_RuntimeTree::ProfileRegisterExpressionFallback()
{
  if (m_activeProfileCounters != nullptr) {
    m_activeProfileCounters->register_expression_fallback_count++;
  }
}

void LN_RuntimeTree::ProfileRegisterSimdBatch(const uint32_t lane_count)
{
  if (m_activeProfileCounters != nullptr) {
    m_activeProfileCounters->register_simd_batch_count++;
    m_activeProfileCounters->register_simd_lane_count += lane_count;
  }
}

static bool RegisterOpHasFloatSimdKernel(const LN_RegisterExpressionOpKind kind)
{
  switch (kind) {
    case LN_RegisterExpressionOpKind::FloatAdd:
    case LN_RegisterExpressionOpKind::FloatSubtract:
    case LN_RegisterExpressionOpKind::FloatMultiply:
    case LN_RegisterExpressionOpKind::FloatDivide:
    case LN_RegisterExpressionOpKind::FloatSafeDivide:
    case LN_RegisterExpressionOpKind::FloatMinimum:
    case LN_RegisterExpressionOpKind::FloatMaximum:
      return true;
    default:
      return false;
  }
}

static float EvaluateRegisterFloatKernel(const LN_RegisterExpressionOpKind kind,
                                         const float a,
                                         const float b)
{
  switch (kind) {
    case LN_RegisterExpressionOpKind::FloatAdd:
      return a + b;
    case LN_RegisterExpressionOpKind::FloatSubtract:
      return a - b;
    case LN_RegisterExpressionOpKind::FloatMultiply:
      return a * b;
    case LN_RegisterExpressionOpKind::FloatDivide:
      return std::fabs(b) <= 1.0e-20f ? 0.0f : a / b;
    case LN_RegisterExpressionOpKind::FloatSafeDivide:
      return b == 0.0f ? 0.0f : a / b;
    case LN_RegisterExpressionOpKind::FloatMinimum:
      return std::min(a, b);
    case LN_RegisterExpressionOpKind::FloatMaximum:
      return std::max(a, b);
    default:
      return 0.0f;
  }
}

bool LN_RuntimeTree::TryEvaluateRegisterExpressionSoABatch(const uint32_t expression_index,
                                                           const LN_TickContext &context)
{
  if (!context.use_register_expression_evaluator || m_program == nullptr) {
    return false;
  }

  const LN_RegisterExpressionProgram &ir = m_program->GetRegisterExpressionProgram();
  if (!ir.valid ||
      (m_activeProfileCounters == nullptr && !m_registerExpressionIRHasProductionBenefit) ||
      expression_index >= ir.float_expression_registers.size())
  {
    return false;
  }

  const uint32_t reg = ir.float_expression_registers[expression_index];
  if (reg == LN_INVALID_INDEX || reg >= m_registerExpressionState.float_valid.size()) {
    return false;
  }
  if (m_registerExpressionState.float_valid[reg] == m_registerExpressionState.active_generation) {
    return true;
  }

  for (const LN_RegisterExpressionSoABatch &batch : ir.soa_batches) {
    if (!batch.simd_candidate || !RegisterOpHasFloatSimdKernel(batch.kind)) {
      continue;
    }
    const uint32_t batch_end = batch.first_op_index + batch.op_count;
    if (batch_end > ir.ops.size()) {
      continue;
    }
    bool contains_expression = false;
    for (uint32_t op_index = batch.first_op_index; op_index < batch_end; op_index++) {
      const LN_RegisterExpressionOp &op = ir.ops[op_index];
      if (op.output_kind == LN_RegisterValueKind::Float && op.expression_index == expression_index)
      {
        contains_expression = true;
        break;
      }
    }
    if (contains_expression && TryEvaluateRegisterFloatSoABatch(ir, batch, context)) {
      return m_registerExpressionState.float_valid[reg] ==
             m_registerExpressionState.active_generation;
    }
  }
  return false;
}

bool LN_RuntimeTree::TryEvaluateRegisterFloatSoABatch(const LN_RegisterExpressionProgram &ir,
                                                      const LN_RegisterExpressionSoABatch &batch,
                                                      const LN_TickContext &context)
{
  if (!batch.simd_candidate || !RegisterOpHasFloatSimdKernel(batch.kind)) {
    return false;
  }
  const uint32_t batch_end = batch.first_op_index + batch.op_count;
  if (batch_end > ir.ops.size()) {
    return false;
  }

  auto batch_outputs_ref = [&](const LN_RegisterExpressionRef &ref) {
    if (ref.value_kind != LN_RegisterValueKind::Float || ref.register_index == LN_INVALID_INDEX) {
      return false;
    }
    for (uint32_t op_index = batch.first_op_index; op_index < batch_end; op_index++) {
      const LN_RegisterExpressionOp &op = ir.ops[op_index];
      if (op.output_kind == LN_RegisterValueKind::Float &&
          op.output_register == ref.register_index)
      {
        return true;
      }
    }
    return false;
  };

  for (uint32_t op_index = batch.first_op_index; op_index < batch_end; op_index++) {
    const LN_RegisterExpressionOp &op = ir.ops[op_index];
    if (op.kind != batch.kind || op.output_kind != LN_RegisterValueKind::Float ||
        batch_outputs_ref(op.input0) || batch_outputs_ref(op.input1))
    {
      return false;
    }
  }

  uint32_t processed_lanes = 0;
#if LN_HAS_SSE_REGISTER_SIMD
  const uint32_t simd_lane_count = (batch.op_count / 4u) * 4u;
  for (; processed_lanes < simd_lane_count; processed_lanes += 4u) {
    float left[4];
    float right[4];
    for (uint32_t lane_offset = 0; lane_offset < 4u; lane_offset++) {
      const LN_RegisterExpressionOp &op =
          ir.ops[batch.first_op_index + processed_lanes + lane_offset];
      if (!TryResolveRegisterFloatExpression(
              op.input0.expression_index, context, left[lane_offset]) ||
          !TryResolveRegisterFloatExpression(
              op.input1.expression_index, context, right[lane_offset]))
      {
        return false;
      }
    }

    const __m128 a = _mm_loadu_ps(left);
    const __m128 b = _mm_loadu_ps(right);
    __m128 value;
    switch (batch.kind) {
      case LN_RegisterExpressionOpKind::FloatAdd:
        value = _mm_add_ps(a, b);
        break;
      case LN_RegisterExpressionOpKind::FloatSubtract:
        value = _mm_sub_ps(a, b);
        break;
      case LN_RegisterExpressionOpKind::FloatMultiply:
        value = _mm_mul_ps(a, b);
        break;
      case LN_RegisterExpressionOpKind::FloatDivide: {
        const __m128 zero = _mm_setzero_ps();
        const __m128 one = _mm_set1_ps(1.0f);
        const __m128 epsilon = _mm_set1_ps(1.0e-20f);
        const __m128 abs_b = _mm_max_ps(b, _mm_sub_ps(zero, b));
        /* The scalar guard treats NaN as a non-zero divisor and therefore propagates NaN. Keep
         * SIMD behavior identical while substituting one only for finite near-zero lanes. */
        const __m128 valid_divisor = _mm_or_ps(_mm_cmpgt_ps(abs_b, epsilon),
                                               _mm_cmpunord_ps(b, b));
        const __m128 safe_divisor = _mm_or_ps(_mm_and_ps(valid_divisor, b),
                                              _mm_andnot_ps(valid_divisor, one));
        value = _mm_and_ps(_mm_div_ps(a, safe_divisor), valid_divisor);
        break;
      }
      case LN_RegisterExpressionOpKind::FloatSafeDivide: {
        const __m128 zero = _mm_setzero_ps();
        const __m128 one = _mm_set1_ps(1.0f);
        const __m128 valid_divisor = _mm_cmpneq_ps(b, zero);
        /* SSE2 division has no per-lane mask. Substitute one in zero-divisor lanes before the
         * divide so safe-divide does not raise a spurious floating-point divide-by-zero exception,
         * then clear those lanes to Blender's exact +0 result. */
        const __m128 safe_divisor = _mm_or_ps(_mm_and_ps(valid_divisor, b),
                                              _mm_andnot_ps(valid_divisor, one));
        value = _mm_and_ps(_mm_div_ps(a, safe_divisor), valid_divisor);
        break;
      }
      case LN_RegisterExpressionOpKind::FloatMinimum:
        value = _mm_min_ps(a, b);
        break;
      case LN_RegisterExpressionOpKind::FloatMaximum:
        value = _mm_max_ps(a, b);
        break;
      default:
        return false;
    }
    float output[4];
    _mm_storeu_ps(output, value);
    for (uint32_t lane_offset = 0; lane_offset < 4u; lane_offset++) {
      const LN_RegisterExpressionOp &op =
          ir.ops[batch.first_op_index + processed_lanes + lane_offset];
      m_registerExpressionState.float_values[op.output_register] = output[lane_offset];
      m_registerExpressionState.float_valid[op.output_register] =
          m_registerExpressionState.active_generation;
    }
  }
  if (simd_lane_count != 0u) {
    ProfileRegisterSimdBatch(simd_lane_count);
  }
#endif

  for (; processed_lanes < batch.op_count; processed_lanes++) {
    const LN_RegisterExpressionOp &op = ir.ops[batch.first_op_index + processed_lanes];
    float left = 0.0f;
    float right = 0.0f;
    if (!TryResolveRegisterFloatExpression(op.input0.expression_index, context, left) ||
        !TryResolveRegisterFloatExpression(op.input1.expression_index, context, right))
    {
      return false;
    }
    m_registerExpressionState.float_values[op.output_register] = EvaluateRegisterFloatKernel(
        batch.kind, left, right);
    m_registerExpressionState.float_valid[op.output_register] =
        m_registerExpressionState.active_generation;
  }
  return true;
}

void LN_RuntimeTree::EnsureRegisterExpressionState(const LN_TickContext &context)
{
  if (m_program == nullptr) {
    return;
  }

  const LN_RegisterExpressionProgram &ir = m_program->GetRegisterExpressionProgram();
  auto clear_valid_generations = [&]() {
    std::fill(m_registerExpressionState.bool_valid.begin(),
              m_registerExpressionState.bool_valid.end(),
              0u);
    std::fill(m_registerExpressionState.int_valid.begin(),
              m_registerExpressionState.int_valid.end(),
              0u);
    std::fill(m_registerExpressionState.float_valid.begin(),
              m_registerExpressionState.float_valid.end(),
              0u);
    std::fill(m_registerExpressionState.vector_valid.begin(),
              m_registerExpressionState.vector_valid.end(),
              0u);
    std::fill(m_registerExpressionState.color_valid.begin(),
              m_registerExpressionState.color_valid.end(),
              0u);
    std::fill(m_registerExpressionState.string_valid.begin(),
              m_registerExpressionState.string_valid.end(),
              0u);
    std::fill(m_registerExpressionState.generic_valid.begin(),
              m_registerExpressionState.generic_valid.end(),
              0u);
  };

  const bool layout_changed =
      m_registerExpressionState.bool_values.size() != ir.bool_register_count ||
      m_registerExpressionState.int_values.size() != ir.int_register_count ||
      m_registerExpressionState.float_values.size() != ir.float_register_count ||
      m_registerExpressionState.vector_values.size() != ir.vector_register_count ||
      m_registerExpressionState.color_values.size() != ir.color_register_count ||
      m_registerExpressionState.string_values.size() != ir.string_register_count ||
      m_registerExpressionState.generic_values.size() != ir.generic_value_register_count;
  if (layout_changed) {
    m_registerExpressionState.bool_values.resize(ir.bool_register_count);
    m_registerExpressionState.int_values.resize(ir.int_register_count);
    m_registerExpressionState.float_values.resize(ir.float_register_count);
    m_registerExpressionState.vector_values.resize(ir.vector_register_count);
    m_registerExpressionState.color_values.resize(ir.color_register_count);
    m_registerExpressionState.string_values.resize(ir.string_register_count);
    m_registerExpressionState.generic_values.resize(ir.generic_value_register_count);
    m_registerExpressionState.bool_valid.resize(ir.bool_register_count);
    m_registerExpressionState.int_valid.resize(ir.int_register_count);
    m_registerExpressionState.float_valid.resize(ir.float_register_count);
    m_registerExpressionState.vector_valid.resize(ir.vector_register_count);
    m_registerExpressionState.color_valid.resize(ir.color_register_count);
    m_registerExpressionState.string_valid.resize(ir.string_register_count);
    m_registerExpressionState.generic_valid.resize(ir.generic_value_register_count);
    clear_valid_generations();
    m_registerExpressionState.active_generation = 1u;
    m_registerExpressionState.cached_tick = UINT64_MAX;
  }
  if (m_registerExpressionState.cached_tick != context.tick_index) {
    m_registerExpressionState.cached_tick = context.tick_index;
    if (!layout_changed) {
      m_registerExpressionState.active_generation++;
      if (m_registerExpressionState.active_generation == 0u) {
        clear_valid_generations();
        m_registerExpressionState.active_generation = 1u;
      }
    }
  }

  if (m_activeProfileCounters != nullptr &&
      m_registerExpressionState.profiled_tick != context.tick_index)
  {
    m_registerExpressionState.profiled_tick = context.tick_index;
    m_activeProfileCounters->register_scalar_op_count += ir.scalar_op_count;
    m_activeProfileCounters->register_simd_candidate_batch_count += ir.simd_candidate_batch_count;
    m_activeProfileCounters->register_simd_candidate_lane_count += ir.simd_candidate_lane_count;
  }
}

bool LN_RuntimeTree::TryResolveRegisterIntExpression(const uint32_t expression_index,
                                                     const LN_TickContext &context,
                                                     int32_t &r_value)
{
  if (!context.use_register_expression_evaluator || m_program == nullptr) {
    return false;
  }
  const LN_RegisterExpressionProgram &ir = m_program->GetRegisterExpressionProgram();
  if (!ir.valid ||
      (m_activeProfileCounters == nullptr && !m_registerExpressionIRHasProductionBenefit) ||
      expression_index >= ir.int_expression_registers.size())
  {
    return false;
  }
  EnsureRegisterExpressionState(context);
  const uint32_t reg = ir.int_expression_registers[expression_index];
  if (reg == LN_INVALID_INDEX || reg >= m_registerExpressionState.int_values.size()) {
    ProfileRegisterExpressionFallback();
    return false;
  }
  if (m_registerExpressionState.int_valid[reg] == m_registerExpressionState.active_generation) {
    r_value = m_registerExpressionState.int_values[reg];
    ProfileRegisterExpressionHit();
    return true;
  }
  const std::vector<LN_IntExpression> &expressions = m_program->GetIntExpressions();
  if (expression_index >= expressions.size()) {
    return false;
  }
  const LN_IntExpression &expression = expressions[expression_index];
  switch (expression.kind) {
    case LN_IntExpressionKind::Constant:
      r_value = expression.int_value;
      break;
    case LN_IntExpressionKind::RuntimeTreeProperty:
      if (const LN_Value *value = GetTreePropertyValue(expression.property_ref_index)) {
        r_value = value->int_value;
        break;
      }
      ProfileRegisterExpressionFallback();
      return false;
    case LN_IntExpressionKind::SnapshotGameProperty:
      if (const LN_Value *value = m_snapshot.GetGamePropertyValue(expression.property_ref_index)) {
        if (value->exists) {
          r_value = value->int_value;
          break;
        }
      }
      ProfileRegisterExpressionFallback();
      return false;
    case LN_IntExpressionKind::SnapshotCollisionGroup:
      if (expression.input0 != LN_INVALID_INDEX) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      r_value = m_snapshot.GetCollisionGroup();
      break;
    case LN_IntExpressionKind::SnapshotCharacterMaxJumps: {
      if (expression.input0 != LN_INVALID_INDEX) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      const LN_ObjectSnapshot &object_snapshot = m_snapshot.GetObjectSnapshot();
      if (!object_snapshot.has_character) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      r_value = object_snapshot.character_max_jumps;
      break;
    }
    case LN_IntExpressionKind::SnapshotCharacterJumpCount: {
      if (expression.input0 != LN_INVALID_INDEX) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      const LN_ObjectSnapshot &object_snapshot = m_snapshot.GetObjectSnapshot();
      if (!object_snapshot.has_character) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      r_value = object_snapshot.character_jump_count;
      break;
    }
    case LN_IntExpressionKind::MouseWheelDelta:
      /* int_value: also what the UI used. */
      r_value = (expression.int_value != 0 ? m_snapshot.GetRawInputSnapshot() :
                                             m_snapshot.GetInputSnapshot())
                    .GetMouse()
                    .wheel_delta;
      break;
    case LN_IntExpressionKind::WindowResolutionWidth: {
      const LN_InputSnapshot &input = m_snapshot.GetInputSnapshot();
      if (!input.HasCanvas()) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      r_value = input.GetCanvasWidth();
      break;
    }
    case LN_IntExpressionKind::WindowResolutionHeight: {
      const LN_InputSnapshot &input = m_snapshot.GetInputSnapshot();
      if (!input.HasCanvas()) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      r_value = input.GetCanvasHeight();
      break;
    }
    case LN_IntExpressionKind::WindowVSyncMode: {
      const LN_InputSnapshot &input = m_snapshot.GetInputSnapshot();
      if (!input.HasCanvasVSyncMode()) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      r_value = input.GetCanvasVSyncMode();
      break;
    }
    case LN_IntExpressionKind::FromGenericValue: {
      LN_Value value;
      if (!TryResolveRegisterValueExpression(expression.input0, context, value)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      r_value = ValueAsInt(value, expression.float_to_int_mode);
      break;
    }
    case LN_IntExpressionKind::FromBool: {
      bool value = false;
      if (!TryResolveRegisterBoolExpression(expression.input0, context, value)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      r_value = value ? 1 : 0;
      break;
    }
    case LN_IntExpressionKind::FromFloat: {
      float value = 0.0f;
      if (!TryResolveRegisterFloatExpression(expression.input0, context, value)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      r_value = LN_ConvertFloatToInt(value, expression.float_to_int_mode);
      break;
    }
    case LN_IntExpressionKind::FromString: {
      std::string text;
      float value = 0.0f;
      if (!TryResolveRegisterStringExpression(expression.input0, context, text)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      if (!LN_TryParseTypecastFloat(text, value)) {
        value = 0.0f;
      }
      r_value = LN_ConvertFloatToInt(value, expression.float_to_int_mode);
      break;
    }
    case LN_IntExpressionKind::DictLength: {
      if (expression.input0 == LN_INVALID_INDEX) {
        r_value = 0;
        break;
      }
      LN_Value dict_value;
      if (!TryResolveRegisterValueExpression(expression.input0, context, dict_value)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      r_value = dict_value.type == LN_ValueType::Dict && dict_value.exists ?
                    int32_t(dict_value.dict_value.size()) :
                    0;
      break;
    }
    case LN_IntExpressionKind::ListLength: {
      if (expression.input0 == LN_INVALID_INDEX) {
        r_value = 0;
        break;
      }
      LN_Value list_value;
      if (!TryResolveRegisterValueExpression(expression.input0, context, list_value)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      r_value = list_value.type == LN_ValueType::List && list_value.exists ?
                    int32_t(list_value.list_value.size()) :
                    0;
      break;
    }
    case LN_IntExpressionKind::StringCount: {
      std::string text;
      std::string needle;
      if (!TryResolveRegisterStringExpression(expression.input0, context, text) ||
          !TryResolveRegisterStringExpression(expression.input1, context, needle))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      r_value = CountStringOccurrences(text, needle);
      break;
    }
    case LN_IntExpressionKind::Random: {
      int32_t min_value = 0;
      int32_t max_value = 1;
      if (!TryResolveRegisterIntExpression(expression.input0, context, min_value) ||
          !TryResolveRegisterIntExpression(expression.input1, context, max_value))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      const int32_t lower = std::min(min_value, max_value);
      const int32_t upper = std::max(min_value, max_value);
      const uint64_t seed = (uint64_t(expression_index) << 32) ^ context.tick_index;
      r_value = lower + int32_t(HashToUnitFloat(seed) * float(upper - lower + 1));
      break;
    }
    default:
      ProfileRegisterExpressionFallback();
      return false;
  }
  m_registerExpressionState.int_values[reg] = r_value;
  m_registerExpressionState.int_valid[reg] = m_registerExpressionState.active_generation;
  ProfileRegisterExpressionHit();
  return true;
}

bool LN_RuntimeTree::TryResolveRegisterFloatExpression(const uint32_t expression_index,
                                                       const LN_TickContext &context,
                                                       float &r_value)
{
  if (!context.use_register_expression_evaluator || m_program == nullptr) {
    return false;
  }
  const LN_RegisterExpressionProgram &ir = m_program->GetRegisterExpressionProgram();
  if (!ir.valid ||
      (m_activeProfileCounters == nullptr && !m_registerExpressionIRHasProductionBenefit) ||
      expression_index >= ir.float_expression_registers.size())
  {
    return false;
  }
  EnsureRegisterExpressionState(context);
  const uint32_t reg = ir.float_expression_registers[expression_index];
  if (reg == LN_INVALID_INDEX || reg >= m_registerExpressionState.float_values.size()) {
    ProfileRegisterExpressionFallback();
    return false;
  }
  if (m_registerExpressionState.float_valid[reg] == m_registerExpressionState.active_generation) {
    r_value = m_registerExpressionState.float_values[reg];
    ProfileRegisterExpressionHit();
    return true;
  }
  if (TryEvaluateRegisterExpressionSoABatch(expression_index, context) &&
      m_registerExpressionState.float_valid[reg] == m_registerExpressionState.active_generation)
  {
    r_value = m_registerExpressionState.float_values[reg];
    ProfileRegisterExpressionHit();
    return true;
  }
  const std::vector<LN_FloatExpression> &expressions = m_program->GetFloatExpressions();
  if (expression_index >= expressions.size()) {
    return false;
  }
  const LN_FloatExpression &expression = expressions[expression_index];
  float value = 0.0f;
  float a = 0.0f;
  float b = 0.0f;
  float c = 0.0f;
  MT_Vector3 vector(0.0f, 0.0f, 0.0f);
  MT_Vector4 color(0.0f, 0.0f, 0.0f, 1.0f);
  switch (expression.kind) {
    case LN_FloatExpressionKind::Constant:
      value = expression.float_value;
      break;
    case LN_FloatExpressionKind::UnscaledDeltaTime:
      value = float(std::max(0.0, context.unscaled_dt));
      break;
    case LN_FloatExpressionKind::RuntimeTreeProperty:
      if (const LN_Value *property_value = GetTreePropertyValue(expression.property_ref_index)) {
        value = property_value->float_value;
        break;
      }
      ProfileRegisterExpressionFallback();
      return false;
    case LN_FloatExpressionKind::SnapshotGameProperty:
      if (const LN_Value *property_value = m_snapshot.GetGamePropertyValue(
              expression.property_ref_index))
      {
        if (property_value->exists) {
          value = property_value->float_value;
          break;
        }
      }
      ProfileRegisterExpressionFallback();
      return false;
    case LN_FloatExpressionKind::SnapshotTimeScale:
      value = m_snapshot.GetTimeScale();
      break;
    case LN_FloatExpressionKind::SnapshotLightPower:
      if (expression.input0 != LN_INVALID_INDEX) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = m_snapshot.GetObjectSnapshot().light_power;
      break;
    case LN_FloatExpressionKind::SnapshotElapsedTime:
      value = m_snapshot.GetElapsedTime();
      break;
    case LN_FloatExpressionKind::SnapshotFrameDelta:
      value = m_snapshot.GetFrameDelta();
      break;
    case LN_FloatExpressionKind::SnapshotFPS:
      value = m_snapshot.GetFPS();
      break;
    case LN_FloatExpressionKind::SnapshotDeltaFactor:
      value = m_snapshot.GetDeltaFactor();
      break;
    case LN_FloatExpressionKind::GamepadButtonStrength: {
      int32_t gamepad_index = 0;
      if (!TryResolveRegisterIntExpression(expression.input0, context, gamepad_index)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      const LN_GamepadSnapshot *gamepad = m_snapshot.GetInputSnapshot().GetGamepad(gamepad_index);
      if (gamepad == nullptr ||
          !LN_TryGetGamepadTriggerStrength(*gamepad, int32_t(expression.input2), value))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      break;
    }
    case LN_FloatExpressionKind::FromGenericValue: {
      LN_Value generic_value;
      if (!TryResolveRegisterValueExpression(expression.input0, context, generic_value) ||
          !ValueAsFloat(generic_value, value))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      break;
    }
    case LN_FloatExpressionKind::FromBool: {
      bool source = false;
      if (!TryResolveRegisterBoolExpression(expression.bool_expr_index, context, source)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = source ? 1.0f : 0.0f;
      break;
    }
    case LN_FloatExpressionKind::FromInt: {
      int32_t source = 0;
      if (!TryResolveRegisterIntExpression(expression.input0, context, source)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = float(source);
      break;
    }
    case LN_FloatExpressionKind::FromString: {
      std::string source;
      if (!TryResolveRegisterStringExpression(expression.string_expr_index, context, source)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      if (!LN_TryParseTypecastFloat(source, value)) {
        value = 0.0f;
      }
      break;
    }
    case LN_FloatExpressionKind::Add:
      if (!TryResolveRegisterFloatExpression(expression.input0, context, a) ||
          !TryResolveRegisterFloatExpression(expression.input1, context, b))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = a + b;
      break;
    case LN_FloatExpressionKind::Subtract:
      if (!TryResolveRegisterFloatExpression(expression.input0, context, a) ||
          !TryResolveRegisterFloatExpression(expression.input1, context, b))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = a - b;
      break;
    case LN_FloatExpressionKind::Multiply:
      if (!TryResolveRegisterFloatExpression(expression.input0, context, a) ||
          !TryResolveRegisterFloatExpression(expression.input1, context, b))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = a * b;
      break;
    case LN_FloatExpressionKind::Divide:
      if (!TryResolveRegisterFloatExpression(expression.input0, context, a) ||
          !TryResolveRegisterFloatExpression(expression.input1, context, b))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = std::fabs(b) <= 1.0e-20f ? 0.0f : a / b;
      break;
    case LN_FloatExpressionKind::SafeDivide:
      if (!TryResolveRegisterFloatExpression(expression.input0, context, a) ||
          !TryResolveRegisterFloatExpression(expression.input1, context, b))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = b == 0.0f ? 0.0f : a / b;
      break;
    case LN_FloatExpressionKind::Power:
      if (!TryResolveRegisterFloatExpression(expression.input0, context, a) ||
          !TryResolveRegisterFloatExpression(expression.input1, context, b))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = std::pow(a, b);
      break;
    case LN_FloatExpressionKind::Minimum:
      if (!TryResolveRegisterFloatExpression(expression.input0, context, a) ||
          !TryResolveRegisterFloatExpression(expression.input1, context, b))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = std::min(a, b);
      break;
    case LN_FloatExpressionKind::Maximum:
      if (!TryResolveRegisterFloatExpression(expression.input0, context, a) ||
          !TryResolveRegisterFloatExpression(expression.input1, context, b))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = std::max(a, b);
      break;
    case LN_FloatExpressionKind::Absolute:
      if (!TryResolveRegisterFloatExpression(expression.input0, context, a)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = std::fabs(a);
      break;
    case LN_FloatExpressionKind::Sign:
      if (!TryResolveRegisterFloatExpression(expression.input0, context, a)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = float((a > 0.0f) - (a < 0.0f));
      break;
    case LN_FloatExpressionKind::Round:
      if (!TryResolveRegisterFloatExpression(expression.input0, context, a)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = std::floor(a + 0.5f);
      break;
    case LN_FloatExpressionKind::Floor:
      if (!TryResolveRegisterFloatExpression(expression.input0, context, a)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = std::floor(a);
      break;
    case LN_FloatExpressionKind::Ceil:
      if (!TryResolveRegisterFloatExpression(expression.input0, context, a)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = std::ceil(a);
      break;
    case LN_FloatExpressionKind::Truncate:
      if (!TryResolveRegisterFloatExpression(expression.input0, context, a)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = std::trunc(a);
      break;
    case LN_FloatExpressionKind::Fraction:
      if (!TryResolveRegisterFloatExpression(expression.input0, context, a)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = a - std::floor(a);
      break;
    case LN_FloatExpressionKind::Modulo:
      if (!TryResolveRegisterFloatExpression(expression.input0, context, a) ||
          !TryResolveRegisterFloatExpression(expression.input1, context, b))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = std::fabs(b) <= 1.0e-20f ? 0.0f : std::fmod(a, b);
      break;
    case LN_FloatExpressionKind::Sine:
      if (!TryResolveRegisterFloatExpression(expression.input0, context, a)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = std::sin(a);
      break;
    case LN_FloatExpressionKind::Cosine:
      if (!TryResolveRegisterFloatExpression(expression.input0, context, a)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = std::cos(a);
      break;
    case LN_FloatExpressionKind::Radians:
      if (!TryResolveRegisterFloatExpression(expression.input0, context, a)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = a * (LN_PI / 180.0f);
      break;
    case LN_FloatExpressionKind::Degrees:
      if (!TryResolveRegisterFloatExpression(expression.input0, context, a)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = a * (180.0f / LN_PI);
      break;
    case LN_FloatExpressionKind::Negate:
      if (!TryResolveRegisterFloatExpression(expression.input0, context, a)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = -a;
      break;
    case LN_FloatExpressionKind::Clamp:
      if (!TryResolveRegisterFloatExpression(expression.input0, context, a) ||
          !TryResolveRegisterFloatExpression(expression.input1, context, b) ||
          !TryResolveRegisterFloatExpression(expression.input2, context, c))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = std::min(std::max(a, std::min(b, c)), std::max(b, c));
      break;
    case LN_FloatExpressionKind::Threshold: {
      if (!TryResolveRegisterFloatExpression(expression.input0, context, a) ||
          !TryResolveRegisterFloatExpression(expression.input1, context, b))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      bool else_zero = expression.bool_value;
      if (expression.bool_expr_index != LN_INVALID_INDEX &&
          !TryResolveRegisterBoolExpression(expression.bool_expr_index, context, else_zero))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      switch (expression.threshold_operation) {
        case LN_ThresholdOperation::Greater:
          value = (a > b) ? a : (else_zero ? 0.0f : b);
          break;
        case LN_ThresholdOperation::Less:
          value = (a < b) ? a : (else_zero ? 0.0f : b);
          break;
      }
      break;
    }
    case LN_FloatExpressionKind::RangedThreshold:
      if (!TryResolveRegisterFloatExpression(expression.input0, context, a) ||
          !TryResolveRegisterFloatExpression(expression.input1, context, b) ||
          !TryResolveRegisterFloatExpression(expression.input2, context, c))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      switch (expression.range_operation) {
        case LN_RangeOperation::Inside:
          value = (b < a && a < c) ? a : 0.0f;
          break;
        case LN_RangeOperation::Outside:
          value = (a < b || a > c) ? a : 0.0f;
          break;
      }
      break;
    case LN_FloatExpressionKind::Select: {
      bool condition = expression.bool_value;
      if (expression.bool_expr_index != LN_INVALID_INDEX &&
          !TryResolveRegisterBoolExpression(expression.bool_expr_index, context, condition))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      if (!TryResolveRegisterFloatExpression(expression.input0, context, a) ||
          !TryResolveRegisterFloatExpression(expression.input1, context, b))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = condition ? a : b;
      break;
    }
    case LN_FloatExpressionKind::Formula: {
      if (!TryResolveRegisterFloatExpression(expression.input0, context, a) ||
          !TryResolveRegisterFloatExpression(expression.input1, context, b))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      const std::vector<LN_StringExpression> &strings = m_program->GetStringExpressions();
      if (expression.string_expr_index >= strings.size() ||
          strings[expression.string_expr_index].kind != LN_StringExpressionKind::Constant)
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = LN::EvaluateFormula(strings[expression.string_expr_index].string_value, a, b);
      break;
    }
    case LN_FloatExpressionKind::FloatCurve:
      if (!TryResolveRegisterFloatExpression(expression.input0, context, a) ||
          !TryResolveRegisterFloatExpression(expression.input1, context, b))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = EvaluateCurveTable(
          expression.int_value >= 0 ? uint32_t(expression.int_value) : LN_INVALID_INDEX, a, b);
      break;
    case LN_FloatExpressionKind::VectorComponent:
      if (!TryResolveRegisterVectorExpression(expression.input0, context, vector) ||
          expression.component_index > 2)
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = vector[expression.component_index];
      break;
    case LN_FloatExpressionKind::ColorComponent:
      if (!TryResolveRegisterColorExpression(expression.input0, context, color) ||
          expression.component_index > 3)
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      switch (expression.component_index) {
        case 0:
          value = color.x();
          break;
        case 1:
          value = color.y();
          break;
        case 2:
          value = color.z();
          break;
        case 3:
          value = color.w();
          break;
      }
      break;
    case LN_FloatExpressionKind::Random:
      if (!TryResolveRegisterFloatExpression(expression.input0, context, a) ||
          !TryResolveRegisterFloatExpression(expression.input1, context, b))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      {
        const float lower = std::min(a, b);
        const float upper = std::max(a, b);
        const uint64_t seed = (uint64_t(expression_index) << 32) ^ context.tick_index;
        value = lower + (upper - lower) * HashToUnitFloat(seed);
      }
      break;
    default:
      ProfileRegisterExpressionFallback();
      return false;
  }
  m_registerExpressionState.float_values[reg] = value;
  m_registerExpressionState.float_valid[reg] = m_registerExpressionState.active_generation;
  r_value = value;
  ProfileRegisterExpressionHit();
  return true;
}

bool LN_RuntimeTree::TryResolveRegisterVectorExpression(const uint32_t expression_index,
                                                        const LN_TickContext &context,
                                                        MT_Vector3 &r_value)
{
  if (!context.use_register_expression_evaluator || m_program == nullptr) {
    return false;
  }
  const LN_RegisterExpressionProgram &ir = m_program->GetRegisterExpressionProgram();
  if (!ir.valid ||
      (m_activeProfileCounters == nullptr && !m_registerExpressionIRHasProductionBenefit) ||
      expression_index >= ir.vector_expression_registers.size())
  {
    return false;
  }
  EnsureRegisterExpressionState(context);
  const uint32_t reg = ir.vector_expression_registers[expression_index];
  if (reg == LN_INVALID_INDEX || reg >= m_registerExpressionState.vector_values.size()) {
    ProfileRegisterExpressionFallback();
    return false;
  }
  if (m_registerExpressionState.vector_valid[reg] == m_registerExpressionState.active_generation) {
    r_value = m_registerExpressionState.vector_values[reg];
    ProfileRegisterExpressionHit();
    return true;
  }
  const std::vector<LN_VectorExpression> &expressions = m_program->GetVectorExpressions();
  if (expression_index >= expressions.size()) {
    return false;
  }
  const LN_VectorExpression &expression = expressions[expression_index];
  MT_Vector3 value(0.0f, 0.0f, 0.0f);
  MT_Vector3 a(0.0f, 0.0f, 0.0f);
  MT_Vector3 b(0.0f, 0.0f, 0.0f);
  float scalar = 0.0f;
  switch (expression.kind) {
    case LN_VectorExpressionKind::Constant:
      value = expression.vector_value;
      break;
    case LN_VectorExpressionKind::RuntimeTreeProperty:
      if (const LN_Value *property_value = GetTreePropertyValue(expression.property_ref_index)) {
        value = (property_value->type == LN_ValueType::Rotation) ?
                    property_value->RotationEulerRadians() :
                    property_value->vector_value;
        break;
      }
      ProfileRegisterExpressionFallback();
      return false;
    case LN_VectorExpressionKind::SnapshotWorldPosition:
      if (expression.input0 != LN_INVALID_INDEX) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = m_snapshot.GetObjectSnapshot().world_position;
      break;
    case LN_VectorExpressionKind::SnapshotLocalPosition:
      if (expression.input0 != LN_INVALID_INDEX) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = m_snapshot.GetObjectSnapshot().local_position;
      break;
    case LN_VectorExpressionKind::SnapshotWorldOrientation:
      if (expression.input0 != LN_INVALID_INDEX) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = m_snapshot.GetObjectSnapshot().world_orientation.ToEulerXYZ();
      break;
    case LN_VectorExpressionKind::SnapshotLocalOrientation:
      if (expression.input0 != LN_INVALID_INDEX) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = m_snapshot.GetObjectSnapshot().local_orientation.ToEulerXYZ();
      break;
    case LN_VectorExpressionKind::SnapshotWorldScale:
      if (expression.input0 != LN_INVALID_INDEX) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = m_snapshot.GetObjectSnapshot().world_scale;
      break;
    case LN_VectorExpressionKind::SnapshotLocalScale:
      if (expression.input0 != LN_INVALID_INDEX) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = m_snapshot.GetObjectSnapshot().local_scale;
      break;
    case LN_VectorExpressionKind::SnapshotLinearVelocity:
      if (expression.input0 != LN_INVALID_INDEX) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = m_snapshot.GetObjectSnapshot().linear_velocity;
      break;
    case LN_VectorExpressionKind::SnapshotLocalLinearVelocity:
      if (expression.input0 != LN_INVALID_INDEX) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = m_snapshot.GetObjectSnapshot().local_linear_velocity;
      break;
    case LN_VectorExpressionKind::SnapshotAngularVelocity:
      if (expression.input0 != LN_INVALID_INDEX) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = m_snapshot.GetObjectSnapshot().angular_velocity;
      break;
    case LN_VectorExpressionKind::SnapshotLocalAngularVelocity:
      if (expression.input0 != LN_INVALID_INDEX) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = m_snapshot.GetObjectSnapshot().local_angular_velocity;
      break;
    case LN_VectorExpressionKind::SnapshotGravity:
      value = m_snapshot.GetGravity();
      break;
    case LN_VectorExpressionKind::BonePoseLocation:
    case LN_VectorExpressionKind::BonePoseScale:
      ProfileRegisterExpressionFallback();
      return false;
    case LN_VectorExpressionKind::SnapshotCharacterGravity: {
      if (expression.input0 != LN_INVALID_INDEX) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      const LN_ObjectSnapshot &object_snapshot = m_snapshot.GetObjectSnapshot();
      if (!object_snapshot.has_character) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = object_snapshot.character_gravity;
      break;
    }
    case LN_VectorExpressionKind::SnapshotCharacterWalkDirection: {
      if (expression.input0 != LN_INVALID_INDEX) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      const LN_ObjectSnapshot &object_snapshot = m_snapshot.GetObjectSnapshot();
      if (!object_snapshot.has_character) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = object_snapshot.character_walk_direction_world;
      break;
    }
    case LN_VectorExpressionKind::SnapshotCharacterLocalWalkDirection: {
      if (expression.input0 != LN_INVALID_INDEX) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      const LN_ObjectSnapshot &object_snapshot = m_snapshot.GetObjectSnapshot();
      if (!object_snapshot.has_character) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = object_snapshot.character_walk_direction_local;
      break;
    }
    case LN_VectorExpressionKind::AxisVector: {
      if (expression.input0 != LN_INVALID_INDEX) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      const MT_Matrix3x3 orientation = m_snapshot.GetObjectSnapshot().world_orientation.ToMatrix();
      const int32_t axis = int32_t(expression.property_ref_index);
      value = axis == 0 || axis == 3 ? orientation * MT_Vector3(1.0f, 0.0f, 0.0f) :
              axis == 1 || axis == 4 ? orientation * MT_Vector3(0.0f, 1.0f, 0.0f) :
                                       orientation * MT_Vector3(0.0f, 0.0f, 1.0f);
      if (axis >= 3) {
        value = -value;
      }
      break;
    }
    case LN_VectorExpressionKind::CursorPosition: {
      const LN_MouseSnapshot &mouse = m_snapshot.GetInputSnapshot().GetMouse();
      float y = mouse.normalized_y;
      if (expression.property_ref_index != 0) {
        y = 1.0f - y;
      }
      value = MT_Vector3(mouse.normalized_x, y, 0.0f);
      break;
    }
    case LN_VectorExpressionKind::CursorMovement: {
      const LN_MouseSnapshot &mouse = m_snapshot.GetInputSnapshot().GetMouse();
      value = MT_Vector3(float(mouse.delta_x), float(mouse.delta_y), 0.0f);
      break;
    }
    case LN_VectorExpressionKind::WindowResolution: {
      const LN_InputSnapshot &input = m_snapshot.GetInputSnapshot();
      if (!input.HasCanvas()) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = MT_Vector3(float(input.GetCanvasWidth()), float(input.GetCanvasHeight()), 0.0f);
      break;
    }
    case LN_VectorExpressionKind::GamepadStick: {
      int32_t gamepad_index = 0;
      float threshold = 0.1f;
      float sensitivity = expression.float_value;
      if (!TryResolveRegisterIntExpression(expression.input0, context, gamepad_index) ||
          !TryResolveRegisterFloatExpression(expression.input1, context, threshold) ||
          !TryResolveRegisterFloatExpression(expression.float_expr_index, context, sensitivity))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      threshold = std::max(0.0f, threshold);
      const LN_GamepadSnapshot *gamepad = m_snapshot.GetInputSnapshot().GetGamepad(gamepad_index);
      if (gamepad == nullptr || !gamepad->connected) {
        ProfileRegisterExpressionFallback();
        return false;
      }

      const int32_t invert_flags = int32_t(expression.property_ref_index);
      const int32_t axis_offset = (expression.input2 == 1u) ? 2 : 0;
      float x = NormalizeGamepadAxisValue(gamepad->axes[size_t(axis_offset)]);
      float y = -NormalizeGamepadAxisValue(gamepad->axes[size_t(axis_offset + 1)]);
      if (invert_flags & 1) {
        x = -x;
      }
      if (invert_flags & 2) {
        y = -y;
      }
      if (std::fabs(x) < threshold) {
        x = 0.0f;
      }
      if (std::fabs(y) < threshold) {
        y = 0.0f;
      }
      value = MT_Vector3(x * sensitivity, y * sensitivity, 0.0f);
      break;
    }
    case LN_VectorExpressionKind::FromGenericValue: {
      LN_Value generic_value;
      if (!TryResolveRegisterValueExpression(expression.input0, context, generic_value) ||
          !ValueAsVector(generic_value, value))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      break;
    }
    case LN_VectorExpressionKind::BroadcastFloat: {
      float scalar = 0.0f;
      if (!TryResolveRegisterFloatExpression(expression.float_expr_index, context, scalar)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = MT_Vector3(scalar, scalar, scalar);
      break;
    }
    case LN_VectorExpressionKind::BroadcastInt: {
      int32_t scalar = 0;
      if (!TryResolveRegisterIntExpression(expression.input0, context, scalar)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = MT_Vector3(float(scalar), float(scalar), float(scalar));
      break;
    }
    case LN_VectorExpressionKind::FromColor: {
      MT_Vector4 source(0.0f, 0.0f, 0.0f, 1.0f);
      if (!TryResolveRegisterColorExpression(expression.input0, context, source)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = MT_Vector3(source.x(), source.y(), source.z());
      break;
    }
    case LN_VectorExpressionKind::Add:
      if (!TryResolveRegisterVectorExpression(expression.input0, context, a) ||
          !TryResolveRegisterVectorExpression(expression.input1, context, b))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = a + b;
      break;
    case LN_VectorExpressionKind::Subtract:
      if (!TryResolveRegisterVectorExpression(expression.input0, context, a) ||
          !TryResolveRegisterVectorExpression(expression.input1, context, b))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = a - b;
      break;
    case LN_VectorExpressionKind::Multiply:
      if (!TryResolveRegisterVectorExpression(expression.input0, context, a) ||
          !TryResolveRegisterVectorExpression(expression.input1, context, b))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = MT_Vector3(a.x() * b.x(), a.y() * b.y(), a.z() * b.z());
      break;
    case LN_VectorExpressionKind::Divide:
      if (!TryResolveRegisterVectorExpression(expression.input0, context, a) ||
          !TryResolveRegisterVectorExpression(expression.input1, context, b))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = MT_Vector3(std::fabs(b.x()) <= 1.0e-20f ? 0.0f : a.x() / b.x(),
                         std::fabs(b.y()) <= 1.0e-20f ? 0.0f : a.y() / b.y(),
                         std::fabs(b.z()) <= 1.0e-20f ? 0.0f : a.z() / b.z());
      break;
    case LN_VectorExpressionKind::Absolute:
      if (!TryResolveRegisterVectorExpression(expression.input0, context, a)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = MT_Vector3(std::fabs(a.x()), std::fabs(a.y()), std::fabs(a.z()));
      break;
    case LN_VectorExpressionKind::Minimum:
      if (!TryResolveRegisterVectorExpression(expression.input0, context, a) ||
          !TryResolveRegisterVectorExpression(expression.input1, context, b))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = MT_Vector3(std::min(a.x(), b.x()), std::min(a.y(), b.y()), std::min(a.z(), b.z()));
      break;
    case LN_VectorExpressionKind::Maximum:
      if (!TryResolveRegisterVectorExpression(expression.input0, context, a) ||
          !TryResolveRegisterVectorExpression(expression.input1, context, b))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = MT_Vector3(std::max(a.x(), b.x()), std::max(a.y(), b.y()), std::max(a.z(), b.z()));
      break;
    case LN_VectorExpressionKind::Scale:
      if (!TryResolveRegisterVectorExpression(expression.input0, context, a) ||
          !TryResolveRegisterFloatExpression(expression.float_expr_index, context, scalar))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = a * scalar;
      break;
    case LN_VectorExpressionKind::Normalize:
      if (!TryResolveRegisterVectorExpression(expression.input0, context, a)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      {
        const float length = a.length();
        value = length <= 1.0e-20f ? MT_Vector3(0.0f, 0.0f, 0.0f) : a * (1.0f / length);
      }
      break;
    case LN_VectorExpressionKind::Resize:
      if (!TryResolveRegisterVectorExpression(expression.input0, context, a)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = expression.float_value >= 3.0f ? a : MT_Vector3(a.x(), a.y(), 0.0f);
      break;
    case LN_VectorExpressionKind::RotateAroundAxis: {
      if (!TryResolveRegisterVectorExpression(expression.input0, context, a) ||
          !TryResolveRegisterVectorExpression(expression.input1, context, b))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      const int32_t rotate_mode = int32_t(expression.property_ref_index) / 4;
      const int32_t rotate_axis = int32_t(expression.property_ref_index) % 4;
      if (rotate_mode == 3) {
        MT_Vector3 euler(0.0f, 0.0f, 0.0f);
        if (!TryResolveRegisterVectorExpression(expression.input2, context, euler)) {
          ProfileRegisterExpressionFallback();
          return false;
        }
        value = b + MT_Matrix3x3(euler) * (a - b);
        break;
      }
      MT_Vector3 axis(0.0f, 0.0f, 1.0f);
      if (rotate_mode == 1) {
        axis = rotate_axis == 0 ? MT_Vector3(1.0f, 0.0f, 0.0f) :
               rotate_axis == 1 ? MT_Vector3(0.0f, 1.0f, 0.0f) :
                                  MT_Vector3(0.0f, 0.0f, 1.0f);
      }
      else if (rotate_mode == 2 &&
               !TryResolveRegisterVectorExpression(expression.input2, context, axis))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      const float axis_length = axis.length();
      if (axis_length <= 1.0e-20f) {
        value = a;
        break;
      }
      if (!TryResolveRegisterFloatExpression(expression.float_expr_index, context, scalar)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      axis *= 1.0f / axis_length;
      const MT_Vector3 relative = a - b;
      const float cosine = std::cos(scalar);
      const float sine = std::sin(scalar);
      value = b + relative * cosine + axis.cross(relative) * sine +
              axis * (axis.dot(relative) * (1.0f - cosine));
      break;
    }
    case LN_VectorExpressionKind::VectorToRotation:
      if (!TryResolveRegisterVectorExpression(expression.input0, context, a) ||
          !TryResolveRegisterVectorExpression(expression.input1, context, b))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = VectorToRotationEuler(a, b, int32_t(expression.property_ref_index));
      break;
    case LN_VectorExpressionKind::Combine: {
      float x = expression.vector_value.x();
      float y = expression.vector_value.y();
      float z = expression.vector_value.z();
      if (expression.input0 != LN_INVALID_INDEX &&
          !TryResolveRegisterFloatExpression(expression.input0, context, x))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      if (expression.input1 != LN_INVALID_INDEX &&
          !TryResolveRegisterFloatExpression(expression.input1, context, y))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      if (expression.input2 != LN_INVALID_INDEX &&
          !TryResolveRegisterFloatExpression(expression.input2, context, z))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = MT_Vector3(x, y, z);
      break;
    }
    case LN_VectorExpressionKind::Random:
      if (!TryResolveRegisterVectorExpression(expression.input0, context, a)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = MT_Vector3(
          a.x() != 0.0f ?
              HashToUnitFloat((uint64_t(expression_index) << 32) ^ context.tick_index ^ 0x13579u) :
              0.0f,
          a.y() != 0.0f ?
              HashToUnitFloat((uint64_t(expression_index) << 32) ^ context.tick_index ^ 0x24680u) :
              0.0f,
          a.z() != 0.0f ? HashToUnitFloat((uint64_t(expression_index) << 32) ^ context.tick_index ^
                                          0xabcdefu) :
                          0.0f);
      break;
    default:
      ProfileRegisterExpressionFallback();
      return false;
  }
  m_registerExpressionState.vector_values[reg] = value;
  m_registerExpressionState.vector_valid[reg] = m_registerExpressionState.active_generation;
  r_value = value;
  ProfileRegisterExpressionHit();
  return true;
}

bool LN_RuntimeTree::TryResolveRegisterColorExpression(const uint32_t expression_index,
                                                       const LN_TickContext &context,
                                                       MT_Vector4 &r_value)
{
  if (!context.use_register_expression_evaluator || m_program == nullptr) {
    return false;
  }
  const LN_RegisterExpressionProgram &ir = m_program->GetRegisterExpressionProgram();
  if (!ir.valid ||
      (m_activeProfileCounters == nullptr && !m_registerExpressionIRHasProductionBenefit) ||
      expression_index >= ir.color_expression_registers.size())
  {
    return false;
  }
  EnsureRegisterExpressionState(context);
  const uint32_t reg = ir.color_expression_registers[expression_index];
  if (reg == LN_INVALID_INDEX || reg >= m_registerExpressionState.color_values.size()) {
    ProfileRegisterExpressionFallback();
    return false;
  }
  if (m_registerExpressionState.color_valid[reg] == m_registerExpressionState.active_generation) {
    r_value = m_registerExpressionState.color_values[reg];
    ProfileRegisterExpressionHit();
    return true;
  }
  const std::vector<LN_ColorExpression> &expressions = m_program->GetColorExpressions();
  if (expression_index >= expressions.size()) {
    return false;
  }
  const LN_ColorExpression &expression = expressions[expression_index];
  MT_Vector4 value(0.0f, 0.0f, 0.0f, 1.0f);
  switch (expression.kind) {
    case LN_ColorExpressionKind::Constant:
      value = expression.color_value;
      break;
    case LN_ColorExpressionKind::RuntimeTreeProperty:
      if (const LN_Value *property_value = GetTreePropertyValue(expression.property_ref_index)) {
        value = property_value->color_value;
        break;
      }
      ProfileRegisterExpressionFallback();
      return false;
    case LN_ColorExpressionKind::SnapshotObjectColor:
      if (expression.input0 != LN_INVALID_INDEX) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = m_snapshot.GetObjectSnapshot().object_color;
      break;
    case LN_ColorExpressionKind::SnapshotLightColor:
      if (expression.input0 != LN_INVALID_INDEX) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = m_snapshot.GetObjectSnapshot().light_color;
      break;
    case LN_ColorExpressionKind::Combine: {
      float r = expression.color_value.x();
      float g = expression.color_value.y();
      float b = expression.color_value.z();
      float a = expression.color_value.w();
      if (expression.input0 != LN_INVALID_INDEX &&
          !TryResolveRegisterFloatExpression(expression.input0, context, r))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      if (expression.input1 != LN_INVALID_INDEX &&
          !TryResolveRegisterFloatExpression(expression.input1, context, g))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      if (expression.input2 != LN_INVALID_INDEX &&
          !TryResolveRegisterFloatExpression(expression.input2, context, b))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      if (expression.input3 != LN_INVALID_INDEX &&
          !TryResolveRegisterFloatExpression(expression.input3, context, a))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = MT_Vector4(r, g, b, a);
      break;
    }
    case LN_ColorExpressionKind::FromGenericValue: {
      LN_Value generic_value;
      if (!TryResolveRegisterValueExpression(expression.input0, context, generic_value) ||
          !ValueAsColor(generic_value, value))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      break;
    }
    default:
      ProfileRegisterExpressionFallback();
      return false;
  }
  m_registerExpressionState.color_values[reg] = value;
  m_registerExpressionState.color_valid[reg] = m_registerExpressionState.active_generation;
  r_value = value;
  ProfileRegisterExpressionHit();
  return true;
}

bool LN_RuntimeTree::TryResolveRegisterStringExpression(const uint32_t expression_index,
                                                        const LN_TickContext &context,
                                                        std::string &r_value)
{
  if (!context.use_register_expression_evaluator || m_program == nullptr) {
    return false;
  }
  const LN_RegisterExpressionProgram &ir = m_program->GetRegisterExpressionProgram();
  if (!ir.valid ||
      (m_activeProfileCounters == nullptr && !m_registerExpressionIRHasProductionBenefit) ||
      expression_index >= ir.string_expression_registers.size() ||
      expression_index >= ir.string_expression_register_kinds.size())
  {
    return false;
  }

  EnsureRegisterExpressionState(context);

  const uint32_t reg = ir.string_expression_registers[expression_index];
  if (reg == LN_INVALID_INDEX) {
    ProfileRegisterExpressionFallback();
    return false;
  }

  const std::vector<LN_StringExpression> &expressions = m_program->GetStringExpressions();
  if (expression_index >= expressions.size()) {
    ProfileRegisterExpressionFallback();
    return false;
  }

  const LN_RegisterValueKind register_kind = ir.string_expression_register_kinds[expression_index];
  if (register_kind == LN_RegisterValueKind::StringId) {
    if (reg >= ir.string_id_register_count ||
        expressions[expression_index].kind != LN_StringExpressionKind::Constant)
    {
      ProfileRegisterExpressionFallback();
      return false;
    }
    r_value = expressions[expression_index].string_value;
    ProfileRegisterExpressionHit();
    return true;
  }

  if (register_kind != LN_RegisterValueKind::String ||
      reg >= m_registerExpressionState.string_values.size())
  {
    ProfileRegisterExpressionFallback();
    return false;
  }

  if (m_registerExpressionState.string_valid[reg] == m_registerExpressionState.active_generation) {
    r_value = m_registerExpressionState.string_values[reg];
    ProfileRegisterExpressionHit();
    return true;
  }

  const LN_StringExpression &expression = expressions[expression_index];
  switch (expression.kind) {
    case LN_StringExpressionKind::SnapshotGameProperty:
      if (const LN_Value *value = m_snapshot.GetGamePropertyValue(expression.property_ref_index)) {
        r_value = value->string_value;
        break;
      }
      ProfileRegisterExpressionFallback();
      return false;
    case LN_StringExpressionKind::RuntimeTreeProperty:
      if (const LN_Value *value = GetTreePropertyValue(expression.property_ref_index)) {
        r_value = value->string_value;
        break;
      }
      ProfileRegisterExpressionFallback();
      return false;
    case LN_StringExpressionKind::Join: {
      std::string left;
      std::string right;
      if (!TryResolveRegisterStringExpression(expression.input0, context, left) ||
          !TryResolveRegisterStringExpression(expression.input1, context, right))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      r_value = left + right;
      break;
    }
    case LN_StringExpressionKind::Replace: {
      std::string value;
      std::string search;
      std::string replacement;
      if (!TryResolveRegisterStringExpression(expression.input0, context, value) ||
          !TryResolveRegisterStringExpression(expression.input1, context, search) ||
          !TryResolveRegisterStringExpression(expression.input2, context, replacement))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      r_value = ReplaceStringOccurrences(value, search, replacement);
      break;
    }
    case LN_StringExpressionKind::ToUppercase: {
      std::string value;
      if (!TryResolveRegisterStringExpression(expression.input0, context, value)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      r_value = ToCaseString(value, true);
      break;
    }
    case LN_StringExpressionKind::ToLowercase: {
      std::string value;
      if (!TryResolveRegisterStringExpression(expression.input0, context, value)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      r_value = ToCaseString(value, false);
      break;
    }
    case LN_StringExpressionKind::ZeroFill: {
      std::string value;
      int32_t width = 0;
      if (!TryResolveRegisterStringExpression(expression.input0, context, value) ||
          !TryResolveRegisterIntExpression(expression.int_expr_index, context, width))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      r_value = ZeroFillString(value, width);
      break;
    }
    case LN_StringExpressionKind::Format: {
      std::string format;
      std::string input1;
      std::string input2;
      std::string input3;
      std::string input4;
      if (!TryResolveRegisterStringExpression(expression.input0, context, format) ||
          !TryResolveRegisterStringExpression(expression.input1, context, input1) ||
          !TryResolveRegisterStringExpression(expression.input2, context, input2) ||
          !TryResolveRegisterStringExpression(expression.input3, context, input3) ||
          !TryResolveRegisterStringExpression(expression.input4, context, input4))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      r_value = FormatStringSlots(format, input1, input2, input3, input4);
      break;
    }
    case LN_StringExpressionKind::FromGenericValue: {
      LN_Value value;
      if (!TryResolveRegisterValueExpression(expression.value_expr_index, context, value)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      r_value = ValueToText(value);
      break;
    }
    case LN_StringExpressionKind::FromBool: {
      bool value = false;
      if (!TryResolveRegisterBoolExpression(expression.input0, context, value)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      r_value = value ? "True" : "False";
      break;
    }
    case LN_StringExpressionKind::FromInt: {
      int32_t value = 0;
      if (!TryResolveRegisterIntExpression(expression.input0, context, value)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      r_value = std::to_string(value);
      break;
    }
    case LN_StringExpressionKind::FromFloat: {
      float value = 0.0f;
      if (!TryResolveRegisterFloatExpression(expression.input0, context, value)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      r_value = LN_TypecastFloatToString(value);
      break;
    }
    case LN_StringExpressionKind::FromVector: {
      MT_Vector3 value(0.0f, 0.0f, 0.0f);
      if (!TryResolveRegisterVectorExpression(expression.input0, context, value)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      r_value = LN_TypecastVectorToString(value);
      break;
    }
    case LN_StringExpressionKind::FromColor: {
      MT_Vector4 value(0.0f, 0.0f, 0.0f, 1.0f);
      if (!TryResolveRegisterColorExpression(expression.input0, context, value)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      r_value = LN_TypecastColorToString(value);
      break;
    }
    default:
      ProfileRegisterExpressionFallback();
      return false;
  }

  m_registerExpressionState.string_values[reg] = r_value;
  m_registerExpressionState.string_valid[reg] = m_registerExpressionState.active_generation;
  ProfileRegisterExpressionHit();
  return true;
}

bool LN_RuntimeTree::TryResolveRegisterValueExpression(const uint32_t expression_index,
                                                       const LN_TickContext &context,
                                                       LN_Value &r_value)
{
  if (!context.use_register_expression_evaluator || m_program == nullptr) {
    return false;
  }
  const LN_RegisterExpressionProgram &ir = m_program->GetRegisterExpressionProgram();
  if (!ir.valid ||
      (m_activeProfileCounters == nullptr && !m_registerExpressionIRHasProductionBenefit) ||
      expression_index >= ir.value_expression_registers.size())
  {
    return false;
  }
  EnsureRegisterExpressionState(context);
  const uint32_t reg = ir.value_expression_registers[expression_index];
  if (reg == LN_INVALID_INDEX || reg >= m_registerExpressionState.generic_values.size()) {
    ProfileRegisterExpressionFallback();
    return false;
  }
  if (m_registerExpressionState.generic_valid[reg] == m_registerExpressionState.active_generation)
  {
    r_value = m_registerExpressionState.generic_values[reg];
    ProfileRegisterExpressionHit();
    return true;
  }
  const std::vector<LN_ValueExpression> &expressions = m_program->GetValueExpressions();
  if (expression_index >= expressions.size()) {
    return false;
  }
  const LN_ValueExpression &expression = expressions[expression_index];
  switch (expression.kind) {
    case LN_ValueExpressionKind::Constant:
      r_value = expression.value;
      break;
    case LN_ValueExpressionKind::SnapshotGameProperty:
      if (const LN_Value *value = m_snapshot.GetGamePropertyValue(expression.property_ref_index)) {
        r_value = *value;
        break;
      }
      ProfileRegisterExpressionFallback();
      return false;
    case LN_ValueExpressionKind::RuntimeTreeProperty:
      if (const LN_Value *value = GetTreePropertyValue(expression.property_ref_index)) {
        r_value = *value;
        break;
      }
      ProfileRegisterExpressionFallback();
      return false;
    case LN_ValueExpressionKind::EmptyDict:
      r_value = LN_Value();
      r_value.type = LN_ValueType::Dict;
      r_value.exists = true;
      break;
    case LN_ValueExpressionKind::EmptyList: {
      int32_t length = 0;
      if (expression.input0 != LN_INVALID_INDEX &&
          !TryResolveRegisterIntExpression(expression.input0, context, length))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      r_value = LN_Value();
      r_value.type = LN_ValueType::List;
      r_value.exists = true;
      if (length > 0) {
        r_value.list_value.reserve(size_t(length));
        for (int32_t index = 0; index < length; index++) {
          r_value.list_value.push_back(MakeNoneValue());
        }
      }
      break;
    }
    case LN_ValueExpressionKind::Select: {
      bool condition = false;
      if (!TryResolveRegisterBoolExpression(expression.bool_expr_index, context, condition)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      if (!TryResolveRegisterValueExpression(
              condition ? expression.input0 : expression.input1, context, r_value))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      break;
    }
    case LN_ValueExpressionKind::Mix: {
      LN_Value a;
      LN_Value b;
      float factor = 0.0f;
      if (!TryResolveRegisterValueExpression(expression.input0, context, a) ||
          !TryResolveRegisterValueExpression(expression.input1, context, b) ||
          !TryResolveRegisterFloatExpression(expression.input2, context, factor))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      r_value = ln::value_interpolation::Mix(a, b, factor);
      break;
    }
    case LN_ValueExpressionKind::FromBool: {
      bool value = false;
      if (!TryResolveRegisterBoolExpression(expression.input0, context, value)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      r_value = LN_Value();
      r_value.type = LN_ValueType::Bool;
      r_value.exists = true;
      r_value.bool_value = value;
      break;
    }
    case LN_ValueExpressionKind::FromInt: {
      int32_t value = 0;
      if (!TryResolveRegisterIntExpression(expression.input0, context, value)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      r_value = LN_Value();
      r_value.type = LN_ValueType::Int;
      r_value.exists = true;
      r_value.int_value = value;
      break;
    }
    case LN_ValueExpressionKind::FromFloat: {
      float value = 0.0f;
      if (!TryResolveRegisterFloatExpression(expression.input0, context, value)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      r_value = LN_Value();
      r_value.type = LN_ValueType::Float;
      r_value.exists = true;
      r_value.float_value = value;
      break;
    }
    case LN_ValueExpressionKind::FromString: {
      std::string value;
      if (!TryResolveRegisterStringExpression(expression.input0, context, value)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      r_value = LN_Value();
      r_value.type = LN_ValueType::String;
      r_value.exists = true;
      r_value.string_value = value;
      break;
    }
    case LN_ValueExpressionKind::FromVector: {
      MT_Vector3 value(0.0f, 0.0f, 0.0f);
      if (!TryResolveRegisterVectorExpression(expression.input0, context, value)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      r_value = LN_Value();
      r_value.type = LN_ValueType::Vector;
      r_value.exists = true;
      r_value.vector_value = value;
      break;
    }
    case LN_ValueExpressionKind::FromColor: {
      MT_Vector4 value(0.0f, 0.0f, 0.0f, 1.0f);
      if (!TryResolveRegisterColorExpression(expression.input0, context, value)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      r_value = LN_Value();
      r_value.type = LN_ValueType::Color;
      r_value.exists = true;
      r_value.color_value = value;
      break;
    }
    case LN_ValueExpressionKind::FromRotation: {
      r_value = LN_Value();
      r_value.type = LN_ValueType::Rotation;
      r_value.exists = true;
      const LN_Rotation rotation = ResolveRotationExpression(
          expression.input0, LN_Rotation::Identity(), context);
      r_value.SetRotation(
          rotation, ResolveVectorExpression(expression.input0, rotation.ToEulerXYZ(), context));
      break;
    }
    case LN_ValueExpressionKind::CombineVector4: {
      if (expression.input_indices.size() < 4) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      float x = 0.0f;
      float y = 0.0f;
      float z = 0.0f;
      float w = 0.0f;
      if (!TryResolveRegisterFloatExpression(expression.input_indices[0], context, x) ||
          !TryResolveRegisterFloatExpression(expression.input_indices[1], context, y) ||
          !TryResolveRegisterFloatExpression(expression.input_indices[2], context, z) ||
          !TryResolveRegisterFloatExpression(expression.input_indices[3], context, w))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      r_value = LN_Value();
      r_value.type = LN_ValueType::Vector4;
      r_value.exists = true;
      r_value.vector4_value = MT_Vector4(x, y, z, w);
      break;
    }
    case LN_ValueExpressionKind::ResizeVectorValue: {
      LN_Value input;
      if (!TryResolveRegisterValueExpression(expression.input0, context, input)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      MT_Vector4 vector;
      if (!ValueAsVector4(input, vector)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      const int32_t target_size = std::clamp(int32_t(expression.property_ref_index), 2, 4);
      r_value = LN_Value();
      r_value.exists = true;
      if (target_size == 4) {
        r_value.type = LN_ValueType::Vector4;
        r_value.vector4_value = vector;
      }
      else {
        r_value.type = LN_ValueType::Vector;
        r_value.vector_value = MT_Vector3(
            vector.x(), vector.y(), target_size >= 3 ? vector.z() : 0.0f);
      }
      break;
    }
    case LN_ValueExpressionKind::EulerToMatrix: {
      MT_Vector3 euler(0.0f, 0.0f, 0.0f);
      if (!TryResolveRegisterVectorExpression(expression.input0, context, euler)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      r_value = LN_Value();
      r_value.type = LN_ValueType::Matrix;
      r_value.exists = true;
      r_value.matrix_value = MatrixFromEuler(euler, expression.property_ref_index);
      break;
    }
    case LN_ValueExpressionKind::MatrixToEuler: {
      LN_Value input;
      if (!TryResolveRegisterValueExpression(expression.input0, context, input)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      MT_Matrix3x3 matrix;
      if (!ValueAsMatrix(input, matrix)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      const MT_Vector3 euler = EulerFromMatrix(matrix, expression.property_ref_index);
      r_value = LN_Value();
      r_value.type = expression.value.type == LN_ValueType::Rotation ? LN_ValueType::Rotation :
                                                                       LN_ValueType::Vector;
      r_value.exists = true;
      if (r_value.type == LN_ValueType::Rotation) {
        r_value.SetRotation(LN_Rotation::FromMatrix(matrix), euler);
      }
      else {
        r_value.vector_value = euler;
      }
      break;
    }
    case LN_ValueExpressionKind::MakeList:
      r_value = LN_Value();
      r_value.type = LN_ValueType::List;
      r_value.exists = true;
      if (expression.input0 != LN_INVALID_INDEX) {
        LN_Value item;
        if (!TryResolveRegisterValueExpression(expression.input0, context, item)) {
          ProfileRegisterExpressionFallback();
          return false;
        }
        r_value.list_value.push_back(item);
      }
      if (expression.input1 != LN_INVALID_INDEX) {
        LN_Value item;
        if (!TryResolveRegisterValueExpression(expression.input1, context, item)) {
          ProfileRegisterExpressionFallback();
          return false;
        }
        r_value.list_value.push_back(item);
      }
      if (expression.input2 != LN_INVALID_INDEX) {
        LN_Value item;
        if (!TryResolveRegisterValueExpression(expression.input2, context, item)) {
          ProfileRegisterExpressionFallback();
          return false;
        }
        r_value.list_value.push_back(item);
      }
      break;
    case LN_ValueExpressionKind::ListFromItems:
      r_value = LN_Value();
      r_value.type = LN_ValueType::List;
      r_value.exists = true;
      r_value.list_value.reserve(expression.input_indices.size());
      for (const uint32_t input_index : expression.input_indices) {
        if (input_index == LN_INVALID_INDEX) {
          continue;
        }
        LN_Value item;
        if (!TryResolveRegisterValueExpression(input_index, context, item)) {
          ProfileRegisterExpressionFallback();
          return false;
        }
        r_value.list_value.push_back(item);
      }
      break;
    case LN_ValueExpressionKind::ListDuplicate:
      r_value = LN_Value();
      r_value.type = LN_ValueType::List;
      r_value.exists = true;
      if (expression.input0 != LN_INVALID_INDEX) {
        LN_Value source;
        if (!TryResolveRegisterValueExpression(expression.input0, context, source)) {
          ProfileRegisterExpressionFallback();
          return false;
        }
        if (source.type == LN_ValueType::List && source.exists) {
          r_value.list_value = source.list_value;
        }
      }
      break;
    case LN_ValueExpressionKind::ListExtend:
      r_value = LN_Value();
      r_value.type = LN_ValueType::List;
      r_value.exists = true;
      if (expression.input0 != LN_INVALID_INDEX) {
        LN_Value list_a;
        if (!TryResolveRegisterValueExpression(expression.input0, context, list_a)) {
          ProfileRegisterExpressionFallback();
          return false;
        }
        if (list_a.type == LN_ValueType::List && list_a.exists) {
          r_value.list_value.insert(
              r_value.list_value.end(), list_a.list_value.begin(), list_a.list_value.end());
        }
      }
      if (expression.input1 != LN_INVALID_INDEX) {
        LN_Value list_b;
        if (!TryResolveRegisterValueExpression(expression.input1, context, list_b)) {
          ProfileRegisterExpressionFallback();
          return false;
        }
        if (list_b.type == LN_ValueType::List && list_b.exists) {
          r_value.list_value.insert(
              r_value.list_value.end(), list_b.list_value.begin(), list_b.list_value.end());
        }
      }
      break;
    case LN_ValueExpressionKind::ListAppend:
      r_value = LN_Value();
      r_value.type = LN_ValueType::List;
      r_value.exists = true;
      if (expression.input0 != LN_INVALID_INDEX) {
        LN_Value list_value;
        if (!TryResolveRegisterValueExpression(expression.input0, context, list_value)) {
          ProfileRegisterExpressionFallback();
          return false;
        }
        if (list_value.type == LN_ValueType::List && list_value.exists) {
          r_value.list_value = list_value.list_value;
        }
      }
      if (expression.input1 != LN_INVALID_INDEX) {
        LN_Value item;
        if (!TryResolveRegisterValueExpression(expression.input1, context, item)) {
          ProfileRegisterExpressionFallback();
          return false;
        }
        r_value.list_value.push_back(item);
      }
      break;
    case LN_ValueExpressionKind::ListRemoveIndex: {
      r_value = LN_Value();
      r_value.type = LN_ValueType::List;
      r_value.exists = true;
      if (expression.input0 != LN_INVALID_INDEX) {
        LN_Value list_value;
        if (!TryResolveRegisterValueExpression(expression.input0, context, list_value)) {
          ProfileRegisterExpressionFallback();
          return false;
        }
        if (list_value.type == LN_ValueType::List && list_value.exists) {
          r_value.list_value = list_value.list_value;
        }
      }
      int32_t index = 0;
      if (expression.input1 != LN_INVALID_INDEX &&
          !TryResolveRegisterIntExpression(expression.input1, context, index))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      size_t normalized_index = 0;
      if (NormalizeListIndex(index, r_value.list_value.size(), normalized_index)) {
        r_value.list_value.erase(r_value.list_value.begin() + normalized_index);
      }
      break;
    }
    case LN_ValueExpressionKind::ListRemoveValue: {
      r_value = LN_Value();
      r_value.type = LN_ValueType::List;
      r_value.exists = true;
      if (expression.input0 != LN_INVALID_INDEX) {
        LN_Value list_value;
        if (!TryResolveRegisterValueExpression(expression.input0, context, list_value)) {
          ProfileRegisterExpressionFallback();
          return false;
        }
        if (list_value.type == LN_ValueType::List && list_value.exists) {
          r_value.list_value = list_value.list_value;
        }
      }
      LN_Value needle = MakeNoneValue();
      if (expression.input1 != LN_INVALID_INDEX &&
          !TryResolveRegisterValueExpression(expression.input1, context, needle))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      const auto iter = std::find_if(
          r_value.list_value.begin(), r_value.list_value.end(), [&](const LN_Value &item) {
            return ValuesEqual(item, needle);
          });
      if (iter != r_value.list_value.end()) {
        r_value.list_value.erase(iter);
      }
      break;
    }
    case LN_ValueExpressionKind::ListSetIndex: {
      r_value = LN_Value();
      r_value.type = LN_ValueType::List;
      r_value.exists = true;
      if (expression.input0 != LN_INVALID_INDEX) {
        LN_Value list_value;
        if (!TryResolveRegisterValueExpression(expression.input0, context, list_value)) {
          ProfileRegisterExpressionFallback();
          return false;
        }
        if (list_value.type == LN_ValueType::List && list_value.exists) {
          r_value.list_value = list_value.list_value;
        }
      }
      int32_t index = 0;
      if (expression.input1 != LN_INVALID_INDEX &&
          !TryResolveRegisterIntExpression(expression.input1, context, index))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      size_t normalized_index = 0;
      if (NormalizeListIndex(index, r_value.list_value.size(), normalized_index)) {
        LN_Value replacement = MakeNoneValue();
        if (expression.input2 != LN_INVALID_INDEX &&
            !TryResolveRegisterValueExpression(expression.input2, context, replacement))
        {
          ProfileRegisterExpressionFallback();
          return false;
        }
        r_value.list_value[normalized_index] = replacement;
      }
      break;
    }
    case LN_ValueExpressionKind::ListElement: {
      LN_Value list_value;
      if (!TryResolveRegisterValueExpression(expression.input0, context, list_value)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      if (list_value.type != LN_ValueType::List || !list_value.exists) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      int32_t index = 0;
      if (!TryResolveRegisterIntExpression(expression.input1, context, index)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      if (index < 0 || size_t(index) >= list_value.list_value.size()) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      r_value = list_value.list_value[size_t(index)];
      break;
    }
    case LN_ValueExpressionKind::ListRandomItem: {
      LN_Value list_value;
      if (!TryResolveRegisterValueExpression(expression.input0, context, list_value)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      if (list_value.type != LN_ValueType::List || !list_value.exists ||
          list_value.list_value.empty())
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      const uint64_t seed = (uint64_t(expression_index) << 32) ^ context.tick_index;
      const size_t index = std::min(size_t(HashToUnitFloat(seed) * list_value.list_value.size()),
                                    list_value.list_value.size() - 1);
      r_value = list_value.list_value[index];
      break;
    }
    case LN_ValueExpressionKind::ValueSwitchList: {
      const size_t pair_count = expression.input_indices.size() / 2;
      for (size_t pair_index = 0; pair_index < pair_count; pair_index++) {
        const uint32_t condition_index = expression.input_indices[pair_index * 2];
        const uint32_t value_index = expression.input_indices[pair_index * 2 + 1];
        if (condition_index == LN_INVALID_INDEX) {
          continue;
        }
        bool condition = false;
        if (!TryResolveRegisterBoolExpression(condition_index, context, condition)) {
          ProfileRegisterExpressionFallback();
          return false;
        }
        if (condition) {
          if (value_index == LN_INVALID_INDEX) {
            r_value = MakeNoneValue();
            break;
          }
          if (!TryResolveRegisterValueExpression(value_index, context, r_value)) {
            ProfileRegisterExpressionFallback();
            return false;
          }
          break;
        }
      }
      if (pair_count == 0 || !r_value.exists) {
        r_value = MakeNoneValue();
      }
      break;
    }
    case LN_ValueExpressionKind::ValueSwitchListCompare: {
      LN_Value switch_value = MakeNoneValue();
      if (expression.input0 != LN_INVALID_INDEX &&
          !TryResolveRegisterValueExpression(expression.input0, context, switch_value))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      const size_t pair_count = expression.input_indices.size() / 2;
      for (size_t pair_index = 0; pair_index < pair_count; pair_index++) {
        const uint32_t case_index = expression.input_indices[pair_index * 2];
        const uint32_t value_index = expression.input_indices[pair_index * 2 + 1];
        LN_Value case_value = MakeNoneValue();
        if (case_index != LN_INVALID_INDEX &&
            !TryResolveRegisterValueExpression(case_index, context, case_value))
        {
          ProfileRegisterExpressionFallback();
          return false;
        }
        if (CompareValues(switch_value,
                          case_value,
                          expression.value.exists ?
                              static_cast<LN_FloatCompareOperation>(expression.value.int_value) :
                              LN_FloatCompareOperation::Equal))
        {
          if (value_index == LN_INVALID_INDEX) {
            r_value = MakeNoneValue();
            break;
          }
          if (!TryResolveRegisterValueExpression(value_index, context, r_value)) {
            ProfileRegisterExpressionFallback();
            return false;
          }
          break;
        }
      }
      if (!r_value.exists) {
        if (expression.input1 == LN_INVALID_INDEX) {
          r_value = MakeNoneValue();
        }
        else if (!TryResolveRegisterValueExpression(expression.input1, context, r_value)) {
          ProfileRegisterExpressionFallback();
          return false;
        }
      }
      break;
    }
    case LN_ValueExpressionKind::MakeDict: {
      r_value = LN_Value();
      r_value.type = LN_ValueType::Dict;
      r_value.exists = true;
      const LN_StringId key_id = ResolveStringExpressionId(expression.input0);
      const std::string key = key_id.IsValid() ? m_program->GetString(key_id) : std::string();
      if (!key.empty()) {
        LN_Value item = MakeNoneValue();
        if (expression.input1 != LN_INVALID_INDEX &&
            !TryResolveRegisterValueExpression(expression.input1, context, item))
        {
          ProfileRegisterExpressionFallback();
          return false;
        }
        r_value.dict_value[key] = item;
      }
      break;
    }
    case LN_ValueExpressionKind::DictGetKey: {
      LN_Value dict_value = MakeNoneValue();
      if (expression.input0 != LN_INVALID_INDEX &&
          !TryResolveRegisterValueExpression(expression.input0, context, dict_value))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      LN_Value default_value = MakeNoneValue();
      if (expression.input2 != LN_INVALID_INDEX &&
          !TryResolveRegisterValueExpression(expression.input2, context, default_value))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      const LN_StringId key_id = ResolveStringExpressionId(expression.input1);
      const std::string key = key_id.IsValid() ? m_program->GetString(key_id) : std::string();
      if (dict_value.type == LN_ValueType::Dict && dict_value.exists && !key.empty()) {
        const auto iter = dict_value.dict_value.find(key);
        if (iter != dict_value.dict_value.end()) {
          r_value = iter->second;
          break;
        }
      }
      r_value = default_value;
      break;
    }
    case LN_ValueExpressionKind::DictSetKey: {
      r_value = LN_Value();
      r_value.type = LN_ValueType::Dict;
      r_value.exists = true;
      if (expression.input0 != LN_INVALID_INDEX) {
        LN_Value dict_value;
        if (!TryResolveRegisterValueExpression(expression.input0, context, dict_value)) {
          ProfileRegisterExpressionFallback();
          return false;
        }
        if (dict_value.type == LN_ValueType::Dict && dict_value.exists) {
          r_value.dict_value = dict_value.dict_value;
        }
      }
      const LN_StringId key_id = ResolveStringExpressionId(expression.input1);
      const std::string key = key_id.IsValid() ? m_program->GetString(key_id) : std::string();
      if (!key.empty()) {
        LN_Value item = MakeNoneValue();
        if (expression.input2 != LN_INVALID_INDEX &&
            !TryResolveRegisterValueExpression(expression.input2, context, item))
        {
          ProfileRegisterExpressionFallback();
          return false;
        }
        r_value.dict_value[key] = item;
      }
      break;
    }
    case LN_ValueExpressionKind::DictRemoveKey:
    case LN_ValueExpressionKind::DictRemoveKeyValue: {
      LN_Value dict_output;
      dict_output.type = LN_ValueType::Dict;
      dict_output.exists = true;
      LN_Value removed_value = MakeNoneValue();
      if (expression.input0 != LN_INVALID_INDEX) {
        LN_Value dict_value;
        if (!TryResolveRegisterValueExpression(expression.input0, context, dict_value)) {
          ProfileRegisterExpressionFallback();
          return false;
        }
        if (dict_value.type == LN_ValueType::Dict && dict_value.exists) {
          dict_output.dict_value = dict_value.dict_value;
        }
      }
      const LN_StringId key_id = ResolveStringExpressionId(expression.input1);
      const std::string key = key_id.IsValid() ? m_program->GetString(key_id) : std::string();
      if (!key.empty()) {
        const auto iter = dict_output.dict_value.find(key);
        if (iter != dict_output.dict_value.end()) {
          removed_value = iter->second;
          dict_output.dict_value.erase(iter);
        }
      }
      r_value = expression.kind == LN_ValueExpressionKind::DictRemoveKeyValue ? removed_value :
                                                                                dict_output;
      break;
    }
    case LN_ValueExpressionKind::DictMerge:
      r_value = LN_Value();
      r_value.type = LN_ValueType::Dict;
      r_value.exists = true;
      if (expression.input0 != LN_INVALID_INDEX) {
        LN_Value dict_a;
        if (!TryResolveRegisterValueExpression(expression.input0, context, dict_a)) {
          ProfileRegisterExpressionFallback();
          return false;
        }
        if (dict_a.type == LN_ValueType::Dict && dict_a.exists) {
          r_value.dict_value = dict_a.dict_value;
        }
      }
      if (expression.input1 != LN_INVALID_INDEX) {
        LN_Value dict_b;
        if (!TryResolveRegisterValueExpression(expression.input1, context, dict_b)) {
          ProfileRegisterExpressionFallback();
          return false;
        }
        if (dict_b.type == LN_ValueType::Dict && dict_b.exists) {
          for (const auto &entry : dict_b.dict_value) {
            r_value.dict_value[entry.first] = entry.second;
          }
        }
      }
      break;
    case LN_ValueExpressionKind::DictGetKeys:
      r_value = LN_Value();
      r_value.type = LN_ValueType::List;
      r_value.exists = true;
      if (expression.input0 != LN_INVALID_INDEX) {
        LN_Value dict_value;
        if (!TryResolveRegisterValueExpression(expression.input0, context, dict_value)) {
          ProfileRegisterExpressionFallback();
          return false;
        }
        if (dict_value.type == LN_ValueType::Dict && dict_value.exists) {
          for (const auto &entry : dict_value.dict_value) {
            LN_Value key_value;
            key_value.type = LN_ValueType::String;
            key_value.exists = true;
            key_value.string_value = entry.first;
            r_value.list_value.push_back(key_value);
          }
        }
      }
      break;
    default:
      ProfileRegisterExpressionFallback();
      return false;
  }
  m_registerExpressionState.generic_values[reg] = r_value;
  m_registerExpressionState.generic_valid[reg] = m_registerExpressionState.active_generation;
  ProfileRegisterExpressionHit();
  return true;
}

bool LN_RuntimeTree::EvaluateMouseMoved(const bool each_logic_tick) const
{
  const LN_InputSnapshot &input = m_snapshot.GetInputSnapshot();
  const bool activated = input.GetStatus(SCA_IInputDevice::MOUSEX,
                                         SCA_InputEvent::JUSTACTIVATED) ||
                         input.GetStatus(SCA_IInputDevice::MOUSEY, SCA_InputEvent::JUSTACTIVATED);
  if (!each_logic_tick) {
    return activated;
  }
  return activated || input.GetStatus(SCA_IInputDevice::MOUSEX, SCA_InputEvent::ACTIVE) ||
         input.GetStatus(SCA_IInputDevice::MOUSEY, SCA_InputEvent::ACTIVE);
}

bool LN_RuntimeTree::TryResolveRegisterBoolExpression(const uint32_t expression_index,
                                                      const LN_TickContext &context,
                                                      bool &r_value)
{
  if (!context.use_register_expression_evaluator || m_program == nullptr) {
    return false;
  }
  const LN_RegisterExpressionProgram &ir = m_program->GetRegisterExpressionProgram();
  if (!ir.valid ||
      (m_activeProfileCounters == nullptr && !m_registerExpressionIRHasProductionBenefit) ||
      expression_index >= ir.bool_expression_registers.size())
  {
    return false;
  }
  EnsureRegisterExpressionState(context);
  const uint32_t reg = ir.bool_expression_registers[expression_index];
  if (reg == LN_INVALID_INDEX || reg >= m_registerExpressionState.bool_values.size()) {
    ProfileRegisterExpressionFallback();
    return false;
  }
  if (m_registerExpressionState.bool_valid[reg] == m_registerExpressionState.active_generation) {
    r_value = m_registerExpressionState.bool_values[reg];
    ProfileRegisterExpressionHit();
    return true;
  }
  const std::vector<LN_BoolExpression> &expressions = m_program->GetBoolExpressions();
  if (expression_index >= expressions.size()) {
    return false;
  }
  const LN_BoolExpression &expression = expressions[expression_index];
  bool value = false;
  switch (expression.kind) {
    case LN_BoolExpressionKind::Constant:
      value = expression.bool_value;
      break;
    case LN_BoolExpressionKind::GamePaused:
      value = context.game_paused;
      break;
    case LN_BoolExpressionKind::RuntimeTreeProperty:
      if (const LN_Value *property_value = GetTreePropertyValue(expression.property_ref_index)) {
        value = property_value->bool_value;
        break;
      }
      ProfileRegisterExpressionFallback();
      return false;
    case LN_BoolExpressionKind::SnapshotGameProperty:
      if (const LN_Value *property_value = m_snapshot.GetGamePropertyValue(
              expression.property_ref_index))
      {
        if (property_value->exists) {
          value = property_value->bool_value;
          break;
        }
      }
      ProfileRegisterExpressionFallback();
      return false;
    case LN_BoolExpressionKind::SnapshotGamePropertyExists:
      if (const LN_Value *property_value = m_snapshot.GetGamePropertyValue(
              expression.property_ref_index))
      {
        value = property_value->exists;
      }
      else {
        value = false;
      }
      break;
    case LN_BoolExpressionKind::SnapshotVisibility:
      if (expression.input0 != LN_INVALID_INDEX) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = m_snapshot.GetObjectSnapshot().visible;
      break;
    case LN_BoolExpressionKind::SnapshotCharacterOnGround: {
      if (expression.input0 != LN_INVALID_INDEX) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      const LN_ObjectSnapshot &object_snapshot = m_snapshot.GetObjectSnapshot();
      if (!object_snapshot.has_character) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = object_snapshot.character_on_ground;
      break;
    }
    case LN_BoolExpressionKind::InputStatus:
      if (expression.int_value == SCA_IInputDevice::NOKEY) {
        value = false;
      }
      else {
        value = (expression.bool_value ? m_snapshot.GetRawInputSnapshot() :
                                         m_snapshot.GetInputSnapshot())
                    .GetStatus(expression.int_value, expression.secondary_int_value);
      }
      break;
    case LN_BoolExpressionKind::KeyboardActive:
      value = KeyboardInputIsActive(m_snapshot.GetInputSnapshot());
      break;
    case LN_BoolExpressionKind::WindowFullscreen: {
      const LN_InputSnapshot &input = m_snapshot.GetInputSnapshot();
      if (!input.HasCanvas()) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = input.GetCanvasFullscreen();
      break;
    }
    case LN_BoolExpressionKind::MouseMoved: {
      value = EvaluateMouseMoved(expression.bool_value);
      break;
    }
    case LN_BoolExpressionKind::MouseWheelMoved: {
      const int32_t wheel_delta = (expression.bool_value ? m_snapshot.GetRawInputSnapshot() :
                                                           m_snapshot.GetInputSnapshot())
                                      .GetMouse()
                                      .wheel_delta;
      if (wheel_delta == 0) {
        value = false;
        break;
      }
      const int32_t direction = expression.int_value > 0 ? expression.int_value : 3;
      if (direction == 1) {
        value = wheel_delta > 0;
      }
      else if (direction == 2) {
        value = wheel_delta < 0;
      }
      else {
        value = true;
      }
      break;
    }
    case LN_BoolExpressionKind::KeyLoggerPressed:
      value = ScanKeyLogger(m_snapshot.GetInputSnapshot(), expression.bool_value).pressed;
      break;
    case LN_BoolExpressionKind::GamepadActive: {
      int32_t gamepad_index = 0;
      if (!TryResolveRegisterIntExpression(expression.input0, context, gamepad_index)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      const LN_GamepadSnapshot *gamepad = m_snapshot.GetInputSnapshot().GetGamepad(gamepad_index);
      if (gamepad == nullptr) {
        value = false;
        break;
      }
      value = LN_GamepadHasActiveInput(*gamepad);
      break;
    }
    case LN_BoolExpressionKind::GamepadButton: {
      int32_t gamepad_index = 0;
      if (!TryResolveRegisterIntExpression(expression.input0, context, gamepad_index)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      const LN_GamepadSnapshot *gamepad = (expression.secondary_int_value != 0 ?
                                               m_snapshot.GetRawInputSnapshot() :
                                               m_snapshot.GetInputSnapshot())
                                              .GetGamepad(gamepad_index);
      if (gamepad == nullptr) {
        value = false;
        break;
      }
      int32_t input_status = SCA_InputEvent::ACTIVE;
      if (expression.int_expr_index != LN_INVALID_INDEX &&
          !TryResolveRegisterIntExpression(expression.int_expr_index, context, input_status))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = LN_GetGamepadControlState(
          *gamepad, expression.int_value, expression.bool_value, input_status);
      break;
    }
    case LN_BoolExpressionKind::Not:
      if (!TryResolveRegisterBoolExpression(expression.input0, context, value)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = !value;
      break;
    case LN_BoolExpressionKind::And: {
      bool a = false;
      bool b = false;
      if (!TryResolveRegisterBoolExpression(expression.input0, context, a) ||
          !TryResolveRegisterBoolExpression(expression.input1, context, b))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = a && b;
      break;
    }
    case LN_BoolExpressionKind::Or: {
      bool a = false;
      bool b = false;
      if (!TryResolveRegisterBoolExpression(expression.input0, context, a) ||
          !TryResolveRegisterBoolExpression(expression.input1, context, b))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = a || b;
      break;
    }
    case LN_BoolExpressionKind::FloatCompare: {
      float a = 0.0f;
      float b = 0.0f;
      if (!TryResolveRegisterFloatExpression(expression.input0, context, a) ||
          !TryResolveRegisterFloatExpression(expression.input1, context, b))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      switch (expression.float_compare_operation) {
        case LN_FloatCompareOperation::Equal:
          value = std::fabs(a - b) <= 1.0e-6f;
          break;
        case LN_FloatCompareOperation::NotEqual:
          value = std::fabs(a - b) > 1.0e-6f;
          break;
        case LN_FloatCompareOperation::GreaterThan:
          value = a > b;
          break;
        case LN_FloatCompareOperation::LessThan:
          value = a < b;
          break;
        case LN_FloatCompareOperation::GreaterEqual:
          value = a >= b;
          break;
        case LN_FloatCompareOperation::LessEqual:
          value = a <= b;
          break;
      }
      break;
    }
    case LN_BoolExpressionKind::StringContains:
    case LN_BoolExpressionKind::StringStartsWith:
    case LN_BoolExpressionKind::StringEndsWith: {
      std::string text;
      std::string needle;
      if (!TryResolveRegisterStringExpression(expression.input0, context, text) ||
          !TryResolveRegisterStringExpression(expression.input1, context, needle))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      if (expression.kind == LN_BoolExpressionKind::StringContains) {
        value = text.find(needle) != std::string::npos;
      }
      else if (expression.kind == LN_BoolExpressionKind::StringStartsWith) {
        value = StringStartsWith(text, needle);
      }
      else {
        value = StringEndsWith(text, needle);
      }
      break;
    }
    case LN_BoolExpressionKind::ValueIsNone: {
      LN_Value generic_value;
      if (!TryResolveRegisterValueExpression(expression.input0, context, generic_value)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = !generic_value.exists || generic_value.type == LN_ValueType::None;
      break;
    }
    case LN_BoolExpressionKind::FromGenericValue: {
      LN_Value generic_value;
      if (!TryResolveRegisterValueExpression(expression.input0, context, generic_value)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = ValueAsBool(generic_value);
      break;
    }
    case LN_BoolExpressionKind::FromInt: {
      int32_t source = 0;
      if (!TryResolveRegisterIntExpression(expression.input0, context, source)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = source != 0;
      break;
    }
    case LN_BoolExpressionKind::FromFloat: {
      float source = 0.0f;
      if (!TryResolveRegisterFloatExpression(expression.input0, context, source)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = source != 0.0f;
      break;
    }
    case LN_BoolExpressionKind::FromString: {
      std::string source;
      if (!TryResolveRegisterStringExpression(expression.input0, context, source)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = !source.empty();
      break;
    }
    case LN_BoolExpressionKind::FromVector: {
      MT_Vector3 source(0.0f, 0.0f, 0.0f);
      if (!TryResolveRegisterVectorExpression(expression.input0, context, source)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = source.length2() > 0.0f;
      break;
    }
    case LN_BoolExpressionKind::FromColor: {
      MT_Vector4 source(0.0f, 0.0f, 0.0f, 0.0f);
      if (!TryResolveRegisterColorExpression(expression.input0, context, source)) {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = source.length2() > 0.0f;
      break;
    }
    case LN_BoolExpressionKind::ValueCompare: {
      LN_Value a;
      LN_Value b;
      if (!TryResolveRegisterValueExpression(expression.input0, context, a) ||
          !TryResolveRegisterValueExpression(expression.input1, context, b))
      {
        ProfileRegisterExpressionFallback();
        return false;
      }
      value = CompareValues(a, b, expression.float_compare_operation);
      break;
    }
    case LN_BoolExpressionKind::GameLoadPulse:
      value = m_logicManager != nullptr && m_logicManager->IsGameLoadPulseActive();
      break;
    case LN_BoolExpressionKind::GameLoadSucceeded:
      value = m_logicManager != nullptr && m_logicManager->DidGameLoadSucceed();
      break;
    default:
      ProfileRegisterExpressionFallback();
      return false;
  }
  m_registerExpressionState.bool_values[reg] = value;
  m_registerExpressionState.bool_valid[reg] = m_registerExpressionState.active_generation;
  r_value = value;
  ProfileRegisterExpressionHit();
  return true;
}

LN_RuntimeTree::LoopRuntimeState &LN_RuntimeTree::LoopState(const uint32_t loop_frame_index)
{
  if (loop_frame_index >= m_loopStates.size()) {
    m_loopStates.resize(size_t(loop_frame_index) + 1);
  }
  return m_loopStates[loop_frame_index];
}

void LN_RuntimeTree::BeginLoopFrame(const uint32_t loop_frame_index, const LN_TickContext &context)
{
  if (m_program == nullptr || loop_frame_index >= m_program->GetLoopFrames().size()) {
    return;
  }

  const LN_LoopFrame &frame = m_program->GetLoopFrames()[loop_frame_index];
  LoopRuntimeState &state = LoopState(loop_frame_index);
  state.triggered = false;

  /* An active frame owns its continuation. Do not resample its trigger, collection, count, or
   * requested duration, and deliberately ignore overlapping input pulses until it completes. */
  if (state.active) {
    return;
  }
  /* Multiple compiled routes for one frame must never restart it or erase its completion pulse
   * inside the same manager tick. */
  if (state.last_batch_tick_index == context.tick_index) {
    return;
  }
  /* A mirrored route (an On Init body in Fixed Update) resumes an active frame but never starts
   * one: its event trigger would otherwise restart the loop on every tick. */
  if (frame.start_event_mask != 0 &&
      (frame.start_event_mask & (1u << uint8_t(context.event))) == 0)
  {
    return;
  }

  state.break_requested = false;
  state.completed_scope_serial = 0;
  const bool trigger = frame.trigger_bool_expr_index == LN_INVALID_INDEX ?
                           true :
                           ResolveBoolExpression(frame.trigger_bool_expr_index, false, context);

  if (!trigger) {
    state.active = false;
    state.trigger_previous = false;
    state.iteration_index = 0;
    state.iteration_count = 0;
    state.batch_index = 0;
    state.batch_count = 1;
    state.batch_end_iteration = 0;
    state.items.clear();
    state.current_value = MakeNoneValue();
    return;
  }

  state.active = true;
  state.triggered = true;
  state.iteration_index = 0;
  state.batch_index = 0;
  state.batch_end_iteration = 0;
  state.items.clear();

  if (frame.kind == LN_LoopKind::FromList) {
    LN_Value list_value = frame.list_value_expr_index == LN_INVALID_INDEX ?
                              MakeNoneValue() :
                              ResolveValueExpression(
                                  frame.list_value_expr_index, MakeNoneValue(), context);
    if (list_value.type == LN_ValueType::List && list_value.exists) {
      state.items = std::move(list_value.list_value);
    }
    state.iteration_count = state.items.size();
  }
  else {
    const int32_t count = frame.count_int_expr_index == LN_INVALID_INDEX ?
                              0 :
                              std::max(
                                  0, ResolveIntExpression(frame.count_int_expr_index, 0, context));
    state.iteration_count = size_t(count);
  }

  const int32_t requested_ticks =
      frame.ticks_to_complete_int_expr_index == LN_INVALID_INDEX ?
          1 :
          std::max(1, ResolveIntExpression(frame.ticks_to_complete_int_expr_index, 1, context));
  /* A non-empty loop must perform at least one iteration on every active tick. Asking for more
   * ticks than iterations therefore completes early rather than inserting empty body ticks. */
  state.batch_count = state.iteration_count == 0 ?
                          1 :
                          std::min(state.iteration_count, size_t(requested_ticks));

  state.trigger_previous = trigger;
}

bool LN_RuntimeTree::AdvanceLoopFrame(const uint32_t loop_frame_index)
{
  LoopRuntimeState &state = LoopState(loop_frame_index);
  if (!state.active || state.iteration_index >= state.iteration_count) {
    state.active = false;
    return false;
  }

  if (m_program != nullptr && loop_frame_index < m_program->GetLoopFrames().size()) {
    const LN_LoopFrame &frame = m_program->GetLoopFrames()[loop_frame_index];
    if (frame.kind == LN_LoopKind::FromList) {
      if (state.iteration_index < state.items.size()) {
        state.current_value = state.items[state.iteration_index];
      }
      else {
        state.current_value = MakeNoneValue();
      }
    }
    else {
      state.current_value = MakeNoneValue();
      state.current_value.type = LN_ValueType::Int;
      state.current_value.exists = true;
      state.current_value.int_value = int32_t(state.iteration_index);
    }
  }

  state.iteration_index++;
  return true;
}

void LN_RuntimeTree::ExecuteEvent(LN_Event event,
                                  const std::vector<LN_InstructionHeader> &instructions,
                                  LN_CommandBuffer &command_buffer,
                                  LN_TickContext context)
{
  context.event = event;
  auto next_execution_scope_serial = [&]() {
    m_executionScopeSerial++;
    if (m_executionScopeSerial == 0) {
      m_executionScopeSerial++;
    }
    return m_executionScopeSerial;
  };
  const uint64_t event_scope_serial = next_execution_scope_serial();
  context.execution_scope_serial = event_scope_serial;
  m_activeExecutionScopeSerial = event_scope_serial;
  uint32_t command_sequence = 0;
  auto command_sort_key = [&](const uint32_t local_instruction_index,
                              const uint32_t local_command_sequence) {
    return CommandSortKey(InstructionStateIndex(event, local_instruction_index),
                          local_command_sequence);
  };
  auto next_command_execution_key = [&]() {
    m_commandExecutionSerial++;
    if (m_commandExecutionSerial == 0) {
      m_commandExecutionSerial++;
    }
    return m_commandExecutionSerial;
  };
  struct ScopedCommandQueryContext {
    LN_TickContext &context;
    LN_CommandBuffer *previous_command_buffer;
    uint64_t previous_command_execution_key;

    ScopedCommandQueryContext(LN_TickContext &context,
                              LN_CommandBuffer &command_buffer,
                              const uint64_t command_execution_key)
        : context(context),
          previous_command_buffer(context.command_buffer),
          previous_command_execution_key(context.command_execution_key)
    {
      context.command_buffer = &command_buffer;
      context.command_execution_key = command_execution_key;
    }

    ~ScopedCommandQueryContext()
    {
      context.command_buffer = previous_command_buffer;
      context.command_execution_key = previous_command_execution_key;
    }
  };
  auto resolve_command_target = [&](const LN_Instruction &instruction) -> KX_GameObject * {
    if (instruction.value_expr_index == LN_INVALID_INDEX) {
      return m_gameObject;
    }

    return ResolveObjectValue(
        ResolveValueExpression(instruction.value_expr_index, MakeNoneValue(), context));
  };
  auto resolve_required_object = [&](const uint32_t expr_index) -> KX_GameObject * {
    if (expr_index == LN_INVALID_INDEX) {
      return nullptr;
    }

    return ResolveObjectValue(ResolveValueExpression(expr_index, MakeNoneValue(), context));
  };
  auto frame_relative_transform_is_safe = [&](KX_GameObject *target,
                                              const LN_Instruction &instruction,
                                              const uint32_t instruction_id,
                                              const LN_CommandBuffer::CommandType command_type) {
    if (event != LN_Event::OnUpdate || target == nullptr || !target->IsDynamic()) {
      return true;
    }

    LN_CommandBuffer::Command diagnostic;
    diagnostic.type = command_type;
    diagnostic.object = target;
    diagnostic.runtime_tree = this;
    diagnostic.sort_key = command_sort_key(instruction_id, command_sequence);
    diagnostic.source_ref_index = instruction.source_ref_index;
    LN_WarnCommandFailureOnce(
        diagnostic,
        "Frame Update cannot apply a relative transform to a dynamic physics body; drive the "
        "node from Logic + Physics flow or use a non-dynamic object");
    return false;
  };
  auto resolve_add_object_template = [&](const uint32_t expr_index) -> KX_GameObject * {
    if (expr_index == LN_INVALID_INDEX) {
      return nullptr;
    }

    const LN_Value value = ResolveValueExpression(expr_index, MakeNoneValue(), context);
    if (value.type != LN_ValueType::ObjectRef) {
      return nullptr;
    }

    if (!value.reference_name.empty()) {
      if (KX_GameObject *template_object = FindSceneObjectByName(value.reference_name, true)) {
        return template_object;
      }
    }

    return ResolveObjectValue(value, true);
  };
  auto resolve_object_expression = [&](const uint32_t expr_index) -> KX_GameObject * {
    if (expr_index == LN_INVALID_INDEX) {
      return m_gameObject;
    }
    return resolve_required_object(expr_index);
  };
  auto vector_command_payload = [&](const LN_Instruction &instruction) {
    if (m_program != nullptr &&
        instruction.command_payload_index < m_program->GetVectorCommandPayloads().size())
    {
      return m_program->GetVectorCommandPayloads()[instruction.command_payload_index];
    }

    LN_VectorCommandPayload payload;
    payload.object_value_expr_index = instruction.value_expr_index;
    payload.vector_expr_index = instruction.vector_expr_index;
    payload.secondary_vector_expr_index = instruction.secondary_vector_expr_index;
    payload.bool_expr_index = instruction.bool_expr_index;
    payload.vector_value = instruction.vector_value;
    payload.secondary_vector_value = instruction.secondary_vector_value;
    payload.operation_mode = instruction.vector_operation_mode;
    payload.operation_channel = instruction.vector_operation_channel;
    payload.operation_mask = instruction.vector_operation_mask;
    payload.bool_value = instruction.bool_value;
    return payload;
  };
  auto game_property_command_payload = [&](const LN_Instruction &instruction) {
    if (m_program != nullptr &&
        instruction.command_payload_index < m_program->GetGamePropertyCommandPayloads().size())
    {
      return m_program->GetGamePropertyCommandPayloads()[instruction.command_payload_index];
    }

    LN_GamePropertyCommandPayload payload;
    payload.property_ref_index = instruction.property_ref_index;
    payload.value_type = instruction.property_value_type;
    payload.bool_expr_index = instruction.bool_expr_index;
    payload.int_expr_index = instruction.int_expr_index;
    payload.float_expr_index = instruction.float_expr_index;
    payload.string_expr_index = instruction.string_expr_index;
    payload.vector_expr_index = instruction.vector_expr_index;
    payload.color_expr_index = instruction.color_expr_index;
    payload.modify_op = LN_PropertyModifyOp(instruction.int_value);
    payload.modify_min_expr_index = instruction.secondary_float_expr_index;
    payload.modify_max_expr_index = instruction.tertiary_float_expr_index;
    if (instruction.property_value_type != LN_ValueType::None &&
        (payload.modify_op != LN_PropertyModifyOp::None ||
         instruction.bool_expr_index != LN_INVALID_INDEX ||
         instruction.int_expr_index != LN_INVALID_INDEX ||
         instruction.float_expr_index != LN_INVALID_INDEX ||
         instruction.string_expr_index != LN_INVALID_INDEX ||
         instruction.vector_expr_index != LN_INVALID_INDEX ||
         instruction.color_expr_index != LN_INVALID_INDEX))
    {
      payload.object_value_expr_index = instruction.value_expr_index;
    }
    else {
      payload.value_expr_index = instruction.value_expr_index;
    }
    return payload;
  };
  auto rigid_body_constraint_command_payload = [&](const LN_Instruction &instruction) {
    if (m_program != nullptr && instruction.command_payload_index <
                                    m_program->GetRigidBodyConstraintCommandPayloads().size())
    {
      return m_program->GetRigidBodyConstraintCommandPayloads()[instruction.command_payload_index];
    }

    LN_RigidBodyConstraintCommandPayload payload;
    payload.object_value_expr_index = instruction.value_expr_index;
    payload.target_value_expr_index = instruction.secondary_value_expr_index;
    payload.name_string_expr_index = instruction.string_expr_index;
    payload.bool_expr_indices[size_t(LN_RigidBodyConstraintBoolInput::UseWorldSpace)] =
        instruction.bool_expr_index;
    payload.vector_expr_indices[size_t(LN_RigidBodyConstraintVectorInput::Pivot)] =
        instruction.vector_expr_index;
    return payload;
  };

  auto execute_mouse_look_command = [&](const LN_Instruction &instruction,
                                        const uint32_t instruction_id) {
    KX_GameObject *body = ResolveObjectValue(
        ResolveValueExpression(instruction.value_expr_index, MakeNoneValue(), context));
    if (body == nullptr) {
      body = m_gameObject;
    }

    KX_GameObject *head = ResolveObjectValue(
        ResolveValueExpression(instruction.property_ref_index, MakeNoneValue(), context));
    if (head == nullptr) {
      head = body;
    }
    if (body == nullptr || head == nullptr) {
      command_sequence++;
      MarkInstructionExecuted(event, instruction_id, context.tick_index);
      return;
    }

    const MouseLookInputState mouse = ReadMouseLookInputState(
        m_snapshot.GetInputSnapshot().GetMouse());
    /* Center Mouse holds the cursor in the middle, which needs a cursor the game can move: on
     * Wayland only a hidden one. A shown cursor there turns the view by how far it moved, as
     * without Center Mouse, instead of by its distance from a middle it never reaches (the view
     * spun). */
    const KX_KetsjiEngine *look_engine = ActiveEngine();
    const bool cursor_movable = look_engine == nullptr || look_engine->IsCursorMovable();
    const bool center_mouse = instruction.bool_value && cursor_movable;
    const int32_t front_axis = std::clamp(instruction.int_value, 0, 1);

    const LN_InputSnapshot &input_snapshot = m_snapshot.GetInputSnapshot();
    RAS_ICanvas *canvas = ActiveCanvas();
    int32_t canvas_width = 0;
    int32_t canvas_height = 0;
    if (canvas != nullptr) {
      canvas_width = canvas->GetWidth();
      canvas_height = canvas->GetHeight();
    }
    else if (input_snapshot.HasCanvas()) {
      canvas_width = input_snapshot.GetCanvasWidth();
      canvas_height = input_snapshot.GetCanvasHeight();
    }
    const uint32_t state_index = InstructionStateIndex(event, instruction_id);
    InstructionRuntimeState *state = state_index < m_instructionStates.size() ?
                                         &m_instructionStates[state_index] :
                                         nullptr;
    /* The mouse is player 0's: while a menu or a UI in the game world uses it (or the window has
     * no focus), the view waits and the cursor stays free. After that (or a pause) it starts over
     * from where the cursor is, so the view does not jump. */
    if (context.mouse_look_waits) {
      if (state != nullptr) {
        state->mouse_look_initialized = false;
        state->mouse_look_has_previous_position = false;
        state->mouse_look_smooth_x = 0.0f;
        state->mouse_look_smooth_y = 0.0f;
        state->mouse_look_ui_serial = context.mouse_ui_serial;
      }
      MarkInstructionExecuted(event, instruction_id, context.tick_index);
      return;
    }
    /* Only while it turns: a menu shows the cursor while the view waits. */
    if (instruction.bool_value && !cursor_movable) {
      static bool warned = false;
      if (!warned) {
        warned = true;
        CM_Warning("Mouse Look: the cursor cannot be held in the middle here (Wayland moves only "
                   "a hidden cursor, and none with --debug), so turning stops at the window's "
                   "edge; hide the cursor with Set Cursor Visibility for endless turning");
      }
    }
    /* Also when it starts or stops holding the cursor (the cursor was hidden or shown). */
    if (state != nullptr && (state->mouse_look_ui_serial != context.mouse_ui_serial ||
                             state->mouse_look_centered != center_mouse))
    {
      state->mouse_look_ui_serial = context.mouse_ui_serial;
      state->mouse_look_centered = center_mouse;
      state->mouse_look_initialized = false;
      state->mouse_look_has_previous_position = false;
      state->mouse_look_smooth_x = 0.0f;
      state->mouse_look_smooth_y = 0.0f;
    }
    auto remember_mouse_position = [&](const int32_t x, const int32_t y) {
      if (state != nullptr) {
        state->mouse_look_previous_x = x;
        state->mouse_look_previous_y = y;
        state->mouse_look_has_previous_position = true;
      }
    };
    auto remember_current_mouse_position = [&]() {
      if (mouse.has_position) {
        remember_mouse_position(mouse.x, mouse.y);
      }
    };
    /* The cursor is held in the middle of the window; with split screen, in the middle of player
     * 0's view (the mouse is theirs; Set Split Screen's Keep Mouse in View holds it there). */
    int32_t center_x = MouseLookCenterPixel(canvas_width);
    int32_t center_y = MouseLookCenterPixel(canvas_height);
    if (KX_Scene *look_scene = ActiveEngine() != nullptr && m_gameObject != nullptr ?
                                   m_gameObject->GetScene() :
                                   nullptr)
    {
      RAS_Rect mouse_view;
      const KX_PlayerViews &views = look_scene->GetPlayerViews();
      if (views.GetMouseView(look_scene->GetActiveCamera(), views.GetWindow(), mouse_view)) {
        center_x = (mouse_view.GetLeft() + mouse_view.GetRight()) / 2;
        center_y = canvas_height - (mouse_view.GetBottom() + mouse_view.GetTop()) / 2;
      }
    }
    auto append_center_cursor_command = [&]() {
      if (canvas == nullptr) {
        return;
      }
      const int32_t x = center_x;
      const int32_t y = center_y;
      command_buffer.AppendSetCursorPosition(
          x, y, command_sort_key(instruction_id, command_sequence), instruction.source_ref_index);
      command_sequence++;
      remember_mouse_position(x, y);
    };

    float offset_x = 0.0f;
    float offset_y = 0.0f;
    const float pixel_to_rotation_scale = MouseLookPixelToRotationScale();
    const bool has_frame_delta = mouse.delta_x != 0 || mouse.delta_y != 0;
    const bool has_canvas_position = mouse.has_position && canvas_width > 0 && canvas_height > 0;
    if (center_mouse && has_canvas_position) {
      offset_x = -float(mouse.x - center_x) * pixel_to_rotation_scale;
      offset_y = -float(mouse.y - center_y) * pixel_to_rotation_scale;
    }
    else if (center_mouse && has_frame_delta) {
      offset_x = -float(mouse.delta_x) * pixel_to_rotation_scale;
      offset_y = -float(mouse.delta_y) * pixel_to_rotation_scale;
    }
    else if (!center_mouse && mouse.has_position && state != nullptr &&
             state->mouse_look_has_previous_position)
    {
      offset_x = -float(mouse.x - state->mouse_look_previous_x) * pixel_to_rotation_scale;
      offset_y = -float(mouse.y - state->mouse_look_previous_y) * pixel_to_rotation_scale;
    }
    else {
      offset_x = -float(mouse.delta_x) * pixel_to_rotation_scale;
      offset_y = -float(mouse.delta_y) * pixel_to_rotation_scale;
    }

    const MT_Vector3 invert = ResolveVectorExpression(
        instruction.secondary_vector_expr_index,
        MT_Vector3(instruction.vector_value.x(), instruction.vector_value.y(), 0.0f),
        context);
    const MT_Vector3 cap_x = ResolveVectorExpression(
        instruction.color_expr_index,
        MT_Vector3(
            instruction.secondary_vector_value.x(), instruction.secondary_vector_value.y(), 0.0f),
        context);
    const MT_Vector3 cap_y = ResolveVectorExpression(
        instruction.int_expr_index,
        MT_Vector3(instruction.color_value.x(), instruction.color_value.y(), 0.0f),
        context);
    const bool use_cap_x = ResolveBoolExpression(
        instruction.bool_expr_index, instruction.secondary_vector_value.z() > 0.5f, context);
    const bool use_cap_y = ResolveBoolExpression(
        instruction.secondary_bool_expr_index, instruction.color_value.w() > 0.5f, context);
    const float sensitivity = std::max(0.0f,
                                       ResolveFloatExpression(instruction.float_expr_index,
                                                              instruction.vector_value.z(),
                                                              context));
    const float smoothing = std::clamp(ResolveFloatExpression(instruction.string_expr_index,
                                                              instruction.color_value.z(),
                                                              context),
                                       0.0f,
                                       1.0f);

    if (std::fabs(invert.x()) > 0.5f) {
      offset_x = -offset_x;
    }
    if (std::fabs(invert.y()) > 0.5f) {
      offset_y = -offset_y;
    }

    offset_x *= sensitivity * 0.002f;
    offset_y *= sensitivity * 0.002f;

    /* Only a real game window has a system pointer to wait for. */
    auto start_settling = [&]() {
      if (canvas != nullptr) {
        state->mouse_look_settle = MOUSE_LOOK_SETTLE_SECONDS;
        state->mouse_look_settle_runs = MOUSE_LOOK_SETTLE_MAX_RUNS;
      }
    };
    if (center_mouse && mouse.has_position && state != nullptr && !state->mouse_look_initialized) {
      state->mouse_look_initialized = true;
      state->mouse_look_smooth_x = 0.0f;
      state->mouse_look_smooth_y = 0.0f;
      start_settling();
      append_center_cursor_command();
      MarkInstructionExecuted(event, instruction_id, context.tick_index);
      return;
    }
    if (!center_mouse && mouse.has_position && state != nullptr &&
        !state->mouse_look_has_previous_position)
    {
      state->mouse_look_initialized = true;
      state->mouse_look_smooth_x = 0.0f;
      state->mouse_look_smooth_y = 0.0f;
      start_settling();
      remember_current_mouse_position();
      MarkInstructionExecuted(event, instruction_id, context.tick_index);
      return;
    }
    /* Settling: the cursor is held, the view does not turn yet. */
    if (state != nullptr && state->mouse_look_settle > 0.0f) {
      const double step = context.unscaled_dt > 0.0 ? context.unscaled_dt : context.delta_time;
      state->mouse_look_settle -= float(std::max(step, 0.0));
      if (--state->mouse_look_settle_runs <= 0) {
        state->mouse_look_settle = 0.0f;
      }
      if (center_mouse && mouse.has_position) {
        append_center_cursor_command();
      }
      else if (!center_mouse) {
        remember_current_mouse_position();
      }
      MarkInstructionExecuted(event, instruction_id, context.tick_index);
      return;
    }
    if (state != nullptr) {
      const float target_x = offset_x * MOUSE_LOOK_REFERENCE_PIXELS;
      const float target_y = offset_y * MOUSE_LOOK_REFERENCE_PIXELS;
      const float smoothing_factor = MouseLookSmoothingFactor(smoothing, context.delta_time);
      state->mouse_look_smooth_x += (target_x - state->mouse_look_smooth_x) * smoothing_factor;
      state->mouse_look_smooth_y += (target_y - state->mouse_look_smooth_y) * smoothing_factor;
      offset_x = state->mouse_look_smooth_x / MOUSE_LOOK_REFERENCE_PIXELS;
      offset_y = state->mouse_look_smooth_y / MOUSE_LOOK_REFERENCE_PIXELS;
      state->mouse_look_initialized = true;
    }

    if (std::fabs(offset_x) <= MT_EPSILON && std::fabs(offset_y) <= MT_EPSILON) {
      if (center_mouse && mouse.has_position) {
        append_center_cursor_command();
      }
      else if (!center_mouse) {
        remember_current_mouse_position();
      }
      MarkInstructionExecuted(event, instruction_id, context.tick_index);
      return;
    }

    const int rot_axis = 1 - front_axis;
    if (state != nullptr) {
      if (use_cap_x) {
        offset_x = ClampMouseLookDelta(state->mouse_look_angle_x, offset_x, cap_x);
      }
      if (use_cap_y) {
        offset_y = ClampMouseLookDelta(state->mouse_look_angle_y, offset_y, cap_y);
      }
    }

    if (std::fabs(offset_x) > MT_EPSILON) {
      command_buffer.AppendApplyRotation(body,
                                         MouseLookRotationVector(2, offset_x),
                                         false,
                                         command_sort_key(instruction_id, command_sequence),
                                         instruction.source_ref_index);
      command_sequence++;
    }
    if (std::fabs(offset_y) > MT_EPSILON) {
      command_buffer.AppendApplyRotation(head,
                                         MouseLookRotationVector(rot_axis, offset_y),
                                         true,
                                         command_sort_key(instruction_id, command_sequence),
                                         instruction.source_ref_index);
      command_sequence++;
    }

    if (center_mouse && mouse.has_position) {
      append_center_cursor_command();
    }
    else if (!center_mouse) {
      remember_current_mouse_position();
    }

    MarkInstructionExecuted(event, instruction_id, context.tick_index);
  };

  auto execute_time_flow_control_instruction = [&](const LN_Instruction &instruction,
                                                   const uint32_t instruction_id) {
    /* Whether a UI event route armed the state: only such states are ticked while paused. */
    const bool ui_event_route = (instructions[instruction_id].flags &
                                 LN_INSTRUCTION_FLAG_UI_EVENT_ROUTE) != 0;
    switch (instruction.opcode) {
      case LN_OpCode::ArmNextFrame: {
        if (instruction.int_value >= 0) {
          if (TimeFlowRuntimeState *state = FindTimeFlowState(uint32_t(instruction.int_value))) {
            state->kind = TimeFlowRuntimeKind::NextFrame;
            state->ui_event_route = ui_event_route;
            state->active = true;
            state->event = LN_Event::OnUpdate;
            state->ignore_timescale = true;
            state->last_update_tick = context.tick_index;
            state->last_update_phase_tick = context.phase_tick_index;
          }
        }
        break;
      }
      case LN_OpCode::ArmTimer: {
        if (instruction.int_value >= 0) {
          if (TimeFlowRuntimeState *state = FindTimeFlowState(uint32_t(instruction.int_value))) {
            const float duration = std::max(
                0.0f, ResolveFloatExpression(instruction.float_expr_index, 0.0f, context));
            state->kind = TimeFlowRuntimeKind::Timer;
            state->ui_event_route = ui_event_route;
            state->ignore_timescale = ResolveBoolExpression(
                instruction.secondary_bool_expr_index, false, context);
            state->event = TimeFlowContinuationEvent(event);
            state->pulse_tick = UINT64_MAX;
            state->last_update_tick = context.tick_index;
            state->last_update_phase_tick = context.phase_tick_index;
            state->accumulated_time = 0.0;
            if (duration <= 0.0f) {
              state->active = false;
              state->remaining_time = 0.0;
              if (context.tick_index != UINT64_MAX) {
                state->pulse_tick = context.tick_index;
              }
            }
            else {
              state->active = true;
              state->remaining_time = double(duration);
            }
          }
        }
        break;
      }
      case LN_OpCode::ArmDelay: {
        if (instruction.int_value >= 0) {
          if (TimeFlowRuntimeState *state = FindTimeFlowState(uint32_t(instruction.int_value))) {
            state->kind = TimeFlowRuntimeKind::Delay;
            state->ui_event_route = ui_event_route;
            state->ignore_timescale = ResolveBoolExpression(
                instruction.secondary_bool_expr_index, false, context);
            state->event = TimeFlowContinuationEvent(event);
            state->last_update_tick = context.tick_index;
            state->last_update_phase_tick = context.phase_tick_index;
            const float duration = std::max(
                0.0f, ResolveFloatExpression(instruction.float_expr_index, 0.0f, context));
            if (state->pulse_tick != context.tick_index) {
              state->pulse_tick = UINT64_MAX;
            }
            state->accumulated_time = 0.0;

            TimeFlowRuntimeState::PendingPulse pulse;
            pulse.remaining_time = double(duration);
            pulse.tree_properties = m_treeProperties;

            if (duration <= 0.0f && context.tick_index != UINT64_MAX &&
                state->pulse_tick != context.tick_index)
            {
              state->remaining_time = 0.0;
              state->active_tree_properties = std::move(pulse.tree_properties);
              state->has_active_tree_properties = true;
              state->pulse_tick = context.tick_index;
            }
            else {
              state->pending_pulses.push_back(std::move(pulse));
              state->active = true;
              state->remaining_time = state->pending_pulses.front().remaining_time;
              for (const TimeFlowRuntimeState::PendingPulse &queued_pulse : state->pending_pulses)
              {
                state->remaining_time = std::min(state->remaining_time,
                                                 queued_pulse.remaining_time);
              }
            }
          }
        }
        break;
      }
      case LN_OpCode::UpdatePulsify: {
        if (instruction.int_value >= 0) {
          if (TimeFlowRuntimeState *state = FindTimeFlowState(uint32_t(instruction.int_value))) {
            const float interval = std::max(
                0.0f, ResolveFloatExpression(instruction.float_expr_index, 0.5f, context));
            const LN_Event continuation_event = TimeFlowContinuationEvent(event);
            const bool continuous = state->active && state->event == continuation_event &&
                                    IsContinuousPhaseTick(state->last_update_phase_tick,
                                                          context.phase_tick_index);
            state->kind = TimeFlowRuntimeKind::Pulsify;
            state->ignore_timescale = ResolveBoolExpression(
                instruction.secondary_bool_expr_index, false, context);
            state->event = continuation_event;
            if (!continuous) {
              state->active = true;
              state->accumulated_time = 0.0;
              if (context.tick_index != UINT64_MAX) {
                state->pulse_tick = context.tick_index;
              }
            }
            else if (context.tick_index != state->last_update_tick) {
              const double step = TimeFlowStep(context, state->ignore_timescale);
              if (interval <= 0.0f) {
                state->pulse_tick = context.tick_index;
              }
              else {
                state->accumulated_time += step;
                if (state->accumulated_time + LN_TIME_FLOW_EPSILON >= double(interval)) {
                  state->pulse_tick = context.tick_index;
                  const double interval_time = double(interval);
                  state->accumulated_time = std::fmod(
                      std::max(0.0, state->accumulated_time - interval_time), interval_time);
                  if (state->accumulated_time <= LN_TIME_FLOW_EPSILON) {
                    state->accumulated_time = 0.0;
                  }
                }
                else {
                  state->pulse_tick = UINT64_MAX;
                }
              }
            }
            state->remaining_time = double(interval);
            state->last_update_tick = context.tick_index;
            state->last_update_phase_tick = context.phase_tick_index;
          }
        }
        break;
      }
      case LN_OpCode::UpdateBarrier: {
        if (instruction.int_value >= 0) {
          if (TimeFlowRuntimeState *state = FindTimeFlowState(uint32_t(instruction.int_value))) {
            const bool condition = instruction.bool_expr_index == LN_INVALID_INDEX ?
                                       true :
                                       ResolveBoolExpression(
                                           instruction.bool_expr_index, false, context);
            const LN_Event continuation_event = TimeFlowContinuationEvent(event);
            const bool continuous = state->active && state->event == continuation_event &&
                                    IsContinuousPhaseTick(state->last_update_phase_tick,
                                                          context.phase_tick_index);
            state->kind = TimeFlowRuntimeKind::Barrier;
            state->ignore_timescale = ResolveBoolExpression(
                instruction.secondary_bool_expr_index, false, context);
            state->event = continuation_event;
            if (!condition) {
              state->active = false;
              state->accumulated_time = 0.0;
              if (state->pulse_tick != context.tick_index) {
                state->pulse_tick = UINT64_MAX;
              }
            }
            else {
              const float duration = std::max(
                  0.0f, ResolveFloatExpression(instruction.float_expr_index, 0.0f, context));
              if (!continuous) {
                state->active = true;
                state->accumulated_time = 0.0;
              }
              else if (context.tick_index != state->last_update_tick) {
                state->accumulated_time += TimeFlowStep(context, state->ignore_timescale);
              }
              state->remaining_time = double(duration);
              if (duration <= 0.0f ||
                  state->accumulated_time + LN_TIME_FLOW_EPSILON >= double(duration))
              {
                if (context.tick_index != UINT64_MAX) {
                  state->pulse_tick = context.tick_index;
                }
                state->accumulated_time = std::max(state->accumulated_time, double(duration));
              }
            }
            state->last_update_tick = context.tick_index;
            state->last_update_phase_tick = context.phase_tick_index;
          }
        }
        break;
      }
      case LN_OpCode::TryOnce: {
        if (instruction.int_value >= 0) {
          BoolExpressionRuntimeState &state = BoolExpressionState(uint32_t(instruction.int_value));
          state.cached_result = !state.active;
          if (state.cached_result) {
            state.active = true;
          }
          state.initialized = true;
          state.last_update_tick = context.execution_scope_serial;
        }
        break;
      }
      case LN_OpCode::ResetOnce: {
        if (instruction.int_value >= 0) {
          BoolExpressionRuntimeState &state = BoolExpressionState(uint32_t(instruction.int_value));
          state.active = false;
          state.cached_result = false;
          state.initialized = true;
          state.last_update_tick = 0;
        }
        break;
      }
      case LN_OpCode::SetFlowGate: {
        if (instruction.int_value < 0 || m_program == nullptr ||
            uint32_t(instruction.int_value) >= m_program->GetBoolExpressions().size())
        {
          return;
        }
        const uint32_t expression_index = uint32_t(instruction.int_value);
        const LN_BoolExpression &expression = m_program->GetBoolExpressions()[expression_index];
        if (expression.kind != LN_BoolExpressionKind::FlowGateOpen) {
          return;
        }
        /* Resolve once before mutation so Start Open is sampled exactly once. */
        (void)ResolveBoolExpression(expression_index, false, context);
        BoolExpressionRuntimeState &state = BoolExpressionState(expression_index);
        switch (LN_FlowGateControl(instruction.secondary_int_value)) {
          case LN_FlowGateControl::Toggle:
            state.active = !state.active;
            break;
          case LN_FlowGateControl::Open:
            state.active = true;
            break;
          case LN_FlowGateControl::Close:
            state.active = false;
            break;
          default:
            return;
        }
        break;
      }
      case LN_OpCode::TryFlowGate: {
        if (instruction.int_value < 0 || m_program == nullptr ||
            uint32_t(instruction.int_value) >= m_program->GetBoolExpressions().size())
        {
          return;
        }
        const uint32_t expression_index = uint32_t(instruction.int_value);
        if (m_program->GetBoolExpressions()[expression_index].kind !=
            LN_BoolExpressionKind::FlowGateOpen)
        {
          return;
        }
        if (!ResolveBoolExpression(expression_index, false, context)) {
          /* A rejected Enter is intentionally not marked executed; the Exit route reads this
           * instruction's pulse and therefore remains false. */
          return;
        }
        break;
      }
      case LN_OpCode::TryCooldown: {
        if (instruction.int_value >= 0) {
          if (TimeFlowRuntimeState *state = FindTimeFlowState(uint32_t(instruction.int_value))) {
            state->kind = TimeFlowRuntimeKind::Cooldown;
            if (state->active) {
              state->accepted_scope_serial = 0;
              state->blocked_scope_serial = context.execution_scope_serial;
              break;
            }

            state->accepted_scope_serial = context.execution_scope_serial;
            state->blocked_scope_serial = 0;
            state->ui_event_route = ui_event_route;
            float duration = ResolveFloatExpression(instruction.float_expr_index, 0.0f, context);
            if (!std::isfinite(duration) || duration <= 0.0f) {
              duration = 0.0f;
            }
            state->ignore_timescale = ResolveBoolExpression(
                instruction.secondary_bool_expr_index, false, context);
            state->event = TimeFlowContinuationEvent(event);
            state->remaining_time = double(duration);
            /* Cooldown uses accumulated_time as its sampled total duration. */
            state->accumulated_time = double(duration);
            state->last_update_tick = context.tick_index;
            state->last_update_phase_tick = context.phase_tick_index;
            state->active = duration > 0.0f;
            if (state->pulse_tick != context.tick_index) {
              state->pulse_tick = UINT64_MAX;
            }
          }
        }
        break;
      }
      case LN_OpCode::ResetCooldown: {
        if (instruction.int_value >= 0) {
          if (TimeFlowRuntimeState *state = FindTimeFlowState(uint32_t(instruction.int_value))) {
            state->kind = TimeFlowRuntimeKind::Cooldown;
            state->active = false;
            state->remaining_time = 0.0;
            state->accumulated_time = 0.0;
            state->pulse_tick = UINT64_MAX;
            state->last_update_tick = context.tick_index;
            state->last_update_phase_tick = context.phase_tick_index;
            state->accepted_scope_serial = 0;
            state->blocked_scope_serial = 0;
            state->ignore_timescale = false;
            state->event = TimeFlowContinuationEvent(event);
          }
        }
        break;
      }
      case LN_OpCode::TryFlowRouter: {
        if (instruction.int_value < 0 || instruction.secondary_int_value <= 0) {
          return;
        }
        if (FlowControlRuntimeState *state = FindFlowControlState(uint32_t(instruction.int_value)))
        {
          const uint32_t output_count = uint32_t(instruction.secondary_int_value);
          const uint32_t selected_index = state->next_route_index % output_count;
          state->last_route_index = selected_index;
          state->next_route_index = (selected_index + 1u) % output_count;
          state->routed_scope_serial = context.execution_scope_serial;
        }
        break;
      }
      case LN_OpCode::ResetFlowRouter: {
        if (instruction.int_value < 0) {
          return;
        }
        if (FlowControlRuntimeState *state = FindFlowControlState(uint32_t(instruction.int_value)))
        {
          state->next_route_index = 0;
          state->last_route_index = LN_INVALID_INDEX;
          state->routed_scope_serial = 0;
        }
        break;
      }
      case LN_OpCode::TryRepeatLimit: {
        if (instruction.int_value < 0 || instruction.int_expr_index == LN_INVALID_INDEX) {
          return;
        }
        if (FlowControlRuntimeState *state = FindFlowControlState(uint32_t(instruction.int_value)))
        {
          const int32_t requested_limit = ResolveIntExpression(
              instruction.int_expr_index, 0, context);
          const uint32_t limit = requested_limit > 0 ? uint32_t(requested_limit) : 0u;
          if (state->accepted_count < limit) {
            state->accepted_count++;
            state->accepted_scope_serial = context.execution_scope_serial;
            state->blocked_scope_serial = 0;
          }
          else {
            state->accepted_scope_serial = 0;
            state->blocked_scope_serial = context.execution_scope_serial;
          }
        }
        break;
      }
      case LN_OpCode::ResetRepeatLimit: {
        if (instruction.int_value < 0) {
          return;
        }
        if (FlowControlRuntimeState *state = FindFlowControlState(uint32_t(instruction.int_value)))
        {
          state->accepted_count = 0;
          state->accepted_scope_serial = 0;
          state->blocked_scope_serial = 0;
        }
        break;
      }
      case LN_OpCode::SampleMotion: {
        if (instruction.int_value < 0) {
          return;
        }
        FlowControlRuntimeState *state = FindFlowControlState(uint32_t(instruction.int_value));
        if (state == nullptr) {
          return;
        }

        /* A graph may route the same pulse to this node more than once. Ignore duplicate samples
         * before resolving their source so a later branch cannot invalidate or replace the sample
         * already accepted for this engine tick. ResetMotion clears this guard intentionally. */
        if (state->motion_last_sample_tick == context.tick_index) {
          break;
        }

        const LN_MotionSourceMode source_mode = LN_MotionSourceMode(
            instruction.secondary_int_value);
        MT_Vector3 position(0.0f, 0.0f, 0.0f);
        LN_RuntimeRef source_ref;
        bool source_valid = false;
        if (source_mode == LN_MotionSourceMode::Position) {
          if (instruction.vector_expr_index != LN_INVALID_INDEX) {
            position = ResolveVectorExpression(
                instruction.vector_expr_index, instruction.vector_value, context);
            source_valid = vector_is_finite(position);
          }
        }
        else if (source_mode == LN_MotionSourceMode::OwnerObject ||
                 source_mode == LN_MotionSourceMode::Object)
        {
          KX_GameObject *object = source_mode == LN_MotionSourceMode::OwnerObject ? m_gameObject :
                                                                                    nullptr;
          if (source_mode == LN_MotionSourceMode::Object &&
              instruction.value_expr_index != LN_INVALID_INDEX)
          {
            object = ResolveObjectValue(
                ResolveValueExpression(instruction.value_expr_index, MakeNoneValue(), context));
          }
          if (object != nullptr) {
            FlushPendingRaycastTransforms(object, context);
            position = object->NodeGetWorldPosition();
            source_ref = MakeObjectRef(object, object->GetName());
            source_valid = vector_is_finite(position) && source_ref.IsValid();
          }
        }
        else {
          return;
        }

        if (!source_valid || !std::isfinite(context.current_time)) {
          state->motion_initialized = false;
          state->motion_valid = false;
          state->motion_velocity = MT_Vector3(0.0f, 0.0f, 0.0f);
          state->motion_direction = MT_Vector3(0.0f, 0.0f, 0.0f);
          state->motion_speed = 0.0f;
          state->motion_predicted_position = state->motion_position;
          break;
        }

        const bool object_source = source_mode != LN_MotionSourceMode::Position;
        const bool source_changed = state->motion_source_mode != int32_t(source_mode) ||
                                    (object_source &&
                                     !RuntimeRefsEqual(state->motion_source_ref, source_ref));
        const double elapsed = context.current_time - state->motion_last_sample_time;
        if (!state->motion_initialized || source_changed || !std::isfinite(elapsed) ||
            elapsed < 0.0)
        {
          state->motion_initialized = true;
          state->motion_valid = false;
          state->motion_source_mode = int32_t(source_mode);
          state->motion_source_ref = source_ref;
          state->motion_last_sample_tick = context.tick_index;
          state->motion_last_sample_time = context.current_time;
          state->motion_position = position;
          state->motion_previous_position = position;
          state->motion_velocity = MT_Vector3(0.0f, 0.0f, 0.0f);
          state->motion_direction = MT_Vector3(0.0f, 0.0f, 0.0f);
          state->motion_predicted_position = position;
          state->motion_speed = 0.0f;
          break;
        }

        /* OnUpdate can run on a render frame where fixed simulation time has not advanced. Keep
         * the last timed sample and its outputs intact; rebasing here would discard displacement
         * and make Valid flicker at render rates above the fixed-step rate. */
        if (elapsed <= 1.0e-9) {
          state->motion_last_sample_tick = context.tick_index;
          break;
        }

        const double raw_x = (double(position.x()) - double(state->motion_previous_position.x())) /
                             elapsed;
        const double raw_y = (double(position.y()) - double(state->motion_previous_position.y())) /
                             elapsed;
        const double raw_z = (double(position.z()) - double(state->motion_previous_position.z())) /
                             elapsed;
        MT_Vector3 velocity{float(raw_x), float(raw_y), float(raw_z)};
        if (!vector_is_finite(velocity)) {
          state->motion_initialized = false;
          state->motion_valid = false;
          state->motion_velocity = MT_Vector3(0.0f, 0.0f, 0.0f);
          state->motion_direction = MT_Vector3(0.0f, 0.0f, 0.0f);
          state->motion_speed = 0.0f;
          state->motion_predicted_position = position;
          state->motion_position = position;
          break;
        }

        float smoothing_time = ResolveFloatExpression(instruction.secondary_float_expr_index,
                                                      instruction.secondary_vector_value.x(),
                                                      context);
        if (!std::isfinite(smoothing_time) || smoothing_time < 0.0f) {
          smoothing_time = 0.0f;
        }
        if (smoothing_time > 0.0f && state->motion_valid) {
          const double alpha = -std::expm1(-elapsed / double(smoothing_time));
          velocity = state->motion_velocity +
                     (velocity - state->motion_velocity) * float(std::clamp(alpha, 0.0, 1.0));
        }

        const double speed_double = std::sqrt(double(velocity.x()) * double(velocity.x()) +
                                              double(velocity.y()) * double(velocity.y()) +
                                              double(velocity.z()) * double(velocity.z()));
        if (!std::isfinite(speed_double) ||
            speed_double > double(std::numeric_limits<float>::max()))
        {
          state->motion_initialized = false;
          state->motion_valid = false;
          state->motion_velocity = MT_Vector3(0.0f, 0.0f, 0.0f);
          state->motion_direction = MT_Vector3(0.0f, 0.0f, 0.0f);
          state->motion_speed = 0.0f;
          state->motion_predicted_position = position;
          state->motion_position = position;
          break;
        }

        float prediction_time = ResolveFloatExpression(
            instruction.float_expr_index, instruction.vector_value.x(), context);
        if (!std::isfinite(prediction_time) || prediction_time < 0.0f) {
          prediction_time = 0.0f;
        }
        const double predicted_x = double(position.x()) +
                                   double(velocity.x()) * double(prediction_time);
        const double predicted_y = double(position.y()) +
                                   double(velocity.y()) * double(prediction_time);
        const double predicted_z = double(position.z()) +
                                   double(velocity.z()) * double(prediction_time);
        const double max_float = double(std::numeric_limits<float>::max());
        const bool predicted_valid = std::isfinite(predicted_x) && std::isfinite(predicted_y) &&
                                     std::isfinite(predicted_z) &&
                                     std::fabs(predicted_x) <= max_float &&
                                     std::fabs(predicted_y) <= max_float &&
                                     std::fabs(predicted_z) <= max_float;

        state->motion_source_mode = int32_t(source_mode);
        state->motion_source_ref = source_ref;
        state->motion_last_sample_tick = context.tick_index;
        state->motion_last_sample_time = context.current_time;
        state->motion_position = position;
        state->motion_previous_position = position;
        state->motion_velocity = velocity;
        state->motion_speed = float(speed_double);
        state->motion_direction = speed_double > double(MT_EPSILON) ?
                                      velocity / state->motion_speed :
                                      MT_Vector3(0.0f, 0.0f, 0.0f);
        state->motion_predicted_position = predicted_valid ? MT_Vector3(float(predicted_x),
                                                                        float(predicted_y),
                                                                        float(predicted_z)) :
                                                             position;
        state->motion_valid = predicted_valid;
        break;
      }
      case LN_OpCode::ResetMotion: {
        if (instruction.int_value < 0) {
          return;
        }
        if (FlowControlRuntimeState *state = FindFlowControlState(uint32_t(instruction.int_value)))
        {
          *state = FlowControlRuntimeState{};
        }
        break;
      }
      case LN_OpCode::BreakLoop: {
        if (instruction.int_value < 0 || instruction.loop_frame_index == LN_INVALID_INDEX ||
            uint32_t(instruction.int_value) != instruction.loop_frame_index ||
            m_activeLoopBodyFrame != instruction.loop_frame_index)
        {
          return;
        }
        LoopState(instruction.loop_frame_index).break_requested = true;
        break;
      }
      default:
        break;
    }

    MarkInstructionExecuted(event, instruction_id, context.tick_index);
  };

  auto execute_global_property_instruction = [&](const LN_Instruction &instruction,
                                                 const uint32_t instruction_id) {
    const std::string category = ResolveStringExpression(
        instruction.string_expr_index, std::string(), context);
    const std::string property = ResolveStringExpression(
        instruction.secondary_string_expr_index, std::string(), context);
    if (category.empty() || property.empty()) {
      return;
    }
    LN_Value value = ResolveValueExpression(
        instruction.value_expr_index, MakeNoneValue(), context);
    ShareValue(value);
    const bool persistent = ResolveBoolExpression(instruction.bool_expr_index, false, context);
    {
      std::lock_guard<std::mutex> lock(GlobalPropertiesMutex());
      if (persistent) {
        EnsureGlobalCategoryLoadedUnlocked(category);
      }
      GlobalProperties()[category][property] = value;
      if (persistent) {
        PersistGlobalCategoryUnlocked(category);
      }
    }
    command_sequence++;
    MarkInstructionExecuted(event, instruction_id, context.tick_index);
  };

  auto execute_variable_instruction = [&](const LN_Instruction &instruction,
                                          const uint32_t instruction_id) {
    switch (instruction.opcode) {
      case LN_OpCode::SaveVariable: {
        const std::string path = ResolveStringExpression(
            instruction.string_expr_index, "//Data", context);
        const std::string file_name = ResolveStringExpression(
            instruction.secondary_string_expr_index, "variables", context);
        const std::string name = ResolveStringExpression(
            instruction.tertiary_string_expr_index, "var", context);
        if (name.empty()) {
          return;
        }
        LN_Value dict = LoadVariableDictFromFile(path, file_name);
        dict.type = LN_ValueType::Dict;
        dict.exists = true;
        dict.dict_value[name] = ResolveValueExpression(
            instruction.value_expr_index, MakeNoneValue(), context);
        SaveVariableDictToFile(path, file_name, dict);
        break;
      }
      case LN_OpCode::SaveVariableDict: {
        const std::string path = ResolveStringExpression(
            instruction.string_expr_index, "//Data", context);
        const std::string file_name = ResolveStringExpression(
            instruction.secondary_string_expr_index, "variables", context);
        LN_Value dict = ResolveValueExpression(
            instruction.value_expr_index, MakeNoneValue(), context);
        if (dict.type != LN_ValueType::Dict) {
          dict.type = LN_ValueType::Dict;
          dict.exists = true;
          dict.dict_value.clear();
        }
        SaveVariableDictToFile(path, file_name, dict);
        break;
      }
      case LN_OpCode::ClearVariables: {
        const std::string path = ResolveStringExpression(
            instruction.string_expr_index, "//Data", context);
        const std::string file_name = ResolveStringExpression(
            instruction.secondary_string_expr_index, "variables", context);
        LN_Value dict;
        dict.type = LN_ValueType::Dict;
        dict.exists = true;
        SaveVariableDictToFile(path, file_name, dict);
        break;
      }
      case LN_OpCode::RemoveVariable: {
        const std::string path = ResolveStringExpression(
            instruction.string_expr_index, "//Data", context);
        const std::string file_name = ResolveStringExpression(
            instruction.secondary_string_expr_index, "variables", context);
        const std::string name = ResolveStringExpression(
            instruction.tertiary_string_expr_index, "var", context);
        if (!name.empty()) {
          LN_Value dict = LoadVariableDictFromFile(path, file_name);
          if (dict.type != LN_ValueType::Dict) {
            dict.type = LN_ValueType::Dict;
            dict.exists = true;
            dict.dict_value.clear();
          }
          dict.dict_value.erase(name);
          SaveVariableDictToFile(path, file_name, dict);
        }
        break;
      }
      case LN_OpCode::InputMap: {
        LN_InputMap &map = LN_InputMap::Get();
        const LN_InputMapOperation operation = LN_InputMapOperation(instruction.int_value);
        if (operation == LN_InputMapOperation::Rebind) {
          const std::string action = ResolveStringExpression(
              instruction.string_expr_index, std::string(), context);
          if (m_logicManager == nullptr) {
            CompletePersistenceOperation(instruction.bool_expr_index, false);
            break;
          }
          /* secondary_int_value: the device class, and the direction in its second byte. */
          m_logicManager->StartInputRebind(
              this,
              instruction.bool_expr_index,
              instruction.source_ref_index,
              map.Find(action),
              ResolveIntExpression(instruction.int_expr_index, 0, context),
              LN_InputDeviceClass(instruction.secondary_int_value & 0xff),
              instruction.secondary_int_value >> 8,
              ResolveFloatExpression(instruction.float_expr_index, 5.0f, context));
          break;
        }
        bool success = true;
        if (operation == LN_InputMapOperation::Reset) {
          map.Reset();
        }
        else {
          const std::string filepath = ResolveVariableFilePath(
              ResolveStringExpression(instruction.string_expr_index, "//Data", context),
              ResolveStringExpression(
                  instruction.secondary_string_expr_index, "input_map", context));
          if (operation == LN_InputMapOperation::Save) {
            /* Write a temporary file and replace the old one only when all of it was written. */
            std::error_code error;
            std::filesystem::create_directories(std::filesystem::path(filepath).parent_path(),
                                                error);
            const std::string temporary = filepath + ".tmp";
            {
              std::ofstream stream(temporary, std::ios::trunc);
              stream << map.Save();
              stream.close();
              success = !stream.fail();
            }
            if (success) {
              std::filesystem::rename(temporary, filepath, error);
              success = !error;
            }
            if (!success) {
              std::filesystem::remove(temporary, error);
            }
          }
          else {
            std::ifstream stream(filepath);
            std::stringstream contents;
            contents << stream.rdbuf();
            success = stream.is_open() && map.Restore(contents.str());
          }
        }
        CompletePersistenceOperation(instruction.bool_expr_index, success);
        break;
      }
      default:
        return;
    }
    command_sequence++;
    MarkInstructionExecuted(event, instruction_id, context.tick_index);
  };

  auto execute_physics_constraint_instruction = [&](const LN_Instruction &instruction,
                                                    const uint32_t instruction_id) {
    switch (instruction.opcode) {
      case LN_OpCode::AddPhysicsConstraint: {
        const LN_RigidBodyConstraintCommandPayload payload = rigid_body_constraint_command_payload(
            instruction);
        KX_GameObject *target = resolve_required_object(payload.object_value_expr_index);
        KX_GameObject *child = resolve_required_object(payload.target_value_expr_index);
        if (target == nullptr && m_gameObject != nullptr &&
            m_gameObject->GetPhysicsController() != nullptr)
        {
          target = m_gameObject;
        }
        KX_GameObject *constraint_object = nullptr;
        if (payload.constraint_object_value_expr_index != LN_INVALID_INDEX) {
          constraint_object = resolve_required_object(payload.constraint_object_value_expr_index);
        }
        else if (m_gameObject != nullptr && m_gameObject != target && m_gameObject != child) {
          constraint_object = m_gameObject;
        }
        KX_Scene *scene = m_gameObject ? m_gameObject->GetScene() : nullptr;
        PHY_IPhysicsEnvironment *environment = scene ? scene->GetPhysicsEnvironment() : nullptr;
        const bool use_jolt = scene && scene->GetBlenderScene() &&
                              static_cast<e_PhysicsEngine>(
                                  scene->GetBlenderScene()->gm.physicsEngine) == UseJolt;
        if (target != nullptr && child != nullptr && target != child && environment != nullptr &&
            use_jolt)
        {
          PHY_IPhysicsController *ctrl1 = target->GetPhysicsController();
          PHY_IPhysicsController *ctrl2 = child->GetPhysicsController();
          if (ctrl1 != nullptr && ctrl2 != nullptr) {
            auto bool_input = [&](const LN_RigidBodyConstraintBoolInput input,
                                  const bool fallback) {
              return ResolveBoolExpression(
                  payload.bool_expr_indices[size_t(input)], fallback, context);
            };
            auto vector_input = [&](const LN_RigidBodyConstraintVectorInput input,
                                    const MT_Vector3 &fallback) {
              return ResolveVectorExpression(
                  payload.vector_expr_indices[size_t(input)], fallback, context);
            };
            auto float_input = [&](const LN_RigidBodyConstraintFloatInput input,
                                   const float fallback) {
              return ResolveFloatExpression(
                  payload.float_expr_indices[size_t(input)], fallback, context);
            };

            MT_Vector3 pivot;
            MT_Matrix3x3 basis;
            if (constraint_object != nullptr) {
              const MT_Matrix3x3 target_inv = target->NodeGetWorldOrientation().inverse();
              pivot = target_inv *
                      (constraint_object->NodeGetWorldPosition() - target->NodeGetWorldPosition());
              basis = target_inv * constraint_object->NodeGetWorldOrientation();
            }
            else {
              pivot = vector_input(LN_RigidBodyConstraintVectorInput::Pivot,
                                   MT_Vector3(0.0f, 0.0f, 0.0f));
              basis = ResolveRotationExpression(payload.vector_expr_indices[size_t(
                                                    LN_RigidBodyConstraintVectorInput::Rotation)],
                                                LN_Rotation::Identity(),
                                                context)
                          .ToMatrix();
              if (bool_input(LN_RigidBodyConstraintBoolInput::UseWorldSpace, false)) {
                const MT_Matrix3x3 target_inv = target->NodeGetWorldOrientation().inverse();
                pivot = target_inv * (pivot - target->NodeGetWorldPosition());
                basis = target_inv * basis;
              }
            }

            PHY_RigidBodyConstraintSettings settings;
            settings.type = payload.type;
            settings.spring_type = payload.spring_type;
            settings.flags = 0;
            if (bool_input(LN_RigidBodyConstraintBoolInput::Enabled, true)) {
              settings.flags |= PHY_RB_CONSTRAINT_ENABLED;
            }
            if (bool_input(LN_RigidBodyConstraintBoolInput::DisableCollisions, true)) {
              settings.flags |= PHY_RB_CONSTRAINT_DISABLE_COLLISIONS;
            }
            if (settings.type != PHY_RigidBodyConstraintType::Motor &&
                bool_input(LN_RigidBodyConstraintBoolInput::Breakable, false))
            {
              settings.flags |= PHY_RB_CONSTRAINT_USE_BREAKING;
            }
            if (bool_input(LN_RigidBodyConstraintBoolInput::OverrideIterations, false)) {
              settings.flags |= PHY_RB_CONSTRAINT_JOLT_OVERRIDE_SOLVER_ITERATIONS;
            }
            if (bool_input(LN_RigidBodyConstraintBoolInput::UseLimitLinX, false)) {
              settings.flags |= PHY_RB_CONSTRAINT_USE_LIMIT_LIN_X;
            }
            if (bool_input(LN_RigidBodyConstraintBoolInput::UseLimitLinY, false)) {
              settings.flags |= PHY_RB_CONSTRAINT_USE_LIMIT_LIN_Y;
            }
            if (bool_input(LN_RigidBodyConstraintBoolInput::UseLimitLinZ, false)) {
              settings.flags |= PHY_RB_CONSTRAINT_USE_LIMIT_LIN_Z;
            }
            if (bool_input(LN_RigidBodyConstraintBoolInput::UseLimitAngX, false)) {
              settings.flags |= PHY_RB_CONSTRAINT_USE_LIMIT_ANG_X;
            }
            if (bool_input(LN_RigidBodyConstraintBoolInput::UseLimitAngY, false)) {
              settings.flags |= PHY_RB_CONSTRAINT_USE_LIMIT_ANG_Y;
            }
            if (bool_input(LN_RigidBodyConstraintBoolInput::UseLimitAngZ, false)) {
              settings.flags |= PHY_RB_CONSTRAINT_USE_LIMIT_ANG_Z;
            }
            if (bool_input(LN_RigidBodyConstraintBoolInput::UseSpringX, false)) {
              settings.flags |= PHY_RB_CONSTRAINT_USE_SPRING_X;
            }
            if (bool_input(LN_RigidBodyConstraintBoolInput::UseSpringY, false)) {
              settings.flags |= PHY_RB_CONSTRAINT_USE_SPRING_Y;
            }
            if (bool_input(LN_RigidBodyConstraintBoolInput::UseSpringZ, false)) {
              settings.flags |= PHY_RB_CONSTRAINT_USE_SPRING_Z;
            }
            if (bool_input(LN_RigidBodyConstraintBoolInput::UseSpringAngX, false)) {
              settings.flags |= PHY_RB_CONSTRAINT_USE_SPRING_ANG_X;
            }
            if (bool_input(LN_RigidBodyConstraintBoolInput::UseSpringAngY, false)) {
              settings.flags |= PHY_RB_CONSTRAINT_USE_SPRING_ANG_Y;
            }
            if (bool_input(LN_RigidBodyConstraintBoolInput::UseSpringAngZ, false)) {
              settings.flags |= PHY_RB_CONSTRAINT_USE_SPRING_ANG_Z;
            }
            if (bool_input(LN_RigidBodyConstraintBoolInput::UseMotorLin, false)) {
              settings.flags |= PHY_RB_CONSTRAINT_USE_MOTOR_LIN;
            }
            if (bool_input(LN_RigidBodyConstraintBoolInput::UseMotorAng, false)) {
              settings.flags |= PHY_RB_CONSTRAINT_USE_MOTOR_ANG;
            }

            settings.jolt_velocity_solver_iterations = std::clamp(
                ResolveIntExpression(payload.velocity_solver_iterations_int_expr_index,
                                     settings.jolt_velocity_solver_iterations,
                                     context),
                1,
                255);
            settings.jolt_position_solver_iterations = std::clamp(
                ResolveIntExpression(payload.position_solver_iterations_int_expr_index,
                                     settings.jolt_position_solver_iterations,
                                     context),
                1,
                255);
            settings.breaking_threshold = float_input(
                LN_RigidBodyConstraintFloatInput::BreakingThreshold, settings.breaking_threshold);
            const MT_Vector3 linear_lower = vector_input(
                LN_RigidBodyConstraintVectorInput::LinearLower,
                MT_Vector3(settings.limit_lin_x_lower,
                           settings.limit_lin_y_lower,
                           settings.limit_lin_z_lower));
            const MT_Vector3 linear_upper = vector_input(
                LN_RigidBodyConstraintVectorInput::LinearUpper,
                MT_Vector3(settings.limit_lin_x_upper,
                           settings.limit_lin_y_upper,
                           settings.limit_lin_z_upper));
            const MT_Vector3 angular_lower = vector_input(
                LN_RigidBodyConstraintVectorInput::AngularLower,
                MT_Vector3(settings.limit_ang_x_lower,
                           settings.limit_ang_y_lower,
                           settings.limit_ang_z_lower));
            const MT_Vector3 angular_upper = vector_input(
                LN_RigidBodyConstraintVectorInput::AngularUpper,
                MT_Vector3(settings.limit_ang_x_upper,
                           settings.limit_ang_y_upper,
                           settings.limit_ang_z_upper));
            const MT_Vector3 spring_stiffness = vector_input(
                LN_RigidBodyConstraintVectorInput::SpringStiffness,
                MT_Vector3(settings.spring_stiffness_x,
                           settings.spring_stiffness_y,
                           settings.spring_stiffness_z));
            const MT_Vector3 spring_damping = vector_input(
                LN_RigidBodyConstraintVectorInput::SpringDamping,
                MT_Vector3(settings.spring_damping_x,
                           settings.spring_damping_y,
                           settings.spring_damping_z));
            const MT_Vector3 angular_spring_stiffness = vector_input(
                LN_RigidBodyConstraintVectorInput::AngularSpringStiffness,
                MT_Vector3(settings.spring_stiffness_ang_x,
                           settings.spring_stiffness_ang_y,
                           settings.spring_stiffness_ang_z));
            const MT_Vector3 angular_spring_damping = vector_input(
                LN_RigidBodyConstraintVectorInput::AngularSpringDamping,
                MT_Vector3(settings.spring_damping_ang_x,
                           settings.spring_damping_ang_y,
                           settings.spring_damping_ang_z));

            settings.limit_lin_x_lower = linear_lower.x();
            settings.limit_lin_y_lower = linear_lower.y();
            settings.limit_lin_z_lower = linear_lower.z();
            settings.limit_lin_x_upper = linear_upper.x();
            settings.limit_lin_y_upper = linear_upper.y();
            settings.limit_lin_z_upper = linear_upper.z();
            settings.limit_ang_x_lower = angular_lower.x();
            settings.limit_ang_y_lower = angular_lower.y();
            settings.limit_ang_z_lower = angular_lower.z();
            settings.limit_ang_x_upper = angular_upper.x();
            settings.limit_ang_y_upper = angular_upper.y();
            settings.limit_ang_z_upper = angular_upper.z();
            settings.spring_stiffness_x = spring_stiffness.x();
            settings.spring_stiffness_y = spring_stiffness.y();
            settings.spring_stiffness_z = spring_stiffness.z();
            settings.spring_damping_x = spring_damping.x();
            settings.spring_damping_y = spring_damping.y();
            settings.spring_damping_z = spring_damping.z();
            settings.spring_stiffness_ang_x = angular_spring_stiffness.x();
            settings.spring_stiffness_ang_y = angular_spring_stiffness.y();
            settings.spring_stiffness_ang_z = angular_spring_stiffness.z();
            settings.spring_damping_ang_x = angular_spring_damping.x();
            settings.spring_damping_ang_y = angular_spring_damping.y();
            settings.spring_damping_ang_z = angular_spring_damping.z();
            settings.motor_lin_target_velocity = float_input(
                LN_RigidBodyConstraintFloatInput::MotorLinTargetVelocity,
                settings.motor_lin_target_velocity);
            settings.motor_lin_max_impulse = float_input(
                LN_RigidBodyConstraintFloatInput::MotorLinMaxImpulse,
                settings.motor_lin_max_impulse);
            settings.motor_ang_target_velocity = float_input(
                LN_RigidBodyConstraintFloatInput::MotorAngTargetVelocity,
                settings.motor_ang_target_velocity);
            settings.motor_ang_max_impulse = float_input(
                LN_RigidBodyConstraintFloatInput::MotorAngMaxImpulse,
                settings.motor_ang_max_impulse);

            std::string constraint_name = ResolveStringExpression(
                payload.name_string_expr_index, "constraint", context);
            if (constraint_name.empty()) {
              constraint_name = "constraint";
            }

            const int constraint_id = environment->CreateRigidBodyConstraint(
                target, child, pivot, basis, settings, constraint_name);
            if (constraint_id != -1) {
              target->AddRuntimeRigidBodyConstraint(
                  constraint_name, target, child, settings, pivot, basis, constraint_id);
            }
          }
        }
        break;
      }
      case LN_OpCode::RemovePhysicsConstraint: {
        if (KX_GameObject *target = resolve_object_expression(instruction.value_expr_index)) {
          if (!UsesJoltPhysics(target)) {
            break;
          }
          if (ResolveBoolExpression(instruction.bool_expr_index, false, context)) {
            target->RemoveRigidBodyConstraints();
            break;
          }
          std::string name = ResolveStringExpression(
              instruction.string_expr_index, "constraint", context);
          if (name.empty()) {
            name = "constraint";
          }
          target->RemoveRigidBodyConstraint(name);
        }
        break;
      }
      default:
        return;
    }
    MarkInstructionExecuted(event, instruction_id, context.tick_index);
  };

  auto execute_navigation_instruction = [&](const LN_Instruction &instruction,
                                            const uint32_t instruction_id) {
    switch (instruction.opcode) {
      case LN_OpCode::Navigate: {
        if (KX_GameObject *target = resolve_command_target(instruction)) {
          const MT_Vector3 destination = ResolveVectorExpression(
              instruction.vector_expr_index, MT_Vector3(0.0f, 0.0f, 0.0f), context);
          const float speed = ResolveFloatExpression(instruction.float_expr_index, 1.0f, context);
          const float threshold = ResolveFloatExpression(
              instruction.secondary_float_expr_index, 1.0f, context);
          const bool move_dynamic = ResolveBoolExpression(
              instruction.bool_expr_index, false, context);
          const bool look_at = ResolveBoolExpression(
              instruction.secondary_bool_expr_index, true, context);
          const bool visualize = ResolveBoolExpression(
              instruction.tertiary_bool_expr_index, false, context);
          const int32_t rot_axis = ResolveIntExpression(instruction.int_expr_index, 2, context);
          const int32_t front_axis = ResolveIntExpression(
              instruction.secondary_int_expr_index, 1, context);
          const float rot_speed = ResolveFloatExpression(
              instruction.tertiary_float_expr_index, 1.0f, context);
          KX_NavMeshObject *navmesh = nullptr;
          if (instruction.secondary_value_expr_index != LN_INVALID_INDEX) {
            const LN_Value navmesh_value = ResolveValueExpression(
                instruction.secondary_value_expr_index, MakeNoneValue(), context);
            navmesh = ResolveNavMeshObject(ResolveObjectValue(navmesh_value));
          }
          KX_GameObject *rotating_object = target;
          if (instruction.tertiary_value_expr_index != LN_INVALID_INDEX) {
            if (KX_GameObject *input_object = ResolveObjectValue(ResolveValueExpression(
                    instruction.tertiary_value_expr_index, MakeNoneValue(), context)))
            {
              rotating_object = input_object;
            }
          }
          const uint32_t state_index = InstructionStateIndex(event, instruction_id);
          if (state_index < m_instructionStates.size()) {
            InstructionRuntimeState &state = m_instructionStates[state_index];
            MT_Vector3 move_target;
            bool reached = false;
            if (ResolvePathMoveTarget(
                    state, target, navmesh, destination, threshold, move_target, reached))
            {
              if (visualize) {
                DrawNavigationPath(target, state, destination);
              }
              command_buffer.AppendMoveToward(target,
                                              move_target,
                                              speed,
                                              threshold,
                                              move_dynamic,
                                              true,
                                              float(context.delta_time),
                                              command_sort_key(instruction_id, command_sequence),
                                              instruction.source_ref_index);
              command_sequence++;
              if (look_at && rotating_object != nullptr) {
                command_buffer.AppendRotateToward(
                    rotating_object,
                    move_target,
                    std::max(0.0f, rot_speed) * float(std::max(0.0, context.delta_time)),
                    rot_axis,
                    front_axis,
                    true,
                    command_sort_key(instruction_id, command_sequence),
                    instruction.source_ref_index);
              }
            }
            if (reached) {
              state.last_reached_tick = context.tick_index;
              state.last_reached_scope_serial = m_activeExecutionScopeSerial;
            }
          }
        }
        command_sequence++;
        MarkInstructionExecuted(event, instruction_id, context.tick_index);
        break;
      }
      case LN_OpCode::FollowPath: {
        if (KX_GameObject *target = resolve_command_target(instruction)) {
          const LN_Value path_value = ResolveValueExpression(
              instruction.secondary_value_expr_index, MakeNoneValue(), context);
          std::vector<MT_Vector3> path_points;
          if (!CollectPathPoints(path_value, path_points)) {
            return;
          }
          const bool loop = ResolveBoolExpression(instruction.bool_expr_index, false, context);
          const bool move_dynamic = ResolveBoolExpression(
              instruction.tertiary_bool_expr_index, false, context);
          const bool look_at = ResolveBoolExpression(
              instruction.quaternary_bool_expr_index, true, context);
          const float speed = ResolveFloatExpression(instruction.float_expr_index, 1.0f, context);
          const float threshold = ResolveFloatExpression(
              instruction.secondary_float_expr_index, 0.2f, context);
          const float rot_speed = ResolveFloatExpression(
              instruction.tertiary_float_expr_index, 1.0f, context);
          const int32_t rot_axis = ResolveIntExpression(instruction.int_expr_index, 2, context);
          const int32_t front_axis = ResolveIntExpression(
              instruction.secondary_int_expr_index, 1, context);
          KX_NavMeshObject *navmesh = nullptr;
          if (instruction.tertiary_value_expr_index != LN_INVALID_INDEX) {
            navmesh = ResolveNavMeshObject(ResolveObjectValue(ResolveValueExpression(
                instruction.tertiary_value_expr_index, MakeNoneValue(), context)));
          }
          KX_GameObject *rotating_object = target;
          if (instruction.quaternary_value_expr_index != LN_INVALID_INDEX) {
            if (KX_GameObject *input_object = ResolveObjectValue(ResolveValueExpression(
                    instruction.quaternary_value_expr_index, MakeNoneValue(), context)))
            {
              rotating_object = input_object;
            }
          }

          const uint32_t state_index = InstructionStateIndex(event, instruction_id);
          if (state_index >= m_instructionStates.size()) {
            return;
          }
          InstructionRuntimeState &state = m_instructionStates[state_index];
          if (state.list_point_index < 0 || state.list_point_index >= int(path_points.size())) {
            state.list_point_index = 0;
          }

          MT_Vector3 move_target;
          while (state.list_point_index < int(path_points.size())) {
            const MT_Vector3 &point = path_points[size_t(state.list_point_index)];
            bool reached = false;
            if (ResolvePathMoveTarget(
                    state, target, navmesh, point, threshold, move_target, reached))
            {
              command_buffer.AppendMoveToward(target,
                                              move_target,
                                              speed,
                                              threshold,
                                              move_dynamic,
                                              true,
                                              float(context.delta_time),
                                              command_sort_key(instruction_id, command_sequence),
                                              instruction.source_ref_index);
              command_sequence++;
              if (look_at && rotating_object != nullptr) {
                command_buffer.AppendRotateToward(
                    rotating_object,
                    move_target,
                    std::max(0.0f, rot_speed) * float(std::max(0.0, context.delta_time)),
                    rot_axis,
                    front_axis,
                    true,
                    command_sort_key(instruction_id, command_sequence),
                    instruction.source_ref_index);
              }
              break;
            }
            if (reached) {
              state.last_reached_tick = context.tick_index;
              state.last_reached_scope_serial = m_activeExecutionScopeSerial;
            }
            state.list_point_index++;
            state.path_len = 0;
            state.waypoint_index = -1;
          }

          if (state.list_point_index >= int(path_points.size()) && loop && !path_points.empty()) {
            state.list_point_index = 0;
            const MT_Vector3 &point = path_points[0];
            bool reached = false;
            if (ResolvePathMoveTarget(
                    state, target, navmesh, point, threshold, move_target, reached))
            {
              command_buffer.AppendMoveToward(target,
                                              move_target,
                                              speed,
                                              threshold,
                                              move_dynamic,
                                              true,
                                              float(context.delta_time),
                                              command_sort_key(instruction_id, command_sequence),
                                              instruction.source_ref_index);
              command_sequence++;
              if (look_at && rotating_object != nullptr) {
                command_buffer.AppendRotateToward(
                    rotating_object,
                    move_target,
                    std::max(0.0f, rot_speed) * float(std::max(0.0, context.delta_time)),
                    rot_axis,
                    front_axis,
                    true,
                    command_sort_key(instruction_id, command_sequence),
                    instruction.source_ref_index);
              }
            }
            if (reached) {
              state.last_reached_tick = context.tick_index;
              state.last_reached_scope_serial = m_activeExecutionScopeSerial;
            }
          }
        }
        command_sequence++;
        MarkInstructionExecuted(event, instruction_id, context.tick_index);
        break;
      }
      default:
        break;
    }
  };

  auto execute_scene_collection_instruction = [&](const LN_Instruction &instruction,
                                                  const uint32_t instruction_id) {
    bool instruction_executed = false;
    auto warn_scene_collection_request = [&](const LN_CommandBuffer::CommandType command_type,
                                             const std::string &reason) {
      LN_CommandBuffer::Command diagnostic;
      diagnostic.type = command_type;
      diagnostic.object = m_gameObject;
      diagnostic.runtime_tree = this;
      diagnostic.sort_key = command_sort_key(instruction_id, command_sequence);
      diagnostic.source_ref_index = instruction.source_ref_index;
      LN_WarnCommandFailureOnce(diagnostic, reason);
    };
    switch (instruction.opcode) {
      case LN_OpCode::SetCollectionVisibility: {
        const LN_Value collection_value = ResolveValueExpression(
            instruction.value_expr_index, MakeNoneValue(), context);
        blender::Collection *collection = nullptr;
        if (collection_value.type == LN_ValueType::CollectionRef &&
            collection_value.runtime_ref.IsValid())
        {
          collection = ResolveCollectionRef(collection_value.runtime_ref);
        }
        KX_Scene *scene = m_gameObject ? m_gameObject->GetScene() : nullptr;
        if (scene == nullptr || collection == nullptr) {
          warn_scene_collection_request(
              LN_CommandBuffer::CommandType::SetVisibility,
              "collection did not resolve in the logic-tree owner's scene");
          break;
        }
        const bool visible = ResolveBoolExpression(instruction.bool_expr_index, true, context);
        const bool include_child_collections = ResolveBoolExpression(
            instruction.secondary_bool_expr_index, false, context);
        if (!scene->SetRuntimeCollectionRenderVisibility(
                collection, visible, include_child_collections))
        {
          warn_scene_collection_request(
              LN_CommandBuffer::CommandType::SetVisibility,
              "collection is not linked to the logic-tree owner's runtime scene");
          break;
        }
        if (m_logicManager != nullptr) {
          m_logicManager->InvalidatePointerHitCache();
        }
        instruction_executed = true;
        break;
      }
      case LN_OpCode::SetCollectionMember: {
        const LN_Value collection_value = ResolveValueExpression(
            instruction.value_expr_index, MakeNoneValue(), context);
        blender::Collection *collection = nullptr;
        if (collection_value.type == LN_ValueType::CollectionRef &&
            collection_value.runtime_ref.IsValid())
        {
          collection = ResolveCollectionRef(collection_value.runtime_ref);
        }
        /* Object: empty is the tree's own object. */
        KX_GameObject *target = instruction.secondary_value_expr_index != LN_INVALID_INDEX ?
                                    ResolveObjectValue(ResolveValueExpression(
                                        instruction.secondary_value_expr_index,
                                        MakeNoneValue(),
                                        context)) :
                                    m_gameObject;
        KX_Scene *scene = target ? target->GetScene() : nullptr;
        if (scene == nullptr || collection == nullptr) {
          warn_scene_collection_request(
              LN_CommandBuffer::CommandType::SetVisibility,
              "Set Collection Member: the object or the collection did not resolve");
          break;
        }
        const bool member = ResolveBoolExpression(instruction.bool_expr_index, true, context);
        const bool include_children = ResolveBoolExpression(
            instruction.secondary_bool_expr_index, false, context);
        std::vector<KX_GameObject *> objects = {target};
        if (include_children) {
          const std::vector<KX_GameObject *> children = target->GetChildrenRecursive();
          objects.insert(objects.end(), children.begin(), children.end());
        }
        bool changed = true;
        for (KX_GameObject *object : objects) {
          changed &= scene->SetCollectionMember(object, collection, member);
        }
        if (!changed) {
          warn_scene_collection_request(
              LN_CommandBuffer::CommandType::SetVisibility,
              "Set Collection Member: the collection is not part of the object's scene");
          break;
        }
        if (m_logicManager != nullptr) {
          m_logicManager->InvalidatePointerHitCache();
        }
        instruction_executed = true;
        break;
      }
      case LN_OpCode::SetOverlayCollection: {
        const LN_Value camera_value = instruction.value_expr_index != LN_INVALID_INDEX ?
                                          ResolveValueExpression(instruction.value_expr_index,
                                                                 MakeNoneValue(),
                                                                 context) :
                                          MakeNoneValue();
        const LN_Value collection_value = ResolveValueExpression(
            instruction.secondary_value_expr_index, MakeNoneValue(), context);
        KX_Camera *camera = dynamic_cast<KX_Camera *>(ResolveObjectValue(camera_value, true));
        blender::Collection *collection = nullptr;
        if (collection_value.type == LN_ValueType::CollectionRef &&
            collection_value.runtime_ref.IsValid())
        {
          collection = ResolveCollectionRef(collection_value.runtime_ref);
        }
        KX_Scene *scene = m_gameObject ? m_gameObject->GetScene() : nullptr;
        auto complete_overlay = [&](const LN_UIResult result) {
          CompleteUIOperation(instruction.property_ref_index, result, 0);
          instruction_executed = true;
        };
        if (m_gameObject == nullptr || scene == nullptr) {
          complete_overlay(LN_UIResult::RuntimeUnavailable);
          warn_scene_collection_request(LN_CommandBuffer::CommandType::SetOverlayCollection,
                                        LN_UIResultMessage(LN_UIResult::RuntimeUnavailable));
          break;
        }
        if (collection == nullptr) {
          complete_overlay(LN_UIResult::InvalidCollection);
          warn_scene_collection_request(LN_CommandBuffer::CommandType::SetOverlayCollection,
                                        LN_UIResultMessage(LN_UIResult::InvalidCollection));
          break;
        }
        const KX_OverlayCollectionResult validation = scene->ValidateAddOverlayCollection(
            camera, collection);
        if (validation == KX_OverlayCollectionResult::NoChange) {
          complete_overlay(LN_UIResult::NoChange);
          break;
        }
        if (!KX_OverlayCollectionResultSucceeded(validation)) {
          complete_overlay(LN_MapOverlayResult(validation));
          warn_scene_collection_request(LN_CommandBuffer::CommandType::SetOverlayCollection,
                                        KX_OverlayCollectionResultMessage(validation));
          break;
        }
        command_buffer.AppendSetOverlayCollection(
            this,
            m_gameObject,
            camera,
            collection,
            instruction.property_ref_index,
            command_sort_key(instruction_id, command_sequence),
            instruction.source_ref_index);
        instruction_executed = true;
        break;
      }
      case LN_OpCode::RemoveOverlayCollection: {
        const LN_Value collection_value = ResolveValueExpression(
            instruction.value_expr_index, MakeNoneValue(), context);
        blender::Collection *collection = nullptr;
        if (collection_value.type == LN_ValueType::CollectionRef &&
            collection_value.runtime_ref.IsValid())
        {
          collection = ResolveCollectionRef(collection_value.runtime_ref);
        }
        KX_Scene *scene = m_gameObject ? m_gameObject->GetScene() : nullptr;
        auto complete_overlay = [&](const LN_UIResult result) {
          CompleteUIOperation(instruction.property_ref_index, result, 0);
          instruction_executed = true;
        };
        if (m_gameObject == nullptr || scene == nullptr) {
          complete_overlay(LN_UIResult::RuntimeUnavailable);
          warn_scene_collection_request(LN_CommandBuffer::CommandType::RemoveOverlayCollection,
                                        LN_UIResultMessage(LN_UIResult::RuntimeUnavailable));
          break;
        }
        if (collection == nullptr) {
          complete_overlay(LN_UIResult::InvalidCollection);
          warn_scene_collection_request(LN_CommandBuffer::CommandType::RemoveOverlayCollection,
                                        LN_UIResultMessage(LN_UIResult::InvalidCollection));
          break;
        }
        const KX_OverlayCollectionResult validation = scene->ValidateRemoveOverlayCollection(
            collection);
        if (validation == KX_OverlayCollectionResult::NoChange) {
          complete_overlay(LN_UIResult::NoChange);
          break;
        }
        if (!KX_OverlayCollectionResultSucceeded(validation)) {
          complete_overlay(LN_MapOverlayResult(validation));
          warn_scene_collection_request(LN_CommandBuffer::CommandType::RemoveOverlayCollection,
                                        KX_OverlayCollectionResultMessage(validation));
          break;
        }
        command_buffer.AppendRemoveOverlayCollection(
            this,
            m_gameObject,
            collection,
            instruction.property_ref_index,
            command_sort_key(instruction_id, command_sequence),
            instruction.source_ref_index);
        instruction_executed = true;
        break;
      }
      case LN_OpCode::OpenObjectMenu: {
        KX_Camera *camera = instruction.value_expr_index != LN_INVALID_INDEX ?
                                dynamic_cast<KX_Camera *>(ResolveObjectValue(
                                    ResolveValueExpression(
                                        instruction.value_expr_index, MakeNoneValue(), context),
                                    true)) :
                                nullptr;
        const LN_Value collection_value = ResolveValueExpression(
            instruction.secondary_value_expr_index, MakeNoneValue(), context);
        blender::Collection *collection = collection_value.type == LN_ValueType::CollectionRef &&
                                                  collection_value.runtime_ref.IsValid() ?
                                              ResolveCollectionRef(collection_value.runtime_ref) :
                                              nullptr;
        if (m_gameObject == nullptr || collection == nullptr) {
          CompleteUIOperation(instruction.property_ref_index,
                              m_gameObject == nullptr ? LN_UIResult::InvalidOwner :
                                                        LN_UIResult::InvalidCollection,
                              0);
          instruction_executed = true;
          break;
        }
        /* Owner: whose menu it is (empty: the opener). */
        KX_GameObject *menu_owner = instruction.tertiary_value_expr_index != LN_INVALID_INDEX ?
                                        ResolveObjectValue(ResolveValueExpression(
                                            instruction.tertiary_value_expr_index,
                                            MakeNoneValue(),
                                            context)) :
                                        nullptr;
        /* From inside a menu: for that menu's player (each player's copy runs this tree). */
        const int32_t menu_user =
            m_logicManager != nullptr ?
                m_logicManager->GetUIService().MenuUserOfObject(m_gameObject) :
                -1;
        command_buffer.AppendOpenObjectMenu(
            this,
            m_gameObject,
            camera,
            menu_owner,
            collection,
            static_cast<LN_UIMenuRouting>(std::clamp(instruction.int_value,
                                                     int32_t(LN_UIMenuRouting::Modal),
                                                     int32_t(LN_UIMenuRouting::PassThrough))),
            ResolveStringExpression(instruction.string_expr_index, std::string(), context),
            LN_PackUIMenuUser(
                instruction.secondary_int_value,
                menu_user >= 0 ?
                    menu_user :
                    std::clamp(ResolveIntExpression(instruction.int_expr_index, 0, context),
                               0,
                               LN_MAX_GAMEPADS - 1),
                24),
            instruction.property_ref_index,
            command_sort_key(instruction_id, command_sequence),
            instruction.source_ref_index);
        instruction_executed = true;
        break;
      }
      case LN_OpCode::CloseObjectMenu: {
        if (m_gameObject == nullptr) {
          CompleteUIOperation(instruction.property_ref_index, LN_UIResult::InvalidOwner, 0);
          instruction_executed = true;
          break;
        }
        /* From inside a menu: that menu's player's menus. */
        const int32_t menu_user =
            m_logicManager != nullptr ?
                m_logicManager->GetUIService().MenuUserOfObject(m_gameObject) :
                -1;
        command_buffer.AppendCloseObjectMenu(
            this,
            m_gameObject,
            ResolveIntExpression(instruction.int_expr_index, 0, context),
            LN_UIMenuCloseOperation(std::clamp(instruction.int_value,
                                               int32_t(LN_UIMenuCloseOperation::Top),
                                               int32_t(LN_UIMenuCloseOperation::All))),
            menu_user >= 0 ?
                menu_user :
                std::clamp(ResolveIntExpression(instruction.secondary_int_expr_index, 0, context),
                           0,
                           LN_MAX_GAMEPADS - 1),
            instruction.property_ref_index,
            command_sort_key(instruction_id, command_sequence),
            instruction.source_ref_index);
        instruction_executed = true;
        break;
      }
      case LN_OpCode::InventoryEdit: {
        /* Read the inventory property, change it and write it back at once: a second inventory
         * change later in this tick works on this one's result. */
        instruction_executed = true;
        KX_GameObject *target = instruction.value_expr_index != LN_INVALID_INDEX ?
                                    ResolveLiveObjectValue(ResolveValueExpression(
                                        instruction.value_expr_index, MakeNoneValue(), context)) :
                                    m_gameObject;
        const std::string property = ResolveStringExpression(
            instruction.string_expr_index, std::string(), context);
        const std::vector<LN_ValueExpression> &expressions = m_program->GetValueExpressions();
        if (instruction.secondary_value_expr_index >= expressions.size()) {
          break;
        }
        const LN_ValueExpression &edit = expressions[instruction.secondary_value_expr_index];
        /* A missing property starts empty; one of another kind is not an inventory. */
        std::string problem;
        const auto read_inventory = [&](KX_GameObject *object, const std::string &name) {
          LN_Value list;
          list.type = LN_ValueType::List;
          list.exists = true;
          if (object == nullptr || name.empty()) {
            problem = "Inventory needs an object and a property name";
          }
          else if (EXP_Value *current = object->GetProperty(name)) {
            LN_Value value = MakeValueFromEXPValue(current, this);
            if (value.exists && value.type == LN_ValueType::List) {
              list = std::move(value);
            }
            else {
              problem = "game property '" + name + "' is not a List or Table";
            }
          }
          return list;
        };
        LN_Value inventory = read_inventory(target, property);
        /* Move To Slot into another inventory (To Property; To Object empty: the same object). */
        KX_GameObject *other_target = nullptr;
        std::string other_property;
        if (LN_InventoryOperation(edit.value.int_value) == LN_InventoryOperation::MoveToSlot &&
            edit.input_indices.size() > LN_INVENTORY_INPUT_TO_PROPERTY)
        {
          const LN_Value name = ResolveValueExpression(
              edit.input_indices[LN_INVENTORY_INPUT_TO_PROPERTY], MakeNoneValue(), context);
          other_property = name.exists && name.type == LN_ValueType::String ? name.string_value :
                                                                              std::string();
          const LN_Value object = ResolveValueExpression(
              edit.input_indices[LN_INVENTORY_INPUT_TO_OBJECT], MakeNoneValue(), context);
          other_target = object.exists && object.type == LN_ValueType::ObjectRef ?
                             ResolveLiveObjectValue(object) :
                             target;
          if (other_target == target && other_property == property) {
            other_property.clear();
          }
        }
        LN_Value other_inventory;
        if (!other_property.empty()) {
          other_inventory = read_inventory(other_target, other_property);
        }
        const std::vector<LN_Value> before = inventory.list_value;
        const std::vector<LN_Value> other_before = other_inventory.list_value;
        /* Each list follows its own Inventory Rules (unless the node overrides them). */
        const LN_InventoryRules list_rules = m_logicManager != nullptr && problem.empty() ?
                                                 m_logicManager->InventoryRulesOf(target,
                                                                                  property) :
                                                 LN_InventoryRules();
        const LN_InventoryRules other_list_rules =
            m_logicManager != nullptr && !other_property.empty() ?
                m_logicManager->InventoryRulesOf(other_target, other_property) :
                LN_InventoryRules();
        InventoryEditResult result = RunInventoryEdit(
            edit,
            inventory.list_value,
            context,
            other_property.empty() ? nullptr : &other_inventory.list_value,
            &list_rules,
            &other_list_rules,
            target);
        std::string reason;
        /* The receiving inventory first: if the source then cannot be written, it is put back. */
        if (problem.empty() && !other_property.empty() &&
            !LN_CommandBuffer::ApplyGamePropertyValue(
                other_target, other_property, other_inventory, this, reason))
        {
          problem = reason;
        }
        if (problem.empty() &&
            !LN_CommandBuffer::ApplyGamePropertyValue(target, property, inventory, this, reason))
        {
          problem = reason;
          if (!other_property.empty()) {
            other_inventory.list_value = other_before;
            LN_CommandBuffer::ApplyGamePropertyValue(
                other_target, other_property, other_inventory, this, reason);
          }
        }
        if (!problem.empty()) {
          /* Nothing was kept: the outputs say nothing changed (Add Item: all left over). */
          warn_scene_collection_request(LN_CommandBuffer::CommandType::SetGameProperty, problem);
          inventory.list_value = before;
          result.amount = result.requested;
          result.made.clear();
          result.outcome = LN_InventoryOutcome::None;
        }
        if (instruction.property_ref_index != LN_INVALID_INDEX) {
          LN_Value stored;
          stored.type = LN_ValueType::Dict;
          stored.exists = true;
          stored.dict_value["amount"] = MakeIntValue(result.amount);
          stored.dict_value["changed"] = MakeIntValue(
              result.Changed(LN_InventoryOperation(edit.value.int_value)));
          stored.dict_value["made"] = MakeStringValue(result.made);
          stored.dict_value["outcome"] = MakeStringValue(
              LN_InventoryOutcomeName(result.outcome, result.craft));
          stored.dict_value["inventory"] = std::move(inventory);
          SetTreePropertyValue(instruction.property_ref_index, stored);
        }
        break;
      }
      case LN_OpCode::DropItem: {
        /* Done runs only when the copy is made or queued: a chain like Drop Item → Remove From
         * Slot must not lose the item when nothing lands in the world. */
        KX_Scene *scene = m_gameObject != nullptr ? m_gameObject->GetScene() : nullptr;
        std::string item;
        int32_t carried = 1;
        if (scene == nullptr || m_logicManager == nullptr ||
            !ItemOf(ResolveValueExpression(
                        instruction.secondary_value_expr_index, MakeNoneValue(), context),
                    item,
                    carried))
        {
          break;
        }
        /* Amount 0 drops all it carries: a whole slot's count, a pickup's count. */
        const int32_t amount = ResolveIntExpression(instruction.int_expr_index, 0, context);
        if (amount < 0) {
          break;
        }
        const blender::Collection *library =
            instruction.value_expr_index != LN_INVALID_INDEX ?
                ResolveCollectionByValue(
                    this,
                    ResolveValueExpression(instruction.value_expr_index, MakeNoneValue(), context)) :
                nullptr;
        if (library == nullptr) {
          library = SceneItemLibrary();
        }
        KX_GameObject *source = LN_FindLibraryItem(*scene, library, item);
        if (source == nullptr) {
          warn_scene_collection_request(LN_CommandBuffer::CommandType::AddObjectFromRef,
                                        "'" + item +
                                            "' has no object in the Item Library (set it on the "
                                            "node or in Scene > Items and UI)");
          break;
        }
        std::string reason;
        if (!m_logicManager->PreflightAddObjectPersistence(source, 0.0f, false, reason)) {
          warn_scene_collection_request(LN_CommandBuffer::CommandType::AddObjectFromRef,
                                        "'" + item + "' cannot be copied into the world (" +
                                            reason + "); keep the Item Library hidden");
          break;
        }
        const MT_Vector3 position = ResolveVectorExpression(
            instruction.vector_expr_index, MT_Vector3(0.0f, 0.0f, 0.0f), context);
        const int32_t count = amount > 0 ? amount : carried;
        /* Objects join the world on Logic + Physics steps: a UI route's drop waits for the next
         * one, like Send Event does. */
        if (event == LN_Event::OnUpdate) {
          m_logicManager->QueueItemDrop(source, position, item, count);
        }
        else if (m_logicManager->SpawnItemDrop(source, position, item, count) == nullptr) {
          break;
        }
        instruction_executed = true;
        break;
      }
      case LN_OpCode::ScrollObjectView: {
        /* An empty Viewport Object is the tree's own object, like the view nodes' default. The
         * view scrolls when it next lays out. */
        KX_GameObject *viewport = instruction.value_expr_index != LN_INVALID_INDEX ?
                                      ResolveObjectValue(ResolveValueExpression(
                                          instruction.value_expr_index, MakeNoneValue(), context)) :
                                      m_gameObject;
        if (viewport == nullptr || m_logicManager == nullptr) {
          break;
        }
        const LN_UIViewScrollMode mode = LN_UIViewScrollMode(
            std::clamp(instruction.int_value,
                       int32_t(LN_UIViewScrollMode::ShowItem),
                       int32_t(LN_UIViewScrollMode::Lines)));
        /* Show Item: a number is an index; an item name, slot or object is looked for. */
        LN_UIService::ScrollTarget target;
        if (mode == LN_UIViewScrollMode::ShowItem) {
          const LN_Value item = ResolveValueExpression(
              instruction.secondary_value_expr_index, MakeNoneValue(), context);
          if (item.exists && (item.type == LN_ValueType::Int || item.type == LN_ValueType::Float)) {
            target.has_index = true;
            target.index = ValueAsInt(item, LN_FloatToIntMode::Round);
          }
          else if (item.exists && item.type == LN_ValueType::ObjectRef) {
            /* A grid slot (a drop's Target): the item it shows; another object: by its item. */
            KX_GameObject *object = ResolveLiveObjectValue(item, true);
            LN_Value bound;
            int32_t bound_index = -1;
            std::string bound_name;
            int32_t bound_count = 0;
            if (object != nullptr &&
                m_logicManager->GetUIService().GetBoundItem(object, bound, bound_index) &&
                bound.exists)
            {
              if (bound.type == LN_ValueType::ObjectRef) {
                target.item = bound.reference_name;
              }
              else if (LN_InventoryShownEntry(bound, bound_name, bound_count)) {
                target.item = bound_name;
              }
            }
            else {
              target.object = object;
              target.item = ItemNameOf(item);
            }
          }
          else if (item.exists && item.type == LN_ValueType::String) {
            target.item = item.string_value;
          }
          else {
            target.item = ItemNameOf(item);
          }
          if (!target.has_index && target.item.empty() && target.object == nullptr) {
            break;
          }
        }
        m_logicManager->GetUIService().RequestViewScroll(
            viewport, mode, ResolveIntExpression(instruction.int_expr_index, 1, context), target);
        instruction_executed = true;
        break;
      }
      case LN_OpCode::ShowMessage: {
        KX_GameObject *object = instruction.value_expr_index != LN_INVALID_INDEX ?
                                    ResolveObjectValue(ResolveValueExpression(
                                        instruction.value_expr_index, MakeNoneValue(), context)) :
                                    m_gameObject;
        if (object == nullptr || m_logicManager == nullptr) {
          break;
        }
        instruction_executed = m_logicManager->GetUIService().ShowMessage(
            object,
            ResolveIntExpression(instruction.secondary_int_expr_index, -1, context),
            ResolveStringExpression(instruction.string_expr_index, std::string(), context),
            ResolveFloatExpression(instruction.float_expr_index, 3.0f, context),
            ResolveIntExpression(instruction.int_expr_index, 4, context),
            ResolveIntExpression(instruction.tertiary_int_expr_index, -1, context));
        break;
      }
      case LN_OpCode::SetUIFocus: {
        if (m_gameObject == nullptr) {
          CompleteUIOperation(instruction.property_ref_index, LN_UIResult::InvalidOwner, 0);
          instruction_executed = true;
          break;
        }
        KX_GameObject *target = instruction.value_expr_index != LN_INVALID_INDEX ?
                                    ResolveObjectValue(ResolveValueExpression(
                                        instruction.value_expr_index, MakeNoneValue(), context)) :
                                    nullptr;
        command_buffer.AppendSetUIFocus(
            this,
            m_gameObject,
            target,
            ResolveIntExpression(instruction.int_expr_index, 0, context),
            ResolveStringExpression(instruction.string_expr_index, std::string(), context),
            instruction.property_ref_index,
            command_sort_key(instruction_id, command_sequence),
            instruction.source_ref_index);
        instruction_executed = true;
        break;
      }
      case LN_OpCode::SetPlayerPointer: {
        if (m_gameObject == nullptr) {
          instruction_executed = true;
          break;
        }
        /* Surface: an empty Surface is the tree's own object; a linked one that resolves to
         * nothing turns the pointer off. View Center has neither. */
        const bool aim = instruction.bool_value;
        KX_GameObject *surface = aim ? nullptr :
                                 instruction.value_expr_index != LN_INVALID_INDEX ?
                                       ResolveObjectValue(ResolveValueExpression(
                                           instruction.value_expr_index, MakeNoneValue(), context)) :
                                       m_gameObject;
        KX_GameObject *cursor = !aim && instruction.secondary_value_expr_index != LN_INVALID_INDEX ?
                                    ResolveObjectValue(
                                        ResolveValueExpression(instruction.secondary_value_expr_index,
                                                               MakeNoneValue(),
                                                               context)) :
                                    nullptr;
        LN_CommandBuffer::PlayerPointerSettings settings;
        settings.aim = aim;
        settings.surface = surface;
        settings.cursor = cursor;
        settings.move_action = ResolveStringExpression(
            instruction.string_expr_index, std::string(), context);
        settings.speed = ResolveFloatExpression(instruction.float_expr_index, 1.0f, context);
        settings.reach = ResolveFloatExpression(
            instruction.secondary_float_expr_index, 3.0f, context);
        settings.press_action = ResolveStringExpression(
            instruction.secondary_string_expr_index, std::string(), context);
        command_buffer.AppendSetPlayerPointer(
            m_gameObject,
            ResolveIntExpression(instruction.int_expr_index, 0, context),
            ResolveBoolExpression(instruction.bool_expr_index, true, context),
            settings,
            command_sort_key(instruction_id, command_sequence),
            instruction.source_ref_index);
        instruction_executed = true;
        break;
      }
      case LN_OpCode::TakeUIControl: {
        if (m_gameObject == nullptr) {
          CompleteUIOperation(instruction.property_ref_index, LN_UIResult::InvalidOwner, 0);
          instruction_executed = true;
          break;
        }
        KX_GameObject *target = instruction.value_expr_index != LN_INVALID_INDEX ?
                                    ResolveObjectValue(ResolveValueExpression(
                                        instruction.value_expr_index, MakeNoneValue(), context)) :
                                    m_gameObject;
        if (target == nullptr) {
          CompleteUIOperation(instruction.property_ref_index, LN_UIResult::InvalidTarget, 0);
          instruction_executed = true;
          break;
        }
        command_buffer.AppendTakeUIControl(
            this,
            m_gameObject,
            target,
            ResolveIntExpression(instruction.int_expr_index, 0, context),
            ResolveBoolExpression(instruction.bool_expr_index, true, context),
            instruction.property_ref_index,
            command_sort_key(instruction_id, command_sequence),
            instruction.source_ref_index);
        instruction_executed = true;
        break;
      }
      case LN_OpCode::ScreenAnchor: {
        /* An empty Object input anchors the tree's own object, like the other UI commands. */
        KX_GameObject *target = instruction.value_expr_index != LN_INVALID_INDEX ?
                                    ResolveObjectValue(ResolveValueExpression(
                                        instruction.value_expr_index, MakeNoneValue(), context)) :
                                    m_gameObject;
        KX_Scene *scene = m_gameObject ? m_gameObject->GetScene() : nullptr;
        KX_Camera *camera = instruction.secondary_value_expr_index != LN_INVALID_INDEX ?
                                ResolveCameraValue(
                                    ResolveValueExpression(instruction.secondary_value_expr_index,
                                                           MakeNoneValue(),
                                                           context)) :
                                nullptr;
        /* Area Player's View: the player's split screen view in the window; nothing while the
         * player has no view on screen. */
        const bool in_player_view = instruction.secondary_int_value == 1;
        const int32_t user = in_player_view ?
                                 ResolveIntExpression(instruction.int_expr_index, 0, context) :
                                 0;
        KX_PlayerViews::Area player_area;
        RAS_Rect player_window;
        if (in_player_view &&
            !ResolvePlayerViewArea(m_gameObject, user, player_area, &player_window))
        {
          break;
        }
        if (camera == nullptr && scene != nullptr) {
          /* The menu camera draws over the whole window; a world object in a player's view is
           * seen through that player's camera. */
          if (target != nullptr && scene->IsOverlayRuntimeObject(target) &&
              scene->GetOverlayCamera() != nullptr)
          {
            camera = scene->GetOverlayCamera();
          }
          else {
            camera = in_player_view ? scene->GetPlayerViews().GetPlayerCamera(
                                          user, scene->GetActiveCamera()) :
                                      scene->GetActiveCamera();
          }
        }
        const float depth = ResolveFloatExpression(instruction.float_expr_index, 1.0f, context);
        KX_KetsjiEngine *engine = KX_GetActiveEngine();
        RAS_ICanvas *canvas = engine ? engine->GetCanvas() : nullptr;
        RAS_Rasterizer *rasterizer = engine ? engine->GetRasterizer() : nullptr;
        if (target == nullptr || camera == nullptr || scene == nullptr ||
            target->GetScene() != scene || camera->GetScene() != scene || canvas == nullptr ||
            rasterizer == nullptr || canvas->GetWidth() <= 0 || canvas->GetHeight() <= 0 ||
            !std::isfinite(depth) || depth < 0.0f)
        {
          break;
        }
        const RAS_Rect display_area = rasterizer->GetRenderArea(
            canvas, RAS_Rasterizer::RAS_STEREO_LEFTEYE);
        RAS_Rect camera_area;
        RAS_Rect viewport;
        engine->GetSceneViewport(scene, camera, display_area, camera_area, viewport);
        /* The rectangle anchored in, in canvas pixels from the top left. */
        const int drawn_extra = LN_CameraViewportDrawnExtra(camera);
        float width = float(viewport.GetWidth() + drawn_extra);
        float height = float(viewport.GetHeight() + drawn_extra);
        float area_left = float(viewport.GetLeft());
        float area_top = float(canvas->GetHeight() - viewport.GetTop());
        if (in_player_view) {
          width = player_area.width * float(player_window.GetWidth());
          height = player_area.height * float(player_window.GetHeight());
          area_left = float(player_window.GetLeft()) +
                      player_area.left * float(player_window.GetWidth());
          area_top = float(canvas->GetHeight() - player_window.GetTop()) +
                     player_area.top * float(player_window.GetHeight());
        }
        if (width <= 0.0f || height <= 0.0f) {
          break;
        }
        MT_Vector3 offset = ResolveVectorExpression(
            instruction.vector_expr_index, MT_Vector3(0.0f, 0.0f, 0.0f), context);
        const MT_Vector3 safe_margin = ResolveVectorExpression(
            instruction.secondary_vector_expr_index, MT_Vector3(0.0f, 0.0f, 0.0f), context);
        MT_Vector3 reference_resolution(1920.0f, 1080.0f, 0.0f);
        if (instruction.tertiary_value_expr_index != LN_INVALID_INDEX) {
          ValueAsVector(ResolveValueExpression(
                            instruction.tertiary_value_expr_index, MakeNoneValue(), context),
                        reference_resolution);
        }
        if (ResolveBoolExpression(instruction.bool_expr_index, true, context) &&
            reference_resolution.x() > 0.0f && reference_resolution.y() > 0.0f)
        {
          offset.x() *= width / reference_resolution.x();
          offset.y() *= height / reference_resolution.y();
        }
        const int anchor = std::clamp(instruction.int_value, 0, 8);
        const int column = anchor % 3;
        const int row = anchor / 3;
        const float margin_x = std::max(0.0f, safe_margin.x());
        const float margin_y = std::max(0.0f, safe_margin.y());
        const float pixel_x = area_left +
                              (column == 0 ? margin_x :
                                             (column == 1 ? width * 0.5f : width - margin_x)) +
                              offset.x();
        const float pixel_y = area_top +
                              (row == 0 ? margin_y :
                                          (row == 1 ? height * 0.5f : height - margin_y)) +
                              offset.y();
        MT_Vector3 position;
        if (ProjectCanvasPixelToWorld(camera, pixel_x, pixel_y, depth, position)) {
          command_buffer.AppendSetWorldPosition(target,
                                                position,
                                                command_sort_key(instruction_id, command_sequence),
                                                instruction.source_ref_index);
          instruction_executed = true;
        }
        break;
      }
      default:
        return;
    }
    command_sequence++;
    if (instruction_executed) {
      MarkInstructionExecuted(event, instruction_id, context.tick_index);
    }
  };

  auto execute_debug_draw_instruction = [&](const LN_Instruction &instruction,
                                            const uint32_t instruction_id) {
    if (!RasterizerReady()) {
      command_sequence++;
      MarkInstructionExecuted(event, instruction_id, context.tick_index);
      return;
    }

    switch (instruction.opcode) {
      case LN_OpCode::DrawLine: {
        const MT_Vector3 from = ResolveVectorExpression(
            instruction.vector_expr_index, instruction.vector_value, context);
        const MT_Vector3 to = ResolveVectorExpression(
            instruction.secondary_vector_expr_index, instruction.secondary_vector_value, context);
        const MT_Vector4 color = ResolveColorExpression(
            instruction.color_expr_index, instruction.color_value, context);
        KX_RasterizerDrawDebugLine(from, to, color);
        break;
      }
      case LN_OpCode::DrawArrow: {
        const MT_Vector3 from = ResolveVectorExpression(
            instruction.vector_expr_index, instruction.vector_value, context);
        const MT_Vector3 to = ResolveVectorExpression(
            instruction.secondary_vector_expr_index, instruction.secondary_vector_value, context);
        const MT_Vector4 color = ResolveColorExpression(
            instruction.color_expr_index, instruction.color_value, context);
        DrawDebugArrow(from, to, color);
        break;
      }
      case LN_OpCode::DrawPath: {
        const LN_Value points_value = ResolveValueExpression(
            instruction.value_expr_index, MakeNoneValue(), context);
        std::vector<MT_Vector3> points;
        if (ValueAsVectorList(points_value, points)) {
          const MT_Vector4 color = ResolveColorExpression(
              instruction.color_expr_index, instruction.color_value, context);
          for (size_t index = 1; index < points.size(); index++) {
            KX_RasterizerDrawDebugLine(points[index - 1], points[index], color);
          }
        }
        break;
      }
      case LN_OpCode::DrawBox: {
        MT_Vector3 origin = ResolveVectorExpression(
            instruction.vector_expr_index, instruction.vector_value, context);
        const MT_Vector4 color = ResolveColorExpression(
            instruction.color_expr_index, instruction.color_value, context);
        const float half_x =
            std::max(0.0f, ResolveFloatExpression(instruction.float_expr_index, 1.0f, context)) *
            0.5f;
        const float half_y = std::max(0.0f,
                                      ResolveFloatExpression(
                                          instruction.secondary_float_expr_index, 1.0f, context)) *
                             0.5f;
        const float half_z = std::max(0.0f,
                                      ResolveFloatExpression(
                                          instruction.tertiary_float_expr_index, 1.0f, context)) *
                             0.5f;
        if (instruction.bool_value) {
          origin += MT_Vector3(half_x, half_y, half_z);
        }
        DrawDebugBox(origin, half_x, half_y, half_z, color);
        break;
      }
      case LN_OpCode::DrawMesh: {
        KX_GameObject *target = m_snapshot.GetObjectSnapshot().object;
        if (instruction.value_expr_index != LN_INVALID_INDEX) {
          const LN_Value object_value = ResolveValueExpression(
              instruction.value_expr_index, MakeNoneValue(), context);
          if (KX_GameObject *object = ResolveObjectValue(object_value)) {
            target = object;
          }
        }
        if (target != nullptr) {
          const MT_Vector4 color = ResolveColorExpression(
              instruction.color_expr_index, instruction.color_value, context);
          const MT_Transform transform = target->NodeGetWorldTransform();
          for (int mesh_index = 0; mesh_index < target->GetMeshCount(); mesh_index++) {
            RAS_MeshObject *mesh = target->GetMesh(mesh_index);
            if (mesh == nullptr) {
              continue;
            }
            for (int poly_index = 0; poly_index < mesh->NumPolygons(); poly_index++) {
              RAS_Polygon *polygon = mesh->GetPolygon(poly_index);
              if (polygon == nullptr || polygon->VertexCount() < 2) {
                continue;
              }
              const int vertex_count = polygon->VertexCount();
              for (int vertex_index = 0; vertex_index < vertex_count; vertex_index++) {
                const RAS_IVertex *from_vertex = polygon->GetVertex(vertex_index);
                const RAS_IVertex *to_vertex = polygon->GetVertex((vertex_index + 1) %
                                                                  vertex_count);
                if (from_vertex == nullptr || to_vertex == nullptr) {
                  continue;
                }
                KX_RasterizerDrawDebugLine(transform * MT_Vector3(from_vertex->getXYZ()),
                                           transform * MT_Vector3(to_vertex->getXYZ()),
                                           color);
              }
            }
          }
        }
        break;
      }
      case LN_OpCode::DrawAxis: {
        KX_GameObject *target = m_snapshot.GetObjectSnapshot().object;
        if (instruction.value_expr_index != LN_INVALID_INDEX) {
          const LN_Value object_value = ResolveValueExpression(
              instruction.value_expr_index, MakeNoneValue(), context);
          if (KX_GameObject *object = ResolveObjectValue(object_value)) {
            target = object;
          }
        }
        if (target != nullptr) {
          const float length = std::max(
              0.0f, ResolveFloatExpression(instruction.float_expr_index, 1.0f, context));
          const MT_Vector3 origin = target->NodeGetWorldPosition();
          const MT_Matrix3x3 orientation = target->NodeGetWorldOrientation();
          const MT_Vector3 axes[3] = {
              orientation * MT_Vector3(length, 0.0f, 0.0f),
              orientation * MT_Vector3(0.0f, length, 0.0f),
              orientation * MT_Vector3(0.0f, 0.0f, length),
          };
          const MT_Vector4 colors[3] = {
              MT_Vector4(1.0f, 0.0f, 0.0f, 1.0f),
              MT_Vector4(0.0f, 1.0f, 0.0f, 1.0f),
              MT_Vector4(0.0f, 0.35f, 1.0f, 1.0f),
          };
          for (int axis = 0; axis < 3; axis++) {
            KX_RasterizerDrawDebugLine(origin, origin + axes[axis], colors[axis]);
            KX_RasterizerDrawDebugLine(origin, origin - axes[axis], colors[axis] * 0.55f);
          }
        }
        break;
      }
      default:
        return;
    }
    command_sequence++;
    MarkInstructionExecuted(event, instruction_id, context.tick_index);
  };

  auto profile_direct_exec_op = [&](const LN_ExecOp &op) {
    if (m_activeProfileCounters == nullptr) {
      return;
    }
    m_activeProfileCounters->instruction_dispatch_count++;
    m_activeProfileCounters->exec_direct_instruction_count++;
    if (op.fallback_requirements != LN_RUNTIME_FALLBACK_NONE) {
      m_activeProfileCounters->fallback_path_count++;
    }
  };

  auto execute_direct_vector_command = [&](const LN_Instruction &instruction,
                                           const uint32_t instruction_id) {
    const LN_VectorCommandPayload payload = vector_command_payload(instruction);
    const LN_VectorOperationMode mode = LN_VectorOperationMode(payload.operation_mode);
    const LN_VectorOperationChannel channel = LN_VectorOperationChannel(payload.operation_channel);
    const bool is_orientation_command = instruction.opcode == LN_OpCode::SetWorldOrientation ||
                                        instruction.opcode == LN_OpCode::SetLocalOrientation ||
                                        (instruction.opcode == LN_OpCode::SetTransformVector &&
                                         channel == LN_VectorOperationChannel::Orientation) ||
                                        (instruction.opcode == LN_OpCode::ApplyTransformVector &&
                                         channel == LN_VectorOperationChannel::Rotation);
    const LN_Rotation rotation = is_orientation_command ?
                                     ResolveRotationExpression(
                                         payload.vector_expr_index,
                                         LN_Rotation::FromEulerXYZ(payload.vector_value),
                                         context) :
                                     LN_Rotation{};
    const MT_Vector3 value = is_orientation_command ?
                                 MT_Vector3(0.0f, 0.0f, 0.0f) :
                                 ResolveVectorExpression(
                                     payload.vector_expr_index, payload.vector_value, context);
    KX_GameObject *target = resolve_object_expression(payload.object_value_expr_index);
    if (target == nullptr) {
      command_sequence++;
      MarkInstructionExecuted(event, instruction_id, context.tick_index);
      return;
    }

    if (instruction.opcode == LN_OpCode::ApplyTransformVector &&
        (channel == LN_VectorOperationChannel::Movement ||
         channel == LN_VectorOperationChannel::Rotation))
    {
      const LN_CommandBuffer::CommandType command_type =
          channel == LN_VectorOperationChannel::Movement ?
              LN_CommandBuffer::CommandType::ApplyMovement :
              LN_CommandBuffer::CommandType::ApplyRotation;
      if (!frame_relative_transform_is_safe(target, instruction, instruction_id, command_type)) {
        command_sequence++;
        return;
      }
    }

    const uint64_t sort_key = command_sort_key(instruction_id, command_sequence);
    switch (instruction.opcode) {
      case LN_OpCode::SetWorldPosition:
        command_buffer.AppendSetWorldPosition(
            target, value, sort_key, instruction.source_ref_index);
        break;
      case LN_OpCode::SetLocalPosition:
        command_buffer.AppendSetLocalPosition(
            target, value, sort_key, instruction.source_ref_index);
        break;
      case LN_OpCode::SetWorldOrientation:
        command_buffer.AppendSetWorldOrientation(
            target, rotation, sort_key, instruction.source_ref_index);
        break;
      case LN_OpCode::SetLocalOrientation:
        command_buffer.AppendSetLocalOrientation(
            target, rotation, sort_key, instruction.source_ref_index);
        break;
      case LN_OpCode::SetWorldScale:
        command_buffer.AppendSetWorldScale(target, value, sort_key, instruction.source_ref_index);
        break;
      case LN_OpCode::SetLocalScale:
        command_buffer.AppendSetLocalScale(target, value, sort_key, instruction.source_ref_index);
        break;
      case LN_OpCode::SetLinearVelocity:
        command_buffer.AppendSetLinearVelocity(
            target, value, sort_key, instruction.source_ref_index);
        break;
      case LN_OpCode::SetLocalLinearVelocity:
        command_buffer.AppendSetLocalLinearVelocity(
            target, value, sort_key, instruction.source_ref_index);
        break;
      case LN_OpCode::SetAngularVelocity:
        command_buffer.AppendSetAngularVelocity(
            target, value, sort_key, instruction.source_ref_index);
        break;
      case LN_OpCode::SetLocalAngularVelocity:
        command_buffer.AppendSetLocalAngularVelocity(
            target, value, sort_key, instruction.source_ref_index);
        break;
      case LN_OpCode::SetTransformVector: {
        const bool local = (mode == LN_VectorOperationMode::LocalFromBool) ?
                               ResolveBoolExpression(payload.bool_expr_index, false, context) :
                               (mode == LN_VectorOperationMode::Local);
        if (channel == LN_VectorOperationChannel::Position) {
          if (local) {
            command_buffer.AppendSetLocalPosition(
                target, value, sort_key, instruction.source_ref_index);
          }
          else {
            command_buffer.AppendSetWorldPosition(
                target, value, sort_key, instruction.source_ref_index);
          }
        }
        else if (channel == LN_VectorOperationChannel::Orientation) {
          if (local) {
            command_buffer.AppendSetLocalOrientation(
                target, rotation, sort_key, instruction.source_ref_index);
          }
          else {
            command_buffer.AppendSetWorldOrientation(
                target, rotation, sort_key, instruction.source_ref_index);
          }
        }
        else if (channel == LN_VectorOperationChannel::Scale) {
          if (local) {
            command_buffer.AppendSetLocalScale(
                target, value, sort_key, instruction.source_ref_index);
          }
          else {
            command_buffer.AppendSetWorldScale(
                target, value, sort_key, instruction.source_ref_index);
          }
        }
        break;
      }
      case LN_OpCode::SetVelocityVector: {
        const bool local = (mode == LN_VectorOperationMode::LocalFromBool) ?
                               ResolveBoolExpression(payload.bool_expr_index, false, context) :
                               (mode == LN_VectorOperationMode::Local);
        if (channel == LN_VectorOperationChannel::LinearVelocity) {
          if (local) {
            command_buffer.AppendSetLocalLinearVelocity(
                target, value, sort_key, instruction.source_ref_index);
          }
          else {
            command_buffer.AppendSetLinearVelocity(
                target, value, sort_key, instruction.source_ref_index);
          }
        }
        else if (channel == LN_VectorOperationChannel::AngularVelocity) {
          if (local) {
            command_buffer.AppendSetLocalAngularVelocity(
                target, value, sort_key, instruction.source_ref_index);
          }
          else {
            command_buffer.AppendSetAngularVelocity(
                target, value, sort_key, instruction.source_ref_index);
          }
        }
        break;
      }
      case LN_OpCode::ApplyTransformVector: {
        const bool local = (mode == LN_VectorOperationMode::LocalFromBool) ?
                               ResolveBoolExpression(payload.bool_expr_index, false, context) :
                               (mode == LN_VectorOperationMode::Local);
        if (channel == LN_VectorOperationChannel::Movement) {
          command_buffer.AppendApplyMovement(
              target, value, local, sort_key, instruction.source_ref_index);
        }
        else if (channel == LN_VectorOperationChannel::Rotation) {
          command_buffer.AppendApplyRotation(
              target, rotation, local, sort_key, instruction.source_ref_index);
        }
        break;
      }
      case LN_OpCode::ApplyPhysicsVector: {
        const bool local = (mode == LN_VectorOperationMode::LocalFromBool) ?
                               ResolveBoolExpression(payload.bool_expr_index, false, context) :
                               (mode == LN_VectorOperationMode::Local);
        if (channel == LN_VectorOperationChannel::Force) {
          command_buffer.AppendApplyForce(
              target, value, local, sort_key, instruction.source_ref_index);
        }
        else if (channel == LN_VectorOperationChannel::Torque) {
          command_buffer.AppendApplyTorque(
              target, value, local, sort_key, instruction.source_ref_index);
        }
        break;
      }
      default:
        break;
    }
    command_sequence++;
    MarkInstructionExecuted(event, instruction_id, context.tick_index);
  };

  auto execute_direct_physics_command = [&](const LN_Instruction &instruction,
                                            const uint32_t instruction_id) {
    auto mark_done = [&]() { MarkInstructionExecuted(event, instruction_id, context.tick_index); };
    auto append_targeted = [&](auto &&append_fn) {
      if (KX_GameObject *target = resolve_command_target(instruction)) {
        append_fn(target, command_sort_key(instruction_id, command_sequence));
        command_sequence++;
      }
      mark_done();
    };
    switch (instruction.opcode) {
      case LN_OpCode::ApplyMovement:
      case LN_OpCode::ApplyRotation:
      case LN_OpCode::ApplyForce:
      case LN_OpCode::ApplyTorque: {
        const LN_VectorCommandPayload payload = vector_command_payload(instruction);
        const bool is_rotation = instruction.opcode == LN_OpCode::ApplyRotation;
        const LN_Rotation rotation = is_rotation ?
                                         ResolveRotationExpression(
                                             payload.vector_expr_index,
                                             LN_Rotation::FromEulerXYZ(payload.vector_value),
                                             context) :
                                         LN_Rotation{};
        const MT_Vector3 value = is_rotation ? MT_Vector3(0.0f, 0.0f, 0.0f) :
                                               ResolveVectorExpression(payload.vector_expr_index,
                                                                       payload.vector_value,
                                                                       context);
        const bool local = ResolveBoolExpression(payload.bool_expr_index, false, context);
        bool accepted = true;
        if (KX_GameObject *target = resolve_object_expression(payload.object_value_expr_index)) {
          if (instruction.opcode == LN_OpCode::ApplyMovement ||
              instruction.opcode == LN_OpCode::ApplyRotation)
          {
            const LN_CommandBuffer::CommandType command_type =
                instruction.opcode == LN_OpCode::ApplyMovement ?
                    LN_CommandBuffer::CommandType::ApplyMovement :
                    LN_CommandBuffer::CommandType::ApplyRotation;
            accepted = frame_relative_transform_is_safe(
                target, instruction, instruction_id, command_type);
          }
          if (!accepted) {
            command_sequence++;
            break;
          }
          const uint64_t sort_key = command_sort_key(instruction_id, command_sequence);
          switch (instruction.opcode) {
            case LN_OpCode::ApplyMovement:
              command_buffer.AppendApplyMovement(
                  target, value, local, sort_key, instruction.source_ref_index);
              break;
            case LN_OpCode::ApplyRotation:
              command_buffer.AppendApplyRotation(
                  target, rotation, local, sort_key, instruction.source_ref_index);
              break;
            case LN_OpCode::ApplyForce:
              command_buffer.AppendApplyForce(
                  target, value, local, sort_key, instruction.source_ref_index);
              break;
            case LN_OpCode::ApplyTorque:
              command_buffer.AppendApplyTorque(
                  target, value, local, sort_key, instruction.source_ref_index);
              break;
            default:
              break;
          }
        }
        command_sequence++;
        if (accepted) {
          mark_done();
        }
        break;
      }
      case LN_OpCode::ApplyImpulse: {
        const LN_VectorCommandPayload payload = vector_command_payload(instruction);
        const MT_Vector3 impulse = ResolveVectorExpression(
            payload.vector_expr_index, payload.vector_value, context);
        const MT_Vector3 attach = ResolveVectorExpression(
            payload.secondary_vector_expr_index, payload.secondary_vector_value, context);
        if (KX_GameObject *target = resolve_object_expression(payload.object_value_expr_index)) {
          command_buffer.AppendApplyImpulse(target,
                                            attach,
                                            impulse,
                                            command_sort_key(instruction_id, command_sequence),
                                            instruction.source_ref_index);
        }
        command_sequence++;
        mark_done();
        break;
      }
      case LN_OpCode::ApplyForceToTarget: {
        MT_Vector3 target_position = ResolveVectorExpression(
            instruction.vector_expr_index, instruction.vector_value, context);
        const float force = ResolveFloatExpression(instruction.float_expr_index, 10.0f, context);
        const float reached_threshold = std::max(
            ResolveFloatExpression(instruction.secondary_float_expr_index, 0.1f, context), 0.0f);
        const float stop_distance = std::max(
            ResolveFloatExpression(instruction.tertiary_float_expr_index, 0.1f, context), 0.0f);
        if (KX_GameObject *target = resolve_command_target(instruction)) {
          const MT_Vector3 delta = target_position - target->NodeGetWorldPosition();
          const float distance_sq = delta.length2();
          if (vector_is_finite(delta)) {
            if (distance_sq <= reached_threshold * reached_threshold) {
              const uint32_t state_index = InstructionStateIndex(event, instruction_id);
              if (state_index != LN_INVALID_INDEX && state_index < m_instructionStates.size()) {
                InstructionRuntimeState &state = m_instructionStates[state_index];
                state.last_reached_tick = context.tick_index;
                state.last_reached_scope_serial = m_activeExecutionScopeSerial;
              }
            }
            if (std::isfinite(force) && distance_sq > stop_distance * stop_distance &&
                distance_sq > MT_EPSILON * MT_EPSILON && std::fabs(force) > MT_EPSILON)
            {
              const MT_Vector3 force_vector = delta * (force / std::sqrt(distance_sq));
              command_buffer.AppendApplyForce(target,
                                              force_vector,
                                              false,
                                              command_sort_key(instruction_id, command_sequence),
                                              instruction.source_ref_index);
            }
          }
        }
        command_sequence++;
        mark_done();
        break;
      }
      case LN_OpCode::Translate: {
        const MT_Vector3 vector = ResolveVectorExpression(
            instruction.vector_expr_index, instruction.vector_value, context);
        const float speed = ResolveFloatExpression(instruction.float_expr_index, 1.0f, context);
        const bool local = ResolveBoolExpression(instruction.bool_expr_index, false, context);
        if (KX_GameObject *target = resolve_command_target(instruction)) {
          const float tick_seconds = float(std::max(0.0, context.delta_time));
          command_buffer.AppendApplyMovement(target,
                                             vector * (speed * tick_seconds),
                                             local,
                                             command_sort_key(instruction_id, command_sequence),
                                             instruction.source_ref_index);
        }
        command_sequence++;
        mark_done();
        break;
      }
      case LN_OpCode::MoveToward: {
        const MT_Vector3 target_pos = ResolveVectorExpression(
            instruction.vector_expr_index, MT_Vector3(0.0f, 0.0f, 0.0f), context);
        const float speed = ResolveFloatExpression(instruction.float_expr_index, 1.0f, context);
        const float stop_distance = ResolveFloatExpression(
            instruction.secondary_float_expr_index, 0.5f, context);
        if (KX_GameObject *target = resolve_command_target(instruction)) {
          /* Reached (only when it is linked: the compiler then gives where the object is): this
           * step's move ends within Stop Distance, as the flush moves by
           * min(Speed * dt, distance - Stop Distance), or the object is already there. */
          const float nan = std::numeric_limits<float>::quiet_NaN();
          const MT_Vector3 position = instruction.secondary_vector_expr_index == LN_INVALID_INDEX ?
                                          MT_Vector3(nan, nan, nan) :
                                          ResolveVectorExpression(
                                              instruction.secondary_vector_expr_index,
                                              MT_Vector3(nan, nan, nan),
                                              context);
          const float distance = (target_pos - position).length();
          const float step = std::max(speed * float(std::max(0.0, context.delta_time)), 0.0f);
          const float tolerance = std::max(
              1.0e-4f, (std::fabs(stop_distance) + float(target_pos.length())) * 1.0e-6f);
          if (std::isfinite(distance) && std::isfinite(step) &&
              distance <= std::max(stop_distance, 0.0f) + step + tolerance)
          {
            const uint32_t state_index = InstructionStateIndex(event, instruction_id);
            if (state_index != LN_INVALID_INDEX && state_index < m_instructionStates.size()) {
              InstructionRuntimeState &state = m_instructionStates[state_index];
              state.last_reached_tick = context.tick_index;
              state.last_reached_scope_serial = m_activeExecutionScopeSerial;
            }
          }
          command_buffer.AppendMoveToward(target,
                                          target_pos,
                                          speed,
                                          stop_distance,
                                          false,
                                          true,
                                          float(context.delta_time),
                                          command_sort_key(instruction_id, command_sequence),
                                          instruction.source_ref_index);
        }
        command_sequence++;
        mark_done();
        break;
      }
      case LN_OpCode::SetCollisionGroup: {
        const int32_t group = ResolveIntExpression(instruction.int_expr_index, 0, context);
        append_targeted([&](KX_GameObject *target, const uint64_t sort_key) {
          command_buffer.AppendSetCollisionGroup(
              target, group, sort_key, instruction.source_ref_index);
        });
        break;
      }
      case LN_OpCode::SetPhysics: {
        const bool active = ResolveBoolExpression(instruction.bool_expr_index, true, context);
        append_targeted([&](KX_GameObject *target, const uint64_t sort_key) {
          command_buffer.AppendSetPhysics(target, active, sort_key, instruction.source_ref_index);
        });
        break;
      }
      case LN_OpCode::SetDynamics: {
        const bool enabled = ResolveBoolExpression(
            instruction.bool_expr_index, instruction.bool_value, context);
        append_targeted([&](KX_GameObject *target, const uint64_t sort_key) {
          command_buffer.AppendSetDynamics(
              target, instruction.int_value, enabled, sort_key, instruction.source_ref_index);
        });
        break;
      }
      case LN_OpCode::RebuildCollisionShape: {
        append_targeted([&](KX_GameObject *target, const uint64_t sort_key) {
          command_buffer.AppendRebuildCollisionShape(
              target, sort_key, instruction.source_ref_index);
        });
        break;
      }
      case LN_OpCode::SetRigidBodyAttribute: {
        const LN_RigidBodyAttribute attribute = static_cast<LN_RigidBodyAttribute>(
            instruction.int_value);
        MT_Vector3 value = instruction.vector_value;
        MT_Vector3 secondary_value = instruction.secondary_vector_value;
        float scalar_value = 0.0f;
        bool bool_value = false;
        bool secondary_bool_value = false;

        switch (attribute) {
          case LN_RigidBodyAttribute::Mass:
          case LN_RigidBodyAttribute::Friction:
          case LN_RigidBodyAttribute::Restitution:
          case LN_RigidBodyAttribute::MinLinearVelocity:
          case LN_RigidBodyAttribute::MaxLinearVelocity:
          case LN_RigidBodyAttribute::MinAngularVelocity:
          case LN_RigidBodyAttribute::MaxAngularVelocity:
          case LN_RigidBodyAttribute::GravityFactor:
            scalar_value = ResolveFloatExpression(
                instruction.float_expr_index, instruction.vector_value.x(), context);
            break;
          case LN_RigidBodyAttribute::Damping:
            value = MT_Vector3(
                ResolveFloatExpression(
                    instruction.float_expr_index, instruction.vector_value.x(), context),
                ResolveFloatExpression(
                    instruction.secondary_float_expr_index, instruction.vector_value.y(), context),
                0.0f);
            break;
          case LN_RigidBodyAttribute::Ccd:
          case LN_RigidBodyAttribute::AllowPhysicsRotation:
            bool_value = ResolveBoolExpression(
                instruction.bool_expr_index, instruction.bool_value, context);
            break;
          case LN_RigidBodyAttribute::Sleeping:
            bool_value = ResolveBoolExpression(
                instruction.bool_expr_index, instruction.bool_value, context);
            secondary_bool_value = ResolveBoolExpression(instruction.secondary_bool_expr_index,
                                                         instruction.secondary_int_value != 0,
                                                         context);
            break;
          case LN_RigidBodyAttribute::AxisLocks:
            value = ResolveVectorExpression(
                instruction.vector_expr_index, instruction.vector_value, context);
            secondary_value = ResolveVectorExpression(instruction.secondary_vector_expr_index,
                                                      instruction.secondary_vector_value,
                                                      context);
            break;
        }

        append_targeted([&](KX_GameObject *target, const uint64_t sort_key) {
          command_buffer.AppendSetRigidBodyAttribute(target,
                                                     attribute,
                                                     value,
                                                     secondary_value,
                                                     scalar_value,
                                                     bool_value,
                                                     secondary_bool_value,
                                                     sort_key,
                                                     instruction.source_ref_index);
        });
        break;
      }
      case LN_OpCode::SetGravity: {
        const LN_VectorCommandPayload payload = vector_command_payload(instruction);
        const MT_Vector3 gravity = ResolveVectorExpression(
            payload.vector_expr_index, payload.vector_value, context);
        command_buffer.AppendSetGravity(m_gameObject,
                                        gravity,
                                        command_sort_key(instruction_id, command_sequence),
                                        instruction.source_ref_index);
        command_sequence++;
        mark_done();
        break;
      }
      case LN_OpCode::CharacterJump: {
        append_targeted([&](KX_GameObject *target, const uint64_t sort_key) {
          command_buffer.AppendCharacterJump(target, sort_key, instruction.source_ref_index);
        });
        break;
      }
      case LN_OpCode::SetCharacterGravity: {
        const LN_VectorCommandPayload payload = vector_command_payload(instruction);
        const MT_Vector3 gravity = ResolveVectorExpression(
            payload.vector_expr_index, payload.vector_value, context);
        if (KX_GameObject *target = resolve_object_expression(payload.object_value_expr_index)) {
          command_buffer.AppendSetCharacterGravity(
              target,
              gravity,
              command_sort_key(instruction_id, command_sequence),
              instruction.source_ref_index);
          command_sequence++;
        }
        mark_done();
        break;
      }
      case LN_OpCode::SetCharacterJumpSpeed: {
        const float jump_speed = ResolveFloatExpression(
            instruction.float_expr_index, instruction.vector_value.x(), context);
        append_targeted([&](KX_GameObject *target, const uint64_t sort_key) {
          command_buffer.AppendSetCharacterJumpSpeed(
              target, jump_speed, sort_key, instruction.source_ref_index);
        });
        break;
      }
      case LN_OpCode::SetCharacterMaxJumps: {
        const int32_t max_jumps = ResolveIntExpression(
            instruction.int_expr_index, instruction.int_value, context);
        append_targeted([&](KX_GameObject *target, const uint64_t sort_key) {
          command_buffer.AppendSetCharacterMaxJumps(
              target, max_jumps, sort_key, instruction.source_ref_index);
        });
        break;
      }
      case LN_OpCode::SetCharacterWalkDirection: {
        const LN_VectorCommandPayload payload = vector_command_payload(instruction);
        const MT_Vector3 walk_direction = ResolveVectorExpression(
            payload.vector_expr_index, payload.vector_value, context);
        const bool local = ResolveBoolExpression(
            payload.bool_expr_index, payload.bool_value, context);
        if (KX_GameObject *target = resolve_object_expression(payload.object_value_expr_index)) {
          command_buffer.AppendSetCharacterWalkDirection(
              target,
              walk_direction,
              local,
              command_sort_key(instruction_id, command_sequence),
              instruction.source_ref_index);
          command_sequence++;
        }
        mark_done();
        break;
      }
      case LN_OpCode::SetCharacterVelocity: {
        const MT_Vector3 velocity = ResolveVectorExpression(
            instruction.vector_expr_index, instruction.vector_value, context);
        const float time = ResolveFloatExpression(
            instruction.float_expr_index, instruction.secondary_vector_value.x(), context);
        const bool local = ResolveBoolExpression(
            instruction.bool_expr_index, instruction.bool_value, context);
        append_targeted([&](KX_GameObject *target, const uint64_t sort_key) {
          command_buffer.AppendSetCharacterVelocity(
              target, velocity, time, local, sort_key, instruction.source_ref_index);
        });
        break;
      }
      case LN_OpCode::VehicleControl: {
        const MT_Vector3 control = ResolveVectorExpression(
            instruction.vector_expr_index, instruction.vector_value, context);
        const float steering = ResolveFloatExpression(
            instruction.float_expr_index, instruction.secondary_vector_value.x(), context);
        append_targeted([&](KX_GameObject *target, const uint64_t sort_key) {
          command_buffer.AppendVehicleControl(
              target, control, steering, sort_key, instruction.source_ref_index);
        });
        break;
      }
      case LN_OpCode::AlignAxisToVector: {
        const MT_Vector3 vector = ResolveVectorExpression(
            instruction.vector_expr_index, MT_Vector3(0.0f, 0.0f, 1.0f), context);
        const int32_t axis = ResolveIntExpression(instruction.int_expr_index, 2, context);
        const float factor = ResolveFloatExpression(instruction.float_expr_index, 1.0f, context);
        if (KX_GameObject *target = resolve_command_target(instruction)) {
          command_buffer.AppendAlignAxisToVector(
              target,
              vector,
              axis,
              factor,
              command_sort_key(instruction_id, command_sequence),
              instruction.source_ref_index);
        }
        command_sequence++;
        mark_done();
        break;
      }
      case LN_OpCode::RotateToward: {
        const MT_Vector3 target_point = ResolveVectorExpression(
            instruction.vector_expr_index, MT_Vector3(1.0f, 0.0f, 0.0f), context);
        const float factor = ResolveFloatExpression(instruction.float_expr_index, 1.0f, context);
        const int32_t rot_axis = ResolveIntExpression(instruction.int_expr_index, 2, context);
        const int32_t front_axis = ResolveIntExpression(
            instruction.secondary_int_expr_index, 1, context);
        if (KX_GameObject *target = resolve_command_target(instruction)) {
          command_buffer.AppendRotateToward(target,
                                            target_point,
                                            factor,
                                            rot_axis,
                                            front_axis,
                                            false,
                                            command_sort_key(instruction_id, command_sequence),
                                            instruction.source_ref_index);
        }
        command_sequence++;
        mark_done();
        break;
      }
      case LN_OpCode::SlowFollow: {
        if (KX_GameObject *target = resolve_command_target(instruction)) {
          const float factor = ResolveFloatExpression(instruction.float_expr_index, 1.0f, context);
          KX_GameObject *follow_target = nullptr;
          if (instruction.secondary_value_expr_index != LN_INVALID_INDEX) {
            const LN_Value target_val = ResolveValueExpression(
                instruction.secondary_value_expr_index, MakeNoneValue(), context);
            follow_target = ResolveObjectValue(target_val);
          }
          if (follow_target) {
            command_buffer.AppendSlowFollow(target,
                                            follow_target,
                                            factor,
                                            0,
                                            command_sort_key(instruction_id, command_sequence),
                                            instruction.source_ref_index);
          }
        }
        command_sequence++;
        mark_done();
        break;
      }
      case LN_OpCode::VehicleApplyEngineForce:
      case LN_OpCode::VehicleApplyBraking:
      case LN_OpCode::VehicleApplySteering:
      case LN_OpCode::SetVehicleSuspensionCompression:
      case LN_OpCode::SetVehicleSuspensionStiffness:
      case LN_OpCode::SetVehicleSuspensionDamping:
      case LN_OpCode::SetVehicleWheelFriction: {
        const float value = ResolveFloatExpression(
            instruction.float_expr_index, instruction.vector_value.x(), context);
        const int32_t wheel_count = ResolveIntExpression(
            instruction.int_expr_index, instruction.int_value, context);
        const int32_t axis = ResolveIntExpression(
            instruction.secondary_int_expr_index, instruction.secondary_int_value, context);
        if (KX_GameObject *target = resolve_command_target(instruction)) {
          const uint64_t sort_key = command_sort_key(instruction_id, command_sequence);
          switch (instruction.opcode) {
            case LN_OpCode::VehicleApplyEngineForce:
              command_buffer.AppendVehicleApplyEngineForce(
                  target, value, wheel_count, axis, sort_key, instruction.source_ref_index);
              break;
            case LN_OpCode::VehicleApplyBraking:
              command_buffer.AppendVehicleApplyBraking(
                  target, value, wheel_count, axis, sort_key, instruction.source_ref_index);
              break;
            case LN_OpCode::VehicleApplySteering:
              command_buffer.AppendVehicleApplySteering(
                  target, value, wheel_count, axis, sort_key, instruction.source_ref_index);
              break;
            case LN_OpCode::SetVehicleSuspensionCompression:
              command_buffer.AppendSetVehicleSuspensionCompression(
                  target, value, wheel_count, axis, sort_key, instruction.source_ref_index);
              break;
            case LN_OpCode::SetVehicleSuspensionStiffness:
              command_buffer.AppendSetVehicleSuspensionStiffness(
                  target, value, wheel_count, axis, sort_key, instruction.source_ref_index);
              break;
            case LN_OpCode::SetVehicleSuspensionDamping:
              command_buffer.AppendSetVehicleSuspensionDamping(
                  target, value, wheel_count, axis, sort_key, instruction.source_ref_index);
              break;
            case LN_OpCode::SetVehicleWheelFriction:
              command_buffer.AppendSetVehicleWheelFriction(
                  target, value, wheel_count, axis, sort_key, instruction.source_ref_index);
              break;
            default:
              break;
          }
          command_sequence++;
        }
        mark_done();
        break;
      }
      default:
        mark_done();
        break;
    }
  };

  auto execute_direct_game_property_command = [&](const LN_Instruction &instruction,
                                                  const uint32_t instruction_id) {
    const LN_GamePropertyCommandPayload payload = game_property_command_payload(instruction);
    if (payload.property_ref_index == LN_INVALID_INDEX || m_program == nullptr ||
        payload.property_ref_index >= m_program->GetGamePropertyRefs().size())
    {
      return;
    }

    if (payload.modify_op != LN_PropertyModifyOp::None) {
      LN_PropertyModify modify;
      modify.op = payload.modify_op;
      modify.operand = ResolveFloatExpression(payload.float_expr_index, 0.0f, context);
      modify.clamped = payload.modify_min_expr_index != LN_INVALID_INDEX &&
                       payload.modify_max_expr_index != LN_INVALID_INDEX;
      if (modify.clamped) {
        modify.min_value = ResolveFloatExpression(payload.modify_min_expr_index, 0.0f, context);
        modify.max_value = ResolveFloatExpression(payload.modify_max_expr_index, 0.0f, context);
      }
      if (KX_GameObject *target = resolve_object_expression(payload.object_value_expr_index)) {
        command_buffer.AppendModifyGameProperty(this,
                                                target,
                                                payload.property_ref_index,
                                                modify,
                                                command_sort_key(instruction_id, command_sequence),
                                                instruction.source_ref_index);
      }
      command_sequence++;
      MarkInstructionExecuted(event, instruction_id, context.tick_index);
      return;
    }

    LN_Value value = ResolveValueExpression(payload.value_expr_index, MakeNoneValue(), context);
    if (!value.exists && payload.value_expr_index == LN_INVALID_INDEX) {
      value.exists = true;
      value.type = payload.value_type;
      switch (payload.value_type) {
        case LN_ValueType::Bool:
          value.bool_value = ResolveBoolExpression(payload.bool_expr_index, false, context);
          break;
        case LN_ValueType::Int:
          value.int_value = ResolveIntExpression(payload.int_expr_index, 0, context);
          break;
        case LN_ValueType::Float:
          value.float_value = ResolveFloatExpression(payload.float_expr_index, 0.0f, context);
          break;
        case LN_ValueType::String:
          value.string_value = ResolveStringExpression(payload.string_expr_index, "", context);
          break;
        case LN_ValueType::Vector:
          value.vector_value = ResolveVectorExpression(
              payload.vector_expr_index, MT_Vector3(0.0f, 0.0f, 0.0f), context);
          break;
        case LN_ValueType::Color:
          value.color_value = ResolveColorExpression(
              payload.color_expr_index, MT_Vector4(0.0f, 0.0f, 0.0f, 1.0f), context);
          break;
        default:
          value.exists = false;
          break;
      }
    }

    if (KX_GameObject *target = resolve_object_expression(payload.object_value_expr_index)) {
      command_buffer.AppendSetGamePropertyRef(this,
                                              target,
                                              payload.property_ref_index,
                                              value,
                                              command_sort_key(instruction_id, command_sequence),
                                              instruction.source_ref_index);
    }
    command_sequence++;
    MarkInstructionExecuted(event, instruction_id, context.tick_index);
  };

  auto execute_direct_object_color_command = [&](const LN_Instruction &instruction,
                                                 const uint32_t instruction_id) {
    const MT_Vector4 color = ResolveColorExpression(
        instruction.color_expr_index, instruction.color_value, context);
    if (KX_GameObject *target = resolve_command_target(instruction)) {
      command_buffer.AppendSetObjectColor(target,
                                          color,
                                          command_sort_key(instruction_id, command_sequence),
                                          instruction.source_ref_index);
    }
    command_sequence++;
    MarkInstructionExecuted(event, instruction_id, context.tick_index);
  };

  auto execute_direct_object_state_command = [&](const LN_Instruction &instruction,
                                                 const uint32_t instruction_id) {
    switch (instruction.opcode) {
      case LN_OpCode::SetVisibility: {
        const bool visible = ResolveBoolExpression(instruction.bool_expr_index, true, context);
        const bool recursive = ResolveBoolExpression(
            instruction.secondary_bool_expr_index, false, context);
        if (KX_GameObject *target = resolve_command_target(instruction)) {
          command_buffer.AppendSetVisibility(target,
                                             visible,
                                             recursive,
                                             command_sort_key(instruction_id, command_sequence),
                                             instruction.source_ref_index);
        }
        break;
      }
      case LN_OpCode::SetUIInteractable: {
        const bool interactable = ResolveBoolExpression(instruction.bool_expr_index, true, context);
        if (KX_GameObject *target = resolve_command_target(instruction)) {
          command_buffer.AppendSetUIInteractable(target,
                                                 interactable,
                                                 command_sort_key(instruction_id, command_sequence),
                                                 instruction.source_ref_index);
        }
        break;
      }
      case LN_OpCode::ResetSmoothing: {
        /* Set Object Attribute's Teleport, right after its write. */
        if (!ResolveBoolExpression(instruction.bool_expr_index, false, context)) {
          break;
        }
        if (KX_GameObject *target = resolve_command_target(instruction)) {
          command_buffer.AppendResetSmoothing(target,
                                              command_sort_key(instruction_id, command_sequence),
                                              instruction.source_ref_index);
        }
        break;
      }
      default:
        break;
    }
    command_sequence++;
    MarkInstructionExecuted(event, instruction_id, context.tick_index);
  };

  auto execute_direct_camera_command = [&](const LN_Instruction &instruction,
                                           const uint32_t instruction_id) {
    KX_GameObject *camera_object = resolve_required_object(instruction.value_expr_index);
    const uint64_t sort_key = command_sort_key(instruction_id, command_sequence);
    switch (instruction.opcode) {
      case LN_OpCode::SetActiveCamera: {
        /* User 0 sets the active camera; 1 to 7 a split screen player's camera. */
        const int32_t user = ResolveIntExpression(instruction.int_expr_index, 0, context);
        if (user == 0) {
          command_buffer.AppendSetActiveCamera(
              camera_object, sort_key, instruction.source_ref_index);
        }
        else {
          command_buffer.AppendSetPlayerCamera(
              m_gameObject, camera_object, user, sort_key, instruction.source_ref_index);
        }
        break;
      }
      case LN_OpCode::SetSplitScreen: {
        const bool enabled = ResolveBoolExpression(instruction.bool_expr_index, true, context);
        const int32_t divider = ResolveIntExpression(instruction.int_expr_index, 0, context);
        const bool keep_mouse = ResolveBoolExpression(
            instruction.secondary_bool_expr_index, true, context);
        command_buffer.AppendSetSplitScreen(m_gameObject,
                                            enabled,
                                            instruction.int_value |
                                                (keep_mouse ? 0 : LN_SPLIT_SCREEN_FREE_MOUSE),
                                            divider,
                                            sort_key,
                                            instruction.source_ref_index);
        break;
      }
      case LN_OpCode::SetCameraViewport: {
        const bool enabled = ResolveBoolExpression(instruction.bool_expr_index, true, context);
        const MT_Vector4 area(
            ResolveFloatExpression(instruction.float_expr_index, 0.7f, context),
            ResolveFloatExpression(instruction.secondary_float_expr_index, 0.05f, context),
            ResolveFloatExpression(instruction.tertiary_float_expr_index, 0.25f, context),
            ResolveFloatExpression(instruction.quaternary_float_expr_index, 0.25f, context));
        const bool on_top = ResolveBoolExpression(
            instruction.secondary_bool_expr_index, true, context);
        /* Area Player's View: the rectangle is a part of that player's view (a User that is no
         * player has no view: not drawn). */
        const int32_t user = ResolveIntExpression(instruction.int_expr_index, 0, context);
        const int32_t player = instruction.secondary_int_value != 1 ? -1 :
                               user >= 0                          ? user :
                                                                    INT32_MAX;
        command_buffer.AppendSetCameraViewport(m_gameObject,
                                               camera_object,
                                               enabled,
                                               area,
                                               on_top,
                                               player,
                                               sort_key,
                                               instruction.source_ref_index);
        break;
      }
      case LN_OpCode::SetCameraVisibility: {
        /* Object: empty is the tree's own object. */
        KX_GameObject *target = instruction.secondary_value_expr_index != LN_INVALID_INDEX ?
                                    resolve_required_object(
                                        instruction.secondary_value_expr_index) :
                                    m_gameObject;
        command_buffer.AppendSetCameraVisibility(
            target,
            camera_object,
            ResolveBoolExpression(instruction.bool_expr_index, false, context),
            ResolveBoolExpression(instruction.secondary_bool_expr_index, false, context),
            ResolveBoolExpression(instruction.tertiary_bool_expr_index, false, context),
            sort_key,
            instruction.source_ref_index);
        break;
      }
      case LN_OpCode::SetCameraFov: {
        const float fov = ResolveFloatExpression(instruction.float_expr_index, 90.0f, context);
        command_buffer.AppendSetCameraFov(
            camera_object, fov, sort_key, instruction.source_ref_index);
        break;
      }
      case LN_OpCode::SetCameraOrthoScale: {
        const float scale = ResolveFloatExpression(instruction.float_expr_index, 1.0f, context);
        command_buffer.AppendSetCameraOrthoScale(
            camera_object, scale, sort_key, instruction.source_ref_index);
        break;
      }
      default:
        break;
    }
    command_sequence++;
    MarkInstructionExecuted(event, instruction_id, context.tick_index);
  };

  auto execute_direct_light_command = [&](const LN_Instruction &instruction,
                                          const uint32_t instruction_id) {
    KX_GameObject *target = resolve_command_target(instruction);
    if (target != nullptr) {
      const uint64_t sort_key = command_sort_key(instruction_id, command_sequence);
      switch (instruction.opcode) {
        case LN_OpCode::MakeLightUnique: {
          command_buffer.AppendMakeLightUnique(target, sort_key, instruction.source_ref_index);
          break;
        }
        case LN_OpCode::SetLightColor: {
          const MT_Vector4 color = ResolveColorExpression(
              instruction.color_expr_index, instruction.color_value, context);
          command_buffer.AppendSetLightColor(
              target, color, sort_key, instruction.source_ref_index);
          break;
        }
        case LN_OpCode::SetLightPower: {
          const float power = ResolveFloatExpression(
              instruction.float_expr_index, instruction.vector_value.x(), context);
          command_buffer.AppendSetLightPower(
              target, power, sort_key, instruction.source_ref_index);
          break;
        }
        case LN_OpCode::SetLightShadow: {
          const bool use_shadow = ResolveBoolExpression(
              instruction.bool_expr_index, true, context);
          command_buffer.AppendSetLightShadow(
              target, use_shadow, sort_key, instruction.source_ref_index);
          break;
        }
        default:
          break;
      }
    }
    command_sequence++;
    MarkInstructionExecuted(event, instruction_id, context.tick_index);
  };

  auto execute_direct_window_command = [&](const LN_Instruction &instruction,
                                           const uint32_t instruction_id) {
    switch (instruction.opcode) {
      case LN_OpCode::SetWindowSize: {
        const int32_t width = std::max(
            1, ResolveIntExpression(instruction.int_expr_index, 1, context));
        const int32_t height = std::max(
            1, ResolveIntExpression(instruction.secondary_int_expr_index, 1, context));
        command_buffer.AppendSetWindowSize(width,
                                           height,
                                           command_sort_key(instruction_id, command_sequence),
                                           instruction.source_ref_index);
        break;
      }
      case LN_OpCode::SetFullscreen: {
        const bool fullscreen = ResolveBoolExpression(instruction.bool_expr_index, false, context);
        command_buffer.AppendSetFullscreen(fullscreen,
                                           command_sort_key(instruction_id, command_sequence),
                                           instruction.source_ref_index);
        break;
      }
      case LN_OpCode::SetVSync: {
        const int32_t mode = ResolveIntExpression(instruction.int_expr_index, VSYNC_OFF, context);
        command_buffer.AppendSetVSync(mode,
                                      command_sort_key(instruction_id, command_sequence),
                                      instruction.source_ref_index);
        break;
      }
      case LN_OpCode::SetShowFramerate: {
        const bool show = ResolveBoolExpression(instruction.bool_expr_index, false, context);
        command_buffer.AppendSetShowFramerate(show,
                                              command_sort_key(instruction_id, command_sequence),
                                              instruction.source_ref_index);
        break;
      }
      case LN_OpCode::SetShowProfile: {
        const bool show = ResolveBoolExpression(instruction.bool_expr_index, false, context);
        command_buffer.AppendSetShowProfile(show,
                                            command_sort_key(instruction_id, command_sequence),
                                            instruction.source_ref_index);
        break;
      }
      case LN_OpCode::SetCursorVisibility: {
        const bool visible = ResolveBoolExpression(instruction.bool_expr_index, true, context);
        command_buffer.AppendSetCursorVisibility(visible,
                                                 command_sort_key(instruction_id, command_sequence),
                                                 instruction.source_ref_index,
                                                 instruction.int_value != 0);
        break;
      }
      case LN_OpCode::SetCursorPosition: {
        if (KX_KetsjiEngine *engine = KX_GetActiveEngine()) {
          if (RAS_ICanvas *canvas = engine->GetCanvas()) {
            const MT_Vector3 position = ResolveVectorExpression(
                instruction.vector_expr_index, instruction.vector_value, context);
            const int32_t x = int32_t(
                std::lround(std::clamp(position.x(), 0.0f, 1.0f) * float(canvas->GetWidth())));
            const int32_t y = int32_t(
                std::lround(std::clamp(position.y(), 0.0f, 1.0f) * float(canvas->GetHeight())));
            command_buffer.AppendSetCursorPosition(
                x,
                y,
                command_sort_key(instruction_id, command_sequence),
                instruction.source_ref_index);
          }
        }
        break;
      }
      case LN_OpCode::SetPlayerGamepad:
        command_buffer.AppendSetPlayerGamepad(
            ResolveIntExpression(instruction.int_expr_index, 0, context),
            ResolveIntExpression(instruction.secondary_int_expr_index, 0, context),
            command_sort_key(instruction_id, command_sequence),
            instruction.source_ref_index);
        break;
      case LN_OpCode::SetGamepadVibration: {
        const int32_t gamepad_index = ResolveIntExpression(instruction.int_expr_index, 0, context);
        const MT_Vector3 vibration = ResolveVectorExpression(
            instruction.vector_expr_index, instruction.vector_value, context);
        const float left = std::clamp(vibration.x(), 0.0f, 1.0f);
        const float right = std::clamp(vibration.y(), 0.0f, 1.0f);
        const uint32_t duration_ms = uint32_t(
            std::max<int32_t>(0, int32_t(std::lround(std::max(vibration.z(), 0.0f) * 1000.0f))));
        command_buffer.AppendSetGamepadVibration(
            gamepad_index,
            left,
            right,
            duration_ms,
            command_sort_key(instruction_id, command_sequence),
            instruction.source_ref_index);
        break;
      }
      default:
        break;
    }
    command_sequence++;
    MarkInstructionExecuted(event, instruction_id, context.tick_index);
  };

  auto execute_direct_input_motion_command = [&](const LN_Instruction &instruction,
                                                 const uint32_t instruction_id) {
    if (instruction.opcode == LN_OpCode::MouseLook) {
      execute_mouse_look_command(instruction, instruction_id);
      return;
    }

    KX_GameObject *body = ResolveObjectValue(
        ResolveValueExpression(instruction.value_expr_index, MakeNoneValue(), context));
    if (body == nullptr) {
      body = m_gameObject;
    }

    KX_GameObject *head = ResolveObjectValue(
        ResolveValueExpression(instruction.property_ref_index, MakeNoneValue(), context));
    if (head == nullptr) {
      head = body;
    }
    if (body == nullptr || head == nullptr) {
      command_sequence++;
      MarkInstructionExecuted(event, instruction_id, context.tick_index);
      return;
    }

    if (instruction.opcode == LN_OpCode::GamepadLook) {
      /* While a menu or a UI in the game world uses the gamepad's player, the view waits. */
      if (context.ui_input_users != 0 && instruction.secondary_int_expr_index != LN_INVALID_INDEX)
      {
        const int32_t user = LN_InputMap::Get().UserOfGamepad(
            ResolveIntExpression(instruction.secondary_int_expr_index, 0, context));
        if (user >= 0 && user < LN_MAX_GAMEPADS &&
            (context.ui_input_users & (1u << uint32_t(user))) != 0)
        {
          command_sequence++;
          MarkInstructionExecuted(event, instruction_id, context.tick_index);
          return;
        }
      }
      MT_Vector3 stick = ResolveVectorExpression(
          instruction.vector_expr_index, MT_Vector3(0.0f, 0.0f, 0.0f), context);
      if (stick.length2() <= (MT_EPSILON * MT_EPSILON)) {
        command_sequence++;
        MarkInstructionExecuted(event, instruction_id, context.tick_index);
        return;
      }

      const MT_Vector3 invert = ResolveVectorExpression(
          instruction.secondary_vector_expr_index,
          MT_Vector3(instruction.vector_value.x(), instruction.vector_value.y(), 0.0f),
          context);
      const MT_Vector3 cap_x = ResolveVectorExpression(
          instruction.color_expr_index,
          MT_Vector3(instruction.secondary_vector_value.x(),
                     instruction.secondary_vector_value.y(),
                     0.0f),
          context);
      const MT_Vector3 cap_y = ResolveVectorExpression(
          instruction.int_expr_index,
          MT_Vector3(instruction.color_value.x(), instruction.color_value.y(), 0.0f),
          context);
      const bool use_cap_x = ResolveBoolExpression(
          instruction.bool_expr_index, instruction.secondary_vector_value.z() > 0.5f, context);
      const bool use_cap_y = ResolveBoolExpression(
          instruction.secondary_bool_expr_index, instruction.color_value.w() > 0.5f, context);
      const float sensitivity = std::max(0.0f,
                                         ResolveFloatExpression(instruction.float_expr_index,
                                                                instruction.vector_value.z(),
                                                                context));
      const float exponent = std::max(0.0f,
                                      ResolveFloatExpression(instruction.string_expr_index,
                                                             instruction.color_value.z(),
                                                             context));

      /* The stick to the right turns right (clockwise seen from above), up looks up. */
      float yaw_delta = -stick.x();
      float pitch_delta = stick.y();
      if (std::fabs(yaw_delta) > MT_EPSILON) {
        yaw_delta = std::copysign(std::pow(std::fabs(yaw_delta), exponent), yaw_delta);
      }
      if (std::fabs(pitch_delta) > MT_EPSILON) {
        pitch_delta = std::copysign(std::pow(std::fabs(pitch_delta), exponent), pitch_delta);
      }

      if (std::fabs(invert.x()) > 0.5f) {
        yaw_delta = -yaw_delta;
      }
      if (std::fabs(invert.y()) > 0.5f) {
        pitch_delta = -pitch_delta;
      }

      const float tick_seconds = float(std::max(0.0, context.delta_time));
      yaw_delta *= sensitivity * tick_seconds;
      pitch_delta *= sensitivity * tick_seconds;
      if (std::fabs(yaw_delta) <= MT_EPSILON && std::fabs(pitch_delta) <= MT_EPSILON) {
        command_sequence++;
        MarkInstructionExecuted(event, instruction_id, context.tick_index);
        return;
      }

      MT_Vector3 body_euler = OrientationToEuler(body->NodeGetLocalOrientation());
      MT_Vector3 head_euler = (head == body) ? body_euler :
                                               OrientationToEuler(head->NodeGetLocalOrientation());

      body_euler[2] += yaw_delta;
      if (use_cap_x) {
        const float min_yaw = std::min(cap_x.x(), cap_x.y());
        const float max_yaw = std::max(cap_x.x(), cap_x.y());
        body_euler[2] = std::clamp(body_euler[2], min_yaw, max_yaw);
      }

      head_euler[0] += pitch_delta;
      if (use_cap_y) {
        const float min_pitch = std::min(cap_y.x(), cap_y.y());
        const float max_pitch = std::max(cap_y.x(), cap_y.y());
        head_euler[0] = std::clamp(head_euler[0], min_pitch, max_pitch);
      }

      if (head == body) {
        body_euler[0] = head_euler[0];
        command_buffer.AppendSetLocalOrientation(
            body,
            body_euler,
            command_sort_key(instruction_id, command_sequence),
            instruction.source_ref_index);
        command_sequence++;
      }
      else {
        command_buffer.AppendSetLocalOrientation(
            body,
            body_euler,
            command_sort_key(instruction_id, command_sequence),
            instruction.source_ref_index);
        command_sequence++;
        command_buffer.AppendSetLocalOrientation(
            head,
            head_euler,
            command_sort_key(instruction_id, command_sequence),
            instruction.source_ref_index);
        command_sequence++;
      }

      MarkInstructionExecuted(event, instruction_id, context.tick_index);
      return;
    }

    MarkInstructionExecuted(event, instruction_id, context.tick_index);
  };

  auto execute_direct_audio_command = [&](const LN_Instruction &instruction,
                                          const uint32_t instruction_id) {
    switch (instruction.opcode) {
      case LN_OpCode::StopAllSounds: {
        command_buffer.AppendStopAllSounds(command_sort_key(instruction_id, command_sequence),
                                           instruction.source_ref_index);
        break;
      }
      case LN_OpCode::PlaySound: {
        const std::string sound_name = ResolveStringExpression(
            instruction.string_expr_index, std::string(), context);
        if (sound_name.empty()) {
          return;
        }
        const float volume = ResolveFloatExpression(
            instruction.float_expr_index, instruction.vector_value.x(), context);
        const float pitch = ResolveFloatExpression(
            instruction.secondary_float_expr_index, instruction.vector_value.y(), context);
        const bool loop = ResolveBoolExpression(
            instruction.bool_expr_index, instruction.bool_value, context);
        command_buffer.AppendPlaySound(sound_name,
                                       volume,
                                       pitch,
                                       loop,
                                       command_sort_key(instruction_id, command_sequence),
                                       instruction.source_ref_index,
                                       instruction.secondary_int_value);
        break;
      }
      case LN_OpCode::SetSoundVolume: {
        const float volume = ResolveFloatExpression(instruction.float_expr_index, 1.0f, context);
        const float fade = ResolveFloatExpression(
            instruction.secondary_float_expr_index, 0.0f, context);
        command_buffer.AppendSetSoundVolume(instruction.int_value,
                                            volume,
                                            fade,
                                            command_sort_key(instruction_id, command_sequence),
                                            instruction.source_ref_index);
        break;
      }
      case LN_OpCode::StopSound: {
        const std::string sound_name = ResolveStringExpression(
            instruction.string_expr_index, std::string(), context);
        if (sound_name.empty()) {
          return;
        }
        command_buffer.AppendStopSound(sound_name,
                                       command_sort_key(instruction_id, command_sequence),
                                       instruction.source_ref_index);
        break;
      }
      case LN_OpCode::PlaySound3D: {
        KX_GameObject *speaker = nullptr;
        if (instruction.value_expr_index != LN_INVALID_INDEX) {
          const LN_Value speaker_value = ResolveValueExpression(
              instruction.value_expr_index, MakeNoneValue(), context);
          speaker = ResolveObjectValue(speaker_value);
        }
        if (speaker == nullptr) {
          speaker = m_snapshot.GetObjectSnapshot().object;
        }
        float default_volume = instruction.vector_value.x();
        float default_pitch = instruction.vector_value.y();
        std::string sound_name = ResolveStringExpression(
            instruction.string_expr_index, std::string(), context);
        if (sound_name.empty()) {
          sound_name = SoundNameFromSpeakerObject(speaker, default_volume, default_pitch);
        }
        if (sound_name.empty()) {
          return;
        }
        const float volume = ResolveFloatExpression(
            instruction.float_expr_index, default_volume, context);
        const float pitch = ResolveFloatExpression(
            instruction.secondary_float_expr_index, default_pitch, context);
        const bool loop = instruction.bool_expr_index != LN_INVALID_INDEX ?
                              ResolveBoolExpression(
                                  instruction.bool_expr_index, instruction.bool_value, context) :
                              ResolveIntExpression(instruction.int_expr_index, 0, context) != 0;
        command_buffer.AppendPlaySound3D(
            speaker,
            sound_name,
            volume,
            pitch,
            loop,
            command_sort_key(instruction_id, command_sequence),
            instruction.source_ref_index,
            instruction.int_value,
            ResolveIntExpression(instruction.secondary_int_expr_index, 0, context),
            instruction.secondary_int_value);
        break;
      }
      case LN_OpCode::PauseSound: {
        const std::string sound_name = ResolveStringExpression(
            instruction.string_expr_index, std::string(), context);
        if (!sound_name.empty()) {
          command_buffer.AppendPauseSound(sound_name,
                                          command_sort_key(instruction_id, command_sequence),
                                          instruction.source_ref_index);
        }
        break;
      }
      case LN_OpCode::ResumeSound: {
        const std::string sound_name = ResolveStringExpression(
            instruction.string_expr_index, std::string(), context);
        if (!sound_name.empty()) {
          command_buffer.AppendResumeSound(sound_name,
                                           command_sort_key(instruction_id, command_sequence),
                                           instruction.source_ref_index);
        }
        break;
      }
      default:
        break;
    }
    command_sequence++;
    MarkInstructionExecuted(event, instruction_id, context.tick_index);
  };

  auto execute_direct_action_command = [&](const LN_Instruction &instruction,
                                           const uint32_t instruction_id) {
    KX_GameObject *target_object = resolve_command_target(instruction);
    if (target_object == nullptr && instruction.opcode != LN_OpCode::PlayAction) {
      return;
    }

    const uint64_t sort_key = command_sort_key(instruction_id, command_sequence);
    switch (instruction.opcode) {
      case LN_OpCode::PlayAction: {
        const std::string action_name = ResolveStringExpression(
            instruction.string_expr_index, std::string(), context);
        const float start_frame = ResolveFloatExpression(
            instruction.float_expr_index, instruction.vector_value.x(), context);
        const float end_frame = ResolveFloatExpression(
            instruction.secondary_float_expr_index, instruction.vector_value.y(), context);
        const float blendin = ResolveFloatExpression(
            instruction.tertiary_float_expr_index, instruction.vector_value.z(), context);
        const float playback_speed = ResolveFloatExpression(
            instruction.quaternary_float_expr_index,
            instruction.secondary_vector_value.y(),
            context);
        const float layer_weight = instruction.secondary_vector_value.x();
        const int32_t layer = ResolveIntExpression(instruction.int_expr_index, 0, context);
        const int32_t priority = ResolveIntExpression(
            instruction.secondary_int_expr_index, 0, context);
        command_buffer.AppendPlayAction(target_object,
                                        this,
                                        action_name,
                                        start_frame,
                                        end_frame,
                                        layer,
                                        priority,
                                        blendin,
                                        layer_weight,
                                        uint32_t(instruction.int_value),
                                        playback_speed,
                                        instruction.property_ref_index,
                                        uint8_t(instruction.secondary_int_value),
                                        sort_key,
                                        instruction.source_ref_index);
        break;
      }
      case LN_OpCode::StopAction: {
        const int32_t layer = ResolveIntExpression(
            instruction.int_expr_index, instruction.int_value, context);
        command_buffer.AppendStopAction(
            target_object, layer, sort_key, instruction.source_ref_index);
        break;
      }
      case LN_OpCode::SetActionFrame: {
        const int32_t layer = ResolveIntExpression(
            instruction.int_expr_index, instruction.int_value, context);
        const float frame = ResolveFloatExpression(
            instruction.float_expr_index, instruction.vector_value.x(), context);
        command_buffer.AppendSetActionFrame(
            target_object, layer, frame, sort_key, instruction.source_ref_index);
        break;
      }
      case LN_OpCode::SetShapeKeyValue: {
        const std::string shape_key_name = ResolveStringExpression(
            instruction.string_expr_index, std::string(), context);
        if (shape_key_name.empty()) {
          return;
        }
        const float value = ResolveFloatExpression(
            instruction.float_expr_index, instruction.vector_value.x(), context);
        command_buffer.AppendSetShapeKeyValue(
            target_object, shape_key_name, value, sort_key, instruction.source_ref_index);
        break;
      }
      default:
        break;
    }
    command_sequence++;
    MarkInstructionExecuted(event, instruction_id, context.tick_index);
  };

  auto execute_direct_bone_command = [&](const LN_Instruction &instruction,
                                         const uint32_t instruction_id) {
    const uint64_t sort_key = command_sort_key(instruction_id, command_sequence);
    switch (instruction.opcode) {
      case LN_OpCode::SetBonePoseLocation: {
        const MT_Vector3 location = ResolveVectorExpression(
            instruction.vector_expr_index, instruction.vector_value, context);
        const std::string bone_name = ResolveStringExpression(
            instruction.string_expr_index, std::string(), context);
        if (KX_GameObject *target = resolve_command_target(instruction)) {
          if (!bone_name.empty()) {
            command_buffer.AppendSetBonePoseLocation(target,
                                                     bone_name,
                                                     location,
                                                     sort_key,
                                                     instruction.source_ref_index,
                                                     instruction.int_value,
                                                     instruction.secondary_int_value != 0);
          }
        }
        break;
      }
      case LN_OpCode::SetBonePoseTransform: {
        const MT_Vector3 location = ResolveVectorExpression(
            instruction.vector_expr_index, instruction.vector_value, context);
        const LN_Rotation rotation = ResolveRotationExpression(
            instruction.secondary_vector_expr_index,
            LN_Rotation::FromEulerXYZ(instruction.secondary_vector_value),
            context);
        const std::string bone_name = ResolveStringExpression(
            instruction.string_expr_index, std::string(), context);
        if (KX_GameObject *target = resolve_command_target(instruction)) {
          if (!bone_name.empty()) {
            command_buffer.AppendSetBonePoseTransform(target,
                                                      bone_name,
                                                      location,
                                                      rotation,
                                                      sort_key,
                                                      instruction.source_ref_index,
                                                      instruction.int_value,
                                                      instruction.secondary_int_value != 0);
          }
        }
        break;
      }
      case LN_OpCode::SetBonePoseRotation: {
        const LN_Rotation rotation = ResolveRotationExpression(
            instruction.vector_expr_index,
            LN_Rotation::FromEulerXYZ(instruction.vector_value),
            context);
        const std::string bone_name = ResolveStringExpression(
            instruction.string_expr_index, std::string(), context);
        if (KX_GameObject *target = resolve_command_target(instruction)) {
          if (!bone_name.empty()) {
            command_buffer.AppendSetBonePoseRotation(target,
                                                     bone_name,
                                                     rotation,
                                                     sort_key,
                                                     instruction.source_ref_index,
                                                     instruction.secondary_int_value != 0,
                                                     instruction.int_value);
          }
        }
        break;
      }
      case LN_OpCode::SetBonePoseScale: {
        const MT_Vector3 scale = ResolveVectorExpression(
            instruction.vector_expr_index, instruction.vector_value, context);
        const std::string bone_name = ResolveStringExpression(
            instruction.string_expr_index, std::string(), context);
        if (KX_GameObject *target = resolve_command_target(instruction)) {
          if (!bone_name.empty()) {
            command_buffer.AppendSetBonePoseScale(
                target, bone_name, scale, sort_key, instruction.source_ref_index);
          }
        }
        break;
      }
      case LN_OpCode::SetBoneAttribute: {
        const std::string bone_name = ResolveStringExpression(
            instruction.string_expr_index, std::string(), context);
        KX_GameObject *target = resolve_command_target(instruction);
        LN_Value value = MakeNoneValue();
        if (instruction.secondary_value_expr_index != LN_INVALID_INDEX) {
          value = ResolveValueExpression(instruction.secondary_value_expr_index, value, context);
        }
        if (target != nullptr && !bone_name.empty()) {
          command_buffer.AppendSetBoneAttribute(target,
                                                bone_name,
                                                instruction.int_value,
                                                instruction.secondary_int_value,
                                                value,
                                                sort_key,
                                                instruction.source_ref_index);
        }
        break;
      }
      case LN_OpCode::SetBoneConstraintInfluence: {
        const std::string bone_name = ResolveStringExpression(
            instruction.string_expr_index, std::string(), context);
        const std::string constraint_name = ResolveStringExpression(
            instruction.secondary_string_expr_index, std::string(), context);
        const float influence = ResolveFloatExpression(
            instruction.float_expr_index, 0.0f, context);
        if (KX_GameObject *target = resolve_command_target(instruction)) {
          if (!bone_name.empty() && !constraint_name.empty()) {
            command_buffer.AppendSetBoneConstraintInfluence(target,
                                                            bone_name,
                                                            constraint_name,
                                                            influence,
                                                            sort_key,
                                                            instruction.source_ref_index);
          }
        }
        break;
      }
      case LN_OpCode::SetBoneConstraintTarget: {
        const std::string bone_name = ResolveStringExpression(
            instruction.string_expr_index, std::string(), context);
        const std::string constraint_name = ResolveStringExpression(
            instruction.secondary_string_expr_index, std::string(), context);
        const LN_Value target_value = ResolveValueExpression(
            instruction.secondary_value_expr_index, MakeNoneValue(), context);
        if (KX_GameObject *target = resolve_command_target(instruction)) {
          if (!bone_name.empty() && !constraint_name.empty()) {
            command_buffer.AppendSetBoneConstraintTarget(this,
                                                         target,
                                                         bone_name,
                                                         constraint_name,
                                                         target_value,
                                                         sort_key,
                                                         instruction.source_ref_index);
          }
        }
        break;
      }
      case LN_OpCode::SetBoneConstraintAttribute: {
        const std::string bone_name = ResolveStringExpression(
            instruction.string_expr_index, std::string(), context);
        const std::string constraint_name = ResolveStringExpression(
            instruction.secondary_string_expr_index, std::string(), context);
        const std::string attribute_name = ResolveStringExpression(
            instruction.tertiary_string_expr_index, std::string(), context);
        const LN_Value value = ResolveValueExpression(
            instruction.secondary_value_expr_index, MakeNoneValue(), context);
        if (KX_GameObject *target = resolve_command_target(instruction)) {
          if (!bone_name.empty() && !constraint_name.empty() && !attribute_name.empty()) {
            command_buffer.AppendSetBoneConstraintAttribute(this,
                                                            target,
                                                            bone_name,
                                                            constraint_name,
                                                            attribute_name,
                                                            value,
                                                            sort_key,
                                                            instruction.source_ref_index);
          }
        }
        break;
      }
      default:
        break;
    }
    command_sequence++;
    MarkInstructionExecuted(event, instruction_id, context.tick_index);
  };

  auto execute_direct_material_command = [&](const LN_Instruction &instruction,
                                             const uint32_t instruction_id) {
    const uint64_t sort_key = command_sort_key(instruction_id, command_sequence);
    switch (instruction.opcode) {
      case LN_OpCode::SetMaterialSlot: {
        const LN_Value material_value = ResolveValueExpression(
            instruction.secondary_value_expr_index, MakeNoneValue(), context);
        const int32_t slot = ResolveIntExpression(instruction.int_expr_index, 0, context);
        if (KX_GameObject *target = resolve_command_target(instruction)) {
          if (slot >= 0) {
            command_buffer.AppendAssignMaterialToSlot(
                this, target, material_value, slot, sort_key, instruction.source_ref_index);
          }
        }
        break;
      }
      case LN_OpCode::SetMaterialNodeSocketValue: {
        const std::string node_name = ResolveStringExpression(
            instruction.string_expr_index, std::string(), context);
        const std::string socket_name = ResolveStringExpression(
            instruction.secondary_string_expr_index, std::string(), context);
        const LN_Value value = ResolveValueExpression(
            instruction.secondary_value_expr_index, MakeNoneValue(), context);
        if (!node_name.empty() && !socket_name.empty()) {
          if (instruction.tertiary_value_expr_index != LN_INVALID_INDEX) {
            const LN_Value material_value = ResolveValueExpression(
                instruction.tertiary_value_expr_index, MakeNoneValue(), context);
            if (material_value.exists) {
              command_buffer.AppendSetMaterialNodeSocketValue(this,
                                                              material_value,
                                                              node_name,
                                                              socket_name,
                                                              value,
                                                              sort_key,
                                                              instruction.source_ref_index);
            }
          }
          else if (KX_GameObject *target = resolve_command_target(instruction)) {
            const int32_t slot = ResolveIntExpression(instruction.int_expr_index, 0, context);
            command_buffer.AppendSetMaterialNodeSocketValue(this,
                                                            target,
                                                            MakeNoneValue(),
                                                            slot,
                                                            node_name,
                                                            socket_name,
                                                            value,
                                                            sort_key,
                                                            instruction.source_ref_index,
                                                            instruction.secondary_int_value == 0);
          }
        }
        break;
      }
      case LN_OpCode::SetMaterialParameter: {
        const std::string node_name = ResolveStringExpression(
            instruction.string_expr_index, std::string(), context);
        const std::string socket_name = ResolveStringExpression(
            instruction.secondary_string_expr_index, std::string(), context);
        const LN_Value value = ResolveValueExpression(
            instruction.secondary_value_expr_index, MakeNoneValue(), context);
        if (!node_name.empty() && !socket_name.empty()) {
          if (instruction.tertiary_value_expr_index != LN_INVALID_INDEX) {
            const LN_Value material_value = ResolveValueExpression(
                instruction.tertiary_value_expr_index, MakeNoneValue(), context);
            if (material_value.exists) {
              command_buffer.AppendSetMaterialParameter(this,
                                                        material_value,
                                                        node_name,
                                                        socket_name,
                                                        value,
                                                        sort_key,
                                                        instruction.source_ref_index);
            }
          }
          else if (KX_GameObject *target = resolve_command_target(instruction)) {
            const int32_t slot = ResolveIntExpression(instruction.int_expr_index, 0, context);
            command_buffer.AppendSetMaterialParameter(this,
                                                      target,
                                                      MakeNoneValue(),
                                                      slot,
                                                      node_name,
                                                      socket_name,
                                                      value,
                                                      sort_key,
                                                      instruction.source_ref_index);
          }
        }
        break;
      }
      case LN_OpCode::SetGeometryNodesInput: {
        const std::string modifier_name = ResolveStringExpression(
            instruction.string_expr_index, std::string(), context);
        const std::string input_identifier = ResolveStringExpression(
            instruction.secondary_string_expr_index, std::string(), context);
        const LN_Value value = ResolveValueExpression(
            instruction.secondary_value_expr_index, MakeNoneValue(), context);
        if (!modifier_name.empty() && !input_identifier.empty()) {
          if (KX_GameObject *target = resolve_command_target(instruction)) {
            command_buffer.AppendSetGeometryNodesInput(this,
                                                       target,
                                                       modifier_name,
                                                       input_identifier,
                                                       value,
                                                       sort_key,
                                                       instruction.source_ref_index);
          }
        }
        break;
      }
      case LN_OpCode::SetGeometryNodeSocketValue: {
        const std::string modifier_name = ResolveStringExpression(
            instruction.string_expr_index, std::string(), context);
        const std::string node_name = ResolveStringExpression(
            instruction.secondary_string_expr_index, std::string(), context);
        const std::string socket_identifier = ResolveStringExpression(
            instruction.tertiary_string_expr_index, std::string(), context);
        const LN_Value value = ResolveValueExpression(
            instruction.secondary_value_expr_index, MakeNoneValue(), context);
        if (!modifier_name.empty() && !node_name.empty() && !socket_identifier.empty()) {
          if (KX_GameObject *target = resolve_command_target(instruction)) {
            command_buffer.AppendSetGeometryNodeSocketValue(this,
                                                            target,
                                                            modifier_name,
                                                            node_name,
                                                            socket_identifier,
                                                            value,
                                                            sort_key,
                                                            instruction.source_ref_index);
          }
        }
        break;
      }
      case LN_OpCode::SetCompositorNodeSocketValue: {
        const std::string target_name = ResolveStringExpression(
            instruction.string_expr_index, std::string(), context);
        const std::string node_name = ResolveStringExpression(
            instruction.secondary_string_expr_index, std::string(), context);
        const std::string socket_identifier = ResolveStringExpression(
            instruction.tertiary_string_expr_index, std::string(), context);
        const LN_Value value = ResolveValueExpression(
            instruction.secondary_value_expr_index, MakeNoneValue(), context);
        if (!target_name.empty() && !node_name.empty() && !socket_identifier.empty()) {
          command_buffer.AppendSetCompositorNodeSocketValue(this,
                                                            int16_t(instruction.int_value),
                                                            target_name,
                                                            node_name,
                                                            socket_identifier,
                                                            value,
                                                            sort_key,
                                                            instruction.source_ref_index);
        }
        break;
      }
      case LN_OpCode::MakeNodeTreeUnique: {
        const std::string target_name = ResolveStringExpression(
            instruction.string_expr_index, std::string(), context);
        const int32_t slot = ResolveIntExpression(instruction.int_expr_index, 0, context);
        KX_GameObject *target = nullptr;
        if (instruction.int_value != 3) {
          target = resolve_command_target(instruction);
        }
        if (instruction.int_value == 3 || target != nullptr) {
          command_buffer.AppendMakeNodeTreeUnique(target,
                                                  instruction.int_value,
                                                  slot,
                                                  target_name,
                                                  sort_key,
                                                  instruction.source_ref_index);
        }
        break;
      }
      case LN_OpCode::SetNodeMute: {
        const std::string target_name = ResolveStringExpression(
            instruction.string_expr_index, std::string(), context);
        const std::string node_name = ResolveStringExpression(
            instruction.secondary_string_expr_index, std::string(), context);
        const bool muted = ResolveBoolExpression(instruction.bool_expr_index, true, context);
        if (!target_name.empty() && !node_name.empty()) {
          command_buffer.AppendSetNodeMute(this,
                                           int16_t(instruction.int_value),
                                           target_name,
                                           node_name,
                                           muted,
                                           sort_key,
                                           instruction.source_ref_index);
        }
        break;
      }
      case LN_OpCode::AssignGeometryNodesModifier: {
        LN_Value result;
        result.type = LN_ValueType::Int;
        result.exists = true;
        result.int_value = 0;
        if (instruction.property_ref_index != LN_INVALID_INDEX) {
          SetTreePropertyValue(instruction.property_ref_index, result);
        }
        const LN_Value node_group_value = ResolveValueExpression(
            instruction.secondary_value_expr_index, MakeNoneValue(), context);
        const std::string name = ResolveStringExpression(
            instruction.string_expr_index, std::string(), context);
        const int32_t index_or_id = ResolveIntExpression(instruction.int_expr_index, 0, context);
        if (KX_GameObject *target = resolve_command_target(instruction)) {
          result.int_value = command_buffer.AssignGeometryNodesModifierNow(
              this,
              target,
              node_group_value,
              static_cast<LN_CommandBuffer::ModifierAssignmentOperation>(instruction.int_value),
              static_cast<LN_CommandBuffer::ModifierTarget>(instruction.secondary_int_value),
              name,
              index_or_id);
          if (instruction.property_ref_index != LN_INVALID_INDEX) {
            SetTreePropertyValue(instruction.property_ref_index, result);
          }
        }
        break;
      }
      case LN_OpCode::EnableDisableModifier: {
        const auto target_mode = static_cast<LN_CommandBuffer::ModifierTarget>(
            instruction.int_value);
        const std::string modifier_name =
            target_mode == LN_CommandBuffer::ModifierTarget::Name ?
                ResolveStringExpression(instruction.string_expr_index, std::string(), context) :
                std::string();
        const int32_t index = ELEM(target_mode,
                                   LN_CommandBuffer::ModifierTarget::Index,
                                   LN_CommandBuffer::ModifierTarget::PersistentId) ?
                                  ResolveIntExpression(instruction.int_expr_index, 0, context) :
                                  0;
        const bool enabled = ResolveBoolExpression(instruction.bool_expr_index, true, context);
        if (KX_GameObject *target = resolve_command_target(instruction)) {
          command_buffer.AppendEnableDisableModifier(target,
                                                     target_mode,
                                                     modifier_name,
                                                     index,
                                                     enabled,
                                                     sort_key,
                                                     instruction.source_ref_index);
        }
        break;
      }
      default:
        break;
    }
    command_sequence++;
    MarkInstructionExecuted(event, instruction_id, context.tick_index);
  };

  auto execute_direct_global_command = [&](const LN_Instruction &instruction,
                                           const uint32_t instruction_id) {
    switch (instruction.opcode) {
      case LN_OpCode::Print: {
        const std::vector<LN_ValueExpression> &value_expressions =
            m_program->GetValueExpressions();
        const LN_ValueExpression *value_expression =
            instruction.value_expr_index < value_expressions.size() ?
                &value_expressions[instruction.value_expr_index] :
                nullptr;
        const std::vector<LN_TreePropertyRef> &tree_property_refs =
            m_program->GetTreePropertyRefs();
        const LN_TreePropertyRef *tree_property_ref = nullptr;
        if (value_expression != nullptr &&
            value_expression->property_ref_index < tree_property_refs.size())
        {
          tree_property_ref = &tree_property_refs[value_expression->property_ref_index];
        }
        const bool print_deferred_add_object_result =
            value_expression != nullptr &&
            value_expression->kind == LN_ValueExpressionKind::RuntimeTreeProperty &&
            tree_property_ref != nullptr &&
            tree_property_ref->name.rfind("__native_add_object_result_", 0) == 0;
        if (print_deferred_add_object_result && m_activeTreePropertySnapshot == nullptr) {
          command_buffer.AppendPrintTreeProperty(
              this,
              value_expression->property_ref_index,
              command_sort_key(instruction_id, command_sequence),
              instruction.source_ref_index);
        }
        else {
          const LN_Value value = ResolveValueExpression(
              instruction.value_expr_index, MakeNoneValue(), context);
          const std::string message = ValueToPrintString(value);
          command_buffer.AppendPrint(message,
                                     command_sort_key(instruction_id, command_sequence),
                                     instruction.source_ref_index);
        }
        break;
      }
      case LN_OpCode::QuitGame:
        command_buffer.AppendQuitGame(command_sort_key(instruction_id, command_sequence),
                                      instruction.source_ref_index);
        break;
      case LN_OpCode::RestartGame:
        command_buffer.AppendRestartGame(command_sort_key(instruction_id, command_sequence),
                                         instruction.source_ref_index);
        break;
      case LN_OpCode::SetTimeScale: {
        const float timescale = ResolveFloatExpression(
            instruction.float_expr_index, 1.0f, context);
        command_buffer.AppendSetTimeScale(m_gameObject,
                                          timescale,
                                          command_sort_key(instruction_id, command_sequence),
                                          instruction.source_ref_index);
        break;
      }
      case LN_OpCode::SetGamePaused: {
        const bool paused = ResolveBoolExpression(instruction.bool_expr_index, true, context);
        command_buffer.AppendSetGamePaused(paused,
                                           command_sort_key(instruction_id, command_sequence),
                                           instruction.source_ref_index);
        break;
      }
      case LN_OpCode::SetPhysicsPaused: {
        const bool paused = ResolveBoolExpression(instruction.bool_expr_index, true, context);
        command_buffer.AppendSetPhysicsPaused(paused,
                                              command_sort_key(instruction_id, command_sequence),
                                              instruction.source_ref_index);
        break;
      }
      default:
        break;
    }
    command_sequence++;
    MarkInstructionExecuted(event, instruction_id, context.tick_index);
  };

  auto execute_direct_file_command = [&](const LN_Instruction &instruction,
                                         const uint32_t instruction_id) {
    switch (instruction.opcode) {
      case LN_OpCode::LoadBlendFile: {
        const std::string filepath = ResolveStringExpression(
            instruction.string_expr_index, std::string(), context);
        command_buffer.AppendLoadBlendFile(filepath,
                                           command_sort_key(instruction_id, command_sequence),
                                           instruction.source_ref_index);
        break;
      }
      case LN_OpCode::SaveGame: {
        const int32_t slot = ResolveIntExpression(instruction.int_expr_index, 0, context);
        const std::string path = ResolveStringExpression(
            instruction.string_expr_index, std::string(), context);
        command_buffer.AppendSaveGame(this,
                                      m_gameObject,
                                      slot,
                                      path,
                                      instruction.bool_expr_index,
                                      command_sort_key(instruction_id, command_sequence),
                                      instruction.source_ref_index);
        break;
      }
      case LN_OpCode::SetSaveExistence: {
        KX_GameObject *target = resolve_command_target(instruction);
        const bool enabled = ResolveBoolExpression(instruction.bool_expr_index, true, context);
        command_buffer.AppendSetSaveExistence(this,
                                              m_gameObject,
                                              target,
                                              enabled,
                                              instruction.secondary_bool_expr_index,
                                              command_sort_key(instruction_id, command_sequence),
                                              instruction.source_ref_index);
        break;
      }
      case LN_OpCode::LoadGame: {
        const int32_t slot = ResolveIntExpression(instruction.int_expr_index, 0, context);
        const std::string path = ResolveStringExpression(
            instruction.string_expr_index, std::string(), context);
        command_buffer.AppendLoadGame(this,
                                      m_gameObject,
                                      slot,
                                      path,
                                      instruction.bool_expr_index,
                                      command_sort_key(instruction_id, command_sequence),
                                      instruction.source_ref_index);
        break;
      }
      default:
        break;
    }
    command_sequence++;
    MarkInstructionExecuted(event, instruction_id, context.tick_index);
  };

  auto execute_direct_scene_command = [&](const LN_Instruction &instruction,
                                          const uint32_t instruction_id) {
    switch (instruction.opcode) {
      case LN_OpCode::LoadScene: {
        const std::string scene_name = ResolveStringExpression(
            instruction.string_expr_index, std::string(), context);
        command_buffer.AppendLoadScene(m_gameObject,
                                       scene_name,
                                       command_sort_key(instruction_id, command_sequence),
                                       instruction.source_ref_index);
        break;
      }
      case LN_OpCode::SetScene: {
        const std::string scene_name = ResolveStringExpression(
            instruction.string_expr_index, std::string(), context);
        command_buffer.AppendSetScene(m_gameObject,
                                      scene_name,
                                      command_sort_key(instruction_id, command_sequence),
                                      instruction.source_ref_index);
        break;
      }
      default:
        break;
    }
    command_sequence++;
    MarkInstructionExecuted(event, instruction_id, context.tick_index);
  };

  auto execute_direct_lifecycle_command = [&](const LN_Instruction &instruction,
                                              const uint32_t instruction_id) {
    switch (instruction.opcode) {
      case LN_OpCode::AddObject: {
        auto clear_add_object_result = [&]() {
          if (instruction.property_ref_index != LN_INVALID_INDEX) {
            LN_Value empty_result;
            empty_result.type = LN_ValueType::ObjectRef;
            SetTreePropertyValue(instruction.property_ref_index, empty_result);
          }
        };

        KX_GameObject *reference_object = resolve_command_target(instruction);
        KX_GameObject *source_object = resolve_add_object_template(
            instruction.secondary_value_expr_index);
        if (reference_object != nullptr && source_object != nullptr) {
          command_buffer.FlushPendingObjectTransformCommands(reference_object);
          command_buffer.ExecuteAddObjectFromRefImmediate(
              this,
              reference_object,
              MakeObjectRef(source_object, source_object->GetName()),
              ResolveFloatExpression(instruction.float_expr_index, 0.0f, context),
              ResolveBoolExpression(instruction.bool_expr_index, false, context),
              ResolveBoolExpression(instruction.secondary_bool_expr_index, false, context),
              instruction.property_ref_index,
              instruction.source_ref_index);
          command_sequence++;
        }
        else {
          clear_add_object_result();
        }
        break;
      }
      case LN_OpCode::RemoveParent: {
        if (KX_GameObject *target = resolve_command_target(instruction)) {
          command_buffer.AppendRemoveParent(target,
                                            command_sort_key(instruction_id, command_sequence),
                                            instruction.source_ref_index);
          command_sequence++;
        }
        break;
      }
      case LN_OpCode::RemoveObject: {
        if (KX_GameObject *target = resolve_command_target(instruction)) {
          command_buffer.AppendRemoveObject(target,
                                            command_sort_key(instruction_id, command_sequence),
                                            instruction.source_ref_index);
          command_sequence++;
        }
        break;
      }
      default:
        break;
    }
    MarkInstructionExecuted(event, instruction_id, context.tick_index);
  };

  auto execute_direct_object_reference_command = [&](const LN_Instruction &instruction,
                                                     const uint32_t instruction_id) {
    switch (instruction.opcode) {
      case LN_OpCode::SetParent: {
        KX_GameObject *child_object = resolve_command_target(instruction);
        if (child_object == nullptr || instruction.secondary_value_expr_index == LN_INVALID_INDEX)
        {
          MarkInstructionExecuted(event, instruction_id, context.tick_index);
          break;
        }

        const LN_Value parent_value = ResolveValueExpression(
            instruction.secondary_value_expr_index, MakeNoneValue(), context);
        KX_GameObject *parent_object = ResolveObjectValue(parent_value);
        if (parent_object != nullptr) {
          LN_RuntimeRef parent_ref = parent_value.runtime_ref;
          if (parent_ref.kind != LN_RuntimeRefKind::Object ||
              ResolveObjectRef(parent_ref) != parent_object)
          {
            parent_ref = MakeObjectRef(parent_object, parent_value.reference_name);
          }
          if (instruction.bool_value) {
            KX_ParentOptions options;
            options.type = static_cast<KX_ParentType>(instruction.int_value);

            if (options.type == KX_ParentType::Object) {
              options.compound = ResolveBoolExpression(
                  instruction.bool_expr_index, false, context);
            }
            else if (options.type == KX_ParentType::Bone) {
              options.bone_name = ResolveStringExpression(
                  instruction.string_expr_index, std::string(), context);
            }

            command_buffer.AppendSetParentFromRef(
                this,
                child_object,
                parent_ref,
                options,
                command_sort_key(instruction_id, command_sequence),
                instruction.source_ref_index);
          }
          else {
            command_buffer.AppendSetParentFromRef(
                this,
                child_object,
                parent_ref,
                ResolveBoolExpression(instruction.bool_expr_index, true, context),
                ResolveBoolExpression(instruction.secondary_bool_expr_index, true, context),
                command_sort_key(instruction_id, command_sequence),
                instruction.source_ref_index);
          }
          command_sequence++;
        }
        MarkInstructionExecuted(event, instruction_id, context.tick_index);
        break;
      }
      case LN_OpCode::ReplaceMesh: {
        if (KX_GameObject *target = resolve_command_target(instruction)) {
          KX_GameObject *mesh_obj = nullptr;
          if (instruction.secondary_value_expr_index != LN_INVALID_INDEX) {
            const LN_Value mesh_val = ResolveValueExpression(
                instruction.secondary_value_expr_index, MakeNoneValue(), context);
            /* A mesh library may sit in a hidden collection (inactive objects). */
            mesh_obj = ResolveObjectValue(mesh_val, true);
          }
          if (mesh_obj) {
            command_buffer.AppendReplaceMesh(target,
                                             mesh_obj,
                                             command_sort_key(instruction_id, command_sequence),
                                             instruction.source_ref_index);
          }
        }
        command_sequence++;
        MarkInstructionExecuted(event, instruction_id, context.tick_index);
        break;
      }
      case LN_OpCode::CopyProperty: {
        /* No Source input: the owner. A linked Source that gives no object copies nothing. */
        KX_GameObject *source = m_gameObject;
        if (instruction.value_expr_index != LN_INVALID_INDEX) {
          source = ResolveObjectValue(
              ResolveValueExpression(instruction.value_expr_index, MakeNoneValue(), context));
        }
        KX_GameObject *target = nullptr;
        if (instruction.secondary_value_expr_index != LN_INVALID_INDEX) {
          const LN_Value tgt_val = ResolveValueExpression(
              instruction.secondary_value_expr_index, MakeNoneValue(), context);
          target = ResolveObjectValue(tgt_val);
        }
        const std::string property = ResolveStringExpression(
            instruction.string_expr_index, std::string(), context);
        if (source && target && !property.empty()) {
          command_buffer.AppendCopyProperty(source,
                                            target,
                                            property,
                                            command_sort_key(instruction_id, command_sequence),
                                            instruction.source_ref_index);
        }
        command_sequence++;
        MarkInstructionExecuted(event, instruction_id, context.tick_index);
        break;
      }
      default:
        MarkInstructionExecuted(event, instruction_id, context.tick_index);
        break;
    }
  };

  auto execute_direct_tree_property_command = [&](const LN_Instruction &instruction,
                                                  const uint32_t instruction_id) {
    if (instruction.property_ref_index == LN_INVALID_INDEX || m_program == nullptr ||
        instruction.property_ref_index >= m_program->GetTreePropertyRefs().size())
    {
      return;
    }

    const std::vector<LN_TreePropertyRef> &property_refs = m_program->GetTreePropertyRefs();
    LN_Value value = ResolveValueExpression(
        instruction.value_expr_index,
        property_refs[instruction.property_ref_index].default_value,
        context);
    if (instruction.value_expr_index == LN_INVALID_INDEX) {
      value = property_refs[instruction.property_ref_index].default_value;
    }

    SetTreePropertyValue(instruction.property_ref_index, value);
    command_buffer.AppendSetTreeProperty(this,
                                         instruction.property_ref_index,
                                         value,
                                         command_sort_key(instruction_id, command_sequence),
                                         instruction.source_ref_index);
    command_sequence++;
    MarkInstructionExecuted(event, instruction_id, context.tick_index);
  };

  auto execute_direct_tree_control_command = [&](const LN_Instruction &instruction,
                                                 const uint32_t instruction_id) {
    KX_GameObject *target_object = resolve_command_target(instruction);
    const std::string tree_name = ResolveStringExpression(
        instruction.string_expr_index, std::string(), context);
    if (target_object == nullptr || tree_name.empty()) {
      return;
    }

    KX_Scene *scene = m_gameObject ? m_gameObject->GetScene() : nullptr;
    if (scene == nullptr || scene->GetLogicNodeManager() == nullptr) {
      command_sequence++;
      MarkInstructionExecuted(event, instruction_id, context.tick_index);
      return;
    }

    switch (instruction.opcode) {
      case LN_OpCode::SetLogicTreeEnabled:
        command_buffer.AppendSetLogicTreeEnabled(
            target_object,
            tree_name,
            instruction.bool_value,
            command_sort_key(instruction_id, command_sequence),
            instruction.source_ref_index);
        break;
      case LN_OpCode::InstallLogicTree:
        command_buffer.AppendInstallLogicTree(
            target_object,
            tree_name,
            ResolveBoolExpression(instruction.bool_expr_index, instruction.bool_value, context),
            command_sort_key(instruction_id, command_sequence),
            instruction.source_ref_index);
        break;
      case LN_OpCode::RunLogicTreeOnce:
        if (LN_Manager *manager = scene->GetLogicNodeManager()) {
          manager->QueueRunLogicTreeOnce(target_object, tree_name);
        }
        break;
      default:
        break;
    }
    command_sequence++;
    MarkInstructionExecuted(event, instruction_id, context.tick_index);
  };

  auto execute_direct_send_event = [&](const LN_Instruction &instruction,
                                       const uint32_t instruction_id) {
    const LN_StringId subject_id = ResolveStringExpressionId(instruction.string_expr_index);
    std::string subject;
    if (!subject_id.IsValid()) {
      subject = ResolveStringExpression(instruction.string_expr_index, std::string(), context);
    }
    else if (m_program->GetString(subject_id).empty()) {
      return;
    }
    if (!subject_id.IsValid() && subject.empty()) {
      return;
    }

    LN_Value content;
    if (instruction.value_expr_index != LN_INVALID_INDEX) {
      content = ResolveValueExpression(instruction.value_expr_index, MakeNoneValue(), context);
    }
    KX_GameObject *messenger = m_gameObject;
    if (instruction.secondary_value_expr_index != LN_INVALID_INDEX) {
      const LN_Value messenger_val = ResolveValueExpression(
          instruction.secondary_value_expr_index, MakeNoneValue(), context);
      if (KX_GameObject *resolved_messenger = ResolveObjectValue(messenger_val)) {
        messenger = resolved_messenger;
      }
    }
    KX_GameObject *target = nullptr;
    if (instruction.int_expr_index != LN_INVALID_INDEX) {
      const LN_Value target_val = ResolveValueExpression(
          instruction.int_expr_index, MakeNoneValue(), context);
      target = ResolveObjectValue(target_val);
      if (target == nullptr) {
        return;
      }
    }
    if (subject_id.IsValid()) {
      command_buffer.AppendSendEvent(this,
                                     subject_id,
                                     content,
                                     messenger,
                                     target,
                                     command_sort_key(instruction_id, command_sequence),
                                     instruction.source_ref_index);
    }
    else {
      command_buffer.AppendSendEvent(subject,
                                     content,
                                     messenger,
                                     target,
                                     command_sort_key(instruction_id, command_sequence),
                                     instruction.source_ref_index,
                                     this);
    }
    command_sequence++;
    MarkInstructionExecuted(event, instruction_id, context.tick_index);
  };

  auto execute_direct_op = [&](const LN_ExecOp &op) {
    if (op.instruction_index >= instructions.size()) {
      return;
    }
    if (m_uiEventRoutesOnly && !op.ui_event_route) {
      return;
    }

    const LN_Instruction &instruction = m_program->GetInstructionPayload(
        event, instructions[op.instruction_index]);
    const uint32_t instruction_id = op.instruction_index;
    ScopedCommandQueryContext command_query_context(
        context, command_buffer, next_command_execution_key());
    profile_direct_exec_op(op);

    const std::vector<LN_Value> *guard_tree_properties = nullptr;
    if (op.guard_bool_expr_index != LN_INVALID_INDEX) {
      if (!ResolveBoolExpression(op.guard_bool_expr_index, false, context)) {
        return;
      }
      guard_tree_properties = DelayTreePropertySnapshotForGuard(op.guard_bool_expr_index,
                                                                context.tick_index);
    }

    const std::vector<LN_Value> *previous_tree_property_snapshot = m_activeTreePropertySnapshot;
    if (guard_tree_properties != nullptr) {
      m_activeTreePropertySnapshot = guard_tree_properties;
    }

    switch (op.kind) {
      case LN_ExecOpKind::Nop:
        MarkInstructionExecuted(event, instruction_id, context.tick_index);
        break;
      case LN_ExecOpKind::BranchRoute: {
        const bool condition = instruction.bool_expr_index == LN_INVALID_INDEX ?
                                   false :
                                   ResolveBoolExpression(
                                       instruction.bool_expr_index, false, context);
        if (condition == instruction.bool_value) {
          MarkInstructionExecuted(event, instruction_id, context.tick_index);
        }
        break;
      }
      case LN_ExecOpKind::TimeFlowControl:
        execute_time_flow_control_instruction(instruction, instruction_id);
        break;
      case LN_ExecOpKind::VectorCommand:
        execute_direct_vector_command(instruction, instruction_id);
        break;
      case LN_ExecOpKind::PhysicsCommand:
        execute_direct_physics_command(instruction, instruction_id);
        break;
      case LN_ExecOpKind::ObjectStateCommand:
        execute_direct_object_state_command(instruction, instruction_id);
        break;
      case LN_ExecOpKind::ObjectColorCommand:
        execute_direct_object_color_command(instruction, instruction_id);
        break;
      case LN_ExecOpKind::CameraCommand:
        execute_direct_camera_command(instruction, instruction_id);
        break;
      case LN_ExecOpKind::LightCommand:
        execute_direct_light_command(instruction, instruction_id);
        break;
      case LN_ExecOpKind::WindowCommand:
        execute_direct_window_command(instruction, instruction_id);
        break;
      case LN_ExecOpKind::InputMotionCommand:
        execute_direct_input_motion_command(instruction, instruction_id);
        break;
      case LN_ExecOpKind::AudioCommand:
        execute_direct_audio_command(instruction, instruction_id);
        break;
      case LN_ExecOpKind::ActionCommand:
        execute_direct_action_command(instruction, instruction_id);
        break;
      case LN_ExecOpKind::BoneCommand:
        execute_direct_bone_command(instruction, instruction_id);
        break;
      case LN_ExecOpKind::MaterialCommand:
        execute_direct_material_command(instruction, instruction_id);
        break;
      case LN_ExecOpKind::GlobalCommand:
        execute_direct_global_command(instruction, instruction_id);
        break;
      case LN_ExecOpKind::FileCommand:
        execute_direct_file_command(instruction, instruction_id);
        break;
      case LN_ExecOpKind::SceneCommand:
        execute_direct_scene_command(instruction, instruction_id);
        break;
      case LN_ExecOpKind::LifecycleCommand:
        execute_direct_lifecycle_command(instruction, instruction_id);
        break;
      case LN_ExecOpKind::ObjectReferenceCommand:
        execute_direct_object_reference_command(instruction, instruction_id);
        break;
      case LN_ExecOpKind::GamePropertyCommand:
        execute_direct_game_property_command(instruction, instruction_id);
        break;
      case LN_ExecOpKind::TreePropertyCommand:
        execute_direct_tree_property_command(instruction, instruction_id);
        break;
      case LN_ExecOpKind::TreeControlCommand:
        execute_direct_tree_control_command(instruction, instruction_id);
        break;
      case LN_ExecOpKind::SendEvent:
        execute_direct_send_event(instruction, instruction_id);
        break;
      case LN_ExecOpKind::GlobalPropertyCommand:
        execute_global_property_instruction(instruction, instruction_id);
        break;
      case LN_ExecOpKind::VariableCommand:
        execute_variable_instruction(instruction, instruction_id);
        break;
      case LN_ExecOpKind::NavigationCommand:
        execute_navigation_instruction(instruction, instruction_id);
        break;
      case LN_ExecOpKind::SceneCollectionCommand:
        execute_scene_collection_instruction(instruction, instruction_id);
        break;
      case LN_ExecOpKind::DebugDrawCommand:
        execute_debug_draw_instruction(instruction, instruction_id);
        break;
      case LN_ExecOpKind::PhysicsConstraintCommand:
        execute_physics_constraint_instruction(instruction, instruction_id);
        break;
    }

    m_activeTreePropertySnapshot = previous_tree_property_snapshot;
  };

  const LN_ExecBlockProgram &ir = m_program->GetExecBlockProgram(event);
  if (!ir.valid) {
    RecordRuntimeSystemFallback(
        LN_RuntimeFallbackReason::UnsupportedOpcode,
        "invalid_exec_block_ir",
        "exec_block_ir_invalid",
        "Fix the Logic Nodes compiler or IR validation error; runtime no longer falls back to the "
        "legacy instruction switch.");
    return;
  }

  /* Runs the blocks in the execution scope `scope_serial`. With `ui_event_state`, only the ops of
   * that On UI Event's route: its repeat for a further event of this frame. */
  std::vector<uint32_t> repeated_loop_frames;
  const auto run_blocks = [&](const uint64_t scope_serial, const uint32_t ui_event_state) {
    const auto selected = [&](const LN_ExecOp &op) {
      return ui_event_state == LN_INVALID_INDEX || op.ui_event_state_index == ui_event_state;
    };
    for (const LN_ExecBlock &block : ir.blocks) {
      if (ui_event_state != LN_INVALID_INDEX &&
          std::none_of(ir.ops.begin() + block.first_op,
                       ir.ops.begin() + block.first_op + block.op_count,
                       selected))
      {
        continue;
      }
      /* A loop body that is not entirely a UI event route must not advance while paused. */
      if (m_uiEventRoutesOnly && block.loop_frame_index != LN_INVALID_INDEX &&
          !block.ui_event_route)
      {
        continue;
      }
      if (m_activeProfileCounters != nullptr) {
        m_activeProfileCounters->exec_block_count++;
      }
      if (block.completed_loop_frame_index != LN_INVALID_INDEX) {
        if (block.completed_loop_frame_index >= m_loopStates.size() ||
            m_loopStates[block.completed_loop_frame_index].completed_scope_serial != scope_serial)
        {
          continue;
        }
      }
      if (block.loop_frame_index != LN_INVALID_INDEX) {
        /* A repeat starts a Loop of the route again, once per repeat (a body copy per incoming
         * path shares the frame). One that still runs over more ticks keeps going instead. */
        if (ui_event_state != LN_INVALID_INDEX &&
            std::find(repeated_loop_frames.begin(),
                      repeated_loop_frames.end(),
                      block.loop_frame_index) == repeated_loop_frames.end())
        {
          repeated_loop_frames.push_back(block.loop_frame_index);
          LoopRuntimeState &repeated = LoopState(block.loop_frame_index);
          if (!repeated.active) {
            repeated.last_batch_tick_index = UINT64_MAX;
          }
        }
        BeginLoopFrame(block.loop_frame_index, context);
        LoopRuntimeState &loop_state = LoopState(block.loop_frame_index);
        if ((loop_state.triggered || loop_state.active) &&
            loop_state.last_batch_tick_index != context.tick_index)
        {
          loop_state.last_batch_tick_index = context.tick_index;
          if (loop_state.batch_end_iteration <= loop_state.iteration_index) {
            const size_t base_batch_size = loop_state.iteration_count / loop_state.batch_count;
            const size_t larger_batch_count = loop_state.iteration_count % loop_state.batch_count;
            const size_t batch_size = base_batch_size +
                                      (loop_state.batch_index < larger_batch_count ? 1 : 0);
            loop_state.batch_end_iteration = std::min(loop_state.iteration_count,
                                                      loop_state.iteration_index + batch_size);
          }
          const size_t remaining_in_batch = loop_state.batch_end_iteration -
                                            loop_state.iteration_index;
          const size_t iterations_to_run = std::min(remaining_in_batch,
                                                    m_remainingLoopIterationBudget);
          size_t batch_iterations = 0;
          while (batch_iterations < iterations_to_run && AdvanceLoopFrame(block.loop_frame_index))
          {
            batch_iterations++;
            const uint32_t previous_loop_body_frame = m_activeLoopBodyFrame;
            m_activeLoopBodyFrame = block.loop_frame_index;
            context.execution_scope_serial = next_execution_scope_serial();
            m_activeExecutionScopeSerial = context.execution_scope_serial;
            for (uint32_t op_index = block.first_op; op_index < block.first_op + block.op_count;
                 op_index++)
            {
              if (selected(ir.ops[op_index])) {
                execute_direct_op(ir.ops[op_index]);
              }
              if (loop_state.break_requested) {
                break;
              }
            }
            m_activeLoopBodyFrame = previous_loop_body_frame;
            if (loop_state.break_requested) {
              loop_state.active = false;
              break;
            }
          }
          m_remainingLoopIterationBudget -= batch_iterations;
          if (loop_state.break_requested ||
              loop_state.iteration_index >= loop_state.iteration_count)
          {
            loop_state.active = false;
            loop_state.completed_scope_serial = scope_serial;
          }
          else if (loop_state.iteration_index >= loop_state.batch_end_iteration) {
            loop_state.batch_index++;
            loop_state.batch_end_iteration = 0;
          }
        }
        context.execution_scope_serial = scope_serial;
        m_activeExecutionScopeSerial = scope_serial;
      }
      else {
        context.execution_scope_serial = scope_serial;
        m_activeExecutionScopeSerial = scope_serial;
        for (uint32_t op_index = block.first_op; op_index < block.first_op + block.op_count;
             op_index++)
        {
          if (selected(ir.ops[op_index])) {
            execute_direct_op(ir.ops[op_index]);
          }
        }
      }
    }
  };
  run_blocks(event_scope_serial, LN_INVALID_INDEX);

  /* On UI Event: each further event that matches a node this frame runs the node's route again,
   * in its own execution scope, with the node's outputs telling about that event. */
  /* (Other per-tick query caches are not run again: a query that takes an event's output as input,
   * such as a Projectile Ray from a drop's Point, gives the first event's answer. Raycasts are
   * evaluated per command and are not affected.) */
  constexpr uint32_t max_ui_event_repeats = uint32_t(LN_UIService::kMaxEventsPerNodeFrame);
  for (const uint32_t state_index : ir.ui_event_states) {
    /* Looked up again after each run: running can grow the state table. */
    const QueryExpressionRuntimeState &first = QueryExpressionState(state_index);
    if (first.cached_tick != context.tick_index || first.cached_result.ui_event_count < 2) {
      continue;
    }
    const uint32_t count = std::min(first.cached_result.ui_event_count, max_ui_event_repeats);
    for (uint32_t cursor = 1; cursor < count; ++cursor) {
      QueryExpressionRuntimeState &state = QueryExpressionState(state_index);
      state.ui_event_cursor = cursor;
      state.cached_tick = UINT64_MAX;
      m_registerExpressionState.cached_tick = UINT64_MAX;
      repeated_loop_frames.clear();
      const uint64_t repeat_scope_serial = next_execution_scope_serial();
      context.execution_scope_serial = repeat_scope_serial;
      m_activeExecutionScopeSerial = repeat_scope_serial;
      run_blocks(repeat_scope_serial, state_index);
    }
    /* The next frame starts at its first event again. Until then, what reads the node's outputs
     * keeps the topmost event (the cached result of the last run). */
    QueryExpressionState(state_index).ui_event_cursor = 0;
  }
  context.execution_scope_serial = event_scope_serial;
  m_activeExecutionScopeSerial = event_scope_serial;
}

LN_Value LN_RuntimeTree::ResolveValueExpression(const uint32_t expression_index,
                                                const LN_Value &fallback,
                                                const LN_TickContext &context)
{
  if (expression_index == LN_INVALID_INDEX || m_program == nullptr) {
    return fallback;
  }

  const std::vector<LN_ValueExpression> &expressions = m_program->GetValueExpressions();
  if (expression_index >= expressions.size()) {
    return fallback;
  }

  const LN_ValueExpression &expression = expressions[expression_index];
  const LN_RuntimeExpressionSemantics semantics = LN_GetRuntimeExpressionSemantics(
      expression.kind);
  ProfileExpressionEvaluation(semantics.fallback_requirements);
  RecordMissingSnapshotChannelsForExpression(semantics);
  LN_Value register_value;
  if (TryResolveRegisterValueExpression(expression_index, context, register_value)) {
    return register_value;
  }
  switch (expression.kind) {
    case LN_ValueExpressionKind::Constant:
      if (expression.value.type == LN_ValueType::ObjectRef && expression.value.exists) {
        LN_Value value = expression.value;
        if (expression.value.runtime_ref.IsValid()) {
          value.exists = ResolveObjectRef(expression.value.runtime_ref) != nullptr;
          if (!value.exists) {
            value.runtime_ref = LN_RuntimeRef();
          }
        }
        else if (KX_GameObject *game_object = ResolveObjectValue(expression.value)) {
          value.runtime_ref = MakeObjectRef(game_object, game_object->GetName());
          value.reference_name = game_object->GetName();
          value.exists = true;
        }
        else {
          value.runtime_ref = LN_RuntimeRef();
          value.exists = false;
        }
        return value;
      }
      if (expression.value.type == LN_ValueType::CollectionRef && expression.value.exists) {
        LN_Value value = expression.value;
        if (blender::Collection *collection = ResolveCollectionByValue(this, value)) {
          value.runtime_ref = MakeCollectionRef(collection, collection->id.name + 2);
          value.reference_name = collection->id.name + 2;
          value.exists = true;
        }
        else {
          value.runtime_ref = LN_RuntimeRef();
          value.exists = false;
        }
        return value;
      }
      return expression.value;
    case LN_ValueExpressionKind::SnapshotGameProperty:
      if (const LN_Value *value = m_snapshot.GetGamePropertyValue(expression.property_ref_index)) {
        return *value;
      }
      return fallback;
    case LN_ValueExpressionKind::RuntimeTreeProperty:
      if (const LN_Value *value = GetTreePropertyValue(expression.property_ref_index)) {
        return *value;
      }
      return fallback;
    case LN_ValueExpressionKind::Select:
      return ResolveBoolExpression(expression.bool_expr_index, false, context) ?
                 ResolveValueExpression(expression.input0, fallback, context) :
                 ResolveValueExpression(expression.input1, fallback, context);
    case LN_ValueExpressionKind::Mix:
      return ln::value_interpolation::Mix(
          ResolveValueExpression(expression.input0, LN_Value(), context),
          ResolveValueExpression(expression.input1, LN_Value(), context),
          ResolveFloatExpression(expression.input2, 0.0f, context));
    case LN_ValueExpressionKind::FromBool: {
      LN_Value value;
      value.type = LN_ValueType::Bool;
      value.exists = true;
      value.bool_value = ResolveBoolExpression(expression.input0, false, context);
      return value;
    }
    case LN_ValueExpressionKind::FromInt: {
      LN_Value value;
      value.type = LN_ValueType::Int;
      value.exists = true;
      value.int_value = ResolveIntExpression(expression.input0, 0, context);
      return value;
    }
    case LN_ValueExpressionKind::FromFloat: {
      LN_Value value;
      value.type = LN_ValueType::Float;
      value.exists = true;
      value.float_value = ResolveFloatExpression(expression.input0, 0.0f, context);
      return value;
    }
    case LN_ValueExpressionKind::FromString: {
      LN_Value value;
      value.type = LN_ValueType::String;
      value.exists = true;
      value.string_value = ResolveStringExpression(expression.input0, "", context);
      return value;
    }
    case LN_ValueExpressionKind::FromVector: {
      LN_Value value;
      value.type = LN_ValueType::Vector;
      value.exists = true;
      value.vector_value = ResolveVectorExpression(
          expression.input0, MT_Vector3(0.0f, 0.0f, 0.0f), context);
      return value;
    }
    case LN_ValueExpressionKind::FromColor: {
      LN_Value value;
      value.type = LN_ValueType::Color;
      value.exists = true;
      value.color_value = ResolveColorExpression(
          expression.input0, MT_Vector4(0.0f, 0.0f, 0.0f, 1.0f), context);
      return value;
    }
    case LN_ValueExpressionKind::FromRotation: {
      LN_Value value;
      value.type = LN_ValueType::Rotation;
      value.exists = true;
      const LN_Rotation rotation = ResolveRotationExpression(
          expression.input0, LN_Rotation::Identity(), context);
      value.SetRotation(
          rotation, ResolveVectorExpression(expression.input0, rotation.ToEulerXYZ(), context));
      return value;
    }
    case LN_ValueExpressionKind::CombineVector4: {
      if (expression.input_indices.size() < 4) {
        return fallback;
      }
      LN_Value value;
      value.type = LN_ValueType::Vector4;
      value.exists = true;
      value.vector4_value = MT_Vector4(
          ResolveFloatExpression(
              expression.input_indices[0], expression.value.vector4_value.x(), context),
          ResolveFloatExpression(
              expression.input_indices[1], expression.value.vector4_value.y(), context),
          ResolveFloatExpression(
              expression.input_indices[2], expression.value.vector4_value.z(), context),
          ResolveFloatExpression(
              expression.input_indices[3], expression.value.vector4_value.w(), context));
      return value;
    }
    case LN_ValueExpressionKind::ResizeVectorValue: {
      const LN_Value input = ResolveValueExpression(expression.input0, MakeNoneValue(), context);
      MT_Vector4 vector;
      if (!ValueAsVector4(input, vector)) {
        return fallback;
      }
      const int32_t target_size = std::clamp(int32_t(expression.property_ref_index), 2, 4);
      LN_Value value;
      value.exists = true;
      if (target_size == 4) {
        value.type = LN_ValueType::Vector4;
        value.vector4_value = vector;
      }
      else {
        value.type = LN_ValueType::Vector;
        value.vector_value = MT_Vector3(
            vector.x(), vector.y(), target_size >= 3 ? vector.z() : 0.0f);
      }
      return value;
    }
    case LN_ValueExpressionKind::EulerToMatrix: {
      const MT_Vector3 euler = ResolveVectorExpression(
          expression.input0, MT_Vector3(0.0f, 0.0f, 0.0f), context);
      LN_Value value;
      value.type = LN_ValueType::Matrix;
      value.exists = true;
      value.matrix_value = MatrixFromEuler(euler, expression.property_ref_index);
      return value;
    }
    case LN_ValueExpressionKind::MatrixToEuler: {
      const LN_Value input = ResolveValueExpression(expression.input0, MakeNoneValue(), context);
      MT_Matrix3x3 matrix;
      if (!ValueAsMatrix(input, matrix)) {
        return fallback;
      }
      const MT_Vector3 euler = EulerFromMatrix(matrix, expression.property_ref_index);
      LN_Value value;
      value.type = expression.value.type == LN_ValueType::Rotation ? LN_ValueType::Rotation :
                                                                     LN_ValueType::Vector;
      value.exists = true;
      if (value.type == LN_ValueType::Rotation) {
        value.SetRotation(LN_Rotation::FromMatrix(matrix), euler);
      }
      else {
        value.vector_value = euler;
      }
      return value;
    }
    case LN_ValueExpressionKind::PlayerViewCamera: {
      KX_Scene *scene = m_gameObject ? m_gameObject->GetScene() : nullptr;
      KX_Camera *camera = scene ? scene->GetPlayerViews().GetPlayerCamera(
                                      ResolveIntExpression(expression.input0, 0, context),
                                      scene->GetActiveCamera()) :
                                  nullptr;
      if (camera == nullptr) {
        return MakeNoneValue();
      }
      LN_Value value;
      value.type = LN_ValueType::ObjectRef;
      value.exists = true;
      value.reference_name = camera->GetName();
      value.runtime_ref = MakeObjectRef(camera, camera->GetName());
      return value;
    }
    case LN_ValueExpressionKind::MenuPlayerOwner: {
      KX_GameObject *menu_owner = m_logicManager != nullptr && m_gameObject != nullptr ?
                                      m_logicManager->GetUIService().MenuOwnerOfObject(
                                          m_gameObject) :
                                      nullptr;
      if (menu_owner == nullptr) {
        return MakeNoneValue();
      }
      LN_Value value;
      value.type = LN_ValueType::ObjectRef;
      value.exists = true;
      value.reference_name = menu_owner->GetName();
      value.runtime_ref = MakeObjectRef(menu_owner, menu_owner->GetName());
      return value;
    }
    case LN_ValueExpressionKind::ActiveCamera: {
      KX_Camera *camera = GetActiveCamera();
      if (camera == nullptr) {
        return MakeNoneValue();
      }

      LN_Value value;
      value.type = LN_ValueType::ObjectRef;
      value.exists = true;
      value.reference_name = camera->GetName();
      value.runtime_ref = MakeObjectRef(camera, camera->GetName());
      return value;
    }
    case LN_ValueExpressionKind::OwnerObject: {
      if (m_gameObject == nullptr) {
        return MakeNoneValue();
      }

      LN_Value value;
      value.type = LN_ValueType::ObjectRef;
      value.exists = true;
      value.reference_name = m_gameObject->GetName();
      value.runtime_ref = MakeObjectRef(m_gameObject, m_gameObject->GetName());
      return value;
    }
    case LN_ValueExpressionKind::ObjectByName: {
      const std::string name = ResolveStringExpression(expression.input0, std::string(), context);
      if (name.empty()) {
        return MakeNoneValue();
      }

      KX_GameObject *object = FindSceneObjectByName(name);
      if (object == nullptr) {
        return MakeNoneValue();
      }

      LN_Value value;
      value.type = LN_ValueType::ObjectRef;
      value.exists = true;
      value.reference_name = object->GetName();
      value.runtime_ref = MakeObjectRef(object, object->GetName());
      return value;
    }
    case LN_ValueExpressionKind::ObjectParent: {
      const LN_Value child_value = ResolveValueExpression(
          expression.input0, MakeNoneValue(), context);
      KX_GameObject *child_object = ResolveObjectValue(child_value);
      KX_GameObject *parent_object = child_object ? child_object->GetParent() : nullptr;
      if (parent_object == nullptr) {
        return MakeNoneValue();
      }

      LN_Value value;
      value.type = LN_ValueType::ObjectRef;
      value.exists = true;
      value.reference_name = parent_object->GetName();
      value.runtime_ref = MakeObjectRef(parent_object, parent_object->GetName());
      return value;
    }
    case LN_ValueExpressionKind::ObjectChild: {
      const LN_Value parent_value = ResolveValueExpression(
          expression.input0, MakeNoneValue(), context);
      KX_GameObject *parent_object = ResolveObjectValue(parent_value);
      if (parent_object == nullptr) {
        return MakeNoneValue();
      }

      const int32_t child_index = ResolveIntExpression(expression.input1, 0, context);
      if (child_index < 0) {
        return MakeNoneValue();
      }

      const std::vector<KX_GameObject *> children = parent_object->GetChildren();
      if (size_t(child_index) >= children.size() || children[size_t(child_index)] == nullptr) {
        return MakeNoneValue();
      }

      KX_GameObject *child_object = children[size_t(child_index)];
      LN_Value value;
      value.type = LN_ValueType::ObjectRef;
      value.exists = true;
      value.reference_name = child_object->GetName();
      value.runtime_ref = MakeObjectRef(child_object, child_object->GetName());
      return value;
    }
    case LN_ValueExpressionKind::ObjectChildByName: {
      const LN_Value parent_value = ResolveValueExpression(
          expression.input0, MakeNoneValue(), context);
      KX_GameObject *parent_object = ResolveObjectValue(parent_value);
      KX_GameObject *child_hint = ResolveObjectValue(
          ResolveValueExpression(expression.input1, MakeNoneValue(), context));
      if (parent_object == nullptr || child_hint == nullptr) {
        return MakeNoneValue();
      }

      const std::string child_name = child_hint->GetName();
      for (KX_GameObject *child_object : parent_object->GetChildren()) {
        if (child_object != nullptr && child_object->GetName() == child_name) {
          LN_Value value;
          value.type = LN_ValueType::ObjectRef;
          value.exists = true;
          value.reference_name = child_object->GetName();
          value.runtime_ref = MakeObjectRef(child_object, child_object->GetName());
          return value;
        }
      }
      return MakeNoneValue();
    }
    case LN_ValueExpressionKind::PhysicsQueryObject: {
      const LN_QueryResult &query = ResolveQueryExpression(expression.input0, context);
      if (!query.physics_query.hit) {
        return MakeNoneValue();
      }

      KX_GameObject *game_object = ResolveObjectRef(query.physics_query.object_ref);
      if (game_object == nullptr) {
        return MakeNoneValue();
      }

      LN_Value value;
      value.type = LN_ValueType::ObjectRef;
      value.exists = true;
      value.runtime_ref = query.physics_query.object_ref;
      value.reference_name = game_object->GetName();
      return value;
    }
    case LN_ValueExpressionKind::UIElementEventObject:
    case LN_ValueExpressionKind::UIElementEventTarget: {
      const LN_QueryResult &query = ResolveQueryExpression(expression.input0, context);
      const LN_RuntimeRef &object_ref = expression.kind ==
                                                LN_ValueExpressionKind::UIElementEventObject ?
                                            query.ui_source_object :
                                            query.ui_target_object;
      KX_GameObject *game_object = ResolveObjectRef(object_ref);
      if (game_object == nullptr) {
        return MakeNoneValue();
      }
      LN_Value value;
      value.type = LN_ValueType::ObjectRef;
      value.exists = true;
      value.runtime_ref = object_ref;
      value.reference_name = game_object->GetName();
      return value;
    }
    case LN_ValueExpressionKind::UIElementEventItem:
    case LN_ValueExpressionKind::UIElementEventTargetItem: {
      const LN_QueryResult &query = ResolveQueryExpression(expression.input0, context);
      const std::shared_ptr<LN_Value> &item =
          expression.kind == LN_ValueExpressionKind::UIElementEventItem ? query.ui_item :
                                                                          query.ui_target_item;
      return item != nullptr ? *item : MakeNoneValue();
    }
    case LN_ValueExpressionKind::UIFocusObject: {
      if (m_logicManager == nullptr) {
        return MakeNoneValue();
      }
      const int32_t user = ResolveIntExpression(expression.input0, 0, context);
      const std::string input_context = ResolveStringExpression(
          expression.input1, std::string(), context);
      KX_GameObject *game_object = m_logicManager->GetUIService().GetFocus(user, input_context);
      if (game_object == nullptr) {
        return MakeNoneValue();
      }
      LN_Value value;
      value.type = LN_ValueType::ObjectRef;
      value.exists = true;
      value.runtime_ref = MakeObjectRef(game_object, game_object->GetName());
      value.reference_name = game_object->GetName();
      return value;
    }
    case LN_ValueExpressionKind::PhysicsQueryObjects:
    case LN_ValueExpressionKind::PhysicsQueryPoints:
    case LN_ValueExpressionKind::PhysicsQueryNormals:
    case LN_ValueExpressionKind::PhysicsQueryDistances:
    case LN_ValueExpressionKind::PhysicsQueryCastPositions:
    case LN_ValueExpressionKind::PhysicsQueryFractions:
    case LN_ValueExpressionKind::PhysicsQueryPenetrationDepths:
    case LN_ValueExpressionKind::PhysicsQueryStartedOverlappingList:
    case LN_ValueExpressionKind::PhysicsQueryFaceIndices:
    case LN_ValueExpressionKind::PhysicsQueryHasUVs:
    case LN_ValueExpressionKind::PhysicsQueryUVs:
    case LN_ValueExpressionKind::PhysicsQueryHits:
    case LN_ValueExpressionKind::PhysicsQueryBlockedList:
    case LN_ValueExpressionKind::PhysicsQueryRayOrigins:
    case LN_ValueExpressionKind::PhysicsQueryRayDirections:
    case LN_ValueExpressionKind::PhysicsQueryEndPoints: {
      const LN_QueryResult &query = ResolveQueryExpression(expression.input0, context);
      LN_Value value;
      value.type = LN_ValueType::List;
      value.exists = true;
      value.list_value.reserve(query.physics_query_results.size());
      const bool hit_detail_output =
          expression.kind == LN_ValueExpressionKind::PhysicsQueryObjects ||
          expression.kind == LN_ValueExpressionKind::PhysicsQueryPoints ||
          expression.kind == LN_ValueExpressionKind::PhysicsQueryNormals ||
          expression.kind == LN_ValueExpressionKind::PhysicsQueryDistances ||
          expression.kind == LN_ValueExpressionKind::PhysicsQueryCastPositions ||
          expression.kind == LN_ValueExpressionKind::PhysicsQueryFractions ||
          expression.kind == LN_ValueExpressionKind::PhysicsQueryPenetrationDepths ||
          expression.kind == LN_ValueExpressionKind::PhysicsQueryStartedOverlappingList ||
          expression.kind == LN_ValueExpressionKind::PhysicsQueryFaceIndices ||
          expression.kind == LN_ValueExpressionKind::PhysicsQueryHasUVs ||
          expression.kind == LN_ValueExpressionKind::PhysicsQueryUVs;
      for (const LN_PhysicsQueryResult &hit : query.physics_query_results) {
        /* Raycast from Mesh retains one internal slot per generated ray so its Hits, Blocked and
         * ray-geometry outputs can diagnose misses. Collision-detail outputs follow the regular
         * physics-query contract, however: they contain matching hits only. In particular, never
         * expose the placeholder end position of a miss as a hit point. */
        if (hit_detail_output && !hit.hit) {
          continue;
        }
        switch (expression.kind) {
          case LN_ValueExpressionKind::PhysicsQueryObjects: {
            LN_Value object_value = MakeNoneValue();
            object_value.type = LN_ValueType::ObjectRef;
            KX_GameObject *game_object = hit.hit ? ResolveObjectRef(hit.object_ref) : nullptr;
            object_value.exists = game_object != nullptr;
            if (game_object) {
              object_value.runtime_ref = hit.object_ref;
              object_value.reference_name = game_object->GetName();
            }
            value.list_value.push_back(object_value);
            break;
          }
          case LN_ValueExpressionKind::PhysicsQueryPoints:
            value.list_value.push_back(MakeVectorValue(hit.hit_position));
            break;
          case LN_ValueExpressionKind::PhysicsQueryNormals:
            value.list_value.push_back(MakeVectorValue(hit.hit_normal));
            break;
          case LN_ValueExpressionKind::PhysicsQueryDistances:
            value.list_value.push_back(MakeFloatValue(hit.hit_distance));
            break;
          case LN_ValueExpressionKind::PhysicsQueryCastPositions:
            value.list_value.push_back(MakeVectorValue(hit.cast_position));
            break;
          case LN_ValueExpressionKind::PhysicsQueryFractions:
            value.list_value.push_back(MakeFloatValue(hit.hit_fraction));
            break;
          case LN_ValueExpressionKind::PhysicsQueryPenetrationDepths:
            value.list_value.push_back(MakeFloatValue(hit.penetration_depth));
            break;
          case LN_ValueExpressionKind::PhysicsQueryStartedOverlappingList:
            value.list_value.push_back(MakeBoolValue(hit.started_overlapping));
            break;
          case LN_ValueExpressionKind::PhysicsQueryFaceIndices:
            value.list_value.push_back(MakeIntValue(hit.polygon_index));
            break;
          case LN_ValueExpressionKind::PhysicsQueryHasUVs:
            value.list_value.push_back(MakeBoolValue(hit.has_uv));
            break;
          case LN_ValueExpressionKind::PhysicsQueryUVs:
            value.list_value.push_back(
                MakeVectorValue(MT_Vector3(hit.hit_uv.x(), hit.hit_uv.y(), 0.0f)));
            break;
          case LN_ValueExpressionKind::PhysicsQueryHits:
            value.list_value.push_back(MakeBoolValue(hit.hit));
            break;
          case LN_ValueExpressionKind::PhysicsQueryBlockedList:
            value.list_value.push_back(MakeBoolValue(hit.blocked));
            break;
          case LN_ValueExpressionKind::PhysicsQueryRayOrigins:
            value.list_value.push_back(MakeVectorValue(hit.ray_origin));
            break;
          case LN_ValueExpressionKind::PhysicsQueryRayDirections:
            value.list_value.push_back(MakeVectorValue(hit.ray_direction));
            break;
          case LN_ValueExpressionKind::PhysicsQueryEndPoints:
            value.list_value.push_back(MakeVectorValue(hit.end_position));
            break;
          default:
            break;
        }
      }
      return value;
    }
    case LN_ValueExpressionKind::ProjectileParabola: {
      const LN_QueryResult &query = ResolveQueryExpression(expression.input0, context);
      LN_Value value;
      value.type = LN_ValueType::List;
      value.exists = true;
      value.list_value.reserve(query.physics_query.parabola_points.size());
      for (const MT_Vector3 &point : query.physics_query.parabola_points) {
        LN_Value point_value;
        point_value.type = LN_ValueType::Vector;
        point_value.exists = true;
        point_value.vector_value = point;
        value.list_value.push_back(point_value);
      }
      return value;
    }
    case LN_ValueExpressionKind::ValueChangedOld:
    case LN_ValueExpressionKind::ValueChangedNew: {
      if (expression.input0 == LN_INVALID_INDEX) {
        return fallback;
      }
      ResolveBoolExpression(expression.input0, false, context);
      const BoolExpressionRuntimeState &state = BoolExpressionState(expression.input0);
      return expression.kind == LN_ValueExpressionKind::ValueChangedOld ? state.stored_old_value :
                                                                          state.stored_new_value;
    }
    case LN_ValueExpressionKind::EventContent: {
      const LN_StringId subject_string_id = ResolveStringExpressionId(expression.input0);
      const LN_EventSubjectId subject_id = GetEventSubjectId(subject_string_id);
      const std::string subject = ResolveStringExpression(
          expression.input0, std::string(), context);
      if (subject.empty()) {
        return fallback;
      }
      KX_GameObject *filter_target = m_gameObject;
      if (expression.value.exists && expression.value.bool_value) {
        if (expression.input1 == LN_INVALID_INDEX) {
          return fallback;
        }
        const LN_Value target_val = ResolveValueExpression(
            expression.input1, MakeNoneValue(), context);
        KX_GameObject *explicit_target = ResolveObjectValue(target_val);
        if (explicit_target == nullptr) {
          return fallback;
        }
        filter_target = explicit_target;
      }
      if (const LN_EventEntry *event = FindFirstActiveEvent(subject_id, subject, filter_target)) {
        LN_Value content = event->content;
        LocalizeValue(content);
        return content;
      }
      return fallback;
    }
    case LN_ValueExpressionKind::EventMessenger: {
      const LN_StringId subject_string_id = ResolveStringExpressionId(expression.input0);
      const LN_EventSubjectId subject_id = GetEventSubjectId(subject_string_id);
      const std::string subject = ResolveStringExpression(
          expression.input0, std::string(), context);
      if (subject.empty()) {
        return fallback;
      }
      KX_GameObject *filter_target = m_gameObject;
      if (expression.value.exists && expression.value.bool_value) {
        if (expression.input1 == LN_INVALID_INDEX) {
          return fallback;
        }
        const LN_Value target_val = ResolveValueExpression(
            expression.input1, MakeNoneValue(), context);
        KX_GameObject *explicit_target = ResolveObjectValue(target_val);
        if (explicit_target == nullptr) {
          return fallback;
        }
        filter_target = explicit_target;
      }
      if (const LN_EventEntry *event = FindFirstActiveEvent(subject_id, subject, filter_target)) {
        /* The messenger may have been removed since the event was sent. */
        if (KX_GameObject *messenger = m_logicManager->ResolveObjectHandle(
                event->messenger_handle))
        {
          LN_Value result;
          result.type = LN_ValueType::ObjectRef;
          result.exists = true;
          result.reference_name = messenger->GetName();
          result.runtime_ref = MakeObjectRef(messenger, result.reference_name);
          return result;
        }
      }
      return fallback;
    }
    case LN_ValueExpressionKind::ObjectAttribute: {
      KX_GameObject *target_object = nullptr;
      if (expression.input0 != LN_INVALID_INDEX) {
        target_object = ResolveObjectValue(
            ResolveValueExpression(expression.input0, MakeNoneValue(), context));
      }
      else if (m_gameObject != nullptr) {
        target_object = m_gameObject;
      }
      if (target_object == nullptr) {
        return fallback;
      }
      const std::string attr = ResolveStringExpression(expression.input1, std::string(), context);
      if (attr.empty()) {
        return fallback;
      }
      if (attr == "worldPosition" || attr == "position") {
        const MT_Vector3 pos = target_object->NodeGetWorldPosition();
        LN_Value value;
        value.type = LN_ValueType::Vector;
        value.vector_value = pos;
        value.exists = true;
        return value;
      }
      if (attr == "localPosition") {
        const MT_Vector3 pos = target_object->NodeGetLocalPosition();
        LN_Value value;
        value.type = LN_ValueType::Vector;
        value.vector_value = pos;
        value.exists = true;
        return value;
      }
      if (attr == "worldOrientation") {
        const MT_Matrix3x3 rot = target_object->NodeGetWorldOrientation();
        LN_Value value;
        value.type = LN_ValueType::Rotation;
        value.SetRotation(LN_Rotation::FromMatrix(rot));
        value.exists = true;
        return value;
      }
      if (attr == "localOrientation") {
        const MT_Matrix3x3 rot = target_object->NodeGetLocalOrientation();
        LN_Value value;
        value.type = LN_ValueType::Rotation;
        value.SetRotation(LN_Rotation::FromMatrix(rot));
        value.exists = true;
        return value;
      }
      if (attr == "worldScale") {
        const MT_Vector3 scale = target_object->NodeGetWorldScaling();
        LN_Value value;
        value.type = LN_ValueType::Vector;
        value.vector_value = scale;
        value.exists = true;
        return value;
      }
      if (attr == "localScale") {
        const MT_Vector3 scale = target_object->NodeGetLocalScaling();
        LN_Value value;
        value.type = LN_ValueType::Vector;
        value.vector_value = scale;
        value.exists = true;
        return value;
      }
      if (attr == "worldLinearVelocity") {
        LN_Value value;
        value.type = LN_ValueType::Vector;
        value.vector_value = target_object->GetLinearVelocity(false);
        value.exists = true;
        return value;
      }
      if (attr == "localLinearVelocity") {
        LN_Value value;
        value.type = LN_ValueType::Vector;
        value.vector_value = target_object->GetLinearVelocity(true);
        value.exists = true;
        return value;
      }
      if (attr == "worldAngularVelocity") {
        LN_Value value;
        value.type = LN_ValueType::Vector;
        value.vector_value = target_object->GetAngularVelocity(false);
        value.exists = true;
        return value;
      }
      if (attr == "localAngularVelocity") {
        LN_Value value;
        value.type = LN_ValueType::Vector;
        value.vector_value = target_object->GetAngularVelocity(true);
        value.exists = true;
        return value;
      }
      if (attr == "color") {
        const MT_Vector4 color = target_object->GetObjectColor();
        LN_Value value;
        value.type = LN_ValueType::Color;
        value.color_value = color;
        value.exists = true;
        return value;
      }
      if (attr == "visible") {
        LN_Value value;
        value.type = LN_ValueType::Bool;
        value.bool_value = target_object->GetVisible();
        value.exists = true;
        return value;
      }
      if (attr == "name") {
        LN_Value value;
        value.type = LN_ValueType::String;
        value.string_value = target_object->GetName();
        value.exists = true;
        return value;
      }
      /* Try as a game property. */
      EXP_Value *prop = target_object->GetProperty(attr);
      if (prop != nullptr) {
        return MakeValueFromEXPValue(prop, this);
      }
      return fallback;
    }
    case LN_ValueExpressionKind::BonePoseRotation: {
      KX_GameObject *target_object = nullptr;
      if (expression.input0 != LN_INVALID_INDEX) {
        target_object = ResolveObjectValue(
            ResolveValueExpression(expression.input0, MakeNoneValue(), context));
      }
      else if (m_gameObject != nullptr) {
        target_object = m_gameObject;
      }
      const std::string bone_name = ResolveStringExpression(
          expression.string_expr_index, std::string(), context);
      LN_Rotation rotation;
      const LN_BonePoseRotationSpace space = NormalizeBonePoseRotationSpace(
          expression.property_ref_index);
      if (!ResolveBonePoseRotation(target_object, bone_name, space, rotation)) {
        return fallback;
      }
      LN_Value value;
      value.type = LN_ValueType::Rotation;
      value.SetRotation(rotation);
      value.exists = true;
      return value;
    }
    case LN_ValueExpressionKind::BoneAttribute: {
      KX_GameObject *target_object = nullptr;
      if (expression.input0 != LN_INVALID_INDEX) {
        target_object = ResolveObjectValue(
            ResolveValueExpression(expression.input0, MakeNoneValue(), context));
      }
      else if (m_gameObject != nullptr) {
        target_object = m_gameObject;
      }
      const std::string bone_name = ResolveStringExpression(
          expression.string_expr_index, std::string(), context);
      BL_ArmatureObject *armature = ArmatureFacade(target_object);
      blender::Object *ob = armature ? armature->GetArmatureObject() : nullptr;
      if (ob == nullptr || ob->type != blender::OB_ARMATURE || ob->pose == nullptr ||
          bone_name.empty())
      {
        return fallback;
      }
      blender::bPoseChannel *pchan = armature->FindPoseChannel(bone_name);
      if (pchan == nullptr || pchan->bone == nullptr) {
        return fallback;
      }
      blender::Bone *bone = pchan->bone;
      const uint32_t property_ref = expression.property_ref_index == LN_INVALID_INDEX ?
                                        0 :
                                        expression.property_ref_index;
      const uint32_t attribute = property_ref & LN_BONE_ATTRIBUTE_INDEX_MASK;
      const bool world_space = (property_ref & LN_BONE_ATTRIBUTE_WORLD_SPACE_FLAG) != 0;

      auto make_position_value = [&](const MT_Vector3 &position) {
        return MakeVectorValue(world_space ? ArmaturePointToWorld(target_object, position) :
                                             position);
      };
      auto make_rest_position_value = [&](const float parent_position[3],
                                          const float armature_position[3]) {
        const float *position = world_space ? armature_position : parent_position;
        MT_Vector3 result(position[0], position[1], position[2]);
        if (world_space) {
          result = ArmaturePointToWorld(target_object, result);
        }
        return MakeVectorValue(result);
      };

      LN_Value value = MakeNoneValue();
      switch (attribute) {
        case 0:
          value.type = LN_ValueType::String;
          value.string_value = bone->name;
          value.exists = true;
          return value;
        case 1:
          return MakeVectorValue(MT_Vector3(pchan->loc[0], pchan->loc[1], pchan->loc[2]));
        case 2:
          value.type = LN_ValueType::Int;
          value.int_value = bone->inherit_scale_mode;
          value.exists = true;
          return value;
        case 4:
          return make_rest_position_value(bone->head, bone->arm_head);
        case 5:
          return make_position_value(
              MT_Vector3(bone->arm_head[0], bone->arm_head[1], bone->arm_head[2]));
        case 6:
          armature->ApplyPose();
          return make_position_value(
              MT_Vector3(pchan->pose_head[0], pchan->pose_head[1], pchan->pose_head[2]));
        case 7: {
          const float parent_center[3] = {
              (bone->head[0] + bone->tail[0]) * 0.5f,
              (bone->head[1] + bone->tail[1]) * 0.5f,
              (bone->head[2] + bone->tail[2]) * 0.5f,
          };
          const float armature_center[3] = {
              (bone->arm_head[0] + bone->arm_tail[0]) * 0.5f,
              (bone->arm_head[1] + bone->arm_tail[1]) * 0.5f,
              (bone->arm_head[2] + bone->arm_tail[2]) * 0.5f,
          };
          return make_rest_position_value(parent_center, armature_center);
        }
        case 8:
          return make_position_value(MT_Vector3((bone->arm_head[0] + bone->arm_tail[0]) * 0.5f,
                                                (bone->arm_head[1] + bone->arm_tail[1]) * 0.5f,
                                                (bone->arm_head[2] + bone->arm_tail[2]) * 0.5f));
        case 9:
          armature->ApplyPose();
          return make_position_value(
              MT_Vector3((pchan->pose_head[0] + pchan->pose_tail[0]) * 0.5f,
                         (pchan->pose_head[1] + pchan->pose_tail[1]) * 0.5f,
                         (pchan->pose_head[2] + pchan->pose_tail[2]) * 0.5f));
        case 10:
          return make_rest_position_value(bone->tail, bone->arm_tail);
        case 11:
          return make_position_value(
              MT_Vector3(bone->arm_tail[0], bone->arm_tail[1], bone->arm_tail[2]));
        case 12:
          armature->ApplyPose();
          return make_position_value(
              MT_Vector3(pchan->pose_tail[0], pchan->pose_tail[1], pchan->pose_tail[2]));
        case 13:
          value.type = LN_ValueType::Bool;
          value.bool_value = (bone->flag & blender::BONE_HINGE) == 0;
          value.exists = true;
          return value;
        case 14:
          value.type = LN_ValueType::Bool;
          value.bool_value = (bone->flag & blender::BONE_CONNECTED) != 0;
          value.exists = true;
          return value;
        case 15:
          value.type = LN_ValueType::Bool;
          value.bool_value = (bone->flag & blender::BONE_NO_DEFORM) == 0;
          value.exists = true;
          return value;
        case 16:
          value.type = LN_ValueType::Bool;
          value.bool_value = (bone->flag & blender::BONE_NO_LOCAL_LOCATION) == 0;
          value.exists = true;
          return value;
        case 17:
          value.type = LN_ValueType::Bool;
          value.bool_value = (bone->flag & blender::BONE_RELATIVE_PARENTING) != 0;
          value.exists = true;
          return value;
        case 18:
          value.type = LN_ValueType::Bool;
          value.bool_value = (bone->bbone_flag & blender::BBONE_SCALE_EASING) != 0;
          value.exists = true;
          return value;
        default:
          return fallback;
      }
    }
    case LN_ValueExpressionKind::MaterialSlot: {
      KX_GameObject *target_object = m_gameObject;
      if (expression.input0 != LN_INVALID_INDEX) {
        target_object = ResolveObjectValue(
            ResolveValueExpression(expression.input0, MakeNoneValue(), context));
      }
      const int32_t slot = ResolveIntExpression(expression.input1, 0, context);
      return MakeMaterialValue(this, ObjectMaterialAtSlot(target_object, slot));
    }
    case LN_ValueExpressionKind::MaterialNodeValue: {
      blender::Material *material = nullptr;
      blender::Object *parameter_object = nullptr;
      const bool object_slot_target = ELEM(expression.property_ref_index, 1u, 2u, 4u);
      if (object_slot_target) {
        KX_GameObject *target_object = m_gameObject;
        if (expression.input0 != LN_INVALID_INDEX) {
          target_object = ResolveObjectValue(
              ResolveValueExpression(expression.input0, MakeNoneValue(), context));
        }
        parameter_object = target_object ? target_object->GetBlenderObject() : nullptr;
        const int32_t slot = ResolveIntExpression(expression.input1, 0, context);
        material = ObjectMaterialAtSlot(target_object, slot);
      }
      else {
        material = ResolveMaterialByValue(
            this, ResolveValueExpression(expression.input0, MakeNoneValue(), context));
      }

      const std::string node_name = ResolveStringExpression(
          expression.string_expr_index, std::string(), context);
      const std::string socket_name = ResolveStringExpression(
          expression.input2, std::string(), context);
      if (material == nullptr || material->nodetree == nullptr || node_name.empty() ||
          socket_name.empty())
      {
        return fallback;
      }
      blender::bNode *node = FindNodeByUserName(material->nodetree, node_name);
      blender::bNodeSocket *socket = FindInputSocketByIdentifierOrName(node, socket_name);
      if (socket == nullptr) {
        return fallback;
      }
      if (expression.property_ref_index >= 3) {
        material->nodetree->ensure_topology_cache();
        if ((socket->flag & blender::SOCK_UNAVAIL) != 0 || socket->is_directly_linked()) {
          return fallback;
        }
      }
      if (expression.property_ref_index == 2) {
        return ReadMaterialParameterValue(parameter_object, material, *node, *socket, this);
      }
      return ReadSocketDefaultValue(*socket, this);
    }
    case LN_ValueExpressionKind::EditorNodeValue: {
      KX_GameObject *target_object = m_gameObject;
      if (expression.input0 != LN_INVALID_INDEX) {
        target_object = ResolveObjectValue(
            ResolveValueExpression(expression.input0, MakeNoneValue(), context));
      }
      const std::string target_name = ResolveStringExpression(
          expression.input1, std::string(), context);
      blender::bNodeTree *tree = ResolveEditorNodeValueTree(
          EditorNodeValueTarget(expression.property_ref_index), target_object, target_name);
      const std::string node_name = ResolveStringExpression(
          expression.string_expr_index, std::string(), context);
      const std::string socket_name = ResolveStringExpression(
          expression.input2, std::string(), context);
      blender::bNode *node = FindNodeByUserName(tree, node_name);
      blender::bNodeSocket *socket = FindInputSocketByIdentifierOrName(node, socket_name);
      if (tree == nullptr || socket == nullptr || (socket->flag & blender::SOCK_UNAVAIL) != 0) {
        return fallback;
      }
      tree->ensure_topology_cache();
      return socket->is_directly_linked() ? fallback : ReadSocketDefaultValue(*socket, this);
    }
    case LN_ValueExpressionKind::ObjectGameProperty: {
      KX_GameObject *target_object = m_gameObject;
      if (expression.input0 != LN_INVALID_INDEX) {
        target_object = ResolveObjectValue(
            ResolveValueExpression(expression.input0, MakeNoneValue(), context));
      }
      if (target_object == nullptr) {
        return fallback;
      }
      const std::string property_name = ResolveStringExpression(
          expression.input1, std::string(), context);
      if (EXP_Value *property = target_object->GetProperty(property_name)) {
        return MakeValueFromEXPValue(property, this);
      }
      return MakeNoneValue();
    }
    case LN_ValueExpressionKind::CollisionHitObject: {
      const CollisionResult &collision_result = ResolveCollisionResult(
          expression.input0,
          expression.input1,
          expression.input2,
          context,
          CollisionResult::Detail::Objects);
      if (!collision_result.hit || collision_result.hit_object == nullptr) {
        return MakeNoneValue();
      }
      return MakeObjectValue(this, collision_result.hit_object);
    }
    case LN_ValueExpressionKind::CollisionHitObjects: {
      const CollisionResult &collision_result = ResolveCollisionResult(
          expression.input0,
          expression.input1,
          expression.input2,
          context,
          CollisionResult::Detail::Objects);
      LN_Value value;
      value.type = LN_ValueType::List;
      value.exists = true;
      value.list_value.reserve(collision_result.hit_objects.size());
      for (KX_GameObject *object : collision_result.hit_objects) {
        value.list_value.push_back(MakeObjectValue(this, object));
      }
      return value;
    }
    case LN_ValueExpressionKind::ConstraintBreakTargetObject: {
      const LN_RigidBodyConstraintBreakEvent *event = ResolveRigidBodyConstraintBreakEvent(
          expression.input0,
          expression.input1,
          expression.input2,
          expression.rigid_body_constraint_match_mode,
          context);
      if (event == nullptr || m_logicManager == nullptr) {
        return MakeNoneValue();
      }
      return MakeObjectValue(this, m_logicManager->ResolveObjectHandle(event->target));
    }
    case LN_ValueExpressionKind::ConstraintBreakEvents:
      return ResolveRigidBodyConstraintBreakEvents(expression.input0,
                                                   expression.input1,
                                                   expression.input2,
                                                   expression.rigid_body_constraint_match_mode,
                                                   context);
    case LN_ValueExpressionKind::CollisionHitPoints: {
      const CollisionResult &collision_result = ResolveCollisionResult(
          expression.input0,
          expression.input1,
          expression.input2,
          context,
          CollisionResult::Detail::Contacts);
      LN_Value value;
      value.type = LN_ValueType::List;
      value.exists = true;
      value.list_value.reserve(collision_result.hit_points.size());
      for (const MT_Vector3 &point : collision_result.hit_points) {
        value.list_value.push_back(MakeVectorValue(point));
      }
      return value;
    }
    case LN_ValueExpressionKind::CollisionHitNormals: {
      const CollisionResult &collision_result = ResolveCollisionResult(
          expression.input0,
          expression.input1,
          expression.input2,
          context,
          CollisionResult::Detail::Contacts);
      LN_Value value;
      value.type = LN_ValueType::List;
      value.exists = true;
      value.list_value.reserve(collision_result.hit_normals.size());
      for (const MT_Vector3 &normal : collision_result.hit_normals) {
        value.list_value.push_back(MakeVectorValue(normal));
      }
      return value;
    }
    case LN_ValueExpressionKind::LoopCurrentValue: {
      const uint32_t loop_frame_index = expression.property_ref_index;
      if (loop_frame_index >= m_loopStates.size()) {
        return fallback;
      }
      return m_loopStates[loop_frame_index].current_value;
    }
    case LN_ValueExpressionKind::ListElement: {
      const LN_Value list_value = ResolveValueExpression(
          expression.input0, MakeNoneValue(), context);
      if (list_value.type != LN_ValueType::List || !list_value.exists) {
        return fallback;
      }
      const int32_t index = ResolveIntExpression(expression.input1, 0, context);
      if (index < 0 || size_t(index) >= list_value.list_value.size()) {
        return fallback;
      }
      return list_value.list_value[size_t(index)];
    }
    case LN_ValueExpressionKind::ListRandomItem: {
      const LN_Value list_value = ResolveValueExpression(
          expression.input0, MakeNoneValue(), context);
      if (list_value.type != LN_ValueType::List || !list_value.exists ||
          list_value.list_value.empty())
      {
        return fallback;
      }
      const uint64_t seed = (uint64_t(expression_index) << 32) ^ context.tick_index;
      const size_t index = std::min(size_t(HashToUnitFloat(seed) * list_value.list_value.size()),
                                    list_value.list_value.size() - 1);
      return list_value.list_value[index];
    }
    case LN_ValueExpressionKind::ValueSwitchList: {
      const size_t pair_count = expression.input_indices.size() / 2;
      for (size_t pair_index = 0; pair_index < pair_count; pair_index++) {
        const uint32_t condition_index = expression.input_indices[pair_index * 2];
        const uint32_t value_index = expression.input_indices[pair_index * 2 + 1];
        if (condition_index != LN_INVALID_INDEX &&
            ResolveBoolExpression(condition_index, false, context))
        {
          return ResolveValueExpression(value_index, MakeNoneValue(), context);
        }
      }
      return MakeNoneValue();
    }
    case LN_ValueExpressionKind::ValueSwitchListCompare: {
      const LN_Value switch_value = ResolveValueExpression(
          expression.input0, MakeNoneValue(), context);
      const size_t pair_count = expression.input_indices.size() / 2;
      for (size_t pair_index = 0; pair_index < pair_count; pair_index++) {
        const uint32_t case_index = expression.input_indices[pair_index * 2];
        const uint32_t value_index = expression.input_indices[pair_index * 2 + 1];
        const LN_Value case_value = ResolveValueExpression(case_index, MakeNoneValue(), context);
        if (CompareValues(switch_value,
                          case_value,
                          expression.value.exists ?
                              static_cast<LN_FloatCompareOperation>(expression.value.int_value) :
                              LN_FloatCompareOperation::Equal))
        {
          return ResolveValueExpression(value_index, MakeNoneValue(), context);
        }
      }
      return ResolveValueExpression(expression.input1, MakeNoneValue(), context);
    }
    case LN_ValueExpressionKind::MakeList: {
      LN_Value value;
      value.type = LN_ValueType::List;
      value.exists = true;
      if (expression.input0 != LN_INVALID_INDEX) {
        value.list_value.push_back(
            ResolveValueExpression(expression.input0, MakeNoneValue(), context));
      }
      if (expression.input1 != LN_INVALID_INDEX) {
        value.list_value.push_back(
            ResolveValueExpression(expression.input1, MakeNoneValue(), context));
      }
      if (expression.input2 != LN_INVALID_INDEX) {
        value.list_value.push_back(
            ResolveValueExpression(expression.input2, MakeNoneValue(), context));
      }
      return value;
    }
    case LN_ValueExpressionKind::ListDuplicate: {
      LN_Value value;
      value.type = LN_ValueType::List;
      value.exists = true;
      if (expression.input0 != LN_INVALID_INDEX) {
        const LN_Value source = ResolveValueExpression(
            expression.input0, MakeNoneValue(), context);
        if (source.type == LN_ValueType::List && source.exists) {
          value.list_value = source.list_value;
        }
      }
      return value;
    }
    case LN_ValueExpressionKind::DictMerge: {
      LN_Value value;
      value.type = LN_ValueType::Dict;
      value.exists = true;
      if (expression.input0 != LN_INVALID_INDEX) {
        const LN_Value dict_a = ResolveValueExpression(
            expression.input0, MakeNoneValue(), context);
        if (dict_a.type == LN_ValueType::Dict && dict_a.exists) {
          value.dict_value = dict_a.dict_value;
        }
      }
      if (expression.input1 != LN_INVALID_INDEX) {
        const LN_Value dict_b = ResolveValueExpression(
            expression.input1, MakeNoneValue(), context);
        if (dict_b.type == LN_ValueType::Dict && dict_b.exists) {
          for (const auto &entry : dict_b.dict_value) {
            value.dict_value[entry.first] = entry.second;
          }
        }
      }
      return value;
    }
    case LN_ValueExpressionKind::ListExtend: {
      LN_Value value;
      value.type = LN_ValueType::List;
      value.exists = true;
      if (expression.input0 != LN_INVALID_INDEX) {
        const LN_Value list_a = ResolveValueExpression(
            expression.input0, MakeNoneValue(), context);
        if (list_a.type == LN_ValueType::List && list_a.exists) {
          value.list_value.insert(
              value.list_value.end(), list_a.list_value.begin(), list_a.list_value.end());
        }
      }
      if (expression.input1 != LN_INVALID_INDEX) {
        const LN_Value list_b = ResolveValueExpression(
            expression.input1, MakeNoneValue(), context);
        if (list_b.type == LN_ValueType::List && list_b.exists) {
          value.list_value.insert(
              value.list_value.end(), list_b.list_value.begin(), list_b.list_value.end());
        }
      }
      return value;
    }
    case LN_ValueExpressionKind::ListAppend: {
      LN_Value value;
      value.type = LN_ValueType::List;
      value.exists = true;
      if (expression.input0 != LN_INVALID_INDEX) {
        const LN_Value list_value = ResolveValueExpression(
            expression.input0, MakeNoneValue(), context);
        if (list_value.type == LN_ValueType::List && list_value.exists) {
          value.list_value = list_value.list_value;
        }
      }
      if (expression.input1 != LN_INVALID_INDEX) {
        value.list_value.push_back(
            ResolveValueExpression(expression.input1, MakeNoneValue(), context));
      }
      return value;
    }
    case LN_ValueExpressionKind::ListRemoveIndex: {
      LN_Value value;
      value.type = LN_ValueType::List;
      value.exists = true;
      if (expression.input0 != LN_INVALID_INDEX) {
        const LN_Value list_value = ResolveValueExpression(
            expression.input0, MakeNoneValue(), context);
        if (list_value.type == LN_ValueType::List && list_value.exists) {
          value.list_value = list_value.list_value;
        }
      }
      const int32_t index = expression.input1 != LN_INVALID_INDEX ?
                                ResolveIntExpression(expression.input1, 0, context) :
                                0;
      size_t normalized_index = 0;
      if (NormalizeListIndex(index, value.list_value.size(), normalized_index)) {
        value.list_value.erase(value.list_value.begin() + normalized_index);
      }
      return value;
    }
    case LN_ValueExpressionKind::ListRemoveValue: {
      LN_Value value;
      value.type = LN_ValueType::List;
      value.exists = true;
      if (expression.input0 != LN_INVALID_INDEX) {
        const LN_Value list_value = ResolveValueExpression(
            expression.input0, MakeNoneValue(), context);
        if (list_value.type == LN_ValueType::List && list_value.exists) {
          value.list_value = list_value.list_value;
        }
      }
      const LN_Value needle = expression.input1 != LN_INVALID_INDEX ?
                                  ResolveValueExpression(
                                      expression.input1, MakeNoneValue(), context) :
                                  MakeNoneValue();
      const auto iter = std::find_if(
          value.list_value.begin(), value.list_value.end(), [&](const LN_Value &item) {
            return ValuesEqual(item, needle);
          });
      if (iter != value.list_value.end()) {
        value.list_value.erase(iter);
      }
      return value;
    }
    case LN_ValueExpressionKind::ListSetIndex: {
      LN_Value value;
      value.type = LN_ValueType::List;
      value.exists = true;
      if (expression.input0 != LN_INVALID_INDEX) {
        const LN_Value list_value = ResolveValueExpression(
            expression.input0, MakeNoneValue(), context);
        if (list_value.type == LN_ValueType::List && list_value.exists) {
          value.list_value = list_value.list_value;
        }
      }
      const int32_t index = expression.input1 != LN_INVALID_INDEX ?
                                ResolveIntExpression(expression.input1, 0, context) :
                                0;
      size_t normalized_index = 0;
      if (NormalizeListIndex(index, value.list_value.size(), normalized_index)) {
        value.list_value[normalized_index] = expression.input2 != LN_INVALID_INDEX ?
                                                 ResolveValueExpression(
                                                     expression.input2, MakeNoneValue(), context) :
                                                 MakeNoneValue();
      }
      return value;
    }
    case LN_ValueExpressionKind::DictGetKey: {
      const LN_Value dict_value = expression.input0 != LN_INVALID_INDEX ?
                                      ResolveValueExpression(
                                          expression.input0, MakeNoneValue(), context) :
                                      MakeNoneValue();
      const std::string key = expression.input1 != LN_INVALID_INDEX ?
                                  ResolveStringExpression(
                                      expression.input1, std::string(), context) :
                                  std::string();
      const LN_Value default_value = expression.input2 != LN_INVALID_INDEX ?
                                         ResolveValueExpression(
                                             expression.input2, MakeNoneValue(), context) :
                                         MakeNoneValue();
      if (dict_value.type == LN_ValueType::Dict && dict_value.exists && !key.empty()) {
        const auto iter = dict_value.dict_value.find(key);
        if (iter != dict_value.dict_value.end()) {
          return iter->second;
        }
      }
      return default_value;
    }
    case LN_ValueExpressionKind::MakeDict: {
      LN_Value value;
      value.type = LN_ValueType::Dict;
      value.exists = true;
      const std::string key = expression.input0 != LN_INVALID_INDEX ?
                                  ResolveStringExpression(
                                      expression.input0, std::string(), context) :
                                  std::string();
      if (!key.empty()) {
        value.dict_value[key] = expression.input1 != LN_INVALID_INDEX ?
                                    ResolveValueExpression(
                                        expression.input1, MakeNoneValue(), context) :
                                    MakeNoneValue();
      }
      return value;
    }
    case LN_ValueExpressionKind::DictSetKey: {
      LN_Value value;
      value.type = LN_ValueType::Dict;
      value.exists = true;
      if (expression.input0 != LN_INVALID_INDEX) {
        const LN_Value dict_value = ResolveValueExpression(
            expression.input0, MakeNoneValue(), context);
        if (dict_value.type == LN_ValueType::Dict && dict_value.exists) {
          value.dict_value = dict_value.dict_value;
        }
      }
      const std::string key = expression.input1 != LN_INVALID_INDEX ?
                                  ResolveStringExpression(
                                      expression.input1, std::string(), context) :
                                  std::string();
      if (!key.empty()) {
        value.dict_value[key] = expression.input2 != LN_INVALID_INDEX ?
                                    ResolveValueExpression(
                                        expression.input2, MakeNoneValue(), context) :
                                    MakeNoneValue();
      }
      return value;
    }
    case LN_ValueExpressionKind::DictAddValue: {
      LN_Value value = ResolveValueExpression(expression.input0, MakeNoneValue(), context);
      if (value.type != LN_ValueType::Dict || !value.exists) {
        value = LN_Value();
        value.type = LN_ValueType::Dict;
        value.exists = true;
      }
      const std::string key = ResolveStringExpression(expression.input1, std::string(), context);
      if (key.empty()) {
        return value;
      }
      float amount = 0.0f;
      ValueAsFloat(ResolveValueExpression(expression.input2, MakeNoneValue(), context), amount);
      LN_Value &entry = value.dict_value[key];
      const bool was_int = entry.exists && entry.type == LN_ValueType::Int;
      if ((was_int || !entry.exists) && std::isfinite(amount) && amount == std::trunc(amount)) {
        /* Whole amounts keep counts as integers. */
        const double sum = double(was_int ? entry.int_value : 0) + double(amount);
        entry = MakeIntValue(int32_t(std::clamp(sum, double(INT32_MIN), double(INT32_MAX))));
      }
      else {
        float current = 0.0f;
        ValueAsFloat(entry, current);
        entry = MakeFloatValue(current + amount);
      }
      return value;
    }
    case LN_ValueExpressionKind::ListSort:
    case LN_ValueExpressionKind::ListFindIndex: {
      LN_Value list = ResolveValueExpression(expression.input0, MakeNoneValue(), context);
      if (list.type != LN_ValueType::List || !list.exists) {
        list = LN_Value();
        list.type = LN_ValueType::List;
        list.exists = true;
      }
      const LN_Value key = ResolveValueExpression(expression.input1, MakeNoneValue(), context);
      if (expression.kind == LN_ValueExpressionKind::ListFindIndex) {
        const auto iter = std::find_if(
            list.list_value.begin(), list.list_value.end(), [&](const LN_Value &item) {
              return ValuesEqual(item, key);
            });
        return MakeIntValue(iter == list.list_value.end() ?
                                -1 :
                                int32_t(iter - list.list_value.begin()));
      }
      SortListValues(list.list_value,
                     key.exists && key.type == LN_ValueType::String ? key.string_value :
                                                                       std::string(),
                     ValueAsBool(ResolveValueExpression(expression.input2, MakeNoneValue(), context)));
      return list;
    }
    case LN_ValueExpressionKind::InventoryEdit:
    case LN_ValueExpressionKind::InventoryAmount:
    case LN_ValueExpressionKind::InventoryChanged:
    case LN_ValueExpressionKind::InventoryMade:
    case LN_ValueExpressionKind::InventoryOutcome: {
      const bool amount = expression.kind == LN_ValueExpressionKind::InventoryAmount ||
                          expression.kind == LN_ValueExpressionKind::InventoryChanged;
      const char *key = expression.kind == LN_ValueExpressionKind::InventoryAmount  ? "amount" :
                        expression.kind == LN_ValueExpressionKind::InventoryChanged ? "changed" :
                        expression.kind == LN_ValueExpressionKind::InventoryMade    ? "made" :
                        expression.kind == LN_ValueExpressionKind::InventoryOutcome ?
                                                                                    "outcome" :
                                                                                    "inventory";
      /* The Inventory node's outputs: what its last run did, kept in a tree property (a dict of
       * the new list, the amount, what it made and the outcome). */
      if (expression.property_ref_index != LN_INVALID_INDEX) {
        const LN_Value *stored = GetTreePropertyValue(expression.property_ref_index);
        if (stored != nullptr && stored->exists && stored->type == LN_ValueType::Dict) {
          const auto item = stored->dict_value.find(key);
          if (item != stored->dict_value.end()) {
            return item->second;
          }
        }
        if (amount) {
          return MakeIntValue(0);
        }
        if (expression.kind != LN_ValueExpressionKind::InventoryEdit) {
          return MakeStringValue(std::string());
        }
        LN_Value empty;
        empty.type = LN_ValueType::List;
        empty.exists = true;
        return empty;
      }
      LN_Value inventory = ResolveValueExpression(expression.input0, MakeNoneValue(), context);
      if (inventory.type != LN_ValueType::List || !inventory.exists) {
        inventory = LN_Value();
        inventory.type = LN_ValueType::List;
        inventory.exists = true;
      }
      const InventoryEditResult result = RunInventoryEdit(expression, inventory.list_value, context);
      switch (expression.kind) {
        case LN_ValueExpressionKind::InventoryAmount:
          return MakeIntValue(result.amount);
        case LN_ValueExpressionKind::InventoryChanged:
          return MakeIntValue(result.Changed(LN_InventoryOperation(expression.value.int_value)));
        case LN_ValueExpressionKind::InventoryMade:
          return MakeStringValue(result.made);
        case LN_ValueExpressionKind::InventoryOutcome:
          return MakeStringValue(LN_InventoryOutcomeName(result.outcome, result.craft));
        default:
          return inventory;
      }
    }
    case LN_ValueExpressionKind::InventoryItemCount: {
      const LN_Value inventory = ResolveValueExpression(
          expression.input0, MakeNoneValue(), context);
      const std::string item = ItemNameOf(
          ResolveValueExpression(expression.input1, MakeNoneValue(), context));
      return MakeIntValue(inventory.type == LN_ValueType::List && inventory.exists ?
                              LN_InventoryCount(inventory.list_value, item) :
                              0);
    }
    case LN_ValueExpressionKind::ItemLibraryInfo: {
      const std::string item = ItemNameOf(
          ResolveValueExpression(expression.input1, MakeNoneValue(), context));
      const blender::Collection *library = ResolveCollectionByValue(
          this, ResolveValueExpression(expression.input0, MakeNoneValue(), context));
      if (library == nullptr) {
        library = SceneItemLibrary();
      }
      KX_Scene *scene = m_gameObject ? m_gameObject->GetScene() : nullptr;
      KX_GameObject *object = scene ? LN_FindLibraryItem(*scene, library, item) : nullptr;
      const auto property = [&](const std::string &name) {
        EXP_Value *value = object ? object->GetProperty(name) : nullptr;
        return value ? MakeValueFromEXPValue(value, this) : MakeNoneValue();
      };
      switch (LN_ItemInfoField(expression.value.int_value)) {
        case LN_ItemInfoField::Found:
          return MakeBoolValue(object != nullptr);
        case LN_ItemInfoField::Object:
          return MakeObjectValue(this, object);
        case LN_ItemInfoField::Name: {
          const LN_Value name = property("name");
          return MakeStringValue(name.exists && name.type == LN_ValueType::String ?
                                     name.string_value :
                                     item);
        }
        case LN_ItemInfoField::MaxStack:
          return MakeIntValue(ValueAsInt(property("max_stack"), LN_FloatToIntMode::Round));
        case LN_ItemInfoField::Category:
        case LN_ItemInfoField::Description: {
          const LN_Value text = property(LN_ItemInfoField(expression.value.int_value) ==
                                                 LN_ItemInfoField::Category ?
                                             "category" :
                                             "description");
          return MakeStringValue(text.exists && text.type == LN_ValueType::String ?
                                     text.string_value :
                                     std::string());
        }
      }
      return fallback;
    }
    case LN_ValueExpressionKind::RecipeInfo: {
      const LN_Value text = ResolveValueExpression(expression.input0, MakeNoneValue(), context);
      LN_Recipe recipe;
      const bool valid = text.exists && text.type == LN_ValueType::String &&
                         LN_ParseRecipe(text.string_value, recipe);
      switch (LN_RecipeInfoField(expression.value.int_value)) {
        case LN_RecipeInfoField::Valid:
          return MakeBoolValue(valid);
        case LN_RecipeInfoField::Result:
          return MakeStringValue(valid ? recipe.results.front().item : std::string());
        case LN_RecipeInfoField::Count:
          return MakeIntValue(valid ? recipe.results.front().count : 0);
        case LN_RecipeInfoField::Results:
        case LN_RecipeInfoField::Ingredients: {
          LN_Value parts;
          parts.type = LN_ValueType::List;
          parts.exists = true;
          for (const LN_RecipePart &part :
               LN_RecipeInfoField(expression.value.int_value) == LN_RecipeInfoField::Results ?
                   recipe.results :
                   recipe.ingredients)
          {
            parts.list_value.push_back(LN_MakeInventoryEntry(part.item, part.count));
          }
          return parts;
        }
        case LN_RecipeInfoField::Station:
          return MakeStringValue(recipe.station);
        case LN_RecipeInfoField::CanCraft:
        case LN_RecipeInfoField::Craftable: {
          const LN_Value inventory = valid ? ResolveValueExpression(
                                                 expression.input1, MakeNoneValue(), context) :
                                             MakeNoneValue();
          const int32_t craftable = inventory.exists && inventory.type == LN_ValueType::List ?
                                        LN_RecipeCraftable(inventory.list_value, recipe) :
                                        0;
          if (LN_RecipeInfoField(expression.value.int_value) == LN_RecipeInfoField::CanCraft) {
            return MakeBoolValue(craftable > 0);
          }
          return MakeIntValue(craftable);
        }
      }
      return fallback;
    }
    case LN_ValueExpressionKind::RecipeFind: {
      const LN_Value recipes_value = ResolveValueExpression(
          expression.input0, MakeNoneValue(), context);
      const LN_Value items = ResolveValueExpression(expression.input1, MakeNoneValue(), context);
      const LN_Value station = ResolveValueExpression(expression.input2, MakeNoneValue(), context);
      /* Trimmed like the stations of the recipe lines. */
      std::string station_name = station.exists && station.type == LN_ValueType::String ?
                                     station.string_value :
                                     std::string();
      const size_t station_start = station_name.find_first_not_of(" \t\r\n");
      station_name = station_start == std::string::npos ?
                         std::string() :
                         station_name.substr(station_start,
                                             station_name.find_last_not_of(" \t\r\n") -
                                                 station_start + 1);
      std::vector<LN_RecipePart> available;
      const auto add_item = [&](const LN_Value &item) {
        LN_RecipePart part;
        if (item.exists && item.type == LN_ValueType::ObjectRef) {
          part.item = item.reference_name;
        }
        else if (!LN_InventoryEntry(item, part.item, part.count)) {
          return;
        }
        available.push_back(std::move(part));
      };
      if (items.exists && items.type == LN_ValueType::List) {
        for (const LN_Value &item : items.list_value) {
          add_item(item);
        }
      }
      else {
        add_item(items);
      }
      std::vector<LN_Recipe> recipes;
      std::vector<int32_t> indices;
      LN_ParseRecipes(recipes_value, recipes, &indices);
      int32_t found = -1;
      for (size_t index = 0; index < recipes.size(); ++index) {
        if (recipes[index].station == station_name &&
            LN_RecipeMatches(recipes[index], available))
        {
          found = int32_t(index);
          break;
        }
      }
      switch (LN_RecipeFindField(expression.value.int_value)) {
        case LN_RecipeFindField::Found:
          return MakeBoolValue(found >= 0);
        case LN_RecipeFindField::Recipe:
          if (found < 0) {
            return MakeStringValue(std::string());
          }
          return MakeStringValue(recipes_value.type == LN_ValueType::List ?
                                     recipes_value.list_value[size_t(indices[size_t(found)])]
                                         .string_value :
                                     recipes_value.string_value);
        case LN_RecipeFindField::Index:
          return MakeIntValue(found >= 0 ? indices[size_t(found)] : -1);
      }
      return fallback;
    }
    case LN_ValueExpressionKind::DictRemoveKey:
    case LN_ValueExpressionKind::DictRemoveKeyValue: {
      LN_Value dict_output;
      dict_output.type = LN_ValueType::Dict;
      dict_output.exists = true;
      LN_Value removed_value = MakeNoneValue();
      if (expression.input0 != LN_INVALID_INDEX) {
        const LN_Value dict_value = ResolveValueExpression(
            expression.input0, MakeNoneValue(), context);
        if (dict_value.type == LN_ValueType::Dict && dict_value.exists) {
          dict_output.dict_value = dict_value.dict_value;
        }
      }
      const std::string key = expression.input1 != LN_INVALID_INDEX ?
                                  ResolveStringExpression(
                                      expression.input1, std::string(), context) :
                                  std::string();
      if (!key.empty()) {
        const auto iter = dict_output.dict_value.find(key);
        if (iter != dict_output.dict_value.end()) {
          removed_value = iter->second;
          dict_output.dict_value.erase(iter);
        }
      }
      return expression.kind == LN_ValueExpressionKind::DictRemoveKeyValue ? removed_value :
                                                                             dict_output;
    }
    case LN_ValueExpressionKind::ListFromItems: {
      LN_Value value;
      value.type = LN_ValueType::List;
      value.exists = true;
      value.list_value.reserve(expression.input_indices.size());
      for (const uint32_t input_index : expression.input_indices) {
        if (input_index != LN_INVALID_INDEX) {
          value.list_value.push_back(
              ResolveValueExpression(input_index, MakeNoneValue(), context));
        }
      }
      return value;
    }
    case LN_ValueExpressionKind::GetGlobalProperty: {
      const std::string category = ResolveStringExpression(
          expression.input0, std::string(), context);
      const std::string property = ResolveStringExpression(
          expression.input1, std::string(), context);
      if (!category.empty() && !property.empty()) {
        std::lock_guard<std::mutex> lock(GlobalPropertiesMutex());
        EnsureGlobalCategoryLoadedUnlocked(category);
        const auto category_iter = GlobalProperties().find(category);
        if (category_iter != GlobalProperties().end()) {
          const auto property_iter = category_iter->second.find(property);
          if (property_iter != category_iter->second.end()) {
            LN_Value value = property_iter->second;
            LocalizeValue(value);
            return value;
          }
        }
      }
      if (expression.input2 != LN_INVALID_INDEX) {
        return ResolveValueExpression(expression.input2, MakeNoneValue(), context);
      }
      return MakeNoneValue();
    }
    case LN_ValueExpressionKind::ListGlobalProperties: {
      const std::string category = ResolveStringExpression(
          expression.input0, std::string(), context);
      LN_Value value;
      value.type = LN_ValueType::Dict;
      value.exists = true;
      if (!category.empty()) {
        std::lock_guard<std::mutex> lock(GlobalPropertiesMutex());
        EnsureGlobalCategoryLoadedUnlocked(category);
        const auto category_iter = GlobalProperties().find(category);
        if (category_iter != GlobalProperties().end()) {
          value.dict_value = category_iter->second;
        }
      }
      LocalizeValue(value);
      return value;
    }
    case LN_ValueExpressionKind::LoadVariable: {
      const std::string path = ResolveStringExpression(expression.input0, "//Data", context);
      const std::string file_name = ResolveStringExpression(
          expression.input1, "variables", context);
      const std::string name = ResolveStringExpression(expression.input2, "var", context);
      const LN_Value dict = LoadVariableDictFromFile(path, file_name);
      if (dict.type == LN_ValueType::Dict) {
        const auto iter = dict.dict_value.find(name);
        if (iter != dict.dict_value.end()) {
          return iter->second;
        }
      }
      if (!expression.input_indices.empty()) {
        return ResolveValueExpression(expression.input_indices.front(), MakeNoneValue(), context);
      }
      return MakeNoneValue();
    }
    case LN_ValueExpressionKind::LoadVariableDict: {
      const std::string path = ResolveStringExpression(expression.input0, "//Data", context);
      const std::string file_name = ResolveStringExpression(
          expression.input1, "variables", context);
      return LoadVariableDictFromFile(path, file_name);
    }
    case LN_ValueExpressionKind::ListSavedVariables: {
      const std::string path = ResolveStringExpression(expression.input0, "//Data", context);
      const std::string file_name = ResolveStringExpression(
          expression.input1, "variables", context);
      const LN_Value dict = LoadVariableDictFromFile(path, file_name);
      LN_Value value;
      value.type = LN_ValueType::List;
      value.exists = true;
      if (dict.type == LN_ValueType::Dict && dict.exists) {
        for (const auto &item : dict.dict_value) {
          LN_Value key;
          key.type = LN_ValueType::String;
          key.exists = true;
          key.string_value = item.first;
          value.list_value.push_back(key);
        }
      }
      return value;
    }
    case LN_ValueExpressionKind::CurrentScene: {
      KX_Scene *scene = m_gameObject ? m_gameObject->GetScene() : nullptr;
      if (scene == nullptr) {
        return MakeNoneValue();
      }
      LN_Value value;
      value.type = LN_ValueType::SceneRef;
      value.exists = true;
      value.reference_name = scene->GetName();
      value.runtime_ref = MakeSceneRef(scene, scene->GetName());
      return value;
    }
    case LN_ValueExpressionKind::CollectionObjects:
    case LN_ValueExpressionKind::CollectionObjectNames: {
      const LN_Value collection_value = ResolveValueExpression(
          expression.input0, MakeNoneValue(), context);
      blender::Collection *collection = nullptr;
      if (collection_value.type == LN_ValueType::CollectionRef &&
          collection_value.runtime_ref.IsValid())
      {
        collection = ResolveCollectionRef(collection_value.runtime_ref);
      }

      LN_Value value;
      value.type = LN_ValueType::List;
      value.exists = true;
      if (collection == nullptr) {
        return value;
      }
      if (expression.kind == LN_ValueExpressionKind::CollectionObjects) {
        /* The scene's objects in the collection while the game runs (KX_Scene::
         * CollectionMembers): Add Object copies and Set Collection Member included. Before
         * 2026-10-15 only the file's direct entries, found by name. */
        KX_Scene *scene = m_gameObject ? m_gameObject->GetScene() : nullptr;
        if (scene == nullptr) {
          return value;
        }
        const bool include_children = ResolveBoolExpression(
            expression.bool_expr_index, false, context);
        const std::vector<KX_GameObject *> &members = scene->CollectionMembers(collection,
                                                                               include_children);
        value.list_value.reserve(members.size());
        for (KX_GameObject *game_object : members) {
          /* Logic in a player's copy of a menu sees its own copy's objects, not the other
           * players' copies of that menu (as when this list was made by name). */
          if (scene->OverlayCounterpart(game_object, m_gameObject) != game_object) {
            continue;
          }
          LN_Value item;
          item.type = LN_ValueType::ObjectRef;
          item.exists = true;
          item.reference_name = game_object->GetName();
          item.runtime_ref = MakeObjectRef(game_object, game_object->GetName());
          value.list_value.push_back(std::move(item));
        }
        return value;
      }
      /* Names: the file's entries (such as a library's template names). */
      for (blender::CollectionObject &collection_object : collection->gobject) {
        if (collection_object.ob == nullptr) {
          continue;
        }
        LN_Value item;
        item.type = LN_ValueType::String;
        item.exists = true;
        item.string_value = collection_object.ob->id.name + 2;
        value.list_value.push_back(item);
      }
      return value;
    }
    case LN_ValueExpressionKind::RigidBodyConstraintNames: {
      LN_Value value;
      value.type = LN_ValueType::List;
      value.exists = true;

      KX_GameObject *target = m_gameObject;
      if (expression.input0 != LN_INVALID_INDEX) {
        target = ResolveObjectValue(
            ResolveValueExpression(expression.input0, MakeNoneValue(), context));
      }
      if (!UsesJoltPhysics(target)) {
        return value;
      }

      const std::vector<KX_GameObject::RigidBodyConstraintData> &constraints =
          target->GetRigidBodyConstraints();
      const LN_RigidBodyConstraintMatchMode match_mode = ValidRigidBodyConstraintMatchMode(
          expression.rigid_body_constraint_match_mode);
      const std::string query = match_mode == LN_RigidBodyConstraintMatchMode::All ?
                                    std::string() :
                                    ResolveStringExpression(
                                        expression.string_expr_index, std::string(), context);
      if (match_mode != LN_RigidBodyConstraintMatchMode::All && query.empty()) {
        return value;
      }
      if (match_mode == LN_RigidBodyConstraintMatchMode::Exact) {
        if (const KX_GameObject::RigidBodyConstraintData *constraint =
                target->FindRigidBodyConstraint(query))
        {
          LN_Value item;
          item.type = LN_ValueType::String;
          item.exists = true;
          item.string_value = constraint->m_name;
          value.list_value.push_back(std::move(item));
        }
        return value;
      }
      value.list_value.reserve(constraints.size());
      for (const KX_GameObject::RigidBodyConstraintData &constraint : constraints) {
        if (!RigidBodyConstraintNameMatches(constraint.m_name, query, match_mode)) {
          continue;
        }
        LN_Value item;
        item.type = LN_ValueType::String;
        item.exists = true;
        item.string_value = constraint.m_name;
        value.list_value.push_back(std::move(item));
      }
      return value;
    }
    case LN_ValueExpressionKind::DictGetKeys: {
      LN_Value value;
      value.type = LN_ValueType::List;
      value.exists = true;
      const LN_Value dict_value = expression.input0 != LN_INVALID_INDEX ?
                                      ResolveValueExpression(
                                          expression.input0, MakeNoneValue(), context) :
                                      MakeNoneValue();
      if (dict_value.type == LN_ValueType::Dict && dict_value.exists) {
        for (const auto &entry : dict_value.dict_value) {
          LN_Value key_value;
          key_value.type = LN_ValueType::String;
          key_value.exists = true;
          key_value.string_value = entry.first;
          value.list_value.push_back(key_value);
        }
      }
      return value;
    }
    case LN_ValueExpressionKind::EmptyList: {
      LN_Value value;
      value.type = LN_ValueType::List;
      value.exists = true;
      const int32_t length = expression.input0 != LN_INVALID_INDEX ?
                                 ResolveIntExpression(expression.input0, 0, context) :
                                 0;
      if (length > 0) {
        value.list_value.reserve(size_t(length));
        for (int32_t index = 0; index < length; index++) {
          value.list_value.push_back(MakeNoneValue());
        }
      }
      return value;
    }
    case LN_ValueExpressionKind::EmptyDict: {
      LN_Value value;
      value.type = LN_ValueType::Dict;
      value.exists = true;
      return value;
    }
    case LN_ValueExpressionKind::Count:
      break;
  }

  return fallback;
}

const LN_QueryResult &LN_RuntimeTree::ResolveQueryExpression(const uint32_t expression_index,
                                                             const LN_TickContext &context)
{
  static const LN_QueryResult fallback;
  if (expression_index == LN_INVALID_INDEX || m_program == nullptr) {
    return fallback;
  }

  const std::vector<LN_QueryExpression> &expressions = m_program->GetQueryExpressions();
  if (expression_index >= expressions.size()) {
    return fallback;
  }

  const LN_QueryExpression &expression = expressions[expression_index];
  const LN_RuntimeExpressionSemantics semantics = LN_GetRuntimeExpressionSemantics(
      expression.kind);
  ProfileExpressionEvaluation(semantics.fallback_requirements);
  RecordMissingSnapshotChannelsForExpression(semantics);
  const uint32_t state_index = expression.runtime_state_index != LN_INVALID_INDEX ?
                                   expression.runtime_state_index :
                                   expression_index;
  QueryExpressionRuntimeState &state = QueryExpressionState(state_index);
  const bool command_ordered_cache = QueryExpressionNeedsCommandOrderedEvaluation(
                                         expression.kind) &&
                                     context.command_buffer != nullptr &&
                                     context.command_execution_key != UINT64_MAX;
  const uint64_t command_execution_key = command_ordered_cache ? context.command_execution_key :
                                                                 UINT64_MAX;
  if (state.cached_tick == context.tick_index &&
      state.cached_command_ordered == command_ordered_cache &&
      (!command_ordered_cache || state.cached_command_execution_key == command_execution_key))
  {
    return state.cached_result;
  }

  state.cached_tick = context.tick_index;
  state.cached_command_execution_key = command_execution_key;
  state.cached_command_ordered = command_ordered_cache;
  state.cached_result = LN_QueryResult();

  if (expression.condition_bool_expr_index != LN_INVALID_INDEX &&
      !ResolveBoolExpression(expression.condition_bool_expr_index, false, context))
  {
    state.cached_result.physics_query.diagnostic_status = LN_QueryDiagnosticStatus::Disabled;
    return state.cached_result;
  }

  switch (expression.kind) {
    case LN_QueryExpressionKind::Raycast:
    case LN_QueryExpressionKind::RaycastAll: {
      const bool collect_all = expression.kind == LN_QueryExpressionKind::RaycastAll;
      const LN_Value caster_value = ResolveValueExpression(
          expression.input0, MakeNoneValue(), context);
      KX_GameObject *caster_object = ResolveObjectValue(caster_value);
      const std::vector<LN_RuntimeRef> extra_ignore_refs = ResolveIgnoreObjectRefs(
          expression.input3, expression.ignore_list_expr_index, context);

      MT_Vector3 origin = ResolveVectorExpression(
          expression.input1, expression.vector_value, context);
      MT_Vector3 destination = ResolveVectorExpression(
          expression.input2, expression.secondary_vector_value, context);
      MT_Vector3 direction = ResolveVectorExpression(
          expression.input4, expression.tertiary_vector_value, context);
      const float requested_distance = ResolveFloatExpression(
          expression.float_expr_index, expression.float_value, context);
      const bool local_space = ResolveBoolExpression(
          expression.bool_expr_index, expression.bool_value, context);
      if (local_space && !caster_object) {
        caster_object = m_gameObject;
      }
      if (local_space && caster_object) {
        FlushPendingRaycastTransforms(caster_object, context);
        if (expression.ray_input_mode == LN_RayInputMode::EndPoint) {
          const MT_Transform transform = caster_object->NodeGetWorldTransform();
          origin = transform * origin;
          destination = transform * destination;
        }
        else {
          origin = caster_object->NodeGetWorldTransform() * origin;
          direction = caster_object->NodeGetWorldOrientation() * direction;
        }
      }

      const bool direction_mode = expression.ray_input_mode == LN_RayInputMode::DirectionDistance;
      const bool finite_inputs = vector_is_finite(origin) &&
                                 (direction_mode ? vector_is_finite(direction) &&
                                                       std::isfinite(requested_distance) :
                                                   vector_is_finite(destination));
      if (!finite_inputs) {
        state.cached_result.physics_query.diagnostic_status =
            LN_QueryDiagnosticStatus::InvalidTarget;
        break;
      }
      if (direction_mode) {
        const float direction_length = direction.length();
        if (requested_distance <= 0.0f || !std::isfinite(direction_length) ||
            direction_length <= MT_EPSILON)
        {
          state.cached_result.physics_query.diagnostic_status =
              LN_QueryDiagnosticStatus::InvalidTarget;
          break;
        }
        direction /= direction_length;
        destination = origin + direction * requested_distance;
        if (!vector_is_finite(destination)) {
          state.cached_result.physics_query.diagnostic_status =
              LN_QueryDiagnosticStatus::InvalidTarget;
          break;
        }
      }
      const float cast_distance = (destination - origin).length();
      if (!std::isfinite(cast_distance) || cast_distance <= MT_EPSILON) {
        state.cached_result.physics_query.diagnostic_status =
            LN_QueryDiagnosticStatus::InvalidTarget;
        break;
      }
      if (!direction_mode) {
        direction = (destination - origin) / cast_distance;
      }

      LN_RuntimeRef caster_ref;
      if (caster_object) {
        caster_ref = MakeObjectRef(caster_object, caster_object->GetName());
      }

      KX_Scene *scene = m_gameObject ? m_gameObject->GetScene() : nullptr;
      PHY_IPhysicsEnvironment *environment = scene ? scene->GetPhysicsEnvironment() : nullptr;
      if (!environment) {
        state.cached_result.physics_query.diagnostic_status =
            LN_QueryDiagnosticStatus::MissingPhysicsWorld;
        break;
      }

      const int32_t mask_value = ResolveIntExpression(
          expression.int_expr_index, expression.int_value, context);
      if (mask_value < 0 || mask_value > LN_RAYCAST_MAX_COLLISION_MASK) {
        state.cached_result.physics_query.diagnostic_status =
            LN_QueryDiagnosticStatus::InvalidFilter;
        break;
      }
      const std::string property_name = ResolveStringExpression(
          expression.string_expr_index, std::string(), context);
      const bool xray = ResolveBoolExpression(
          expression.secondary_bool_expr_index, false, context);
      const int32_t max_results = collect_all ? std::clamp(ResolveIntExpression(
                                                               expression.secondary_int_expr_index,
                                                               expression.secondary_int_value,
                                                               context),
                                                           1,
                                                           256) :
                                                1;

      PHY_RayQuerySettings settings;
      settings.origin = origin;
      settings.destination = destination;
      settings.max_results = uint32_t(max_results +
                                      (collect_all && !xray && !property_name.empty() ? 1 : 0));
      if ((expression.ray_query_detail_flags & LN_RAY_QUERY_DETAIL_FACE_INDEX) != 0) {
        settings.detail_flags |= PHY_RAY_QUERY_DETAIL_FACE_INDEX;
      }
      if ((expression.ray_query_detail_flags & LN_RAY_QUERY_DETAIL_UV) != 0) {
        settings.detail_flags |= PHY_RAY_QUERY_DETAIL_UV;
      }
      settings.include_sensors = ResolveBoolExpression(
          expression.quaternary_bool_expr_index, false, context);
      settings.hit_back_faces = ResolveBoolExpression(
          expression.quinary_bool_expr_index, false, context);

      LN_PhysicsQueryResult fallback;
      fallback.hit_position = destination;
      fallback.hit_normal = MT_Vector3(0.0f, 0.0f, 0.0f);
      fallback.ray_direction = direction;
      fallback.end_position = destination;
      fallback.hit_fraction = 1.0f;
      fallback.hit_distance = cast_distance;
      state.cached_result.physics_query = fallback;

      bool blocked = false;
      bool supported = false;
      LN_PhysicsQueryResult blocker;
      state.cached_result.physics_query_results = RayCast(settings,
                                                          caster_ref,
                                                          uint32_t(mask_value),
                                                          property_name,
                                                          xray,
                                                          extra_ignore_refs,
                                                          blocked,
                                                          blocker,
                                                          supported);
      if (!supported) {
        state.cached_result.physics_query = LN_PhysicsQueryResult();
        state.cached_result.physics_query.diagnostic_status =
            LN_QueryDiagnosticStatus::UnsupportedPhysicsBackend;
        state.cached_result.physics_query_results.clear();
        break;
      }
      if (!state.cached_result.physics_query_results.empty()) {
        state.cached_result.physics_query = state.cached_result.physics_query_results.front();
      }
      else if (blocked) {
        state.cached_result.physics_query = blocker;
      }
      state.cached_result.physics_query.blocked = blocked;
      if (state.cached_result.physics_query_results.size() > size_t(max_results)) {
        state.cached_result.physics_query_results.resize(size_t(max_results));
      }
      if (ResolveBoolExpression(expression.tertiary_bool_expr_index, false, context)) {
        const bool physical_stop = state.cached_result.physics_query.hit || blocked;
        DrawDebugRaycast(origin,
                         state.cached_result.physics_query.hit ?
                             state.cached_result.physics_query.hit_position :
                         blocked ? blocker.hit_position :
                                   destination,
                         physical_stop,
                         state.cached_result.physics_query.hit ?
                             &state.cached_result.physics_query.hit_normal :
                             nullptr,
                         cast_distance);
      }
      break;
    }
    case LN_QueryExpressionKind::RaycastFromMesh: {
      KX_GameObject *source_object = expression.input0 != LN_INVALID_INDEX ?
                                         ResolveObjectValue(ResolveValueExpression(
                                             expression.input0, MakeNoneValue(), context)) :
                                         m_gameObject;
      if (!source_object) {
        state.cached_result.physics_query.diagnostic_status =
            LN_QueryDiagnosticStatus::InvalidTarget;
        break;
      }

      FlushPendingRaycastTransforms(source_object, context);
      const float start_offset = ResolveFloatExpression(
          expression.float_expr_index, expression.float_value, context);
      const float max_distance = ResolveFloatExpression(
          expression.secondary_float_expr_index, expression.secondary_float_value, context);
      if (!std::isfinite(start_offset) || start_offset < 0.0f || !std::isfinite(max_distance) ||
          max_distance <= MT_EPSILON)
      {
        state.cached_result.physics_query.diagnostic_status =
            LN_QueryDiagnosticStatus::InvalidTarget;
        break;
      }

      const int32_t mask_value = ResolveIntExpression(
          expression.int_expr_index, expression.int_value, context);
      if (mask_value < 0 || mask_value > LN_RAYCAST_MAX_COLLISION_MASK) {
        state.cached_result.physics_query.diagnostic_status =
            LN_QueryDiagnosticStatus::InvalidFilter;
        break;
      }
      if ((expression.mesh_ray_domain != LN_MeshRayDomain::Vertices &&
           expression.mesh_ray_domain != LN_MeshRayDomain::Faces) ||
          (expression.mesh_ray_direction != LN_MeshRayDirection::Normals &&
           expression.mesh_ray_direction != LN_MeshRayDirection::InvertedNormals))
      {
        state.cached_result.physics_query.diagnostic_status =
            LN_QueryDiagnosticStatus::InvalidTarget;
        break;
      }
      const size_t max_rays = size_t(std::clamp(
          ResolveIntExpression(
              expression.secondary_int_expr_index, expression.secondary_int_value, context),
          1,
          LN_RAYCAST_FROM_MESH_MAX_RAYS));

      const MT_Transform world_transform = source_object->NodeGetWorldTransform();
      if (!vector_is_finite(world_transform.getOrigin())) {
        state.cached_result.physics_query.diagnostic_status =
            LN_QueryDiagnosticStatus::InvalidTarget;
        break;
      }
      MT_Matrix3x3 normal_matrix;
      if (!BuildMeshNormalMatrix(world_transform.getBasis(), normal_matrix)) {
        state.cached_result.physics_query.diagnostic_status =
            LN_QueryDiagnosticStatus::InvalidTarget;
        break;
      }

      const std::vector<LN_MeshRaySample> samples = SampleMeshRays(
          *source_object, expression.mesh_ray_domain, max_rays);
      if (samples.empty()) {
        state.cached_result.physics_query.diagnostic_status =
            LN_QueryDiagnosticStatus::InvalidTarget;
        break;
      }

      std::vector<PHY_RayQuerySettings> settings;
      settings.reserve(samples.size());
      state.cached_result.physics_query_results.reserve(samples.size());
      const bool invert_direction = expression.mesh_ray_direction ==
                                    LN_MeshRayDirection::InvertedNormals;
      const bool include_sensors = ResolveBoolExpression(
          expression.quaternary_bool_expr_index, false, context);
      const bool hit_back_faces = ResolveBoolExpression(
          expression.quinary_bool_expr_index, false, context);
      for (const LN_MeshRaySample &sample : samples) {
        MT_Vector3 direction = normal_matrix * sample.normal;
        if (!NormalizeFinite(direction)) {
          continue;
        }
        if (invert_direction) {
          direction = -direction;
        }
        const MT_Vector3 surface_position = world_transform * sample.position;
        const MT_Vector3 origin = surface_position + direction * start_offset;
        const MT_Vector3 destination = origin + direction * max_distance;
        if (!vector_is_finite(surface_position) || !vector_is_finite(origin) ||
            !vector_is_finite(destination))
        {
          continue;
        }
        /* Validate the segment after performing the world-space float arithmetic. At very large
         * coordinates a finite positive Max Distance can round away entirely, while an extreme
         * finite distance can still overflow the derived length. Report the segment that is
         * actually sent to physics rather than the idealized input distance. */
        MT_Vector3 cast_direction = destination - origin;
        const float cast_distance = cast_direction.length();
        if (!std::isfinite(cast_distance) || cast_distance <= MT_EPSILON) {
          continue;
        }
        cast_direction /= cast_distance;
        if (!vector_is_finite(cast_direction)) {
          continue;
        }

        PHY_RayQuerySettings ray_settings;
        ray_settings.origin = origin;
        ray_settings.destination = destination;
        ray_settings.max_results = 1;
        if ((expression.ray_query_detail_flags & LN_RAY_QUERY_DETAIL_FACE_INDEX) != 0) {
          ray_settings.detail_flags |= PHY_RAY_QUERY_DETAIL_FACE_INDEX;
        }
        if ((expression.ray_query_detail_flags & LN_RAY_QUERY_DETAIL_UV) != 0) {
          ray_settings.detail_flags |= PHY_RAY_QUERY_DETAIL_UV;
        }
        ray_settings.include_sensors = include_sensors;
        ray_settings.hit_back_faces = hit_back_faces;
        settings.push_back(ray_settings);

        LN_PhysicsQueryResult miss;
        miss.diagnostic_status = LN_QueryDiagnosticStatus::NoHit;
        miss.hit_position = destination;
        miss.hit_normal = MT_Vector3(0.0f, 0.0f, 0.0f);
        miss.ray_origin = origin;
        miss.ray_direction = cast_direction;
        miss.end_position = destination;
        miss.hit_fraction = 1.0f;
        miss.hit_distance = cast_distance;
        miss.polygon_index = -1;
        state.cached_result.physics_query_results.push_back(std::move(miss));
      }
      if (settings.empty()) {
        state.cached_result.physics_query_results.clear();
        state.cached_result.physics_query.diagnostic_status =
            LN_QueryDiagnosticStatus::InvalidTarget;
        break;
      }

      KX_Scene *scene = source_object->GetScene();
      PHY_IPhysicsEnvironment *environment = scene ? scene->GetPhysicsEnvironment() : nullptr;
      if (!environment) {
        state.cached_result.physics_query = state.cached_result.physics_query_results.front();
        state.cached_result.physics_query.diagnostic_status =
            LN_QueryDiagnosticStatus::MissingPhysicsWorld;
        break;
      }

      const std::vector<LN_RuntimeRef> extra_ignore_refs = ResolveIgnoreObjectRefs(
          expression.input1, expression.ignore_list_expr_index, context);
      const LN_PhysicsControllerIgnoreSet extra_ignore_controllers(*this, extra_ignore_refs);
      const std::string property_name = ResolveStringExpression(
          expression.string_expr_index, std::string(), context);
      const bool xray = ResolveBoolExpression(
          expression.secondary_bool_expr_index, false, context);
      LN_RayQueryFilterCallback filter(RaycastOwningPhysicsController(source_object),
                                       extra_ignore_controllers,
                                       uint32_t(mask_value),
                                       property_name,
                                       xray);
      std::vector<std::vector<PHY_RayCastResult>> physics_results;
      if (!environment->RayCastBatch(
              settings, filter, PHY_RayBatchExecution::Adaptive, physics_results))
      {
        state.cached_result.physics_query = state.cached_result.physics_query_results.front();
        state.cached_result.physics_query.diagnostic_status =
            LN_QueryDiagnosticStatus::UnsupportedPhysicsBackend;
        break;
      }

      bool any_hit = false;
      bool any_blocked = false;
      size_t first_hit = size_t(-1);
      const size_t result_count = std::min(physics_results.size(),
                                           state.cached_result.physics_query_results.size());
      for (size_t ray_index = 0; ray_index < result_count; ray_index++) {
        LN_PhysicsQueryResult &result = state.cached_result.physics_query_results[ray_index];
        if (physics_results[ray_index].empty()) {
          continue;
        }
        const PHY_RayCastResult &physics_result = physics_results[ray_index].front();
        KX_ClientObjectInfo *info = physics_result.m_controller ?
                                        static_cast<KX_ClientObjectInfo *>(
                                            physics_result.m_controller->GetNewClientInfo()) :
                                        nullptr;
        KX_GameObject *hit_object = info ? info->m_gameobject : nullptr;
        if (!hit_object) {
          continue;
        }

        if (!std::isfinite(physics_result.m_fraction) ||
            !vector_is_finite(physics_result.m_hitPoint))
        {
          continue;
        }
        const float fraction = std::clamp(physics_result.m_fraction, 0.0f, 1.0f);
        result.hit_position = physics_result.m_hitPoint;
        result.hit_fraction = fraction;
        result.hit_distance *= fraction;
        const bool property_matches = property_name.empty() ||
                                      hit_object->GetProperty(property_name);
        if (!property_matches) {
          if (!xray) {
            result.blocked = true;
            any_blocked = true;
          }
          continue;
        }

        result.hit = true;
        result.diagnostic_status = LN_QueryDiagnosticStatus::Hit;
        result.object_ref = MakeObjectRef(hit_object, hit_object->GetName());
        result.hit_normal = vector_is_finite(physics_result.m_hitNormal) ?
                                physics_result.m_hitNormal :
                                MT_Vector3(0.0f, 0.0f, 0.0f);
        result.polygon_index = physics_result.m_polygon;
        result.has_uv = physics_result.m_hitUVOK != 0 &&
                        std::isfinite(physics_result.m_hitUV.x()) &&
                        std::isfinite(physics_result.m_hitUV.y());
        result.hit_uv = result.has_uv ? physics_result.m_hitUV : MT_Vector2(0.0f, 0.0f);
        any_hit = true;
        if (first_hit == size_t(-1)) {
          first_hit = ray_index;
        }
      }

      state.cached_result.physics_query =
          first_hit != size_t(-1) ? state.cached_result.physics_query_results[first_hit] :
                                    state.cached_result.physics_query_results.front();
      state.cached_result.physics_query.hit = any_hit;
      state.cached_result.physics_query.blocked = any_blocked;
      state.cached_result.physics_query.diagnostic_status = any_hit ?
                                                                LN_QueryDiagnosticStatus::Hit :
                                                                LN_QueryDiagnosticStatus::NoHit;

      if (ResolveBoolExpression(expression.tertiary_bool_expr_index, false, context)) {
        for (const LN_PhysicsQueryResult &result : state.cached_result.physics_query_results) {
          const bool physical_stop = result.hit || result.blocked;
          DrawDebugRaycast(result.ray_origin,
                           physical_stop ? result.hit_position : result.end_position,
                           physical_stop,
                           result.hit ? &result.hit_normal : nullptr,
                           max_distance);
        }
      }
      break;
    }
    case LN_QueryExpressionKind::ShapeCast:
    case LN_QueryExpressionKind::ShapeCastAll: {
      const bool collect_all = expression.kind == LN_QueryExpressionKind::ShapeCastAll;
      const LN_Value caster_value = ResolveValueExpression(
          expression.input0, MakeNoneValue(), context);
      KX_GameObject *caster_object = ResolveObjectValue(caster_value);
      const std::vector<LN_RuntimeRef> extra_ignore_refs = ResolveIgnoreObjectRefs(
          expression.input3, expression.ignore_list_expr_index, context);

      MT_Vector3 origin = ResolveVectorExpression(
          expression.input1, expression.vector_value, context);
      MT_Vector3 destination = ResolveVectorExpression(
          expression.input2, expression.secondary_vector_value, context);
      const LN_Rotation rotation = ResolveRotationExpression(
          expression.input4, LN_Rotation::FromEulerXYZ(expression.tertiary_vector_value), context);
      const MT_Vector3 half_extents = ResolveVectorExpression(
          expression.input5, expression.quaternary_vector_value, context);
      MT_Matrix3x3 orientation = rotation.ToMatrix();
      const bool local_space = ResolveBoolExpression(
          expression.bool_expr_index, expression.bool_value, context);
      if (local_space && !caster_object) {
        caster_object = m_gameObject;
      }
      if (local_space && caster_object) {
        FlushPendingRaycastTransforms(caster_object, context);
        const MT_Transform transform = caster_object->NodeGetWorldTransform();
        origin = transform * origin;
        destination = transform * destination;
        orientation = caster_object->NodeGetWorldOrientation() * orientation;
      }

      LN_RuntimeRef caster_ref;
      if (caster_object) {
        caster_ref = MakeObjectRef(caster_object, caster_object->GetName());
      }

      const float radius = ResolveFloatExpression(
          expression.float_expr_index, expression.float_value, context);
      const float height = ResolveFloatExpression(
          expression.secondary_float_expr_index, expression.secondary_float_value, context);
      const float extra_radius = ResolveFloatExpression(
          expression.tertiary_float_expr_index, expression.tertiary_float_value, context);
      const bool finite_inputs = vector_is_finite(origin) && vector_is_finite(destination) &&
                                 vector_is_finite(half_extents) && std::isfinite(radius) &&
                                 std::isfinite(height) && std::isfinite(extra_radius);
      const bool valid_shape = expression.shape_cast_type == LN_ShapeCastType::Box ?
                                   half_extents.x() > 0.0f && half_extents.y() > 0.0f &&
                                       half_extents.z() > 0.0f :
                               expression.shape_cast_type == LN_ShapeCastType::Capsule ?
                                   radius > 0.0f && height > 0.0f :
                                   radius > 0.0f;
      if (!finite_inputs || !valid_shape || extra_radius < 0.0f) {
        state.cached_result.physics_query.diagnostic_status =
            LN_QueryDiagnosticStatus::InvalidTarget;
        break;
      }
      const MT_Vector3 cast_displacement = destination - origin;
      const float cast_distance = cast_displacement.length();
      if (!std::isfinite(cast_distance)) {
        state.cached_result.physics_query.diagnostic_status =
            LN_QueryDiagnosticStatus::InvalidTarget;
        break;
      }

      const int32_t mask_value = ResolveIntExpression(
          expression.int_expr_index, expression.int_value, context);
      if (mask_value < 0 || mask_value > LN_RAYCAST_MAX_COLLISION_MASK) {
        state.cached_result.physics_query.diagnostic_status =
            LN_QueryDiagnosticStatus::InvalidFilter;
        break;
      }
      const int32_t max_results = collect_all ? std::clamp(ResolveIntExpression(
                                                               expression.secondary_int_expr_index,
                                                               expression.secondary_int_value,
                                                               context),
                                                           1,
                                                           256) :
                                                1;
      KX_Scene *scene = m_gameObject ? m_gameObject->GetScene() : nullptr;
      if (!scene || !scene->GetPhysicsEnvironment()) {
        state.cached_result.physics_query.diagnostic_status =
            LN_QueryDiagnosticStatus::MissingPhysicsWorld;
        break;
      }

      PHY_ShapeCastSettings settings;
      settings.type = static_cast<PHY_ShapeCastType>(expression.shape_cast_type);
      settings.origin = origin;
      settings.destination = destination;
      settings.orientation = orientation;
      settings.half_extents = half_extents;
      settings.radius = radius;
      settings.height = height;
      settings.extra_radius = extra_radius;
      if ((expression.ray_query_detail_flags & LN_RAY_QUERY_DETAIL_FACE_INDEX) != 0) {
        settings.detail_flags |= PHY_RAY_QUERY_DETAIL_FACE_INDEX;
      }
      if ((expression.ray_query_detail_flags & LN_RAY_QUERY_DETAIL_UV) != 0) {
        settings.detail_flags |= PHY_RAY_QUERY_DETAIL_UV;
      }
      settings.include_sensors = ResolveBoolExpression(
          expression.quaternary_bool_expr_index, false, context);
      settings.hit_back_faces = ResolveBoolExpression(
          expression.quinary_bool_expr_index, false, context);

      const std::string property_name = ResolveStringExpression(
          expression.string_expr_index, std::string(), context);
      const bool xray = ResolveBoolExpression(
          expression.secondary_bool_expr_index, false, context);
      settings.max_results = uint32_t(max_results +
                                      (collect_all && !xray && !property_name.empty() ? 1 : 0));
      LN_PhysicsQueryResult fallback;
      fallback.cast_position = destination;
      fallback.ray_direction = cast_distance > MT_EPSILON ? cast_displacement / cast_distance :
                                                            MT_Vector3(0.0f, 0.0f, 0.0f);
      fallback.hit_fraction = 1.0f;
      fallback.hit_distance = cast_distance;
      state.cached_result.physics_query = fallback;

      bool blocked = false;
      bool supported = false;
      LN_PhysicsQueryResult blocker;
      state.cached_result.physics_query_results = ShapeCast(settings,
                                                            caster_ref,
                                                            uint32_t(mask_value),
                                                            property_name,
                                                            xray,
                                                            extra_ignore_refs,
                                                            blocked,
                                                            blocker,
                                                            supported);
      if (!supported) {
        state.cached_result.physics_query = LN_PhysicsQueryResult();
        state.cached_result.physics_query.diagnostic_status =
            LN_QueryDiagnosticStatus::UnsupportedPhysicsBackend;
        break;
      }
      if (!state.cached_result.physics_query_results.empty()) {
        state.cached_result.physics_query = state.cached_result.physics_query_results.front();
      }
      else if (blocked) {
        state.cached_result.physics_query = blocker;
      }
      state.cached_result.physics_query.blocked = blocked;
      if (state.cached_result.physics_query_results.size() > size_t(max_results)) {
        state.cached_result.physics_query_results.resize(size_t(max_results));
      }

      if (ResolveBoolExpression(expression.tertiary_bool_expr_index, false, context)) {
        DrawDebugShapeCast(settings,
                           state.cached_result.physics_query_results,
                           blocked ? &blocker : nullptr,
                           collect_all);
      }
      break;
    }
    case LN_QueryExpressionKind::MouseRay: {
      const float distance = ResolveFloatExpression(
          expression.float_expr_index, expression.float_value, context);
      if (distance < 0.0f) {
        state.cached_result.physics_query.diagnostic_status =
            LN_QueryDiagnosticStatus::InvalidTarget;
      }
      else {
        const std::string property_name = ResolveStringExpression(
            expression.string_expr_index, std::string(), context);
        const bool xray = ResolveBoolExpression(
            expression.bool_expr_index, expression.bool_value, context);
        const int32_t mask_value = ResolveIntExpression(
            expression.int_expr_index, expression.int_value, context);
        if (mask_value < 0 || mask_value > LN_RAYCAST_MAX_COLLISION_MASK) {
          state.cached_result.physics_query.diagnostic_status =
              LN_QueryDiagnosticStatus::InvalidFilter;
          break;
        }

        KX_Scene *scene = m_gameObject ? m_gameObject->GetScene() : nullptr;
        auto cast_view = [&](KX_Camera *camera,
                             const LN_InteractionDomain domain) -> LN_PhysicsQueryResult {
          LN_PhysicsQueryResult result;
          if (camera == nullptr || camera->GetScene() != scene) {
            result.diagnostic_status = LN_QueryDiagnosticStatus::InvalidTarget;
            return result;
          }
          FlushPendingRaycastTransforms(camera, context);
          MT_Vector3 from;
          MT_Vector3 to;
          if (!ComputeMouseRay(camera, distance, from, to)) {
            result.diagnostic_status = LN_QueryDiagnosticStatus::InvalidTarget;
            return result;
          }
          const LN_RuntimeRef camera_ref = MakeObjectRef(camera, camera->GetName());
          return RaycastPointer(from,
                                to,
                                camera,
                                camera_ref,
                                uint32_t(mask_value),
                                property_name,
                                xray,
                                domain,
                                context.command_execution_key);
        };

        switch (expression.interaction_view) {
          case LN_InteractionView::Automatic: {
            if (scene != nullptr && scene->GetOverlayCamera() != nullptr) {
              LN_PhysicsQueryResult overlay_result = cast_view(scene->GetOverlayCamera(),
                                                               LN_InteractionDomain::Overlay);
              if (overlay_result.hit || overlay_result.blocked) {
                state.cached_result.physics_query = std::move(overlay_result);
                break;
              }
            }
            state.cached_result.physics_query = cast_view(PointerWorldCamera(scene),
                                                          LN_InteractionDomain::Main);
            break;
          }
          case LN_InteractionView::Main:
            state.cached_result.physics_query = cast_view(PointerWorldCamera(scene),
                                                          LN_InteractionDomain::Main);
            break;
          case LN_InteractionView::Overlay:
            state.cached_result.physics_query = cast_view(
                scene ? scene->GetOverlayCamera() : nullptr, LN_InteractionDomain::Overlay);
            break;
          case LN_InteractionView::CustomCamera: {
            KX_Camera *camera = expression.input0 != LN_INVALID_INDEX ?
                                    ResolveCameraValue(ResolveValueExpression(
                                        expression.input0, MakeNoneValue(), context)) :
                                    nullptr;
            state.cached_result.physics_query = cast_view(camera, LN_InteractionDomain::Any);
            break;
          }
        }
      }
      break;
    }
    case LN_QueryExpressionKind::CameraRay: {
      KX_Camera *camera = GetActiveCamera();
      FlushPendingRaycastTransforms(camera, context);
      const MT_Vector3 aim = ResolveVectorExpression(
          expression.input1, expression.vector_value, context);
      const float distance = ResolveFloatExpression(
          expression.float_expr_index, expression.float_value, context);
      MT_Vector3 from;
      MT_Vector3 to;
      if (distance < 0.0f) {
        state.cached_result.physics_query.diagnostic_status =
            LN_QueryDiagnosticStatus::InvalidTarget;
      }
      else if (ProjectScreenToWorld(camera, aim.x(), aim.y(), 0.0f, from) &&
               ProjectScreenToWorld(camera, aim.x(), aim.y(), std::max(distance, 0.0f), to))
      {
        const std::string property_name = ResolveStringExpression(
            expression.string_expr_index, std::string(), context);
        const bool xray = ResolveBoolExpression(
            expression.bool_expr_index, expression.bool_value, context);
        const int32_t mask_value = ResolveIntExpression(
            expression.int_expr_index, expression.int_value, context);
        if (mask_value < 0 || mask_value > LN_RAYCAST_MAX_COLLISION_MASK) {
          state.cached_result.physics_query.diagnostic_status =
              LN_QueryDiagnosticStatus::InvalidFilter;
          break;
        }
        LN_RuntimeRef camera_ref;
        if (camera != nullptr) {
          camera_ref = MakeObjectRef(camera, camera->GetName());
        }
        state.cached_result.physics_query = Raycast(
            from, to, camera_ref, uint32_t(mask_value), property_name, xray);
      }
      else {
        state.cached_result.physics_query.diagnostic_status =
            LN_QueryDiagnosticStatus::InvalidTarget;
      }
      break;
    }
    case LN_QueryExpressionKind::UIElementEvent: {
      if (LN_UIElementEvent(expression.int_value) == LN_UIElementEvent::UISound) {
        /* On UI Sound: this frame's UI sounds, one a player, heard like On UI Event's events. */
        size_t count = 0;
        const LN_UIService::SoundEvent *sound =
            m_logicManager != nullptr ?
                m_logicManager->GetUIService().FindSoundEvent(
                    m_logicManager->GetUIService().MenuUserOfObject(m_gameObject),
                    state.ui_event_cursor,
                    &count) :
                nullptr;
        state.cached_result.ui_event_count = uint32_t(count);
        state.cached_result.valid = sound != nullptr;
        state.cached_result.physics_query.hit = sound != nullptr;
        state.cached_result.physics_query.diagnostic_status =
            sound != nullptr ? LN_QueryDiagnosticStatus::Hit : LN_QueryDiagnosticStatus::NoHit;
        if (sound == nullptr) {
          break;
        }
        state.cached_result.ui_sound = int32_t(sound->sound);
        state.cached_result.ui_menu = sound->menu;
        state.cached_result.ui_action = sound->action;
        state.cached_result.ui_user = sound->user;
        if (KX_GameObject *object = m_logicManager->ResolveObjectHandle(sound->object)) {
          state.cached_result.ui_source_object = MakeObjectRef(object, object->GetName());
        }
        break;
      }
      /* A tree in a player's menu hears only that player's UI events (split screen: each menu's
       * copy of the same tree); elsewhere it hears every player's. The cursor picks the event
       * whose turn it is: the route runs again for each further one (ExecuteEvent), last event
       * first, so the topmost element's event runs last and what it sets stays. */
      size_t count = 0;
      const LN_UIService::ElementEvent *event =
          m_logicManager != nullptr ?
              m_logicManager->GetUIService().FindElementEvent(
                  LN_UIElementEvent(expression.int_value),
                  ResolveStringExpression(expression.string_expr_index, std::string(), context),
                  m_logicManager->GetUIService().MenuUserOfObject(m_gameObject),
                  state.ui_event_cursor,
                  &count,
                  true) :
              nullptr;
      state.cached_result.ui_event_count = uint32_t(count);
      state.cached_result.valid = event != nullptr;
      state.cached_result.physics_query.hit = event != nullptr;
      state.cached_result.physics_query.diagnostic_status =
          event != nullptr ? LN_QueryDiagnosticStatus::Hit : LN_QueryDiagnosticStatus::NoHit;
      if (event == nullptr) {
        break;
      }
      state.cached_result.physics_query.hit_position = event->point;
      state.cached_result.physics_query.hit_normal = event->normal;
      state.cached_result.ui_action = event->action;
      state.cached_result.ui_target_action = event->target_action;
      state.cached_result.ui_outcome = event->outcome;
      if (KX_GameObject *object = m_logicManager->ResolveObjectHandle(event->object)) {
        state.cached_result.ui_source_object = MakeObjectRef(object, object->GetName());
      }
      if (KX_GameObject *object = m_logicManager->ResolveObjectHandle(event->target)) {
        state.cached_result.ui_target_object = MakeObjectRef(object, object->GetName());
      }
      if (event->item.exists) {
        state.cached_result.ui_item = std::make_shared<LN_Value>(event->item);
      }
      if (event->target_item.exists) {
        state.cached_result.ui_target_item = std::make_shared<LN_Value>(event->target_item);
      }
      state.cached_result.ui_index = event->index;
      state.cached_result.ui_amount = event->amount;
      state.cached_result.ui_target_index = event->target_index;
      state.cached_result.ui_user = event->user;
      state.cached_result.ui_accepted = event->accepted;
      break;
    }
    case LN_QueryExpressionKind::ObjectScrollView:
    case LN_QueryExpressionKind::ObjectGridView: {
      if (m_logicManager == nullptr) {
        state.cached_result.physics_query.diagnostic_status =
            LN_QueryDiagnosticStatus::InvalidTarget;
        break;
      }
      const LN_UIService::ViewState &view = m_logicManager->GetUIService().GetViewState(
          {this, expression_index});
      state.cached_result.valid = view.valid;
      state.cached_result.ui_scrolled = view.scrolled;
      state.cached_result.ui_scroll_at_start = view.at_start;
      state.cached_result.ui_scroll_at_end = view.at_end;
      state.cached_result.ui_items_rebound = view.items_rebound;
      state.cached_result.ui_first_visible_index = view.first_visible_index;
      state.cached_result.ui_last_visible_index = view.last_visible_index;
      state.cached_result.ui_page = view.page;
      state.cached_result.ui_page_count = view.page_count;
      state.cached_result.ui_scroll_offset = view.offset;
      state.cached_result.ui_scroll_normalized = view.normalized;
      state.cached_result.physics_query.diagnostic_status =
          view.valid ? LN_QueryDiagnosticStatus::Hit : LN_QueryDiagnosticStatus::InvalidTarget;
      break;
    }
    case LN_QueryExpressionKind::MouseOver: {
      KX_GameObject *target_object = nullptr;
      if (expression.input0 == LN_INVALID_INDEX) {
        target_object = m_gameObject;
      }
      else {
        const LN_Value target_value = ResolveValueExpression(
            expression.input0, MakeNoneValue(), context);
        target_object = ResolveObjectValue(target_value);
      }

      LN_PhysicsQueryResult hit_result;
      bool is_over = false;
      if (target_object != nullptr) {
        FlushPendingRaycastTransforms(target_object, context);
        KX_Scene *scene = m_gameObject ? m_gameObject->GetScene() : nullptr;
        const bool target_is_overlay = scene != nullptr &&
                                       scene->IsOverlayRuntimeObject(target_object);
        const LN_InteractionDomain target_domain = target_is_overlay ?
                                                       LN_InteractionDomain::Overlay :
                                                       LN_InteractionDomain::Main;

        auto cast_target = [&](KX_Camera *camera,
                               const LN_InteractionDomain domain) -> LN_PhysicsQueryResult {
          LN_PhysicsQueryResult result;
          if (camera == nullptr || camera->GetScene() != scene) {
            result.diagnostic_status = LN_QueryDiagnosticStatus::InvalidTarget;
            return result;
          }
          FlushPendingRaycastTransforms(camera, context);
          const float distance = std::max(camera->GetCameraFar(), 0.0f);
          MT_Vector3 from;
          MT_Vector3 to;
          if (!ComputeMouseRay(camera, distance, from, to)) {
            result.diagnostic_status = LN_QueryDiagnosticStatus::InvalidTarget;
            return result;
          }
          const LN_RuntimeRef camera_ref = MakeObjectRef(camera, camera->GetName());
          return RaycastMouseOverTarget(
              from, to, target_object, camera, camera_ref, domain, context.command_execution_key);
        };

        if (expression.interaction_view == LN_InteractionView::Automatic && !target_is_overlay &&
            scene != nullptr && scene->GetOverlayCamera() != nullptr)
        {
          KX_Camera *overlay_camera = scene->GetOverlayCamera();
          FlushPendingRaycastTransforms(overlay_camera, context);
          const float distance = std::max(overlay_camera->GetCameraFar(), 0.0f);
          MT_Vector3 from;
          MT_Vector3 to;
          if (ComputeMouseRay(overlay_camera, distance, from, to)) {
            const LN_RuntimeRef camera_ref = MakeObjectRef(overlay_camera,
                                                           overlay_camera->GetName());
            LN_PhysicsQueryResult overlay_blocker = RaycastPointer(from,
                                                                   to,
                                                                   overlay_camera,
                                                                   camera_ref,
                                                                   0,
                                                                   std::string(),
                                                                   false,
                                                                   LN_InteractionDomain::Overlay,
                                                                   context.command_execution_key);
            if (overlay_blocker.hit || overlay_blocker.blocked) {
              overlay_blocker.hit = false;
              overlay_blocker.blocked = true;
              overlay_blocker.object_ref = LN_RuntimeRef();
              hit_result = std::move(overlay_blocker);
            }
          }
        }

        if (!hit_result.blocked) {
          KX_Camera *camera = nullptr;
          LN_InteractionDomain domain = target_domain;
          switch (expression.interaction_view) {
            case LN_InteractionView::Automatic:
              camera = target_is_overlay ? (scene ? scene->GetOverlayCamera() : nullptr) :
                                           PointerWorldCamera(scene, target_object);
              break;
            case LN_InteractionView::Main:
              camera = PointerWorldCamera(scene, target_object);
              domain = LN_InteractionDomain::Main;
              break;
            case LN_InteractionView::Overlay:
              camera = scene ? scene->GetOverlayCamera() : nullptr;
              domain = LN_InteractionDomain::Overlay;
              break;
            case LN_InteractionView::CustomCamera:
              camera = expression.input1 != LN_INVALID_INDEX ?
                           ResolveCameraValue(ResolveValueExpression(
                               expression.input1, MakeNoneValue(), context)) :
                           nullptr;
              break;
          }
          hit_result = cast_target(camera, domain);
        }
        is_over = hit_result.hit;
      }
      else {
        hit_result.diagnostic_status = LN_QueryDiagnosticStatus::InvalidTarget;
      }

      const size_t event_index = std::min<size_t>(size_t(context.event),
                                                  state.hover_by_event.size() - 1);
      QueryExpressionRuntimeState::HoverRuntimeState &hover = state.hover_by_event[event_index];
      if (hover.baseline_tick != context.tick_index) {
        hover.baseline_tick = context.tick_index;
        hover.baseline_initialized = hover.initialized;
        hover.baseline_previous = hover.previous;
      }
      state.cached_result.over = is_over;
      state.cached_result.entered = is_over &&
                                    (!hover.baseline_initialized || !hover.baseline_previous);
      state.cached_result.exited = !is_over && hover.baseline_initialized &&
                                   hover.baseline_previous;
      state.cached_result.physics_query = hit_result;
      hover.initialized = true;
      hover.previous = is_over;
      break;
    }
    case LN_QueryExpressionKind::ProjectileRay: {
      const LN_Value caster_value = ResolveValueExpression(
          expression.input0, MakeNoneValue(), context);
      KX_GameObject *caster_object = ResolveObjectValue(caster_value);
      LN_RuntimeRef caster_ref;
      if (caster_object != nullptr) {
        caster_ref = MakeObjectRef(caster_object, caster_object->GetName());
      }

      MT_Vector3 origin = ResolveVectorExpression(
          expression.input1, expression.vector_value, context);
      MT_Vector3 aim = ResolveVectorExpression(
          expression.input2, expression.secondary_vector_value, context);
      const bool local_space = ResolveBoolExpression(
          expression.bool_expr_index, expression.bool_value, context);
      if (local_space && caster_object != nullptr) {
        origin = caster_object->NodeGetWorldPosition() +
                 caster_object->NodeGetWorldOrientation() * origin;
        aim = caster_object->NodeGetWorldOrientation() * aim;
      }
      else {
        aim -= origin;
      }
      if (aim.length2() > 0.0f) {
        aim.normalize();
      }
      else {
        state.cached_result.physics_query.diagnostic_status =
            LN_QueryDiagnosticStatus::InvalidTarget;
        break;
      }
      const float power = ResolveFloatExpression(
          expression.float_expr_index, expression.float_value, context);
      aim *= power;

      MT_Vector3 gravity(0.0f, 0.0f, -9.8f);
      if (KX_Scene *scene = m_gameObject ? m_gameObject->GetScene() : nullptr) {
        gravity = scene->GetGravity();
      }

      const std::string property_name = ResolveStringExpression(
          expression.string_expr_index, std::string(), context);
      const bool xray = ResolveBoolExpression(
          expression.secondary_bool_expr_index, false, context);
      const int32_t mask_value = ResolveIntExpression(
          expression.int_expr_index, expression.int_value, context);
      if (mask_value < 0 || mask_value > LN_RAYCAST_MAX_COLLISION_MASK) {
        state.cached_result.physics_query.diagnostic_status =
            LN_QueryDiagnosticStatus::InvalidFilter;
        break;
      }
      const uint32_t collision_mask = uint32_t(mask_value);
      const bool visualize = ResolveBoolExpression(
          expression.tertiary_bool_expr_index, false, context);

      const float distance = std::max(
          0.0f, ResolveFloatExpression(expression.secondary_float_expr_index, 100.0f, context));
      const float resolution = std::clamp(
          ResolveFloatExpression(expression.tertiary_float_expr_index, 0.9f, context),
          0.0f,
          0.99f);
      const float time_step = std::max(0.01f, 1.0f - resolution);
      float total_distance = 0.0f;
      float time_index = time_step;
      std::vector<MT_Vector3> points;
      points.push_back(origin);

      LN_PhysicsQueryResult hit_result;
      for (int step = 0; step < 2048 && total_distance < distance; step++) {
        const MT_Vector3 target = ProjectilePoint(aim, origin, gravity, time_index);
        const MT_Vector3 start = points.back();
        const MT_Vector3 segment = target - start;
        const float segment_distance = segment.length();
        if (segment_distance <= 1.0e-6f) {
          break;
        }

        /* Distance limits the arc length, not merely which sample starts the
         * final ray. Clip the last segment so a coarse resolution cannot hit
         * an object beyond the requested range. */
        const float remaining_distance = distance - total_distance;
        const float ray_distance = std::min(segment_distance, remaining_distance);
        const MT_Vector3 segment_end = ray_distance < segment_distance ?
                                           start + segment * (ray_distance / segment_distance) :
                                           target;
        hit_result = Raycast(start, segment_end, caster_ref, collision_mask, property_name, xray);
        total_distance += ray_distance;
        const MT_Vector3 end = hit_result.hit ? hit_result.hit_position : segment_end;
        if (visualize) {
          DrawDebugRaycast(start,
                           end,
                           hit_result.hit,
                           hit_result.hit ? &hit_result.hit_normal : nullptr,
                           ray_distance);
        }
        if (!hit_result.hit) {
          points.push_back(segment_end);
        }
        else {
          points.push_back(hit_result.hit_position);
          break;
        }
        time_index += time_step;
      }

      hit_result.parabola_points = points;
      state.cached_result.physics_query = hit_result;
      break;
    }
    case LN_QueryExpressionKind::VectorAverage: {
      const LN_Value source = ResolveValueExpression(expression.input0, MakeNoneValue(), context);
      double sum[3] = {0.0, 0.0, 0.0};
      uint64_t count = 0;

      switch (expression.average_center_source) {
        case LN_AverageCenterSource::VectorList:
          if (source.exists && source.type == LN_ValueType::List) {
            for (const LN_Value &item : source.list_value) {
              if (item.exists && item.type == LN_ValueType::Vector) {
                accumulate_finite_position(item.vector_value, sum, count);
              }
            }
          }
          break;
        case LN_AverageCenterSource::ObjectList:
          if (source.exists && source.type == LN_ValueType::List) {
            for (const LN_Value &item : source.list_value) {
              if (KX_GameObject *game_object = ResolveObjectValue(item)) {
                accumulate_finite_position(game_object->NodeGetWorldPosition(), sum, count);
              }
            }
          }
          break;
        case LN_AverageCenterSource::Collection: {
          blender::Collection *collection = source.exists ?
                                                ResolveCollectionByValue(this, source) :
                                                nullptr;
          KX_Scene *scene = m_gameObject != nullptr ? m_gameObject->GetScene() : nullptr;
          if (collection != nullptr && scene != nullptr) {
            for (blender::CollectionObject &collection_object : collection->gobject) {
              if (collection_object.ob == nullptr) {
                continue;
              }
              if (KX_GameObject *game_object = scene->GetGameObjectFromObject(
                      collection_object.ob))
              {
                accumulate_finite_position(game_object->NodeGetWorldPosition(), sum, count);
              }
            }
          }
          break;
        }
        default:
          break;
      }
      if (count == 0) {
        break;
      }

      const double inverse_count = 1.0 / double(count);
      const MT_Vector3 average(float(sum[0] * inverse_count),
                               float(sum[1] * inverse_count),
                               float(sum[2] * inverse_count));
      if (!vector_is_finite(average)) {
        break;
      }
      state.cached_result.vector_average = average;
      state.cached_result.vector_count = int32_t(
          std::min(count, uint64_t(std::numeric_limits<int32_t>::max())));
      state.cached_result.valid = true;
      break;
    }
    case LN_QueryExpressionKind::DistanceBetween: {
      auto resolve_endpoint = [&](const LN_DistanceEndpointMode mode,
                                  const uint32_t input,
                                  const bool owner_when_empty,
                                  MT_Vector3 &r_position) {
        if (mode == LN_DistanceEndpointMode::Position) {
          if (input == LN_INVALID_INDEX) {
            return false;
          }
          r_position = ResolveVectorExpression(input, MT_Vector3(0.0f, 0.0f, 0.0f), context);
          return vector_is_finite(r_position);
        }
        if (mode != LN_DistanceEndpointMode::Object) {
          return false;
        }

        KX_GameObject *object = nullptr;
        if (input != LN_INVALID_INDEX) {
          object = ResolveObjectValue(ResolveValueExpression(input, MakeNoneValue(), context));
        }
        else if (owner_when_empty) {
          object = m_gameObject;
        }
        if (object == nullptr) {
          return false;
        }
        FlushPendingRaycastTransforms(object, context);
        r_position = object->NodeGetWorldPosition();
        return vector_is_finite(r_position);
      };

      MT_Vector3 a(0.0f, 0.0f, 0.0f);
      MT_Vector3 b(0.0f, 0.0f, 0.0f);
      if (!resolve_endpoint(expression.distance_endpoint_a, expression.input0, true, a) ||
          !resolve_endpoint(expression.distance_endpoint_b, expression.input1, false, b))
      {
        break;
      }

      const double dx = double(a.x()) - double(b.x());
      const double dy = double(a.y()) - double(b.y());
      const double dz = double(a.z()) - double(b.z());
      const double squared_distance = dx * dx + dy * dy + dz * dz;
      if (!std::isfinite(squared_distance) || squared_distance < 0.0) {
        break;
      }
      const double max_float = double(std::numeric_limits<float>::max());
      state.cached_result.squared_distance = float(std::min(squared_distance, max_float));
      state.cached_result.distance = float(std::min(std::sqrt(squared_distance), max_float));
      state.cached_result.valid = true;
      break;
    }
    case LN_QueryExpressionKind::Count:
      break;
  }

  return state.cached_result;
}

LN_RuntimeTree::CollisionEventPayload *LN_RuntimeTree::FindCollisionEventPayload(
    KX_GameObject *owner, const std::string &property_filter, const std::string &material_filter)
{
  /* No object (a missing one) has no events; a removed owner's payload also resolves to null. */
  if (owner == nullptr) {
    return nullptr;
  }
  for (CollisionEventPayload &payload : m_collisionEventPayloads) {
    const CollisionResult &result = payload.last_result;
    if (ResolveCollisionPayloadObjectRef(payload.owner_ref) == owner &&
        result.property_filter == property_filter && result.material_filter == material_filter)
    {
      return &payload;
    }
  }
  return nullptr;
}

void LN_RuntimeTree::StoreCollisionEventPayload(const CollisionResult &result,
                                                const uint64_t tick_index)
{
  if (!result.hit || result.owner == nullptr ||
      result.detail == CollisionResult::Detail::StateOnly)
  {
    return;
  }

  CollisionEventPayload *payload = FindCollisionEventPayload(
      result.owner, result.property_filter, result.material_filter);
  if (payload == nullptr) {
    m_collisionEventPayloads.push_back(CollisionEventPayload{});
    payload = &m_collisionEventPayloads.back();
  }

  if (ResolveCollisionPayloadObjectRef(payload->owner_ref) != result.owner) {
    payload->owner_ref = MakeObjectRef(result.owner, "collision event owner");
  }
  if (ResolveCollisionPayloadObjectRef(payload->hit_object_ref) != result.hit_object) {
    payload->hit_object_ref = MakeObjectRef(result.hit_object, "collision event object");
  }

  size_t valid_hit_object_count = 0;
  bool hit_object_refs_match = true;
  for (KX_GameObject *object : result.hit_objects) {
    if (object == nullptr) {
      continue;
    }
    if (valid_hit_object_count >= payload->hit_object_refs.size() ||
        ResolveCollisionPayloadObjectRef(payload->hit_object_refs[valid_hit_object_count]) !=
            object)
    {
      hit_object_refs_match = false;
      break;
    }
    valid_hit_object_count++;
  }
  hit_object_refs_match = hit_object_refs_match &&
                          valid_hit_object_count == payload->hit_object_refs.size();
  if (!hit_object_refs_match) {
    payload->hit_object_refs.clear();
    payload->hit_object_refs.reserve(result.hit_objects.size());
    for (KX_GameObject *object : result.hit_objects) {
      if (object == nullptr) {
        continue;
      }
      payload->hit_object_refs.push_back(object == result.hit_object ?
                                             payload->hit_object_ref :
                                             MakeObjectRef(object, "collision event object"));
    }
  }

  payload->last_result = result;
  payload->last_result.owner = nullptr;
  payload->last_result.hit_object = nullptr;
  payload->last_result.hit_objects.clear();
  payload->last_hit_tick = tick_index;
}

void LN_RuntimeTree::MarkCollisionExitPayload(const CollisionResult &current_query,
                                              const uint64_t tick_index)
{
  CollisionEventPayload *payload = FindCollisionEventPayload(
      current_query.owner, current_query.property_filter, current_query.material_filter);
  if (payload != nullptr && payload->last_hit_tick != UINT64_MAX) {
    payload->exit_tick = tick_index;
  }
}

const LN_RuntimeTree::CollisionResult *LN_RuntimeTree::FindCollisionExitPayload(
    KX_GameObject *owner,
    const std::string &property_filter,
    const std::string &material_filter,
    const uint64_t tick_index)
{
  CollisionEventPayload *payload = FindCollisionEventPayload(
      owner, property_filter, material_filter);
  if (payload == nullptr || payload->exit_tick != tick_index) {
    return nullptr;
  }

  m_collisionExitPayloadScratch = payload->last_result;
  m_collisionExitPayloadScratch.owner = ResolveCollisionPayloadObjectRef(payload->owner_ref);
  m_collisionExitPayloadScratch.hit_object = ResolveCollisionPayloadObjectRef(
      payload->hit_object_ref);
  m_collisionExitPayloadScratch.hit_objects.clear();
  m_collisionExitPayloadScratch.hit_objects.reserve(payload->hit_object_refs.size());
  for (const LN_RuntimeRef &object_ref : payload->hit_object_refs) {
    if (KX_GameObject *object = ResolveCollisionPayloadObjectRef(object_ref)) {
      m_collisionExitPayloadScratch.hit_objects.push_back(object);
    }
  }
  if (m_collisionExitPayloadScratch.hit_object == nullptr &&
      !m_collisionExitPayloadScratch.hit_objects.empty())
  {
    m_collisionExitPayloadScratch.hit_object = m_collisionExitPayloadScratch.hit_objects.front();
  }
  return &m_collisionExitPayloadScratch;
}

const LN_RuntimeTree::CollisionResult &LN_RuntimeTree::ResolveCollisionResult(
    const uint32_t object_expr_index,
    const uint32_t property_expr_index,
    const uint32_t material_expr_index,
    const LN_TickContext &context,
    const CollisionResult::Detail detail,
    const bool allow_exit_payload)
{
  /* No Object input: the owner. A linked or picked object that is gone (or a link that gives no
   * object) collides with nothing, not with the owner. */
  KX_GameObject *owner = m_gameObject;
  if (object_expr_index != LN_INVALID_INDEX) {
    owner = ResolveObjectValue(
        ResolveValueExpression(object_expr_index, MakeNoneValue(), context));
  }

  const std::string property_filter = ResolveStringExpression(
      property_expr_index, std::string(), context);
  const std::string material_filter =
      material_expr_index != LN_INVALID_INDEX ?
          MaterialFilterNameFromValue(
              this, ResolveValueExpression(material_expr_index, MakeNoneValue(), context)) :
          std::string();

  if (m_collisionResultsTick != context.tick_index) {
    m_collisionResults.clear();
    m_collisionResultsTick = context.tick_index;
  }

  for (auto it = m_collisionResults.begin(); it != m_collisionResults.end(); ++it) {
    CollisionResult &cached_result = *it;
    if (cached_result.owner != owner || cached_result.property_filter != property_filter ||
        cached_result.material_filter != material_filter)
    {
      continue;
    }
    if (uint8_t(cached_result.detail) >= uint8_t(detail)) {
      if (allow_exit_payload && !cached_result.hit && detail != CollisionResult::Detail::StateOnly)
      {
        if (const CollisionResult *exit_payload = FindCollisionExitPayload(
                owner, property_filter, material_filter, context.tick_index))
        {
          cached_result = *exit_payload;
          cached_result.cached_tick = context.tick_index;
          if (uint8_t(cached_result.detail) < uint8_t(detail)) {
            cached_result.detail = detail;
          }
        }
      }
      return cached_result;
    }
    m_collisionResults.erase(it);
    break;
  }

  CollisionResult result;
  result.cached_tick = context.tick_index;
  result.owner = owner;
  result.property_filter = property_filter;
  result.material_filter = material_filter;
  result.detail = detail;
  m_collisionResults.push_back(std::move(result));
  CollisionResult &collision_result = m_collisionResults.back();

  if (owner == nullptr) {
    return collision_result;
  }

  KX_Scene *scene = owner->GetScene();
  if (scene == nullptr) {
    return collision_result;
  }
  PHY_IPhysicsEnvironment *env = scene->GetPhysicsEnvironment();
  if (env == nullptr) {
    return collision_result;
  }
  PHY_IPhysicsController *ctrl_a = owner->GetPhysicsController();

  const bool collect_contacts = detail == CollisionResult::Detail::Contacts;
  const std::vector<const PHY_CachedCollisionContact *> *cached_contacts = nullptr;
  const bool has_cached_contacts =
      env->GetCachedCollisionContactRefsForObject(owner, cached_contacts, collect_contacts) ||
      (ctrl_a != nullptr &&
       env->GetCachedCollisionContactRefs(ctrl_a, cached_contacts, collect_contacts));
  if (has_cached_contacts) {
    for (const PHY_CachedCollisionContact *contact_ptr : *cached_contacts) {
      if (contact_ptr == nullptr) {
        continue;
      }
      const PHY_CachedCollisionContact &contact = *contact_ptr;
      KX_GameObject *other = nullptr;
      bool owner_is_first = true;
      if (contact.object0 == owner) {
        other = contact.object1;
      }
      else if (contact.object1 == owner) {
        other = contact.object0;
        owner_is_first = false;
      }
      else {
        continue;
      }
      if (other == nullptr || other == owner) {
        continue;
      }
      if (!property_filter.empty() && other->GetProperty(property_filter) == nullptr) {
        continue;
      }
      if (!GameObjectUsesMaterial(other, material_filter)) {
        continue;
      }

      collision_result.hit = true;
      if (collision_result.hit_object == nullptr) {
        collision_result.hit_object = other;
      }
      if (std::find(collision_result.hit_objects.begin(),
                    collision_result.hit_objects.end(),
                    other) == collision_result.hit_objects.end())
      {
        collision_result.hit_objects.push_back(other);
      }
      collision_result.contact_count += std::max(contact.contact_count, 1);

      if (collect_contacts) {
        const size_t contact_detail_count = std::min(contact.points.size(),
                                                     contact.normals.size());
        for (size_t contact_index = 0; contact_index < contact_detail_count; contact_index++) {
          const MT_Vector3 point = contact.points[contact_index];
          const MT_Vector3 normal = owner_is_first ? contact.normals[contact_index] :
                                                     -contact.normals[contact_index];
          if (collision_result.hit_points.empty()) {
            collision_result.hit_point = point;
          }
          if (collision_result.hit_normals.empty()) {
            collision_result.hit_normal = normal;
          }
          collision_result.hit_points.push_back(point);
          collision_result.hit_normals.push_back(normal);
        }
      }

      if (detail == CollisionResult::Detail::StateOnly) {
        return collision_result;
      }
    }

    if (collision_result.hit) {
      StoreCollisionEventPayload(collision_result, context.tick_index);
    }
    else if (allow_exit_payload && detail != CollisionResult::Detail::StateOnly) {
      if (const CollisionResult *exit_payload = FindCollisionExitPayload(
              owner, property_filter, material_filter, context.tick_index))
      {
        collision_result = *exit_payload;
        collision_result.cached_tick = context.tick_index;
        if (uint8_t(collision_result.detail) < uint8_t(detail)) {
          collision_result.detail = detail;
        }
      }
    }

    return collision_result;
  }

  if (ctrl_a == nullptr) {
    return collision_result;
  }

  for (KX_GameObject *other : scene->GetObjectList()) {
    if (other == nullptr || other == owner) {
      continue;
    }
    if (!property_filter.empty() && other->GetProperty(property_filter) == nullptr) {
      continue;
    }
    if (!GameObjectUsesMaterial(other, material_filter)) {
      continue;
    }
    PHY_IPhysicsController *ctrl_b = other->GetPhysicsController();
    if (ctrl_b == nullptr) {
      continue;
    }
    const PHY_CollisionTestResult hit = env->CheckCollision(ctrl_a, ctrl_b, collect_contacts);
    if (hit.collide) {
      collision_result.hit = true;
      if (collision_result.hit_object == nullptr) {
        collision_result.hit_object = other;
      }
      collision_result.hit_objects.push_back(other);

      int32_t pair_contact_count = 0;
      if (hit.collData && hit.collData->GetNumContacts() > 0) {
        pair_contact_count = int32_t(hit.collData->GetNumContacts());
        for (unsigned int contact_index = 0; contact_index < hit.collData->GetNumContacts();
             contact_index++)
        {
          const MT_Vector3 point = hit.collData->GetWorldPoint(contact_index, hit.isFirst);
          const MT_Vector3 normal = hit.collData->GetNormal(contact_index, hit.isFirst);
          if (collision_result.hit_points.empty()) {
            collision_result.hit_point = point;
          }
          if (collision_result.hit_normals.empty()) {
            collision_result.hit_normal = normal;
          }
          collision_result.hit_points.push_back(point);
          collision_result.hit_normals.push_back(normal);
        }
      }
      collision_result.contact_count += (detail == CollisionResult::Detail::Contacts) ?
                                            std::max(pair_contact_count, 1) :
                                            1;
      delete hit.collData;
      if (detail == CollisionResult::Detail::StateOnly) {
        return collision_result;
      }
    }
    else {
      delete hit.collData;
    }
  }

  if (collision_result.hit) {
    StoreCollisionEventPayload(collision_result, context.tick_index);
  }
  else if (allow_exit_payload && detail != CollisionResult::Detail::StateOnly) {
    if (const CollisionResult *exit_payload = FindCollisionExitPayload(
            owner, property_filter, material_filter, context.tick_index))
    {
      collision_result = *exit_payload;
      collision_result.cached_tick = context.tick_index;
      if (uint8_t(collision_result.detail) < uint8_t(detail)) {
        collision_result.detail = detail;
      }
    }
  }

  return collision_result;
}

bool LN_RuntimeTree::ResolveBoolExpression(const uint32_t expression_index,
                                           const bool fallback,
                                           const LN_TickContext &context)
{
  if (expression_index == LN_INVALID_INDEX || m_program == nullptr) {
    return fallback;
  }

  const std::vector<LN_BoolExpression> &expressions = m_program->GetBoolExpressions();
  if (expression_index >= expressions.size()) {
    return fallback;
  }

  const LN_BoolExpression &expression = expressions[expression_index];
  const LN_RuntimeExpressionSemantics semantics = LN_GetRuntimeExpressionSemantics(
      expression.kind);
  ProfileExpressionEvaluation(semantics.fallback_requirements);
  RecordMissingSnapshotChannelsForExpression(semantics);
  bool register_bool_value = false;
  if (TryResolveRegisterBoolExpression(expression_index, context, register_bool_value)) {
    return register_bool_value;
  }
  if (context.use_register_expression_evaluator) {
    RecordRegisterExpressionFallback(LN_RuntimeExpressionFamily::Bool, expression_index);
  }
  switch (expression.kind) {
    case LN_BoolExpressionKind::Constant:
      return expression.bool_value;
    case LN_BoolExpressionKind::GamePaused:
      return context.game_paused;
    case LN_BoolExpressionKind::SnapshotGameProperty:
      if (const LN_Value *value = m_snapshot.GetGamePropertyValue(expression.property_ref_index)) {
        return value->bool_value;
      }
      return fallback;
    case LN_BoolExpressionKind::RuntimeTreeProperty:
      if (const LN_Value *value = GetTreePropertyValue(expression.property_ref_index)) {
        return value->bool_value;
      }
      return fallback;
    case LN_BoolExpressionKind::Not:
      return !ResolveBoolExpression(expression.input0, false, context);
    case LN_BoolExpressionKind::And:
      return ResolveBoolExpression(expression.input0, false, context) &&
             ResolveBoolExpression(expression.input1, false, context);
    case LN_BoolExpressionKind::Or:
      return ResolveBoolExpression(expression.input0, false, context) ||
             ResolveBoolExpression(expression.input1, false, context);
    case LN_BoolExpressionKind::FloatCompare: {
      const float a = ResolveFloatExpression(expression.input0, 0.0f, context);
      const float b = ResolveFloatExpression(expression.input1, 0.0f, context);
      switch (expression.float_compare_operation) {
        case LN_FloatCompareOperation::Equal:
          return std::fabs(a - b) <= 1.0e-6f;
        case LN_FloatCompareOperation::NotEqual:
          return std::fabs(a - b) > 1.0e-6f;
        case LN_FloatCompareOperation::GreaterThan:
          return a > b;
        case LN_FloatCompareOperation::LessThan:
          return a < b;
        case LN_FloatCompareOperation::GreaterEqual:
          return a >= b;
        case LN_FloatCompareOperation::LessEqual:
          return a <= b;
      }
      break;
    }
    case LN_BoolExpressionKind::StringContains:
      return ResolveStringExpression(expression.input0, "", context)
                 .find(ResolveStringExpression(expression.input1, "", context)) !=
             std::string::npos;
    case LN_BoolExpressionKind::StringStartsWith:
      return StringStartsWith(ResolveStringExpression(expression.input0, "", context),
                              ResolveStringExpression(expression.input1, "", context));
    case LN_BoolExpressionKind::StringEndsWith:
      return StringEndsWith(ResolveStringExpression(expression.input0, "", context),
                            ResolveStringExpression(expression.input1, "", context));
    case LN_BoolExpressionKind::SnapshotVisibility: {
      LN_Snapshot snapshot;
      const LN_ObjectSnapshot *object_snapshot = &m_snapshot.GetObjectSnapshot();
      if (expression.input0 != LN_INVALID_INDEX) {
        if (KX_GameObject *game_object = ResolveObjectValue(
                ResolveValueExpression(expression.input0, MakeNoneValue(), context)))
        {
          snapshot.Capture(game_object, nullptr);
          object_snapshot = &snapshot.GetObjectSnapshot();
        }
        else {
          return fallback;
        }
      }
      return object_snapshot->visible;
    }
    case LN_BoolExpressionKind::SnapshotCharacterOnGround: {
      LN_Snapshot snapshot;
      const LN_ObjectSnapshot *object_snapshot = &m_snapshot.GetObjectSnapshot();
      if (expression.input0 != LN_INVALID_INDEX) {
        if (KX_GameObject *game_object = ResolveObjectValue(
                ResolveValueExpression(expression.input0, MakeNoneValue(), context)))
        {
          snapshot.Capture(game_object, nullptr);
          object_snapshot = &snapshot.GetObjectSnapshot();
        }
        else {
          return fallback;
        }
      }
      return object_snapshot->has_character ? object_snapshot->character_on_ground : fallback;
    }
    case LN_BoolExpressionKind::WindowFullscreen:
      if (RAS_ICanvas *canvas = ActiveCanvas()) {
        return canvas->GetFullScreen();
      }
      return fallback;
    case LN_BoolExpressionKind::SnapshotGamePropertyExists:
      if (const LN_Value *value = m_snapshot.GetGamePropertyValue(expression.property_ref_index)) {
        return value->exists;
      }
      return false;
    case LN_BoolExpressionKind::ValueIsNone: {
      const LN_Value value = ResolveValueExpression(expression.input0, MakeNoneValue(), context);
      return !value.exists || value.type == LN_ValueType::None;
    }
    case LN_BoolExpressionKind::FromGenericValue: {
      const LN_Value value = ResolveValueExpression(expression.input0, MakeNoneValue(), context);
      return ValueAsBool(value);
    }
    case LN_BoolExpressionKind::FromInt:
      return ResolveIntExpression(expression.input0, 0, context) != 0;
    case LN_BoolExpressionKind::FromFloat:
      return ResolveFloatExpression(expression.input0, 0.0f, context) != 0.0f;
    case LN_BoolExpressionKind::FromString:
      return !ResolveStringExpression(expression.input0, std::string(), context).empty();
    case LN_BoolExpressionKind::FromVector:
      return ResolveVectorExpression(expression.input0, MT_Vector3(0.0f, 0.0f, 0.0f), context)
                 .length2() > 0.0f;
    case LN_BoolExpressionKind::FromColor:
      return ResolveColorExpression(expression.input0, MT_Vector4(0.0f, 0.0f, 0.0f, 0.0f), context)
                 .length2() > 0.0f;
    case LN_BoolExpressionKind::RigidBodyAttribute: {
      PHY_IPhysicsController *controller = ResolveRigidBodyAttributeController(expression.input0,
                                                                               context);
      if (controller == nullptr) {
        return false;
      }
      return ReadRigidBodyBoolAttribute(*controller,
                                        static_cast<LN_RigidBodyAttribute>(expression.int_value),
                                        expression.secondary_int_value);
    }
    case LN_BoolExpressionKind::RigidBodyConstraintFound: {
      KX_GameObject *target = m_gameObject;
      if (expression.input0 != LN_INVALID_INDEX) {
        target = ResolveObjectValue(
            ResolveValueExpression(expression.input0, MakeNoneValue(), context));
      }
      if (!UsesJoltPhysics(target)) {
        return false;
      }
      const LN_RigidBodyConstraintMatchMode match_mode = ValidRigidBodyConstraintMatchMode(
          expression.rigid_body_constraint_match_mode);
      if (match_mode == LN_RigidBodyConstraintMatchMode::All) {
        return target->HasRigidBodyConstraints();
      }
      const std::string query = ResolveStringExpression(
          expression.property_ref_index, std::string(), context);
      return FindRigidBodyConstraintMatch(*target, query, match_mode) != nullptr;
    }
    case LN_BoolExpressionKind::ValueCompare: {
      const LN_Value a = ResolveValueExpression(expression.input0, MakeNoneValue(), context);
      const LN_Value b = ResolveValueExpression(expression.input1, MakeNoneValue(), context);
      return CompareValues(a, b, expression.float_compare_operation);
    }
    case LN_BoolExpressionKind::MaterialSlotFound: {
      KX_GameObject *target_object = m_gameObject;
      if (expression.input0 != LN_INVALID_INDEX) {
        target_object = ResolveObjectValue(
            ResolveValueExpression(expression.input0, MakeNoneValue(), context));
      }
      const int32_t slot = ResolveIntExpression(expression.input1, 0, context);
      return ObjectMaterialAtSlot(target_object, slot) != nullptr;
    }
    case LN_BoolExpressionKind::MaterialNodeValueFound: {
      blender::Material *material = nullptr;
      const bool object_slot_target = ELEM(expression.property_ref_index, 1u, 2u, 4u);
      if (object_slot_target) {
        KX_GameObject *target_object = m_gameObject;
        if (expression.input0 != LN_INVALID_INDEX) {
          target_object = ResolveObjectValue(
              ResolveValueExpression(expression.input0, MakeNoneValue(), context));
        }
        const int32_t slot = ResolveIntExpression(expression.input1, 0, context);
        material = ObjectMaterialAtSlot(target_object, slot);
      }
      else {
        material = ResolveMaterialByValue(
            this, ResolveValueExpression(expression.input0, MakeNoneValue(), context));
      }

      const std::string node_name = ResolveStringExpression(
          expression.input2, std::string(), context);
      const std::string socket_name = ResolveStringExpression(
          expression.int_expr_index, std::string(), context);
      if (material == nullptr || material->nodetree == nullptr || node_name.empty() ||
          socket_name.empty())
      {
        return false;
      }
      blender::bNode *node = FindNodeByUserName(material->nodetree, node_name);
      blender::bNodeSocket *socket = FindInputSocketByIdentifierOrName(node, socket_name);
      if (socket == nullptr) {
        return false;
      }
      if (expression.property_ref_index >= 3) {
        material->nodetree->ensure_topology_cache();
        if ((socket->flag & blender::SOCK_UNAVAIL) != 0 || socket->is_directly_linked()) {
          return false;
        }
      }
      if (expression.property_ref_index == 2) {
        return upbge::logic_nodes::game_state::material_parameter_socket_supports_object_attribute(
            *socket);
      }
      return ReadSocketDefaultValue(*socket, this).exists;
    }
    case LN_BoolExpressionKind::EditorNodeValueFound: {
      KX_GameObject *target_object = m_gameObject;
      if (expression.input0 != LN_INVALID_INDEX) {
        target_object = ResolveObjectValue(
            ResolveValueExpression(expression.input0, MakeNoneValue(), context));
      }
      const std::string target_name = ResolveStringExpression(
          expression.input1, std::string(), context);
      blender::bNodeTree *tree = ResolveEditorNodeValueTree(
          EditorNodeValueTarget(expression.property_ref_index), target_object, target_name);
      const std::string node_name = ResolveStringExpression(
          expression.input2, std::string(), context);
      const std::string socket_name = ResolveStringExpression(
          expression.int_expr_index, std::string(), context);
      blender::bNode *node = FindNodeByUserName(tree, node_name);
      blender::bNodeSocket *socket = FindInputSocketByIdentifierOrName(node, socket_name);
      if (tree == nullptr || socket == nullptr || (socket->flag & blender::SOCK_UNAVAIL) != 0) {
        return false;
      }
      tree->ensure_topology_cache();
      return !socket->is_directly_linked() && ReadSocketDefaultValue(*socket, this).exists;
    }
    case LN_BoolExpressionKind::DictHasKey: {
      const LN_Value dict_value = expression.input0 != LN_INVALID_INDEX ?
                                      ResolveValueExpression(
                                          expression.input0, MakeNoneValue(), context) :
                                      MakeNoneValue();
      const std::string key = expression.input1 != LN_INVALID_INDEX ?
                                  ResolveStringExpression(
                                      expression.input1, std::string(), context) :
                                  std::string();
      if (dict_value.type == LN_ValueType::Dict && dict_value.exists && !key.empty()) {
        return dict_value.dict_value.find(key) != dict_value.dict_value.end();
      }
      return false;
    }
    case LN_BoolExpressionKind::StoreValueDone: {
      /* Evaluated by the route on Store Value's Flow, when the flow arrives: store now. */
      const std::vector<LN_FloatExpression> &float_expressions = m_program->GetFloatExpressions();
      if (expression.float_expr_index >= float_expressions.size() ||
          float_expressions[expression.float_expr_index].kind != LN_FloatExpressionKind::StoreValue)
      {
        return fallback;
      }
      const LN_FloatExpression &store = float_expressions[expression.float_expr_index];
      FloatExpressionRuntimeState &state = FloatExpressionState(expression.float_expr_index);
      state.initialized = true;
      state.stored_value = ResolveFloatExpression(store.input0, store.float_value, context);
      state.last_store_tick = context.tick_index;
      return true;
    }
    case LN_BoolExpressionKind::ListContains: {
      const LN_Value list_value = expression.input0 != LN_INVALID_INDEX ?
                                      ResolveValueExpression(
                                          expression.input0, MakeNoneValue(), context) :
                                      MakeNoneValue();
      const LN_Value needle = expression.input1 != LN_INVALID_INDEX ?
                                  ResolveValueExpression(
                                      expression.input1, MakeNoneValue(), context) :
                                  MakeNoneValue();
      if (list_value.type != LN_ValueType::List || !list_value.exists) {
        return false;
      }
      for (const LN_Value &item : list_value.list_value) {
        if (ValuesEqual(item, needle)) {
          return true;
        }
      }
      return false;
    }
    case LN_BoolExpressionKind::InstructionExecuted: {
      const uint32_t state_index = InstructionStateIndex(context.event, expression.input0);
      if (state_index == LN_INVALID_INDEX || state_index >= m_instructionStates.size()) {
        return false;
      }
      const InstructionRuntimeState &state = m_instructionStates[state_index];
      if (state.last_execution_tick != context.tick_index) {
        return false;
      }
      return context.execution_scope_serial == 0 ||
             state.last_execution_scope_serial == context.execution_scope_serial;
    }
    case LN_BoolExpressionKind::InstructionReached: {
      const uint32_t state_index = InstructionStateIndex(context.event, expression.input0);
      if (state_index == LN_INVALID_INDEX || state_index >= m_instructionStates.size()) {
        return false;
      }
      const InstructionRuntimeState &state = m_instructionStates[state_index];
      if (state.last_reached_tick != context.tick_index) {
        return false;
      }
      return context.execution_scope_serial == 0 ||
             state.last_reached_scope_serial == context.execution_scope_serial;
    }
    case LN_BoolExpressionKind::InputStatus: {
      const int32_t input_code = expression.int_value;
      if (input_code == SCA_IInputDevice::NOKEY) {
        return false;
      }
      /* bool_value: also what the UI used. */
      return (expression.bool_value ? m_snapshot.GetRawInputSnapshot() :
                                      m_snapshot.GetInputSnapshot())
          .GetStatus(input_code, expression.secondary_int_value);
    }
    case LN_BoolExpressionKind::InputActionState: {
      const LN_InputActionState *state = m_snapshot.GetInputActionState(
          LN_InputMap::Get().Find(
              ResolveStringExpression(expression.input1, std::string(), context)),
          ResolveIntExpression(expression.input0, 0, context),
          expression.bool_value);
      if (state == nullptr) {
        return false;
      }
      switch (expression.secondary_int_value) {
        case SCA_InputEvent::JUSTACTIVATED:
          return state->pressed;
        case SCA_InputEvent::JUSTRELEASED:
          return state->released;
        default:
          return state->active;
      }
    }
    case LN_BoolExpressionKind::KeyboardActive:
      return KeyboardInputIsActive(m_snapshot.GetInputSnapshot());
    case LN_BoolExpressionKind::MouseMoved: {
      return EvaluateMouseMoved(expression.bool_value);
    }
    case LN_BoolExpressionKind::MouseWheelMoved: {
      const int32_t wheel_delta = (expression.bool_value ? m_snapshot.GetRawInputSnapshot() :
                                                           m_snapshot.GetInputSnapshot())
                                      .GetMouse()
                                      .wheel_delta;
      if (wheel_delta == 0) {
        return false;
      }
      const int32_t direction = expression.int_value > 0 ? expression.int_value : 3;
      if (direction == 1) {
        return wheel_delta > 0;
      }
      if (direction == 2) {
        return wheel_delta < 0;
      }
      return true;
    }
    case LN_BoolExpressionKind::KeyLoggerPressed:
      return ScanKeyLogger(m_snapshot.GetInputSnapshot(), expression.bool_value).pressed;
    case LN_BoolExpressionKind::GamepadActive: {
      const int32_t gamepad_index = ResolveIntExpression(expression.input0, 0, context);
      const LN_GamepadSnapshot *gamepad = m_snapshot.GetInputSnapshot().GetGamepad(gamepad_index);
      return gamepad != nullptr && LN_GamepadHasActiveInput(*gamepad);
    }
    case LN_BoolExpressionKind::GamepadButton: {
      const int32_t gamepad_index = ResolveIntExpression(expression.input0, 0, context);
      /* secondary_int_value: also what the UI used. */
      const LN_GamepadSnapshot *gamepad = (expression.secondary_int_value != 0 ?
                                               m_snapshot.GetRawInputSnapshot() :
                                               m_snapshot.GetInputSnapshot())
                                              .GetGamepad(gamepad_index);
      if (gamepad == nullptr) {
        return false;
      }
      const int32_t input_status = expression.int_expr_index != LN_INVALID_INDEX ?
                                       ResolveIntExpression(expression.int_expr_index,
                                                            SCA_InputEvent::ACTIVE,
                                                            context) :
                                       SCA_InputEvent::ACTIVE;
      return LN_GetGamepadControlState(
          *gamepad, expression.int_value, expression.bool_value, input_status);
    }
    case LN_BoolExpressionKind::PhysicsQueryDone:
      /* Invalid inputs and an unavailable backend are still completed query attempts. A disabled
       * query, however, did not receive Flow and must not emit an execution pulse. */
      return ResolveQueryExpression(expression.input0, context).physics_query.diagnostic_status !=
             LN_QueryDiagnosticStatus::Disabled;
    case LN_BoolExpressionKind::PhysicsQueryHit:
      return ResolveQueryExpression(expression.input0, context).physics_query.hit;
    case LN_BoolExpressionKind::PhysicsQueryBlocked:
      return ResolveQueryExpression(expression.input0, context).physics_query.blocked;
    case LN_BoolExpressionKind::PhysicsQueryHasUV:
      return ResolveQueryExpression(expression.input0, context).physics_query.has_uv;
    case LN_BoolExpressionKind::PhysicsQueryStartedOverlapping:
      return ResolveQueryExpression(expression.input0, context).physics_query.started_overlapping;
    case LN_BoolExpressionKind::PhysicsQueryAllHit: {
      const std::vector<LN_PhysicsQueryResult> &results =
          ResolveQueryExpression(expression.input0, context).physics_query_results;
      return !results.empty() &&
             std::all_of(results.begin(), results.end(), [](const LN_PhysicsQueryResult &result) {
               return result.hit;
             });
    }
    case LN_BoolExpressionKind::QueryValid:
      return ResolveQueryExpression(expression.input0, context).valid;
    case LN_BoolExpressionKind::MotionValid: {
      if (expression.int_value < 0) {
        return false;
      }
      const FlowControlRuntimeState *state = FindFlowControlState(uint32_t(expression.int_value));
      return state != nullptr && state->motion_valid;
    }
    case LN_BoolExpressionKind::TweenValue: {
      return UpdateTweenValue(expression_index, context);
    }
    case LN_BoolExpressionKind::TweenReached: {
      if (expression.input0 == LN_INVALID_INDEX || m_program == nullptr) {
        return fallback;
      }
      const std::vector<LN_BoolExpression> &expressions = m_program->GetBoolExpressions();
      if (expression.input0 >= expressions.size()) {
        return fallback;
      }
      UpdateTweenValue(expression.input0, context);
      return BoolExpressionState(expression.input0).cached_result;
    }
    case LN_BoolExpressionKind::TimerElapsed: {
      if (expression.int_value < 0 || context.tick_index == UINT64_MAX) {
        return fallback;
      }
      const TimeFlowRuntimeState *state = FindTimeFlowState(uint32_t(expression.int_value));
      return state != nullptr && state->pulse_tick == context.tick_index;
    }
    case LN_BoolExpressionKind::DelayDone: {
      if (expression.int_value < 0 || context.tick_index == UINT64_MAX) {
        return fallback;
      }
      const TimeFlowRuntimeState *state = FindTimeFlowState(uint32_t(expression.int_value));
      return state != nullptr && state->pulse_tick == context.tick_index;
    }
    case LN_BoolExpressionKind::PulsifyPulse: {
      if (expression.int_value < 0 || context.tick_index == UINT64_MAX) {
        return fallback;
      }
      const TimeFlowRuntimeState *state = FindTimeFlowState(uint32_t(expression.int_value));
      return state != nullptr && (state->pulse_tick == context.tick_index ||
                                  (state->kind == TimeFlowRuntimeKind::Pulsify &&
                                   state->last_update_tick == context.tick_index &&
                                   state->accumulated_time <= LN_TIME_FLOW_EPSILON));
    }
    case LN_BoolExpressionKind::BarrierPassed: {
      if (expression.int_value < 0 || context.tick_index == UINT64_MAX) {
        return fallback;
      }
      const TimeFlowRuntimeState *state = FindTimeFlowState(uint32_t(expression.int_value));
      return state != nullptr &&
             (state->pulse_tick == context.tick_index ||
              (state->kind == TimeFlowRuntimeKind::Barrier && state->active &&
               state->remaining_time >= 0.0 &&
               state->accumulated_time + LN_TIME_FLOW_EPSILON >= state->remaining_time));
    }
    case LN_BoolExpressionKind::CooldownAccepted:
    case LN_BoolExpressionKind::CooldownBlocked: {
      if (expression.int_value < 0 || context.execution_scope_serial == 0) {
        return fallback;
      }
      const TimeFlowRuntimeState *state = FindTimeFlowState(uint32_t(expression.int_value));
      if (state == nullptr) {
        return fallback;
      }
      return expression.kind == LN_BoolExpressionKind::CooldownAccepted ?
                 state->accepted_scope_serial == context.execution_scope_serial :
                 state->blocked_scope_serial == context.execution_scope_serial;
    }
    case LN_BoolExpressionKind::CooldownCompleted: {
      if (expression.int_value < 0 || context.tick_index == UINT64_MAX) {
        return fallback;
      }
      const TimeFlowRuntimeState *state = FindTimeFlowState(uint32_t(expression.int_value));
      return state != nullptr && state->pulse_tick == context.tick_index;
    }
    case LN_BoolExpressionKind::CooldownReady: {
      if (expression.int_value < 0) {
        return fallback;
      }
      const TimeFlowRuntimeState *state = FindTimeFlowState(uint32_t(expression.int_value));
      return state != nullptr && !state->active;
    }
    case LN_BoolExpressionKind::PersistenceCompleted: {
      BoolExpressionRuntimeState &state = BoolExpressionState(expression_index);
      if (state.last_update_tick == context.tick_index) {
        return state.cached_result;
      }
      state.cached_result = state.pending_persistence_count != 0;
      if (state.cached_result) {
        state.last_persistence_success = (state.pending_persistence_results & uint64_t(1)) != 0;
        state.pending_persistence_results >>= 1;
        state.pending_persistence_count--;
      }
      state.last_update_tick = context.tick_index;
      return state.cached_result;
    }
    case LN_BoolExpressionKind::PersistenceSucceeded: {
      if (expression.input0 == LN_INVALID_INDEX ||
          expression.input0 >= m_program->GetBoolExpressions().size() ||
          m_program->GetBoolExpressions()[expression.input0].kind !=
              LN_BoolExpressionKind::PersistenceCompleted)
      {
        return fallback;
      }
      const bool completed = ResolveBoolExpression(expression.input0, false, context);
      return completed && BoolExpressionState(expression.input0).last_persistence_success;
    }
    case LN_BoolExpressionKind::UICommandCompleted: {
      UICommandCompletionState &state = m_uiCommandCompletions[expression_index];
      if (state.current_tick != context.tick_index) {
        state.current_tick = context.tick_index;
        state.has_current = !state.pending.empty();
        if (state.has_current) {
          state.current = state.pending.front();
          state.pending.pop_front();
          state.has_value = true;
        }
      }
      return state.has_current;
    }
    case LN_BoolExpressionKind::UICommandSucceeded:
    case LN_BoolExpressionKind::UICommandFailed:
    case LN_BoolExpressionKind::UICommandClosed: {
      if (expression.input0 == LN_INVALID_INDEX ||
          expression.input0 >= m_program->GetBoolExpressions().size() ||
          m_program->GetBoolExpressions()[expression.input0].kind !=
              LN_BoolExpressionKind::UICommandCompleted)
      {
        return fallback;
      }
      if (!ResolveBoolExpression(expression.input0, false, context)) {
        return false;
      }
      const UICommandCompletionState &state = m_uiCommandCompletions[expression.input0];
      const bool succeeded = state.current.result == LN_UIResult::Success ||
                             state.current.result == LN_UIResult::NoChange;
      /* A toggled menu that closed is neither opened nor failed. */
      const bool closed = state.current.result == LN_UIResult::Closed;
      switch (expression.kind) {
        case LN_BoolExpressionKind::UICommandSucceeded:
          return succeeded;
        case LN_BoolExpressionKind::UICommandClosed:
          return closed;
        default:
          return !succeeded && !closed;
      }
    }
    case LN_BoolExpressionKind::PlayerJoinPulse:
      return m_logicManager != nullptr && m_logicManager->IsPlayerJoinActive();
    case LN_BoolExpressionKind::PlayerGamepadConnected: {
      const int32_t gamepad = LN_InputMap::Get().GamepadOfUser(
          ResolveIntExpression(expression.input0, 0, context));
      DEV_Joystick *joystick = gamepad >= 0 ? DEV_Joystick::GetInstance(short(gamepad)) :
                                                    nullptr;
      return joystick != nullptr && joystick->Connected();
    }
    case LN_BoolExpressionKind::PlayerGamepadJoined:
      return LN_InputMap::Get().IsUserJoined(ResolveIntExpression(expression.input0, 0, context));
    case LN_BoolExpressionKind::PlayerViewShown: {
      KX_PlayerViews::Area area;
      return ResolvePlayerViewArea(
          m_gameObject, ResolveIntExpression(expression.input0, 0, context), area);
    }
    case LN_BoolExpressionKind::ActionLifecycleEvent: {
      ActionCompletionState &state = m_actionCompletions[expression_index];
      if (state.current_tick != context.tick_index) {
        state.current_tick = context.tick_index;
        state.has_current = !state.pending.empty();
        if (state.has_current) {
          state.current = state.pending.front();
          state.pending.pop_front();
        }
      }
      return state.has_current;
    }
    case LN_BoolExpressionKind::ActionStarted:
    case LN_BoolExpressionKind::ActionCompleted:
    case LN_BoolExpressionKind::ActionInterrupted:
    case LN_BoolExpressionKind::ActionFailed: {
      if (expression.input0 == LN_INVALID_INDEX ||
          expression.input0 >= m_program->GetBoolExpressions().size() ||
          m_program->GetBoolExpressions()[expression.input0].kind !=
              LN_BoolExpressionKind::ActionLifecycleEvent)
      {
        return fallback;
      }
      if (!ResolveBoolExpression(expression.input0, false, context)) {
        return false;
      }
      const LN_ActionResult result = m_actionCompletions[expression.input0].current;
      switch (expression.kind) {
        case LN_BoolExpressionKind::ActionStarted:
          return result == LN_ActionResult::Started;
        case LN_BoolExpressionKind::ActionCompleted:
          return result == LN_ActionResult::Completed;
        case LN_BoolExpressionKind::ActionInterrupted:
          return result == LN_ActionResult::Interrupted;
        case LN_BoolExpressionKind::ActionFailed:
          return result == LN_ActionResult::Failed;
        default:
          return false;
      }
    }
    case LN_BoolExpressionKind::GameLoadPulse:
      return m_logicManager != nullptr && m_logicManager->IsGameLoadPulseActive();
    case LN_BoolExpressionKind::GameLoadSucceeded:
      return m_logicManager != nullptr && m_logicManager->DidGameLoadSucceed();
    case LN_BoolExpressionKind::MouseOverEnter:
      return ResolveQueryExpression(expression.input0, context).entered;
    case LN_BoolExpressionKind::MouseOverOver:
      return ResolveQueryExpression(expression.input0, context).over;
    case LN_BoolExpressionKind::MouseOverExit:
      return ResolveQueryExpression(expression.input0, context).exited;
    case LN_BoolExpressionKind::UIElementEventFired:
      return ResolveQueryExpression(expression.input0, context).valid;
    case LN_BoolExpressionKind::UISoundFired: {
      const LN_QueryResult &sound = ResolveQueryExpression(expression.input0, context);
      return sound.valid && sound.ui_sound == expression.int_value;
    }
    case LN_BoolExpressionKind::UIElementEventAccepted:
      return ResolveQueryExpression(expression.input0, context).ui_accepted;
    case LN_BoolExpressionKind::UIScrolled:
      return ResolveQueryExpression(expression.input0, context).ui_scrolled;
    case LN_BoolExpressionKind::UIScrollAtStart:
      return ResolveQueryExpression(expression.input0, context).ui_scroll_at_start;
    case LN_BoolExpressionKind::UIScrollAtEnd:
      return ResolveQueryExpression(expression.input0, context).ui_scroll_at_end;
    case LN_BoolExpressionKind::UIItemsRebound:
      return ResolveQueryExpression(expression.input0, context).ui_items_rebound;
    case LN_BoolExpressionKind::LogicTreeRunning: {
      KX_GameObject *target_object = nullptr;
      if (expression.input0 != LN_INVALID_INDEX) {
        target_object = ResolveObjectValue(
            ResolveValueExpression(expression.input0, MakeNoneValue(), context));
      }
      else if (m_gameObject != nullptr) {
        target_object = m_gameObject;
      }
      const std::string tree_name = expression.input1 != LN_INVALID_INDEX ?
                                        ResolveStringExpression(
                                            expression.input1, expression.string_value, context) :
                                        expression.string_value;
      if (target_object == nullptr || tree_name.empty()) {
        return fallback;
      }
      KX_Scene *scene = m_gameObject ? m_gameObject->GetScene() : nullptr;
      LN_Manager *manager = scene ? scene->GetLogicNodeManager() : nullptr;
      if (manager == nullptr) {
        return fallback;
      }
      return manager->IsLogicTreeRunning(target_object, tree_name);
    }
    case LN_BoolExpressionKind::LogicTreeStopped: {
      KX_GameObject *target_object = nullptr;
      if (expression.input0 != LN_INVALID_INDEX) {
        target_object = ResolveObjectValue(
            ResolveValueExpression(expression.input0, MakeNoneValue(), context));
      }
      else if (m_gameObject != nullptr) {
        target_object = m_gameObject;
      }
      const std::string tree_name = expression.input1 != LN_INVALID_INDEX ?
                                        ResolveStringExpression(
                                            expression.input1, expression.string_value, context) :
                                        expression.string_value;
      if (target_object == nullptr || tree_name.empty()) {
        return fallback;
      }
      KX_Scene *scene = m_gameObject ? m_gameObject->GetScene() : nullptr;
      LN_Manager *manager = scene ? scene->GetLogicNodeManager() : nullptr;
      if (manager == nullptr) {
        return fallback;
      }
      return !manager->IsLogicTreeRunning(target_object, tree_name);
    }
    case LN_BoolExpressionKind::ActionDone: {
      KX_GameObject *target_object = nullptr;
      if (expression.input0 != LN_INVALID_INDEX) {
        target_object = ResolveObjectValue(
            ResolveValueExpression(expression.input0, MakeNoneValue(), context));
      }
      else if (m_gameObject != nullptr) {
        target_object = m_gameObject;
      }
      if (target_object == nullptr) {
        return fallback;
      }
      const int32_t layer = ResolveIntExpression(
          expression.int_expr_index, expression.int_value, context);
      return target_object->IsActionDone(short(layer));
    }
    case LN_BoolExpressionKind::AnimationPlaying: {
      KX_GameObject *target_object = nullptr;
      if (expression.input0 != LN_INVALID_INDEX) {
        target_object = ResolveObjectValue(
            ResolveValueExpression(expression.input0, MakeNoneValue(), context));
      }
      else if (m_gameObject != nullptr) {
        target_object = m_gameObject;
      }
      if (target_object == nullptr) {
        return fallback;
      }
      const int32_t layer = ResolveIntExpression(
          expression.int_expr_index, expression.int_value, context);
      const std::string action_name = target_object->GetActionName(short(layer));
      return !action_name.empty() && !target_object->IsActionDone(short(layer));
    }
    case LN_BoolExpressionKind::ObjectsColliding: {
      KX_GameObject *obj_a = m_gameObject;
      KX_GameObject *obj_b = nullptr;
      if (expression.input0 != LN_INVALID_INDEX) {
        obj_a = ResolveObjectValue(
            ResolveValueExpression(expression.input0, MakeNoneValue(), context));
      }
      if (expression.input1 != LN_INVALID_INDEX) {
        obj_b = ResolveObjectValue(
            ResolveValueExpression(expression.input1, MakeNoneValue(), context));
      }
      if (obj_a == nullptr || obj_b == nullptr) {
        return fallback;
      }
      PHY_IPhysicsController *ctrl_a = obj_a->GetPhysicsController();
      PHY_IPhysicsController *ctrl_b = obj_b->GetPhysicsController();
      if (ctrl_a == nullptr || ctrl_b == nullptr) {
        return false;
      }
      KX_Scene *scene = m_gameObject ? m_gameObject->GetScene() : nullptr;
      if (scene == nullptr) {
        scene = obj_a->GetScene();
      }
      if (scene == nullptr) {
        scene = obj_b->GetScene();
      }
      PHY_IPhysicsEnvironment *env = scene ? scene->GetPhysicsEnvironment() : nullptr;
      if (env == nullptr) {
        return false;
      }
      const PHY_CollisionTestResult hit = env->CheckCollision(ctrl_a, ctrl_b);
      const bool collide = hit.collide;
      delete hit.collData;
      return collide;
    }
    case LN_BoolExpressionKind::EventReceived: {
      const LN_StringId subject_string_id = ResolveStringExpressionId(expression.input0);
      const LN_EventSubjectId subject_id = GetEventSubjectId(subject_string_id);
      const std::string subject = ResolveStringExpression(
          expression.input0, expression.string_value, context);
      if (subject.empty()) {
        return false;
      }
      KX_GameObject *filter_target = m_gameObject;
      if (expression.bool_value) {
        if (expression.input1 == LN_INVALID_INDEX) {
          return false;
        }
        const LN_Value target_val = ResolveValueExpression(
            expression.input1, MakeNoneValue(), context);
        KX_GameObject *explicit_target = ResolveObjectValue(target_val);
        if (explicit_target == nullptr) {
          return false;
        }
        filter_target = explicit_target;
      }
      return FindFirstActiveEvent(subject_id, subject, filter_target) != nullptr;
    }
    case LN_BoolExpressionKind::CollisionDetected: {
      return ResolveCollisionResult(expression.input0,
                                    expression.input1,
                                    expression.input2,
                                    context,
                                    CollisionResult::Detail::StateOnly)
          .hit;
    }
    case LN_BoolExpressionKind::ConstraintBreak:
      return ResolveRigidBodyConstraintBreakEvent(expression.input0,
                                                  expression.input1,
                                                  expression.input2,
                                                  expression.rigid_body_constraint_match_mode,
                                                  context) != nullptr;
    case LN_BoolExpressionKind::CollisionEnter:
    case LN_BoolExpressionKind::CollisionStay:
    case LN_BoolExpressionKind::CollisionExit: {
      BoolExpressionRuntimeState &state = BoolExpressionState(expression_index);
      if (state.last_update_tick == context.tick_index) {
        return state.cached_result;
      }

      CollisionResult::Detail detail = CollisionResult::Detail::StateOnly;
      if (expression.int_value >= static_cast<int>(CollisionResult::Detail::Contacts) ||
          expression.bool_value)
      {
        detail = CollisionResult::Detail::Contacts;
      }
      else if (expression.int_value == static_cast<int>(CollisionResult::Detail::Objects)) {
        detail = CollisionResult::Detail::Objects;
      }
      const CollisionResult &collision_query = ResolveCollisionResult(
          expression.input0, expression.input1, expression.input2, context, detail, false);
      const bool current = collision_query.hit;
      const bool previous = state.initialized ? state.previous_bool : false;
      switch (expression.kind) {
        case LN_BoolExpressionKind::CollisionEnter:
          state.cached_result = current && !previous;
          break;
        case LN_BoolExpressionKind::CollisionStay:
          state.cached_result = current && previous;
          break;
        case LN_BoolExpressionKind::CollisionExit:
          state.cached_result = !current && previous;
          break;
        default:
          state.cached_result = false;
          break;
      }
      if (expression.kind == LN_BoolExpressionKind::CollisionExit && state.cached_result) {
        MarkCollisionExitPayload(collision_query, context.tick_index);
      }
      state.previous_bool = current;
      state.initialized = true;
      state.last_update_tick = context.tick_index;
      return state.cached_result;
    }
    case LN_BoolExpressionKind::NextFrameDone: {
      if (expression.int_value < 0 || context.tick_index == UINT64_MAX) {
        return fallback;
      }
      const TimeFlowRuntimeState *state = FindTimeFlowState(uint32_t(expression.int_value));
      return state != nullptr && state->pulse_tick == context.tick_index;
    }
    case LN_BoolExpressionKind::BooleanEdge: {
      BoolExpressionRuntimeState &state = BoolExpressionState(expression_index);
      if (context.tick_index != UINT64_MAX && state.last_update_tick == context.tick_index) {
        return state.cached_result;
      }

      const bool current = ResolveBoolExpression(expression.input0, false, context);
      const bool previous = state.initialized ? state.previous_bool : false;
      state.cached_result = current && !previous;
      state.active = !current && previous;
      state.previous_bool = current;
      state.initialized = true;
      state.last_update_tick = context.tick_index;
      return state.cached_result;
    }
    case LN_BoolExpressionKind::BooleanEdgeFalling: {
      if (expression.input0 == LN_INVALID_INDEX || m_program == nullptr ||
          expression.input0 >= m_program->GetBoolExpressions().size())
      {
        return fallback;
      }
      ResolveBoolExpression(expression.input0, false, context);
      return BoolExpressionState(expression.input0).active;
    }
    case LN_BoolExpressionKind::Once: {
      BoolExpressionRuntimeState &state = BoolExpressionState(expression_index);
      const uint64_t scope_serial = context.execution_scope_serial;
      return scope_serial != 0 && state.initialized && state.last_update_tick == scope_serial &&
             state.cached_result;
    }
    case LN_BoolExpressionKind::FlowGateOpen: {
      BoolExpressionRuntimeState &state = BoolExpressionState(expression_index);
      if (!state.initialized) {
        state.active = ResolveBoolExpression(expression.input0, false, context);
        state.initialized = true;
      }
      return state.active;
    }
    case LN_BoolExpressionKind::FlowRouterSelected: {
      if (expression.int_value < 0 || expression.secondary_int_value < 0 ||
          context.execution_scope_serial == 0)
      {
        return fallback;
      }
      const FlowControlRuntimeState *state = FindFlowControlState(uint32_t(expression.int_value));
      return state != nullptr && state->routed_scope_serial == context.execution_scope_serial &&
             state->last_route_index == uint32_t(expression.secondary_int_value);
    }
    case LN_BoolExpressionKind::FlowRouterLastRoute: {
      if (expression.int_value < 0 || expression.secondary_int_value < 0) {
        return fallback;
      }
      const FlowControlRuntimeState *state = FindFlowControlState(uint32_t(expression.int_value));
      return state != nullptr &&
             state->last_route_index == uint32_t(expression.secondary_int_value);
    }
    case LN_BoolExpressionKind::RepeatLimitAccepted:
    case LN_BoolExpressionKind::RepeatLimitBlocked: {
      if (expression.int_value < 0 || context.execution_scope_serial == 0) {
        return fallback;
      }
      const FlowControlRuntimeState *state = FindFlowControlState(uint32_t(expression.int_value));
      if (state == nullptr) {
        return fallback;
      }
      return expression.kind == LN_BoolExpressionKind::RepeatLimitAccepted ?
                 state->accepted_scope_serial == context.execution_scope_serial :
                 state->blocked_scope_serial == context.execution_scope_serial;
    }
    case LN_BoolExpressionKind::RepeatLimitExhausted: {
      if (expression.int_value < 0 || expression.input0 == LN_INVALID_INDEX) {
        return fallback;
      }
      const FlowControlRuntimeState *state = FindFlowControlState(uint32_t(expression.int_value));
      if (state == nullptr) {
        return fallback;
      }
      const int32_t requested_limit = ResolveIntExpression(expression.input0, 0, context);
      const uint32_t limit = requested_limit > 0 ? uint32_t(requested_limit) : 0u;
      return state->accepted_count >= limit;
    }
    case LN_BoolExpressionKind::Timer: {
      BoolExpressionRuntimeState &state = BoolExpressionState(expression_index);
      const bool condition = ResolveBoolExpression(expression.input0, false, context);
      const bool rising_edge = condition && (!state.initialized || !state.previous_bool);
      const float duration = std::max(
          0.0f,
          ResolveFloatExpression(expression.float_expr_index, expression.float_value, context));

      if (rising_edge) {
        state.active = true;
        state.due_time = context.current_time + double(duration);
      }

      bool result = false;
      if (state.active && context.current_time >= state.due_time) {
        result = true;
        state.active = false;
      }

      if (!condition && !state.active) {
        state.due_time = 0.0;
      }
      state.previous_bool = condition;
      state.initialized = true;
      return result;
    }
    case LN_BoolExpressionKind::LoopActive: {
      const uint32_t loop_frame_index = uint32_t(expression.int_value);
      return m_activeLoopBodyFrame == loop_frame_index;
    }
    case LN_BoolExpressionKind::ValueChanged:
    case LN_BoolExpressionKind::ValueChangedTo: {
      BoolExpressionRuntimeState &state = BoolExpressionState(expression_index);
      if (state.last_update_tick == context.tick_index) {
        return state.cached_result;
      }

      const LN_Value value = ResolveValueExpression(expression.input0, MakeNoneValue(), context);
      if (!state.has_previous_value) {
        if (expression.bool_value && (!value.exists || value.type == LN_ValueType::None)) {
          state.cached_result = false;
          state.last_update_tick = context.tick_index;
          return false;
        }
        state.previous_value = value;
        state.has_previous_value = true;
        state.stored_old_value = value;
        state.stored_new_value = value;
        state.cached_result = false;
        state.last_update_tick = context.tick_index;
        return false;
      }

      const bool changed = !ValuesEqual(value, state.previous_value);
      state.stored_old_value = state.previous_value;
      state.stored_new_value = value;
      state.previous_value = value;

      if (expression.kind == LN_BoolExpressionKind::ValueChanged) {
        state.cached_result = changed;
        state.last_update_tick = context.tick_index;
        return state.cached_result;
      }

      const LN_Value target = ResolveValueExpression(expression.input1, MakeNoneValue(), context);
      state.cached_result = changed && ValueChangedToTarget(state.stored_old_value, value, target);
      state.last_update_tick = context.tick_index;
      return state.cached_result;
    }
    case LN_BoolExpressionKind::Count:
      break;
  }

  return fallback;
}

int32_t LN_RuntimeTree::ResolveIntExpression(uint32_t expression_index,
                                             int32_t fallback,
                                             const LN_TickContext &context)
{
  if (expression_index == LN_INVALID_INDEX || m_program == nullptr) {
    return fallback;
  }

  const std::vector<LN_IntExpression> &expressions = m_program->GetIntExpressions();
  if (expression_index >= expressions.size()) {
    return fallback;
  }

  const LN_IntExpression &expression = expressions[expression_index];
  const LN_RuntimeExpressionSemantics semantics = LN_GetRuntimeExpressionSemantics(
      expression.kind);
  ProfileExpressionEvaluation(semantics.fallback_requirements);
  RecordMissingSnapshotChannelsForExpression(semantics);
  int32_t register_int_value = 0;
  if (TryResolveRegisterIntExpression(expression_index, context, register_int_value)) {
    return register_int_value;
  }
  if (context.use_register_expression_evaluator) {
    RecordRegisterExpressionFallback(LN_RuntimeExpressionFamily::Int, expression_index);
  }
  switch (expression.kind) {
    case LN_IntExpressionKind::Constant:
      return expression.int_value;
    case LN_IntExpressionKind::SnapshotGameProperty:
      if (const LN_Value *value = m_snapshot.GetGamePropertyValue(expression.property_ref_index)) {
        return value->int_value;
      }
      return fallback;
    case LN_IntExpressionKind::RuntimeTreeProperty:
      if (const LN_Value *value = GetTreePropertyValue(expression.property_ref_index)) {
        return value->int_value;
      }
      return fallback;
    case LN_IntExpressionKind::StringCount:
      return CountStringOccurrences(ResolveStringExpression(expression.input0, "", context),
                                    ResolveStringExpression(expression.input1, "", context));
    case LN_IntExpressionKind::SnapshotCollisionGroup:
      if (expression.input0 != LN_INVALID_INDEX) {
        if (KX_GameObject *game_object = ResolveObjectValue(
                ResolveValueExpression(expression.input0, MakeNoneValue(), context)))
        {
          LN_Snapshot snapshot;
          snapshot.Capture(game_object, nullptr);
          return snapshot.GetCollisionGroup();
        }
        return fallback;
      }
      return m_snapshot.GetCollisionGroup();
    case LN_IntExpressionKind::MouseWheelDelta:
      return (expression.int_value != 0 ? m_snapshot.GetRawInputSnapshot() :
                                          m_snapshot.GetInputSnapshot())
          .GetMouse()
          .wheel_delta;
    case LN_IntExpressionKind::PlayerJoinUser:
      return m_logicManager != nullptr && m_logicManager->IsPlayerJoinActive() ?
                 m_logicManager->PlayerJoinUser() :
                 fallback;
    case LN_IntExpressionKind::PlayerJoinGamepad:
      return m_logicManager != nullptr && m_logicManager->IsPlayerJoinActive() ?
                 m_logicManager->PlayerJoinGamepad() :
                 fallback;
    case LN_IntExpressionKind::PlayerGamepad:
      return LN_InputMap::Get().GamepadOfUser(ResolveIntExpression(expression.input0, 0, context));
    case LN_IntExpressionKind::MenuPlayerUser:
      return m_logicManager != nullptr && m_gameObject != nullptr ?
                 m_logicManager->GetUIService().UserOfObject(m_gameObject) :
                 0;
    case LN_IntExpressionKind::PlayerViewCount:
      if (KX_Scene *scene = m_gameObject ? m_gameObject->GetScene() : nullptr) {
        return scene->GetPlayerViews().GetViewCount(scene->GetActiveCamera());
      }
      return fallback;
    case LN_IntExpressionKind::WindowResolutionWidth:
      if (RAS_ICanvas *canvas = ActiveCanvas()) {
        return canvas->GetWidth();
      }
      return fallback;
    case LN_IntExpressionKind::WindowResolutionHeight:
      if (RAS_ICanvas *canvas = ActiveCanvas()) {
        return canvas->GetHeight();
      }
      return fallback;
    case LN_IntExpressionKind::WindowVSyncMode: {
      int32_t mode = VSYNC_OFF;
      if (ActiveCanvasVSyncMode(mode)) {
        return mode;
      }
      return fallback;
    }
    case LN_IntExpressionKind::SnapshotCharacterMaxJumps: {
      LN_Snapshot snapshot;
      const LN_ObjectSnapshot *object_snapshot = &m_snapshot.GetObjectSnapshot();
      if (expression.input0 != LN_INVALID_INDEX) {
        if (KX_GameObject *game_object = ResolveObjectValue(
                ResolveValueExpression(expression.input0, MakeNoneValue(), context)))
        {
          snapshot.Capture(game_object, nullptr);
          object_snapshot = &snapshot.GetObjectSnapshot();
        }
        else {
          return fallback;
        }
      }
      return object_snapshot->has_character ? object_snapshot->character_max_jumps : fallback;
    }
    case LN_IntExpressionKind::SnapshotCharacterJumpCount: {
      LN_Snapshot snapshot;
      const LN_ObjectSnapshot *object_snapshot = &m_snapshot.GetObjectSnapshot();
      if (expression.input0 != LN_INVALID_INDEX) {
        if (KX_GameObject *game_object = ResolveObjectValue(
                ResolveValueExpression(expression.input0, MakeNoneValue(), context)))
        {
          snapshot.Capture(game_object, nullptr);
          object_snapshot = &snapshot.GetObjectSnapshot();
        }
        else {
          return fallback;
        }
      }
      return object_snapshot->has_character ? object_snapshot->character_jump_count : fallback;
    }
    case LN_IntExpressionKind::ListLength: {
      const LN_Value list_value = ResolveValueExpression(
          expression.input0, MakeNoneValue(), context);
      if (list_value.type == LN_ValueType::List && list_value.exists) {
        return int32_t(list_value.list_value.size());
      }
      return 0;
    }
    case LN_IntExpressionKind::DictLength: {
      const LN_Value dict_value = ResolveValueExpression(
          expression.input0, MakeNoneValue(), context);
      if (dict_value.type == LN_ValueType::Dict && dict_value.exists) {
        return int32_t(dict_value.dict_value.size());
      }
      return 0;
    }
    case LN_IntExpressionKind::FromGenericValue: {
      const LN_Value value = ResolveValueExpression(expression.input0, MakeNoneValue(), context);
      return ValueAsInt(value, expression.float_to_int_mode);
    }
    case LN_IntExpressionKind::FromBool:
      return ResolveBoolExpression(expression.input0, false, context) ? 1 : 0;
    case LN_IntExpressionKind::FromFloat:
      return LN_ConvertFloatToInt(ResolveFloatExpression(expression.input0, 0.0f, context),
                                  expression.float_to_int_mode);
    case LN_IntExpressionKind::FromString: {
      const std::string text = ResolveStringExpression(expression.input0, std::string(), context);
      float value = 0.0f;
      return LN_TryParseTypecastFloat(text, value) ?
                 LN_ConvertFloatToInt(value, expression.float_to_int_mode) :
                 0;
    }
    case LN_IntExpressionKind::Random: {
      const int32_t min_value = ResolveIntExpression(expression.input0, 0, context);
      const int32_t max_value = ResolveIntExpression(expression.input1, 1, context);
      const int32_t lower = std::min(min_value, max_value);
      const int32_t upper = std::max(min_value, max_value);
      const uint64_t seed = (uint64_t(expression_index) << 32) ^ context.tick_index;
      return lower + int32_t(HashToUnitFloat(seed) * float(upper - lower + 1));
    }
    case LN_IntExpressionKind::MaterialSlotCount: {
      KX_GameObject *target_object = m_gameObject;
      if (expression.input0 != LN_INVALID_INDEX) {
        target_object = ResolveObjectValue(
            ResolveValueExpression(expression.input0, MakeNoneValue(), context));
      }
      blender::Object *object = target_object ? target_object->GetBlenderObject() : nullptr;
      return object ? int32_t(object->totcol) : fallback;
    }
    case LN_IntExpressionKind::CollisionContactCount:
      return ResolveCollisionResult(expression.input0,
                                    expression.input1,
                                    expression.input2,
                                    context,
                                    CollisionResult::Detail::Contacts)
          .contact_count;
    case LN_IntExpressionKind::PhysicsQueryFaceIndex:
      return ResolveQueryExpression(expression.input0, context).physics_query.polygon_index;
    case LN_IntExpressionKind::PhysicsQueryHitCount: {
      const std::vector<LN_PhysicsQueryResult> &results =
          ResolveQueryExpression(expression.input0, context).physics_query_results;
      return int32_t(std::count_if(
          results.begin(), results.end(), [](const auto &result) { return result.hit; }));
    }
    case LN_IntExpressionKind::PhysicsQueryRayCount:
      return int32_t(
          ResolveQueryExpression(expression.input0, context).physics_query_results.size());
    case LN_IntExpressionKind::QueryVectorCount:
      return ResolveQueryExpression(expression.input0, context).vector_count;
    case LN_IntExpressionKind::UIFirstVisibleIndex:
      return ResolveQueryExpression(expression.input0, context).ui_first_visible_index;
    case LN_IntExpressionKind::UILastVisibleIndex:
      return ResolveQueryExpression(expression.input0, context).ui_last_visible_index;
    case LN_IntExpressionKind::UIElementEventIndex:
      return ResolveQueryExpression(expression.input0, context).ui_index;
    case LN_IntExpressionKind::UIElementEventTargetIndex:
      return ResolveQueryExpression(expression.input0, context).ui_target_index;
    case LN_IntExpressionKind::UIElementEventUser:
      return ResolveQueryExpression(expression.input0, context).ui_user;
    case LN_IntExpressionKind::UIElementEventAmount:
      return ResolveQueryExpression(expression.input0, context).ui_amount;
    case LN_IntExpressionKind::UIViewPage:
      return ResolveQueryExpression(expression.input0, context).ui_page;
    case LN_IntExpressionKind::UIViewPageCount:
      return ResolveQueryExpression(expression.input0, context).ui_page_count;
    case LN_IntExpressionKind::UICommandResult:
    case LN_IntExpressionKind::UICommandHandle: {
      if (expression.input0 == LN_INVALID_INDEX ||
          expression.input0 >= m_program->GetBoolExpressions().size() ||
          m_program->GetBoolExpressions()[expression.input0].kind !=
              LN_BoolExpressionKind::UICommandCompleted)
      {
        return fallback;
      }
      /* Advance a pending completion for this tick when the value is consumed without a connected
       * success/failure flow, then retain the last completed value for later commands. */
      ResolveBoolExpression(expression.input0, false, context);
      const UICommandCompletionState &state = m_uiCommandCompletions[expression.input0];
      if (!state.has_value) {
        return fallback;
      }
      return expression.kind == LN_IntExpressionKind::UICommandHandle ?
                 state.current.handle :
                 int32_t(state.current.result);
    }
    case LN_IntExpressionKind::ConstraintBreakCount:
      return ResolveRigidBodyConstraintBreakEventCount(expression.input0,
                                                       expression.input1,
                                                       expression.input2,
                                                       expression.rigid_body_constraint_match_mode,
                                                       context);
    case LN_IntExpressionKind::LoopIndex: {
      const uint32_t loop_frame_index = uint32_t(expression.int_value);
      if (loop_frame_index >= m_loopStates.size()) {
        return fallback;
      }
      const LoopRuntimeState &state = m_loopStates[loop_frame_index];
      if (state.iteration_index == 0) {
        return 0;
      }
      return int32_t(state.iteration_index - 1);
    }
    case LN_IntExpressionKind::RepeatLimitRemaining: {
      if (expression.int_value < 0 || expression.input0 == LN_INVALID_INDEX) {
        return fallback;
      }
      const FlowControlRuntimeState *state = FindFlowControlState(uint32_t(expression.int_value));
      if (state == nullptr) {
        return fallback;
      }
      const int32_t requested_limit = ResolveIntExpression(expression.input0, 0, context);
      const uint32_t limit = requested_limit > 0 ? uint32_t(requested_limit) : 0u;
      const uint32_t remaining = limit > state->accepted_count ? limit - state->accepted_count :
                                                                 0u;
      return remaining > uint32_t(std::numeric_limits<int32_t>::max()) ?
                 std::numeric_limits<int32_t>::max() :
                 int32_t(remaining);
    }
    case LN_IntExpressionKind::Count:
      break;
  }

  return fallback;
}

bool LN_RuntimeTree::UpdateTweenValue(const uint32_t tween_expr_index,
                                      const LN_TickContext &context)
{
  if (tween_expr_index == LN_INVALID_INDEX || m_program == nullptr) {
    return false;
  }

  const std::vector<LN_BoolExpression> &bool_expressions = m_program->GetBoolExpressions();
  if (tween_expr_index >= bool_expressions.size()) {
    return false;
  }

  const LN_BoolExpression &tween = bool_expressions[tween_expr_index];
  BoolExpressionRuntimeState &state = BoolExpressionState(tween_expr_index);
  if (context.tick_index != UINT64_MAX && state.last_update_tick == context.tick_index) {
    return state.active || state.cached_result;
  }

  const bool on_demand = tween.bool_value;
  bool forward = false;
  bool back = false;
  state.cached_result = false;
  if (on_demand) {
    forward = true;
    const bool skipped_phase_tick = PhaseTickWasSkipped(state.last_update_phase_tick,
                                                        context.phase_tick_index);
    if (tween.float_value > 0.0f && skipped_phase_tick) {
      state.previous_float = 0.0f;
      state.active = false;
      state.previous_bool = true;
      state.initialized = false;
    }
  }
  else {
    forward = ResolveBoolExpression(tween.input0, false, context);
    back = ResolveBoolExpression(tween.input1, false, context);
  }

  float duration = ResolveFloatExpression(tween.float_expr_index, 1.0f, context);
  if (!std::isfinite(duration) || duration < 0.0f) {
    duration = 0.0f;
  }
  const bool ignore_timescale = ResolveBoolExpression(tween.input2, false, context);
  const float delta_time = float(TimeFlowStep(context, ignore_timescale));
  const float epsilon = 1.0e-6f;
  if (duration > 0.0f) {
    state.previous_float = std::clamp(state.previous_float, 0.0f, duration);
  }
  else {
    state.previous_float = 0.0f;
  }

  if (forward) {
    const bool already_at_target = duration <= 0.0f && state.initialized && state.previous_bool;
    state.previous_bool = true;
    state.active = duration <= 0.0f ? !already_at_target :
                                      state.previous_float < duration - epsilon;
  }
  else if (back) {
    const bool already_at_target = duration <= 0.0f && state.initialized && !state.previous_bool;
    state.previous_bool = false;
    state.active = duration <= 0.0f ? !already_at_target : state.previous_float > epsilon;
  }

  if (state.active && duration > 0.0f) {
    if (state.previous_bool && state.previous_float >= duration - epsilon) {
      state.previous_float = duration;
      state.cached_result = true;
      state.active = false;
    }
    else if (!state.previous_bool && state.previous_float <= epsilon) {
      state.previous_float = 0.0f;
      state.cached_result = true;
      state.active = false;
    }
  }

  if (!state.active) {
    state.initialized = true;
    state.last_update_tick = context.tick_index;
    state.last_update_phase_tick = context.phase_tick_index;
    return state.cached_result;
  }

  if (duration <= 0.0f) {
    state.cached_result = true;
    state.active = false;
  }
  else if (state.previous_bool) {
    const float previous_time = state.previous_float;
    state.previous_float = std::min(state.previous_float + delta_time, duration);
    if (state.previous_float >= duration - epsilon) {
      state.previous_float = duration;
      state.cached_result = previous_time < duration - epsilon;
      state.active = false;
    }
  }
  else {
    const float previous_time = state.previous_float;
    state.previous_float = std::max(state.previous_float - delta_time, 0.0f);
    if (state.previous_float <= epsilon) {
      state.previous_float = 0.0f;
      state.cached_result = previous_time > epsilon;
      state.active = false;
    }
  }

  state.initialized = true;
  state.last_update_tick = context.tick_index;
  state.last_update_phase_tick = context.phase_tick_index;
  return state.active || state.cached_result;
}

float LN_RuntimeTree::EvaluateCurveTable(const uint32_t table_index,
                                         const float input,
                                         float influence) const
{
  if (!std::isfinite(input)) {
    return 0.0f;
  }
  if (!std::isfinite(influence)) {
    return input;
  }
  if (influence == 0.0f || m_program == nullptr ||
      table_index >= m_program->GetCurveTables().size())
  {
    return input;
  }

  const LN_CurveTable &table = m_program->GetCurveTables()[table_index];
  const float domain_span = table.domain_max - table.domain_min;
  if (!std::isfinite(table.domain_min) || !std::isfinite(table.domain_max) ||
      !std::isfinite(domain_span) || domain_span <= 0.0f || !std::isfinite(table.sample_scale) ||
      table.sample_scale <= 0.0f || !std::isfinite(table.table_min_x) ||
      !std::isfinite(table.table_max_x) || table.table_min_x > table.table_max_x)
  {
    return input;
  }
  if (table.clip_output && (!std::isfinite(table.clip_min) || !std::isfinite(table.clip_max) ||
                            table.clip_min > table.clip_max))
  {
    return input;
  }

  float mapped = input;
  const float sample_position = (input - table.domain_min) * table.sample_scale;
  if (!std::isfinite(sample_position) || sample_position < 0.0f ||
      sample_position > float(LN_CURVE_SAMPLE_COUNT - 1))
  {
    if (input <= table.table_min_x) {
      mapped = table.samples.front();
      if (table.extrapolate) {
        mapped = table.extend_in_x == 0.0f ?
                     mapped + table.extend_in_y * 10000.0f :
                     mapped + table.extend_in_y * (input - table.table_min_x) / table.extend_in_x;
      }
    }
    else if (input >= table.table_max_x) {
      mapped = table.samples.back();
      if (table.extrapolate) {
        mapped = table.extend_out_x == 0.0f ?
                     mapped - table.extend_out_y * 10000.0f :
                     mapped +
                         table.extend_out_y * (input - table.table_max_x) / table.extend_out_x;
      }
    }
    else {
      /* Mirrors Blender's malformed rounding-gap fallback in curvemap_calc_extend(). */
      mapped = 0.0f;
    }
  }
  else {
    const int sample_index_0 = std::min(int(sample_position), LN_CURVE_SAMPLE_COUNT - 1);
    const int sample_index_1 = std::min(sample_index_0 + 1, LN_CURVE_SAMPLE_COUNT - 1);
    const float blend = sample_position - float(sample_index_0);
    mapped = table.samples[size_t(sample_index_0)] * (1.0f - blend) +
             table.samples[size_t(sample_index_1)] * blend;
  }

  if (table.clip_output) {
    mapped = std::clamp(mapped, table.clip_min, table.clip_max);
  }
  if (!std::isfinite(mapped)) {
    return input;
  }
  if (influence == 1.0f) {
    return mapped;
  }

  /* Preserve Blender's normal finite-value arithmetic exactly. If float intermediates overflow
   * even though the mathematical result is representable, retry in double precision. */
  const float result = (1.0f - influence) * input + influence * mapped;
  if (std::isfinite(result)) {
    return result;
  }

  const double precise_result = (1.0 - double(influence)) * double(input) +
                                double(influence) * double(mapped);
  if (!std::isfinite(precise_result) ||
      precise_result < -double(std::numeric_limits<float>::max()) ||
      precise_result > double(std::numeric_limits<float>::max()))
  {
    return input;
  }
  return float(precise_result);
}

float LN_RuntimeTree::ResolveTweenMappedFactor(const uint32_t tween_expr_index,
                                               const LN_TickContext &context)
{
  if (tween_expr_index == LN_INVALID_INDEX || m_program == nullptr) {
    return 0.0f;
  }

  const std::vector<LN_BoolExpression> &bool_expressions = m_program->GetBoolExpressions();
  if (tween_expr_index >= bool_expressions.size()) {
    return 0.0f;
  }

  const LN_BoolExpression &tween = bool_expressions[tween_expr_index];
  UpdateTweenValue(tween_expr_index, context);
  const BoolExpressionRuntimeState &state = BoolExpressionState(tween_expr_index);
  float duration = ResolveFloatExpression(tween.float_expr_index, 1.0f, context);
  if (!std::isfinite(duration) || duration < 0.0f) {
    duration = 0.0f;
  }
  const float linear_factor = duration > 0.0f ? state.previous_float / duration :
                                                (state.previous_bool ? 1.0f : 0.0f);
  const float factor = std::clamp(linear_factor, 0.0f, 1.0f);
  return EvaluateCurveTable(tween.int_expr_index, factor, 1.0f);
}

float LN_RuntimeTree::ResolveFloatExpression(const uint32_t expression_index,
                                             const float fallback,
                                             const LN_TickContext &context)
{
  if (expression_index == LN_INVALID_INDEX || m_program == nullptr) {
    return fallback;
  }

  const std::vector<LN_FloatExpression> &expressions = m_program->GetFloatExpressions();
  if (expression_index >= expressions.size()) {
    return fallback;
  }

  const LN_FloatExpression &expression = expressions[expression_index];
  const LN_RuntimeExpressionSemantics semantics = LN_GetRuntimeExpressionSemantics(
      expression.kind);
  ProfileExpressionEvaluation(semantics.fallback_requirements);
  RecordMissingSnapshotChannelsForExpression(semantics);
  float register_float_value = 0.0f;
  if (TryResolveRegisterFloatExpression(expression_index, context, register_float_value)) {
    return register_float_value;
  }
  if (context.use_register_expression_evaluator) {
    RecordRegisterExpressionFallback(LN_RuntimeExpressionFamily::Float, expression_index);
  }
  switch (expression.kind) {
    case LN_FloatExpressionKind::Constant:
      return expression.float_value;
    case LN_FloatExpressionKind::UnscaledDeltaTime:
      return float(std::max(0.0, context.unscaled_dt));
    case LN_FloatExpressionKind::SnapshotGameProperty:
      if (const LN_Value *value = m_snapshot.GetGamePropertyValue(expression.property_ref_index)) {
        return value->float_value;
      }
      return fallback;
    case LN_FloatExpressionKind::RuntimeTreeProperty:
      if (const LN_Value *value = GetTreePropertyValue(expression.property_ref_index)) {
        return value->float_value;
      }
      return fallback;
    case LN_FloatExpressionKind::FromGenericValue: {
      const LN_Value value = ResolveValueExpression(expression.input0, MakeNoneValue(), context);
      float converted = 0.0f;
      if (ValueAsFloat(value, converted)) {
        return converted;
      }
      return fallback;
    }
    case LN_FloatExpressionKind::FromBool:
      return ResolveBoolExpression(expression.bool_expr_index, false, context) ? 1.0f : 0.0f;
    case LN_FloatExpressionKind::FromInt:
      return float(ResolveIntExpression(expression.input0, 0, context));
    case LN_FloatExpressionKind::FromString: {
      const std::string text = ResolveStringExpression(
          expression.string_expr_index, std::string(), context);
      float value = 0.0f;
      return LN_TryParseTypecastFloat(text, value) ? value : 0.0f;
    }
    case LN_FloatExpressionKind::SnapshotTimeScale:
      return m_snapshot.GetTimeScale();
    case LN_FloatExpressionKind::SnapshotLightPower:
      if (expression.input0 != LN_INVALID_INDEX) {
        if (KX_GameObject *game_object = ResolveObjectValue(
                ResolveValueExpression(expression.input0, MakeNoneValue(), context)))
        {
          LN_Snapshot snapshot;
          snapshot.Capture(game_object, nullptr);
          return snapshot.GetObjectSnapshot().light_power;
        }
        return fallback;
      }
      return m_snapshot.GetObjectSnapshot().light_power;
    case LN_FloatExpressionKind::SnapshotElapsedTime:
      return m_snapshot.GetElapsedTime();
    case LN_FloatExpressionKind::SnapshotFrameDelta:
      return m_snapshot.GetFrameDelta();
    case LN_FloatExpressionKind::SnapshotFPS:
      return m_snapshot.GetFPS();
    case LN_FloatExpressionKind::SnapshotDeltaFactor:
      return m_snapshot.GetDeltaFactor();
    case LN_FloatExpressionKind::AnimationFrame: {
      KX_GameObject *target_object = nullptr;
      if (expression.input0 != LN_INVALID_INDEX) {
        target_object = ResolveObjectValue(
            ResolveValueExpression(expression.input0, MakeNoneValue(), context));
      }
      else if (m_gameObject != nullptr) {
        target_object = m_gameObject;
      }
      if (target_object == nullptr) {
        return fallback;
      }
      const int32_t layer = ResolveIntExpression(expression.input1, 0, context);
      return target_object->GetActionFrame(short(layer));
    }
    case LN_FloatExpressionKind::BoneLength: {
      KX_GameObject *armature_object = nullptr;
      if (expression.input0 == LN_INVALID_INDEX) {
        armature_object = m_snapshot.GetObjectSnapshot().object;
      }
      else {
        const LN_Value armature_value = ResolveValueExpression(
            expression.input0, MakeNoneValue(), context);
        armature_object = ResolveObjectValue(armature_value);
      }
      if (armature_object == nullptr) {
        return fallback;
      }
      blender::Object *ob = armature_object->GetBlenderObject();
      if (ob == nullptr || ob->type != blender::OB_ARMATURE) {
        return fallback;
      }
      blender::bArmature *armature = reinterpret_cast<blender::bArmature *>(ob->data);
      if (armature == nullptr) {
        return fallback;
      }
      const std::string bone_name = ResolveStringExpression(
          expression.input1, std::string(), context);
      if (bone_name.empty()) {
        return fallback;
      }
      blender::Bone *bone = BKE_armature_find_bone_name(armature, bone_name.c_str());
      if (bone == nullptr) {
        return fallback;
      }
      return bone->length;
    }
    case LN_FloatExpressionKind::Add:
      return ResolveFloatExpression(expression.input0, 0.0f, context) +
             ResolveFloatExpression(expression.input1, 0.0f, context);
    case LN_FloatExpressionKind::Subtract:
      return ResolveFloatExpression(expression.input0, 0.0f, context) -
             ResolveFloatExpression(expression.input1, 0.0f, context);
    case LN_FloatExpressionKind::Multiply:
      return ResolveFloatExpression(expression.input0, 0.0f, context) *
             ResolveFloatExpression(expression.input1, 0.0f, context);
    case LN_FloatExpressionKind::Divide: {
      const float divisor = ResolveFloatExpression(expression.input1, 0.0f, context);
      if (std::fabs(divisor) <= 1.0e-20f) {
        return 0.0f;
      }
      return ResolveFloatExpression(expression.input0, 0.0f, context) / divisor;
    }
    case LN_FloatExpressionKind::SafeDivide: {
      const float divisor = ResolveFloatExpression(expression.input1, 0.0f, context);
      return divisor == 0.0f ? 0.0f :
                               ResolveFloatExpression(expression.input0, 0.0f, context) / divisor;
    }
    case LN_FloatExpressionKind::Power:
      return std::pow(ResolveFloatExpression(expression.input0, 0.0f, context),
                      ResolveFloatExpression(expression.input1, 0.0f, context));
    case LN_FloatExpressionKind::Minimum:
      return std::min(ResolveFloatExpression(expression.input0, 0.0f, context),
                      ResolveFloatExpression(expression.input1, 0.0f, context));
    case LN_FloatExpressionKind::Maximum:
      return std::max(ResolveFloatExpression(expression.input0, 0.0f, context),
                      ResolveFloatExpression(expression.input1, 0.0f, context));
    case LN_FloatExpressionKind::Absolute:
      return std::fabs(ResolveFloatExpression(expression.input0, 0.0f, context));
    case LN_FloatExpressionKind::Sign: {
      const float value = ResolveFloatExpression(expression.input0, 0.0f, context);
      return float((value > 0.0f) - (value < 0.0f));
    }
    case LN_FloatExpressionKind::Round:
      return std::floor(ResolveFloatExpression(expression.input0, 0.0f, context) + 0.5f);
    case LN_FloatExpressionKind::Floor:
      return std::floor(ResolveFloatExpression(expression.input0, 0.0f, context));
    case LN_FloatExpressionKind::Ceil:
      return std::ceil(ResolveFloatExpression(expression.input0, 0.0f, context));
    case LN_FloatExpressionKind::Truncate:
      return std::trunc(ResolveFloatExpression(expression.input0, 0.0f, context));
    case LN_FloatExpressionKind::Fraction: {
      const float value = ResolveFloatExpression(expression.input0, 0.0f, context);
      return value - std::floor(value);
    }
    case LN_FloatExpressionKind::Modulo: {
      const float divisor = ResolveFloatExpression(expression.input1, 0.0f, context);
      if (std::fabs(divisor) <= 1.0e-20f) {
        return 0.0f;
      }
      return std::fmod(ResolveFloatExpression(expression.input0, 0.0f, context), divisor);
    }
    case LN_FloatExpressionKind::Sine:
      return std::sin(ResolveFloatExpression(expression.input0, 0.0f, context));
    case LN_FloatExpressionKind::Cosine:
      return std::cos(ResolveFloatExpression(expression.input0, 0.0f, context));
    case LN_FloatExpressionKind::Radians:
      return ResolveFloatExpression(expression.input0, 0.0f, context) * (LN_PI / 180.0f);
    case LN_FloatExpressionKind::Degrees:
      return ResolveFloatExpression(expression.input0, 0.0f, context) * (180.0f / LN_PI);
    case LN_FloatExpressionKind::Negate:
      return -ResolveFloatExpression(expression.input0, 0.0f, context);
    case LN_FloatExpressionKind::Clamp: {
      const float value = ResolveFloatExpression(expression.input0, 0.0f, context);
      const float min_value = ResolveFloatExpression(expression.input1, 0.0f, context);
      const float max_value = ResolveFloatExpression(expression.input2, 1.0f, context);
      const float lower = std::min(min_value, max_value);
      const float upper = std::max(min_value, max_value);
      return std::min(std::max(value, lower), upper);
    }
    case LN_FloatExpressionKind::Threshold: {
      const float value = ResolveFloatExpression(expression.input0, 0.0f, context);
      const float threshold = ResolveFloatExpression(expression.input1, 0.0f, context);
      const bool else_zero = ResolveBoolExpression(
          expression.bool_expr_index, expression.bool_value, context);
      switch (expression.threshold_operation) {
        case LN_ThresholdOperation::Greater:
          return (value > threshold) ? value : (else_zero ? 0.0f : threshold);
        case LN_ThresholdOperation::Less:
          return (value < threshold) ? value : (else_zero ? 0.0f : threshold);
      }
      break;
    }
    case LN_FloatExpressionKind::RangedThreshold: {
      const float value = ResolveFloatExpression(expression.input0, 0.0f, context);
      const float min_value = ResolveFloatExpression(expression.input1, 0.0f, context);
      const float max_value = ResolveFloatExpression(expression.input2, 0.0f, context);
      switch (expression.range_operation) {
        case LN_RangeOperation::Inside:
          return (min_value < value && value < max_value) ? value : 0.0f;
        case LN_RangeOperation::Outside:
          return (value < min_value || value > max_value) ? value : 0.0f;
      }
      break;
    }
    case LN_FloatExpressionKind::VectorComponent: {
      const MT_Vector3 vector = ResolveVectorExpression(
          expression.input0, MT_Vector3(0.0f, 0.0f, 0.0f), context);
      switch (expression.component_index) {
        case 0:
          return vector.x();
        case 1:
          return vector.y();
        case 2:
          return vector.z();
      }
      break;
    }
    case LN_FloatExpressionKind::ColorComponent: {
      const MT_Vector4 color = ResolveColorExpression(
          expression.input0, MT_Vector4(0.0f, 0.0f, 0.0f, 1.0f), context);
      switch (expression.component_index) {
        case 0:
          return color.x();
        case 1:
          return color.y();
        case 2:
          return color.z();
        case 3:
          return color.w();
      }
      break;
    }
    case LN_FloatExpressionKind::Random: {
      const float min_value = ResolveFloatExpression(expression.input0, 0.0f, context);
      const float max_value = ResolveFloatExpression(expression.input1, 1.0f, context);
      const float lower = std::min(min_value, max_value);
      const float upper = std::max(min_value, max_value);
      const uint64_t seed = (uint64_t(expression_index) << 32) ^ context.tick_index;
      return lower + (upper - lower) * HashToUnitFloat(seed);
    }
    case LN_FloatExpressionKind::Select: {
      const bool condition = ResolveBoolExpression(
          expression.bool_expr_index, expression.bool_value, context);
      return condition ?
                 ResolveFloatExpression(expression.input0, expression.float_value, context) :
                 ResolveFloatExpression(expression.input1, expression.float_value, context);
    }
    case LN_FloatExpressionKind::StoreValue: {
      /* The stored value. Storing happens when Flow arrives (StoreValueDone, on the node's Done
       * route), not when the value is read: a read in another tick or event must not store. */
      FloatExpressionRuntimeState &state = FloatExpressionState(expression_index);
      if (!state.initialized) {
        state.initialized = true;
        if (expression.bool_value) {
          state.stored_value = ResolveFloatExpression(
              expression.input0, expression.float_value, context);
          state.last_store_tick = context.tick_index;
        }
      }
      return state.stored_value;
    }
    case LN_FloatExpressionKind::Formula: {
      const std::string formula = ResolveStringExpression(
          expression.string_expr_index, "a + b", context);
      const float a = ResolveFloatExpression(expression.input0, 0.0f, context);
      const float b = ResolveFloatExpression(expression.input1, 0.0f, context);
      return LN::EvaluateFormula(formula, a, b);
    }
    case LN_FloatExpressionKind::FloatCurve: {
      const float input = ResolveFloatExpression(expression.input0, 1.0f, context);
      const float influence = ResolveFloatExpression(expression.input1, 1.0f, context);
      const uint32_t table_index = expression.int_value >= 0 ? uint32_t(expression.int_value) :
                                                               LN_INVALID_INDEX;
      return EvaluateCurveTable(table_index, input, influence);
    }
    case LN_FloatExpressionKind::TweenFactor:
      return ResolveTweenMappedFactor(expression.input0, context);
    case LN_FloatExpressionKind::TweenFloatResult: {
      const float tween_factor = ResolveTweenMappedFactor(expression.input0, context);
      const float from_value = ResolveFloatExpression(expression.input1, 0.0f, context);
      const float to_value = ResolveFloatExpression(expression.input2, 1.0f, context);
      return from_value + (to_value - from_value) * tween_factor;
    }
    case LN_FloatExpressionKind::GamepadButtonStrength: {
      const int32_t gamepad_index = ResolveIntExpression(expression.input0, 0, context);
      const LN_GamepadSnapshot *gamepad = m_snapshot.GetInputSnapshot().GetGamepad(gamepad_index);
      float strength = 0.0f;
      if (gamepad == nullptr ||
          !LN_TryGetGamepadTriggerStrength(*gamepad, int32_t(expression.input2), strength))
      {
        return fallback;
      }
      return strength;
    }
    case LN_FloatExpressionKind::PlayerViewArea: {
      KX_PlayerViews::Area area;
      if (!ResolvePlayerViewArea(
              m_gameObject, ResolveIntExpression(expression.input0, 0, context), area))
      {
        return 0.0f;
      }
      const float components[] = {area.left, area.top, area.width, area.height};
      return components[std::min<uint8_t>(expression.component_index, 3)];
    }
    case LN_FloatExpressionKind::InputActionValue: {
      const LN_InputActionState *state = m_snapshot.GetInputActionState(
          LN_InputMap::Get().Find(
              ResolveStringExpression(expression.string_expr_index, std::string(), context)),
          ResolveIntExpression(expression.input0, 0, context),
          expression.bool_value);
      return state != nullptr ? state->value : 0.0f;
    }
    case LN_FloatExpressionKind::ObjectDistance: {
      KX_GameObject *object = m_gameObject;
      if (expression.input0 != LN_INVALID_INDEX) {
        object = ResolveObjectValue(
            ResolveValueExpression(expression.input0, MakeNoneValue(), context));
      }
      KX_GameObject *target = nullptr;
      if (expression.input1 != LN_INVALID_INDEX) {
        target = ResolveObjectValue(
            ResolveValueExpression(expression.input1, MakeNoneValue(), context));
      }
      if (object == nullptr || target == nullptr) {
        return fallback;
      }
      return (object->NodeGetWorldPosition() - target->NodeGetWorldPosition()).length();
    }
    case LN_FloatExpressionKind::PhysicsQueryDistance:
      return ResolveQueryExpression(expression.input0, context).physics_query.hit_distance;
    case LN_FloatExpressionKind::PhysicsQueryFraction:
      return ResolveQueryExpression(expression.input0, context).physics_query.hit_fraction;
    case LN_FloatExpressionKind::PhysicsQueryPenetrationDepth:
      return ResolveQueryExpression(expression.input0, context).physics_query.penetration_depth;
    case LN_FloatExpressionKind::QueryDistance:
      return ResolveQueryExpression(expression.input0, context).distance;
    case LN_FloatExpressionKind::QuerySquaredDistance:
      return ResolveQueryExpression(expression.input0, context).squared_distance;
    case LN_FloatExpressionKind::MotionSpeed: {
      if (expression.int_value < 0) {
        return fallback;
      }
      const FlowControlRuntimeState *state = FindFlowControlState(uint32_t(expression.int_value));
      return state != nullptr ? state->motion_speed : fallback;
    }
    case LN_FloatExpressionKind::RigidBodyAttribute: {
      PHY_IPhysicsController *controller = ResolveRigidBodyAttributeController(expression.input0,
                                                                               context);
      if (controller == nullptr) {
        return 0.0f;
      }
      return ReadRigidBodyFloatAttribute(
          *controller,
          static_cast<LN_RigidBodyAttribute>(expression.property_ref_index),
          expression.component_index);
    }
    case LN_FloatExpressionKind::ConstraintBreakLinearImpulse:
    case LN_FloatExpressionKind::ConstraintBreakAngularImpulse:
    case LN_FloatExpressionKind::ConstraintBreakAppliedImpulse: {
      const LN_RigidBodyConstraintBreakEvent *event = ResolveRigidBodyConstraintBreakEvent(
          expression.input0,
          expression.input1,
          expression.input2,
          expression.rigid_body_constraint_match_mode,
          context);
      if (event == nullptr) {
        return fallback;
      }
      switch (expression.kind) {
        case LN_FloatExpressionKind::ConstraintBreakLinearImpulse:
          return event->linear_impulse;
        case LN_FloatExpressionKind::ConstraintBreakAngularImpulse:
          return event->angular_impulse;
        case LN_FloatExpressionKind::ConstraintBreakAppliedImpulse:
          return event->applied_impulse;
        default:
          break;
      }
      return fallback;
    }
    case LN_FloatExpressionKind::CooldownRemaining: {
      if (expression.int_value < 0) {
        return fallback;
      }
      const TimeFlowRuntimeState *state = FindTimeFlowState(uint32_t(expression.int_value));
      return state != nullptr ? float(std::max(0.0, state->remaining_time)) : fallback;
    }
    case LN_FloatExpressionKind::CooldownProgress: {
      if (expression.int_value < 0) {
        return fallback;
      }
      const TimeFlowRuntimeState *state = FindTimeFlowState(uint32_t(expression.int_value));
      if (state == nullptr) {
        return fallback;
      }
      if (!state->active || state->accumulated_time <= 0.0) {
        return 1.0f;
      }
      const double progress = 1.0 - state->remaining_time / state->accumulated_time;
      return float(std::clamp(progress, 0.0, 1.0));
    }
    case LN_FloatExpressionKind::Count:
      break;
  }

  return fallback;
}

LN_StringId LN_RuntimeTree::ResolveStringExpressionId(const uint32_t expression_index) const
{
  if (expression_index == LN_INVALID_INDEX || m_program == nullptr) {
    return LN_StringId{};
  }

  const std::vector<LN_StringExpression> &expressions = m_program->GetStringExpressions();
  if (expression_index >= expressions.size()) {
    return LN_StringId{};
  }

  const LN_RegisterExpressionProgram &ir = m_program->GetRegisterExpressionProgram();
  if (ir.valid && expression_index < ir.string_expression_registers.size() &&
      expression_index < ir.string_expression_register_kinds.size() &&
      ir.string_expression_register_kinds[expression_index] == LN_RegisterValueKind::StringId)
  {
    const uint32_t reg = ir.string_expression_registers[expression_index];
    if (reg != LN_INVALID_INDEX && reg < ir.string_id_register_count) {
      return expressions[expression_index].string_id;
    }
  }

  const LN_StringExpression &expression = expressions[expression_index];
  if (expression.kind == LN_StringExpressionKind::Constant) {
    return expression.string_id;
  }
  return LN_StringId{};
}

std::string LN_RuntimeTree::ResolveStringExpression(uint32_t expression_index,
                                                    const std::string &fallback,
                                                    const LN_TickContext &context)
{
  if (expression_index == LN_INVALID_INDEX || m_program == nullptr) {
    return fallback;
  }

  const std::vector<LN_StringExpression> &expressions = m_program->GetStringExpressions();
  if (expression_index >= expressions.size()) {
    return fallback;
  }

  std::string register_string_value;
  if (TryResolveRegisterStringExpression(expression_index, context, register_string_value)) {
    return register_string_value;
  }

  const LN_StringExpression &expression = expressions[expression_index];
  const LN_RuntimeExpressionSemantics semantics = LN_GetRuntimeExpressionSemantics(
      expression.kind);
  ProfileExpressionEvaluation(semantics.fallback_requirements);
  RecordMissingSnapshotChannelsForExpression(semantics);
  switch (expression.kind) {
    case LN_StringExpressionKind::Constant:
      return expression.string_value;
    case LN_StringExpressionKind::SnapshotGameProperty:
      if (const LN_Value *value = m_snapshot.GetGamePropertyValue(expression.property_ref_index)) {
        return value->string_value;
      }
      return fallback;
    case LN_StringExpressionKind::RuntimeTreeProperty:
      if (const LN_Value *value = GetTreePropertyValue(expression.property_ref_index)) {
        return value->string_value;
      }
      return fallback;
    case LN_StringExpressionKind::FromGenericValue: {
      const LN_Value value = ResolveValueExpression(
          expression.value_expr_index, MakeNoneValue(), context);
      return ValueToText(value);
    }
    case LN_StringExpressionKind::FromBool:
      return ResolveBoolExpression(expression.input0, false, context) ? "True" : "False";
    case LN_StringExpressionKind::UIElementEventAction:
      return ResolveQueryExpression(expression.input0, context).ui_action;
    case LN_StringExpressionKind::UIElementEventTargetAction:
      return ResolveQueryExpression(expression.input0, context).ui_target_action;
    case LN_StringExpressionKind::UIElementEventOutcome:
      return ResolveQueryExpression(expression.input0, context).ui_outcome;
    case LN_StringExpressionKind::UIElementEventMenu:
      return ResolveQueryExpression(expression.input0, context).ui_menu;
    case LN_StringExpressionKind::FromInt:
      return std::to_string(ResolveIntExpression(expression.input0, 0, context));
    case LN_StringExpressionKind::FromFloat:
      return LN_TypecastFloatToString(ResolveFloatExpression(expression.input0, 0.0f, context));
    case LN_StringExpressionKind::FromVector:
      return LN_TypecastVectorToString(
          ResolveVectorExpression(expression.input0, MT_Vector3(0.0f, 0.0f, 0.0f), context));
    case LN_StringExpressionKind::FromColor:
      return LN_TypecastColorToString(
          ResolveColorExpression(expression.input0, MT_Vector4(0.0f, 0.0f, 0.0f, 1.0f), context));
    case LN_StringExpressionKind::Join:
      return ResolveStringExpression(expression.input0, "", context) +
             ResolveStringExpression(expression.input1, "", context);
    case LN_StringExpressionKind::Replace:
      return ReplaceStringOccurrences(ResolveStringExpression(expression.input0, "", context),
                                      ResolveStringExpression(expression.input1, "", context),
                                      ResolveStringExpression(expression.input2, "", context));
    case LN_StringExpressionKind::ToUppercase:
      return ToCaseString(ResolveStringExpression(expression.input0, "", context), true);
    case LN_StringExpressionKind::ToLowercase:
      return ToCaseString(ResolveStringExpression(expression.input0, "", context), false);
    case LN_StringExpressionKind::ZeroFill:
      return ZeroFillString(ResolveStringExpression(expression.input0, "", context),
                            ResolveIntExpression(expression.int_expr_index, 0, context));
    case LN_StringExpressionKind::Format:
      return FormatStringSlots(ResolveStringExpression(expression.input0, "", context),
                               ResolveStringExpression(expression.input1, "", context),
                               ResolveStringExpression(expression.input2, "", context),
                               ResolveStringExpression(expression.input3, "", context),
                               ResolveStringExpression(expression.input4, "", context));
    case LN_StringExpressionKind::KeyLoggerCharacter: {
      const bool only_characters = expression.int_expr_index != LN_INVALID_INDEX ?
                                       ResolveIntExpression(
                                           expression.int_expr_index, 1, context) != 0 :
                                       true;
      const KeyLoggerScanResult scan = ScanKeyLogger(m_snapshot.GetInputSnapshot(),
                                                     only_characters);
      return scan.character;
    }
    case LN_StringExpressionKind::KeyLoggerKeycode: {
      const bool only_characters = expression.int_expr_index != LN_INVALID_INDEX ?
                                       ResolveIntExpression(
                                           expression.int_expr_index, 1, context) != 0 :
                                       true;
      const KeyLoggerScanResult scan = ScanKeyLogger(m_snapshot.GetInputSnapshot(),
                                                     only_characters);
      return scan.keycode;
    }
    case LN_StringExpressionKind::InputBindingText: {
      const LN_InputMap &map = LN_InputMap::Get();
      /* Player 0's, like Automatic: their gamepad's own button names. */
      return map.ActionText(
          map.Find(ResolveStringExpression(expression.input0, std::string(), context)),
          LN_InputDeviceClass(std::clamp(ResolveIntExpression(expression.input1, 0, context),
                                         int32_t(LN_InputDeviceClass::Automatic),
                                         int32_t(LN_InputDeviceClass::Gamepad))),
          ResolveIntExpression(expression.input2, 0, context),
          map.PadFamilyOfUser(0));
    }
    case LN_StringExpressionKind::AnimationActionName: {
      KX_GameObject *target_object = nullptr;
      if (expression.input0 != LN_INVALID_INDEX) {
        target_object = ResolveObjectValue(
            ResolveValueExpression(expression.input0, MakeNoneValue(), context));
      }
      else if (m_gameObject != nullptr) {
        target_object = m_gameObject;
      }
      if (target_object == nullptr) {
        return fallback;
      }
      const int32_t layer = ResolveIntExpression(expression.int_expr_index, 0, context);
      return target_object->GetActionName(short(layer));
    }
    case LN_StringExpressionKind::MasterFolder: {
      const std::string folder_name = ResolveStringExpression(
          expression.input0, std::string(), context);
      std::filesystem::path path(KX_GetMainPath());
      if (path.empty()) {
        path = std::filesystem::current_path();
      }
      if (std::filesystem::is_regular_file(path)) {
        path = path.parent_path();
      }
      if (folder_name.empty()) {
        return path.string();
      }
      for (std::filesystem::path current = path; !current.empty(); current = current.parent_path())
      {
        if (current.filename() == folder_name) {
          return current.string();
        }
        if (current == current.parent_path()) {
          break;
        }
      }
      return fallback;
    }
    case LN_StringExpressionKind::ObjectID: {
      KX_GameObject *target_object = nullptr;
      if (expression.input0 != LN_INVALID_INDEX) {
        target_object = ResolveObjectValue(
            ResolveValueExpression(expression.input0, MakeNoneValue(), context));
      }
      else if (m_gameObject != nullptr) {
        target_object = m_gameObject;
      }
      if (target_object != nullptr) {
        return game_object_id_name(target_object);
      }
      return fallback;
    }
    case LN_StringExpressionKind::MaterialName: {
      blender::Material *material = ResolveMaterialByValue(
          this, ResolveValueExpression(expression.input0, MakeNoneValue(), context));
      return material ? std::string(material->id.name + 2) : fallback;
    }
    case LN_StringExpressionKind::ConstraintBreakName:
      if (const LN_RigidBodyConstraintBreakEvent *event = ResolveRigidBodyConstraintBreakEvent(
              expression.input0,
              expression.input1,
              expression.input2,
              expression.rigid_body_constraint_match_mode,
              context))
      {
        return event->constraint_name;
      }
      return fallback;
    case LN_StringExpressionKind::RigidBodyConstraintName: {
      KX_GameObject *target = m_gameObject;
      if (expression.value_expr_index != LN_INVALID_INDEX) {
        target = ResolveObjectValue(
            ResolveValueExpression(expression.value_expr_index, MakeNoneValue(), context));
      }
      if (!UsesJoltPhysics(target)) {
        return std::string();
      }
      const LN_RigidBodyConstraintMatchMode match_mode = ValidRigidBodyConstraintMatchMode(
          expression.rigid_body_constraint_match_mode);
      const std::string query = match_mode == LN_RigidBodyConstraintMatchMode::All ?
                                    std::string() :
                                    ResolveStringExpression(
                                        expression.input0, std::string(), context);
      const KX_GameObject::RigidBodyConstraintData *constraint = FindRigidBodyConstraintMatch(
          *target, query, match_mode);
      return constraint ? constraint->m_name : std::string();
    }
    case LN_StringExpressionKind::Count:
      break;
  }

  return fallback;
}

LN_Rotation LN_RuntimeTree::ResolveRotationExpression(const uint32_t expression_index,
                                                      const LN_Rotation &fallback,
                                                      const LN_TickContext &context)
{
  if (expression_index == LN_INVALID_INDEX || m_program == nullptr) {
    return fallback;
  }

  const std::vector<LN_VectorExpression> &expressions = m_program->GetVectorExpressions();
  if (expression_index >= expressions.size()) {
    return fallback;
  }

  const bool cacheable = context.tick_index != UINT64_MAX;
  if (cacheable) {
    if (m_rotationExpressionValues.size() != expressions.size()) {
      m_rotationExpressionValues.assign(expressions.size(), LN_Rotation::Identity());
      m_rotationExpressionTicks.assign(expressions.size(), UINT64_MAX);
    }
    if (m_rotationExpressionTicks[expression_index] == context.tick_index) {
      return m_rotationExpressionValues[expression_index];
    }
  }

  const LN_VectorExpression &expression = expressions[expression_index];
  const LN_RuntimeExpressionSemantics semantics = LN_GetRuntimeExpressionSemantics(
      expression.kind);
  ProfileExpressionEvaluation(semantics.fallback_requirements);
  RecordMissingSnapshotChannelsForExpression(semantics);
  LN_Rotation result = fallback;
  bool resolved = false;
  switch (expression.kind) {
    case LN_VectorExpressionKind::Constant:
      result = expression.output_type == LN_ValueType::Rotation ?
                   expression.rotation_value :
                   LN_Rotation::FromEulerXYZ(expression.vector_value);
      resolved = true;
      break;
    case LN_VectorExpressionKind::SnapshotWorldOrientation:
      if (expression.input0 == LN_INVALID_INDEX) {
        result = m_snapshot.GetObjectSnapshot().world_orientation;
        resolved = true;
      }
      else if (KX_GameObject *object = ResolveObjectValue(
                   ResolveValueExpression(expression.input0, MakeNoneValue(), context)))
      {
        result = LN_Rotation::FromMatrix(object->NodeGetWorldOrientation());
        resolved = true;
      }
      break;
    case LN_VectorExpressionKind::SnapshotLocalOrientation:
      if (expression.input0 == LN_INVALID_INDEX) {
        result = m_snapshot.GetObjectSnapshot().local_orientation;
        resolved = true;
      }
      else if (KX_GameObject *object = ResolveObjectValue(
                   ResolveValueExpression(expression.input0, MakeNoneValue(), context)))
      {
        result = LN_Rotation::FromMatrix(object->NodeGetLocalOrientation());
        resolved = true;
      }
      break;
    case LN_VectorExpressionKind::RuntimeTreeProperty:
      if (const LN_Value *value = GetTreePropertyValue(expression.property_ref_index)) {
        if (value->type == LN_ValueType::Rotation) {
          result = value->rotation_value;
          resolved = true;
        }
        else if (value->type == LN_ValueType::Vector) {
          result = LN_Rotation::FromEulerXYZ(value->vector_value);
          resolved = true;
        }
      }
      break;
    case LN_VectorExpressionKind::FromGenericValue: {
      const LN_Value value = ResolveValueExpression(expression.input0, MakeNoneValue(), context);
      if (value.type == LN_ValueType::Rotation) {
        result = value.rotation_value;
        resolved = true;
      }
      else {
        MT_Vector3 euler;
        if (ValueAsVector(value, euler)) {
          result = LN_Rotation::FromEulerXYZ(euler);
          resolved = true;
        }
      }
      break;
    }
    case LN_VectorExpressionKind::Combine: {
      const float x = ResolveFloatExpression(
          expression.input0, expression.vector_value.x(), context);
      const float y = ResolveFloatExpression(
          expression.input1, expression.vector_value.y(), context);
      const float z = ResolveFloatExpression(
          expression.input2, expression.vector_value.z(), context);
      result = LN_Rotation::FromEulerXYZ(MT_Vector3(x, y, z));
      resolved = true;
      break;
    }
    case LN_VectorExpressionKind::VectorToRotation:
      result = LN_Rotation::FromEulerXYZ(VectorToRotationEuler(
          ResolveVectorExpression(expression.input0, MT_Vector3(0.0f, 0.0f, 1.0f), context),
          ResolveVectorExpression(expression.input1, MT_Vector3(0.0f, 1.0f, 0.0f), context),
          int32_t(expression.property_ref_index)));
      resolved = true;
      break;
    case LN_VectorExpressionKind::BoneRotationToObjectRotation:
      result = LN_BoneRotationToObjectRotation(
          ResolveRotationExpression(expression.input0, LN_Rotation::Identity(), context));
      resolved = true;
      break;
    case LN_VectorExpressionKind::TweenRotationResult: {
      const float factor = ResolveTweenMappedFactor(expression.input0, context);
      if (expression.property_ref_index != 0) {
        const LN_Rotation from = ResolveRotationExpression(
            expression.input1, LN_Rotation::Identity(), context);
        const LN_Rotation to = ResolveRotationExpression(
            expression.input2, LN_Rotation::Identity(), context);
        result = from.Slerp(to, factor);
      }
      else {
        /* Euler mode is an explicit component-wise node contract. Preserve authored radians,
         * including turns outside the canonical [-pi, pi] representation, until after the
         * interpolation. */
        const MT_Vector3 from = ResolveVectorExpression(
            expression.input1, MT_Vector3(0.0f, 0.0f, 0.0f), context);
        const MT_Vector3 to = ResolveVectorExpression(
            expression.input2, MT_Vector3(0.0f, 0.0f, 0.0f), context);
        result = LN_Rotation::FromEulerXYZ(from + (to - from) * factor);
      }
      resolved = true;
      break;
    }
    default:
      result = LN_Rotation::FromEulerXYZ(
          ResolveVectorExpression(expression_index, fallback.ToEulerXYZ(), context));
      break;
  }

  /* Never cache a caller-provided fallback. The same shared expression may be consumed by
   * sockets with different legacy defaults in one tick. */
  if (cacheable && resolved) {
    m_rotationExpressionValues[expression_index] = result;
    m_rotationExpressionTicks[expression_index] = context.tick_index;
  }
  return result;
}

MT_Vector3 LN_RuntimeTree::ResolveVectorExpression(const uint32_t expression_index,
                                                   const MT_Vector3 &fallback,
                                                   const LN_TickContext &context)
{
  if (expression_index == LN_INVALID_INDEX || m_program == nullptr) {
    return fallback;
  }

  const std::vector<LN_VectorExpression> &expressions = m_program->GetVectorExpressions();
  if (expression_index >= expressions.size()) {
    return fallback;
  }

  const LN_VectorExpression &expression = expressions[expression_index];
  const LN_RuntimeExpressionSemantics semantics = LN_GetRuntimeExpressionSemantics(
      expression.kind);
  ProfileExpressionEvaluation(semantics.fallback_requirements);
  RecordMissingSnapshotChannelsForExpression(semantics);
  MT_Vector3 register_vector_value(0.0f, 0.0f, 0.0f);
  if (TryResolveRegisterVectorExpression(expression_index, context, register_vector_value)) {
    return register_vector_value;
  }
  if (context.use_register_expression_evaluator) {
    RecordRegisterExpressionFallback(LN_RuntimeExpressionFamily::Vector, expression_index);
  }
  LN_Snapshot snapshot;
  const LN_ObjectSnapshot *object_snapshot = &m_snapshot.GetObjectSnapshot();
  auto resolve_snapshot_object = [&]() -> const LN_ObjectSnapshot * {
    if (expression.input0 == LN_INVALID_INDEX) {
      return object_snapshot;
    }
    if (KX_GameObject *game_object = ResolveObjectValue(
            ResolveValueExpression(expression.input0, MakeNoneValue(), context)))
    {
      snapshot.Capture(game_object, nullptr);
      object_snapshot = &snapshot.GetObjectSnapshot();
      return object_snapshot;
    }
    return nullptr;
  };
  switch (expression.kind) {
    case LN_VectorExpressionKind::Constant:
      return expression.vector_value;
    case LN_VectorExpressionKind::SnapshotWorldPosition:
      return object_snapshot->world_position;
    case LN_VectorExpressionKind::SnapshotLocalPosition:
      return object_snapshot->local_position;
    case LN_VectorExpressionKind::SnapshotWorldOrientation:
      return object_snapshot->world_orientation.ToEulerXYZ();
    case LN_VectorExpressionKind::SnapshotLocalOrientation:
      return object_snapshot->local_orientation.ToEulerXYZ();
    case LN_VectorExpressionKind::SnapshotWorldScale:
      return object_snapshot->world_scale;
    case LN_VectorExpressionKind::SnapshotLocalScale:
      return object_snapshot->local_scale;
    case LN_VectorExpressionKind::SnapshotLinearVelocity:
      return object_snapshot->linear_velocity;
    case LN_VectorExpressionKind::SnapshotLocalLinearVelocity:
      return object_snapshot->local_linear_velocity;
    case LN_VectorExpressionKind::SnapshotAngularVelocity:
      return object_snapshot->angular_velocity;
    case LN_VectorExpressionKind::SnapshotLocalAngularVelocity:
      return object_snapshot->local_angular_velocity;
    case LN_VectorExpressionKind::SnapshotGravity:
      return m_snapshot.GetGravity();
    case LN_VectorExpressionKind::SnapshotCharacterGravity: {
      if (const LN_ObjectSnapshot *resolved_object_snapshot = resolve_snapshot_object()) {
        return resolved_object_snapshot->has_character ?
                   resolved_object_snapshot->character_gravity :
                   fallback;
      }
      return fallback;
    }
    case LN_VectorExpressionKind::SnapshotCharacterWalkDirection: {
      if (const LN_ObjectSnapshot *resolved_object_snapshot = resolve_snapshot_object()) {
        return resolved_object_snapshot->has_character ?
                   resolved_object_snapshot->character_walk_direction_world :
                   fallback;
      }
      return fallback;
    }
    case LN_VectorExpressionKind::SnapshotCharacterLocalWalkDirection: {
      if (const LN_ObjectSnapshot *resolved_object_snapshot = resolve_snapshot_object()) {
        return resolved_object_snapshot->has_character ?
                   resolved_object_snapshot->character_walk_direction_local :
                   fallback;
      }
      return fallback;
    }
    case LN_VectorExpressionKind::WindowResolution:
      if (RAS_ICanvas *canvas = ActiveCanvas()) {
        return MT_Vector3(float(canvas->GetWidth()), float(canvas->GetHeight()), 0.0f);
      }
      return fallback;
    case LN_VectorExpressionKind::WorldToScreen: {
      const LN_Value camera_value = ResolveValueExpression(
          expression.input1, MakeNoneValue(), context);
      KX_Camera *camera = ResolveCameraValue(camera_value);
      MT_Vector3 screen_position;
      if (ProjectWorldToScreen(
              camera,
              ResolveVectorExpression(expression.input0, expression.vector_value, context),
              screen_position))
      {
        return screen_position;
      }
      return fallback;
    }
    case LN_VectorExpressionKind::BoneHeadWorld: {
      KX_GameObject *armature_object = nullptr;
      if (expression.input0 == LN_INVALID_INDEX) {
        armature_object = m_snapshot.GetObjectSnapshot().object;
      }
      else {
        const LN_Value armature_value = ResolveValueExpression(
            expression.input0, MakeNoneValue(), context);
        armature_object = ResolveObjectValue(armature_value);
      }
      if (armature_object == nullptr) {
        return fallback;
      }
      blender::Object *ob = armature_object->GetBlenderObject();
      if (ob == nullptr || ob->type != blender::OB_ARMATURE) {
        return fallback;
      }
      blender::bArmature *armature = reinterpret_cast<blender::bArmature *>(ob->data);
      if (armature == nullptr) {
        return fallback;
      }
      const std::string bone_name = ResolveStringExpression(
          expression.input1, std::string(), context);
      if (bone_name.empty()) {
        return fallback;
      }
      blender::Bone *bone = BKE_armature_find_bone_name(armature, bone_name.c_str());
      if (bone == nullptr) {
        return fallback;
      }
      const MT_Transform world_xform = armature_object->NodeGetWorldTransform();
      return world_xform * MT_Vector3(bone->arm_head[0], bone->arm_head[1], bone->arm_head[2]);
    }
    case LN_VectorExpressionKind::BoneTailWorld: {
      KX_GameObject *armature_object = nullptr;
      if (expression.input0 == LN_INVALID_INDEX) {
        armature_object = m_snapshot.GetObjectSnapshot().object;
      }
      else {
        const LN_Value armature_value = ResolveValueExpression(
            expression.input0, MakeNoneValue(), context);
        armature_object = ResolveObjectValue(armature_value);
      }
      if (armature_object == nullptr) {
        return fallback;
      }
      blender::Object *ob = armature_object->GetBlenderObject();
      if (ob == nullptr || ob->type != blender::OB_ARMATURE) {
        return fallback;
      }
      blender::bArmature *armature = reinterpret_cast<blender::bArmature *>(ob->data);
      if (armature == nullptr) {
        return fallback;
      }
      const std::string bone_name = ResolveStringExpression(
          expression.input1, std::string(), context);
      if (bone_name.empty()) {
        return fallback;
      }
      blender::Bone *bone = BKE_armature_find_bone_name(armature, bone_name.c_str());
      if (bone == nullptr) {
        return fallback;
      }
      const MT_Transform world_xform = armature_object->NodeGetWorldTransform();
      return world_xform * MT_Vector3(bone->arm_tail[0], bone->arm_tail[1], bone->arm_tail[2]);
    }
    case LN_VectorExpressionKind::BoneHeadPoseWorld:
    case LN_VectorExpressionKind::BoneTailPoseWorld:
    case LN_VectorExpressionKind::BoneCenterPoseWorld: {
      KX_GameObject *armature_object = nullptr;
      if (expression.input0 == LN_INVALID_INDEX) {
        armature_object = m_snapshot.GetObjectSnapshot().object;
      }
      else {
        const LN_Value armature_value = ResolveValueExpression(
            expression.input0, MakeNoneValue(), context);
        armature_object = ResolveObjectValue(armature_value);
      }
      const std::string bone_name = ResolveStringExpression(
          expression.input1, std::string(), context);
      MT_Vector3 pose_position;
      BonePosePoint point = BonePosePoint::Head;
      if (expression.kind == LN_VectorExpressionKind::BoneTailPoseWorld) {
        point = BonePosePoint::Tail;
      }
      else if (expression.kind == LN_VectorExpressionKind::BoneCenterPoseWorld) {
        point = BonePosePoint::Center;
      }
      const LN_BonePosePositionSpace space = NormalizeBonePosePositionSpace(
          expression.property_ref_index);
      if (ResolveBonePosePosition(armature_object, bone_name, point, space, pose_position)) {
        return pose_position;
      }
      return fallback;
    }
    case LN_VectorExpressionKind::BonePoseLocation:
    case LN_VectorExpressionKind::BonePoseScale: {
      KX_GameObject *armature_object = nullptr;
      if (expression.input0 == LN_INVALID_INDEX) {
        armature_object = m_snapshot.GetObjectSnapshot().object;
      }
      else {
        const LN_Value armature_value = ResolveValueExpression(
            expression.input0, MakeNoneValue(), context);
        armature_object = ResolveObjectValue(armature_value);
      }
      const std::string bone_name = ResolveStringExpression(
          expression.input1, std::string(), context);
      MT_Vector3 value;
      if (expression.kind == LN_VectorExpressionKind::BonePoseScale) {
        return ResolveBonePoseScale(armature_object, bone_name, value) ? value : fallback;
      }
      const LN_BonePoseRotationSpace space = NormalizeBonePoseRotationSpace(
          expression.property_ref_index);
      return ResolveBonePoseLocation(armature_object, bone_name, space, value) ? value : fallback;
    }
    case LN_VectorExpressionKind::BoneCenterWorld: {
      KX_GameObject *armature_object = nullptr;
      if (expression.input0 == LN_INVALID_INDEX) {
        armature_object = m_snapshot.GetObjectSnapshot().object;
      }
      else {
        const LN_Value armature_value = ResolveValueExpression(
            expression.input0, MakeNoneValue(), context);
        armature_object = ResolveObjectValue(armature_value);
      }
      if (armature_object == nullptr) {
        return fallback;
      }
      blender::Object *ob = armature_object->GetBlenderObject();
      if (ob == nullptr || ob->type != blender::OB_ARMATURE) {
        return fallback;
      }
      blender::bArmature *armature = reinterpret_cast<blender::bArmature *>(ob->data);
      if (armature == nullptr) {
        return fallback;
      }
      const std::string bone_name = ResolveStringExpression(
          expression.input1, std::string(), context);
      if (bone_name.empty()) {
        return fallback;
      }
      blender::Bone *bone = BKE_armature_find_bone_name(armature, bone_name.c_str());
      if (bone == nullptr) {
        return fallback;
      }
      const MT_Transform world_xform = armature_object->NodeGetWorldTransform();
      const MT_Vector3 head = world_xform *
                              MT_Vector3(bone->arm_head[0], bone->arm_head[1], bone->arm_head[2]);
      const MT_Vector3 tail = world_xform *
                              MT_Vector3(bone->arm_tail[0], bone->arm_tail[1], bone->arm_tail[2]);
      return (head + tail) * 0.5f;
    }
    case LN_VectorExpressionKind::ScreenToWorld: {
      const LN_Value camera_value = ResolveValueExpression(
          expression.input0, MakeNoneValue(), context);
      KX_Camera *camera = ResolveCameraValue(camera_value);
      MT_Vector3 world_position;
      if (ProjectScreenToWorld(
              camera,
              ResolveFloatExpression(expression.input1, expression.vector_value.x(), context),
              ResolveFloatExpression(expression.input2, expression.vector_value.y(), context),
              ResolveFloatExpression(
                  expression.float_expr_index, expression.vector_value.z(), context),
              world_position))
      {
        return world_position;
      }
      return fallback;
    }
    case LN_VectorExpressionKind::RuntimeTreeProperty:
      if (const LN_Value *value = GetTreePropertyValue(expression.property_ref_index)) {
        return (value->type == LN_ValueType::Rotation) ? value->RotationEulerRadians() :
                                                         value->vector_value;
      }
      return fallback;
    case LN_VectorExpressionKind::FromGenericValue: {
      const LN_Value value = ResolveValueExpression(expression.input0, MakeNoneValue(), context);
      MT_Vector3 vector;
      if (ValueAsVector(value, vector)) {
        return vector;
      }
      return fallback;
    }
    case LN_VectorExpressionKind::BroadcastFloat: {
      const float scalar = ResolveFloatExpression(expression.float_expr_index, 0.0f, context);
      return MT_Vector3(scalar, scalar, scalar);
    }
    case LN_VectorExpressionKind::BroadcastInt: {
      const float scalar = float(ResolveIntExpression(expression.input0, 0, context));
      return MT_Vector3(scalar, scalar, scalar);
    }
    case LN_VectorExpressionKind::FromColor: {
      const MT_Vector4 color = ResolveColorExpression(
          expression.input0, MT_Vector4(0.0f, 0.0f, 0.0f, 1.0f), context);
      return MT_Vector3(color.x(), color.y(), color.z());
    }
    case LN_VectorExpressionKind::CursorPosition: {
      const LN_MouseSnapshot &mouse = m_snapshot.GetInputSnapshot().GetMouse();
      float y = mouse.normalized_y;
      if (expression.property_ref_index != 0) {
        y = 1.0f - y;
      }
      return MT_Vector3(mouse.normalized_x, y, 0.0f);
    }
    case LN_VectorExpressionKind::CursorMovement: {
      const LN_MouseSnapshot &mouse = m_snapshot.GetInputSnapshot().GetMouse();
      return MT_Vector3(float(mouse.delta_x), float(mouse.delta_y), 0.0f);
    }
    case LN_VectorExpressionKind::GamepadStick: {
      const int32_t gamepad_index = ResolveIntExpression(expression.input0, 0, context);
      const float threshold = std::max(0.0f,
                                       ResolveFloatExpression(expression.input1, 0.1f, context));
      const float sensitivity = ResolveFloatExpression(
          expression.float_expr_index, expression.float_value, context);
      const LN_GamepadSnapshot *gamepad = m_snapshot.GetInputSnapshot().GetGamepad(gamepad_index);
      if (gamepad == nullptr || !gamepad->connected) {
        return fallback;
      }

      const int32_t invert_flags = int32_t(expression.property_ref_index);
      const int32_t axis_offset = (expression.input2 == 1u) ? 2 : 0;
      float x = NormalizeGamepadAxisValue(gamepad->axes[size_t(axis_offset)]);
      float y = -NormalizeGamepadAxisValue(gamepad->axes[size_t(axis_offset + 1)]);
      if (invert_flags & 1) {
        x = -x;
      }
      if (invert_flags & 2) {
        y = -y;
      }
      if (std::fabs(x) < threshold) {
        x = 0.0f;
      }
      if (std::fabs(y) < threshold) {
        y = 0.0f;
      }
      return MT_Vector3(x * sensitivity, y * sensitivity, 0.0f);
    }
    case LN_VectorExpressionKind::InputActionVector: {
      const LN_InputActionState *state = m_snapshot.GetInputActionState(
          LN_InputMap::Get().Find(
              ResolveStringExpression(expression.input1, std::string(), context)),
          ResolveIntExpression(expression.input0, 0, context),
          expression.bool_value);
      return state != nullptr ? state->vector : MT_Vector3(0.0f, 0.0f, 0.0f);
    }
    case LN_VectorExpressionKind::TweenVectorResult: {
      const float tween_factor = ResolveTweenMappedFactor(expression.input0, context);
      const MT_Vector3 from_value = ResolveVectorExpression(
          expression.input1, MT_Vector3(0.0f, 0.0f, 0.0f), context);
      const MT_Vector3 to_value = ResolveVectorExpression(
          expression.input2, MT_Vector3(1.0f, 1.0f, 1.0f), context);
      return from_value + (to_value - from_value) * tween_factor;
    }
    case LN_VectorExpressionKind::TweenRotationResult: {
      if (expression.property_ref_index != 0) {
        return ResolveRotationExpression(expression_index, LN_Rotation::Identity(), context)
            .ToEulerXYZ();
      }
      const float tween_factor = ResolveTweenMappedFactor(expression.input0, context);
      const MT_Vector3 from_value = ResolveVectorExpression(
          expression.input1, MT_Vector3(0.0f, 0.0f, 0.0f), context);
      const MT_Vector3 to_value = ResolveVectorExpression(
          expression.input2, MT_Vector3(0.0f, 0.0f, 0.0f), context);
      return InterpolateRotationEuler(from_value, to_value, tween_factor);
    }
    case LN_VectorExpressionKind::PhysicsQueryPoint: {
      const LN_PhysicsQueryResult &query =
          ResolveQueryExpression(expression.input0, context).physics_query;
      return query.hit ? query.hit_position : MT_Vector3(0.0f, 0.0f, 0.0f);
    }
    case LN_VectorExpressionKind::PhysicsQueryNormal: {
      const LN_PhysicsQueryResult &query =
          ResolveQueryExpression(expression.input0, context).physics_query;
      return query.hit ? query.hit_normal : MT_Vector3(0.0f, 0.0f, 0.0f);
    }
    case LN_VectorExpressionKind::UIScrollOffset:
      return ResolveQueryExpression(expression.input0, context).ui_scroll_offset;
    case LN_VectorExpressionKind::UIScrollNormalized:
      return ResolveQueryExpression(expression.input0, context).ui_scroll_normalized;
    case LN_VectorExpressionKind::PhysicsQueryCastPosition:
      return ResolveQueryExpression(expression.input0, context).physics_query.cast_position;
    case LN_VectorExpressionKind::PhysicsQueryDirection:
      return ResolveQueryExpression(expression.input0, context).physics_query.ray_direction;
    case LN_VectorExpressionKind::PhysicsQueryEndPoint:
      return ResolveQueryExpression(expression.input0, context).physics_query.end_position;
    case LN_VectorExpressionKind::PhysicsQueryUV: {
      const MT_Vector2 uv =
          ResolveQueryExpression(expression.input0, context).physics_query.hit_uv;
      return MT_Vector3(uv.x(), uv.y(), 0.0f);
    }
    case LN_VectorExpressionKind::QueryVectorAverage:
      return ResolveQueryExpression(expression.input0, context).vector_average;
    case LN_VectorExpressionKind::MotionPosition:
    case LN_VectorExpressionKind::MotionVelocity:
    case LN_VectorExpressionKind::MotionDirection:
    case LN_VectorExpressionKind::MotionPredictedPosition: {
      if (expression.property_ref_index == LN_INVALID_INDEX) {
        return fallback;
      }
      const FlowControlRuntimeState *state = FindFlowControlState(expression.property_ref_index);
      if (state == nullptr) {
        return fallback;
      }
      if (expression.kind == LN_VectorExpressionKind::MotionPosition) {
        return state->motion_position;
      }
      if (expression.kind == LN_VectorExpressionKind::MotionVelocity) {
        return state->motion_velocity;
      }
      if (expression.kind == LN_VectorExpressionKind::MotionDirection) {
        return state->motion_direction;
      }
      return state->motion_predicted_position;
    }
    case LN_VectorExpressionKind::Add:
      return ResolveVectorExpression(expression.input0, MT_Vector3(0.0f, 0.0f, 0.0f), context) +
             ResolveVectorExpression(expression.input1, MT_Vector3(0.0f, 0.0f, 0.0f), context);
    case LN_VectorExpressionKind::Subtract:
      return ResolveVectorExpression(expression.input0, MT_Vector3(0.0f, 0.0f, 0.0f), context) -
             ResolveVectorExpression(expression.input1, MT_Vector3(0.0f, 0.0f, 0.0f), context);
    case LN_VectorExpressionKind::Multiply: {
      const MT_Vector3 a = ResolveVectorExpression(
          expression.input0, MT_Vector3(0.0f, 0.0f, 0.0f), context);
      const MT_Vector3 b = ResolveVectorExpression(
          expression.input1, MT_Vector3(0.0f, 0.0f, 0.0f), context);
      return MT_Vector3(a.x() * b.x(), a.y() * b.y(), a.z() * b.z());
    }
    case LN_VectorExpressionKind::Divide: {
      const MT_Vector3 a = ResolveVectorExpression(
          expression.input0, MT_Vector3(0.0f, 0.0f, 0.0f), context);
      const MT_Vector3 b = ResolveVectorExpression(
          expression.input1, MT_Vector3(0.0f, 0.0f, 0.0f), context);
      return MT_Vector3(std::fabs(b.x()) <= 1.0e-20f ? 0.0f : a.x() / b.x(),
                        std::fabs(b.y()) <= 1.0e-20f ? 0.0f : a.y() / b.y(),
                        std::fabs(b.z()) <= 1.0e-20f ? 0.0f : a.z() / b.z());
    }
    case LN_VectorExpressionKind::Absolute: {
      const MT_Vector3 vector = ResolveVectorExpression(
          expression.input0, MT_Vector3(0.0f, 0.0f, 0.0f), context);
      return MT_Vector3(std::fabs(vector.x()), std::fabs(vector.y()), std::fabs(vector.z()));
    }
    case LN_VectorExpressionKind::Minimum: {
      const MT_Vector3 a = ResolveVectorExpression(
          expression.input0, MT_Vector3(0.0f, 0.0f, 0.0f), context);
      const MT_Vector3 b = ResolveVectorExpression(
          expression.input1, MT_Vector3(0.0f, 0.0f, 0.0f), context);
      return MT_Vector3(std::min(a.x(), b.x()), std::min(a.y(), b.y()), std::min(a.z(), b.z()));
    }
    case LN_VectorExpressionKind::Maximum: {
      const MT_Vector3 a = ResolveVectorExpression(
          expression.input0, MT_Vector3(0.0f, 0.0f, 0.0f), context);
      const MT_Vector3 b = ResolveVectorExpression(
          expression.input1, MT_Vector3(0.0f, 0.0f, 0.0f), context);
      return MT_Vector3(std::max(a.x(), b.x()), std::max(a.y(), b.y()), std::max(a.z(), b.z()));
    }
    case LN_VectorExpressionKind::Scale:
      return ResolveVectorExpression(expression.input0, MT_Vector3(0.0f, 0.0f, 0.0f), context) *
             ResolveFloatExpression(expression.float_expr_index, expression.float_value, context);
    case LN_VectorExpressionKind::Normalize: {
      const MT_Vector3 vector = ResolveVectorExpression(
          expression.input0, MT_Vector3(0.0f, 0.0f, 0.0f), context);
      const float length = vector.length();
      return length <= 1.0e-20f ? MT_Vector3(0.0f, 0.0f, 0.0f) : vector * (1.0f / length);
    }
    case LN_VectorExpressionKind::Resize: {
      const MT_Vector3 vector = ResolveVectorExpression(
          expression.input0, MT_Vector3(0.0f, 0.0f, 0.0f), context);
      if (expression.float_value >= 3.0f) {
        return vector;
      }
      return MT_Vector3(vector.x(), vector.y(), 0.0f);
    }
    case LN_VectorExpressionKind::RotateAroundAxis: {
      const MT_Vector3 origin = ResolveVectorExpression(
          expression.input0, MT_Vector3(0.0f, 0.0f, 0.0f), context);
      const MT_Vector3 pivot = ResolveVectorExpression(
          expression.input1, MT_Vector3(0.0f, 0.0f, 0.0f), context);
      const int32_t rotate_mode = int32_t(expression.property_ref_index) / 4;
      const int32_t rotate_axis = int32_t(expression.property_ref_index) % 4;
      if (rotate_mode == 3) {
        const LN_Rotation rotation = ResolveRotationExpression(
            expression.input2, LN_Rotation::Identity(), context);
        return pivot + rotation.ToMatrix() * (origin - pivot);
      }
      MT_Vector3 axis(0.0f, 0.0f, 1.0f);
      if (rotate_mode == 1) {
        axis = rotate_axis == 0 ? MT_Vector3(1.0f, 0.0f, 0.0f) :
               rotate_axis == 1 ? MT_Vector3(0.0f, 1.0f, 0.0f) :
                                  MT_Vector3(0.0f, 0.0f, 1.0f);
      }
      else if (rotate_mode == 2) {
        axis = ResolveVectorExpression(expression.input2, MT_Vector3(0.0f, 0.0f, 1.0f), context);
      }
      const float axis_length = axis.length();
      if (axis_length <= 1.0e-20f) {
        return origin;
      }
      axis *= 1.0f / axis_length;
      const float angle = ResolveFloatExpression(
          expression.float_expr_index, expression.float_value, context);
      const MT_Vector3 relative = origin - pivot;
      const float cosine = std::cos(angle);
      const float sine = std::sin(angle);
      return pivot + relative * cosine + axis.cross(relative) * sine +
             axis * (axis.dot(relative) * (1.0f - cosine));
    }
    case LN_VectorExpressionKind::VectorToRotation:
      return VectorToRotationEuler(
          ResolveVectorExpression(expression.input0, MT_Vector3(0.0f, 0.0f, 1.0f), context),
          ResolveVectorExpression(expression.input1, MT_Vector3(0.0f, 1.0f, 0.0f), context),
          int32_t(expression.property_ref_index));
    case LN_VectorExpressionKind::BoneRotationToObjectRotation:
      return LN_BoneRotationToObjectRotation(
                 ResolveRotationExpression(expression.input0, LN_Rotation::Identity(), context))
          .ToEulerXYZ();
    case LN_VectorExpressionKind::AxisVector: {
      KX_GameObject *target_object = nullptr;
      if (expression.input0 != LN_INVALID_INDEX) {
        target_object = ResolveObjectValue(
            ResolveValueExpression(expression.input0, MakeNoneValue(), context));
      }
      else if (m_gameObject != nullptr) {
        target_object = m_gameObject;
      }
      if (target_object == nullptr) {
        return fallback;
      }
      const MT_Matrix3x3 orientation = target_object->NodeGetWorldOrientation();
      const int32_t axis = int32_t(expression.property_ref_index);
      MT_Vector3 vector = axis == 0 || axis == 3 ? orientation * MT_Vector3(1.0f, 0.0f, 0.0f) :
                          axis == 1 || axis == 4 ? orientation * MT_Vector3(0.0f, 1.0f, 0.0f) :
                                                   orientation * MT_Vector3(0.0f, 0.0f, 1.0f);
      if (axis >= 3) {
        vector = -vector;
      }
      return vector;
    }
    case LN_VectorExpressionKind::RigidBodyAttribute: {
      PHY_IPhysicsController *controller = ResolveRigidBodyAttributeController(expression.input0,
                                                                               context);
      if (controller == nullptr) {
        return MT_Vector3(0.0f, 0.0f, 0.0f);
      }
      return ReadRigidBodyAxisLockAttribute(*controller, expression.bool_value);
    }
    case LN_VectorExpressionKind::Combine:
      return MT_Vector3(
          ResolveFloatExpression(expression.input0, expression.vector_value.x(), context),
          ResolveFloatExpression(expression.input1, expression.vector_value.y(), context),
          ResolveFloatExpression(expression.input2, expression.vector_value.z(), context));
    case LN_VectorExpressionKind::CollisionHitPoint:
      if (const CollisionResult &collision_result = ResolveCollisionResult(
              expression.input0,
              expression.input1,
              expression.input2,
              context,
              CollisionResult::Detail::Contacts);
          collision_result.hit)
      {
        return collision_result.hit_point;
      }
      return fallback;
    case LN_VectorExpressionKind::CollisionHitNormal:
      if (const CollisionResult &collision_result = ResolveCollisionResult(
              expression.input0,
              expression.input1,
              expression.input2,
              context,
              CollisionResult::Detail::Contacts);
          collision_result.hit)
      {
        return collision_result.hit_normal;
      }
      return fallback;
    case LN_VectorExpressionKind::ConstraintBreakAnchorPosition:
      if (const LN_RigidBodyConstraintBreakEvent *event = ResolveRigidBodyConstraintBreakEvent(
              expression.input0,
              expression.input1,
              expression.input2,
              expression.rigid_body_constraint_match_mode,
              context))
      {
        return event->anchor_position;
      }
      return fallback;
    case LN_VectorExpressionKind::EvaluateCurveAtFactor: {
      KX_GameObject *curve_object = nullptr;
      if (expression.input0 == LN_INVALID_INDEX) {
        curve_object = m_snapshot.GetObjectSnapshot().object;
      }
      else {
        const LN_Value curve_value = ResolveValueExpression(
            expression.input0, MakeNoneValue(), context);
        curve_object = ResolveObjectValue(curve_value);
      }
      if (curve_object == nullptr) {
        return fallback;
      }
      blender::Object *ob = curve_object->GetBlenderObject();
      if (ob == nullptr) {
        return fallback;
      }
      const float factor = ResolveFloatExpression(expression.float_expr_index, 0.0f, context);
      float path_position[3];
      if (!BKE_where_on_path(ob, factor, path_position, nullptr, nullptr, nullptr, nullptr)) {
        return fallback;
      }
      const MT_Transform world_xform = curve_object->NodeGetWorldTransform();
      return world_xform * MT_Vector3(path_position[0], path_position[1], path_position[2]);
    }
    case LN_VectorExpressionKind::InstructionNextPoint: {
      const uint32_t state_index = InstructionStateIndex(context.event, expression.input0);
      if (state_index == LN_INVALID_INDEX || state_index >= m_instructionStates.size()) {
        return fallback;
      }
      return m_instructionStates[state_index].next_path_point;
    }
    case LN_VectorExpressionKind::Random: {
      const MT_Vector3 axes = ResolveVectorExpression(
          expression.input0, MT_Vector3(1.0f, 1.0f, 1.0f), context);
      return MT_Vector3(
          axes.x() != 0.0f ?
              HashToUnitFloat((uint64_t(expression_index) << 32) ^ context.tick_index ^ 0x13579u) :
              0.0f,
          axes.y() != 0.0f ?
              HashToUnitFloat((uint64_t(expression_index) << 32) ^ context.tick_index ^ 0x24680u) :
              0.0f,
          axes.z() != 0.0f ? HashToUnitFloat((uint64_t(expression_index) << 32) ^
                                             context.tick_index ^ 0xabcdefu) :
                             0.0f);
    }
    case LN_VectorExpressionKind::Count:
      break;
  }

  return fallback;
}

MT_Vector4 LN_RuntimeTree::ResolveColorExpression(const uint32_t expression_index,
                                                  const MT_Vector4 &fallback,
                                                  const LN_TickContext &context)
{
  if (expression_index == LN_INVALID_INDEX || m_program == nullptr) {
    return fallback;
  }

  const std::vector<LN_ColorExpression> &expressions = m_program->GetColorExpressions();
  if (expression_index >= expressions.size()) {
    return fallback;
  }

  const LN_ColorExpression &expression = expressions[expression_index];
  const LN_RuntimeExpressionSemantics semantics = LN_GetRuntimeExpressionSemantics(
      expression.kind);
  ProfileExpressionEvaluation(semantics.fallback_requirements);
  RecordMissingSnapshotChannelsForExpression(semantics);
  MT_Vector4 register_color_value(0.0f, 0.0f, 0.0f, 1.0f);
  if (TryResolveRegisterColorExpression(expression_index, context, register_color_value)) {
    return register_color_value;
  }
  if (context.use_register_expression_evaluator) {
    RecordRegisterExpressionFallback(LN_RuntimeExpressionFamily::Color, expression_index);
  }
  switch (expression.kind) {
    case LN_ColorExpressionKind::Constant:
      return expression.color_value;
    case LN_ColorExpressionKind::SnapshotObjectColor:
      if (expression.input0 != LN_INVALID_INDEX) {
        if (KX_GameObject *game_object = ResolveObjectValue(
                ResolveValueExpression(expression.input0, MakeNoneValue(), context)))
        {
          LN_Snapshot snapshot;
          snapshot.Capture(game_object, nullptr);
          return snapshot.GetObjectSnapshot().object_color;
        }
        return fallback;
      }
      return m_snapshot.GetObjectSnapshot().object_color;
    case LN_ColorExpressionKind::SnapshotLightColor:
      if (expression.input0 != LN_INVALID_INDEX) {
        if (KX_GameObject *game_object = ResolveObjectValue(
                ResolveValueExpression(expression.input0, MakeNoneValue(), context)))
        {
          LN_Snapshot snapshot;
          snapshot.Capture(game_object, nullptr);
          return snapshot.GetObjectSnapshot().light_color;
        }
        return fallback;
      }
      return m_snapshot.GetObjectSnapshot().light_color;
    case LN_ColorExpressionKind::RuntimeTreeProperty:
      if (const LN_Value *value = GetTreePropertyValue(expression.property_ref_index)) {
        return value->color_value;
      }
      return fallback;
    case LN_ColorExpressionKind::Combine:
      return MT_Vector4(
          ResolveFloatExpression(expression.input0, expression.color_value.x(), context),
          ResolveFloatExpression(expression.input1, expression.color_value.y(), context),
          ResolveFloatExpression(expression.input2, expression.color_value.z(), context),
          ResolveFloatExpression(expression.input3, expression.color_value.w(), context));
    case LN_ColorExpressionKind::FromGenericValue: {
      MT_Vector4 value;
      return ValueAsColor(ResolveValueExpression(expression.input0, MakeNoneValue(), context),
                          value) ?
                 value :
                 fallback;
    }
    case LN_ColorExpressionKind::Count:
      break;
  }

  return fallback;
}

uint64_t LN_RuntimeTree::CommandSortKey(uint32_t /*instruction_index*/,
                                        uint32_t /*command_sequence*/) const
{
  /* The command buffer preserves record order within one runtime tree. Keep this key to the
   * stable tree identity so control-flow iteration order is never re-sorted by static
   * instruction index. */
  return (static_cast<uint64_t>(m_sceneObjectIndex) << 32) |
         static_cast<uint64_t>(m_appliedTreeIndex);
}
