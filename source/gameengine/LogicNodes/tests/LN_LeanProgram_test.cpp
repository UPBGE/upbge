/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * The lean program and the mode switch (lean runtime plan, phase A16): the lowering makes one
 * generic lean op for every op of a program, the dump is stable, the mode follows the test setter,
 * a program grown after it was lowered is lowered again, and a Force run of the unit suite
 * fails when no tree ran on the lean VM. The lean VM against the old interpreter is in
 * LN_LeanDifferential_test.cpp. */

#include "testing/testing.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "LN_JobPool.h"
#include "LN_LeanProgram.h"
#include "LN_TreeBatch.h"
#include "LN_SemRig.h"

using namespace ln_test;
using namespace ln_test::sem;

namespace {

struct ModeScope {
  explicit ModeScope(const int mode)
  {
    LN_SetLeanModeForTests(mode);
  }
  ~ModeScope()
  {
    LN_SetLeanModeForTests(-1);
  }
};

}  // namespace

TEST(LN_LeanProgram, EveryExecOpHasOneGenericLeanOpInTheSameOrder)
{
  BrainOfOptions options;
  const std::shared_ptr<LN_Program> program = BrainOf(options);
  ASSERT_NE(program, nullptr);
  const std::shared_ptr<const LN_LeanProgram> lean = LN_LowerToLean(*program);
  ASSERT_NE(lean, nullptr);
  EXPECT_TRUE(lean->valid);
  uint32_t ops = 0, generic = 0, native = 0;
  for (size_t event = 0; event < lean->events.size(); event++) {
    const LN_ExecBlockProgram &ir = program->GetExecBlockProgram(LN_Event(event));
    ASSERT_EQ(lean->events[event].ops.size(), ir.ops.size());
    for (uint32_t index = 0; index < ir.ops.size(); index++) {
      EXPECT_EQ(lean->events[event].ops[index].exec_op_index, index);
      /* The brain's ops all have native handlers (A18, A21): flow, number writes, Apply
       * Movement / Rotation (all vector commands since A21's rest). */
      uint16_t expected = uint16_t(LN_LeanNative::Generic);
      switch (ir.ops[index].kind) {
        case LN_ExecOpKind::Nop:
          expected = uint16_t(LN_LeanNative::Nop);
          break;
        case LN_ExecOpKind::BranchRoute:
          expected = uint16_t(LN_LeanNative::BranchRoute);
          break;
        case LN_ExecOpKind::GamePropertyCommand:
          expected = uint16_t(LN_LeanNative::SetNumber);
          break;
        case LN_ExecOpKind::VectorCommand:
          expected = uint16_t(LN_LeanNative::VectorCommand);
          break;
        default:
          break;
      }
      EXPECT_EQ(lean->events[event].ops[index].native, expected) << "op " << index;
      (expected == uint16_t(LN_LeanNative::Generic) ? generic : native)++;
    }
    ops += uint32_t(ir.ops.size());
  }
  EXPECT_EQ(lean->generic_op_count, generic);
  EXPECT_EQ(lean->native_op_count, native);
  EXPECT_EQ(generic + native, ops);
  EXPECT_GT(native, 0u);
  EXPECT_GT(ops, 5u);
  EXPECT_EQ(lean->expression_entries[2], uint32_t(program->GetFloatExpressions().size()));
  EXPECT_EQ(lean->expression_entries[7], uint32_t(program->GetQueryExpressions().size()));
}

TEST(LN_LeanProgram, TheLeanProgramDumpIsStableAndMatchesItsGolden)
{
  BrainOfOptions plain;
  BrainOfOptions stepped;
  stepped.chains = 2;
  stepped.step_every = 4;
  stepped.measure_distance = false;
  stepped.look = true;
  std::string text;
  for (const BrainOfOptions &options : {plain, stepped}) {
    const std::shared_ptr<LN_Program> a = BrainOf(options);
    const std::shared_ptr<LN_Program> b = BrainOf(options);
    ASSERT_NE(a, nullptr);
    const std::string first = LN_DumpLeanProgramText(*LN_LowerToLean(*a), *a);
    EXPECT_EQ(first, LN_DumpLeanProgramText(*LN_LowerToLean(*b), *b));
    text += first + "----\n";
  }
  const std::string verdict = CheckGolden("lean_program_dump", text);
  EXPECT_TRUE(verdict.empty()) << verdict;
}

TEST(LN_LeanProgram, TheModeFollowsTheTestSetter)
{
  {
    ModeScope scope(0);
    EXPECT_EQ(LN_GetLeanMode(), LN_LeanMode::Off);
  }
  {
    ModeScope scope(1);
    EXPECT_EQ(LN_GetLeanMode(), LN_LeanMode::Auto);
  }
  {
    ModeScope scope(2);
    EXPECT_EQ(LN_GetLeanMode(), LN_LeanMode::Force);
  }
}

/* A tree makes its program's lean form when it first runs lean, a program grown afterwards drops
 * it, and the next run makes it again (the main thread is the only maker). */
TEST(LN_LeanProgram, AProgramGrownAfterItWasLoweredIsLoweredAgain)
{
  ModeScope scope(2);
  TreeBuilder b;
  GenNode &event = b.Add("LogicNativeOnFixedUpdate");
  GenNode &set = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(set, "Property", "p");
  b.Float(set, "Value", 3.0f);
  b.Link(event, "Out", set, "Flow");
  std::string report;
  const std::shared_ptr<LN_Program> program = b.Compile(&report);
  ASSERT_NE(program, nullptr) << report;
  SemRig rig(program, kMemoOff);
  const uint64_t runs_before = LN_LeanTreeRunCount();
  rig.Step();
  EXPECT_GT(LN_LeanTreeRunCount(), runs_before);
  EXPECT_EQ(rig.Float(rig.Object(0), "p"), 3.0f);
  const std::shared_ptr<const LN_LeanProgram> first = program->GetLeanProgram();
  ASSERT_NE(first, nullptr);
  /* Grow the program: a new expression drops the lean form. */
  LN_FloatExpression extra;
  extra.kind = LN_FloatExpressionKind::Constant;
  extra.float_value = 41.5f;
  program->AddFloatExpression(extra);
  EXPECT_EQ(program->GetLeanProgram(), nullptr);
  rig.Step();
  const std::shared_ptr<const LN_LeanProgram> second = program->GetLeanProgram();
  ASSERT_NE(second, nullptr);
  EXPECT_NE(second.get(), first.get());
  EXPECT_EQ(second->expression_entries[2], first->expression_entries[2] + 1u);
}

/* Mode Off runs the old interpreter: the run count does not move and no lean form is made. */
TEST(LN_LeanProgram, ModeOffRunsTheOldInterpreterAndMakesNoLeanProgram)
{
  ModeScope scope(0);
  TreeBuilder b;
  GenNode &event = b.Add("LogicNativeOnFixedUpdate");
  GenNode &set = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(set, "Property", "p");
  b.Float(set, "Value", 4.0f);
  b.Link(event, "Out", set, "Flow");
  std::string report;
  const std::shared_ptr<LN_Program> program = b.Compile(&report);
  ASSERT_NE(program, nullptr) << report;
  SemRig rig(program, kMemoOff);
  const uint64_t runs_before = LN_LeanTreeRunCount();
  rig.Step();
  EXPECT_EQ(LN_LeanTreeRunCount(), runs_before);
  EXPECT_EQ(program->GetLeanProgram(), nullptr);
  EXPECT_EQ(rig.Float(rig.Object(0), "p"), 4.0f);
}

/* A micro benchmark (DISABLED_: run with --gtest_also_run_disabled_tests): 2,000 NPC brains
 * (BrainOf) in the manager's tick order, one Step a round, best of 15, by the old interpreter
 * (memo on and off) and by the lean VM's generic form. The generic form has no memo and no value
 * cache yet, so it shows what the native families of the next phases have to beat. */
TEST(LN_LeanProgram, DISABLED_GenericFormBench)
{
  for (const bool flow_heavy : {false, true}) {
    BrainOfOptions options;
    options.chains = flow_heavy ? 12 : 4;
    options.step_every = 1; /* on Logic + Physics Step, which SemRig::Step runs */
    const std::shared_ptr<LN_Program> program = BrainOf(options);
    ASSERT_NE(program, nullptr);
    struct Setup {
      const char *name;
      int lean;
      const SemMode *mode;
    };
    for (const Setup &setup : {Setup{"old, memo on", 0, &kMemoOn},
                               Setup{"old, memo off", 0, &kMemoOff},
                               Setup{"lean generic", 2, &kMemoOff}})
    {
      ModeScope scope(setup.lean);
      SemRig rig(program, *setup.mode, 2000);
      for (size_t index = 0; index < 2000; index++) {
        rig.SetFloat(rig.Object(index), "dist", 20.0f);
      }
      double best = 1.0e9;
      for (int round = 0; round < 15; round++) {
        const auto start = std::chrono::steady_clock::now();
        rig.Step();
        best = std::min(best,
                        std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - start)
                            .count());
      }
      std::printf("LEAN_BENCH brain chains=%d %-14s %.3f ms a step for 2000 brains\n",
                  options.chains,
                  setup.name,
                  best);
    }
  }
}

/* The brain on 10,000 objects, one thread, in the manager's order without the rig's dump
 * (DISABLED_: run with --gtest_also_run_disabled_tests). Prints ns per NPC for the snapshot, the
 * recording and the flush apart, by the old interpreter and the lean VM; UPBGE_LN_BENCH_ROUNDS
 * sets the rounds (default 15; a stack sampler wants hundreds), UPBGE_LN_BENCH_MODE = old|lean
 * runs one setting only, UPBGE_LN_BENCH_CHAINS the brain's chains (default 4: research.py's). */
TEST(LN_LeanProgram, DISABLED_BrainBench)
{
  const char *rounds_text = std::getenv("UPBGE_LN_BENCH_ROUNDS");
  const int rounds = rounds_text != nullptr ? std::max(1, std::atoi(rounds_text)) : 15;
  const char *only = std::getenv("UPBGE_LN_BENCH_MODE");
  const char *chains_text = std::getenv("UPBGE_LN_BENCH_CHAINS");
  BrainOfOptions options;
  options.chains = chains_text != nullptr ? std::max(0, std::atoi(chains_text)) : 4;
  options.step_every = 1;
  const std::shared_ptr<LN_Program> program = BrainOf(options);
  ASSERT_NE(program, nullptr);
  constexpr uint32_t kOwners = 10000;
  struct Setup {
    const char *name;
    int lean;
  };
  for (const Setup &setup : {Setup{"old", 0}, Setup{"lean", 2}}) {
    if (only != nullptr && std::strcmp(only, setup.name) != 0) {
      continue;
    }
    ModeScope scope(setup.lean);
    SemRig rig(program, kMemoOn, kOwners);
    /* UPBGE_LN_BENCH_BATCH=chunks: record as a tree batch on no helper thread (with
     * UPBGE_LN_BLOCKS=force the batch runs its trees in blocks). */
    const char *batch_text = std::getenv("UPBGE_LN_BENCH_BATCH");
    if (batch_text != nullptr) {
      rig.SetBatchRecording(uint32_t(std::max(1, std::atoi(batch_text))), 0);
    }
    rig.SetDump(false);
    rig.SetCapturePrints(false);
    for (uint32_t index = 0; index < kOwners; index++) {
      rig.SetFloat(rig.Object(index), "dist", 20.0f);
    }
    rig.Step();
    rig.Step();
    SemRig::PhaseTimes best = {1.0e9, 1.0e9, 1.0e9};
    double best_total = 1.0e9;
    for (int round = 0; round < rounds; round++) {
      rig.ResetTimes();
      rig.Step();
      const SemRig::PhaseTimes &t = rig.Times();
      const double total = t.capture_ms + t.record_ms + t.flush_ms;
      if (total < best_total) {
        best_total = total;
        best = t;
      }
    }
    std::printf(
        "LEAN_BENCH brain10k %-4s  snapshot %.0f  record %.0f  flush %.0f  total %.0f ns an "
        "NPC (%.2f ms)\n",
        setup.name,
        best.capture_ms * 1.0e6 / kOwners,
        best.record_ms * 1.0e6 / kOwners,
        best.flush_ms * 1.0e6 / kOwners,
        best_total * 1.0e6 / kOwners,
        best_total);
  }
}

/* The memory one brain costs (DISABLED_): sizes of the tree, its state block, its snapshot and the
 * pieces a recording touches, to decide what to shrink (the game is 2-4x slower per NPC than the
 * rig for the same work: memory traffic). */
TEST(LN_LeanProgram, DISABLED_FootprintReport)
{
  BrainOfOptions options;
  options.step_every = 1;
  const std::shared_ptr<LN_Program> program = BrainOf(options);
  ASSERT_NE(program, nullptr);
  SemRig rig(program, kMemoOn, 2);
  rig.Step();
  const LN_RuntimeTree::StateBlockInfo info = rig.Tree(0).GetStateBlockInfo();
  std::printf("FOOTPRINT sizeof(LN_RuntimeTree)=%zu sizeof(LN_Snapshot)=%zu sizeof(LN_QueryResult)=%zu "
              "sizeof(LN_Value)=%zu sizeof(KX_GameObject)=%zu sizeof(LN_Instruction)=%zu\n",
              sizeof(LN_RuntimeTree),
              sizeof(LN_Snapshot),
              sizeof(LN_QueryResult),
              sizeof(LN_Value),
              sizeof(KX_GameObject),
              sizeof(LN_Instruction));
  std::printf("FOOTPRINT state block %zu bytes: stamps %u bool %u float %u motion %u changes %u "
              "timeflows %u flowctl %u queries %u overflow %u own_input %d\n",
              info.block_bytes,
              info.instruction_stamps,
              info.bool_states,
              info.float_states,
              info.motion_states,
              info.value_changes,
              info.time_flows,
              info.flow_controls,
              info.queries,
              info.overflow_states,
              info.has_own_input ? 1 : 0);
}

/* A micro benchmark of expression evaluation alone (DISABLED_): a tree-property sum chain of 40
 * Adds, a Multiply, a Divide, a Clamp and a compare, worked out 20,000 times in each setting: the
 * Resolve functions with the memo off, and the lean reader. Best of 7 rounds. */
TEST(LN_LeanProgram, DISABLED_ExpressionBench)
{
  LN_Program program;
  const auto add_float_property = [&](const char *name, const float value) {
    LN_TreePropertyRef ref;
    ref.name = name;
    ref.value_type = LN_ValueType::Float;
    ref.default_value.type = LN_ValueType::Float;
    ref.default_value.exists = true;
    ref.default_value.float_value = value;
    LN_FloatExpression e;
    e.kind = LN_FloatExpressionKind::RuntimeTreeProperty;
    e.property_ref_index = program.AddTreePropertyRef(ref);
    return program.AddFloatExpression(e);
  };
  uint32_t sum = add_float_property("p0", 1.0f);
  for (int index = 1; index <= 40; index++) {
    LN_FloatExpression add;
    add.kind = LN_FloatExpressionKind::Add;
    add.input0 = sum;
    add.input1 = add_float_property(("p" + std::to_string(index)).c_str(), 0.5f + float(index));
    sum = program.AddFloatExpression(add);
  }
  LN_FloatExpression mul;
  mul.kind = LN_FloatExpressionKind::Multiply;
  mul.input0 = sum;
  mul.input1 = add_float_property("scale", 0.25f);
  const uint32_t scaled = program.AddFloatExpression(mul);
  LN_FloatExpression div;
  div.kind = LN_FloatExpressionKind::Divide;
  div.input0 = scaled;
  div.input1 = add_float_property("divisor", 3.0f);
  const uint32_t divided = program.AddFloatExpression(div);
  LN_FloatExpression clamp;
  clamp.kind = LN_FloatExpressionKind::Clamp;
  clamp.input0 = divided;
  clamp.input1 = add_float_property("lo", 1.0f);
  clamp.input2 = add_float_property("hi", 500.0f);
  const uint32_t root = program.AddFloatExpression(clamp);
  const std::shared_ptr<LN_Program> shared = std::make_shared<LN_Program>(std::move(program));
  SemRig rig(shared, kMemoOff);
  LN_TickContext context;
  context.use_fixed_timestep = true;
  context.tick_index = 1;
  for (const bool lean : {false, true}) {
    double best = 1.0e9;
    for (int round = 0; round < 7; round++) {
      const auto start = std::chrono::steady_clock::now();
      for (int index = 0; index < 20000; index++) {
        rig.Tree(0).EvaluateExpressionForTests('F', root, context, lean);
      }
      best = std::min(best,
                      std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - start)
                          .count());
    }
    std::printf("LEAN_BENCH expression chain %-12s %.3f ms for 20000 evaluations of 45 nodes\n",
                lean ? "lean reader" : "Resolve",
                best);
  }
}

/* A tree batch makes every tree's lean form on the main thread before the chunks go out (the
 * prepare pass, plan phase A17): the helpers only read. The trees here are disabled, so nothing
 * but the prepare pass can make the program's lean form. */
TEST(LN_LeanProgram, ABatchMakesTheLeanFormBeforeItsChunksRun)
{
  ModeScope scope(2);
  TreeBuilder b;
  GenNode &event = b.Add("LogicNativeOnFixedUpdate");
  GenNode &set = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(set, "Property", "p");
  b.Float(set, "Value", 3.0f);
  b.Link(event, "Out", set, "Flow");
  std::string report;
  const std::shared_ptr<LN_Program> program = b.Compile(&report);
  ASSERT_NE(program, nullptr) << report;
  SemRig rig(program, kMemoOff, 4);
  std::vector<LN_RuntimeTree *> trees;
  for (size_t index = 0; index < rig.TreeCount(); index++) {
    rig.Tree(index).SetEnabled(false);
    trees.push_back(&rig.Tree(index));
  }
  ASSERT_EQ(program->GetLeanProgram(), nullptr);
  LN_JobPool pool;
  LN_TreeBatchRecorder recorder;
  LN_CommandBuffer buffer;
  buffer.BeginRecording();
  LN_TickContext context;
  context.event = LN_Event::OnFixedUpdate;
  context.use_fixed_timestep = true;
  context.tick_index = 1;
  recorder.Record(pool, buffer, trees.data(), trees.size(), context, nullptr, nullptr, 2);
  buffer.EndRecording();
  EXPECT_NE(program->GetLeanProgram(), nullptr);
}

/* A Force run of the unit suite (UPBGE_LN_LEAN=force) that ran no tree on the lean VM did not test
 * the lean VM: this environment fails it when the process had LN tests and no lean run. */
namespace {
class LeanForceEnvironment : public ::testing::Environment {
 public:
  void TearDown() override
  {
    const char *value = std::getenv("UPBGE_LN_LEAN");
    if (value == nullptr || (std::strcmp(value, "force") != 0 && std::strcmp(value, "2") != 0)) {
      return;
    }
    bool ran_ln_tests = false;
    const ::testing::UnitTest &unit = *::testing::UnitTest::GetInstance();
    for (int index = 0; index < unit.total_test_suite_count(); index++) {
      const ::testing::TestSuite *suite = unit.GetTestSuite(index);
      if (std::strncmp(suite->name(), "LN_", 3) == 0 && suite->test_to_run_count() > 0) {
        ran_ln_tests = true;
      }
    }
    if (ran_ln_tests && LN_LeanTreeRunCount() == 0) {
      ADD_FAILURE() << "UPBGE_LN_LEAN=force, but no tree ran on the lean VM";
    }
  }
};
const ::testing::Environment *const g_lean_force_environment =
    ::testing::AddGlobalTestEnvironment(new LeanForceEnvironment());
}  // namespace
