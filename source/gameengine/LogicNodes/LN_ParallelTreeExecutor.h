/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_ParallelTreeExecutor.h
 *  \ingroup logicnodes
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "LN_CommandBuffer.h"
#include "LN_Types.h"

class KX_Scene;
class LN_RuntimeTree;

/** Evaluates independent runtime trees on worker threads and merges command lists on the main
 * thread. */
class LN_ParallelTreeExecutor {
 public:
  struct RuntimeTreeWorkItem {
    size_t runtime_tree_index = 0;
    LN_RuntimeTree *runtime_tree = nullptr;
  };

  struct SchedulerBatch {
    std::vector<RuntimeTreeWorkItem> items;
    uint32_t resource_access = LN_SCHEDULER_RESOURCE_NONE;
    uint32_t snapshot_channels = LN_DEP_SNAPSHOT_NONE;
    uint32_t input_channels = LN_DEP_INPUT_NONE;
    uint32_t event_channels = LN_DEP_EVENT_NONE;
    uint32_t query_channels = LN_DEP_QUERY_NONE;
    uint32_t command_classes = LN_DEP_COMMAND_NONE;
    LN_SchedulerRequiredPhase required_phase = LN_SchedulerRequiredPhase::None;
    LN_EstimatedWorkClass estimated_work_class = LN_EstimatedWorkClass::Trivial;
    LN_MainThreadOnlyReason main_thread_reason = LN_MainThreadOnlyReason::None;
    size_t grain_size = 1;
    bool worker_eligible = false;
  };

  struct SchedulerPlan {
    std::vector<SchedulerBatch> worker_batches;
    std::vector<RuntimeTreeWorkItem> main_thread_items;
    uint32_t planned_tree_count = 0;
    uint32_t worker_tree_count = 0;
    uint32_t main_thread_tree_count = 0;
    uint32_t command_resource_classes = LN_DEP_COMMAND_NONE;
    uint32_t worker_resource_access = LN_SCHEDULER_RESOURCE_NONE;
    uint32_t main_thread_resource_access = LN_SCHEDULER_RESOURCE_NONE;
    uint32_t query_tree_count = 0;
    uint32_t snapshot_only_tree_count = 0;
    uint32_t command_record_only_tree_count = 0;
    uint32_t main_thread_fallback_count = 0;
    bool immutable_worker_inputs = true;
    bool worker_metrics_are_local = true;
    bool deterministic_merge_order = true;
    bool main_thread_flush_isolated = true;
  };

  struct SchedulerPlannerStats {
    uint32_t planned_tree_count = 0;
    uint32_t worker_tree_count = 0;
    uint32_t main_thread_tree_count = 0;
    uint32_t worker_batch_count = 0;
    uint32_t main_thread_batch_count = 0;
    uint32_t scheduler_job_count = 0;
    uint32_t max_trees_per_worker_batch = 0;
    uint32_t average_trees_per_worker_batch_x100 = 0;
    uint32_t command_resource_classes = LN_DEP_COMMAND_NONE;
    uint32_t worker_resource_access = LN_SCHEDULER_RESOURCE_NONE;
    uint32_t main_thread_resource_access = LN_SCHEDULER_RESOURCE_NONE;
    uint32_t query_tree_count = 0;
    uint32_t snapshot_only_tree_count = 0;
    uint32_t command_record_only_tree_count = 0;
    uint32_t main_thread_fallback_count = 0;
    bool immutable_worker_inputs = true;
    bool worker_metrics_are_local = true;
    bool deterministic_merge_order = true;
    bool main_thread_flush_isolated = true;
  };

  /** True when parallel tree execution is enabled for this scene (env var overrides). */
  static bool IsEnabled(KX_Scene &scene);
  static size_t MinimumTreeCount();
  static SchedulerPlan BuildSchedulerPlan(const std::vector<RuntimeTreeWorkItem> &runtime_trees,
                                          bool parallel_enabled);
  static SchedulerPlannerStats StatsForPlan(const SchedulerPlan &plan);
  static bool ShouldUseDirectSerialCommandRecording(const SchedulerPlan &plan,
                                                    const SchedulerPlannerStats &stats,
                                                    bool has_pending_run_once);

  std::vector<LN_CommandBuffer::RecordedCommandList> ExecuteTreesToCommandLists(
      const std::vector<RuntimeTreeWorkItem> &runtime_trees,
      const LN_TickContext &context,
      bool collect_profile_counters = false);

  const SchedulerPlannerStats &GetLastPlannerStats() const
  {
    return m_lastPlannerStats;
  }

 private:
  SchedulerPlannerStats m_lastPlannerStats;
};
