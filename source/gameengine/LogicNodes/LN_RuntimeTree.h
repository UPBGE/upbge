/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_RuntimeTree.h
 *  \ingroup logicnodes
 */

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "LN_DenseIds.h"
#include "LN_EventBus.h"
#include "LN_Inventory.h"
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
struct LN_RigidBodyConstraintBreakEvent;

namespace blender {
struct Collection;
struct ID;
}  // namespace blender

/** Forget all Global Properties and which categories were read from disk. Called when a game
 * launch starts; the values persist across restarts and scene changes within one launch. */
void LN_ClearGlobalProperties();

class EXP_Value;
/** A game property's value as Logic Nodes read it, for code without a tree (the UI service):
 * objects in it become references of \a manager's shared domain, the very objects that
 * LN_CommandBuffer::ApplyGamePropertyValue without a tree writes back (by name without a
 * manager). An empty value when \a property is null. Main thread only. */
LN_Value LN_GamePropertyValue(EXP_Value *property, LN_Manager *manager);

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
  /** Register object scroll/grid layout before dependent interactable bounds are collected. */
  void CollectUIViews(const LN_TickContext &context);
  /** Forget values cached while collecting, before the UI resolved and consumed input. */
  void InvalidateUICollectionCaches();
  bool ShouldCaptureSnapshot() const;
  bool ShouldCaptureSnapshotForForcedUpdate() const;
  void ExecuteForcedUpdate(LN_CommandBuffer &command_buffer,
                           const LN_TickContext &context,
                           LN_RuntimeProfileCounters *profile_counters = nullptr);
  void ExecuteReady(LN_CommandBuffer &command_buffer,
                    const LN_TickContext &context,
                    LN_RuntimeProfileCounters *profile_counters = nullptr);

  bool CanTick() const;
  /** Return whether this tree has work in the requested recurring phase. */
  bool CanTick(LN_Event event) const;
  /** Object UI declarations stay registered every frame, even while this tree is paused or has
   * no wired output, so hover, focus and scroll state never reset behind the user's back. */
  bool CollectsObjectUI() const;
  /** While the game is paused, a tree that While Paused does not keep running still runs its
   * UI event routes (button clicks...), and only those. Set before each Frame Update. */
  void SetUIEventRoutesOnly(const bool ui_event_routes_only)
  {
    m_uiEventRoutesOnly = ui_event_routes_only;
  }
  /** Resolve statically-authored While Paused scopes before paused trees are scheduled. */
  void CollectPauseProcessingObjects(std::vector<KX_GameObject *> &r_objects);
  bool OwnsGameObject(KX_GameObject *gameobj) const;
  void DetachGameObject();
  void SetProgram(std::shared_ptr<const LN_Program> program);
  void SetLogicManager(LN_Manager *logic_manager);
  void BindDenseIds(LN_DenseIdRegistry &registry);
  LN_RuntimeRef MakeObjectRef(KX_GameObject *gameobj, const std::string &debug_name = "");
  KX_GameObject *ResolveObjectRef(const LN_RuntimeRef &runtime_ref) const;
  /** Convert references to tree-independent form before storing a value where other trees read
   * it. May create object handles, so worker trees must not call it (event content is shared
   * when the command buffer flushes). */
  void ShareValue(LN_Value &value);
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
  LN_PhysicsQueryResult Raycast(
      const MT_Vector3 &from,
      const MT_Vector3 &to,
      const LN_RuntimeRef &ignore_object_ref,
      uint32_t collision_mask,
      const std::string &property_name = std::string(),
      bool xray = false,
      const std::vector<LN_RuntimeRef> &extra_ignore_object_refs = {},
      LN_InteractionDomain interaction_domain = LN_InteractionDomain::Any,
      bool respect_runtime_visibility = false,
      const KX_Camera *view_camera = nullptr);
  std::vector<LN_PhysicsQueryResult> RaycastAll(
      const MT_Vector3 &from,
      const MT_Vector3 &to,
      const LN_RuntimeRef &ignore_object_ref,
      uint32_t collision_mask,
      const std::string &property_name = std::string(),
      bool xray = false,
      const std::vector<LN_RuntimeRef> &extra_ignore_object_refs = {});
  std::vector<LN_PhysicsQueryResult> RayCast(
      const PHY_RayQuerySettings &settings,
      const LN_RuntimeRef &ignore_object_ref,
      uint32_t collision_mask,
      const std::string &property_name,
      bool xray,
      const std::vector<LN_RuntimeRef> &extra_ignore_object_refs,
      bool &r_blocked,
      LN_PhysicsQueryResult &r_blocker,
      bool &r_supported);
  std::vector<LN_PhysicsQueryResult> ShapeCast(
      const PHY_ShapeCastSettings &settings,
      const LN_RuntimeRef &ignore_object_ref,
      uint32_t collision_mask,
      const std::string &property_name,
      bool xray,
      const std::vector<LN_RuntimeRef> &extra_ignore_object_refs,
      bool &r_blocked,
      LN_PhysicsQueryResult &r_blocker,
      bool &r_supported);

  const LN_Value *GetTreePropertyValue(uint32_t property_ref_index) const;
  bool SetTreePropertyValue(uint32_t property_ref_index, const LN_Value &value);
  void CompletePersistenceOperation(uint32_t expression_index, bool success);
  /** Whether one deferred UI result can be accepted without dropping an older result. */
  bool CanCompleteUIOperation(uint32_t expression_index) const;
  /** Whether the graph reads a UI command's result: one of the node's outputs is wired. */
  bool ReadsUIOperation(uint32_t expression_index) const;
  /** Queue a deferred UI command result. Returns false when the bounded result queue is full. */
  bool CompleteUIOperation(uint32_t expression_index, LN_UIResult result, int32_t handle);
  /** Queue one lifecycle event for an exact Play Action request. */
  bool CompleteActionOperation(uint32_t expression_index, LN_ActionResult result);

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

  uint64_t GetProgramRevision() const
  {
    return m_programRevision;
  }

  /** A UI widget changed a game property of the owner after the snapshot: read them again. */
  void RefreshSnapshotGameProperties()
  {
    m_snapshot.RefreshGameProperties(m_gameObject, m_program.get());
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
  /**
   * Deterministic per-tree budget for loop body executions in one scheduler tick. Excess work is
   * retained and resumed on later fixed ticks instead of freezing the main thread or dropping
   * iterations.
   */
  static constexpr size_t kMaxLoopIterationsPerTick = 10000;

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
    /** LN_TickContext::mouse_ui_serial at its last run, and whether it held the cursor then. */
    uint64_t mouse_look_ui_serial = 0;
    bool mouse_look_centered = false;
    /** Seconds Mouse Look still holds the cursor without turning after it started (over): the
     * system may report where its pointer really is a moment later (Wayland, a window that just
     * opened or got the pointer back). */
    float mouse_look_settle = 0.0f;
    int mouse_look_settle_runs = 0;
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
    uint64_t last_update_phase_tick = UINT64_MAX;
    uint64_t pending_persistence_results = 0;
    uint8_t pending_persistence_count = 0;
    bool last_persistence_success = false;
  };

  struct UICommandCompletion {
    LN_UIResult result = LN_UIResult::RuntimeUnavailable;
    int32_t handle = 0;
  };

  struct UICommandCompletionState {
    std::deque<UICommandCompletion> pending;
    UICommandCompletion current;
    uint64_t current_tick = UINT64_MAX;
    bool has_current = false;
    /** Value outputs retain the most recently completed result after the one-tick flow pulse. */
    bool has_value = false;
  };

  struct ActionCompletionState {
    std::deque<LN_ActionResult> pending;
    LN_ActionResult current = LN_ActionResult::Failed;
    uint64_t current_tick = UINT64_MAX;
    bool has_current = false;
  };

  struct FloatExpressionRuntimeState {
    bool initialized = false;
    float stored_value = 0.0f;
    uint64_t last_store_tick = UINT64_MAX;
  };

  enum class TimeFlowRuntimeKind : uint8_t {
    None = 0,
    NextFrame,
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
    uint64_t last_update_phase_tick = UINT64_MAX;
    uint64_t accepted_scope_serial = 0;
    uint64_t blocked_scope_serial = 0;
    std::vector<PendingPulse> pending_pulses;
    std::vector<LN_Value> active_tree_properties;
    bool has_active_tree_properties = false;
    bool ignore_timescale = false;
    /** Armed by a UI event route: still ticked while only UI event routes run (paused game). */
    bool ui_event_route = false;
    LN_Event event = LN_Event::OnUpdate;
  };

  /** Persistent, fixed-size state for routing and bounded flow-control nodes. */
  struct FlowControlRuntimeState {
    uint32_t next_route_index = 0;
    uint32_t last_route_index = LN_INVALID_INDEX;
    uint32_t accepted_count = 0;
    uint64_t routed_scope_serial = 0;
    uint64_t accepted_scope_serial = 0;
    uint64_t blocked_scope_serial = 0;

    bool motion_initialized = false;
    bool motion_valid = false;
    int32_t motion_source_mode = int32_t(LN_MotionSourceMode::Position);
    uint64_t motion_last_sample_tick = UINT64_MAX;
    double motion_last_sample_time = 0.0;
    LN_RuntimeRef motion_source_ref;
    MT_Vector3 motion_position = MT_Vector3(0.0f, 0.0f, 0.0f);
    MT_Vector3 motion_previous_position = MT_Vector3(0.0f, 0.0f, 0.0f);
    MT_Vector3 motion_velocity = MT_Vector3(0.0f, 0.0f, 0.0f);
    MT_Vector3 motion_direction = MT_Vector3(0.0f, 0.0f, 0.0f);
    MT_Vector3 motion_predicted_position = MT_Vector3(0.0f, 0.0f, 0.0f);
    float motion_speed = 0.0f;
  };

  struct QueryExpressionRuntimeState {
    struct HoverRuntimeState {
      uint64_t baseline_tick = UINT64_MAX;
      bool initialized = false;
      bool previous = false;
      bool baseline_initialized = false;
      bool baseline_previous = false;
    };

    uint64_t cached_tick = UINT64_MAX;
    uint64_t cached_command_execution_key = UINT64_MAX;
    LN_QueryResult cached_result;
    bool cached_command_ordered = false;
    std::array<HoverRuntimeState, 3> hover_by_event;
    /** On UI Event: which of this frame's matching events the query reports (0: the first). */
    uint32_t ui_event_cursor = 0;
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

  bool UpdateTweenValue(uint32_t tween_expr_index, const LN_TickContext &context);
  bool EvaluateMouseMoved(bool each_logic_tick) const;
  float EvaluateCurveTable(uint32_t table_index, float input, float influence) const;
  float ResolveTweenMappedFactor(uint32_t tween_expr_index, const LN_TickContext &context);

  struct LoopRuntimeState {
    bool active = false;
    bool triggered = false;
    bool break_requested = false;
    bool trigger_previous = false;
    size_t iteration_index = 0;
    size_t iteration_count = 0;
    size_t batch_index = 0;
    size_t batch_count = 1;
    size_t batch_end_iteration = 0;
    uint64_t last_batch_tick_index = UINT64_MAX;
    uint64_t completed_scope_serial = 0;
    std::vector<LN_Value> items;
    LN_Value current_value;
  };

  size_t m_remainingLoopIterationBudget = kMaxLoopIterationsPerTick;
  uint64_t m_loopIterationBudgetTick = UINT64_MAX;

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
  void EnsureFlowControlStateSize(bool preserve_existing);
  void EnsureQueryExpressionStateSize();
  void EnsureTreePropertyStateSize(bool preserve_existing);
  void RefreshOptimizedPathEligibility();
  BoolExpressionRuntimeState &BoolExpressionState(uint32_t expression_index);
  FloatExpressionRuntimeState &FloatExpressionState(uint32_t expression_index);
  TimeFlowRuntimeState *FindTimeFlowState(uint32_t state_index);
  const TimeFlowRuntimeState *FindTimeFlowState(uint32_t state_index) const;
  FlowControlRuntimeState *FindFlowControlState(uint32_t state_index);
  const FlowControlRuntimeState *FindFlowControlState(uint32_t state_index) const;
  void TickLatentTimeFlowStates(const LN_TickContext &context);
  QueryExpressionRuntimeState &QueryExpressionState(uint32_t state_index);
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
  const CollisionResult &ResolveCollisionResult(
      uint32_t object_expr_index,
      uint32_t property_expr_index,
      uint32_t material_expr_index,
      const LN_TickContext &context,
      CollisionResult::Detail detail = CollisionResult::Detail::StateOnly,
      bool allow_exit_payload = true);
  bool ResolveRigidBodyConstraintBreakEventQuery(uint32_t object_expr_index,
                                                 uint32_t constraint_name_expr_index,
                                                 uint32_t match_any_expr_index,
                                                 LN_RigidBodyConstraintMatchMode match_mode,
                                                 const LN_TickContext &context,
                                                 KX_GameObject *&r_owner,
                                                 std::string &r_constraint_name,
                                                 LN_RigidBodyConstraintMatchMode &r_match_mode);
  const LN_RigidBodyConstraintBreakEvent *ResolveRigidBodyConstraintBreakEvent(
      uint32_t object_expr_index,
      uint32_t constraint_name_expr_index,
      uint32_t match_any_expr_index,
      LN_RigidBodyConstraintMatchMode match_mode,
      const LN_TickContext &context);
  int32_t ResolveRigidBodyConstraintBreakEventCount(uint32_t object_expr_index,
                                                    uint32_t constraint_name_expr_index,
                                                    uint32_t match_any_expr_index,
                                                    LN_RigidBodyConstraintMatchMode match_mode,
                                                    const LN_TickContext &context);
  LN_Value ResolveRigidBodyConstraintBreakEvents(uint32_t object_expr_index,
                                                 uint32_t constraint_name_expr_index,
                                                 uint32_t match_any_expr_index,
                                                 LN_RigidBodyConstraintMatchMode match_mode,
                                                 const LN_TickContext &context);
  MT_Vector3 ResolveVectorExpression(uint32_t expression_index,
                                     const MT_Vector3 &fallback,
                                     const LN_TickContext &context);
  LN_Rotation ResolveRotationExpression(uint32_t expression_index,
                                        const LN_Rotation &fallback,
                                        const LN_TickContext &context);
  MT_Vector4 ResolveColorExpression(uint32_t expression_index,
                                    const MT_Vector4 &fallback,
                                    const LN_TickContext &context);
  KX_Camera *GetActiveCamera() const;
  KX_Camera *ResolveCameraValue(const LN_Value &value) const;
  KX_GameObject *ResolveObjectValue(const LN_Value &value, bool include_inactive = false) const;
  /** Like ResolveObjectValue, but a reference to an object that is gone stays gone: it does not
   * find another object of the same name. */
  KX_GameObject *ResolveLiveObjectValue(const LN_Value &value, bool include_inactive = false) const;
  /** An item value: text (one of it), an inventory slot, or an object, which is its `item` and
   * `count` game properties (else its name and one). */
  bool ItemOf(const LN_Value &value, std::string &r_item, int32_t &r_count) const;
  std::string ItemNameOf(const LN_Value &value) const;
  /** Where a Library input is empty: the scene's Item Library (Scene > Items and UI). */
  const blender::Collection *SceneItemLibrary() const;
  struct InventoryEditResult {
    int32_t amount = 0;
    /** Add Item: how many it was asked to add (all left over when the edit cannot be kept). */
    int32_t requested = 0;
    std::string made;
    LN_InventoryOutcome outcome = LN_InventoryOutcome::None;
    bool craft = false;
    /** The Removed output: how many it removed or crafted; Add Item: how many went in. */
    int32_t Changed(const LN_InventoryOperation operation) const
    {
      return operation == LN_InventoryOperation::AddItem ? std::max(0, requested - amount) :
                                                           amount;
    }
  };
  /** The Inventory node's operation on `slots`, with the inputs `edit` holds. Amount 0 means all:
   * the whole slot, every one of the item, or the count an added slot or object carries.
   * `other_slots`: Move To Slot into that other inventory. `list_rules` / `other_list_rules`:
   * the Inventory Rules of the two lists (of `owner`), used unless the node's Override is on;
   * null (a list value, no object) uses the node's own inputs. Their Accept Categories always
   * apply. */
  InventoryEditResult RunInventoryEdit(const LN_ValueExpression &edit,
                                       std::vector<LN_Value> &slots,
                                       const LN_TickContext &context,
                                       std::vector<LN_Value> *other_slots = nullptr,
                                       const LN_InventoryRules *list_rules = nullptr,
                                       const LN_InventoryRules *other_list_rules = nullptr,
                                       KX_GameObject *owner = nullptr);
  /** Convert references back into this tree's tables after reading a shared value. */
  void LocalizeValue(LN_Value &value);
  std::vector<LN_RuntimeRef> ResolveIgnoreObjectRefs(uint32_t object_expr_index,
                                                     uint32_t list_expr_index,
                                                     const LN_TickContext &context);
  PHY_IPhysicsController *ResolveRigidBodyAttributeController(uint32_t object_expr_index,
                                                              const LN_TickContext &context);
  KX_GameObject *FindSceneObjectByName(const std::string &name,
                                       bool include_inactive = false) const;
  LN_PhysicsQueryResult RaycastMouseOverTarget(const MT_Vector3 &from,
                                               const MT_Vector3 &to,
                                               KX_GameObject *target_object,
                                               KX_Camera *camera,
                                               const LN_RuntimeRef &ignore_object_ref,
                                               LN_InteractionDomain interaction_domain,
                                               uint64_t command_execution_key);
  LN_PhysicsQueryResult RaycastPointer(const MT_Vector3 &from,
                                       const MT_Vector3 &to,
                                       KX_Camera *camera,
                                       const LN_RuntimeRef &ignore_object_ref,
                                       uint32_t collision_mask,
                                       const std::string &property_name,
                                       bool xray,
                                       LN_InteractionDomain interaction_domain,
                                       uint64_t command_execution_key);
  /** The main-world camera under the mouse (null outside player 0's view while the window is
   * split: the mouse is theirs); for the object a held mouse press drags, that press's camera,
   * so the drag goes on past the view's edge. */
  KX_Camera *PointerWorldCamera(KX_Scene *scene, const KX_GameObject *target = nullptr) const;
  bool ComputeMouseRay(KX_Camera *camera,
                       float distance,
                       MT_Vector3 &r_from,
                       MT_Vector3 &r_to) const;
  bool ProjectWorldToScreen(KX_Camera *camera,
                            const MT_Vector3 &world_position,
                            MT_Vector3 &r_screen_position) const;
  bool ProjectScreenToWorld(KX_Camera *camera,
                            float screen_x,
                            float screen_y,
                            float depth,
                            MT_Vector3 &r_world_position) const;
  bool ProjectCanvasPixelToWorld(KX_Camera *camera,
                                 float pixel_x,
                                 float pixel_y,
                                 float depth,
                                 MT_Vector3 &r_world_position) const;
  /** Stable cross-tree ordering key; intra-tree order is command-record order. */
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
  uint64_t m_programRevision = 1;
  KX_GameObject *m_gameObject = nullptr;
  LN_Manager *m_logicManager = nullptr;
  uint32_t m_sceneObjectIndex = 0;
  uint32_t m_appliedTreeIndex = 0;
  bool m_enabled = true;
  bool m_runtimeActive = true;
  bool m_initExecuted = false;
  bool m_uiEventRoutesOnly = false;
  uint64_t m_snapshotTick = UINT64_MAX;
  uint64_t m_queryCacheWarmTick = UINT64_MAX;
  uint64_t m_executionScopeSerial = 0;
  uint64_t m_activeExecutionScopeSerial = 0;
  /** Monotonic execution point for command-ordered query cache invalidation. */
  uint64_t m_commandExecutionSerial = 0;
  bool m_registerExpressionIRHasProductionBenefit = false;
  LN_SnapshotChannelMask m_snapshotChannelMask = LN_SNAPSHOT_CHANNEL_NONE;
  bool m_hasTimeFlowRuntimeState = false;
  bool m_hasQueryExpressions = false;
  std::vector<InstructionRuntimeState> m_instructionStates;
  std::vector<BoolExpressionRuntimeState> m_boolExpressionStates;
  std::unordered_map<uint32_t, UICommandCompletionState> m_uiCommandCompletions;
  std::unordered_map<uint32_t, ActionCompletionState> m_actionCompletions;
  std::vector<FloatExpressionRuntimeState> m_floatExpressionStates;
  std::vector<TimeFlowRuntimeState> m_timeFlowStates;
  std::vector<FlowControlRuntimeState> m_flowControlStates;
  std::vector<QueryExpressionRuntimeState> m_queryExpressionStates;
  RegisterExpressionRuntimeState m_registerExpressionState;
  std::vector<LN_Rotation> m_rotationExpressionValues;
  std::vector<uint64_t> m_rotationExpressionTicks;
  uint64_t m_collisionResultsTick = UINT64_MAX;
  std::vector<CollisionResult> m_collisionResults;
  std::vector<CollisionEventPayload> m_collisionEventPayloads;
  CollisionResult m_collisionExitPayloadScratch;
  std::vector<ObjectRefSlot> m_objectRefs;
  std::vector<RuntimeRefSlot> m_runtimeRefs;
  std::vector<LN_Value> m_treeProperties;
  std::vector<LoopRuntimeState> m_loopStates;
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
