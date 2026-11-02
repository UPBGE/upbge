/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Semantics tests (lean runtime plan, phase A4): the rules of today's interpreter written down
 * as tests, so the lean runtime can be held to them and the agreed fixes (decision D1, phase A15)
 * change exactly what the quirk tests say they change.
 *
 * A test named Quirk... pinned a result that looked like a bug (the memo, the rotation cache, a
 * snapshot read ...); phase A15 fixed all seven and the tests now say the agreed behaviour in every
 * mode. The others pin ordinary rules (defaults, evaluation order, state). Every test runs in the
 * production setup spelled out in LN_SemRig.h and, where a switch matters, in the four modes
 * (production, memo on, memo off, profiled). */

#include "testing/testing.h"

#include <cmath>
#include <cstdarg>
#include <map>
#include <sstream>

#include "LN_SemRig.h"

using namespace ln_test;
using namespace ln_test::sem;

namespace {

float ZAngle(KX_GameObject &object)
{
  const MT_Matrix3x3 m = object.NodeGetLocalOrientation();
  return std::atan2(float(m[1][0]), float(m[0][0]));
}

std::string Fmt(const char *format, ...)
{
  char text[300];
  va_list args;
  va_start(args, format);
  std::vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  return text;
}

/** A float property of the first owner after one Logic + Physics step... helpers for the trees. */
const char *ModeName(const SemMode &mode)
{
  return mode.name;
}

/** Compiles `b` (the test fails when it does not compile). */
std::shared_ptr<LN_Program> CompileOrFail(TreeBuilder &b)
{
  std::string report;
  std::shared_ptr<LN_Program> program = b.Compile(&report);
  EXPECT_NE(program, nullptr) << report;
  return program;
}

/* ---------------------------------------------------------------------------------------------
 * The former quirk tests. Plan phase A4 pinned seven results that looked like bugs (decision D1);
 * phase A15 fixed them, and these tests now say the agreed behaviour in every mode: the per-tick
 * memo never changes a result, and a profiled run equals an unprofiled one.
 * ------------------------------------------------------------------------------------------- */

/* D1 (a), fixed in A15: Set Tree Property writes the tree's property at once, and a Get Tree Property
 * read after it in the same tick sees the new value in every mode. (Before, the register memo kept
 * the first read of the tick: 0 instead of 5.) A tree property is a mutable read; it is never kept. */
TEST(LN_Semantics, AGetTreePropertyReadAfterASetInOnePassSeesTheNewValueInEveryMode)
{
  for (const SemMode &mode : AllModes()) {
    SCOPED_TRACE(mode.name);
    TreeBuilder b;
    GenNode &event = b.Add("LogicNativeOnFixedUpdate");
    GenNode &get = b.Add("LogicNativeGetTreeProperty");
    b.String(get, "Property", "p");
    GenNode &sequence = b.Add("LogicNativeSequence", 0, 0, 3);
    b.Link(event, "Out", sequence, "Flow");
    GenNode &first = b.Add("LogicNativeSetGamePropertyFloat");
    b.String(first, "Property", "before");
    b.Link(sequence, "Then 0", first, "Flow");
    b.Link(get, "Value", first, "Value");
    GenNode &seed = b.Add("LogicNativeGetGamePropertyFloat");
    b.String(seed, "Property", "seed");
    GenNode &set = b.Add("LogicNativeSetTreeProperty");
    b.String(set, "Property", "p");
    b.Link(sequence, "Then 1", set, "Flow");
    b.Link(seed, "Value", set, "Value");
    GenNode &second = b.Add("LogicNativeSetGamePropertyFloat");
    b.String(second, "Property", "after");
    b.Link(sequence, "Then 2", second, "Flow");
    b.Link(get, "Value", second, "Value");
    const std::shared_ptr<LN_Program> program = CompileOrFail(b);
    ASSERT_NE(program, nullptr);
    SemRig rig(program, mode);
    rig.SetFloat(rig.Object(0), "seed", 5.0f);
    rig.Step();
    EXPECT_EQ(rig.Float(rig.Object(0), "before"), 0.0f);
    EXPECT_EQ(rig.Float(rig.Object(0), "after"), 5.0f) << "the write is seen";
  }
}

/* D1 (a), across a forced pass, fixed in A15: a Run Logic Tree pass in the same tick sees the tree
 * property as the normal pass left it, so a counter written as "p = p + 1" counts once per pass in
 * every mode (the memo used to hold the first read of the tick). */
TEST(LN_Semantics, ARunLogicTreePassSeesTheTreePropertyTheNormalPassLeft)
{
  const struct {
    const SemMode *mode;
    const char *printed;
  } expected[] = {
      /* tick 1: normal pass, forced pass; tick 2: normal pass. Per pass: p before and after the set. */
      {&kProduction, "None\n1\n1\n2\n2\n3\n"},
      {&kMemoOn, "None\n1\n1\n2\n2\n3\n"},
      {&kMemoOff, "None\n1\n1\n2\n2\n3\n"},
      {&kProfiled, "None\n1\n1\n2\n2\n3\n"},
  };
  for (const auto &entry : expected) {
    SCOPED_TRACE(entry.mode->name);
    TreeBuilder b;
    GenNode &event = b.Add("LogicNativeOnFixedUpdate");
    GenNode &get = b.Add("LogicNativeGetTreeProperty");
    b.String(get, "Property", "p");
    GenNode &sequence = b.Add("LogicNativeSequence", 0, 0, 3);
    b.Link(event, "Out", sequence, "Flow");
    GenNode &print_before = b.Add("LogicNativePrint");
    b.Link(sequence, "Then 0", print_before, "Flow");
    b.Link(get, "Value", print_before, "Message");
    GenNode &add = b.Add("LogicNativeMath", blender::NODE_MATH_ADD);
    b.Link(get, "Value", add, "A");
    b.Float(add, "B", 1.0f);
    GenNode &set = b.Add("LogicNativeSetTreeProperty");
    b.String(set, "Property", "p");
    b.Link(sequence, "Then 1", set, "Flow");
    b.Link(add, "Result", set, "Value");
    GenNode &print_after = b.Add("LogicNativePrint");
    b.Link(sequence, "Then 2", print_after, "Flow");
    b.Link(get, "Value", print_after, "Message");
    const std::shared_ptr<LN_Program> program = CompileOrFail(b);
    ASSERT_NE(program, nullptr);
    SemRig rig(program, *entry.mode);
    rig.Tick(LN_Event::OnFixedUpdate, [&](LN_CommandBuffer &buffer, const LN_TickContext &context) {
      rig.Force(0, buffer, context);
    });
    rig.Tick(LN_Event::OnFixedUpdate);
    EXPECT_EQ(rig.Printed(), entry.printed);
  }
}

/* D1 (b), fixed in A15: Unscaled Delta Time (While Paused's Delta Time) is a context read and is never
 * kept for the tick. In step 2 the Every 2 route reads 2 / 60 s while the ungated route reads
 * 1 / 60 s, in every mode. */
TEST(LN_Semantics, UnscaledDeltaTimeOfAGatedRouteCoversItsStepsWhateverTheUngatedRouteRead)
{
  for (const SemMode &mode : AllModes()) {
    SCOPED_TRACE(mode.name);
    TreeBuilder b;
    GenNode &paused = b.Add("LogicNativeWhilePaused");
    GenNode &gated = b.Add("LogicNativeOnFixedUpdate", 2, 0);
    GenNode &free_event = b.Add("LogicNativeOnFixedUpdate", 1, 0);
    GenNode &set_gated = b.Add("LogicNativeSetGamePropertyFloat");
    b.String(set_gated, "Property", "gated");
    b.Link(gated, "Out", set_gated, "Flow");
    b.Link(paused, "Delta Time", set_gated, "Value");
    GenNode &set_free = b.Add("LogicNativeSetGamePropertyFloat");
    b.String(set_free, "Property", "free");
    b.Link(free_event, "Out", set_free, "Flow");
    b.Link(paused, "Delta Time", set_free, "Value");
    const std::shared_ptr<LN_Program> program = CompileOrFail(b);
    ASSERT_NE(program, nullptr);
    SemRig rig(program, mode);
    for (int step = 0; step < 3; step++) {
      rig.Step();
    }
    const float one_step = float(SemRig::kStep);
    EXPECT_EQ(rig.Float(rig.Object(0), "free"), one_step);
    EXPECT_EQ(Bits(rig.Float(rig.Object(0), "gated")), Bits(float(2.0 * SemRig::kStep)));
  }
}

/* D1 (c), the rotation cache, removed in A15: a rotation expression is worked out at every read. An
 * Apply Rotation in a Loop whose rotation is the loop item turns the object by 0.1 + 0.2 + 0.4
 * radians (the cache turned it by the first item three times: 0.3). */
TEST(LN_Semantics, ARotationFromLoopItemsIsEachItemInTurn)
{
  for (const SemMode &mode : AllModes()) {
    SCOPED_TRACE(mode.name);
    TreeBuilder b;
    GenNode &event = b.Add("LogicNativeOnFixedUpdate");
    GenNode &list = b.Add("LogicNativeListFromItems", 5); /* custom1 5: the Vectors input */
    for (int i = 0; i < 3; i++) {
      GenNode &v = b.Add("LogicNativeCombineXYZ");
      b.Float(v, "Z", 0.1f * float(1 << i));
      b.Link(v, "Vector", list, "Vectors");
    }
    GenNode &loop = b.Add("LogicNativeLoopFromList");
    b.Link(event, "Out", loop, "Flow");
    b.Link(list, "Vectors", loop, "List");
    GenNode &rotate = b.Add("LogicNativeApplyRotation");
    b.Link(loop, "Loop", rotate, "Flow");
    b.Link(loop, "List Item", rotate, "Rotation");
    b.Bool(rotate, "Local", true);
    const std::shared_ptr<LN_Program> program = CompileOrFail(b);
    ASSERT_NE(program, nullptr);
    SemRig rig(program, mode);
    rig.Step();
    EXPECT_NEAR(ZAngle(rig.Object(0)), 0.7f, 1.0e-5f);
  }
}

/* The same loop with Set Object Attribute (World Rotation): the three items set the orientation one
 * after the other and it ends at the last item (Z 0.4), in every mode. */
TEST(LN_Semantics, SetOrientationFromLoopItemsEndsAtTheLastItem)
{
  for (const SemMode &mode : AllModes()) {
    SCOPED_TRACE(mode.name);
    TreeBuilder b;
    GenNode &event = b.Add("LogicNativeOnFixedUpdate");
    GenNode &list = b.Add("LogicNativeListFromItems", 5);
    for (int i = 0; i < 3; i++) {
      GenNode &v = b.Add("LogicNativeCombineXYZ");
      b.Float(v, "Z", 0.1f * float(1 << i));
      b.Link(v, "Vector", list, "Vectors");
    }
    GenNode &loop = b.Add("LogicNativeLoopFromList");
    b.Link(event, "Out", loop, "Flow");
    b.Link(list, "Vectors", loop, "List");
    GenNode &set = b.Add("LogicNativeSetObjectAttribute", 1); /* World Rotation */
    b.Link(loop, "Loop", set, "Flow");
    b.Link(loop, "List Item", set, "Rotation");
    const std::shared_ptr<LN_Program> program = CompileOrFail(b);
    ASSERT_NE(program, nullptr);
    SemRig rig(program, mode);
    rig.Step();
    EXPECT_NEAR(ZAngle(rig.Object(0)), 0.4f, 1.0e-5f);
  }
}

/* D1 (c), another object's orientation, fixed in A15: a normal pass and a Run Logic Tree pass of one tick
 * read Target's orientation through one rotation expression. The second pass reads the live
 * orientation (Target has turned on to 1.0 after a Get Distance applied its pending turn), so the
 * owner ends at 1.5 (the rotation cache gave 1.0). */
TEST(LN_Semantics, AnotherObjectsOrientationIsReadLiveInEveryPassOfOneTick)
{
  for (const SemMode &mode : AllModes()) {
    SCOPED_TRACE(mode.name);
    TreeBuilder b;
    GenNode &event = b.Add("LogicNativeOnFixedUpdate");
    GenNode &attr = b.Add("LogicNativeGetObjectAttribute", 8); /* World Rotation */
    b.ObjectNamed(attr, "Object", &diff::TargetBlenderObject());
    GenNode &sequence = b.Add("LogicNativeSequence", 0, 0, 3);
    b.Link(event, "Out", sequence, "Flow");
    GenNode &turn = b.Add("LogicNativeApplyRotation");
    b.ObjectNamed(turn, "Object", &diff::TargetBlenderObject());
    b.Link(sequence, "Then 0", turn, "Flow");
    b.Rotation(turn, "Rotation", 0.0f, 0.0f, 0.5f);
    b.Bool(turn, "Local", true);
    GenNode &distance = b.Add("LogicNativeGetDistance", 0, 0);
    b.ObjectNamed(distance, "Object B", &diff::TargetBlenderObject());
    GenNode &store = b.Add("LogicNativeSetGamePropertyFloat");
    b.String(store, "Property", "d");
    b.Link(sequence, "Then 1", store, "Flow");
    b.Link(distance, "Distance", store, "Value");
    GenNode &copy = b.Add("LogicNativeApplyRotation");
    b.Link(sequence, "Then 2", copy, "Flow");
    b.Link(attr, "Orientation", copy, "Rotation");
    b.Bool(copy, "Local", true);
    const std::shared_ptr<LN_Program> program = CompileOrFail(b);
    ASSERT_NE(program, nullptr);
    SemRig rig(program, mode);
    rig.AddObject("Target", MT_Vector3(3.0f, 4.0f, 0.0f));
    rig.Tick(LN_Event::OnFixedUpdate, [&](LN_CommandBuffer &buffer, const LN_TickContext &context) {
      rig.Force(0, buffer, context);
    });
    EXPECT_NEAR(ZAngle(rig.Object(1)), 1.0f, 1.0e-5f);
    EXPECT_NEAR(ZAngle(rig.Object(0)), 1.5f, 1.0e-5f);
    EXPECT_EQ(rig.Float(rig.Object(0), "d"), 5.0f);
  }
}

/* D1 (d) and (e), fixed in A15: Get Axis Vector of the own object reads the tick's snapshot (the pose at
 * the start of the step) in every mode, profiled or not. A turn of 90 degrees was applied by a Get
 * Distance's pending-move apply before the read; the axis is still the start-of-step X axis. */
TEST(LN_Semantics, GetAxisVectorOfTheOwnObjectIsTheSnapshotInEveryMode)
{
  struct Axis {
    float x, y;
  };
  std::map<std::string, Axis> seen;
  for (const SemMode &mode : AllModes()) {
    SCOPED_TRACE(mode.name);
    TreeBuilder b;
    GenNode &event = b.Add("LogicNativeOnFixedUpdate");
    GenNode &sequence = b.Add("LogicNativeSequence", 0, 0, 3);
    b.Link(event, "Out", sequence, "Flow");
    GenNode &turn = b.Add("LogicNativeApplyRotation");
    b.Link(sequence, "Then 0", turn, "Flow");
    b.Rotation(turn, "Rotation", 0.0f, 0.0f, 1.5707964f);
    b.Bool(turn, "Local", true);
    GenNode &distance = b.Add("LogicNativeGetDistance", 0, 0);
    b.ObjectNamed(distance, "Object B", &diff::TargetBlenderObject());
    GenNode &store = b.Add("LogicNativeSetGamePropertyFloat");
    b.String(store, "Property", "d");
    b.Link(sequence, "Then 1", store, "Flow");
    b.Link(distance, "Distance", store, "Value");
    GenNode &axis = b.Add("LogicNativeGetAxisVector", 0); /* the X axis */
    GenNode &split = b.Add("LogicNativeSeparateXYZ");
    b.Link(axis, "Vector", split, "Vector");
    GenNode &ax = b.Add("LogicNativeSetGamePropertyFloat");
    b.String(ax, "Property", "ax");
    b.Link(sequence, "Then 2", ax, "Flow");
    b.Link(split, "X", ax, "Value");
    GenNode &ay = b.Add("LogicNativeSetGamePropertyFloat");
    b.String(ay, "Property", "ay");
    b.Link(ax, "Done", ay, "Flow");
    b.Link(split, "Y", ay, "Value");
    const std::shared_ptr<LN_Program> program = CompileOrFail(b);
    ASSERT_NE(program, nullptr);
    SemRig rig(program, mode);
    rig.AddObject("Target", MT_Vector3(3.0f, 4.0f, 0.0f));
    rig.Step();
    seen[mode.name] = {rig.Float(rig.Object(0), "ax"), rig.Float(rig.Object(0), "ay")};
  }
  const auto expect_snapshot = [&](const char *name) {
    EXPECT_NEAR(seen[name].x, 1.0f, 1.0e-6f) << name;
    EXPECT_NEAR(seen[name].y, 0.0f, 1.0e-6f) << name;
  };
  for (const SemMode &mode : AllModes()) {
    expect_snapshot(mode.name);
  }
}

/* D1 (f), folding, fixed in A15: a folded Invert Value or Absolute of constants gives the same zero as
 * the runtime. Invert of 0.0 is -0.0 and Absolute of -0.0 is +0.0, folded or not. (The compiler
 * folded them right but its constant table merged a folded -0.0 with the +0.0 already there:
 * constants are now compared by their bits.) */
TEST(LN_Semantics, FoldedConstantsAndRuntimeReadsGiveTheSameZero)
{
  for (const bool absolute : {false, true}) {
    for (const bool folded : {false, true}) {
      SCOPED_TRACE(std::string(absolute ? "Absolute(-0.0)" : "Invert(0.0)") +
                   (folded ? " folded" : " runtime"));
      TreeBuilder b;
      GenNode &event = b.Add("LogicNativeOnFixedUpdate");
      GenNode &node = absolute ? b.Add("LogicNativeMath", blender::NODE_MATH_ABSOLUTE) :
                                 b.Add("LogicNativeInvertValue");
      const char *pin = absolute ? "A" : "Value";
      const float value = absolute ? -0.0f : 0.0f;
      GenNode &read = b.Add("LogicNativeGetGamePropertyFloat");
      b.String(read, "Property", "x");
      if (folded) {
        b.Float(node, pin, value);
      }
      else {
        b.Link(read, "Value", node, pin);
      }
      GenNode &store = b.Add("LogicNativeSetGamePropertyFloat");
      b.String(store, "Property", "r");
      b.Link(event, "Out", store, "Flow");
      b.Link(node, absolute ? "Result" : "Value", store, "Value");
      const std::shared_ptr<LN_Program> program = CompileOrFail(b);
      ASSERT_NE(program, nullptr);
      SemRig rig(program, kProduction);
      rig.SetFloat(rig.Object(0), "x", value);
      rig.Step();
      const bool negative_zero = Bits(rig.Float(rig.Object(0), "r")) == "0x80000000";
      /* Invert of 0.0 is -0.0; Absolute of -0.0 is +0.0; folded or not. */
      EXPECT_EQ(negative_zero, !absolute);
    }
  }
}


/* ---------------------------------------------------------------------------------------------
 * Semantics tests: ordinary rules of today's interpreter, pinned.
 * ------------------------------------------------------------------------------------------- */

/** The names of the instructions of one event in a program dump, in order ("Op::Nop" ...). */
std::vector<std::string> OpNames(const LN_Program &program, const char *event)
{
  std::vector<std::string> names;
  std::istringstream lines(LN_DumpProgramText(program));
  std::string line;
  bool inside = false;
  const std::string header = std::string("instructions ") + event + " (";
  while (std::getline(lines, line)) {
    if (line.rfind("instructions ", 0) == 0) {
      inside = line.rfind(header, 0) == 0;
      continue;
    }
    if (inside && line.rfind("  [", 0) == 0) {
      const size_t at = line.find("Op::");
      if (at != std::string::npos) {
        names.push_back(line.substr(at, line.find(' ', at) - at));
      }
    }
    else if (inside && line.rfind("blocks ", 0) == 0) {
      inside = false;
    }
  }
  return names;
}

/* The NPC brain (research.py brain_of, the benchmark's tree) compiles to 16 instructions: the
 * distance stored, then Done routes (BranchRoute) leading the four bookkeeping chains one after the
 * other, the near/far branch and the moves. The optimizer phases keep this shape: the goldens pin
 * the whole listing, this test names the part everything else depends on. */
TEST(LN_Semantics, TheBrainsSixteenOpsAndItsDoneRoutesAreAsCompiled)
{
  BrainOfOptions options;
  options.chains = 4;
  const std::shared_ptr<LN_Program> program = BrainOf(options);
  ASSERT_NE(program, nullptr);
  const std::vector<std::string> expected = {
      "Op::Nop",
      "Op::SetGameProperty",
      "Op::BranchRoute",
      "Op::BranchRoute",
      "Op::BranchRoute",
      "Op::SetGameProperty",
      "Op::BranchRoute",
      "Op::SetGameProperty",
      "Op::BranchRoute",
      "Op::SetGameProperty",
      "Op::BranchRoute",
      "Op::SetGameProperty",
      "Op::ApplyTransformVector",
      "Op::BranchRoute",
      "Op::ApplyTransformVector",
      "Op::ApplyTransformVector",
  };
  EXPECT_EQ(OpNames(*program, "OnUpdate"), expected);
  EXPECT_TRUE(OpNames(*program, "OnFixedUpdate").empty());
  /* Each Done route is a BranchRoute whose pulse is "that route ran" (InstructionExecuted), and
   * every chain step is guarded by the pulse of the route before it. */
  const std::string dump = LN_DumpProgramText(*program);
  size_t pulses = 0;
  for (size_t at = dump.find("Bool::InstructionExecuted"); at != std::string::npos;
       at = dump.find("Bool::InstructionExecuted", at + 1))
  {
    pulses++;
  }
  EXPECT_EQ(pulses, 7u);
}

/* And and Or give their truth tables whichever mode runs. (The register path evaluates both
 * operands, the tree path only the second when it must; a Flow Gate's Is Open keeps its state in
 * its own instruction, so neither shows: a stateful operand that does not exist as data output.) */
TEST(LN_Semantics, AndAndOrOfAFlagAndAFlowGateStateGiveTheirTruthTableInEveryMode)
{
  for (const int op : {0, 1}) {
    for (const SemMode &mode : AllModes()) {
      SCOPED_TRACE(std::string(op == 0 ? "And " : "Or ") + mode.name);
      TreeBuilder b;
      GenNode &event = b.Add("LogicNativeOnFixedUpdate");
      GenNode &flag = b.Add("LogicNativeGetGamePropertyBool");
      b.String(flag, "Property", "flag");
      GenNode &flow_gate = b.Add("LogicNativeFlowGate");
      b.Link(event, "Out", flow_gate, "Toggle");
      GenNode &gate = b.Add("LogicNativeGate", int16_t(op));
      b.Link(flag, "Value", gate, "Conditions");
      b.Link(flow_gate, "Is Open", gate, "Conditions");
      GenNode &branch = b.Add("LogicNativeBranch");
      b.Link(event, "Out", branch, "Flow");
      b.Link(gate, "Result", branch, "Condition");
      GenNode &hit = b.Add("LogicNativeSetGamePropertyFloat");
      b.String(hit, "Property", "hit");
      b.Float(hit, "Value", 1.0f);
      b.Link(branch, "True", hit, "Flow");
      const std::shared_ptr<LN_Program> program = CompileOrFail(b);
      ASSERT_NE(program, nullptr);
      SemRig rig(program, mode);
      std::string hits;
      for (int step = 0; step < 3; step++) {
        rig.SetFloat(rig.Object(0), "hit", 0.0f);
        rig.SetBool(rig.Object(0), "flag", op == 0 ? step >= 1 : step < 1);
        rig.Step();
        hits += rig.Float(rig.Object(0), "hit") > 0.5f ? '1' : '0';
      }
      /* The gate opens on the first toggle, closes on the second, opens on the third. And needs the
       * flag (steps 1, 2) and the open gate (steps 0, 2); Or needs either (flag: step 0 only). */
      EXPECT_EQ(hits, op == 0 ? "001" : "101");
    }
  }
}

/* Random: every object gets the same number in a tick (the seed is the expression index and the
 * tick: decision D2, kept through A and B), a new number each tick, and two Random nodes differ. */
TEST(LN_Semantics, RandomGivesEveryObjectTheSameNumberInATickAndTwoNodesDiffer)
{
  for (const SemMode &mode : {kProduction, kMemoOff}) {
    SCOPED_TRACE(mode.name);
    TreeBuilder b;
    GenNode &event = b.Add("LogicNativeOnFixedUpdate");
    GenNode &first = b.Add("LogicNativeRandomFloat");
    b.Float(first, "Min", 0.0f);
    b.Float(first, "Max", 1.0f);
    GenNode &second = b.Add("LogicNativeRandomFloat");
    b.Float(second, "Min", 0.0f);
    b.Float(second, "Max", 1.0f);
    GenNode &sequence = b.Add("LogicNativeSequence", 0, 0, 2);
    b.Link(event, "Out", sequence, "Flow");
    GenNode &set_first = b.Add("LogicNativeSetGamePropertyFloat");
    b.String(set_first, "Property", "r1");
    b.Link(sequence, "Then 0", set_first, "Flow");
    b.Link(first, "Value", set_first, "Value");
    GenNode &set_second = b.Add("LogicNativeSetGamePropertyFloat");
    b.String(set_second, "Property", "r2");
    b.Link(sequence, "Then 1", set_second, "Flow");
    b.Link(second, "Value", set_second, "Value");
    const std::shared_ptr<LN_Program> program = CompileOrFail(b);
    ASSERT_NE(program, nullptr);
    SemRig rig(program, mode, 3);
    std::string previous;
    for (int step = 0; step < 2; step++) {
      rig.Step();
      const std::string r1 = Bits(rig.Float(rig.Object(0), "r1"));
      const std::string r2 = Bits(rig.Float(rig.Object(0), "r2"));
      for (int owner = 1; owner < 3; owner++) {
        EXPECT_EQ(Bits(rig.Float(rig.Object(owner), "r1")), r1);
        EXPECT_EQ(Bits(rig.Float(rig.Object(owner), "r2")), r2);
      }
      EXPECT_NE(r1, r2) << "the seed includes the expression index";
      EXPECT_NE(r1, previous) << "a new number every tick";
      previous = r1;
    }
  }
}

/* A constant object by name that does not exist yet writes nothing (and not to the owner); it is
 * found as soon as it exists (the scene's object list changed). */
TEST(LN_Semantics, AConstantObjectMissingAtFirstIsFoundWhenItIsSpawned)
{
  TreeBuilder b;
  GenNode &event = b.Add("LogicNativeOnFixedUpdate");
  GenNode &set = b.Add("LogicNativeSetGamePropertyFloat");
  b.ObjectNamed(set, "Object", &diff::TargetBlenderObject());
  b.String(set, "Property", "mark");
  b.Float(set, "Value", 7.0f);
  b.Link(event, "Out", set, "Flow");
  const std::shared_ptr<LN_Program> program = CompileOrFail(b);
  ASSERT_NE(program, nullptr);
  SemRig rig(program, kProduction);
  rig.Step();
  EXPECT_FALSE(rig.Has(rig.Object(0), "mark"));
  KX_GameObject &target = rig.AddObject("Target", MT_Vector3(1.0f, 2.0f, 3.0f));
  rig.Step();
  EXPECT_EQ(rig.Float(target, "mark"), 7.0f);
  EXPECT_FALSE(rig.Has(rig.Object(0), "mark"));
}

/* An empty Object input is the tree's own object: two owners of one program each write their own. */
TEST(LN_Semantics, AnEmptyObjectInputIsTheOwnerOfEachTree)
{
  TreeBuilder b;
  GenNode &event = b.Add("LogicNativeOnFixedUpdate");
  GenNode &set = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(set, "Property", "mark");
  b.Float(set, "Value", 3.0f);
  b.Link(event, "Out", set, "Flow");
  const std::shared_ptr<LN_Program> program = CompileOrFail(b);
  ASSERT_NE(program, nullptr);
  SemRig rig(program, kProduction, 2);
  rig.Step();
  EXPECT_EQ(rig.Float(rig.Object(0), "mark"), 3.0f);
  EXPECT_EQ(rig.Float(rig.Object(1), "mark"), 3.0f);
}

/* A Flow Gate refuses while it is closed (its Exit route is not marked, so what follows does not
 * run) and passes while it is open. It starts closed unless Start Open is set. */
TEST(LN_Semantics, AClosedFlowGateRefusesAndDoesNotMarkItsExitRoute)
{
  for (const bool start_open : {false, true}) {
    SCOPED_TRACE(start_open ? "start open" : "start closed");
    TreeBuilder b;
    GenNode &event = b.Add("LogicNativeOnFixedUpdate");
    GenNode &gate = b.Add("LogicNativeFlowGate");
    b.Bool(gate, "Start Open", start_open);
    b.Link(event, "Out", gate, "Enter");
    GenNode &after = b.Add("LogicNativeSetGamePropertyFloat");
    b.String(after, "Property", "after");
    b.Float(after, "Value", 1.0f);
    b.Link(gate, "Exit", after, "Flow");
    const std::shared_ptr<LN_Program> program = CompileOrFail(b);
    ASSERT_NE(program, nullptr);
    SemRig rig(program, kProduction);
    rig.Step();
    EXPECT_EQ(rig.Has(rig.Object(0), "after"), start_open);
  }
}

/* Store Value stores when its Flow arrives and gives the stored value to every read, so a read
 * that runs before the store in a tick gets the value of the earlier store. The very first read of
 * an untouched Store Value initializes it from its input (here x = 1 in step 1, before any Flow). */
TEST(LN_Semantics, StoreValueStoresWhenFlowArrivesAndReadsGiveTheLastStoredValue)
{
  TreeBuilder b;
  GenNode &event = b.Add("LogicNativeOnFixedUpdate");
  GenNode &x = b.Add("LogicNativeGetGamePropertyFloat");
  b.String(x, "Property", "x");
  GenNode &store = b.Add("LogicNativeStoreValue");
  b.Link(x, "Value", store, "Value");
  GenNode &sequence = b.Add("LogicNativeSequence", 0, 0, 2);
  b.Link(event, "Out", sequence, "Flow");
  GenNode &latest = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(latest, "Property", "latest");
  b.Link(sequence, "Then 0", latest, "Flow");
  b.Link(store, "Value", latest, "Value");
  GenNode &flag = b.Add("LogicNativeGetGamePropertyBool");
  b.String(flag, "Property", "flag");
  GenNode &branch = b.Add("LogicNativeBranch");
  b.Link(sequence, "Then 1", branch, "Flow");
  b.Link(flag, "Value", branch, "Condition");
  b.Link(branch, "True", store, "Flow");
  const std::shared_ptr<LN_Program> program = CompileOrFail(b);
  ASSERT_NE(program, nullptr);
  SemRig rig(program, kMemoOff);
  const struct {
    float x;
    bool flag;
    float latest;
  } steps[] = {{1.0f, true, 1.0f}, {2.0f, false, 1.0f}, {3.0f, true, 1.0f}, {4.0f, false, 3.0f}};
  for (const auto &step : steps) {
    rig.SetFloat(rig.Object(0), "x", step.x);
    rig.SetBool(rig.Object(0), "flag", step.flag);
    rig.Step();
    EXPECT_EQ(rig.Float(rig.Object(0), "latest"), step.latest) << "x = " << step.x;
  }
}


/* A constant object in a menu copy: a tree's name lookup goes through the scene's overlay rule, so
 * the tree of a player's menu copy finds that copy's object where the name is the source's. */
TEST(LN_Semantics, AConstantObjectInAMenuCopyIsTheCopysCounterpart)
{
  TreeBuilder b;
  GenNode &event = b.Add("LogicNativeOnFixedUpdate");
  GenNode &set = b.Add("LogicNativeSetGamePropertyFloat");
  b.ObjectNamed(set, "Object", &diff::TargetBlenderObject());
  b.String(set, "Property", "mark");
  b.Float(set, "Value", 9.0f);
  b.Link(event, "Out", set, "Flow");
  const std::shared_ptr<LN_Program> program = CompileOrFail(b);
  ASSERT_NE(program, nullptr);
  SemRig rig(program, kProduction);
  KX_GameObject &source = rig.AddObject("Target", MT_Vector3(1.0f, 0.0f, 0.0f));
  KX_GameObject &copy = rig.AddObject("Target.001", MT_Vector3(2.0f, 0.0f, 0.0f));
  rig.SetOverlayCounterpart([&](KX_GameObject *object, KX_GameObject *viewer) -> KX_GameObject * {
    return object == &source && viewer == &rig.Object(0) ? &copy : object;
  });
  rig.Step();
  EXPECT_EQ(rig.Float(copy, "mark"), 9.0f);
  EXPECT_FALSE(rig.Has(source, "mark"));
}

/* The tree property a Delay's Done route reads: the Delay keeps a copy of the tree's properties from
 * the moment it was armed and gives it to an op guarded by DelayDone itself (the swap). The compiler
 * guards the ops after a Delay by the Done route's pulse (an InstructionExecuted), never by DelayDone
 * itself, so every op after a Delay reads the live property: 4 at the Done tick although the
 * Delay was armed when it was 1. The swap is not reached by the shapes the compiler makes. */
TEST(LN_Semantics, TheDelaysTreePropertySwapIsNotReachedByTheOpsAfterADelay)
{
  TreeBuilder b;
  GenNode &event = b.Add("LogicNativeOnFixedUpdate");
  GenNode &seed = b.Add("LogicNativeGetGamePropertyFloat");
  b.String(seed, "Property", "seed");
  GenNode &get = b.Add("LogicNativeGetTreeProperty");
  b.String(get, "Property", "p");
  GenNode &sequence = b.Add("LogicNativeSequence", 0, 0, 2);
  b.Link(event, "Out", sequence, "Flow");
  GenNode &set = b.Add("LogicNativeSetTreeProperty");
  b.String(set, "Property", "p");
  b.Link(sequence, "Then 0", set, "Flow");
  b.Link(seed, "Value", set, "Value");
  GenNode &once = b.Add("LogicNativeOnce");
  b.Link(sequence, "Then 1", once, "Flow");
  GenNode &delay = b.Add("LogicNativeDelay");
  b.Float(delay, "Delay", 0.04f);
  b.Link(once, "Out", delay, "Flow");
  GenNode &direct = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(direct, "Property", "direct");
  b.Link(delay, "Out", direct, "Flow");
  b.Link(get, "Value", direct, "Value");
  GenNode &branch = b.Add("LogicNativeBranch");
  b.Bool(branch, "Condition", true);
  b.Link(delay, "Out", branch, "Flow");
  GenNode &down = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(down, "Property", "down");
  b.Link(branch, "True", down, "Flow");
  b.Link(get, "Value", down, "Value");
  const std::shared_ptr<LN_Program> program = CompileOrFail(b);
  ASSERT_NE(program, nullptr);
  for (const SemMode &mode : AllModes()) {
    SCOPED_TRACE(mode.name);
    SemRig rig(program, mode);
    std::string seen;
    for (int step = 0; step < 6; step++) {
      rig.SetFloat(rig.Object(0), "seed", float(step + 1));
      rig.Step();
      seen += Fmt(" %g/%g", rig.Float(rig.Object(0), "direct", -1.0f), rig.Float(rig.Object(0), "down", -1.0f));
    }
    /* The Delay (0.04 s) is armed in step 0 and done in step 3, where the property holds 4. */
    EXPECT_EQ(seen, " -1/-1 -1/-1 -1/-1 4/4 4/4 4/4");
  }
}

/* ExecuteForcedUpdate (a Run Logic Tree pass) against ExecuteReady (the tick): a tree disabled by
 * its node still runs a forced pass; a suspended owner runs neither; an Every N Steps route that
 * is not due runs in a forced pass. */
TEST(LN_Semantics, AForcedPassRunsWhatTheTickWouldNotAndStopsAtASuspendedOwner)
{
  TreeBuilder b;
  GenNode &event = b.Add("LogicNativeOnFixedUpdate", 4, 0); /* Every 4 steps, not spread out */
  GenNode &ran = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(ran, "Property", "ran");
  b.Float(ran, "Value", 1.0f);
  b.Link(event, "Out", ran, "Flow");
  const std::shared_ptr<LN_Program> program = CompileOrFail(b);
  ASSERT_NE(program, nullptr);
  SemRig rig(program, kProduction);
  const auto forced = [&]() {
    rig.Tick(LN_Event::OnFixedUpdate, [&](LN_CommandBuffer &buffer, const LN_TickContext &context) {
      rig.Force(0, buffer, context);
    });
  };
  KX_GameObject &owner = rig.Object(0);
  rig.Step(); /* step 0: due */
  EXPECT_EQ(rig.Float(owner, "ran"), 1.0f);
  rig.SetFloat(owner, "ran", 0.0f);
  rig.Step(); /* step 1: not due */
  EXPECT_EQ(rig.Float(owner, "ran"), 0.0f);
  forced(); /* step 2: not due, but a forced pass runs the gated route */
  EXPECT_EQ(rig.Float(owner, "ran"), 1.0f);
  rig.SetFloat(owner, "ran", 0.0f);
  rig.Tree(0).SetEnabled(false);
  rig.Step();
  EXPECT_EQ(rig.Float(owner, "ran"), 0.0f) << "a disabled tree does not tick";
  forced();
  EXPECT_EQ(rig.Float(owner, "ran"), 1.0f) << "but runs a forced pass";
  rig.SetFloat(owner, "ran", 0.0f);
  rig.Tree(0).SetEnabled(true);
  owner.SuspendLogic();
  rig.Step();
  forced();
  EXPECT_EQ(rig.Float(owner, "ran"), 0.0f) << "a suspended owner runs neither";
}


/* A tree that starts while the game is paused (a menu added then) runs On Init fully and, from
 * then on, only its UI event routes; a forced pass of a tree whose init has not run runs On Init
 * too. */
TEST(LN_Semantics, OnInitRunsFullyWhileTheGameIsPausedAndOtherRoutesWait)
{
  TreeBuilder b;
  GenNode &init = b.Add("LogicNativeOnInit");
  GenNode &set_init = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(set_init, "Property", "init");
  b.Float(set_init, "Value", 1.0f);
  b.Link(init, "Out", set_init, "Flow");
  GenNode &update = b.Add("LogicNativeOnUpdate");
  GenNode &set_update = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(set_update, "Property", "update");
  b.Float(set_update, "Value", 1.0f);
  b.Link(update, "Out", set_update, "Flow");
  const std::shared_ptr<LN_Program> program = CompileOrFail(b);
  ASSERT_NE(program, nullptr);
  SemRig rig(program, kProduction);
  rig.Tree(0).SetUIEventRoutesOnly(true);
  rig.Update();
  EXPECT_EQ(rig.Float(rig.Object(0), "init"), 1.0f);
  EXPECT_FALSE(rig.Has(rig.Object(0), "update"));
  rig.Tree(0).SetUIEventRoutesOnly(false);
  rig.Update();
  EXPECT_EQ(rig.Float(rig.Object(0), "update"), 1.0f);
}


/* A bad property reference never reaches the runtime: Set Game Property with an empty property name
 * does not compile ("requires a constant property name"), so no op is left that would run without
 * marking itself. */
TEST(LN_Semantics, ASetGamePropertyWithoutAPropertyNameIsRefusedAtCompileTime)
{
  TreeBuilder b;
  GenNode &event = b.Add("LogicNativeOnFixedUpdate");
  GenNode &set = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(set, "Property", "");
  b.Float(set, "Value", 1.0f);
  b.Link(event, "Out", set, "Flow");
  std::string report;
  EXPECT_EQ(b.Compile(&report), nullptr);
  const std::shared_ptr<LN_Program> raw = LN_TreeCompiler().Compile(b.Tree().tree);
  ASSERT_NE(raw, nullptr);
  EXPECT_TRUE(HasErrorContaining(raw->GetCompileReport(), "requires a constant property name"))
      << FormatCompileReport(raw->GetCompileReport());
}

}  // namespace
