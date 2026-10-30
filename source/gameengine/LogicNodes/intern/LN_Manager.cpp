/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_Manager.cpp
 *  \ingroup logicnodes
 */

#include "LN_Manager.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <optional>
#include <unordered_set>
#include <utility>

#include "BLI_assert.h"
#include "BKE_context.hh"
#include "BKE_main.hh"
#include "CM_Message.h"
#include "DNA_node_types.h"
#include "DNA_scene_types.h"
#include "LN_BindingSource.h"
#include "LN_Performance.h"
#include "LN_Program.h"
#include "LN_RuntimeTree.h"
#include "LN_ParallelTreeExecutor.h"
#include "LN_TreeCompiler.h"
#include "LN_Types.h"
#include "LN_UIService.h"
#include "BL_ActionManager.h"
#include "KX_FontObject.h"
#include "KX_GameObject.h"
#include "KX_Globals.h"
#include "DEV_InputDevice.h"
#include "KX_KetsjiEngine.h"
#include "KX_Scene.h"
#include "PHY_IPhysicsController.h"
#include "PHY_IPhysicsEnvironment.h"
#include "SCA_IInputDevice.h"
#include "SCA_InputEvent.h"

/** Input that UI Elements route: pointer, wheel, keyboard and gamepad navigation. */
static constexpr uint32_t kUIElementInputChannels = LN_DEP_INPUT_MOUSE | LN_DEP_INPUT_WHEEL |
                                                    LN_DEP_INPUT_KEYBOARD |
                                                    LN_DEP_INPUT_GAMEPAD_BUTTON;
static constexpr const char *LN_DEBUG_SET_WORLD_POSITION = "__ln_debug_set_world_position";
static constexpr const char *LN_DEBUG_SET_WORLD_POSITION_X = "__ln_debug_set_world_position_x";
static constexpr const char *LN_DEBUG_SET_WORLD_POSITION_Y = "__ln_debug_set_world_position_y";
static constexpr const char *LN_DEBUG_SET_WORLD_POSITION_Z = "__ln_debug_set_world_position_z";

static bool logic_nodes_report_tick_timings(KX_Scene &scene)
{
  const char *value = std::getenv("UPBGE_LOGIC_NODES_PROFILE");
  if (value != nullptr && value[0] != '\0') {
    return value[0] != '0';
  }

  const blender::Scene *blender_scene = scene.GetBlenderScene();
  return blender_scene != nullptr &&
         (blender_scene->gm.flag & GAME_SHOW_LOGIC_NODES_PROFILE) != 0;
}

static bool logic_nodes_enable_debug_property_bindings()
{
  const char *value = std::getenv("UPBGE_LOGIC_NODES_DEBUG_BINDINGS");
  return value != nullptr && value[0] != '\0' && value[0] != '0';
}

template<typename T>
static void logic_nodes_profiled_reserve(std::vector<T> &values,
                                         const size_t size,
                                         LN_TickProfileMetrics &profile)
{
  if (size > values.capacity()) {
    profile.allocation_count++;
  }
  values.reserve(size);
}

static uint32_t logic_nodes_count_command_string_payloads(
    const std::vector<LN_CommandBuffer::Command> &commands)
{
  uint32_t count = 0;
  for (const LN_CommandBuffer::Command &command : commands) {
    count += command.property_name.empty() ? 0 : 1;
    count += command.secondary_property_name.empty() ? 0 : 1;
    count += command.tertiary_property_name.empty() ? 0 : 1;
    count += command.quaternary_property_name.empty() ? 0 : 1;
    count += command.property_value.string_value.empty() ? 0 : 1;
  }
  return count;
}

static uint32_t logic_nodes_count_commands(
    const std::vector<LN_CommandBuffer::RecordedCommandList> &command_lists)
{
  uint32_t count = 0;
  for (const LN_CommandBuffer::RecordedCommandList &command_list : command_lists) {
    count += uint32_t(command_list.commands.size());
  }
  return count;
}

static void logic_nodes_accumulate_runtime_profile_counts(
    const LN_RuntimeProfileCounters &counters,
    LN_TickProfileMetrics &profile)
{
  profile.instruction_count += counters.instruction_dispatch_count;
  profile.exec_block_count += counters.exec_block_count;
  profile.exec_direct_instruction_count +=
      counters.exec_direct_instruction_count;
  profile.exec_fallback_instruction_count +=
      counters.exec_fallback_instruction_count;
  profile.exec_fallback_block_count += counters.exec_fallback_block_count;
  profile.exec_fallback_ns += counters.exec_fallback_ns;
  profile.expression_count += counters.expression_evaluation_count;
  profile.register_expression_hit_count +=
      counters.register_expression_hit_count;
  profile.register_expression_fallback_count +=
      counters.register_expression_fallback_count;
  profile.register_scalar_op_count += counters.register_scalar_op_count;
  profile.register_simd_candidate_batch_count +=
      counters.register_simd_candidate_batch_count;
  profile.register_simd_candidate_lane_count +=
      counters.register_simd_candidate_lane_count;
  profile.register_simd_batch_count += counters.register_simd_batch_count;
  profile.register_simd_lane_count += counters.register_simd_lane_count;
  profile.fallback_path_count += counters.fallback_path_count;
  profile.runtime_fallback_reason_mask |= counters.runtime_fallback_reason_mask;
  profile.runtime_expression_fallback_reason_mask |=
      counters.runtime_expression_fallback_reason_mask;
  profile.runtime_system_fallback_reason_mask |=
      counters.runtime_system_fallback_reason_mask;
  profile.runtime_system_fallback_count += counters.runtime_system_fallback_count;
}

static void logic_nodes_accumulate_recorded_profile_counts(
    const LN_CommandBuffer::RecordedCommandList &command_list,
    LN_TickProfileMetrics &profile)
{
  logic_nodes_accumulate_runtime_profile_counts(command_list.profile_counters, profile);
}

static void logic_nodes_accumulate_command_stream_stats(
    const LN_CommandBuffer::CommandStreamStats &stats,
    LN_TickProfileMetrics &profile)
{
  profile.command_legacy_count += stats.legacy_command_count;
  profile.command_typed_transform_count += stats.typed_transform_count;
  profile.command_typed_velocity_count += stats.typed_velocity_count;
  profile.command_typed_relative_vector_count += stats.typed_relative_vector_count;
  profile.command_typed_motion_count += stats.typed_motion_count;
  profile.command_typed_property_count += stats.typed_property_count;
  profile.command_typed_event_count += stats.typed_event_count;
  profile.command_typed_audio_count += stats.typed_audio_count;
  profile.command_typed_lifecycle_count += stats.typed_lifecycle_count;
  profile.command_typed_object_service_count += stats.typed_object_service_count;
  profile.command_typed_runtime_service_count += stats.typed_runtime_service_count;
  profile.command_typed_animation_count += stats.typed_animation_count;
  profile.command_typed_armature_count += stats.typed_armature_count;
  profile.command_typed_material_count += stats.typed_material_count;
  profile.command_typed_physics_count += stats.typed_physics_count;
  profile.command_coalesced_count += stats.coalesced_command_count;
}

static void logic_nodes_accumulate_command_stream_stats(
    const LN_CommandBuffer::RecordedCommandList &command_list,
    LN_TickProfileMetrics &profile)
{
  logic_nodes_accumulate_command_stream_stats(command_list.command_stream_stats, profile);
}

static uint32_t logic_nodes_count_command_string_payloads(
    const std::vector<LN_CommandBuffer::RecordedCommandList> &command_lists)
{
  uint32_t count = 0;
  for (const LN_CommandBuffer::RecordedCommandList &command_list : command_lists) {
    count += logic_nodes_count_command_string_payloads(command_list.commands);
  }
  return count;
}

static LN_CommandBuffer::RecordedCommandList logic_nodes_record_tree_commands(
    const size_t runtime_tree_index,
    LN_RuntimeTree &runtime_tree,
    const LN_TickContext &context,
    const bool forced_update,
    const std::thread::id main_thread_id,
    const bool collect_profile_counters)
{
  LN_CommandBuffer local_buffer;
  local_buffer.SetMainThreadId(main_thread_id);
  local_buffer.SetTypedCommandStreamsEnabled(context.use_typed_command_streams);
  local_buffer.BeginRecording();
  LN_RuntimeProfileCounters profile_counters;
  if (forced_update) {
    runtime_tree.ExecuteForcedUpdate(local_buffer,
                                     context,
                                     collect_profile_counters ? &profile_counters : nullptr);
  }
  else {
    runtime_tree.ExecuteReady(local_buffer,
                              context,
                              collect_profile_counters ? &profile_counters : nullptr);
  }
  local_buffer.EndRecording();

  LN_CommandBuffer::RecordedCommandList command_list;
  command_list.runtime_tree_index = runtime_tree_index;
  command_list.commands = local_buffer.TakeRecordedCommands();
  command_list.command_stream_stats = local_buffer.GetLastCommandStreamStats();
  const LN_RuntimeTree::RuntimeFallbackReport &fallback_report =
      runtime_tree.GetRuntimeFallbackReport();
  profile_counters.runtime_fallback_reason_mask = fallback_report.reason_mask;
  profile_counters.runtime_expression_fallback_reason_mask =
      fallback_report.expression_reason_mask;
  profile_counters.runtime_system_fallback_reason_mask = fallback_report.system_reason_mask;
  profile_counters.runtime_system_fallback_count = fallback_report.system_fallback_count;
  command_list.profile_counters = profile_counters;
  return command_list;
}

LN_Manager::LN_Manager(KX_Scene &scene)
    : m_scene(scene),
      m_uiService(std::make_unique<LN_UIService>(*this, scene)),
      m_mainThreadId(std::this_thread::get_id())
{
  static std::atomic<uint32_t> next_shared_ref_domain{1};
  m_sharedRefDomain = next_shared_ref_domain.fetch_add(1, std::memory_order_relaxed);
  /* A scene that starts later (Restart Scene, the next level) hears On Player Join for the
   * players already in, then the joins after it. */
  const LN_InputMap &input_map = LN_InputMap::Get();
  m_lastPlayerJoinSerial = input_map.JoinSerial();
  for (int32_t user = 0; user < LN_MAX_GAMEPADS; ++user) {
    if (input_map.IsUserJoined(user)) {
      m_joinsAtStart.emplace_back(user, input_map.GamepadOfUser(user));
    }
  }
  m_commandBuffer.SetMainThreadId(m_mainThreadId);
  m_commandBuffer.SetLogicManager(this);
}

LN_Manager::~LN_Manager()
{
  BeginShutdown();
}

LN_UIService &LN_Manager::GetUIService()
{
  return *m_uiService;
}

const LN_UIService &LN_Manager::GetUIService() const
{
  return *m_uiService;
}

LN_RuntimeTree *LN_Manager::RegisterDebugHardcodedSetWorldPosition(KX_GameObject *gameobj,
                                                                   const MT_Vector3 &position)
{
  AssertMainThread();

  std::shared_ptr<const LN_Program> program = LN_Program::CreateDebugSetWorldPosition(position);
  const uint32_t applied_tree_index = static_cast<uint32_t>(m_runtimeTrees.size());
  const uint32_t scene_object_index = applied_tree_index;
  return RegisterCompiledProgram(gameobj, program, scene_object_index, applied_tree_index, true);
}

LN_RuntimeTree *LN_Manager::RegisterCompiledProgram(KX_GameObject *gameobj,
                                                   std::shared_ptr<const LN_Program> program,
                                                   uint32_t scene_object_index,
                                                   uint32_t applied_tree_index,
                                                   bool enabled,
                                                   bool runtime_active)
{
  AssertMainThread();

  if (m_isShuttingDown || gameobj == nullptr || program == nullptr ||
      IsGameObjectQueuedForRemoval(gameobj))
  {
    return nullptr;
  }

  if (!program->IsCurrentRuntimeCompatible()) {
    CM_Warning("Logic Nodes: rejected stale compiled program '" << program->GetName() << "'");
    return nullptr;
  }
  if (m_validatedPrograms.find(program.get()) == m_validatedPrograms.end()) {
    std::vector<std::string> payload_errors;
    if (!program->ValidateInstructionPayloads(&payload_errors)) {
      CM_Warning("Logic Nodes: rejected invalid compiled program '"
                 << program->GetName() << "': " << payload_errors.front()
                 << " (" << program->DescribeSchedulerSummary()
                 << ", on_init=" << program->GetInstructionHeaders(LN_Event::OnInit).size()
                 << ", on_update="
                 << program->GetInstructionHeaders(LN_Event::OnUpdate).size()
                 << ", on_fixed_update="
                 << program->GetInstructionHeaders(LN_Event::OnFixedUpdate).size() << ")");
      return nullptr;
    }
    m_validatedPrograms.insert(program.get());
  }

  if (LN_RuntimeTree *runtime_tree = FindRuntimeTree(gameobj, applied_tree_index)) {
    std::erase_if(m_actionOperations,
                  [runtime_tree](const ActionOperation &operation) {
                    return operation.runtime_tree == runtime_tree;
                  });
    runtime_tree->SetProgram(std::move(program));
    runtime_tree->SetLogicManager(this);
    runtime_tree->SetSceneObjectIndex(scene_object_index);
    runtime_tree->SetEnabled(enabled);
    runtime_tree->SetRuntimeActive(runtime_active);
    runtime_tree->BindDenseIds(m_denseIds);
    IndexRuntimeTree(runtime_tree);
    CacheCompiledProgram(runtime_tree->GetProgram());
    m_commandBuffer.PrewarmMaterialParameterBindings(*runtime_tree);
    return runtime_tree;
  }

  m_runtimeTrees.push_back(std::make_unique<LN_RuntimeTree>(
      std::move(program), gameobj, scene_object_index, applied_tree_index));
  LN_RuntimeTree *runtime_tree = m_runtimeTrees.back().get();
  runtime_tree->SetLogicManager(this);
  runtime_tree->SetEnabled(enabled);
  runtime_tree->SetRuntimeActive(runtime_active);
  runtime_tree->BindDenseIds(m_denseIds);
  IndexRuntimeTree(runtime_tree);
  CacheCompiledProgram(runtime_tree->GetProgram());
  m_commandBuffer.PrewarmMaterialParameterBindings(*runtime_tree);
  return runtime_tree;
}

void LN_Manager::NotifyGameObjectQueuedForRemoval(KX_GameObject *gameobj,
                                                  const KX_ObjectRemovalReason reason)
{
  AssertMainThread();

  if (gameobj == nullptr) {
    return;
  }

  m_uiService->NotifyObjectRemoved(gameobj);

  NotifyGameStateObjectRemoved(gameobj, reason);
  InvalidatePointerHitCache();
  m_pauseProcessingObjects.erase(gameobj);
  AddQueuedRemovalObject(gameobj);
  std::erase_if(m_pendingRunOnce, [gameobj](const std::pair<KX_GameObject *, std::string> &entry) {
    return entry.first == gameobj;
  });
  InvalidateRuntimeRefs(LN_RuntimeRefKind::Object, gameobj);
  m_commandBuffer.RemoveCommandsForGameObject(gameobj);
  DetachRuntimeTreesForGameObject(gameobj);

  if (!m_isTicking) {
    RemoveDetachedRuntimeTrees();
  }
}

void LN_Manager::NotifyGameObjectsQueuedForBulkRemoval(
    const std::vector<KX_GameObject *> &gameobjects)
{
  AssertMainThread();

  if (gameobjects.empty()) {
    return;
  }

  std::unordered_set<KX_GameObject *> removal_set;
  removal_set.reserve(gameobjects.size());
  for (KX_GameObject *gameobj : gameobjects) {
    if (gameobj == nullptr) {
      continue;
    }
    removal_set.insert(gameobj);
  }

  if (removal_set.empty()) {
    return;
  }

  InvalidatePointerHitCache();

  std::unordered_set<KX_GameObject *> queued_set(m_queuedRemovalObjects.begin(),
                                                m_queuedRemovalObjects.end());
  queued_set.reserve(m_queuedRemovalObjects.size() + removal_set.size());
  for (KX_GameObject *gameobj : removal_set) {
    if (queued_set.insert(gameobj).second) {
      m_queuedRemovalObjects.push_back(gameobj);
    }
  }

  std::erase_if(m_pendingRunOnce,
                [&removal_set](const std::pair<KX_GameObject *, std::string> &entry) {
                  return removal_set.find(entry.first) != removal_set.end();
                });
  m_commandBuffer.Clear();

  for (KX_GameObject *gameobj : removal_set) {
    m_uiService->NotifyObjectRemoved(gameobj);
    m_pauseProcessingObjects.erase(gameobj);
    m_denseIds.InvalidateHandle(LN_DenseHandleKind::Object, gameobj);
  }

  std::vector<LN_RuntimeTree *> affected_referencing_trees;
  {
    std::lock_guard<std::mutex> lock(m_runtimeObjectRefIndexMutex);
    for (KX_GameObject *gameobj : removal_set) {
      if (const auto item = m_runtimeTreesByReferencedObject.find(gameobj);
          item != m_runtimeTreesByReferencedObject.end())
      {
        affected_referencing_trees.insert(affected_referencing_trees.end(),
                                          item->second.begin(),
                                          item->second.end());
      }
    }
  }

  std::sort(affected_referencing_trees.begin(), affected_referencing_trees.end());
  affected_referencing_trees.erase(std::unique(affected_referencing_trees.begin(),
                                               affected_referencing_trees.end()),
                                   affected_referencing_trees.end());
  for (LN_RuntimeTree *runtime_tree : affected_referencing_trees) {
    if (runtime_tree != nullptr) {
      for (KX_GameObject *gameobj : removal_set) {
        runtime_tree->InvalidateObjectRef(gameobj);
      }
    }
  }

  std::vector<LN_RuntimeTree *> owned_trees;
  for (KX_GameObject *gameobj : removal_set) {
    if (const auto item = m_runtimeTreesByObject.find(gameobj);
        item != m_runtimeTreesByObject.end())
    {
      owned_trees.insert(owned_trees.end(), item->second.begin(), item->second.end());
    }
  }
  if (owned_trees.empty()) {
    for (const std::unique_ptr<LN_RuntimeTree> &runtime_tree : m_runtimeTrees) {
      if (runtime_tree != nullptr && runtime_tree->IsAttached() &&
          removal_set.find(runtime_tree->GetGameObject()) != removal_set.end())
      {
        owned_trees.push_back(runtime_tree.get());
      }
    }
  }

  std::sort(owned_trees.begin(), owned_trees.end());
  owned_trees.erase(std::unique(owned_trees.begin(), owned_trees.end()), owned_trees.end());
  for (LN_RuntimeTree *runtime_tree : owned_trees) {
    if (runtime_tree != nullptr && runtime_tree->IsAttached() &&
        removal_set.find(runtime_tree->GetGameObject()) != removal_set.end())
    {
      UnindexRuntimeTree(runtime_tree);
      runtime_tree->DetachGameObject();
      m_hasDetachedRuntimeTrees = true;
    }
  }

  if (!m_isTicking) {
    RemoveDetachedRuntimeTrees();
  }
}

void LN_Manager::UnregisterGameObject(KX_GameObject *gameobj)
{
  UnregisterGameObject(gameobj, KX_ObjectRemovalReason::SystemCleanup);
}

void LN_Manager::UnregisterGameObject(KX_GameObject *gameobj,
                                      const KX_ObjectRemovalReason reason)
{
  AssertMainThread();

  if (gameobj == nullptr) {
    return;
  }

  LN_CommandBuffer::ForgetSpatialSoundSpeaker(gameobj);
  NotifyGameStateObjectRemoved(gameobj, reason);
  m_pauseProcessingObjects.erase(gameobj);
  if (IsGameObjectQueuedForRemoval(gameobj)) {
    RemoveQueuedRemovalObject(gameobj);
    if (!m_isTicking) {
      RemoveDetachedRuntimeTrees();
    }
    return;
  }

  /* Objects destroyed without being queued (children of a removed parent, failed spawns, load
   * reconciliation) need the same cleanup as NotifyGameObjectQueuedForRemoval. The UI must be
   * told before the object handle is invalidated. */
  m_uiService->NotifyObjectRemoved(gameobj);
  InvalidatePointerHitCache();
  std::erase_if(m_pendingRunOnce, [gameobj](const std::pair<KX_GameObject *, std::string> &entry) {
    return entry.first == gameobj;
  });
  InvalidateRuntimeRefs(LN_RuntimeRefKind::Object, gameobj);
  m_commandBuffer.RemoveCommandsForGameObject(gameobj);
  DetachRuntimeTreesForGameObject(gameobj);
  RemoveQueuedRemovalObject(gameobj);

  if (!m_isTicking) {
    RemoveDetachedRuntimeTrees();
  }
}

void LN_Manager::InvalidateRuntimeRefs(const LN_RuntimeRefKind kind, const void *pointer)
{
  AssertMainThread();

  if (pointer == nullptr || kind == LN_RuntimeRefKind::None) {
    return;
  }

  if (kind == LN_RuntimeRefKind::Object) {
    KX_GameObject *gameobj = static_cast<KX_GameObject *>(const_cast<void *>(pointer));
    m_denseIds.InvalidateHandle(LN_DenseHandleKind::Object, gameobj);

    std::vector<LN_RuntimeTree *> trees;
    {
      std::lock_guard<std::mutex> lock(m_runtimeObjectRefIndexMutex);
      const auto item = m_runtimeTreesByReferencedObject.find(gameobj);
      if (item != m_runtimeTreesByReferencedObject.end()) {
        trees = item->second;
      }
    }

    for (LN_RuntimeTree *runtime_tree : trees) {
      if (runtime_tree != nullptr) {
        runtime_tree->InvalidateObjectRef(gameobj);
      }
    }
    return;
  }

  if (kind == LN_RuntimeRefKind::Scene) {
    m_denseIds.InvalidateHandle(LN_DenseHandleKind::Scene, pointer);
  }
  else if (kind == LN_RuntimeRefKind::Datablock ||
           kind == LN_RuntimeRefKind::Collection)
  {
    m_denseIds.InvalidateHandle(LN_DenseHandleKind::Datablock, pointer);
  }

  for (std::unique_ptr<LN_RuntimeTree> &runtime_tree : m_runtimeTrees) {
    runtime_tree->InvalidateRuntimeRef(kind, pointer);
  }
}

void LN_Manager::ClearPendingRuntimeWork()
{
  AssertMainThread();

  m_commandBuffer.Clear();
  InvalidatePointerHitCache();
  m_gameStateService.CancelPending(
      [this](const LN_GameStateCompletionTarget &target,
             const LN_GameStateOperation operation,
             const std::string &filepath,
             const LN_GameStateResult result,
             const std::string &error) {
        CompleteGameStateOperation(target, operation, filepath, result, error, false);
      });
  m_queuedRemovalObjects.clear();
  RemoveDetachedRuntimeTrees();
}

void LN_Manager::BeginShutdown()
{
  AssertMainThread();

  if (m_isShuttingDown) {
    return;
  }

  m_isShuttingDown = true;
  LN_CommandBuffer::ForgetSpatialSoundScene(&m_scene);
  Clear();
}

void LN_Manager::ReplicateRuntimeTrees(KX_GameObject *source_gameobj,
                                       KX_GameObject *replica_gameobj,
                                       uint32_t scene_object_index)
{
  AssertMainThread();

  if (source_gameobj == nullptr || replica_gameobj == nullptr ||
      IsGameObjectQueuedForRemoval(source_gameobj) ||
      IsGameObjectQueuedForRemoval(replica_gameobj))
  {
    return;
  }

  std::array<LN_RuntimeTree *, 4> inline_source_trees{};
  size_t inline_source_tree_count = 0;
  std::vector<LN_RuntimeTree *> overflow_source_trees;
  auto append_source_tree = [&](LN_RuntimeTree *runtime_tree) {
    if (runtime_tree == nullptr || !runtime_tree->IsAttached() ||
        !runtime_tree->OwnsGameObject(source_gameobj))
    {
      return;
    }
    if (inline_source_tree_count < inline_source_trees.size()) {
      inline_source_trees[inline_source_tree_count++] = runtime_tree;
      return;
    }
    overflow_source_trees.push_back(runtime_tree);
  };

  if (const auto trees = m_runtimeTreesByObject.find(source_gameobj);
      trees != m_runtimeTreesByObject.end())
  {
    for (LN_RuntimeTree *runtime_tree : trees->second) {
      append_source_tree(runtime_tree);
    }
  }
  else {
    for (std::unique_ptr<LN_RuntimeTree> &runtime_tree : m_runtimeTrees) {
      append_source_tree(runtime_tree.get());
    }
  }

  const size_t source_tree_count = inline_source_tree_count + overflow_source_trees.size();
  if (source_tree_count == 0) {
    return;
  }
  m_runtimeTrees.reserve(m_runtimeTrees.size() + source_tree_count);
  m_runtimeTreeByObjectAndAppliedIndex.reserve(m_runtimeTreeByObjectAndAppliedIndex.size() +
                                               source_tree_count);
  m_runtimeTreesByObject.reserve(m_runtimeTreesByObject.size() + 1);

  auto replicate_source_tree = [&](LN_RuntimeTree *source_tree) {
    RegisterCompiledProgram(replica_gameobj,
                            source_tree->GetProgram(),
                            scene_object_index,
                            source_tree->GetAppliedTreeIndex(),
                            source_tree->IsEnabled(),
                            true);
  };
  for (size_t index = 0; index < inline_source_tree_count; index++) {
    replicate_source_tree(inline_source_trees[index]);
  }
  for (LN_RuntimeTree *source_tree : overflow_source_trees) {
    replicate_source_tree(source_tree);
  }
}

bool LN_Manager::CaptureRuntimeTreeRecipes(KX_GameObject *gameobj,
                                           std::vector<LN_RuntimeTreeRecipe> &r_recipes,
                                           std::string &r_error) const
{
  AssertMainThread();
  r_recipes.clear();
  if (gameobj == nullptr) {
    r_error = "cannot capture C++ Logic Node bindings for a null object";
    return false;
  }

  try {
    const auto found = m_runtimeTreesByObject.find(gameobj);
    if (found == m_runtimeTreesByObject.end()) {
      return true;
    }
    r_recipes.reserve(found->second.size());
    for (LN_RuntimeTree *runtime_tree : found->second) {
      if (runtime_tree == nullptr || !runtime_tree->IsAttached() ||
          !runtime_tree->OwnsGameObject(gameobj) || runtime_tree->GetProgram() == nullptr)
      {
        r_error = "runtime C++ Logic Node binding is incomplete";
        r_recipes.clear();
        return false;
      }
      r_recipes.push_back({runtime_tree->GetProgram(),
                           runtime_tree->GetAppliedTreeIndex(),
                           runtime_tree->IsEnabled()});
    }
    std::sort(r_recipes.begin(),
              r_recipes.end(),
              [](const LN_RuntimeTreeRecipe &a, const LN_RuntimeTreeRecipe &b) {
                return a.applied_tree_index < b.applied_tree_index;
              });
  }
  catch (const std::exception &exception) {
    r_recipes.clear();
    r_error = std::string("could not capture C++ Logic Node bindings: ") + exception.what();
    return false;
  }
  catch (...) {
    r_recipes.clear();
    r_error = "could not capture C++ Logic Node bindings";
    return false;
  }
  return true;
}

void LN_Manager::SetRuntimeTreesActiveForObject(KX_GameObject *gameobj, const bool active)
{
  AssertMainThread();
  const auto found = m_runtimeTreesByObject.find(gameobj);
  if (found == m_runtimeTreesByObject.end()) {
    return;
  }
  for (LN_RuntimeTree *runtime_tree : found->second) {
    if (runtime_tree != nullptr && runtime_tree->IsAttached() &&
        runtime_tree->OwnsGameObject(gameobj))
    {
      runtime_tree->SetRuntimeActive(active);
    }
  }
}

void LN_Manager::AbsorbRuntimeTreesFrom(LN_Manager &other)
{
  AssertMainThread();
  other.AssertMainThread();

  /* Events belong to the pre-merge scene timeline and cannot safely cross
   * rebuilt dense-object identities. */
  ClearRigidBodyConstraintBreakEvents();
  ClearPendingRigidBodyConstraintBreakEvents();
  other.ClearRigidBodyConstraintBreakEvents();
  other.ClearPendingRigidBodyConstraintBreakEvents();
  other.ClearPendingRuntimeWork();
  m_actionOperations.insert(m_actionOperations.end(),
                            std::make_move_iterator(other.m_actionOperations.begin()),
                            std::make_move_iterator(other.m_actionOperations.end()));
  other.m_actionOperations.clear();
  for (std::unique_ptr<LN_RuntimeTree> &runtime_tree : other.m_runtimeTrees) {
    bool runtime_active = false;
    const int scene_object_index = FindSceneObjectIndex(runtime_tree->GetGameObject(),
                                                       &runtime_active);
    if (scene_object_index < 0) {
      runtime_tree->DetachGameObject();
      continue;
    }

    runtime_tree->SetSceneObjectIndex(static_cast<uint32_t>(scene_object_index));
    runtime_tree->SetRuntimeActive(runtime_active);
    m_runtimeTrees.push_back(std::move(runtime_tree));
  }
  m_denseIds.ClearSceneLocalState();
  for (std::unique_ptr<LN_RuntimeTree> &runtime_tree : m_runtimeTrees) {
    if (runtime_tree != nullptr) {
      runtime_tree->BindDenseIds(m_denseIds);
      runtime_tree->SetLogicManager(this);
    }
  }
  RebuildRuntimeTreeIndexes();
  for (const auto &entry : other.m_programBySourceTreeName) {
    m_programBySourceTreeName[entry.first] = entry.second;
  }
  m_cachedPrograms.clear();
  for (const std::unique_ptr<LN_RuntimeTree> &runtime_tree : m_runtimeTrees) {
    if (runtime_tree != nullptr) {
      CacheCompiledProgram(runtime_tree->GetProgram());
      m_commandBuffer.PrewarmMaterialParameterBindings(*runtime_tree);
    }
  }
  m_pendingRunOnce.insert(m_pendingRunOnce.end(),
                          std::make_move_iterator(other.m_pendingRunOnce.begin()),
                          std::make_move_iterator(other.m_pendingRunOnce.end()));
  other.m_pendingRunOnce.clear();
  other.m_programBySourceTreeName.clear();
  other.m_cachedPrograms.clear();
  other.ClearRuntimeTreeIndexes();
  other.m_runtimeTrees.clear();
  other.m_tickIndex = 0;
  other.m_updateTickIndex = 0;
  other.m_fixedTickIndex = 0;
}

void LN_Manager::CaptureInput(const bool frame_update_follows)
{
  AssertMainThread();
  /* Device input can be sampled at the fixed scheduler cadence even when rendering is capped much
   * lower. Frame and fixed consumers have independent accumulators: consuming an edge in one
   * domain must never make the other domain miss it. */
  const bool had_input_capture = m_hasFrameInput || m_hasPendingUpdateInput ||
                                 m_hasPendingFixedInput;
  const uint32_t input_channels = InputDependencyChannels();
  if (input_channels != LN_DEP_INPUT_NONE) {
    bool discard_fixed_transients = false;
    if (KX_KetsjiEngine *engine = KX_GetActiveEngine()) {
      m_inputSample.Capture(engine->GetInputDevice(), engine->GetCanvas());
      /* Fixed simulation does not advance while scheduling is paused, either explicitly or by a
       * zero time scale. Do not replay button edges, text, wheel input, or accumulated pointer
       * motion from an arbitrarily long suspension when it resumes. Latest held state and
       * absolute pointer position remain available. */
      discard_fixed_transients = engine->IsGamePaused() || engine->GetTimeScale() <= 0.0;
    }
    else {
      m_inputSample.Capture(nullptr, nullptr);
    }

    if (m_hasPendingUpdateInput) {
      m_pendingUpdateInput.AccumulateFrame(m_inputSample);
    }
    else {
      m_pendingUpdateInput.CopyFrom(m_inputSample);
      m_hasPendingUpdateInput = true;
    }
    if (m_hasPendingFixedInput) {
      m_pendingFixedInput.AccumulateFrame(m_inputSample);
    }
    else {
      m_pendingFixedInput.CopyFrom(m_inputSample);
      m_hasPendingFixedInput = true;
    }
    /* What the UI consumes is removed from m_pendingFixedInput in place: keep a copy with it for
     * the nodes that react to it as well. */
    if ((input_channels & LN_DEP_INPUT_RAW) == 0u) {
      m_hasPendingFixedRawInput = false;
    }
    else if (m_hasPendingFixedRawInput) {
      m_pendingFixedRawInput.AccumulateFrame(m_inputSample);
    }
    else {
      m_pendingFixedRawInput.CopyFrom(m_inputSample);
      m_hasPendingFixedRawInput = true;
    }
    if (discard_fixed_transients) {
      m_pendingFixedInput.ClearTransientState();
      if (m_hasPendingFixedRawInput) {
        m_pendingFixedRawInput.ClearTransientState();
      }
    }
    m_inputConsumption.NoteSample(m_inputSample);
    if (m_inputRebind.active) {
      /* While Rebind Input Action waits, the player picks a key: the game reacts to none. */
      ConsumeNewPresses(m_inputSample);
    }
    /* Without a frame update before the next fixed steps, the UI cannot see this input first:
     * let it claim, from what it saw last frame, what will be its own. */
    if (!frame_update_follows) {
      m_uiService->ConsumeInputBeforeResolve(m_inputSample);
    }
    m_inputConsumption.ApplyStrokesTo(m_pendingFixedInput);
  }
  else {
    /* Input dependencies can disappear when trees are disabled, removed, or recompiled. Drop
     * capture history at that boundary so enabling an input tree later establishes fresh mouse
     * and gamepad baselines instead of manufacturing motion or edges from stale state. Keep the
     * allocations on the steady no-input path by resetting only on the transition. */
    if (had_input_capture) {
      m_inputSample = LN_InputSnapshot();
      m_pendingUpdateInput = LN_InputSnapshot();
      m_frameInput = LN_InputSnapshot();
      m_gameFrameInput = LN_InputSnapshot();
      m_pendingFixedInput = LN_InputSnapshot();
      m_pendingFixedRawInput = LN_InputSnapshot();
      m_inputConsumption.Clear();
    }
    m_hasPendingUpdateInput = false;
    m_hasFrameInput = false;
    m_hasPendingFixedInput = false;
    m_hasPendingFixedRawInput = false;
  }
}

void LN_Manager::ConsumeInputPress(const int32_t input_code)
{
  m_inputConsumption.ConsumePress(input_code);
  m_gameFrameInput.ConsumePress(input_code);
  m_pendingFixedInput.ConsumePress(input_code);
}

void LN_Manager::ConsumeGamepadPress(const int32_t gamepad_index, const int32_t button)
{
  m_inputConsumption.ConsumeGamepadPress(gamepad_index, button);
  m_gameFrameInput.ConsumeGamepadPress(gamepad_index, button);
  m_pendingFixedInput.ConsumeGamepadPress(gamepad_index, button);
}

void LN_Manager::ConsumeWheel()
{
  m_inputConsumption.ConsumeWheel();
  m_gameFrameInput.ConsumeWheel();
  m_pendingFixedInput.ConsumeWheel();
}

void LN_Manager::ConsumeInputStroke(const int32_t input_code)
{
  m_inputConsumption.ConsumeStroke(
      input_code, m_inputSample.GetStatus(input_code, SCA_InputEvent::ACTIVE));
  m_gameFrameInput.ConsumeStroke(input_code);
  m_pendingFixedInput.ConsumeStroke(input_code);
}

void LN_Manager::BeginFrame()
{
  AssertMainThread();
  PollGameStateCompletions();
  PollActionOperations();
  /* Frame-update readers see events on the next rendered frame. The event bus independently
   * retains every publication for the next fixed step. */
  m_eventBus.BeginFrame();

  m_inputConsumption.BeginFrame();
  CaptureInput(true);
  if (m_hasPendingUpdateInput) {
    m_frameInput.CopyFrom(m_pendingUpdateInput);
    m_hasFrameInput = true;
    m_hasPendingUpdateInput = false;
  }
  else {
    m_hasFrameInput = false;
  }
}

void LN_Manager::RegisterActionOperation(
    LN_RuntimeTree *runtime_tree,
    const uint32_t completion_expression_index,
    const uint8_t output_mask,
    std::shared_ptr<const BL_ActionPlayback> playback)
{
  AssertMainThread();
  constexpr uint8_t terminal_mask = LN_ACTION_OUTPUT_COMPLETED |
                                    LN_ACTION_OUTPUT_INTERRUPTED;
  if (runtime_tree == nullptr || !runtime_tree->IsAttached() || playback == nullptr ||
      completion_expression_index == LN_INVALID_INDEX || (output_mask & terminal_mask) == 0u)
  {
    return;
  }

  ActionOperation operation;
  operation.runtime_tree = runtime_tree;
  operation.program_revision = runtime_tree->GetProgramRevision();
  operation.completion_expression_index = completion_expression_index;
  operation.output_mask = output_mask;
  operation.playback = std::move(playback);
  m_actionOperations.push_back(std::move(operation));
}

void LN_Manager::PollActionOperations()
{
  AssertMainThread();
  for (auto item = m_actionOperations.begin(); item != m_actionOperations.end();) {
    LN_RuntimeTree *runtime_tree = item->runtime_tree;
    if (runtime_tree == nullptr || !runtime_tree->IsAttached() ||
        runtime_tree->GetProgramRevision() != item->program_revision || item->playback == nullptr)
    {
      item = m_actionOperations.erase(item);
      continue;
    }

    const BL_ActionPlaybackStatus status = item->playback->GetStatus();
    if (status == BL_ActionPlaybackStatus::Playing) {
      ++item;
      continue;
    }

    const bool emit_completed = status == BL_ActionPlaybackStatus::Completed &&
                                (item->output_mask & LN_ACTION_OUTPUT_COMPLETED) != 0u;
    const bool emit_interrupted = status == BL_ActionPlaybackStatus::Interrupted &&
                                  (item->output_mask & LN_ACTION_OUTPUT_INTERRUPTED) != 0u;
    if (emit_completed || emit_interrupted) {
      const LN_ActionResult result = emit_completed ? LN_ActionResult::Completed :
                                                     LN_ActionResult::Interrupted;
      if (!runtime_tree->CompleteActionOperation(item->completion_expression_index, result)) {
        CM_Warning("Logic Nodes: dropped Play Action lifecycle event because its bounded output "
                   "queue is full");
      }
    }
    item = m_actionOperations.erase(item);
  }
}

/** Text objects show `{Action}` as that action's key or button for their player: the player of
 * the menu they are in, else the Player of the UI Element they are or hang from, else player 0. */
static std::string LN_ExpandFontText(KX_FontObject &font, const std::string &text, bool &r_live)
{
  int32_t user = 0;
  LN_Manager *manager = nullptr;
  if (KX_Scene *scene = font.GetScene()) {
    manager = scene->GetLogicNodeManager();
    if (manager != nullptr) {
      user = manager->GetUIService().UserOfObject(&font);
    }
  }
  /* Braces that are no action name may show a game property ("{Player.gold}"): such a text is
   * checked on every update, also while the property (or its object) does not exist yet. */
  r_live = false;
  const LN_InputMap::TokenResolver resolver = [&](const std::string &token,
                                                  std::string &r_text) {
    r_live = true;
    return manager != nullptr && manager->TextPropertyValue(font, token, r_text);
  };
  return LN_InputMap::Get().ExpandActionNames(text, user, &resolver);
}

bool LN_Manager::TextPropertyValue(KX_GameObject &font,
                                   const std::string &token,
                                   std::string &r_text)
{
  /* ":N" at the end: N decimals (0 to 6). */
  std::string name = token;
  int decimals = -1;
  if (const size_t colon = name.rfind(':');
      colon != std::string::npos && colon + 1 < name.size() && name.size() - colon - 1 == 1 &&
      std::isdigit(static_cast<unsigned char>(name[colon + 1])))
  {
    decimals = std::min(name[colon + 1] - '0', 6);
    name.resize(colon);
  }
  if (name.empty()) {
    return false;
  }
  EXP_Value *value = nullptr;
  /* "Object.property", at the last dot: object names often have one ("Cube.001"). */
  if (const size_t dot = name.rfind('.');
      dot != std::string::npos && dot > 0 && dot + 1 < name.size())
  {
    const std::string object_name = name.substr(0, dot);
    const uint64_t generation = m_scene.GetObjectListGeneration();
    if (generation != m_textObjectsGeneration) {
      m_textObjects.clear();
      m_textObjectsGeneration = generation;
    }
    KX_GameObject *object = nullptr;
    if (const auto cached = m_textObjects.find(object_name); cached != m_textObjects.end()) {
      object = ResolveObjectHandle(cached->second);
    }
    else {
      EXP_ListValue<KX_GameObject> *objects = m_scene.GetObjectList();
      object = objects != nullptr ? objects->FindValue(object_name) : nullptr;
      if (object != nullptr && IsGameObjectQueuedForRemoval(object)) {
        object = nullptr;
      }
      m_textObjects.emplace(object_name,
                            object != nullptr ? MakeObjectHandle(object, object_name) :
                                                LN_ObjectHandle());
    }
    /* In a player's copy of a menu, the menu's names mean that copy's objects. */
    object = m_scene.OverlayCounterpart(object, &font);
    if (object != nullptr) {
      value = object->GetProperty(name.substr(dot + 1));
    }
  }
  /* "property": the text's own, else its nearest parent's (a name or health bar above each
   * enemy copy shows that enemy's). */
  for (KX_GameObject *object = &font; object != nullptr && value == nullptr;
       object = object->GetParent())
  {
    value = object->GetProperty(name);
  }
  /* Else, in a menu, its owner's: each player's copy of a per-player menu shows that player's. */
  if (value == nullptr) {
    if (KX_GameObject *menu_owner = m_uiService->MenuOwnerOfObject(&font)) {
      value = menu_owner->GetProperty(name);
    }
  }
  if (value == nullptr) {
    return false;
  }
  const auto number_text = [&](const double number) {
    char text[64];
    if (decimals >= 0) {
      std::snprintf(text, sizeof(text), "%.*f", decimals, number);
      return std::string(text);
    }
    /* Whole numbers without decimals, others with up to two (75, 0.5, 12.34). */
    if (std::abs(number - std::round(number)) < 1.0e-6 * std::max(1.0, std::abs(number))) {
      std::snprintf(text, sizeof(text), "%.0f", std::round(number));
      return std::string(text);
    }
    std::snprintf(text, sizeof(text), "%.2f", number);
    std::string result = text;
    while (!result.empty() && result.back() == '0') {
      result.pop_back();
    }
    if (!result.empty() && result.back() == '.') {
      result.pop_back();
    }
    return result;
  };
  switch (value->GetValueType()) {
    case VALUE_INT_TYPE:
    case VALUE_FLOAT_TYPE: {
      const double number = value->GetNumber();
      r_text = std::isfinite(number) ? number_text(number) : value->GetText();
      /* Not "-0" or "-0.00" for a value just below zero. */
      if (r_text.size() > 1 && r_text[0] == '-' &&
          r_text.find_first_not_of("0.", 1) == std::string::npos)
      {
        r_text.erase(0, 1);
      }
      break;
    }
    case VALUE_BOOL_TYPE:
      r_text = value->GetNumber() != 0.0 ? "On" : "Off";
      break;
    default:
      r_text = value->GetText();
      break;
  }
  return true;
}

void LN_Manager::Update(const double current_time,
                        const double scaled_delta_time,
                        const double unscaled_delta_time,
                        const bool use_fixed_timestep,
                        const bool game_paused)
{
  {
    /* Text with action names changes when a binding, player 0's device or a menu changes. */
    const LN_InputMap &input_map = LN_InputMap::Get();
    uint64_t text_revision = input_map.Revision();
    text_revision = text_revision * 1000003u + input_map.DeviceRevision();
    text_revision = text_revision * 1000003u + LN_UIService::MenuRevision();
    KX_FontObject::SetTextFilter(&LN_ExpandFontText, text_revision);
  }
  if (game_paused) {
    RebuildPauseProcessingObjects();
  }
  else {
    ClearPauseProcessingObjects();
  }
  /* Fixed-step break events are also exposed once to the next render update.
   * This gives frame logic an immutable post-physics snapshot without
   * extending the fixed event pulse or polling the physics world. */
  ActivatePendingRigidBodyConstraintBreakEvents();
  /* Every frame, whatever Tick does: a rebind waiting here blocks the menus of every scene. */
  ProcessInputRebind(std::max(0.0, unscaled_delta_time));
  Tick(current_time,
       scaled_delta_time,
       unscaled_delta_time,
       use_fixed_timestep,
       game_paused,
       LN_Event::OnUpdate);
  ClearRigidBodyConstraintBreakEvents();
  /* Set Cursor Visibility's Only in Menus: menus this frame opened or closed are in place. */
  m_uiService->UpdateMenuCursor();
  if (game_paused) {
    /* No Logic + Physics step comes while paused: a drop from a paused menu lands now. */
    SpawnPendingItemDrops();
  }
  /* Play Sound 3D: follow the speakers and the cameras that hear them. */
  LN_CommandBuffer::UpdateSpatialSounds(&m_scene);
  /* Set Volume's fades (by the clock: another scene's call this frame changes nothing more). */
  LN_CommandBuffer::UpdateSoundGroups();
}

void LN_Manager::RebuildPauseProcessingObjects()
{
  m_pauseProcessingObjects.clear();
  m_pauseProcessingObjectsScratch.clear();

  for (const std::unique_ptr<LN_RuntimeTree> &runtime_tree : m_runtimeTrees) {
    if (runtime_tree == nullptr || runtime_tree->GetProgram() == nullptr ||
        !runtime_tree->GetProgram()->HasPauseProcessingRequests())
    {
      continue;
    }
    runtime_tree->CollectPauseProcessingObjects(m_pauseProcessingObjectsScratch);
  }

  if (m_pauseProcessingObjects.bucket_count() < m_pauseProcessingObjectsScratch.size()) {
    m_pauseProcessingObjects.reserve(m_pauseProcessingObjectsScratch.size());
  }
  for (KX_GameObject *game_object : m_pauseProcessingObjectsScratch) {
    if (game_object != nullptr && game_object->GetScene() == &m_scene &&
        !IsGameObjectQueuedForRemoval(game_object))
    {
      m_pauseProcessingObjects.insert(game_object);
    }
  }
}

bool LN_Manager::IsObjectPauseProcessed(const KX_GameObject *gameobj) const
{
  return gameobj != nullptr &&
         m_pauseProcessingObjects.find(const_cast<KX_GameObject *>(gameobj)) !=
             m_pauseProcessingObjects.end();
}

void LN_Manager::RefreshPauseProcessingObjects()
{
  AssertMainThread();
  RebuildPauseProcessingObjects();
}

void LN_Manager::ClearPauseProcessingObjects()
{
  m_pauseProcessingObjects.clear();
  m_pauseProcessingObjectsScratch.clear();
}

void LN_Manager::FixedUpdate(const double current_time,
                             const double fixed_delta_time,
                             const double unscaled_delta_time)
{
  /* A pulse is active for one fixed step, including when several catch-up steps run in one outer
   * frame. Events remain queued across outer frames that contain no fixed step. */
  m_eventBus.BeginFixedTick();
  CollectRigidBodyConstraintBreakEvents();
  SpawnPendingItemDrops();
  Tick(current_time,
       fixed_delta_time,
       unscaled_delta_time,
       true,
       false,
       LN_Event::OnFixedUpdate);
  if (m_hasPendingFixedInput) {
    m_pendingFixedInput.ClearTransientState();
  }
  if (m_hasPendingFixedRawInput) {
    m_pendingFixedRawInput.ClearTransientState();
  }
  QueueRigidBodyConstraintBreakEventsForUpdate();
}

KX_GameObject *LN_Manager::SpawnItemDrop(KX_GameObject *source,
                                         const MT_Vector3 &position,
                                         const std::string &item,
                                         const int32_t count)
{
  if (source == nullptr || IsGameObjectQueuedForRemoval(source)) {
    return nullptr;
  }
  std::string error;
  KX_GameObject *copy = LN_CommandBuffer::AddTrackedObject(this, source, source, error);
  if (copy == nullptr) {
    CM_Warning("Logic Nodes: Drop Item could not put '" << item << "' in the world: " << error);
    return nullptr;
  }
  /* The scene holds the copy; this call's reference is not kept (as for Add Object). */
  copy->Release();
  copy->NodeSetWorldPosition(position);
  copy->NodeUpdateGS(0.0f);
  /* It starts here, not at the library object: no interpolated frame in between, and physics
   * bodies (also kinematic ones, which follow their object only on the next step) are here now. */
  const std::function<void(SG_Node *)> rebase = [&](SG_Node *node) {
    if (KX_GameObject *object = static_cast<KX_GameObject *>(node->GetSGClientObject())) {
      object->RebasePhysicsInterpolationState();
      if (PHY_IPhysicsController *controller = object->GetPhysicsController()) {
        controller->SetTransform();
        controller->RebaseKinematicMotionState();
      }
    }
    for (SG_Node *child : node->GetSGChildren()) {
      rebase(child);
    }
  };
  rebase(copy->GetSGNode());
  /* The copy carries what it is and how many, so picking it up gives the same stack back. */
  LN_Value item_value;
  item_value.type = LN_ValueType::String;
  item_value.exists = true;
  item_value.string_value = item;
  LN_Value count_value;
  count_value.type = LN_ValueType::Int;
  count_value.exists = true;
  count_value.int_value = count;
  std::string reason;
  if (!LN_CommandBuffer::ApplyGamePropertyValue(copy, "item", item_value, nullptr, reason) ||
      !LN_CommandBuffer::ApplyGamePropertyValue(copy, "count", count_value, nullptr, reason))
  {
    CM_Warning("Logic Nodes: Drop Item: " << reason);
  }
  /* A drop that is saved (its library object has Save Object State) keeps what it holds. */
  if (copy->GetSaveGameObjectState()) {
    copy->AddSaveGameProperty("item", KX_GameObject::SaveGamePropertyMetadata::Type::String);
    copy->AddSaveGameProperty("count", KX_GameObject::SaveGamePropertyMetadata::Type::Int);
  }
  return copy;
}

void LN_Manager::QueueItemDrop(KX_GameObject *source,
                               const MT_Vector3 &position,
                               const std::string &item,
                               const int32_t count)
{
  if (source == nullptr) {
    return;
  }
  PendingItemDrop &drop = m_pendingItemDrops.emplace_back();
  drop.source = MakeObjectHandle(source, source->GetName());
  drop.position = position;
  drop.item = item;
  drop.count = count;
}

void LN_Manager::QueuedItemDropPositions(std::vector<MT_Vector3> &r_positions) const
{
  for (const PendingItemDrop &drop : m_pendingItemDrops) {
    r_positions.push_back(drop.position);
  }
}

void LN_Manager::EnsureInventoryRuleSets()
{
  if (m_inventoryRuleSetsLoaded) {
    return;
  }
  m_inventoryRuleSetsLoaded = true;
  const blender::Scene *scene = m_scene.GetBlenderScene();
  if (scene == nullptr) {
    return;
  }
  for (const blender::GameInventoryRules &source : scene->gm.inventory_rules) {
    LN_InventoryRuleSet rules;
    rules.capacity = source.capacity;
    rules.capacity_from = source.capacity_from;
    rules.stack = (source.flag & GAME_INVENTORY_STACK) != 0;
    rules.stack_from = source.stack_from;
    rules.max_stack = source.max_stack;
    rules.max_stack_from = source.max_stack_from;
    rules.keep_places = (source.flag & GAME_INVENTORY_KEEP_PLACES) != 0;
    rules.keep_places_from = source.keep_places_from;
    rules.recipes = source.recipes;
    rules.accept_categories = source.accept_categories;
    rules.accept_categories_from = source.accept_categories_from;
    rules.library = source.item_library;
    m_inventoryRuleSets.emplace(source.name, std::move(rules));
  }
}

LN_InventoryRules LN_Manager::InventoryRulesOf(KX_GameObject *owner, const std::string &property)
{
  const std::string *name = owner != nullptr ? owner->GetInventoryRuleSet(property) : nullptr;
  if (name == nullptr) {
    /* No pick: the defaults, and no scene read (the unit tests' scene is not one). */
    return LN_InventoryRules();
  }
  EnsureInventoryRuleSets();
  const auto found = m_inventoryRuleSets.find(*name);
  if (found == m_inventoryRuleSets.end()) {
    if (m_inventoryRuleWarnings.insert("set\x1f" + *name).second) {
      CM_Warning("Inventory Rules: the scene has no set \""
                 << *name << "\", which game property '" << property << "' of '"
                 << owner->GetName() << "' picks; it follows the defaults");
    }
    return LN_InventoryRules();
  }
  std::vector<std::string> missing;
  LN_InventoryRules rules = LN_ResolveInventoryRules(&found->second, owner, &missing);
  for (const std::string &from : missing) {
    if (m_inventoryRuleWarnings.insert(*name + "\x1f" + owner->GetName() + "\x1f" + from).second)
    {
      CM_Warning("Inventory Rules \"" << *name << "\": '" << owner->GetName()
                                      << "' has no game property '" << from
                                      << "' to read from; its value is used");
    }
  }
  return rules;
}

void LN_Manager::SetInventoryRuleSetForTests(const std::string &name,
                                             const LN_InventoryRuleSet &rules)
{
  m_inventoryRuleSetsLoaded = true;
  m_inventoryRuleSets.insert_or_assign(name, rules);
}

void LN_Manager::SpawnPendingItemDrops()
{
  if (m_pendingItemDrops.empty()) {
    return;
  }
  /* A drop can queue no new drop while it spawns, but keep the loop safe if one does. */
  std::vector<PendingItemDrop> drops = std::move(m_pendingItemDrops);
  m_pendingItemDrops.clear();
  for (const PendingItemDrop &drop : drops) {
    SpawnItemDrop(ResolveObjectHandle(drop.source), drop.position, drop.item, drop.count);
  }
}

bool LN_Manager::HasInputDependencies() const
{
  return InputDependencyChannels() != LN_DEP_INPUT_NONE;
}

uint32_t LN_Manager::InputDependencyChannels() const
{
  /* UI reads mouse, keyboard and gamepad too: sample every pass so a click that falls between
   * two rendered frames is not lost. */
  uint32_t channels = m_uiService->HasElements() ? kUIElementInputChannels : LN_DEP_INPUT_NONE;
  for (const std::unique_ptr<LN_RuntimeTree> &runtime_tree : m_runtimeTrees) {
    if (runtime_tree == nullptr) {
      continue;
    }
    if (runtime_tree->CollectsObjectUI()) {
      channels |= kUIElementInputChannels;
    }
    if (const std::shared_ptr<const LN_Program> program = runtime_tree->GetProgram()) {
      channels |= program->GetDependencySummary().input_channels;
    }
  }
  return channels;
}

void LN_Manager::CollectRigidBodyConstraintBreakEvents()
{
  AssertMainThread();
  ClearRigidBodyConstraintBreakEvents();

  PHY_IPhysicsEnvironment *physics_env = m_scene.GetPhysicsEnvironment();
  if (physics_env == nullptr) {
    return;
  }

  std::vector<PHY_RigidBodyConstraintBreakEvent> physics_events;
  physics_env->ConsumeRigidBodyConstraintBreakEvents(physics_events);
  if (physics_events.empty()) {
    return;
  }

  /* Breaks are exceptional, so resolve names once instead of retaining raw
   * physics pointers across callbacks or object destruction. */
  RebuildObjectNameCache();
  if (m_rigidBodyConstraintBreakEvents.capacity() < physics_events.size()) {
    m_rigidBodyConstraintBreakEvents.reserve(physics_events.size());
  }

  for (PHY_RigidBodyConstraintBreakEvent &physics_event : physics_events) {
    if (physics_event.owner_name.empty() || physics_event.constraint_name.empty()) {
      continue;
    }

    const auto owner_iter = m_objectByName.find(physics_event.owner_name);
    if (owner_iter == m_objectByName.end() || owner_iter->second == nullptr ||
        IsGameObjectQueuedForRemoval(owner_iter->second))
    {
      continue;
    }

    KX_GameObject *target = nullptr;
    if (!physics_event.target_name.empty()) {
      const auto target_iter = m_objectByName.find(physics_event.target_name);
      if (target_iter != m_objectByName.end() && target_iter->second != nullptr &&
          !IsGameObjectQueuedForRemoval(target_iter->second))
      {
        target = target_iter->second;
      }
    }

    LN_RigidBodyConstraintBreakEvent event;
    event.owner = MakeObjectHandle(owner_iter->second, physics_event.owner_name);
    event.target = MakeObjectHandle(target, physics_event.target_name);
    event.constraint_name = std::move(physics_event.constraint_name);
    event.anchor_position = physics_event.anchor_position;
    event.linear_impulse = physics_event.linear_impulse;
    event.angular_impulse = physics_event.angular_impulse;
    event.applied_impulse = physics_event.applied_impulse;
    m_rigidBodyConstraintBreakEvents.push_back(std::move(event));
  }

  IndexRigidBodyConstraintBreakEvents();
}

void LN_Manager::ActivatePendingRigidBodyConstraintBreakEvents()
{
  AssertMainThread();
  ClearRigidBodyConstraintBreakEvents();
  if (m_pendingRigidBodyConstraintBreakEvents.empty()) {
    return;
  }

  m_rigidBodyConstraintBreakEvents.swap(m_pendingRigidBodyConstraintBreakEvents);
  IndexRigidBodyConstraintBreakEvents();
}

void LN_Manager::QueueRigidBodyConstraintBreakEventsForUpdate()
{
  AssertMainThread();
  if (m_rigidBodyConstraintBreakEvents.empty()) {
    ClearRigidBodyConstraintBreakEvents();
    return;
  }

  /* Accumulate every fixed substep completed before the next render update.
   * The first batch swaps in O(1); catch-up substeps only move exceptional
   * event records and preserve deterministic chronological/constraint-ID
   * order. */
  if (m_pendingRigidBodyConstraintBreakEvents.empty()) {
    m_pendingRigidBodyConstraintBreakEvents.swap(m_rigidBodyConstraintBreakEvents);
  }
  else {
    const size_t required_size = m_pendingRigidBodyConstraintBreakEvents.size() +
                                 m_rigidBodyConstraintBreakEvents.size();
    if (m_pendingRigidBodyConstraintBreakEvents.capacity() < required_size) {
      m_pendingRigidBodyConstraintBreakEvents.reserve(required_size);
    }
    m_pendingRigidBodyConstraintBreakEvents.insert(
        m_pendingRigidBodyConstraintBreakEvents.end(),
        std::make_move_iterator(m_rigidBodyConstraintBreakEvents.begin()),
        std::make_move_iterator(m_rigidBodyConstraintBreakEvents.end()));
    m_rigidBodyConstraintBreakEvents.clear();
  }

  m_rigidBodyConstraintBreakEventLookup.clear();
  m_rigidBodyConstraintBreakEventOwnerLookup.clear();
  m_rigidBodyConstraintBreakEventNextByOwner.clear();
}

void LN_Manager::IndexRigidBodyConstraintBreakEvents()
{
  m_rigidBodyConstraintBreakEventLookup.clear();
  m_rigidBodyConstraintBreakEventOwnerLookup.clear();
  m_rigidBodyConstraintBreakEventNextByOwner.assign(m_rigidBodyConstraintBreakEvents.size(),
                                                    LN_INVALID_INDEX);

  const size_t event_count = m_rigidBodyConstraintBreakEvents.size();
  if (event_count == 0) {
    return;
  }
  if (m_rigidBodyConstraintBreakEventLookup.bucket_count() < event_count) {
    m_rigidBodyConstraintBreakEventLookup.reserve(event_count);
  }
  if (m_rigidBodyConstraintBreakEventOwnerLookup.bucket_count() < event_count) {
    m_rigidBodyConstraintBreakEventOwnerLookup.reserve(event_count);
  }

  for (uint32_t event_index = 0; event_index < m_rigidBodyConstraintBreakEvents.size();
       event_index++)
  {
    const LN_RigidBodyConstraintBreakEvent &event =
        m_rigidBodyConstraintBreakEvents[event_index];
    KX_GameObject *owner = ResolveObjectHandle(event.owner);
    if (owner == nullptr || IsGameObjectQueuedForRemoval(owner) ||
        event.constraint_name.empty())
    {
      continue;
    }

    const auto [owner_event_iter, inserted_owner_event] =
        m_rigidBodyConstraintBreakEventOwnerLookup.try_emplace(
            owner, ConstraintBreakOwnerEventRange{event_index, event_index, 1});
    if (!inserted_owner_event) {
      ConstraintBreakOwnerEventRange &owner_events = owner_event_iter->second;
      if (owner_events.last_event_index < m_rigidBodyConstraintBreakEventNextByOwner.size()) {
        m_rigidBodyConstraintBreakEventNextByOwner[owner_events.last_event_index] = event_index;
      }
      owner_events.last_event_index = event_index;
      if (owner_events.event_count != UINT32_MAX) {
        owner_events.event_count++;
      }
    }

    const ConstraintBreakLookupKey key{owner,
                                       std::hash<std::string>{}(event.constraint_name)};
    /* Normal authored/runtime constraints have unique names. If malformed
     * data duplicates a name, keep every record but route the first one. */
    bool has_matching_name = false;
    const auto range = m_rigidBodyConstraintBreakEventLookup.equal_range(key);
    for (auto item = range.first; item != range.second; ++item) {
      if (m_rigidBodyConstraintBreakEvents[item->second].constraint_name ==
          event.constraint_name)
      {
        has_matching_name = true;
        break;
      }
    }
    if (!has_matching_name) {
      m_rigidBodyConstraintBreakEventLookup.emplace(key, event_index);
    }
  }
}

void LN_Manager::ClearRigidBodyConstraintBreakEvents()
{
  m_rigidBodyConstraintBreakEvents.clear();
  m_rigidBodyConstraintBreakEventLookup.clear();
  m_rigidBodyConstraintBreakEventOwnerLookup.clear();
  m_rigidBodyConstraintBreakEventNextByOwner.clear();
}

void LN_Manager::ClearPendingRigidBodyConstraintBreakEvents()
{
  m_pendingRigidBodyConstraintBreakEvents.clear();
}

void LN_Manager::TickUIElementsWithoutTrees(const double delta_time,
                                            const double unscaled_delta_time)
{
  /* A scene can show UI Elements (an anchored HUD, hover looks) without any logic tree. */
  m_uiService->RefreshElementRegistry();
  if (!m_uiService->HasElements()) {
    return;
  }
  const uint64_t tick_index = m_tickIndex++;
  CaptureSharedTickReadContext(
      kUIElementInputChannels, std::max(0.0, delta_time), tick_index, LN_Event::OnUpdate);
  m_uiService->BeginTick(
      LN_Event::OnUpdate, tick_index, UIFrameInput(), std::max(0.0, unscaled_delta_time));
  m_uiService->CollectElements();
  m_uiService->ResolveInteractions();
  m_uiService->UpdateElementPresentation(unscaled_delta_time);
}

void LN_Manager::Tick(const double current_time,
                      const double delta_time,
                      const double unscaled_delta_time,
                      const bool use_fixed_timestep,
                      const bool game_paused,
                      const LN_Event event)
{
  AssertMainThread();
  InvalidatePointerHitCache();

  if (!m_debugBindingsScanned && logic_nodes_enable_debug_property_bindings()) {
    RegisterDebugPropertyBindings();
  }
  else if (!m_debugBindingsScanned) {
    m_debugBindingsScanned = true;
  }

  m_profileTickMetricsEnabled = false;

  if (m_gameStateTransitionPending) {
    if (PHY_IPhysicsEnvironment *physics_env = m_scene.GetPhysicsEnvironment()) {
      physics_env->SetLogicCollisionContactCacheEnabled(false);
    }
    return;
  }

  if (m_runtimeTrees.empty()) {
    if (PHY_IPhysicsEnvironment *physics_env = m_scene.GetPhysicsEnvironment()) {
      physics_env->SetLogicCollisionContactCacheEnabled(false);
    }
    if (event == LN_Event::OnUpdate) {
      TickUIElementsWithoutTrees(delta_time, unscaled_delta_time);
    }
    return;
  }

  if (!use_fixed_timestep) {
    if (PHY_IPhysicsEnvironment *physics_env = m_scene.GetPhysicsEnvironment()) {
      physics_env->SetLogicCollisionContactCacheEnabled(false);
    }
    if (!m_reportedVariableTimestepSkip) {
      CM_Warning("Logic Nodes skipped: v1 requires fixed physics timestep mode.");
      m_reportedVariableTimestepSkip = true;
    }
    return;
  }

  /* On Game Loaded runs in the update phase: keep the pulse pending through the init and fixed
   * ticks that can come first (e.g. right after a cross-scene load). */
  if (event == LN_Event::OnUpdate) {
    /* One join a frame: two players joining in one frame come one frame apart. A join while the
     * game is paused waits until it runs again (On Player Join's route doesn't run paused). */
    m_playerJoinActive = false;
    if (!game_paused && !m_joinsAtStart.empty()) {
      m_playerJoinActive = true;
      m_playerJoinUser = m_joinsAtStart.front().first;
      m_playerJoinGamepad = m_joinsAtStart.front().second;
      m_joinsAtStart.erase(m_joinsAtStart.begin());
    }
    else if (!game_paused) {
      m_playerJoinActive = LN_InputMap::Get().NextJoin(
          m_lastPlayerJoinSerial, m_playerJoinUser, m_playerJoinGamepad);
    }
    m_gameLoadPulseActive = std::exchange(m_gameLoadPulsePending, false);
    m_gameLoadPulseSucceeded = m_gameLoadPulseActive && m_pendingGameLoadSucceeded;
    m_pendingGameLoadSucceeded = false;
  }

  const bool report_tick_timings = logic_nodes_report_tick_timings(m_scene);
  m_profileTickMetricsEnabled = report_tick_timings;

  std::optional<LN_TickProfileMetrics> profile;
  if (report_tick_timings) {
    profile.emplace();
    m_tickHotMapLookupCount.store(0, std::memory_order_relaxed);
  }

  std::vector<LN_ParallelTreeExecutor::RuntimeTreeWorkItem> &runnable_trees =
      m_tickRunnableTreesScratch;
  runnable_trees.clear();
  if (report_tick_timings) {
    logic_nodes_profiled_reserve(runnable_trees, m_runtimeTrees.size(), *profile);
  }
  else if (runnable_trees.capacity() < m_runtimeTrees.size()) {
    runnable_trees.reserve(m_runtimeTrees.size());
  }
  /* While paused, a Frame Update runs the trees While Paused keeps running, and of every other
   * tree only its UI event routes, so menu buttons work in a paused game. */
  const auto pause_eligible = [&](LN_RuntimeTree &runtime_tree) {
    return !game_paused || event != LN_Event::OnUpdate ||
           IsObjectPauseProcessed(runtime_tree.GetGameObject()) ||
           (runtime_tree.GetProgram() != nullptr && runtime_tree.GetProgram()->HasUIEventRoutes());
  };
  for (std::unique_ptr<LN_RuntimeTree> &runtime_tree : m_runtimeTrees) {
    runtime_tree->SetUIEventRoutesOnly(game_paused && event == LN_Event::OnUpdate &&
                                       !IsObjectPauseProcessed(runtime_tree->GetGameObject()));
  }
  for (size_t index = 0; index < m_runtimeTrees.size(); index++) {
    std::unique_ptr<LN_RuntimeTree> &runtime_tree = m_runtimeTrees[index];
    if (runtime_tree->CanTick(event) && pause_eligible(*runtime_tree)) {
      LN_ParallelTreeExecutor::RuntimeTreeWorkItem item;
      item.runtime_tree_index = index;
      item.runtime_tree = runtime_tree.get();
      runnable_trees.push_back(item);
    }
  }

  std::unordered_set<LN_RuntimeTree *> pending_forced_trees;
  if (!m_pendingRunOnce.empty()) {
    pending_forced_trees.reserve(m_pendingRunOnce.size());
    for (const std::pair<KX_GameObject *, std::string> &item : m_pendingRunOnce) {
      if (item.first == nullptr || item.second.empty() || IsGameObjectQueuedForRemoval(item.first)) {
        continue;
      }
      if (LN_RuntimeTree *runtime_tree = FindRuntimeTreeBySourceName(item.first, item.second)) {
        if (!runtime_tree->IsRuntimeActive() ||
            (game_paused && event == LN_Event::OnUpdate &&
             !IsObjectPauseProcessed(runtime_tree->GetGameObject())))
        {
          continue;
        }
        pending_forced_trees.insert(runtime_tree);
      }
    }
  }

  uint32_t shared_input_channels = LN_DEP_INPUT_NONE;
  uint32_t shared_query_channels = LN_DEP_QUERY_NONE;
  for (const LN_ParallelTreeExecutor::RuntimeTreeWorkItem &item : runnable_trees) {
    std::shared_ptr<const LN_Program> program = item.runtime_tree != nullptr ?
                                                    item.runtime_tree->GetProgram() :
                                                    nullptr;
    if (program != nullptr) {
      const LN_ProgramDependencySummary &dependencies = program->GetDependencySummary();
      shared_input_channels |= dependencies.input_channels;
      shared_query_channels |= dependencies.query_channels;
    }
  }
  for (LN_RuntimeTree *runtime_tree : pending_forced_trees) {
    std::shared_ptr<const LN_Program> program = runtime_tree != nullptr ?
                                                    runtime_tree->GetProgram() :
                                                    nullptr;
    if (program != nullptr) {
      const LN_ProgramDependencySummary &dependencies = program->GetDependencySummary();
      shared_input_channels |= dependencies.input_channels;
      shared_query_channels |= dependencies.query_channels;
    }
  }
  /* Object UI resolves once per rendered frame, also while paused, so menus react at display
   * rate and keep working in a paused game. Logic + Physics steps only read its latest state. */
  const bool resolves_ui = event == LN_Event::OnUpdate;
  if (resolves_ui) {
    for (const std::unique_ptr<LN_RuntimeTree> &runtime_tree : m_runtimeTrees) {
      if (runtime_tree->CollectsObjectUI()) {
        shared_input_channels |= runtime_tree->GetProgram()->GetDependencySummary().input_channels;
      }
    }
    m_uiService->RefreshElementRegistry();
    if (m_uiService->HasElements()) {
      shared_input_channels |= kUIElementInputChannels;
    }
  }

  if (PHY_IPhysicsEnvironment *physics_env = m_scene.GetPhysicsEnvironment()) {
    physics_env->SetLogicCollisionContactCacheEnabled(
        (shared_query_channels & LN_DEP_QUERY_COLLISION_CONTACT) != 0,
        (shared_query_channels & LN_DEP_QUERY_COLLISION_CONTACT_DETAILS) != 0);
  }

  LN_TickContext context;
  context.event = event;
  context.current_time = current_time;
  context.delta_time = std::max(0.0, delta_time);
  context.unscaled_dt = std::max(0.0, unscaled_delta_time);
  context.use_fixed_timestep = use_fixed_timestep;
  context.game_paused = game_paused;
  for (int32_t user = 0; user < LN_MAX_GAMEPADS; ++user) {
    if (m_uiService->UIUsesPlayerInput(user)) {
      context.ui_input_users |= uint8_t(1u << uint32_t(user));
    }
  }
  /* An unfocused window's mouse is another program's: Mouse Look waits and starts over after. */
  const DEV_InputDevice *device = KX_GetActiveEngine() != nullptr ?
                                      dynamic_cast<const DEV_InputDevice *>(
                                          KX_GetActiveEngine()->GetInputDevice()) :
                                      nullptr;
  context.mouse_look_waits = (context.ui_input_users & 1u) != 0 ||
                             (device != nullptr && !device->IsWindowFocused());
  /* Once when a pause or a wait starts: a Mouse Look that kept running through it (While Paused)
   * starts over only then, one that stopped starts over at its next run. */
  if (event == LN_Event::OnUpdate) {
    const bool held = game_paused || context.mouse_look_waits;
    if (held && !m_mouseLookHeld) {
      m_mouseUISerial++;
    }
    m_mouseLookHeld = held;
  }
  context.mouse_ui_serial = m_mouseUISerial;
  static const LN_RuntimeFeatureConfig feature_config = LN_DefaultRuntimeFeatureConfig();
  context.use_register_expression_evaluator = LN_RuntimeFeatureEnabled(
      feature_config, LN_RUNTIME_FEATURE_REGISTER_EXPRESSION_EVALUATOR);
  context.use_typed_command_streams = LN_RuntimeFeatureEnabled(
      feature_config, LN_RUNTIME_FEATURE_TYPED_COMMAND_STREAMS);
  context.tick_index = m_tickIndex++;
  context.phase_tick_index =
      event == LN_Event::OnUpdate ? m_updateTickIndex++ : m_fixedTickIndex++;
  m_commandBuffer.SetTypedCommandStreamsEnabled(context.use_typed_command_streams);

  m_isTicking = true;
  m_objectNameCacheValid = false;

  std::chrono::steady_clock::time_point snapshot_start;
  if (report_tick_timings) {
    snapshot_start = std::chrono::steady_clock::now();
  }
  std::chrono::steady_clock::time_point event_delivery_start;
  if (report_tick_timings) {
    event_delivery_start = std::chrono::steady_clock::now();
  }
  if (report_tick_timings) {
    const auto event_delivery_end = std::chrono::steady_clock::now();
    profile->event_delivery_ms = std::chrono::duration<double, std::milli>(
                                     event_delivery_end - event_delivery_start)
                                     .count();
    profile->event_count = m_eventBus.TickEventCount();
  }
  CaptureSharedTickReadContext(
      shared_input_channels, context.delta_time, context.tick_index, event);
  /* Before the UI is collected: its declarations may read Input Map actions too. */
  UpdateInputActions(event, shared_input_channels, context.phase_tick_index);
  if (resolves_ui) {
    m_uiService->BeginTick(event, context.tick_index, UIFrameInput(), context.unscaled_dt);
    if (m_hasFrameInput && (InputDependencyChannels() & LN_DEP_INPUT_ACTION) != 0u) {
      /* For Get Input Binding's Automatic, also when no frame-update tree reads actions. */
      LN_InputMap::Get().NoteInput(m_frameInput);
    }
  }
  else {
    m_uiService->BeginPassivePhase(event, m_sharedTickReadContext.input);
  }
  for (std::unique_ptr<LN_RuntimeTree> &runtime_tree : m_runtimeTrees) {
    const bool force_snapshot =
        !pending_forced_trees.empty() &&
        pending_forced_trees.find(runtime_tree.get()) != pending_forced_trees.end();
    const bool collects_ui = resolves_ui && runtime_tree->CollectsObjectUI();
    if (((!runtime_tree->CanTick(event) && !force_snapshot) || !pause_eligible(*runtime_tree)) &&
        !collects_ui)
    {
      continue;
    }
    if (runtime_tree->ShouldCaptureSnapshot() ||
        (force_snapshot && runtime_tree->ShouldCaptureSnapshotForForcedUpdate()))
    {
      runtime_tree->CaptureSnapshot(&m_sharedTickReadContext,
                                    float(context.delta_time),
                                    context.tick_index,
                                    force_snapshot);
      if (report_tick_timings) {
        const LN_SnapshotCaptureStats &snapshot_stats =
            runtime_tree->GetSnapshot().GetCaptureStats();
        profile->snapshot_tree_count++;
        profile->snapshot_captured_channel_mask |= snapshot_stats.captured_channels;
        profile->snapshot_skipped_channel_mask |= snapshot_stats.skipped_channels;
        profile->snapshot_shared_input_tree_count += snapshot_stats.used_shared_input ? 1u : 0u;
        profile->snapshot_property_storage_reuse_count +=
            snapshot_stats.reused_property_storage ? 1u : 0u;
      }
    }
  }

  if (resolves_ui) {
    /* Object UI is scene-wide. Resolve container layout first so slot/content transforms and
     * clipping are authoritative when interactable bounds are collected in the same phase. */
    for (std::unique_ptr<LN_RuntimeTree> &runtime_tree : m_runtimeTrees) {
      if (runtime_tree->CollectsObjectUI()) {
        runtime_tree->CollectUIViews(context);
      }
    }
    m_uiService->ResolveViews();

    /* Collect every UI Element, then resolve one deterministic routing pass before individual
     * trees warm or consume their query outputs. */
    m_uiService->CollectElements();
    m_uiService->ResolveInteractions();
    m_uiService->UpdateElementPresentation(context.unscaled_dt);
    /* A widget wrote its game property after the trees took their snapshot: the owner's trees
     * read the new value in this tick (On UI Event Changed → Get Game Property). */
    for (const LN_ObjectHandle &owner_handle : m_uiService->WidgetChangedOwners()) {
      if (KX_GameObject *owner = ResolveObjectHandle(owner_handle)) {
        for (std::unique_ptr<LN_RuntimeTree> &runtime_tree : m_runtimeTrees) {
          if (runtime_tree->GetGameObject() == owner) {
            runtime_tree->RefreshSnapshotGameProperties();
          }
        }
      }
    }
    for (std::unique_ptr<LN_RuntimeTree> &runtime_tree : m_runtimeTrees) {
      if (runtime_tree->CollectsObjectUI()) {
        runtime_tree->InvalidateUICollectionCaches();
      }
    }
  }
  if (resolves_ui) {
    RefreshInputActions();
  }

  for (std::unique_ptr<LN_RuntimeTree> &runtime_tree : m_runtimeTrees) {
    const bool force_snapshot =
        !pending_forced_trees.empty() &&
        pending_forced_trees.find(runtime_tree.get()) != pending_forced_trees.end();
    if ((!runtime_tree->CanTick(event) && !force_snapshot) || !pause_eligible(*runtime_tree)) {
      continue;
    }
    const size_t query_count = runtime_tree->WarmQueryCache(context);
    if (report_tick_timings) {
      profile->query_count += uint32_t(query_count);
    }
  }
  if (report_tick_timings) {
    const auto snapshot_end = std::chrono::steady_clock::now();
    profile->snapshot_ms = std::chrono::duration<double, std::milli>(snapshot_end - snapshot_start)
                               .count();
  }

  const bool has_command_work = !runnable_trees.empty() || !m_pendingRunOnce.empty();
  if (has_command_work) {
    m_commandBuffer.Clear();

    const bool resource_scheduler_enabled = LN_RuntimeFeatureEnabled(
        feature_config, LN_RUNTIME_FEATURE_RESOURCE_CLASS_SCHEDULER);
    const bool bypass_scheduler_for_serial_recording =
        !resource_scheduler_enabled && m_pendingRunOnce.empty();
    LN_ParallelTreeExecutor::SchedulerPlan scheduler_plan;
    LN_ParallelTreeExecutor::SchedulerPlannerStats scheduler_stats;
    if (bypass_scheduler_for_serial_recording) {
      scheduler_stats.planned_tree_count = uint32_t(runnable_trees.size());
      scheduler_stats.main_thread_tree_count = uint32_t(runnable_trees.size());
      scheduler_stats.main_thread_batch_count = runnable_trees.empty() ? 0u : 1u;
      scheduler_stats.scheduler_job_count = scheduler_stats.main_thread_batch_count;
    }
    else {
      const bool parallel_enabled = resource_scheduler_enabled &&
                                    LN_ParallelTreeExecutor::IsEnabled(m_scene) &&
                                    runnable_trees.size() >=
                                        LN_ParallelTreeExecutor::MinimumTreeCount();
      scheduler_plan = LN_ParallelTreeExecutor::BuildSchedulerPlan(runnable_trees,
                                                                   parallel_enabled);
      scheduler_stats = LN_ParallelTreeExecutor::StatsForPlan(scheduler_plan);
    }
    if (report_tick_timings) {
      profile->scheduler_planned_tree_count = scheduler_stats.planned_tree_count;
      profile->scheduler_worker_batch_count = scheduler_stats.worker_batch_count;
      profile->scheduler_main_thread_batch_count = scheduler_stats.main_thread_batch_count;
      profile->scheduler_max_trees_per_worker_batch =
          scheduler_stats.max_trees_per_worker_batch;
      profile->scheduler_average_trees_per_worker_batch_x100 =
          scheduler_stats.average_trees_per_worker_batch_x100;
      profile->scheduler_worker_utilization_x100 =
          scheduler_stats.planned_tree_count == 0 ?
              0 :
              (scheduler_stats.worker_tree_count * 100u) / scheduler_stats.planned_tree_count;
      profile->scheduler_command_resource_classes = scheduler_stats.command_resource_classes;
      profile->scheduler_worker_resource_access = scheduler_stats.worker_resource_access;
      profile->scheduler_main_thread_resource_access =
          scheduler_stats.main_thread_resource_access;
      profile->scheduler_query_tree_count = scheduler_stats.query_tree_count;
      profile->scheduler_snapshot_only_tree_count = scheduler_stats.snapshot_only_tree_count;
      profile->scheduler_command_record_only_tree_count =
          scheduler_stats.command_record_only_tree_count;
      profile->scheduler_immutable_worker_inputs = scheduler_stats.immutable_worker_inputs;
      profile->scheduler_worker_metrics_are_local = scheduler_stats.worker_metrics_are_local;
      profile->scheduler_deterministic_merge_order =
          scheduler_stats.deterministic_merge_order;
      profile->scheduler_main_thread_flush_isolated =
          scheduler_stats.main_thread_flush_isolated;
    }

    const bool direct_serial_recording =
        (bypass_scheduler_for_serial_recording && !runnable_trees.empty()) ||
        LN_ParallelTreeExecutor::ShouldUseDirectSerialCommandRecording(
            scheduler_plan, scheduler_stats, !m_pendingRunOnce.empty());
    if (direct_serial_recording) {
      std::chrono::steady_clock::time_point serial_start;
      if (report_tick_timings) {
        serial_start = std::chrono::steady_clock::now();
      }

      m_commandBuffer.BeginRecording();
      for (const LN_ParallelTreeExecutor::RuntimeTreeWorkItem &item : runnable_trees) {
        if (item.runtime_tree == nullptr) {
          continue;
        }
        LN_RuntimeProfileCounters profile_counters;
        item.runtime_tree->ExecuteReady(
            m_commandBuffer, context, report_tick_timings ? &profile_counters : nullptr);
        if (report_tick_timings) {
          const LN_RuntimeTree::RuntimeFallbackReport &fallback_report =
              item.runtime_tree->GetRuntimeFallbackReport();
          profile_counters.runtime_fallback_reason_mask = fallback_report.reason_mask;
          profile_counters.runtime_expression_fallback_reason_mask =
              fallback_report.expression_reason_mask;
          profile_counters.runtime_system_fallback_reason_mask =
              fallback_report.system_reason_mask;
          profile_counters.runtime_system_fallback_count =
              fallback_report.system_fallback_count;
          logic_nodes_accumulate_runtime_profile_counts(profile_counters, *profile);
          profile->serial_tree_count++;
        }
      }
      m_commandBuffer.EndRecording();

      if (report_tick_timings) {
        const auto serial_end = std::chrono::steady_clock::now();
        profile->serial_record_ms = std::chrono::duration<double, std::milli>(serial_end -
                                                                              serial_start)
                                        .count();
        profile->command_list_count = m_commandBuffer.Size() == 0 ? 0u : 1u;
        profile->command_count = uint32_t(m_commandBuffer.Size());
        profile->scheduler_job_count = profile->command_list_count;
      }

      std::chrono::steady_clock::time_point flush_start;
      if (report_tick_timings) {
        flush_start = std::chrono::steady_clock::now();
      }
      m_commandBuffer.Flush();
      PollActionOperations();
      if (report_tick_timings) {
        const auto flush_end = std::chrono::steady_clock::now();
        profile->flush_ms = std::chrono::duration<double, std::milli>(flush_end - flush_start)
                                .count();
        const LN_CommandBuffer::CommandStreamStats &command_stats =
            m_commandBuffer.GetLastCommandStreamStats();
        logic_nodes_accumulate_command_stream_stats(command_stats, *profile);
        profile->hot_map_lookup_count = m_tickHotMapLookupCount.load(std::memory_order_relaxed);
        profile->event_indexed_lookup_count = m_eventBus.IndexedLookupCount();
        profile->event_fallback_scan_count = m_eventBus.FallbackScanCount();
        CM_Debug(LN_FormatTickProfileLine(*profile));
      }

      m_isTicking = false;
      m_objectNameCacheValid = false;
      m_hasSharedTickInput = false;
      m_sharedTickReadContext = LN_TickReadContext();
      m_profileTickMetricsEnabled = false;
      m_tickRunnableTreesScratch.clear();
      m_gameLoadPulseActive = false;
      m_gameLoadPulseSucceeded = false;
      m_playerJoinActive = false;
      RemoveDetachedRuntimeTrees();
      return;
    }

    std::vector<LN_CommandBuffer::RecordedCommandList> command_lists;
    if (report_tick_timings) {
      logic_nodes_profiled_reserve(command_lists,
                                   scheduler_stats.main_thread_tree_count +
                                       scheduler_stats.worker_tree_count +
                                       m_pendingRunOnce.size(),
                                   *profile);
    }
    else {
      command_lists.reserve(scheduler_stats.main_thread_tree_count +
                            scheduler_stats.worker_tree_count +
                            m_pendingRunOnce.size());
    }

    for (const LN_ParallelTreeExecutor::RuntimeTreeWorkItem &item :
         scheduler_plan.main_thread_items)
    {
      if (item.runtime_tree == nullptr) {
        continue;
      }
      std::chrono::steady_clock::time_point serial_start;
      if (report_tick_timings) {
        serial_start = std::chrono::steady_clock::now();
      }
      command_lists.push_back(logic_nodes_record_tree_commands(item.runtime_tree_index,
                                                               *item.runtime_tree,
                                                               context,
                                                               false,
                                                               m_mainThreadId,
                                                               report_tick_timings));
      if (report_tick_timings) {
        logic_nodes_accumulate_recorded_profile_counts(command_lists.back(), *profile);
        const auto serial_end = std::chrono::steady_clock::now();
        profile->serial_record_ms += std::chrono::duration<double, std::milli>(serial_end -
                                                                               serial_start)
                                         .count();
        profile->serial_tree_count++;
      }
    }
    if (report_tick_timings) {
      profile->main_thread_fallback_tree_count = scheduler_stats.main_thread_fallback_count;
    }

    std::vector<LN_ParallelTreeExecutor::RuntimeTreeWorkItem> worker_trees;
    if (scheduler_stats.worker_tree_count != 0u) {
      if (report_tick_timings) {
        logic_nodes_profiled_reserve(worker_trees, scheduler_stats.worker_tree_count, *profile);
      }
      else {
        worker_trees.reserve(scheduler_stats.worker_tree_count);
      }
      for (const LN_ParallelTreeExecutor::SchedulerBatch &batch :
           scheduler_plan.worker_batches)
      {
        worker_trees.insert(worker_trees.end(), batch.items.begin(), batch.items.end());
      }
    }

    if (!worker_trees.empty()) {
      if (!m_objectNameCacheValid && m_objectNameLookupUsed.load(std::memory_order_relaxed)) {
        RebuildObjectNameCache();
      }
      LN_ParallelTreeExecutor parallel_executor;
      std::chrono::steady_clock::time_point parallel_start;
      if (report_tick_timings) {
        parallel_start = std::chrono::steady_clock::now();
      }
      std::vector<LN_CommandBuffer::RecordedCommandList> parallel_command_lists =
          parallel_executor.ExecuteTreesToCommandLists(worker_trees,
                                                      context,
                                                      report_tick_timings);
      if (report_tick_timings) {
        const auto parallel_end = std::chrono::steady_clock::now();
        profile->parallel_record_ms = std::chrono::duration<double, std::milli>(parallel_end -
                                                                                parallel_start)
                                          .count();
        profile->parallel_tree_count = scheduler_stats.worker_tree_count;
        profile->parallel_executor_used = true;
        const LN_ParallelTreeExecutor::SchedulerPlannerStats &executed_stats =
            parallel_executor.GetLastPlannerStats();
        profile->scheduler_worker_batch_count = executed_stats.worker_batch_count;
        profile->scheduler_max_trees_per_worker_batch =
            executed_stats.max_trees_per_worker_batch;
        profile->scheduler_average_trees_per_worker_batch_x100 =
            executed_stats.average_trees_per_worker_batch_x100;
        for (const LN_CommandBuffer::RecordedCommandList &command_list : parallel_command_lists) {
          logic_nodes_accumulate_recorded_profile_counts(command_list, *profile);
        }
      }
      command_lists.insert(command_lists.end(),
                           std::make_move_iterator(parallel_command_lists.begin()),
                           std::make_move_iterator(parallel_command_lists.end()));
    }

    const size_t run_once_before = command_lists.size();
    std::chrono::steady_clock::time_point run_once_start;
    if (report_tick_timings) {
      run_once_start = std::chrono::steady_clock::now();
    }
    ProcessPendingRunOnce(command_lists, context, report_tick_timings);
    if (report_tick_timings) {
      const auto run_once_end = std::chrono::steady_clock::now();
      profile->run_once_record_ms = std::chrono::duration<double, std::milli>(run_once_end -
                                                                              run_once_start)
                                        .count();
      profile->run_once_count = uint32_t(command_lists.size() - run_once_before);
      for (size_t index = run_once_before; index < command_lists.size(); index++) {
        logic_nodes_accumulate_recorded_profile_counts(command_lists[index], *profile);
      }
      profile->scheduler_job_count = profile->scheduler_worker_batch_count +
                                     profile->scheduler_main_thread_batch_count +
                                     profile->run_once_count;
    }

    if (report_tick_timings) {
      profile->command_list_count = uint32_t(command_lists.size());
      profile->command_count = logic_nodes_count_commands(command_lists);
      profile->string_copy_count = logic_nodes_count_command_string_payloads(command_lists);
      for (const LN_CommandBuffer::RecordedCommandList &command_list : command_lists) {
        logic_nodes_accumulate_command_stream_stats(command_list, *profile);
      }
    }

    std::chrono::steady_clock::time_point merge_start;
    if (report_tick_timings) {
      merge_start = std::chrono::steady_clock::now();
    }
    m_commandBuffer.MergeRecordedCommandLists(std::move(command_lists));
    if (report_tick_timings) {
      const auto merge_end = std::chrono::steady_clock::now();
      profile->merge_ms = std::chrono::duration<double, std::milli>(merge_end - merge_start)
                              .count();
    }

    std::chrono::steady_clock::time_point flush_start;
    if (report_tick_timings) {
      flush_start = std::chrono::steady_clock::now();
    }
    m_commandBuffer.Flush();
    PollActionOperations();
    if (report_tick_timings) {
      const auto flush_end = std::chrono::steady_clock::now();
      profile->flush_ms = std::chrono::duration<double, std::milli>(flush_end - flush_start)
                              .count();
      const LN_CommandBuffer::CommandStreamStats &command_stats =
          m_commandBuffer.GetLastCommandStreamStats();
      profile->command_coalesced_count = command_stats.coalesced_command_count;
      profile->hot_map_lookup_count = m_tickHotMapLookupCount.load(std::memory_order_relaxed);
      profile->event_indexed_lookup_count = m_eventBus.IndexedLookupCount();
      profile->event_fallback_scan_count = m_eventBus.FallbackScanCount();
    }

    if (report_tick_timings) {
      CM_Debug(LN_FormatTickProfileLine(*profile));
    }
  }

  m_isTicking = false;
  m_objectNameCacheValid = false;
  m_hasSharedTickInput = false;
  m_sharedTickReadContext = LN_TickReadContext();
  m_profileTickMetricsEnabled = false;
  m_tickRunnableTreesScratch.clear();
  m_gameLoadPulseActive = false;
  m_gameLoadPulseSucceeded = false;
  m_playerJoinActive = false;
  RemoveDetachedRuntimeTrees();
}

void LN_Manager::Clear()
{
  AssertMainThread();

  m_commandBuffer.Clear();
  m_actionOperations.clear();
  m_uiService->Clear();
  InvalidatePointerHitCache();
  m_gameStateService.CancelPending();
  m_commandBuffer.ClearMaterialParameterDefaultState();
  m_programBySourceTreeName.clear();
  m_validatedPrograms.clear();
  m_cachedPrograms.clear();
  m_pendingRunOnce.clear();
  m_pendingItemDrops.clear();
  ClearPauseProcessingObjects();
  for (std::unique_ptr<LN_RuntimeTree> &runtime_tree : m_runtimeTrees) {
    if (runtime_tree != nullptr) {
      m_denseIds.InvalidateRuntimeTree(runtime_tree.get());
    }
  }
  m_runtimeTrees.clear();
  ClearRuntimeTreeIndexes();
  m_queuedRemovalObjects.clear();
  m_tickIndex = 0;
  m_updateTickIndex = 0;
  m_fixedTickIndex = 0;
  m_isTicking = false;
  m_hasDetachedRuntimeTrees = false;
  m_debugBindingsScanned = false;
  m_reportedVariableTimestepSkip = false;
  m_gameStateTransitionPending = false;
  m_gameLoadPulsePending = false;
  m_pendingGameLoadSucceeded = false;
  m_gameLoadPulseActive = false;
  m_gameLoadPulseSucceeded = false;
  m_playerJoinActive = false;
  m_joinsAtStart.clear();
  m_eventBus.ClearAll();
  m_inputSample = LN_InputSnapshot();
  m_pendingUpdateInput = LN_InputSnapshot();
  m_frameInput = LN_InputSnapshot();
  m_gameFrameInput = LN_InputSnapshot();
  m_pendingFixedInput = LN_InputSnapshot();
  m_pendingFixedRawInput = LN_InputSnapshot();
  m_inputConsumption.Clear();
  m_sharedTickReadContext = LN_TickReadContext();
  m_hasSharedTickInput = false;
  m_hasPendingUpdateInput = false;
  m_hasFrameInput = false;
  m_hasPendingFixedInput = false;
  m_hasPendingFixedRawInput = false;
  m_inputActions = {};
  if (m_inputRebind.active) {
    /* Its tree is gone: nothing to report to. */
    LN_InputMap::Get().EndCapture(UINT64_MAX);
  }
  m_inputRebind = InputRebind();
  ClearRigidBodyConstraintBreakEvents();
  ClearPendingRigidBodyConstraintBreakEvents();
  m_profileTickMetricsEnabled = false;
  m_tickHotMapLookupCount.store(0, std::memory_order_relaxed);
  m_denseIds.Clear();
}

void LN_Manager::InvalidatePointerHitCache()
{
  m_pointerHitCache.clear();
}

void LN_Manager::PushEvent(LN_EventEntry event)
{
  m_eventBus.Publish(std::move(event));
}

const LN_EventBus &LN_Manager::GetEventBus() const
{
  return m_eventBus;
}

LN_EventSubjectId LN_Manager::InternEventSubject(const std::string &subject)
{
  return m_denseIds.InternEventSubject(subject);
}

LN_GamePropertyId LN_Manager::InternGamePropertyName(const std::string &name)
{
  return m_denseIds.InternGameProperty(name);
}

LN_SoundId LN_Manager::InternSoundName(const std::string &name)
{
  return m_denseIds.InternSound(name);
}

LN_ObjectHandle LN_Manager::MakeObjectHandle(KX_GameObject *gameobj,
                                             const std::string &debug_name)
{
  return m_denseIds.MakeObjectHandle(gameobj, debug_name);
}

KX_GameObject *LN_Manager::ResolveObjectHandle(const LN_ObjectHandle &handle) const
{
  return static_cast<KX_GameObject *>(m_denseIds.ResolveHandle(handle));
}

LN_ObjectHandle LN_Manager::FindObjectHandle(const KX_GameObject *gameobj) const
{
  return m_denseIds.FindObjectHandle(gameobj);
}

LN_RuntimeRef LN_Manager::MakeSharedObjectRef(KX_GameObject *gameobj,
                                              const std::string &debug_name)
{
  LN_RuntimeRef runtime_ref;
  runtime_ref.kind = LN_RuntimeRefKind::Object;
  runtime_ref.debug_name = debug_name;
  const LN_ObjectHandle handle = m_denseIds.MakeObjectHandle(gameobj, debug_name);
  if (handle.IsValid()) {
    runtime_ref.slot = handle.index;
    runtime_ref.generation = handle.generation;
    runtime_ref.shared_domain = m_sharedRefDomain;
  }
  return runtime_ref;
}

KX_GameObject *LN_Manager::ResolveSharedObjectRef(const LN_RuntimeRef &runtime_ref) const
{
  if (runtime_ref.kind != LN_RuntimeRefKind::Object ||
      runtime_ref.shared_domain != m_sharedRefDomain)
  {
    return nullptr;
  }
  LN_ObjectHandle handle;
  handle.index = runtime_ref.slot;
  handle.generation = runtime_ref.generation;
  return ResolveObjectHandle(handle);
}

const LN_RigidBodyConstraintBreakEvent *LN_Manager::FindRigidBodyConstraintBreakEvent(
    KX_GameObject *owner,
    const std::string &constraint_name,
    const LN_RigidBodyConstraintMatchMode match_mode) const
{
  AssertMainThread();
  if (owner == nullptr || IsGameObjectQueuedForRemoval(owner))
  {
    return nullptr;
  }

  if (match_mode == LN_RigidBodyConstraintMatchMode::All) {
    return RigidBodyConstraintBreakEventAt(FirstRigidBodyConstraintBreakEventIndex(owner));
  }

  if (constraint_name.empty()) {
    return nullptr;
  }

  if (match_mode == LN_RigidBodyConstraintMatchMode::Contains) {
    for (uint32_t event_index = FirstRigidBodyConstraintBreakEventIndex(owner);
         event_index != LN_INVALID_INDEX;
         event_index = NextRigidBodyConstraintBreakEventIndex(event_index))
    {
      const LN_RigidBodyConstraintBreakEvent *event =
          RigidBodyConstraintBreakEventAt(event_index);
      if (event != nullptr &&
          event->constraint_name.find(constraint_name) != std::string::npos)
      {
        return event;
      }
    }
    return nullptr;
  }

  if (m_rigidBodyConstraintBreakEventLookup.empty()) {
    return nullptr;
  }
  const ConstraintBreakLookupKey key{owner,
                                     std::hash<std::string>{}(constraint_name)};
  const auto range = m_rigidBodyConstraintBreakEventLookup.equal_range(key);
  for (auto item = range.first; item != range.second; ++item) {
    const LN_RigidBodyConstraintBreakEvent *event = RigidBodyConstraintBreakEventAt(item->second);
    if (event != nullptr && event->constraint_name == constraint_name) {
      return event;
    }
  }
  return nullptr;
}

uint32_t LN_Manager::FirstRigidBodyConstraintBreakEventIndex(KX_GameObject *owner) const
{
  AssertMainThread();
  if (owner == nullptr || IsGameObjectQueuedForRemoval(owner)) {
    return LN_INVALID_INDEX;
  }
  const auto owner_events = m_rigidBodyConstraintBreakEventOwnerLookup.find(owner);
  return owner_events != m_rigidBodyConstraintBreakEventOwnerLookup.end() ?
             owner_events->second.first_event_index :
             LN_INVALID_INDEX;
}

uint32_t LN_Manager::NextRigidBodyConstraintBreakEventIndex(const uint32_t event_index) const
{
  AssertMainThread();
  return event_index < m_rigidBodyConstraintBreakEventNextByOwner.size() ?
             m_rigidBodyConstraintBreakEventNextByOwner[event_index] :
             LN_INVALID_INDEX;
}

uint32_t LN_Manager::RigidBodyConstraintBreakEventCount(KX_GameObject *owner) const
{
  AssertMainThread();
  if (owner == nullptr || IsGameObjectQueuedForRemoval(owner)) {
    return 0;
  }
  const auto owner_events = m_rigidBodyConstraintBreakEventOwnerLookup.find(owner);
  return owner_events != m_rigidBodyConstraintBreakEventOwnerLookup.end() ?
             owner_events->second.event_count :
             0;
}

const LN_RigidBodyConstraintBreakEvent *LN_Manager::RigidBodyConstraintBreakEventAt(
    const uint32_t event_index) const
{
  AssertMainThread();
  return event_index < m_rigidBodyConstraintBreakEvents.size() ?
             &m_rigidBodyConstraintBreakEvents[event_index] :
             nullptr;
}

const std::string &LN_Manager::DebugName(const LN_EventSubjectId id) const
{
  return m_denseIds.DebugName(id);
}

const std::string &LN_Manager::DebugName(const LN_GamePropertyId id) const
{
  return m_denseIds.DebugName(id);
}

const std::string &LN_Manager::DebugName(const LN_SoundId id) const
{
  return m_denseIds.DebugName(id);
}

LN_RuntimeTree *LN_Manager::FindRuntimeTree(KX_GameObject *gameobj,
                                            uint32_t applied_tree_index) const
{
  if (gameobj == nullptr) {
    return nullptr;
  }

  const RuntimeTreeLookupKey key{gameobj, applied_tree_index};
  if (const auto item = m_runtimeTreeByObjectAndAppliedIndex.find(key);
      item != m_runtimeTreeByObjectAndAppliedIndex.end())
  {
    LN_RuntimeTree *runtime_tree = item->second;
    if (runtime_tree != nullptr && runtime_tree->IsAttached() &&
        runtime_tree->OwnsGameObject(gameobj) &&
        runtime_tree->GetAppliedTreeIndex() == applied_tree_index)
    {
      return runtime_tree;
    }
  }
  return nullptr;
}

void LN_Manager::IndexRuntimeTree(LN_RuntimeTree *runtime_tree)
{
  if (runtime_tree == nullptr || !runtime_tree->IsAttached()) {
    return;
  }

  KX_GameObject *gameobj = runtime_tree->GetGameObject();
  if (gameobj == nullptr) {
    return;
  }

  m_runtimeTreeByObjectAndAppliedIndex[RuntimeTreeLookupKey{
      gameobj, runtime_tree->GetAppliedTreeIndex()}] = runtime_tree;

  std::vector<LN_RuntimeTree *> &trees = m_runtimeTreesByObject[gameobj];
  if (std::find(trees.begin(), trees.end(), runtime_tree) == trees.end()) {
    trees.push_back(runtime_tree);
  }
}

void LN_Manager::UnindexRuntimeTree(LN_RuntimeTree *runtime_tree)
{
  if (runtime_tree == nullptr) {
    return;
  }

  std::erase_if(m_actionOperations,
                [runtime_tree](const ActionOperation &operation) {
                  return operation.runtime_tree == runtime_tree;
                });

  m_uiService->NotifyRuntimeTreeRemoved(runtime_tree);

  if (KX_GameObject *gameobj = runtime_tree->GetGameObject()) {
    m_runtimeTreeByObjectAndAppliedIndex.erase(
        RuntimeTreeLookupKey{gameobj, runtime_tree->GetAppliedTreeIndex()});
    if (auto item = m_runtimeTreesByObject.find(gameobj); item != m_runtimeTreesByObject.end()) {
      std::vector<LN_RuntimeTree *> &trees = item->second;
      trees.erase(std::remove(trees.begin(), trees.end(), runtime_tree), trees.end());
      if (trees.empty()) {
        m_runtimeTreesByObject.erase(item);
      }
    }
    return;
  }

  for (auto item = m_runtimeTreeByObjectAndAppliedIndex.begin();
       item != m_runtimeTreeByObjectAndAppliedIndex.end();)
  {
    if (item->second == runtime_tree) {
      item = m_runtimeTreeByObjectAndAppliedIndex.erase(item);
    }
    else {
      ++item;
    }
  }

  for (auto item = m_runtimeTreesByObject.begin(); item != m_runtimeTreesByObject.end();) {
    std::vector<LN_RuntimeTree *> &trees = item->second;
    trees.erase(std::remove(trees.begin(), trees.end(), runtime_tree), trees.end());
    if (trees.empty()) {
      item = m_runtimeTreesByObject.erase(item);
    }
    else {
      ++item;
    }
  }
}

void LN_Manager::ClearRuntimeTreeIndexes()
{
  m_runtimeTreeByObjectAndAppliedIndex.clear();
  m_runtimeTreesByObject.clear();
  std::lock_guard<std::mutex> lock(m_runtimeObjectRefIndexMutex);
  m_runtimeTreesByReferencedObject.clear();
  m_referencedObjectsByRuntimeTree.clear();
}

void LN_Manager::RebuildRuntimeTreeIndexes()
{
  ClearRuntimeTreeIndexes();
  for (const std::unique_ptr<LN_RuntimeTree> &runtime_tree : m_runtimeTrees) {
    IndexRuntimeTree(runtime_tree.get());
    IndexRuntimeTreeObjectRefs(runtime_tree.get());
  }
}

void LN_Manager::IndexRuntimeTreeObjectRefs(LN_RuntimeTree *runtime_tree)
{
  if (runtime_tree == nullptr) {
    return;
  }

  for (const LN_RuntimeTree::ObjectRefSlot &slot : runtime_tree->m_objectRefs) {
    if (slot.object != nullptr) {
      RegisterRuntimeObjectRef(runtime_tree, slot.object);
    }
  }
}

void LN_Manager::RegisterRuntimeObjectRef(LN_RuntimeTree *runtime_tree,
                                          KX_GameObject *referenced_object)
{
  if (runtime_tree == nullptr || referenced_object == nullptr) {
    return;
  }

  std::lock_guard<std::mutex> lock(m_runtimeObjectRefIndexMutex);

  std::vector<LN_RuntimeTree *> &trees = m_runtimeTreesByReferencedObject[referenced_object];
  if (std::find(trees.begin(), trees.end(), runtime_tree) == trees.end()) {
    trees.push_back(runtime_tree);
  }

  std::vector<KX_GameObject *> &objects = m_referencedObjectsByRuntimeTree[runtime_tree];
  if (std::find(objects.begin(), objects.end(), referenced_object) == objects.end()) {
    objects.push_back(referenced_object);
  }
}

void LN_Manager::UnregisterRuntimeObjectRef(LN_RuntimeTree *runtime_tree,
                                            KX_GameObject *referenced_object)
{
  if (runtime_tree == nullptr || referenced_object == nullptr) {
    return;
  }

  std::lock_guard<std::mutex> lock(m_runtimeObjectRefIndexMutex);

  if (auto item = m_runtimeTreesByReferencedObject.find(referenced_object);
      item != m_runtimeTreesByReferencedObject.end())
  {
    std::vector<LN_RuntimeTree *> &trees = item->second;
    trees.erase(std::remove(trees.begin(), trees.end(), runtime_tree), trees.end());
    if (trees.empty()) {
      m_runtimeTreesByReferencedObject.erase(item);
    }
  }

  if (auto item = m_referencedObjectsByRuntimeTree.find(runtime_tree);
      item != m_referencedObjectsByRuntimeTree.end())
  {
    std::vector<KX_GameObject *> &objects = item->second;
    objects.erase(std::remove(objects.begin(), objects.end(), referenced_object), objects.end());
    if (objects.empty()) {
      m_referencedObjectsByRuntimeTree.erase(item);
    }
  }
}

void LN_Manager::UnregisterRuntimeTreeObjectRefs(LN_RuntimeTree *runtime_tree)
{
  if (runtime_tree == nullptr) {
    return;
  }

  std::lock_guard<std::mutex> lock(m_runtimeObjectRefIndexMutex);
  const auto item = m_referencedObjectsByRuntimeTree.find(runtime_tree);
  if (item == m_referencedObjectsByRuntimeTree.end()) {
    return;
  }

  std::vector<KX_GameObject *> referenced_objects = std::move(item->second);
  m_referencedObjectsByRuntimeTree.erase(item);
  for (KX_GameObject *referenced_object : referenced_objects) {
    if (auto object_item = m_runtimeTreesByReferencedObject.find(referenced_object);
        object_item != m_runtimeTreesByReferencedObject.end())
    {
      std::vector<LN_RuntimeTree *> &trees = object_item->second;
      trees.erase(std::remove(trees.begin(), trees.end(), runtime_tree), trees.end());
      if (trees.empty()) {
        m_runtimeTreesByReferencedObject.erase(object_item);
      }
    }
  }
}

void LN_Manager::CaptureSharedTickReadContext(const uint32_t input_channels,
                                              const double fixed_dt,
                                              const uint64_t tick_index,
                                              const LN_Event event)
{
  m_hasSharedTickInput = false;
  m_sharedTickReadContext = LN_TickReadContext();
  m_sharedTickReadContext.input_channels = input_channels;
  m_sharedTickReadContext.tick_index = tick_index;
  m_sharedTickReadContext.gravity = m_scene.GetGravity();
  m_sharedTickReadContext.has_gravity = true;

  if (KX_KetsjiEngine *engine = KX_GetActiveEngine()) {
    m_sharedTickReadContext.time_scale = float(engine->GetTimeScale());
    m_sharedTickReadContext.elapsed_time = float(engine->GetFrameTime());
    m_sharedTickReadContext.fps = float(engine->GetAverageFrameRate());
    m_sharedTickReadContext.frame_delta = fixed_dt >= 0.0 ? float(fixed_dt) :
                                                        (m_sharedTickReadContext.fps > 0.0f ?
                                                             (1.0f / m_sharedTickReadContext.fps) :
                                                             0.0f);
    m_sharedTickReadContext.delta_factor = m_sharedTickReadContext.frame_delta * 60.0f;
    m_sharedTickReadContext.has_timing = true;
  }
  if (input_channels == LN_DEP_INPUT_NONE) {
    return;
  }
  KX_KetsjiEngine *engine = KX_GetActiveEngine();
  const bool fixed = event == LN_Event::OnFixedUpdate;
  LN_InputSnapshot &input = fixed ? m_pendingFixedInput : m_frameInput;
  bool &has_input = fixed ? m_hasPendingFixedInput : m_hasFrameInput;
  if (!has_input) {
    input.Capture(engine ? engine->GetInputDevice() : nullptr, engine ? engine->GetCanvas() : nullptr);
    has_input = true;
    if (fixed) {
      m_inputConsumption.ApplyStrokesTo(m_pendingFixedInput);
    }
  }
  /* Frame logic reads the frame input without what the UI consumed; the UI reads it whole. */
  if (!fixed) {
    m_gameFrameInput.CopyFrom(m_frameInput);
    m_inputConsumption.ApplyToFrame(m_gameFrameInput);
  }
  m_sharedTickReadContext.input = fixed ? &m_pendingFixedInput : &m_gameFrameInput;
  m_sharedTickReadContext.raw_input = fixed ? (m_hasPendingFixedRawInput ? &m_pendingFixedRawInput :
                                                                           nullptr) :
                                              &m_frameInput;
  m_sharedTickReadContext.has_input = true;
  m_hasSharedTickInput = true;
}

/** The engine's input device generation, or UINT64_MAX without an engine. */
static uint64_t logic_nodes_input_revision()
{
  KX_KetsjiEngine *engine = KX_GetActiveEngine();
  SCA_IInputDevice *device = engine != nullptr ? engine->GetInputDevice() : nullptr;
  return device != nullptr ? device->GetClearRevision() : UINT64_MAX;
}

const LN_InputSnapshot *LN_Manager::UIFrameInput() const
{
  if (LN_InputMap::Get().BlocksUI(logic_nodes_input_revision())) {
    return nullptr;
  }
  return m_hasSharedTickInput && m_hasFrameInput ? &m_frameInput : nullptr;
}

void LN_Manager::UpdateInputActions(const LN_Event event,
                                    const uint32_t input_channels,
                                    const uint64_t phase_tick_index)
{
  if ((input_channels & LN_DEP_INPUT_ACTION) == 0u || !m_sharedTickReadContext.has_input ||
      m_sharedTickReadContext.input == nullptr)
  {
    return;
  }
  const size_t phase = event == LN_Event::OnFixedUpdate ? 2 : 0;
  const LN_InputContexts contexts = InputContexts();
  m_inputActions[phase].Update(*m_sharedTickReadContext.input, contexts, phase_tick_index);
  m_sharedTickReadContext.input_actions = &m_inputActions[phase];
  if ((input_channels & LN_DEP_INPUT_RAW) != 0u && m_sharedTickReadContext.raw_input != nullptr) {
    m_inputActions[phase + 1].Update(
        *m_sharedTickReadContext.raw_input, contexts, phase_tick_index);
    m_sharedTickReadContext.raw_input_actions = &m_inputActions[phase + 1];
  }
}

LN_InputContexts LN_Manager::InputContexts() const
{
  LN_InputContexts contexts;
  for (int32_t user = 0; user < LN_MAX_GAMEPADS; ++user) {
    contexts[size_t(user)] = m_uiService->GetActiveInputContext(user);
  }
  return contexts;
}

void LN_Manager::RefreshInputActions()
{
  /* The frame's actions without the presses the UI took meanwhile. */
  if (m_sharedTickReadContext.input_actions == &m_inputActions[0]) {
    m_inputActions[0].Refresh(*m_sharedTickReadContext.input, InputContexts());
  }
}

void LN_Manager::ConsumeNewPresses(const LN_InputSnapshot &sample)
{
  for (int32_t code = SCA_IInputDevice::BEGINKEY + 1; code < SCA_IInputDevice::ENDMOUSEBUTTONS;
       ++code)
  {
    if (sample.GetStatus(code, SCA_InputEvent::JUSTACTIVATED)) {
      ConsumeInputStroke(code);
    }
  }
  if (sample.GetMouse().wheel_delta != 0) {
    ConsumeWheel();
  }
  for (int32_t gamepad_index = 0; gamepad_index < LN_MAX_GAMEPADS; ++gamepad_index) {
    const LN_GamepadSnapshot *gamepad = sample.GetGamepad(gamepad_index);
    if (gamepad == nullptr || !gamepad->connected) {
      continue;
    }
    for (int32_t button = 0; button < LN_MAX_GAMEPAD_BUTTONS; ++button) {
      if (gamepad->pressed[size_t(button)] != 0) {
        ConsumeGamepadPress(gamepad_index, button);
      }
    }
  }
}

void LN_Manager::StartInputRebind(LN_RuntimeTree *runtime_tree,
                                  const uint32_t completion_expression_index,
                                  const uint32_t source_ref_index,
                                  const int32_t action,
                                  const int32_t user,
                                  const LN_InputDeviceClass device_class,
                                  const int direction,
                                  const double timeout)
{
  AssertMainThread();
  if (runtime_tree == nullptr) {
    return;
  }
  if (action < 0 || action >= int32_t(LN_InputMap::Get().Size()) || user < 0 ||
      user >= LN_MAX_GAMEPADS ||
      (device_class != LN_InputDeviceClass::Gamepad && user != 0))
  {
    runtime_tree->CompletePersistenceOperation(completion_expression_index, false);
    return;
  }
  if (m_inputRebind.active) {
    /* A flow that keeps firing does not restart the wait it started. */
    if (m_denseIds.ResolveRuntimeTreeId(m_inputRebind.target.runtime_tree_id) == runtime_tree &&
        m_inputRebind.target.program_revision == runtime_tree->GetProgramRevision() &&
        m_inputRebind.target.expression_index == completion_expression_index)
    {
      return;
    }
    FinishInputRebind(false);
  }
  m_inputRebind = InputRebind();
  m_inputRebind.target.runtime_tree_id = runtime_tree->GetDenseRuntimeTreeId();
  m_inputRebind.target.program_revision = runtime_tree->GetProgramRevision();
  m_inputRebind.target.expression_index = completion_expression_index;
  m_inputRebind.target.source_ref_index = source_ref_index;
  m_inputRebind.action = action;
  m_inputRebind.user = user;
  m_inputRebind.device_class = device_class == LN_InputDeviceClass::Gamepad ?
                                   LN_InputDeviceClass::Gamepad :
                                   LN_InputDeviceClass::KeyboardMouse;
  m_inputRebind.direction = uint8_t(std::clamp(direction, 0, 3));
  m_inputRebind.remaining = timeout;
  m_inputRebind.active = true;
  LN_InputMap::Get().BeginCapture();
}

void LN_Manager::ProcessInputRebind(const double unscaled_delta_time)
{
  if (!m_inputRebind.active) {
    return;
  }
  const LN_RuntimeTree *runtime_tree = static_cast<const LN_RuntimeTree *>(
      m_denseIds.ResolveRuntimeTreeId(m_inputRebind.target.runtime_tree_id));
  if (runtime_tree == nullptr ||
      runtime_tree->GetProgramRevision() != m_inputRebind.target.program_revision)
  {
    m_inputRebind.active = false;
    LN_InputMap::Get().EndCapture(logic_nodes_input_revision());
    return;
  }
  const auto cancel = [&]() {
    /* Esc only cancels: it must not also close a menu or end the game. */
    if (SCA_IInputDevice *device = m_frameInput.GetDevice()) {
      device->ConsumePressTransition(SCA_IInputDevice::ESCKEY);
    }
    ConsumeInputStroke(SCA_IInputDevice::ESCKEY);
    FinishInputRebind(false);
  };
  if (m_inputRebind.skip_frame) {
    m_inputRebind.skip_frame = false;
    if (m_hasFrameInput &&
        m_frameInput.GetStatus(SCA_IInputDevice::ESCKEY, SCA_InputEvent::JUSTACTIVATED))
    {
      cancel();
    }
    return;
  }
  if (m_inputRebind.remaining > 0.0) {
    m_inputRebind.remaining -= unscaled_delta_time;
    if (m_inputRebind.remaining <= 0.0) {
      FinishInputRebind(false);
      return;
    }
  }
  if (!m_hasFrameInput) {
    return;
  }
  LN_InputMap &map = LN_InputMap::Get();
  const bool vector = map.Action(m_inputRebind.action).vector;
  LN_InputBinding binding;
  switch (m_inputRebind.capture.Update(
      m_frameInput, m_inputRebind.user, m_inputRebind.device_class, vector, binding))
  {
    case LN_InputCapture::Result::Waiting:
      return;
    case LN_InputCapture::Result::Cancelled:
      cancel();
      return;
    case LN_InputCapture::Result::Captured:
      break;
  }
  /* The press is the new binding: game logic must not react to it as well. */
  if (binding.device == LN_InputDevice::Gamepad) {
    if (binding.code <= GAME_INPUT_GAMEPAD_DPAD_RIGHT) {
      ConsumeGamepadPress(LN_InputMap::Get().GamepadOfUser(m_inputRebind.user), binding.code);
    }
  }
  else if (binding.input == SCA_IInputDevice::WHEELUPMOUSE ||
           binding.input == SCA_IInputDevice::WHEELDOWNMOUSE)
  {
    ConsumeWheel();
  }
  else {
    ConsumeInputStroke(binding.input);
  }
  /* A vector action's whole stick has its own direction; anything else pushes the chosen way. */
  if (vector && binding.code != GAME_INPUT_GAMEPAD_LEFT_STICK &&
      binding.code != GAME_INPUT_GAMEPAD_RIGHT_STICK)
  {
    binding.direction = m_inputRebind.direction;
  }
  map.Rebind(m_inputRebind.action, binding);
  FinishInputRebind(true);
}

void LN_Manager::FinishInputRebind(const bool success)
{
  if (!m_inputRebind.active) {
    return;
  }
  m_inputRebind.active = false;
  LN_InputMap::Get().EndCapture(logic_nodes_input_revision());
  LN_RuntimeTree *runtime_tree = static_cast<LN_RuntimeTree *>(
      m_denseIds.ResolveRuntimeTreeId(m_inputRebind.target.runtime_tree_id));
  if (runtime_tree != nullptr &&
      runtime_tree->GetProgramRevision() == m_inputRebind.target.program_revision)
  {
    runtime_tree->CompletePersistenceOperation(m_inputRebind.target.expression_index, success);
  }
}

void LN_Manager::RebuildObjectNameCache()
{
  m_objectByName.clear();
  if (EXP_ListValue<KX_GameObject> *object_list = m_scene.GetObjectList()) {
    for (KX_GameObject *game_object : *object_list) {
      if (game_object == nullptr) {
        continue;
      }
      m_objectByName.emplace(game_object->GetName(), game_object);
    }
  }
  m_objectNameCacheValid = true;
}

KX_GameObject *LN_Manager::FindObjectByName(const std::string &name)
{
  if (!m_isTicking || name.empty()) {
    return nullptr;
  }
  m_objectNameLookupUsed.store(true, std::memory_order_relaxed);
  if (!m_objectNameCacheValid) {
    if (std::this_thread::get_id() != m_mainThreadId) {
      /* Worker trees must not rebuild the shared cache (see m_objectNameLookupUsed). */
      if (EXP_ListValue<KX_GameObject> *object_list = m_scene.GetObjectList()) {
        for (KX_GameObject *game_object : *object_list) {
          if (game_object != nullptr && game_object->GetName() == name) {
            return game_object;
          }
        }
      }
      return nullptr;
    }
    RebuildObjectNameCache();
  }
  if (m_profileTickMetricsEnabled) {
    m_tickHotMapLookupCount.fetch_add(1, std::memory_order_relaxed);
  }
  const auto iter = m_objectByName.find(name);
  return iter != m_objectByName.end() ? iter->second : nullptr;
}

void LN_Manager::RegisterDebugPropertyBindings()
{
  AssertMainThread();
  BLI_assert(!m_debugBindingsScanned);

  m_debugBindingsScanned = true;

  for (KX_GameObject *gameobj : *m_scene.GetObjectList()) {
    if (gameobj == nullptr) {
      continue;
    }

    if (IsGameObjectQueuedForRemoval(gameobj)) {
      continue;
    }

    if (gameobj->GetPropertyNumber(LN_DEBUG_SET_WORLD_POSITION, 0.0f) == 0.0f) {
      continue;
    }

    const MT_Vector3 current_position = gameobj->NodeGetWorldPosition();
    const MT_Vector3 target_position(
        gameobj->GetPropertyNumber(LN_DEBUG_SET_WORLD_POSITION_X, current_position.x()),
        gameobj->GetPropertyNumber(LN_DEBUG_SET_WORLD_POSITION_Y, current_position.y()),
        gameobj->GetPropertyNumber(LN_DEBUG_SET_WORLD_POSITION_Z, current_position.z()));
    RegisterDebugHardcodedSetWorldPosition(gameobj, target_position);
  }
}

void LN_Manager::DetachRuntimeTreesForGameObject(KX_GameObject *gameobj)
{
  if (gameobj == nullptr) {
    return;
  }

  std::vector<LN_RuntimeTree *> trees;
  if (const auto item = m_runtimeTreesByObject.find(gameobj); item != m_runtimeTreesByObject.end()) {
    trees = item->second;
  }
  else {
    trees.reserve(m_runtimeTrees.size());
    for (const std::unique_ptr<LN_RuntimeTree> &runtime_tree : m_runtimeTrees) {
      if (runtime_tree != nullptr && runtime_tree->OwnsGameObject(gameobj)) {
        trees.push_back(runtime_tree.get());
      }
    }
  }

  for (LN_RuntimeTree *runtime_tree : trees) {
    if (runtime_tree != nullptr && runtime_tree->IsAttached() &&
        runtime_tree->OwnsGameObject(gameobj))
    {
      UnindexRuntimeTree(runtime_tree);
      runtime_tree->DetachGameObject();
      m_hasDetachedRuntimeTrees = true;
    }
  }
}

void LN_Manager::RemoveDetachedRuntimeTrees()
{
  BLI_assert(!m_isTicking);

  if (!m_hasDetachedRuntimeTrees) {
    return;
  }

  for (const std::unique_ptr<LN_RuntimeTree> &runtime_tree : m_runtimeTrees) {
    if (runtime_tree != nullptr && !runtime_tree->IsAttached()) {
      UnindexRuntimeTree(runtime_tree.get());
      UnregisterRuntimeTreeObjectRefs(runtime_tree.get());
      m_denseIds.InvalidateRuntimeTree(runtime_tree.get());
    }
  }
  m_runtimeTrees.erase(std::remove_if(m_runtimeTrees.begin(),
                                      m_runtimeTrees.end(),
                                      [](const std::unique_ptr<LN_RuntimeTree> &runtime_tree) {
                                        return !runtime_tree->IsAttached();
                                      }),
                       m_runtimeTrees.end());
  m_hasDetachedRuntimeTrees = false;
}

void LN_Manager::AddQueuedRemovalObject(KX_GameObject *gameobj)
{
  if (gameobj == nullptr || IsGameObjectQueuedForRemoval(gameobj)) {
    return;
  }

  m_queuedRemovalObjects.push_back(gameobj);
}

void LN_Manager::RemoveQueuedRemovalObject(KX_GameObject *gameobj)
{
  if (gameobj == nullptr || m_queuedRemovalObjects.empty()) {
    return;
  }

  m_queuedRemovalObjects.erase(std::remove(m_queuedRemovalObjects.begin(),
                                           m_queuedRemovalObjects.end(),
                                           gameobj),
                               m_queuedRemovalObjects.end());
}

bool LN_Manager::IsGameObjectQueuedForRemoval(KX_GameObject *gameobj) const
{
  return gameobj != nullptr &&
         std::find(m_queuedRemovalObjects.begin(), m_queuedRemovalObjects.end(), gameobj) !=
             m_queuedRemovalObjects.end();
}

int LN_Manager::FindSceneObjectIndex(KX_GameObject *gameobj, bool *r_runtime_active) const
{
  if (r_runtime_active != nullptr) {
    *r_runtime_active = false;
  }

  if (gameobj == nullptr) {
    return -1;
  }

  EXP_ListValue<KX_GameObject> *objectlist = m_scene.GetObjectList();
  if (objectlist != nullptr) {
    for (int i = 0; i < objectlist->GetCount(); i++) {
      if (objectlist->GetValue(i) == gameobj) {
        if (r_runtime_active != nullptr) {
          *r_runtime_active = true;
        }
        return i;
      }
    }
  }

  EXP_ListValue<KX_GameObject> *inactivelist = m_scene.GetInactiveList();
  if (inactivelist != nullptr) {
    const int active_count = objectlist ? objectlist->GetCount() : 0;
    for (int i = 0; i < inactivelist->GetCount(); i++) {
      if (inactivelist->GetValue(i) == gameobj) {
        return active_count + i;
      }
    }
  }
  return -1;
}

void LN_Manager::CacheCompiledProgram(const std::shared_ptr<const LN_Program> &program)
{
  if (program == nullptr) {
    return;
  }
  const std::string &name = program->GetSourceTreeName();
  if (m_cachedPrograms.find(program.get()) != m_cachedPrograms.end()) {
    if (!name.empty()) {
      m_programBySourceTreeName[name] = program;
    }
    return;
  }
  if (!program->IsCurrentRuntimeCompatible()) {
    CM_Warning("Logic Nodes: rejected stale compiled program '" << program->GetName() << "'");
    return;
  }
  if (m_validatedPrograms.find(program.get()) == m_validatedPrograms.end()) {
    std::vector<std::string> payload_errors;
    if (!program->ValidateInstructionPayloads(&payload_errors)) {
      CM_Warning("Logic Nodes: rejected invalid compiled program '"
                 << program->GetName() << "': " << payload_errors.front());
      return;
    }
    m_validatedPrograms.insert(program.get());
  }
  for (const std::string &value : program->GetStringTable()) {
    m_denseIds.InternString(value);
    m_denseIds.InternEventSubject(value);
  }
  for (const LN_GamePropertyRef &property_ref : program->GetGamePropertyRefs()) {
    m_denseIds.InternGameProperty(property_ref.name);
  }
  for (const LN_TreePropertyRef &property_ref : program->GetTreePropertyRefs()) {
    m_denseIds.InternTreeProperty(property_ref.name);
  }
  m_cachedPrograms.insert(program.get());
  if (name.empty()) {
    return;
  }
  m_programBySourceTreeName[name] = program;
}

std::shared_ptr<const LN_Program> LN_Manager::EnsureCompiledProgram(const std::string &tree_name)
{
  return EnsureCompiledProgramImpl(tree_name, nullptr);
}

std::shared_ptr<const LN_Program> LN_Manager::EnsureCompiledProgram(
    const std::string &tree_name, const std::string &library_path)
{
  return EnsureCompiledProgramImpl(tree_name, &library_path);
}

std::shared_ptr<const LN_Program> LN_Manager::EnsureCompiledProgramImpl(
    const std::string &tree_name, const std::string *library_path)
{
  AssertMainThread();

  if (tree_name.empty()) {
    return nullptr;
  }

  KX_KetsjiEngine *engine = KX_GetActiveEngine();
  blender::bContext *ctx = engine ? engine->GetContext() : nullptr;
  blender::Main *bmain = ctx ? blender::CTX_data_main(ctx) : nullptr;
  if (bmain == nullptr) {
    CM_Warning("Logic Nodes: cannot compile tree '" << tree_name << "': no Blender Main context");
    return nullptr;
  }

  blender::bNodeTree *node_tree = nullptr;
  auto is_logic_node_tree = [](const blender::bNodeTree &tree) {
    return tree.type == blender::NTREE_LOGIC || std::strcmp(tree.idname, "LogicNodeTree") == 0;
  };

  for (blender::bNodeTree &tree : bmain->nodetrees) {
    if (!is_logic_node_tree(tree)) {
      continue;
    }
    const std::string current_library_path = tree.id.lib ? tree.id.lib->filepath : "";
    if (tree_name == std::string(tree.id.name + 2) &&
        (library_path == nullptr || *library_path == current_library_path))
    {
      if (node_tree != nullptr) {
        CM_Warning("Logic Nodes: tree identity '" << tree_name
                                                  << "' resolves to multiple datablocks");
        return nullptr;
      }
      node_tree = &tree;
    }
  }

  if (node_tree == nullptr) {
    CM_Warning("Logic Nodes: missing LogicNodeTree '" << tree_name << "'");
    return nullptr;
  }

  const std::string source_tree_library_path = node_tree->id.lib ?
                                                   node_tree->id.lib->filepath :
                                                   "";
  const std::string source_checksum = LN_TreeCompiler::BuildSourceChecksum(*node_tree);
  const auto cached = m_programBySourceTreeName.find(tree_name);
  if (cached != m_programBySourceTreeName.end()) {
    if (cached->second != nullptr &&
        cached->second->MatchesSource(tree_name, source_tree_library_path, source_checksum))
    {
      return cached->second;
    }
    m_programBySourceTreeName.erase(cached);
  }

  LN_TreeCompiler compiler;
  std::shared_ptr<LN_Program> program = compiler.Compile(*node_tree);
  if (program == nullptr) {
    return nullptr;
  }

  if (program->GetCompileReport().HasErrors()) {
    CM_Warning("Logic Nodes: compile errors for tree '" << tree_name << "'");
    return nullptr;
  }

  if (!program->IsCurrentRuntimeCompatible()) {
    CM_Warning("Logic Nodes: rejected stale compiled program for tree '" << tree_name << "'");
    return nullptr;
  }

  std::vector<std::string> payload_errors;
  if (!program->ValidateInstructionPayloads(&payload_errors)) {
    CM_Warning("Logic Nodes: rejected invalid compiled program for tree '"
               << tree_name << "': " << payload_errors.front());
    return nullptr;
  }

  CacheCompiledProgram(program);
  return program;
}

LN_RuntimeTree *LN_Manager::FindRuntimeTreeBySourceName(KX_GameObject *gameobj,
                                                       const std::string &tree_name) const
{
  if (gameobj == nullptr) {
    return nullptr;
  }

  if (const auto item = m_runtimeTreesByObject.find(gameobj); item != m_runtimeTreesByObject.end()) {
    for (LN_RuntimeTree *runtime_tree : item->second) {
      if (runtime_tree == nullptr || !runtime_tree->IsAttached() ||
          !runtime_tree->OwnsGameObject(gameobj))
      {
        continue;
      }
      std::shared_ptr<const LN_Program> program = runtime_tree->GetProgram();
      if (program && program->GetSourceTreeName() == tree_name) {
        return runtime_tree;
      }
    }
    return nullptr;
  }

  for (const std::unique_ptr<LN_RuntimeTree> &runtime_tree : m_runtimeTrees) {
    if (runtime_tree == nullptr || !runtime_tree->IsAttached() ||
        !runtime_tree->OwnsGameObject(gameobj))
    {
      continue;
    }
    std::shared_ptr<const LN_Program> program = runtime_tree->GetProgram();
    if (program && program->GetSourceTreeName() == tree_name) {
      return runtime_tree.get();
    }
  }
  return nullptr;
}

uint32_t LN_Manager::AllocateNextAppliedTreeIndex(KX_GameObject *gameobj) const
{
  uint32_t max_index = 0;
  bool any = false;
  if (const auto item = m_runtimeTreesByObject.find(gameobj); item != m_runtimeTreesByObject.end()) {
    for (LN_RuntimeTree *runtime_tree : item->second) {
      if (runtime_tree != nullptr && runtime_tree->IsAttached() &&
          runtime_tree->OwnsGameObject(gameobj))
      {
        any = true;
        max_index = std::max(max_index, runtime_tree->GetAppliedTreeIndex());
      }
    }
    return any ? (max_index + 1) : 0;
  }

  for (const std::unique_ptr<LN_RuntimeTree> &runtime_tree : m_runtimeTrees) {
    if (runtime_tree != nullptr && runtime_tree->IsAttached() &&
        runtime_tree->OwnsGameObject(gameobj))
    {
      any = true;
      max_index = std::max(max_index, runtime_tree->GetAppliedTreeIndex());
    }
  }
  return any ? (max_index + 1) : 0;
}

bool LN_Manager::IsLogicTreeRunning(KX_GameObject *gameobj, const std::string &tree_name) const
{
  LN_RuntimeTree *runtime_tree = FindRuntimeTreeBySourceName(gameobj, tree_name);
  if (runtime_tree == nullptr || !runtime_tree->IsAttached()) {
    return false;
  }
  if (!runtime_tree->IsRuntimeActive() || !runtime_tree->IsEnabled()) {
    return false;
  }
  KX_GameObject *owner = runtime_tree->GetGameObject();
  return owner != nullptr && !owner->IsLogicSuspended();
}

void LN_Manager::QueueRunLogicTreeOnce(KX_GameObject *target_object, const std::string &tree_name)
{
  AssertMainThread();

  if (target_object == nullptr || tree_name.empty() ||
      IsGameObjectQueuedForRemoval(target_object))
  {
    return;
  }
  m_pendingRunOnce.push_back({target_object, tree_name});
}

void LN_Manager::ProcessPendingRunOnce(
    std::vector<LN_CommandBuffer::RecordedCommandList> &command_lists,
    const LN_TickContext &context,
    const bool collect_profile_counters)
{
  /* Nested Run Logic Tree calls append here during forced execution; cap total pops per tick. */
  constexpr int kMaxPops = 128;
  std::unordered_set<uint64_t> active_stack;
  std::deque<std::pair<KX_GameObject *, std::string>> deferred_while_paused;
  size_t run_once_index = m_runtimeTrees.size();

  const auto run_key = [](KX_GameObject *go, const std::string &name) -> uint64_t {
    const uintptr_t p = reinterpret_cast<uintptr_t>(go);
    uint64_t h = std::hash<std::string>{}(name);
    h ^= uint64_t(p) << 1;
    return h;
  };

  for (int iter = 0; iter < kMaxPops && !m_pendingRunOnce.empty(); iter++) {
    std::pair<KX_GameObject *, std::string> item = std::move(m_pendingRunOnce.front());
    m_pendingRunOnce.pop_front();

    KX_GameObject *target = item.first;
    const std::string &tree_name = item.second;
    if (target == nullptr || tree_name.empty() || IsGameObjectQueuedForRemoval(target)) {
      continue;
    }
    if (context.game_paused && !IsObjectPauseProcessed(target)) {
      deferred_while_paused.push_back(std::move(item));
      continue;
    }

    const uint64_t key = run_key(target, tree_name);
    if (active_stack.find(key) != active_stack.end()) {
      CM_Warning(
          "Logic Nodes: skipped nested Run Logic Tree for the same object/tree within one tick");
      continue;
    }

    active_stack.insert(key);
    if (LN_RuntimeTree *target_tree = FindRuntimeTreeBySourceName(target, tree_name)) {
      command_lists.push_back(logic_nodes_record_tree_commands(run_once_index,
                                                               *target_tree,
                                                               context,
                                                               true,
                                                               m_mainThreadId,
                                                               collect_profile_counters));
    }
    active_stack.erase(key);
    run_once_index++;
  }

  while (!deferred_while_paused.empty()) {
    m_pendingRunOnce.push_front(std::move(deferred_while_paused.back()));
    deferred_while_paused.pop_back();
  }
}

bool LN_Manager::FlushSetLogicTreeEnabled(KX_GameObject *target_object,
                                         const std::string &tree_name,
                                         const bool enabled)
{
  if (target_object == nullptr || tree_name.empty() ||
      IsGameObjectQueuedForRemoval(target_object))
  {
    return true;
  }
  LN_RuntimeTree *runtime_tree = FindRuntimeTreeBySourceName(target_object, tree_name);
  if (runtime_tree == nullptr) {
    return false;
  }
  runtime_tree->SetEnabled(enabled);
  return true;
}

void LN_Manager::FlushInstallLogicTree(KX_GameObject *target_object,
                                      const std::string &tree_name,
                                      const bool initial_enabled)
{
  if (target_object == nullptr || tree_name.empty() ||
      IsGameObjectQueuedForRemoval(target_object))
  {
    return;
  }

  if (FindRuntimeTreeBySourceName(target_object, tree_name) != nullptr) {
    return;
  }

  std::shared_ptr<const LN_Program> program = EnsureCompiledProgram(tree_name);
  if (program == nullptr) {
    return;
  }

  bool runtime_active = false;
  const int scene_object_index = FindSceneObjectIndex(target_object, &runtime_active);
  if (scene_object_index < 0) {
    return;
  }

  RegisterCompiledProgram(target_object,
                          program,
                          uint32_t(scene_object_index),
                          AllocateNextAppliedTreeIndex(target_object),
                          initial_enabled,
                          runtime_active);
}

bool LN_Manager::FlushSaveGame(LN_RuntimeTree *runtime_tree,
                               const int32_t slot,
                               const std::string &path,
                               const uint32_t completion_expression_index,
                               const uint32_t source_ref_index,
                               std::string &r_error)
{
  AssertMainThread();
  if (runtime_tree == nullptr || !runtime_tree->IsAttached() ||
      m_denseIds.ResolveRuntimeTreeId(runtime_tree->GetDenseRuntimeTreeId()) != runtime_tree)
  {
    r_error = "owning runtime logic tree is no longer active";
    return false;
  }
  if (m_gameStateTransitionPending) {
    r_error = "Save Game is unavailable while this scene is being replaced by Load Game";
    runtime_tree->CompletePersistenceOperation(completion_expression_index, false);
    return false;
  }
  LN_GameStateCompletionTarget target;
  target.runtime_tree_id = runtime_tree->GetDenseRuntimeTreeId();
  target.program_revision = runtime_tree->GetProgramRevision();
  target.expression_index = completion_expression_index;
  target.source_ref_index = source_ref_index;
  const bool queued = m_gameStateService.RequestSave(m_scene, slot, path, target, r_error);
  if (!queued) {
    runtime_tree->CompletePersistenceOperation(completion_expression_index, false);
  }
  return queued;
}

bool LN_Manager::FlushLoadGame(LN_RuntimeTree *runtime_tree,
                               const int32_t slot,
                               const std::string &path,
                               const uint32_t completion_expression_index,
                               const uint32_t source_ref_index,
                               std::string &r_error)
{
  AssertMainThread();
  if (runtime_tree == nullptr || !runtime_tree->IsAttached() ||
      m_denseIds.ResolveRuntimeTreeId(runtime_tree->GetDenseRuntimeTreeId()) != runtime_tree)
  {
    r_error = "owning runtime logic tree is no longer active";
    return false;
  }
  if (m_gameStateTransitionPending) {
    r_error = "Load Game is unavailable while this scene is already being replaced";
    runtime_tree->CompletePersistenceOperation(completion_expression_index, false);
    return false;
  }
  LN_GameStateCompletionTarget target;
  target.runtime_tree_id = runtime_tree->GetDenseRuntimeTreeId();
  target.program_revision = runtime_tree->GetProgramRevision();
  target.expression_index = completion_expression_index;
  target.source_ref_index = source_ref_index;
  const bool queued = m_gameStateService.RequestLoad(slot, path, target, r_error);
  if (!queued) {
    runtime_tree->CompletePersistenceOperation(completion_expression_index, false);
  }
  return queued;
}

LN_GameStateResult LN_Manager::ApplyTransferredGameState(
    const LN_GameStateLoadPayload &payload, std::string &r_error)
{
  AssertMainThread();
  return m_gameStateService.ApplyTransferredLoad(m_scene, payload, r_error);
}

void LN_Manager::CompleteTransferredGameStateFailure(
    const LN_GameStateCompletionTarget &target,
    const std::string &filepath,
    const std::string &error)
{
  m_gameStateTransitionPending = false;
  CompleteGameStateOperation(target,
                             LN_GameStateOperation::Load,
                             filepath,
                             LN_GameStateResult::InternalError,
                             error,
                             true);
}

void LN_Manager::NotifyGameLoadFinished(const bool succeeded)
{
  AssertMainThread();
  m_gameLoadPulsePending = true;
  m_pendingGameLoadSucceeded = succeeded;
  if (succeeded) {
    /* Drops asked for before the load belong to the state it replaced. */
    m_pendingItemDrops.clear();
  }
}

bool LN_Manager::InitializeGameStateRegistry(std::string &r_error)
{
  AssertMainThread();
  return LN_GameStateSucceeded(m_gameStateService.InitializeRegistry(m_scene, r_error));
}

bool LN_Manager::PreflightAddObjectPersistence(KX_GameObject *source,
                                               const float life_seconds,
                                               const bool full_copy,
                                               std::string &r_error) const
{
  if (source == nullptr) {
    r_error = "Add Object source is missing";
    return false;
  }
  return LN_GameStateSucceeded(
      m_gameStateService.PreflightAddObject(m_scene, *source, life_seconds, full_copy, r_error));
}

bool LN_Manager::RegisterAddedObjectPersistence(KX_GameObject *source,
                                                KX_GameObject *root,
                                                const std::vector<KX_GameObject *> &members,
                                                const float life_seconds,
                                                const bool full_copy,
                                                std::string &r_error)
{
  AssertMainThread();
  if (source == nullptr || root == nullptr) {
    r_error = "Add Object persistence registration is missing its source or root";
    return false;
  }
  return LN_GameStateSucceeded(m_gameStateService.RegisterAddedObject(
      m_scene, *source, *root, members, life_seconds, full_copy, r_error));
}

bool LN_Manager::SetTrackedSaveExistence(KX_GameObject *root,
                                         const bool enabled,
                                         std::string &r_error)
{
  AssertMainThread();
  if (m_gameStateTransitionPending) {
    r_error = "Set Save Object State is unavailable while this scene is being replaced";
    return false;
  }
  return LN_GameStateSucceeded(
      m_gameStateService.SetSaveExistence(m_scene, root, enabled, r_error));
}

void LN_Manager::NotifyGameStateObjectRemoved(KX_GameObject *object,
                                              const KX_ObjectRemovalReason reason)
{
  AssertMainThread();
  if (object != nullptr) {
    m_gameStateService.NotifyObjectRemoved(*object, reason);
  }
}

void LN_Manager::CompleteGameStateOperation(const LN_GameStateCompletionTarget &target,
                                            const LN_GameStateOperation operation,
                                            const std::string &filepath,
                                            const LN_GameStateResult result,
                                            const std::string &error,
                                            const bool report_failure)
{
  AssertMainThread();
  const bool success = LN_GameStateSucceeded(result);
  if (operation == LN_GameStateOperation::Load) {
    NotifyGameLoadFinished(success);
  }
  if (!success && report_failure) {
    const char *operation_name = operation == LN_GameStateOperation::Save ? "Save Game" :
                                                                            "Load Game";
    CM_Warning("Logic Nodes " << operation_name << " failed for '" << filepath << "': " << error
                              << " (source ref " << target.source_ref_index << ")");
  }
  LN_RuntimeTree *runtime_tree = static_cast<LN_RuntimeTree *>(
      m_denseIds.ResolveRuntimeTreeId(target.runtime_tree_id));
  if (runtime_tree != nullptr && runtime_tree->GetProgramRevision() == target.program_revision) {
    runtime_tree->CompletePersistenceOperation(target.expression_index, success);
  }
}

void LN_Manager::PollGameStateCompletions()
{
  AssertMainThread();
  m_gameStateService.Poll(
      m_scene,
      [this](const LN_GameStateCompletionTarget &target,
             const LN_GameStateOperation operation,
             const std::string &filepath,
             const LN_GameStateResult result,
             const std::string &error) {
        CompleteGameStateOperation(target, operation, filepath, result, error, true);
      },
      [this](const LN_GameStateCompletionTarget &target,
             const std::string &filepath,
             std::unique_ptr<LN_GameStateLoadPayload> payload,
             std::string &r_error) {
        KX_KetsjiEngine *engine = KX_GetActiveEngine();
        if (engine == nullptr) {
          r_error = "the active game engine is unavailable for checkpoint scene switching";
          return LN_GameStateResult::InternalError;
        }
        const LN_GameStateResult result = engine->RequestGameLoadSceneTransition(
            m_scene, target, filepath, std::move(payload), r_error);
        if (LN_GameStateSucceeded(result)) {
          m_gameStateTransitionPending = true;
        }
        return result;
      });
}

void LN_Manager::AssertMainThread() const
{
  BLI_assert(std::this_thread::get_id() == m_mainThreadId);
}
