/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Random numbers of Logic Nodes (2026-11-07, LN_Random.h): Random Value's types, Per Object and
 * Same for All, the Seed input, the game's seed (Scene > Game > Random Numbers, Set Random Seed),
 * steps that do not depend on frames, numbers that stay while other nodes are edited, a Loop's
 * Index as the seed. Wide runs against column passes: LN_Columns_test.cpp. */

#include "testing/testing.h"

#include <cstring>
#include <map>
#include <set>

#include "LN_Random.h"
#include "LN_SemRig.h"

using namespace ln_test;
using namespace ln_test::sem;

namespace {

std::shared_ptr<LN_Program> CompileOrFail(TreeBuilder &b)
{
  std::string report;
  std::shared_ptr<LN_Program> program = b.Compile(&report);
  EXPECT_NE(program, nullptr) << report;
  return program;
}

/** A test's game seed; the one before comes back after (0: what the goldens were recorded
 * with). */
struct SeedScope {
  uint32_t kept = LN_GetRandomGameSeed();
  explicit SeedScope(const uint32_t seed)
  {
    LN_SetRandomGameSeed(seed);
    LN_ClearPendingRandomGameSeedForTests();
  }
  ~SeedScope()
  {
    LN_SetRandomGameSeed(kept);
    LN_ClearPendingRandomGameSeedForTests();
  }
};

enum RandomType : int16_t { kFloat = 0, kInteger = 1, kVector = 2, kBoolean = 3 };
enum Variation : int16_t { kPerObject = 0, kSameForAll = 1 };

/** Writes a value into the owner's game properties, one writer after the other. */
struct Writers {
  TreeBuilder &b;
  GenNode *flow;
  const char *flow_pin = "Out";
  void Write(const char *kind, const char *property, GenNode &source, const char *pin)
  {
    GenNode &writer = b.Add(kind);
    b.String(writer, "Property", property);
    b.Link(*flow, flow_pin, writer, "Flow");
    b.Link(source, pin, writer, "Value");
    flow = &writer;
    flow_pin = "Done";
  }
};

/** On Logic + Physics Step: a Random Value of `type` and `variation` written to "r" ("rx", "ry",
 * "rz" for a vector); its Seed from the game property "seed" with `seed_from_property`. */
std::shared_ptr<LN_Program> RandomTree(const int16_t type,
                                       const int16_t variation,
                                       const bool seed_from_property = false,
                                       const char *rename = nullptr,
                                       const int extra_nodes_before = 0)
{
  TreeBuilder b;
  GenNode &event = b.Add("LogicNativeOnFixedUpdate");
  /* Other nodes made before the random node (they move its expression index and the builder's
   * running names): a renamed random node must not change. */
  for (int extra = 0; extra < extra_nodes_before; extra++) {
    GenNode &other = b.Add("LogicNativeRandomValue", kFloat, kPerObject);
    Writers w{b, &event};
    w.Write("LogicNativeSetGamePropertyFloat", ("other" + std::to_string(extra)).c_str(), other,
            "Value");
  }
  GenNode &random = b.Add("LogicNativeRandomValue", type, variation);
  if (rename != nullptr) {
    std::strncpy(random.node.name, rename, sizeof(random.node.name) - 1);
  }
  b.Float(random, "Min", -2.0f);
  b.Float(random, "Max", 7.0f);
  b.Int(random, "Min_Int", 3);
  b.Int(random, "Max_Int", 9);
  b.Vector(random, "Min_Vector", -1.0f, 4.0f, 2.0f);
  b.Vector(random, "Max_Vector", 1.0f, 4.0f, 3.0f);
  b.Float(random, "Probability", 0.25f);
  if (seed_from_property) {
    GenNode &seed = b.Add("LogicNativeGetGamePropertyInt");
    b.String(seed, "Property", "seed");
    b.Link(seed, "Value", random, "Seed");
  }
  Writers w{b, &event};
  switch (type) {
    case kInteger:
      w.Write("LogicNativeSetGamePropertyInt", "r", random, "Value_Int");
      break;
    case kVector: {
      GenNode &split = b.Add("LogicNativeSeparateXYZ");
      b.Link(random, "Value_Vector", split, "Vector");
      w.Write("LogicNativeSetGamePropertyFloat", "rx", split, "X");
      w.Write("LogicNativeSetGamePropertyFloat", "ry", split, "Y");
      w.Write("LogicNativeSetGamePropertyFloat", "rz", split, "Z");
      break;
    }
    case kBoolean:
      w.Write("LogicNativeSetGamePropertyBool", "r", random, "Value_Bool");
      break;
    default:
      w.Write("LogicNativeSetGamePropertyFloat", "r", random, "Value");
      break;
  }
  return CompileOrFail(b);
}

/** Every owner's "r" (or "rx" "ry" "rz") as exact bits, one line per owner. */
std::vector<std::string> Values(SemRig &rig, const uint32_t owners, const bool vector)
{
  std::vector<std::string> out;
  for (uint32_t owner = 0; owner < owners; owner++) {
    KX_GameObject &object = rig.Object(owner);
    out.push_back(vector ? Bits(rig.Float(object, "rx")) + " " + Bits(rig.Float(object, "ry")) +
                               " " + Bits(rig.Float(object, "rz")) :
                           Bits(rig.Float(object, "r")));
  }
  return out;
}

}  // namespace

/* The arithmetic of LN_Random.h: ranges with both ends in either order, the whole int32 range,
 * equal ends, probabilities 0 and 1, list places. */
TEST(LN_Random, TheRangesHoldTheirEndsAndEveryValueComesUp)
{
  std::set<int32_t> small;
  std::set<size_t> places;
  int chances = 0;
  bool negative = false, positive = false;
  for (uint64_t index = 0; index < 20000; index++) {
    const uint64_t bits = LN_RandomMix(index * 0x9e3779b97f4a7c15ull + 1);
    const int32_t a = LN_RandomIntInRange(bits, 3, 5);
    const int32_t reversed = LN_RandomIntInRange(bits, 5, 3);
    EXPECT_EQ(a, reversed) << "the two ends in either order";
    ASSERT_GE(a, 3);
    ASSERT_LE(a, 5);
    small.insert(a);
    const int32_t whole = LN_RandomIntInRange(bits, INT32_MIN, INT32_MAX);
    negative |= whole < 0;
    positive |= whole > 0;
    EXPECT_EQ(LN_RandomIntInRange(bits, 7, 7), 7);
    const float f = LN_RandomFloatInRange(bits, 7.0f, -2.0f);
    ASSERT_GE(f, -2.0f);
    ASSERT_LT(f, 7.0f);
    EXPECT_EQ(LN_RandomFloatInRange(bits, 1.5f, 1.5f), 1.5f);
    EXPECT_FALSE(LN_RandomChance(bits, 0.0f));
    EXPECT_TRUE(LN_RandomChance(bits, 1.0f));
    chances += LN_RandomChance(bits, 0.25f) ? 1 : 0;
    const size_t place = LN_RandomIndex(bits, 7);
    ASSERT_LT(place, 7u);
    places.insert(place);
  }
  EXPECT_EQ(small, (std::set<int32_t>{3, 4, 5})) << "both ends come up";
  EXPECT_TRUE(negative && positive) << "the whole int32 range, no overflow";
  EXPECT_NEAR(double(chances) / 20000.0, 0.25, 0.02);
  EXPECT_EQ(places.size(), 7u);
}

/* Every type stays in its range for many objects: Integer from Min to Max (both come up), Vector
 * each axis between its ends (an axis with equal ends is that value), Boolean about as often as
 * its Probability. */
TEST(LN_Random, EveryTypeStaysInItsRange)
{
  SeedScope seed(0);
  constexpr uint32_t kOwners = 400;
  for (const SemMode &mode : {kProduction, kMemoOff}) {
    SCOPED_TRACE(mode.name);
    {
      SemRig rig(RandomTree(kFloat, kPerObject), mode, kOwners);
      rig.Step();
      for (uint32_t owner = 0; owner < kOwners; owner++) {
        const float r = rig.Float(rig.Object(owner), "r");
        ASSERT_GE(r, -2.0f);
        ASSERT_LT(r, 7.0f);
      }
    }
    {
      SemRig rig(RandomTree(kInteger, kPerObject), mode, kOwners);
      rig.Step();
      std::set<int> seen;
      for (uint32_t owner = 0; owner < kOwners; owner++) {
        const int r = int(rig.Float(rig.Object(owner), "r"));
        ASSERT_GE(r, 3);
        ASSERT_LE(r, 9);
        seen.insert(r);
      }
      EXPECT_EQ(seen.size(), 7u) << "3 to 9, both ends included";
    }
    {
      SemRig rig(RandomTree(kVector, kPerObject), mode, kOwners);
      rig.Step();
      for (uint32_t owner = 0; owner < kOwners; owner++) {
        KX_GameObject &object = rig.Object(owner);
        ASSERT_GE(rig.Float(object, "rx"), -1.0f);
        ASSERT_LT(rig.Float(object, "rx"), 1.0f);
        ASSERT_EQ(rig.Float(object, "ry"), 4.0f) << "equal ends: that value";
        ASSERT_GE(rig.Float(object, "rz"), 2.0f);
        ASSERT_LT(rig.Float(object, "rz"), 3.0f);
      }
    }
    {
      SemRig rig(RandomTree(kBoolean, kPerObject), mode, kOwners);
      int trues = 0;
      for (int step = 0; step < 5; step++) {
        rig.Step();
        for (uint32_t owner = 0; owner < kOwners; owner++) {
          trues += rig.Float(rig.Object(owner), "r") != 0.0f ? 1 : 0;
        }
      }
      EXPECT_NEAR(double(trues) / double(kOwners * 5), 0.25, 0.05) << "Probability 0.25";
    }
  }
}

/* Per Object: every object its own number, also when all trees are the same program; Same for
 * All: one number for all of them. Both give a new number every step. */
TEST(LN_Random, EachObjectGetsItsOwnNumbersUnlessSameForAll)
{
  SeedScope seed(0);
  constexpr uint32_t kOwners = 50;
  for (const SemMode &mode : {kProduction, kMemoOff}) {
    SCOPED_TRACE(mode.name);
    for (const int16_t type : {kFloat, kInteger, kVector, kBoolean}) {
      SCOPED_TRACE("type " + std::to_string(type));
      SemRig own(RandomTree(type, kPerObject), mode, kOwners);
      SemRig shared(RandomTree(type, kSameForAll), mode, kOwners);
      std::vector<std::string> previous;
      for (int step = 0; step < 4; step++) {
        own.Step();
        shared.Step();
        const std::vector<std::string> mine = Values(own, kOwners, type == kVector);
        const std::vector<std::string> all = Values(shared, kOwners, type == kVector);
        const std::set<std::string> distinct(mine.begin(), mine.end());
        /* (Integers 3..9 and booleans have few values: many owners share one, never all.) */
        EXPECT_GT(distinct.size(), type == kBoolean ? 1u : (type == kInteger ? 5u : 45u));
        EXPECT_EQ(std::set<std::string>(all.begin(), all.end()).size(), 1u) << "Same for All";
        if (type == kFloat || type == kVector) {
          EXPECT_NE(mine, previous) << "a new number every step";
        }
        previous = mine;
      }
    }
  }
}

/* The Seed input gives other numbers; the game's seed gives other numbers; the same seeds give the
 * same numbers again (the run is repeated with new rigs). */
TEST(LN_Random, TheSeedsChangeTheNumbersAndGiveThemAgain)
{
  constexpr uint32_t kOwners = 8;
  const auto run = [&](const uint32_t game_seed, const int seed_input) {
    SeedScope seed(game_seed);
    SemRig rig(RandomTree(kFloat, kPerObject, true), kProduction, kOwners);
    for (uint32_t owner = 0; owner < kOwners; owner++) {
      rig.SetInt(rig.Object(owner), "seed", seed_input);
    }
    std::vector<std::string> out;
    for (int step = 0; step < 3; step++) {
      rig.Step();
      for (const std::string &value : Values(rig, kOwners, false)) {
        out.push_back(value);
      }
    }
    return out;
  };
  const std::vector<std::string> base = run(1234, 0);
  EXPECT_EQ(run(1234, 0), base) << "the same seeds: the same numbers";
  EXPECT_NE(run(1234, 1), base) << "the Seed input";
  EXPECT_NE(run(99, 0), base) << "the game's seed";
  /* Every number differs, not just one. */
  const std::vector<std::string> other = run(99, 0);
  for (size_t index = 0; index < base.size(); index++) {
    EXPECT_NE(other[index], base[index]) << index;
  }
}

/* A step's numbers do not depend on how many frames came between the steps (a computer at 30 or
 * 144 fps plays the same game). */
TEST(LN_Random, StepNumbersDoNotDependOnTheFrameRate)
{
  SeedScope seed(77);
  SemRig steps_only(RandomTree(kFloat, kPerObject), kProduction, 4);
  SemRig with_frames(RandomTree(kFloat, kPerObject), kProduction, 4);
  for (int step = 0; step < 6; step++) {
    steps_only.Step();
    for (int frame = 0; frame <= step % 3; frame++) {
      with_frames.Update(); /* 1 to 3 frames between the steps */
    }
    with_frames.Step();
    EXPECT_EQ(Values(steps_only, 4, false), Values(with_frames, 4, false)) << "step " << step;
  }
}

/* A random node is known by its tree's and its own name: other nodes added to the tree (which
 * move its expression index) leave its numbers as they were. */
TEST(LN_Random, ANodeKeepsItsNumbersWhenOtherNodesAreAdded)
{
  SeedScope seed(5);
  SemRig alone(RandomTree(kFloat, kPerObject, false, "Dice"), kProduction, 6);
  SemRig crowded(RandomTree(kFloat, kPerObject, false, "Dice", 3), kProduction, 6);
  for (int step = 0; step < 3; step++) {
    alone.Step();
    crowded.Step();
    EXPECT_EQ(Values(alone, 6, false), Values(crowded, 6, false)) << "step " << step;
  }
}

/* A Loop's Index as the Seed: every iteration its own value (the memo must not keep the first
 * iteration's), the same values again in the next run. */
TEST(LN_Random, ALoopsIndexAsTheSeedGivesEveryIterationItsOwnValue)
{
  SeedScope seed(3);
  TreeBuilder b;
  GenNode &event = b.Add("LogicNativeOnFixedUpdate");
  GenNode &loop = b.Add("LogicNativeLoop");
  b.Int(loop, "Count", 6);
  b.Link(event, "Out", loop, "Flow");
  GenNode &random = b.Add("LogicNativeRandomValue", kInteger, kPerObject);
  b.Int(random, "Min_Int", 0);
  b.Int(random, "Max_Int", 1000000);
  b.Link(loop, "Index", random, "Seed");
  GenNode &print = b.Add("LogicNativePrint");
  b.Link(loop, "Loop", print, "Flow");
  b.Link(random, "Value_Int", print, "Message");
  const std::shared_ptr<LN_Program> program = CompileOrFail(b);
  ASSERT_NE(program, nullptr);
  std::string first_run;
  for (const SemMode &mode : {kProduction, kMemoOn, kMemoOff}) {
    SCOPED_TRACE(mode.name);
    SemRig rig(program, mode, 1);
    rig.Step();
    std::set<std::string> lines;
    size_t start = 0;
    const std::string &printed = rig.Printed();
    while (start < printed.size()) {
      const size_t end = printed.find('\n', start);
      lines.insert(printed.substr(start, end - start));
      start = end == std::string::npos ? printed.size() : end + 1;
    }
    EXPECT_EQ(lines.size(), 6u) << printed;
    if (first_run.empty()) {
      first_run = printed;
    }
    EXPECT_EQ(printed, first_run) << "every mode gives the same values";
  }
}

/* On Init counts steps (2026-11-07): a tree starting after 3 steps (a copy added in a step) rolls
 * the same On Init number whether its first tick is a frame or a step and however many frames ran
 * before, so a copy's first numbers do not follow the frame rate. The frame route of that first
 * tick reads the same Random Value with the frame's own count: the tick's memo must not hand it
 * On Init's number (the same with the memo off). */
TEST(LN_Random, OnInitNumbersCountStepsWhateverTheFirstTick)
{
  SeedScope seed(11);
  TreeBuilder b;
  GenNode &init = b.Add("LogicNativeOnInit");
  GenNode &frame = b.Add("LogicNativeOnUpdate");
  GenNode &random = b.Add("LogicNativeRandomValue", kFloat, kPerObject);
  b.Float(random, "Min", 0.0f);
  b.Float(random, "Max", 1000.0f);
  Writers on_init{b, &init};
  on_init.Write("LogicNativeSetGamePropertyFloat", "r", random, "Value");
  Writers on_frame{b, &frame};
  on_frame.Write("LogicNativeSetGamePropertyFloat", "f", random, "Value");
  const std::shared_ptr<LN_Program> program = CompileOrFail(b);
  ASSERT_NE(program, nullptr);

  struct Copy {
    std::string r;
    std::string f;
  };
  const auto run = [&](const SemMode &mode, const int frames_between, const bool frame_first) {
    SemRig rig(program, mode, 1);
    for (int step = 0; step < 3; step++) {
      for (int frame_index = 0; frame_index < frames_between; frame_index++) {
        rig.Update();
      }
      rig.Step();
    }
    KX_GameObject &copy = rig.AddObject("Copy", MT_Vector3(0.0f, 0.0f, 0.0f));
    rig.AddTreeOf(program, 1, 1, 0);
    if (frame_first) {
      rig.Update();
    }
    else {
      rig.Step();
    }
    return Copy{Bits(rig.Float(copy, "r")),
                rig.Has(copy, "f") ? Bits(rig.Float(copy, "f")) : std::string("none")};
  };

  const Copy step_first = run(kProduction, 0, false);
  for (const SemMode &mode : AllModes()) {
    for (const int frames_between : {0, 1, 2}) {
      const Copy frame_first = run(mode, frames_between, true);
      EXPECT_EQ(frame_first.r, step_first.r)
          << mode.name << ", " << frames_between << " frames a step";
      if (frames_between == 2) {
        /* 6 frames and 3 steps before the copy's first tick: the frame route has a number of its
         * own (with 1 frame a step both counts are 3, the same number). */
        EXPECT_NE(frame_first.f, frame_first.r) << mode.name;
      }
      EXPECT_EQ(frame_first.f, run(kMemoOff, frames_between, true).f)
          << mode.name << ", " << frames_between << " frames a step";
    }
  }
  EXPECT_EQ(run(kMemoOff, 0, false).r, step_first.r);
}

/* Set Random Seed: the new seed takes effect after the tick it was set in (the trees of that tick
 * all used the old one), the same as a game started with that seed from then on; its Seed output
 * reads the seed now. */
TEST(LN_Random, SetRandomSeedTakesEffectAfterItsTick)
{
  constexpr uint32_t kOwners = 5;
  /* Step 0: Set Random Seed 4242 (On Init would run before the first step too: a step tree that
   * sets it on its first step only, through Once). */
  TreeBuilder b;
  GenNode &event = b.Add("LogicNativeOnFixedUpdate");
  GenNode &once = b.Add("LogicNativeOnce");
  b.Link(event, "Out", once, "Flow");
  GenNode &set = b.Add("LogicNativeSetRandomSeed", 0, 0);
  b.Int(set, "Seed", 4242);
  b.Link(once, "Out", set, "Flow");
  GenNode &random = b.Add("LogicNativeRandomValue", kFloat, kPerObject);
  std::strncpy(random.node.name, "Dice", sizeof(random.node.name) - 1);
  Writers w{b, &event};
  w.Write("LogicNativeSetGamePropertyFloat", "r", random, "Value");
  w.Write("LogicNativeSetGamePropertyInt", "seed_now", set, "Seed");
  const std::shared_ptr<LN_Program> setter = CompileOrFail(b);
  ASSERT_NE(setter, nullptr);

  SeedScope seed(10);
  SemRig rig(setter, kProduction, kOwners);
  /* The same tree's numbers in a game that started with 4242 (Dice is the same node). */
  TreeBuilder c;
  GenNode &event_c = c.Add("LogicNativeOnFixedUpdate");
  GenNode &random_c = c.Add("LogicNativeRandomValue", kFloat, kPerObject);
  std::strncpy(random_c.node.name, "Dice", sizeof(random_c.node.name) - 1);
  Writers wc{c, &event_c};
  wc.Write("LogicNativeSetGamePropertyFloat", "r", random_c, "Value");
  const std::shared_ptr<LN_Program> reader = CompileOrFail(c);
  ASSERT_NE(reader, nullptr);

  rig.Step();
  EXPECT_EQ(LN_GetRandomGameSeed(), 4242u) << "applied after the tick";
  EXPECT_EQ(int(rig.Float(rig.Object(0), "seed_now")), 10) << "the tick read the old seed";
  const std::vector<std::string> first = Values(rig, kOwners, false);
  rig.Step();
  EXPECT_EQ(int(rig.Float(rig.Object(0), "seed_now")), 4242);
  const std::vector<std::string> second = Values(rig, kOwners, false);

  {
    SeedScope old_seed(10);
    SemRig plain(reader, kProduction, kOwners);
    plain.Step();
    EXPECT_EQ(Values(plain, kOwners, false), first) << "step 0 still rolled from seed 10";
  }
  {
    SeedScope new_seed(4242);
    SemRig plain(reader, kProduction, kOwners);
    plain.Step();
    plain.Step();
    EXPECT_EQ(Values(plain, kOwners, false), second) << "step 1 rolled from 4242";
  }
}
