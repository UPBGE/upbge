/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Tween Value's Forward and Back as flow inputs, and a Branch read as a value (2026-11-17).
 *
 * Forward and Back were read as values when the tween updated: a Branch counted as always on (its
 * Condition ignored) and a route node's pulse (Sequence, Flip-Flop, Once...) was never seen (its
 * route runs after the step's own ops). Now each route reaching them runs an Op::TweenRequest
 * and the tween takes the latest request at its next update; a sensor's or Boolean Edge's pulse
 * is requested where the tween compiles, so it starts the tween in the step it fires, as before.
 * A Branch's output read as a value (a query's or Store Value's Flow, a Loop's start) is now the
 * pulse of its route, as every other flow node's output.
 *
 * Each tree runs tree by tree (two rig modes) and wide (columns, 1 and 3 helpers), 4 owners, and
 * every way must give the same timeline.
 */

#include "testing/testing.h"

#include <cmath>
#include <string>
#include <vector>

#include "LN_Columns.h"
#include "LN_RuntimeTree.h"
#include "LN_SemRig.h"

using namespace ln_test;
using namespace ln_test::sem;

namespace {

/** Linear easing (custom2 bits 2-5), and Repeat (bits 8-9). */
constexpr int16_t kLinear = int16_t(1 << 2);
constexpr int16_t kPingPong = int16_t(kLinear | (2 << 8));

/** A Float Tween from 0 to 60 over 1 s (one per 1/60 s step), its Result written to "t" every
 * step (a Set Game Property on On Logic + Physics Step: the step's own path). */
GenNode &AddTween(TreeBuilder &b, GenNode &step, const int16_t custom2 = kLinear)
{
  GenNode &tween = b.Add("LogicNativeTweenValue", 0, custom2);
  b.Float(tween, "FromFloat", 0.0f);
  b.Float(tween, "ToFloat", 60.0f);
  b.Float(tween, "Duration", 1.0f);
  GenNode &write = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(write, "Property", "t");
  b.Link(tween, "ResultFloat", write, "Value");
  b.Link(step, "Out", write, "Flow");
  return tween;
}

GenNode &AddBoolRead(TreeBuilder &b, const char *property)
{
  GenNode &read = b.Add("LogicNativeGetGamePropertyBool");
  b.String(read, "Property", property);
  return read;
}

/** What to switch before each step: the step number -> the properties "go" / "back" / "dir". */
struct Script {
  std::vector<int> go_steps;
  std::vector<int> back_steps;
  std::vector<std::pair<int, int>> dir;
  /** Steps a key ("A") is pressed (only that step: Just Pressed). */
  std::vector<int> key_steps;
};

bool Has(const std::vector<int> &steps, const int step)
{
  return std::find(steps.begin(), steps.end(), step) != steps.end();
}

/** "t" of owner 0 after each of `steps` steps (rounded to a thousandth), every owner checked
 * to say the same; the timeline of each way the tree may run, which must all agree. */
std::string RunTimeline(const std::shared_ptr<LN_Program> &program,
                        const Script &script,
                        const int steps,
                        const bool columns,
                        const uint32_t helpers,
                        const SemMode &mode,
                        size_t *r_wide_lanes = nullptr)
{
  constexpr size_t kOwners = 4;
  SemRig rig(program, mode, kOwners);
  if (columns) {
    rig.SetColumns(true, helpers);
  }
  LN_InputSnapshot input;
  rig.SetInput(&input);
  for (size_t owner = 0; owner < kOwners; owner++) {
    KX_GameObject &object = rig.Object(owner);
    rig.SetFloat(object, "t", -1.0f);
    rig.SetFloat(object, "x", 0.0f);
    rig.SetFloat(object, "n", 0.0f);
    rig.SetFloat(object, "v", 0.0f);
    rig.SetFloat(object, "s", -1.0f);
    rig.SetBool(object, "go", false);
    rig.SetBool(object, "back", false);
    rig.SetInt(object, "dir", 0);
  }
  std::string timeline;
  size_t wide = 0;
  int dir = 0;
  for (int step = 1; step <= steps; step++) {
    for (const auto &[at, value] : script.dir) {
      if (at == step) {
        dir = value;
      }
    }
    for (size_t owner = 0; owner < kOwners; owner++) {
      KX_GameObject &object = rig.Object(owner);
      rig.SetBool(object, "go", Has(script.go_steps, step));
      rig.SetBool(object, "back", Has(script.back_steps, step));
      rig.SetInt(object, "dir", dir);
      rig.SetFloat(object, "v", float(step));
    }
    const bool key = Has(script.key_steps, step);
    input.SetKeyForTests(SCA_IInputDevice::AKEY, key, key, false);
    rig.Step();
    wide += rig.LastColumnLanes();
    const float t = rig.Float(rig.Object(0), "t");
    for (size_t owner = 1; owner < kOwners; owner++) {
      EXPECT_EQ(rig.Float(rig.Object(owner), "t"), t) << "owner " << owner << " step " << step;
    }
    char text[32];
    std::snprintf(text, sizeof(text), "%.3f ", double(t));
    timeline += text;
  }
  if (r_wide_lanes != nullptr) {
    *r_wide_lanes = wide;
  }
  return timeline;
}

/** The timeline "t" must follow: `moves[k]` is t after step k + 1. */
std::string Expected(const std::vector<float> &values)
{
  std::string timeline;
  for (const float value : values) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.3f ", double(value));
    timeline += text;
  }
  return timeline;
}

/** Runs `program` every way and checks each against `expected`; `wide_expected`: the wide runs
 * must have taken lanes (the request runs as a column node there). */
void ExpectTimeline(const std::shared_ptr<LN_Program> &program,
                    const Script &script,
                    const std::vector<float> &expected,
                    const bool wide_expected = true)
{
  ASSERT_NE(program, nullptr);
  const std::string want = Expected(expected);
  for (const SemMode &mode : {kProduction, kMemoOff}) {
    SCOPED_TRACE(std::string(mode.name) + ", tree by tree");
    EXPECT_EQ(RunTimeline(program, script, int(expected.size()), false, 0, mode), want);
  }
  for (const uint32_t helpers : {0u, 3u}) {
    SCOPED_TRACE("wide, " + std::to_string(helpers) + " helpers");
    size_t lanes = 0;
    EXPECT_EQ(RunTimeline(program, script, int(expected.size()), true, helpers, kProduction, &lanes),
              want);
    /* (UPBGE_LN_COLUMN_DIRECT=force runs every tree as a column pass: the timelines are still
     * compared, no lane forms.) */
    if (wide_expected && LN_GetColumnDirectMode() != LN_ColumnDirectMode::Force) {
      EXPECT_GT(lanes, 0u) << "no wide run";
    }
  }
}

std::shared_ptr<LN_Program> Compile(TreeBuilder &b)
{
  std::string report;
  std::shared_ptr<LN_Program> program = b.Compile(&report);
  EXPECT_NE(program, nullptr) << report;
  return program;
}

/** t after each step of a tween started (moving its first step) at step `start`, over `steps`. */
std::vector<float> StartingAt(const int start, const int steps)
{
  std::vector<float> values;
  for (int step = 1; step <= steps; step++) {
    values.push_back(step < start ? 0.0f : float(std::min(step - start + 1, 60)));
  }
  return values;
}

}  // namespace

/* -------------------------------------------------------------------- */
/** \name The sources that worked as values start the tween in the same step as before
 * \{ */

/** On Logic + Physics Step, a key and a Boolean Edge into Forward: the tween moves in the step
 * the pulse fires (as when Forward was read as a value). */
TEST(LN_TweenFlow, StepsKeysAndBooleanEdgesStartItInTheStepTheyFire)
{
  {
    SCOPED_TRACE("On Logic + Physics Step");
    TreeBuilder b;
    GenNode &step = b.Add("LogicNativeOnFixedUpdate");
    GenNode &tween = AddTween(b, step);
    b.Link(step, "Out", tween, "Forward");
    ExpectTimeline(Compile(b), {}, StartingAt(1, 8));
  }
  {
    SCOPED_TRACE("Keyboard Key");
    TreeBuilder b;
    GenNode &step = b.Add("LogicNativeOnFixedUpdate");
    GenNode &tween = AddTween(b, step);
    GenNode &key = b.Add("LogicNativeKeyboardKey");
    b.String(key, "Key", "A");
    b.Link(key, "Out", tween, "Forward");
    Script script;
    script.key_steps = {3};
    ExpectTimeline(Compile(b), script, StartingAt(3, 8));
  }
  {
    SCOPED_TRACE("Boolean Edge");
    TreeBuilder b;
    GenNode &step = b.Add("LogicNativeOnFixedUpdate");
    GenNode &tween = AddTween(b, step);
    GenNode &edge = b.Add("LogicNativeBooleanEdge");
    b.Link(AddBoolRead(b, "go"), "Value", edge, "Condition");
    b.Link(edge, "Rising", tween, "Forward");
    Script script;
    script.go_steps = {4, 5, 6};
    ExpectTimeline(Compile(b), script, StartingAt(4, 9));
  }
}

/** On Update (frames) into Forward with the tween read on steps: each frame asks, the step's
 * update takes it (a frame then a step: the tween moves every step, as before). */
TEST(LN_TweenFlow, AFrameRequestIsTakenByTheNextStep)
{
  TreeBuilder b;
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode &frame = b.Add("LogicNativeOnUpdate");
  GenNode &tween = AddTween(b, step);
  b.Link(frame, "Out", tween, "Forward");
  const std::shared_ptr<LN_Program> program = Compile(b);
  ASSERT_NE(program, nullptr);
  for (const bool columns : {false, true}) {
    SCOPED_TRACE(columns ? "wide" : "tree by tree");
    SemRig rig(program, kProduction, 4);
    if (columns) {
      rig.SetColumns(true, 0);
    }
    rig.SetFloat(rig.Object(0), "t", -1.0f);
    for (int frame_index = 1; frame_index <= 5; frame_index++) {
      rig.Frame();
      EXPECT_NEAR(rig.Float(rig.Object(0), "t"), float(frame_index), 1.0e-3f) << frame_index;
    }
    /* Steps without frames: the request was taken, the tween plays on by itself. */
    rig.Step();
    rig.Step();
    EXPECT_NEAR(rig.Float(rig.Object(0), "t"), 7.0f, 1.0e-3f);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Every flow node into Forward / Back
 * \{ */

/** A route node's pulse: its route runs after the step's own ops (the write of "t"), so the
 * tween, read before it, moves from the next step. Before, a Branch counted as always on (the
 * tween ran from the first step whatever its Condition) and Sequence, Once, Flip-Flop never
 * started it. */
TEST(LN_TweenFlow, EveryRouteNodeStartsItAfterItsPulse)
{
  {
    SCOPED_TRACE("Branch True");
    TreeBuilder b;
    GenNode &step = b.Add("LogicNativeOnFixedUpdate");
    GenNode &tween = AddTween(b, step);
    GenNode &branch = b.Add("LogicNativeBranch");
    b.Link(AddBoolRead(b, "go"), "Value", branch, "Condition");
    b.Link(step, "Out", branch, "Flow");
    b.Link(branch, "True", tween, "Forward");
    Script script;
    script.go_steps = {3};
    /* Not before the Condition holds (it ran from step 1 before). */
    ExpectTimeline(Compile(b), script, StartingAt(4, 8));
  }
  {
    SCOPED_TRACE("Branch True into Forward, False into Back");
    /* A door: open while "go", close while not. Each step's request is taken by the next
     * step's write of "t": "go" on 3-6 opens from 4 to 7, closing from 8. */
    TreeBuilder b;
    GenNode &step = b.Add("LogicNativeOnFixedUpdate");
    GenNode &tween = AddTween(b, step);
    GenNode &branch = b.Add("LogicNativeBranch");
    b.Link(AddBoolRead(b, "go"), "Value", branch, "Condition");
    b.Link(step, "Out", branch, "Flow");
    b.Link(branch, "True", tween, "Forward");
    b.Link(branch, "False", tween, "Back");
    Script script;
    script.go_steps = {3, 4, 5, 6};
    ExpectTimeline(Compile(b), script, {0, 0, 0, 1, 2, 3, 4, 3, 2, 1, 0, 0});
  }
  {
    SCOPED_TRACE("Sequence");
    TreeBuilder b;
    GenNode &step = b.Add("LogicNativeOnFixedUpdate");
    GenNode &tween = AddTween(b, step);
    GenNode &sequence = b.Add("LogicNativeSequence", 0, 0, 2);
    b.Link(step, "Out", sequence, "Flow");
    b.Link(sequence, "Then 1", tween, "Forward");
    ExpectTimeline(Compile(b), {}, StartingAt(2, 6));
  }
  {
    SCOPED_TRACE("Once");
    TreeBuilder b;
    GenNode &step = b.Add("LogicNativeOnFixedUpdate");
    GenNode &tween = AddTween(b, step);
    GenNode &once = b.Add("LogicNativeOnce");
    b.Link(step, "Out", once, "Flow");
    b.Link(once, "Out", tween, "Forward");
    ExpectTimeline(Compile(b), {}, StartingAt(2, 6));
  }
  {
    SCOPED_TRACE("A node's Done");
    TreeBuilder b;
    GenNode &step = b.Add("LogicNativeOnFixedUpdate");
    GenNode &tween = AddTween(b, step);
    GenNode &write = b.Add("LogicNativeSetGamePropertyFloat");
    b.String(write, "Property", "x");
    b.Float(write, "Value", 1.0f);
    b.Link(step, "Out", write, "Flow");
    b.Link(write, "Done", tween, "Forward");
    ExpectTimeline(Compile(b), {}, StartingAt(2, 6));
  }
  {
    SCOPED_TRACE("Integer Switch Flow");
    TreeBuilder b;
    GenNode &step = b.Add("LogicNativeOnFixedUpdate");
    GenNode &tween = AddTween(b, step);
    GenNode &dir = b.Add("LogicNativeGetGamePropertyInt");
    b.String(dir, "Property", "dir");
    GenNode &choose = b.Add("LogicNativeSwitchFlowInt", 0, 0, 3);
    b.Link(step, "Out", choose, "Flow");
    b.Link(dir, "Value", choose, "Index");
    b.Link(choose, "Then 1", tween, "Forward");
    b.Link(choose, "Then 2", tween, "Back");
    /* 0 on steps 1-2 (neither), 1 from step 3 (forward from 4), 2 from step 6 (back from 7). */
    Script script;
    script.dir = {{3, 1}, {6, 2}};
    ExpectTimeline(Compile(b), script, {0, 0, 0, 1, 2, 3, 2, 1, 0, 0});
  }
  {
    SCOPED_TRACE("Any Flow");
    TreeBuilder b;
    GenNode &step = b.Add("LogicNativeOnFixedUpdate");
    GenNode &tween = AddTween(b, step);
    GenNode &branch = b.Add("LogicNativeBranch");
    b.Link(AddBoolRead(b, "go"), "Value", branch, "Condition");
    b.Link(step, "Out", branch, "Flow");
    GenNode &any = b.Add("LogicNativeAnyFlow");
    b.Link(branch, "True", any, "In");
    b.Link(any, "Out", tween, "Forward");
    Script script;
    script.go_steps = {2};
    ExpectTimeline(Compile(b), script, StartingAt(3, 6));
  }
  {
    SCOPED_TRACE("Delay's Done");
    TreeBuilder b;
    GenNode &step = b.Add("LogicNativeOnFixedUpdate");
    GenNode &tween = AddTween(b, step);
    GenNode &branch = b.Add("LogicNativeBranch");
    b.Link(AddBoolRead(b, "go"), "Value", branch, "Condition");
    b.Link(step, "Out", branch, "Flow");
    GenNode &delay = b.Add("LogicNativeDelay");
    b.Float(delay, "Delay", 3.0f / 60.0f);
    b.Link(branch, "True", delay, "Flow");
    b.Link(delay, "Out", tween, "Forward");
    Script script;
    script.go_steps = {2};
    /* Armed on step 2, Done 3 steps later (step 5, its route after the write): moves from 6. */
    const std::shared_ptr<LN_Program> program = Compile(b);
    ASSERT_NE(program, nullptr);
    const std::string reference = RunTimeline(program, script, 10, false, 0, kProduction);
    EXPECT_EQ(reference, Expected(StartingAt(6, 10)));
    ExpectTimeline(program, script, StartingAt(6, 10), false);
  }
}

/** Flip-Flop's A into Forward and B into Back, a Boolean Edge into its Flow: each press turns
 * the tween round (a door opened and closed by one key; it never started before). */
TEST(LN_TweenFlow, AFlipFlopTogglesItsWay)
{
  TreeBuilder b;
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode &tween = AddTween(b, step);
  GenNode &edge = b.Add("LogicNativeBooleanEdge");
  b.Link(AddBoolRead(b, "go"), "Value", edge, "Condition");
  GenNode &flip = b.Add("LogicNativeFlipFlop");
  b.Link(edge, "Rising", flip, "Flow");
  b.Link(flip, "A", tween, "Forward");
  b.Link(flip, "B", tween, "Back");
  Script script;
  script.go_steps = {2, 7};
  /* Press on 2: A, forward from 3; press on 7: B, back from 8. */
  ExpectTimeline(Compile(b), script, {0, 0, 1, 2, 3, 4, 5, 4, 3, 2, 1, 0}, false);
}

/** Forward and Back in one step: the latest request wins (Sequence: Then 0 before Then 1). */
TEST(LN_TweenFlow, TheLatestRequestOfAStepWins)
{
  for (const bool forward_last : {false, true}) {
    SCOPED_TRACE(forward_last ? "Back, then Forward" : "Forward, then Back");
    TreeBuilder b;
    GenNode &step = b.Add("LogicNativeOnFixedUpdate");
    GenNode &tween = AddTween(b, step);
    GenNode &sequence = b.Add("LogicNativeSequence", 0, 0, 2);
    b.Link(step, "Out", sequence, "Flow");
    b.Link(sequence, "Then 0", tween, forward_last ? "Back" : "Forward");
    b.Link(sequence, "Then 1", tween, forward_last ? "Forward" : "Back");
    ExpectTimeline(Compile(b), {}, forward_last ? StartingAt(2, 6) : StartingAt(100, 6));
  }
}

/** Ping-Pong: one pulse starts it and it goes round by itself; a Back pulse while it goes
 * forward turns it round at once. */
TEST(LN_TweenFlow, OnePulseStartsAPingPongAndBackTurnsIt)
{
  TreeBuilder b;
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode &tween = AddTween(b, step, kPingPong);
  GenNode &forward = b.Add("LogicNativeBooleanEdge");
  b.Link(AddBoolRead(b, "go"), "Value", forward, "Condition");
  b.Link(forward, "Rising", tween, "Forward");
  GenNode &back = b.Add("LogicNativeBooleanEdge");
  b.Link(AddBoolRead(b, "back"), "Value", back, "Condition");
  b.Link(back, "Rising", tween, "Back");
  Script script;
  script.go_steps = {1};
  script.back_steps = {6};
  /* Forward from step 1 (1..5), Back on 6 (sensor: in that step): 4, 3, ... 0, then it turns at
   * the start and goes forward again. */
  ExpectTimeline(Compile(b), script, {1, 2, 3, 4, 5, 4, 3, 2, 1, 0, 1, 2});
}

/** Apply To moves the object in the step a key starts the tween (the requests come before Apply
 * To's route reads the tween), wide and tree by tree. */
TEST(LN_TweenFlow, ApplyToMovesTheObjectInTheStepTheKeyStartsIt)
{
  TreeBuilder b;
  b.Add("LogicNativeOnFixedUpdate");
  /* Vector, Linear, Apply To World Position (custom2 bits 10-12: 1). */
  GenNode &tween = b.Add("LogicNativeTweenValue", 1, int16_t(kLinear | (1 << 10)));
  b.Vector(tween, "FromVector", 0.0f, 0.0f, 0.0f);
  b.Vector(tween, "ToVector", 60.0f, 0.0f, 0.0f);
  b.Float(tween, "Duration", 1.0f);
  GenNode &key = b.Add("LogicNativeKeyboardKey");
  b.String(key, "Key", "A");
  b.Link(key, "Out", tween, "Forward");
  const std::shared_ptr<LN_Program> program = Compile(b);
  ASSERT_NE(program, nullptr);
  for (const bool columns : {false, true}) {
    SCOPED_TRACE(columns ? "wide" : "tree by tree");
    SemRig rig(program, kProduction, 3);
    if (columns) {
      rig.SetColumns(true, 0);
    }
    LN_InputSnapshot input;
    rig.SetInput(&input);
    for (int at = 1; at <= 4; at++) {
      const bool pressed = at == 2;
      input.SetKeyForTests(SCA_IInputDevice::AKEY, pressed, pressed, false);
      rig.Step();
      for (size_t owner = 0; owner < 3; owner++) {
        EXPECT_NEAR(rig.Object(owner).NodeGetWorldPosition().x(), at < 2 ? 0.0f : float(at - 1),
                    1.0e-3f)
            << "step " << at << " owner " << owner;
      }
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name A Branch's output read as a value is its route's pulse
 * \{ */

/** A Loop behind a Branch starts only on the steps the Branch takes that way (its start counted
 * the Branch as always on: the Loop ran every step). */
TEST(LN_BranchValue, ALoopBehindABranchRunsOnlyWhenTheBranchTakesThatWay)
{
  TreeBuilder b;
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode &branch = b.Add("LogicNativeBranch");
  b.Link(AddBoolRead(b, "go"), "Value", branch, "Condition");
  b.Link(step, "Out", branch, "Flow");
  GenNode &loop = b.Add("LogicNativeLoop");
  b.Int(loop, "Count", 2);
  b.Link(branch, "True", loop, "Flow");
  GenNode &count = b.Add("LogicNativeModifyProperty");
  b.String(count, "Property", "n");
  b.Float(count, "Value", 1.0f);
  b.Link(loop, "Loop", count, "Flow");
  const std::shared_ptr<LN_Program> program = Compile(b);
  ASSERT_NE(program, nullptr);
  for (const bool columns : {false, true}) {
    for (const SemMode &mode : {kProduction, kMemoOff}) {
      SCOPED_TRACE(std::string(mode.name) + (columns ? ", wide" : ""));
      SemRig rig(program, mode, 3);
      if (columns) {
        rig.SetColumns(true, 0);
      }
      for (size_t owner = 0; owner < 3; owner++) {
        rig.SetFloat(rig.Object(owner), "n", 0.0f);
        rig.SetBool(rig.Object(owner), "go", false);
      }
      std::string seen;
      for (int at = 1; at <= 6; at++) {
        for (size_t owner = 0; owner < 3; owner++) {
          rig.SetBool(rig.Object(owner), "go", at == 3 || at == 4);
        }
        rig.Step();
        seen += std::to_string(int(rig.Float(rig.Object(0), "n"))) + " ";
      }
      /* Two iterations on each of steps 3 and 4. */
      EXPECT_EQ(seen, "0 0 2 4 4 4 ");
    }
  }
}

/** Store Value's Flow from a Branch stores on the Branch's route only (on its Done route: the
 * step's own write of "s" reads it before, so it shows the next step); its first read stores
 * (Store On Init). The Branch's pulse as its Flow changes none of it. */
TEST(LN_BranchValue, StoreValueBehindABranchStoresOnTheBranchsRoute)
{
  TreeBuilder b;
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode &branch = b.Add("LogicNativeBranch");
  b.Link(AddBoolRead(b, "go"), "Value", branch, "Condition");
  b.Link(step, "Out", branch, "Flow");
  GenNode &store = b.Add("LogicNativeStoreValue");
  GenNode &value = b.Add("LogicNativeGetGamePropertyFloat");
  b.String(value, "Property", "v");
  b.Link(value, "Value", store, "Value");
  b.Link(branch, "True", store, "Flow");
  GenNode &write = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(write, "Property", "s");
  b.Link(store, "Value", write, "Value");
  b.Link(step, "Out", write, "Flow");
  const std::shared_ptr<LN_Program> program = Compile(b);
  ASSERT_NE(program, nullptr);
  Script script;
  script.go_steps = {3};
  for (const bool columns : {false, true}) {
    SCOPED_TRACE(columns ? "wide" : "tree by tree");
    SemRig rig(program, kProduction, 2);
    if (columns) {
      rig.SetColumns(true, 0);
    }
    std::string seen;
    for (int at = 1; at <= 6; at++) {
      for (size_t owner = 0; owner < 2; owner++) {
        rig.SetBool(rig.Object(owner), "go", at == 3);
        rig.SetFloat(rig.Object(owner), "v", float(at));
      }
      rig.Step();
      seen += std::to_string(int(rig.Float(rig.Object(0), "s", -1.0f))) + " ";
    }
    /* "v" is the step's number: stored at the first read (1) and on step 3. */
    EXPECT_EQ(seen, "1 1 1 3 3 3 ");
  }
}

/** \} */
