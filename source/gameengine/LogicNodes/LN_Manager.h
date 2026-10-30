/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_Manager.h
 *  \ingroup logicnodes
 */

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "LN_CommandBuffer.h"
#include "LN_DenseIds.h"
#include "LN_EventBus.h"
#include "LN_GameState.h"
#include "LN_InputMap.h"
#include "LN_Inventory.h"
#include "LN_ParallelTreeExecutor.h"
#include "LN_Snapshot.h"
#include "MT_Vector3.h"

class KX_GameObject;
class KX_Camera;
class KX_Scene;
class BL_ActionPlayback;
enum class KX_ObjectRemovalReason : uint8_t;
class LN_Program;
class LN_RuntimeTree;
class LN_UIService;

/** Immutable C++ Logic Node binding retained by an authored reconstruction recipe. */
struct LN_RuntimeTreeRecipe {
  std::shared_ptr<const LN_Program> program;
  uint32_t applied_tree_index = 0;
  bool enabled = true;
};

struct LN_RuntimeTreeAttachment {
  std::string tree_name;
  std::string library_path;
  uint32_t applied_tree_index = 0;
  bool enabled = true;
};

struct LN_RuntimeTreeRestoreTarget {
  KX_GameObject *gameobj = nullptr;
  std::vector<LN_RuntimeTreeAttachment> attachments;
};

/** Immutable main-thread record for one automatic rigid-body constraint
 * break. It is delivered immediately to the next fixed Logic Nodes tick and
 * remains available as a one-shot snapshot to the following update tick. */
struct LN_RigidBodyConstraintBreakEvent {
  LN_ObjectHandle owner;
  LN_ObjectHandle target;
  std::string constraint_name;
  MT_Vector3 anchor_position = MT_Vector3(0.0f, 0.0f, 0.0f);
  float linear_impulse = 0.0f;
  float angular_impulse = 0.0f;
  float applied_impulse = 0.0f;
};

class LN_PreparedRuntimeTreeRestore {
 public:
  LN_PreparedRuntimeTreeRestore();
  ~LN_PreparedRuntimeTreeRestore();
  LN_PreparedRuntimeTreeRestore(LN_PreparedRuntimeTreeRestore &&) noexcept;
  LN_PreparedRuntimeTreeRestore &operator=(LN_PreparedRuntimeTreeRestore &&) noexcept;

  LN_PreparedRuntimeTreeRestore(const LN_PreparedRuntimeTreeRestore &) = delete;
  LN_PreparedRuntimeTreeRestore &operator=(const LN_PreparedRuntimeTreeRestore &) = delete;

 private:
  struct Impl;
  void Release();
  void Reset();
  std::unique_ptr<Impl> m_impl;

  friend class LN_Manager;
};

struct LN_TickContext;
class LN_Manager {
 public:
  explicit LN_Manager(KX_Scene &scene);
  ~LN_Manager();

  KX_Scene &GetScene()
  {
    return m_scene;
  }

  LN_UIService &GetUIService();
  const LN_UIService &GetUIService() const;

  LN_RuntimeTree *RegisterCompiledProgram(KX_GameObject *gameobj,
                                          std::shared_ptr<const LN_Program> program,
                                          uint32_t scene_object_index,
                                          uint32_t applied_tree_index,
                                          bool enabled,
                                          bool runtime_active = true);

  bool IsLogicTreeRunning(KX_GameObject *gameobj, const std::string &tree_name) const;
  void QueueRunLogicTreeOnce(KX_GameObject *target_object, const std::string &tree_name);

  void NotifyGameObjectQueuedForRemoval(KX_GameObject *gameobj, KX_ObjectRemovalReason reason);
  void NotifyGameObjectsQueuedForBulkRemoval(const std::vector<KX_GameObject *> &gameobjects);
  /** Compatibility/system-cleanup entry point for callers without a gameplay removal source. */
  void UnregisterGameObject(KX_GameObject *gameobj);
  void UnregisterGameObject(KX_GameObject *gameobj, KX_ObjectRemovalReason reason);
  void InvalidateRuntimeRefs(LN_RuntimeRefKind kind, const void *pointer);
  void ClearPendingRuntimeWork();
  void BeginShutdown();
  void ReplicateRuntimeTrees(KX_GameObject *source_gameobj,
                             KX_GameObject *replica_gameobj,
                             uint32_t scene_object_index);
  bool CaptureRuntimeTreeRecipes(KX_GameObject *gameobj,
                                 std::vector<LN_RuntimeTreeRecipe> &r_recipes,
                                 std::string &r_error) const;
  LN_GameStateResult PrepareGameStateRuntimeTreeRestore(
      const std::vector<LN_RuntimeTreeRestoreTarget> &targets,
      const std::vector<KX_GameObject *> &removed_objects,
      LN_PreparedRuntimeTreeRestore &r_prepared,
      std::string &r_error);
  void CommitGameStateRuntimeTreeRestore(LN_PreparedRuntimeTreeRestore &prepared);
  void SetRuntimeTreesActiveForObject(KX_GameObject *gameobj, bool active);
  void AbsorbRuntimeTreesFrom(LN_Manager &other);
  /**
   * Sample the latest device state and accumulate transients independently for the next frame
   * update and the next fixed simulation step. This may run more often than rendered frames when
   * fixed simulation is scheduled independently from rendering.
   */
  void CaptureInput(bool frame_update_follows = false);
  /** The UI used this press (Esc closing a menu, Enter pressing a button): hide it from game
   * logic's input of this frame and of the next fixed step, so Keyboard Key and Gamepad Button
   * nodes do not react to it as well. The UI keeps reading the unfiltered frame input. */
  void ConsumeInputPress(int32_t input_code);
  void ConsumeGamepadPress(int32_t gamepad_index, int32_t button);
  /** The UI owns this button's whole press (a click or drag on UI): game logic sees no press,
   * hold or release of it until it is released. */
  void ConsumeInputStroke(int32_t input_code);
  /** A scroll or grid view used this frame's mouse wheel. */
  void ConsumeWheel();
  const LN_InputConsumption &GetInputConsumption() const
  {
    return m_inputConsumption;
  }
  /**
   * Rebind Input Action: the player's next key, mouse button or gamepad control (of the device
   * class) replaces one of the action's bindings, unless Esc cancels or `timeout` seconds pass
   * (none when not positive). `completion_expression_index`, a Persistence Completed expression
   * of the tree, reports Done and Success. A rebind already waiting for another node ends without
   * success; the same node asking again keeps waiting. Game logic sees no new press meanwhile.
   */
  void StartInputRebind(LN_RuntimeTree *runtime_tree,
                        uint32_t completion_expression_index,
                        uint32_t source_ref_index,
                        int32_t action,
                        int32_t user,
                        LN_InputDeviceClass device_class,
                        int direction,
                        double timeout);
  /** Commit completed Save Game loads at the scene pre-frame boundary. */
  void BeginFrame();
  /** Run the render-frame update phase, including while scaled simulation time is stopped. */
  void Update(double current_time,
              double scaled_delta_time,
              double unscaled_delta_time,
              bool use_fixed_timestep,
              bool game_paused);
  /** Run one deterministic fixed-simulation phase. */
  void FixedUpdate(double current_time, double fixed_delta_time, double unscaled_delta_time);
  bool IsObjectPauseProcessed(const KX_GameObject *gameobj) const;
  /** Resolve While Paused scopes immediately when pause begins between update phases. */
  void RefreshPauseProcessingObjects();
  void ClearPauseProcessingObjects();
  void Clear();

  bool HasRuntimeTrees() const
  {
    return !m_runtimeTrees.empty();
  }

  size_t GetCachedProgramCountForTests() const
  {
    return m_cachedPrograms.size();
  }

  /** False when the object has no such tree (nothing to start or stop). An object being removed,
   * or no object or name, is no failure. */
  bool FlushSetLogicTreeEnabled(KX_GameObject *target_object,
                                const std::string &tree_name,
                                bool enabled);
  void FlushInstallLogicTree(KX_GameObject *target_object,
                             const std::string &tree_name,
                             bool initial_enabled);
  bool FlushSaveGame(LN_RuntimeTree *runtime_tree,
                     int32_t slot,
                     const std::string &path,
                     uint32_t completion_expression_index,
                     uint32_t source_ref_index,
                     std::string &r_error);
  bool FlushLoadGame(LN_RuntimeTree *runtime_tree,
                     int32_t slot,
                     const std::string &path,
                     uint32_t completion_expression_index,
                     uint32_t source_ref_index,
                     std::string &r_error);
  LN_GameStateResult ApplyTransferredGameState(const LN_GameStateLoadPayload &payload,
                                               std::string &r_error);
  void CompleteTransferredGameStateFailure(const LN_GameStateCompletionTarget &target,
                                           const std::string &filepath,
                                           const std::string &error);
  void NotifyGameLoadFinished(bool succeeded);
  bool IsGameLoadPulseActive() const
  {
    return m_gameLoadPulseActive;
  }
  /** The Input Map actions of the tick being run (null before the first one samples them). */
  const LN_InputActionTracker *FrameInputActions() const
  {
    return m_sharedTickReadContext.raw_input_actions != nullptr ?
               m_sharedTickReadContext.raw_input_actions :
               m_sharedTickReadContext.input_actions;
  }
  /** On Player Join: a player joined (one per frame, LN_InputMap::NextJoin). */
  bool IsPlayerJoinActive() const
  {
    return m_playerJoinActive;
  }
  int32_t PlayerJoinUser() const
  {
    return m_playerJoinUser;
  }
  int32_t PlayerJoinGamepad() const
  {
    return m_playerJoinGamepad;
  }
  bool DidGameLoadSucceed() const
  {
    return m_gameLoadPulseActive && m_gameLoadPulseSucceeded;
  }
  bool InitializeGameStateRegistry(std::string &r_error);
  /** Track terminal state for one exact action request accepted during command flush. */
  void RegisterActionOperation(LN_RuntimeTree *runtime_tree,
                               uint32_t completion_expression_index,
                               uint8_t output_mask,
                               std::shared_ptr<const BL_ActionPlayback> playback);
  bool PreflightAddObjectPersistence(KX_GameObject *source,
                                     float life_seconds,
                                     bool full_copy,
                                     std::string &r_error) const;
  bool RegisterAddedObjectPersistence(KX_GameObject *source,
                                      KX_GameObject *root,
                                      const std::vector<KX_GameObject *> &members,
                                      float life_seconds,
                                      bool full_copy,
                                      std::string &r_error);
  bool SetTrackedSaveExistence(KX_GameObject *root, bool enabled, std::string &r_error);
  void NotifyGameStateObjectRemoved(KX_GameObject *object, KX_ObjectRemovalReason reason);

  void PushEvent(LN_EventEntry event);
  const LN_EventBus &GetEventBus() const;
  LN_EventSubjectId InternEventSubject(const std::string &subject);
  LN_GamePropertyId InternGamePropertyName(const std::string &name);
  LN_SoundId InternSoundName(const std::string &name);
  LN_ObjectHandle MakeObjectHandle(KX_GameObject *gameobj, const std::string &debug_name = "");
  /** Resolve a generation-checked scene-local object handle. */
  KX_GameObject *ResolveObjectHandle(const LN_ObjectHandle &handle) const;
  /** Read-only handle lookup, safe while worker trees execute. */
  LN_ObjectHandle FindObjectHandle(const KX_GameObject *gameobj) const;
  /** Tree-independent reference for values that other trees read (see LN_RuntimeRef). */
  LN_RuntimeRef MakeSharedObjectRef(KX_GameObject *gameobj, const std::string &debug_name);
  KX_GameObject *ResolveSharedObjectRef(const LN_RuntimeRef &runtime_ref) const;
  const LN_RigidBodyConstraintBreakEvent *FindRigidBodyConstraintBreakEvent(
      KX_GameObject *owner,
      const std::string &constraint_name,
      LN_RigidBodyConstraintMatchMode match_mode) const;
  const std::string &DebugName(LN_EventSubjectId id) const;
  const std::string &DebugName(LN_GamePropertyId id) const;
  const std::string &DebugName(LN_SoundId id) const;

  /** Per-tick object name lookup (valid only while `m_isTicking`). */
  KX_GameObject *FindObjectByName(const std::string &name);

  /** A value shown in text, `token` from between braces: "Player.gold" (an object's game
   * property) or "health" (of `font`, else of its nearest parent that has it), with ":N" for N
   * decimals ("Player.speed:1"). False when there is no such property. */
  bool TextPropertyValue(KX_GameObject &font, const std::string &token, std::string &r_text);

  /** Drop Item: put a copy of the library item `source` in the world at `position`, with its
   * `item` and `count` game properties; the scene owns the copy. Spawn does it now (a Logic +
   * Physics step or On Init); Queue keeps it for the start of the next Logic + Physics step (a UI
   * route), or the end of the frame while the game is paused. */
  KX_GameObject *SpawnItemDrop(KX_GameObject *source,
                               const MT_Vector3 &position,
                               const std::string &item,
                               int32_t count);
  void QueueItemDrop(KX_GameObject *source,
                     const MT_Vector3 &position,
                     const std::string &item,
                     int32_t count);
  /** Where the queued drops will appear (they are not in the world yet). */
  void QueuedItemDropPositions(std::vector<MT_Vector3> &r_positions) const;

  /** The Inventory Rules the list \a property of \a owner follows now: the scene's set its game
   * property picks, its "or from" values read from \a owner; the defaults when it picks none (or
   * a set the scene does not have, with one warning). Main thread only. The library stays null
   * when the set names none: callers use the scene's Item Library then. */
  LN_InventoryRules InventoryRulesOf(KX_GameObject *owner, const std::string &property);
  /** Unit tests: gives the scene a rule set without a Blender scene (no scene is read then). */
  void SetInventoryRuleSetForTests(const std::string &name, const LN_InventoryRuleSet &rules);

 private:
  /** The scene's Inventory Rules by name, read once on the first lookup that needs them. */
  void EnsureInventoryRuleSets();
  std::unordered_map<std::string, LN_InventoryRuleSet> m_inventoryRuleSets;
  bool m_inventoryRuleSetsLoaded = false;
  /** Warnings already given, so each shows once. */
  std::unordered_set<std::string> m_inventoryRuleWarnings;

  struct PendingItemDrop {
    LN_ObjectHandle source;
    MT_Vector3 position = MT_Vector3(0.0f, 0.0f, 0.0f);
    std::string item;
    int32_t count = 1;
  };
  void SpawnPendingItemDrops();
  struct RuntimeTreeLookupKey {
    KX_GameObject *gameobj = nullptr;
    uint32_t applied_tree_index = 0;

    bool operator==(const RuntimeTreeLookupKey &other) const
    {
      return gameobj == other.gameobj && applied_tree_index == other.applied_tree_index;
    }
  };

  struct RuntimeTreeLookupKeyHash {
    size_t operator()(const RuntimeTreeLookupKey &key) const
    {
      const size_t object_hash = std::hash<KX_GameObject *>{}(key.gameobj);
      const size_t index_hash = std::hash<uint32_t>{}(key.applied_tree_index);
      return object_hash ^ (index_hash + 0x9e3779b9u + (object_hash << 6u) + (object_hash >> 2u));
    }
  };

  struct ConstraintBreakLookupKey {
    KX_GameObject *owner = nullptr;
    size_t constraint_name_hash = 0;

    bool operator==(const ConstraintBreakLookupKey &other) const
    {
      return owner == other.owner && constraint_name_hash == other.constraint_name_hash;
    }
  };

  struct ConstraintBreakLookupKeyHash {
    size_t operator()(const ConstraintBreakLookupKey &key) const
    {
      size_t result = std::hash<KX_GameObject *>{}(key.owner);
      result ^= key.constraint_name_hash + 0x9e3779b9u + (result << 6u) + (result >> 2u);
      return result;
    }
  };

  struct PointerHitCacheKey {
    KX_Camera *camera = nullptr;
    KX_GameObject *ignored_object = nullptr;
    MT_Vector3 from = MT_Vector3(0.0f, 0.0f, 0.0f);
    MT_Vector3 to = MT_Vector3(0.0f, 0.0f, 0.0f);
    std::string property_name;
    uint64_t command_execution_key = UINT64_MAX;
    uint32_t collision_mask = 0;
    LN_InteractionDomain domain = LN_InteractionDomain::Any;
    bool xray = false;
    bool respect_runtime_visibility = false;

    bool operator==(const PointerHitCacheKey &other) const
    {
      return camera == other.camera && ignored_object == other.ignored_object &&
             from.x() == other.from.x() && from.y() == other.from.y() &&
             from.z() == other.from.z() && to.x() == other.to.x() && to.y() == other.to.y() &&
             to.z() == other.to.z() && property_name == other.property_name &&
             command_execution_key == other.command_execution_key &&
             collision_mask == other.collision_mask && domain == other.domain &&
             xray == other.xray &&
             respect_runtime_visibility == other.respect_runtime_visibility;
    }
  };

  struct PointerHitCacheKeyHash {
    size_t operator()(const PointerHitCacheKey &key) const
    {
      size_t result = std::hash<KX_Camera *>{}(key.camera);
      const auto combine = [&result](const size_t value) {
        result ^= value + 0x9e3779b97f4a7c15ULL + (result << 6u) + (result >> 2u);
      };
      combine(std::hash<KX_GameObject *>{}(key.ignored_object));
      combine(std::hash<float>{}(key.from.x()));
      combine(std::hash<float>{}(key.from.y()));
      combine(std::hash<float>{}(key.from.z()));
      combine(std::hash<float>{}(key.to.x()));
      combine(std::hash<float>{}(key.to.y()));
      combine(std::hash<float>{}(key.to.z()));
      combine(std::hash<std::string>{}(key.property_name));
      combine(std::hash<uint64_t>{}(key.command_execution_key));
      combine(std::hash<uint32_t>{}(key.collision_mask));
      combine(std::hash<uint8_t>{}(uint8_t(key.domain)));
      combine(std::hash<bool>{}(key.xray));
      combine(std::hash<bool>{}(key.respect_runtime_visibility));
      return result;
    }
  };

  struct PointerHitCacheValue {
    LN_PhysicsQueryResult result;
    KX_GameObject *hit_object = nullptr;
  };

  struct ActionOperation {
    LN_RuntimeTree *runtime_tree = nullptr;
    uint64_t program_revision = 0;
    uint32_t completion_expression_index = LN_INVALID_INDEX;
    uint8_t output_mask = LN_ACTION_OUTPUT_NONE;
    std::shared_ptr<const BL_ActionPlayback> playback;
  };

  /** Compact, allocation-free per-owner event chain. The global event vector
   * remains in Jolt's deterministic constraint-ID order; these links retain
   * that order for a single owner without per-owner heap allocations. */
  struct ConstraintBreakOwnerEventRange {
    uint32_t first_event_index = LN_INVALID_INDEX;
    uint32_t last_event_index = LN_INVALID_INDEX;
    uint32_t event_count = 0;
  };

  LN_RuntimeTree *RegisterDebugHardcodedSetWorldPosition(KX_GameObject *gameobj,
                                                         const MT_Vector3 &position);
  void RegisterDebugPropertyBindings();
  void CacheCompiledProgram(const std::shared_ptr<const LN_Program> &program);
  std::shared_ptr<const LN_Program> EnsureCompiledProgram(const std::string &tree_name);
  std::shared_ptr<const LN_Program> EnsureCompiledProgram(
      const std::string &tree_name, const std::string &library_path);
  std::shared_ptr<const LN_Program> EnsureCompiledProgramImpl(
      const std::string &tree_name, const std::string *library_path);
  LN_RuntimeTree *FindRuntimeTreeBySourceName(KX_GameObject *gameobj,
                                              const std::string &tree_name) const;
  uint32_t AllocateNextAppliedTreeIndex(KX_GameObject *gameobj) const;
  void Tick(double current_time,
            double delta_time,
            double unscaled_delta_time,
            bool use_fixed_timestep,
            bool game_paused,
            LN_Event event);
  void TickUIElementsWithoutTrees(double delta_time, double unscaled_delta_time);
  void RebuildPauseProcessingObjects();
  void ProcessPendingRunOnce(std::vector<LN_CommandBuffer::RecordedCommandList> &command_lists,
                             const LN_TickContext &context,
                             bool collect_profile_counters);
  void CaptureSharedTickReadContext(uint32_t input_channels,
                                    double fixed_dt,
                                    uint64_t tick_index,
                                    LN_Event event);
  /** This frame's unfiltered input for the UI, after CaptureSharedTickReadContext. */
  const LN_InputSnapshot *UIFrameInput() const;
  bool HasInputDependencies() const;
  uint32_t InputDependencyChannels() const;
  /** Samples the Input Map's actions for the trees of this tick. */
  void UpdateInputActions(LN_Event event, uint32_t input_channels, uint64_t phase_tick_index);
  /** Samples the frame's actions again once the UI took its presses. */
  void RefreshInputActions();
  /** Each player's input context, from their own topmost menu. */
  LN_InputContexts InputContexts() const;
  void ProcessInputRebind(double unscaled_delta_time);
  void FinishInputRebind(bool success);
  /** Hides every new press in `sample` from game logic. */
  void ConsumeNewPresses(const LN_InputSnapshot &sample);
  void CollectRigidBodyConstraintBreakEvents();
  void ActivatePendingRigidBodyConstraintBreakEvents();
  void QueueRigidBodyConstraintBreakEventsForUpdate();
  void IndexRigidBodyConstraintBreakEvents();
  void ClearRigidBodyConstraintBreakEvents();
  void ClearPendingRigidBodyConstraintBreakEvents();
  uint32_t FirstRigidBodyConstraintBreakEventIndex(KX_GameObject *owner) const;
  uint32_t NextRigidBodyConstraintBreakEventIndex(uint32_t event_index) const;
  uint32_t RigidBodyConstraintBreakEventCount(KX_GameObject *owner) const;
  const LN_RigidBodyConstraintBreakEvent *RigidBodyConstraintBreakEventAt(
      uint32_t event_index) const;
  void RebuildObjectNameCache();
  void PollGameStateCompletions();
  void PollActionOperations();
  void CompleteGameStateOperation(const LN_GameStateCompletionTarget &target,
                                  LN_GameStateOperation operation,
                                  const std::string &filepath,
                                  LN_GameStateResult result,
                                  const std::string &error,
                                  bool report_failure);

  friend class LN_RuntimeTree;
  friend class LN_CommandBuffer;
  friend class LN_UIService;
  friend class LN_PreparedRuntimeTreeRestore;
  friend class LN_ManagerTestAccess;

  LN_RuntimeTree *FindRuntimeTree(KX_GameObject *gameobj, uint32_t applied_tree_index) const;
  void IndexRuntimeTree(LN_RuntimeTree *runtime_tree);
  void UnindexRuntimeTree(LN_RuntimeTree *runtime_tree);
  void ClearRuntimeTreeIndexes();
  void RebuildRuntimeTreeIndexes();
  void IndexRuntimeTreeObjectRefs(LN_RuntimeTree *runtime_tree);
  void RegisterRuntimeObjectRef(LN_RuntimeTree *runtime_tree, KX_GameObject *referenced_object);
  void UnregisterRuntimeObjectRef(LN_RuntimeTree *runtime_tree, KX_GameObject *referenced_object);
  void UnregisterRuntimeTreeObjectRefs(LN_RuntimeTree *runtime_tree);
  void DetachRuntimeTreesForGameObject(KX_GameObject *gameobj);
  void RemoveDetachedRuntimeTrees();
  void AddQueuedRemovalObject(KX_GameObject *gameobj);
  void RemoveQueuedRemovalObject(KX_GameObject *gameobj);
  bool IsGameObjectQueuedForRemoval(KX_GameObject *gameobj) const;
  int FindSceneObjectIndex(KX_GameObject *gameobj, bool *r_runtime_active = nullptr) const;
  void AssertMainThread() const;
  void InvalidatePointerHitCache();

  KX_Scene &m_scene;
  std::unordered_map<std::string, std::shared_ptr<const LN_Program>> m_programBySourceTreeName;
  std::unordered_set<const LN_Program *> m_validatedPrograms;
  std::unordered_set<const LN_Program *> m_cachedPrograms;
  std::deque<std::pair<KX_GameObject *, std::string>> m_pendingRunOnce;
  std::vector<PendingItemDrop> m_pendingItemDrops;
  std::vector<std::unique_ptr<LN_RuntimeTree>> m_runtimeTrees;
  std::unordered_map<RuntimeTreeLookupKey, LN_RuntimeTree *, RuntimeTreeLookupKeyHash>
      m_runtimeTreeByObjectAndAppliedIndex;
  std::unordered_map<KX_GameObject *, std::vector<LN_RuntimeTree *>> m_runtimeTreesByObject;
  std::unordered_map<KX_GameObject *, std::vector<LN_RuntimeTree *>>
      m_runtimeTreesByReferencedObject;
  std::unordered_map<LN_RuntimeTree *, std::vector<KX_GameObject *>>
      m_referencedObjectsByRuntimeTree;
  std::mutex m_runtimeObjectRefIndexMutex;
  std::vector<KX_GameObject *> m_queuedRemovalObjects;
  LN_CommandBuffer m_commandBuffer;
  std::unique_ptr<LN_UIService> m_uiService;
  LN_GameStateService m_gameStateService;
  std::vector<ActionOperation> m_actionOperations;
  std::thread::id m_mainThreadId;
  uint64_t m_tickIndex = 0;
  uint64_t m_updateTickIndex = 0;
  uint64_t m_fixedTickIndex = 0;
  /** LN_TickContext::mouse_ui_serial, and whether the last frame paused or held Mouse Look. */
  uint64_t m_mouseUISerial = 0;
  bool m_mouseLookHeld = false;
  bool m_isTicking = false;
  bool m_isShuttingDown = false;
  bool m_hasDetachedRuntimeTrees = false;
  bool m_debugBindingsScanned = false;
  bool m_reportedVariableTimestepSkip = false;
  bool m_gameStateTransitionPending = false;
  bool m_gameLoadPulsePending = false;
  bool m_pendingGameLoadSucceeded = false;
  bool m_gameLoadPulseActive = false;
  bool m_gameLoadPulseSucceeded = false;
  /** On Player Join: the last join this scene saw, and this frame's. */
  uint64_t m_lastPlayerJoinSerial = 0;
  /** The players already joined when the scene started (user, gamepad), told first. */
  std::vector<std::pair<int32_t, int32_t>> m_joinsAtStart;
  bool m_playerJoinActive = false;
  int32_t m_playerJoinUser = -1;
  int32_t m_playerJoinGamepad = -1;
  LN_EventBus m_eventBus;
  std::vector<LN_RigidBodyConstraintBreakEvent> m_rigidBodyConstraintBreakEvents;
  std::vector<LN_RigidBodyConstraintBreakEvent> m_pendingRigidBodyConstraintBreakEvents;
  std::unordered_multimap<ConstraintBreakLookupKey,
                          uint32_t,
                          ConstraintBreakLookupKeyHash>
      m_rigidBodyConstraintBreakEventLookup;
  std::unordered_map<KX_GameObject *, ConstraintBreakOwnerEventRange>
      m_rigidBodyConstraintBreakEventOwnerLookup;
  std::vector<uint32_t> m_rigidBodyConstraintBreakEventNextByOwner;
  /** Raw device sample; owns mouse/gamepad history used to derive edges and relative motion. */
  LN_InputSnapshot m_inputSample;
  /** Input accumulated until the next render-frame update. */
  LN_InputSnapshot m_pendingUpdateInput;
  /** Current render-frame input, unfiltered: what the UI reads. */
  LN_InputSnapshot m_frameInput;
  /** m_frameInput without what the UI consumed: what frame logic reads. */
  LN_InputSnapshot m_gameFrameInput;
  LN_InputConsumption m_inputConsumption;
  /** Latest state plus transients accumulated until the next fixed simulation step. */
  LN_InputSnapshot m_pendingFixedInput;
  /** m_pendingFixedInput with what the UI consumed, kept while a tree reads it. */
  LN_InputSnapshot m_pendingFixedRawInput;
  bool m_hasPendingFixedRawInput = false;
  /** Input Map actions: frame and fixed step, each without and with what the UI used. */
  std::array<LN_InputActionTracker, 4> m_inputActions;
  struct InputRebind {
    LN_GameStateCompletionTarget target;
    LN_InputCapture capture;
    int32_t action = -1;
    int32_t user = 0;
    LN_InputDeviceClass device_class = LN_InputDeviceClass::KeyboardMouse;
    uint8_t direction = 0;
    double remaining = 0.0;
    bool active = false;
    /** The input of the frame that started it belongs to what started it. */
    bool skip_frame = true;
  };
  InputRebind m_inputRebind;
  LN_TickReadContext m_sharedTickReadContext;
  bool m_hasSharedTickInput = false;
  bool m_hasPendingUpdateInput = false;
  bool m_hasFrameInput = false;
  bool m_hasPendingFixedInput = false;
  std::vector<LN_ParallelTreeExecutor::RuntimeTreeWorkItem> m_tickRunnableTreesScratch;
  std::unordered_set<KX_GameObject *> m_pauseProcessingObjects;
  std::vector<KX_GameObject *> m_pauseProcessingObjectsScratch;
  std::unordered_map<std::string, KX_GameObject *> m_objectByName;
  bool m_objectNameCacheValid = false;
  /** Objects named in text values ("{Player.gold}"), found again when the object list changes;
   * an invalid handle is a name with no object. */
  std::unordered_map<std::string, LN_ObjectHandle> m_textObjects;
  uint64_t m_textObjectsGeneration = UINT64_MAX;
  /** Set once any tree looks up an object by name; the main thread then rebuilds the name
   * cache before worker trees run, since they may only read it. */
  std::atomic<bool> m_objectNameLookupUsed = false;
  std::unordered_map<PointerHitCacheKey, PointerHitCacheValue, PointerHitCacheKeyHash>
      m_pointerHitCache;
  LN_DenseIdRegistry m_denseIds;
  /** Process-unique id, so shared references from another scene are never misresolved. */
  uint32_t m_sharedRefDomain = 0;
  bool m_profileTickMetricsEnabled = false;
  mutable std::atomic<uint64_t> m_tickHotMapLookupCount = 0;
};
