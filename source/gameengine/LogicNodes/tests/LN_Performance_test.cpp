/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "LN_Performance.h"
#include "LN_Program.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <string>

#ifndef LOGIC_NODES_TEST_SOURCE_DIR
#  define LOGIC_NODES_TEST_SOURCE_DIR "."
#endif

namespace {

bool StringContains(const std::string &text, const std::string &needle)
{
  return text.find(needle) != std::string::npos;
}

uint32_t ScenarioBit(const LN_BenchmarkScenarioKind kind)
{
  return 1u << uint8_t(kind);
}

uint32_t EquivalenceBit(const LN_EquivalenceDomain domain)
{
  return 1u << uint8_t(domain);
}

void FillDirectOpcodeTestOperands(LN_Program &program, LN_Instruction &instruction)
{
  switch (instruction.opcode) {
    case LN_OpCode::SetGameProperty: {
      LN_GamePropertyRef property_ref;
      property_ref.name = "test_property";
      property_ref.value_type = LN_ValueType::Float;
      instruction.property_ref_index = program.AddGamePropertyRef(property_ref);
      instruction.property_value_type = LN_ValueType::Float;
      break;
    }
    case LN_OpCode::SetTreeProperty:
    case LN_OpCode::AddObject: {
      LN_TreePropertyRef property_ref;
      property_ref.name = "test_tree_property";
      property_ref.value_type = instruction.opcode == LN_OpCode::AddObject ?
                                    LN_ValueType::ObjectRef :
                                    LN_ValueType::Float;
      instruction.property_ref_index = program.AddTreePropertyRef(property_ref);
      break;
    }
    default:
      break;
  }
}

uint32_t ReplayDomainBit(const LN_EquivalenceDomain domain)
{
  switch (domain) {
    case LN_EquivalenceDomain::RuntimeExecution:
      return LN_BENCHMARK_REPLAY_RUNTIME_EXECUTION;
    case LN_EquivalenceDomain::EventOrder:
      return LN_BENCHMARK_REPLAY_EVENT_ORDER;
    case LN_EquivalenceDomain::CommandOrder:
      return LN_BENCHMARK_REPLAY_COMMAND_ORDER;
    case LN_EquivalenceDomain::SnapshotReads:
      return LN_BENCHMARK_REPLAY_SNAPSHOT_READS;
    case LN_EquivalenceDomain::SchedulerMergeOrder:
      return LN_BENCHMARK_REPLAY_SCHEDULER_MERGE_ORDER;
    case LN_EquivalenceDomain::Diagnostics:
      return LN_BENCHMARK_REPLAY_DIAGNOSTICS;
    case LN_EquivalenceDomain::Count:
      break;
  }
  return 0;
}

uint32_t ServiceFixtureBit(const LN_ServiceBlendFixtureKind kind)
{
  return 1u << uint8_t(kind);
}

bool BlendFileHasValidBinaryHeader(const char *path)
{
  if (path == nullptr) {
    return false;
  }

  auto open_file = [](const std::string &filepath) {
    return std::ifstream(filepath, std::ios::binary);
  };

  std::ifstream file = open_file(path);
  if (!file.good()) {
    file = open_file(std::string(LOGIC_NODES_TEST_SOURCE_DIR) + "/" + path);
  }
  if (!file.good()) {
    return false;
  }

  unsigned char header[7] = {};
  file.read(reinterpret_cast<char *>(header), sizeof(header));
  if (file.gcount() < 4) {
    return false;
  }

  const bool is_zstd = header[0] == 0x28 && header[1] == 0xb5 && header[2] == 0x2f &&
                       header[3] == 0xfd;
  const bool is_uncompressed_blend = file.gcount() == sizeof(header) &&
                                     std::string(reinterpret_cast<const char *>(header),
                                                 sizeof(header)) == "BLENDER";
  return is_zstd || is_uncompressed_blend;
}

void SetEnvironmentVariableForTest(const char *name, const char *value)
{
#ifdef _WIN32
  _putenv_s(name, value);
#else
  setenv(name, value, 1);
#endif
}

void UnsetEnvironmentVariableForTest(const char *name)
{
#ifdef _WIN32
  _putenv_s(name, "");
#else
  unsetenv(name);
#endif
}

class ScopedEnvironmentVariable {
 public:
  ScopedEnvironmentVariable(const char *name, const char *value) : m_name(name)
  {
    const char *old_value = std::getenv(name);
    if (old_value != nullptr) {
      m_hadValue = true;
      m_oldValue = old_value;
    }

    if (value != nullptr) {
      SetEnvironmentVariableForTest(name, value);
    }
    else {
      UnsetEnvironmentVariableForTest(name);
    }
  }

  ~ScopedEnvironmentVariable()
  {
    if (m_hadValue) {
      SetEnvironmentVariableForTest(m_name, m_oldValue.c_str());
    }
    else {
      UnsetEnvironmentVariableForTest(m_name);
    }
  }

 private:
  const char *m_name = nullptr;
  bool m_hadValue = false;
  std::string m_oldValue;
};

class ScopedDefaultRuntimeFeatureEnvironment {
 public:
  ScopedDefaultRuntimeFeatureEnvironment()
      : m_typedCommandStreams("UPBGE_LN_FEATURE_TYPED_COMMAND_STREAMS", nullptr),
        m_resourceClassScheduler("UPBGE_LN_FEATURE_RESOURCE_CLASS_SCHEDULER", nullptr),
        m_registerExpressionEvaluator("UPBGE_LN_FEATURE_REGISTER_EXPRESSION_EVALUATOR",
                                      nullptr),
        m_enableGuardrails("UPBGE_LN_ENABLE_GUARDRAIL_ASSERTIONS", nullptr),
        m_enableDualPathValidation("UPBGE_LN_ENABLE_DUAL_PATH_VALIDATION", nullptr),
        m_disableGuardrails("UPBGE_LN_DISABLE_GUARDRAIL_ASSERTIONS", nullptr),
        m_disableDualPathValidation("UPBGE_LN_DISABLE_DUAL_PATH_VALIDATION", nullptr),
        m_benchmarkSummaries("UPBGE_LN_BENCHMARK_SUMMARIES", nullptr)
  {
  }

 private:
  ScopedEnvironmentVariable m_typedCommandStreams;
  ScopedEnvironmentVariable m_resourceClassScheduler;
  ScopedEnvironmentVariable m_registerExpressionEvaluator;
  ScopedEnvironmentVariable m_enableGuardrails;
  ScopedEnvironmentVariable m_enableDualPathValidation;
  ScopedEnvironmentVariable m_disableGuardrails;
  ScopedEnvironmentVariable m_disableDualPathValidation;
  ScopedEnvironmentVariable m_benchmarkSummaries;
};

}  // namespace

TEST(LN_Performance, BenchmarkScenarioCatalogCoversRoadmapScenes)
{
  uint32_t scenario_mask = 0;
  const auto &scenarios = LN_GetBenchmarkScenarios();
  EXPECT_EQ(scenarios.size(), size_t(LN_BenchmarkScenarioKind::Count));

  for (const LN_BenchmarkScenario &scenario : scenarios) {
    EXPECT_NE(scenario.id, nullptr);
    EXPECT_NE(scenario.description, nullptr);
    EXPECT_STRNE(scenario.id, "");
    EXPECT_STRNE(scenario.description, "");
    EXPECT_TRUE(LN_ProfileMetricsCoverRequired(scenario.required_metrics));
    EXPECT_STRNE(LN_BenchmarkScenarioKindName(scenario.kind), "unknown");
    scenario_mask |= ScenarioBit(scenario.kind);
  }

  EXPECT_EQ(scenario_mask,
            ScenarioBit(LN_BenchmarkScenarioKind::ManySmallTrees) |
                ScenarioBit(LN_BenchmarkScenarioKind::FewHeavyTrees) |
                ScenarioBit(LN_BenchmarkScenarioKind::InstructionHeavy) |
                ScenarioBit(LN_BenchmarkScenarioKind::EventHeavy) |
                ScenarioBit(LN_BenchmarkScenarioKind::SnapshotHeavy) |
                ScenarioBit(LN_BenchmarkScenarioKind::CommandHeavy) |
                ScenarioBit(LN_BenchmarkScenarioKind::MathHeavy) |
                ScenarioBit(LN_BenchmarkScenarioKind::MixedSerialParallel) |
                ScenarioBit(LN_BenchmarkScenarioKind::TransformHeavy) |
                ScenarioBit(LN_BenchmarkScenarioKind::PhysicsQueryHeavy) |
                ScenarioBit(LN_BenchmarkScenarioKind::PropertyEventHeavy) |
                ScenarioBit(LN_BenchmarkScenarioKind::SpawnLifecycleHeavy));
}

TEST(LN_Performance, SyntheticBenchmarkDriversCoverPhaseZeroWorkloads)
{
  const auto &scenarios = LN_GetBenchmarkScenarios();
  const auto &drivers = LN_GetSyntheticBenchmarkDrivers();
  EXPECT_EQ(drivers.size(), scenarios.size());

  uint32_t driver_mask = 0;
  for (size_t index = 0; index < drivers.size(); index++) {
    const LN_SyntheticBenchmarkDriver &driver = drivers[index];
    const LN_BenchmarkScenario &scenario = scenarios[index];
    EXPECT_EQ(driver.scenario, scenario.kind);
    EXPECT_TRUE(LN_BenchmarkDriverMatchesScenario(driver, scenario)) << driver.id;
    driver_mask |= ScenarioBit(driver.scenario);
  }

  EXPECT_NE(driver_mask & ScenarioBit(LN_BenchmarkScenarioKind::ManySmallTrees), 0u);
  EXPECT_NE(driver_mask & ScenarioBit(LN_BenchmarkScenarioKind::InstructionHeavy), 0u);
  EXPECT_NE(driver_mask & ScenarioBit(LN_BenchmarkScenarioKind::EventHeavy), 0u);
  EXPECT_NE(driver_mask & ScenarioBit(LN_BenchmarkScenarioKind::SnapshotHeavy), 0u);
  EXPECT_NE(driver_mask & ScenarioBit(LN_BenchmarkScenarioKind::CommandHeavy), 0u);
  EXPECT_NE(driver_mask & ScenarioBit(LN_BenchmarkScenarioKind::MathHeavy), 0u);
  EXPECT_NE(driver_mask & ScenarioBit(LN_BenchmarkScenarioKind::MixedSerialParallel), 0u);
}

TEST(LN_Performance, EngineOwnedBenchmarkFixturesReplaceUserSceneRequirement)
{
  const LN_BenchmarkProtocol &protocol = LN_GetBenchmarkProtocol();
  const auto &drivers = LN_GetSyntheticBenchmarkDrivers();
  const auto &fixtures = LN_GetBenchmarkFixtureSpecs();
  ASSERT_EQ(fixtures.size(), drivers.size());

  uint32_t fixture_mask = 0;
  for (size_t index = 0; index < fixtures.size(); index++) {
    const LN_BenchmarkFixtureSpec &fixture = fixtures[index];
    const LN_SyntheticBenchmarkDriver &driver = drivers[index];

    EXPECT_EQ(fixture.scenario, driver.scenario);
    EXPECT_TRUE(LN_BenchmarkFixtureMatchesDriver(fixture, driver, protocol)) << fixture.id;
    EXPECT_EQ(fixture.source, LN_BenchmarkFixtureSource::CppSyntheticGraph) << fixture.id;
    EXPECT_STREQ(LN_BenchmarkFixtureSourceName(fixture.source), "cpp_synthetic_graph");
    EXPECT_EQ(fixture.blend_fixture_path, nullptr) << fixture.id;
    EXPECT_FALSE(fixture.user_scene_required) << fixture.id;
    EXPECT_NE(fixture.engine_service_boundary, nullptr);
    EXPECT_NE(fixture.deterministic_seed, 0u);
    for (size_t previous_index = 0; previous_index < index; previous_index++) {
      EXPECT_NE(fixture.deterministic_seed, fixtures[previous_index].deterministic_seed)
          << fixture.id;
    }
    EXPECT_NE(fixture.replay_domain_mask & LN_BENCHMARK_REPLAY_RUNTIME_EXECUTION, 0u)
        << fixture.id;
    EXPECT_TRUE(fixture.produces_legacy_mode);
    EXPECT_TRUE(fixture.produces_optimized_mode);

    fixture_mask |= ScenarioBit(fixture.scenario);
  }

  EXPECT_EQ(fixture_mask,
            ScenarioBit(LN_BenchmarkScenarioKind::ManySmallTrees) |
                ScenarioBit(LN_BenchmarkScenarioKind::FewHeavyTrees) |
                ScenarioBit(LN_BenchmarkScenarioKind::InstructionHeavy) |
                ScenarioBit(LN_BenchmarkScenarioKind::EventHeavy) |
                ScenarioBit(LN_BenchmarkScenarioKind::SnapshotHeavy) |
                ScenarioBit(LN_BenchmarkScenarioKind::CommandHeavy) |
                ScenarioBit(LN_BenchmarkScenarioKind::MathHeavy) |
                ScenarioBit(LN_BenchmarkScenarioKind::MixedSerialParallel) |
                ScenarioBit(LN_BenchmarkScenarioKind::TransformHeavy) |
                ScenarioBit(LN_BenchmarkScenarioKind::PhysicsQueryHeavy) |
                ScenarioBit(LN_BenchmarkScenarioKind::PropertyEventHeavy) |
                ScenarioBit(LN_BenchmarkScenarioKind::SpawnLifecycleHeavy));
}

TEST(LN_Performance, ServiceBlendFixturesCoverEngineServiceBoundaries)
{
  const auto &fixtures = LN_GetServiceBlendFixtureSpecs();
  ASSERT_EQ(fixtures.size(), size_t(LN_ServiceBlendFixtureKind::Count));

  uint32_t fixture_mask = 0;
  for (const LN_ServiceBlendFixtureSpec &fixture : fixtures) {
    EXPECT_NE(fixture.id, nullptr);
    EXPECT_STRNE(fixture.id, "");
    EXPECT_NE(fixture.blend_fixture_path, nullptr) << fixture.id;
    EXPECT_STRNE(fixture.blend_fixture_path, "") << fixture.id;
    EXPECT_TRUE(StringContains(fixture.blend_fixture_path, ".blend")) << fixture.blend_fixture_path;
    EXPECT_TRUE(BlendFileHasValidBinaryHeader(fixture.blend_fixture_path))
        << fixture.blend_fixture_path;
    EXPECT_NE(fixture.engine_service_boundary, nullptr) << fixture.id;
    EXPECT_STRNE(fixture.engine_service_boundary, "") << fixture.id;
    EXPECT_FALSE(fixture.user_scene_required) << fixture.id;
    EXPECT_TRUE(fixture.validates_legacy_mode) << fixture.id;
    EXPECT_TRUE(fixture.validates_optimized_mode) << fixture.id;
    EXPECT_NE(fixture.replay_domain_mask & LN_BENCHMARK_REPLAY_RUNTIME_EXECUTION, 0u)
        << fixture.id;
    EXPECT_NE(fixture.replay_domain_mask & LN_BENCHMARK_REPLAY_COMMAND_ORDER, 0u)
        << fixture.id;
    EXPECT_NE(fixture.replay_domain_mask & LN_BENCHMARK_REPLAY_DIAGNOSTICS, 0u)
        << fixture.id;
    EXPECT_STRNE(LN_ServiceBlendFixtureKindName(fixture.kind), "unknown") << fixture.id;

    fixture_mask |= ServiceFixtureBit(fixture.kind);
  }

  EXPECT_EQ(fixture_mask,
            ServiceFixtureBit(LN_ServiceBlendFixtureKind::PhysicsWorld) |
                ServiceFixtureBit(LN_ServiceBlendFixtureKind::CharacterController) |
                ServiceFixtureBit(LN_ServiceBlendFixtureKind::AudioDevice) |
                ServiceFixtureBit(LN_ServiceBlendFixtureKind::MaterialDatablock) |
                ServiceFixtureBit(LN_ServiceBlendFixtureKind::SceneObjectLifecycle) |
                ServiceFixtureBit(LN_ServiceBlendFixtureKind::BlenderPlayerRestartLoad));
}

TEST(LN_Performance, BenchmarkProtocolAndBaselineExpectationsAreDeterministic)
{
  const LN_BenchmarkProtocol &protocol = LN_GetBenchmarkProtocol();
  EXPECT_STREQ(protocol.build_command, "make NPROCS=32");
  EXPECT_GT(protocol.fixed_dt, 0.0);
  EXPECT_GT(protocol.warmup_frame_count, 0u);
  EXPECT_GT(protocol.measured_frame_count, protocol.warmup_frame_count);
  EXPECT_TRUE(protocol.run_serial);
  EXPECT_TRUE(protocol.run_parallel);
  EXPECT_TRUE(protocol.report_median);
  EXPECT_TRUE(protocol.report_p95);
  EXPECT_TRUE(protocol.report_worst_frame);

  const auto &baselines = LN_GetBenchmarkBaselineExpectations();
  EXPECT_EQ(baselines.size(), size_t(LN_BenchmarkScenarioKind::Count));
  for (const LN_BenchmarkBaselineExpectation &baseline : baselines) {
    EXPECT_TRUE(LN_BenchmarkBaselineMatchesProtocol(baseline, protocol));
  }
}

TEST(LN_Performance, ReplayTraceCatalogCoversCommittedFixtureDomains)
{
  const auto &traces = LN_GetBenchmarkReplayTraceSpecs();
  ASSERT_EQ(traces.size(), size_t(LN_EquivalenceDomain::Count));

  uint32_t domain_mask = 0;
  for (const LN_BenchmarkReplayTraceSpec &trace : traces) {
    EXPECT_TRUE(LN_BenchmarkReplayTraceCoversDomain(trace, trace.domain)) << trace.id;
    EXPECT_NE(trace.scenario_mask, 0u);
    EXPECT_TRUE(trace.compares_legacy_and_optimized);
    EXPECT_TRUE(trace.deterministic_checksum_required);
    EXPECT_STRNE(LN_EquivalenceDomainName(trace.domain), "unknown");
    domain_mask |= EquivalenceBit(trace.domain);
  }

  for (const LN_BenchmarkFixtureSpec &fixture : LN_GetBenchmarkFixtureSpecs()) {
    for (const LN_BenchmarkReplayTraceSpec &trace : traces) {
      if ((trace.scenario_mask & ScenarioBit(fixture.scenario)) == 0u) {
        continue;
      }
      EXPECT_NE(fixture.replay_domain_mask & ReplayDomainBit(trace.domain), 0u)
          << fixture.id << " missing replay domain " << trace.id;
    }
  }

  EXPECT_EQ(domain_mask,
            EquivalenceBit(LN_EquivalenceDomain::RuntimeExecution) |
                EquivalenceBit(LN_EquivalenceDomain::EventOrder) |
                EquivalenceBit(LN_EquivalenceDomain::CommandOrder) |
                EquivalenceBit(LN_EquivalenceDomain::SnapshotReads) |
                EquivalenceBit(LN_EquivalenceDomain::SchedulerMergeOrder) |
                EquivalenceBit(LN_EquivalenceDomain::Diagnostics));
}

TEST(LN_Performance, BenchmarkRunnerExecutesFixturesInRequiredModesAndEmitsSummaries)
{
  const LN_BenchmarkProtocol &protocol = LN_GetBenchmarkProtocol();
  const auto &drivers = LN_GetSyntheticBenchmarkDrivers();
  const auto &fixtures = LN_GetBenchmarkFixtureSpecs();
  ASSERT_EQ(fixtures.size(), drivers.size());

  for (size_t index = 0; index < fixtures.size(); index++) {
    const LN_BenchmarkFixtureRunSummary summary = LN_RunBenchmarkFixture(
        fixtures[index], drivers[index], protocol);
    EXPECT_TRUE(LN_BenchmarkRunSummaryMatchesProtocol(summary, protocol)) << fixtures[index].id;
    EXPECT_EQ(summary.run_count, uint32_t(LN_BenchmarkRunMode::Count));
    EXPECT_TRUE(summary.emitted_json_summary);
    EXPECT_TRUE(summary.emitted_profile_line_summaries);

    const std::string json = LN_FormatBenchmarkSummaryJson(summary);
    EXPECT_TRUE(StringContains(json, "\"fixture\""));
    EXPECT_TRUE(StringContains(json, "\"scenario\""));
    EXPECT_TRUE(StringContains(json, "\"legacy_serial\""));
    EXPECT_TRUE(StringContains(json, "\"legacy_parallel\""));
    EXPECT_TRUE(StringContains(json, "\"optimized_serial\""));
    EXPECT_TRUE(StringContains(json, "\"optimized_parallel\""));
    EXPECT_TRUE(StringContains(json, "\"median_ms\""));
    EXPECT_TRUE(StringContains(json, "\"p95_ms\""));
    EXPECT_TRUE(StringContains(json, "\"worst_ms\""));
    EXPECT_TRUE(StringContains(json, "\"fallback_paths\""));

    for (const LN_BenchmarkRunResult &result : summary.runs) {
      ASSERT_TRUE(result.valid);
      EXPECT_STRNE(LN_BenchmarkRunModeName(result.mode), "unknown");
      EXPECT_EQ(result.warmup_frame_count, protocol.warmup_frame_count);
      EXPECT_EQ(result.measured_frame_count, protocol.measured_frame_count);
      EXPECT_TRUE(result.runtime_fixture_measured);
      EXPECT_GT(result.runtime_fixture_tick_count, 0u);
      EXPECT_GT(result.runtime_fixture_tree_count, 0u);
      if (fixtures[index].scenario == LN_BenchmarkScenarioKind::ManySmallTrees) {
        EXPECT_EQ(result.runtime_fixture_tree_count, drivers[index].tree_count);
        EXPECT_GE(result.runtime_fixture_tree_count, 4096u);
      }
      EXPECT_GT(result.runtime_fixture_instruction_count, 0u);
      EXPECT_TRUE(result.runtime_fixture_record_phase_measured);
      EXPECT_TRUE(result.runtime_fixture_command_flush_measured);
      EXPECT_FALSE(result.runtime_fixture_service_boundary_measured);
      EXPECT_FALSE(result.formula_timing_estimate_used);
      EXPECT_GT(result.median_ms, 0.0);
      EXPECT_GE(result.p95_ms, result.median_ms);
      EXPECT_GE(result.worst_frame_ms, result.p95_ms);

      const std::string profile_line = LN_FormatBenchmarkRunProfileLine(summary, result);
      EXPECT_TRUE(StringContains(profile_line, "Logic Nodes benchmark:"));
      EXPECT_TRUE(StringContains(profile_line, "fixture="));
      EXPECT_TRUE(StringContains(profile_line, "mode="));
      EXPECT_TRUE(StringContains(profile_line, "median_ms="));
      EXPECT_TRUE(StringContains(profile_line, "runtime_fixture_measured=1"));
      EXPECT_TRUE(StringContains(profile_line, "runtime_fixture_record_phase=1"));
      EXPECT_TRUE(StringContains(profile_line, "runtime_fixture_command_flush=1"));
      EXPECT_TRUE(StringContains(profile_line, "runtime_fixture_service_boundary=0"));
      EXPECT_TRUE(StringContains(profile_line, "runtime_fixture_ticks="));
      EXPECT_TRUE(StringContains(profile_line, "formula_timing_estimate=0"));
      EXPECT_TRUE(StringContains(profile_line, "Logic Nodes tick:"));
    }

    EXPECT_TRUE(StringContains(json, "\"runtime_fixture_measured\""));
    EXPECT_TRUE(StringContains(json, "\"runtime_fixture_record_phase_measured\""));
    EXPECT_TRUE(StringContains(json, "\"runtime_fixture_command_flush_measured\""));
    EXPECT_TRUE(StringContains(json, "\"runtime_fixture_service_boundary_measured\""));
    EXPECT_TRUE(StringContains(json, "\"runtime_fixture_ticks\""));
    EXPECT_TRUE(StringContains(json, "\"runtime_fixture_instructions\""));
    EXPECT_TRUE(StringContains(json, "\"formula_timing_estimate_used\""));
  }
}

TEST(LN_Performance, RelativeBaselinesGateBenchmarkSummaries)
{
  const LN_BenchmarkProtocol &protocol = LN_GetBenchmarkProtocol();
  const auto &baselines = LN_GetBenchmarkRelativeBaselines();
  ASSERT_EQ(baselines.size(), size_t(LN_BenchmarkPlatform::Count));

  bool found_linux_ci_gate = false;
  for (const LN_BenchmarkRelativeBaseline &baseline : baselines) {
    EXPECT_TRUE(LN_BenchmarkRelativeBaselineMatchesProtocol(baseline, protocol));
    EXPECT_STRNE(LN_BenchmarkPlatformName(baseline.platform), "unknown");
    if (baseline.platform == LN_BenchmarkPlatform::Linux) {
      EXPECT_TRUE(baseline.ci_gate_enabled);
      found_linux_ci_gate = true;
    }
  }
  EXPECT_TRUE(found_linux_ci_gate);

  const LN_BenchmarkRelativeBaseline &linux_baseline = baselines[size_t(LN_BenchmarkPlatform::Linux)];
  const auto &drivers = LN_GetSyntheticBenchmarkDrivers();
  const auto &fixtures = LN_GetBenchmarkFixtureSpecs();
  uint32_t measured_pass_count = 0;
  for (size_t index = 0; index < fixtures.size(); index++) {
    const LN_BenchmarkFixtureRunSummary summary = LN_RunBenchmarkFixture(
        fixtures[index], drivers[index], protocol);
    EXPECT_TRUE(LN_BenchmarkRunSummaryMatchesProtocol(summary, protocol)) << fixtures[index].id;
    for (const LN_BenchmarkRunResult &result : summary.runs) {
      if (result.valid) {
        EXPECT_TRUE(result.runtime_fixture_measured) << fixtures[index].id;
        EXPECT_TRUE(result.runtime_fixture_record_phase_measured) << fixtures[index].id;
        EXPECT_TRUE(result.runtime_fixture_command_flush_measured) << fixtures[index].id;
        EXPECT_FALSE(result.runtime_fixture_service_boundary_measured) << fixtures[index].id;
        EXPECT_FALSE(result.formula_timing_estimate_used) << fixtures[index].id;
      }
    }
    if (LN_BenchmarkRunSummaryPassesRelativeBaseline(summary, linux_baseline)) {
      measured_pass_count++;
    }
    EXPECT_TRUE(LN_BenchmarkRunSummaryPassesRelativeBaseline(summary, linux_baseline))
        << fixtures[index].id;
  }
  EXPECT_EQ(measured_pass_count, uint32_t(fixtures.size()));

  LN_BenchmarkFixtureRunSummary regressed_summary = LN_RunBenchmarkFixture(
      fixtures[size_t(LN_BenchmarkScenarioKind::InstructionHeavy)],
      drivers[size_t(LN_BenchmarkScenarioKind::InstructionHeavy)],
      protocol);
  regressed_summary.runs[size_t(LN_BenchmarkRunMode::OptimizedSerial)].median_ms =
      regressed_summary.runs[size_t(LN_BenchmarkRunMode::LegacySerial)].median_ms * 2.0;
  EXPECT_FALSE(LN_BenchmarkRunSummaryPassesRelativeBaseline(regressed_summary, linux_baseline));
}

TEST(LN_Performance, HotPathGateRejectsUnexpectedOptimizedWarmTickWork)
{
  const LN_BenchmarkProtocol &protocol = LN_GetBenchmarkProtocol();
  const auto &drivers = LN_GetSyntheticBenchmarkDrivers();
  const auto &fixtures = LN_GetBenchmarkFixtureSpecs();
  ASSERT_EQ(fixtures.size(), drivers.size());

  for (size_t index = 0; index < fixtures.size(); index++) {
    const LN_BenchmarkFixtureRunSummary summary = LN_RunBenchmarkFixture(
        fixtures[index], drivers[index], protocol);
    EXPECT_TRUE(LN_BenchmarkRunSummaryPassesHotPathGate(summary)) << fixtures[index].id;
  }

  LN_BenchmarkFixtureRunSummary regressed_summary = LN_RunBenchmarkFixture(
      fixtures[size_t(LN_BenchmarkScenarioKind::CommandHeavy)],
      drivers[size_t(LN_BenchmarkScenarioKind::CommandHeavy)],
      protocol);
  ASSERT_TRUE(LN_BenchmarkRunSummaryPassesHotPathGate(regressed_summary));

  LN_BenchmarkFixtureRunSummary allocation_regression = regressed_summary;
  allocation_regression.runs[size_t(LN_BenchmarkRunMode::OptimizedSerial)].metrics.allocation_count =
      1u;
  EXPECT_FALSE(LN_BenchmarkRunSummaryPassesHotPathGate(allocation_regression));

  LN_BenchmarkFixtureRunSummary string_regression = regressed_summary;
  string_regression.runs[size_t(LN_BenchmarkRunMode::OptimizedSerial)].metrics.string_copy_count =
      1u;
  EXPECT_FALSE(LN_BenchmarkRunSummaryPassesHotPathGate(string_regression));

  LN_BenchmarkFixtureRunSummary lookup_regression = regressed_summary;
  lookup_regression.runs[size_t(LN_BenchmarkRunMode::OptimizedParallel)]
      .metrics.hot_map_lookup_count = 1u;
  EXPECT_FALSE(LN_BenchmarkRunSummaryPassesHotPathGate(lookup_regression));

  LN_BenchmarkFixtureRunSummary event_scan_regression = regressed_summary;
  event_scan_regression.runs[size_t(LN_BenchmarkRunMode::OptimizedParallel)]
      .metrics.event_fallback_scan_count = 1u;
  EXPECT_FALSE(LN_BenchmarkRunSummaryPassesHotPathGate(event_scan_regression));

  LN_BenchmarkFixtureRunSummary legacy_command_regression = regressed_summary;
  legacy_command_regression.runs[size_t(LN_BenchmarkRunMode::OptimizedSerial)]
      .metrics.command_legacy_count = 1u;
  EXPECT_FALSE(LN_BenchmarkRunSummaryPassesHotPathGate(legacy_command_regression));

  LN_BenchmarkFixtureRunSummary full_snapshot_regression = regressed_summary;
  LN_TickProfileMetrics &snapshot_metrics =
      full_snapshot_regression.runs[size_t(LN_BenchmarkRunMode::OptimizedParallel)].metrics;
  snapshot_metrics.snapshot_tree_count = 1u;
  snapshot_metrics.snapshot_captured_channel_mask = 0xffu;
  snapshot_metrics.snapshot_skipped_channel_mask = 0u;
  EXPECT_FALSE(LN_BenchmarkRunSummaryPassesHotPathGate(full_snapshot_regression));
}

TEST(LN_Performance, SimdBenefitGateRequiresMeasuredMathHeavySimdLanes)
{
  const LN_BenchmarkProtocol &protocol = LN_GetBenchmarkProtocol();
  const LN_BenchmarkRelativeBaseline &linux_baseline =
      LN_GetBenchmarkRelativeBaselines()[size_t(LN_BenchmarkPlatform::Linux)];
  const auto &drivers = LN_GetSyntheticBenchmarkDrivers();
  const auto &fixtures = LN_GetBenchmarkFixtureSpecs();
  const size_t math_index = size_t(LN_BenchmarkScenarioKind::MathHeavy);

  LN_BenchmarkFixtureRunSummary summary = LN_RunBenchmarkFixture(
      fixtures[math_index], drivers[math_index], protocol);
  EXPECT_TRUE(LN_BenchmarkRunSummaryPassesSimdBenefitGate(summary, linux_baseline));

  const LN_BenchmarkRunResult &optimized_serial =
      summary.runs[size_t(LN_BenchmarkRunMode::OptimizedSerial)];
  const LN_BenchmarkRunResult &optimized_parallel =
      summary.runs[size_t(LN_BenchmarkRunMode::OptimizedParallel)];
  EXPECT_GT(optimized_serial.metrics.register_simd_batch_count, 0u);
  EXPECT_GE(optimized_serial.metrics.register_simd_lane_count, 4u);
  EXPECT_GT(optimized_parallel.metrics.register_simd_batch_count, 0u);
  EXPECT_GE(optimized_parallel.metrics.register_simd_lane_count, 4u);

  LN_BenchmarkFixtureRunSummary simd_disabled = summary;
  for (LN_BenchmarkRunMode mode :
       {LN_BenchmarkRunMode::OptimizedSerial, LN_BenchmarkRunMode::OptimizedParallel})
  {
    LN_TickProfileMetrics &metrics = simd_disabled.runs[size_t(mode)].metrics;
    metrics.register_simd_batch_count = 0u;
    metrics.register_simd_lane_count = 0u;
  }
  EXPECT_FALSE(LN_BenchmarkRunSummaryPassesSimdBenefitGate(simd_disabled, linux_baseline));

  LN_BenchmarkFixtureRunSummary non_math_summary = LN_RunBenchmarkFixture(
      fixtures[size_t(LN_BenchmarkScenarioKind::InstructionHeavy)],
      drivers[size_t(LN_BenchmarkScenarioKind::InstructionHeavy)],
      protocol);
  EXPECT_TRUE(LN_BenchmarkRunSummaryPassesSimdBenefitGate(non_math_summary, linux_baseline));
}

TEST(LN_Performance, EquivalenceHarnessCatalogCoversRuntimeBoundaries)
{
  uint32_t domain_mask = 0;
  for (const LN_EquivalenceCheckSpec &spec : LN_GetEquivalenceCheckSpecs()) {
    EXPECT_NE(spec.id, nullptr);
    EXPECT_NE(spec.description, nullptr);
    EXPECT_STRNE(spec.id, "");
    EXPECT_STRNE(spec.description, "");
    EXPECT_TRUE(spec.requires_legacy_path);
    EXPECT_TRUE(spec.requires_deterministic_replay);
    EXPECT_STRNE(LN_EquivalenceDomainName(spec.domain), "unknown");
    domain_mask |= EquivalenceBit(spec.domain);
  }

  EXPECT_EQ(domain_mask,
            EquivalenceBit(LN_EquivalenceDomain::RuntimeExecution) |
                EquivalenceBit(LN_EquivalenceDomain::EventOrder) |
                EquivalenceBit(LN_EquivalenceDomain::CommandOrder) |
                EquivalenceBit(LN_EquivalenceDomain::SnapshotReads) |
                EquivalenceBit(LN_EquivalenceDomain::SchedulerMergeOrder) |
                EquivalenceBit(LN_EquivalenceDomain::Diagnostics));
}

TEST(LN_Performance, RuntimeFeatureTogglesDefaultToDisabledForOptionalPaths)
{
  const LN_RuntimeFeatureConfig defaults;
  EXPECT_FALSE(LN_RuntimeFeatureEnabled(defaults, LN_RUNTIME_FEATURE_TYPED_COMMAND_STREAMS));
  EXPECT_FALSE(LN_RuntimeFeatureEnabled(defaults, LN_RUNTIME_FEATURE_RESOURCE_CLASS_SCHEDULER));
  EXPECT_FALSE(
      LN_RuntimeFeatureEnabled(defaults, LN_RUNTIME_FEATURE_REGISTER_EXPRESSION_EVALUATOR));
  EXPECT_FALSE(defaults.debug_assertions);
  EXPECT_FALSE(defaults.dual_path_validation);

  for (uint8_t value = 0; value < uint8_t(LN_RuntimeGuardrailAssertion::Count); value++) {
    const LN_RuntimeGuardrailAssertion assertion = LN_RuntimeGuardrailAssertion(value);
    EXPECT_FALSE(LN_RuntimeGuardrailAssertionEnabled(defaults, assertion));
  }
}

TEST(LN_Performance, DefaultRuntimeFeatureConfigEnablesStableOptimizedPaths)
{
  const ScopedDefaultRuntimeFeatureEnvironment scoped_env;
  const LN_RuntimeFeatureConfig defaults = LN_DefaultRuntimeFeatureConfig();

  EXPECT_TRUE(LN_RuntimeFeatureEnabled(defaults, LN_RUNTIME_FEATURE_TYPED_COMMAND_STREAMS));
  EXPECT_TRUE(LN_RuntimeFeatureEnabled(defaults, LN_RUNTIME_FEATURE_RESOURCE_CLASS_SCHEDULER));
  EXPECT_TRUE(
      LN_RuntimeFeatureEnabled(defaults, LN_RUNTIME_FEATURE_REGISTER_EXPRESSION_EVALUATOR));
  EXPECT_FALSE(defaults.debug_assertions);
  EXPECT_FALSE(defaults.dual_path_validation);
  EXPECT_FALSE(defaults.benchmark_profile_summaries);
}

TEST(LN_Performance, DefaultRuntimeFeatureConfigAllowsOptInDeveloperValidation)
{
  const ScopedDefaultRuntimeFeatureEnvironment scoped_env;
  const ScopedEnvironmentVariable enable_guardrails("UPBGE_LN_ENABLE_GUARDRAIL_ASSERTIONS", "1");
  const ScopedEnvironmentVariable enable_dual_path("UPBGE_LN_ENABLE_DUAL_PATH_VALIDATION", "1");
  const LN_RuntimeFeatureConfig defaults = LN_DefaultRuntimeFeatureConfig();

  EXPECT_TRUE(defaults.debug_assertions);
  EXPECT_TRUE(defaults.dual_path_validation);

  for (uint8_t value = 0; value < uint8_t(LN_RuntimeGuardrailAssertion::Count); value++) {
    const LN_RuntimeGuardrailAssertion assertion = LN_RuntimeGuardrailAssertion(value);
    EXPECT_TRUE(LN_RuntimeGuardrailAssertionEnabled(defaults, assertion));
  }
}

TEST(LN_Performance, DefaultRuntimeFeatureConfigDisableValidationOverridesOptIn)
{
  const ScopedDefaultRuntimeFeatureEnvironment scoped_env;
  const ScopedEnvironmentVariable enable_guardrails("UPBGE_LN_ENABLE_GUARDRAIL_ASSERTIONS", "1");
  const ScopedEnvironmentVariable enable_dual_path("UPBGE_LN_ENABLE_DUAL_PATH_VALIDATION", "1");
  const ScopedEnvironmentVariable disable_guardrails("UPBGE_LN_DISABLE_GUARDRAIL_ASSERTIONS", "1");
  const ScopedEnvironmentVariable disable_dual_path("UPBGE_LN_DISABLE_DUAL_PATH_VALIDATION", "1");
  const LN_RuntimeFeatureConfig defaults = LN_DefaultRuntimeFeatureConfig();

  EXPECT_FALSE(defaults.debug_assertions);
  EXPECT_FALSE(defaults.dual_path_validation);
}

TEST(LN_Performance, DefaultRuntimeFeatureConfigAllowsEmergencyFeatureDisableOverrides)
{
  const ScopedDefaultRuntimeFeatureEnvironment scoped_env;
  const ScopedEnvironmentVariable disable_register_eval(
      "UPBGE_LN_FEATURE_REGISTER_EXPRESSION_EVALUATOR", "off");
  const ScopedEnvironmentVariable disable_command_streams("UPBGE_LN_FEATURE_TYPED_COMMAND_STREAMS",
                                                         "false");
  const LN_RuntimeFeatureConfig defaults = LN_DefaultRuntimeFeatureConfig();

  EXPECT_FALSE(LN_RuntimeFeatureEnabled(defaults, LN_RUNTIME_FEATURE_TYPED_COMMAND_STREAMS));
  EXPECT_TRUE(LN_RuntimeFeatureEnabled(defaults, LN_RUNTIME_FEATURE_RESOURCE_CLASS_SCHEDULER));
  EXPECT_FALSE(
      LN_RuntimeFeatureEnabled(defaults, LN_RUNTIME_FEATURE_REGISTER_EXPRESSION_EVALUATOR));
}

TEST(LN_Performance, ReleaseModeSoakReplaysCommittedFixturesDeterministically)
{
  const LN_BenchmarkProtocol &protocol = LN_GetBenchmarkProtocol();
  const LN_BenchmarkSoakSpec &soak = LN_GetBenchmarkSoakSpec();
  const auto &drivers = LN_GetSyntheticBenchmarkDrivers();
  const auto &fixtures = LN_GetBenchmarkFixtureSpecs();
  ASSERT_EQ(fixtures.size(), drivers.size());

  EXPECT_NE(soak.id, nullptr);
  EXPECT_GE(soak.tick_count, protocol.warmup_frame_count + protocol.measured_frame_count);
  EXPECT_TRUE(soak.release_mode_required);
  EXPECT_TRUE(soak.compares_legacy_and_optimized);
  EXPECT_TRUE(soak.compares_serial_and_parallel);
  EXPECT_TRUE(soak.compares_command_outputs);
  EXPECT_TRUE(soak.compares_event_outputs);
  EXPECT_TRUE(soak.compares_snapshot_outputs);
  EXPECT_TRUE(soak.deterministic_checksum_required);

  for (size_t index = 0; index < fixtures.size(); index++) {
    const LN_BenchmarkFixtureSoakResult result = LN_RunBenchmarkFixtureSoak(
        fixtures[index], drivers[index], protocol, soak);
    EXPECT_TRUE(LN_BenchmarkFixtureSoakResultPassesSpec(result, soak)) << fixtures[index].id;
    EXPECT_EQ(result.legacy_checksum, result.optimized_checksum) << fixtures[index].id;
    EXPECT_EQ(result.serial_checksum, result.parallel_checksum) << fixtures[index].id;
    EXPECT_TRUE(result.command_outputs_match) << fixtures[index].id;
    EXPECT_TRUE(result.event_outputs_match) << fixtures[index].id;
    EXPECT_TRUE(result.snapshot_outputs_match) << fixtures[index].id;
  }
}

TEST(LN_Performance, RuntimeFallbackDiagnosticsCoverRequiredProductionReasons)
{
  const auto &diagnostics = LN_GetRuntimeFallbackDiagnosticSpecs();
  ASSERT_EQ(diagnostics.size(), size_t(LN_RuntimeFallbackReason::Count));

  uint32_t reason_mask = 0;
  for (const LN_RuntimeFallbackDiagnosticSpec &diagnostic : diagnostics) {
    EXPECT_TRUE(LN_RuntimeFallbackDiagnosticCoversReason(diagnostic, diagnostic.reason))
        << diagnostic.id;
    EXPECT_TRUE(diagnostic.warning_required_for_hotspot) << diagnostic.id;
    EXPECT_STRNE(LN_RuntimeFallbackReasonName(diagnostic.reason), "unknown");
    reason_mask |= 1u << uint8_t(diagnostic.reason);
  }

  EXPECT_EQ(reason_mask,
            (1u << uint8_t(LN_RuntimeFallbackReason::UnsupportedOpcode)) |
                (1u << uint8_t(LN_RuntimeFallbackReason::UnsupportedExpression)) |
                (1u << uint8_t(LN_RuntimeFallbackReason::DynamicLookup)) |
                (1u << uint8_t(LN_RuntimeFallbackReason::MainThreadLiveRead)) |
                (1u << uint8_t(LN_RuntimeFallbackReason::CommandBarrier)) |
                (1u << uint8_t(LN_RuntimeFallbackReason::UnsafeResourceClass)) |
                (1u << uint8_t(LN_RuntimeFallbackReason::MissingSnapshotChannel)) |
                (1u << uint8_t(LN_RuntimeFallbackReason::StaleHandle)) |
                (1u << uint8_t(LN_RuntimeFallbackReason::EngineServiceBoundary)));
}

TEST(LN_Performance, RuntimeFeatureFlagsExposeOptionalRuntimeOverrides)
{
  const auto &statuses = LN_GetRuntimeFeatureFlagStatuses();

  for (const LN_RuntimeFeatureFlagStatus &status : statuses) {
    EXPECT_TRUE(status.default_enabled) << status.feature_name;
    EXPECT_FALSE(status.required_for_user_activation) << status.feature_name;
    EXPECT_TRUE(status.emergency_override) << status.feature_name;
    EXPECT_NE(status.owner, nullptr) << status.feature_name;
    EXPECT_STRNE(status.owner, "") << status.feature_name;
    EXPECT_NE(status.diagnostic_purpose, nullptr) << status.feature_name;
    EXPECT_STRNE(status.diagnostic_purpose, "") << status.feature_name;
    EXPECT_NE(status.removal_condition, nullptr) << status.feature_name;
    EXPECT_STRNE(status.removal_condition, "") << status.feature_name;
    EXPECT_NE(status.removal_review_date, nullptr) << status.feature_name;
    EXPECT_STRNE(status.removal_review_date, "") << status.feature_name;
  }
}

TEST(LN_Performance, AcceptanceTargetsAreCIReadyAndDoNotRequireUserScenes)
{
  const auto &targets = LN_GetAcceptanceTargetSpecs();
  ASSERT_EQ(targets.size(), size_t(LN_AcceptanceTargetKind::Count));

  uint32_t target_mask = 0;
  for (const LN_AcceptanceTargetSpec &target : targets) {
    EXPECT_TRUE(LN_AcceptanceTargetMatchesPhaseNineContract(target)) << target.id;
    EXPECT_STRNE(LN_AcceptanceTargetKindName(target.kind), "unknown");
    target_mask |= 1u << uint8_t(target.kind);
  }

  EXPECT_EQ(target_mask,
            (1u << uint8_t(LN_AcceptanceTargetKind::FocusedGTest)) |
                (1u << uint8_t(LN_AcceptanceTargetKind::RuntimeAcceptanceCTest)) |
                (1u << uint8_t(LN_AcceptanceTargetKind::AutomatedBenchmarkGate)) |
                (1u << uint8_t(LN_AcceptanceTargetKind::DeterministicReplaySoak)) |
                (1u << uint8_t(LN_AcceptanceTargetKind::ServiceFixtureCTest)));
}

TEST(LN_Performance, AutomatedBenchmarkAcceptanceGatePassesCommittedMeasuredFixtures)
{
  const LN_BenchmarkProtocol &protocol = LN_GetBenchmarkProtocol();
  const LN_BenchmarkRelativeBaseline &linux_baseline =
      LN_GetBenchmarkRelativeBaselines()[size_t(LN_BenchmarkPlatform::Linux)];
  const LN_BenchmarkSoakSpec &soak = LN_GetBenchmarkSoakSpec();

  EXPECT_TRUE(LN_AutomatedBenchmarkAcceptanceGatePasses(protocol, linux_baseline, soak));

  const std::vector<LN_BenchmarkMeasuredRegressionDiagnostic> diagnostics =
      LN_GetBenchmarkMeasuredRegressionDiagnostics(protocol, linux_baseline);
  EXPECT_TRUE(diagnostics.empty());

  LN_BenchmarkFixtureRunSummary regressed_summary = LN_RunBenchmarkFixture(
      LN_GetBenchmarkFixtureSpecs()[size_t(LN_BenchmarkScenarioKind::InstructionHeavy)],
      LN_GetSyntheticBenchmarkDrivers()[size_t(LN_BenchmarkScenarioKind::InstructionHeavy)],
      protocol);
  regressed_summary.runs[size_t(LN_BenchmarkRunMode::OptimizedSerial)].median_ms =
      regressed_summary.runs[size_t(LN_BenchmarkRunMode::LegacySerial)].median_ms * 2.0;
  EXPECT_FALSE(LN_BenchmarkRunSummaryPassesRelativeBaseline(regressed_summary, linux_baseline));
}

TEST(LN_Performance, FinalRoadmapGateStatusRequiresAutomatedVerification)
{
  const LN_FinalRoadmapGateStatus &status = LN_GetFinalRoadmapGateStatus();
  EXPECT_TRUE(LN_FinalRoadmapGateStatusIsComplete(status));
  EXPECT_STREQ(status.build_command, "make NPROCS=32");
  EXPECT_TRUE(status.build_required);
  EXPECT_TRUE(status.focused_gtests_required);
  EXPECT_TRUE(status.acceptance_ctests_required);
  EXPECT_TRUE(status.deterministic_replay_checks_required);
  EXPECT_TRUE(status.automated_benchmark_gates_required);
  EXPECT_FALSE(status.user_benchmark_scenes_required);
  EXPECT_TRUE(status.phase10_full_conversion_complete);
  EXPECT_FALSE(status.measured_service_fixtures_complete);
  EXPECT_TRUE(status.production_readiness_review_complete);

  LN_FinalRoadmapGateStatus incomplete_status = status;
  incomplete_status.phase10_full_conversion_complete = false;
  EXPECT_FALSE(LN_FinalRoadmapGateStatusIsComplete(incomplete_status));

  incomplete_status = status;
  incomplete_status.production_readiness_review_complete = false;
  EXPECT_FALSE(LN_FinalRoadmapGateStatusIsComplete(incomplete_status));
}

TEST(LN_Performance, Phase10OpcodeConversionMatrixCoversEveryOpcode)
{
  const auto &matrix = LN_GetPhase10OpcodeConversionMatrix();
  EXPECT_TRUE(LN_Phase10OpcodeConversionMatrixIsComplete(matrix));
  EXPECT_EQ(matrix.size(), size_t(LN_OpCode::Count));

  bool found_direct_exec_path = false;
  bool found_static_operand_path = false;
  bool found_safely_convertible_converted_path = false;
  uint32_t remaining_runtime_opcode_fallbacks = 0;
  for (const LN_Phase10OpcodeConversionEntry &entry : matrix) {
    EXPECT_NE(entry.name, nullptr);
    EXPECT_NE(entry.completion_note, nullptr);
    EXPECT_STRNE(LN_Phase10ConversionPathName(entry.current_path), "unknown");
    EXPECT_STRNE(LN_Phase10ConversionPathName(entry.target_path), "unknown");
    EXPECT_NE(entry.current_path, LN_Phase10ConversionPath::Unclassified) << entry.name;
    EXPECT_NE(entry.target_path, LN_Phase10ConversionPath::Unclassified) << entry.name;

    found_direct_exec_path |= entry.current_path == LN_Phase10ConversionPath::ExecBlockIR;
    found_static_operand_path |= entry.requires_static_operands;
    found_safely_convertible_converted_path |= entry.safely_convertible && entry.currently_converted;
    EXPECT_FALSE(entry.safely_convertible && !entry.currently_converted)
        << entry.name << " is marked safely convertible but is not converted";
    if (!entry.currently_converted) {
      remaining_runtime_opcode_fallbacks++;
    }
  }

  EXPECT_TRUE(found_direct_exec_path);
  EXPECT_TRUE(found_static_operand_path);
  EXPECT_TRUE(found_safely_convertible_converted_path);
  EXPECT_EQ(remaining_runtime_opcode_fallbacks, 0u);
}

TEST(LN_Performance, Phase10DirectOpcodeMatrixMatchesExecBlockCompiler)
{
  const auto &matrix = LN_GetPhase10OpcodeConversionMatrix();
  uint32_t checked_direct_opcode_count = 0;

  for (const LN_Phase10OpcodeConversionEntry &entry : matrix) {
    if (entry.current_path != LN_Phase10ConversionPath::ExecBlockIR) {
      continue;
    }

    LN_Program program;
    LN_Instruction instruction;
    instruction.opcode = entry.opcode;
    FillDirectOpcodeTestOperands(program, instruction);
    program.AddInstruction(LN_Event::OnFixedUpdate, instruction);

    const LN_ExecBlockProgram &exec_program =
        program.GetExecBlockProgram(LN_Event::OnFixedUpdate);
    ASSERT_EQ(exec_program.blocks.size(), 1u) << entry.name;
    ASSERT_EQ(exec_program.ops.size(), 1u) << entry.name;
    EXPECT_EQ(exec_program.direct_instruction_count, 1u) << entry.name;
    EXPECT_EQ(exec_program.fallback_instruction_count, 0u) << entry.name;
    EXPECT_EQ(exec_program.fallback_block_count, 0u) << entry.name;
    EXPECT_EQ(exec_program.ops[0].opcode, entry.opcode) << entry.name;
    checked_direct_opcode_count++;
  }

  EXPECT_GT(checked_direct_opcode_count, 0u);
}

TEST(LN_Performance, Phase10OpcodeFallbackDiagnosticsCoverEveryRemainingFallback)
{
  const auto &matrix = LN_GetPhase10OpcodeConversionMatrix();
  const auto &diagnostics = LN_GetPhase10OpcodeFallbackDiagnostics();
  EXPECT_TRUE(LN_Phase10OpcodeFallbackDiagnosticsAreComplete(matrix, diagnostics));
  EXPECT_TRUE(diagnostics.empty());
  for (const LN_Phase10OpcodeFallbackDiagnostic &diagnostic : diagnostics) {
    EXPECT_NE(diagnostic.name, nullptr);
    EXPECT_NE(diagnostic.profile_counter_name, nullptr);
    EXPECT_NE(diagnostic.removal_condition, nullptr);
    EXPECT_TRUE(diagnostic.source_ref_required) << diagnostic.name;
    EXPECT_TRUE(diagnostic.hot_path_warning_required) << diagnostic.name;
    EXPECT_STREQ(diagnostic.profile_counter_name, "exec_fallback_instruction_count")
        << diagnostic.name;
    EXPECT_STRNE(LN_RuntimeFallbackReasonName(diagnostic.reason), "unknown");

  }
}

TEST(LN_Performance, Phase10ExpressionConversionMatrixCoversEveryExpressionKind)
{
  const auto &matrix = LN_GetPhase10ExpressionConversionMatrix();
  const size_t expected_count = uint32_t(LN_BoolExpressionKind::Count) +
                                uint32_t(LN_FloatExpressionKind::Count) +
                                uint32_t(LN_IntExpressionKind::Count) +
                                uint32_t(LN_StringExpressionKind::Count) +
                                uint32_t(LN_VectorExpressionKind::Count) +
                                uint32_t(LN_ColorExpressionKind::Count) +
                                uint32_t(LN_ValueExpressionKind::Count) +
                                uint32_t(LN_QueryExpressionKind::Count);
  EXPECT_TRUE(LN_Phase10ExpressionConversionMatrixIsComplete(matrix));
  EXPECT_EQ(matrix.size(), expected_count);

  bool found_register_path = false;
  bool found_string_constant_register_path = false;
  bool found_typed_value_register_path = false;
  bool found_safely_convertible_converted_path = false;
  bool found_intentional_fallback = false;
  for (const LN_Phase10ExpressionConversionEntry &entry : matrix) {
    EXPECT_NE(entry.name, nullptr);
    EXPECT_NE(entry.completion_note, nullptr);
    EXPECT_STRNE(LN_Phase10ConversionPathName(entry.current_path), "unknown");
    EXPECT_STRNE(LN_Phase10ConversionPathName(entry.target_path), "unknown");
    EXPECT_NE(entry.current_path, LN_Phase10ConversionPath::Unclassified) << entry.name;
    EXPECT_NE(entry.target_path, LN_Phase10ConversionPath::Unclassified) << entry.name;

    found_register_path |= entry.current_path == LN_Phase10ConversionPath::RegisterExpressionIR;
    found_string_constant_register_path |=
        entry.family == LN_RuntimeExpressionFamily::String &&
        entry.kind == uint32_t(LN_StringExpressionKind::Constant) &&
        entry.current_path == LN_Phase10ConversionPath::RegisterExpressionIR &&
        entry.safely_convertible && entry.currently_converted;
    found_typed_value_register_path |=
        entry.family == LN_RuntimeExpressionFamily::Value &&
        entry.kind == uint32_t(LN_ValueExpressionKind::FromInt) &&
        entry.current_path == LN_Phase10ConversionPath::RegisterExpressionIR &&
        entry.safely_convertible && entry.currently_converted;
    found_safely_convertible_converted_path |= entry.safely_convertible &&
                                               entry.currently_converted;
    EXPECT_FALSE(entry.safely_convertible && !entry.currently_converted)
        << entry.name << " is marked safely convertible but is not converted";
    found_intentional_fallback |= !entry.safely_convertible &&
                                  entry.target_path ==
                                      LN_Phase10ConversionPath::PermanentDynamicFallback;
  }

  EXPECT_TRUE(found_register_path);
  EXPECT_TRUE(found_string_constant_register_path);
  EXPECT_TRUE(found_typed_value_register_path);
  EXPECT_TRUE(found_safely_convertible_converted_path);
  EXPECT_TRUE(found_intentional_fallback);
}

TEST(LN_Performance, Phase10ExpressionFallbackDiagnosticsCoverEveryRemainingFallback)
{
  const auto &matrix = LN_GetPhase10ExpressionConversionMatrix();
  const auto &diagnostics = LN_GetPhase10ExpressionFallbackDiagnostics();
  EXPECT_TRUE(LN_Phase10ExpressionFallbackDiagnosticsAreComplete(matrix, diagnostics));
  ASSERT_FALSE(diagnostics.empty());
  EXPECT_LT(diagnostics.size(), matrix.size());

  bool found_unsupported_expression = false;
  bool found_dynamic_lookup = false;
  bool found_main_thread_live_read = false;
  for (const LN_Phase10ExpressionFallbackDiagnostic &diagnostic : diagnostics) {
    EXPECT_NE(diagnostic.name, nullptr);
    EXPECT_NE(diagnostic.profile_counter_name, nullptr);
    EXPECT_NE(diagnostic.removal_condition, nullptr);
    EXPECT_TRUE(diagnostic.source_ref_required) << diagnostic.name;
    EXPECT_TRUE(diagnostic.hot_path_warning_required) << diagnostic.name;
    EXPECT_STREQ(diagnostic.profile_counter_name, "register_expression_fallback_count")
        << diagnostic.name;
    EXPECT_STRNE(LN_RuntimeFallbackReasonName(diagnostic.reason), "unknown");

    found_unsupported_expression |=
        diagnostic.reason == LN_RuntimeFallbackReason::UnsupportedExpression;
    found_dynamic_lookup |= diagnostic.reason == LN_RuntimeFallbackReason::DynamicLookup;
    found_main_thread_live_read |=
        diagnostic.reason == LN_RuntimeFallbackReason::MainThreadLiveRead;
  }

  EXPECT_TRUE(found_unsupported_expression);
  EXPECT_TRUE(found_dynamic_lookup);
  EXPECT_TRUE(found_main_thread_live_read);
}

TEST(LN_Performance, ProfileLineReportsRequiredRegressionGateMetrics)
{
  LN_TickProfileMetrics metrics;
  metrics.snapshot_ms = 1.0;
  metrics.serial_record_ms = 2.0;
  metrics.parallel_record_ms = 3.0;
  metrics.run_once_record_ms = 4.0;
  metrics.merge_ms = 5.0;
  metrics.flush_ms = 6.0;
  metrics.event_delivery_ms = 6.5;
  metrics.allocation_count = 7;
  metrics.string_copy_count = 8;
  metrics.hot_map_lookup_count = 9;
  metrics.snapshot_tree_count = 10;
  metrics.snapshot_captured_channel_mask = 0x3;
  metrics.snapshot_skipped_channel_mask = 0x4;
  metrics.snapshot_shared_input_tree_count = 5;
  metrics.snapshot_property_storage_reuse_count = 6;
  metrics.event_count = 11;
  metrics.event_snapshot_tree_count = 12;
  metrics.event_indexed_lookup_count = 24;
  metrics.event_fallback_scan_count = 25;
  metrics.query_count = 13;
  metrics.command_count = 14;
  metrics.command_legacy_count = 31;
  metrics.command_typed_transform_count = 32;
  metrics.command_typed_velocity_count = 33;
  metrics.command_typed_relative_vector_count = 34;
  metrics.command_typed_motion_count = 60;
  metrics.command_typed_property_count = 35;
  metrics.command_typed_event_count = 36;
  metrics.command_typed_audio_count = 37;
  metrics.command_typed_lifecycle_count = 38;
  metrics.command_typed_object_service_count = 58;
  metrics.command_typed_runtime_service_count = 61;
  metrics.command_typed_animation_count = 62;
  metrics.command_typed_armature_count = 64;
  metrics.command_typed_material_count = 63;
  metrics.command_typed_physics_count = 59;
  metrics.command_coalesced_count = 39;
  metrics.command_list_count = 15;
  metrics.scheduler_job_count = 16;
  metrics.scheduler_planned_tree_count = 38;
  metrics.scheduler_worker_batch_count = 39;
  metrics.scheduler_main_thread_batch_count = 40;
  metrics.scheduler_max_trees_per_worker_batch = 41;
  metrics.scheduler_average_trees_per_worker_batch_x100 = 425;
  metrics.scheduler_worker_utilization_x100 = 88;
  metrics.scheduler_command_resource_classes = 0x5;
  metrics.scheduler_worker_resource_access = 0x7;
  metrics.scheduler_main_thread_resource_access = 0x8;
  metrics.scheduler_query_tree_count = 42;
  metrics.scheduler_snapshot_only_tree_count = 43;
  metrics.scheduler_command_record_only_tree_count = 44;
  metrics.exec_block_count = 45;
  metrics.exec_direct_instruction_count = 46;
  metrics.exec_fallback_instruction_count = 47;
  metrics.exec_fallback_block_count = 48;
  metrics.exec_fallback_ns = 49;
  metrics.register_expression_hit_count = 50;
  metrics.register_expression_fallback_count = 51;
  metrics.register_scalar_op_count = 52;
  metrics.register_simd_candidate_batch_count = 53;
  metrics.register_simd_candidate_lane_count = 54;
  metrics.register_simd_batch_count = 55;
  metrics.register_simd_lane_count = 56;
  metrics.instruction_count = 17;
  metrics.expression_count = 18;
  metrics.fallback_path_count = 19;
  metrics.runtime_fallback_reason_mask = 0x12;
  metrics.runtime_expression_fallback_reason_mask = 0x24;
  metrics.runtime_system_fallback_reason_mask = 0x48;
  metrics.runtime_system_fallback_count = 57;
  metrics.main_thread_fallback_tree_count = 20;
  metrics.serial_tree_count = 21;
  metrics.parallel_tree_count = 22;
  metrics.run_once_count = 23;
  metrics.parallel_executor_used = true;

  const std::string line = LN_FormatTickProfileLine(metrics);
  EXPECT_TRUE(StringContains(line, "snapshot_ms="));
  EXPECT_TRUE(StringContains(line, "serial_ms="));
  EXPECT_TRUE(StringContains(line, "parallel_ms="));
  EXPECT_TRUE(StringContains(line, "record_ms="));
  EXPECT_TRUE(StringContains(line, "merge_ms="));
  EXPECT_TRUE(StringContains(line, "flush_ms="));
  EXPECT_TRUE(StringContains(line, "event_ms="));
  EXPECT_TRUE(StringContains(line, "allocations="));
  EXPECT_TRUE(StringContains(line, "string_copies="));
  EXPECT_TRUE(StringContains(line, "hot_map_lookups="));
  EXPECT_TRUE(StringContains(line, "snapshot_channels=0x"));
  EXPECT_TRUE(StringContains(line, "skipped_snapshot_channels=0x"));
  EXPECT_TRUE(StringContains(line, "shared_input_trees="));
  EXPECT_TRUE(StringContains(line, "property_storage_reuse="));
  EXPECT_TRUE(StringContains(line, "events="));
  EXPECT_TRUE(StringContains(line, "event_trees="));
  EXPECT_TRUE(StringContains(line, "event_indexed_lookups="));
  EXPECT_TRUE(StringContains(line, "event_fallback_scans="));
  EXPECT_TRUE(StringContains(line, "queries="));
  EXPECT_TRUE(StringContains(line, "commands="));
  EXPECT_TRUE(StringContains(line, "legacy_commands="));
  EXPECT_TRUE(StringContains(line, "typed_transform_commands="));
  EXPECT_TRUE(StringContains(line, "typed_velocity_commands="));
  EXPECT_TRUE(StringContains(line, "typed_relative_vector_commands="));
  EXPECT_TRUE(StringContains(line, "typed_motion_commands="));
  EXPECT_TRUE(StringContains(line, "typed_property_commands="));
  EXPECT_TRUE(StringContains(line, "typed_event_commands="));
  EXPECT_TRUE(StringContains(line, "typed_audio_commands="));
  EXPECT_TRUE(StringContains(line, "typed_lifecycle_commands="));
  EXPECT_TRUE(StringContains(line, "typed_object_service_commands="));
  EXPECT_TRUE(StringContains(line, "typed_runtime_service_commands="));
  EXPECT_TRUE(StringContains(line, "typed_animation_commands="));
  EXPECT_TRUE(StringContains(line, "typed_armature_commands="));
  EXPECT_TRUE(StringContains(line, "typed_material_commands="));
  EXPECT_TRUE(StringContains(line, "typed_physics_commands="));
  EXPECT_TRUE(StringContains(line, "coalesced_commands="));
  EXPECT_TRUE(StringContains(line, "scheduler_jobs="));
  EXPECT_TRUE(StringContains(line, "scheduler_planned_trees="));
  EXPECT_TRUE(StringContains(line, "scheduler_worker_batches="));
  EXPECT_TRUE(StringContains(line, "scheduler_main_thread_batches="));
  EXPECT_TRUE(StringContains(line, "scheduler_max_trees_per_worker_batch="));
  EXPECT_TRUE(StringContains(line, "scheduler_avg_trees_per_worker_batch_x100="));
  EXPECT_TRUE(StringContains(line, "scheduler_worker_utilization_x100="));
  EXPECT_TRUE(StringContains(line, "scheduler_command_resources=0x"));
  EXPECT_TRUE(StringContains(line, "scheduler_worker_resources=0x"));
  EXPECT_TRUE(StringContains(line, "scheduler_main_thread_resources=0x"));
  EXPECT_TRUE(StringContains(line, "scheduler_query_trees="));
  EXPECT_TRUE(StringContains(line, "scheduler_snapshot_only_trees="));
  EXPECT_TRUE(StringContains(line, "scheduler_command_record_only_trees="));
  EXPECT_TRUE(StringContains(line, "scheduler_immutable_worker_inputs=yes"));
  EXPECT_TRUE(StringContains(line, "scheduler_worker_metrics_local=yes"));
  EXPECT_TRUE(StringContains(line, "scheduler_deterministic_merge=yes"));
  EXPECT_TRUE(StringContains(line, "scheduler_main_thread_flush_isolated=yes"));
  EXPECT_TRUE(StringContains(line, "instructions="));
  EXPECT_TRUE(StringContains(line, "exec_blocks="));
  EXPECT_TRUE(StringContains(line, "exec_direct_instructions="));
  EXPECT_TRUE(StringContains(line, "exec_fallback_instructions="));
  EXPECT_TRUE(StringContains(line, "exec_fallback_blocks="));
  EXPECT_TRUE(StringContains(line, "exec_fallback_ns="));
  EXPECT_TRUE(StringContains(line, "expressions="));
  EXPECT_TRUE(StringContains(line, "register_expr_hits="));
  EXPECT_TRUE(StringContains(line, "register_expr_fallbacks="));
  EXPECT_TRUE(StringContains(line, "register_scalar_ops="));
  EXPECT_TRUE(StringContains(line, "register_simd_candidate_batches="));
  EXPECT_TRUE(StringContains(line, "register_simd_candidate_lanes="));
  EXPECT_TRUE(StringContains(line, "register_simd_batches="));
  EXPECT_TRUE(StringContains(line, "register_simd_lanes="));
  EXPECT_TRUE(StringContains(line, "fallback_paths="));
  EXPECT_TRUE(StringContains(line, "fallback_reasons=0x"));
  EXPECT_TRUE(StringContains(line, "expression_fallback_reasons=0x"));
  EXPECT_TRUE(StringContains(line, "system_fallback_reasons=0x"));
  EXPECT_TRUE(StringContains(line, "system_fallbacks="));
  EXPECT_TRUE(StringContains(line, "main_thread_fallback_trees="));
  EXPECT_TRUE(StringContains(line, "parallel=on"));
}
