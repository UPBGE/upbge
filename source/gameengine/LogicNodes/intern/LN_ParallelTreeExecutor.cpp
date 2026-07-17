/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_ParallelTreeExecutor.cpp
 *  \ingroup logicnodes
 */

#include "LN_ParallelTreeExecutor.h"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <utility>

#include "BLI_assert.h"
#include "BLI_task.hh"

#include "DNA_scene_types.h"
#include "LN_CommandBuffer.h"
#include "LN_Program.h"
#include "LN_RuntimeTree.h"
#include "KX_Scene.h"

namespace {

size_t WorkClassGrainSize(const LN_EstimatedWorkClass work_class)
{
  switch (work_class) {
    case LN_EstimatedWorkClass::Trivial:
      return 16;
    case LN_EstimatedWorkClass::Small:
      return 8;
    case LN_EstimatedWorkClass::Medium:
      return 4;
    case LN_EstimatedWorkClass::Heavy:
      return 1;
  }
  return 1;
}

bool IsSnapshotOnlyTree(const LN_ProgramDependencySummary &dependencies)
{
  return dependencies.command_classes == LN_DEP_COMMAND_NONE &&
         dependencies.query_channels == LN_DEP_QUERY_NONE &&
         dependencies.dynamic_flags == LN_DEP_DYNAMIC_NONE &&
         (dependencies.snapshot_channels != LN_DEP_SNAPSHOT_NONE ||
          dependencies.input_channels != LN_DEP_INPUT_NONE ||
          dependencies.event_channels != LN_DEP_EVENT_NONE);
}

bool IsCommandRecordOnlyTree(const LN_ProgramDependencySummary &dependencies)
{
  return dependencies.command_classes != LN_DEP_COMMAND_NONE &&
         dependencies.snapshot_channels == LN_DEP_SNAPSHOT_NONE &&
         dependencies.input_channels == LN_DEP_INPUT_NONE &&
         dependencies.event_channels == LN_DEP_EVENT_NONE &&
         dependencies.query_channels == LN_DEP_QUERY_NONE &&
         dependencies.dynamic_flags == LN_DEP_DYNAMIC_NONE;
}

bool IsWorkerEligible(const LN_RuntimeTree *runtime_tree)
{
  if (runtime_tree == nullptr) {
    return false;
  }
  std::shared_ptr<const LN_Program> program = runtime_tree->GetProgram();
  if (program == nullptr) {
    return false;
  }
  const LN_SchedulerSummary &scheduler = program->GetSchedulerSummary();
  const LN_ProgramDependencySummary &dependencies = program->GetDependencySummary();
  return program->IsParallelEligible() && scheduler.worker_lane_eligible &&
         dependencies.worker_lane_eligible &&
         scheduler.main_thread_only_reason == LN_MainThreadOnlyReason::None &&
         dependencies.main_thread_reason_mask == 0u;
}

bool BatchesHaveCompatibleResourceClass(
    const LN_ParallelTreeExecutor::SchedulerBatch &batch,
    const LN_SchedulerSummary &scheduler,
    const LN_ProgramDependencySummary &dependencies)
{
  return batch.required_phase == scheduler.required_phase &&
         batch.estimated_work_class == scheduler.estimated_work_class &&
         batch.resource_access == scheduler.resource_access &&
         batch.snapshot_channels == dependencies.snapshot_channels &&
         batch.input_channels == dependencies.input_channels &&
         batch.event_channels == dependencies.event_channels &&
         batch.query_channels == dependencies.query_channels &&
         batch.command_classes == dependencies.command_classes;
}

LN_ParallelTreeExecutor::SchedulerBatch MakeWorkerBatch(
    const LN_ParallelTreeExecutor::RuntimeTreeWorkItem &item,
    const LN_SchedulerSummary &scheduler,
    const LN_ProgramDependencySummary &dependencies)
{
  LN_ParallelTreeExecutor::SchedulerBatch batch;
  batch.items.push_back(item);
  batch.resource_access = scheduler.resource_access;
  batch.snapshot_channels = dependencies.snapshot_channels;
  batch.input_channels = dependencies.input_channels;
  batch.event_channels = dependencies.event_channels;
  batch.query_channels = dependencies.query_channels;
  batch.command_classes = dependencies.command_classes;
  batch.required_phase = scheduler.required_phase;
  batch.estimated_work_class = scheduler.estimated_work_class;
  batch.main_thread_reason = LN_MainThreadOnlyReason::None;
  batch.grain_size = WorkClassGrainSize(scheduler.estimated_work_class);
  batch.worker_eligible = true;
  return batch;
}

void AppendWorkerBatchChunks(const LN_ParallelTreeExecutor::SchedulerBatch &source,
                             std::vector<LN_ParallelTreeExecutor::SchedulerBatch> &batches)
{
  const size_t grain_size = std::max<size_t>(source.grain_size, 1);
  for (size_t first = 0; first < source.items.size(); first += grain_size) {
    LN_ParallelTreeExecutor::SchedulerBatch chunk = source;
    chunk.items.clear();
    const size_t end = std::min(first + grain_size, source.items.size());
    chunk.items.insert(chunk.items.end(), source.items.begin() + first, source.items.begin() + end);
    batches.push_back(std::move(chunk));
  }
}

LN_CommandBuffer::RecordedCommandList RecordReadyTreeCommandList(
    const LN_ParallelTreeExecutor::RuntimeTreeWorkItem &item,
    const LN_TickContext &context,
    const bool allow_worker_recording,
    const bool collect_profile_counters)
{
  LN_CommandBuffer::RecordedCommandList command_list;
  command_list.runtime_tree_index = item.runtime_tree_index;
  if (item.runtime_tree == nullptr) {
    return command_list;
  }

  LN_RuntimeProfileCounters profile_counters;
  LN_CommandBuffer local_buffer;
  local_buffer.SetTypedCommandStreamsEnabled(context.use_typed_command_streams);
  local_buffer.SetAllowWorkerRecording(allow_worker_recording);
  local_buffer.BeginRecording();
  item.runtime_tree->ExecuteReady(local_buffer,
                                  context,
                                  collect_profile_counters ? &profile_counters : nullptr);
  local_buffer.EndRecording();

  command_list.commands = local_buffer.TakeRecordedCommands();
  LN_CommandBuffer::SortCommands(command_list.commands);
  command_list.command_stream_stats = local_buffer.GetLastCommandStreamStats();
  command_list.profile_counters = profile_counters;
  return command_list;
}

std::vector<LN_CommandBuffer::RecordedCommandList> RecordReadyBatchCommandLists(
    const LN_ParallelTreeExecutor::SchedulerBatch &batch,
    const LN_TickContext &context,
    const bool allow_worker_recording,
    const bool collect_profile_counters)
{
  std::vector<LN_CommandBuffer::RecordedCommandList> command_lists;
  command_lists.reserve(batch.items.size());
  for (const LN_ParallelTreeExecutor::RuntimeTreeWorkItem &item : batch.items) {
    command_lists.push_back(RecordReadyTreeCommandList(item,
                                                       context,
                                                       allow_worker_recording,
                                                       collect_profile_counters));
  }
  return command_lists;
}

}  // namespace

bool LN_ParallelTreeExecutor::IsEnabled(KX_Scene &scene)
{
  const char *value = std::getenv("UPBGE_LOGIC_NODES_PARALLEL");
  if (value != nullptr && value[0] != '\0') {
    return value[0] != '0';
  }

  const blender::Scene *blender_scene = scene.GetBlenderScene();
  return blender_scene != nullptr &&
         (blender_scene->gm.flag & GAME_USE_LOGIC_NODES_PARALLEL) != 0;
}

size_t LN_ParallelTreeExecutor::MinimumTreeCount()
{
  return 2;
}

LN_ParallelTreeExecutor::SchedulerPlan LN_ParallelTreeExecutor::BuildSchedulerPlan(
    const std::vector<RuntimeTreeWorkItem> &runtime_trees, const bool parallel_enabled)
{
  SchedulerPlan plan;
  plan.planned_tree_count = uint32_t(runtime_trees.size());

  std::vector<SchedulerBatch> unchunked_worker_batches;
  for (const RuntimeTreeWorkItem &item : runtime_trees) {
    std::shared_ptr<const LN_Program> program = item.runtime_tree != nullptr ?
                                                    item.runtime_tree->GetProgram() :
                                                    nullptr;
    if (program == nullptr) {
      plan.main_thread_items.push_back(item);
      plan.main_thread_tree_count++;
      plan.main_thread_fallback_count++;
      continue;
    }

    const LN_SchedulerSummary &scheduler = program->GetSchedulerSummary();
    const LN_ProgramDependencySummary &dependencies = program->GetDependencySummary();
    plan.command_resource_classes |= dependencies.command_classes;
    if (dependencies.query_channels != LN_DEP_QUERY_NONE) {
      plan.query_tree_count++;
    }
    if (IsSnapshotOnlyTree(dependencies)) {
      plan.snapshot_only_tree_count++;
    }
    if (IsCommandRecordOnlyTree(dependencies)) {
      plan.command_record_only_tree_count++;
    }

    if (!parallel_enabled || !IsWorkerEligible(item.runtime_tree)) {
      plan.main_thread_items.push_back(item);
      plan.main_thread_tree_count++;
      plan.main_thread_resource_access |= scheduler.resource_access;
      plan.main_thread_fallback_count++;
      continue;
    }

    plan.worker_tree_count++;
    plan.worker_resource_access |= scheduler.resource_access;

    bool appended = false;
    for (SchedulerBatch &batch : unchunked_worker_batches) {
      if (BatchesHaveCompatibleResourceClass(batch, scheduler, dependencies)) {
        batch.items.push_back(item);
        appended = true;
        break;
      }
    }
    if (!appended) {
      unchunked_worker_batches.push_back(MakeWorkerBatch(item, scheduler, dependencies));
    }
  }

  for (const SchedulerBatch &batch : unchunked_worker_batches) {
    AppendWorkerBatchChunks(batch, plan.worker_batches);
  }
  return plan;
}

LN_ParallelTreeExecutor::SchedulerPlannerStats LN_ParallelTreeExecutor::StatsForPlan(
    const SchedulerPlan &plan)
{
  SchedulerPlannerStats stats;
  stats.planned_tree_count = plan.planned_tree_count;
  stats.worker_tree_count = plan.worker_tree_count;
  stats.main_thread_tree_count = plan.main_thread_tree_count;
  stats.worker_batch_count = uint32_t(plan.worker_batches.size());
  stats.main_thread_batch_count = plan.main_thread_items.empty() ? 0u : 1u;
  stats.scheduler_job_count = stats.worker_batch_count + stats.main_thread_batch_count;
  stats.command_resource_classes = plan.command_resource_classes;
  stats.worker_resource_access = plan.worker_resource_access;
  stats.main_thread_resource_access = plan.main_thread_resource_access;
  stats.query_tree_count = plan.query_tree_count;
  stats.snapshot_only_tree_count = plan.snapshot_only_tree_count;
  stats.command_record_only_tree_count = plan.command_record_only_tree_count;
  stats.main_thread_fallback_count = plan.main_thread_fallback_count;
  stats.immutable_worker_inputs = plan.immutable_worker_inputs;
  stats.worker_metrics_are_local = plan.worker_metrics_are_local;
  stats.deterministic_merge_order = plan.deterministic_merge_order;
  stats.main_thread_flush_isolated = plan.main_thread_flush_isolated;

  uint32_t total_worker_batch_trees = 0;
  for (const SchedulerBatch &batch : plan.worker_batches) {
    const uint32_t batch_size = uint32_t(batch.items.size());
    total_worker_batch_trees += batch_size;
    stats.max_trees_per_worker_batch = std::max(stats.max_trees_per_worker_batch, batch_size);
  }
  if (!plan.worker_batches.empty()) {
    stats.average_trees_per_worker_batch_x100 =
        (total_worker_batch_trees * 100u) / uint32_t(plan.worker_batches.size());
  }
  return stats;
}

bool LN_ParallelTreeExecutor::ShouldUseDirectSerialCommandRecording(
    const SchedulerPlan &plan,
    const SchedulerPlannerStats &stats,
    const bool has_pending_run_once)
{
  if (has_pending_run_once || stats.planned_tree_count == 0u) {
    return false;
  }

  if (stats.worker_tree_count == 0u) {
    return true;
  }

  for (const SchedulerBatch &batch : plan.worker_batches) {
    if (static_cast<uint8_t>(batch.estimated_work_class) >
        static_cast<uint8_t>(LN_EstimatedWorkClass::Small))
    {
      return false;
    }
  }

  if (stats.command_record_only_tree_count == stats.planned_tree_count) {
    return true;
  }

  static constexpr uint32_t kMinMostlyCommandRecordOnlyTrees = 64;
  if (stats.command_record_only_tree_count < kMinMostlyCommandRecordOnlyTrees) {
    return false;
  }

  if ((stats.command_record_only_tree_count * 4u) < (stats.planned_tree_count * 3u)) {
    return false;
  }

  const uint32_t mixed_tree_count =
      stats.planned_tree_count - stats.command_record_only_tree_count;
  const uint32_t mixed_tree_limit = std::max(16u, stats.planned_tree_count / 16u);
  return mixed_tree_count <= mixed_tree_limit;
}

std::vector<LN_CommandBuffer::RecordedCommandList>
LN_ParallelTreeExecutor::ExecuteTreesToCommandLists(
    const std::vector<RuntimeTreeWorkItem> &runtime_trees,
    const LN_TickContext &context,
    const bool collect_profile_counters)
{
  if (runtime_trees.empty()) {
    m_lastPlannerStats = SchedulerPlannerStats();
    return {};
  }

  for (const RuntimeTreeWorkItem &item : runtime_trees) {
    if (item.runtime_tree != nullptr) {
      item.runtime_tree->WarmQueryCache(context);
    }
  }

  const SchedulerPlan plan = BuildSchedulerPlan(runtime_trees, true);
  m_lastPlannerStats = StatsForPlan(plan);

  std::vector<LN_CommandBuffer::RecordedCommandList> command_lists;
  command_lists.reserve(runtime_trees.size());

  for (const RuntimeTreeWorkItem &item : plan.main_thread_items) {
    SchedulerBatch batch;
    batch.items.push_back(item);
    std::vector<LN_CommandBuffer::RecordedCommandList> local_lists =
        RecordReadyBatchCommandLists(batch, context, false, collect_profile_counters);
    command_lists.insert(command_lists.end(),
                         std::make_move_iterator(local_lists.begin()),
                         std::make_move_iterator(local_lists.end()));
  }

  const size_t worker_batch_count = plan.worker_batches.size();
  if (worker_batch_count == 1) {
    std::vector<LN_CommandBuffer::RecordedCommandList> local_lists =
        RecordReadyBatchCommandLists(plan.worker_batches.front(),
                                     context,
                                     true,
                                     collect_profile_counters);
    command_lists.insert(command_lists.end(),
                         std::make_move_iterator(local_lists.begin()),
                         std::make_move_iterator(local_lists.end()));
  }
  else if (worker_batch_count > 1) {
    std::vector<std::vector<LN_CommandBuffer::RecordedCommandList>> batch_command_lists(
        worker_batch_count);
    blender::threading::parallel_for(
        blender::IndexRange(worker_batch_count), 1, [&](const blender::IndexRange range) {
        for (const int64_t index : range) {
          batch_command_lists[size_t(index)] =
              RecordReadyBatchCommandLists(plan.worker_batches[size_t(index)],
                                           context,
                                           true,
                                           collect_profile_counters);
        }
      });
    for (std::vector<LN_CommandBuffer::RecordedCommandList> &local_lists : batch_command_lists) {
      command_lists.insert(command_lists.end(),
                           std::make_move_iterator(local_lists.begin()),
                           std::make_move_iterator(local_lists.end()));
    }
  }

  std::stable_sort(command_lists.begin(),
                   command_lists.end(),
                   [](const LN_CommandBuffer::RecordedCommandList &left,
                      const LN_CommandBuffer::RecordedCommandList &right) {
                     return left.runtime_tree_index < right.runtime_tree_index;
                   });
  return command_lists;
}
