/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_RuntimeTree.h
 *  \ingroup logicnodes
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "LN_DenseIds.h"
#include "LN_EventBus.h"
#include "LN_NodeStateArena.h"
#include "LN_Performance.h"
#include "LN_Program.h"
#include "LN_Snapshot.h"
#include "LN_Types.h"

class KX_GameObject;
class KX_NavMeshObject;
class KX_Camera;
class KX_Scene;
class LN_CommandBuffer;
class LN_DenseIdRegistry;
class LN_Manager;
class LN_RuntimeTreeTestAccess;
struct LN_RuntimeProfileCounters;
class PHY_ICharacter;
class PHY_IPhysicsController;
class PHY_IVehicle;
struct PHY_RayQuerySettings;
struct PHY_ShapeCastSettings;

namespace blender {
struct Collection;
struct ID;
}  // namespace blender

class LN_RuntimeTree {
 public:
  LN_RuntimeTree(std::shared_ptr<const LN_Program> program,
                 KX_GameObject *gameobj,
                 uint32_t scene_object_index,
                 uint32_t applied_tree_index);

  void CaptureSnapshot(const LN_TickReadContext *tick_context = nullptr,
                       float fixed_delta = -1.0f,
                       uint64_t tick_index = UINT64_MAX,
                       bool force_disabled = false);
  size_t WarmQueryCache(const LN_TickContext &context);
  bool ShouldCaptureSnapshot() const;
  bool ShouldCaptureSnapshotForForcedUpdate() const;
  void ExecuteForcedUpdate(LN_CommandBuffer &command_buffer,
                           const LN_TickContext &context,
                           LN_RuntimeProfileCounters *profile_counters = nullptr);
  void ExecuteReady(LN_CommandBuffer &command_buffer,
                    const LN_TickContext &context,
                    LN_RuntimeProfileCounters *profile_counters = nullptr);

  bool CanTick() const;
  bool OwnsGameObject(KX_GameObject *gameobj) const;
  void DetachGameObject();
  void SetProgram(std::shared_ptr<const LN_Program> program);
  void SetLogicManager(LN_Manager *logic_manager);
  void BindDenseIds(LN_DenseIdRegistry &registry);
  LN_RuntimeRef MakeObjectRef(KX_GameObject *gameobj, const std::string &debug_name = "");
  KX_GameObject *ResolveObjectRef(const LN_RuntimeRef &runtime_ref) const;
  void InvalidateObjectRef(KX_GameObject *gameobj);
  LN_RuntimeRef MakeSceneRef(KX_Scene *scene, const std::string &debug_name = "");
  KX_Scene *ResolveSceneRef(const LN_RuntimeRef &runtime_ref) const;
  LN_RuntimeRef MakeCollectionRef(blender::Collection *collection,
                                  const std::string &debug_name = "");
  blender::Collection *ResolveCollectionRef(const LN_RuntimeRef &runtime_ref) const;
  LN_RuntimeRef MakeDatablockRef(blender::ID *id, const std::string &debug_name = "");
  blender::ID *ResolveDatablockRef(const LN_RuntimeRef &runtime_ref) const;
  void InvalidateRuntimeRef(LN_RuntimeRefKind kind, const void *pointer);
  LN_PhysicsControllerHandle MakePhysicsControllerHandle(const LN_RuntimeRef &object_ref) const;
  PHY_IPhysicsController *ResolvePhysicsControllerHandle(
      const LN_PhysicsControllerHandle &handle) const;
  PHY_ICharacter *ResolveCharacterController(const LN_RuntimeRef &object_ref) const;
  PHY_IVehicle *ResolveVehicleController(const LN_RuntimeRef &object_ref) const;
  LN_PhysicsQueryResult Raycast(const MT_Vector3 &from,
                                const MT_Vector3 &to,
                                const LN_RuntimeRef &ignore_object_ref,
                                uint32_t collision_mask,
                                const std::string &property_name = std::string(),
                                bool xray = false,
                                const LN_RuntimeRef &extra_ignore_object_ref = LN_RuntimeRef());
  std::vector<LN_PhysicsQueryResult> RaycastAll(
      const MT_Vector3 &from,
      const MT_Vector3 &to,
      const LN_RuntimeRef &ignore_object_ref,
      uint32_t collision_mask,
      const std::string &property_name = std::string(),
      bool xray = false,
      const LN_RuntimeRef &extra_ignore_object_ref = LN_RuntimeRef());
  std::vector<LN_PhysicsQueryResult> RayCast(
      const PHY_RayQuerySettings &settings,
      const LN_RuntimeRef &ignore_object_ref,
      uint32_t collision_mask,
      const std::string &property_name,
      bool xray,
      const LN_RuntimeRef &extra_ignore_object_ref,
      bool &r_blocked,
      LN_PhysicsQueryResult &r_blocker,
      bool &r_supported);
  std::vector<LN_PhysicsQueryResult> ShapeCast(
      const PHY_ShapeCastSettings &settings,
      const LN_RuntimeRef &ignore_object_ref,
      uint32_t collision_mask,
      const std::string &property_name,
      bool xray,
      const LN_RuntimeRef &extra_ignore_object_ref,
      bool &r_blocked,
      LN_PhysicsQueryResult &r_blocker,
      bool &r_supported);

  const LN_Value *GetTreePropertyValue(uint32_t property_ref_index) const;
  bool SetTreePropertyValue(uint32_t property_ref_index, const LN_Value &value);

  bool IsAttached() const
  {
    return m_gameObject != nullptr;
  }

  KX_GameObject *GetGameObject() const
  {
    return m_gameObject;
  }

  std::shared_ptr<const LN_Program> GetProgram() const
  {
    return m_program;
  }

  const LN_Snapshot &GetSnapshot() const
  {
    return m_snapshot;
  }

  LN_Snapshot &GetSnapshot()
  {
    return m_snapshot;
  }

  bool IsEnabled() const
  {
    return m_enabled;
  }

  void SetEnabled(bool enabled)
  {
    m_enabled = enabled;
  }

  bool IsRuntimeActive() const
  {
    return m_runtimeActive;
  }

  void SetRuntimeActive(bool runtime_active)
  {
    m_runtimeActive = runtime_active;
  }

  uint32_t GetAppliedTreeIndex() const
  {
    return m_appliedTreeIndex;
  }

  uint32_t GetSceneObjectIndex() const
  {
    return m_sceneObjectIndex;
  }

  LN_RuntimeTreeId GetDenseRuntimeTreeId() const
  {
    return m_denseRuntimeTreeId;
  }

  LN_ObjectHandle GetOwnerObjectHandle() const
  {
    return m_ownerObjectHandle;
  }

  LN_EventSubjectId GetEventSubjectId(LN_StringId string_id) const;
  LN_GamePropertyId GetGamePropertyId(uint32_t property_ref_index) const;
  LN_TreePropertyId GetTreePropertyId(uint32_t property_ref_index) const;

  void SetSceneObjectIndex(uint32_t scene_object_index)
  {
    m_sceneObjectIndex = scene_object_index;
  }

  struct RuntimeFallbackHit {
    LN_OpCode opcode = LN_OpCode::Nop;
    LN_RuntimeFallbackReason reason = LN_RuntimeFallbackReason::UnsupportedOpcode;
    LN_Event event = LN_Event::OnFixedUpdate;
    uint32_t count = 0;
    uint32_t first_instruction_index = LN_INVALID_INDEX;
    uint32_t first_source_ref_index = LN_INVALID_INDEX;
    const char *opcode_name = nullptr;
    const char *profile_counter_name = nullptr;
    const char *removal_condition = nullptr;
    bool source_ref_available = false;
    bool hot_path_warning_required = true;
  };

  struct RuntimeExpressionFallbackHit {
    LN_RuntimeExpressionFamily family = LN_RuntimeExpressionFamily::Bool;
    uint32_t kind = 0;
    uint32_t expression_index = LN_INVALID_INDEX;
    LN_RuntimeFallbackReason reason = LN_RuntimeFallbackReason::UnsupportedExpression;
    uint32_t count = 0;
    const char *expression_name = nullptr;
    const char *profile_counter_name = nullptr;
    const char *removal_condition = nullptr;
    bool source_ref_required = true;
    bool hot_path_warning_required = true;
  };

  struct RuntimeSystemFallbackHit {
    LN_RuntimeFallbackReason reason = LN_RuntimeFallbackReason::StaleHandle;
    uint32_t count = 0;
    std::string debug_name;
    const char *profile_counter_name = nullptr;
    const char *removal_condition = nullptr;
    bool hot_path_warning_required = true;
  };

  struct RuntimeFallbackReport {
    uint64_t tick_index = UINT64_MAX;
    uint32_t fallback_block_count = 0;
    uint32_t fallback_instruction_count = 0;
    uint32_t expression_fallback_count = 0;
    uint32_t system_fallback_count = 0;
    uint32_t reason_mask = 0;
    uint32_t expression_reason_mask = 0;
    uint32_t system_reason_mask = 0;
    bool optimized_execution_partial = false;
    std::vector<RuntimeFallbackHit> hits;
    std::vector<RuntimeExpressionFallbackHit> expression_hits;
    std::vector<RuntimeSystemFallbackHit> system_hits;
  };

  const RuntimeFallbackReport &GetRuntimeFallbackReport() const
  {
    return m_runtimeFallbackReport;
  }

 private:
  static constexpr int kMaxPathLength = 128;

  struct InstructionRuntimeState {
    uint64_t last_execution_tick = UINT64_MAX;
    uint64_t last_execution_scope_serial = 0;
    float mouse_look_smooth_x = 0.0f;
    float mouse_look_smooth_y = 0.0f;
    float mouse_look_angle_x = 0.0f;
    float mouse_look_angle_y = 0.0f;
    int32_t mouse_look_previous_x = 0;
    int32_t mouse_look_previous_y = 0;
    bool mouse_look_initialized = false;
    bool mouse_look_has_previous_position = false;
    int path_len = 0;
    int waypoint_index = -1;
    int list_point_index = 0;
    float path[kMaxPathLength * 3] = {};
    MT_Vector3 next_path_point = MT_Vector3(0.0f, 0.0f, 0.0f);
    double path_update_time = -1.0;
    uint64_t last_reached_tick = UINT64_MAX;
    uint64_t last_reached_scope_serial = 0;
  };

  struct BoolExpressionRuntimeState {
    bool initialized = false;
    bool previous_bool = false;
    float previous_float = 0.0f;
    bool active = false;
    bool cached_result = false;
    double start_time = 0.0;
    double due_time = 0.0;
    double last_pulse_time = 0.0;
    bool has_previous_value = false;
    LN_Value previous_value;
    LN_Value stored_old_value;
    LN_Value stored_new_value;
    uint64_t last_update_tick = UINT64_MAX;
  };

  struct FloatExpressionRuntimeState {
    bool initialized = false;
    float stored_value = 0.0f;
    uint64_t last_store_tick = UINT64_MAX;
  };

  enum class TimeFlowRuntimeKind : uint8_t {
    None = 0,
    Timer,
    Delay,
    Pulsify,
    Barrier,
    Cooldown,
  };

  struct TimeFlowRuntimeState {
    struct PendingPulse {
      double remaining_time = 0.0;
      std::vector<LN_Value> tree_properties;
    };

    TimeFlowRuntimeKind kind = TimeFlowRuntimeKind::None;
    bool active = false;
    double remaining_time = 0.0;
    double accumulated_time = 0.0;
    uint64_t pulse_tick = UINT64_MAX;
    uint64_t last_update_tick = UINT64_MAX;
    uint64_t accepted_scope_serial = 0;
    uint64_t blocked_scope_serial = 0;
    std::vector<PendingPulse> pending_pulses;
    std::vector<LN_Value> active_tree_properties;
    bool has_active_tree_properties = false;
    bool ignore_timescale = false;
  };

  struct QueryExpressionRuntimeState {
    uint64_t cached_tick = UINT64_MAX;
    uint64_t cached_command_sort_key = UINT64_MAX;
    LN_QueryResult cached_result;
    uint64_t hover_baseline_tick = UINT64_MAX;
    bool cached_command_ordered = false;
    bool hover_initialized = false;
    bool hover_previous = false;
    bool hover_baseline_initialized = false;
    bool hover_baseline_previous = false;
  };

  struct RegisterExpressionRuntimeState {
    uint64_t cached_tick = UINT64_MAX;
    uint64_t profiled_tick = UINT64_MAX;
    std::vector<bool> bool_values;
    std::vector<int32_t> int_values;
    std::vector<float> float_values;
    std::vector<MT_Vector3> vector_values;
    std::vector<MT_Vector4> color_values;
    std::vector<std::string> string_values;
    std::vector<LN_Value> generic_values;
    std::vector<uint32_t> bool_valid;
    std::vector<uint32_t> int_valid;
    std::vector<uint32_t> float_valid;
    std::vector<uint32_t> vector_valid;
    std::vector<uint32_t> color_valid;
    std::vector<uint32_t> string_valid;
    std::vector<uint32_t> generic_valid;
    uint32_t active_generation = 1;
  };

  struct CollisionResult {
    enum class Detail : uint8_t {
      StateOnly = 0,
      Objects = 1,
      Contacts = 2,
    };

    bool hit = false;
    KX_GameObject *owner = nullptr;
    KX_GameObject *hit_object = nullptr;
    std::vector<KX_GameObject *> hit_objects;
    std::vector<MT_Vector3> hit_points;
    std::vector<MT_Vector3> hit_normals;
    std::string property_filter;
    std::string material_filter;
    int32_t contact_count = 0;
    MT_Vector3 hit_point = MT_Vector3(0.0f, 0.0f, 0.0f);
    MT_Vector3 hit_normal = MT_Vector3(0.0f, 0.0f, 0.0f);
    Detail detail = Detail::StateOnly;
    uint64_t cached_tick = UINT64_MAX;
  };

  struct CollisionEventPayload {
    /* Persistent collision payloads own managed object references. Object pointers
     * in last_result are always cleared and reconstructed into per-tick scratch. */
    CollisionResult last_result;
    LN_RuntimeRef owner_ref;
    LN_RuntimeRef hit_object_ref;
    std::vector<LN_RuntimeRef> hit_object_refs;
    uint64_t last_hit_tick = UINT64_MAX;
    uint64_t exit_tick = UINT64_MAX;
  };

  struct SpawnPoolTimedObject {
    KX_GameObject *object = nullptr;
    double destroy_time = 0.0;
  };

  struct SpawnPoolRuntimeState {
    std::vector<KX_GameObject *> pooled_objects;
    std::vector<SpawnPoolTimedObject> timed_objects;
    KX_GameObject *spawner = nullptr;
    MT_Vector3 reset_position = MT_Vector3(0.0f, 0.0f, -100.0f);
    float lifetime = 3.0f;
    int32_t spawn_type = 0;
    uint32_t raycast_mask = 65535;
    bool visualize = false;
    size_t next_spawn_index = 0;
    uint64_t spawned_tick = UINT64_MAX;
    uint64_t hit_tick = UINT64_MAX;
    KX_GameObject *hit_object = nullptr;
    MT_Vector3 hit_point = MT_Vector3(0.0f, 0.0f, 0.0f);
    MT_Vector3 hit_normal = MT_Vector3(0.0f, 0.0f, 1.0f);
    MT_Vector3 hit_direction = MT_Vector3(0.0f, 0.0f, 1.0f);
  };

  struct SpawnPoolBulletRuntimeState {
    int32_t pool_index = 0;
    KX_GameObject *object = nullptr;
    MT_Vector3 position = MT_Vector3(0.0f, 0.0f, 0.0f);
    MT_Vector3 direction = MT_Vector3(0.0f, 1.0f, 0.0f);
    float speed = 0.0f;
    int32_t spawn_type = 0;
    double destroy_time = 0.0;
    uint32_t raycast_mask = 65535;
    bool visualize = false;
  };

  void UpdateSpawnPoolBullets(const LN_TickContext &context);
  bool UpdateTweenValue(uint32_t tween_expr_index, const LN_TickContext &context);
  float ResolveTweenMappedFactor(uint32_t tween_expr_index, const LN_TickContext &context);

  struct LoopRuntimeState {
    bool active = false;
    bool trigger_previous = false;
    size_t iteration_index = 0;
    size_t iteration_count = 0;
    std::vector<LN_Value> items;
    LN_Value current_value;
  };

  struct ObjectRefSlot {
    KX_GameObject *object = nullptr;
    uint32_t generation = 1;
    std::string debug_name;
  };

  struct RuntimeRefSlot {
    void *pointer = nullptr;
    LN_RuntimeRefKind kind = LN_RuntimeRefKind::None;
    uint32_t generation = 1;
    std::string debug_name;
  };

  friend class LN_Manager;
  friend class LN_RuntimeTreeTestAccess;

  void EnsureInstructionStateSize();
  void EnsureBoolExpressionStateSize();
  void EnsureFloatExpressionStateSize();
  void EnsureTimeFlowStateSize(bool preserve_existing);
  void EnsureQueryExpressionStateSize();
  void EnsureTreePropertyStateSize(bool preserve_existing);
  void EnsureSpawnPoolStateSize(bool preserve_existing);
  void RefreshOptimizedPathEligibility();
  BoolExpressionRuntimeState &BoolExpressionState(uint32_t expression_index);
  FloatExpressionRuntimeState &FloatExpressionState(uint32_t expression_index);
  TimeFlowRuntimeState *FindTimeFlowState(uint32_t state_index);
  const TimeFlowRuntimeState *FindTimeFlowState(uint32_t state_index) const;
  void TickLatentTimeFlowStates(const LN_TickContext &context);
  QueryExpressionRuntimeState &QueryExpressionState(uint32_t state_index);
  SpawnPoolRuntimeState *FindSpawnPoolState(uint32_t state_index);
  const SpawnPoolRuntimeState *FindSpawnPoolState(uint32_t state_index) const;
  SpawnPoolRuntimeState &EnsureSpawnPoolState(uint32_t state_index);
  uint32_t InstructionStateIndex(LN_Event event, uint32_t instruction_index) const;
  bool ResolvePathMoveTarget(InstructionRuntimeState &state,
                             KX_GameObject *target,
                             KX_NavMeshObject *navmesh,
                             const MT_Vector3 &destination,
                             float reach_threshold,
                             MT_Vector3 &r_move_target,
                             bool &r_reached) const;
  void DrawNavigationPath(KX_GameObject *target,
                          const InstructionRuntimeState &state,
                          const MT_Vector3 &destination) const;
  void MarkInstructionExecuted(LN_Event event, uint32_t instruction_index, uint64_t tick_index);
  void ProfileInstructionDispatch(const LN_Instruction &instruction);
  void ProfileExpressionEvaluation(uint32_t fallback_requirements);
  void RecordMissingSnapshotChannelsForExpression(
      const LN_RuntimeExpressionSemantics &semantics) const;
  void ClearRuntimeFallbackReport(uint64_t tick_index);
  void RecordRuntimeFallbackInstruction(LN_Event event,
                                        uint32_t instruction_index,
                                        const LN_Instruction &instruction);
  void RecordRegisterExpressionFallback(LN_RuntimeExpressionFamily family,
                                        uint32_t expression_index);
  void RecordRuntimeSystemFallback(LN_RuntimeFallbackReason reason,
                                   const std::string &debug_name,
                                   const char *profile_counter_name,
                                   const char *removal_condition) const;
  void EnsureRegisterExpressionState(const LN_TickContext &context);
  bool TryResolveRegisterBoolExpression(uint32_t expression_index,
                                        const LN_TickContext &context,
                                        bool &r_value);
  bool TryResolveRegisterIntExpression(uint32_t expression_index,
                                       const LN_TickContext &context,
                                       int32_t &r_value);
  bool TryResolveRegisterFloatExpression(uint32_t expression_index,
                                         const LN_TickContext &context,
                                         float &r_value);
  bool TryResolveRegisterVectorExpression(uint32_t expression_index,
                                          const LN_TickContext &context,
                                          MT_Vector3 &r_value);
  bool TryResolveRegisterColorExpression(uint32_t expression_index,
                                         const LN_TickContext &context,
                                         MT_Vector4 &r_value);
  bool TryResolveRegisterValueExpression(uint32_t expression_index,
                                         const LN_TickContext &context,
                                         LN_Value &r_value);
  bool TryResolveRegisterStringExpression(uint32_t expression_index,
                                          const LN_TickContext &context,
                                          std::string &r_value);
  bool TryEvaluateRegisterExpressionSoABatch(uint32_t expression_index,
                                             const LN_TickContext &context);
  bool TryEvaluateRegisterFloatSoABatch(const LN_RegisterExpressionProgram &ir,
                                        const LN_RegisterExpressionSoABatch &batch,
                                        const LN_TickContext &context);
  void ProfileRegisterExpressionHit();
  void ProfileRegisterExpressionFallback();
  void ProfileRegisterSimdBatch(uint32_t lane_count);
  void ExecuteEvent(LN_Event event,
                    const std::vector<LN_InstructionHeader> &instructions,
                    LN_CommandBuffer &command_buffer,
                    LN_TickContext context);
  LoopRuntimeState &LoopState(uint32_t loop_frame_index);
  void BeginLoopFrame(uint32_t loop_frame_index, const LN_TickContext &context);
  bool AdvanceLoopFrame(uint32_t loop_frame_index);
  const std::vector<LN_EventEntry> &ActiveTickEvents() const;
  const LN_EventEntry *FindFirstActiveEvent(LN_EventSubjectId subject_id,
                                            const std::string &subject,
                                            KX_GameObject *filter_target) const;
  KX_GameObject *ResolveCollisionPayloadObjectRef(const LN_RuntimeRef &object_ref) const;
  bool ResolveBoolExpression(uint32_t expression_index,
                             bool fallback,
                             const LN_TickContext &context);
  int32_t ResolveIntExpression(uint32_t expression_index,
                               int32_t fallback,
             const LN_TickContext &context = LN_TickContext());
  float ResolveFloatExpression(uint32_t expression_index,
                               float fallback,
                               const LN_TickContext &context);
  std::string ResolveStringExpression(uint32_t expression_index,
                                      const std::string &fallback,
                                      const LN_TickContext &context = LN_TickContext());
  LN_StringId ResolveStringExpressionId(uint32_t expression_index) const;
  LN_Value ResolveValueExpression(uint32_t expression_index,
                                  const LN_Value &fallback,
                                  const LN_TickContext &context);
  const LN_QueryResult &ResolveQueryExpression(uint32_t expression_index,
                                               const LN_TickContext &context);
  const CollisionResult &ResolveCollisionResult(uint32_t object_expr_index,
                                                uint32_t property_expr_index,
                                                uint32_t material_expr_index,
                                                const LN_TickContext &context,
                                                CollisionResult::Detail detail =
                                                    CollisionResult::Detail::StateOnly,
                                                bool allow_exit_payload = true);
  MT_Vector3 ResolveVectorExpression(uint32_t expression_index,
                                     const MT_Vector3 &fallback,
                                     const LN_TickContext &context);
  MT_Vector4 ResolveColorExpression(uint32_t expression_index,
                                    const MT_Vector4 &fallback,
                                    const LN_TickContext &context);
  KX_Camera *GetActiveCamera() const;
  KX_Camera *ResolveCameraValue(const LN_Value &value) const;
  KX_GameObject *ResolveObjectValue(const LN_Value &value, bool include_inactive = false) const;
  PHY_IPhysicsController *ResolveRigidBodyAttributeController(uint32_t object_expr_index,
                                                              const LN_TickContext &context);
  KX_GameObject *FindSceneObjectByName(const std::string &name,
                                       bool include_inactive = false) const;
  LN_PhysicsQueryResult RaycastMouseOverTarget(const MT_Vector3 &from,
                                               const MT_Vector3 &to,
                                               KX_GameObject *target_object,
                                               const LN_RuntimeRef &ignore_object_ref);
  bool ComputeMouseRay(float distance, MT_Vector3 &r_from, MT_Vector3 &r_to) const;
  bool ProjectWorldToScreen(KX_Camera *camera,
                            const MT_Vector3 &world_position,
                            MT_Vector3 &r_screen_position) const;
  bool ProjectScreenToWorld(KX_Camera *camera,
                            float screen_x,
                            float screen_y,
                            float depth,
                            MT_Vector3 &r_world_position) const;
  uint64_t CommandSortKey(uint32_t instruction_index, uint32_t command_sequence) const;
  const std::vector<LN_Value> *DelayTreePropertySnapshotForGuard(uint32_t bool_expr_index,
                                                                 uint64_t tick_index) const;
  CollisionEventPayload *FindCollisionEventPayload(KX_GameObject *owner,
                                                   const std::string &property_filter,
                                                   const std::string &material_filter);
  void StoreCollisionEventPayload(const CollisionResult &result, uint64_t tick_index);
  void MarkCollisionExitPayload(const CollisionResult &current_query, uint64_t tick_index);
  const CollisionResult *FindCollisionExitPayload(KX_GameObject *owner,
                                                  const std::string &property_filter,
                                                  const std::string &material_filter,
                                                  uint64_t tick_index);

  std::shared_ptr<const LN_Program> m_program;
  KX_GameObject *m_gameObject = nullptr;
  LN_Manager *m_logicManager = nullptr;
  uint32_t m_sceneObjectIndex = 0;
  uint32_t m_appliedTreeIndex = 0;
  bool m_enabled = true;
  bool m_runtimeActive = true;
  bool m_initExecuted = false;
  uint64_t m_snapshotTick = UINT64_MAX;
  uint64_t m_queryCacheWarmTick = UINT64_MAX;
  uint64_t m_executionScopeSerial = 0;
  uint64_t m_activeExecutionScopeSerial = 0;
  bool m_registerExpressionIRHasProductionBenefit = false;
  LN_SnapshotChannelMask m_snapshotChannelMask = LN_SNAPSHOT_CHANNEL_NONE;
  bool m_hasTimeFlowRuntimeState = false;
  bool m_hasQueryExpressions = false;
  bool m_hasSpawnPoolRuntimeState = false;
  std::vector<InstructionRuntimeState> m_instructionStates;
  std::vector<BoolExpressionRuntimeState> m_boolExpressionStates;
  std::vector<FloatExpressionRuntimeState> m_floatExpressionStates;
  std::vector<TimeFlowRuntimeState> m_timeFlowStates;
  std::vector<QueryExpressionRuntimeState> m_queryExpressionStates;
  RegisterExpressionRuntimeState m_registerExpressionState;
  uint64_t m_collisionResultsTick = UINT64_MAX;
  std::vector<CollisionResult> m_collisionResults;
  std::vector<CollisionEventPayload> m_collisionEventPayloads;
  CollisionResult m_collisionExitPayloadScratch;
  std::vector<ObjectRefSlot> m_objectRefs;
  std::vector<RuntimeRefSlot> m_runtimeRefs;
  std::vector<LN_Value> m_treeProperties;
  std::vector<LoopRuntimeState> m_loopStates;
  std::vector<SpawnPoolRuntimeState> m_spawnPools;
  std::vector<SpawnPoolBulletRuntimeState> m_spawnPoolBullets;
  LN_RuntimeTreeId m_denseRuntimeTreeId;
  LN_ObjectHandle m_ownerObjectHandle;
  std::vector<LN_SharedStringId> m_sharedStringIds;
  std::vector<LN_EventSubjectId> m_eventSubjectIds;
  std::vector<LN_GamePropertyId> m_gamePropertyIds;
  std::vector<LN_TreePropertyId> m_treePropertyIds;
  uint32_t m_activeLoopBodyFrame = LN_INVALID_INDEX;
  const std::vector<LN_Value> *m_activeTreePropertySnapshot = nullptr;
  LN_NodeStateArena m_stateArena;
  LN_Snapshot m_snapshot;
  LN_RuntimeProfileCounters *m_activeProfileCounters = nullptr;
  mutable RuntimeFallbackReport m_runtimeFallbackReport;
};
