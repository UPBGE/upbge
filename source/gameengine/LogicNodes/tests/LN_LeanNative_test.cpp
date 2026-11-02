/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * The native ops of the lean VM (lean runtime plan, phases A18 and A21): the flow core (Nop,
 * BranchRoute), Set Game Property number writes and Apply Movement / Apply Rotation each do what
 * the generic handler does. Every test builds a small tree by hand, runs it in the differential
 * world (diff::RunWorld: 3 owners with edge-case properties, 8 frames) by the old interpreter and
 * by the lean VM in Force mode, and compares the whole text (recorded commands, applied world,
 * every tree's state); the tests that matter for a case also read results from the rig. */

#include "testing/testing.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "LN_LeanProgram.h"
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

blender::Object &NobodyObject()
{
  static blender::Object object;
  static const bool named = (SetCString(object.id.name, "OBNobody"), true);
  (void)named;
  return object;
}

std::shared_ptr<LN_Program> CompileOrFail(TreeBuilder &b)
{
  std::string report;
  std::shared_ptr<LN_Program> program = b.Compile(&report);
  EXPECT_NE(program, nullptr) << report;
  return program;
}

/** The old interpreter and the lean VM (Force) give the same text in the differential world;
 * returns the number of native ops the lean program has. */
uint32_t ExpectSameBothWays(const std::shared_ptr<LN_Program> &program, const char *what)
{
  diff::RunOptions old_way;
  old_way.lean = 0;
  diff::RunOptions lean = old_way;
  lean.lean = 2;
  const uint64_t runs_before = LN_LeanTreeRunCount();
  const diff::RunResult a = diff::RunWorld(program, old_way);
  const diff::RunResult b = diff::RunWorld(program, lean);
  EXPECT_EQ(a.text, b.text) << what << ": " << diff::FirstDifference(a.text, b.text);
  EXPECT_TRUE(b.failure.empty()) << what << ": " << b.failure;
  EXPECT_GT(LN_LeanTreeRunCount(), runs_before) << what << ": no tree ran on the lean VM";
  const std::shared_ptr<const LN_LeanProgram> &lean_program = program->GetLeanProgram();
  return lean_program != nullptr ? lean_program->native_op_count : 0u;
}

}  // namespace

/* Set Game Property Bool, Int and Float write a number: constants, a linked value, and a value
 * that reads another property. The lean program lowers all of them natively. */
TEST(LN_LeanNative, NumberWritesOfEveryKindEndAsTheGenericHandlerEndsThem)
{
  TreeBuilder b;
  GenNode &event = b.Add("LogicNativeOnFixedUpdate");
  GenNode *flow = &event;
  const char *pin = "Out";
  const auto chain = [&](GenNode &node) {
    b.Link(*flow, pin, node, "Flow");
    flow = &node;
    pin = "Done";
  };
  GenNode &set_float = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(set_float, "Property", "out_f");
  b.Float(set_float, "Value", 2.5f);
  chain(set_float);
  GenNode &set_int = b.Add("LogicNativeSetGamePropertyInt");
  b.String(set_int, "Property", "out_i");
  b.Int(set_int, "Value", -7);
  chain(set_int);
  GenNode &set_bool = b.Add("LogicNativeSetGamePropertyBool");
  b.String(set_bool, "Property", "out_b");
  b.Bool(set_bool, "Value", true);
  chain(set_bool);
  GenNode &read = b.Add("LogicNativeGetGamePropertyFloat");
  b.String(read, "Property", "pf0");
  GenNode &add = b.Add("LogicNativeMath", blender::NODE_MATH_ADD);
  b.Link(read, "Value", add, "A");
  b.Float(add, "B", 0.25f);
  GenNode &set_linked = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(set_linked, "Property", "pf1");
  b.Link(add, "Result", set_linked, "Value");
  chain(set_linked);
  const std::shared_ptr<LN_Program> program = CompileOrFail(b);
  ASSERT_NE(program, nullptr);
  ModeScope scope(2);
  EXPECT_GE(ExpectSameBothWays(program, "number writes"), 4u);
  /* The values themselves, read from the rig. */
  SemRig rig(program, kMemoOff);
  rig.SetFloat(rig.Object(0), "pf0", 4.0f);
  rig.Step();
  EXPECT_EQ(rig.Float(rig.Object(0), "out_f"), 2.5f);
  EXPECT_EQ(rig.Float(rig.Object(0), "out_i"), -7.0f);
  EXPECT_EQ(rig.Float(rig.Object(0), "out_b"), 1.0f);
  EXPECT_EQ(rig.Float(rig.Object(0), "pf1"), 4.25f);
}

/* A number write whose Object input names an object that is not there records nothing (and the
 * route goes on): the generic handler marks the instruction without a command. */
TEST(LN_LeanNative, ANumberWriteToAMissingObjectRecordsNothingAndTheRouteGoesOn)
{
  TreeBuilder b;
  GenNode &event = b.Add("LogicNativeOnFixedUpdate");
  GenNode &lost = b.Add("LogicNativeSetGamePropertyFloat");
  b.ObjectNamed(lost, "Object", &NobodyObject());
  b.String(lost, "Property", "lost");
  b.Float(lost, "Value", 3.0f);
  b.Link(event, "Out", lost, "Flow");
  GenNode &after = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(after, "Property", "after");
  b.Float(after, "Value", 6.0f);
  b.Link(lost, "Done", after, "Flow");
  const std::shared_ptr<LN_Program> program = CompileOrFail(b);
  ASSERT_NE(program, nullptr);
  ModeScope scope(2);
  EXPECT_GE(ExpectSameBothWays(program, "missing object"), 2u);
  SemRig rig(program, kMemoOff);
  rig.Step();
  EXPECT_EQ(rig.Float(rig.Object(0), "after"), 6.0f);
  EXPECT_FALSE(rig.Has(rig.Object(0), "lost"));
}

/* A Branch sends the flow down one route: the writes behind each route run only when the
 * condition says so, step after step as the property flips (BranchRoute marks, the guards of the
 * ops behind read the marks). */
TEST(LN_LeanNative, ABranchRunsOnlyTheRouteTheConditionPicks)
{
  TreeBuilder b;
  GenNode &event = b.Add("LogicNativeOnFixedUpdate");
  GenNode &flag = b.Add("LogicNativeGetGamePropertyBool");
  b.String(flag, "Property", "flag");
  GenNode &branch = b.Add("LogicNativeBranch");
  b.Link(event, "Out", branch, "Flow");
  b.Link(flag, "Value", branch, "Condition");
  GenNode &yes = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(yes, "Property", "yes");
  b.Float(yes, "Value", 1.0f);
  b.Link(branch, "True", yes, "Flow");
  GenNode &no = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(no, "Property", "no");
  b.Float(no, "Value", 2.0f);
  b.Link(branch, "False", no, "Flow");
  GenNode &chained = b.Add("LogicNativeSetGamePropertyInt");
  b.String(chained, "Property", "chained");
  b.Int(chained, "Value", 5);
  b.Link(yes, "Done", chained, "Flow");
  const std::shared_ptr<LN_Program> program = CompileOrFail(b);
  ASSERT_NE(program, nullptr);
  ModeScope scope(2);
  EXPECT_GE(ExpectSameBothWays(program, "branch"), 4u);
  SemRig rig(program, kMemoOff);
  KX_GameObject &o = rig.Object(0);
  const struct {
    bool flag;
    bool yes_written, no_written, chained_written;
  } steps[] = {{true, true, false, true}, {false, true, true, true}};
  for (const auto &step : steps) {
    rig.SetBool(o, "flag", step.flag);
    rig.Step();
    EXPECT_EQ(rig.Has(o, "yes"), step.yes_written);
    EXPECT_EQ(rig.Has(o, "no"), step.no_written);
    EXPECT_EQ(rig.Has(o, "chained"), step.chained_written);
  }
  EXPECT_EQ(rig.Float(o, "no"), 2.0f);
  EXPECT_EQ(rig.Float(o, "chained"), 5.0f);
}

/* Apply Movement and Apply Rotation, world and local, with constants and with linked vectors,
 * and one aimed at an object that is not there. */
TEST(LN_LeanNative, MovementAndRotationRecordTheCommandsTheGenericHandlerRecords)
{
  TreeBuilder b;
  GenNode &event = b.Add("LogicNativeOnFixedUpdate");
  GenNode *flow = &event;
  const char *pin = "Out";
  const auto chain = [&](GenNode &node) {
    b.Link(*flow, pin, node, "Flow");
    flow = &node;
    pin = "Done";
  };
  GenNode &walk_local = b.Add("LogicNativeApplyMovement");
  b.Vector(walk_local, "Vector", 0.0f, 0.02f, 0.0f);
  b.Bool(walk_local, "Local", true);
  chain(walk_local);
  GenNode &walk_world = b.Add("LogicNativeApplyMovement");
  b.Vector(walk_world, "Vector", 0.01f, 0.0f, -0.005f);
  b.Bool(walk_world, "Local", false);
  chain(walk_world);
  GenNode &turn_local = b.Add("LogicNativeApplyRotation");
  b.Rotation(turn_local, "Rotation", 0.1f, 0.0f, 0.2f);
  b.Bool(turn_local, "Local", true);
  chain(turn_local);
  GenNode &turn_world = b.Add("LogicNativeApplyRotation");
  b.Rotation(turn_world, "Rotation", 0.0f, -0.3f, 0.05f);
  b.Bool(turn_world, "Local", false);
  chain(turn_world);
  /* A rotation from a linked vector (the constant is not used). */
  GenNode &spin = b.Add("LogicNativeCombineXYZ");
  b.Float(spin, "X", 0.01f);
  b.Float(spin, "Y", 0.02f);
  b.Float(spin, "Z", 0.03f);
  GenNode &aim = b.Add("LogicNativeVectorToRotation");
  b.Link(spin, "Vector", aim, "Direction");
  GenNode &turn_linked = b.Add("LogicNativeApplyRotation");
  b.Link(aim, "Rotation", turn_linked, "Rotation");
  b.Bool(turn_linked, "Local", true);
  chain(turn_linked);
  GenNode &walk_lost = b.Add("LogicNativeApplyMovement");
  b.ObjectNamed(walk_lost, "Object", &NobodyObject());
  b.Vector(walk_lost, "Vector", 5.0f, 5.0f, 5.0f);
  chain(walk_lost);
  GenNode &after = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(after, "Property", "after");
  b.Float(after, "Value", 1.0f);
  chain(after);
  const std::shared_ptr<LN_Program> program = CompileOrFail(b);
  ASSERT_NE(program, nullptr);
  ModeScope scope(2);
  EXPECT_GE(ExpectSameBothWays(program, "movement and rotation"), 7u);
  SemRig rig(program, kMemoOff);
  rig.Step();
  EXPECT_EQ(rig.Float(rig.Object(0), "after"), 1.0f) << "the route goes on past a missing object";
}

/* The rotation of a constant Euler vector is worked out once when the program is lowered: it is
 * the very rotation the generic handler works out at every run. */
TEST(LN_LeanNative, TheLoweredRotationConstantIsTheRotationOfTheConstantVector)
{
  TreeBuilder b;
  GenNode &event = b.Add("LogicNativeOnFixedUpdate");
  GenNode &turn = b.Add("LogicNativeApplyRotation");
  b.Rotation(turn, "Rotation", 0.3f, -0.2f, 0.7f);
  b.Link(event, "Out", turn, "Flow");
  const std::shared_ptr<LN_Program> program = CompileOrFail(b);
  ASSERT_NE(program, nullptr);
  const std::shared_ptr<const LN_LeanProgram> lean = LN_LowerToLean(*program);
  ASSERT_NE(lean, nullptr);
  ASSERT_EQ(lean->rotation_constants.size(), 1u);
  const LN_Rotation expected = LN_Rotation::FromEulerXYZ(MT_Vector3(0.3f, -0.2f, 0.7f));
  const LN_Rotation &made = lean->rotation_constants[0];
  /* Compared by what both give as a matrix, bit for bit. */
  const MT_Matrix3x3 a = expected.ToMatrix();
  const MT_Matrix3x3 b_matrix = made.ToMatrix();
  for (int row = 0; row < 3; row++) {
    for (int column = 0; column < 3; column++) {
      EXPECT_EQ(a[row][column], b_matrix[row][column]);
    }
  }
}

/* A Get Distance read again at each step starts from a clean result: when an end stops giving a
 * finite position (a NaN), the distance and its Valid output go back to 0 and false, as when the
 * result is built anew (the lean VM clears only the three fields Get Distance sets). */
TEST(LN_LeanNative, AGetDistanceThatBecomesInvalidReadsInvalidAgain)
{
  TreeBuilder b;
  GenNode &event = b.Add("LogicNativeOnFixedUpdate");
  GenNode &tx = b.Add("LogicNativeGetGamePropertyFloat");
  b.String(tx, "Property", "tx");
  GenNode &end = b.Add("LogicNativeCombineXYZ");
  b.Link(tx, "Value", end, "X");
  GenNode &distance = b.Add("LogicNativeGetDistance", 0, 1); /* A: an object (the owner), B: a position */
  b.Link(end, "Vector", distance, "Position B");
  GenNode &store = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(store, "Property", "d");
  b.Link(distance, "Distance", store, "Value");
  b.Link(event, "Out", store, "Flow");
  GenNode &store_valid = b.Add("LogicNativeSetGamePropertyBool");
  b.String(store_valid, "Property", "v");
  b.Link(distance, "Valid", store_valid, "Value");
  b.Link(store, "Done", store_valid, "Flow");
  const std::shared_ptr<LN_Program> program = CompileOrFail(b);
  ASSERT_NE(program, nullptr);
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float xs[] = {3.0f, nan, 5.0f, nan, nan, 2.0f};
  std::vector<std::pair<float, float>> results[2];
  for (const int mode : {0, 2}) {
    ModeScope scope(mode);
    SemRig rig(program, kMemoOff);
    for (const float x : xs) {
      rig.SetFloat(rig.Object(0), "tx", x);
      rig.Step();
      results[mode / 2].push_back({rig.Float(rig.Object(0), "d"), rig.Float(rig.Object(0), "v")});
    }
  }
  ASSERT_EQ(results[0].size(), results[1].size());
  for (size_t step = 0; step < results[0].size(); step++) {
    EXPECT_EQ(results[0][step], results[1][step]) << "step " << step;
    const bool finite = std::isfinite(xs[step]);
    EXPECT_EQ(results[1][step].second, finite ? 1.0f : 0.0f) << "step " << step << " (Valid)";
    EXPECT_EQ(results[1][step].first, finite ? std::fabs(xs[step]) : 0.0f)
        << "step " << step << " (Distance)";
  }
}

/* Auto mode runs the lean VM only for an event whose every op has a native handler; an event with
 * an op that has none (a text write here) stays on the old interpreter, and both give their
 * results. Force runs the mixed event lean too. */
TEST(LN_LeanNative, AutoRunsTheLeanVMOnlyForAnEventWhoseOpsAreAllNative)
{
  const auto build = [](const bool with_text) {
    TreeBuilder b;
    GenNode &event = b.Add("LogicNativeOnFixedUpdate");
    GenNode &set = b.Add("LogicNativeSetGamePropertyFloat");
    b.String(set, "Property", "n");
    b.Float(set, "Value", 4.0f);
    b.Link(event, "Out", set, "Flow");
    if (with_text) {
      GenNode &text = b.Add("LogicNativeSetGamePropertyString");
      b.String(text, "Property", "t");
      b.String(text, "Value", "hello");
      b.Link(set, "Done", text, "Flow");
    }
    return CompileOrFail(b);
  };
  const std::shared_ptr<LN_Program> native_only = build(false);
  const std::shared_ptr<LN_Program> mixed = build(true);
  ASSERT_NE(native_only, nullptr);
  ASSERT_NE(mixed, nullptr);
  struct Case {
    int mode;
    const std::shared_ptr<LN_Program> &program;
    bool runs_lean;
  };
  for (const Case &c : {Case{1, native_only, true},
                        Case{1, mixed, false},
                        Case{2, native_only, true},
                        Case{2, mixed, true},
                        Case{0, native_only, false}})
  {
    ModeScope scope(c.mode);
    SemRig rig(c.program, kMemoOff);
    rig.Step(); /* the first step also runs On Init, an event with no op at all (always lean) */
    const uint64_t runs_before = LN_LeanTreeRunCount();
    rig.Step();
    const bool ran_lean = LN_LeanTreeRunCount() > runs_before;
    EXPECT_EQ(ran_lean, c.runs_lean) << "mode " << c.mode << (c.program == mixed ? " mixed" : "");
    EXPECT_EQ(rig.Float(rig.Object(0), "n"), 4.0f);
    if (c.program == mixed) {
      EXPECT_EQ(rig.Text(rig.Object(0), "t"), "hello");
    }
  }
}

/* A number write counts as one of the last-write-wins writes the flush's merge gate counts, built
 * in place in its stream like every other: three number writes, three counted, by either way. */
TEST(LN_LeanNative, NumberWritesCountForTheFlushMergeGate)
{
  TreeBuilder b;
  GenNode &event = b.Add("LogicNativeOnFixedUpdate");
  GenNode *flow = &event;
  const char *pin = "Out";
  for (const char *kind : {"LogicNativeSetGamePropertyFloat",
                           "LogicNativeSetGamePropertyInt",
                           "LogicNativeSetGamePropertyBool"})
  {
    GenNode &set = b.Add(kind);
    b.String(set, "Property", (std::string("w_") + kind).c_str());
    b.Link(*flow, pin, set, "Flow");
    flow = &set;
    pin = "Done";
  }
  const std::shared_ptr<LN_Program> program = CompileOrFail(b);
  ASSERT_NE(program, nullptr);
  for (const int mode : {0, 2}) {
    ModeScope scope(mode);
    SemRig rig(program, kMemoOff);
    rig.SetFlush(false);
    rig.Step();
    const std::pair<bool, uint32_t> decision = rig.Buffer().FlushDecisionForTests();
    EXPECT_TRUE(decision.first) << "mode " << mode;
    EXPECT_EQ(decision.second, 3u) << "mode " << mode;
  }
}

/* Every setting of Set Object Attribute (positions, orientations, scales, velocities, transform and
 * velocity vectors) and Apply Force / Torque, with constants and with vectors read from a property,
 * ends as the generic handler ends it. (The differential tester's family table has none of these
 * nodes, so this sweep is their coverage.) Settings that do not compile alone are skipped; the count
 * of those that do and of the native ops they make is checked, so the sweep cannot go empty. */
TEST(LN_LeanNative, EveryVectorCommandSettingEndsAsTheGenericHandlerEndsIt)
{
  uint32_t compiled = 0, native_ops = 0;
  const auto run = [&](const char *idname,
                       const int16_t custom1,
                       const int16_t custom2,
                       const bool linked,
                       const std::vector<const char *> &vector_pins,
                       const char *rotation_pin,
                       const bool local) {
    TreeBuilder b;
    GenNode &event = b.Add("LogicNativeOnFixedUpdate");
    GenNode &node = b.Add(idname, custom1, custom2);
    b.Link(event, "Out", node, "Flow");
    if (linked) {
      GenNode &x = b.Add("LogicNativeGetGamePropertyFloat");
      b.String(x, "Property", "pf0");
      GenNode &vector = b.Add("LogicNativeCombineXYZ");
      b.Link(x, "Value", vector, "X");
      b.Float(vector, "Y", -0.4f);
      b.Float(vector, "Z", 0.9f);
      for (const char *pin : vector_pins) {
        b.Link(vector, "Vector", node, pin);
      }
      if (rotation_pin != nullptr) {
        GenNode &aim = b.Add("LogicNativeVectorToRotation");
        b.Link(vector, "Vector", aim, "Direction");
        b.Link(aim, "Rotation", node, rotation_pin);
      }
    }
    else {
      float step = 0.0f;
      for (const char *pin : vector_pins) {
        b.Vector(node, pin, 0.3f + step, -0.2f, 0.5f - step);
        step += 0.11f;
      }
      if (rotation_pin != nullptr) {
        b.Rotation(node, rotation_pin, 0.2f, 0.1f, -0.4f);
      }
    }
    if (local) {
      b.Bool(node, "Local", true);
    }
    std::string report;
    const std::shared_ptr<LN_Program> program = b.Compile(&report);
    if (program == nullptr) {
      return;
    }
    compiled++;
    native_ops += ExpectSameBothWays(program, idname);
  };
  for (int16_t custom1 = 0; custom1 < 40; custom1++) {
    for (int16_t custom2 = 0; custom2 < 2; custom2++) {
      for (const bool linked : {false, true}) {
        run("LogicNativeSetObjectAttribute",
            custom1,
            custom2,
            linked,
            {"Value", "Position", "Scale"},
            "Rotation",
            false);
      }
    }
  }
  for (const char *idname : {"LogicNativeApplyForce", "LogicNativeApplyTorque"}) {
    const char *pin = std::strcmp(idname, "LogicNativeApplyForce") == 0 ? "Force" : "Torque";
    for (const bool linked : {false, true}) {
      for (const bool local : {false, true}) {
        run(idname, 0, 0, linked, {pin}, nullptr, local);
      }
    }
  }
  std::printf("LEAN_NATIVE vector sweep: %u programs compiled, %u native ops\n", compiled, native_ops);
  EXPECT_GE(compiled, 12u) << "settings that compile alone";
  EXPECT_GE(native_ops, 12u) << "native ops among them";
}

/* Modify Property (every operation, with and without the clamp, on the owner and on a missing
 * object, the operand constant and read from a property) ends as the generic handler ends it, step
 * after step (the change applies to the live value at the flush). */
TEST(LN_LeanNative, ModifyPropertyEndsAsTheGenericHandlerEndsIt)
{
  uint32_t compiled = 0, native_ops = 0;
  for (const int16_t op_kind : {int16_t(blender::NODE_MATH_ADD),
                                int16_t(blender::NODE_MATH_SUBTRACT),
                                int16_t(blender::NODE_MATH_MULTIPLY),
                                int16_t(blender::NODE_MATH_DIVIDE),
                                int16_t(blender::NODE_MATH_POWER),
                                int16_t(blender::NODE_MATH_MODULO),
                                int16_t(blender::NODE_MATH_FLOOR)})
  {
    for (const bool clamped : {false, true}) {
      for (const bool linked : {false, true}) {
        TreeBuilder b;
        GenNode &event = b.Add("LogicNativeOnFixedUpdate");
        GenNode &modify = b.Add("LogicNativeModifyProperty", op_kind, clamped ? 1 : 0);
        b.String(modify, "Property", "pf1");
        b.Link(event, "Out", modify, "Flow");
        if (linked) {
          GenNode &read = b.Add("LogicNativeGetGamePropertyFloat");
          b.String(read, "Property", "pf0");
          b.Link(read, "Value", modify, "Value");
        }
        else {
          b.Float(modify, "Value", 1.75f);
        }
        if (clamped) {
          b.Float(modify, "Min", -2.0f);
          b.Float(modify, "Max", 6.0f);
        }
        std::string report;
        const std::shared_ptr<LN_Program> program = b.Compile(&report);
        if (program == nullptr) {
          continue;
        }
        compiled++;
        native_ops += ExpectSameBothWays(program, "modify property");
      }
    }
  }
  std::printf("LEAN_NATIVE modify sweep: %u programs compiled, %u native ops\n", compiled, native_ops);
  EXPECT_EQ(compiled, 28u);
  EXPECT_GE(native_ops, 28u);
}

/* Mode Off runs no native op: no lean tree run is counted, and the write is the old
 * interpreter's. */
TEST(LN_LeanNative, ModeOffNeverRunsANativeOp)
{
  TreeBuilder b;
  GenNode &event = b.Add("LogicNativeOnFixedUpdate");
  GenNode &set = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(set, "Property", "p");
  b.Float(set, "Value", 3.0f);
  b.Link(event, "Out", set, "Flow");
  const std::shared_ptr<LN_Program> program = CompileOrFail(b);
  ASSERT_NE(program, nullptr);
  ModeScope scope(0);
  SemRig rig(program, kMemoOff);
  const uint64_t runs_before = LN_LeanTreeRunCount();
  rig.Step();
  EXPECT_EQ(LN_LeanTreeRunCount(), runs_before);
  EXPECT_EQ(rig.Float(rig.Object(0), "p"), 3.0f);
}
