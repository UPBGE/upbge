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

/** Compiled routes on (1) or off (0) for a scope (plan phase A33). */
struct RouteScope {
  explicit RouteScope(const int mode)
  {
    LN_SetRouteModeForTests(mode);
  }
  ~RouteScope()
  {
    LN_SetRouteModeForTests(-1);
  }
};

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

/* Plan step R3: the reference mode (0) and the lean VM (1 and 2, the old Auto and Force). */
TEST(LN_LeanProgram, TheModeFollowsTheTestSetter)
{
  {
    ModeScope scope(0);
    EXPECT_EQ(LN_GetLeanMode(), LN_LeanMode::Reference);
  }
  {
    ModeScope scope(1);
    EXPECT_EQ(LN_GetLeanMode(), LN_LeanMode::Lean);
  }
  {
    ModeScope scope(2);
    EXPECT_EQ(LN_GetLeanMode(), LN_LeanMode::Lean);
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
/* Plan step R3: the reference mode (D10) runs every op through its generic handler and makes no
 * lean program; its run is counted with the lean VM's (the only interpreter left). */
TEST(LN_LeanProgram, TheReferenceModeMakesNoLeanProgram)
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
  EXPECT_GT(LN_LeanTreeRunCount(), runs_before);
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
  /* UPBGE_LN_BENCH_NODIST=1: the stored distance instead of Get Distance (what the query costs). */
  options.measure_distance = std::getenv("UPBGE_LN_BENCH_NODIST") == nullptr;
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

/* A tree whose pure expressions are read many times (DISABLED_): a chain of six math nodes on two
 * properties feeds twelve property writes. The old interpreter's per-tick memo works the chain out
 * once; the lean VM has no memo and works it out for each write. Decides whether making the lean
 * VM the only interpreter (plan A28 and on) would slow such trees down. */
namespace {

/** 12 writes of one 6-node chain (two property reads, Add, Multiply, Subtract...) by `writer`,
 * on 4,000 NPCs: the record cost an NPC in the old interpreter (its memo works the chain out
 * once a tick), lean Auto and lean Force. */
void SharedChainBench(const char *writer, const char *what)
{
  TreeBuilder b;
  GenNode &event = b.Add("LogicNativeOnFixedUpdate");
  GenNode &a = b.Add("LogicNativeGetGamePropertyFloat");
  b.String(a, "Property", "pf0");
  GenNode &c = b.Add("LogicNativeGetGamePropertyFloat");
  b.String(c, "Property", "pf1");
  GenNode *chain = &b.Add("LogicNativeMath", blender::NODE_MATH_ADD);
  b.Link(a, "Value", *chain, "A");
  b.Link(c, "Value", *chain, "B");
  for (int step = 0; step < 5; step++) {
    GenNode &next = b.Add("LogicNativeMath", step % 2 == 0 ? blender::NODE_MATH_MULTIPLY :
                                                             blender::NODE_MATH_SUBTRACT);
    b.Link(*chain, "Result", next, "A");
    b.Float(next, "B", 1.25f + float(step));
    chain = &next;
  }
  GenNode *flow = &event;
  const char *pin = "Out";
  for (int index = 0; index < 12; index++) {
    GenNode &set = b.Add(writer);
    b.String(set, "Property", ("o" + std::to_string(index)).c_str());
    b.Link(*chain, "Result", set, "Value");
    b.Link(*flow, pin, set, "Flow");
    flow = &set;
    pin = "Done";
  }
  std::string report;
  const std::shared_ptr<LN_Program> program = b.Compile(&report);
  ASSERT_NE(program, nullptr) << report;
  constexpr uint32_t kOwners = 4000;
  struct Setup {
    const char *name;
    int lean;
  };
  for (const Setup &setup :
       {Setup{"old (memo)", 0}, Setup{"lean auto", 1}, Setup{"lean force", 2}})
  {
    ModeScope scope(setup.lean);
    SemRig rig(program, kMemoOn, kOwners);
    rig.SetDump(false);
    rig.SetCapturePrints(false);
    for (uint32_t index = 0; index < kOwners; index++) {
      rig.SetFloat(rig.Object(index), "pf0", float(index % 17));
      rig.SetFloat(rig.Object(index), "pf1", 0.5f);
    }
    rig.Step();
    rig.Step();
    double best = 1.0e9;
    for (int round = 0; round < 25; round++) {
      rig.ResetTimes();
      rig.Step();
      best = std::min(best, rig.Times().record_ms);
    }
    std::printf("LEAN_BENCH shared chain, %-22s %-12s record %.0f ns an NPC\n",
                what,
                setup.name,
                best * 1.0e6 / kOwners);
  }
}

}  // namespace

/* The memo-heavy tree of plan step A28: one chain read by 12 native writers (Set Game Property
 * Float), then by 12 generic ones (Set Object Property: a value; plan "Attribute and Retirement",
 * R1, where the lean memo must count the generic ops' reads). */
TEST(LN_LeanProgram, DISABLED_SharedSubexpressionBench)
{
  SharedChainBench("LogicNativeSetGamePropertyFloat", "native writers");
  SharedChainBench("LogicNativeSetGameProperty", "generic writers");
}

/* A micro benchmark of expression evaluation alone (DISABLED_): a tree-property sum chain of 40
 * Adds, a Multiply, a Divide, a Clamp and a compare, worked out 20,000 times through the lean reader
 * (the Resolve functions it was compared with went with plan step R4). Best of 7 rounds. */
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
  double best = 1.0e9;
  for (int round = 0; round < 7; round++) {
    const auto start = std::chrono::steady_clock::now();
    for (int index = 0; index < 20000; index++) {
      rig.Tree(0).EvaluateExpressionForTests('F', root, context);
    }
    best = std::min(best,
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - start)
                        .count());
  }
  std::printf("LEAN_BENCH expression chain, lean reader %.3f ms for 20000 evaluations of 45 nodes\n",
              best);
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

namespace {

/** BrainOf's program as straight code (2026-11-02, the question of plan phase B4): what a perfect
 * block compiler or a JIT could make of it. Its constants are taken from the compiled program
 * once (the "compile"); per NPC it does only the brain's own work: the distance, the compare,
 * the property reads and adds, the five number writes and the moves, through the very appends
 * the native handlers use, in their order. It skips everything an interpreter adds: dispatch,
 * op preludes, guards, stamps, the query cache, the tick bookkeeping of ExecuteReady. Recording
 * the same commands is checked (TheIdealBrainRecordsExactlyWhatTheLeanVMRecords), so its speed
 * is the floor any form of blocks, kernels or compilation could reach for this program. */
class IdealBrain {
 public:
  IdealBrain(const LN_Program &program, const LN_Event event, const float threshold)
      : m_threshold(threshold)
  {
    program.EnsureLeanProgram();
    const std::shared_ptr<const LN_LeanProgram> &lean = program.GetLeanProgram();
    if (lean == nullptr) {
      return;
    }
    const LN_ExecBlockProgram &ir = program.GetExecBlockProgram(event);
    const std::vector<LN_InstructionHeader> &headers = program.GetInstructionHeaders(event);
    const std::vector<LN_FloatExpression> &floats = program.GetFloatExpressions();
    const std::vector<LN_VectorExpression> &vectors = program.GetVectorExpressions();
    std::vector<Move> found;
    for (const LN_LeanOp &lean_op : lean->events[size_t(event)].ops) {
      const LN_ExecOp &op = ir.ops[lean_op.exec_op_index];
      const LN_Instruction &instruction = program.GetInstructionPayload(
          event, headers[op.instruction_index]);
      if (lean_op.native == uint16_t(LN_LeanNative::SetNumber)) {
        const LN_GamePropertyCommandPayload &payload =
            program.GetGamePropertyCommandPayloads()[instruction.command_payload_index];
        Write write;
        write.property_ref = payload.property_ref_index;
        write.source_ref = instruction.source_ref_index;
        const LN_FloatExpression &value = floats[payload.float_expr_index];
        if (value.kind == LN_FloatExpressionKind::QueryDistance) {
          write.distance = true;
        }
        else if (value.kind == LN_FloatExpressionKind::Add &&
                 floats[value.input0].kind == LN_FloatExpressionKind::SnapshotGameProperty &&
                 floats[value.input1].kind == LN_FloatExpressionKind::Constant)
        {
          write.read_ref = floats[value.input0].property_ref_index;
          write.add = floats[value.input1].float_value;
        }
        else {
          return;
        }
        m_writes.push_back(write);
      }
      else if (lean_op.native == uint16_t(LN_LeanNative::VectorCommand)) {
        const LN_VectorCommandPayload &payload =
            program.GetVectorCommandPayloads()[instruction.command_payload_index];
        Move move;
        move.source_ref = instruction.source_ref_index;
        const LN_VectorOperationMode mode = LN_VectorOperationMode(payload.operation_mode);
        if (mode == LN_VectorOperationMode::LocalFromBool) {
          /* The Local input: a constant here (BrainOf's moves are all local). */
          const std::vector<LN_BoolExpression> &bools = program.GetBoolExpressions();
          if (payload.bool_expr_index >= bools.size() ||
              bools[payload.bool_expr_index].kind != LN_BoolExpressionKind::Constant)
          {
            return;
          }
          move.local = bools[payload.bool_expr_index].bool_value;
        }
        else {
          move.local = mode == LN_VectorOperationMode::Local;
        }
        move.rotation_channel = LN_VectorOperationChannel(payload.operation_channel) ==
                                LN_VectorOperationChannel::Rotation;
        move.value = payload.vector_value;
        if (payload.vector_expr_index != LN_INVALID_INDEX &&
            vectors[payload.vector_expr_index].kind == LN_VectorExpressionKind::Constant)
        {
          move.value = vectors[payload.vector_expr_index].vector_value;
        }
        move.rotation = LN_Rotation::FromEulerXYZ(move.value);
        found.push_back(move);
      }
    }
    /* dist + 4 chains; approach (True), turn and walk (False). BrainOf's approach is the longer
     * of the two movements (0.002 against the walk's 0.001). */
    const Move *turn = nullptr;
    for (const Move &move : found) {
      if (move.rotation_channel) {
        turn = &move;
      }
    }
    if (m_writes.size() != 5 || !m_writes[0].distance || found.size() != 3 || turn == nullptr) {
      return;
    }
    const Move *walk = nullptr;
    const Move *approach = nullptr;
    for (const Move &move : found) {
      if (move.rotation_channel) {
        continue;
      }
      if (approach == nullptr || move.value.length() > approach->value.length()) {
        walk = approach;
        approach = &move;
      }
      else {
        walk = &move;
      }
    }
    if (walk == nullptr || approach == nullptr) {
      return;
    }
    m_moves = {*approach, *turn, *walk};
    m_ok = true;
  }

  bool Ok() const
  {
    return m_ok;
  }

  /** The B4 form of the same work: every lane's distance and every property read worked out in
   * columns first (one pass per value over the block), then the appends lane by lane (the order
   * the commands must have). */
  void RecordColumns(LN_CommandBuffer &buffer, const std::vector<LN_RuntimeTree *> &trees) const
  {
    const double max_float = double(std::numeric_limits<float>::max());
    const size_t count = trees.size();
    thread_local std::vector<float> distances;
    thread_local std::vector<float> values;
    distances.resize(count);
    values.resize(count * m_writes.size());
    for (size_t lane = 0; lane < count; lane++) {
      const MT_Vector3 a = trees[lane]->GetGameObject()->NodeGetWorldPosition();
      const double dx = double(a.x()), dy = double(a.y()), dz = double(a.z());
      distances[lane] = float(std::min(std::sqrt(dx * dx + dy * dy + dz * dz), max_float));
    }
    for (size_t column = 0; column < m_writes.size(); column++) {
      const Write &write = m_writes[column];
      float *out = values.data() + column * count;
      if (write.distance) {
        std::copy(distances.begin(), distances.end(), out);
        continue;
      }
      for (size_t lane = 0; lane < count; lane++) {
        LN_Snapshot::GamePropertyView view;
        const float read = trees[lane]->GetSnapshot().ReadGameProperty(write.read_ref, view) ?
                               view.float_value :
                               0.0f;
        out[lane] = read + write.add;
      }
    }
    for (size_t lane = 0; lane < count; lane++) {
      LN_RuntimeTree *tree = trees[lane];
      KX_GameObject *owner = tree->GetGameObject();
      const LN_Snapshot &snapshot = tree->GetSnapshot();
      const uint64_t sort_key = (uint64_t(tree->GetSceneObjectIndex()) << 32) |
                                uint64_t(tree->GetAppliedTreeIndex());
      for (size_t column = 0; column < m_writes.size(); column++) {
        const Write &write = m_writes[column];
        LN_PropertyNumber number;
        number.type = LN_ValueType::Float;
        number.float_value = values[column * count + lane];
        uint64_t stamp = 0;
        EXP_Value *handle = snapshot.GetGamePropertyHandle(owner, write.property_ref, stamp);
        buffer.AppendSetGamePropertyNumberRef(
            tree, owner, write.property_ref, number, sort_key, write.source_ref, handle, stamp);
      }
      if (distances[lane] < m_threshold) {
        const Move &approach = m_moves[0];
        buffer.AppendApplyMovement(
            owner, approach.value, approach.local, sort_key, approach.source_ref);
      }
      else {
        const Move &turn = m_moves[1];
        buffer.AppendApplyRotation(owner, turn.rotation, turn.local, sort_key, turn.source_ref);
        const Move &walk = m_moves[2];
        buffer.AppendApplyMovement(owner, walk.value, walk.local, sort_key, walk.source_ref);
      }
    }
  }

  void Record(LN_CommandBuffer &buffer, const std::vector<LN_RuntimeTree *> &trees) const
  {
    const double max_float = double(std::numeric_limits<float>::max());
    for (LN_RuntimeTree *tree : trees) {
      KX_GameObject *owner = tree->GetGameObject();
      const LN_Snapshot &snapshot = tree->GetSnapshot();
      const uint64_t sort_key = (uint64_t(tree->GetSceneObjectIndex()) << 32) |
                                uint64_t(tree->GetAppliedTreeIndex());
      /* Get Distance: the owner to the origin, as the query works it out. */
      const MT_Vector3 a = owner->NodeGetWorldPosition();
      const double dx = double(a.x()), dy = double(a.y()), dz = double(a.z());
      const double squared = dx * dx + dy * dy + dz * dz;
      const float distance = float(std::min(std::sqrt(squared), max_float));
      for (const Write &write : m_writes) {
        LN_PropertyNumber number;
        number.type = LN_ValueType::Float;
        if (write.distance) {
          number.float_value = distance;
        }
        else {
          LN_Snapshot::GamePropertyView view;
          const float read = snapshot.ReadGameProperty(write.read_ref, view) ? view.float_value :
                                                                               0.0f;
          number.float_value = read + write.add;
        }
        uint64_t stamp = 0;
        EXP_Value *handle = snapshot.GetGamePropertyHandle(owner, write.property_ref, stamp);
        buffer.AppendSetGamePropertyNumberRef(
            tree, owner, write.property_ref, number, sort_key, write.source_ref, handle, stamp);
      }
      if (distance < m_threshold) {
        const Move &approach = m_moves[0];
        buffer.AppendApplyMovement(
            owner, approach.value, approach.local, sort_key, approach.source_ref);
      }
      else {
        const Move &turn = m_moves[1];
        buffer.AppendApplyRotation(owner, turn.rotation, turn.local, sort_key, turn.source_ref);
        const Move &walk = m_moves[2];
        buffer.AppendApplyMovement(owner, walk.value, walk.local, sort_key, walk.source_ref);
      }
    }
  }

 private:
  struct Write {
    uint32_t property_ref = LN_INVALID_INDEX;
    uint32_t source_ref = LN_INVALID_INDEX;
    bool distance = false;
    uint32_t read_ref = LN_INVALID_INDEX;
    float add = 0.0f;
  };
  struct Move {
    uint32_t source_ref = LN_INVALID_INDEX;
    bool local = false;
    bool rotation_channel = false;
    MT_Vector3 value = MT_Vector3(0.0f, 0.0f, 0.0f);
    LN_Rotation rotation;
  };
  std::vector<Write> m_writes;
  std::vector<Move> m_moves;
  float m_threshold = 6.0f;
  bool m_ok = false;
};

/** Owners spread over near and far (both branches), properties of their own. */
void SpreadBrainOwners(SemRig &rig, const uint32_t owners)
{
  for (uint32_t index = 0; index < owners; index++) {
    const float angle = float(index) * 0.37f;
    const float radius = 2.0f + float(index % 9);
    rig.Object(index).NodeSetWorldPosition(
        MT_Vector3(radius * std::cos(angle), radius * std::sin(angle), 0.0f));
    rig.Object(index).NodeUpdateGS(0.0);
    for (int chain = 0; chain < 4; chain++) {
      rig.SetFloat(rig.Object(index), ("t" + std::to_string(chain)).c_str(), float(index % 7) + chain);
    }
    rig.SetFloat(rig.Object(index), "dist", 20.0f);
  }
}

}  // namespace

/* The ideal brain is faithful: step after step it records exactly what the lean VM records
 * (every field of every command, record sequences included), objects walking across the
 * threshold, so its speed in DISABLED_IdealBrainBench is a true floor. */
TEST(LN_LeanProgram, TheIdealBrainRecordsExactlyWhatTheLeanVMRecords)
{
  BrainOfOptions options;
  options.step_every = 1;
  const std::shared_ptr<LN_Program> program = BrainOf(options);
  ASSERT_NE(program, nullptr);
  const IdealBrain ideal(*program, LN_Event::OnFixedUpdate, options.threshold);
  ASSERT_TRUE(ideal.Ok()) << LN_DumpProgramText(*program);
  constexpr uint32_t kOwners = 200;
  ModeScope scope(2);
  SemRig lean(program, kMemoOn, kOwners);
  SemRig straight(program, kMemoOn, kOwners);
  SpreadBrainOwners(lean, kOwners);
  SpreadBrainOwners(straight, kOwners);
  lean.Step();
  straight.Step();
  straight.SetRecorder([&](LN_CommandBuffer &buffer,
                           const LN_TickContext & /*context*/,
                           const std::vector<LN_RuntimeTree *> &trees) {
    ideal.Record(buffer, trees);
  });
  SemRig columns(program, kMemoOn, kOwners);
  SpreadBrainOwners(columns, kOwners);
  columns.Step();
  columns.SetRecorder([&](LN_CommandBuffer &buffer,
                          const LN_TickContext & /*context*/,
                          const std::vector<LN_RuntimeTree *> &trees) {
    ideal.RecordColumns(buffer, trees);
  });
  for (int step = 0; step < 40; step++) {
    lean.Step();
    straight.Step();
    columns.Step();
    ASSERT_TRUE(lean.LastRecorded() == straight.LastRecorded())
        << "step " << step << ": "
        << diff::FirstDifference(lean.LastRecorded(), straight.LastRecorded());
    ASSERT_TRUE(lean.LastRecorded() == columns.LastRecorded())
        << "step " << step << " (columns): "
        << diff::FirstDifference(lean.LastRecorded(), columns.LastRecorded());
  }
  EXPECT_FALSE(lean.LastRecorded().empty());
}

/* The floor (DISABLED_): the brain on 10,000 objects, one thread, recorded by the old
 * interpreter, the lean VM and the ideal straight code. The gap between the lean VM and the
 * ideal is all any block, kernel or compiler work could ever win on the recording. */
TEST(LN_LeanProgram, DISABLED_IdealBrainBench)
{
  BrainOfOptions options;
  options.step_every = 1;
  const std::shared_ptr<LN_Program> program = BrainOf(options);
  ASSERT_NE(program, nullptr);
  const IdealBrain ideal(*program, LN_Event::OnFixedUpdate, options.threshold);
  ASSERT_TRUE(ideal.Ok());
  constexpr uint32_t kOwners = 10000;
  const char *rounds_text = std::getenv("UPBGE_LN_BENCH_ROUNDS");
  const int rounds = rounds_text != nullptr ? std::max(1, std::atoi(rounds_text)) : 40;
  struct Setup {
    const char *name;
    int lean;
    int straight; /* 1 per tree, 2 in columns */
    int routes;
  };
  for (const Setup &setup : {Setup{"old", 0, 0, 0},
                             Setup{"lean", 2, 0, 0},
                             Setup{"route", 2, 0, 1},
                             Setup{"ideal", 2, 1, 0},
                             Setup{"cols", 2, 2, 0}})
  {
    ModeScope scope(setup.lean);
    RouteScope routes(setup.routes);
    SemRig rig(program, kMemoOn, kOwners);
    rig.SetDump(false);
    rig.SetCapturePrints(false);
    SpreadBrainOwners(rig, kOwners);
    rig.Step();
    rig.Step();
    if (setup.straight != 0) {
      const bool in_columns = setup.straight == 2;
      rig.SetRecorder([&ideal, in_columns](LN_CommandBuffer &buffer,
                                           const LN_TickContext & /*context*/,
                                           const std::vector<LN_RuntimeTree *> &trees) {
        if (in_columns) {
          ideal.RecordColumns(buffer, trees);
        }
        else {
          ideal.Record(buffer, trees);
        }
      });
    }
    SemRig::PhaseTimes best = {1.0e9, 1.0e9, 1.0e9};
    for (int round = 0; round < rounds; round++) {
      rig.ResetTimes();
      rig.Step();
      const SemRig::PhaseTimes &t = rig.Times();
      best.capture_ms = std::min(best.capture_ms, t.capture_ms);
      best.record_ms = std::min(best.record_ms, t.record_ms);
      best.flush_ms = std::min(best.flush_ms, t.flush_ms);
    }
    std::printf("IDEAL_BENCH brain10k %-5s  snapshot %.0f  record %.0f  flush %.0f ns an NPC\n",
                setup.name,
                best.capture_ms * 1.0e6 / kOwners,
                best.record_ms * 1.0e6 / kOwners,
                best.flush_ms * 1.0e6 / kOwners);
  }
}

/* What a tree costs the lean VM before its ops (DISABLED_): one number write per tree on 10,000
 * objects, the lean VM against the single append written straight. The difference is ExecuteReady's
 * per tree work plus one op's dispatch: what a compiled form of the routes would still pay in part. */
TEST(LN_LeanProgram, DISABLED_TreeEntryBench)
{
  TreeBuilder b;
  GenNode &event = b.Add("LogicNativeOnFixedUpdate");
  GenNode &set = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(set, "Property", "dist");
  b.Float(set, "Value", 1.5f);
  b.Link(event, "Out", set, "Flow");
  std::string report;
  const std::shared_ptr<LN_Program> program = b.Compile(&report);
  ASSERT_NE(program, nullptr) << report;
  constexpr uint32_t kOwners = 10000;
  for (const bool straight : {false, true}) {
    ModeScope scope(2);
    SemRig rig(program, kMemoOn, kOwners);
    rig.SetDump(false);
    rig.SetCapturePrints(false);
    for (uint32_t index = 0; index < kOwners; index++) {
      rig.SetFloat(rig.Object(index), "dist", 0.0f);
    }
    rig.Step();
    rig.Step();
    if (straight) {
      rig.SetRecorder([](LN_CommandBuffer &buffer,
                         const LN_TickContext & /*context*/,
                         const std::vector<LN_RuntimeTree *> &trees) {
        for (LN_RuntimeTree *tree : trees) {
          KX_GameObject *owner = tree->GetGameObject();
          LN_PropertyNumber number;
          number.type = LN_ValueType::Float;
          number.float_value = 1.5f;
          uint64_t stamp = 0;
          EXP_Value *handle = tree->GetSnapshot().GetGamePropertyHandle(owner, 0, stamp);
          buffer.AppendSetGamePropertyNumberRef(
              tree,
              owner,
              0,
              number,
              (uint64_t(tree->GetSceneObjectIndex()) << 32) | tree->GetAppliedTreeIndex(),
              1,
              handle,
              stamp);
        }
      });
    }
    double best = 1.0e9;
    for (int round = 0; round < 40; round++) {
      rig.ResetTimes();
      rig.Step();
      best = std::min(best, rig.Times().record_ms);
    }
    std::printf("ENTRY_BENCH one write %-8s record %.0f ns a tree\n",
                straight ? "straight" : "lean",
                best * 1.0e6 / kOwners);
  }
}
