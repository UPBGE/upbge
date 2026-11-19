/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* Input Type Tap, Double Tap and Hold, and the Input Combo node (2026-11-17,
 * LN_InputGestures.h): the rules step by step, then a Keyboard Key fed key states (the rig's
 * input) and an Input Combo fed game properties, in every mode. */

#include "testing/testing.h"

#include <string>
#include <vector>

#include "LN_InputGestures.h"
#include "LN_SemRig.h"
#include "SCA_IInputDevice.h"

using namespace ln_test;
using namespace ln_test::sem;

namespace {

/** A press script: one letter a step of 0.05 s: '.' up, '#' held, 'v' pressed and held, '^'
 * released, '!' pressed and released within the step. The steps a gesture fires in. */
std::string GestureFires(const LN_InputGesture gesture, const double time, const std::string &script)
{
  LN_InputGestureState state;
  std::string fires;
  for (size_t k = 0; k < script.size(); k++) {
    const char c = script[k];
    const bool pressed = c == 'v' || c == '!';
    const bool released = c == '^' || c == '!';
    const bool active = c == '#' || c == 'v';
    fires += LN_InputGestureStep(state, gesture, double(k) * 0.05, time, active, pressed, released) ?
                 'X' :
                 '.';
  }
  return fires;
}

}  // namespace

TEST(LN_InputGestures, TapIsAShortPress)
{
  using G = LN_InputGesture;
  /* Released after 0.2 s (Time 0.3): a tap; after 0.4 s: none. */
  EXPECT_EQ(GestureFires(G::Tap, 0.3, "v###^....."), "....X.....");
  EXPECT_EQ(GestureFires(G::Tap, 0.3, "v#######^."), "..........");
  EXPECT_EQ(GestureFires(G::Tap, 0.3, ".!..!....."), ".X..X.....") << "within one step";
  EXPECT_EQ(GestureFires(G::Tap, 0.3, "^...v^...."), ".....X....") << "a release first: none";
}

TEST(LN_InputGestures, DoubleTapIsTwoPressesWithinTime)
{
  using G = LN_InputGesture;
  EXPECT_EQ(GestureFires(G::DoubleTap, 0.3, "v^..v^...."), "....X.....");
  EXPECT_EQ(GestureFires(G::DoubleTap, 0.3, "v^......v^v^"), "..........X.")
      << "too slow: the second press starts a new pair";
  EXPECT_EQ(GestureFires(G::DoubleTap, 0.3, "!!!!"), ".X.X") << "a pair, then a new pair";
}

TEST(LN_InputGestures, HoldFiresOnceAPress)
{
  using G = LN_InputGesture;
  /* Time 0.2: the fifth step of the press (0.2 s after it). */
  EXPECT_EQ(GestureFires(G::Hold, 0.2, "v#######^."), "....X.....");
  EXPECT_EQ(GestureFires(G::Hold, 0.2, "v#^.v####^"), "........X.") << "again after a release";
  EXPECT_EQ(GestureFires(G::Hold, 0.2, "v##^......"), "..........") << "let go too early";
  EXPECT_EQ(GestureFires(G::Hold, 0.0, "v#^v"), "X..X") << "no time: at the press";
}

namespace {

/** One letter a step, per step input '0'-'3' held (a set of digits between |), '.' none. */
std::string ComboFires(const LN_InputComboMode mode,
                       const double time,
                       const bool strict,
                       const uint32_t linked,
                       const std::vector<uint32_t> &held)
{
  LN_InputComboState state;
  std::string fires;
  for (size_t k = 0; k < held.size(); k++) {
    int32_t progress = 0;
    const bool fired = LN_InputComboStep(
        state, mode, double(k) * 0.05, time, strict, linked, held[k], progress);
    fires += fired ? 'X' : char('0' + progress);
  }
  return fires;
}

}  // namespace

TEST(LN_InputGestures, ASequenceIsStepsInOrderWithinTime)
{
  using M = LN_InputComboMode;
  constexpr uint32_t A = 1, B = 2, C = 4;
  /* A, B, C each within 0.2 s of the one before: done; the progress between. */
  EXPECT_EQ(ComboFires(M::Sequence, 0.2, true, A | B | C, {A, 0, B, 0, C, 0}), "1122X0");
  /* Too slow between B and C (0.25 s): back to the start. */
  EXPECT_EQ(ComboFires(M::Sequence, 0.2, true, A | B | C, {A, B, 0, 0, 0, 0, C}), "1222220");
  /* Strict: a wrong step starts again; not strict: it is ignored. */
  EXPECT_EQ(ComboFires(M::Sequence, 0.5, true, A | B | C, {A, 0, C, 0, B, 0, C}), "1100000");
  EXPECT_EQ(ComboFires(M::Sequence, 0.5, false, A | B | C, {A, 0, C, 0, B, 0, C}), "111122X");
  /* A wrong step that is the first step: again from one. */
  EXPECT_EQ(ComboFires(M::Sequence, 0.5, true, A | B, {A, 0, A, B}), "111X");
  /* Unlinked steps are no part of it. */
  EXPECT_EQ(ComboFires(M::Sequence, 0.5, true, A | C, {A, B, C}), "11X");
}

TEST(LN_InputGestures, AChordIsStepsHeldTogether)
{
  using M = LN_InputComboMode;
  constexpr uint32_t A = 1, B = 2;
  /* A, then B 0.1 s later while A is held: once, until one lets go; B again 0.25 s after A
   * was pressed (A held all along): not together. */
  EXPECT_EQ(ComboFires(M::Chord, 0.2, true, A | B, {A, A, A | B, A | B, A, A | B}), "11X212");
  /* B 0.3 s after A: not together. */
  EXPECT_EQ(ComboFires(M::Chord, 0.2, true, A | B, {A, A, A, A, A, A | B}), "111112");
  /* Both in one step. */
  EXPECT_EQ(ComboFires(M::Chord, 0.0, true, A | B, {0, A | B, A | B, 0}), "0X20");
}

namespace {

/* Keyboard Key "A" in each gesture mode (custom1), Time from a property; each counts its pulses. */
std::shared_ptr<LN_Program> GestureProgram()
{
  TreeBuilder b;
  GenNode &time = b.Add("LogicNativeGetGamePropertyFloat");
  b.String(time, "Property", "time");
  for (int16_t mode = 3; mode <= 5; mode++) {
    GenNode &key = b.Add("LogicNativeKeyboardKey", mode);
    b.String(key, "Key", "A");
    b.Link(time, "Value", key, "Time");
    GenNode &count = b.Add("LogicNativeModifyProperty", int16_t(blender::NODE_MATH_ADD));
    b.String(count, "Property", ("mode" + std::to_string(mode)).c_str());
    b.Float(count, "Value", 1.0f);
    b.Link(key, "Out", count, "Flow");
  }
  std::string report;
  std::shared_ptr<LN_Program> program = b.Compile(&report);
  EXPECT_NE(program, nullptr) << report;
  return program;
}

}  // namespace

/* The Keyboard Key's Tap, Double Tap and Hold read the key as the rules do; every mode alike. */
TEST(LN_InputGestures, KeyboardKeyGesturesFollowTheRules)
{
  const std::shared_ptr<LN_Program> program = GestureProgram();
  ASSERT_NE(program, nullptr);
  /* Steps of 1/60 s: Time 0.1 = 6 steps. Script: '.', '#', 'v', '^', '!' as GestureFires. */
  const std::string script = "..!.....v^...v#^.......v###########^....";
  std::string reference;
  for (const SemMode &mode : AllModes()) {
    SCOPED_TRACE(mode.name);
    SemRig rig(program, mode, 1);
    KX_GameObject &object = rig.Object(0);
    rig.SetFloat(object, "time", 0.1f);
    for (const char *name : {"mode3", "mode4", "mode5"}) {
      rig.SetFloat(object, name, 0.0f);
    }
    LN_InputSnapshot input;
    rig.SetInput(&input);
    LN_InputGestureState models[3];
    int counts[3] = {};
    std::string text;
    for (size_t k = 0; k < script.size(); k++) {
      const char c = script[k];
      const bool pressed = c == 'v' || c == '!';
      const bool released = c == '^' || c == '!';
      const bool active = c == '#' || c == 'v';
      input.SetKeyForTests(SCA_IInputDevice::AKEY, active, pressed, released);
      rig.Step();
      for (int g = 0; g < 3; g++) {
        counts[g] += LN_InputGestureStep(models[g],
                                         LN_InputGesture(3 + g),
                                         rig.LastContext().current_time,
                                         0.1,
                                         active,
                                         pressed,
                                         released) ?
                         1 :
                         0;
        const std::string name = "mode" + std::to_string(3 + g);
        EXPECT_EQ(int(rig.Float(object, name.c_str())), counts[g]) << name << " step " << k;
      }
      text += std::to_string(int(rig.Float(object, "mode3"))) +
              std::to_string(int(rig.Float(object, "mode4"))) +
              std::to_string(int(rig.Float(object, "mode5"))) + " ";
    }
    EXPECT_GT(counts[0], 0) << "a tap happened";
    EXPECT_GT(counts[1], 0) << "a double tap happened";
    EXPECT_GT(counts[2], 0) << "a hold happened";
    if (reference.empty()) {
      reference = text;
    }
    EXPECT_EQ(text, reference);
  }
}

/* Input Combo fed game properties: Sequence and Chord as the rules say, Progress too. */
TEST(LN_InputGestures, InputComboNodeFollowsTheRules)
{
  for (const bool chord : {false, true}) {
    SCOPED_TRACE(chord ? "Chord" : "Sequence");
    TreeBuilder b;
    GenNode &step = b.Add("LogicNativeOnFixedUpdate");
    GenNode &combo = b.Add("LogicNativeInputCombo", chord ? 1 : 0);
    b.Float(combo, "Time", 0.1f);
    for (int k = 1; k <= 3; k++) {
      GenNode &held = b.Add("LogicNativeGetGamePropertyBool");
      b.String(held, "Property", ("s" + std::to_string(k)).c_str());
      b.Link(held, "Value", combo, ("Step " + std::to_string(k)).c_str());
    }
    GenNode &count = b.Add("LogicNativeModifyProperty", int16_t(blender::NODE_MATH_ADD));
    b.String(count, "Property", "done");
    b.Float(count, "Value", 1.0f);
    b.Link(combo, "Triggered", count, "Flow");
    GenNode &progress = b.Add("LogicNativeSetGamePropertyInt");
    b.String(progress, "Property", "progress");
    b.Link(combo, "Progress", progress, "Value");
    b.Link(step, "Out", progress, "Flow");
    std::string report;
    const std::shared_ptr<LN_Program> program = b.Compile(&report);
    ASSERT_NE(program, nullptr) << report;
    /* Held steps a step (bits 0-2). */
    const std::vector<uint32_t> held = chord ? std::vector<uint32_t>{0, 1, 1, 3, 7, 7, 0, 7, 0} :
                                               std::vector<uint32_t>{0, 1, 0, 2, 0, 4, 0, 1, 2};
    std::string reference;
    for (const SemMode &mode : AllModes()) {
      SCOPED_TRACE(mode.name);
      SemRig rig(program, mode, 1);
      KX_GameObject &object = rig.Object(0);
      rig.SetFloat(object, "done", 0.0f);
      LN_InputComboState model;
      int done = 0;
      std::string text;
      for (size_t k = 0; k < held.size(); k++) {
        for (int s = 0; s < 3; s++) {
          rig.SetBool(object, ("s" + std::to_string(s + 1)).c_str(), (held[k] & (1u << s)) != 0);
        }
        rig.Step();
        int32_t expected_progress = 0;
        done += LN_InputComboStep(model,
                                  chord ? LN_InputComboMode::Chord : LN_InputComboMode::Sequence,
                                  rig.LastContext().current_time,
                                  0.1,
                                  true,
                                  7u,
                                  held[k],
                                  expected_progress) ?
                    1 :
                    0;
        EXPECT_EQ(int(rig.Float(object, "done")), done) << "step " << k;
        EXPECT_EQ(int(rig.Float(object, "progress")), expected_progress) << "step " << k;
        text += std::to_string(done) + std::to_string(expected_progress) + " ";
      }
      EXPECT_GT(done, 0);
      if (reference.empty()) {
        reference = text;
      }
      EXPECT_EQ(text, reference);
    }
  }
}
