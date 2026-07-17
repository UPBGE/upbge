/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_Manager.cpp
 *  \ingroup logicnodes
 */

#include "LN_Manager.h"

#include <algorithm>
#include <array>
#include <chrono>
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
#include "KX_GameObject.h"
#include "KX_Globals.h"
#include "KX_KetsjiEngine.h"
#include "KX_Scene.h"
#include "PHY_IPhysicsEnvironment.h"

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
      m_mainThreadId(std::this_thread::get_id())
{
  m_commandBuffer.SetMainThreadId(m_mainThreadId);
  m_commandBuffer.SetLogicManager(this);
}

LN_Manager::~LN_Manager()
{
  BeginShutdown();
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
                 << ", on_fixed_update="
                 << program->GetInstructionHeaders(LN_Event::OnFixedUpdate).size() << ")");
      return nullptr;
    }
    m_validatedPrograms.insert(program.get());
  }

  if (LN_RuntimeTree *runtime_tree = FindRuntimeTree(gameobj, applied_tree_index)) {
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

void LN_Manager::NotifyGameObjectQueuedForRemoval(KX_GameObject *gameobj)
{
  AssertMainThread();

  if (gameobj == nullptr) {
    return;
  }

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
  AssertMainThread();

  if (gameobj == nullptr) {
    return;
  }

  if (IsGameObjectQueuedForRemoval(gameobj)) {
    RemoveQueuedRemovalObject(gameobj);
    if (!m_isTicking) {
      RemoveDetachedRuntimeTrees();
    }
    return;
  }

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

void LN_Manager::AbsorbRuntimeTreesFrom(LN_Manager &other)
{
  AssertMainThread();
  other.AssertMainThread();

  other.ClearPendingRuntimeWork();
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
}

void LN_Manager::Tick(double current_time, double fixed_dt, bool use_fixed_timestep)
{
  AssertMainThread();

  if (!m_debugBindingsScanned && logic_nodes_enable_debug_property_bindings()) {
    RegisterDebugPropertyBindings();
  }
  else if (!m_debugBindingsScanned) {
    m_debugBindingsScanned = true;
  }

  m_profileTickMetricsEnabled = false;

  if (m_runtimeTrees.empty()) {
    if (PHY_IPhysicsEnvironment *physics_env = m_scene.GetPhysicsEnvironment()) {
      physics_env->SetLogicCollisionContactCacheEnabled(false);
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
  for (size_t index = 0; index < m_runtimeTrees.size(); index++) {
    std::unique_ptr<LN_RuntimeTree> &runtime_tree = m_runtimeTrees[index];
    if (runtime_tree->CanTick()) {
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
        if (!runtime_tree->IsRuntimeActive()) {
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

  if (PHY_IPhysicsEnvironment *physics_env = m_scene.GetPhysicsEnvironment()) {
    physics_env->SetLogicCollisionContactCacheEnabled(
        (shared_query_channels & LN_DEP_QUERY_COLLISION_CONTACT) != 0,
        (shared_query_channels & LN_DEP_QUERY_COLLISION_CONTACT_DETAILS) != 0);
  }

  LN_TickContext context;
  context.current_time = current_time;
  context.fixed_dt = fixed_dt;
  context.unscaled_dt = fixed_dt;
  if (KX_KetsjiEngine *engine = KX_GetActiveEngine()) {
    const double time_scale = engine->GetTimeScale();
    if (time_scale > 1.0e-8) {
      context.unscaled_dt = fixed_dt / time_scale;
    }
  }
  context.use_fixed_timestep = true;
  static const LN_RuntimeFeatureConfig feature_config = LN_DefaultRuntimeFeatureConfig();
  context.use_register_expression_evaluator = LN_RuntimeFeatureEnabled(
      feature_config, LN_RUNTIME_FEATURE_REGISTER_EXPRESSION_EVALUATOR);
  context.use_typed_command_streams = LN_RuntimeFeatureEnabled(
      feature_config, LN_RUNTIME_FEATURE_TYPED_COMMAND_STREAMS);
  context.tick_index = m_tickIndex++;
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
  /* Events queued by Send Event on the previous tick become shared read-only tick storage. */
  m_eventBus.BeginTick();
  if (report_tick_timings) {
    const auto event_delivery_end = std::chrono::steady_clock::now();
    profile->event_delivery_ms = std::chrono::duration<double, std::milli>(
                                     event_delivery_end - event_delivery_start)
                                     .count();
    profile->event_count = m_eventBus.TickEventCount();
  }
  CaptureSharedTickReadContext(shared_input_channels, fixed_dt, context.tick_index);
  for (std::unique_ptr<LN_RuntimeTree> &runtime_tree : m_runtimeTrees) {
    const bool force_snapshot =
        !pending_forced_trees.empty() &&
        pending_forced_trees.find(runtime_tree.get()) != pending_forced_trees.end();
    if (runtime_tree->ShouldCaptureSnapshot() ||
        (force_snapshot && runtime_tree->ShouldCaptureSnapshotForForcedUpdate()))
    {
      runtime_tree->CaptureSnapshot(&m_sharedTickReadContext,
                                    float(fixed_dt),
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
  RemoveDetachedRuntimeTrees();
}

void LN_Manager::Clear()
{
  AssertMainThread();

  m_commandBuffer.Clear();
  m_commandBuffer.ClearMaterialParameterDefaultState();
  m_programBySourceTreeName.clear();
  m_validatedPrograms.clear();
  m_cachedPrograms.clear();
  m_pendingRunOnce.clear();
  for (std::unique_ptr<LN_RuntimeTree> &runtime_tree : m_runtimeTrees) {
    if (runtime_tree != nullptr) {
      m_denseIds.InvalidateRuntimeTree(runtime_tree.get());
    }
  }
  m_runtimeTrees.clear();
  ClearRuntimeTreeIndexes();
  m_queuedRemovalObjects.clear();
  m_tickIndex = 0;
  m_isTicking = false;
  m_hasDetachedRuntimeTrees = false;
  m_debugBindingsScanned = false;
  m_reportedVariableTimestepSkip = false;
  m_eventBus.ClearAll();
  m_profileTickMetricsEnabled = false;
  m_tickHotMapLookupCount.store(0, std::memory_order_relaxed);
  m_denseIds.Clear();
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
                                              const uint64_t tick_index)
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
    if (input_channels != LN_DEP_INPUT_NONE) {
      m_sharedTickInput.Capture(engine->GetInputDevice(), engine->GetCanvas());
      m_sharedTickReadContext.input = &m_sharedTickInput;
      m_sharedTickReadContext.has_input = true;
      m_hasSharedTickInput = true;
    }
  }
  else if (input_channels != LN_DEP_INPUT_NONE) {
    m_sharedTickInput.Capture(nullptr, nullptr);
    m_sharedTickReadContext.input = &m_sharedTickInput;
    m_sharedTickReadContext.has_input = true;
    m_hasSharedTickInput = true;
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
  AssertMainThread();
  if (!m_objectNameCacheValid) {
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
    if (tree_name == std::string(tree.id.name + 2)) {
      node_tree = &tree;
      break;
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
  size_t run_once_index = m_runtimeTrees.size();

  const auto run_key = [](KX_GameObject *go, const std::string &name) -> uint64_t {
    const uintptr_t p = reinterpret_cast<uintptr_t>(go);
    uint64_t h = std::hash<std::string>{}(name);
    h ^= uint64_t(p) << 1;
    return h;
  };

  for (int iter = 0; iter < kMaxPops && !m_pendingRunOnce.empty(); iter++) {
    const std::pair<KX_GameObject *, std::string> item = m_pendingRunOnce.front();
    m_pendingRunOnce.pop_front();

    KX_GameObject *target = item.first;
    const std::string &tree_name = item.second;
    if (target == nullptr || tree_name.empty() || IsGameObjectQueuedForRemoval(target)) {
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
}

void LN_Manager::FlushSetLogicTreeEnabled(KX_GameObject *target_object,
                                         const std::string &tree_name,
                                         const bool enabled)
{
  if (target_object == nullptr || tree_name.empty() ||
      IsGameObjectQueuedForRemoval(target_object))
  {
    return;
  }
  if (LN_RuntimeTree *runtime_tree = FindRuntimeTreeBySourceName(target_object, tree_name)) {
    runtime_tree->SetEnabled(enabled);
  }
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

void LN_Manager::AssertMainThread() const
{
  BLI_assert(std::this_thread::get_id() == m_mainThreadId);
}
