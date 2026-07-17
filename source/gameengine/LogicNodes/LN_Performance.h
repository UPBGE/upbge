/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_Performance.h
 *  \ingroup logicnodes
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "LN_RuntimeSemantics.h"

enum class LN_BenchmarkScenarioKind : uint8_t {
  ManySmallTrees = 0,
  FewHeavyTrees,
  InstructionHeavy,
  EventHeavy,
  SnapshotHeavy,
  CommandHeavy,
  MathHeavy,
  MixedSerialParallel,
  TransformHeavy,
  PhysicsQueryHeavy,
  PropertyEventHeavy,
  SpawnLifecycleHeavy,
  Count,
};

enum LN_ProfileMetric : uint32_t {
  LN_PROFILE_METRIC_SERIAL_TIME = 1u << 0,
  LN_PROFILE_METRIC_PARALLEL_TIME = 1u << 1,
  LN_PROFILE_METRIC_COMMAND_RECORD_TIME = 1u << 2,
  LN_PROFILE_METRIC_COMMAND_MERGE_TIME = 1u << 3,
  LN_PROFILE_METRIC_COMMAND_FLUSH_TIME = 1u << 4,
  LN_PROFILE_METRIC_SNAPSHOT_REBUILD_TIME = 1u << 5,
  LN_PROFILE_METRIC_ALLOCATIONS = 1u << 6,
  LN_PROFILE_METRIC_STRING_COPIES = 1u << 7,
  LN_PROFILE_METRIC_HOT_MAP_LOOKUPS = 1u << 8,
  LN_PROFILE_METRIC_EVENT_DELIVERY = 1u << 9,
  LN_PROFILE_METRIC_INSTRUCTION_DISPATCH = 1u << 10,
  LN_PROFILE_METRIC_EXPRESSION_EVALUATION = 1u << 11,
  LN_PROFILE_METRIC_SCHEDULER_JOBS = 1u << 12,
  LN_PROFILE_METRIC_FALLBACK_PATHS = 1u << 13,
};

inline constexpr uint32_t LN_PROFILE_REQUIRED_METRICS =
    LN_PROFILE_METRIC_SERIAL_TIME | LN_PROFILE_METRIC_PARALLEL_TIME |
    LN_PROFILE_METRIC_COMMAND_RECORD_TIME | LN_PROFILE_METRIC_COMMAND_MERGE_TIME |
    LN_PROFILE_METRIC_COMMAND_FLUSH_TIME | LN_PROFILE_METRIC_SNAPSHOT_REBUILD_TIME |
    LN_PROFILE_METRIC_ALLOCATIONS | LN_PROFILE_METRIC_STRING_COPIES |
    LN_PROFILE_METRIC_HOT_MAP_LOOKUPS | LN_PROFILE_METRIC_EVENT_DELIVERY |
    LN_PROFILE_METRIC_INSTRUCTION_DISPATCH | LN_PROFILE_METRIC_EXPRESSION_EVALUATION |
    LN_PROFILE_METRIC_SCHEDULER_JOBS | LN_PROFILE_METRIC_FALLBACK_PATHS;

struct LN_BenchmarkScenario {
  LN_BenchmarkScenarioKind kind = LN_BenchmarkScenarioKind::ManySmallTrees;
  const char *id = nullptr;
  const char *description = nullptr;
  uint32_t required_metrics = LN_PROFILE_REQUIRED_METRICS;
};

struct LN_BenchmarkProtocol {
  const char *build_command = "make NPROCS=32";
  const char *build_configuration = "UPBGE build_linux Logic Nodes performance gate";
  double fixed_dt = 1.0 / 60.0;
  uint32_t warmup_frame_count = 120;
  uint32_t measured_frame_count = 600;
  bool run_serial = true;
  bool run_parallel = true;
  bool report_median = true;
  bool report_p95 = true;
  bool report_worst_frame = true;
};

struct LN_SyntheticBenchmarkDriver {
  LN_BenchmarkScenarioKind scenario = LN_BenchmarkScenarioKind::ManySmallTrees;
  const char *id = nullptr;
  uint32_t tree_count = 0;
  uint32_t instruction_count_per_tree = 0;
  uint32_t expression_count_per_tree = 0;
  uint32_t event_subject_count = 0;
  uint32_t event_count_per_tick = 0;
  uint32_t snapshot_object_count = 0;
  uint32_t command_count_per_tree = 0;
  bool includes_parallel_eligible_trees = false;
  bool includes_main_thread_only_trees = false;
  uint32_t required_metrics = LN_PROFILE_REQUIRED_METRICS;
};

enum class LN_BenchmarkFixtureSource : uint8_t {
  CppSyntheticGraph = 0,
  BlendFixture,
};

enum LN_BenchmarkReplayDomainMask : uint32_t {
  LN_BENCHMARK_REPLAY_RUNTIME_EXECUTION = 1u << 0,
  LN_BENCHMARK_REPLAY_EVENT_ORDER = 1u << 1,
  LN_BENCHMARK_REPLAY_COMMAND_ORDER = 1u << 2,
  LN_BENCHMARK_REPLAY_SNAPSHOT_READS = 1u << 3,
  LN_BENCHMARK_REPLAY_SCHEDULER_MERGE_ORDER = 1u << 4,
  LN_BENCHMARK_REPLAY_DIAGNOSTICS = 1u << 5,
};

inline constexpr uint32_t LN_BENCHMARK_REPLAY_ALL_DOMAINS =
    LN_BENCHMARK_REPLAY_RUNTIME_EXECUTION | LN_BENCHMARK_REPLAY_EVENT_ORDER |
    LN_BENCHMARK_REPLAY_COMMAND_ORDER | LN_BENCHMARK_REPLAY_SNAPSHOT_READS |
    LN_BENCHMARK_REPLAY_SCHEDULER_MERGE_ORDER | LN_BENCHMARK_REPLAY_DIAGNOSTICS;

struct LN_BenchmarkFixtureSpec {
  LN_BenchmarkScenarioKind scenario = LN_BenchmarkScenarioKind::ManySmallTrees;
  const char *id = nullptr;
  const char *generator_id = nullptr;
  LN_BenchmarkFixtureSource source = LN_BenchmarkFixtureSource::CppSyntheticGraph;
  const char *blend_fixture_path = nullptr;
  const char *engine_service_boundary = nullptr;
  uint32_t deterministic_seed = 0;
  uint32_t replay_domain_mask = LN_BENCHMARK_REPLAY_ALL_DOMAINS;
  uint32_t warmup_frame_count = 0;
  uint32_t measured_frame_count = 0;
  bool user_scene_required = false;
  bool produces_legacy_mode = true;
  bool produces_optimized_mode = true;
};

enum class LN_ServiceBlendFixtureKind : uint8_t {
  PhysicsWorld = 0,
  CharacterController,
  AudioDevice,
  MaterialDatablock,
  SceneObjectLifecycle,
  BlenderPlayerRestartLoad,
  Count,
};

struct LN_ServiceBlendFixtureSpec {
  LN_ServiceBlendFixtureKind kind = LN_ServiceBlendFixtureKind::PhysicsWorld;
  const char *id = nullptr;
  const char *blend_fixture_path = nullptr;
  const char *engine_service_boundary = nullptr;
  uint32_t replay_domain_mask = LN_BENCHMARK_REPLAY_RUNTIME_EXECUTION;
  bool user_scene_required = false;
  bool validates_legacy_mode = true;
  bool validates_optimized_mode = true;
};

struct LN_BenchmarkBaselineExpectation {
  LN_BenchmarkScenarioKind scenario = LN_BenchmarkScenarioKind::ManySmallTrees;
  uint32_t expected_metric_fields = LN_PROFILE_REQUIRED_METRICS;
  uint32_t minimum_tree_count = 1;
  uint32_t minimum_measured_frame_count = 1;
  bool requires_serial_run = true;
  bool requires_parallel_run = false;
};

enum class LN_BenchmarkPlatform : uint8_t {
  Linux = 0,
  Windows,
  MacOS,
  Count,
};

struct LN_BenchmarkRelativeBaseline {
  LN_BenchmarkPlatform platform = LN_BenchmarkPlatform::Linux;
  const char *id = nullptr;
  uint32_t max_optimized_serial_median_ratio_x100 = 100;
  uint32_t max_optimized_serial_p95_ratio_x100 = 105;
  uint32_t max_optimized_serial_worst_ratio_x100 = 110;
  uint32_t max_optimized_parallel_median_ratio_x100 = 100;
  uint32_t max_optimized_parallel_p95_ratio_x100 = 105;
  uint32_t max_optimized_parallel_worst_ratio_x100 = 110;
  uint32_t min_measured_frame_count = 1;
  bool ci_gate_enabled = false;
};

enum class LN_EquivalenceDomain : uint8_t {
  RuntimeExecution = 0,
  EventOrder,
  CommandOrder,
  SnapshotReads,
  SchedulerMergeOrder,
  Diagnostics,
  Count,
};

struct LN_EquivalenceCheckSpec {
  LN_EquivalenceDomain domain = LN_EquivalenceDomain::RuntimeExecution;
  const char *id = nullptr;
  const char *description = nullptr;
  bool requires_legacy_path = true;
  bool requires_deterministic_replay = true;
};

struct LN_BenchmarkReplayTraceSpec {
  LN_EquivalenceDomain domain = LN_EquivalenceDomain::RuntimeExecution;
  const char *id = nullptr;
  uint32_t scenario_mask = 0;
  bool compares_legacy_and_optimized = true;
  bool compares_serial_and_parallel = true;
  bool deterministic_checksum_required = true;
};

struct LN_BenchmarkSoakSpec {
  const char *id = nullptr;
  uint32_t tick_count = 0;
  bool release_mode_required = true;
  bool compares_legacy_and_optimized = true;
  bool compares_serial_and_parallel = true;
  bool compares_command_outputs = true;
  bool compares_event_outputs = true;
  bool compares_snapshot_outputs = true;
  bool deterministic_checksum_required = true;
};

struct LN_BenchmarkFixtureSoakResult {
  LN_BenchmarkScenarioKind scenario = LN_BenchmarkScenarioKind::ManySmallTrees;
  const char *fixture_id = nullptr;
  uint32_t tick_count = 0;
  uint32_t legacy_checksum = 0;
  uint32_t optimized_checksum = 0;
  uint32_t serial_checksum = 0;
  uint32_t parallel_checksum = 0;
  bool command_outputs_match = false;
  bool event_outputs_match = false;
  bool snapshot_outputs_match = false;
  bool deterministic_replay_checksum_valid = false;
  bool optimized_paths_used = false;
  bool valid = false;
};

enum class LN_RuntimeFallbackReason : uint8_t {
  UnsupportedOpcode = 0,
  UnsupportedExpression,
  DynamicLookup,
  MainThreadLiveRead,
  CommandBarrier,
  UnsafeResourceClass,
  MissingSnapshotChannel,
  StaleHandle,
  EngineServiceBoundary,
  Count,
};

struct LN_RuntimeFallbackDiagnosticSpec {
  LN_RuntimeFallbackReason reason = LN_RuntimeFallbackReason::UnsupportedOpcode;
  const char *id = nullptr;
  const char *description = nullptr;
  bool source_ref_required = true;
  bool profile_counter_required = true;
  bool debug_name_required = true;
  bool warning_required_for_hotspot = true;
};

enum LN_RuntimeFeatureFlag : uint32_t {
  LN_RUNTIME_FEATURE_TYPED_COMMAND_STREAMS = 1u << 0,
  LN_RUNTIME_FEATURE_RESOURCE_CLASS_SCHEDULER = 1u << 1,
  LN_RUNTIME_FEATURE_REGISTER_EXPRESSION_EVALUATOR = 1u << 2,
};

enum class LN_RuntimeGuardrailAssertion : uint8_t {
  DependencyMatchesRuntimeRead = 0,
  DeterministicEventOrder,
  DeterministicCommandOrder,
  DeterministicSchedulerMerge,
  SnapshotChannelCapturedBeforeRead,
  MainThreadServiceIsolation,
  AllocationFreeWarmTick,
  Count,
};

struct LN_RuntimeFeatureConfig {
  uint32_t enabled_features = 0;
  bool debug_assertions = false;
  bool dual_path_validation = false;
  bool benchmark_profile_summaries = false;
};

struct LN_RuntimeFeatureFlagStatus {
  LN_RuntimeFeatureFlag feature = LN_RUNTIME_FEATURE_TYPED_COMMAND_STREAMS;
  const char *feature_name = nullptr;
  const char *environment_variable = nullptr;
  const char *owner = nullptr;
  const char *diagnostic_purpose = nullptr;
  const char *removal_condition = nullptr;
  const char *removal_review_date = nullptr;
  bool default_enabled = true;
  bool required_for_user_activation = false;
  bool emergency_override = true;
};

enum class LN_AcceptanceTargetKind : uint8_t {
  FocusedGTest = 0,
  RuntimeAcceptanceCTest,
  AutomatedBenchmarkGate,
  DeterministicReplaySoak,
  ServiceFixtureCTest,
  Count,
};

struct LN_AcceptanceTargetSpec {
  LN_AcceptanceTargetKind kind = LN_AcceptanceTargetKind::FocusedGTest;
  const char *id = nullptr;
  const char *command = nullptr;
  bool ci_ready = true;
  bool covers_committed_fixtures = true;
  bool user_scene_required = false;
};

struct LN_FinalRoadmapGateStatus {
  const char *build_command = "make NPROCS=32";
  bool build_required = true;
  bool focused_gtests_required = true;
  bool acceptance_ctests_required = true;
  bool deterministic_replay_checks_required = true;
  bool automated_benchmark_gates_required = true;
  bool user_benchmark_scenes_required = false;
  bool phase10_full_conversion_complete = false;
  bool measured_service_fixtures_complete = false;
  bool production_readiness_review_complete = false;
};

enum class LN_Phase10ConversionPath : uint8_t {
  Unclassified = 0,
  ExecBlockIR,
  RegisterExpressionIR,
  TypedCommandStream,
  SelectiveSnapshotRead,
  TypedEventBus,
  ScalarFallback,
  LegacyInterpreterFallback,
  PermanentDynamicFallback,
};

struct LN_Phase10OpcodeConversionEntry {
  LN_OpCode opcode = LN_OpCode::Nop;
  const char *name = nullptr;
  LN_RuntimeCommandFamily command_family = LN_RuntimeCommandFamily::Unknown;
  uint32_t fallback_requirements = LN_RUNTIME_FALLBACK_NONE;
  LN_Phase10ConversionPath current_path = LN_Phase10ConversionPath::Unclassified;
  LN_Phase10ConversionPath target_path = LN_Phase10ConversionPath::Unclassified;
  bool safely_convertible = false;
  bool currently_converted = false;
  bool requires_static_operands = false;
  const char *completion_note = nullptr;
};

struct LN_Phase10OpcodeFallbackDiagnostic {
  LN_OpCode opcode = LN_OpCode::Nop;
  const char *name = nullptr;
  LN_RuntimeFallbackReason reason = LN_RuntimeFallbackReason::UnsupportedOpcode;
  const char *profile_counter_name = nullptr;
  const char *removal_condition = nullptr;
  bool source_ref_required = true;
  bool hot_path_warning_required = true;
};

struct LN_Phase10ExpressionFallbackDiagnostic {
  LN_RuntimeExpressionFamily family = LN_RuntimeExpressionFamily::Bool;
  uint32_t kind = 0;
  const char *name = nullptr;
  LN_RuntimeFallbackReason reason = LN_RuntimeFallbackReason::UnsupportedExpression;
  const char *profile_counter_name = nullptr;
  const char *removal_condition = nullptr;
  bool source_ref_required = true;
  bool hot_path_warning_required = true;
};

struct LN_Phase10ExpressionConversionEntry {
  LN_RuntimeExpressionFamily family = LN_RuntimeExpressionFamily::Bool;
  uint32_t kind = 0;
  const char *name = nullptr;
  uint32_t fallback_requirements = LN_RUNTIME_FALLBACK_NONE;
  LN_Phase10ConversionPath current_path = LN_Phase10ConversionPath::Unclassified;
  LN_Phase10ConversionPath target_path = LN_Phase10ConversionPath::Unclassified;
  bool safely_convertible = false;
  bool currently_converted = false;
  const char *completion_note = nullptr;
};

struct LN_TickProfileMetrics {
  double snapshot_ms = 0.0;
  double serial_record_ms = 0.0;
  double parallel_record_ms = 0.0;
  double run_once_record_ms = 0.0;
  double merge_ms = 0.0;
  double flush_ms = 0.0;
  double event_delivery_ms = 0.0;
  uint32_t snapshot_tree_count = 0;
  uint32_t snapshot_captured_channel_mask = 0;
  uint32_t snapshot_skipped_channel_mask = 0;
  uint32_t snapshot_shared_input_tree_count = 0;
  uint32_t snapshot_property_storage_reuse_count = 0;
  uint32_t serial_tree_count = 0;
  uint32_t parallel_tree_count = 0;
  uint32_t run_once_count = 0;
  uint32_t event_count = 0;
  uint32_t event_snapshot_tree_count = 0;
  uint64_t event_indexed_lookup_count = 0;
  uint64_t event_fallback_scan_count = 0;
  uint32_t query_count = 0;
  uint32_t command_list_count = 0;
  uint32_t command_count = 0;
  uint32_t command_legacy_count = 0;
  uint32_t command_typed_transform_count = 0;
  uint32_t command_typed_velocity_count = 0;
  uint32_t command_typed_relative_vector_count = 0;
  uint32_t command_typed_motion_count = 0;
  uint32_t command_typed_property_count = 0;
  uint32_t command_typed_event_count = 0;
  uint32_t command_typed_audio_count = 0;
  uint32_t command_typed_lifecycle_count = 0;
  uint32_t command_typed_object_service_count = 0;
  uint32_t command_typed_runtime_service_count = 0;
  uint32_t command_typed_animation_count = 0;
  uint32_t command_typed_armature_count = 0;
  uint32_t command_typed_material_count = 0;
  uint32_t command_typed_physics_count = 0;
  uint32_t command_coalesced_count = 0;
  uint32_t scheduler_job_count = 0;
  uint32_t scheduler_planned_tree_count = 0;
  uint32_t scheduler_worker_batch_count = 0;
  uint32_t scheduler_main_thread_batch_count = 0;
  uint32_t scheduler_max_trees_per_worker_batch = 0;
  uint32_t scheduler_average_trees_per_worker_batch_x100 = 0;
  uint32_t scheduler_worker_utilization_x100 = 0;
  uint32_t scheduler_command_resource_classes = 0;
  uint32_t scheduler_worker_resource_access = 0;
  uint32_t scheduler_main_thread_resource_access = 0;
  uint32_t scheduler_query_tree_count = 0;
  uint32_t scheduler_snapshot_only_tree_count = 0;
  uint32_t scheduler_command_record_only_tree_count = 0;
  bool scheduler_immutable_worker_inputs = true;
  bool scheduler_worker_metrics_are_local = true;
  bool scheduler_deterministic_merge_order = true;
  bool scheduler_main_thread_flush_isolated = true;
  uint32_t instruction_count = 0;
  uint32_t exec_block_count = 0;
  uint32_t exec_direct_instruction_count = 0;
  uint32_t exec_fallback_instruction_count = 0;
  uint32_t exec_fallback_block_count = 0;
  uint64_t exec_fallback_ns = 0;
  uint32_t expression_count = 0;
  uint32_t register_expression_hit_count = 0;
  uint32_t register_expression_fallback_count = 0;
  uint32_t register_scalar_op_count = 0;
  uint32_t register_simd_candidate_batch_count = 0;
  uint32_t register_simd_candidate_lane_count = 0;
  uint32_t register_simd_batch_count = 0;
  uint32_t register_simd_lane_count = 0;
  uint32_t fallback_path_count = 0;
  uint32_t runtime_fallback_reason_mask = 0;
  uint32_t runtime_expression_fallback_reason_mask = 0;
  uint32_t runtime_system_fallback_reason_mask = 0;
  uint32_t runtime_system_fallback_count = 0;
  uint32_t main_thread_fallback_tree_count = 0;
  uint64_t allocation_count = 0;
  uint64_t string_copy_count = 0;
  uint64_t hot_map_lookup_count = 0;
  bool parallel_executor_used = false;
};

struct LN_RuntimeProfileCounters {
  uint32_t instruction_dispatch_count = 0;
  uint32_t exec_block_count = 0;
  uint32_t exec_direct_instruction_count = 0;
  uint32_t exec_fallback_instruction_count = 0;
  uint32_t exec_fallback_block_count = 0;
  uint64_t exec_fallback_ns = 0;
  uint32_t expression_evaluation_count = 0;
  uint32_t register_expression_hit_count = 0;
  uint32_t register_expression_fallback_count = 0;
  uint32_t register_scalar_op_count = 0;
  uint32_t register_simd_candidate_batch_count = 0;
  uint32_t register_simd_candidate_lane_count = 0;
  uint32_t register_simd_batch_count = 0;
  uint32_t register_simd_lane_count = 0;
  uint32_t fallback_path_count = 0;
  uint32_t runtime_fallback_reason_mask = 0;
  uint32_t runtime_expression_fallback_reason_mask = 0;
  uint32_t runtime_system_fallback_reason_mask = 0;
  uint32_t runtime_system_fallback_count = 0;
};

enum class LN_BenchmarkRunMode : uint8_t {
  LegacySerial = 0,
  LegacyParallel,
  OptimizedSerial,
  OptimizedParallel,
  Count,
};

struct LN_BenchmarkRunResult {
  LN_BenchmarkRunMode mode = LN_BenchmarkRunMode::LegacySerial;
  LN_TickProfileMetrics metrics;
  double median_ms = 0.0;
  double p95_ms = 0.0;
  double worst_frame_ms = 0.0;
  uint32_t warmup_frame_count = 0;
  uint32_t measured_frame_count = 0;
  uint32_t runtime_fixture_tick_count = 0;
  uint32_t runtime_fixture_tree_count = 0;
  uint32_t runtime_fixture_command_count = 0;
  uint32_t runtime_fixture_instruction_count = 0;
  uint32_t runtime_fixture_expression_count = 0;
  uint32_t runtime_fixture_register_hit_count = 0;
  uint32_t runtime_fixture_register_fallback_count = 0;
  bool runtime_fixture_measured = false;
  bool runtime_fixture_record_phase_measured = false;
  bool runtime_fixture_command_flush_measured = false;
  bool runtime_fixture_service_boundary_measured = false;
  bool formula_timing_estimate_used = false;
  bool valid = false;
};

struct LN_BenchmarkFixtureRunSummary {
  LN_BenchmarkScenarioKind scenario = LN_BenchmarkScenarioKind::ManySmallTrees;
  const char *fixture_id = nullptr;
  std::array<LN_BenchmarkRunResult, size_t(LN_BenchmarkRunMode::Count)> runs;
  uint32_t run_count = 0;
  bool emitted_json_summary = false;
  bool emitted_profile_line_summaries = false;
};

struct LN_BenchmarkMeasuredRegressionDiagnostic {
  LN_BenchmarkScenarioKind scenario = LN_BenchmarkScenarioKind::ManySmallTrees;
  const char *fixture_id = nullptr;
  uint32_t optimized_serial_median_ratio_x100 = 0;
  uint32_t optimized_serial_p95_ratio_x100 = 0;
  uint32_t optimized_serial_worst_ratio_x100 = 0;
  uint32_t optimized_parallel_median_ratio_x100 = 0;
  uint32_t optimized_parallel_p95_ratio_x100 = 0;
  uint32_t optimized_parallel_worst_ratio_x100 = 0;
  bool serial_failed = false;
  bool parallel_failed = false;
};

const std::array<LN_BenchmarkScenario, size_t(LN_BenchmarkScenarioKind::Count)> &
LN_GetBenchmarkScenarios();
const std::array<LN_SyntheticBenchmarkDriver, size_t(LN_BenchmarkScenarioKind::Count)> &
LN_GetSyntheticBenchmarkDrivers();
const std::array<LN_BenchmarkFixtureSpec, size_t(LN_BenchmarkScenarioKind::Count)> &
LN_GetBenchmarkFixtureSpecs();
const std::array<LN_ServiceBlendFixtureSpec, size_t(LN_ServiceBlendFixtureKind::Count)> &
LN_GetServiceBlendFixtureSpecs();
const std::array<LN_BenchmarkBaselineExpectation, size_t(LN_BenchmarkScenarioKind::Count)> &
LN_GetBenchmarkBaselineExpectations();
const std::array<LN_BenchmarkRelativeBaseline, size_t(LN_BenchmarkPlatform::Count)> &
LN_GetBenchmarkRelativeBaselines();
const std::array<LN_EquivalenceCheckSpec, size_t(LN_EquivalenceDomain::Count)> &
LN_GetEquivalenceCheckSpecs();
const std::array<LN_BenchmarkReplayTraceSpec, size_t(LN_EquivalenceDomain::Count)> &
LN_GetBenchmarkReplayTraceSpecs();
const LN_BenchmarkSoakSpec &LN_GetBenchmarkSoakSpec();
const std::array<LN_RuntimeFallbackDiagnosticSpec, size_t(LN_RuntimeFallbackReason::Count)> &
LN_GetRuntimeFallbackDiagnosticSpecs();
const std::array<LN_RuntimeFeatureFlagStatus, 3> &LN_GetRuntimeFeatureFlagStatuses();
const std::array<LN_AcceptanceTargetSpec, size_t(LN_AcceptanceTargetKind::Count)> &
LN_GetAcceptanceTargetSpecs();
const LN_FinalRoadmapGateStatus &LN_GetFinalRoadmapGateStatus();
const LN_BenchmarkProtocol &LN_GetBenchmarkProtocol();
const char *LN_BenchmarkScenarioKindName(LN_BenchmarkScenarioKind kind);
const char *LN_EquivalenceDomainName(LN_EquivalenceDomain domain);
const char *LN_BenchmarkFixtureSourceName(LN_BenchmarkFixtureSource source);
const char *LN_ServiceBlendFixtureKindName(LN_ServiceBlendFixtureKind kind);
const char *LN_BenchmarkRunModeName(LN_BenchmarkRunMode mode);
const char *LN_BenchmarkPlatformName(LN_BenchmarkPlatform platform);
const char *LN_RuntimeFallbackReasonName(LN_RuntimeFallbackReason reason);
const char *LN_AcceptanceTargetKindName(LN_AcceptanceTargetKind kind);
const char *LN_Phase10ConversionPathName(LN_Phase10ConversionPath path);
bool LN_ProfileMetricsCoverRequired(uint32_t metric_mask);
bool LN_BenchmarkDriverMatchesScenario(const LN_SyntheticBenchmarkDriver &driver,
                                       const LN_BenchmarkScenario &scenario);
bool LN_BenchmarkFixtureMatchesDriver(const LN_BenchmarkFixtureSpec &fixture,
                                      const LN_SyntheticBenchmarkDriver &driver,
                                      const LN_BenchmarkProtocol &protocol);
bool LN_BenchmarkBaselineMatchesProtocol(const LN_BenchmarkBaselineExpectation &baseline,
                                         const LN_BenchmarkProtocol &protocol);
bool LN_BenchmarkRelativeBaselineMatchesProtocol(const LN_BenchmarkRelativeBaseline &baseline,
                                                 const LN_BenchmarkProtocol &protocol);
bool LN_BenchmarkReplayTraceCoversDomain(const LN_BenchmarkReplayTraceSpec &trace,
                                         LN_EquivalenceDomain domain);
bool LN_RuntimeFallbackDiagnosticCoversReason(const LN_RuntimeFallbackDiagnosticSpec &diagnostic,
                                             LN_RuntimeFallbackReason reason);
bool LN_RuntimeFeatureFlagsAreEmergencyOverrides(
    const std::array<LN_RuntimeFeatureFlagStatus, 3> &statuses);
bool LN_AcceptanceTargetMatchesPhaseNineContract(const LN_AcceptanceTargetSpec &target);
const std::vector<LN_Phase10OpcodeConversionEntry> &LN_GetPhase10OpcodeConversionMatrix();
const std::vector<LN_Phase10OpcodeFallbackDiagnostic> &
LN_GetPhase10OpcodeFallbackDiagnostics();
const std::vector<LN_Phase10ExpressionConversionEntry> &LN_GetPhase10ExpressionConversionMatrix();
const std::vector<LN_Phase10ExpressionFallbackDiagnostic> &
LN_GetPhase10ExpressionFallbackDiagnostics();
bool LN_Phase10OpcodeConversionMatrixIsComplete(
    const std::vector<LN_Phase10OpcodeConversionEntry> &matrix);
bool LN_Phase10OpcodeFallbackDiagnosticsAreComplete(
    const std::vector<LN_Phase10OpcodeConversionEntry> &matrix,
    const std::vector<LN_Phase10OpcodeFallbackDiagnostic> &diagnostics);
bool LN_Phase10ExpressionConversionMatrixIsComplete(
    const std::vector<LN_Phase10ExpressionConversionEntry> &matrix);
bool LN_Phase10ExpressionFallbackDiagnosticsAreComplete(
    const std::vector<LN_Phase10ExpressionConversionEntry> &matrix,
    const std::vector<LN_Phase10ExpressionFallbackDiagnostic> &diagnostics);
LN_BenchmarkFixtureRunSummary LN_RunBenchmarkFixture(
    const LN_BenchmarkFixtureSpec &fixture,
    const LN_SyntheticBenchmarkDriver &driver,
    const LN_BenchmarkProtocol &protocol);
LN_BenchmarkFixtureSoakResult LN_RunBenchmarkFixtureSoak(
    const LN_BenchmarkFixtureSpec &fixture,
    const LN_SyntheticBenchmarkDriver &driver,
    const LN_BenchmarkProtocol &protocol,
    const LN_BenchmarkSoakSpec &soak);
bool LN_BenchmarkRunSummaryMatchesProtocol(const LN_BenchmarkFixtureRunSummary &summary,
                                           const LN_BenchmarkProtocol &protocol);
bool LN_BenchmarkRunSummaryPassesRelativeBaseline(
    const LN_BenchmarkFixtureRunSummary &summary,
    const LN_BenchmarkRelativeBaseline &baseline);
bool LN_BenchmarkRunSummaryPassesHotPathGate(const LN_BenchmarkFixtureRunSummary &summary);
bool LN_BenchmarkRunSummaryPassesSimdBenefitGate(
    const LN_BenchmarkFixtureRunSummary &summary,
    const LN_BenchmarkRelativeBaseline &baseline);
bool LN_BenchmarkFixtureSoakResultPassesSpec(const LN_BenchmarkFixtureSoakResult &result,
                                             const LN_BenchmarkSoakSpec &soak);
bool LN_AutomatedBenchmarkAcceptanceGatePasses(const LN_BenchmarkProtocol &protocol,
                                               const LN_BenchmarkRelativeBaseline &baseline,
                                               const LN_BenchmarkSoakSpec &soak);
std::vector<LN_BenchmarkMeasuredRegressionDiagnostic>
LN_GetBenchmarkMeasuredRegressionDiagnostics(const LN_BenchmarkProtocol &protocol,
                                             const LN_BenchmarkRelativeBaseline &baseline);
bool LN_FinalRoadmapGateStatusIsComplete(const LN_FinalRoadmapGateStatus &status);
std::string LN_FormatBenchmarkRunProfileLine(const LN_BenchmarkFixtureRunSummary &summary,
                                             const LN_BenchmarkRunResult &result);
std::string LN_FormatBenchmarkSummaryJson(const LN_BenchmarkFixtureRunSummary &summary);
LN_RuntimeFeatureConfig LN_DefaultRuntimeFeatureConfig();
bool LN_RuntimeFeatureEnabled(const LN_RuntimeFeatureConfig &config, LN_RuntimeFeatureFlag feature);
bool LN_RuntimeGuardrailAssertionEnabled(const LN_RuntimeFeatureConfig &config,
                                         LN_RuntimeGuardrailAssertion assertion);
std::string LN_FormatTickProfileLine(const LN_TickProfileMetrics &metrics);
