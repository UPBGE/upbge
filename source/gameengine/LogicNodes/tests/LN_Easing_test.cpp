/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* Tween Value's easing presets and Repeat, and the Oscillate node (2026-11-17, LN_Easing.h): the
 * functions against values worked out by hand, then the nodes step by step against models
 * written here, in every mode and in a wide run. */

#include "testing/testing.h"

#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "LN_Easing.h"
#include "LN_SemRig.h"

using namespace ln_test;
using namespace ln_test::sem;

namespace {
constexpr double kPi = 3.14159265358979323846;
}

TEST(LN_Easing, EveryPresetGoesFromZeroToOne)
{
  for (int transition = 1; transition <= 11; transition++) {
    for (int ease = 0; ease <= 2; ease++) {
      SCOPED_TRACE("transition " + std::to_string(transition) + " ease " + std::to_string(ease));
      const LN_TweenTransition t = LN_TweenTransition(transition);
      const LN_TweenEase e = LN_TweenEase(ease);
      EXPECT_NEAR(LN_Ease(t, e, 0.0f), 0.0f, 1.0e-6f);
      EXPECT_NEAR(LN_Ease(t, e, 1.0f), 1.0f, 1.0e-6f);
      EXPECT_NEAR(LN_Ease(t, e, -3.0f), 0.0f, 1.0e-6f) << "clamped";
      EXPECT_NEAR(LN_Ease(t, e, 7.0f), 1.0f, 1.0e-6f) << "clamped";
      EXPECT_NEAR(LN_Ease(t, e, std::nanf("")), 0.0f, 1.0e-6f);
      if (e == LN_TweenEase::InOut) {
        EXPECT_NEAR(LN_Ease(t, e, 0.5f), 0.5f, 1.0e-6f) << "the middle";
      }
      /* Out mirrors In. */
      for (const float x : {0.1f, 0.3f, 0.7f}) {
        EXPECT_NEAR(LN_Ease(t, LN_TweenEase::Out, x),
                    1.0f - LN_Ease(t, LN_TweenEase::In, 1.0f - x),
                    1.0e-6f);
      }
    }
  }
}

TEST(LN_Easing, PresetsGiveTheirTextbookValues)
{
  using T = LN_TweenTransition;
  using E = LN_TweenEase;
  EXPECT_FLOAT_EQ(LN_Ease(T::Linear, E::In, 0.3f), 0.3f);
  EXPECT_FLOAT_EQ(LN_Ease(T::Quad, E::In, 0.5f), 0.25f);
  EXPECT_FLOAT_EQ(LN_Ease(T::Quad, E::Out, 0.5f), 0.75f);
  EXPECT_FLOAT_EQ(LN_Ease(T::Quad, E::InOut, 0.25f), 0.125f);
  EXPECT_FLOAT_EQ(LN_Ease(T::Cubic, E::In, 0.5f), 0.125f);
  EXPECT_FLOAT_EQ(LN_Ease(T::Quart, E::In, 0.5f), 0.0625f);
  EXPECT_FLOAT_EQ(LN_Ease(T::Quint, E::In, 0.5f), 0.03125f);
  EXPECT_NEAR(LN_Ease(T::Sine, E::In, 0.5f), 1.0 - std::cos(kPi / 4.0), 1.0e-6);
  EXPECT_FLOAT_EQ(LN_Ease(T::Expo, E::In, 0.5f), 0.03125f);
  EXPECT_NEAR(LN_Ease(T::Circ, E::In, 0.5f), 1.0 - std::sqrt(0.75), 1.0e-6);
  /* Back pulls back below 0 first (In) and overshoots above 1 (Out). */
  EXPECT_NEAR(LN_Ease(T::Back, E::In, 0.5f), 2.70158 * 0.125 - 1.70158 * 0.25, 1.0e-6);
  EXPECT_LT(LN_Ease(T::Back, E::In, 0.2f), 0.0f);
  EXPECT_GT(LN_Ease(T::Back, E::Out, 0.8f), 1.0f);
  /* Elastic Out at the middle: 1 + 2^-5 x sin(pi / 6). */
  EXPECT_NEAR(LN_Ease(T::Elastic, E::Out, 0.5f), 1.015625, 1.0e-6);
  /* Bounce Out: the second hop's top at 0.75 of the way... and 0.765625 at the middle. */
  EXPECT_NEAR(LN_Ease(T::Bounce, E::Out, 0.5f), 0.765625, 1.0e-6);
  EXPECT_NEAR(LN_Ease(T::Bounce, E::Out, 1.0f / 2.75f), 1.0, 1.0e-6) << "the first landing";
}

TEST(LN_Easing, TheTweenSettingsComeApartAsStored)
{
  /* custom2 >> 2: bits 0-3 the transition, 4-5 the ease, 6-7 the repeat. */
  const int32_t packed = 10 | (1 << 4) | (2 << 6);
  EXPECT_EQ(LN_TweenTransitionOf(packed), LN_TweenTransition::Elastic);
  EXPECT_EQ(LN_TweenEaseOf(packed), LN_TweenEase::Out);
  EXPECT_EQ(LN_TweenRepeatOf(packed), LN_TweenRepeat::PingPong);
  EXPECT_EQ(LN_TweenTransitionOf(0), LN_TweenTransition::Curve) << "every older node";
  EXPECT_EQ(LN_TweenRepeatOf(0), LN_TweenRepeat::Once);
  EXPECT_EQ(LN_TweenTransitionOf(15), LN_TweenTransition::Curve) << "unknown: the curve";
}

TEST(LN_Easing, OscillateWaves)
{
  using W = LN_OscillateWave;
  const float quarter[] = {0.0f, 0.25f, 0.5f, 0.75f};
  const float sine[] = {0.0f, 1.0f, 0.0f, -1.0f};
  const float saw[] = {0.0f, 0.5f, -1.0f, -0.5f};
  for (int k = 0; k < 4; k++) {
    EXPECT_NEAR(LN_OscillateWaveValue(W::Sine, quarter[k]), sine[k], 1.0e-6f);
    EXPECT_NEAR(LN_OscillateWaveValue(W::Triangle, quarter[k]), sine[k], 1.0e-6f);
    EXPECT_NEAR(LN_OscillateWaveValue(W::Sawtooth, quarter[k]), saw[k], 1.0e-6f);
  }
  EXPECT_NEAR(LN_OscillateWaveValue(W::Triangle, 0.125f), 0.5f, 1.0e-6f) << "a straight line";
  EXPECT_EQ(LN_OscillateWaveValue(W::Square, 0.0f), 1.0f);
  EXPECT_EQ(LN_OscillateWaveValue(W::Square, 0.49f), 1.0f);
  EXPECT_EQ(LN_OscillateWaveValue(W::Square, 0.5f), -1.0f);
  EXPECT_NEAR(LN_OscillateWaveValue(W::Sawtooth, 0.49f), 0.98f, 1.0e-5f);

  /* The cycle: only its fraction, in double (hours of play keep their precision). */
  EXPECT_FLOAT_EQ(LN_OscillateCycle(2.25f, 1.0f, 0.5f, 0.0f), 0.75f);
  EXPECT_FLOAT_EQ(LN_OscillateCycle(0.25f, -1.0f, 0.0f, 0.0f), 0.75f) << "backwards";
  EXPECT_FLOAT_EQ(LN_OscillateCycle(36000.25f, 1.0f, 0.0f, 0.0f), 0.25f);
  EXPECT_FLOAT_EQ(LN_OscillateCycle(1.0f, 2.0f, 0.0f, 0.3f), 0.3f);
  EXPECT_EQ(LN_OscillateCycle(std::nanf(""), 1.0f, 0.0f, 0.0f), 0.0f);
  EXPECT_EQ(LN_OscillateCycle(INFINITY, 1.0f, 0.0f, 0.0f), 0.0f);
  const float just_below = std::nextafter(1.0f, 0.0f);
  EXPECT_LT(LN_OscillateCycle(1.0f, just_below, 0.0f, 0.0f), 1.0f) << "never 1";

  /* Bob and Orbit. */
  const MT_Vector3 bob = LN_OscillateBob(MT_Vector3(0.0f, 0.0f, 2.0f), 1.5f, 1.0f);
  EXPECT_FLOAT_EQ(bob.z(), 3.0f);
  const auto near = [](const MT_Vector3 &a, const MT_Vector3 &b) {
    return (a - b).length() < 1.0e-5f;
  };
  EXPECT_TRUE(near(LN_OscillateOrbit(MT_Vector3(0, 0, 1), 2.0f, 0.0f), MT_Vector3(2, 0, 0)));
  EXPECT_TRUE(near(LN_OscillateOrbit(MT_Vector3(0, 0, 1), 2.0f, 0.25f), MT_Vector3(0, 2, 0)))
      << "around Z from +X towards +Y";
  EXPECT_TRUE(near(LN_OscillateOrbit(MT_Vector3(0, 0, 5), 2.0f, 0.5f), MT_Vector3(-2, 0, 0)))
      << "the axis's length does not matter";
  EXPECT_TRUE(near(LN_OscillateOrbit(MT_Vector3(1, 0, 0), 1.0f, 0.0f), MT_Vector3(0, 1, 0)))
      << "an axis near X starts at +Y";
  EXPECT_TRUE(near(LN_OscillateOrbit(MT_Vector3(1, 0, 0), 1.0f, 0.25f), MT_Vector3(0, 0, 1)));
  EXPECT_TRUE(near(LN_OscillateOrbit(MT_Vector3(0, 0, 0), 1.0f, 0.25f), MT_Vector3(0, 1, 0)))
      << "no axis: Z";
  const MT_Vector3 tilted = LN_OscillateOrbit(MT_Vector3(1, 2, 3), 3.0f, 0.37f);
  EXPECT_NEAR(tilted.length(), 3.0f, 1.0e-5f);
  EXPECT_NEAR(tilted.dot(MT_Vector3(1, 2, 3)), 0.0f, 1.0e-4f) << "in the plane across the axis";
}

namespace {

/** Tween Value On Demand (or Forward-started), Float 0 to 10, with every Easing and Repeat on
 * its own node; each step writes Factor and Result and counts Reached. */
struct TweenCase {
  int16_t transition, ease, repeat;
  bool on_demand;
};

const TweenCase kTweenCases[] = {
    {0, 0, 0, true},  /* the curve (linear), once: every older node */
    {3, 2, 0, true},  /* Quadratic In Out, once */
    {11, 1, 1, true}, /* Bounce Out, loop */
    {9, 0, 2, true},  /* Back In, ping-pong */
    {10, 2, 2, true}, /* Elastic In Out, ping-pong */
    {2, 1, 1, false}, /* Sine Out, loop, started by Forward */
    {4, 0, 2, false}, /* Cubic In, ping-pong, started by Forward */
    {1, 0, 0, false}, /* Linear, once, started by Forward */
};

std::shared_ptr<LN_Program> TweenProgram()
{
  TreeBuilder b;
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode *flow = &step;
  const char *flow_pin = "Out";
  const auto write = [&](const char *idname, const std::string &property, GenNode &from,
                         const char *pin) {
    GenNode &set = b.Add(idname);
    b.String(set, "Property", property.c_str());
    b.Link(from, pin, set, "Value");
    b.Link(*flow, flow_pin, set, "Flow");
    flow = &set;
    flow_pin = "Done";
  };
  GenNode &duration = b.Add("LogicNativeGetGamePropertyFloat");
  b.String(duration, "Property", "duration");
  GenNode &start = b.Add("LogicNativeGetGamePropertyBool");
  b.String(start, "Property", "start");
  for (size_t k = 0; k < std::size(kTweenCases); k++) {
    const TweenCase &c = kTweenCases[k];
    const int16_t custom2 = int16_t((c.on_demand ? 1 : 0) | (c.transition << 2) | (c.ease << 6) |
                                    (c.repeat << 8));
    GenNode &tween = b.Add("LogicNativeTweenValue", 0, custom2);
    b.Link(duration, "Value", tween, "Duration");
    b.Float(tween, "FromFloat", 0.0f);
    b.Float(tween, "ToFloat", 10.0f);
    if (!c.on_demand) {
      /* (A pulse: a Branch's True into Forward reads as always on, BuildInputExecutionExpression;
       * an older rule, noted in the plan.) */
      GenNode &edge = b.Add("LogicNativeBooleanEdge");
      b.Link(start, "Value", edge, "Condition");
      b.Link(edge, "Rising", tween, "Forward");
    }
    const std::string name = "t" + std::to_string(k);
    write("LogicNativeSetGamePropertyFloat", name + "_factor", tween, "Factor");
    write("LogicNativeSetGamePropertyFloat", name + "_result", tween, "ResultFloat");
    GenNode &count = b.Add("LogicNativeModifyProperty", int16_t(blender::NODE_MATH_ADD));
    b.String(count, "Property", (name + "_reached").c_str());
    b.Float(count, "Value", 1.0f);
    b.Link(tween, "Reached", count, "Flow");
  }
  std::string report;
  std::shared_ptr<LN_Program> program = b.Compile(&report);
  EXPECT_NE(program, nullptr) << report;
  return program;
}

/** The Tween's time, its direction and its Reached count, step by step. */
struct TweenModel {
  TweenCase c;
  double duration = 0.25;
  bool started = false, forward = true, active = false;
  float time = 0.0f;
  int reached = 0;
  void Step(const float dt, const bool start)
  {
    const bool repeating = c.repeat != 0;
    if (c.on_demand || start) {
      started = true;
      if (!c.on_demand) {
        forward = true;
      }
      active = repeating || time < float(duration) - 1.0e-6f;
    }
    if (!started || !active) {
      return;
    }
    const float d = float(duration);
    if (!repeating) {
      time = std::min(time + dt, d);
      if (time >= d - 1.0e-6f) {
        time = d;
        reached++;
        active = false;
      }
      return;
    }
    const float next = time + (forward ? dt : -dt);
    if (forward && next >= d - 1.0e-6f) {
      const float over = std::clamp(next - d, 0.0f, d);
      reached++;
      if (c.repeat == 1) {
        time = over;
      }
      else {
        time = d - over;
        forward = false;
      }
    }
    else if (!forward && next <= 1.0e-6f) {
      const float over = std::clamp(-next, 0.0f, d);
      reached++;
      if (c.repeat == 1) {
        time = d - over;
      }
      else {
        time = over;
        forward = true;
      }
    }
    else {
      time = next;
    }
  }
  float Factor() const
  {
    const float linear = std::clamp(time / float(duration), 0.0f, 1.0f);
    if (c.transition == 0) {
      return linear; /* the default curve is a straight line */
    }
    return LN_Ease(LN_TweenTransition(c.transition), LN_TweenEase(c.ease), linear);
  }
};

}  // namespace

/* Every preset and Repeat against the model, both On Demand and started by Forward; every mode
 * and a wide run give one text. */
TEST(LN_Easing, TweenPresetsLoopAndPingPongFollowTheModel)
{
  const std::shared_ptr<LN_Program> program = TweenProgram();
  ASSERT_NE(program, nullptr);
  const uint32_t owners = 2;
  const float durations[] = {0.25f, 0.4f};
  constexpr int kSteps = 70;
  const auto run = [&](SemRig &rig, const bool check) {
    std::vector<std::vector<TweenModel>> models(owners);
    for (uint32_t owner = 0; owner < owners; owner++) {
      KX_GameObject &object = rig.Object(owner);
      rig.SetFloat(object, "duration", durations[owner]);
      for (size_t k = 0; k < std::size(kTweenCases); k++) {
        rig.SetFloat(object, ("t" + std::to_string(k) + "_reached").c_str(), 0.0f);
        TweenModel model;
        model.c = kTweenCases[k];
        model.duration = durations[owner];
        models[owner].push_back(model);
      }
    }
    std::string text;
    for (int step = 0; step < kSteps; step++) {
      const bool start = step == 3;
      for (uint32_t owner = 0; owner < owners; owner++) {
        rig.SetBool(rig.Object(owner), "start", start);
      }
      rig.Step();
      for (uint32_t owner = 0; owner < owners; owner++) {
        KX_GameObject &object = rig.Object(owner);
        for (size_t k = 0; k < std::size(kTweenCases); k++) {
          const std::string name = "t" + std::to_string(k);
          const float factor = rig.Float(object, (name + "_factor").c_str());
          const float result = rig.Float(object, (name + "_result").c_str());
          const int reached = int(rig.Float(object, (name + "_reached").c_str()));
          text += LN_DumpFloatBits(factor) + " " + std::to_string(reached) + " ";
          if (check) {
            /* (The first step runs On Init too: the Tweens start in it.) */
            TweenModel &model = models[owner][k];
            model.Step(float(SemRig::kStep), start);
            SCOPED_TRACE("owner " + std::to_string(owner) + " tween " + std::to_string(k) +
                         " step " + std::to_string(step));
            EXPECT_NEAR(factor, model.Factor(), 1.0e-4f);
            EXPECT_NEAR(result, 10.0f * model.Factor(), 1.0e-3f);
            if (!model.c.on_demand) {
              /* (On Demand hides Reached.) */
              EXPECT_EQ(reached, model.reached);
            }
          }
        }
        text += "\n";
      }
    }
    return text;
  };
  {
    SemRig rig(program, kProduction, owners);
    run(rig, true);
  }
  std::string reference;
  for (const SemMode &mode : AllModes()) {
    SCOPED_TRACE(mode.name);
    SemRig rig(program, mode, owners);
    const std::string text = run(rig, false);
    if (reference.empty()) {
      reference = text;
    }
    EXPECT_EQ(text, reference);
  }
  SemRig wide(program, kProduction, owners);
  wide.SetColumns(true, 2, 1);
  EXPECT_EQ(run(wide, false), reference) << "a wide run";
}

namespace {

std::shared_ptr<LN_Program> OscillateProgram()
{
  TreeBuilder b;
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode *flow = &step;
  const char *flow_pin = "Out";
  const auto write = [&](const char *idname, const std::string &property, GenNode &from,
                         const char *pin) {
    GenNode &set = b.Add(idname);
    b.String(set, "Property", property.c_str());
    b.Link(from, pin, set, "Value");
    b.Link(*flow, flow_pin, set, "Flow");
    flow = &set;
    flow_pin = "Done";
  };
  const auto read = [&](const char *idname, const char *property) -> GenNode & {
    GenNode &node = b.Add(idname);
    b.String(node, "Property", property);
    return node;
  };
  GenNode &frequency = read("LogicNativeGetGamePropertyFloat", "frequency");
  GenNode &amplitude = read("LogicNativeGetGamePropertyFloat", "amplitude");
  GenNode &center = read("LogicNativeGetGamePropertyFloat", "center");
  GenNode &phase = read("LogicNativeGetGamePropertyFloat", "phase");
  GenNode &random = read("LogicNativeGetGamePropertyBool", "random");
  GenNode &axis = b.Add("LogicNativeCombineXYZ");
  b.Link(read("LogicNativeGetGamePropertyFloat", "ax"), "Value", axis, "X");
  b.Link(read("LogicNativeGetGamePropertyFloat", "ay"), "Value", axis, "Y");
  b.Link(read("LogicNativeGetGamePropertyFloat", "az"), "Value", axis, "Z");
  for (int16_t wave = 0; wave < 4; wave++) {
    GenNode &oscillate = b.Add("LogicNativeOscillate", wave);
    b.Link(frequency, "Value", oscillate, "Frequency");
    b.Link(amplitude, "Value", oscillate, "Amplitude");
    b.Link(center, "Value", oscillate, "Center");
    b.Link(phase, "Value", oscillate, "Phase");
    b.Link(random, "Value", oscillate, "Random Phase");
    b.Link(axis, "Vector", oscillate, "Axis");
    const std::string name = "w" + std::to_string(wave);
    write("LogicNativeSetGamePropertyFloat", name + "_value", oscillate, "Value");
    for (const char *output : {"Bob", "Orbit"}) {
      GenNode &split = b.Add("LogicNativeSeparateXYZ");
      b.Link(oscillate, output, split, "Vector");
      for (const char *component : {"X", "Y", "Z"}) {
        write("LogicNativeSetGamePropertyFloat",
              name + "_" + output + component,
              split,
              component);
      }
    }
  }
  std::string report;
  std::shared_ptr<LN_Program> program = b.Compile(&report);
  EXPECT_NE(program, nullptr) << report;
  return program;
}

struct OscillateCase {
  float frequency, amplitude, center, phase;
  bool random;
  float ax, ay, az;
};

const OscillateCase kOscillateCases[] = {
    {1.0f, 1.0f, 0.0f, 0.0f, false, 0.0f, 0.0f, 1.0f},
    {0.5f, 2.0f, 3.0f, 0.25f, false, 0.0f, 0.0f, 2.0f},
    {-2.0f, 0.5f, -1.0f, 0.1f, false, 1.0f, 0.0f, 0.0f},
    {3.0f, 1.5f, 0.0f, 0.0f, true, 0.0f, 1.0f, 1.0f},
    {3.0f, 1.5f, 0.0f, 0.0f, true, 0.0f, 1.0f, 1.0f},
    {1.0f, 1.0f, 0.0f, 0.0f, true, 0.0f, 0.0f, 1.0f},
};

}  // namespace

/* The node's outputs against waves written here (time: the steps' game time); Random Phase gives
 * each object its own fixed start; every mode and a wide run give one text (kernels FOscCycle,
 * FOscillate, VOscillate). */
TEST(LN_Easing, OscillateFollowsTheWaves)
{
  const std::shared_ptr<LN_Program> program = OscillateProgram();
  ASSERT_NE(program, nullptr);
  const uint32_t owners = uint32_t(std::size(kOscillateCases));
  constexpr int kSteps = 40;
  /* Each node its own start, per object: [owner][wave]. */
  std::vector<std::array<double, 4>> offsets(owners, {-1.0, -1.0, -1.0, -1.0});
  const auto run = [&](SemRig &rig, const bool check) {
    for (uint32_t owner = 0; owner < owners; owner++) {
      const OscillateCase &c = kOscillateCases[owner];
      KX_GameObject &object = rig.Object(owner);
      rig.SetFloat(object, "frequency", c.frequency);
      rig.SetFloat(object, "amplitude", c.amplitude);
      rig.SetFloat(object, "center", c.center);
      rig.SetFloat(object, "phase", c.phase);
      rig.SetBool(object, "random", c.random);
      rig.SetFloat(object, "ax", c.ax);
      rig.SetFloat(object, "ay", c.ay);
      rig.SetFloat(object, "az", c.az);
    }
    std::string text;
    for (int step = 0; step < kSteps; step++) {
      rig.Step();
      const double time = rig.LastContext().game_time;
      for (uint32_t owner = 0; owner < owners; owner++) {
        const OscillateCase &c = kOscillateCases[owner];
        KX_GameObject &object = rig.Object(owner);
        for (int wave = 0; wave < 4; wave++) {
          const std::string name = "w" + std::to_string(wave);
          const float value = rig.Float(object, (name + "_value").c_str());
          const MT_Vector3 bob(rig.Float(object, (name + "_BobX").c_str()),
                               rig.Float(object, (name + "_BobY").c_str()),
                               rig.Float(object, (name + "_BobZ").c_str()));
          const MT_Vector3 orbit(rig.Float(object, (name + "_OrbitX").c_str()),
                                 rig.Float(object, (name + "_OrbitY").c_str()),
                                 rig.Float(object, (name + "_OrbitZ").c_str()));
          text += LN_DumpFloatBits(value) + " " + LN_DumpFloatBits(bob.z()) + " " +
                  LN_DumpFloatBits(orbit.x()) + " ";
          if (!check) {
            continue;
          }
          SCOPED_TRACE("owner " + std::to_string(owner) + " wave " + std::to_string(wave) +
                       " step " + std::to_string(step));
          double cycle = float(time) * double(c.frequency) + c.phase;
          if (c.random) {
            /* The object's own start (each node its own): worked out from the node's Orbit (the
             * angle), the same at every step. */
            {
              const MT_Vector3 axis = MT_Vector3(c.ax, c.ay, c.az).normalized();
              const MT_Vector3 start = LN_OscillateOrbit(axis, 1.0f, 0.0f);
              const MT_Vector3 across = axis.cross(start);
              double angle = std::atan2(orbit.dot(across), orbit.dot(start)) / (2.0 * kPi);
              angle -= std::floor(angle);
              double offset = angle - (cycle - std::floor(cycle));
              offset -= std::floor(offset);
              if (offsets[owner][wave] < 0.0) {
                offsets[owner][wave] = offset;
              }
              const double drift = std::fabs(offset - offsets[owner][wave]);
              EXPECT_LT(std::min(drift, 1.0 - drift), 1.0e-4) << "a fixed start";
            }
            cycle += offsets[owner][wave];
          }
          cycle -= std::floor(cycle);
          double w = std::sin(2.0 * kPi * cycle);
          if (wave == 1) {
            w = cycle < 0.25 ? 4.0 * cycle : cycle < 0.75 ? 2.0 - 4.0 * cycle : 4.0 * cycle - 4.0;
          }
          else if (wave == 2) {
            w = cycle < 0.5 ? 1.0 : -1.0;
          }
          else if (wave == 3) {
            w = cycle < 0.5 ? 2.0 * cycle : 2.0 * cycle - 2.0;
          }
          const bool edge = (wave == 2 && std::fabs(cycle - 0.5) < 1.0e-4) ||
                            (wave == 3 && std::fabs(cycle - 0.5) < 1.0e-4) || cycle > 1.0 - 1.0e-4 ||
                            cycle < 1.0e-4;
          if (!edge) {
            EXPECT_NEAR(value, c.center + c.amplitude * w, 1.0e-3);
            EXPECT_NEAR(bob.z(), c.az * c.amplitude * w, 1.0e-3);
          }
          const MT_Vector3 expected_orbit = LN_OscillateOrbit(
              MT_Vector3(c.ax, c.ay, c.az), c.amplitude, float(cycle));
          EXPECT_LT((orbit - expected_orbit).length(), 1.0e-3f);
          EXPECT_NEAR(orbit.length(), std::fabs(c.amplitude), 1.0e-4);
        }
      }
      text += "\n";
    }
    return text;
  };
  {
    SemRig rig(program, kProduction, owners);
    run(rig, true);
    EXPECT_GT(std::fabs(offsets[3][0] - offsets[5][0]), 1.0e-3) << "each object its own start";
    EXPECT_GT(std::fabs(offsets[3][0] - offsets[4][0]), 1.0e-3);
    EXPECT_GT(std::fabs(offsets[3][0] - offsets[3][1]), 1.0e-3) << "each node its own start";
  }
  std::string reference;
  for (const SemMode &mode : AllModes()) {
    SCOPED_TRACE(mode.name);
    SemRig rig(program, mode, owners);
    const std::string text = run(rig, false);
    if (reference.empty()) {
      reference = text;
    }
    EXPECT_EQ(text, reference);
  }
  SemRig wide(program, kProduction, owners);
  wide.SetColumns(true, 2, 1);
  EXPECT_EQ(run(wide, false), reference) << "a wide run";
  if (LN_GetColumnDirectMode() != LN_ColumnDirectMode::Force) {
    EXPECT_EQ(wide.LastColumnLanes(), uint64_t(owners))
        << "the objects ran wide: " << wide.LastColumnRefusal();
  }
}
