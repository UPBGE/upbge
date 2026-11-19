/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* Delay's two modes (Queue: LogicNativeDelay, Restart: LogicNativeTimer) with Stop, Pause and
 * Resume, Remaining and Waiting (2026-11-17): a schedule of commands, step by step, against a
 * model written here, in every mode and in a wide run (the columns' time op kernel). */

#include "testing/testing.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "LN_SemRig.h"

using namespace ln_test;
using namespace ln_test::sem;

namespace {

/** The node reads its seconds and four switches from game properties; a Sequence runs Arm, Stop,
 * Pause and Resume in that order, then writes Remaining and Waiting; Done counts. */
std::shared_ptr<LN_Program> CountdownProgram(const bool timer)
{
  TreeBuilder b;
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode &sequence = b.Add("LogicNativeSequence", 0, 0, 5);
  b.Link(step, "Out", sequence, "Flow");
  GenNode &countdown = b.Add(timer ? "LogicNativeTimer" : "LogicNativeDelay");
  GenNode &seconds = b.Add("LogicNativeGetGamePropertyFloat");
  b.String(seconds, "Property", "seconds");
  b.Link(seconds, "Value", countdown, timer ? "Seconds" : "Delay");
  const char *flows[] = {timer ? "Set Timer" : "Flow", "Stop", "Pause", "Resume"};
  const char *switches[] = {"arm", "stop", "pause", "resume"};
  for (int k = 0; k < 4; k++) {
    GenNode &read = b.Add("LogicNativeGetGamePropertyBool");
    b.String(read, "Property", switches[k]);
    GenNode &branch = b.Add("LogicNativeBranch");
    b.Link(read, "Value", branch, "Condition");
    b.Link(sequence, ("Then " + std::to_string(k)).c_str(), branch, "Flow");
    b.Link(branch, "True", countdown, flows[k]);
  }
  GenNode &remaining = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(remaining, "Property", "remaining");
  b.Link(countdown, "Remaining", remaining, "Value");
  b.Link(sequence, "Then 4", remaining, "Flow");
  GenNode &waiting = b.Add("LogicNativeSetGamePropertyBool");
  b.String(waiting, "Property", "waiting");
  b.Link(countdown, "Waiting", waiting, "Value");
  b.Link(remaining, "Done", waiting, "Flow");
  GenNode &count = b.Add("LogicNativeModifyProperty", int16_t(blender::NODE_MATH_ADD));
  b.String(count, "Property", "done");
  b.Float(count, "Value", 1.0f);
  b.Link(countdown, "Out", count, "Flow");
  std::string report;
  std::shared_ptr<LN_Program> program = b.Compile(&report);
  EXPECT_NE(program, nullptr) << report;
  return program;
}

struct Schedule {
  float seconds;
  /** One letter a step: A arm, S stop, P pause, R resume, '.' nothing. */
  const char *steps;
};

/* Each owner its own story (the steps from 2 on: On Init runs in the first). Delay's and
 * Timer's runs differ only in owner 3 (a second Flow while waiting: Queue keeps both, Restart
 * starts again). */
const Schedule kSchedules[] = {
    /* Pause after 5 steps, held 5 steps, resumed: fires 5 steps late. */
    {0.1f, "..A.....P....R.................."},
    /* Stopped while waiting (no Done); paused and stopped (the pause ends); armed again. */
    {0.25f, "..A......S......A..........P.S..A..................."},
    /* Paused while idle, then armed: waits paused until Resume. */
    {0.05f, "..P..A......R..........."},
    /* A second Flow while the first waits. */
    {0.2f, "..A...A........................"},
    /* No time: Done at once, every Flow. */
    {0.0f, "..A....A....P.A..R..."},
    /* Quick Pause / Resume, then Stop. */
    {0.1f, "..A.P.R.P.R.S..A.P..S..R......."},
};

struct Model {
  bool timer = false;
  bool paused = false;
  std::vector<double> pending; /* Queue: every waiting countdown; Restart: at most one */
  int done = 0;

  void Tick(const double dt)
  {
    if (paused || pending.empty()) {
      return;
    }
    for (double &remaining : pending) {
      remaining = std::max(0.0, remaining - dt);
    }
    /* One Done a step: the first due. */
    for (size_t k = 0; k < pending.size(); k++) {
      if (pending[k] <= 1.0e-6) {
        pending.erase(pending.begin() + std::ptrdiff_t(k));
        done++;
        break;
      }
    }
  }
  void Command(const char c, const double seconds)
  {
    switch (c) {
      case 'A':
        if (timer) {
          pending.clear();
        }
        /* (No time: at the next step, as this tree's Done route comes before the Flow in the
         * step: TimeFlowZeroPulseNow.) */
        pending.push_back(std::max(0.0, seconds));
        break;
      case 'S':
        pending.clear();
        paused = false;
        break;
      case 'P':
        paused = true;
        break;
      case 'R':
        paused = false;
        break;
      default:
        break;
    }
  }
  double Remaining() const
  {
    return pending.empty() ? 0.0 : *std::min_element(pending.begin(), pending.end());
  }
};

constexpr int kSteps = 52;

std::string RunSchedules(SemRig &rig, const uint32_t owners)
{
  std::string text;
  for (uint32_t index = 0; index < owners; index++) {
    KX_GameObject &object = rig.Object(index);
    rig.SetFloat(object, "seconds", kSchedules[index].seconds);
    rig.SetFloat(object, "done", 0.0f);
    rig.SetFloat(object, "remaining", -1.0f);
    rig.SetBool(object, "waiting", false);
  }
  for (int step = 0; step < kSteps; step++) {
    for (uint32_t index = 0; index < owners; index++) {
      const std::string steps = kSchedules[index].steps;
      const char c = step < int(steps.size()) ? steps[size_t(step)] : '.';
      KX_GameObject &object = rig.Object(index);
      rig.SetBool(object, "arm", c == 'A');
      rig.SetBool(object, "stop", c == 'S');
      rig.SetBool(object, "pause", c == 'P');
      rig.SetBool(object, "resume", c == 'R');
    }
    rig.Step();
    for (uint32_t index = 0; index < owners; index++) {
      KX_GameObject &object = rig.Object(index);
      text += std::to_string(int(rig.Float(object, "done"))) + " " +
              LN_DumpFloatBits(rig.Float(object, "remaining")) + " " +
              std::to_string(int(rig.Float(object, "waiting"))) + (index + 1 < owners ? " | " : "\n");
    }
  }
  return text;
}

void CheckAgainstTheModel(SemRig &rig, const bool timer, const uint32_t owners)
{
  std::vector<Model> models(owners);
  for (Model &model : models) {
    model.timer = timer;
  }
  for (uint32_t index = 0; index < owners; index++) {
    KX_GameObject &object = rig.Object(index);
    rig.SetFloat(object, "seconds", kSchedules[index].seconds);
    rig.SetFloat(object, "done", 0.0f);
  }
  const double dt = 1.0 / 60.0;
  for (int step = 0; step < kSteps; step++) {
    for (uint32_t index = 0; index < owners; index++) {
      const std::string steps = kSchedules[index].steps;
      const char c = step < int(steps.size()) ? steps[size_t(step)] : '.';
      KX_GameObject &object = rig.Object(index);
      rig.SetBool(object, "arm", c == 'A');
      rig.SetBool(object, "stop", c == 'S');
      rig.SetBool(object, "pause", c == 'P');
      rig.SetBool(object, "resume", c == 'R');
      models[index].Tick(dt);
      models[index].Command(c, kSchedules[index].seconds);
    }
    rig.Step();
    for (uint32_t index = 0; index < owners; index++) {
      SCOPED_TRACE("owner " + std::to_string(index) + " step " + std::to_string(step));
      KX_GameObject &object = rig.Object(index);
      const Model &model = models[index];
      EXPECT_EQ(int(rig.Float(object, "done")), model.done);
      EXPECT_NEAR(rig.Float(object, "remaining"), model.Remaining(), 1.0e-5);
      EXPECT_EQ(rig.Float(object, "waiting") != 0.0f, !model.pending.empty());
    }
  }
}

}  // namespace

/* The model's answers, both modes; every mode of the rig and a wide run give one text. */
TEST(LN_Countdown, StopPauseResumeRemainingAndWaitingFollowTheModel)
{
  const uint32_t owners = uint32_t(std::size(kSchedules));
  for (const bool timer : {false, true}) {
    SCOPED_TRACE(timer ? "Restart (Timer)" : "Queue (Delay)");
    const std::shared_ptr<LN_Program> program = CountdownProgram(timer);
    ASSERT_NE(program, nullptr);
    {
      SemRig rig(program, kProduction, owners);
      CheckAgainstTheModel(rig, timer, owners);
    }
    std::string reference;
    for (const SemMode &mode : AllModes()) {
      SCOPED_TRACE(mode.name);
      SemRig rig(program, mode, owners);
      const std::string text = RunSchedules(rig, owners);
      if (reference.empty()) {
        reference = text;
      }
      EXPECT_EQ(text, reference);
    }
    SemRig wide(program, kProduction, owners);
    wide.SetColumns(true, 2, 1);
    EXPECT_EQ(RunSchedules(wide, owners), reference) << "a wide run";
    if (LN_GetColumnDirectMode() != LN_ColumnDirectMode::Force) {
      EXPECT_EQ(wide.LastColumnLanes(), uint64_t(owners))
          << "the objects ran wide: " << wide.LastColumnRefusal();
    }
  }
}

/* A Delay saved before 2026-11-17 has no control links: its program has no control op, Waiting
 * and Remaining are only read where linked. Stop on a node that never ran is harmless. */
TEST(LN_Countdown, StopWithoutACountdownDoesNothing)
{
  const std::shared_ptr<LN_Program> program = CountdownProgram(false);
  ASSERT_NE(program, nullptr);
  SemRig rig(program, kProduction, 1);
  KX_GameObject &object = rig.Object(0);
  rig.SetFloat(object, "seconds", 0.1f);
  rig.SetFloat(object, "done", 0.0f);
  for (const char *name : {"arm", "pause", "resume"}) {
    rig.SetBool(object, name, false);
  }
  rig.SetBool(object, "stop", true);
  for (int step = 0; step < 4; step++) {
    rig.Step();
  }
  EXPECT_EQ(rig.Float(object, "done"), 0.0f);
  EXPECT_EQ(rig.Float(object, "remaining"), 0.0f);
  EXPECT_EQ(rig.Float(object, "waiting"), 0.0f);
}

/* A Flow with no time to wait never loses its Done (2026-11-17): at once when the Done's route
 * comes after the Flow in the step (as before), at the next step when it came before (behind a
 * Branch: the route had already looked) or the Flow came in On Init (its Done runs on the steps).
 * Before, those two never gave a Done. Both modes, every rig mode and a wide run. */
TEST(LN_Countdown, ANoTimeFlowNeverLosesItsDone)
{
  for (const bool timer : {false, true}) {
    for (const int shape : {0, 1, 2}) {
      SCOPED_TRACE(std::string(timer ? "Timer" : "Delay") + " shape " + std::to_string(shape));
      TreeBuilder b;
      GenNode &countdown = b.Add(timer ? "LogicNativeTimer" : "LogicNativeDelay");
      b.Float(countdown, timer ? "Seconds" : "Delay", 0.0f);
      const char *flow = timer ? "Set Timer" : "Flow";
      if (shape == 0) {
        GenNode &step = b.Add("LogicNativeOnFixedUpdate");
        b.Link(step, "Out", countdown, flow);
      }
      else if (shape == 1) {
        GenNode &step = b.Add("LogicNativeOnFixedUpdate");
        GenNode &sequence = b.Add("LogicNativeSequence", 0, 0, 2);
        b.Link(step, "Out", sequence, "Flow");
        GenNode &branch = b.Add("LogicNativeBranch");
        b.Bool(branch, "Condition", true);
        b.Link(sequence, "Then 0", branch, "Flow");
        b.Link(branch, "True", countdown, flow);
      }
      else {
        GenNode &init = b.Add("LogicNativeOnInit");
        b.Link(init, "Out", countdown, flow);
      }
      GenNode &count = b.Add("LogicNativeModifyProperty", int16_t(blender::NODE_MATH_ADD));
      b.String(count, "Property", "done");
      b.Float(count, "Value", 1.0f);
      b.Link(countdown, "Out", count, "Flow");
      std::string report;
      const std::shared_ptr<LN_Program> program = b.Compile(&report);
      ASSERT_NE(program, nullptr) << report;
      /* Dones after each of 5 steps: a Flow every step gives one a step, at once (shape 0) or a
       * step later (shape 1); On Init's one in the first step (the rig's first Step runs On
       * Init, then the step). */
      const std::string expected = shape == 0 ? "1 2 3 4 5" : shape == 1 ? "0 1 2 3 4" :
                                                                           "1 1 1 1 1";
      std::vector<std::string> texts;
      const auto run = [&](SemRig &rig) {
        for (uint32_t owner = 0; owner < 3; owner++) {
          rig.SetFloat(rig.Object(owner), "done", 0.0f);
        }
        std::string text;
        for (int step = 0; step < 5; step++) {
          rig.Step();
          text += (step > 0 ? " " : "") + std::to_string(int(rig.Float(rig.Object(2), "done")));
        }
        return text;
      };
      for (const SemMode &mode : AllModes()) {
        SCOPED_TRACE(mode.name);
        SemRig rig(program, mode, 3);
        EXPECT_EQ(run(rig), expected);
      }
      SemRig wide(program, kProduction, 3);
      wide.SetColumns(true, 2, 1);
      EXPECT_EQ(run(wide), expected) << "a wide run";
    }
  }
}
