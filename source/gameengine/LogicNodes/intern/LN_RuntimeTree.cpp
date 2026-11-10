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
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <type_traits>
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
#include "BKE_layer.hh"
#include "BKE_scene.hh"
#include "DEG_depsgraph_query.hh"
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
#include "DNA_mesh_types.h"
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
#include "LN_MeshRays.h"
#include "LN_Performance.h"
#include "LN_Random.h"
#include "LN_RuntimeSemantics.h"
#include "LN_TreeBatch.h"
#include "LN_LeanReader.h"
#include "LN_StrictDump.h"
#include "LN_TypeConversion.h"
#include "LN_Columns.h"
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
static constexpr int32_t LN_RAYCAST_MAX_COLLISION_MASK = (1 << 10) - 1;
static constexpr int32_t LN_RAYCAST_FROM_MESH_MAX_RAYS = 256;

/** `stride`: steps between two runs of the route (LN_TickContext::phase_tick_stride), so a route
 * of Every N Steps that ran N steps ago skipped nothing. */
static constexpr bool PhaseTickWasSkipped(const uint64_t previous_tick,
                                          const uint64_t current_tick,
                                          const uint32_t stride = 1)
{
  return previous_tick != UINT64_MAX && current_tick > previous_tick &&
         current_tick - previous_tick > std::max<uint64_t>(stride, 1);
}

static constexpr bool IsContinuousPhaseTick(const uint64_t previous_tick,
                                            const uint64_t current_tick,
                                            const uint32_t stride = 1)
{
  return previous_tick != UINT64_MAX && !PhaseTickWasSkipped(previous_tick, current_tick, stride);
}

static_assert(!PhaseTickWasSkipped(0, 1));
static_assert(PhaseTickWasSkipped(0, 2));
static_assert(!PhaseTickWasSkipped(0, 4, 4));
static_assert(PhaseTickWasSkipped(0, 5, 4));
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

/* Tests (plan A15): 0 turns the per-tick memo of pure expressions off. */
static bool g_memo_enabled = true;
/* The lean reader's memo (plan phase A33): UPBGE_LN_LEAN_MEMO 0 / off = none, unset / 1 / on = the
 * expressions the lowering marked (LN_LeanProgram::memo_*; the default since A33's gate). Tests
 * set it with SetLeanMemoForTests (-1: the environment again). */
static bool lean_memo_from_environment()
{
  const char *value = std::getenv("UPBGE_LN_LEAN_MEMO");
  if (value == nullptr || value[0] == '\0') {
    return true;
  }
  return !(std::strcmp(value, "0") == 0 || std::strcmp(value, "off") == 0);
}
/* Read once at start-up (a plain static: no guard to check at every read). */
static const bool g_lean_memo_environment = lean_memo_from_environment();
static int g_lean_memo_override = -1;
static inline bool lean_memo_enabled()
{
  return g_lean_memo_override >= 0 ? g_lean_memo_override != 0 : g_lean_memo_environment;
}
/* Tests (plan A13): where the expression reads of a kind are recorded; null records nothing. */
static std::vector<LN_RuntimeTree::ReadTraceEntry> *g_read_trace = nullptr;
static inline void NoteExpressionRead(const char family, const uint32_t expression_index)
{
  if (g_read_trace != nullptr) {
    g_read_trace->push_back({family, expression_index});
  }
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

/**
 * A game mesh's vertex positions as the game started: the converter gave Blender vertex `vert`
 * its copies in `m_sharedvertex_map[vert]` (every copy has the same position).
 */
static bool GameMeshStartPosition(const RAS_MeshObject &game_mesh,
                                  const int vert,
                                  blender::float3 &r_position)
{
  if (vert < 0 || size_t(vert) >= game_mesh.m_sharedvertex_map.size()) {
    return false;
  }
  const std::vector<RAS_MeshObject::SharedVertex> &copies = game_mesh.m_sharedvertex_map[vert];
  if (copies.empty()) {
    return false;
  }
  const RAS_MeshObject::SharedVertex &first = copies.front();
  if (!first.m_darray || first.m_offset < 0 ||
      size_t(first.m_offset) >= first.m_darray->GetVertexCount())
  {
    return false;
  }
  const RAS_IVertex *vertex = first.m_darray->GetVertex(uint32_t(first.m_offset));
  if (!vertex) {
    return false;
  }
  const float *xyz = vertex->getXYZ();
  r_position = blender::float3(xyz[0], xyz[1], xyz[2]);
  return std::isfinite(r_position.x) && std::isfinite(r_position.y) &&
         std::isfinite(r_position.z);
}

/** The positions a mesh ray block reads: the game mesh's start shape, or Blender's own. */
struct LN_MeshRayPositions {
  const RAS_MeshObject *game_mesh = nullptr;
  blender::Span<blender::float3> blender_positions;
  bool start_shape = false;

  bool operator()(const int vert, blender::float3 &r_position) const
  {
    if (start_shape) {
      return GameMeshStartPosition(*game_mesh, vert, r_position);
    }
    if (vert < 0 || vert >= blender_positions.size()) {
      return false;
    }
    r_position = blender_positions[vert];
    return true;
  }
};

/** The game scene's evaluated copy of `object` (the graph the engine draws), else `object`. */
static blender::Object *EvaluatedBlenderObject(KX_Scene *scene, blender::Object *object)
{
  blender::Scene *blender_scene = scene ? scene->GetBlenderScene() : nullptr;
  blender::ViewLayer *view_layer = blender_scene ?
                                       blender::BKE_view_layer_default_view(blender_scene) :
                                       nullptr;
  blender::Depsgraph *depsgraph = view_layer ?
                                      blender::BKE_scene_get_depsgraph(blender_scene, view_layer) :
                                      nullptr;
  if (depsgraph != nullptr) {
    if (blender::Object *evaluated = blender::DEG_get_evaluated(depsgraph, object)) {
      return evaluated;
    }
  }
  return object;
}

/** Whether a game mesh was converted from a Blender mesh with this topology: the converter added
 * one vertex slot per Blender vertex and every triangle of `corner_tris`. */
static bool GameMeshMatchesBlenderMesh(RAS_MeshObject &game_mesh, const blender::Mesh &mesh)
{
  return mesh.verts_num == int64_t(game_mesh.m_sharedvertex_map.size()) &&
         int64_t(mesh.corners_num) - 2 * int64_t(mesh.faces_num) ==
             int64_t(game_mesh.NumPolygons());
}

/**
 * The Blender mesh whose faces and vertices give a game mesh's rays: the evaluated mesh of the
 * object the game mesh was converted from (as the converter and Jolt's collision shape read it),
 * else the game mesh's own Blender mesh. While its topology is the game mesh's, `r_start_shape`
 * is set: positions then come from the game mesh, the shape when the game started, so an armature
 * or shape keys never move the rays and no normal of a bent mesh is computed. A mesh whose
 * topology changed since (geometry nodes) gives its own current positions.
 */
/** Which Blender data a mesh ray block's topology came from: the session UID of the object (its
 * evaluated mesh) or of the mesh itself. */
struct LN_MeshRaySourceId {
  uint32_t session_uid = 0;
  bool is_object = false;
};

static const blender::Mesh *MeshRaySourceOf(KX_Scene *scene,
                                            RAS_MeshObject &game_mesh,
                                            bool &r_start_shape,
                                            LN_MeshRaySourceId &r_source_id)
{
  const blender::Mesh *candidates[2] = {nullptr, game_mesh.GetOrigMesh()};
  blender::Object *object = game_mesh.GetOriginalObject();
  if (object != nullptr && object->type == blender::OB_MESH) {
    const blender::Object *evaluated = EvaluatedBlenderObject(scene, object);
    if (evaluated != nullptr && evaluated->type == blender::OB_MESH) {
      candidates[0] = reinterpret_cast<const blender::Mesh *>(evaluated->data);
    }
  }
  const auto source_id_of = [&](const int candidate) {
    if (candidate == 0) {
      return LN_MeshRaySourceId{object->id.session_uid, true};
    }
    return LN_MeshRaySourceId{candidates[1]->id.session_uid, false};
  };
  for (int candidate = 0; candidate < 2; candidate++) {
    const blender::Mesh *mesh = candidates[candidate];
    if (mesh != nullptr && GameMeshMatchesBlenderMesh(game_mesh, *mesh)) {
      r_start_shape = true;
      r_source_id = source_id_of(candidate);
      return mesh;
    }
  }
  r_start_shape = false;
  const int candidate = candidates[0] != nullptr ? 0 : 1;
  if (candidates[candidate] != nullptr) {
    r_source_id = source_id_of(candidate);
  }
  return candidates[candidate];
}

/**
 * Raycast from Mesh's samples in the source object's mesh space: Blender faces (centre, face
 * normal) or vertices (vertex normal) of every game mesh of the object, in order, Max Rays at most
 * (LN_MeshRays.h). Only the picked elements are read. A non-empty `vertex_group` keeps the members
 * of that group only (found once per mesh in `groups`, every time without it).
 */
static std::vector<LN_MeshRaySample> SampleMeshRays(KX_GameObject &source_object,
                                                    const LN_MeshRayDomain domain,
                                                    const size_t max_rays,
                                                    const std::string &vertex_group,
                                                    LN_MeshRayGroupCache *groups)
{
  struct Block {
    LN_MeshRayPositions positions;
    LN_MeshRayGeometry geometry;
    /** The group's members (element indices); null: every element. */
    std::shared_ptr<const std::vector<int>> members;
    uint64_t begin = 0;
    uint64_t end = 0;
  };
  const bool faces = domain == LN_MeshRayDomain::Faces;
  /* Reserved once: each block's geometry refers to its own `positions`. */
  std::vector<Block> blocks;
  blocks.reserve(source_object.GetMeshCount());
  uint64_t candidate_count = 0;
  for (unsigned int mesh_index = 0; mesh_index < source_object.GetMeshCount(); mesh_index++) {
    RAS_MeshObject *game_mesh = source_object.GetMesh(mesh_index);
    if (!game_mesh) {
      continue;
    }
    bool start_shape = false;
    LN_MeshRaySourceId source_id;
    const blender::Mesh *mesh = MeshRaySourceOf(
        source_object.GetScene(), *game_mesh, start_shape, source_id);
    if (!mesh) {
      continue;
    }
    std::shared_ptr<const std::vector<int>> members;
    if (!vertex_group.empty()) {
      if (groups != nullptr) {
        members = groups->Members(source_id.session_uid,
                                  source_id.is_object,
                                  *mesh,
                                  vertex_group,
                                  faces,
                                  source_object.GetName());
      }
      else {
        LN_MeshRayGroupCache once;
        members = once.Members(0, false, *mesh, vertex_group, faces, source_object.GetName());
      }
      if (!members) {
        continue;
      }
    }
    const int64_t count = members ? int64_t(members->size()) :
                          faces   ? int64_t(mesh->faces_num) :
                                    int64_t(mesh->verts_num);
    if (count <= 0) {
      continue;
    }
    Block &block = blocks.emplace_back();
    block.members = std::move(members);
    block.positions.game_mesh = game_mesh;
    block.positions.start_shape = start_shape;
    if (!start_shape) {
      block.positions.blender_positions = mesh->vert_positions();
    }
    block.geometry.faces = mesh->faces();
    block.geometry.corner_verts = mesh->corner_verts();
    if (domain == LN_MeshRayDomain::Vertices) {
      block.geometry.vert_to_face_map = mesh->vert_to_face_map();
    }
    block.geometry.position = block.positions;
    block.begin = candidate_count;
    block.end = candidate_count + uint64_t(count);
    candidate_count = block.end;
  }

  return LN_PickMeshRaySamples(
      candidate_count, max_rays, [&](const uint64_t ordinal, LN_MeshRaySample &r_sample) {
        const auto block = std::upper_bound(
            blocks.begin(), blocks.end(), ordinal, [](const uint64_t value, const Block &item) {
              return value < item.end;
            });
        if (block == blocks.end() || ordinal < block->begin) {
          return false;
        }
        const uint64_t local_index = ordinal - block->begin;
        const int element = block->members ? (*block->members)[size_t(local_index)] :
                                             int(local_index);
        return faces ? LN_MeshFaceRaySample(block->geometry, element, r_sample) :
                       LN_MeshVertexRaySample(block->geometry, element, r_sample);
      });
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

/** A random node's bits in a tree now (LN_Random.h): the game's seed, the node, its event's tick
 * count, its Seed input and, unless the node is Same for All, the tree's stream. The column
 * kernels compute the same through LN_RandomNodePart / LN_RandomBits. */
static uint64_t RandomNodeBits(const uint32_t identity,
                               const int32_t seed,
                               const uint32_t salt,
                               const uint64_t tick,
                               const uint64_t stream)
{
  return LN_RandomBits(LN_RandomNodePart(LN_GetRandomGameSeed(), identity, tick),
                       seed,
                       (identity & LN_RANDOM_SAME_FOR_ALL) != 0 ? 0 : stream,
                       salt);
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

bool LN_RuntimeTree::ValueChangeStep(const uint32_t expression_index,
                                     const bool wait_for_value,
                                     const LN_Value &value,
                                     const uint64_t tick,
                                     bool &r_changed)
{
  BoolExpressionRuntimeState &state = BoolExpressionState(expression_index);
  ValueChangeState &values = ValueChangeStateOf(expression_index);
  if (!values.has_previous_value) {
    if (wait_for_value && (!value.exists || value.type == LN_ValueType::None)) {
      state.cached_result = false;
      state.last_update_tick = tick;
      return false;
    }
    values.previous_value = value;
    values.has_previous_value = true;
    values.stored_old_value = value;
    values.stored_new_value = value;
    state.cached_result = false;
    state.last_update_tick = tick;
    return false;
  }
  r_changed = !ValuesEqual(value, values.previous_value);
  values.stored_old_value = values.previous_value;
  values.stored_new_value = value;
  values.previous_value = value;
  return true;
}

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

template<class In>
LN_RuntimeTree::InventoryEditResult LN_RuntimeTree::RunInventoryEdit(In &in,
    const LN_ValueExpression &expression,
    std::vector<LN_Value> &slots,
    std::vector<LN_Value> *other_slots,
    const LN_InventoryRules *list_rules,
    const LN_InventoryRules *other_list_rules,
    KX_GameObject *owner)
{
  const LN_TickContext &context = in.context;
  const auto input = [&](const LN_InventoryInput index) {
    return in.Value(index < expression.input_indices.size() ?
                                      expression.input_indices[index] :
                                      LN_INVALID_INDEX, MakeNoneValue());
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
  /* Until the manager numbers it (AssignRandomStream): the object's and tree's indexes, so test
   * rigs without a manager give each object its own numbers too. */
  m_randomStream = LN_RandomMix((uint64_t(scene_object_index) << 32) ^ applied_tree_index ^
                                0x5eed5eed00000000ull);
  LN_NoteProgramsChanged();
  RefreshOptimizedPathEligibility();
  RebuildStateBlock(StateKeep::None);
  EnsureTreePropertyStateSize(false);
}

LN_RuntimeTree::~LN_RuntimeTree()
{
  DestroyStateBlock();
}

void LN_RuntimeTree::RefreshOptimizedPathEligibility()
{
  m_snapshotChannelMask = LN_SNAPSHOT_CHANNEL_NONE;
  m_hasTimeFlowRuntimeState = false;
  m_hasQueryExpressions = false;
  /* A new program's gates start over (RebuildStateBlock makes them anew): each one's first run
   * covers at most its N steps. */
  m_stepGatesStep = UINT64_MAX;
  m_stepGatesAnyDue = false;

  if (m_program == nullptr) {
    return;
  }

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
  RefreshStateBlockLayout();
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
  CollectUIViewsWith<LN_LeanReader>(context);
}

template<class Reader>
void LN_RuntimeTree::CollectUIViewsWith(const LN_TickContext &context)
{
  if (!m_runtimeActive || !m_enabled || m_program == nullptr || m_gameObject == nullptr ||
      m_logicManager == nullptr)
  {
    return;
  }
  RefreshStateBlockLayout();
  Reader in(*this, context);
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
            ResolveObjectValue(in.Value(expression.input0, MakeNoneValue()),
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
        camera = camera_input != LN_INVALID_INDEX ? ResolveCameraValue(in.Value(camera_input, MakeNoneValue())) :
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
    registration.enabled = in.Bool(expression.bool_expr_index, expression.bool_value);
    registration.user = std::max(
        0, in.Int(expression.int_expr_index, expression.int_value));
    /* In a menu: that menu's player (each player's copy of a per-player menu). */
    if (const int32_t menu_user = m_logicManager->GetUIService().MenuUserOfObject(viewport);
        menu_user >= 0)
    {
      registration.user = menu_user;
    }
    registration.priority = in.Int(expression.secondary_int_expr_index, expression.secondary_int_value);
    registration.padding = std::max(
        0.0f,
        in.Float(expression.float_expr_index, expression.float_value));

    if (expression.kind == LN_QueryExpressionKind::ObjectScrollView) {
      registration.content = expression.input1 != LN_INVALID_INDEX ?
                                 ResolveObjectValue(in.Value(expression.input1, MakeNoneValue()),
                                                    true) :
                                 nullptr;
      registration.min_offset = in.Vector(expression.input3, expression.vector_value);
      registration.max_offset = in.Vector(expression.input4, expression.secondary_vector_value);
      registration.wheel_step = in.Vector(expression.input5, expression.tertiary_vector_value);
      registration.horizontal = in.Bool(expression.secondary_bool_expr_index, false);
      registration.vertical = in.Bool(expression.tertiary_bool_expr_index, true);
      registration.cull_outside = in.Bool(expression.quaternary_bool_expr_index, true);
      registration.focus_follow = in.Bool(expression.quinary_bool_expr_index, true);
    }
    else {
      const LN_Value slots = in.Value(expression.input2, MakeNoneValue());
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
      std::string list_property = in.String(grid_input(LN_GRID_INPUT_PROPERTY), std::string());
      if (!list_property.empty()) {
        KX_GameObject *list_object = nullptr;
        if (const uint32_t object_input = grid_input(LN_GRID_INPUT_OBJECT);
            object_input != LN_INVALID_INDEX)
        {
          list_object = ResolveObjectValue(
              in.Value(object_input, MakeNoneValue()), true);
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
        LN_Value items = in.Value(expression.input3, MakeNoneValue());
        if (items.type == LN_ValueType::List) {
          ShareValue(items);
          registration.items = std::move(items.list_value);
        }
      }
      LN_UIService::GridInventory &inventory = registration.inventory;
      inventory.handle_moves = registration.list_object != nullptr &&
                               in.Bool(grid_input(LN_GRID_INPUT_HANDLE_MOVES), false);
      if (inventory.handle_moves) {
        inventory.split_action = in.String(grid_input(LN_GRID_INPUT_SPLIT_ACTION), std::string());
        inventory.split_amount = std::max(
            0, in.Int(grid_input(LN_GRID_INPUT_SPLIT_AMOUNT), 0));
        inventory.swap = in.Bool(grid_input(LN_GRID_INPUT_SWAP), true);
        inventory.drop_into_world = in.Bool(grid_input(LN_GRID_INPUT_DROP_INTO_WORLD), false);
        inventory.drop_at_pointer = in.Bool(grid_input(LN_GRID_INPUT_DROP_AT_POINTER), false);
        inventory.drop_in_front = in.Bool(grid_input(LN_GRID_INPUT_DROP_IN_FRONT), false);
        inventory.distance = std::max(
            0.0f, in.Float(grid_input(LN_GRID_INPUT_DISTANCE), 1.5f));
        inventory.hover_height = in.Float(grid_input(LN_GRID_INPUT_HOVER_HEIGHT), 0.0f);
      }
      if (const uint32_t hint_input = grid_input(LN_GRID_INPUT_HINT_TEXT);
          hint_input != LN_INVALID_INDEX)
      {
        KX_GameObject *hint = ResolveObjectValue(
            in.Value(hint_input, MakeNoneValue()), true);
        if (hint != nullptr && hint->GetScene() == scene) {
          inventory.hint_text = hint;
          inventory.accept_hint = in.String(grid_input(LN_GRID_INPUT_ACCEPT_HINT), "Put {name} here");
          inventory.refuse_hint = in.String(grid_input(LN_GRID_INPUT_REFUSE_HINT), "{name} does not fit here");
        }
      }
      registration.show_categories = in.String(grid_input(LN_GRID_INPUT_SHOW_CATEGORIES), std::string());
      registration.search = in.String(grid_input(LN_GRID_INPUT_SEARCH), std::string());
      registration.use_action = in.String(grid_input(LN_GRID_INPUT_USE_ACTION), std::string());
      registration.use_keys = in.String(grid_input(LN_GRID_INPUT_USE_KEYS), std::string());
      registration.requested_first_index = in.Int(expression.input4, expression.int_value);
      registration.wheel_rows = std::max(1, in.Int(expression.input5, 1));
      registration.columns = std::max(
          1, in.Int(expression.tertiary_int_expr_index, 1));
      registration.cull_outside = in.Bool(expression.secondary_bool_expr_index, true);
      registration.focus_follow = in.Bool(expression.tertiary_bool_expr_index, true);
      /* The list's Inventory Rules (its game property's set): how the grid stacks, keeps places
       * and combines it, and the library its icons come from (none: the scene's Item Library,
       * like the inventory nodes). A list Items gives follows the defaults. */
      registration.rules = m_logicManager->InventoryRulesOf(registration.list_object,
                                                            registration.list_property);
      registration.icon_library = const_cast<blender::Collection *>(
          registration.rules.library != nullptr ? registration.rules.library :
                                                  SceneItemLibrary());
      registration.show_counts = in.Bool(expression.quaternary_bool_expr_index, true);
      registration.show_names = in.Bool(expression.quinary_bool_expr_index, false);
      registration.show_empty = in.Bool(expression.senary_bool_expr_index, false);
      registration.slot_template = expression.input7 != LN_INVALID_INDEX ?
                                       ResolveObjectValue(in.Value(expression.input7, MakeNoneValue()),
                                                          true) :
                                       nullptr;
      registration.rows = std::max(
          0, in.Int(expression.quaternary_int_expr_index, 0));
      registration.spacing = in.Vector(expression.input8, MT_Vector3(0.0f, 0.0f, 0.0f));
      registration.horizontal = expression.grid_horizontal;
      registration.vertical = !expression.grid_horizontal;
    }
    registration.overlay = is_overlay;
    KX_GameObject *scrollbar = expression.input9 != LN_INVALID_INDEX ?
                                   ResolveObjectValue(in.Value(expression.input9, MakeNoneValue()),
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
  return WarmQueryCacheWith<LN_LeanReader>(context);
}

template<class Reader>
size_t LN_RuntimeTree::WarmQueryCacheWith(const LN_TickContext &context)
{
  if (!m_runtimeActive || !m_enabled || m_program == nullptr ||
      m_queryCacheWarmTick == context.tick_index || !m_hasQueryExpressions)
  {
    return 0;
  }
  RefreshStateBlockLayout();
  Reader in(*this, context);

  const std::vector<LN_QueryExpression> &queries = m_program->GetQueryExpressions();
  size_t warmed_count = 0;
  for (uint32_t query_index = 0; query_index < queries.size(); query_index++) {
    if (!QueryExpressionCanWarmCache(queries[query_index])) {
      continue;
    }
    in.Query(query_index);
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

bool LN_GamePropertyAsBool(EXP_Value *property)
{
  return ValueAsBool(MakeValueFromEXPValue(property, nullptr, nullptr));
}

std::string LN_GamePropertyAsText(EXP_Value *property)
{
  return ValueToText(MakeValueFromEXPValue(property, nullptr, nullptr));
}

bool LN_ApplyPropertyEdit(LN_Value &collection,
                          const LN_PropertyEditOp op,
                          const LN_Value &value,
                          const int32_t index,
                          const std::string &key,
                          const float amount)
{
  std::vector<LN_Value> &list = collection.list_value;
  size_t normalized_index = 0;
  switch (op) {
    case LN_PropertyEditOp::ListAppend:
      list.push_back(value);
      return true;
    case LN_PropertyEditOp::ListRemoveIndex:
      if (!NormalizeListIndex(index, list.size(), normalized_index)) {
        return false;
      }
      list.erase(list.begin() + normalized_index);
      return true;
    case LN_PropertyEditOp::ListRemoveValue: {
      const auto iter = std::find_if(list.begin(), list.end(), [&](const LN_Value &item) {
        return ValuesEqual(item, value);
      });
      if (iter == list.end()) {
        return false;
      }
      list.erase(iter);
      return true;
    }
    case LN_PropertyEditOp::ListSetIndex:
      if (!NormalizeListIndex(index, list.size(), normalized_index)) {
        return false;
      }
      list[normalized_index] = value;
      return true;
    case LN_PropertyEditOp::DictSetKey:
      if (key.empty()) {
        return false;
      }
      collection.dict_value[key] = value;
      return true;
    case LN_PropertyEditOp::DictAddValue: {
      if (key.empty()) {
        return false;
      }
      /* The DictAddValue kind's arithmetic, word for word. */
      LN_Value &entry = collection.dict_value[key];
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
      return true;
    }
    case LN_PropertyEditOp::DictRemoveKey: {
      if (key.empty()) {
        return false;
      }
      const auto iter = collection.dict_value.find(key);
      if (iter == collection.dict_value.end()) {
        return false;
      }
      collection.dict_value.erase(iter);
      return true;
    }
  }
  return false;
}

bool LN_ApplyGamePropertyEdit(KX_GameObject *object,
                              const std::string &property_name,
                              const LN_PropertyEditOp op,
                              const LN_Value &value,
                              const int32_t index,
                              const std::string &key,
                              const float amount,
                              LN_RuntimeTree *runtime_tree,
                              std::string &r_reason)
{
  if (object == nullptr || property_name.empty()) {
    r_reason = "a list or dictionary change needs an object and a property name";
    return false;
  }
  const bool list = LN_PropertyEditIsList(op);
  const LN_ValueType type = list ? LN_ValueType::List : LN_ValueType::Dict;
  LN_Value collection;
  EXP_Value *current = object->GetProperty(property_name);
  if (current == nullptr) {
    if (op != LN_PropertyEditOp::ListAppend && op != LN_PropertyEditOp::DictSetKey &&
        op != LN_PropertyEditOp::DictAddValue)
    {
      return true; /* Nothing to remove or replace. */
    }
    collection.type = type;
    collection.exists = true;
  }
  else {
    collection = MakeValueFromEXPValue(current, runtime_tree);
    if (!collection.exists || collection.type != type) {
      r_reason = "game property '" + property_name + "' holds no " +
                 (list ? "List" : "Dictionary") + ": the change was not made";
      return false;
    }
  }
  if (!LN_ApplyPropertyEdit(collection, op, value, index, key, amount)) {
    return true;
  }
  return LN_CommandBuffer::ApplyGamePropertyValue(
      object, property_name, collection, runtime_tree, r_reason);
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
  /* The program's bit first: the manager asks every tree several times a frame, and most have
   * no UI (CanTick reads the object). */
  return m_program != nullptr &&
         (m_program->GetDependencySummary().query_channels & LN_DEP_QUERY_OBJECT_UI) != 0u &&
         CanTick();
}

bool LN_RuntimeTree::CanTick(const LN_Event event) const
{
  if (m_program == nullptr) {
    return false;
  }
  /* A step whose work is all behind Every N Steps gates, none of them due, has nothing to run
   * unless a Delay, Timer... counts down (below). */
  const bool has_work = !m_program->GetInstructionHeaders(event).empty() &&
                        (event != LN_Event::OnFixedUpdate || m_stepGatesAnyDue ||
                         !m_program->GetExecBlockProgram(event).all_blocks_step_gated);
  /* Answered from the program alone first, without reading the object: every tree is asked at
   * every tick, mostly about a phase it has nothing in (a step-only tree at each frame, a tree
   * asleep between its Every N Steps). */
  if (m_initExecuted && !has_work && !m_hasTimeFlowRuntimeState) {
    return false;
  }
  if (!CanTick()) {
    return false;
  }

  /* Initialization is deliberately owned by the first recurring phase (normally Update), so
   * On Init also runs for trees that have no instructions in that recurring phase. */
  if (!m_initExecuted || has_work) {
    return true;
  }

  if (m_hasTimeFlowRuntimeState) {
    const bool has_phase_time_flow = std::any_of(
        m_timeFlowStates.begin(),
        m_timeFlowStates.end(),
        [event](const TimeFlowRuntimeState &state) {
          if (state.event != event || (!state.active && state.pending_pulses.empty())) {
            return false;
          }
          /* Pulsify counts only when its flow arrives, and a Barrier fed every N steps checks its
           * own continuity when its route runs: neither needs the steps between. */
          return state.kind != TimeFlowRuntimeKind::Pulsify &&
                 !(state.kind == TimeFlowRuntimeKind::Barrier &&
                   state.last_update_phase_stride > 1);
        });
    if (has_phase_time_flow) {
      return true;
    }
  }

  return false;
}

void LN_RuntimeTree::PrepareStepGates(const uint64_t step,
                                      const double delta_time,
                                      const double unscaled_delta_time)
{
  if (m_stepGates.empty() || m_program == nullptr || step == m_stepGatesStep) {
    return;
  }
  const std::vector<LN_StepGate> &gates = m_program->GetStepGates();
  if (gates.size() != m_stepGates.size()) {
    return;
  }
  m_stepGatesStep = step;
  if (m_stepGatesFirstStep == UINT64_MAX || step < m_stepGatesFirstStep) {
    m_stepGatesFirstStep = step;
  }
  m_stepGatesAnyDue = false;
  for (size_t index = 0; index < gates.size(); index++) {
    const LN_StepGate &gate = gates[index];
    StepGateState &state = m_stepGates[index];
    const uint64_t interval = std::max<uint64_t>(gate.interval, 1);
    const uint64_t offset = gate.spread ? uint64_t(m_stepSpread) % interval : 0;
    state.due = (step % interval + offset) % interval == 0;
    if (!state.due) {
      continue;
    }
    m_stepGatesAnyDue = true;
    /* The steps since the route last ran: due steps are N apart, so N, but the first run covers
     * only the steps the tree has been there, and a Run Logic Tree pass in between already
     * covered its own. A tree that was off for a while (disabled) or missed its step (suspended)
     * gets N too, as a step node without Every gets one step. */
    const uint64_t steps = StepGateStepsSinceLastRun(state, step, interval);
    state.time_passed = double(steps) * std::max(0.0, delta_time);
    state.unscaled_time_passed = double(steps) * std::max(0.0, unscaled_delta_time);
    state.steps = uint32_t(steps);
  }
}

uint64_t LN_RuntimeTree::StepGateStepsSinceLastRun(const StepGateState &state,
                                                   const uint64_t step,
                                                   const uint64_t interval) const
{
  uint64_t steps = 1;
  if (state.last_run_step != UINT64_MAX) {
    if (step > state.last_run_step) {
      steps = step - state.last_run_step;
    }
  }
  else if (m_stepGatesFirstStep != UINT64_MAX && step >= m_stepGatesFirstStep) {
    steps = step - m_stepGatesFirstStep + 1;
  }
  return std::clamp<uint64_t>(steps, 1, std::max<uint64_t>(interval, 1));
}

void LN_RuntimeTree::PrepareForcedStepGates(const uint64_t step,
                                            const double delta_time,
                                            const double unscaled_delta_time)
{
  if (m_stepGates.empty() || m_program == nullptr) {
    return;
  }
  const std::vector<LN_StepGate> &gates = m_program->GetStepGates();
  if (gates.size() != m_stepGates.size()) {
    return;
  }
  for (size_t index = 0; index < gates.size(); index++) {
    StepGateState &state = m_stepGates[index];
    const uint64_t steps = StepGateStepsSinceLastRun(state, step, gates[index].interval);
    state.time_passed = double(steps) * std::max(0.0, delta_time);
    state.unscaled_time_passed = double(steps) * std::max(0.0, unscaled_delta_time);
    state.steps = uint32_t(steps);
  }
}

void LN_RuntimeTree::NoteStepGatesRan(const uint64_t step, const bool forced)
{
  for (StepGateState &state : m_stepGates) {
    if (forced || (state.due && m_stepGatesStep == step)) {
      state.last_run_step = step;
    }
  }
}

bool LN_RuntimeTree::StepGateRuns(const uint32_t step_gate_index) const
{
  if (m_stepGatesForced) {
    return true;
  }
  return step_gate_index < m_stepGates.size() && m_stepGates[step_gate_index].due &&
         m_stepGatesStep != UINT64_MAX;
}

void LN_RuntimeTree::CollectPauseProcessingObjects(std::vector<KX_GameObject *> &r_objects)
{
  CollectPauseProcessingObjectsWith<LN_LeanReader>(r_objects);
}

template<class Reader>
void LN_RuntimeTree::CollectPauseProcessingObjectsWith(std::vector<KX_GameObject *> &r_objects)
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

  Reader in(*this, static_context);
  for (const LN_PauseProcessingRequest &request : m_program->GetPauseProcessingRequests()) {
    switch (request.scope) {
      case LN_PauseProcessingScope::Owner:
        break;
      case LN_PauseProcessingScope::ObjectHierarchy: {
        KX_GameObject *root = m_gameObject;
        if (request.target_value_expr_index != LN_INVALID_INDEX) {
          KX_GameObject *resolved = ResolveObjectValue(in.Value(request.target_value_expr_index, MakeNoneValue()));
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
        const LN_Value collection_value = in.Value(request.target_value_expr_index, MakeNoneValue());
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

  LN_NoteProgramsChanged();
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
    RefreshOptimizedPathEligibility();
    m_snapshotTick = UINT64_MAX;
    m_queryCacheWarmTick = UINT64_MAX;
    RebuildStateBlock(StateKeep::BetweenTicks);
    EnsureTreePropertyStateSize(true);
    return;
  }

  m_program = std::move(program);
  RefreshOptimizedPathEligibility();
  m_initExecuted = false;
  /* Each Object collision nodes are numbered per program: another program's touching objects
   * mean nothing here (the same tree's new compile, above, numbers them alike and keeps them). */
  m_collisionEventNodes.clear();
  m_snapshotTick = UINT64_MAX;
  m_queryCacheWarmTick = UINT64_MAX;
  m_executionScopeSerial = 0;
  m_activeExecutionScopeSerial = 0;
  RebuildStateBlock(StateKeep::None);
  EnsureTreePropertyStateSize(false);
  m_loopStates.clear();
  m_activeLoopBodyFrame = LN_INVALID_INDEX;
}

/* Object ref slots a tree scans for an object before it keeps an object -> slot map
 * (MakeObjectRef). */
static constexpr size_t kObjectRefShortScan = 16;

LN_RuntimeRef LN_RuntimeTree::MakeObjectRef(KX_GameObject *gameobj, const std::string &debug_name)
{
  LN_RuntimeRef runtime_ref;
  runtime_ref.kind = LN_RuntimeRefKind::Object;
  if (gameobj == nullptr) {
    runtime_ref.debug_name = debug_name;
    return runtime_ref;
  }

  /* The object's slot: a scan of a few, the map past that. */
  uint32_t found = LN_INVALID_INDEX;
  if (m_objectRefsIndexed) {
    if (const uint32_t *index = m_objectRefIndex.lookup_ptr(gameobj)) {
      found = *index;
    }
  }
  else {
    for (uint32_t index = 0; index < m_objectRefs.size(); index++) {
      if (m_objectRefs[index].object == gameobj) {
        found = index;
        break;
      }
    }
  }
  if (found != LN_INVALID_INDEX) {
    const ObjectRefSlot &slot = m_objectRefs[found];
    runtime_ref.slot = found;
    runtime_ref.generation = slot.generation;
    runtime_ref.debug_name = slot.debug_name;
    return runtime_ref;
  }

  /* A new object: the lowest slot an invalidation emptied, else a new slot. */
  uint32_t index;
  if (!m_freeObjectRefSlots.empty()) {
    std::pop_heap(
        m_freeObjectRefSlots.begin(), m_freeObjectRefSlots.end(), std::greater<uint32_t>());
    index = m_freeObjectRefSlots.back();
    m_freeObjectRefSlots.pop_back();
    ObjectRefSlot &slot = m_objectRefs[index];
    slot.object = gameobj;
    slot.generation++;
    slot.debug_name = debug_name;
  }
  else {
    index = uint32_t(m_objectRefs.size());
    ObjectRefSlot &slot = m_objectRefs.emplace_back();
    slot.object = gameobj;
    slot.debug_name = debug_name;
  }
  if (m_objectRefsIndexed) {
    m_objectRefIndex.add_new(gameobj, index);
  }
  else if (m_objectRefs.size() > kObjectRefShortScan) {
    m_objectRefIndex.clear();
    for (uint32_t listed = 0; listed < m_objectRefs.size(); listed++) {
      if (m_objectRefs[listed].object != nullptr) {
        m_objectRefIndex.add_new(m_objectRefs[listed].object, listed);
      }
    }
    m_objectRefsIndexed = true;
  }
  if (m_logicManager != nullptr) {
    m_logicManager->RegisterRuntimeObjectRef(this, gameobj);
  }

  const ObjectRefSlot &slot = m_objectRefs[index];
  runtime_ref.slot = index;
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

bool LN_RuntimeTree::IsRemovedObjectRef(const LN_RuntimeRef &runtime_ref) const
{
  if (runtime_ref.kind != LN_RuntimeRefKind::Object || !runtime_ref.IsValid()) {
    return false;
  }
  if (runtime_ref.shared_domain != 0) {
    return m_logicManager != nullptr && m_logicManager->OwnsSharedObjectRef(runtime_ref) &&
           m_logicManager->ResolveSharedObjectRef(runtime_ref) == nullptr;
  }
  /* Only InvalidateObjectRef (the object's removal) moves a slot's generation on; a slot this
   * tree never made is no judgement of this tree's. */
  return runtime_ref.slot < m_objectRefs.size() &&
         m_objectRefs[runtime_ref.slot].generation != runtime_ref.generation;
}

void LN_RuntimeTree::ForgetRemovedObjects(LN_Value &value) const
{
  switch (value.type) {
    case LN_ValueType::ObjectRef:
      if (IsRemovedObjectRef(value.runtime_ref)) {
        value = MakeNoneValue();
      }
      break;
    case LN_ValueType::List:
      for (LN_Value &item : value.list_value) {
        ForgetRemovedObjects(item);
      }
      break;
    case LN_ValueType::Dict:
      for (auto &item : value.dict_value) {
        ForgetRemovedObjects(item.second);
      }
      break;
    default:
      break;
  }
}

bool LN_RuntimeTree::ValueAsBoolHere(const LN_Value &value) const
{
  return !IsRemovedObjectValue(value) && ValueAsBool(value);
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
    /* Each object is listed once; its per-object contact info goes with it. */
    for (size_t index = 0; index < result.hit_objects.size(); index++) {
      if (result.hit_objects[index] != gameobj) {
        continue;
      }
      result.hit_objects.erase(result.hit_objects.begin() + index);
      if (index < result.object_contact_counts.size()) {
        result.object_contact_counts.erase(result.object_contact_counts.begin() + index);
        result.object_points.erase(result.object_points.begin() + index);
        result.object_normals.erase(result.object_normals.begin() + index);
        result.object_has_point.erase(result.object_has_point.begin() + index);
      }
      break;
    }
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
  /* This tick's Each Object lists (their identities stay: the next update compares them). */
  for (CollisionEventNodeState &state : m_collisionEventNodes) {
    std::replace(state.current.begin(), state.current.end(), gameobj, (KX_GameObject *)nullptr);
  }

  bool invalidated = false;
  const auto empty_slot = [&](const uint32_t index) {
    ObjectRefSlot &slot = m_objectRefs[index];
    slot.object = nullptr;
    slot.generation++;
    m_freeObjectRefSlots.push_back(index);
    std::push_heap(
        m_freeObjectRefSlots.begin(), m_freeObjectRefSlots.end(), std::greater<uint32_t>());
    invalidated = true;
  };
  if (m_objectRefsIndexed) {
    if (const std::optional<uint32_t> index = m_objectRefIndex.pop_try(gameobj)) {
      empty_slot(*index);
    }
  }
  else {
    for (uint32_t index = 0; index < m_objectRefs.size(); index++) {
      if (m_objectRefs[index].object == gameobj) {
        empty_slot(index);
      }
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

void LN_RuntimeTree::RayCast(
    const PHY_RayQuerySettings &settings,
    const LN_RuntimeRef &ignore_object_ref,
    const uint32_t collision_mask,
    const std::string &property_name,
    const bool xray,
    const std::vector<LN_RuntimeRef> &extra_ignore_object_refs,
    bool &r_blocked,
    LN_PhysicsQueryResult &r_blocker,
    bool &r_supported,
    std::vector<LN_PhysicsQueryResult> &r_results)
{
  r_results.clear();
  r_blocked = false;
  r_blocker = LN_PhysicsQueryResult();
  r_supported = false;
  KX_Scene *scene = m_gameObject ? m_gameObject->GetScene() : nullptr;
  PHY_IPhysicsEnvironment *environment = scene ? scene->GetPhysicsEnvironment() : nullptr;
  if (!environment) {
    return;
  }

  KX_GameObject *ignore_object = ResolveObjectRef(ignore_object_ref);
  const LN_PhysicsControllerIgnoreSet extra_ignore_controllers(*this, extra_ignore_object_refs);
  LN_RayQueryFilterCallback filter(RaycastOwningPhysicsController(ignore_object),
                                   extra_ignore_controllers,
                                   collision_mask,
                                   property_name,
                                   xray);
  /* (Kept per thread: rays run on helpers in a column run's segments.) */
  thread_local std::vector<PHY_RayCastResult> physics_results;
  physics_results.clear();
  r_supported = environment->RayCast(settings, filter, physics_results);
  if (!r_supported) {
    return;
  }

  const MT_Vector3 displacement = settings.destination - settings.origin;
  const float cast_length = displacement.length();
  if (!std::isfinite(cast_length) || cast_length <= MT_EPSILON) {
    return;
  }
  const MT_Vector3 direction = displacement / cast_length;
  std::vector<LN_PhysicsQueryResult> &results = r_results;
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
    results.push_back(std::move(result));
  }
}

void LN_RuntimeTree::ShapeCast(
    const PHY_ShapeCastSettings &settings,
    const LN_RuntimeRef &ignore_object_ref,
    const uint32_t collision_mask,
    const std::string &property_name,
    const bool xray,
    const std::vector<LN_RuntimeRef> &extra_ignore_object_refs,
    bool &r_blocked,
    LN_PhysicsQueryResult &r_blocker,
    bool &r_supported,
    std::vector<LN_PhysicsQueryResult> &r_results)
{
  r_results.clear();
  r_blocked = false;
  r_blocker = LN_PhysicsQueryResult();
  r_supported = false;
  KX_Scene *scene = m_gameObject ? m_gameObject->GetScene() : nullptr;
  PHY_IPhysicsEnvironment *environment = scene ? scene->GetPhysicsEnvironment() : nullptr;
  if (!environment) {
    return;
  }

  KX_GameObject *ignore_object = ResolveObjectRef(ignore_object_ref);
  const LN_PhysicsControllerIgnoreSet extra_ignore_controllers(*this, extra_ignore_object_refs);
  LN_ShapeCastFilterCallback filter(RaycastOwningPhysicsController(ignore_object),
                                    extra_ignore_controllers,
                                    collision_mask,
                                    property_name,
                                    xray);
  /* (Kept per thread: rays run on helpers in a column run's segments.) */
  thread_local std::vector<PHY_ShapeCastResult> physics_results;
  physics_results.clear();
  r_supported = environment->ShapeCast(settings, filter, physics_results);
  if (!r_supported) {
    return;
  }

  const MT_Vector3 displacement = settings.destination - settings.origin;
  const float cast_length = displacement.length();
  if (!std::isfinite(cast_length)) {
    return;
  }
  const MT_Vector3 direction = cast_length > MT_EPSILON ? displacement / cast_length :
                                                          MT_Vector3(0.0f, 0.0f, 0.0f);
  std::vector<LN_PhysicsQueryResult> &results = r_results;
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
    results.push_back(std::move(result));
  }
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

bool LN_RuntimeTree::SetTreePropertyValue(const uint32_t property_ref_index, LN_Value &&value)
{
  if (property_ref_index >= m_treeProperties.size()) {
    return false;
  }
  m_treeProperties[property_ref_index] = std::move(value);
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

  if (const LN_SceneAccessForTests *access = LN_GetSceneAccessForTests()) {
    /* The same rules with the unit test's object lists (see LN_SceneAccessForTests). */
    const auto counterpart = [&](KX_GameObject *object) {
      return access->overlay_counterpart ? access->overlay_counterpart(object, m_gameObject) :
                                           object;
    };
    const auto find_in = [&](const std::vector<KX_GameObject *> &list) -> KX_GameObject * {
      for (KX_GameObject *game_object : list) {
        if (game_object_matches_name(game_object, name)) {
          return game_object;
        }
      }
      return nullptr;
    };
    if (include_inactive) {
      if (KX_GameObject *object = find_in(access->inactive)) {
        return object;
      }
    }
    if (access->ticking && m_logicManager != nullptr) {
      if (KX_GameObject *object = m_logicManager->FindObjectByName(name)) {
        return counterpart(object);
      }
    }
    if (KX_GameObject *object = find_in(access->objects)) {
      return counterpart(object);
    }
    return include_inactive ? find_in(access->inactive) : nullptr;
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
    /* Plan C1-10: no object was added since the lookup was made, so the scan below would find
     * the first object whose Blender object has the name, or none: the lookup knows it (a name
     * no object has, read by every NPC of a crowd, scanned the whole list each time). */
    KX_GameObject *by_blender_name = nullptr;
    if (manager->AnswerObjectNameMiss(name, by_blender_name)) {
      if (by_blender_name != nullptr) {
        return scene->OverlayCounterpart(by_blender_name, m_gameObject);
      }
      return include_inactive ? find_inactive() : nullptr;
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

namespace {

enum class ConstObjectsMode : int8_t { Off = 0, On = 1, Verify = 2 };
std::atomic<int> g_const_objects_override{-1};
std::atomic<uint32_t> g_const_objects_differences{0};

ConstObjectsMode const_objects_mode()
{
  const int override_mode = g_const_objects_override.load(std::memory_order_relaxed);
  if (override_mode >= 0) {
    return ConstObjectsMode(override_mode);
  }
  static const ConstObjectsMode from_environment = [] {
    const char *value = std::getenv("UPBGE_LN_CONST_OBJECTS");
    if (value == nullptr || value[0] == '\0') {
      return ConstObjectsMode::On;
    }
    if (std::strcmp(value, "0") == 0 || std::strcmp(value, "off") == 0) {
      return ConstObjectsMode::Off;
    }
    if (std::strcmp(value, "verify") == 0) {
      return ConstObjectsMode::Verify;
    }
    return ConstObjectsMode::On;
  }();
  return from_environment;
}

}  // namespace

void LN_RuntimeTree::SetConstantObjectModeForTests(const int mode)
{
  g_const_objects_override.store(mode, std::memory_order_relaxed);
}

uint32_t LN_RuntimeTree::ConstantObjectVerifyDifferencesForTests()
{
  return g_const_objects_differences.load(std::memory_order_relaxed);
}

KX_GameObject *LN_RuntimeTree::ResolveConstantObject(const uint32_t expression_index,
                                                     const LN_Value &constant)
{
  const ConstObjectsMode mode = const_objects_mode();
  const std::string &name = constant.reference_name;
  if (mode == ConstObjectsMode::Off || name.empty()) {
    return ResolveObjectValue(constant);
  }

  /* What the lookup depends on: the scene (or the unit test's seam), its object list generation
   * and whether a tick runs (the manager's name table answers only then). */
  const LN_SceneAccessForTests *access = LN_GetSceneAccessForTests();
  const void *scene_key = nullptr;
  uint64_t generation = 0;
  bool ticking = false;
  KX_Scene *scene = nullptr;
  if (access != nullptr) {
    scene_key = access;
    generation = access->object_list_generation;
    ticking = access->ticking;
  }
  else {
    scene = m_gameObject ? m_gameObject->GetScene() : nullptr;
    if (scene == nullptr) {
      return ResolveObjectValue(constant);
    }
    scene_key = scene;
    generation = scene->GetObjectListGeneration();
    ticking = m_logicManager != nullptr && m_logicManager->IsTicking();
  }

  ConstantObjectEntry *entry = nullptr;
  for (ConstantObjectEntry &candidate : m_constantObjects) {
    if (candidate.expression_index == expression_index) {
      entry = &candidate;
      break;
    }
  }
  if (entry == nullptr) {
    m_constantObjects.emplace_back();
    entry = &m_constantObjects.back();
    entry->expression_index = expression_index;
  }

  if (!entry->resolved || entry->scene_key != scene_key || entry->generation != generation ||
      entry->ticking != ticking)
  {
    /* FindSceneObjectByName (not looking at inactive objects) up to its menu copy rule: the
     * manager's table first, then the first object of the scene whose name it is. */
    KX_GameObject *base = nullptr;
    if (access != nullptr) {
      if (access->ticking && m_logicManager != nullptr) {
        base = m_logicManager->FindObjectByName(name);
      }
      if (base == nullptr) {
        for (KX_GameObject *game_object : access->objects) {
          if (game_object_matches_name(game_object, name)) {
            base = game_object;
            break;
          }
        }
      }
    }
    else {
      if (LN_Manager *manager = scene->GetLogicNodeManager()) {
        base = manager->FindObjectByName(name);
      }
      if (base == nullptr && scene->GetObjectList() != nullptr) {
        for (KX_GameObject *game_object : *scene->GetObjectList()) {
          if (game_object_matches_name(game_object, name)) {
            base = game_object;
            break;
          }
        }
      }
    }
    entry->scene_key = scene_key;
    entry->generation = generation;
    entry->ticking = ticking;
    entry->base = base;
    entry->resolved = true;
  }

  KX_GameObject *object = entry->base;
  if (object != nullptr) {
    if (access != nullptr) {
      if (access->overlay_counterpart) {
        object = access->overlay_counterpart(object, m_gameObject);
      }
    }
    else {
      object = scene->OverlayCounterpart(object, m_gameObject);
    }
  }

  if (mode == ConstObjectsMode::Verify) {
    KX_GameObject *expected = ResolveObjectValue(constant);
    if (expected != object) {
      g_const_objects_differences++;
      CM_Warning("Logic Nodes: lean const objects differs: '" << name
                                                               << "' is not what the name finds");
    }
  }
  return object;
}

namespace {

enum class FastAttributesMode : int8_t { Off = 0, On = 1, Verify = 2 };
std::atomic<int> g_fast_attributes_override{-1};
std::atomic<uint32_t> g_fast_attributes_differences{0};

FastAttributesMode fast_attributes_mode()
{
  const int override_mode = g_fast_attributes_override.load(std::memory_order_relaxed);
  if (override_mode >= 0) {
    return FastAttributesMode(override_mode);
  }
  static const FastAttributesMode from_environment = [] {
    const char *value = std::getenv("UPBGE_LN_FAST_ATTRIBUTES");
    if (value == nullptr || value[0] == '\0') {
      return FastAttributesMode::On;
    }
    if (std::strcmp(value, "0") == 0 || std::strcmp(value, "off") == 0) {
      return FastAttributesMode::Off;
    }
    if (std::strcmp(value, "verify") == 0) {
      return FastAttributesMode::Verify;
    }
    return FastAttributesMode::On;
  }();
  return from_environment;
}

}  // namespace

void LN_RuntimeTree::SetFastAttributeModeForTests(const int mode)
{
  g_fast_attributes_override.store(mode, std::memory_order_relaxed);
}

uint32_t LN_RuntimeTree::FastAttributeVerifyDifferencesForTests()
{
  return g_fast_attributes_differences.load(std::memory_order_relaxed);
}

void LN_RuntimeTree::NoteFastAttributeDifference(const char *what)
{
  g_fast_attributes_differences++;
  CM_Warning("Logic Nodes: lean fast attributes differ: " << what);
}

LN_ObjectAttributeId LN_RuntimeTree::FastObjectAttribute(const uint32_t value_index) const
{
  const LN_RegisterExpressionProgram &ir = m_program->GetRegisterExpressionProgram();
  if (value_index >= ir.value_object_attribute.size() ||
      ir.value_object_attribute[value_index] == uint8_t(LN_ObjectAttributeId::None))
  {
    return LN_ObjectAttributeId::None;
  }
  /* Profiling counts every read and the read trace lists it: both take the value path. A kept
   * (memo STORE) attribute cannot be: the kind is not pure; the check keeps the typed reads
   * exact whatever the catalog says. */
  if (m_activeProfileCounters != nullptr || g_read_trace != nullptr ||
      value_index >= ir.value_memo_class.size() ||
      ir.value_memo_class[value_index] == uint8_t(LN_MEMO_STORE) ||
      fast_attributes_mode() == FastAttributesMode::Off)
  {
    return LN_ObjectAttributeId::None;
  }
  return LN_ObjectAttributeId(ir.value_object_attribute[value_index]);
}

KX_GameObject *LN_RuntimeTree::ConstantObjectTarget(const uint32_t value_index,
                                                    const LN_Value &constant)
{
  /* ValueKind's Constant case for an object makes a value that ResolveObjectValue then resolves:
   * the same calls in the same cases, without the value. */
  if (constant.runtime_ref.IsValid()) {
    if (KX_GameObject *object = ResolveObjectRef(constant.runtime_ref)) {
      return object;
    }
    return constant.reference_name.empty() || IsRemovedObjectRef(constant.runtime_ref) ?
               nullptr :
               FindSceneObjectByName(constant.reference_name);
  }
  KX_GameObject *object = ResolveConstantObject(value_index, constant);
  if (object == nullptr) {
    return constant.reference_name.empty() ? nullptr :
                                             FindSceneObjectByName(constant.reference_name);
  }
  const LN_RuntimeRef ref = ConstantObjectRef(value_index, object);
  if (ref.IsValid()) {
    if (KX_GameObject *resolved = ResolveObjectRef(ref)) {
      return resolved;
    }
  }
  const std::string name = object->GetName();
  return name.empty() ? nullptr : FindSceneObjectByName(name);
}

LN_RuntimeRef LN_RuntimeTree::ConstantObjectRef(const uint32_t expression_index,
                                                KX_GameObject *object)
{
  if (object == nullptr || const_objects_mode() == ConstObjectsMode::Off) {
    return MakeObjectRef(object, object ? object->GetName() : std::string());
  }
  for (ConstantObjectEntry &entry : m_constantObjects) {
    if (entry.expression_index == expression_index) {
      if (entry.ref.IsValid() && ResolveObjectRef(entry.ref) == object) {
        return entry.ref;
      }
      entry.ref = MakeObjectRef(object, object->GetName());
      return entry.ref;
    }
  }
  return MakeObjectRef(object, object->GetName());
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
    /* Its object was removed: None, not another object of its name (IsRemovedObjectRef). */
    if (IsRemovedObjectRef(value.runtime_ref)) {
      return nullptr;
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
        if (IsRemovedObjectRef(value.runtime_ref)) {
          /* Kept as a name, readers would find another object of that name. */
          value = MakeNoneValue();
          break;
        }
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
        if (object == nullptr && IsRemovedObjectRef(value.runtime_ref)) {
          /* Removed since it was shared: None (not a name readers would find again). */
          value = MakeNoneValue();
          break;
        }
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

template<class In>
std::vector<LN_RuntimeRef> LN_RuntimeTree::ResolveIgnoreObjectRefs(In &in,
    const uint32_t object_expr_index,
    const uint32_t list_expr_index)
{
  const LN_TickContext &context = in.context;
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
        ResolveObjectValue(in.Value(object_expr_index, MakeNoneValue())));
  }
  if (list_expr_index != LN_INVALID_INDEX) {
    const LN_Value list = in.Value(list_expr_index, MakeNoneValue());
    if (list.exists && list.type == LN_ValueType::List) {
      refs.reserve(refs.size() + list.list_value.size());
      for (const LN_Value &item : list.list_value) {
        append_object(ResolveObjectValue(item));
      }
    }
  }
  return refs;
}

template<class In>
PHY_IPhysicsController *LN_RuntimeTree::ResolveRigidBodyAttributeController(In &in,
    const uint32_t object_expr_index)
{
  const LN_TickContext &context = in.context;
  KX_GameObject *target_object = m_gameObject;
  if (object_expr_index != LN_INVALID_INDEX) {
    target_object = ResolveObjectValue(
        in.Value(object_expr_index, MakeNoneValue()));
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

template<class In>
bool LN_RuntimeTree::ResolveRigidBodyConstraintBreakEventQuery(In &in,
    const uint32_t object_expr_index,
    const uint32_t constraint_name_expr_index,
    const uint32_t match_any_expr_index,
    const LN_RigidBodyConstraintMatchMode match_mode,
    KX_GameObject *&r_owner,
    std::string &r_constraint_name,
    LN_RigidBodyConstraintMatchMode &r_match_mode)
{
  const LN_TickContext &context = in.context;
  r_owner = nullptr;
  r_constraint_name.clear();
  r_match_mode = ValidRigidBodyConstraintMatchMode(match_mode);
  if (m_logicManager == nullptr) {
    return false;
  }

  r_owner = m_gameObject;
  if (object_expr_index != LN_INVALID_INDEX) {
    r_owner = ResolveObjectValue(
        in.Value(object_expr_index, MakeNoneValue()));
  }
  if (r_owner == nullptr) {
    return false;
  }

  const bool match_any = match_any_expr_index != LN_INVALID_INDEX &&
                         in.Bool(match_any_expr_index, false);
  if (match_any) {
    r_match_mode = LN_RigidBodyConstraintMatchMode::All;
    return true;
  }

  r_constraint_name = in.String(constraint_name_expr_index, std::string());
  return !r_constraint_name.empty();
}

template<class In>
const LN_RigidBodyConstraintBreakEvent *LN_RuntimeTree::ResolveRigidBodyConstraintBreakEvent(In &in,
    const uint32_t object_expr_index,
    const uint32_t constraint_name_expr_index,
    const uint32_t match_any_expr_index,
    const LN_RigidBodyConstraintMatchMode match_mode)
{
  const LN_TickContext &context = in.context;
  KX_GameObject *owner = nullptr;
  std::string constraint_name;
  LN_RigidBodyConstraintMatchMode resolved_match_mode = LN_RigidBodyConstraintMatchMode::Exact;
  if (!ResolveRigidBodyConstraintBreakEventQuery(in, object_expr_index, constraint_name_expr_index, match_any_expr_index, match_mode, owner, constraint_name, resolved_match_mode))
  {
    return nullptr;
  }
  return m_logicManager->FindRigidBodyConstraintBreakEvent(
      owner, constraint_name, resolved_match_mode);
}

template<class In>
int32_t LN_RuntimeTree::ResolveRigidBodyConstraintBreakEventCount(In &in,
    const uint32_t object_expr_index,
    const uint32_t constraint_name_expr_index,
    const uint32_t match_any_expr_index,
    const LN_RigidBodyConstraintMatchMode match_mode)
{
  const LN_TickContext &context = in.context;
  KX_GameObject *owner = nullptr;
  std::string constraint_name;
  LN_RigidBodyConstraintMatchMode resolved_match_mode = LN_RigidBodyConstraintMatchMode::Exact;
  if (!ResolveRigidBodyConstraintBreakEventQuery(in, object_expr_index, constraint_name_expr_index, match_any_expr_index, match_mode, owner, constraint_name, resolved_match_mode))
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

template<class In>
LN_Value LN_RuntimeTree::ResolveRigidBodyConstraintBreakEvents(In &in,
    const uint32_t object_expr_index,
    const uint32_t constraint_name_expr_index,
    const uint32_t match_any_expr_index,
    const LN_RigidBodyConstraintMatchMode match_mode)
{
  const LN_TickContext &context = in.context;
  LN_Value events;
  events.type = LN_ValueType::List;
  events.exists = true;

  KX_GameObject *owner = nullptr;
  std::string constraint_name;
  LN_RigidBodyConstraintMatchMode resolved_match_mode = LN_RigidBodyConstraintMatchMode::Exact;
  if (!ResolveRigidBodyConstraintBreakEventQuery(in, object_expr_index, constraint_name_expr_index, match_any_expr_index, match_mode, owner, constraint_name, resolved_match_mode))
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

/** A camera's projection for a ray through the screen: the one it was last drawn with, or, for a
 * camera not drawn with (never, or not since its lens changed), the one the engine would draw it
 * with, as ProjectCanvasPixelToWorld asks for it (2026-11-08: Camera Ray took the active camera
 * instead of such a camera). Without an engine (unit tests) the camera's own. Only reads. */
static MT_Matrix4x4 camera_ray_projection(KX_Camera *camera)
{
  if (!camera->hasValidProjectionMatrix()) {
    KX_KetsjiEngine *engine = KX_GetActiveEngine();
    RAS_ICanvas *canvas = engine ? engine->GetCanvas() : nullptr;
    RAS_Rasterizer *rasterizer = engine ? engine->GetRasterizer() : nullptr;
    KX_Scene *scene = camera->GetScene();
    if (canvas != nullptr && rasterizer != nullptr && scene != nullptr &&
        canvas->GetWidth() > 0 && canvas->GetHeight() > 0)
    {
      const RAS_Rect display_area = rasterizer->GetRenderArea(canvas,
                                                              RAS_Rasterizer::RAS_STEREO_LEFTEYE);
      RAS_Rect camera_area;
      RAS_Rect viewport;
      engine->GetSceneViewport(scene, camera, display_area, camera_area, viewport);
      if (viewport.GetWidth() > 0 && viewport.GetHeight() > 0) {
        return engine->GetCameraProjectionMatrix(
            scene, camera, RAS_Rasterizer::RAS_STEREO_LEFTEYE, viewport, camera_area);
      }
    }
  }
  return camera->GetProjectionMatrix();
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

  MT_Matrix4x4 inverse_projection = camera_ray_projection(camera);
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

/* -------------------------------------------------------------------- */
/** \name The state block (2026-10-31)
 *
 * A tree's per-object state lives in one allocation laid out by its program, so a tree's run
 * reads a few neighbouring cache lines instead of a dozen separate lists, and a state that only
 * some nodes need (the 1.5 KB navigation path, the three values of Value Changed) is not carried
 * by every instruction and expression.
 * \{ */

namespace {

/** Bool expression kinds that keep a state between evaluations (BoolExpressionState names their
 * own expression, or for TweenReached, PersistenceSucceeded, BooleanEdgeFalling and the Value
 * Changed outputs, the expression they read): their expressions get a state slot. Any other
 * expression asking for one gets an overflow state, and a warning. */
bool BoolExpressionKindKeepsState(const LN_BoolExpressionKind kind)
{
  switch (kind) {
    case LN_BoolExpressionKind::TweenValue:
    case LN_BoolExpressionKind::PersistenceCompleted:
    case LN_BoolExpressionKind::CollisionEnter:
    case LN_BoolExpressionKind::CollisionStay:
    case LN_BoolExpressionKind::CollisionExit:
    case LN_BoolExpressionKind::BooleanEdge:
    case LN_BoolExpressionKind::Once:
    case LN_BoolExpressionKind::FlowGateOpen:
    case LN_BoolExpressionKind::ValueChanged:
    case LN_BoolExpressionKind::ValueChangedTo:
      return true;
    default:
      return false;
  }
}

/** Opcodes whose instructions keep a motion state (Mouse Look, paths, Reached). */
bool OpcodeKeepsMotionState(const LN_OpCode opcode)
{
  switch (opcode) {
    case LN_OpCode::MouseLook:
    case LN_OpCode::Navigate:
    case LN_OpCode::FollowPath:
    case LN_OpCode::ApplyForceToTarget:
    case LN_OpCode::MoveToward:
      return true;
    default:
      return false;
  }
}

template<typename T>
LN_StateSpan<T> ConstructStateArray(std::byte *base,
                                    const LN_TreeStateLayout &layout,
                                    const LN_TreeStateLayout::Array array)
{
  const uint32_t count = layout.counts[array];
  if (count == 0) {
    return {};
  }
  T *data = reinterpret_cast<T *>(base + layout.offsets[array]);
  std::uninitialized_value_construct_n(data, count);
  return {data, count};
}

template<typename T> void DestroyStateArray(LN_StateSpan<T> &span)
{
  std::destroy_n(span.data(), span.size());
  span = {};
}

/** Moves the first elements of `from` into `to` (both constructed). */
template<typename T> void MoveStatePrefix(LN_StateSpan<T> from, LN_StateSpan<T> to)
{
  const uint32_t count = std::min(from.size(), to.size());
  for (uint32_t index = 0; index < count; index++) {
    to[index] = std::move(from[index]);
  }
}

/** Tests turn slot planning off: every state then lives apart, as before the state block. */
bool g_plan_state_slots = true;

void WarnUnplannedState(const char *what, const uint32_t kind)
{
  if (!g_plan_state_slots) {
    return;
  }
  /* Trees record on helper threads too (step 7). */
  static std::mutex warned_mutex;
  static std::unordered_set<uint64_t> warned;
  const uint64_t key = (uint64_t(uint8_t(what[0])) << 32) | kind;
  std::lock_guard<std::mutex> lock(warned_mutex);
  if (warned.insert(key).second) {
    CM_Warning("Logic Nodes: no planned state slot for " << what << " " << kind
                                                        << " (kept apart; tell the developers)");
  }
}

}  // namespace

bool LN_TreeStateLayout::Matches(const LN_Program &program) const
{
  const LN_RegisterExpressionProgram &ir = program.GetRegisterExpressionProgram();
  return init_instruction_count == program.GetInstructionHeaders(LN_Event::OnInit).size() &&
         update_instruction_count == program.GetInstructionHeaders(LN_Event::OnUpdate).size() &&
         fixed_instruction_count ==
             program.GetInstructionHeaders(LN_Event::OnFixedUpdate).size() &&
         bool_expression_count == program.GetBoolExpressions().size() &&
         float_expression_count == program.GetFloatExpressions().size() &&
         time_flow_count == program.GetTimeFlowStateCount() &&
         flow_control_count == program.GetFlowControlStateCount() &&
         query_count == program.GetQueryRuntimeStateCount() &&
         step_gate_count == program.GetStepGates().size() &&
         register_counts[0] == ir.bool_register_count &&
         register_counts[1] == ir.int_register_count &&
         register_counts[2] == ir.float_register_count &&
         register_counts[3] == ir.vector_register_count &&
         register_counts[4] == ir.color_register_count &&
         register_counts[5] == ir.string_register_count &&
         register_counts[6] == ir.generic_value_register_count;
}

std::shared_ptr<const LN_TreeStateLayout> LN_RuntimeTree::BuildStateLayout(
    const LN_Program &program)
{
  using Layout = LN_TreeStateLayout;
  std::shared_ptr<Layout> layout = std::make_shared<Layout>();
  const LN_RegisterExpressionProgram &ir = program.GetRegisterExpressionProgram();
  layout->init_instruction_count = uint32_t(program.GetInstructionHeaders(LN_Event::OnInit).size());
  layout->update_instruction_count = uint32_t(
      program.GetInstructionHeaders(LN_Event::OnUpdate).size());
  layout->fixed_instruction_count = uint32_t(
      program.GetInstructionHeaders(LN_Event::OnFixedUpdate).size());
  layout->bool_expression_count = uint32_t(program.GetBoolExpressions().size());
  layout->float_expression_count = uint32_t(program.GetFloatExpressions().size());
  layout->time_flow_count = program.GetTimeFlowStateCount();
  layout->flow_control_count = program.GetFlowControlStateCount();
  layout->query_count = program.GetQueryRuntimeStateCount();
  layout->step_gate_count = uint32_t(program.GetStepGates().size());
  layout->register_counts[0] = ir.bool_register_count;
  layout->register_counts[1] = ir.int_register_count;
  layout->register_counts[2] = ir.float_register_count;
  layout->register_counts[3] = ir.vector_register_count;
  layout->register_counts[4] = ir.color_register_count;
  layout->register_counts[5] = ir.string_register_count;
  layout->register_counts[6] = ir.generic_value_register_count;

  const std::vector<LN_BoolExpression> &bools = program.GetBoolExpressions();
  layout->bool_state_slots.assign(bools.size(), LN_INVALID_INDEX);
  layout->value_change_slots.assign(bools.size(), LN_INVALID_INDEX);
  uint32_t bool_states = 0;
  uint32_t value_changes = 0;
  for (uint32_t index = 0; index < bools.size() && g_plan_state_slots; index++) {
    if (BoolExpressionKindKeepsState(bools[index].kind)) {
      layout->bool_state_slots[index] = bool_states++;
    }
    if (ELEM(bools[index].kind,
             LN_BoolExpressionKind::ValueChanged,
             LN_BoolExpressionKind::ValueChangedTo))
    {
      layout->value_change_slots[index] = value_changes++;
    }
  }
  const std::vector<LN_FloatExpression> &floats = program.GetFloatExpressions();
  layout->float_state_slots.assign(floats.size(), LN_INVALID_INDEX);
  uint32_t float_states = 0;
  for (uint32_t index = 0; index < floats.size() && g_plan_state_slots; index++) {
    if (floats[index].kind == LN_FloatExpressionKind::StoreValue) {
      layout->float_state_slots[index] = float_states++;
    }
  }
  /* Instruction state indices: On Init, then Frame Update, then Logic + Physics instructions
   * (InstructionStateIndex). */
  const uint32_t instruction_count = layout->init_instruction_count +
                                     layout->update_instruction_count +
                                     layout->fixed_instruction_count;
  layout->motion_slots.assign(instruction_count, LN_INVALID_INDEX);
  uint32_t motion_states = 0;
  uint32_t state_index = 0;
  for (const LN_Event event : {LN_Event::OnInit, LN_Event::OnUpdate, LN_Event::OnFixedUpdate}) {
    for (const LN_InstructionHeader &header : program.GetInstructionHeaders(event)) {
      if (OpcodeKeepsMotionState(header.opcode) && g_plan_state_slots) {
        layout->motion_slots[state_index] = motion_states++;
      }
      state_index++;
    }
  }

  layout->counts[Layout::InstructionStamps] = instruction_count;
  layout->counts[Layout::RegisterFloatValid] = ir.float_register_count;
  layout->counts[Layout::RegisterFloats] = ir.float_register_count;
  layout->counts[Layout::RegisterVectorValid] = ir.vector_register_count;
  layout->counts[Layout::RegisterVectors] = ir.vector_register_count;
  layout->counts[Layout::RegisterBoolValid] = ir.bool_register_count;
  layout->counts[Layout::RegisterBools] = ir.bool_register_count;
  layout->counts[Layout::RegisterIntValid] = ir.int_register_count;
  layout->counts[Layout::RegisterInts] = ir.int_register_count;
  layout->counts[Layout::RegisterColorValid] = ir.color_register_count;
  layout->counts[Layout::RegisterColors] = ir.color_register_count;
  layout->counts[Layout::RegisterStringValid] = ir.string_register_count;
  layout->counts[Layout::RegisterStrings] = ir.string_register_count;
  layout->counts[Layout::RegisterGenericValid] = ir.generic_value_register_count;
  layout->counts[Layout::RegisterGenerics] = ir.generic_value_register_count;
  layout->counts[Layout::StepGates] = layout->step_gate_count;
  layout->counts[Layout::Queries] = layout->query_count;
  layout->counts[Layout::BoolStates] = bool_states;
  layout->counts[Layout::FloatStates] = float_states;
  layout->counts[Layout::TimeFlows] = layout->time_flow_count;
  layout->counts[Layout::FlowControls] = layout->flow_control_count;
  layout->counts[Layout::MotionStates] = motion_states;
  layout->counts[Layout::ValueChanges] = value_changes;

  /* Arrays in that order: what every run reads first (stamps, registers), the rest after. */
  const std::array<std::pair<size_t, size_t>, Layout::ArrayCount> element = {{
      {sizeof(InstructionStamp), alignof(InstructionStamp)},
      {sizeof(uint32_t), alignof(uint32_t)},
      {sizeof(float), alignof(float)},
      {sizeof(uint32_t), alignof(uint32_t)},
      {sizeof(MT_Vector3), alignof(MT_Vector3)},
      {sizeof(uint32_t), alignof(uint32_t)},
      {sizeof(uint8_t), alignof(uint8_t)},
      {sizeof(uint32_t), alignof(uint32_t)},
      {sizeof(int32_t), alignof(int32_t)},
      {sizeof(uint32_t), alignof(uint32_t)},
      {sizeof(MT_Vector4), alignof(MT_Vector4)},
      {sizeof(uint32_t), alignof(uint32_t)},
      {sizeof(std::string), alignof(std::string)},
      {sizeof(uint32_t), alignof(uint32_t)},
      {sizeof(LN_Value), alignof(LN_Value)},
      {sizeof(StepGateState), alignof(StepGateState)},
      {sizeof(QueryExpressionRuntimeState), alignof(QueryExpressionRuntimeState)},
      {sizeof(BoolExpressionRuntimeState), alignof(BoolExpressionRuntimeState)},
      {sizeof(FloatExpressionRuntimeState), alignof(FloatExpressionRuntimeState)},
      {sizeof(TimeFlowRuntimeState), alignof(TimeFlowRuntimeState)},
      {sizeof(FlowControlRuntimeState), alignof(FlowControlRuntimeState)},
      {sizeof(InstructionMotionState), alignof(InstructionMotionState)},
      {sizeof(ValueChangeState), alignof(ValueChangeState)},
  }};
  size_t offset = 0;
  for (size_t array = 0; array < Layout::ArrayCount; array++) {
    const size_t align = element[array].second;
    BLI_assert(align <= LN_StateBlockMemory::kAlignment);
    offset = (offset + align - 1) / align * align;
    layout->offsets[array] = offset;
    offset += element[array].first * layout->counts[array];
  }
  layout->bytes = offset;
  return layout;
}

std::shared_ptr<const LN_TreeStateLayout> LN_RuntimeTree::StateLayoutFor(
    const LN_Program &program)
{
  const std::shared_ptr<const LN_TreeStateLayout> &cached = program.CachedTreeStateLayout();
  if (cached != nullptr && cached->Matches(program)) {
    return cached;
  }
  std::shared_ptr<const LN_TreeStateLayout> layout = BuildStateLayout(program);
  program.CacheTreeStateLayout(layout);
  return layout;
}

void LN_RuntimeTree::DestroyStateBlock()
{
  DestroyStateArray(m_instructionStamps);
  DestroyStateArray(m_registerExpressionState.float_valid);
  DestroyStateArray(m_registerExpressionState.float_values);
  DestroyStateArray(m_registerExpressionState.vector_valid);
  DestroyStateArray(m_registerExpressionState.vector_values);
  DestroyStateArray(m_registerExpressionState.bool_valid);
  DestroyStateArray(m_registerExpressionState.bool_values);
  DestroyStateArray(m_registerExpressionState.int_valid);
  DestroyStateArray(m_registerExpressionState.int_values);
  DestroyStateArray(m_registerExpressionState.color_valid);
  DestroyStateArray(m_registerExpressionState.color_values);
  DestroyStateArray(m_registerExpressionState.string_valid);
  DestroyStateArray(m_registerExpressionState.string_values);
  DestroyStateArray(m_registerExpressionState.generic_valid);
  DestroyStateArray(m_registerExpressionState.generic_values);
  DestroyStateArray(m_stepGates);
  DestroyStateArray(m_queryExpressionStates);
  DestroyStateArray(m_boolStates);
  DestroyStateArray(m_floatStates);
  DestroyStateArray(m_timeFlowStates);
  DestroyStateArray(m_flowControlStates);
  DestroyStateArray(m_motionStates);
  DestroyStateArray(m_valueChangeStates);
  m_stateBlock = LN_StateBlockMemory();
  m_stateLayout.reset();
}

void LN_RuntimeTree::RebuildStateBlock(const StateKeep keep)
{
  const bool preserve = keep != StateKeep::None;
  using Layout = LN_TreeStateLayout;
  /* The old block stays alive until its states have moved over. */
  const std::shared_ptr<const Layout> old_layout = std::move(m_stateLayout);
  LN_StateBlockMemory old_block = std::move(m_stateBlock);
  const LN_StateSpan<InstructionStamp> old_stamps = m_instructionStamps;
  const RegisterExpressionRuntimeState old_registers = m_registerExpressionState;
  const LN_StateSpan<StepGateState> old_step_gates = m_stepGates;
  const LN_StateSpan<QueryExpressionRuntimeState> old_queries = m_queryExpressionStates;
  const LN_StateSpan<BoolExpressionRuntimeState> old_bools = m_boolStates;
  const LN_StateSpan<FloatExpressionRuntimeState> old_floats = m_floatStates;
  const LN_StateSpan<TimeFlowRuntimeState> old_time_flows = m_timeFlowStates;
  const LN_StateSpan<FlowControlRuntimeState> old_flow_controls = m_flowControlStates;
  const LN_StateSpan<InstructionMotionState> old_motions = m_motionStates;
  const LN_StateSpan<ValueChangeState> old_value_changes = m_valueChangeStates;

  m_instructionStamps = {};
  m_registerExpressionState = RegisterExpressionRuntimeState();
  m_stepGates = {};
  m_queryExpressionStates = {};
  m_boolStates = {};
  m_floatStates = {};
  m_timeFlowStates = {};
  m_flowControlStates = {};
  m_motionStates = {};
  m_valueChangeStates = {};
  if (!preserve) {
    m_stateOverflow.reset();
  }

  if (m_program != nullptr) {
    m_stateLayout = StateLayoutFor(*m_program);
    const Layout &layout = *m_stateLayout;
    m_stateBlock = LN_StateBlockMemory(layout.bytes);
    std::byte *base = m_stateBlock.Data();
    RegisterExpressionRuntimeState &registers = m_registerExpressionState;
    m_instructionStamps = ConstructStateArray<InstructionStamp>(
        base, layout, Layout::InstructionStamps);
    registers.float_valid = ConstructStateArray<uint32_t>(base, layout, Layout::RegisterFloatValid);
    registers.float_values = ConstructStateArray<float>(base, layout, Layout::RegisterFloats);
    registers.vector_valid = ConstructStateArray<uint32_t>(
        base, layout, Layout::RegisterVectorValid);
    registers.vector_values = ConstructStateArray<MT_Vector3>(
        base, layout, Layout::RegisterVectors);
    registers.bool_valid = ConstructStateArray<uint32_t>(base, layout, Layout::RegisterBoolValid);
    registers.bool_values = ConstructStateArray<uint8_t>(base, layout, Layout::RegisterBools);
    registers.int_valid = ConstructStateArray<uint32_t>(base, layout, Layout::RegisterIntValid);
    registers.int_values = ConstructStateArray<int32_t>(base, layout, Layout::RegisterInts);
    registers.color_valid = ConstructStateArray<uint32_t>(base, layout, Layout::RegisterColorValid);
    registers.color_values = ConstructStateArray<MT_Vector4>(base, layout, Layout::RegisterColors);
    registers.string_valid = ConstructStateArray<uint32_t>(
        base, layout, Layout::RegisterStringValid);
    registers.string_values = ConstructStateArray<std::string>(
        base, layout, Layout::RegisterStrings);
    registers.generic_valid = ConstructStateArray<uint32_t>(
        base, layout, Layout::RegisterGenericValid);
    registers.generic_values = ConstructStateArray<LN_Value>(
        base, layout, Layout::RegisterGenerics);
    m_stepGates = ConstructStateArray<StepGateState>(base, layout, Layout::StepGates);
    m_queryExpressionStates = ConstructStateArray<QueryExpressionRuntimeState>(
        base, layout, Layout::Queries);
    m_boolStates = ConstructStateArray<BoolExpressionRuntimeState>(
        base, layout, Layout::BoolStates);
    m_floatStates = ConstructStateArray<FloatExpressionRuntimeState>(
        base, layout, Layout::FloatStates);
    m_timeFlowStates = ConstructStateArray<TimeFlowRuntimeState>(base, layout, Layout::TimeFlows);
    m_flowControlStates = ConstructStateArray<FlowControlRuntimeState>(
        base, layout, Layout::FlowControls);
    m_motionStates = ConstructStateArray<InstructionMotionState>(
        base, layout, Layout::MotionStates);
    m_valueChangeStates = ConstructStateArray<ValueChangeState>(
        base, layout, Layout::ValueChanges);
  }

  if (preserve && old_layout != nullptr && m_stateLayout != nullptr) {
    /* What a tree keeps between ticks. Slot states move by the expression or instruction they
     * belong to; one that ends up with no slot (or gets one) moves between block and overflow. */
    MoveStatePrefix(old_stamps, m_instructionStamps);
    MoveStatePrefix(old_time_flows, m_timeFlowStates);
    MoveStatePrefix(old_flow_controls, m_flowControlStates);
    if (keep == StateKeep::All) {
      /* The same program laid out again, maybe in the middle of a step: its step gates too
       * (PrepareStepGates does not prepare a step twice). */
      MoveStatePrefix(old_queries, m_queryExpressionStates);
      MoveStatePrefix(old_step_gates, m_stepGates);
    }
    const auto move_slots = [](const std::vector<uint32_t> &old_slots, auto old_states, auto &&to) {
      for (uint32_t key = 0; key < old_slots.size(); key++) {
        if (old_slots[key] != LN_INVALID_INDEX && old_slots[key] < old_states.size()) {
          to(key) = std::move(old_states[old_slots[key]]);
        }
      }
    };
    move_slots(old_layout->bool_state_slots, old_bools, [&](uint32_t key) -> auto & {
      return BoolExpressionState(key);
    });
    move_slots(old_layout->float_state_slots, old_floats, [&](uint32_t key) -> auto & {
      return FloatExpressionState(key);
    });
    move_slots(old_layout->motion_slots, old_motions, [&](uint32_t key) -> auto & {
      return MotionState(key);
    });
    move_slots(old_layout->value_change_slots, old_value_changes, [&](uint32_t key) -> auto & {
      return ValueChangeStateOf(key);
    });
    /* Overflow states that now have a slot move into the block. */
    if (m_stateOverflow != nullptr) {
      const auto adopt = [](auto &overflow, const std::vector<uint32_t> &slots, auto states) {
        for (auto it = overflow.begin(); it != overflow.end();) {
          if (it->first < slots.size() && slots[it->first] != LN_INVALID_INDEX) {
            states[slots[it->first]] = std::move(it->second);
            it = overflow.erase(it);
          }
          else {
            ++it;
          }
        }
      };
      adopt(m_stateOverflow->bool_states, m_stateLayout->bool_state_slots, m_boolStates);
      adopt(m_stateOverflow->float_states, m_stateLayout->float_state_slots, m_floatStates);
      adopt(m_stateOverflow->motion_states, m_stateLayout->motion_slots, m_motionStates);
      adopt(m_stateOverflow->value_changes, m_stateLayout->value_change_slots, m_valueChangeStates);
      for (auto it = m_stateOverflow->queries.begin();
           keep == StateKeep::All && it != m_stateOverflow->queries.end();)
      {
        if (it->first < m_queryExpressionStates.size()) {
          m_queryExpressionStates[it->first] = std::move(it->second);
          it = m_stateOverflow->queries.erase(it);
        }
        else {
          ++it;
        }
      }
    }
  }

  /* The old block's elements go now, then its memory (old_block). */
  {
    LN_StateSpan<InstructionStamp> stamps = old_stamps;
    RegisterExpressionRuntimeState registers = old_registers;
    LN_StateSpan<StepGateState> step_gates = old_step_gates;
    LN_StateSpan<QueryExpressionRuntimeState> queries = old_queries;
    LN_StateSpan<BoolExpressionRuntimeState> bools = old_bools;
    LN_StateSpan<FloatExpressionRuntimeState> floats = old_floats;
    LN_StateSpan<TimeFlowRuntimeState> time_flows = old_time_flows;
    LN_StateSpan<FlowControlRuntimeState> flow_controls = old_flow_controls;
    LN_StateSpan<InstructionMotionState> motions = old_motions;
    LN_StateSpan<ValueChangeState> value_changes = old_value_changes;
    DestroyStateArray(stamps);
    DestroyStateArray(registers.string_values);
    DestroyStateArray(registers.generic_values);
    DestroyStateArray(step_gates);
    DestroyStateArray(queries);
    DestroyStateArray(bools);
    DestroyStateArray(floats);
    DestroyStateArray(time_flows);
    DestroyStateArray(flow_controls);
    DestroyStateArray(motions);
    DestroyStateArray(value_changes);
  }
  /* A new generation for the new registers (nothing cached in them yet). */
  m_registerExpressionState.cached_tick = UINT64_MAX;
}

void LN_RuntimeTree::RefreshStateBlockLayout()
{
  /* A program drops its cached layout whenever it grows (LN_Program::RebuildExecBlockIR,
   * RebuildRegisterExpressionIR, AddTimeFlowState...), so one pointer compare tells. */
  if (m_program != nullptr && m_stateLayout.get() != m_program->CachedTreeStateLayout().get()) {
    RebuildStateBlock(StateKeep::All);
  }
  BLI_assert(m_program == nullptr || m_stateLayout->Matches(*m_program));
}

void LN_RuntimeTree::SetStateSlotsPlannedForTests(const bool planned)
{
  g_plan_state_slots = planned;
}

void LN_RuntimeTree::SetMemoForTests(const int mode)
{
  g_memo_enabled = mode != 0;
}

void LN_RuntimeTree::SetLeanMemoForTests(const int mode)
{
  g_lean_memo_override = mode < 0 ? -1 : (mode != 0 ? 1 : 0);
}

void LN_RuntimeTree::SetReadTraceForTests(std::vector<ReadTraceEntry> *trace)
{
  g_read_trace = trace;
}

void LN_RuntimeTree::EvaluateExpressionForTests(const char family,
                                                const uint32_t index,
                                                const LN_TickContext &context)
{
  LN_LeanReader in(*this, context);
  switch (family) {
    case 'B':
      in.Bool(index, false);
      break;
    case 'I':
      in.Int(index, 0);
      break;
    case 'F':
      in.Float(index, 0.0f);
      break;
    case 'S':
      in.String(index, std::string());
      break;
    case 'V':
      in.Vector(index, MT_Vector3(0.0f, 0.0f, 0.0f));
      break;
    case 'R':
      in.Rotation(index, LN_Rotation());
      break;
    case 'C':
      in.Color(index, MT_Vector4(0.0f, 0.0f, 0.0f, 1.0f));
      break;
    case 'A':
      in.Value(index, MakeNoneValue());
      break;
    case 'Q':
      in.Query(index);
      break;
  }
}

bool LN_RuntimeTree::PrepareParallelCapture(const LN_TickReadContext *tick_context)
{
  if (m_program == nullptr || m_gameObject == nullptr || tick_context == nullptr) {
    return false;
  }
  constexpr LN_SnapshotChannelMask safe_channels = LN_SNAPSHOT_CHANNEL_OBJECT_IDENTITY |
                                                   LN_SNAPSHOT_CHANNEL_TRANSFORM |
                                                   LN_SNAPSHOT_CHANNEL_COLOR |
                                                   LN_SNAPSHOT_CHANNEL_VISIBILITY |
                                                   LN_SNAPSHOT_CHANNEL_GRAVITY |
                                                   LN_SNAPSHOT_CHANNEL_TIMING |
                                                   LN_SNAPSHOT_CHANNEL_GAME_PROPERTY |
                                                   LN_SNAPSHOT_CHANNEL_TREE_PROPERTY;
  const LN_SnapshotChannelMask declared = LN_SnapshotChannelMaskForProgram(m_program.get());
  if ((declared & ~safe_channels) != 0u) {
    return false;
  }
  /* What a capture would otherwise look up on the engine or the input device. */
  if (((declared & LN_SNAPSHOT_CHANNEL_TIMING) != 0u && !tick_context->has_timing) ||
      ((declared & LN_SNAPSHOT_CHANNEL_GRAVITY) != 0u && !tick_context->has_gravity) ||
      (!tick_context->has_input && tick_context->input_channels != LN_DEP_INPUT_NONE))
  {
    return false;
  }
  RefreshStateBlockLayout();
  if ((declared & LN_SNAPSHOT_CHANNEL_GAME_PROPERTY) != 0u) {
    m_program->GetGamePropertyLayout(); /* made at the first ask, on this thread */
  }
  return true;
}

LN_RuntimeTree::StateBlockInfo LN_RuntimeTree::GetStateBlockInfo() const
{
  StateBlockInfo info;
  info.block_bytes = m_stateBlock.Size();
  info.instruction_stamps = m_instructionStamps.size();
  info.bool_states = m_boolStates.size();
  info.float_states = m_floatStates.size();
  info.motion_states = m_motionStates.size();
  info.value_changes = m_valueChangeStates.size();
  info.time_flows = m_timeFlowStates.size();
  info.flow_controls = m_flowControlStates.size();
  info.queries = m_queryExpressionStates.size();
  if (m_stateOverflow != nullptr) {
    info.overflow_states = uint32_t(
        m_stateOverflow->bool_states.size() + m_stateOverflow->float_states.size() +
        m_stateOverflow->motion_states.size() + m_stateOverflow->value_changes.size() +
        m_stateOverflow->queries.size());
  }
  info.has_own_input = m_snapshot.HasOwnInput();
  return info;
}

LN_RuntimeTree::StateOverflow &LN_RuntimeTree::Overflow()
{
  if (m_stateOverflow == nullptr) {
    m_stateOverflow = std::make_unique<StateOverflow>();
  }
  return *m_stateOverflow;
}

LN_RuntimeTree::BoolExpressionRuntimeState &LN_RuntimeTree::BoolExpressionState(
    const uint32_t expression_index)
{
  if (m_stateLayout != nullptr && expression_index < m_stateLayout->bool_state_slots.size()) {
    const uint32_t slot = m_stateLayout->bool_state_slots[expression_index];
    if (slot != LN_INVALID_INDEX && slot < m_boolStates.size()) {
      return m_boolStates[slot];
    }
    WarnUnplannedState("bool expression kind",
                       uint32_t(m_program->GetBoolExpressions()[expression_index].kind));
  }
  return Overflow().bool_states[expression_index];
}

LN_RuntimeTree::FloatExpressionRuntimeState &LN_RuntimeTree::FloatExpressionState(
    const uint32_t expression_index)
{
  if (m_stateLayout != nullptr && expression_index < m_stateLayout->float_state_slots.size()) {
    const uint32_t slot = m_stateLayout->float_state_slots[expression_index];
    if (slot != LN_INVALID_INDEX && slot < m_floatStates.size()) {
      return m_floatStates[slot];
    }
    WarnUnplannedState("float expression kind",
                       uint32_t(m_program->GetFloatExpressions()[expression_index].kind));
  }
  return Overflow().float_states[expression_index];
}

LN_RuntimeTree::InstructionMotionState &LN_RuntimeTree::MotionState(const uint32_t state_index)
{
  if (m_stateLayout != nullptr && state_index < m_stateLayout->motion_slots.size()) {
    const uint32_t slot = m_stateLayout->motion_slots[state_index];
    if (slot != LN_INVALID_INDEX && slot < m_motionStates.size()) {
      return m_motionStates[slot];
    }
    WarnUnplannedState("motion state of instruction", state_index);
  }
  return Overflow().motion_states[state_index];
}

const LN_RuntimeTree::InstructionMotionState *LN_RuntimeTree::FindMotionState(
    const uint32_t state_index) const
{
  if (m_stateLayout != nullptr && state_index < m_stateLayout->motion_slots.size()) {
    const uint32_t slot = m_stateLayout->motion_slots[state_index];
    if (slot != LN_INVALID_INDEX && slot < m_motionStates.size()) {
      return &m_motionStates[slot];
    }
  }
  if (m_stateOverflow != nullptr) {
    const auto found = m_stateOverflow->motion_states.find(state_index);
    if (found != m_stateOverflow->motion_states.end()) {
      return &found->second;
    }
  }
  return nullptr;
}

LN_RuntimeTree::ValueChangeState &LN_RuntimeTree::ValueChangeStateOf(
    const uint32_t expression_index)
{
  if (m_stateLayout != nullptr && expression_index < m_stateLayout->value_change_slots.size()) {
    const uint32_t slot = m_stateLayout->value_change_slots[expression_index];
    if (slot != LN_INVALID_INDEX && slot < m_valueChangeStates.size()) {
      return m_valueChangeStates[slot];
    }
    WarnUnplannedState("value change of bool expression kind",
                       uint32_t(m_program->GetBoolExpressions()[expression_index].kind));
  }
  return Overflow().value_changes[expression_index];
}

const LN_RuntimeTree::ValueChangeState *LN_RuntimeTree::FindValueChangeState(
    const uint32_t expression_index) const
{
  if (m_stateLayout != nullptr && expression_index < m_stateLayout->value_change_slots.size()) {
    const uint32_t slot = m_stateLayout->value_change_slots[expression_index];
    if (slot != LN_INVALID_INDEX && slot < m_valueChangeStates.size()) {
      return &m_valueChangeStates[slot];
    }
  }
  if (m_stateOverflow != nullptr) {
    const auto found = m_stateOverflow->value_changes.find(expression_index);
    if (found != m_stateOverflow->value_changes.end()) {
      return &found->second;
    }
  }
  return nullptr;
}

/** \} */

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
  /* Once a tick: a Run Logic Tree pass after the tree's own run (or a second pass) in the same
   * step counted the step's time again, so a 1 s Timer called 4 times a step fired after
   * 0.2 s, and a Delay's Done could run twice (fixed 2026-10-31). */
  if (m_latentTimeFlowTick == context.tick_index) {
    return;
  }
  m_latentTimeFlowTick = context.tick_index;

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
        if (state.active && PhaseTickWasSkipped(state.last_update_phase_tick,
                                                context.phase_tick_index,
                                                state.last_update_phase_stride))
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

LN_RuntimeTree::QueryExpressionRuntimeState &LN_RuntimeTree::QueryExpressionState(
    const uint32_t state_index)
{
  if (state_index < m_queryExpressionStates.size()) {
    return m_queryExpressionStates[state_index];
  }
  /* Past the program's query states: only seeded by tests. */
  return Overflow().queries[state_index];
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

bool LN_RuntimeTree::ResolvePathMoveTarget(InstructionMotionState &state,
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
                                        const InstructionMotionState &state,
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
  if (state_index == LN_INVALID_INDEX || state_index >= m_instructionStamps.size()) {
    return;
  }

  m_instructionStamps[state_index].last_execution_tick = tick_index;
  m_instructionStamps[state_index].last_execution_scope_serial = m_activeExecutionScopeSerial;
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

void LN_RuntimeTree::EnsureRegisterExpressionState(const LN_TickContext &context)
{
  if (m_program == nullptr) {
    return;
  }

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

  /* The registers live in the state block, laid out for this program's register file (a
   * program grown after the tree took it gets a new block at the next run: until then its new
   * registers are out of range, and their expressions take the evaluator without registers). */
  if (m_registerExpressionState.cached_tick != context.tick_index) {
    m_registerExpressionState.cached_tick = context.tick_index;
    m_registerExpressionState.active_generation++;
    if (m_registerExpressionState.active_generation == 0u) {
      clear_valid_generations();
      m_registerExpressionState.active_generation = 1u;
    }
  }
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

LN_RuntimeTree::LoopRuntimeState &LN_RuntimeTree::LoopState(const uint32_t loop_frame_index)
{
  if (loop_frame_index >= m_loopStates.size()) {
    m_loopStates.resize(size_t(loop_frame_index) + 1);
  }
  return m_loopStates[loop_frame_index];
}

template<class In>
void LN_RuntimeTree::BeginLoopFrame(In &in,
    const uint32_t loop_frame_index)
{
  const LN_TickContext &context = in.context;
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
                           in.Bool(frame.trigger_bool_expr_index, false);

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
                              in.Value(frame.list_value_expr_index, MakeNoneValue());
    if (list_value.type == LN_ValueType::List && list_value.exists) {
      state.items = std::move(list_value.list_value);
    }
    state.iteration_count = state.items.size();
  }
  else {
    const int32_t count = frame.count_int_expr_index == LN_INVALID_INDEX ?
                              0 :
                              std::max(
                                  0, in.Int(frame.count_int_expr_index, 0));
    state.iteration_count = size_t(count);
  }

  const int32_t requested_ticks =
      frame.ticks_to_complete_int_expr_index == LN_INVALID_INDEX ?
          1 :
          std::max(1, in.Int(frame.ticks_to_complete_int_expr_index, 1));
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

#include "LN_ExecFrame.h"
#include "LN_RuntimeTreeOps.inl"

void LN_RuntimeTree::PrepareLeanProgram() const
{
  if (m_program != nullptr && LN_GetLeanMode() == LN_LeanMode::Lean) {
    m_program->EnsureLeanProgram();
  }
}

void LN_RuntimeTree::CaptureColumnProperties(const uint64_t tick_index, const bool transform)
{
  if (m_program != nullptr && m_gameObject != nullptr) {
    m_snapshot.CaptureGamePropertiesOnly(m_gameObject, *m_program, tick_index, transform);
  }
}

void LN_RuntimeTree::CompleteColumnLaneSnapshot(const LN_TickReadContext *tick_context,
                                                const float fixed_delta,
                                                const uint64_t tick_index,
                                                const bool transform_taken)
{
  if (!m_columnLane) {
    return;
  }
  m_columnLane = false;
  /* As the manager's capture loop: only a tree that captures at all (CaptureSnapshot's test). */
  if (!ShouldCaptureSnapshot()) {
    return;
  }
  RefreshStateBlockLayout();
  m_snapshot.CompleteLaneCapture(
      m_gameObject, *m_program, tick_context, fixed_delta, transform_taken);
  m_snapshotTick = tick_index;
  m_queryCacheWarmTick = UINT64_MAX;
}

uint64_t LN_RuntimeTree::BeginColumnPass(const LN_TickContext &context)
{
  /* ExecuteReady's prelude (On Init has run: a lane needs it). */
  ClearRuntimeFallbackReport(context.tick_index);
  RefreshStateBlockLayout();
  if (context.tick_index == UINT64_MAX || m_loopIterationBudgetTick != context.tick_index) {
    m_remainingLoopIterationBudget = kMaxLoopIterationsPerTick;
    m_loopIterationBudgetTick = context.tick_index;
  }
  /* The Delays, Timers... count the tick's time down before the pass. */
  if (m_hasTimeFlowRuntimeState) {
    TickLatentTimeFlowStates(context);
  }
  if (context.event == LN_Event::OnFixedUpdate && !m_stepGates.empty()) {
    PrepareStepGates(context.phase_tick_index, context.delta_time, context.unscaled_dt);
  }
  /* ExecuteEvent's. */
  m_eventDeltaTime = std::max(0.0, context.delta_time);
  const uint64_t serial = NextExecutionScopeSerial();
  m_activeExecutionScopeSerial = serial;
  return serial;
}

void LN_RuntimeTree::EndColumnPass(const LN_TickContext &context)
{
  if (context.event == LN_Event::OnFixedUpdate && !m_stepGates.empty()) {
    NoteStepGatesRan(context.phase_tick_index, false);
  }
}

uint32_t LN_RuntimeTree::StepGateStepsCovered(const uint32_t step_gate_index) const
{
  if (step_gate_index != LN_INVALID_INDEX && step_gate_index < m_stepGates.size()) {
    return std::max<uint32_t>(m_stepGates[step_gate_index].steps, 1);
  }
  return 1;
}

float LN_RuntimeTree::ColumnStepTimePassed(const uint32_t step_gate_index) const
{
  /* FloatKind's StepTimePassed. */
  if (step_gate_index != LN_INVALID_INDEX && step_gate_index < m_stepGates.size()) {
    return float(m_stepGates[step_gate_index].time_passed);
  }
  return float(m_eventDeltaTime);
}

float LN_RuntimeTree::ColumnGateDelta(const uint32_t step_gate_index,
                                      const bool raw,
                                      const LN_TickContext &context) const
{
  const double delta = step_gate_index != LN_INVALID_INDEX && step_gate_index < m_stepGates.size() ?
                           m_stepGates[step_gate_index].time_passed :
                           context.delta_time;
  return raw ? float(delta) : float(std::max(0.0, delta));
}

void LN_RuntimeTree::ColumnNoteReached(const uint32_t state_index, const uint64_t tick_index)
{
  /* MoveToward's handler. */
  if (state_index != LN_INVALID_INDEX && state_index < m_instructionStamps.size()) {
    InstructionMotionState &state = MotionState(state_index);
    state.last_reached_tick = tick_index;
    state.last_reached_scope_serial = m_activeExecutionScopeSerial;
  }
}

void LN_RuntimeTree::TimeFlowArmNextFrame(const uint32_t state_index,
                                          const bool ui_event_route,
                                          const LN_TickContext &context)
{
  if (TimeFlowRuntimeState *state = FindTimeFlowState(state_index)) {
    state->kind = TimeFlowRuntimeKind::NextFrame;
    state->ui_event_route = ui_event_route;
    state->active = true;
    state->event = LN_Event::OnUpdate;
    state->ignore_timescale = true;
    state->last_update_tick = context.tick_index;
    state->last_update_phase_tick = context.phase_tick_index;
  }
}

void LN_RuntimeTree::TimeFlowArmTimer(const uint32_t state_index,
                                      const bool ui_event_route,
                                      const LN_Event event,
                                      const LN_TickContext &context,
                                      const blender::FunctionRef<float()> duration_input,
                                      const blender::FunctionRef<bool()> ignore_timescale)
{
  if (TimeFlowRuntimeState *state = FindTimeFlowState(state_index)) {
    const float duration = std::max(0.0f, duration_input());
    state->kind = TimeFlowRuntimeKind::Timer;
    state->ui_event_route = ui_event_route;
    state->ignore_timescale = ignore_timescale();
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

void LN_RuntimeTree::TimeFlowArmDelay(const uint32_t state_index,
                                      const bool ui_event_route,
                                      const LN_Event event,
                                      const LN_TickContext &context,
                                      const blender::FunctionRef<float()> duration_input,
                                      const blender::FunctionRef<bool()> ignore_timescale)
{
  if (TimeFlowRuntimeState *state = FindTimeFlowState(state_index)) {
    state->kind = TimeFlowRuntimeKind::Delay;
    state->ui_event_route = ui_event_route;
    state->ignore_timescale = ignore_timescale();
    state->event = TimeFlowContinuationEvent(event);
    state->last_update_tick = context.tick_index;
    state->last_update_phase_tick = context.phase_tick_index;
    const float duration = std::max(0.0f, duration_input());
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
      for (const TimeFlowRuntimeState::PendingPulse &queued_pulse : state->pending_pulses) {
        state->remaining_time = std::min(state->remaining_time, queued_pulse.remaining_time);
      }
    }
  }
}

void LN_RuntimeTree::TimeFlowUpdatePulsify(const uint32_t state_index,
                                           const LN_Event event,
                                           const LN_TickContext &context,
                                           const blender::FunctionRef<float()> interval_input,
                                           const blender::FunctionRef<bool()> ignore_timescale)
{
  if (TimeFlowRuntimeState *state = FindTimeFlowState(state_index)) {
    const float interval = std::max(0.0f, interval_input());
    const LN_Event continuation_event = TimeFlowContinuationEvent(event);
    const bool continuous = state->active && state->event == continuation_event &&
                            IsContinuousPhaseTick(state->last_update_phase_tick,
                                                  context.phase_tick_index,
                                                  context.phase_tick_stride);
    state->kind = TimeFlowRuntimeKind::Pulsify;
    state->ignore_timescale = ignore_timescale();
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
    state->last_update_phase_stride = context.phase_tick_stride;
  }
}

void LN_RuntimeTree::TimeFlowUpdateBarrier(const uint32_t state_index,
                                           const LN_Event event,
                                           const LN_TickContext &context,
                                           const blender::FunctionRef<bool()> condition_input,
                                           const blender::FunctionRef<float()> duration_input,
                                           const blender::FunctionRef<bool()> ignore_timescale)
{
  if (TimeFlowRuntimeState *state = FindTimeFlowState(state_index)) {
    const bool condition = condition_input();
    const LN_Event continuation_event = TimeFlowContinuationEvent(event);
    const bool continuous = state->active && state->event == continuation_event &&
                            IsContinuousPhaseTick(state->last_update_phase_tick,
                                                  context.phase_tick_index,
                                                  context.phase_tick_stride);
    state->kind = TimeFlowRuntimeKind::Barrier;
    state->ignore_timescale = ignore_timescale();
    state->event = continuation_event;
    if (!condition) {
      state->active = false;
      state->accumulated_time = 0.0;
      if (state->pulse_tick != context.tick_index) {
        state->pulse_tick = UINT64_MAX;
      }
    }
    else {
      const float duration = std::max(0.0f, duration_input());
      if (!continuous) {
        state->active = true;
        state->accumulated_time = 0.0;
      }
      else if (context.tick_index != state->last_update_tick) {
        state->accumulated_time += TimeFlowStep(context, state->ignore_timescale);
      }
      state->remaining_time = double(duration);
      if (duration <= 0.0f || state->accumulated_time + LN_TIME_FLOW_EPSILON >= double(duration)) {
        if (context.tick_index != UINT64_MAX) {
          state->pulse_tick = context.tick_index;
        }
        state->accumulated_time = std::max(state->accumulated_time, double(duration));
      }
    }
    state->last_update_tick = context.tick_index;
    state->last_update_phase_tick = context.phase_tick_index;
    state->last_update_phase_stride = context.phase_tick_stride;
  }
}

void LN_RuntimeTree::TimeFlowTryCooldown(const uint32_t state_index,
                                         const bool ui_event_route,
                                         const LN_Event event,
                                         const LN_TickContext &context,
                                         const blender::FunctionRef<float()> duration_input,
                                         const blender::FunctionRef<bool()> ignore_timescale)
{
  if (TimeFlowRuntimeState *state = FindTimeFlowState(state_index)) {
    state->kind = TimeFlowRuntimeKind::Cooldown;
    if (state->active) {
      state->accepted_scope_serial = 0;
      state->blocked_scope_serial = context.execution_scope_serial;
      return;
    }

    state->accepted_scope_serial = context.execution_scope_serial;
    state->blocked_scope_serial = 0;
    state->ui_event_route = ui_event_route;
    float duration = duration_input();
    if (!std::isfinite(duration) || duration <= 0.0f) {
      duration = 0.0f;
    }
    state->ignore_timescale = ignore_timescale();
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

void LN_RuntimeTree::TimeFlowResetCooldown(const uint32_t state_index,
                                           const LN_Event event,
                                           const LN_TickContext &context)
{
  if (TimeFlowRuntimeState *state = FindTimeFlowState(state_index)) {
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

float LN_RuntimeTree::ColumnGateUnscaledDelta(const uint32_t step_gate_index,
                                              const LN_TickContext &context) const
{
  /* FloatKind's UnscaledDeltaTime inside RunLeanRoute's / the column walk's gate time. */
  if (step_gate_index != LN_INVALID_INDEX && step_gate_index < m_stepGates.size()) {
    return float(std::max(0.0, m_stepGates[step_gate_index].unscaled_time_passed));
  }
  return float(std::max(0.0, context.unscaled_dt));
}

namespace {

/** The reader a column run's bridge reads through: the lean reader with the event's lean program
 * (what ExecuteEvent gives the lean frame's reader). */
struct ColumnBridgeReader {
  LN_LeanReader reader;
  ColumnBridgeReader(LN_RuntimeTree &tree, LN_TickContext &context, const LN_LeanProgram *lean)
      : reader(tree, context)
  {
    reader.lean = lean;
  }
};

}  // namespace

bool LN_RuntimeTree::ColumnEvalBool(const uint32_t index,
                                    const bool fallback,
                                    LN_TickContext &context)
{
  if (context.command_buffer != nullptr) {
    /* A command-ordered query read here is the op's (the column walk gives each op a key). */
    context.command_execution_key = NextCommandExecutionKey();
  }
  m_activeExecutionScopeSerial = context.execution_scope_serial;
  ColumnBridgeReader bridge(*this, context, LeanProgramForRun());
  return LeanBool(bridge.reader, index, fallback);
}

int32_t LN_RuntimeTree::ColumnEvalInt(const uint32_t index,
                                      const int32_t fallback,
                                      LN_TickContext &context)
{
  if (context.command_buffer != nullptr) {
    /* A command-ordered query read here is the op's (the column walk gives each op a key). */
    context.command_execution_key = NextCommandExecutionKey();
  }
  m_activeExecutionScopeSerial = context.execution_scope_serial;
  ColumnBridgeReader bridge(*this, context, LeanProgramForRun());
  return LeanInt(bridge.reader, index, fallback);
}

float LN_RuntimeTree::ColumnEvalFloat(const uint32_t index,
                                      const float fallback,
                                      LN_TickContext &context)
{
  if (context.command_buffer != nullptr) {
    /* A command-ordered query read here is the op's (the column walk gives each op a key). */
    context.command_execution_key = NextCommandExecutionKey();
  }
  m_activeExecutionScopeSerial = context.execution_scope_serial;
  ColumnBridgeReader bridge(*this, context, LeanProgramForRun());
  return LeanFloat(bridge.reader, index, fallback);
}

MT_Vector3 LN_RuntimeTree::ColumnEvalVector(const uint32_t index,
                                            const MT_Vector3 &fallback,
                                            LN_TickContext &context)
{
  if (context.command_buffer != nullptr) {
    /* A command-ordered query read here is the op's (the column walk gives each op a key). */
    context.command_execution_key = NextCommandExecutionKey();
  }
  m_activeExecutionScopeSerial = context.execution_scope_serial;
  ColumnBridgeReader bridge(*this, context, LeanProgramForRun());
  return LeanVector(bridge.reader, index, fallback);
}

LN_Rotation LN_RuntimeTree::ColumnEvalRotation(const uint32_t index,
                                               const LN_Rotation &fallback,
                                               LN_TickContext &context)
{
  if (context.command_buffer != nullptr) {
    /* A command-ordered query read here is the op's (the column walk gives each op a key). */
    context.command_execution_key = NextCommandExecutionKey();
  }
  m_activeExecutionScopeSerial = context.execution_scope_serial;
  ColumnBridgeReader bridge(*this, context, LeanProgramForRun());
  return LeanRotation(bridge.reader, index, fallback);
}

MT_Vector4 LN_RuntimeTree::ColumnEvalColor(const uint32_t index,
                                           const MT_Vector4 &fallback,
                                           LN_TickContext &context)
{
  if (context.command_buffer != nullptr) {
    /* A command-ordered query read here is the op's (the column walk gives each op a key). */
    context.command_execution_key = NextCommandExecutionKey();
  }
  m_activeExecutionScopeSerial = context.execution_scope_serial;
  ColumnBridgeReader bridge(*this, context, LeanProgramForRun());
  return LeanColor(bridge.reader, index, fallback);
}

const LN_QueryResult &LN_RuntimeTree::ColumnCastQuery(const uint32_t index,
                                                     LN_TickContext &context,
                                                     QueryExpressionRuntimeState &scratch)
{
  /* QueryEntry without its cache: the result reset as it resets a ray's (only the physics
   * fields once the scratch holds a ray's result), then the kind. */
  m_activeExecutionScopeSerial = context.execution_scope_serial;
  if (scratch.cached_result_physics_only) {
    scratch.cached_result.physics_query = LN_PhysicsQueryResult();
    scratch.cached_result.physics_query_results.clear();
  }
  else {
    scratch.cached_result = LN_QueryResult();
  }
  scratch.cached_result_physics_only = true; /* (a Raycast sets its physics fields only) */
  ColumnBridgeReader bridge(*this, context, LeanProgramForRun());
  QueryKind(bridge.reader, index, m_program->GetQueryExpressions()[index], scratch);
  return scratch.cached_result;
}

void LN_RuntimeTree::ColumnRunOp(const LN_Event event,
                                 const uint32_t op_index,
                                 LN_CommandBuffer &buffer,
                                 LN_TickContext &context)
{
  /* The column walk's per-op prelude, then the op: OpLean with the lean program, or, in the
   * reference mode (and without one), its generic handler (OpDirect). */
  const LN_LeanProgram *lean = LeanProgramForRun();
  const LN_ExecBlockProgram &ir = m_program->GetExecBlockProgram(event);
  if (op_index >= ir.ops.size() ||
      (lean != nullptr && op_index >= lean->events[size_t(event)].ops.size()))
  {
    return;
  }
  LN_CommandBuffer *const previous_buffer = context.command_buffer;
  context.event = event;
  context.command_buffer = &buffer;
  context.command_execution_key = NextCommandExecutionKey();
  m_activeExecutionScopeSerial = context.execution_scope_serial;
  LN_LeanFrame frame(*this, event, m_program->GetInstructionHeaders(event), buffer, context);
  if (lean != nullptr) {
    frame.lean = lean;
    frame.reader.lean = lean;
    OpLean(frame, ir.ops[op_index], lean->events[size_t(event)].ops[op_index]);
  }
  else {
    OpDirect(frame, ir.ops[op_index]);
  }
  context.command_buffer = previous_buffer;
}

void LN_RuntimeTree::ColumnRunLoopBlock(const LN_Event event,
                                        const uint32_t block_index,
                                        LN_CommandBuffer &buffer,
                                        LN_TickContext &context)
{
  const LN_LeanProgram *lean = LeanProgramForRun();
  const LN_ExecBlockProgram &ir = m_program->GetExecBlockProgram(event);
  if (block_index >= ir.blocks.size()) {
    return;
  }
  LN_CommandBuffer *const previous_buffer = context.command_buffer;
  context.event = event;
  context.command_buffer = &buffer;
  m_activeExecutionScopeSerial = context.execution_scope_serial;
  LN_LeanFrame frame(*this, event, m_program->GetInstructionHeaders(event), buffer, context);
  std::vector<uint32_t> repeated_loop_frames;
  const uint64_t scope_serial = context.execution_scope_serial;
  if (lean != nullptr) {
    frame.lean = lean;
    frame.reader.lean = lean;
    const LN_LeanEvent &lean_event = lean->events[size_t(event)];
    RunLoopBody(frame,
                ir,
                ir.blocks[block_index],
                repeated_loop_frames,
                scope_serial,
                LN_INVALID_INDEX,
                [&](const uint32_t op_index) {
                  OpLean(frame, ir.ops[op_index], lean_event.ops[op_index]);
                });
  }
  else {
    RunLoopBody(frame,
                ir,
                ir.blocks[block_index],
                repeated_loop_frames,
                scope_serial,
                LN_INVALID_INDEX,
                [&](const uint32_t op_index) { OpDirect(frame, ir.ops[op_index]); });
  }
  context.command_buffer = previous_buffer;
}

template<class Frame, class RunOp>
void LN_RuntimeTree::ColumnWalkEvent(Frame &frame,
                                     const LN_ExecBlockProgram &ir,
                                     const uint64_t scope_serial,
                                     RunOp &&run_op)
{
  /* The column system's walk of an event's blocks (plan C2 P5-P7; the old system's walk, whose
   * rules it took, was removed with the old system, P10). `run_op` is chosen once per event: a
   * runner that branched per op cost 2-3% a tree. */
  LN_TickContext &context = frame.context;
  const std::vector<LN_StepGate> &gates = m_program->GetStepGates();
  std::vector<uint32_t> repeated_loop_frames;
  /* On Collision's Each Object routes (LN_IsCollisionRepeatRoute keys): each op tells the kinds
   * whose run it belongs to (m_collisionEventRoute; the object is m_collisionEventCursor's), so
   * Collided Object & co. read the run's object inside such a route and the first contact
   * anywhere else. Events without them keep the plain runner (no work per op). */
  const bool collision_runs = std::any_of(
      ir.ui_event_states.begin(), ir.ui_event_states.end(), LN_IsCollisionRepeatRoute);
  const auto run_tagged = [&](const uint32_t op_index) {
    const uint32_t key = ir.ops[op_index].ui_event_state_index;
    m_collisionEventRoute = LN_IsCollisionRepeatRoute(key) ? key : LN_INVALID_INDEX;
    run_op(op_index);
  };
  /* The blocks once, in scope `serial`; with `ui_event_state`, only the ops of that repeat key's
   * route (a repeat for a further event of this tick: On UI Event's, or an Each Object
   * collision event's). */
  const auto walk = [&](auto &&runner, const uint64_t serial, const uint32_t ui_event_state) {
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
      const uint32_t gate = block.step_gate_index;
      if (gate != LN_INVALID_INDEX && !StepGateRuns(gate)) {
        continue;
      }
      if (m_uiEventRoutesOnly && block.loop_frame_index != LN_INVALID_INDEX &&
          !block.ui_event_route)
      {
        continue;
      }
      /* The profile line's exec_blocks (the old walk counted them here; until plan C2 P10 a
       * column pass left them out). */
      if (m_activeProfileCounters != nullptr) {
        m_activeProfileCounters->exec_block_count++;
      }
      const uint32_t completed = block.completed_loop_frame_index;
      if (completed != LN_INVALID_INDEX &&
          (completed >= m_loopStates.size() ||
           m_loopStates[completed].completed_scope_serial != serial))
      {
        continue;
      }
      /* The gate's time and stride for the block (ScopedStepGateTime's rule). */
      const double delta_time = context.delta_time;
      const double unscaled_dt = context.unscaled_dt;
      const uint32_t stride = context.phase_tick_stride;
      if (gate != LN_INVALID_INDEX) {
        if (gate < m_stepGates.size()) {
          context.delta_time = m_stepGates[gate].time_passed;
          context.unscaled_dt = m_stepGates[gate].unscaled_time_passed;
        }
        if (gate < gates.size()) {
          context.phase_tick_stride = gates[gate].interval;
        }
      }
      if (block.loop_frame_index != LN_INVALID_INDEX) {
        if (collision_runs && block.op_count > 0) {
          /* The Loop's own inputs (read when it starts, before its body's first op) belong to
           * the route of its body (every op of a body block shares it). */
          const uint32_t key = ir.ops[block.first_op].ui_event_state_index;
          m_collisionEventRoute = LN_IsCollisionRepeatRoute(key) ? key : LN_INVALID_INDEX;
        }
        RunLoopBody(frame, ir, block, repeated_loop_frames, serial, ui_event_state, runner);
      }
      else {
        context.execution_scope_serial = serial;
        m_activeExecutionScopeSerial = serial;
        const uint32_t end = block.first_op + block.op_count;
        if (ui_event_state == LN_INVALID_INDEX) {
          for (uint32_t op_index = block.first_op; op_index < end; op_index++) {
            runner(op_index);
          }
        }
        else {
          for (uint32_t op_index = block.first_op; op_index < end; op_index++) {
            if (selected(ir.ops[op_index])) {
              runner(op_index);
            }
          }
        }
      }
      context.delta_time = delta_time;
      context.unscaled_dt = unscaled_dt;
      context.phase_tick_stride = stride;
    }
  };
  m_collisionEventCursor = 0;
  if (collision_runs) {
    walk(run_tagged, scope_serial, LN_INVALID_INDEX);
  }
  else {
    walk(run_op, scope_serial, LN_INVALID_INDEX);
  }
  /* On UI Event: each further event that matches a node this frame runs the node's route again,
   * in a scope of its own, its outputs telling about that event. */
  constexpr uint32_t max_ui_event_repeats = uint32_t(LN_UIService::kMaxEventsPerNodeFrame);
  for (const uint32_t state_index : ir.ui_event_states) {
    if (LN_IsCollisionRepeatRoute(state_index)) {
      /* On Collision, Events = Each Object: the route ran for the event's first object in the
       * walk above; it runs again for each further one, in order, in a scope of its own. */
      const uint32_t node = LN_CollisionRepeatRouteNode(state_index);
      if (node >= m_collisionEventNodes.size() ||
          m_collisionEventNodes[node].tick != context.tick_index)
      {
        continue; /* No update this tick: the route's event did not fire. */
      }
      const LN_CollisionEventKind kind = LN_CollisionRepeatRouteKind(state_index);
      for (uint32_t cursor = 1;; ++cursor) {
        /* Read the count every time: the node's lists stay as made this tick (a route's own
         * reads only add contact info), but the vector may be reallocated by a resize. */
        const CollisionEventNodeState &events = m_collisionEventNodes[node];
        const size_t count = kind == LN_CollisionEventKind::Enter ? events.entered.size() :
                             kind == LN_CollisionEventKind::Stay  ? events.stayed.size() :
                                                                   events.exited.size();
        if (cursor >= count) {
          break;
        }
        m_collisionEventCursor = cursor;
        m_registerExpressionState.cached_tick = UINT64_MAX;
        repeated_loop_frames.clear();
        const uint64_t repeat_serial = NextExecutionScopeSerial();
        context.execution_scope_serial = repeat_serial;
        m_activeExecutionScopeSerial = repeat_serial;
        walk(run_tagged, repeat_serial, state_index);
      }
      m_collisionEventCursor = 0;
      continue;
    }
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
      const uint64_t repeat_serial = NextExecutionScopeSerial();
      context.execution_scope_serial = repeat_serial;
      m_activeExecutionScopeSerial = repeat_serial;
      if (collision_runs) {
        walk(run_tagged, repeat_serial, state_index);
      }
      else {
        walk(run_op, repeat_serial, state_index);
      }
    }
    QueryExpressionState(state_index).ui_event_cursor = 0;
  }
  m_collisionEventRoute = LN_INVALID_INDEX;
  m_collisionEventCursor = 0;
  context.execution_scope_serial = scope_serial;
  m_activeExecutionScopeSerial = scope_serial;
}

void LN_RuntimeTree::ColumnRunDirectEvent(const LN_Event event,
                                          const uint64_t scope_serial,
                                          LN_CommandBuffer &buffer,
                                          LN_TickContext &context)
{
  /* ExecuteEvent's frame and choice of route or walk (a frame holds no state between ops). */
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
  const bool reference = LN_GetLeanMode() == LN_LeanMode::Reference;
  const LN_LeanProgram *lean = reference ? nullptr : LeanProgramForRun();
  if (lean != nullptr && lean->events[size_t(event)].ops.size() != ir.ops.size()) {
    lean = nullptr; /* (a program that changed since it was lowered: the generic handlers) */
  }
  if (lean == nullptr && !reference) {
    if (m_activeProfileCounters != nullptr) {
      m_activeProfileCounters->lean_missing++;
    }
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true)) {
      CM_Warning("Logic Nodes: lean missing: a tree ran the generic handlers without its lean "
                 "program");
    }
  }
  LN_LeanFrame frame(*this, event, m_program->GetInstructionHeaders(event), buffer, context);
  if (lean != nullptr) {
    LN_NoteLeanTreeRun();
    if (m_activeProfileCounters != nullptr) {
      m_activeProfileCounters->lean_trees++;
    }
    frame.lean = lean;
    frame.reader.lean = lean;
    const LN_LeanEvent &lean_event = lean->events[size_t(event)];
    if (lean_event.has_route && !m_uiEventRoutesOnly && m_activeProfileCounters == nullptr &&
        g_read_trace == nullptr && LN_GetRouteMode() == LN_RouteMode::On)
    {
      LN_NoteRouteRun();
      RunLeanRoute(frame, lean_event.route, scope_serial);
      return;
    }
    ColumnWalkEvent(frame, ir, scope_serial, [&](const uint32_t op_index) {
      OpLean(frame, ir.ops[op_index], lean_event.ops[op_index]);
    });
    return;
  }
  if (reference) {
    LN_NoteLeanTreeRun();
  }
  ColumnWalkEvent(
      frame, ir, scope_serial, [&](const uint32_t op_index) { OpDirect(frame, ir.ops[op_index]); });
}

void LN_RuntimeTree::ColumnExecuteEvent(const LN_Event event,
                                        LN_CommandBuffer &buffer,
                                        LN_TickContext context)
{
  /* ExecuteEvent's prelude: the event's delta, a new scope. */
  context.event = event;
  m_eventDeltaTime = std::max(0.0, context.delta_time);
  const uint64_t serial = NextExecutionScopeSerial();
  context.execution_scope_serial = serial;
  m_activeExecutionScopeSerial = serial;
  ColumnRunDirectEvent(event, serial, buffer, context);
}

void LN_RuntimeTree::RunInitEvent(LN_CommandBuffer &command_buffer, const LN_TickContext &context)
{
  /* On Init's random numbers count the steps done (RandomTick). In a frame tick that is another
   * count than the frame's own routes use, so the tick's memo keeps nothing across On Init: a
   * random value read by both would be the first one's (a step tick has one count for both). */
  const bool frame_tick = context.event != LN_Event::OnFixedUpdate;
  if (frame_tick) {
    m_registerExpressionState.cached_tick = UINT64_MAX;
  }
  m_randomInInit = true;
  ColumnExecuteEvent(LN_Event::OnInit, command_buffer, context);
  m_randomInInit = false;
  if (frame_tick) {
    m_registerExpressionState.cached_tick = UINT64_MAX;
  }
}

void LN_RuntimeTree::ExecuteReady(LN_CommandBuffer &command_buffer,
                                  const LN_TickContext &context,
                                  LN_RuntimeProfileCounters *profile_counters)
{
  /* ExecuteReady's bookkeeping, in its order. */
  ClearRuntimeFallbackReport(context.tick_index);
  if (!m_runtimeActive || !m_enabled || m_program == nullptr || m_gameObject == nullptr) {
    return;
  }
  RefreshStateBlockLayout();
  if (context.tick_index == UINT64_MAX || m_loopIterationBudgetTick != context.tick_index) {
    m_remainingLoopIterationBudget = kMaxLoopIterationsPerTick;
    m_loopIterationBudgetTick = context.tick_index;
  }
  LN_RuntimeProfileCounters *previous_profile_counters = m_activeProfileCounters;
  if (profile_counters != nullptr) {
    m_activeProfileCounters = profile_counters;
  }
  if (!m_initExecuted) {
    const bool ui_event_routes_only = std::exchange(m_uiEventRoutesOnly, false);
    RunInitEvent(command_buffer, context);
    m_uiEventRoutesOnly = ui_event_routes_only;
    m_initExecuted = true;
  }
  if (m_hasTimeFlowRuntimeState) {
    TickLatentTimeFlowStates(context);
  }
  const bool fixed_gates = context.event == LN_Event::OnFixedUpdate && !m_stepGates.empty();
  if (fixed_gates) {
    PrepareStepGates(context.phase_tick_index, context.delta_time, context.unscaled_dt);
  }
  ColumnExecuteEvent(context.event, command_buffer, context);
  if (fixed_gates) {
    NoteStepGatesRan(context.phase_tick_index, false);
  }
  if (profile_counters != nullptr) {
    m_activeProfileCounters = previous_profile_counters;
  }
}

void LN_RuntimeTree::ExecuteForcedUpdate(LN_CommandBuffer &command_buffer,
                                         const LN_TickContext &context,
                                         LN_RuntimeProfileCounters *profile_counters)
{
  /* ExecuteForcedUpdate's bookkeeping, in its order. */
  ClearRuntimeFallbackReport(context.tick_index);
  if (!m_runtimeActive || m_program == nullptr || m_gameObject == nullptr) {
    return;
  }
  RefreshStateBlockLayout();
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
    RunInitEvent(command_buffer, context);
    m_initExecuted = true;
  }
  if (m_hasTimeFlowRuntimeState) {
    TickLatentTimeFlowStates(context);
  }
  const bool fixed_gates = context.event == LN_Event::OnFixedUpdate && !m_stepGates.empty();
  if (fixed_gates) {
    PrepareForcedStepGates(context.phase_tick_index, context.delta_time, context.unscaled_dt);
  }
  const bool previous_forced = std::exchange(m_stepGatesForced, true);
  ColumnExecuteEvent(context.event, command_buffer, context);
  m_stepGatesForced = previous_forced;
  if (fixed_gates) {
    NoteStepGatesRan(context.phase_tick_index, true);
  }
  if (profile_counters != nullptr) {
    m_activeProfileCounters = previous_profile_counters;
  }
}

bool LN_RuntimeTree::ColumnLoopCompleted(const uint32_t frame, const uint64_t scope_serial) const
{
  return frame < m_loopStates.size() && m_loopStates[frame].completed_scope_serial == scope_serial;
}

bool LN_RuntimeTree::ColumnStampRan(const uint32_t state_index,
                                    const uint64_t tick_index,
                                    const uint64_t scope_serial) const
{
  /* InstructionExecutedNow by stamp index. */
  if (state_index >= m_instructionStamps.size()) {
    return false;
  }
  const InstructionStamp &state = m_instructionStamps[state_index];
  if (state.last_execution_tick != tick_index) {
    return false;
  }
  return scope_serial == 0 || state.last_execution_scope_serial == scope_serial;
}

bool LN_RuntimeTree::ColumnForceToTarget(const MT_Vector3 &target_position,
                                         const float force,
                                         const float reached_threshold,
                                         const float stop_distance,
                                         const MT_Vector3 &position,
                                         bool &r_reached,
                                         MT_Vector3 &r_force)
{
  const float reached = std::max(reached_threshold, 0.0f);
  const float stop = std::max(stop_distance, 0.0f);
  const MT_Vector3 delta = target_position - position;
  const float distance_sq = delta.length2();
  r_reached = false;
  if (!(std::isfinite(delta.x()) && std::isfinite(delta.y()) && std::isfinite(delta.z()))) {
    return false;
  }
  r_reached = distance_sq <= reached * reached;
  if (std::isfinite(force) && distance_sq > stop * stop && distance_sq > MT_EPSILON * MT_EPSILON &&
      std::fabs(force) > MT_EPSILON)
  {
    r_force = delta * (force / std::sqrt(distance_sq));
    return true;
  }
  return false;
}

MT_Vector3 LN_RuntimeTree::ColumnVectorToRotationEuler(const MT_Vector3 &direction,
                                                       const MT_Vector3 &up,
                                                       const int32_t axis_selector)
{
  return VectorToRotationEuler(direction, up, axis_selector);
}

bool LN_RuntimeTree::ColumnValueAsFloat(const LN_Value &value, float &r_value)
{
  return ValueAsFloat(value, r_value);
}

bool LN_RuntimeTree::ColumnValueAsVector(const LN_Value &value, MT_Vector3 &r_vector)
{
  return ValueAsVector(value, r_vector);
}

KX_GameObject *LN_RuntimeTree::ColumnObjectOfValue(const LN_Value &value) const
{
  return ResolveObjectValue(value);
}

void LN_RuntimeTree::PrepareObjectNameLookup() const
{
  if (m_logicManager != nullptr) {
    m_logicManager->PrepareObjectNameLookup();
  }
}

int32_t LN_RuntimeTree::ColumnValueAsInt(const LN_Value &value, const LN_FloatToIntMode mode)
{
  return ValueAsInt(value, mode);
}

bool LN_RuntimeTree::ColumnValueAsBool(const LN_Value &value)
{
  return ValueAsBool(value);
}

KX_GameObject *LN_RuntimeTree::ResolveObjectValueExpressionForColumns(
    const uint32_t value_expr_index, const LN_TickContext &context)
{
  /* A named object (a constant): the same calls without the 304-byte value (ReadObjectInput's
   * way; plan C2 P1: a run of 1 resolved its named objects through the whole value, ~10% of a
   * 1,000-tree scene of runs of 1). */
  LN_LeanReader in(*this, context);
  if (m_program != nullptr) {
    return ReadObjectInput(in, value_expr_index);
  }
  return ResolveObjectValue(in.Value(value_expr_index, MakeNoneValue()));
}

const LN_LeanProgram *LN_RuntimeTree::LeanProgramForRun() const
{
  if (m_program == nullptr || LN_GetLeanMode() != LN_LeanMode::Lean) {
    return nullptr;
  }
  m_program->EnsureLeanProgram(); /* a no-op on a helper thread of the job pool */
  const std::shared_ptr<const LN_LeanProgram> &lean = m_program->GetLeanProgram();
  return lean != nullptr && lean->valid ? lean.get() : nullptr;
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
  /* (A copy given back on the exit tick is not stored again.) */
  payload->last_result.payload_stored = true;
  payload->last_result.owner = nullptr;
  payload->last_result.hit_object = nullptr;
  payload->last_result.hit_objects.clear();
  /* Parallel to hit_objects, which the payload rebuilds from references (some may be gone):
   * only the live result has them (Each Object events never read a payload). */
  payload->last_result.object_contact_counts.clear();
  payload->last_result.object_points.clear();
  payload->last_result.object_normals.clear();
  payload->last_result.object_has_point.clear();
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

static const LN_CollisionContactsForTests *g_collision_contacts_for_tests = nullptr;

void LN_SetCollisionContactsForTests(const LN_CollisionContactsForTests *contacts)
{
  g_collision_contacts_for_tests = contacts;
}

static bool LN_CollisionContactsTestsActive()
{
  return g_collision_contacts_for_tests != nullptr;
}

bool LN_RuntimeTree::CachedCollisionContacts(
    KX_GameObject *owner,
    const bool collect_contacts,
    const std::vector<const PHY_CachedCollisionContact *> *&r_contacts) const
{
  r_contacts = nullptr;
  if (owner == nullptr) {
    return false;
  }
  if (const LN_CollisionContactsForTests *tests = g_collision_contacts_for_tests) {
    const auto found = tests->find(owner);
    if (found == tests->end()) {
      return false;
    }
    r_contacts = &found->second;
    return true;
  }
  KX_Scene *scene = owner->GetScene();
  PHY_IPhysicsEnvironment *env = scene != nullptr ? scene->GetPhysicsEnvironment() : nullptr;
  if (env == nullptr) {
    return false;
  }
  PHY_IPhysicsController *controller = owner->GetPhysicsController();
  return env->GetCachedCollisionContactRefsForObject(owner, r_contacts, collect_contacts) ||
         (controller != nullptr &&
          env->GetCachedCollisionContactRefs(controller, r_contacts, collect_contacts));
}

uint64_t LN_RuntimeTree::CollisionObjectIdentity(KX_GameObject *object)
{
  if (object == nullptr) {
    return 0;
  }
  if (m_logicManager != nullptr) {
    /* The manager's dense handle: removing an object bumps its generation, so an object made
     * later at the same address is another object. Main thread only (it may add an entry). */
    const LN_ObjectHandle handle = m_logicManager->MakeObjectHandle(object);
    return (uint64_t(handle.index) << 32) | uint64_t(handle.generation);
  }
  /* A tree without a manager (unit tests, where no object is removed while touching): its
   * address. */
  return uint64_t(reinterpret_cast<uintptr_t>(object));
}

KX_GameObject *LN_RuntimeTree::ResolveCollisionObjectIdentity(const uint64_t identity) const
{
  if (identity == 0) {
    return nullptr;
  }
  if (m_logicManager != nullptr) {
    LN_ObjectHandle handle;
    handle.index = uint32_t(identity >> 32);
    handle.generation = uint32_t(identity & 0xffffffffu);
    return m_logicManager->ResolveObjectHandle(handle);
  }
  return reinterpret_cast<KX_GameObject *>(uintptr_t(identity));
}

void LN_RuntimeTree::HoldCollisionIdentitiesAsObjects()
{
  if (m_collisionIdentitiesHeld) {
    return;
  }
  m_collisionIdentitiesHeld = true;
  for (CollisionEventNodeState &state : m_collisionEventNodes) {
    for (uint64_t &identity : state.previous) {
      identity = uint64_t(reinterpret_cast<uintptr_t>(ResolveCollisionObjectIdentity(identity)));
    }
    /* The rest is this tick's (and the next manager counts its own ticks): made anew. */
    state.tick = UINT64_MAX;
    state.contacts = false;
    state.current.clear();
    state.current_ids.clear();
    state.entered.clear();
    state.stayed.clear();
    state.exited.clear();
  }
}

void LN_RuntimeTree::RenewCollisionIdentities()
{
  if (!m_collisionIdentitiesHeld) {
    return;
  }
  m_collisionIdentitiesHeld = false;
  for (CollisionEventNodeState &state : m_collisionEventNodes) {
    /* A removed object (0) stays 0: the next update reports it as left, as it would have. */
    for (uint64_t &identity : state.previous) {
      identity = CollisionObjectIdentity(
          reinterpret_cast<KX_GameObject *>(uintptr_t(identity)));
    }
  }
}

bool LN_RuntimeTree::ActiveCollisionEventSlot(const int32_t node_field,
                                              LN_CollisionEventKind &r_kind,
                                              uint32_t &r_cursor) const
{
  if (node_field <= 0 || !LN_IsCollisionRepeatRoute(m_collisionEventRoute) ||
      LN_CollisionRepeatRouteNode(m_collisionEventRoute) != uint32_t(node_field - 1))
  {
    return false;
  }
  r_kind = LN_CollisionRepeatRouteKind(m_collisionEventRoute);
  r_cursor = m_collisionEventCursor;
  return true;
}

int32_t LN_RuntimeTree::CollisionEventCurrentIndex(const CollisionEventNodeState &state,
                                                   const LN_CollisionEventKind kind,
                                                   const uint32_t cursor)
{
  const std::vector<uint32_t> *list = kind == LN_CollisionEventKind::Enter ? &state.entered :
                                      kind == LN_CollisionEventKind::Stay  ? &state.stayed :
                                                                             nullptr;
  if (list == nullptr || cursor >= list->size()) {
    return -1;
  }
  return int32_t((*list)[cursor]);
}

KX_GameObject *LN_RuntimeTree::CollisionEventObject(const CollisionEventNodeState &state,
                                                    const LN_CollisionEventKind kind,
                                                    const uint32_t cursor) const
{
  if (kind == LN_CollisionEventKind::Exit) {
    return cursor < state.exited.size() ? ResolveCollisionObjectIdentity(state.exited[cursor]) :
                                          nullptr;
  }
  const int32_t index = CollisionEventCurrentIndex(state, kind, cursor);
  return index >= 0 && size_t(index) < state.current.size() ? state.current[index] : nullptr;
}

template<class In>
const LN_RuntimeTree::CollisionEventNodeState *LN_RuntimeTree::UpdateCollisionEvents(
    In &in,
    const int32_t node_field,
    const uint32_t object_expr_index,
    const uint32_t property_expr_index,
    const uint32_t material_expr_index,
    const bool contacts)
{
  const uint64_t tick = in.context.tick_index;
  if (node_field <= 0 || m_program == nullptr || tick == UINT64_MAX ||
      uint32_t(node_field - 1) >= m_program->GetCollisionEventNodeCount())
  {
    return nullptr;
  }
  const uint32_t node = uint32_t(node_field - 1);
  if (m_collisionEventNodes.size() < m_program->GetCollisionEventNodeCount()) {
    m_collisionEventNodes.resize(m_program->GetCollisionEventNodeCount());
  }
  CollisionEventNodeState &state = m_collisionEventNodes[node];
  const auto take_contacts = [&](const CollisionResult &result) {
    state.contacts = result.object_contact_counts.size() == state.current.size() &&
                     result.hit_objects.size() == state.current.size() &&
                     std::equal(result.hit_objects.begin(),
                                result.hit_objects.end(),
                                state.current.begin());
    if (state.contacts) {
      state.contact_counts = result.object_contact_counts;
      state.points = result.object_points;
      state.normals = result.object_normals;
      state.has_point = result.object_has_point;
    }
    else {
      state.contact_counts.clear();
      state.points.clear();
      state.normals.clear();
      state.has_point.clear();
    }
  };

  if (state.tick == tick) {
    /* Once a tick: a later read only adds what it needs (each object's contacts, from the same
     * contacts: they do not change within a tick). */
    if (contacts && !state.contacts) {
      take_contacts(ResolveCollisionResult(in,
                                           object_expr_index,
                                           property_expr_index,
                                           material_expr_index,
                                           CollisionResult::Detail::Contacts,
                                           false,
                                           false));
    }
    return &state;
  }

  const CollisionResult &result = ResolveCollisionResult(
      in,
      object_expr_index,
      property_expr_index,
      material_expr_index,
      contacts ? CollisionResult::Detail::Contacts : CollisionResult::Detail::Objects,
      false,
      false);
  state.tick = tick;
  state.current.assign(result.hit_objects.begin(), result.hit_objects.end());
  if (contacts) {
    take_contacts(result);
  }
  else {
    take_contacts(CollisionResult{});
  }
  const size_t count = state.current.size();
  state.current_ids.resize(count);
  for (size_t index = 0; index < count; index++) {
    state.current_ids[index] = CollisionObjectIdentity(state.current[index]);
  }

  /* The compare with the last update: a short scan for a few objects, a set past that. */
  constexpr size_t kShortScan = 16;
  state.entered.clear();
  state.stayed.clear();
  state.exited.clear();
  if (count <= kShortScan && state.previous.size() <= kShortScan) {
    for (size_t index = 0; index < count; index++) {
      const bool was_touching = std::find(state.previous.begin(),
                                          state.previous.end(),
                                          state.current_ids[index]) != state.previous.end();
      (was_touching ? state.stayed : state.entered).push_back(uint32_t(index));
    }
    for (const uint64_t identity : state.previous) {
      if (std::find(state.current_ids.begin(), state.current_ids.end(), identity) ==
          state.current_ids.end())
      {
        state.exited.push_back(identity);
      }
    }
  }
  else {
    blender::Set<uint64_t> &ids = m_collisionIdentityScratch;
    ids.clear_and_keep_capacity();
    for (const uint64_t identity : state.previous) {
      ids.add(identity);
    }
    for (size_t index = 0; index < count; index++) {
      (ids.contains(state.current_ids[index]) ? state.stayed : state.entered)
          .push_back(uint32_t(index));
    }
    ids.clear_and_keep_capacity();
    for (const uint64_t identity : state.current_ids) {
      ids.add(identity);
    }
    for (const uint64_t identity : state.previous) {
      if (!ids.contains(identity)) {
        state.exited.push_back(identity);
      }
    }
  }
  state.previous.assign(state.current_ids.begin(), state.current_ids.end());
  return &state;
}

template<class In>
const LN_RuntimeTree::CollisionResult &LN_RuntimeTree::ResolveCollisionResult(In &in,
    const uint32_t object_expr_index,
    const uint32_t property_expr_index,
    const uint32_t material_expr_index,
    const CollisionResult::Detail detail,
    const bool allow_exit_payload,
    const bool store_payload)
{
  const LN_TickContext &context = in.context;
  /* No Object input: the owner. A linked or picked object that is gone (or a link that gives no
   * object) collides with nothing, not with the owner. */
  KX_GameObject *owner = m_gameObject;
  if (object_expr_index != LN_INVALID_INDEX) {
    owner = ResolveObjectValue(
        in.Value(object_expr_index, MakeNoneValue()));
  }

  const std::string property_filter = in.String(property_expr_index, std::string());
  const std::string material_filter =
      material_expr_index != LN_INVALID_INDEX ?
          MaterialFilterNameFromValue(
              this, in.Value(material_expr_index, MakeNoneValue())) :
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
      if (store_payload && cached_result.hit && !cached_result.payload_stored) {
        /* Made by a read that keeps no payload (Each Object's): the first that does keeps it. */
        StoreCollisionEventPayload(cached_result, context.tick_index);
        cached_result.payload_stored = true;
      }
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

  const bool collect_contacts = detail == CollisionResult::Detail::Contacts;
  /* Each touching object once, in contact order. A short scan finds an object among the first
   * kShortScan listed ones; past that an object -> index map does (a floor touched by 4,000 objects
   * scanned the whole list for every contact: 3.4 ms a step, 2026-11-09). With Contacts, each
   * listed object also gets its own contact count and first point / normal (parallel arrays). */
  constexpr size_t kShortScan = 16;
  bool listed_in_map = false;
  const auto list_object = [&](KX_GameObject *other) -> uint32_t {
    std::vector<KX_GameObject *> &objects = collision_result.hit_objects;
    const uint32_t next = uint32_t(objects.size());
    uint32_t index = next;
    if (!listed_in_map) {
      const auto found = std::find(objects.begin(), objects.end(), other);
      if (found != objects.end()) {
        return uint32_t(found - objects.begin());
      }
      if (objects.size() >= kShortScan) {
        m_collisionIndexScratch.clear_and_keep_capacity();
        for (uint32_t listed = 0; listed < next; listed++) {
          m_collisionIndexScratch.add_new(objects[listed], listed);
        }
        m_collisionIndexScratch.add_new(other, next);
        listed_in_map = true;
      }
    }
    else {
      index = m_collisionIndexScratch.lookup_or_add(other, next);
      if (index != next) {
        return index;
      }
    }
    objects.push_back(other);
    if (collect_contacts) {
      collision_result.object_contact_counts.push_back(0);
      collision_result.object_points.push_back(MT_Vector3(0.0f, 0.0f, 0.0f));
      collision_result.object_normals.push_back(MT_Vector3(0.0f, 0.0f, 0.0f));
      collision_result.object_has_point.push_back(0);
    }
    return index;
  };

  const std::vector<const PHY_CachedCollisionContact *> *cached_contacts = nullptr;
  if (CachedCollisionContacts(owner, collect_contacts, cached_contacts)) {
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
      if (detail == CollisionResult::Detail::StateOnly) {
        /* As before the per-object work: the first object listed, its count, done. */
        collision_result.hit_objects.push_back(other);
        collision_result.contact_count += std::max(contact.contact_count, 1);
        return collision_result;
      }
      const uint32_t object_index = list_object(other);
      const int32_t pair_contact_count = std::max(contact.contact_count, 1);
      collision_result.contact_count += pair_contact_count;

      if (collect_contacts) {
        collision_result.object_contact_counts[object_index] += pair_contact_count;
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
          if (collision_result.object_has_point[object_index] == 0) {
            collision_result.object_points[object_index] = point;
            collision_result.object_normals[object_index] = normal;
            collision_result.object_has_point[object_index] = 1;
          }
          collision_result.hit_points.push_back(point);
          collision_result.hit_normals.push_back(normal);
        }
      }
    }

    if (collision_result.hit && store_payload) {
      StoreCollisionEventPayload(collision_result, context.tick_index);
      collision_result.payload_stored = true;
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

  /* No contact cache: ask the physics about every object of the scene (each object once). */
  KX_Scene *scene = owner->GetScene();
  PHY_IPhysicsEnvironment *env = scene != nullptr ? scene->GetPhysicsEnvironment() : nullptr;
  PHY_IPhysicsController *ctrl_a = owner->GetPhysicsController();
  if (env == nullptr || ctrl_a == nullptr || LN_CollisionContactsTestsActive()) {
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
      const size_t object_index = collision_result.hit_objects.size() - 1;
      if (collect_contacts) {
        collision_result.object_contact_counts.push_back(0);
        collision_result.object_points.push_back(MT_Vector3(0.0f, 0.0f, 0.0f));
        collision_result.object_normals.push_back(MT_Vector3(0.0f, 0.0f, 0.0f));
        collision_result.object_has_point.push_back(0);
      }

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
          if (collect_contacts && collision_result.object_has_point[object_index] == 0) {
            collision_result.object_points[object_index] = point;
            collision_result.object_normals[object_index] = normal;
            collision_result.object_has_point[object_index] = 1;
          }
          collision_result.hit_points.push_back(point);
          collision_result.hit_normals.push_back(normal);
        }
      }
      collision_result.contact_count += (detail == CollisionResult::Detail::Contacts) ?
                                            std::max(pair_contact_count, 1) :
                                            1;
      if (collect_contacts) {
        collision_result.object_contact_counts[object_index] = std::max(pair_contact_count, 1);
      }
      delete hit.collData;
      if (detail == CollisionResult::Detail::StateOnly) {
        return collision_result;
      }
    }
    else {
      delete hit.collData;
    }
  }

  if (collision_result.hit && store_payload) {
    StoreCollisionEventPayload(collision_result, context.tick_index);
    collision_result.payload_stored = true;
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

template<class In>
bool LN_RuntimeTree::UpdateTweenValue(In &in,
    const uint32_t tween_expr_index)
{
  const LN_TickContext &context = in.context;
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
    const bool skipped_phase_tick = PhaseTickWasSkipped(
        state.last_update_phase_tick, context.phase_tick_index, context.phase_tick_stride);
    if (tween.float_value > 0.0f && skipped_phase_tick) {
      state.previous_float = 0.0f;
      state.active = false;
      state.previous_bool = true;
      state.initialized = false;
    }
  }
  else {
    forward = in.Bool(tween.input0, false);
    back = in.Bool(tween.input1, false);
  }

  float duration = in.Float(tween.float_expr_index, 1.0f);
  if (!std::isfinite(duration) || duration < 0.0f) {
    duration = 0.0f;
  }
  const bool ignore_timescale = in.Bool(tween.input2, false);
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

template<class In>
float LN_RuntimeTree::ResolveTweenMappedFactor(In &in,
    const uint32_t tween_expr_index)
{
  const LN_TickContext &context = in.context;
  if (tween_expr_index == LN_INVALID_INDEX || m_program == nullptr) {
    return 0.0f;
  }

  const std::vector<LN_BoolExpression> &bool_expressions = m_program->GetBoolExpressions();
  if (tween_expr_index >= bool_expressions.size()) {
    return 0.0f;
  }

  const LN_BoolExpression &tween = bool_expressions[tween_expr_index];
  UpdateTweenValue(in, tween_expr_index);
  const BoolExpressionRuntimeState &state = BoolExpressionState(tween_expr_index);
  float duration = in.Float(tween.float_expr_index, 1.0f);
  if (!std::isfinite(duration) || duration < 0.0f) {
    duration = 0.0f;
  }
  const float linear_factor = duration > 0.0f ? state.previous_float / duration :
                                                (state.previous_bool ? 1.0f : 0.0f);
  const float factor = std::clamp(linear_factor, 0.0f, 1.0f);
  return EvaluateCurveTable(tween.int_expr_index, factor, 1.0f);
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

uint64_t LN_RuntimeTree::CommandSortKey(uint32_t /*instruction_index*/,
                                        uint32_t /*command_sequence*/) const
{
  /* The command buffer preserves record order within one runtime tree. Keep this key to the
   * stable tree identity so control-flow iteration order is never re-sorted by static
   * instruction index. */
  return (static_cast<uint64_t>(m_sceneObjectIndex) << 32) |
         static_cast<uint64_t>(m_appliedTreeIndex);
}

/* The expression kinds (plan phases A13 and A14); last, so every helper above is visible. */
#include "LN_RuntimeTreeKinds.inl"

/* Plan C1-9: after LN_RuntimeTreeKinds.inl, which defines VectorAverageKind. */
LN_QueryResult LN_RuntimeTree::ColumnVectorAverage(const uint32_t query_index,
                                                   const LN_TickContext &context)
{
  LN_QueryResult result;
  const std::vector<LN_QueryExpression> &queries = m_program->GetQueryExpressions();
  if (query_index < queries.size()) {
    LN_LeanReader in(*this, context);
    VectorAverageKind(in, queries[query_index], result);
  }
  return result;
}

float LN_RuntimeTree::ColumnInvariantFloat(const uint32_t index,
                                           const float fallback,
                                           const LN_TickContext &context)
{
  const std::vector<LN_FloatExpression> &floats = m_program->GetFloatExpressions();
  if (index >= floats.size()) {
    return fallback;
  }
  LN_LeanReader in(*this, context);
  return FloatKind(in, index, floats[index], fallback);
}

int32_t LN_RuntimeTree::ColumnInvariantInt(const uint32_t index,
                                           const int32_t fallback,
                                           const LN_TickContext &context)
{
  const std::vector<LN_IntExpression> &ints = m_program->GetIntExpressions();
  if (index >= ints.size()) {
    return fallback;
  }
  LN_LeanReader in(*this, context);
  return IntKind(in, index, ints[index], fallback);
}

bool LN_RuntimeTree::ColumnInvariantBool(const uint32_t index,
                                         const bool fallback,
                                         const LN_TickContext &context)
{
  const std::vector<LN_BoolExpression> &bools = m_program->GetBoolExpressions();
  if (index >= bools.size()) {
    return fallback;
  }
  LN_LeanReader in(*this, context);
  return BoolKind(in, index, bools[index], fallback);
}


/* Plan C1: after LN_RuntimeTreeKinds.inl, which defines ObjectAttributeAsVector. */
bool LN_RuntimeTree::ColumnObjectAttributeVector(KX_GameObject &object,
                                                 const LN_ObjectAttributeId attribute,
                                                 MT_Vector3 &r_vector)
{
  return ObjectAttributeAsVector(object, attribute, r_vector);
}
