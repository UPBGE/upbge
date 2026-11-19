/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * The "also small" conveniences (2026-11-17): Draw's Sphere and Duration, Date and Time, Save
 * Slot. Each through a tree built by idname and run in the rig (SemRig).
 */

#include "testing/testing.h"

#include <cmath>
#include <string>
#include <vector>

#include "LN_DateTime.h"
#include "LN_RuntimeTree.h"
#include "LN_SemRig.h"

using namespace ln_test;
using namespace ln_test::sem;

namespace {

/** The rig's lines while it lives (LN_SetDebugLinesForTests, LN_SetTimedDebugLinesForTests). */
class SeenLines {
 public:
  SeenLines()
  {
    LN_SetDebugLinesForTests(&lines);
    LN_SetTimedDebugLinesForTests(&timed);
  }
  ~SeenLines()
  {
    LN_SetDebugLinesForTests(nullptr);
    LN_SetTimedDebugLinesForTests(nullptr);
  }
  LN_DebugLinesForTests lines;
  LN_TimedDebugLines timed;
};

/** [On Logic + Physics Step] -> Draw (`mode`) with Duration `duration`. */
std::shared_ptr<LN_Program> DrawProgram(const int16_t mode, const float duration)
{
  TreeBuilder b;
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode &draw = b.Add("LogicNativeDraw", mode);
  b.Link(step, "Out", draw, "Flow");
  b.Color(draw, "Color", 0.2f, 0.8f, 0.4f, 1.0f);
  b.Vector(draw, "Origin", 1.0f, 2.0f, 3.0f);
  b.Vector(draw, "Target", 4.0f, 2.0f, 3.0f);
  b.Float(draw, "Radius", 2.0f);
  b.Float(draw, "Width", 2.0f);
  b.Float(draw, "Duration", duration);
  std::string report;
  std::shared_ptr<LN_Program> program = b.Compile(&report);
  EXPECT_NE(program, nullptr) << report;
  return program;
}

}  // namespace

/* -------------------------------------------------------------------- */
/** \name Draw: Sphere and Duration
 * \{ */

/** Sphere: three circles of Radius around the Origin (24 lines each) in the Color; with a
 * Duration every line is kept to be drawn again until its time is up, without one none is. */
TEST(LN_Draw, SphereIsThreeCirclesOfItsRadiusAroundTheOrigin)
{
  for (const float duration : {0.0f, 0.5f}) {
    SCOPED_TRACE(duration);
    const std::shared_ptr<LN_Program> program = DrawProgram(7, duration);
    ASSERT_NE(program, nullptr);
    SemRig rig(program, kProduction);
    SeenLines seen;
    rig.Step();
    ASSERT_EQ(seen.lines.size(), 72u);
    const MT_Vector3 center(1.0f, 2.0f, 3.0f);
    for (const auto &[line, line_duration] : seen.lines) {
      EXPECT_NEAR((line.from - center).length(), 2.0f, 1.0e-4f);
      EXPECT_NEAR((line.to - center).length(), 2.0f, 1.0e-4f);
      EXPECT_EQ(line.color, MT_Vector4(0.2f, 0.8f, 0.4f, 1.0f));
      EXPECT_EQ(line_duration, duration);
    }
    EXPECT_EQ(seen.timed.Count(), duration > 0.0f ? 72u : 0u);
  }
}

/** Every shape takes the Duration: a line, a box; the kept lines go when their time is up
 * (the rig's time is 1/60 a step). */
TEST(LN_Draw, EveryShapeKeepsItsLinesForItsDuration)
{
  const std::shared_ptr<LN_Program> line_program = DrawProgram(0, 1.0f);
  const std::shared_ptr<LN_Program> cube_program = DrawProgram(3, 1.0f);
  ASSERT_NE(line_program, nullptr);
  ASSERT_NE(cube_program, nullptr);
  {
    SemRig rig(line_program, kProduction);
    SeenLines seen;
    rig.Step();
    ASSERT_EQ(seen.lines.size(), 1u);
    EXPECT_EQ(seen.lines[0].first.from, MT_Vector3(1.0f, 2.0f, 3.0f));
    EXPECT_EQ(seen.lines[0].first.to, MT_Vector3(4.0f, 2.0f, 3.0f));
    EXPECT_EQ(seen.lines[0].second, 1.0f);
    EXPECT_EQ(seen.timed.Count(), 1u);
    seen.lines.clear();
    seen.timed.Draw(0.5);
    EXPECT_EQ(seen.lines.size(), 1u);
    seen.timed.Draw(2.0);
    EXPECT_EQ(seen.timed.Count(), 0u);
  }
  {
    SemRig rig(cube_program, kProduction);
    SeenLines seen;
    rig.Step();
    /* A cube of Width 2: 12 edges, each 2 long. */
    ASSERT_EQ(seen.lines.size(), 12u);
    for (const auto &[line, line_duration] : seen.lines) {
      EXPECT_NEAR((line.to - line.from).length(), 2.0f, 1.0e-5f);
      EXPECT_EQ(line_duration, 1.0f);
    }
    EXPECT_EQ(seen.timed.Count(), 12u);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Date and Time
 * \{ */

namespace {

/** The rig's wall clock while it lives (LN_SetWallClockForTests). */
class FixedClock {
 public:
  explicit FixedClock(const double seconds) : m_seconds(seconds)
  {
    LN_SetWallClockForTests(&m_seconds);
  }
  ~FixedClock()
  {
    LN_SetWallClockForTests(nullptr);
  }

 private:
  double m_seconds;
};

}  // namespace

/** The parts of known moments in UTC: the epoch (a Thursday), a leap day, an afternoon with a
 * fraction of a second, the last second of a year. */
TEST(LN_DateTime, KnownMomentsInUTC)
{
  LN_DateTimeParts parts;
  ASSERT_TRUE(LN_DateTimeFromSeconds(0.0, true, parts));
  EXPECT_EQ(parts.year, 1970);
  EXPECT_EQ(parts.month, 1);
  EXPECT_EQ(parts.day, 1);
  EXPECT_EQ(parts.hour, 0);
  EXPECT_EQ(parts.weekday, 3); /* Thursday */
  EXPECT_EQ(parts.day_number, 0);
  EXPECT_EQ(parts.day_fraction, 0.0);

  /* 2000-02-29 00:00:00, a Tuesday. */
  ASSERT_TRUE(LN_DateTimeFromSeconds(951782400.0, true, parts));
  EXPECT_EQ(parts.year, 2000);
  EXPECT_EQ(parts.month, 2);
  EXPECT_EQ(parts.day, 29);
  EXPECT_EQ(parts.weekday, 1);
  EXPECT_EQ(parts.day_number, 11016);

  /* 2023-11-14 22:13:20.5, a Tuesday: 80000.5 seconds into the day. */
  ASSERT_TRUE(LN_DateTimeFromSeconds(1700000000.5, true, parts));
  EXPECT_EQ(parts.year, 2023);
  EXPECT_EQ(parts.month, 11);
  EXPECT_EQ(parts.day, 14);
  EXPECT_EQ(parts.hour, 22);
  EXPECT_EQ(parts.minute, 13);
  EXPECT_EQ(parts.second, 20);
  EXPECT_EQ(parts.weekday, 1);
  EXPECT_EQ(parts.day_number, 19675);
  EXPECT_NEAR(parts.day_fraction, 80000.5 / 86400.0, 1.0e-12);

  /* 2026-12-31 23:59:59, a Thursday; the next second a new year and day. */
  ASSERT_TRUE(LN_DateTimeFromSeconds(1798761599.0, true, parts));
  EXPECT_EQ(parts.year, 2026);
  EXPECT_EQ(parts.month, 12);
  EXPECT_EQ(parts.day, 31);
  EXPECT_EQ(parts.second, 59);
  EXPECT_EQ(parts.weekday, 3);
  const int32_t last_day = parts.day_number;
  ASSERT_TRUE(LN_DateTimeFromSeconds(1798761600.0, true, parts));
  EXPECT_EQ(parts.year, 2027);
  EXPECT_EQ(parts.day_number, last_day + 1);
  EXPECT_EQ(parts.weekday, 4);
  EXPECT_LT(parts.day_fraction, 1.0e-12);

  /* Nothing to read: 1970's parts. */
  EXPECT_FALSE(LN_DateTimeFromSeconds(std::nan(""), true, parts));
  EXPECT_EQ(parts.year, 1970);
}

/** The node gives every part of the tick's clock (UTC, and the local time the C library gives
 * for the same moment), each read of a tick the same. */
TEST(LN_DateTime, TheNodeGivesTheTicksClock)
{
  for (const bool utc : {true, false}) {
    SCOPED_TRACE(utc ? "UTC" : "local");
    TreeBuilder b;
    GenNode &step = b.Add("LogicNativeOnFixedUpdate");
    GenNode &date = b.Add("LogicNativeDateTime", utc ? 1 : 0);
    GenNode *flow = &step;
    const char *flow_pin = "Out";
    const char *const ints[] = {
        "Year", "Month", "Day", "Hour", "Minute", "Second", "Weekday", "Day Number"};
    for (const char *name : ints) {
      GenNode &set = b.Add("LogicNativeSetGamePropertyInt");
      b.String(set, "Property", name);
      b.Link(*flow, flow_pin, set, "Flow");
      b.Link(date, name, set, "Value");
      flow = &set;
      flow_pin = "Done";
    }
    GenNode &fraction = b.Add("LogicNativeSetGamePropertyFloat");
    b.String(fraction, "Property", "Day Fraction");
    b.Link(*flow, flow_pin, fraction, "Flow");
    b.Link(date, "Day Fraction", fraction, "Value");
    std::string report;
    const std::shared_ptr<LN_Program> program = b.Compile(&report);
    ASSERT_NE(program, nullptr) << report;

    const double moment = 1700000000.5;
    FixedClock clock(moment);
    LN_DateTimeParts expected;
    ASSERT_TRUE(LN_DateTimeFromSeconds(moment, utc, expected));
    for (const SemMode &mode : {kProduction, kMemoOff}) {
      SemRig rig(program, mode, 3);
      rig.Step();
      for (uint32_t owner = 0; owner < 3; owner++) {
        KX_GameObject &object = rig.Object(owner);
        EXPECT_EQ(rig.Float(object, "Year"), float(expected.year));
        EXPECT_EQ(rig.Float(object, "Month"), float(expected.month));
        EXPECT_EQ(rig.Float(object, "Day"), float(expected.day));
        EXPECT_EQ(rig.Float(object, "Hour"), float(expected.hour));
        EXPECT_EQ(rig.Float(object, "Minute"), float(expected.minute));
        EXPECT_EQ(rig.Float(object, "Second"), float(expected.second));
        EXPECT_EQ(rig.Float(object, "Weekday"), float(expected.weekday));
        EXPECT_EQ(rig.Float(object, "Day Number"), float(expected.day_number));
        EXPECT_FLOAT_EQ(rig.Float(object, "Day Fraction"), float(expected.day_fraction));
      }
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name On Removed
 * \{ */

/** On Removed's route runs in the step its tree is flagged (LN_Manager's hold sets the flag for
 * the object's last step), in every rig mode and with wide runs, and only for the flagged trees;
 * the program says it reads the flag (the manager holds only such objects). */
TEST(LN_OnRemoved, TheRouteRunsInTheFlaggedStepOnly)
{
  TreeBuilder b;
  GenNode &removed = b.Add("LogicNativeOnRemoved");
  GenNode &count = b.Add("LogicNativeModifyProperty");
  b.Link(removed, "Out", count, "Flow");
  b.String(count, "Property", "drops");
  b.Float(count, "Value", 1.0f);
  std::string report;
  const std::shared_ptr<LN_Program> program = b.Compile(&report);
  ASSERT_NE(program, nullptr) << report;
  EXPECT_TRUE(program->UsesBeingRemoved());

  for (const bool columns : {false, true}) {
    for (const SemMode &mode : {kProduction, kMemoOff}) {
      SCOPED_TRACE(std::string(mode.name) + (columns ? ", wide" : ""));
      SemRig rig(program, mode, 6);
      if (columns) {
        rig.SetColumns(true, 0);
      }
      for (uint32_t owner = 0; owner < 6; owner++) {
        rig.SetFloat(rig.Object(owner), "drops", 0.0f);
      }
      rig.Step();
      /* Owners 1 and 4 removed: their last step. */
      rig.Tree(1).SetBeingRemoved(true);
      rig.Tree(4).SetBeingRemoved(true);
      rig.Step();
      for (uint32_t owner = 0; owner < 6; owner++) {
        EXPECT_EQ(rig.Float(rig.Object(owner), "drops"), owner == 1 || owner == 4 ? 1.0f : 0.0f)
            << "owner " << owner;
      }
    }
  }

  /* A program without the node: never held. */
  TreeBuilder plain;
  plain.Add("LogicNativeOnFixedUpdate");
  const std::shared_ptr<LN_Program> without = plain.Compile(&report);
  ASSERT_NE(without, nullptr) << report;
  EXPECT_FALSE(without->UsesBeingRemoved());
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Tween Value's Apply To
 * \{ */

/** Apply To writes the Result to the object on each step the tween moves (linear: 4 m in a
 * second of 60 steps), wide and tree by tree, and nothing once it stopped (another mover is not
 * fought); a Rotation tween turns the object. */
TEST(LN_TweenApplyTo, TheTweenMovesAndTurnsTheObjectWhileItRuns)
{
  const auto program_of = [](const int16_t type, const int apply_to) {
    TreeBuilder b;
    GenNode &step = b.Add("LogicNativeOnFixedUpdate");
    /* Forward held every step; Linear easing; Apply To. */
    GenNode &tween = b.Add(
        "LogicNativeTweenValue", type, int16_t((1 << 2) | (2 << 6) | (apply_to << 10)));
    b.Link(step, "Out", tween, "Forward");
    b.Float(tween, "Duration", 1.0f);
    if (type == 1) {
      b.Vector(tween, "FromVector", 0.0f, 0.0f, 0.0f);
      b.Vector(tween, "ToVector", 4.0f, 0.0f, 0.0f);
    }
    else {
      b.Rotation(tween, "FromRotation", 0.0f, 0.0f, 0.0f);
      b.Rotation(tween, "ToRotation", 0.0f, 0.0f, float(M_PI_2));
    }
    std::string report;
    std::shared_ptr<LN_Program> program = b.Compile(&report);
    EXPECT_NE(program, nullptr) << report;
    return program;
  };
  const std::shared_ptr<LN_Program> slide = program_of(1, 1 /* World Position */);
  const std::shared_ptr<LN_Program> turn = program_of(2, 3 /* World Rotation */);
  ASSERT_NE(slide, nullptr);
  ASSERT_NE(turn, nullptr);

  for (const bool columns : {false, true}) {
    SCOPED_TRACE(columns ? "wide" : "tree by tree");
    SemRig rig(slide, kProduction, 4);
    if (columns) {
      rig.SetColumns(true, 0);
    }
    for (int step = 0; step < 30; step++) {
      rig.Step();
    }
    for (uint32_t owner = 0; owner < 4; owner++) {
      EXPECT_NEAR(rig.Object(owner).NodeGetWorldPosition().x(), 2.0f, 0.15f) << owner;
    }
    for (int step = 0; step < 40; step++) {
      rig.Step();
    }
    EXPECT_NEAR(rig.Object(0).NodeGetWorldPosition().x(), 4.0f, 1.0e-4f);
    /* Stopped: moved elsewhere, it stays there. */
    rig.Object(0).NodeSetWorldPosition(MT_Vector3(9.0f, 1.0f, 0.0f));
    rig.Object(0).NodeUpdateGS(0.0);
    rig.Step();
    EXPECT_NEAR(rig.Object(0).NodeGetWorldPosition().x(), 9.0f, 1.0e-5f);
  }
  {
    SemRig rig(turn, kProduction, 1);
    for (int step = 0; step < 70; step++) {
      rig.Step();
    }
    const MT_Vector3 x_axis = rig.Object(0).NodeGetWorldOrientation() * MT_Vector3(1, 0, 0);
    EXPECT_NEAR(x_axis.x(), 0.0f, 1.0e-4f);
    EXPECT_NEAR(x_axis.y(), 1.0f, 1.0e-4f);
  }
}

/** \} */
