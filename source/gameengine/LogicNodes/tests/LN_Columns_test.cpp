/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Plan C1 prototype (LN_Columns.h): trees run in columns end exactly as on the lean VM. Every test
 * runs the same program on the same objects twice, once on the lean VM and once in columns, and
 * compares the whole world after every tick (every pose bit, every game property, the world write
 * stamps). */

#include "testing/testing.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "LN_Columns.h"
#include "LN_LeanProgram.h"
#include "LN_SemRig.h"
#include "LN_StrictDump.h"

using namespace ln_test;
using namespace ln_test::sem;

namespace {

struct ColumnScope {
  explicit ColumnScope(const int mode)
  {
    LN_SetColumnModeForTests(mode);
  }
  ~ColumnScope()
  {
    LN_SetColumnModeForTests(-1);
  }
};

/** Direct programs (plan C2 P5) in a mode for a test (-1 at its end: the run's own setting). */
struct DirectScope {
  explicit DirectScope(const int mode)
  {
    LN_SetColumnDirectModeForTests(mode);
  }
  ~DirectScope()
  {
    LN_SetColumnDirectModeForTests(-1);
  }
};

/** The bridge on (UPBGE_LN_COLUMN_BRIDGE, read when a program is lowered) for a test of it, the
 * run's own setting back afterwards: a suite run with the bridge off still tests the bridge. */
struct BridgeOn {
  /** `on` false: the bridge off (a native read of what the bridge would read). */
  explicit BridgeOn(const bool on = true)
  {
    const char *outside = std::getenv("UPBGE_LN_COLUMN_BRIDGE");
    had = outside != nullptr;
    kept = had ? outside : "";
    setenv("UPBGE_LN_COLUMN_BRIDGE", on ? "1" : "0", 1);
  }
  ~BridgeOn()
  {
    if (had) {
      setenv("UPBGE_LN_COLUMN_BRIDGE", kept.c_str(), 1);
    }
    else {
      unsetenv("UPBGE_LN_COLUMN_BRIDGE");
    }
  }
  bool had = false;
  std::string kept;
};

std::string WorldOf(SemRig &rig)
{
  std::vector<KX_GameObject *> objects;
  for (size_t index = 0; index < rig.ObjectCount(); index++) {
    objects.push_back(&rig.Object(index));
  }
  std::string world = DumpWorld(objects);
  /* What Print printed (the bridge records it: its order is the flush's). */
  world += "printed:" + rig.Printed() + "\n";
  /* Forces and torques (plan C2 P5: the rig has no physics to show them). */
  world += "forces:" + rig.Forces() + "\n";
  /* What Set Visible, Set Object Color and a Teleport change (the reset is taken: both rigs). */
  for (KX_GameObject *object : objects) {
    const MT_Vector4 &color = object->GetObjectColor();
    world += "visible=" + std::to_string(object->GetVisible() ? 1 : 0) + " color=" +
             LN_DumpFloatBits(color.x()) + "," + LN_DumpFloatBits(color.y()) + "," +
             LN_DumpFloatBits(color.z()) + "," + LN_DumpFloatBits(color.w()) +
             " teleport=" + std::to_string(object->TakeSmoothMotionReset().active ? 1 : 0) + "\n";
  }
  return world;
}

/** Owners near and far (both branches of a brain), with properties of their own; one in seven
 * misses some (a write then makes them), and a "Target" others measure. */
void Spread(SemRig &rig, const uint32_t owners)
{
  for (uint32_t index = 0; index < owners; index++) {
    KX_GameObject &object = rig.Object(index);
    const float angle = float(index) * 0.37f;
    const float radius = 2.0f + float(index % 9);
    object.NodeSetWorldPosition(MT_Vector3(radius * std::cos(angle), radius * std::sin(angle), 0.0f));
    object.NodeUpdateGS(0.0);
    for (int chain = 0; chain < 15; chain++) {
      rig.SetFloat(object, ("t" + std::to_string(chain)).c_str(), float(index % 7) + chain);
    }
    rig.SetFloat(object, "dist", 20.0f);
    if (index % 7 != 3) {
      rig.SetFloat(object, "pf0", float(index % 11) * 0.5f - 2.0f);
      rig.SetFloat(object, "pf1", float(index % 5) - 1.0f);
      rig.SetInt(object, "pi0", int(index % 13) - 6);
      rig.SetBool(object, "pb0", index % 2 == 0);
    }
    rig.SetFloat(object, "pf2", 1.5f);
  }
}

/** Plan C2 P5: UPBGE_LN_COLUMN_DIRECT=force runs every program as a direct program, so the checks
 * of how runs formed (waves, fallbacks to the old system, split runs, lane counts) do not hold;
 * the worlds are still compared with the old system's. */
bool DirectForced()
{
  return LN_GetColumnDirectMode() == LN_ColumnDirectMode::Force;
}

/** Plan C2 fix 1: UPBGE_LN_COLUMN_SPLIT_PUSHED=0 gives a pushed lane's whole run back (no split
 * to count; the worlds are still compared). */
bool PushedSplitsOff()
{
  const char *value = std::getenv("UPBGE_LN_COLUMN_SPLIT_PUSHED");
  return value != nullptr && (std::strcmp(value, "0") == 0 || std::strcmp(value, "off") == 0);
}

/** `program` on `owners` objects for `ticks` ticks of `event`, on the lean VM and in columns: the
 * worlds must be the same after every tick. Returns the trees run in columns. */
uint64_t ExpectColumnsMatch(const std::shared_ptr<LN_Program> &program,
                            const LN_Event event,
                            const uint32_t owners,
                            const int ticks,
                            const std::string &what,
                            const uint32_t helpers = 0,
                            std::string *r_refusal = nullptr,
                            const std::function<void(SemRig &)> &extra = {},
                            const uint32_t stride = 1)
{
  ColumnScope scope(1);
  /* Every comparison checks the flush's write counts too (plan C1-11 C-1). */
  LN_SetColumnCountVerifyForTests(1);
  const uint64_t mismatches = LN_ColumnCountMismatchCount();
  struct VerifyOff {
    ~VerifyOff()
    {
      LN_SetColumnCountVerifyForTests(-1);
    }
  } verify_off;
  SemRig lean(program, kMemoOn, owners, stride);
  SemRig cols(program, kMemoOn, owners, stride);
  lean.AddObject("Target", MT_Vector3(3.0f, 1.0f, 0.5f));
  cols.AddObject("Target", MT_Vector3(3.0f, 1.0f, 0.5f));
  Spread(lean, owners);
  Spread(cols, owners);
  if (extra) {
    extra(lean);
    extra(cols);
  }
  lean.PublishEachTick(true);
  cols.PublishEachTick(true);
  lean.SetDump(false);
  cols.SetDump(false);
  cols.SetColumns(true, helpers, 1);
  uint64_t lanes = 0;
  for (int tick = 0; tick < ticks; tick++) {
    lean.Tick(event);
    cols.Tick(event);
    /* (Plan C2 P6: forced direct, the column passes are the trees the column system ran.) */
    lanes += cols.LastColumnLanes() + (DirectForced() ? cols.LastColumnPasses() : 0);
    if (r_refusal != nullptr && cols.LastColumnLanes() == 0 && tick > 0) {
      *r_refusal = cols.LastColumnRefusal();
    }
    std::string a = WorldOf(lean);
    std::string b = WorldOf(cols);
    /* The trees' states too (meaning, never bookkeeping): flow states, tree properties, gates
     * (the first 300: every lane's state is made by the same kernels). */
    for (size_t index = 0; index < lean.TreeCount() && index < 300; index++) {
      a += lean.Tree(index).DumpStateForTests(uint64_t(tick)).meaning;
      b += cols.Tree(index).DumpStateForTests(uint64_t(tick)).meaning;
    }
    if (a != b) {
      ADD_FAILURE() << what << ", tick " << tick << ": " << diff::FirstDifference(a, b);
      return lanes;
    }
    if (LN_ColumnCountMismatchCount() != mismatches) {
      ADD_FAILURE() << what << ", tick " << tick << ": the flush's write counts differ";
      return lanes;
    }
  }
  return lanes;
}

}  // namespace

TEST(LN_Columns, ModeIsOnUnlessSwitchedOff)
{
  {
    ColumnScope scope(0);
    EXPECT_EQ(LN_GetColumnMode(), LN_ColumnMode::Off);
  }
  {
    ColumnScope scope(1);
    EXPECT_EQ(LN_GetColumnMode(), LN_ColumnMode::On);
  }
  {
    ColumnScope scope(2);
    EXPECT_EQ(LN_GetColumnMode(), LN_ColumnMode::All);
  }
  const char *value = std::getenv("UPBGE_LN_COLUMNS");
  if (value == nullptr) {
    EXPECT_EQ(LN_GetColumnMode(), LN_ColumnMode::On);
  }
}

/* Plan C2 P0: the All mode runs a run shorter than the shortest run (3 trees of one program,
 * shortest run 16) in columns, the On mode leaves it to the old system; both end as the old
 * system. */
TEST(LN_Columns, TheAllModeRunsShortRunsInColumns)
{
  if (DirectForced()) {
    GTEST_SKIP() << "every program a direct program: no run to plan";
  }
  BrainOfOptions options;
  options.step_every = 1;
  const std::shared_ptr<LN_Program> program = BrainOf(options);
  ASSERT_NE(program, nullptr);
  constexpr uint32_t kOwners = 3;
  for (const int mode : {1, 2}) {
    ColumnScope scope(mode);
    SemRig lean(program, kMemoOn, kOwners);
    SemRig cols(program, kMemoOn, kOwners);
    Spread(lean, kOwners);
    Spread(cols, kOwners);
    lean.SetDump(false);
    cols.SetDump(false);
    cols.SetColumns(true, 0, 16);
    uint64_t lanes = 0;
    for (int tick = 0; tick < 6; tick++) {
      lean.Tick(LN_Event::OnFixedUpdate);
      cols.Tick(LN_Event::OnFixedUpdate);
      /* (Plan C2 P6: forced direct, the column passes are the trees the column system ran.) */
    lanes += cols.LastColumnLanes() + (DirectForced() ? cols.LastColumnPasses() : 0);
      EXPECT_EQ(WorldOf(lean), WorldOf(cols)) << "mode " << mode << ", tick " << tick;
    }
    EXPECT_EQ(lanes != 0, mode == 2) << "mode " << mode;
  }
}

/* The research brain lowers to columns in all its forms (On Update, on steps, measuring or
 * reading its distance, 0 to 15 chains, the look at a named object is refused: Get Object
 * Attribute is no column node). */
TEST(LN_Columns, TheBrainLowersToColumns)
{
  for (const int step : {0, 1}) {
    for (const bool measure : {true, false}) {
      for (const int chains : {0, 4, 15}) {
        BrainOfOptions options;
        options.step_every = step;
        options.measure_distance = measure;
        options.chains = chains;
        const std::shared_ptr<LN_Program> program = BrainOf(options);
        ASSERT_NE(program, nullptr);
        std::string refusal;
        const LN_Event event = step > 0 ? LN_Event::OnFixedUpdate : LN_Event::OnUpdate;
        const std::shared_ptr<const LN_ColumnProgram> columns = LN_CompileColumnProgram(
            *program, event, &refusal);
        EXPECT_NE(columns, nullptr) << "step " << step << " measure " << measure << " chains "
                                    << chains << ": " << refusal;
      }
    }
  }
  /* Reading the Player's visibility has no kernel: the bridge runs the VM's code for it (plan
   * C1-8); with the bridge off the program stays on the VM. */
  BrainOfOptions look;
  look.look = true;
  std::string refusal;
  /* The run's own UPBGE_LN_COLUMN_BRIDGE is set back afterwards. */
  const char *outside = std::getenv("UPBGE_LN_COLUMN_BRIDGE");
  const std::string kept = outside != nullptr ? outside : "";
  setenv("UPBGE_LN_COLUMN_BRIDGE", "1", 1);
  EXPECT_NE(LN_CompileColumnProgram(*BrainOf(look), LN_Event::OnUpdate, &refusal), nullptr)
      << refusal;
  setenv("UPBGE_LN_COLUMN_BRIDGE", "0", 1);
  EXPECT_EQ(LN_CompileColumnProgram(*BrainOf(look), LN_Event::OnUpdate, &refusal), nullptr);
  if (outside != nullptr) {
    setenv("UPBGE_LN_COLUMN_BRIDGE", kept.c_str(), 1);
  }
  else {
    unsetenv("UPBGE_LN_COLUMN_BRIDGE");
  }
  EXPECT_FALSE(refusal.empty());
}

/* The research brain in columns ends exactly as on the lean VM, tick after tick, NPCs walking and
 * turning across the threshold: On Update and on steps, measured and read distances, 0 to 15
 * chains, one thread and chunks on helper threads. */
TEST(LN_Columns, TheBrainInColumnsEndsAsOnTheLeanVM)
{
  uint64_t lanes = 0;
  for (const int step : {0, 1}) {
    for (const bool measure : {true, false}) {
      for (const int chains : {0, 4, 15}) {
        BrainOfOptions options;
        options.step_every = step;
        options.measure_distance = measure;
        options.chains = chains;
        options.threshold = 5.0f;
        const std::shared_ptr<LN_Program> program = BrainOf(options);
        ASSERT_NE(program, nullptr);
        const LN_Event event = step > 0 ? LN_Event::OnFixedUpdate : LN_Event::OnUpdate;
        const std::string what = "brain step " + std::to_string(step) + " measure " +
                                 std::to_string(measure) + " chains " + std::to_string(chains);
        lanes += ExpectColumnsMatch(program, event, 300, 30, what);
        lanes += ExpectColumnsMatch(program, event, 700, 8, what + " (3 helpers)", 3);
      }
    }
  }
  EXPECT_GT(lanes, 0u) << "no tree ran in columns";
}

namespace {

/** A random tree of the column nodes (and, `odd` > 0, now and then a node columns refuse). */
class RandomColumnTree {
 public:
  RandomColumnTree(const uint32_t seed, const bool on_steps) : m_rng(seed)
  {
    /* A tree that moves its object measures no distance from it (batch safety refuses a distance
     * from the object read after the tree moves it): its distances start at a position. */
    m_moves = seed % 3 != 0;
    m_event = &m_b.Add(on_steps ? "LogicNativeOnFixedUpdate" : "LogicNativeOnUpdate");
    const int chains = 1 + Pick(3);
    for (int chain = 0; chain < chains; chain++) {
      Chain(*m_event, "Out", 0);
    }
  }
  std::shared_ptr<LN_Program> Compile()
  {
    std::string report;
    return m_b.Compile(&report);
  }

 private:
  struct Source {
    GenNode *node = nullptr;
    const char *pin = nullptr;
  };
  int Pick(const int count)
  {
    return int(m_rng() % uint32_t(count));
  }
  float Value(const float low, const float high)
  {
    return low + (high - low) * float(m_rng() % 10000) / 10000.0f;
  }
  const char *FloatProperty()
  {
    static const char *names[] = {"pf0", "pf1", "pf2", "missing_f"};
    return names[Pick(4)];
  }
  Source FloatSource(const int depth)
  {
    const int kind = depth > 2 ? Pick(2) : Pick(5);
    switch (kind) {
      case 0: {
        GenNode &read = m_b.Add("LogicNativeGetGamePropertyFloat");
        m_b.String(read, "Property", FloatProperty());
        return {&read, "Value"};
      }
      case 1: {
        /* End A the owner (trees without moves), a position, or the named Target; end B a
         * position or the Target. A position to an object stays on the VM (the VM reads B only
         * when A is valid), so a tree that moves measures from the Target instead. */
        int16_t mode_a = m_moves ? 1 : 0;
        int16_t mode_b = int16_t(Pick(2));
        bool target_a = false;
        if (mode_a == 1 && mode_b == 0) {
          mode_a = 0;
          mode_b = 1;
          target_a = true;
        }
        GenNode &distance = m_b.Add("LogicNativeGetDistance", mode_a, mode_b);
        m_b.Vector(distance, "Position A", Value(-3, 3), Value(-3, 3), 0.0f);
        if (target_a) {
          m_b.ObjectNamed(distance, "Object A", &diff::TargetBlenderObject());
        }
        if (mode_b == 0 && Pick(2) == 0) {
          m_b.ObjectNamed(distance, "Object B", &diff::TargetBlenderObject());
        }
        m_b.Vector(distance, "Position B", Value(-3, 3), Value(-3, 3), 0.0f);
        return {&distance, Pick(3) == 0 ? "Squared Distance" : "Distance"};
      }
      default: {
        static const int16_t ops[] = {int16_t(blender::NODE_MATH_ADD),
                                      int16_t(blender::NODE_MATH_SUBTRACT),
                                      int16_t(blender::NODE_MATH_MULTIPLY),
                                      int16_t(blender::NODE_MATH_DIVIDE),
                                      int16_t(blender::NODE_MATH_MINIMUM),
                                      int16_t(blender::NODE_MATH_MAXIMUM),
                                      int16_t(blender::NODE_MATH_ABSOLUTE)};
        GenNode &math = m_b.Add("LogicNativeMath", ops[Pick(7)]);
        const Source a = FloatSource(depth + 1);
        m_b.Link(*a.node, a.pin, math, "A");
        if (Pick(2) == 0) {
          const Source b = FloatSource(depth + 1);
          m_b.Link(*b.node, b.pin, math, "B");
        }
        else {
          m_b.Float(math, "B", Value(-2, 2));
        }
        return {&math, "Result"};
      }
    }
  }
  Source BoolSource()
  {
    if (Pick(4) == 0) {
      GenNode &read = m_b.Add("LogicNativeGetGamePropertyBool");
      m_b.String(read, "Property", Pick(3) == 0 ? "missing_b" : "pb0");
      return {&read, "Value"};
    }
    GenNode &compare = m_b.Add("LogicNativeCompare", int16_t(Pick(6)));
    const Source a = FloatSource(1);
    m_b.Link(*a.node, a.pin, compare, "A");
    if (Pick(2) == 0) {
      const Source b = FloatSource(1);
      m_b.Link(*b.node, b.pin, compare, "B");
    }
    else {
      m_b.Float(compare, "B", Value(-3, 8));
    }
    return {&compare, "Result"};
  }
  /** A statement after `from`'s `out`; returns the node whose Done the next one follows. */
  GenNode *Statement(GenNode &from, const char *out, const int depth)
  {
    int kind = depth >= 2 ? Pick(6) : Pick(7);
    if (!m_moves && (kind == 4 || kind == 5)) {
      kind = Pick(4); /* a property write instead of a move */
    }
    GenNode *node = nullptr;
    switch (kind) {
      case 0: {
        node = &m_b.Add("LogicNativeSetGamePropertyFloat");
        m_b.String(*node, "Property", FloatProperty());
        const Source value = FloatSource(0);
        m_b.Link(*value.node, value.pin, *node, "Value");
        break;
      }
      case 1: {
        node = &m_b.Add("LogicNativeSetGamePropertyInt");
        m_b.String(*node, "Property", Pick(4) == 0 ? "missing_i" : "pi0");
        if (Pick(2) == 0) {
          GenNode &read = m_b.Add("LogicNativeGetGamePropertyInt");
          m_b.String(read, "Property", "pi0");
          m_b.Link(read, "Value", *node, "Value");
        }
        else {
          m_b.Int(*node, "Value", Pick(9) - 4);
        }
        break;
      }
      case 2: {
        node = &m_b.Add("LogicNativeSetGamePropertyBool");
        m_b.String(*node, "Property", Pick(4) == 0 ? "missing_b" : "pb0");
        const Source value = BoolSource();
        m_b.Link(*value.node, value.pin, *node, "Value");
        break;
      }
      case 3: {
        static const int16_t ops[] = {int16_t(blender::NODE_MATH_ADD),
                                      int16_t(blender::NODE_MATH_SUBTRACT),
                                      int16_t(blender::NODE_MATH_MULTIPLY)};
        const bool clamped = Pick(2) == 0;
        node = &m_b.Add("LogicNativeModifyProperty", ops[Pick(3)], clamped ? 1 : 0);
        m_b.String(*node, "Property", FloatProperty());
        if (Pick(2) == 0) {
          const Source value = FloatSource(1);
          m_b.Link(*value.node, value.pin, *node, "Value");
        }
        else {
          m_b.Float(*node, "Value", Value(-1, 1.5f));
        }
        if (clamped) {
          m_b.Float(*node, "Min", Value(-5, 0));
          m_b.Float(*node, "Max", Value(0, 9));
        }
        break;
      }
      case 4: {
        node = &m_b.Add("LogicNativeApplyMovement");
        if (Pick(2) == 0) {
          GenNode &combine = m_b.Add("LogicNativeCombineXYZ");
          const Source x = FloatSource(2);
          GenNode &scale = m_b.Add("LogicNativeMath", int16_t(blender::NODE_MATH_MULTIPLY));
          m_b.Link(*x.node, x.pin, scale, "A");
          m_b.Float(scale, "B", 0.001f);
          m_b.Link(scale, "Result", combine, "X");
          m_b.Float(combine, "Y", Value(-0.01f, 0.01f));
          m_b.Float(combine, "Z", 0.0f);
          m_b.Link(combine, "Vector", *node, "Vector");
        }
        else {
          m_b.Vector(*node, "Vector", Value(-0.01f, 0.01f), Value(-0.01f, 0.01f), Value(-0.002f, 0.002f));
        }
        m_b.Bool(*node, "Local", Pick(2) == 0);
        break;
      }
      case 5: {
        node = &m_b.Add("LogicNativeApplyRotation");
        m_b.Rotation(*node, "Rotation", Value(-0.02f, 0.02f), Value(-0.02f, 0.02f), Value(-0.05f, 0.05f));
        m_b.Bool(*node, "Local", Pick(2) == 0);
        break;
      }
      default: {
        node = &m_b.Add("LogicNativeBranch");
        const Source condition = BoolSource();
        m_b.Link(*condition.node, condition.pin, *node, "Condition");
        m_b.Link(from, out, *node, "Flow");
        if (Pick(4) != 0) {
          Chain(*node, "True", depth + 1);
        }
        if (Pick(4) != 0) {
          Chain(*node, "False", depth + 1);
        }
        return nullptr; /* a Branch ends its chain */
      }
    }
    m_b.Link(from, out, *node, "Flow");
    return node;
  }
  void Chain(GenNode &from, const char *out, const int depth)
  {
    GenNode *tail = &from;
    const char *tail_out = out;
    const int statements = 1 + Pick(4);
    for (int index = 0; index < statements && tail != nullptr; index++) {
      tail = Statement(*tail, tail_out, depth);
      tail_out = "Done";
    }
  }

  std::mt19937 m_rng;
  TreeBuilder m_b;
  GenNode *m_event = nullptr;
  bool m_moves = true;
};

}  // namespace

/* Random trees of every column node (property reads and writes of the three number types, missing
 * properties, Math, Compare, Branch nests, Modify Property with and without its clamp, Apply
 * Movement local and world from constants and Combine XYZ, Apply Rotation, Get Distance to a
 * position and to a named object, its squared distance), On Update and on steps: in columns they
 * end exactly as on the lean VM. */
TEST(LN_Columns, RandomTreesInColumnsEndAsOnTheLeanVM)
{
  const char *count_text = std::getenv("UPBGE_LN_COLUMNS_SEEDS");
  const uint32_t seeds = count_text != nullptr ? uint32_t(std::max(1, std::atoi(count_text))) : 300;
  uint32_t compiled = 0, lowered = 0;
  uint64_t lanes = 0;
  std::string refusals;
  for (uint32_t seed = 1; seed <= seeds; seed++) {
    const bool on_steps = seed % 2 == 0;
    RandomColumnTree tree(seed * 7919u, on_steps);
    const std::shared_ptr<LN_Program> program = tree.Compile();
    if (program == nullptr) {
      continue;
    }
    compiled++;
    const LN_Event event = on_steps ? LN_Event::OnFixedUpdate : LN_Event::OnUpdate;
    std::string refusal;
    if (LN_CompileColumnProgram(*program, event, &refusal) != nullptr) {
      lowered++;
    }
    else if (refusals.size() < 400) {
      refusals += "  seed " + std::to_string(seed) + ": " + refusal + "\n";
    }
    lanes += ExpectColumnsMatch(program, event, 40, 12, "seed " + std::to_string(seed));
    if (HasFailure()) {
      break;
    }
  }
  std::printf("COLUMNS random: %u compiled, %u lowered to columns, %llu lanes\n%s",
              compiled,
              lowered,
              (unsigned long long)lanes,
              refusals.c_str());
  EXPECT_GE(compiled, seeds / 2);
  /* The rest measure the named object on some routes only (refused on purpose). */
  EXPECT_GE(lowered, compiled / 2);
  EXPECT_GT(lanes, 0u);
}

namespace {

enum class Other { Push, Write, Measure, MeasureNamed, Place };

/** A tree of another kind next to the crowd, acting on a crowd NPC: found at runtime by the name
 * its object's "target" property holds (Push, Write, Measure), or named ("Target", MeasureNamed).
 * Push moves it (world), Write sets its "t0", the measures store their distance to it in "seen". */
std::shared_ptr<LN_Program> OtherTree(const Other kind, const bool on_steps)
{
  TreeBuilder b;
  GenNode &event = b.Add(on_steps ? "LogicNativeOnFixedUpdate" : "LogicNativeOnUpdate");
  GenNode *found = nullptr;
  if (kind != Other::MeasureNamed) {
    GenNode &name = b.Add("LogicNativeGetGamePropertyString");
    b.String(name, "Property", "target");
    found = &b.Add("LogicNativeObjectByName");
    b.Link(name, "Value", *found, "Name");
  }
  switch (kind) {
    case Other::Push: {
      GenNode &push = b.Add("LogicNativeApplyMovement");
      b.Link(*found, "Object", push, "Object");
      b.Vector(push, "Vector", 0.03f, -0.02f, 0.0f);
      b.Bool(push, "Local", false);
      b.Link(event, "Out", push, "Flow");
      break;
    }
    case Other::Place: {
      GenNode &place = b.Add("LogicNativeSetObjectAttribute", 0, 15);
      b.Link(*found, "Object", place, "Object");
      b.Vector(place, "Value", 4.0f, 4.0f, 1.0f);
      b.Link(event, "Out", place, "Flow");
      break;
    }
    case Other::Write: {
      GenNode &write = b.Add("LogicNativeSetGamePropertyFloat");
      b.Link(*found, "Object", write, "Object");
      b.String(write, "Property", "t0");
      b.Float(write, "Value", 42.5f);
      b.Link(event, "Out", write, "Flow");
      break;
    }
    case Other::Measure:
    case Other::MeasureNamed: {
      GenNode &distance = b.Add("LogicNativeGetDistance", 0, 0);
      if (found != nullptr) {
        b.Link(*found, "Object", distance, "Object B");
      }
      else {
        b.ObjectNamed(distance, "Object B", &diff::TargetBlenderObject());
      }
      GenNode &store = b.Add("LogicNativeSetGamePropertyFloat");
      b.String(store, "Property", "seen");
      b.Link(distance, "Distance", store, "Value");
      b.Link(event, "Out", store, "Flow");
      break;
    }
  }
  std::string report;
  std::shared_ptr<LN_Program> program = b.Compile(&report);
  EXPECT_NE(program, nullptr) << report;
  return program;
}

}  // namespace

/* Columns next to other trees that act on the crowd's NPCs (plan C1-1): trees recorded before the
 * crowd and after it, with sort keys between the NPCs', move NPCs found at runtime (their moves
 * meet the NPCs' own in the flush, by sort key), write a property the crowd writes too, and
 * measure NPCs, found at runtime and by name, after the crowd moved them (the reads apply the
 * NPCs' column moves early). On Update and on steps; the worlds stay identical tick by tick, and
 * the crowd runs in columns. */
TEST(LN_Columns, ColumnsNextToTreesThatActOnTheCrowdEndAsOnTheLeanVM)
{
  uint64_t lanes = 0;
  for (const bool on_steps : {false, true}) {
    BrainOfOptions options;
    options.step_every = on_steps ? 1 : 0;
    options.chains = 2;
    options.threshold = 5.0f;
    const std::shared_ptr<LN_Program> crowd = BrainOf(options);
    ASSERT_NE(crowd, nullptr);
    const std::shared_ptr<LN_Program> push = OtherTree(Other::Push, on_steps);
    const std::shared_ptr<LN_Program> write = OtherTree(Other::Write, on_steps);
    const std::shared_ptr<LN_Program> measure = OtherTree(Other::Measure, on_steps);
    const std::shared_ptr<LN_Program> measure_named = OtherTree(Other::MeasureNamed, on_steps);
    ASSERT_TRUE(push && write && measure && measure_named);
    constexpr uint32_t kOwners = 60;
    const auto extra = [&](SemRig &rig) {
      /* NPC 5 is the "Target" a tree names (the extra "Target" object is renamed out of the way). */
      rig.Object(kOwners).SetName("Spare");
      rig.Object(5).SetName("Target");
      struct Placement {
        std::shared_ptr<LN_Program> program;
        const char *target;
        uint32_t scene_index;
        bool first;
      };
      /* Crowd NPC i has scene index 2i; odd indexes fall between them. */
      const Placement placements[] = {
          {push, "Obj3", 7, false},     /* after the crowd, key just after NPC 3's */
          {push, "Obj20", 39, true},    /* before the crowd, key just before NPC 20's */
          {write, "Obj8", 15, true},    /* before, key before NPC 8's: NPC 8's own t0 write wins */
          {write, "Obj9", 19, false},   /* after, key after NPC 9's: this write wins */
          {measure, "Obj11", 23, false},
          {measure, "Obj3", 5, false},  /* NPC 3 is moved by its own run and the pusher */
          {measure_named, "", 1, false},
      };
      for (const Placement &placement : placements) {
        KX_GameObject &object = rig.AddObject("Other" + std::to_string(rig.ObjectCount()),
                                              MT_Vector3(1.0f, -2.0f, 0.0f));
        rig.SetString(object, "target", placement.target);
        rig.SetFloat(object, "seen", -1.0f);
        rig.AddTree(placement.program,
                    rig.ObjectCount() - 1,
                    placement.scene_index,
                    placement.first);
      }
    };
    lanes += ExpectColumnsMatch(crowd,
                                on_steps ? LN_Event::OnFixedUpdate : LN_Event::OnUpdate,
                                kOwners,
                                25,
                                on_steps ? "mixed, on steps" : "mixed, On Update",
                                0,
                                nullptr,
                                extra,
                                2);
  }
  EXPECT_GT(lanes, 0u) << "the crowd never ran in columns";
}

/* A live read of the owner's position (Get Object Attribute; Move To reads one too) after the
 * pass's first distance from the owner: that read applied the pending moves other trees made on
 * the NPC (pushed before its run), as the run applies them when it starts. Read before the
 * distance, the VM's live read sees them unapplied: since plan C2 fix 1 such a program starts
 * quietly (a pushed NPC records on the VM in its place, the NPCs around it run; many pushed NPCs
 * give the run back whole), so every NPC that runs reads its owner with nothing pending. */
TEST(LN_Columns, AnOwnPoseReadAfterADistanceFromTheOwnerSeesTheAppliedPushes)
{
  const std::shared_ptr<LN_Program> push = OtherTree(Other::Push, false);
  ASSERT_NE(push, nullptr);
  /* Pushers before NPCs 20 and 33 (keys just before theirs) and after NPC 3; `dense`: before
   * every second NPC of the first 40 (over a quarter of the run). */
  const auto pushers = [&](const bool dense) {
    return [&push, dense](SemRig &rig) {
      std::vector<std::pair<std::string, std::pair<uint32_t, bool>>> placements = {
          {"Obj20", {39, true}}, {"Obj33", {65, true}}, {"Obj3", {7, false}}};
      if (dense) {
        placements.clear();
        for (uint32_t npc = 0; npc < 40; npc += 2) {
          placements.push_back({"Obj" + std::to_string(npc), {npc * 2 + 1, true}});
        }
      }
      for (const auto &placement : placements) {
        KX_GameObject &object = rig.AddObject("Pusher" + std::to_string(rig.ObjectCount()),
                                              MT_Vector3(1.0f, -2.0f, 0.0f));
        rig.SetString(object, "target", placement.first.c_str());
        rig.AddTree(push, rig.ObjectCount() - 1, placement.second.first, placement.second.second);
      }
    };
  };
  {
    /* The distance first on one branch only, then the pose, then the distance again for every NPC:
     * where the branch is not taken, the pose is read before the pass's first distance. */
    TreeBuilder b;
    GenNode &event = b.Add("LogicNativeOnUpdate");
    GenNode &distance = b.Add("LogicNativeGetDistance", 0, 0);
    b.ObjectNamed(distance, "Object B", &diff::TargetBlenderObject());
    GenNode &flag = b.Add("LogicNativeGetGamePropertyBool");
    b.String(flag, "Property", "pb0");
    GenNode &sequence = b.Add("LogicNativeSequence", 0, 0, 2);
    b.Link(event, "Out", sequence, "Flow");
    GenNode &branch = b.Add("LogicNativeBranch");
    b.Link(flag, "Value", branch, "Condition");
    b.Link(sequence, "Then 0", branch, "Flow");
    GenNode &early = b.Add("LogicNativeSetGamePropertyFloat");
    b.String(early, "Property", "pf1");
    b.Link(distance, "Distance", early, "Value");
    b.Link(branch, "True", early, "Flow");
    GenNode &own = b.Add("LogicNativeGetObjectAttribute", 0);
    GenNode &split = b.Add("LogicNativeSeparateXYZ");
    b.Link(own, "Vector", split, "Vector");
    GenNode &pose = b.Add("LogicNativeSetGamePropertyFloat");
    b.String(pose, "Property", "pf3");
    b.Link(split, "X", pose, "Value");
    b.Link(sequence, "Then 1", pose, "Flow");
    GenNode &late = b.Add("LogicNativeSetGamePropertyFloat");
    b.String(late, "Property", "dist");
    b.Link(distance, "Distance", late, "Value");
    b.Link(pose, "Done", late, "Flow");
    std::string report;
    const std::shared_ptr<LN_Program> crowd = b.Compile(&report);
    ASSERT_NE(crowd, nullptr) << report;
    std::string refusal;
    ASSERT_NE(LN_CompileColumnProgram(*crowd, LN_Event::OnUpdate, &refusal), nullptr)
        << refusal << "\n" << LN_DumpProgramText(*crowd);
    for (const bool dense : {false, true}) {
      const uint64_t splits = LN_ColumnPushedSplitCount();
      EXPECT_GT(ExpectColumnsMatch(crowd, LN_Event::OnUpdate, 60, 25,
                                   dense ? "pose on some routes first, many pushed" :
                                           "pose on some routes first, pushed",
                                   0, nullptr, pushers(dense), 2),
                0u);
      if (!dense && !DirectForced() && !PushedSplitsOff()) {
        EXPECT_GT(LN_ColumnPushedSplitCount(), splits) << "a pushed NPC split no run";
      }
    }
  }
  for (const bool pose_first : {false, true}) {
    TreeBuilder b;
    GenNode &event = b.Add("LogicNativeOnUpdate");
    GenNode &distance = b.Add("LogicNativeGetDistance", 0, 0);
    b.ObjectNamed(distance, "Object B", &diff::TargetBlenderObject());
    GenNode &dist = b.Add("LogicNativeSetGamePropertyFloat");
    b.String(dist, "Property", "dist");
    b.Link(distance, "Distance", dist, "Value");
    GenNode &own = b.Add("LogicNativeGetObjectAttribute", 0);
    GenNode &split = b.Add("LogicNativeSeparateXYZ");
    b.Link(own, "Vector", split, "Vector");
    GenNode &pose = b.Add("LogicNativeSetGamePropertyFloat");
    b.String(pose, "Property", "pf3");
    b.Link(split, "X", pose, "Value");
    GenNode &move = b.Add("LogicNativeApplyMovement");
    b.Vector(move, "Vector", 0.1f, 0.05f, 0.0f);
    if (pose_first) {
      b.Link(event, "Out", pose, "Flow");
      b.Link(pose, "Done", dist, "Flow");
      b.Link(dist, "Done", move, "Flow");
    }
    else {
      b.Link(event, "Out", dist, "Flow");
      b.Link(dist, "Done", pose, "Flow");
      b.Link(pose, "Done", move, "Flow");
    }
    std::string report;
    const std::shared_ptr<LN_Program> crowd = b.Compile(&report);
    ASSERT_NE(crowd, nullptr) << report;
    std::string refusal;
    const bool lowers = LN_CompileColumnProgram(*crowd, LN_Event::OnUpdate, &refusal) != nullptr;
    ASSERT_TRUE(lowers) << refusal << "\n" << LN_DumpProgramText(*crowd);
    for (const bool dense : {false, true}) {
      EXPECT_GT(ExpectColumnsMatch(crowd, LN_Event::OnUpdate, 60, 25,
                                   std::string(pose_first ? "pose before distance" :
                                                            "pose after distance") +
                                       (dense ? ", many pushed" : ""),
                                   0, nullptr, pushers(dense), 2),
                0u);
    }
  }
}

/* A program with a node columns refuse stays on the lean VM (no lane); columns off plans
 * nothing. */
TEST(LN_Columns, RefusedProgramsRunAsDirectPrograms)
{
  TreeBuilder b;
  GenNode &event = b.Add("LogicNativeOnFixedUpdate");
  GenNode &sine = b.Add("LogicNativeMath", int16_t(blender::NODE_MATH_SINE));
  GenNode &read = b.Add("LogicNativeGetGamePropertyFloat");
  b.String(read, "Property", "pf0");
  b.Link(read, "Value", sine, "A");
  GenNode &set = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(set, "Property", "pf1");
  b.Link(sine, "Result", set, "Value");
  b.Link(event, "Out", set, "Flow");
  /* Set Cursor Visibility (the window's) has no column kernel and no bridge: the VM runs it. */
  GenNode &cursor = b.Add("LogicNativeSetCursorVisibility");
  b.Link(set, "Done", cursor, "Flow");
  std::string report;
  const std::shared_ptr<LN_Program> program = b.Compile(&report);
  ASSERT_NE(program, nullptr) << report;
  std::string refusal;
  EXPECT_EQ(LN_CompileColumnProgram(*program, LN_Event::OnFixedUpdate, &refusal), nullptr);
  EXPECT_NE(refusal.find("no column kernel"), std::string::npos) << refusal;
  {
    /* Direct programs off: the old system runs it. */
    DirectScope off(0);
    std::string tick_refusal;
    EXPECT_EQ(
        ExpectColumnsMatch(program, LN_Event::OnFixedUpdate, 50, 4, "cursor", 0, &tick_refusal),
        0u);
    EXPECT_FALSE(tick_refusal.empty());
  }
  {
    /* Plan C2 P5 / P6 (Auto): a direct program: no lane, its trees take the column system's pass
     * where the manager records them (On Init's tick too); the compiler's reason stays its
     * answer. */
    DirectScope as_set(1);
    std::string why;
    const std::shared_ptr<const LN_ColumnProgram> direct = LN_CompileColumnProgram(
        *program, LN_Event::OnFixedUpdate, &why, true);
    ASSERT_NE(direct, nullptr) << why;
    EXPECT_NE(why.find("no column kernel"), std::string::npos) << why;
    EXPECT_NE(LN_DescribeColumnProgram(*direct).find("direct"), std::string::npos);
    for (const uint32_t helpers : {0u, 3u}) {
      const uint64_t passes = LN_ColumnDirectLaneCount();
      EXPECT_EQ(ExpectColumnsMatch(program, LN_Event::OnFixedUpdate, 50, 4, "cursor direct", helpers),
                0u);
      EXPECT_EQ(LN_ColumnDirectLaneCount() - passes, 50u * 4u) << helpers << " helpers";
    }
  }
  /* A Loop needs the bridge (plan C1-9: its body runs through the VM's walk). */
  {
    TreeBuilder lb;
    GenNode &step = lb.Add("LogicNativeOnFixedUpdate");
    GenNode &loop = lb.Add("LogicNativeLoop");
    lb.Int(loop, "Count", 3);
    lb.Link(step, "Out", loop, "Flow");
    GenNode &body = lb.Add("LogicNativeModifyProperty");
    lb.String(body, "Property", "pf2");
    lb.Float(body, "Value", 1.0f);
    lb.Link(loop, "Loop", body, "Flow");
    const std::shared_ptr<LN_Program> looped = lb.Compile(&report);
    ASSERT_NE(looped, nullptr) << report;
    const char *outside = std::getenv("UPBGE_LN_COLUMN_BRIDGE");
    const std::string kept = outside != nullptr ? outside : "";
    setenv("UPBGE_LN_COLUMN_BRIDGE", "0", 1);
    EXPECT_EQ(LN_CompileColumnProgram(*looped, LN_Event::OnFixedUpdate, &refusal), nullptr);
    EXPECT_EQ(refusal, "a Loop");
    setenv("UPBGE_LN_COLUMN_BRIDGE", "1", 1);
    EXPECT_NE(LN_CompileColumnProgram(*looped, LN_Event::OnFixedUpdate, &refusal), nullptr)
        << refusal;
    if (outside != nullptr) {
      setenv("UPBGE_LN_COLUMN_BRIDGE", kept.c_str(), 1);
    }
    else {
      unsetenv("UPBGE_LN_COLUMN_BRIDGE");
    }
  }

  /* Columns off: nothing planned. */
  BrainOfOptions options;
  const std::shared_ptr<LN_Program> brain = BrainOf(options);
  ColumnScope off(0);
  SemRig rig(brain, kMemoOn, 50);
  rig.SetColumns(true, 0, 1);
  rig.Update();
  rig.Update();
  EXPECT_EQ(rig.LastColumnLanes(), 0u);
  EXPECT_EQ(rig.LastColumnRefusal(), "off");
}

/* The cost (DISABLED_): the research brain on 10,000 objects, the lean VM against columns, one
 * thread and 8 helper threads (the lean VM as a tree batch then); ns an NPC for the snapshot (or
 * the lanes' property capture), the recording (or the column run) and the flush (with the
 * write-back). UPBGE_LN_BENCH_CHAINS sets the chains (4). */
/* The flush's merge decides as if the column runs' writes were recorded commands: with 64 or more
 * last-write-wins writes in a step only the last write of a property is applied, so a property
 * missing before is made by the last write (Int here, not the first write's Float). */
TEST(LN_Columns, TheFlushMergesColumnWritesAsRecordedOnes)
{
  TreeBuilder b;
  GenNode &event = b.Add("LogicNativeOnFixedUpdate");
  GenNode &first = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(first, "Property", "fresh");
  b.Float(first, "Value", 1.5f);
  b.Link(event, "Out", first, "Flow");
  GenNode &second = b.Add("LogicNativeSetGamePropertyInt");
  b.String(second, "Property", "fresh");
  b.Int(second, "Value", 3);
  b.Link(first, "Done", second, "Flow");
  /* Two world positions: merged, only the last moves the object (its world stamp moves once). */
  GenNode &place = b.Add("LogicNativeSetObjectAttribute", 0, 15);
  b.Vector(place, "Value", 1.0f, 2.0f, 0.0f);
  b.Link(second, "Done", place, "Flow");
  GenNode &replace = b.Add("LogicNativeSetObjectAttribute", 0, 15);
  b.Vector(replace, "Value", -1.0f, 0.5f, 0.25f);
  b.Link(place, "Done", replace, "Flow");
  std::string report;
  const std::shared_ptr<LN_Program> program = b.Compile(&report);
  ASSERT_NE(program, nullptr) << report;
  /* 100 NPCs (merged writes) and 10 (below the gate, every write applied). */
  EXPECT_GT(ExpectColumnsMatch(program, LN_Event::OnFixedUpdate, 100, 3, "merged writes"), 0u);
  EXPECT_GT(ExpectColumnsMatch(program, LN_Event::OnFixedUpdate, 10, 3, "unmerged writes"), 0u);
  /* Trees that place crowd NPCs too, before and after them in the flush: their world position
   * writes merge with the NPCs' own as recorded ones would. */
  const std::shared_ptr<LN_Program> placer = OtherTree(Other::Place, true);
  ASSERT_NE(placer, nullptr);
  const auto extra = [&](SemRig &rig) {
    const struct {
      const char *target;
      uint32_t scene_index;
      bool first;
    } placements[] = {{"Obj4", 7, true}, {"Obj30", 63, false}, {"Obj31", 61, true}};
    for (const auto &placement : placements) {
      KX_GameObject &object = rig.AddObject("Other" + std::to_string(rig.ObjectCount()),
                                            MT_Vector3(1.0f, -2.0f, 0.0f));
      rig.SetString(object, "target", placement.target);
      rig.AddTree(placer, rig.ObjectCount() - 1, placement.scene_index, placement.first);
    }
  };
  /* A crowd with one position write each (nothing of its own merges): only the buffer's writes on
   * the same NPCs make its writes mergeable. */
  TreeBuilder once_b;
  GenNode &once_event = once_b.Add("LogicNativeOnFixedUpdate");
  GenNode &number = once_b.Add("LogicNativeSetGamePropertyFloat");
  once_b.String(number, "Property", "pf0");
  once_b.Float(number, "Value", 2.5f);
  once_b.Link(once_event, "Out", number, "Flow");
  GenNode &single = once_b.Add("LogicNativeSetObjectAttribute", 0, 15);
  once_b.Vector(single, "Value", 0.5f, 0.5f, 0.0f);
  once_b.Link(number, "Done", single, "Flow");
  const std::shared_ptr<LN_Program> single_place = once_b.Compile(&report);
  ASSERT_NE(single_place, nullptr) << report;
  EXPECT_GT(ExpectColumnsMatch(single_place,
                               LN_Event::OnFixedUpdate,
                               100,
                               3,
                               "placed by others",
                               0,
                               nullptr,
                               extra,
                               2),
            0u);
  /* A crowd whose moves end every run of merged writes: two trees placing the Target before and
   * after NPCs of it in the flush are both applied (no merge across an NPC's move). */
  TreeBuilder move_b;
  GenNode &move_event = move_b.Add("LogicNativeOnFixedUpdate");
  GenNode &move_number = move_b.Add("LogicNativeSetGamePropertyFloat");
  move_b.String(move_number, "Property", "pf0");
  move_b.Float(move_number, "Value", 1.25f);
  move_b.Link(move_event, "Out", move_number, "Flow");
  GenNode &move = move_b.Add("LogicNativeApplyMovement");
  move_b.Vector(move, "Vector", 0.0f, 0.1f, 0.0f);
  move_b.Link(move_number, "Done", move, "Flow");
  const std::shared_ptr<LN_Program> mover = move_b.Compile(&report);
  ASSERT_NE(mover, nullptr) << report;
  const auto place_target = [&](SemRig &rig) {
    for (const auto &placement : {std::make_pair(7u, true), std::make_pair(63u, false)}) {
      KX_GameObject &object = rig.AddObject("Other" + std::to_string(rig.ObjectCount()),
                                            MT_Vector3(1.0f, -2.0f, 0.0f));
      rig.SetString(object, "target", "Target");
      rig.AddTree(placer, rig.ObjectCount() - 1, placement.first, placement.second);
    }
  };
  EXPECT_GT(ExpectColumnsMatch(
                mover, LN_Event::OnFixedUpdate, 100, 3, "moves end runs", 0, nullptr, place_target, 2),
            0u);
}

/* Every N Steps in columns: gated routes run on their gate's steps with Spread Out, their Time
 * Passed and unscaled delta are the gate's, a distance read only behind a gate applies pending
 * moves only on steps some NPC's gate lets through; Once, Flow Router, Repeat Limit and tree
 * properties keep their states exactly. */
TEST(LN_Columns, GatedRoutesAndFlowStatesEndAsOnTheLeanVM)
{
  for (const int16_t every : {2, 3, 5}) {
    for (const int16_t spread : {0, 1}) {
      TreeBuilder b;
      GenNode &event = b.Add("LogicNativeOnFixedUpdate", every, spread);
      /* Time Passed and the unscaled delta into properties. */
      GenNode &passed = b.Add("LogicNativeSetGamePropertyFloat");
      b.String(passed, "Property", "passed");
      b.Link(event, "Time Passed", passed, "Value");
      b.Link(event, "Out", passed, "Flow");
      /* The distance to the Target, read only on the gate's steps, kept in a tree property; the
       * property is read first (the last run's value) into "dist", then written. */
      GenNode &read = b.Add("LogicNativeGetTreeProperty");
      b.String(read, "Property", "near");
      GenNode &store = b.Add("LogicNativeSetGamePropertyFloat");
      b.String(store, "Property", "dist");
      b.Link(read, "Value", store, "Value");
      b.Link(passed, "Done", store, "Flow");
      GenNode &distance = b.Add("LogicNativeGetDistance");
      b.ObjectNamed(distance, "Object B", &diff::TargetBlenderObject());
      GenNode &keep = b.Add("LogicNativeSetTreeProperty");
      b.String(keep, "Property", "near");
      b.Link(distance, "Distance", keep, "Value");
      b.Link(store, "Done", keep, "Flow");
      /* Once, a Flow Router, a Repeat Limit: counters in properties. */
      GenNode &once = b.Add("LogicNativeOnce");
      b.Link(store, "Done", once, "Flow");
      GenNode &once_count = b.Add("LogicNativeSetGamePropertyInt");
      b.String(once_count, "Property", "pi0");
      b.Int(once_count, "Value", 77);
      b.Link(once, "Out", once_count, "Flow");
      GenNode &router = b.Add("LogicNativeFlowRouter", 0, 0, 3);
      b.Link(store, "Done", router, "Flow");
      GenNode &route_a = b.Add("LogicNativeApplyMovement");
      b.Vector(route_a, "Vector", 0.25f, 0.0f, 0.0f);
      b.Link(router, "Then 1", route_a, "Flow");
      GenNode &route_b = b.Add("LogicNativeSetGamePropertyBool");
      b.String(route_b, "Property", "pb0");
      b.Bool(route_b, "Value", true);
      b.Link(router, "Then 2", route_b, "Flow");
      GenNode &limit = b.Add("LogicNativeRepeatLimit");
      b.Int(limit, "Limit", 4);
      b.Link(store, "Done", limit, "Flow");
      GenNode &limited = b.Add("LogicNativeModifyProperty");
      b.String(limited, "Property", "pf2");
      b.Float(limited, "Value", 1.0f);
      b.Link(limit, "Accepted", limited, "Flow");
      std::string report;
      const std::shared_ptr<LN_Program> program = b.Compile(&report);
      ASSERT_NE(program, nullptr) << report;
      std::string refusal;
      ASSERT_NE(LN_CompileColumnProgram(*program, LN_Event::OnFixedUpdate, &refusal), nullptr)
          << refusal << "\n" << LN_DumpProgramText(*program);
      const std::string what = "every " + std::to_string(every) + " spread " +
                               std::to_string(spread);
      EXPECT_GT(ExpectColumnsMatch(program, LN_Event::OnFixedUpdate, 60, 16, what), 0u) << what;
      EXPECT_GT(ExpectColumnsMatch(program, LN_Event::OnFixedUpdate, 600, 7, what + " helpers", 3),
                0u)
          << what;
    }
  }
}

namespace {

/** Delay, Timer, Pulsify, Cooldown (with its Reset), Barrier and, on frames, Wait for Next Frame:
 * their durations read per NPC from properties the routes change, so every NPC's timers run apart. */
std::shared_ptr<LN_Program> TimeNodesTree(const bool frames,
                                          const int16_t every,
                                          const int16_t spread,
                                          std::string *r_report)
{
  TreeBuilder b;
  GenNode &event = frames ? b.Add("LogicNativeOnUpdate") :
                            b.Add("LogicNativeOnFixedUpdate", every, spread);
  const auto read_float = [&](const char *name) -> GenNode & {
    GenNode &read = b.Add("LogicNativeGetGamePropertyFloat");
    b.String(read, "Property", name);
    return read;
  };
  const auto read_bool = [&](const char *name) -> GenNode & {
    GenNode &read = b.Add("LogicNativeGetGamePropertyBool");
    b.String(read, "Property", name);
    return read;
  };
  const auto modify = [&](const char *name, const float by) -> GenNode & {
    GenNode &node = b.Add("LogicNativeModifyProperty");
    b.String(node, "Property", name);
    b.Float(node, "Value", by);
    return node;
  };
  /* Delay: its length per NPC (pf1: -1..3 s, below 0 at once), Ignore Timescale from pb0. */
  GenNode &delay = b.Add("LogicNativeDelay");
  b.Link(event, "Out", delay, "Flow");
  b.Link(read_float("pf1"), "Value", delay, "Delay");
  b.Link(read_bool("pb0"), "Value", delay, "Ignore Timescale");
  GenNode &delayed = modify("pf0", 0.5f);
  b.Link(delay, "Out", delayed, "Flow");
  /* Cooldown: Duration from pf0 (which the Delay raises), every output used. */
  GenNode &cooldown = b.Add("LogicNativeCooldown");
  b.Link(event, "Out", cooldown, "Flow");
  b.Link(read_float("pf0"), "Value", cooldown, "Duration");
  GenNode &accepted = modify("pf2", 1.0f);
  b.Link(cooldown, "Accepted", accepted, "Flow");
  GenNode &ready = b.Add("LogicNativeSetGamePropertyBool");
  b.String(ready, "Property", "ready");
  b.Link(cooldown, "Is Ready", ready, "Value");
  b.Link(accepted, "Done", ready, "Flow");
  GenNode &blocked = b.Add("LogicNativeApplyMovement");
  b.Vector(blocked, "Vector", 0.05f, 0.0f, 0.0f);
  b.Link(cooldown, "Blocked", blocked, "Flow");
  GenNode &remaining = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(remaining, "Property", "rem");
  b.Link(cooldown, "Remaining", remaining, "Value");
  b.Link(blocked, "Done", remaining, "Flow");
  GenNode &progress = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(progress, "Property", "prog");
  b.Link(cooldown, "Progress", progress, "Value");
  b.Link(remaining, "Done", progress, "Flow");
  GenNode &completed = modify("dist", -0.25f);
  b.Link(cooldown, "Completed", completed, "Flow");
  /* Pulsify (its gap per NPC): resets the Cooldown and starts a Timer. */
  GenNode &pulsify = b.Add("LogicNativePulsify");
  b.Link(event, "Out", pulsify, "Flow");
  b.Link(read_float("pf2"), "Value", pulsify, "Gap");
  b.Link(pulsify, "Out", cooldown, "Reset");
  GenNode &timer = b.Add("LogicNativeTimer");
  b.Link(pulsify, "Out", timer, "Set Timer");
  b.Link(read_float("pf1"), "Value", timer, "Seconds");
  GenNode &elapsed = b.Add("LogicNativeSetGamePropertyBool");
  b.String(elapsed, "Property", "pb0");
  b.Link(read_bool("ready"), "Value", elapsed, "Value");
  b.Link(timer, "Out", elapsed, "Flow");
  /* Barrier: open while pb0 holds, for 0.3 s. */
  GenNode &barrier = b.Add("LogicNativeBarrier");
  b.Link(event, "Out", barrier, "Flow");
  b.Link(read_bool("pb0"), "Value", barrier, "Condition");
  b.Float(barrier, "Time", 0.3f);
  GenNode &passed = modify("dist", 1.0f);
  b.Link(barrier, "Out", passed, "Flow");
  if (frames) {
    /* Wait for Next Frame behind a condition that changes. */
    GenNode &branch = b.Add("LogicNativeBranch");
    b.Link(event, "Out", branch, "Flow");
    b.Link(read_bool("ready"), "Value", branch, "Condition");
    GenNode &next = b.Add("LogicNativeOnNextFrame");
    b.Link(branch, "True", next, "Flow");
    GenNode &later = b.Add("LogicNativeApplyRotation");
    b.Rotation(later, "Rotation", 0.0f, 0.0f, 0.1f);
    b.Link(next, "Completed", later, "Flow");
  }
  return b.Compile(r_report);
}

}  // namespace

/* The time nodes keep their states in the trees: the columns change them with the VM's own code
 * (LN_RuntimeTree::TimeFlow*), NPC by NPC, and count them down as the VM does before the pass. */
TEST(LN_Columns, TimeNodesEndAsOnTheLeanVM)
{
  struct Case {
    bool frames;
    int16_t every, spread;
  };
  for (const Case c : {Case{true, 0, 0}, Case{false, 1, 0}, Case{false, 3, 0}, Case{false, 4, 1}}) {
    std::string report;
    const std::shared_ptr<LN_Program> program = TimeNodesTree(c.frames, c.every, c.spread, &report);
    ASSERT_NE(program, nullptr) << report;
    const LN_Event event = c.frames ? LN_Event::OnUpdate : LN_Event::OnFixedUpdate;
    std::string refusal;
    ASSERT_NE(LN_CompileColumnProgram(*program, event, &refusal), nullptr)
        << refusal << "\n" << LN_DumpProgramText(*program);
    const std::string what = std::string(c.frames ? "frames" : "steps") + " every " +
                             std::to_string(c.every) + " spread " + std::to_string(c.spread);
    EXPECT_GT(ExpectColumnsMatch(program, event, 40, 300, what), 0u) << what;
    EXPECT_GT(ExpectColumnsMatch(program, event, 600, 30, what + " helpers", 3), 0u) << what;
  }
}

/* Only an op whose own guard is a Delay's Done reads the tree properties as they were when the
 * Delay was armed (DelayTreePropertySnapshotForGuard); the compiler puts a Done route behind a
 * branch on it, so the route reads them as they are, in columns as on the VM. */
TEST(LN_Columns, ADelaysDoneRouteReadsTreePropertiesAsTheVMDoes)
{
  TreeBuilder b;
  GenNode &event = b.Add("LogicNativeOnUpdate");
  GenNode &delay = b.Add("LogicNativeDelay");
  b.Float(delay, "Delay", 0.1f);
  b.Link(event, "Out", delay, "Flow");
  /* After the Delay is armed the tree property changes every frame. */
  GenNode &source = b.Add("LogicNativeGetGamePropertyFloat");
  b.String(source, "Property", "pf2");
  GenNode &keep = b.Add("LogicNativeSetTreeProperty");
  b.String(keep, "Property", "tp0");
  b.Link(source, "Value", keep, "Value");
  b.Link(event, "Out", keep, "Flow");
  GenNode &grow = b.Add("LogicNativeModifyProperty");
  b.String(grow, "Property", "pf2");
  b.Float(grow, "Value", 1.0f);
  b.Link(event, "Out", grow, "Flow");
  GenNode &read = b.Add("LogicNativeGetTreeProperty");
  b.String(read, "Property", "tp0");
  GenNode &store = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(store, "Property", "pf0");
  b.Link(read, "Value", store, "Value");
  b.Link(delay, "Out", store, "Flow");
  std::string report;
  const std::shared_ptr<LN_Program> program = b.Compile(&report);
  ASSERT_NE(program, nullptr) << report;
  std::string refusal;
  ASSERT_NE(LN_CompileColumnProgram(*program, LN_Event::OnUpdate, &refusal), nullptr)
      << refusal << "\n" << LN_DumpProgramText(*program);
  EXPECT_GT(ExpectColumnsMatch(program, LN_Event::OnUpdate, 40, 60, "delay done"), 0u);
}

/* Orientations worked out per NPC: Vector To Rotation (as a rotation), Euler angles from
 * properties, a position as Euler angles into Set Object Attribute's orientation, the own
 * orientation read live; set and applied. */
TEST(LN_Columns, ComputedRotationsEndAsOnTheLeanVM)
{
  for (const bool local : {false, true}) {
    TreeBuilder b;
    GenNode &event = b.Add("LogicNativeOnFixedUpdate");
    /* The Target's position as Euler angles, into Set Object Attribute's orientation. */
    GenNode &target = b.Add("LogicNativeGetObjectAttribute", 0);
    b.ObjectNamed(target, "Object", &diff::TargetBlenderObject());
    GenNode &set = b.Add("LogicNativeSetObjectAttribute", 1, 15);
    b.Link(target, "Vector", set, "Value");
    b.Link(event, "Out", set, "Flow");
    /* A turn from properties (Euler into Apply Rotation's Rotation). */
    GenNode &pf0 = b.Add("LogicNativeGetGamePropertyFloat");
    b.String(pf0, "Property", "pf0");
    GenNode &pf1 = b.Add("LogicNativeGetGamePropertyFloat");
    b.String(pf1, "Property", "pf1");
    GenNode &turn = b.Add("LogicNativeEuler");
    b.Link(pf0, "Value", turn, "X");
    b.Link(pf1, "Value", turn, "Z");
    GenNode &apply = b.Add("LogicNativeApplyRotation");
    b.Link(turn, "Rotation", apply, "Rotation");
    b.Bool(apply, "Local", local);
    b.Link(set, "Done", apply, "Flow");
    /* Vector To Rotation of a vector from properties. */
    GenNode &axis = b.Add("LogicNativeCombineXYZ");
    b.Link(pf1, "Value", axis, "X");
    b.Float(axis, "Z", 1.0f);
    GenNode &aim = b.Add("LogicNativeVectorToRotation");
    b.Link(axis, "Vector", aim, "Direction");
    GenNode &apply_aim = b.Add("LogicNativeApplyRotation");
    b.Link(aim, "Rotation", apply_aim, "Rotation");
    b.Link(apply, "Done", apply_aim, "Flow");
    /* The own orientation read live, applied again (local). */
    GenNode &own = b.Add("LogicNativeGetObjectAttribute", 7);
    GenNode &again = b.Add("LogicNativeApplyRotation");
    b.Link(own, "Orientation", again, "Rotation");
    b.Bool(again, "Local", true);
    b.Link(apply_aim, "Done", again, "Flow");
    GenNode &grow = b.Add("LogicNativeModifyProperty");
    b.String(grow, "Property", "pf0");
    b.Float(grow, "Value", 0.01f);
    b.Link(again, "Done", grow, "Flow");
    std::string report;
    const std::shared_ptr<LN_Program> program = b.Compile(&report);
    ASSERT_NE(program, nullptr) << report;
    std::string refusal;
    ASSERT_NE(LN_CompileColumnProgram(*program, LN_Event::OnFixedUpdate, &refusal), nullptr)
        << refusal << "\n" << LN_DumpProgramText(*program);
    const std::string what = local ? "local" : "world";
    EXPECT_GT(ExpectColumnsMatch(program, LN_Event::OnFixedUpdate, 40, 60, what), 0u) << what;
    EXPECT_GT(
        ExpectColumnsMatch(program, LN_Event::OnFixedUpdate, 600, 8, what + " helpers", 3), 0u)
        << what;
  }
}

/* Set Visible, Set Object Color and a Teleport (Set Object Attribute) from per-NPC values, between
 * moves and property writes: applied at the tree's place in the flush, in op order. */
TEST(LN_Columns, VisibilityColorAndTeleportEndAsOnTheLeanVM)
{
  TreeBuilder b;
  GenNode &event = b.Add("LogicNativeOnFixedUpdate");
  GenNode &pb0 = b.Add("LogicNativeGetGamePropertyBool");
  b.String(pb0, "Property", "pb0");
  GenNode &pf0 = b.Add("LogicNativeGetGamePropertyFloat");
  b.String(pf0, "Property", "pf0");
  GenNode &pf1 = b.Add("LogicNativeGetGamePropertyFloat");
  b.String(pf1, "Property", "pf1");
  GenNode &visible = b.Add("LogicNativeSetObjectAttribute", 12, 15);
  b.Link(pb0, "Value", visible, "Visible");
  b.Link(event, "Out", visible, "Flow");
  GenNode &rgba = b.Add("LogicNativeColorRGBA");
  b.Link(pf0, "Value", rgba, "R");
  b.Link(pf1, "Value", rgba, "B");
  GenNode &color = b.Add("LogicNativeSetObjectAttribute", 11, 15);
  b.Link(rgba, "Color", color, "Color");
  b.Link(visible, "Done", color, "Flow");
  GenNode &place = b.Add("LogicNativeSetObjectAttribute", 0, 15);
  b.Vector(place, "Value", 1.0f, 2.0f, 0.0f);
  b.Link(pb0, "Value", place, "Teleport");
  b.Link(color, "Done", place, "Flow");
  GenNode &flip = b.Add("LogicNativeToggleProperty");
  b.String(flip, "Property", "pb0");
  b.Link(place, "Done", flip, "Flow");
  GenNode &grow = b.Add("LogicNativeModifyProperty");
  b.String(grow, "Property", "pf0");
  b.Float(grow, "Value", 0.125f);
  b.Link(place, "Done", grow, "Flow");
  std::string report;
  const std::shared_ptr<LN_Program> program = b.Compile(&report);
  ASSERT_NE(program, nullptr) << report;
  std::string refusal;
  ASSERT_NE(LN_CompileColumnProgram(*program, LN_Event::OnFixedUpdate, &refusal), nullptr)
      << refusal << "\n" << LN_DumpProgramText(*program);
  EXPECT_GT(ExpectColumnsMatch(program, LN_Event::OnFixedUpdate, 40, 30, "state"), 0u);
  EXPECT_GT(ExpectColumnsMatch(program, LN_Event::OnFixedUpdate, 600, 6, "state helpers", 3), 0u);
}

/* A Stored Value read behind a Value Switch whose condition changes per NPC: its first read
 * stores the input (Initialize) only where the VM reads it, so NPCs store different values at
 * different steps. */
TEST(LN_Columns, AStoredValueReadBehindAConditionStoresWhereTheVMReadsIt)
{
  TreeBuilder b;
  GenNode &event = b.Add("LogicNativeOnUpdate");
  GenNode &pb0 = b.Add("LogicNativeGetGamePropertyBool");
  b.String(pb0, "Property", "pb0");
  GenNode &pf2 = b.Add("LogicNativeGetGamePropertyFloat");
  b.String(pf2, "Property", "pf2");
  GenNode &stored = b.Add("LogicNativeStoreValue");
  b.Link(pf2, "Value", stored, "Value");
  GenNode &pick = b.Add("LogicNativeValueSwitch");
  b.Link(pb0, "Value", pick, "Condition");
  b.Link(stored, "Value", pick, "True");
  b.Float(pick, "False", -1.0f);
  GenNode &keep = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(keep, "Property", "pf3");
  b.Link(pick, "Value", keep, "Value");
  b.Link(event, "Out", keep, "Flow");
  /* pf2 grows every frame, pb0 turns on for a few NPCs at a time. */
  GenNode &grow = b.Add("LogicNativeModifyProperty");
  b.String(grow, "Property", "pf2");
  b.Float(grow, "Value", 1.0f);
  b.Link(keep, "Done", grow, "Flow");
  GenNode &chance = b.Add("LogicNativeGetGamePropertyFloat");
  b.String(chance, "Property", "pf0");
  GenNode &above = b.Add("LogicNativeCompare", 4);
  b.Link(chance, "Value", above, "A");
  b.Float(above, "B", 1.0f);
  GenNode &turn_on = b.Add("LogicNativeSetGamePropertyBool");
  b.String(turn_on, "Property", "pb0");
  b.Link(above, "Result", turn_on, "Value");
  b.Link(grow, "Done", turn_on, "Flow");
  GenNode &drift = b.Add("LogicNativeModifyProperty");
  b.String(drift, "Property", "pf0");
  b.Float(drift, "Value", 0.25f);
  b.Link(turn_on, "Done", drift, "Flow");
  std::string report;
  const std::shared_ptr<LN_Program> program = b.Compile(&report);
  ASSERT_NE(program, nullptr) << report;
  std::string refusal;
  ASSERT_NE(LN_CompileColumnProgram(*program, LN_Event::OnUpdate, &refusal), nullptr)
      << refusal << "\n" << LN_DumpProgramText(*program);
  EXPECT_GT(ExpectColumnsMatch(program, LN_Event::OnUpdate, 40, 40, "stored"), 0u);
}

/* Found by the column soak (seed 19618): route A (every 3 steps, the same step for all) reads the
 * owner's distance first; route B (every 3 steps, spread out) moves the owner and then reads the
 * distance. On a step where only B runs, its read is the pass's first and the VM applies B's move
 * before it: B's read must not reuse A's. Moving after its read, B lowers and ends as the VM. */
TEST(LN_Columns, AGatedDistanceReadAfterAMoveIsNotTakenFromAnotherRoute)
{
  for (const bool move_first : {true, false}) {
    TreeBuilder b;
    GenNode &route_a = b.Add("LogicNativeOnFixedUpdate", 3, 1);
    GenNode &route_b = b.Add("LogicNativeOnFixedUpdate", 3, 0);
    GenNode &distance = b.Add("LogicNativeGetDistance", 0, 0);
    b.ObjectNamed(distance, "Object B", &diff::TargetBlenderObject());
    GenNode &keep_a = b.Add("LogicNativeSetGamePropertyFloat");
    b.String(keep_a, "Property", "pf1");
    b.Link(distance, "Distance", keep_a, "Value");
    b.Link(route_a, "Out", keep_a, "Flow");
    GenNode &move = b.Add("LogicNativeApplyMovement");
    b.Vector(move, "Vector", 0.75f, -0.5f, 0.0f);
    GenNode &keep_b = b.Add("LogicNativeSetGamePropertyFloat");
    b.String(keep_b, "Property", "pf0");
    b.Link(distance, "Distance", keep_b, "Value");
    if (move_first) {
      b.Link(route_b, "Out", move, "Flow");
      b.Link(move, "Done", keep_b, "Flow");
    }
    else {
      b.Link(route_b, "Out", keep_b, "Flow");
      b.Link(keep_b, "Done", move, "Flow");
    }
    std::string report;
    const std::shared_ptr<LN_Program> program = b.Compile(&report);
    ASSERT_NE(program, nullptr) << report;
    std::string refusal;
    const bool lowers = LN_CompileColumnProgram(*program, LN_Event::OnFixedUpdate, &refusal) !=
                        nullptr;
    if (move_first) {
      EXPECT_FALSE(lowers) << LN_DumpProgramText(*program);
      EXPECT_EQ(refusal, "a distance from the owner first read after a move of it");
    }
    else {
      ASSERT_TRUE(lowers) << refusal << "\n" << LN_DumpProgramText(*program);
      EXPECT_GT(ExpectColumnsMatch(program, LN_Event::OnFixedUpdate, 40, 30, "read first"), 0u);
    }
  }
}

namespace {

/* The bridge (plan C1-8): nodes without a kernel run the VM's own code NPC by NPC inside the
 * column run: Switch Flow (Integer), On Value Changed (its Old as a generic value), Formula, text
 * writes (the property stream's side list) next to number writes, Print (its order is the flush's),
 * another object's visibility. */
std::shared_ptr<LN_Program> BridgedTree(std::string *r_report)
{
  TreeBuilder b;
  GenNode &event = b.Add("LogicNativeOnUpdate");
  GenNode &pi0 = b.Add("LogicNativeGetGamePropertyInt");
  b.String(pi0, "Property", "pi0");
  GenNode &state = b.Add("LogicNativeSwitchFlowInt", 0, 0, 2);
  b.Link(pi0, "Value", state, "Index");
  b.Link(event, "Out", state, "Flow");
  GenNode &to_one = b.Add("LogicNativeSetGamePropertyInt");
  b.String(to_one, "Property", "pi0");
  b.Int(to_one, "Value", 1);
  b.Link(state, "Then 0", to_one, "Flow");
  GenNode &grow = b.Add("LogicNativeModifyProperty");
  b.String(grow, "Property", "pf0");
  b.Float(grow, "Value", 0.5f);
  b.Link(to_one, "Done", grow, "Flow");
  GenNode &to_zero = b.Add("LogicNativeSetGamePropertyInt");
  b.String(to_zero, "Property", "pi0");
  b.Int(to_zero, "Value", 0);
  b.Link(state, "Then 1", to_zero, "Flow");
  GenNode &text = b.Add("LogicNativeSetGamePropertyString");
  b.String(text, "Property", "ps0");
  b.String(text, "Value", "one");
  b.Link(to_zero, "Done", text, "Flow");
  GenNode &reset = b.Add("LogicNativeSetGamePropertyInt");
  b.String(reset, "Property", "pi0");
  b.Int(reset, "Value", 0);
  b.Link(state, "Default", reset, "Flow");
  GenNode &pf1 = b.Add("LogicNativeGetGamePropertyFloat");
  b.String(pf1, "Property", "pf1");
  GenNode &say = b.Add("LogicNativePrint");
  b.Link(pf1, "Value", say, "Message");
  b.Link(reset, "Done", say, "Flow");
  /* Formula (a + b) into a number write. */
  GenNode &pf2 = b.Add("LogicNativeGetGamePropertyFloat");
  b.String(pf2, "Property", "pf2");
  GenNode &formula = b.Add("LogicNativeFormula", 1);
  b.Link(pf1, "Value", formula, "a");
  b.Link(pf2, "Value", formula, "b");
  GenNode &sum = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(sum, "Property", "pf2");
  b.Link(formula, "Out", sum, "Value");
  b.Link(event, "Out", sum, "Flow");
  /* On Value Changed of pf0: its Old value and a text. */
  GenNode &pf0 = b.Add("LogicNativeGetGamePropertyFloat");
  b.String(pf0, "Property", "pf0");
  GenNode &changed = b.Add("LogicNativeValueChanged");
  b.Link(pf0, "Value", changed, "Value");
  GenNode &old = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(old, "Property", "pf3");
  b.Link(changed, "Old", old, "Value");
  b.Link(changed, "If Changed", old, "Flow");
  GenNode &note = b.Add("LogicNativeSetGamePropertyString");
  b.String(note, "Property", "ps1");
  b.String(note, "Value", "changed");
  b.Link(old, "Done", note, "Flow");
  /* Send Event with a subject some NPCs hold: with an empty one the handler returns before it
   * stamps the op, so its Done route does not run for them. */
  GenNode &name = b.Add("LogicNativeGetGamePropertyString");
  b.String(name, "Property", "ps2");
  GenNode &send = b.Add("LogicNativeSendEvent");
  b.Link(name, "Value", send, "Subject");
  b.Link(sum, "Done", send, "Flow");
  GenNode &sent = b.Add("LogicNativeModifyProperty");
  b.String(sent, "Property", "pf4");
  b.Float(sent, "Value", 1.0f);
  b.Link(send, "Done", sent, "Flow");
  /* An on-demand Tween (its state moves where it is read) read only where pb0 holds. */
  GenNode &tween = b.Add("LogicNativeTweenValue", 0, 1);
  b.Float(tween, "FromFloat", 0.0f);
  b.Float(tween, "ToFloat", 10.0f);
  b.Float(tween, "Duration", 0.25f);
  GenNode &pb0 = b.Add("LogicNativeGetGamePropertyBool");
  b.String(pb0, "Property", "pb0");
  GenNode &pick = b.Add("LogicNativeValueSwitch");
  b.Link(pb0, "Value", pick, "Condition");
  b.Link(tween, "ResultFloat", pick, "True");
  b.Float(pick, "False", -1.0f);
  GenNode &tweened = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(tweened, "Property", "pf5");
  b.Link(pick, "Value", tweened, "Value");
  b.Link(sent, "Done", tweened, "Flow");
  GenNode &flip = b.Add("LogicNativeToggleProperty");
  b.String(flip, "Property", "pb0");
  b.Link(tweened, "Done", flip, "Flow");
  return b.Compile(r_report);
}

/* Half the NPCs name the Target (Send Event finds it), the others name nothing. */
void NameTheTargetForHalf(SemRig &rig)
{
  for (uint32_t index = 0; index < 40 && index < rig.ObjectCount(); index += 2) {
    rig.SetString(rig.Object(index), "ps2", "Target");
  }
}

}  // namespace

TEST(LN_Columns, BridgedNodesEndAsOnTheLeanVM)
{
  BridgeOn bridge;
  std::string report;
  const std::shared_ptr<LN_Program> program = BridgedTree(&report);
  ASSERT_NE(program, nullptr) << report;
  std::string refusal;
  ASSERT_NE(LN_CompileColumnProgram(*program, LN_Event::OnUpdate, &refusal), nullptr)
      << refusal << "\n" << LN_DumpProgramText(*program);
  EXPECT_GT(ExpectColumnsMatch(program, LN_Event::OnUpdate, 40, 20, "bridged", 0, nullptr,
                               NameTheTargetForHalf),
            0u);
  uint64_t helper_before = 0, main_before = 0;
  LN_ColumnSegmentCounts(helper_before, main_before);
  EXPECT_GT(ExpectColumnsMatch(program, LN_Event::OnUpdate, 600, 6, "bridged helpers", 3, nullptr,
                               NameTheTargetForHalf),
            0u);
  /* Plan C1-9: the helpers took segments of it and the main thread the rest (Send Event with a
   * subject read while playing stays here). A run with UPBGE_LN_COLUMN_SEGMENTS=0 (read once a
   * process) has none: there the worlds check the path before segments. */
  uint64_t helper_after = 0, main_after = 0;
  LN_ColumnSegmentCounts(helper_after, main_after);
  const char *segments = std::getenv("UPBGE_LN_COLUMN_SEGMENTS");
  if (!DirectForced() &&
      (segments == nullptr ||
       (std::strcmp(segments, "0") != 0 && std::strcmp(segments, "off") != 0)))
  {
    EXPECT_GT(helper_after, helper_before);
    EXPECT_GT(main_after, main_before);
  }
  /* The research brain that reads the Player's visibility (another object's snapshot). */
  BrainOfOptions look;
  look.look = true;
  EXPECT_GT(ExpectColumnsMatch(BrainOf(look), LN_Event::OnUpdate, 60, 20, "look"), 0u);
}

/* The compiler's table of the fields each kind reads (intern/LN_KindInputsData.inc) is the
 * recorded golden: after re-recording kind_inputs_v0, run lean_plan/make_kind_inputs_inc.py. */
TEST(LN_Columns, TheCompiledKindInputsAreTheRecordedOnes)
{
  std::ifstream file(GoldenPath("kind_inputs_v0"));
  ASSERT_TRUE(file.good()) << GoldenPath("kind_inputs_v0");
  std::string recorded;
  std::string line;
  while (std::getline(file, line)) {
    if (!line.empty() && line[0] != '#') {
      recorded += line + "\n";
    }
  }
  EXPECT_EQ(LN_ColumnKindInputsText(), recorded)
      << "run ~/Desktop/CPP_LN_TESTS/lean_plan/make_kind_inputs_inc.py";
}

namespace {

enum class Reads { Property, KeptDistance, LateDistance, Tween, GatedDistance };

/* On Logic + Physics Step: the distance to the Target (kept in pf1 before the move, unless
 * LateDistance), a move of the owner, then Switch Flow (Integer) on a property and On Value
 * Changed (of a property, of the distance, or of a Tween, a kind whose inputs the walk does not
 * know) into writes: bridged nodes after the move. */
std::shared_ptr<LN_Program> BridgedAfterMoveTree(const Reads reads, std::string *r_report)
{
  TreeBuilder b;
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode &distance = b.Add("LogicNativeGetDistance", 0, 0);
  b.ObjectNamed(distance, "Object B", &diff::TargetBlenderObject());
  GenNode &move = b.Add("LogicNativeApplyMovement");
  b.Vector(move, "Vector", 0.25f, -0.5f, 0.0f);
  if (reads == Reads::GatedDistance) {
    /* Kept only every third step: on the others the read after the move is the pass's first. */
    GenNode &every = b.Add("LogicNativeOnFixedUpdate", 3, 0);
    GenNode &keep = b.Add("LogicNativeSetGamePropertyFloat");
    b.String(keep, "Property", "pf1");
    b.Link(distance, "Distance", keep, "Value");
    b.Link(every, "Out", keep, "Flow");
    b.Link(step, "Out", move, "Flow");
  }
  else if (reads != Reads::LateDistance) {
    GenNode &keep = b.Add("LogicNativeSetGamePropertyFloat");
    b.String(keep, "Property", "pf1");
    b.Link(distance, "Distance", keep, "Value");
    b.Link(step, "Out", keep, "Flow");
    b.Link(keep, "Done", move, "Flow");
  }
  else {
    b.Link(step, "Out", move, "Flow");
  }
  GenNode &pi0 = b.Add("LogicNativeGetGamePropertyInt");
  b.String(pi0, "Property", "pi0");
  GenNode &state = b.Add("LogicNativeSwitchFlowInt", 0, 0, 2);
  b.Link(pi0, "Value", state, "Index");
  b.Link(move, "Done", state, "Flow");
  GenNode &bump = b.Add("LogicNativeSetGamePropertyInt");
  b.String(bump, "Property", "pi0");
  b.Int(bump, "Value", 1);
  b.Link(state, "Then 0", bump, "Flow");
  GenNode &changed = b.Add("LogicNativeValueChanged");
  GenNode &pf0 = b.Add("LogicNativeGetGamePropertyFloat");
  b.String(pf0, "Property", "pf0");
  if (reads == Reads::Tween) {
    GenNode &tween = b.Add("LogicNativeTweenValue", 0, 1);
    b.Float(tween, "FromFloat", 0.0f);
    b.Float(tween, "ToFloat", 10.0f);
    b.Float(tween, "Duration", 0.25f);
    b.Link(tween, "ResultFloat", changed, "Value");
  }
  else if (reads != Reads::Property) {
    b.Link(distance, "Distance", changed, "Value");
  }
  else {
    b.Link(pf0, "Value", changed, "Value");
  }
  GenNode &old = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(old, "Property", "pf3");
  b.Link(changed, "Old", old, "Value");
  b.Link(state, "Then 1", old, "Flow");
  GenNode &grow = b.Add("LogicNativeModifyProperty");
  b.String(grow, "Property", "pf0");
  b.Float(grow, "Value", 0.5f);
  b.Link(old, "Done", grow, "Flow");
  GenNode &back = b.Add("LogicNativeSetGamePropertyInt");
  b.String(back, "Property", "pi0");
  b.Int(back, "Value", 0);
  b.Link(grow, "Done", back, "Flow");
  return b.Compile(r_report);
}

}  // namespace

/* A bridged node after a move of the owner, in a program that measures from the owner. It runs in
 * columns when it reads no distance (On Value Changed of a property), or a distance surely read
 * before the move (the VM keeps that one for the pass: it reads the value before the move as the
 * run does); it is refused when it would be the distance's first read (the VM's read would apply
 * the move first). */
TEST(LN_Columns, ABridgedNodeAfterAMoveLowersUnlessItMayReadADistance)
{
  BridgeOn bridge;
  for (const Reads reads :
       {Reads::Property, Reads::KeptDistance, Reads::LateDistance, Reads::Tween,
        Reads::GatedDistance})
  {
    std::string report;
    const std::shared_ptr<LN_Program> program = BridgedAfterMoveTree(reads, &report);
    ASSERT_NE(program, nullptr) << report;
    std::string refusal;
    const bool lowers = LN_CompileColumnProgram(*program, LN_Event::OnFixedUpdate, &refusal) !=
                        nullptr;
    if (reads == Reads::LateDistance) {
      EXPECT_FALSE(lowers) << LN_DumpProgramText(*program);
      EXPECT_NE(refusal.find("after a move of the owner: it reads a distance"), std::string::npos)
          << refusal;
    }
    else if (reads == Reads::GatedDistance) {
      EXPECT_FALSE(lowers) << LN_DumpProgramText(*program);
      EXPECT_NE(refusal.find("it reads a distance from the owner not surely read before the move"),
                std::string::npos)
          << refusal;
    }
    else if (reads == Reads::Tween) {
      EXPECT_FALSE(lowers) << LN_DumpProgramText(*program);
      EXPECT_NE(refusal.find("after a move of the owner: it may read a ray or a distance ("),
                std::string::npos)
          << refusal;
    }
    else {
      ASSERT_TRUE(lowers) << refusal << "\n" << LN_DumpProgramText(*program);
      EXPECT_GT(ExpectColumnsMatch(program, LN_Event::OnFixedUpdate, 40, 30,
                                   reads == Reads::KeptDistance ? "kept distance after a move" :
                                                                  "property after a move"),
                0u);
    }
  }
}

/* The walk of the kind input table: an expression reaches the queries its inputs read, through
 * every family; a set field its kind was never seen to read (Not's input1, here a distance it does
 * not read) makes it unknown, and a field on the checked list as data (Is Executed's instruction)
 * reaches nothing. */
namespace {

/** A named template object for Add Object (no scene has it: these tests only compile). */
blender::Object &SparkBlenderObject()
{
  static blender::Object object;
  static const bool named = (SetCString(object.id.name, "OBSpark"), true);
  (void)named;
  return object;
}

enum class AddCase {
  Alone,
  Twice,
  AfterAMove,
  NextToAFinder,
  TemplateAlsoMeasured,
};

/* On Logic + Physics Step: a distance to the Target kept in pf1, Add Object (the template Spark,
 * at the owner), then a move; the cases add what the rules refuse. */
std::shared_ptr<LN_Program> AddObjectTree(const AddCase add, std::string *r_report)
{
  TreeBuilder b;
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode &distance = b.Add("LogicNativeGetDistance", 0, 0);
  b.ObjectNamed(distance,
                "Object B",
                add == AddCase::TemplateAlsoMeasured ? &SparkBlenderObject() :
                                                       &diff::TargetBlenderObject());
  GenNode &keep = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(keep, "Property", "pf1");
  b.Link(distance, "Distance", keep, "Value");
  b.Link(step, "Out", keep, "Flow");
  GenNode &move = b.Add("LogicNativeApplyMovement");
  b.Vector(move, "Vector", 0.25f, -0.5f, 0.0f);
  GenNode &spawn = b.Add("LogicNativeAddObject");
  b.ObjectNamed(spawn, "Object to Add", &SparkBlenderObject());
  if (add == AddCase::AfterAMove) {
    b.Link(keep, "Done", move, "Flow");
    b.Link(move, "Done", spawn, "Flow");
  }
  else {
    b.Link(keep, "Done", spawn, "Flow");
    b.Link(spawn, "Out", move, "Flow");
  }
  if (add == AddCase::Twice) {
    GenNode &again = b.Add("LogicNativeAddObject");
    b.ObjectNamed(again, "Object to Add", &SparkBlenderObject());
    b.Link(move, "Done", again, "Flow");
  }
  if (add == AddCase::NextToAFinder) {
    /* An object found by name (it could be a copy made earlier in the tick). */
    GenNode &find = b.Add("LogicNativeObjectByName");
    b.String(find, "Name", "Target");
    GenNode &copy = b.Add("LogicNativeCopyProperty");
    b.Link(find, "Object", copy, "Source");
    b.String(copy, "Property", "pf2");
    b.Link(step, "Out", copy, "Flow");
  }
  return b.Compile(r_report);
}

}  // namespace

/* Phase C1-9a: the nodes that change objects run through the bridge: Set Parent, Remove Parent,
 * Replace Mesh and Copy Property record commands for the flush; Add Object makes its copy at once,
 * so the compiler takes it only under its own rules. */
TEST(LN_Columns, TheNodesThatChangeObjectsLowerUnderTheirRules)
{
  BridgeOn bridge;
  struct Expect {
    AddCase add;
    const char *refusal; /* nullptr: lowers */
  };
  const Expect cases[] = {
      {AddCase::Alone, nullptr},
      /* (The second template, named alike, is found first: either way refused.) */
      {AddCase::Twice, "Add Object"},
      {AddCase::AfterAMove, "Add Object at the owner after a move of it"},
      {AddCase::NextToAFinder, "Add Object next to a bridged node that may see its copies"},
      {AddCase::TemplateAlsoMeasured, "Add Object of an object the program also names"},
  };
  for (const Expect &expect : cases) {
    std::string report;
    const std::shared_ptr<LN_Program> program = AddObjectTree(expect.add, &report);
    ASSERT_NE(program, nullptr) << report;
    std::string refusal;
    const bool lowers = LN_CompileColumnProgram(*program, LN_Event::OnFixedUpdate, &refusal) !=
                        nullptr;
    if (expect.refusal == nullptr) {
      EXPECT_TRUE(lowers) << int(expect.add) << ": " << refusal << "\n"
                          << LN_DumpProgramText(*program);
    }
    else {
      EXPECT_FALSE(lowers) << int(expect.add);
      EXPECT_NE(refusal.find(expect.refusal), std::string::npos)
          << int(expect.add) << ": " << refusal;
    }
  }
  /* The recorded-command nodes lower next to a move. */
  for (const char *idname : {"LogicNativeSetParent",
                             "LogicNativeRemoveParent",
                             "LogicNativeReplaceMesh",
                             "LogicNativeCopyProperty"})
  {
    TreeBuilder b;
    GenNode &step = b.Add("LogicNativeOnFixedUpdate");
    GenNode &node = b.Add(idname);
    b.Link(step, "Out", node, "Flow");
    if (std::strcmp(idname, "LogicNativeSetParent") == 0) {
      b.ObjectNamed(node, "Parent Object", &diff::TargetBlenderObject());
    }
    else if (std::strcmp(idname, "LogicNativeReplaceMesh") == 0) {
      b.ObjectNamed(node, "Mesh Object", &SparkBlenderObject());
    }
    else if (std::strcmp(idname, "LogicNativeCopyProperty") == 0) {
      b.ObjectNamed(node, "Source", &diff::TargetBlenderObject());
      b.String(node, "Property", "pf2");
    }
    GenNode &move = b.Add("LogicNativeApplyMovement");
    b.Vector(move, "Vector", 0.25f, -0.5f, 0.0f);
    b.Link(node, std::strcmp(idname, "LogicNativeRemoveParent") == 0 ? "Out" : "Done", move,
           "Flow");
    std::string report;
    const std::shared_ptr<LN_Program> program = b.Compile(&report);
    ASSERT_NE(program, nullptr) << idname << ": " << report;
    std::string refusal;
    EXPECT_NE(LN_CompileColumnProgram(*program, LN_Event::OnFixedUpdate, &refusal), nullptr)
        << idname << ": " << refusal << "\n"
        << LN_DumpProgramText(*program);
  }
}

/* Copy Property in columns copies, at the flush, what the VM's copy would copy: the Target's pf2 into
 * each NPC, next to number writes of the same property (a merge run ends at the copy). */
TEST(LN_Columns, CopyPropertyEndsAsOnTheLeanVM)
{
  BridgeOn bridge;
  TreeBuilder b;
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode &before = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(before, "Property", "pf2");
  b.Float(before, "Value", 1.5f);
  b.Link(step, "Out", before, "Flow");
  GenNode &copy = b.Add("LogicNativeCopyProperty");
  b.ObjectNamed(copy, "Source", &diff::TargetBlenderObject());
  b.String(copy, "Property", "pf2");
  b.Link(before, "Done", copy, "Flow");
  GenNode &grow = b.Add("LogicNativeModifyProperty");
  b.String(grow, "Property", "pf0");
  b.Float(grow, "Value", 0.5f);
  b.Link(copy, "Done", grow, "Flow");
  std::string report;
  const std::shared_ptr<LN_Program> program = b.Compile(&report);
  ASSERT_NE(program, nullptr) << report;
  const auto target_value = [](SemRig &rig) {
    for (size_t index = 0; index < rig.ObjectCount(); index++) {
      if (rig.Object(index).GetName() == "Target") {
        rig.SetFloat(rig.Object(index), "pf2", 7.25f);
      }
    }
  };
  std::string refusal;
  ASSERT_NE(LN_CompileColumnProgram(*program, LN_Event::OnFixedUpdate, &refusal), nullptr)
      << refusal;
  EXPECT_GT(ExpectColumnsMatch(program, LN_Event::OnFixedUpdate, 40, 10, "copy property", 0,
                               nullptr, target_value),
            0u);
}

TEST(LN_Columns, TheQueryWalkFollowsTheRecordedInputsAndDistrustsOthers)
{
  std::string report;
  const std::shared_ptr<LN_Program> program = BridgedAfterMoveTree(Reads::KeptDistance, &report);
  ASSERT_NE(program, nullptr) << report;
  uint32_t distance = LN_INVALID_INDEX;
  const std::vector<LN_FloatExpression> &floats = program->GetFloatExpressions();
  for (uint32_t index = 0; index < floats.size(); index++) {
    if (floats[index].kind == LN_FloatExpressionKind::QueryDistance) {
      distance = index;
    }
  }
  ASSERT_NE(distance, LN_INVALID_INDEX) << LN_DumpProgramText(*program);
  const uint32_t query = floats[distance].input0;
  const std::string reached = "q" + std::to_string(query);
  EXPECT_EQ(LN_ColumnQueryReachForTests(*program, 'F', distance), reached);
  EXPECT_EQ(LN_ColumnQueryReachForTests(*program, 'Q', query), reached);
  /* Through a value, a compare, a Not. */
  LN_ValueExpression value;
  value.kind = LN_ValueExpressionKind::FromFloat;
  value.input0 = distance;
  const uint32_t value_index = program->AddValueExpression(value);
  EXPECT_EQ(LN_ColumnQueryReachForTests(*program, 'A', value_index), reached);
  LN_BoolExpression less;
  less.kind = LN_BoolExpressionKind::FloatCompare;
  less.input0 = distance;
  LN_FloatExpression six;
  six.kind = LN_FloatExpressionKind::Constant;
  six.float_value = 6.0f;
  less.input1 = program->AddFloatExpression(six);
  const uint32_t less_index = program->AddBoolExpression(less);
  EXPECT_EQ(LN_ColumnQueryReachForTests(*program, 'B', less_index), reached);
  /* A property read (a constant input would be folded away with the Not). */
  LN_BoolExpression property;
  property.kind = LN_BoolExpressionKind::SnapshotGameProperty;
  property.property_ref_index = 0;
  const uint32_t property_index = program->AddBoolExpression(property);
  LN_BoolExpression negation;
  negation.kind = LN_BoolExpressionKind::Not;
  negation.input0 = less_index;
  EXPECT_EQ(LN_ColumnQueryReachForTests(*program, 'B', program->AddBoolExpression(negation)),
            reached);
  negation.input0 = property_index;
  EXPECT_EQ(LN_ColumnQueryReachForTests(*program, 'B', program->AddBoolExpression(negation)), "");
  /* A field Not never reads, set: the walk cannot know it is not read. */
  negation.input1 = distance;
  EXPECT_EQ(LN_ColumnQueryReachForTests(*program, 'B', program->AddBoolExpression(negation)),
            "unknown: Bool::Not's input1");
  /* Is Executed keeps an instruction number in input0 (the checked list): nothing read. */
  LN_BoolExpression ran;
  ran.kind = LN_BoolExpressionKind::InstructionExecuted;
  ran.input0 = distance;
  EXPECT_EQ(LN_ColumnQueryReachForTests(*program, 'B', program->AddBoolExpression(ran)), "");
  /* A kind the table records only through its state. */
  LN_FloatExpression tween;
  tween.kind = LN_FloatExpressionKind::TweenFactor;
  EXPECT_EQ(LN_ColumnQueryReachForTests(*program, 'F', program->AddFloatExpression(tween)),
            "unknown: Float::TweenFactor (reads through its state)");
}

namespace {

/* On Logic + Physics Step: the distance to the Target kept in pf4, then a move (native). */
std::shared_ptr<LN_Program> NativeMoverTree(std::string *r_report)
{
  TreeBuilder b;
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode &distance = b.Add("LogicNativeGetDistance", 0, 0);
  b.ObjectNamed(distance, "Object B", &diff::TargetBlenderObject());
  GenNode &keep = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(keep, "Property", "pf4");
  b.Link(distance, "Distance", keep, "Value");
  b.Link(step, "Out", keep, "Flow");
  GenNode &move = b.Add("LogicNativeApplyMovement");
  b.Vector(move, "Vector", -0.125f, 0.375f, 0.0f);
  b.Link(keep, "Done", move, "Flow");
  return b.Compile(r_report);
}

/* On Logic + Physics Step: bridged nodes that read nothing of the owner's pose (Switch Flow on a
 * property, On Value Changed of a property). */
std::shared_ptr<LN_Program> BridgedNoOwnerTree(std::string *r_report)
{
  TreeBuilder b;
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode &pi1 = b.Add("LogicNativeGetGamePropertyInt");
  b.String(pi1, "Property", "pi1");
  GenNode &state = b.Add("LogicNativeSwitchFlowInt", 0, 0, 2);
  b.Link(pi1, "Value", state, "Index");
  b.Link(step, "Out", state, "Flow");
  GenNode &one = b.Add("LogicNativeSetGamePropertyInt");
  b.String(one, "Property", "pi1");
  b.Int(one, "Value", 1);
  b.Link(state, "Then 0", one, "Flow");
  GenNode &zero = b.Add("LogicNativeSetGamePropertyInt");
  b.String(zero, "Property", "pi1");
  b.Int(zero, "Value", 0);
  b.Link(state, "Then 1", zero, "Flow");
  GenNode &pf4 = b.Add("LogicNativeGetGamePropertyFloat");
  b.String(pf4, "Property", "pf4");
  GenNode &changed = b.Add("LogicNativeValueChanged");
  b.Link(pf4, "Value", changed, "Value");
  GenNode &old = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(old, "Property", "pf5");
  b.Link(changed, "Old", old, "Value");
  b.Link(changed, "If Changed", old, "Flow");
  return b.Compile(r_report);
}

}  // namespace

/* Plan C1-9: objects with two trees run in groups (their first trees for every object, then
 * their second trees) and end as on the lean VM: a bridged second tree that reads nothing of its
 * owner next to the first tree's pending moves; a native second tree measuring from its owner
 * after the first moved it (the run applies the move, as the VM's read does); a bridged second
 * tree measuring from its owner (it gives its NPCs back to the VM). */
TEST(LN_Columns, ObjectsWithTwoTreesRunInGroupsAndEndAsOnTheLeanVM)
{
  BridgeOn bridge;
  std::string report;
  const std::shared_ptr<LN_Program> mover = NativeMoverTree(&report);
  ASSERT_NE(mover, nullptr) << report;
  const std::shared_ptr<LN_Program> no_owner = BridgedNoOwnerTree(&report);
  ASSERT_NE(no_owner, nullptr) << report;
  const std::shared_ptr<LN_Program> bridged_mover = BridgedAfterMoveTree(Reads::KeptDistance,
                                                                         &report);
  ASSERT_NE(bridged_mover, nullptr) << report;
  struct Pair {
    std::shared_ptr<LN_Program> first, second;
    const char *what;
    bool falls_back;
  };
  const Pair pairs[] = {
      {mover, no_owner, "native mover, bridged reader of nothing of the owner", false},
      {bridged_mover, mover, "bridged mover, native distance reader", false},
      {mover, bridged_mover, "native mover, bridged distance reader", true},
  };
  constexpr uint32_t kOwners = 40;
  for (const Pair &pair : pairs) {
    for (const uint32_t helpers : {0u, 3u}) {
      const auto two_trees = [&](SemRig &rig) { rig.AddSecondTrees(pair.second, kOwners); };
      const uint64_t fallbacks = LN_ColumnBridgeFallbackCount();
      const uint64_t lanes = ExpectColumnsMatch(pair.first,
                                                LN_Event::OnFixedUpdate,
                                                kOwners,
                                                20,
                                                std::string(pair.what) + (helpers ? " (3 helpers)" : ""),
                                                helpers,
                                                nullptr,
                                                two_trees);
      EXPECT_GT(lanes, 0u) << pair.what;
      if (pair.falls_back) {
        if (!DirectForced()) {
          EXPECT_GT(LN_ColumnBridgeFallbackCount(), fallbacks) << pair.what;
        }
      }
    }
  }
}

namespace {

enum class Found { Chase, PoseFirst, AfterAMove };

/* On Logic + Physics Step: the object named in pf-free "Target" text found while playing (Get
 * Object By Name), its distance kept in pf1, its X in pf2 (a live pose read), then a move. */
std::shared_ptr<LN_Program> FoundObjectTree(const Found found,
                                            const char *name,
                                            std::string *r_report)
{
  TreeBuilder b;
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode &find = b.Add("LogicNativeObjectByName");
  b.String(find, "Name", name);
  GenNode &distance = b.Add("LogicNativeGetDistance", 0, 0);
  b.Link(find, "Object", distance, "Object B");
  GenNode &keep = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(keep, "Property", "pf1");
  b.Link(distance, "Distance", keep, "Value");
  GenNode &pose = b.Add("LogicNativeGetObjectAttribute", 0);
  b.Link(find, "Object", pose, "Object");
  GenNode &split = b.Add("LogicNativeSeparateXYZ");
  b.Link(pose, "Vector", split, "Vector");
  GenNode &x = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(x, "Property", "pf2");
  b.Link(split, "X", x, "Value");
  GenNode &move = b.Add("LogicNativeApplyMovement");
  b.Vector(move, "Vector", 0.25f, -0.125f, 0.0f);
  switch (found) {
    case Found::Chase:
      b.Link(step, "Out", keep, "Flow");
      b.Link(keep, "Done", x, "Flow");
      b.Link(x, "Done", move, "Flow");
      break;
    case Found::PoseFirst:
      b.Link(step, "Out", x, "Flow");
      b.Link(x, "Done", keep, "Flow");
      b.Link(keep, "Done", move, "Flow");
      break;
    case Found::AfterAMove:
      b.Link(step, "Out", move, "Flow");
      b.Link(move, "Done", keep, "Flow");
      b.Link(keep, "Done", x, "Flow");
      break;
  }
  return b.Compile(r_report);
}

}  // namespace

/* Plan C1-9: objects found while playing (here by name) as distance ends and live pose reads. The
 * run finds each NPC's object when it starts and applies its pending moves there (a tree before
 * the crowd pushes the Target), as the first NPC's read would; an object that is a lane's own
 * (the NPCs chase each other) gives the run back to the VM. A pose read before a sure distance to
 * the same object, and a distance from the owner after a move of it, are refused. */
TEST(LN_Columns, ObjectsFoundWhilePlayingEndAsOnTheLeanVM)
{
  std::string report;
  const std::shared_ptr<LN_Program> chase = FoundObjectTree(Found::Chase, "Target", &report);
  ASSERT_NE(chase, nullptr) << report;
  std::string refusal;
  ASSERT_NE(LN_CompileColumnProgram(*chase, LN_Event::OnFixedUpdate, &refusal), nullptr)
      << refusal << "\n" << LN_DumpProgramText(*chase);
  EXPECT_GT(ExpectColumnsMatch(chase, LN_Event::OnFixedUpdate, 40, 20, "chase"), 0u);
  const std::shared_ptr<LN_Program> push = OtherTree(Other::Push, true);
  ASSERT_NE(push, nullptr);
  const auto pushed = [&](SemRig &rig) {
    KX_GameObject &object = rig.AddObject("Pusher" + std::to_string(rig.ObjectCount()),
                                          MT_Vector3(1.0f, -2.0f, 0.0f));
    rig.SetString(object, "target", "Target");
    rig.AddTree(push, rig.ObjectCount() - 1, 0, true);
  };
  EXPECT_GT(ExpectColumnsMatch(chase, LN_Event::OnFixedUpdate, 40, 20, "chase, Target pushed", 3,
                               nullptr, pushed),
            0u);
  /* Each NPC chases Obj3, a lane's object: back to the VM. */
  const std::shared_ptr<LN_Program> each_other = FoundObjectTree(Found::Chase, "Obj3", &report);
  ASSERT_NE(each_other, nullptr) << report;
  const uint64_t fallbacks = LN_ColumnRuntimeFallbackCount();
  ExpectColumnsMatch(each_other, LN_Event::OnFixedUpdate, 40, 10, "chase a lane's object");
  if (!DirectForced()) {
    EXPECT_GT(LN_ColumnRuntimeFallbackCount(), fallbacks);
  }
  /* A pose read before a sure distance to the same object: since plan C2 fix 1 the run starts
   * quietly (the Target pushed before the crowd gives the run back, as the VM's first read
   * would apply the push between the NPCs' reads). */
  {
    const std::shared_ptr<LN_Program> program = FoundObjectTree(Found::PoseFirst, "Target",
                                                                &report);
    ASSERT_NE(program, nullptr) << report;
    ASSERT_NE(LN_CompileColumnProgram(*program, LN_Event::OnFixedUpdate, &refusal), nullptr)
        << refusal << "\n" << LN_DumpProgramText(*program);
    EXPECT_GT(ExpectColumnsMatch(program, LN_Event::OnFixedUpdate, 40, 20, "pose first"), 0u);
    const uint64_t back = LN_ColumnRuntimeFallbackCount();
    ExpectColumnsMatch(program, LN_Event::OnFixedUpdate, 40, 20, "pose first, Target pushed", 3,
                       nullptr, pushed);
    if (!DirectForced()) {
      EXPECT_GT(LN_ColumnRuntimeFallbackCount(), back);
    }
  }
  const std::shared_ptr<LN_Program> program = FoundObjectTree(Found::AfterAMove, "Target",
                                                              &report);
  ASSERT_NE(program, nullptr) << report;
  EXPECT_EQ(LN_CompileColumnProgram(*program, LN_Event::OnFixedUpdate, &refusal), nullptr);
  EXPECT_NE(refusal.find("after a move of it"), std::string::npos) << refusal;
}

namespace {

/* On Logic + Physics Step: a Loop of pi0 iterations (over `ticks` ticks) whose body writes its
 * Index and adds to pf0 (behind a Branch on the index), its Completed route writes pi4 and moves
 * the owner (a column op behind the loop's completion); a distance kept before it. */
std::shared_ptr<LN_Program> LoopTree(const int ticks, std::string *r_report)
{
  TreeBuilder b;
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode &distance = b.Add("LogicNativeGetDistance", 0, 0);
  b.ObjectNamed(distance, "Object B", &diff::TargetBlenderObject());
  GenNode &keep = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(keep, "Property", "pf1");
  b.Link(distance, "Distance", keep, "Value");
  b.Link(step, "Out", keep, "Flow");
  GenNode &count = b.Add("LogicNativeGetGamePropertyInt");
  b.String(count, "Property", "pi0");
  GenNode &loop = b.Add("LogicNativeLoop");
  b.Link(count, "Value", loop, "Count");
  b.Int(loop, "Ticks to Complete", ticks);
  b.Link(keep, "Done", loop, "Flow");
  GenNode &index = b.Add("LogicNativeSetGamePropertyInt");
  b.String(index, "Property", "pi3");
  b.Link(loop, "Index", index, "Value");
  b.Link(loop, "Loop", index, "Flow");
  GenNode &odd = b.Add("LogicNativeGetGamePropertyBool");
  b.String(odd, "Property", "pb0");
  GenNode &branch = b.Add("LogicNativeBranch");
  b.Link(odd, "Value", branch, "Condition");
  b.Link(index, "Done", branch, "Flow");
  GenNode &grow = b.Add("LogicNativeModifyProperty");
  b.String(grow, "Property", "pf0");
  b.Float(grow, "Value", 0.5f);
  b.Link(branch, "True", grow, "Flow");
  GenNode &done = b.Add("LogicNativeSetGamePropertyInt");
  b.String(done, "Property", "pi4");
  b.Int(done, "Value", 7);
  b.Link(loop, "Completed", done, "Flow");
  GenNode &move = b.Add("LogicNativeApplyMovement");
  b.Vector(move, "Vector", 0.25f, 0.0f, 0.125f);
  b.Link(done, "Done", move, "Flow");
  return b.Compile(r_report);
}

}  // namespace

/* Plan C1-9: a Loop's body runs through the VM's own block walk NPC by NPC inside the column run
 * (its iterations, budget and ticks to complete the VM's), its Completed route in columns behind
 * the loop's completion; the world and the trees' states end as on the lean VM. */
TEST(LN_Columns, LoopsRunThroughTheVMsWalkAndEndAsOnTheLeanVM)
{
  BridgeOn bridge;
  for (const int ticks : {1, 3}) {
    std::string report;
    const std::shared_ptr<LN_Program> program = LoopTree(ticks, &report);
    ASSERT_NE(program, nullptr) << report;
    std::string refusal;
    ASSERT_NE(LN_CompileColumnProgram(*program, LN_Event::OnFixedUpdate, &refusal), nullptr)
        << refusal << "\n"
        << LN_DumpProgramText(*program);
    for (const uint32_t helpers : {0u, 3u}) {
      EXPECT_GT(ExpectColumnsMatch(program,
                                   LN_Event::OnFixedUpdate,
                                   helpers ? 600 : 40,
                                   12,
                                   "loop over " + std::to_string(ticks) + " ticks" +
                                       (helpers ? " (3 helpers)" : ""),
                                   helpers),
                0u);
    }
  }
}

/* A named object another tree moved earlier in the step (the player): the bridged run plays its
 * first tree on the VM, whose distance read applies the move where the VM applies it, and runs
 * the others in columns. Pushes of the Target come before the crowd and after it. */
TEST(LN_Columns, ANamedObjectsPendingMoveIsAppliedByTheRunsFirstTreeOnTheVM)
{
  BridgeOn bridge;
  std::string report;
  const std::shared_ptr<LN_Program> program = BridgedAfterMoveTree(Reads::KeptDistance, &report);
  ASSERT_NE(program, nullptr) << report;
  const std::shared_ptr<LN_Program> push = OtherTree(Other::Push, true);
  ASSERT_NE(push, nullptr);
  for (const bool before : {true, false}) {
    const auto extra = [&](SemRig &rig) {
      KX_GameObject &object = rig.AddObject("Pusher" + std::to_string(rig.ObjectCount()),
                                            MT_Vector3(1.0f, -2.0f, 0.0f));
      rig.SetString(object, "target", "Target");
      rig.AddTree(push, rig.ObjectCount() - 1, before ? 0 : 100000, before);
    };
    const uint64_t fallbacks = LN_ColumnBridgeFallbackCount();
    EXPECT_GT(ExpectColumnsMatch(program, LN_Event::OnFixedUpdate, 40, 20,
                                 before ? "Target pushed first" : "Target pushed last", 0, nullptr,
                                 extra),
              0u);
    const uint64_t played = LN_ColumnBridgeFallbackCount() - fallbacks;
    if (before) {
      if (!DirectForced()) {
        EXPECT_GT(played, 0u) << "no tree played on the VM for the Target's move";
      }
      EXPECT_LE(played, 20u) << "more than the run's first tree played on the VM";
    }
  }
}

/* A bridged run whose NPCs another tree pushed earlier in the tick (pending moves the VM's code
 * would apply where it reads them) is given back to the VM: the same world, no lanes run then. */
TEST(LN_Columns, ABridgedRunWithPendingMovesFallsBackToTheVM)
{
  BridgeOn bridge;
  std::string report;
  const std::shared_ptr<LN_Program> program = BridgedTree(&report);
  ASSERT_NE(program, nullptr) << report;
  const std::shared_ptr<LN_Program> push = OtherTree(Other::Push, false);
  ASSERT_NE(push, nullptr);
  constexpr uint32_t kOwners = 40;
  const auto extra = [&](SemRig &rig) {
    KX_GameObject &object = rig.AddObject("Pusher" + std::to_string(rig.ObjectCount()),
                                          MT_Vector3(1.0f, -2.0f, 0.0f));
    rig.SetString(object, "target", "Obj7");
    rig.AddTree(push, rig.ObjectCount() - 1, 1, true);
  };
  /* It reads nothing of its owner's pose (plan C1-9): the pushed NPCs run in columns, the push
   * left pending as the VM's pass leaves it. */
  uint64_t before = LN_ColumnBridgeFallbackCount();
  EXPECT_GT(
      ExpectColumnsMatch(program, LN_Event::OnUpdate, kOwners, 10, "pushed", 0, nullptr, extra, 2),
      0u);
  EXPECT_EQ(LN_ColumnBridgeFallbackCount(), before) << "a run that reads nothing of its owner fell back";
  /* One that measures from its owner is given back to the VM. */
  const std::shared_ptr<LN_Program> measures = BridgedAfterMoveTree(Reads::KeptDistance, &report);
  ASSERT_NE(measures, nullptr) << report;
  const std::shared_ptr<LN_Program> step_push = OtherTree(Other::Push, true);
  ASSERT_NE(step_push, nullptr);
  const auto step_extra = [&](SemRig &rig) {
    KX_GameObject &object = rig.AddObject("Pusher" + std::to_string(rig.ObjectCount()),
                                          MT_Vector3(1.0f, -2.0f, 0.0f));
    rig.SetString(object, "target", "Obj7");
    rig.AddTree(step_push, rig.ObjectCount() - 1, 1, true);
  };
  before = LN_ColumnBridgeFallbackCount();
  const uint64_t direct_before = LN_ColumnDirectLaneCount();
  ExpectColumnsMatch(
      measures, LN_Event::OnFixedUpdate, kOwners, 10, "fallback", 0, nullptr, step_extra, 2);
  /* Plan C2 P6: the lanes a run gives back run as direct passes in their places. */
  if (LN_GetColumnDirectMode() != LN_ColumnDirectMode::Off) {
    EXPECT_GT(LN_ColumnDirectLaneCount(), direct_before) << "no given-back lane ran direct";
  }
  if (!DirectForced()) {
    EXPECT_GT(LN_ColumnBridgeFallbackCount(), before) << "the pushed run never fell back";
  }
}

namespace {

/** A named object no scene has (a Get Distance end that never resolves). */
blender::Object &GhostBlenderObject()
{
  static blender::Object object;
  static const bool named = (SetCString(object.id.name, "OBGhost"), true);
  (void)named;
  return object;
}

}  // namespace

/* The VM reads a distance's end B only when end A is valid: from an object no scene has to the
 * Target, the Target's pending moves (a tree before the crowd pushes it) are not applied by the
 * crowd, so a tree after the crowd reads the Target's live pose before the push. */
TEST(LN_Columns, ADistanceWhoseFirstEndFailsReadsNothingOfTheSecond)
{
  TreeBuilder crowd_b;
  {
    GenNode &event = crowd_b.Add("LogicNativeOnUpdate");
    GenNode &distance = crowd_b.Add("LogicNativeGetDistance", 0, 0);
    crowd_b.ObjectNamed(distance, "Object A", &GhostBlenderObject());
    crowd_b.ObjectNamed(distance, "Object B", &diff::TargetBlenderObject());
    GenNode &store = crowd_b.Add("LogicNativeSetGamePropertyFloat");
    crowd_b.String(store, "Property", "dist");
    crowd_b.Link(distance, "Distance", store, "Value");
    crowd_b.Link(event, "Out", store, "Flow");
  }
  std::string report;
  const std::shared_ptr<LN_Program> crowd = crowd_b.Compile(&report);
  ASSERT_NE(crowd, nullptr) << report;
  std::string refusal;
  ASSERT_NE(LN_CompileColumnProgram(*crowd, LN_Event::OnUpdate, &refusal), nullptr) << refusal;

  /* The pusher moves the Target (found by name at run time); the reader keeps its live x. */
  const auto other = [](const bool push) {
    TreeBuilder b;
    GenNode &event = b.Add("LogicNativeOnUpdate");
    GenNode &name = b.Add("LogicNativeGetGamePropertyString");
    b.String(name, "Property", "target");
    GenNode &found = b.Add("LogicNativeObjectByName");
    b.Link(name, "Value", found, "Name");
    if (push) {
      GenNode &move = b.Add("LogicNativeApplyMovement");
      b.Link(found, "Object", move, "Object");
      b.Vector(move, "Vector", 0.5f, 0.0f, 0.0f);
      b.Link(event, "Out", move, "Flow");
    }
    else {
      GenNode &pose = b.Add("LogicNativeGetObjectAttribute", 0);
      b.Link(found, "Object", pose, "Object");
      GenNode &split = b.Add("LogicNativeSeparateXYZ");
      b.Link(pose, "Vector", split, "Vector");
      GenNode &store = b.Add("LogicNativeSetGamePropertyFloat");
      b.String(store, "Property", "seen");
      b.Link(split, "X", store, "Value");
      b.Link(event, "Out", store, "Flow");
    }
    std::string text;
    std::shared_ptr<LN_Program> program = b.Compile(&text);
    EXPECT_NE(program, nullptr) << text;
    return program;
  };
  const std::shared_ptr<LN_Program> pusher = other(true);
  const std::shared_ptr<LN_Program> reader = other(false);
  ASSERT_TRUE(pusher && reader);
  constexpr uint32_t kOwners = 40;
  const auto extra = [&](SemRig &rig) {
    const struct {
      const std::shared_ptr<LN_Program> *program;
      uint32_t scene_index;
      bool first;
    } placements[] = {{&pusher, 1, true}, {&reader, 999, false}};
    for (const auto &placement : placements) {
      KX_GameObject &object = rig.AddObject("Other" + std::to_string(rig.ObjectCount()),
                                            MT_Vector3(1.0f, -2.0f, 0.0f));
      rig.SetString(object, "target", "Target");
      rig.SetFloat(object, "seen", -1.0f);
      rig.AddTree(*placement.program, rig.ObjectCount() - 1, placement.scene_index, placement.first);
    }
  };
  EXPECT_GT(ExpectColumnsMatch(crowd, LN_Event::OnUpdate, kOwners, 6, "ghost to target", 0, nullptr, extra, 2),
            0u);
}

/* Behind an Every N Steps gate a route reads its gate's unscaled delta (While Paused's Delta
 * Time), not the step's. */
TEST(LN_Columns, AGatedRouteReadsItsGatesUnscaledDelta)
{
  TreeBuilder b;
  GenNode &event = b.Add("LogicNativeOnFixedUpdate", 3, 1);
  GenNode &paused = b.Add("LogicNativeWhilePaused");
  GenNode &store = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(store, "Property", "pf1");
  b.Link(paused, "Delta Time", store, "Value");
  b.Link(event, "Out", store, "Flow");
  std::string report;
  const std::shared_ptr<LN_Program> program = b.Compile(&report);
  ASSERT_NE(program, nullptr) << report;
  std::string refusal;
  ASSERT_NE(LN_CompileColumnProgram(*program, LN_Event::OnFixedUpdate, &refusal), nullptr)
      << refusal;
  EXPECT_GT(ExpectColumnsMatch(program, LN_Event::OnFixedUpdate, 30, 9, "gated unscaled delta"), 0u);
}

/* A tool, not a check (run with --gtest_also_run_disabled_tests): plan C2 P0's coverage scan.
 * Every node of the descriptors is built into a small tree in a working shape and asked whether
 * it runs in columns, and how. Its flow comes from an On Logic + Physics Step (an event node is
 * the root itself; Break Loop sits in a Loop body), its number, bool and vector inputs from live
 * property reads (so nothing folds), lists from List From Items, dictionaries from Make Dict,
 * asset pins (collection, material, image, sound, font, text) get an asset, a Speaker an object,
 * an empty text a name, and its data outputs of any type go one at a time into a node that uses
 * it. The sockets are the editor's (TreeBuilder::SetRealSockets: collision layers are ints). When a
 * shape does not compile, or leaves no work, the next is tried: defaults instead of live reads,
 * flow outputs unlinked, other data outputs, custom1 = 1..12 (the node's mode). A node counts only
 * when it leaves work in the program (the same tree without it has fewer instructions or
 * expressions). Verdicts: native (lowers with the bridge off), bridged (only with the bridge; "main
 * thread" when its run stays there), refused (the column compiler's reason), old system (On Init),
 * flow only, constant, nothing (compiles to no work in any shape), unavailable (no runtime in
 * either system), not built (no shape compiled). One line per node (tab separated: category, name,
 * idname, verdict, detail), then the counts; UPBGE_LN_SCAN_OUT=file writes the lines there too,
 * UPBGE_LN_SCAN_DUMP=part-of-idname prints the program of matching nodes. */
TEST(LN_Columns, DISABLED_ColumnCoverageScan)
{
  using blender::nodes::logic::NodeMetadata;
  using blender::nodes::logic::PinMetadata;
  using blender::nodes::logic::PinType;
  /* The assets pins get (an ID is every asset's first member, and the compiler reads names only). */
  static blender::ID assets[9];
  static const bool named = [] {
    const char *const names[9] = {"GRScanCollection",
                                  "MAScanMaterial",
                                  "IMScanImage",
                                  "SOScanSound",
                                  "VFScanFont",
                                  "TXScanText",
                                  "OBScanSpeaker",
                                  "NTScanTree",
                                  "ACScanAction"};
    for (int index = 0; index < 9; index++) {
      SetCString(assets[index].name, names[index]);
    }
    return true;
  }();
  (void)named;
  const auto asset_for = [](const PinType type) -> blender::ID * {
    switch (type) {
      case PinType::Collection:
        return &assets[0];
      case PinType::Material:
        return &assets[1];
      case PinType::Image:
        return &assets[2];
      case PinType::Sound:
        return &assets[3];
      case PinType::Font:
        return &assets[4];
      case PinType::Text:
        return &assets[5];
      default:
        return nullptr;
    }
  };
  /* The ops a node of `idname` made that do something: not a Nop, not a branch on a constant or
   * on an op having run (a node's Done route). */
  const auto ops_of = [](const LN_Program &program, const std::string &idname) {
    size_t size = 0;
    const std::vector<LN_BoolExpression> &bools = program.GetBoolExpressions();
    const std::vector<LN_SourceRef> &refs = program.GetSourceRefs();
    for (const LN_Event event : {LN_Event::OnInit, LN_Event::OnUpdate, LN_Event::OnFixedUpdate}) {
      for (const LN_InstructionHeader &header : program.GetInstructionHeaders(event)) {
        const LN_Instruction &instruction = program.GetInstructionPayload(event, header);
        if (instruction.source_ref_index >= refs.size() ||
            refs[instruction.source_ref_index].node_idname != idname)
        {
          continue;
        }
        if (instruction.opcode == LN_OpCode::Nop) {
          continue;
        }
        if (instruction.opcode == LN_OpCode::BranchRoute &&
            (instruction.bool_expr_index >= bools.size() ||
             bools[instruction.bool_expr_index].kind == LN_BoolExpressionKind::Constant ||
             bools[instruction.bool_expr_index].kind == LN_BoolExpressionKind::InstructionExecuted))
        {
          continue;
        }
        size++;
      }
    }
    return size;
  };
  /* Expressions that are not constants (a constant node, or a folded one, adds none). */
  const auto live_expressions_of = [](const LN_Program &program) {
    size_t size = program.GetQueryExpressions().size();
    const auto count = [&](const auto &expressions, const auto constant) {
      for (const auto &expression : expressions) {
        size += expression.kind != constant ? 1 : 0;
      }
    };
    count(program.GetBoolExpressions(), LN_BoolExpressionKind::Constant);
    count(program.GetIntExpressions(), LN_IntExpressionKind::Constant);
    count(program.GetFloatExpressions(), LN_FloatExpressionKind::Constant);
    count(program.GetStringExpressions(), LN_StringExpressionKind::Constant);
    count(program.GetVectorExpressions(), LN_VectorExpressionKind::Constant);
    count(program.GetColorExpressions(), LN_ColorExpressionKind::Constant);
    count(program.GetValueExpressions(), LN_ValueExpressionKind::Constant);
    return size;
  };
  /* Expressions other than the scaffold's own (property reads, constants, a combined vector,
   * lists and dictionaries made for inputs, generic wrappers, Done routes' checks), and queries:
   * what a flow node that makes no op of its own may still leave (a Loop's index, a list
   * operation's result). */
  const auto own_expressions_of = [](const LN_Program &program) {
    std::istringstream text(LN_DumpProgramText(program));
    std::string line;
    size_t size = 0;
    while (std::getline(text, line)) {
      if (line.size() < 6 || line.compare(0, 3, "  [") != 0) {
        continue;
      }
      const size_t space = line.find("] ");
      const size_t colons = line.find("::");
      if (space == std::string::npos || colons == std::string::npos || colons < space) {
        continue;
      }
      const std::string family = line.substr(space + 2, colons - space - 2);
      const size_t end = line.find_first_of(" \n", colons + 2);
      const std::string kind = line.substr(colons + 2, end == std::string::npos ? std::string::npos :
                                                                                  end - colons - 2);
      if (family == "Op" || kind == "SnapshotGameProperty" || kind == "Constant" ||
          kind == "InstructionExecuted" || (family == "Vector" && kind == "Combine") ||
          (family == "Value" && (kind == "ListFromItems" || kind == "MakeDict" ||
                                 kind.compare(0, 4, "From") == 0)) ||
          (family == "Rotation" && kind == "Euler"))
      {
        continue;
      }
      size++;
    }
    return size;
  };
  struct Attempt {
    bool live = true;
    bool routes = true;
    /** The data output used; one past the last: none (a flow node needs no consumer). */
    size_t output = 0;
    int16_t custom1 = 0;
    /** The node's own data-block (Play Sound's sound, Set Node Mute's tree...): an asset index. */
    int node_id = -1;
    /** custom2 (some nodes keep a mode there: Set Editor Node Value's editor in bits 4-5). */
    int16_t custom2 = 0;
  };
  /* An event root: an Events node without a flow input (Send Event and Do Once take flow). */
  const auto is_event_root = [](const NodeMetadata &meta) {
    if (std::string(meta.category) != "Events") {
      return false;
    }
    for (const PinMetadata &pin : meta.inputs) {
      if (pin.type == PinType::Execution) {
        return false;
      }
    }
    return true;
  };
  /* `with_node` false: the same scaffold without the node (its consumers left unlinked). */
  const auto build = [&](const NodeMetadata &meta,
                         const Attempt &attempt,
                         const bool with_node,
                         std::string *r_report) -> std::shared_ptr<LN_Program> {
    const std::string idname = meta.idname;
    const bool is_event = is_event_root(meta);
    TreeBuilder b;
    b.SetRealSockets(true);
    GenNode *root = nullptr;
    GenNode *node = nullptr;
    if (is_event) {
      if (!with_node) {
        return b.Compile(r_report);
      }
      root = node = &b.Add(meta.idname, attempt.custom1, attempt.custom2);
    }
    else {
      root = &b.Add("LogicNativeOnFixedUpdate");
      if (with_node) {
        node = &b.Add(meta.idname, attempt.custom1, attempt.custom2);
      }
    }
    if (node != nullptr && attempt.node_id >= 0) {
      node->node.id = &assets[attempt.node_id];
    }
    const char *root_out = nullptr;
    {
      const NodeMetadata *root_meta = blender::nodes::logic::find_logic_node_metadata(
          is_event ? meta.idname : "LogicNativeOnFixedUpdate");
      for (const PinMetadata &pin : root_meta->outputs) {
        if (pin.type == PinType::Execution && !pin.is_variadic) {
          root_out = pin.identifier;
          break;
        }
      }
    }
    /* The node's flow: the root's, or a Loop body's for Break Loop. */
    GenNode *flow_from = root;
    const char *flow_out = root_out;
    if (with_node && idname == "LogicNativeBreakLoop") {
      GenNode &loop = b.Add("LogicNativeLoop");
      b.Int(loop, "Count", 3);
      b.Link(*root, root_out, loop, "Flow");
      flow_from = &loop;
      flow_out = "Loop";
    }
    const auto source = [&](const PinType type) -> std::pair<GenNode *, const char *> {
      switch (type) {
        case PinType::Int:
        case PinType::CollisionLayers: {
          GenNode &read = b.Add("LogicNativeGetGamePropertyInt");
          b.String(read, "Property", "pi0");
          return {&read, "Value"};
        }
        case PinType::Bool:
        case PinType::Condition: {
          GenNode &read = b.Add("LogicNativeGetGamePropertyBool");
          b.String(read, "Property", "pb0");
          return {&read, "Value"};
        }
        case PinType::String: {
          GenNode &read = b.Add("LogicNativeGetGamePropertyString");
          b.String(read, "Property", "ps0");
          return {&read, "Value"};
        }
        case PinType::Vector:
        case PinType::VectorXYAngle: {
          GenNode &read = b.Add("LogicNativeGetGamePropertyFloat");
          b.String(read, "Property", "pf0");
          GenNode &combine = b.Add("LogicNativeCombineXYZ");
          b.Link(read, "Value", combine, "X");
          return {&combine, "Vector"};
        }
        case PinType::Float:
        case PinType::Angle:
        case PinType::Generic: {
          GenNode &read = b.Add("LogicNativeGetGamePropertyFloat");
          b.String(read, "Property", "pf0");
          return {&read, "Value"};
        }
        case PinType::List: {
          GenNode &list = b.Add("LogicNativeListFromItems", 2); /* custom1 2: Integers */
          GenNode &read = b.Add("LogicNativeGetGamePropertyInt");
          b.String(read, "Property", "pi0");
          b.Link(read, "Value", list, "Integers");
          return {&list, "Integers"};
        }
        case PinType::Rotation: {
          GenNode &read = b.Add("LogicNativeGetGamePropertyFloat");
          b.String(read, "Property", "pf0");
          GenNode &euler = b.Add("LogicNativeEuler");
          b.Link(read, "Value", euler, "X");
          return {&euler, "Rotation"};
        }
        case PinType::Dictionary: {
          GenNode &dict = b.Add("LogicNativeMakeDict");
          b.String(dict, "Key", "k");
          GenNode &read = b.Add("LogicNativeGetGamePropertyFloat");
          b.String(read, "Property", "pf0");
          b.Link(read, "Value", dict, "Value");
          return {&dict, "Dictionary"};
        }
        default:
          return {nullptr, nullptr};
      }
    };
    if (node != nullptr) {
      bool flow_linked = is_event;
      for (const PinMetadata &pin : meta.inputs) {
        if (pin.type == PinType::Execution) {
          if (!flow_linked && !pin.is_variadic) {
            b.Link(*flow_from, flow_out, *node, pin.identifier);
            flow_linked = true;
          }
          continue;
        }
        if (pin.is_variadic) {
          continue;
        }
        if (blender::ID *asset = asset_for(pin.type)) {
          b.Asset(*node, pin.identifier, asset);
          continue;
        }
        if (pin.type == PinType::Object && std::string(pin.identifier) == "Speaker") {
          b.Asset(*node, pin.identifier, &assets[6]);
          continue;
        }
        if ((pin.type == PinType::String || pin.type == PinType::Scene) && !pin.requires_link &&
            (pin.default_value == nullptr || pin.default_value[0] == '\0'))
        {
          /* An empty text usually means "do nothing": give it a name. */
          const std::string name = pin.identifier;
          b.String(*node,
                   pin.identifier,
                   pin.type == PinType::Scene                 ? "ScanScene" :
                   name.find("Property") != std::string::npos ? "pf0" :
                                                                "s1");
          continue;
        }
        const bool number = pin.type == PinType::Int || pin.type == PinType::Bool ||
                            pin.type == PinType::Condition || pin.type == PinType::Float ||
                            pin.type == PinType::Angle || pin.type == PinType::Vector ||
                            pin.type == PinType::VectorXYAngle || pin.type == PinType::Rotation;
        const bool structure = pin.type == PinType::List || pin.type == PinType::Dictionary;
        if (!(pin.requires_link || pin.type == PinType::Generic || structure ||
              (attempt.live && number)))
        {
          continue;
        }
        const auto from = source(pin.type);
        if (from.first != nullptr && from.second != nullptr) {
          b.Link(*from.first, from.second, *node, pin.identifier);
        }
      }
    }
    /* Each flow output drives a write (so a flow node's routes exist and keep their states). */
    int route = 0;
    if (node != nullptr && attempt.routes) {
      for (const PinMetadata &pin : meta.outputs) {
        if (pin.type != PinType::Execution || pin.is_variadic ||
            (is_event && pin.identifier == std::string(root_out)))
        {
          continue;
        }
        GenNode &write = b.Add("LogicNativeSetGamePropertyFloat");
        b.String(write, "Property", "pf8");
        b.Float(write, "Value", float(++route));
        b.Link(*node, pin.identifier, write, "Flow");
      }
    }
    /* The node's data output number `output`, used by a node that takes its type. */
    size_t data_output = 0;
    bool root_used = false;
    for (const PinMetadata &pin : meta.outputs) {
      if (pin.type == PinType::Execution) {
        continue;
      }
      if (data_output++ != attempt.output) {
        continue;
      }
      GenNode *consumer = nullptr;
      const char *value_pin = "Value";
      switch (pin.type) {
        case PinType::Float:
        case PinType::Angle:
          consumer = &b.Add("LogicNativeSetGamePropertyFloat");
          b.String(*consumer, "Property", "pf9");
          break;
        case PinType::Int:
          consumer = &b.Add("LogicNativeSetGamePropertyInt");
          b.String(*consumer, "Property", "pi9");
          break;
        case PinType::Bool:
        case PinType::Condition:
          consumer = &b.Add("LogicNativeSetGamePropertyBool");
          b.String(*consumer, "Property", "pb9");
          break;
        case PinType::String:
          consumer = &b.Add("LogicNativeSetGamePropertyString");
          b.String(*consumer, "Property", "ps9");
          break;
        case PinType::Vector:
        case PinType::VectorXYAngle:
          consumer = &b.Add("LogicNativeSetObjectAttribute", 0, 15);
          break;
        case PinType::Rotation:
          consumer = &b.Add("LogicNativeApplyRotation");
          value_pin = "Rotation";
          break;
        case PinType::Color:
          consumer = &b.Add("LogicNativeSetObjectAttribute", 11, 15);
          value_pin = "Color";
          break;
        case PinType::Object:
          consumer = &b.Add("LogicNativeApplyMovement");
          b.Vector(*consumer, "Vector", 0.1f, 0.0f, 0.0f);
          value_pin = "Object";
          break;
        default:
          consumer = &b.Add("LogicNativeSetGameProperty");
          b.String(*consumer, "Property", "pg9");
          break;
      }
      if (node != nullptr) {
        b.Link(*node, pin.identifier, *consumer, value_pin);
      }
      b.Link(*root, root_out, *consumer, "Flow");
      root_used = true;
      break;
    }
    if (is_event && node != nullptr && !root_used) {
      /* An event's own flow drives a write too (On Value Changed To has no other output). */
      GenNode &write = b.Add("LogicNativeSetGamePropertyFloat");
      b.String(write, "Property", "pf7");
      b.Float(write, "Value", 1.0f);
      b.Link(*root, root_out, write, "Flow");
    }
    return b.Compile(r_report);
  };
  std::map<std::string, size_t> counts;
  std::vector<std::string> lines;
  size_t total = 0;
  for (const NodeMetadata &meta : blender::nodes::logic::logic_node_metadata()) {
    total++;
    const std::string idname = meta.idname;
    const bool is_event = is_event_root(meta);
    size_t data_outputs = 0;
    bool has_flow = false;
    bool has_data_input = false;
    for (const PinMetadata &pin : meta.outputs) {
      data_outputs += pin.type != PinType::Execution ? 1 : 0;
      has_flow = has_flow || pin.type == PinType::Execution;
    }
    for (const PinMetadata &pin : meta.inputs) {
      has_flow = has_flow || pin.type == PinType::Execution;
      has_data_input = has_data_input || pin.type != PinType::Execution;
    }
    /* Shapes, the plainest first: mode 0 before the others, live reads before defaults, flow
     * outputs used before unused, each data output. */
    std::vector<Attempt> attempts;
    /* (The node's own data-block last: most nodes have none; the assets are sound, node tree,
     * material, action, image, text.) */
    for (const int16_t custom2 : {0, 1, 2, 16, 32, 48}) {
      for (const int node_id : {-1, 3, 7, 1, 8, 2, 5}) {
        for (int16_t custom1 = 0; custom1 <= 12; custom1++) {
          for (const bool live : {true, false}) {
            for (const bool routes : {true, false}) {
              for (size_t output = 0; output <= data_outputs; output++) {
                attempts.push_back({live, routes, output, custom1, node_id, custom2});
              }
            }
          }
        }
      }
    }
    std::shared_ptr<LN_Program> program, compiled;
    std::string first_error;
    for (const Attempt &attempt : attempts) {
      std::string report;
      std::shared_ptr<LN_Program> candidate = build(meta, attempt, true, &report);
      if (candidate == nullptr) {
        if (first_error.empty()) {
          first_error = report.substr(0, report.find('\n'));
        }
        continue;
      }
      if (compiled == nullptr) {
        compiled = candidate;
      }
      std::shared_ptr<LN_Program> baseline = build(meta, attempt, false, nullptr);
      /* A command leaves ops; a data node (or an event) a value that is not a constant. */
      const size_t own_ops = ops_of(*candidate, idname) -
                             (baseline != nullptr ? ops_of(*baseline, idname) : 0);
      const size_t more_expressions = live_expressions_of(*candidate) -
                                      std::min(live_expressions_of(*candidate),
                                               baseline != nullptr ?
                                                   live_expressions_of(*baseline) :
                                                   size_t(0));
      const bool contributes = (has_flow && !is_event) ?
                                   own_ops > 0 ||
                                       own_expressions_of(*candidate) >
                                           (baseline != nullptr ? own_expressions_of(*baseline) :
                                                                  size_t(0)) :
                               is_event                ? own_ops > 0 || more_expressions > 0 ||
                                                  candidate->GetInstructionHeaders(LN_Event::OnInit)
                                                          .size() > 0 :
                                                         more_expressions > 0;
      if (contributes) {
        program = candidate;
        break;
      }
    }
    /* Whether a program's events lower (On Init never lowers to a wide run: the column pass runs
     * it). */
    const auto lowering = [&](const LN_Program &p, std::string &r_detail) -> std::string {
      bool any_ops = false;
      for (const LN_Event event : {LN_Event::OnInit, LN_Event::OnUpdate, LN_Event::OnFixedUpdate})
      {
        if (p.GetInstructionHeaders(event).empty()) {
          continue;
        }
        any_ops = true;
        if (event == LN_Event::OnInit) {
          /* (Plan C2 P7: the column pass runs it first, in a tree's first tick.) */
          r_detail = "On Init: the column pass runs it";
          return "direct";
        }
        std::string why;
        std::shared_ptr<const LN_ColumnProgram> bridged_columns;
        {
          BridgeOn bridge(true);
          bridged_columns = LN_CompileColumnProgram(p, event, &why);
        }
        if (bridged_columns == nullptr) {
          r_detail = why;
          /* Plan C2 P5: a direct program (the Auto mode leaves an event with a compiled route,
           * or a tree batch candidate while the game has helpers, to the old system). */
          std::shared_ptr<const LN_ColumnProgram> direct;
          {
            BridgeOn bridge(true);
            DirectScope auto_mode(1);
            direct = LN_CompileColumnProgram(p, event, nullptr, true);
          }
          if (direct != nullptr) {
            const std::string text = LN_DescribeColumnProgram(*direct);
            if (text.find("has_route=1") != std::string::npos) {
              r_detail += " (direct: an event with a route, the old system in Auto)";
            }
            else if (text.find("batch_safe=1") != std::string::npos) {
              r_detail += " (direct: a tree batch candidate, the old system with helpers)";
            }
            return "direct";
          }
          return "refused";
        }
        std::shared_ptr<const LN_ColumnProgram> native_columns;
        {
          BridgeOn bridge(false);
          native_columns = LN_CompileColumnProgram(p, event, nullptr);
        }
        if (native_columns == nullptr) {
          const std::string text = LN_DescribeColumnProgram(*bridged_columns);
          r_detail = text.find("main_thread=1") != std::string::npos ? "main thread" : "helpers";
          return "bridged";
        }
      }
      if (!any_ops) {
        r_detail = "no ops";
        return "nothing";
      }
      return "native";
    };
    std::string verdict, detail;
    if (program != nullptr) {
      verdict = lowering(*program, detail);
    }
    else if (!has_flow && !has_data_input) {
      /* A value node that leaves no work (Float, Vector, Key Code, Get Object...): its value is
       * known when compiled. (One reading live state, Time or Get Owner, leaves work.) */
      verdict = "constant";
      detail = "nothing to run";
    }
    else if (compiled != nullptr) {
      if (!has_flow && !has_data_input) {
        verdict = "constant";
      }
      else if (has_flow && data_outputs == 0) {
        verdict = "flow only";
        std::string how;
        const std::string lowered = lowering(*compiled, how);
        detail = lowered + (how.empty() ? std::string() : ": " + how);
      }
      else if (!has_flow) {
        /* A data node whose value is known when compiled (Get Sound, Key Code, File Path...). */
        verdict = "constant";
        detail = "folds to a constant";
      }
      else {
        verdict = "nothing";
        detail = "compiles to no work in any shape";
      }
    }
    else if (first_error.find("unavailable in this release") != std::string::npos) {
      verdict = "unavailable";
      detail = "no runtime in either system";
    }
    else {
      verdict = "not built";
      detail = first_error;
    }
    counts[verdict]++;
    const char *dump = std::getenv("UPBGE_LN_SCAN_DUMP");
    const std::shared_ptr<LN_Program> shown = program != nullptr ? program : compiled;
    if (shown != nullptr && dump != nullptr && std::strstr(dump, meta.idname + 11) != nullptr) {
      std::printf("DUMP %s\n%s\nREPORT %s\n",
                  meta.idname,
                  LN_DumpProgramText(*shown).c_str(),
                  FormatCompileReport(shown->GetCompileReport()).c_str());
    }
    lines.push_back(std::string(meta.category) + "\t" + meta.ui_name + "\t" + meta.idname + "\t" +
                    verdict + "\t" + detail);
    std::printf("SCAN\t%s\n", lines.back().c_str());
  }
  std::printf("SCAN of %zu nodes:", total);
  for (const auto &entry : counts) {
    std::printf(" %s %zu;", entry.first.c_str(), entry.second);
  }
  std::printf("\n");
  if (const char *out = std::getenv("UPBGE_LN_SCAN_OUT")) {
    std::ofstream file(out);
    for (const std::string &line : lines) {
      file << line << "\n";
    }
  }
}

namespace {

/** On Logic + Physics Step, after a Delay of two steps (the first tick runs On Init on the old
 * system; "made" is made in a tick the columns run), writes of the property "made" of the owner
 * (`target`: of the Target) one after the other: `pattern` names them, 'n' a native Set Game
 * Property Int (3), 'b' a bridged Set Object Property of the owner's float pf1. */
std::shared_ptr<LN_Program> MadeWritesTree(const char *pattern,
                                           const bool target,
                                           std::string *r_report)
{
  TreeBuilder b;
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode &delay = b.Add("LogicNativeDelay");
  b.Float(delay, "Delay", 0.03f);
  b.Link(step, "Out", delay, "Flow");
  GenNode *tail = &delay;
  const char *tail_out = "Out";
  for (const char *c = pattern; *c != '\0'; c++) {
    GenNode &write = b.Add(*c == 'n' ? "LogicNativeSetGamePropertyInt" :
                                       "LogicNativeSetGameProperty");
    b.String(write, "Property", "made");
    if (target) {
      b.ObjectNamed(write, "Object", &diff::TargetBlenderObject());
    }
    if (*c == 'n') {
      b.Int(write, "Value", 3);
    }
    else {
      GenNode &read = b.Add("LogicNativeGetGamePropertyFloat");
      b.String(read, "Property", "pf1");
      b.Link(read, "Value", write, "Value");
    }
    b.Link(*tail, tail_out, write, "Flow");
    tail = &write;
    tail_out = "Done";
  }
  return b.Compile(r_report);
}

}  // namespace

/* Plan C2 P0 (found by the random tester's Set Shape Key Value and Set Geometry Nodes Input rows):
 * a bridged op's last-write-wins command takes part in the flush's merge with its own key, as the
 * VM's recorded command does. Only writes in the step, so the flush merges: with a native Set Game
 * Property Int and then a bridged Set Object Property of "made", the VM leaves the first out and
 * "made" is made a Float; the columns applied both (a bridged op's commands ended the merge's run)
 * and made it an Int. On the owner and on the Target (every lane writes it: merged across lanes),
 * with a write between two bridged ones, one and three helpers. */
TEST(LN_Columns, ABridgedOpsWritesMergeAsTheVMsRecordedCommands)
{
  BridgeOn bridge;
  for (const bool target : {false, true}) {
    for (const char *pattern : {"nb", "bb", "bnb", "nbn"}) {
      std::string report;
      const std::shared_ptr<LN_Program> program = MadeWritesTree(pattern, target, &report);
      ASSERT_NE(program, nullptr) << report;
      std::string refusal;
      ASSERT_NE(LN_CompileColumnProgram(*program, LN_Event::OnFixedUpdate, &refusal), nullptr)
          << refusal;
      for (const uint32_t helpers : {0u, 3u}) {
        const std::string what = std::string(pattern) + (target ? " on the Target" : "") +
                                 ", helpers " + std::to_string(helpers);
        EXPECT_GT(ExpectColumnsMatch(program,
                                     LN_Event::OnFixedUpdate,
                                     40,
                                     3,
                                     what,
                                     helpers,
                                     nullptr,
                                     [](SemRig &rig) { rig.BindIds(); }),
                  0u)
            << what;
      }
    }
  }
}

TEST(LN_Columns, DISABLED_ColumnsBench)
{
  BrainOfOptions options;
  const char *chains_text = std::getenv("UPBGE_LN_BENCH_CHAINS");
  options.chains = chains_text != nullptr ? std::atoi(chains_text) : 4;
  const std::shared_ptr<LN_Program> program = BrainOf(options);
  ASSERT_NE(program, nullptr);
  /* UPBGE_LN_BENCH_OWNERS (default 10,000) and UPBGE_LN_BENCH_MINRUN (default 16): small runs and
   * single trees in columns against the lean VM (the one-system plan's question). */
  const char *owners_text = std::getenv("UPBGE_LN_BENCH_OWNERS");
  const uint32_t kOwners = owners_text != nullptr ? uint32_t(std::max(1, std::atoi(owners_text))) :
                                                    10000u;
  const char *minrun_text = std::getenv("UPBGE_LN_BENCH_MINRUN");
  const size_t min_run = minrun_text != nullptr ? size_t(std::max(1, std::atoi(minrun_text))) : 16;
  const char *rounds_text = std::getenv("UPBGE_LN_BENCH_ROUNDS");
  const int rounds = rounds_text != nullptr ? std::max(1, std::atoi(rounds_text)) : 30;
  struct Setup {
    const char *name;
    bool columns;
    uint32_t helpers;
  };
  ColumnScope scope(1);
  /* UPBGE_LN_BENCH_SETUP: run only the setups whose name contains it (stack sampling). */
  const char *only_setup = std::getenv("UPBGE_LN_BENCH_SETUP");
  for (const Setup &setup : {Setup{"lean, 1 thread", false, 0},
                             Setup{"columns, 1 thread", true, 0},
                             Setup{"lean batch, 9", false, 8},
                             Setup{"columns, 9", true, 8}})
  {
    if (only_setup != nullptr && std::strstr(setup.name, only_setup) == nullptr) {
      continue;
    }
    SemRig rig(program, kMemoOn, kOwners);
    rig.SetDump(false);
    rig.SetCapturePrints(false);
    Spread(rig, kOwners);
    if (setup.columns) {
      rig.SetColumns(true, setup.helpers, min_run);
    }
    else if (setup.helpers > 0) {
      rig.SetBatchRecording(36, setup.helpers);
      rig.SetParallelCapture(setup.helpers);
    }
    rig.Update();
    rig.Update();
    SemRig::PhaseTimes best = {1.0e9, 1.0e9, 1.0e9};
    double best_total = 1.0e9;
    for (int round = 0; round < rounds; round++) {
      rig.ResetTimes();
      rig.Update();
      const SemRig::PhaseTimes &t = rig.Times();
      best.capture_ms = std::min(best.capture_ms, t.capture_ms);
      best.record_ms = std::min(best.record_ms, t.record_ms);
      best.flush_ms = std::min(best.flush_ms, t.flush_ms);
      best_total = std::min(best_total, t.capture_ms + t.record_ms + t.flush_ms);
    }
    std::printf("COLUMNS_BENCH brain%d %-18s capture %4.0f  record %4.0f  flush %4.0f  total %4.0f "
                "ns an NPC (lanes %zu)\n",
                options.chains,
                setup.name,
                best.capture_ms * 1.0e6 / kOwners,
                best.record_ms * 1.0e6 / kOwners,
                best.flush_ms * 1.0e6 / kOwners,
                best_total * 1.0e6 / kOwners,
                rig.LastColumnLanes());
  }
}

/* Plan C2 P1: short runs against the old system, ns an NPC (capture + record + flush, the best
 * round), one thread. (1) One program on 1, 2, 4, 8 and 16 objects (the brain with 4 and with 15
 * chains). (2) 1,000 objects of 10 programs (brains of 1 to 10 chains): interleaved, A B C ... A B
 * C (runs of 1), or sorted (runs of 100). Columns with the shortest run 1 (`min 1`) and 16 (the
 * default: the interleaved scene then runs on the old system). UPBGE_LN_BENCH_ROUNDS (default
 * 400), UPBGE_LN_BENCH_SETUP (only setups whose name contains it). Run with
 * --gtest_also_run_disabled_tests. */
TEST(LN_Columns, DISABLED_ShortRunsBench)
{
  const char *rounds_text = std::getenv("UPBGE_LN_BENCH_ROUNDS");
  const int rounds = rounds_text != nullptr ? std::max(1, std::atoi(rounds_text)) : 400;
  const char *only_setup = std::getenv("UPBGE_LN_BENCH_SETUP");
  const auto wanted = [&](const std::string &name) {
    return only_setup == nullptr || name.find(only_setup) != std::string::npos;
  };
  ColumnScope scope(1);
  /* UPBGE_LN_BENCH_PHASES: each way also prints its best plan + capture / record / flush. */
  const bool phases = std::getenv("UPBGE_LN_BENCH_PHASES") != nullptr;
  std::string phase_text;
  /* The best round's ns an NPC of `rig` after two warm ticks. */
  const auto measure = [&](SemRig &rig, const size_t npcs) {
    rig.SetDump(false);
    rig.SetCapturePrints(false);
    rig.Update();
    rig.Update();
    double best = 1.0e30;
    SemRig::PhaseTimes best_phase = {1.0e30, 1.0e30, 1.0e30, 1.0e30};
    for (int round = 0; round < rounds; round++) {
      rig.ResetTimes();
      rig.Update();
      const SemRig::PhaseTimes &t = rig.Times();
      best = std::min(best, t.capture_ms + t.record_ms + t.flush_ms);
      best_phase.capture_ms = std::min(best_phase.capture_ms, t.capture_ms);
      best_phase.record_ms = std::min(best_phase.record_ms, t.record_ms);
      best_phase.flush_ms = std::min(best_phase.flush_ms, t.flush_ms);
      best_phase.plan_ms = std::min(best_phase.plan_ms, t.plan_ms);
    }
    if (phases) {
      char text[96];
      std::snprintf(text,
                    sizeof(text),
                    " [plan %.0f, %.0f/%.0f/%.0f]",
                    best_phase.plan_ms * 1.0e6 / double(npcs),
                    best_phase.capture_ms * 1.0e6 / double(npcs),
                    best_phase.record_ms * 1.0e6 / double(npcs),
                    best_phase.flush_ms * 1.0e6 / double(npcs));
      phase_text = text;
    }
    return best * 1.0e6 / double(npcs);
  };
  struct Way {
    const char *name;
    bool columns;
    size_t min_run;
    /* Helper threads: the old system's tree batches, the columns' chunks and waves. */
    uint32_t helpers = 0;
  };
  /* (min_run 0: the runner's own shortest run, with the size floors and the wave rule.) */
  const Way ways[] = {{"old", false, 1},
                      {"columns min 1", true, 1},
                      {"columns min 16", true, 16},
                      {"columns floors", true, 0},
                      {"old 9 threads", false, 1, 8},
                      {"columns 9 threads", true, 1, 8},
                      {"columns floors 9 threads", true, 0, 8}};
  /* UPBGE_LN_BENCH_WAY: only the ways whose name contains it (stack sampling). */
  const char *only_way = std::getenv("UPBGE_LN_BENCH_WAY");
  const auto way_wanted = [&](const Way &way) {
    return only_way == nullptr || std::strstr(way.name, only_way) != nullptr;
  };
  for (const int chains : {4, 15}) {
    BrainOfOptions options;
    options.chains = chains;
    const std::shared_ptr<LN_Program> program = BrainOf(options);
    ASSERT_NE(program, nullptr);
    for (const uint32_t owners : {1u, 2u, 4u, 8u, 16u}) {
      std::string line = "SHORT_RUNS brain" + std::to_string(chains) + " x" +
                         std::to_string(owners) + ":";
      if (!wanted(line)) {
        continue;
      }
      for (const Way &way : ways) {
        if ((way.columns && way.min_run > owners) || !way_wanted(way)) {
          continue;
        }
        SemRig rig(program, kMemoOn, owners);
        Spread(rig, owners);
        rig.SetColumns(way.columns, way.helpers, way.min_run);
        if (!way.columns && way.helpers > 0) {
          rig.SetBatchRecording(36, way.helpers);
          rig.SetParallelCapture(way.helpers);
        }
        char text[96];
        std::snprintf(text, sizeof(text), "  %s %5.0f", way.name, measure(rig, owners));
        line += text + phase_text;
      }
      std::printf("%s\n", line.c_str());
    }
  }
  /* (3) The same with 10 tiny programs: the brain without chains (8 nodes), each with its own
   * distance threshold: a run's fixed cost against a small tree's. UPBGE_LN_BENCH_TINY_CHAINS:
   * that many chains instead (the break-even size of a run of 1). */
  const char *tiny_text = std::getenv("UPBGE_LN_BENCH_TINY_CHAINS");
  const int tiny_chains = tiny_text != nullptr ? std::atoi(tiny_text) : 0;
  std::vector<std::shared_ptr<LN_Program>> programs, tiny;
  for (int chains = 1; chains <= 10; chains++) {
    BrainOfOptions options;
    options.chains = chains;
    programs.push_back(BrainOf(options));
    ASSERT_NE(programs.back(), nullptr);
    options.chains = tiny_chains;
    options.threshold = 2.0f + float(chains);
    tiny.push_back(BrainOf(options));
    ASSERT_NE(tiny.back(), nullptr);
  }
  constexpr uint32_t kObjects = 1000;
  for (const int scene : {0, 1, 2}) {
    const bool interleaved = scene != 1;
    std::string line = std::string(scene == 2 ? "SHORT_RUNS tiny 1000 x 10 programs, " :
                                                "SHORT_RUNS mixed 1000 x 10 programs, ") +
                       (interleaved ? "interleaved:" : "sorted:");
    if (!wanted(line)) {
      continue;
    }
    const std::vector<std::shared_ptr<LN_Program>> &scene_programs = scene == 2 ? tiny : programs;
    for (const Way &way : ways) {
      if (!way_wanted(way)) {
        continue;
      }
      SemRig rig(scene_programs[0], kMemoOn, 0);
      for (uint32_t index = 0; index < kObjects; index++) {
        rig.AddObject("Obj" + std::to_string(index), MT_Vector3(0.0f, 0.0f, 0.0f));
        const size_t which = interleaved ? index % 10 : index / (kObjects / 10);
        rig.AddTree(scene_programs[which], index, index, false);
      }
      Spread(rig, kObjects);
      rig.SetColumns(way.columns, way.helpers, way.min_run);
      if (!way.columns && way.helpers > 0) {
        rig.SetBatchRecording(36, way.helpers);
        rig.SetParallelCapture(way.helpers);
      }
      char text[96];
      std::snprintf(text, sizeof(text), "  %s %5.0f", way.name, measure(rig, kObjects));
      line += text + phase_text;
    }
    std::printf("%s\n", line.c_str());
  }
}

/* Plan C2 P0: what lowering to columns costs, beside the compiles every program already pays (the
 * tree compiler, the lean VM's lowering), and what a column program holds in memory. Programs: the
 * brains (BrainOf with 1, 4 and 15 chains) and the differential tester's random trees (every
 * family, `UPBGE_LN_BENCH_SEEDS`, default 2,000). Each column compile is timed apart (best of 5
 * for the brains). Run with --gtest_also_run_disabled_tests. */
TEST(LN_Columns, DISABLED_ColumnCompileCost)
{
  using Clock = std::chrono::steady_clock;
  const auto us = [](const Clock::time_point a, const Clock::time_point b) {
    return std::chrono::duration<double, std::micro>(b - a).count();
  };
  ColumnScope scope(1);
  for (const int chains : {0, 1, 2, 4, 15}) {
    BrainOfOptions options;
    options.chains = chains;
    double tree_best = 1e30, lean_best = 1e30, column_best = 1e30;
    LN_ColumnMemory memory;
    size_t nodes = 0;
    std::string described;
    for (int round = 0; round < 5; round++) {
      const Clock::time_point t0 = Clock::now();
      const std::shared_ptr<LN_Program> program = BrainOf(options);
      const Clock::time_point t1 = Clock::now();
      ASSERT_NE(program, nullptr);
      const LN_Event event = program->GetInstructionHeaders(LN_Event::OnUpdate).empty() ?
                                 LN_Event::OnFixedUpdate :
                                 LN_Event::OnUpdate;
      program->EnsureLeanProgram();
      const Clock::time_point t2 = Clock::now();
      std::string refusal;
      const std::shared_ptr<const LN_ColumnProgram> columns = LN_CompileColumnProgram(
          *program, event, &refusal);
      const Clock::time_point t3 = Clock::now();
      ASSERT_NE(columns, nullptr) << refusal;
      tree_best = std::min(tree_best, us(t0, t1));
      lean_best = std::min(lean_best, us(t1, t2));
      column_best = std::min(column_best, us(t2, t3));
      memory = LN_ColumnProgramMemory(*columns);
      nodes = program->GetInstructionHeaders(event).size();
      described = LN_DescribeColumnProgram(*columns);
      described = described.substr(described.find(" nodes="), 11);
    }
    std::printf("COMPILE_COST brain%-2d ops %3zu%s: tree compile + build %7.1f us, lean %6.1f us, "
                "columns %6.1f us; column program %6zu bytes, %4zu bytes a lane, chunk scratch "
                "%6zu bytes\n",
                chains,
                nodes,
                described.c_str(),
                tree_best,
                lean_best,
                column_best,
                memory.program_bytes,
                memory.lane_bytes,
                memory.chunk_bytes);
  }
  const char *seeds_text = std::getenv("UPBGE_LN_BENCH_SEEDS");
  const uint64_t seeds = seeds_text != nullptr ? std::strtoull(seeds_text, nullptr, 10) : 2000;
  double tree_total = 0, lean_total = 0, column_total = 0, column_max = 0;
  size_t compiled = 0, events = 0, lowered = 0, program_bytes = 0, lane_bytes = 0,
         chunk_bytes = 0, program_max = 0, lane_max = 0;
  for (uint64_t seed = 1; seed <= seeds; seed++) {
    diff::TreeGen generator{seed};
    std::unique_ptr<diff::GeneratedTree> tree = generator.Build();
    const Clock::time_point t0 = Clock::now();
    std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree->tree);
    const Clock::time_point t1 = Clock::now();
    if (program == nullptr || program->GetCompileReport().HasErrors()) {
      continue;
    }
    compiled++;
    tree_total += us(t0, t1);
    const Clock::time_point t2 = Clock::now();
    program->EnsureLeanProgram();
    lean_total += us(t2, Clock::now());
    for (const LN_Event event : {LN_Event::OnUpdate, LN_Event::OnFixedUpdate}) {
      if (program->GetInstructionHeaders(event).empty()) {
        continue;
      }
      events++;
      const Clock::time_point t3 = Clock::now();
      const std::shared_ptr<const LN_ColumnProgram> columns = LN_CompileColumnProgram(*program,
                                                                                      event);
      const double cost = us(t3, Clock::now());
      column_total += cost;
      column_max = std::max(column_max, cost);
      if (columns != nullptr) {
        lowered++;
        const LN_ColumnMemory memory = LN_ColumnProgramMemory(*columns);
        program_bytes += memory.program_bytes;
        lane_bytes += memory.lane_bytes;
        chunk_bytes += memory.chunk_bytes;
        program_max = std::max(program_max, memory.program_bytes);
        lane_max = std::max(lane_max, memory.lane_bytes);
      }
    }
  }
  ASSERT_GT(compiled, 0u);
  ASSERT_GT(lowered, 0u);
  std::printf("COMPILE_COST random trees: %zu programs, %zu events (%zu lower): tree compile "
              "%.1f us a program, lean %.1f us, columns %.1f us an event (max %.1f); column "
              "program %zu bytes on average (max %zu), %zu bytes a lane (max %zu), chunk scratch "
              "%zu bytes\n",
              compiled,
              events,
              lowered,
              tree_total / compiled,
              lean_total / compiled,
              column_total / events,
              column_max,
              program_bytes / lowered,
              program_max,
              lane_bytes / lowered,
              lane_max,
              chunk_bytes / lowered);
}

namespace {

/** The Blender object "Turret": Get Child By Name's name hint (no scene has it). */
blender::Object &TurretBlenderObject()
{
  static blender::Object object;
  static const bool named = (SetCString(object.id.name, "OBTurret"), true);
  (void)named;
  return object;
}

enum class TurretFind { ChildByName, ChildIndex, ByName };
enum class TurretOrder { TurretFirst, OwnerFirst, PlacedPair, MeasuredToo, NamedMeasured };

/* On Logic + Physics Step: the owner's distance to the Target kept in pf1, then its turret (its
 * child by name or by index, or the object named Turret: every NPC turns that one) turned and
 * placed in the world and the owner walked, in either order, the turret's X read back live into
 * pf2; `bridged` prints pf1 too (a bridged op, so the turret's name hint is a named object of a
 * bridged program). PlacedPair places the turret then the owner (two world positions in a row,
 * keyed by their objects in the flush's merge). MeasuredToo first keeps the distance to the
 * turret (refused); NamedMeasured to the object named Turret (NPC 0's, named in the tree: with
 * ByName every NPC moves it, so the run gives the NPCs back to the VM). */
std::shared_ptr<LN_Program> TurretTree(const TurretFind find,
                                       const TurretOrder order,
                                       const bool bridged,
                                       std::string *r_report)
{
  TreeBuilder b;
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode &owner = b.Add("LogicNativeGetOwner");
  GenNode *turret = nullptr;
  const char *out = "Object";
  switch (find) {
    case TurretFind::ChildByName:
      turret = &b.Add("LogicNativeGetChildByName");
      b.Link(owner, "Owner Object", *turret, "Parent");
      b.ObjectNamed(*turret, "Child", &TurretBlenderObject());
      out = "Child";
      break;
    case TurretFind::ChildIndex:
      turret = &b.Add("LogicNativeGetChild");
      b.Link(owner, "Owner Object", *turret, "Parent Object");
      b.Int(*turret, "Index", 0);
      out = "Child Object";
      break;
    case TurretFind::ByName:
      turret = &b.Add("LogicNativeObjectByName");
      b.String(*turret, "Name", "Turret");
      break;
  }
  GenNode &distance = b.Add("LogicNativeGetDistance", 0, 0);
  b.ObjectNamed(distance,
                "Object B",
                order == TurretOrder::NamedMeasured ? &TurretBlenderObject() :
                                                      &diff::TargetBlenderObject());
  GenNode &keep = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(keep, "Property", "pf1");
  b.Link(distance, "Distance", keep, "Value");
  if (order == TurretOrder::MeasuredToo) {
    /* The distance to the turret first (before any move). */
    GenNode &to_turret = b.Add("LogicNativeGetDistance", 0, 0);
    b.Link(*turret, out, to_turret, "Object B");
    GenNode &seen = b.Add("LogicNativeSetGamePropertyFloat");
    b.String(seen, "Property", "pf0");
    b.Link(to_turret, "Distance", seen, "Value");
    b.Link(step, "Out", seen, "Flow");
    b.Link(seen, "Done", keep, "Flow");
  }
  else {
    b.Link(step, "Out", keep, "Flow");
  }
  GenNode &aim = b.Add("LogicNativeApplyRotation");
  b.Link(*turret, out, aim, "Object");
  b.Rotation(aim, "Rotation", 0.0f, 0.0f, 0.07f);
  b.Bool(aim, "Local", false);
  GenNode &place = b.Add("LogicNativeSetObjectAttribute", 0, 15);
  b.Link(*turret, out, place, "Object");
  b.Vector(place, "Value", 1.0f, 2.0f, 1.5f);
  GenNode &walk = b.Add("LogicNativeApplyMovement");
  b.Vector(walk, "Vector", 0.25f, -0.125f, 0.0f);
  b.Bool(walk, "Local", false);
  GenNode &pose = b.Add("LogicNativeGetObjectAttribute", 0);
  b.Link(*turret, out, pose, "Object");
  GenNode &split = b.Add("LogicNativeSeparateXYZ");
  b.Link(pose, "Vector", split, "Vector");
  GenNode &x = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(x, "Property", "pf2");
  b.Link(split, "X", x, "Value");
  if (order == TurretOrder::OwnerFirst) {
    b.Link(keep, "Done", walk, "Flow");
    b.Link(walk, "Done", place, "Flow");
    b.Link(place, "Done", aim, "Flow");
    b.Link(aim, "Done", x, "Flow");
  }
  else if (order == TurretOrder::PlacedPair) {
    GenNode &place_owner = b.Add("LogicNativeSetObjectAttribute", 0, 15);
    b.Vector(place_owner, "Value", -1.0f, 0.5f, 0.0f);
    b.Link(keep, "Done", aim, "Flow");
    b.Link(aim, "Done", place, "Flow");
    b.Link(place, "Done", place_owner, "Flow");
    b.Link(place_owner, "Done", x, "Flow");
  }
  else {
    /* A force on the turret too (applied at the flush; none for an NPC without one). */
    GenNode &force = b.Add("LogicNativeApplyForce");
    b.Link(*turret, out, force, "Object");
    b.Vector(force, "Force", 0.0f, 0.0f, 2.0f);
    b.Link(keep, "Done", aim, "Flow");
    b.Link(aim, "Done", place, "Flow");
    b.Link(place, "Done", force, "Flow");
    b.Link(force, "Done", walk, "Flow");
    b.Link(walk, "Done", x, "Flow");
  }
  GenNode *last = &x;
  if (bridged) {
    GenNode &pf1 = b.Add("LogicNativeGetGamePropertyFloat");
    b.String(pf1, "Property", "pf1");
    GenNode &say = b.Add("LogicNativePrint");
    b.Link(pf1, "Value", say, "Message");
    b.Link(*last, "Done", say, "Flow");
  }
  return b.Compile(r_report);
}

/** Gives each of the first `owners` objects but every fifth a child "Turret" half a metre beside
 * and a metre over it (its local pose), and puts trees next to the crowd that read, place and push the object
 * named Turret (NPC 0's, `first_on_its_own`: no one's child and not pushed; `only_first_named`: the
 * others are Turret1, Turret2...): a measure after NPC 1 (its read applies NPC 0's moves of the turret and
 * of NPC 0 early, in NPC 0's op order), a place between NPC 0 and NPC 1, a push before the crowd. */
void AddTurrets(SemRig &rig,
                const uint32_t owners,
                const bool on_steps,
                const bool only_first_named = false,
                const bool first_on_its_own = false)
{
  for (uint32_t index = 0; index < owners; index++) {
    if (index % 5 == 4) {
      continue; /* no turret: the NPC's op finds nothing (stamped, nothing recorded) */
    }
    rig.AddObject(only_first_named && index > 0 ? "Turret" + std::to_string(index) : "Turret",
                  MT_Vector3(0.5f, 0.0f, 1.0f));
    if (index > 0 || !first_on_its_own) {
      rig.Parent(rig.ObjectCount() - 1, index);
    }
  }
  struct Placement {
    std::shared_ptr<LN_Program> program;
    uint32_t scene_index;
    bool first;
  };
  const Placement placements[] = {
      {OtherTree(Other::Measure, on_steps), 3, false},
      {OtherTree(Other::Place, on_steps), 1, false},
      {OtherTree(Other::Push, on_steps), 5, true},
  };
  for (const Placement &placement : placements) {
    ASSERT_NE(placement.program, nullptr);
    if (first_on_its_own && placement.first) {
      continue; /* no push before the crowd: only the NPCs' moves mark the lone Turret */
    }
    KX_GameObject &object = rig.AddObject("Other" + std::to_string(rig.ObjectCount()),
                                          MT_Vector3(1.0f, -2.0f, 0.0f));
    rig.SetString(object, "target", "Turret");
    rig.SetFloat(object, "seen", -1.0f);
    rig.AddTree(placement.program, rig.ObjectCount() - 1, placement.scene_index, placement.first);
  }
}

}  // namespace

/* Plan C1-9 E2: an NPC moves an object other than its own, found while playing (its turret as a
 * child by name or by index; or the object named Turret, which all the NPCs move). The run finds
 * each NPC's object when it starts; the flush applies the moves at the NPC's sort key, and a later
 * tree's read of a turret applies the NPC's moves of the turret and of the NPC itself in its op
 * order, as the VM's recorded commands are applied. Trees before and after the crowd place, push
 * and measure the object named Turret. A distance to an object the program moves is refused; an
 * NPC that moves another lane's object gives the run back to the VM. */
TEST(LN_Columns, MovesOfObjectsFoundWhilePlayingEndAsOnTheLeanVM)
{
  constexpr uint32_t kOwners = 40;
  for (const bool bridged : {false, true}) {
    BridgeOn bridge;
    for (const TurretFind find :
         {TurretFind::ChildByName, TurretFind::ChildIndex, TurretFind::ByName})
    {
      for (const TurretOrder order :
           {TurretOrder::TurretFirst, TurretOrder::OwnerFirst, TurretOrder::PlacedPair})
      {
        std::string report;
        const std::shared_ptr<LN_Program> program = TurretTree(find, order, bridged, &report);
        ASSERT_NE(program, nullptr) << report;
        std::string refusal;
        ASSERT_NE(LN_CompileColumnProgram(*program, LN_Event::OnFixedUpdate, &refusal), nullptr)
            << refusal << "\n" << LN_DumpProgramText(*program);
        const std::string what = std::string(bridged ? "bridged " : "") + "find " +
                                 std::to_string(int(find)) + " order " +
                                 std::to_string(int(order));
        /* By name: the Turret every NPC moves is also a root object once (only its own pending
         * mark then tells the measure after the crowd about the NPCs' moves). */
        for (const bool on_its_own : {false, true}) {
          if (on_its_own && find != TurretFind::ByName) {
            continue;
          }
        const auto turrets = [&](SemRig &rig) {
          AddTurrets(rig, kOwners, true, false, on_its_own);
        };
        const uint64_t fallbacks = LN_ColumnRuntimeFallbackCount();
        EXPECT_GT(ExpectColumnsMatch(program, LN_Event::OnFixedUpdate, kOwners, 12, what, 0,
                                     nullptr, turrets),
                  0u)
            << what;
        EXPECT_GT(ExpectColumnsMatch(program, LN_Event::OnFixedUpdate, kOwners, 12,
                                     what + " (3 helpers)", 3, nullptr, turrets),
                  0u)
            << what;
        /* The crowd never gives its lanes back (40 a tick). The tree measuring the Turret it finds
         * by its text property lowers too since plan C1-10 (a one-lane run here, where any run
         * may form), and gives its lane back at each pass: its Turret is an NPC's child. */
        EXPECT_LE(LN_ColumnRuntimeFallbackCount() - fallbacks, 2u * 12u) << what;
        }
      }
    }
  }
  std::string report;
  std::string refusal;
  const std::shared_ptr<LN_Program> measured = TurretTree(
      TurretFind::ChildByName, TurretOrder::MeasuredToo, false, &report);
  ASSERT_NE(measured, nullptr) << report;
  EXPECT_EQ(LN_CompileColumnProgram(*measured, LN_Event::OnFixedUpdate, &refusal), nullptr);
  EXPECT_NE(refusal.find("a distance to an object the program moves"), std::string::npos)
      << refusal;
  /* Every NPC moves the object named Turret and measures it by its name: back to the VM (an NPC's
   * read applies the earlier NPCs' moves of it there). The bridge off: the turret's live pose read
   * native, so the run's own rule decides (with the bridge, the pushed Turret's pending moves give
   * the run back first). */
  {
    BridgeOn bridge_off(false);
    const std::shared_ptr<LN_Program> named = TurretTree(
        TurretFind::ByName, TurretOrder::NamedMeasured, false, &report);
    ASSERT_NE(named, nullptr) << report;
    ASSERT_NE(LN_CompileColumnProgram(*named, LN_Event::OnFixedUpdate, &refusal), nullptr)
        << refusal;
    const auto turrets = [&](SemRig &rig) { AddTurrets(rig, kOwners, true, true); };
    const uint64_t fallbacks = LN_ColumnRuntimeFallbackCount();
    std::string why;
    const uint64_t lanes = ExpectColumnsMatch(
        named, LN_Event::OnFixedUpdate, kOwners, 6, "named turret", 0, &why, turrets);
    if (!DirectForced()) {
      EXPECT_GT(LN_ColumnRuntimeFallbackCount(), fallbacks) << lanes << " lanes; " << why;
    }
  }
  /* Every NPC keeps its distance to the Target, then moves Obj3, a lane's object: back to the VM
   * (Obj3's own read applies the earlier NPCs' moves of it there). */
  TreeBuilder b;
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode &distance = b.Add("LogicNativeGetDistance", 0, 0);
  b.ObjectNamed(distance, "Object B", &diff::TargetBlenderObject());
  GenNode &keep = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(keep, "Property", "pf1");
  b.Link(distance, "Distance", keep, "Value");
  b.Link(step, "Out", keep, "Flow");
  GenNode &find = b.Add("LogicNativeObjectByName");
  b.String(find, "Name", "Obj3");
  GenNode &push = b.Add("LogicNativeApplyMovement");
  b.Link(find, "Object", push, "Object");
  b.Vector(push, "Vector", 0.03f, -0.02f, 0.0f);
  b.Link(keep, "Done", push, "Flow");
  const std::shared_ptr<LN_Program> pusher = b.Compile(&report);
  ASSERT_NE(pusher, nullptr) << report;
  ASSERT_NE(LN_CompileColumnProgram(*pusher, LN_Event::OnFixedUpdate, &refusal), nullptr)
      << refusal;
  const uint64_t fallbacks = LN_ColumnRuntimeFallbackCount();
  ExpectColumnsMatch(pusher, LN_Event::OnFixedUpdate, 40, 6, "push a lane's object");
  if (!DirectForced()) {
    EXPECT_GT(LN_ColumnRuntimeFallbackCount(), fallbacks);
  }
}

namespace {

/** The Blender object "Leader" (a named object of the flock tests; no scene has it). */
blender::Object &LeaderBlenderObject()
{
  static blender::Object object;
  static const bool named = (SetCString(object.id.name, "OBLeader"), true);
  (void)named;
  return object;
}

enum class FlockSource {
  /* The Target, the Leader and the owner: differs between the NPCs (bridged). */
  ObjectsAndOwner,
  /* Three vectors from the owner's properties (bridged). */
  Vectors,
  /* The Target and the Leader: the same for every NPC (worked out once a run, native). */
  Named,
  /* Named, and a distance from the Target to the Leader found by name read after the average
   * (the run must give the NPCs back when the Leader has pending moves). */
  NamedThenFoundDistance,
  /* Named, read before the distance to the Target (pushed before the crowd): the run's first
   * tree plays on the VM, whose distance read applies the push, as for a bridged run. */
  NamedBeforeDistance,
  /* The Leader alone (Count 1). */
  LeaderOnly,
  /* Named, and no object distance (positions only): no read applies the NPCs' own pending moves,
   * so a crowd NPC pushed by another tree keeps its run in columns. */
  NamedNoObjectDistance,
};

/* On Logic + Physics Step: the distance to the Target kept in pf1, then Get Average Center
 * Position of `source`: its X into pf2, a move of the owner, then its Y into pf0, its Count into
 * pi0 and its Valid into pb0 (read again after the move: the VM reads the positions live, the
 * move still pending). */
std::shared_ptr<LN_Program> FlockTree(const FlockSource flock, std::string *r_report)
{
  const bool objects = flock != FlockSource::Vectors;
  TreeBuilder b;
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  const bool positions = flock == FlockSource::NamedNoObjectDistance;
  GenNode &distance = b.Add("LogicNativeGetDistance", positions ? 1 : 0, positions ? 1 : 0);
  if (positions) {
    b.Vector(distance, "Position B", 3.0f, 4.0f, 0.0f);
  }
  else {
    b.ObjectNamed(distance, "Object B", &diff::TargetBlenderObject());
  }
  GenNode &keep = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(keep, "Property", "pf1");
  b.Link(distance, "Distance", keep, "Value");
  const bool average_first = flock == FlockSource::NamedBeforeDistance;
  if (!average_first) {
    b.Link(step, "Out", keep, "Flow");
  }
  GenNode &center = b.Add("LogicNativeGetAverageCenterPosition", objects ? 1 : 0);
  if (objects) {
    GenNode &list = b.Add("LogicNativeListFromItems", 10); /* custom1 10: the Objects input */
    GenNode &target = b.Add("LogicNativeFindObject");
    b.ObjectNamed(target, "Object", &diff::TargetBlenderObject());
    GenNode &leader = b.Add("LogicNativeFindObject");
    b.ObjectNamed(leader, "Object", &LeaderBlenderObject());
    if (flock != FlockSource::LeaderOnly) {
      b.Link(target, "Object", list, "Objects");
    }
    b.Link(leader, "Object", list, "Objects");
    if (flock == FlockSource::ObjectsAndOwner) {
      GenNode &owner = b.Add("LogicNativeGetOwner");
      b.Link(owner, "Owner Object", list, "Objects");
    }
    b.Link(list, "Objects", center, "Objects");
  }
  else {
    GenNode &list = b.Add("LogicNativeListFromItems", 5); /* custom1 5: the Vectors input */
    for (const char *name : {"t0", "t1", "t2"}) {
      GenNode &read = b.Add("LogicNativeGetGamePropertyFloat");
      b.String(read, "Property", name);
      GenNode &v = b.Add("LogicNativeCombineXYZ");
      b.Link(read, "Value", v, "X");
      b.Float(v, "Y", 0.5f);
      b.Link(v, "Vector", list, "Vectors");
    }
    b.Link(list, "Vectors", center, "Vectors");
  }
  GenNode &split = b.Add("LogicNativeSeparateXYZ");
  b.Link(center, "Center", split, "Vector");
  GenNode &x = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(x, "Property", "pf2");
  b.Link(split, "X", x, "Value");
  GenNode &walk = b.Add("LogicNativeApplyMovement");
  b.Vector(walk, "Vector", 0.25f, -0.125f, 0.0f);
  b.Bool(walk, "Local", false);
  if (average_first) {
    /* The average's X first, then the distance (it applies the Target's push), then the move. */
    b.Link(step, "Out", x, "Flow");
    b.Link(x, "Done", keep, "Flow");
    b.Link(keep, "Done", walk, "Flow");
  }
  else {
    b.Link(keep, "Done", x, "Flow");
    b.Link(x, "Done", walk, "Flow");
  }
  GenNode &y = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(y, "Property", "pf0");
  b.Link(split, "Y", y, "Value");
  b.Link(walk, "Done", y, "Flow");
  GenNode &count = b.Add("LogicNativeSetGamePropertyInt");
  b.String(count, "Property", "pi0");
  b.Link(center, "Count", count, "Value");
  b.Link(y, "Done", count, "Flow");
  GenNode &valid = b.Add("LogicNativeSetGamePropertyBool");
  b.String(valid, "Property", "pb0");
  b.Link(center, "Valid", valid, "Value");
  b.Link(count, "Done", valid, "Flow");
  if (flock == FlockSource::NamedThenFoundDistance) {
    GenNode &find = b.Add("LogicNativeObjectByName");
    b.String(find, "Name", "Leader");
    /* From the Target (the owner moved before: a distance from it would be refused). */
    GenNode &to_leader = b.Add("LogicNativeGetDistance", 0, 0);
    b.ObjectNamed(to_leader, "Object A", &diff::TargetBlenderObject());
    b.Link(find, "Object", to_leader, "Object B");
    GenNode &seen = b.Add("LogicNativeSetGamePropertyFloat");
    b.String(seen, "Property", "dist");
    b.Link(to_leader, "Distance", seen, "Value");
    b.Link(valid, "Done", seen, "Flow");
  }
  return b.Compile(r_report);
}

}  // namespace

/* Plan C1-9 E3: Get Average Center Position runs bridged. It reads its objects' live positions and
 * applies no pending move, so an NPC that averages a Leader (pushed by a tree before the crowd: it
 * has pending moves when the run starts, which the VM leaves pending too), the Target and itself,
 * before and after its own move, ends as on the lean VM; so does an average of vectors. */
TEST(LN_Columns, FlockCentersEndAsOnTheLeanVM)
{
  BridgeOn bridge;
  constexpr uint32_t kOwners = 40;
  for (const FlockSource flock : {FlockSource::ObjectsAndOwner,
                                  FlockSource::Vectors,
                                  FlockSource::Named,
                                  FlockSource::NamedThenFoundDistance,
                                  FlockSource::NamedBeforeDistance,
                                  FlockSource::LeaderOnly,
                                  FlockSource::NamedNoObjectDistance})
  {
    std::string report;
    const std::shared_ptr<LN_Program> program = FlockTree(flock, &report);
    ASSERT_NE(program, nullptr) << report;
    std::string refusal;
    const std::shared_ptr<const LN_ColumnProgram> columns = LN_CompileColumnProgram(
        *program, LN_Event::OnFixedUpdate, &refusal);
    ASSERT_NE(columns, nullptr) << refusal << "\n" << LN_DumpProgramText(*program);
    /* A source the same for every NPC is worked out once a run, natively: nothing bridged. */
    const std::string text = LN_DescribeColumnProgram(*columns);
    const bool once = flock != FlockSource::ObjectsAndOwner && flock != FlockSource::Vectors;
    EXPECT_EQ(text.find("averages_once=1") != std::string::npos, once) << text;
    EXPECT_EQ(text.find("bridged=1") != std::string::npos, !once) << text;
    const auto leader = [&](SemRig &rig) {
      if (flock == FlockSource::NamedNoObjectDistance) {
        /* A crowd NPC (Obj3) pushed before the crowd every step. */
        KX_GameObject &pusher = rig.AddObject("NpcPusher", MT_Vector3(1.0f, -2.0f, 0.0f));
        rig.SetString(pusher, "target", "Obj3");
        rig.AddTree(OtherTree(Other::Push, true), rig.ObjectCount() - 1, 7, true);
      }
      if (flock == FlockSource::NamedBeforeDistance) {
        /* The Target pushed before the crowd every step. */
        KX_GameObject &pusher = rig.AddObject("TargetPusher", MT_Vector3(1.0f, -2.0f, 0.0f));
        rig.SetString(pusher, "target", "Target");
        rig.AddTree(OtherTree(Other::Push, true), rig.ObjectCount() - 1, 7, true);
      }
      rig.AddObject("Leader", MT_Vector3(-3.0f, 2.0f, 0.0f));
      for (const auto &[kind, first, index] :
           {std::tuple{Other::Push, true, 5u}, std::tuple{Other::Measure, false, 3u}})
      {
        KX_GameObject &object = rig.AddObject("Other" + std::to_string(rig.ObjectCount()),
                                              MT_Vector3(1.0f, -2.0f, 0.0f));
        rig.SetString(object, "target", "Leader");
        rig.SetFloat(object, "seen", -1.0f);
        rig.AddTree(OtherTree(kind, true), rig.ObjectCount() - 1, index, first);
      }
    };
    const std::string what = "source " + std::to_string(int(flock));
    const uint64_t fallbacks = LN_ColumnBridgeFallbackCount();
    const uint64_t runtime_fallbacks = LN_ColumnRuntimeFallbackCount();
    const uint64_t lanes = ExpectColumnsMatch(
        program, LN_Event::OnFixedUpdate, kOwners, 12, what, 0, nullptr, leader);
    const uint64_t helper_lanes = ExpectColumnsMatch(
        program, LN_Event::OnFixedUpdate, kOwners, 12, what + " (3 helpers)", 3, nullptr, leader);
    if (flock == FlockSource::NamedThenFoundDistance) {
      /* The Leader is pushed before the crowd every step: the run gives its NPCs back. */
      if (!DirectForced()) {
        EXPECT_GT(LN_ColumnRuntimeFallbackCount(), runtime_fallbacks) << what;
      }
    }
    else if (flock == FlockSource::NamedBeforeDistance) {
      /* The first tree of each run plays on the VM (the Target's push), the rest in columns. */
      if (!DirectForced()) {
        EXPECT_GT(LN_ColumnBridgeFallbackCount(), fallbacks) << what;
      }
      EXPECT_GT(lanes, 0u) << what;
    }
    else {
      EXPECT_GT(lanes, 0u) << what;
      EXPECT_GT(helper_lanes, 0u) << what;
      EXPECT_EQ(LN_ColumnBridgeFallbackCount(), fallbacks) << what;
    }
  }
  /* The same with the Leader not pushed (nothing pending): the found-distance case runs. */
  std::string report;
  const std::shared_ptr<LN_Program> program = FlockTree(FlockSource::NamedThenFoundDistance,
                                                        &report);
  ASSERT_NE(program, nullptr) << report;
  const auto still_leader = [&](SemRig &rig) {
    rig.AddObject("Leader", MT_Vector3(-3.0f, 2.0f, 0.0f));
  };
  EXPECT_GT(ExpectColumnsMatch(program, LN_Event::OnFixedUpdate, kOwners, 8, "still leader", 0,
                               nullptr, still_leader),
            0u);
}

namespace {

/** How a crowd NPC names the object whose properties it writes (plan C1-10). */
enum class Victim {
  /* The Target, named (every NPC writes one object). */
  Named,
  /* Get Object By Name of its own text property "victim" (another NPC, the Target, a missing
   * name, an empty one). */
  ByProperty,
  /* Get Parent of its child Turret (itself, through a find: the snapshot's own object). */
  ParentOfChild,
  /* Get Parent of the turret its "victim" property names (that turret's NPC, a lane). */
  ParentOfFound,
  /* The object it touches (Collision Hit Object; the rig has no physics: none, nothing written). */
  Collision,
};

/** On Logic + Physics Step: the NPC writes its own "hits" (a float from t1: missing, so the first
 * write the flush applies makes it, a float or the int a neighbour writes), then the victim's
 * "hp" (from t2), changes the victim's "score" by t3 (clamped between t4 and 50 when `clamped`)
 * and sets the victim's "hits" to 7 (an int); the victim's "seen" last (a bool). */
std::shared_ptr<LN_Program> VictimTree(const Victim victim, const bool clamped, std::string *r_report)
{
  TreeBuilder b;
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode *found = nullptr;
  const char *out = "Object";
  const auto named_by_property = [&]() -> GenNode & {
    GenNode &name = b.Add("LogicNativeGetGamePropertyString");
    b.String(name, "Property", "victim");
    GenNode &by_name = b.Add("LogicNativeObjectByName");
    b.Link(name, "Value", by_name, "Name");
    return by_name;
  };
  switch (victim) {
    case Victim::Named:
      break;
    case Victim::ByProperty:
      found = &named_by_property();
      break;
    case Victim::ParentOfChild: {
      GenNode &owner = b.Add("LogicNativeGetOwner");
      GenNode &child = b.Add("LogicNativeGetChildByName");
      b.Link(owner, "Owner Object", child, "Parent");
      b.ObjectNamed(child, "Child", &TurretBlenderObject());
      found = &b.Add("LogicNativeGetParent");
      b.Link(child, "Child", *found, "Child Object");
      out = "Parent Object";
      break;
    }
    case Victim::ParentOfFound: {
      GenNode &turret = named_by_property();
      found = &b.Add("LogicNativeGetParent");
      b.Link(turret, "Object", *found, "Child Object");
      out = "Parent Object";
      break;
    }
    case Victim::Collision:
      found = &b.Add("LogicNativeCollision");
      out = "Collided Object";
      break;
  }
  const auto target = [&](GenNode &node) {
    if (found != nullptr) {
      b.Link(*found, out, node, "Object");
    }
    else {
      b.ObjectNamed(node, "Object", &diff::TargetBlenderObject());
    }
  };
  const auto live = [&](const char *property) -> GenNode & {
    GenNode &read = b.Add("LogicNativeGetGamePropertyFloat");
    b.String(read, "Property", property);
    return read;
  };
  GenNode &own = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(own, "Property", "hits");
  b.Link(live("t1"), "Value", own, "Value");
  b.Link(step, "Out", own, "Flow");
  GenNode &hp = b.Add("LogicNativeSetGamePropertyFloat");
  target(hp);
  b.String(hp, "Property", "hp");
  b.Link(live("t2"), "Value", hp, "Value");
  b.Link(own, "Done", hp, "Flow");
  GenNode &score = b.Add("LogicNativeModifyProperty", int16_t(blender::NODE_MATH_ADD), clamped ? 1 : 0);
  target(score);
  b.String(score, "Property", "score");
  b.Link(live("t3"), "Value", score, "Value");
  if (clamped) {
    b.Link(live("t4"), "Value", score, "Min");
    b.Float(score, "Max", 50.0f);
  }
  b.Link(hp, "Done", score, "Flow");
  GenNode &hits = b.Add("LogicNativeSetGamePropertyInt");
  target(hits);
  b.String(hits, "Property", "hits");
  b.Int(hits, "Value", 7);
  b.Link(score, "Done", hits, "Flow");
  GenNode &seen = b.Add("LogicNativeSetGamePropertyBool");
  target(seen);
  b.String(seen, "Property", "seen");
  b.Bool(seen, "Value", true);
  b.Link(hits, "Done", seen, "Flow");
  return b.Compile(r_report);
}

/** The crowd's victims and their properties: "hp" a float on a third, an int on a third, missing
 * on the rest (a write makes it: the first write of the flush decides its type, so the merge of
 * the writes must be the VM's), "score" on half; each NPC's "victim" names another NPC, the
 * Target, nobody or nothing (by turret names for ParentOfFound). */
void AddVictims(SemRig &rig, const uint32_t owners, const Victim victim)
{
  for (uint32_t index = 0; index < owners; index++) {
    KX_GameObject &object = rig.Object(index);
    if (index % 3 == 0) {
      rig.SetFloat(object, "hp", 10.0f + float(index));
    }
    else if (index % 3 == 1) {
      rig.SetInt(object, "hp", int(index));
    }
    if (index % 2 == 0) {
      rig.SetFloat(object, "score", float(index % 5));
    }
    /* Far, or the next NPC (its own "hits" write comes right after this one's write of it: the
     * flush's merge decides which makes the property). */
    const uint32_t other = index % 4 == 0 ? (index * 7 + 3) % owners : (index + 1) % owners;
    std::string name;
    switch (index % 4) {
      case 0:
      case 3:
        name = victim == Victim::ParentOfFound ?
                   (other == 0 ? std::string("Turret") : "Turret" + std::to_string(other)) :
                   "Obj" + std::to_string(other);
        break;
      case 1:
        name = victim == Victim::ParentOfFound ? "Turret1" : "Target";
        break;
      case 2:
        name = index % 8 == 2 ? "Nobody" : "";
        break;
    }
    rig.SetString(object, "victim", name.c_str());
  }
  /* The Target's own: an int hp (a float write keeps it an int), a score. */
  for (size_t index = 0; index < rig.ObjectCount(); index++) {
    if (rig.Object(index).GetName() == "Target") {
      rig.SetInt(rig.Object(index), "hp", 100);
      rig.SetFloat(rig.Object(index), "score", 1.5f);
    }
  }
  if (victim == Victim::ParentOfChild || victim == Victim::ParentOfFound) {
    /* Every NPC but every fifth a child Turret (ParentOfFound: Turret, Turret1, Turret2...), and
     * the trees that place, push and measure the object named Turret next to the crowd. */
    AddTurrets(rig, owners, true, victim == Victim::ParentOfFound);
  }
}

}  // namespace

/* Plan C1-10: property writes and changes of an object other than the owner run in columns: a
 * named one, one found by the name the NPC keeps in a text property, the parent of a found one
 * (also the NPC itself), the object a collision finds. A victim found while playing is found
 * after the run, NPC by NPC, where a write's op ran; the writes are applied at each NPC's sort
 * key and listed for the flush's merge whenever they may share a key (another NPC's write of a
 * lane's property, the Target's). The worlds and the trees' states stay identical tick by tick,
 * with the merge on (80+ writes a step) and off, with and without helpers. */
TEST(LN_Columns, PropertyWritesOfOtherObjectsEndAsOnTheLeanVM)
{
  constexpr uint32_t kOwners = 40;
  for (const bool bridged : {false, true}) {
    BridgeOn bridge(bridged);
    for (const Victim victim : {Victim::Named,
                                Victim::ByProperty,
                                Victim::ParentOfChild,
                                Victim::ParentOfFound,
                                Victim::Collision})
    {
      for (const bool clamped : {false, true}) {
        std::string report;
        const std::shared_ptr<LN_Program> program = VictimTree(victim, clamped, &report);
        ASSERT_NE(program, nullptr) << report;
        std::string refusal;
        const std::shared_ptr<const LN_ColumnProgram> columns = LN_CompileColumnProgram(
            *program, LN_Event::OnFixedUpdate, &refusal);
        ASSERT_NE(columns, nullptr) << refusal << "\n" << LN_DumpProgramText(*program);
        const std::string text = LN_DescribeColumnProgram(*columns);
        /* Native writes: nothing bridged; a found victim is found after the run. */
        EXPECT_NE(text.find("bridged=0"), std::string::npos) << text;
        EXPECT_EQ(text.find("found_after_run=1") != std::string::npos, victim != Victim::Named)
            << text;
        EXPECT_EQ(text.find("collision_finds=1") != std::string::npos,
                  victim == Victim::Collision)
            << text;
        const std::string what = std::string(bridged ? "bridge on, " : "") + "victim " +
                                 std::to_string(int(victim)) + (clamped ? " clamped" : "");
        const auto victims = [&](SemRig &rig) {
          AddVictims(rig, kOwners, victim);
          rig.BindIds();
        };
        const uint64_t fallbacks = LN_ColumnRuntimeFallbackCount();
        for (const uint32_t owners : {kOwners, 20u}) {
          /* 40 NPCs: 200 writes a step (the merge on); 20: under the flush's 64-write gate
           * with the Target's and the other trees' writes alone (some ticks merge, some not). */
          const std::function<void(SemRig &)> some = [&](SemRig &rig) {
            AddVictims(rig, owners, victim);
            rig.BindIds();
          };
          EXPECT_GT(ExpectColumnsMatch(program, LN_Event::OnFixedUpdate, owners, 10,
                                       what + " (" + std::to_string(owners) + " NPCs)", 0,
                                       nullptr, some, 2),
                    0u)
              << what;
        }
        EXPECT_GT(ExpectColumnsMatch(program, LN_Event::OnFixedUpdate, kOwners, 10,
                                     what + " (3 helpers)", 3, nullptr, victims, 2),
                  0u)
            << what;
        /* The crowd never gives its lanes back (the turret trees' measure may: one lane). */
        EXPECT_LE(LN_ColumnRuntimeFallbackCount() - fallbacks, 3u * 10u) << what;
      }
    }
  }
}

/* Plan C1-10's limits: a collision's object only written, in a program whose reads apply no
 * pending move and that has no On Exit; a group mixes no program finding objects with one adding
 * copies, nor one finding a collision's object with one applying moves (those objects' trees run
 * on the VM, exactly). */
TEST(LN_Columns, ACollisionsObjectIsOnlyWrittenAndGroupsKeepFindsApart)
{
  /* (On Exit is bridged: with the bridge off it is refused before this rule is asked.) */
  BridgeOn bridge;
  const auto collision_tree = [](const int extra, std::string *r_report) {
    TreeBuilder b;
    GenNode &step = b.Add("LogicNativeOnFixedUpdate");
    GenNode &touch = b.Add(extra == 1 ? "LogicNativeOnCollision" : "LogicNativeCollision");
    GenNode &write = b.Add("LogicNativeSetGamePropertyFloat");
    b.Link(touch, "Collided Object", write, "Object");
    b.String(write, "Property", "hp");
    b.Float(write, "Value", 1.0f);
    if (extra == 1) {
      /* On Exit's route (it marks the payload Collided Object gives). */
      b.Link(touch, "On Exit", write, "Flow");
    }
    else {
      b.Link(step, "Out", write, "Flow");
    }
    if (extra == 2) {
      /* A distance from the owner: its read applies pending moves. */
      GenNode &distance = b.Add("LogicNativeGetDistance", 0, 0);
      b.ObjectNamed(distance, "Object B", &diff::TargetBlenderObject());
      GenNode &keep = b.Add("LogicNativeSetGamePropertyFloat");
      b.String(keep, "Property", "d");
      b.Link(distance, "Distance", keep, "Value");
      b.Link(write, "Done", keep, "Flow");
    }
    if (extra == 3) {
      /* The touched object moved. */
      GenNode &push = b.Add("LogicNativeApplyMovement");
      b.Link(touch, "Collided Object", push, "Object");
      b.Vector(push, "Vector", 0.1f, 0.0f, 0.0f);
      b.Bool(push, "Local", false);
      b.Link(write, "Done", push, "Flow");
    }
    return b.Compile(r_report);
  };
  /* (A distance next to it lowers: such a run starts only when nothing it applies is pending.) */
  const char *const refusals[] = {nullptr, "next to an On Exit", nullptr, "read or moved"};
  for (int extra = 0; extra < 4; extra++) {
    std::string report;
    const std::shared_ptr<LN_Program> program = collision_tree(extra, &report);
    ASSERT_NE(program, nullptr) << report;
    std::string refusal;
    const std::shared_ptr<const LN_ColumnProgram> columns = LN_CompileColumnProgram(
        *program, LN_Event::OnFixedUpdate, &refusal);
    if (refusals[extra] == nullptr) {
      EXPECT_NE(columns, nullptr) << refusal;
    }
    else {
      EXPECT_EQ(columns, nullptr) << extra;
      EXPECT_NE(refusal.find(refusals[extra]), std::string::npos) << refusal;
    }
  }
  std::string report;
  {
    /* A collision's object writer that measures the Target, pushed before the crowd every step:
     * the run starts quietly (its first NPC plays on the VM, whose read applies the push). */
    const std::shared_ptr<LN_Program> measuring = collision_tree(2, &report);
    ASSERT_NE(measuring, nullptr) << report;
    const std::string text = LN_DescribeColumnProgram(
        *LN_CompileColumnProgram(*measuring, LN_Event::OnFixedUpdate, nullptr));
    EXPECT_NE(text.find("collision_finds=1"), std::string::npos) << text;
    const auto pushed = [&](SemRig &rig) {
      KX_GameObject &pusher = rig.AddObject("TargetPusher", MT_Vector3(1.0f, -2.0f, 0.0f));
      rig.SetString(pusher, "target", "Target");
      rig.AddTree(OtherTree(Other::Push, true), rig.ObjectCount() - 1, 7, true);
    };
    const uint64_t fallbacks = LN_ColumnBridgeFallbackCount();
    EXPECT_GT(ExpectColumnsMatch(measuring, LN_Event::OnFixedUpdate, 40, 6, "quiet start", 0,
                                 nullptr, pushed, 2),
              0u);
    if (!DirectForced()) {
      EXPECT_GT(LN_ColumnBridgeFallbackCount(), fallbacks);
    }
  }
  /* A collision's object writer next to a tree measuring the owner, on every NPC: no group (the
   * measure's applies could drop the contacts a later NPC reads). Since plan C2 P2 each tree runs
   * alone in its place (on the VM before), exact. */
  const std::shared_ptr<LN_Program> writer = collision_tree(0, &report);
  ASSERT_NE(writer, nullptr) << report;
  BrainOfOptions options;
  options.step_every = 1;
  const std::shared_ptr<LN_Program> brain = BrainOf(options);
  ASSERT_NE(brain, nullptr);
  constexpr uint32_t kOwners = 40;
  const auto two_trees = [&](SemRig &rig) { rig.AddSecondTrees(brain, kOwners); };
  std::string group_refusal;
  EXPECT_GT(ExpectColumnsMatch(writer, LN_Event::OnFixedUpdate, kOwners, 6, "collision + brain",
                               0, &group_refusal, two_trees),
            0u);
  EXPECT_EQ(group_refusal.find("a group whose trees change what another one finds"),
            std::string::npos)
      << group_refusal;
  /* The same writer next to a tree that applies nothing groups. */
  const std::shared_ptr<LN_Program> victim_writer = VictimTree(Victim::Named, false, &report);
  ASSERT_NE(victim_writer, nullptr) << report;
  const auto calm_trees = [&](SemRig &rig) { rig.AddSecondTrees(victim_writer, kOwners); };
  EXPECT_GT(ExpectColumnsMatch(writer, LN_Event::OnFixedUpdate, kOwners, 6, "collision + writer",
                               0, nullptr, calm_trees),
            0u);
}

/* Plan C1-10: a lane another program's lane writes a property of lists its own writes for the
 * flush's merge. The crowd writes its own "hits" (a float) and "mark"; writer trees between its
 * NPCs (their own runs) write the next NPC's "hits" as an int, right before that NPC's own write.
 * The merge decides only where "hits" is missing (the first write the flush applies makes it; a
 * later one converts in place): both start writing at age 3 (an "age" each counts after its
 * writes), when the writers are lanes too. The VM's merge leaves the writer's int out (the NPC's
 * float supersedes it): the crowd's write must be in the merge's walk. */
TEST(LN_Columns, ALaneAnotherProgramWritesListsItsOwnWrites)
{
  /* step: Sequence (Then 0: age > 2.5 ? write : nothing; Then 1: age + 1), the writer's age + 1
   * first (a property change ends the merge's run: none between its write and the NPC's). */
  const auto aged = [](const bool crowd, std::string *r_report) {
    TreeBuilder b;
    GenNode &step = b.Add("LogicNativeOnFixedUpdate");
    GenNode &sequence = b.Add("LogicNativeSequence", 0, 0, 2);
    b.Link(step, "Out", sequence, "Flow");
    GenNode &age = b.Add("LogicNativeGetGamePropertyFloat");
    b.String(age, "Property", "age");
    GenNode &old_enough = b.Add("LogicNativeCompare", 4);
    b.Link(age, "Value", old_enough, "A");
    b.Float(old_enough, "B", 2.5f);
    GenNode &branch = b.Add("LogicNativeBranch");
    b.Link(old_enough, "Result", branch, "Condition");
    b.Link(sequence, crowd ? "Then 0" : "Then 1", branch, "Flow");
    if (crowd) {
      GenNode &hits = b.Add("LogicNativeSetGamePropertyFloat");
      b.String(hits, "Property", "hits");
      GenNode &t1 = b.Add("LogicNativeGetGamePropertyFloat");
      b.String(t1, "Property", "t1");
      b.Link(t1, "Value", hits, "Value");
      b.Link(branch, "True", hits, "Flow");
      GenNode &mark = b.Add("LogicNativeSetGamePropertyFloat");
      b.String(mark, "Property", "mark");
      GenNode &t2 = b.Add("LogicNativeGetGamePropertyFloat");
      b.String(t2, "Property", "t2");
      b.Link(t2, "Value", mark, "Value");
      b.Link(hits, "Done", mark, "Flow");
    }
    else {
      GenNode &name = b.Add("LogicNativeGetGamePropertyString");
      b.String(name, "Property", "victim");
      GenNode &victim = b.Add("LogicNativeObjectByName");
      b.Link(name, "Value", victim, "Name");
      GenNode &hits = b.Add("LogicNativeSetGamePropertyInt");
      b.Link(victim, "Object", hits, "Object");
      b.String(hits, "Property", "hits");
      b.Int(hits, "Value", 7);
      b.Link(branch, "True", hits, "Flow");
    }
    GenNode &grow = b.Add("LogicNativeModifyProperty", int16_t(blender::NODE_MATH_ADD), 0);
    b.String(grow, "Property", "age");
    b.Float(grow, "Value", 1.0f);
    b.Link(sequence, crowd ? "Then 1" : "Then 0", grow, "Flow");
    return b.Compile(r_report);
  };
  std::string report;
  const std::shared_ptr<LN_Program> crowd = aged(true, &report);
  ASSERT_NE(crowd, nullptr) << report;
  const std::shared_ptr<LN_Program> writer = aged(false, &report);
  ASSERT_NE(writer, nullptr) << report;
  std::string refusal;
  ASSERT_NE(LN_CompileColumnProgram(*crowd, LN_Event::OnFixedUpdate, &refusal), nullptr)
      << refusal;
  ASSERT_NE(LN_CompileColumnProgram(*writer, LN_Event::OnFixedUpdate, &refusal), nullptr)
      << refusal;
  constexpr uint32_t kOwners = 40;
  /* The writers' first pass runs their On Init on the VM: one of the two start ages lines their
   * first write up with the crowd's. */
  for (const float writer_age : {0.0f, 1.0f}) {
    for (const uint32_t writers : {20u, 3u}) {
      const auto interleaved = [&](SemRig &rig) {
        for (uint32_t index = 0; index < kOwners; index++) {
          rig.SetFloat(rig.Object(index), "age", 0.0f);
        }
        for (uint32_t k = 0; k < writers; k++) {
          KX_GameObject &object = rig.AddObject("Writer" + std::to_string(k),
                                                MT_Vector3(0.0f, -3.0f, 0.0f));
          /* NPC k+1 (scene index 2k + 2) records right after this writer (2k + 1). */
          rig.SetString(object, "victim", ("Obj" + std::to_string(k + 1)).c_str());
          rig.SetFloat(object, "age", writer_age);
          rig.AddTree(writer, rig.ObjectCount() - 1, 2 * k + 1, false);
        }
        rig.BindIds(); /* as the manager: one property's writes share a key across trees */
      };
      const std::string what = std::to_string(writers) + " writers from age " +
                               std::to_string(int(writer_age));
      EXPECT_GT(ExpectColumnsMatch(crowd, LN_Event::OnFixedUpdate, kOwners, 7, what, 0, nullptr,
                                   interleaved, 2),
                0u)
          << what;
    }
  }
}

namespace {

/** Which object a crowd NPC acts on (plan C1-11), and what it does with it. */
enum class Acted {
  /* Its child turret: everything (moves too: a turret is no NPC). */
  TurretChild,
  /* Its child turret: a Translate and the axis read only (the move alone notes the turret
   * moved). */
  TurretTranslateOnly,
  /* The object its "victim" text names (another NPC, the Target, nobody): no move (a move of
   * another NPC gives the run back). */
  ByProperty,
  /* The Target by name: everything. */
  Named,
  /* The Target by name: a Translate only (no parent's moves carry it along: a tree measuring
   * the Target after the crowd must find the crowd's moves of it). */
  NamedTranslateOnly,
  /* The object its "victim" text names: its axis read only (found when the run starts for
   * that read alone). */
  AxisOnly,
  /* The object it touches: written only (motions, visibility, color). */
  Collision,
};

/** On Logic + Physics Step, on the acted-on object: Translate, Rotate To, Align Axis To Vector,
 * Move To (its Reached sets "reached": a pose read), Set Visible, Set Object Color, a World
 * Position with Teleport (a move), and its Y axis's X read into pf3; `bridged`: a Print too. */
std::shared_ptr<LN_Program> ActsTree(const Acted acted, const bool bridged, std::string *r_report)
{
  TreeBuilder b;
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode *found = nullptr;
  const char *out = "Object";
  switch (acted) {
    case Acted::TurretChild:
    case Acted::TurretTranslateOnly: {
      GenNode &owner = b.Add("LogicNativeGetOwner");
      found = &b.Add("LogicNativeGetChildByName");
      b.Link(owner, "Owner Object", *found, "Parent");
      b.ObjectNamed(*found, "Child", &TurretBlenderObject());
      out = "Child";
      break;
    }
    case Acted::ByProperty:
    case Acted::AxisOnly: {
      GenNode &name = b.Add("LogicNativeGetGamePropertyString");
      b.String(name, "Property", "victim");
      found = &b.Add("LogicNativeObjectByName");
      b.Link(name, "Value", *found, "Name");
      break;
    }
    case Acted::Named:
    case Acted::NamedTranslateOnly:
      break;
    case Acted::Collision:
      found = &b.Add("LogicNativeCollision");
      out = "Collided Object";
      break;
  }
  const auto object = [&](GenNode &node) {
    if (found != nullptr) {
      b.Link(*found, out, node, "Object");
    }
    else {
      b.ObjectNamed(node, "Object", &diff::TargetBlenderObject());
    }
  };
  const auto live = [&](const char *property) -> GenNode & {
    GenNode &read = b.Add("LogicNativeGetGamePropertyFloat");
    b.String(read, "Property", property);
    return read;
  };
  GenNode &pb0 = b.Add("LogicNativeGetGamePropertyBool");
  b.String(pb0, "Property", "pb0");
  const bool moves = acted == Acted::TurretChild || acted == Acted::Named ||
                     acted == Acted::TurretTranslateOnly || acted == Acted::NamedTranslateOnly;
  const bool reads = acted != Acted::Collision && acted != Acted::NamedTranslateOnly;
  const bool all = acted != Acted::TurretTranslateOnly && acted != Acted::NamedTranslateOnly &&
                   acted != Acted::AxisOnly;
  GenNode *tail = &step;
  const char *tail_out = "Out";
  const auto chain = [&](GenNode &node, const char *done = "Done") {
    b.Link(*tail, tail_out, node, "Flow");
    tail = &node;
    tail_out = done;
  };
  if (moves) {
    GenNode &translate = b.Add("LogicNativeTranslate");
    object(translate);
    b.Vector(translate, "Vector", 0.05f, -0.02f, 0.0f);
    b.Link(live("pf2"), "Value", translate, "Speed");
    b.Bool(translate, "Local", false);
    chain(translate, "When Done");
  }
  if (all) {
    GenNode &rotate = b.Add("LogicNativeRotateToward");
    object(rotate);
    b.Vector(rotate, "Target", 3.0f, 1.0f, 0.5f);
    b.Link(live("pf1"), "Value", rotate, "Factor");
    chain(rotate);
    GenNode &align = b.Add("LogicNativeAlignAxisToVector");
    object(align);
    b.Vector(align, "Vector", 0.0f, 0.3f, 1.0f);
    b.Float(align, "Factor", 0.5f);
    chain(align);
    GenNode &move = b.Add("LogicNativeMoveToward");
    object(move);
    b.Vector(move, "Target Position", 2.0f, 2.0f, 0.0f);
    b.Link(live("pf2"), "Value", move, "Speed");
    b.Float(move, "Stop Distance", 0.5f);
    chain(move);
    if (reads) {
      GenNode &reached = b.Add("LogicNativeSetGamePropertyBool");
      b.String(reached, "Property", "reached");
      b.Bool(reached, "Value", true);
      b.Link(move, "Reached", reached, "Flow");
    }
    GenNode &visible = b.Add("LogicNativeSetObjectAttribute", 12, 15);
    object(visible);
    b.Link(pb0, "Value", visible, "Visible");
    chain(visible);
    GenNode &rgba = b.Add("LogicNativeColorRGBA");
    b.Link(live("pf0"), "Value", rgba, "R");
    b.Link(live("pf1"), "Value", rgba, "B");
    GenNode &color = b.Add("LogicNativeSetObjectAttribute", 11, 15);
    object(color);
    b.Link(rgba, "Color", color, "Color");
    chain(color);
  }
  if (moves && all) {
    GenNode &place = b.Add("LogicNativeSetObjectAttribute", 0, 15);
    object(place);
    b.Vector(place, "Value", 1.0f, 2.0f, 1.5f);
    b.Link(pb0, "Value", place, "Teleport");
    chain(place);
  }
  if (reads) {
    GenNode &axis = b.Add("LogicNativeGetAxisVector", 1);
    object(axis);
    GenNode &split = b.Add("LogicNativeSeparateXYZ");
    b.Link(axis, "Vector", split, "Vector");
    GenNode &keep = b.Add("LogicNativeSetGamePropertyFloat");
    b.String(keep, "Property", "pf3");
    b.Link(split, "X", keep, "Value");
    chain(keep);
  }
  if (bridged) {
    GenNode &say = b.Add("LogicNativePrint");
    b.Link(live("pf1"), "Value", say, "Message");
    chain(say);
  }
  return b.Compile(r_report);
}

}  // namespace

/* Plan C1-11: motions (Translate, Rotate To, Align Axis To Vector, Move To with Reached), Set
 * Visible, Set Object Color and a Teleport of an object other than the owner, and its axis read
 * live, in columns: a child turret, an object named by a text property (another NPC, the Target,
 * nobody), the Target by name, a collision's object (written only). Motions and object state
 * apply at the flush, at the NPC's place. Every object is turned a little when the world is made
 * (the rig updates no world pose after a flush, so motions show only in local poses, and an
 * object's axis must differ from its NPC's). Native and bridged, with helpers; the crowd runs in
 * columns every tick (no run given back). */
TEST(LN_Columns, MotionsAndStatesOfOtherObjectsEndAsOnTheLeanVM)
{
  constexpr uint32_t kOwners = 40;
  for (const bool bridged : {false, true}) {
    BridgeOn bridge(bridged);
    for (const Acted acted : {Acted::TurretChild,
                              Acted::TurretTranslateOnly,
                              Acted::ByProperty,
                              Acted::Named,
                              Acted::NamedTranslateOnly,
                              Acted::AxisOnly,
                              Acted::Collision})
    {
      std::string report;
      const std::shared_ptr<LN_Program> program = ActsTree(acted, bridged, &report);
      ASSERT_NE(program, nullptr) << report;
      std::string refusal;
      const std::shared_ptr<const LN_ColumnProgram> columns = LN_CompileColumnProgram(
          *program, LN_Event::OnFixedUpdate, &refusal);
      ASSERT_NE(columns, nullptr) << refusal << "\n" << LN_DumpProgramText(*program);
      const std::string what = std::string(bridged ? "bridged " : "") + "acted " +
                               std::to_string(int(acted));
      const bool turrets = acted == Acted::TurretChild || acted == Acted::TurretTranslateOnly;
      const auto world = [&](SemRig &rig) {
        if (turrets) {
          AddTurrets(rig, kOwners, true);
        }
        else {
          AddVictims(rig, kOwners, Victim::ByProperty);
        }
        if (acted == Acted::Named || acted == Acted::NamedTranslateOnly) {
          /* A tree measuring the Target between NPC 1 and NPC 2 (recorded after the crowd). */
          rig.AddObject("TargetMeasurer", MT_Vector3(1.0f, -2.0f, 0.0f));
          rig.SetFloat(rig.Object(rig.ObjectCount() - 1), "seen", -1.0f);
          rig.AddTree(OtherTree(Other::MeasureNamed, true), rig.ObjectCount() - 1, 3, false);
        }
        for (uint32_t index = 0; index < kOwners; index++) {
          rig.SetFloat(rig.Object(index), "pf2", 0.5f + float(index % 4));
        }
        /* Every object turned (its own angle), its world pose updated. */
        for (size_t index = 0; index < rig.ObjectCount(); index++) {
          KX_GameObject &object = rig.Object(index);
          const float angle = 0.3f + float(index % 9) * 0.41f;
          object.NodeSetLocalOrientation(MT_Matrix3x3(MT_Vector3(0.2f, -0.1f, angle)));
        }
        for (size_t index = 0; index < rig.ObjectCount(); index++) {
          rig.Object(index).NodeUpdateGS(0.0);
        }
        rig.BindIds();
      };
      const uint64_t fallbacks = LN_ColumnRuntimeFallbackCount();
      EXPECT_GT(ExpectColumnsMatch(program, LN_Event::OnFixedUpdate, kOwners, 12, what, 0,
                                   nullptr, world, 2),
                0u)
          << what;
      EXPECT_GT(ExpectColumnsMatch(program, LN_Event::OnFixedUpdate, kOwners, 12,
                                   what + " (3 helpers)", 3, nullptr, world, 2),
                0u)
          << what;
      /* (The turret trees' measure gives its own lane back at each pass: one a tick.) */
      EXPECT_LE(LN_ColumnRuntimeFallbackCount() - fallbacks, 2u * 12u) << what;
    }
  }
}

namespace {

/** What a crowd NPC keeps in tree properties (plan C1-11). */
enum class Memory {
  /* "home": a vector (native), read as a vector (Move To's target, its X) and as a number (a
   * generic value's conversion after a vector write). */
  HomeVector,
  /* "target": an object (Get Object By Name of its "victim" text: the bridge writes it), then a
   * Modify Property, a Set Game Property, a Rotate To and a Set Visible of that object. */
  TargetObject,
  /* "mood": text (bridged), copied into a game property (a generic write, bridged); "count": a
   * number (native) next to it. */
  MoodText,
  /* "tint": a colour (bridged), the owner's colour set from it. */
  TintColor,
  /* "n": a number (native), then text (bridged) into the same property, then read as numbers:
   * the fields read again after the bridged write. */
  ChangesType,
  /* A constant text into a tree property (native: the value as it is), copied out. */
  ConstantText,
  /* "aim": a rotation (Vector To Rotation, bridged), read back as a vector (its Euler angles). */
  AimRotation,
};

std::shared_ptr<LN_Program> MemoryTree(const Memory memory, std::string *r_report)
{
  /* Each statement on its own output of a Sequence, in order (Set Tree Property has no Done:
   * it ends its route). */
  TreeBuilder b;
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode &sequence = b.Add("LogicNativeSequence", 0, 0, 4);
  b.Link(step, "Out", sequence, "Flow");
  GenNode *tail = nullptr;
  std::string tail_out;
  int next_route = 0;
  const auto route = [&]() {
    tail = &sequence;
    tail_out = "Then " + std::to_string(next_route++);
  };
  const auto chain = [&](GenNode &node, const char *done = "Done") {
    b.Link(*tail, tail_out.c_str(), node, "Flow");
    tail = &node;
    tail_out = done;
  };
  const auto live = [&](const char *property) -> GenNode & {
    GenNode &read = b.Add("LogicNativeGetGamePropertyFloat");
    b.String(read, "Property", property);
    return read;
  };
  const auto get = [&](const char *property) -> GenNode & {
    GenNode &read = b.Add("LogicNativeGetTreeProperty");
    b.String(read, "Property", property);
    return read;
  };
  /* A Set Tree Property ending the current route. */
  const auto set = [&](const char *property, GenNode &from, const char *out) {
    GenNode &write = b.Add("LogicNativeSetTreeProperty");
    b.String(write, "Property", property);
    b.Link(from, out, write, "Value");
    b.Link(*tail, tail_out.c_str(), write, "Flow");
  };
  const auto keep_float = [&](const char *property, GenNode &from, const char *out) {
    GenNode &write = b.Add("LogicNativeSetGamePropertyFloat");
    b.String(write, "Property", property);
    b.Link(from, out, write, "Value");
    chain(write);
  };
  const auto victim_name = [&]() -> GenNode & {
    GenNode &name = b.Add("LogicNativeGetGamePropertyString");
    b.String(name, "Property", "victim");
    return name;
  };
  GenNode &pb0 = b.Add("LogicNativeGetGamePropertyBool");
  b.String(pb0, "Property", "pb0");
  const auto behind_pb0 = [&]() {
    GenNode &branch = b.Add("LogicNativeBranch");
    b.Link(pb0, "Value", branch, "Condition");
    chain(branch, "True");
  };
  switch (memory) {
    case Memory::HomeVector: {
      route();
      GenNode &before = b.Add("LogicNativeSeparateXYZ");
      b.Link(get("home"), "Value", before, "Vector");
      keep_float("hx0", before, "X");
      route();
      behind_pb0();
      GenNode &where = b.Add("LogicNativeCombineXYZ");
      b.Link(live("pf0"), "Value", where, "X");
      b.Link(live("pf1"), "Value", where, "Y");
      b.Float(where, "Z", 0.5f);
      set("home", where, "Vector");
      route();
      GenNode &go = b.Add("LogicNativeMoveToward");
      b.Link(get("home"), "Value", go, "Target Position");
      b.Link(live("pf2"), "Value", go, "Speed");
      b.Float(go, "Stop Distance", 0.25f);
      chain(go);
      GenNode &after = b.Add("LogicNativeSeparateXYZ");
      b.Link(get("home"), "Value", after, "Vector");
      keep_float("hx1", after, "X");
      keep_float("hf", get("home"), "Value");
      /* A vector as a truth value: its length (the VM's ValueAsBool), never a number field. */
      GenNode &truth = b.Add("LogicNativeSetGamePropertyBool");
      b.String(truth, "Property", "hb");
      b.Link(get("home"), "Value", truth, "Value");
      chain(truth);
      break;
    }
    case Memory::TargetObject: {
      route();
      behind_pb0();
      GenNode &found = b.Add("LogicNativeObjectByName");
      b.Link(victim_name(), "Value", found, "Name");
      set("target", found, "Object");
      route();
      GenNode &target = get("target");
      GenNode &hit = b.Add("LogicNativeModifyProperty", int16_t(blender::NODE_MATH_ADD), 0);
      b.Link(target, "Value", hit, "Object");
      b.String(hit, "Property", "score");
      b.Link(live("pf0"), "Value", hit, "Value");
      chain(hit);
      GenNode &mark = b.Add("LogicNativeSetGamePropertyFloat");
      b.Link(target, "Value", mark, "Object");
      b.String(mark, "Property", "mark");
      b.Link(live("pf1"), "Value", mark, "Value");
      chain(mark);
      GenNode &aim = b.Add("LogicNativeRotateToward");
      b.Link(target, "Value", aim, "Object");
      b.Vector(aim, "Target", 3.0f, 1.0f, 0.5f);
      b.Float(aim, "Factor", 0.5f);
      chain(aim);
      GenNode &show = b.Add("LogicNativeSetObjectAttribute", 12, 15);
      b.Link(target, "Value", show, "Object");
      b.Link(pb0, "Value", show, "Visible");
      chain(show);
      break;
    }
    case Memory::MoodText: {
      route();
      set("mood", victim_name(), "Value");
      route();
      GenNode &copy = b.Add("LogicNativeSetGameProperty");
      b.String(copy, "Property", "mood_copy");
      b.Link(get("mood"), "Value", copy, "Value");
      chain(copy);
      route();
      set("count", live("pf0"), "Value");
      route();
      keep_float("cnt", get("count"), "Value");
      break;
    }
    case Memory::TintColor: {
      route();
      GenNode &rgba = b.Add("LogicNativeColorRGBA");
      b.Link(live("pf0"), "Value", rgba, "R");
      b.Float(rgba, "G", 0.25f);
      b.Link(live("pf1"), "Value", rgba, "B");
      set("tint", rgba, "Color");
      route();
      GenNode &paint = b.Add("LogicNativeSetObjectAttribute", 11, 15);
      b.Link(get("tint"), "Value", paint, "Color");
      chain(paint);
      break;
    }
    case Memory::ChangesType: {
      route();
      set("n", live("pf0"), "Value");
      route();
      keep_float("n1", get("n"), "Value");
      route();
      behind_pb0();
      set("n", victim_name(), "Value");
      route();
      keep_float("n2", get("n"), "Value");
      GenNode &as_int = b.Add("LogicNativeSetGamePropertyInt");
      b.String(as_int, "Property", "n3");
      b.Link(get("n"), "Value", as_int, "Value");
      chain(as_int);
      break;
    }
    case Memory::ConstantText: {
      route();
      GenNode &calm = b.Add("LogicNativeValueString");
      b.OutputString(calm, "String", "calm");
      set("label", calm, "String");
      route();
      GenNode &copy = b.Add("LogicNativeSetGameProperty");
      b.String(copy, "Property", "label_copy");
      b.Link(get("label"), "Value", copy, "Value");
      chain(copy);
      break;
    }
    case Memory::AimRotation: {
      route();
      GenNode &direction = b.Add("LogicNativeCombineXYZ");
      b.Link(live("pf0"), "Value", direction, "X");
      b.Link(live("pf1"), "Value", direction, "Y");
      b.Float(direction, "Z", 1.0f);
      GenNode &turn = b.Add("LogicNativeVectorToRotation");
      b.Link(direction, "Vector", turn, "Direction");
      set("aim", turn, "Rotation");
      route();
      GenNode &angles = b.Add("LogicNativeSeparateXYZ");
      b.Link(get("aim"), "Value", angles, "Vector");
      keep_float("ax", angles, "X");
      keep_float("az", angles, "Z");
      break;
    }
  }
  return b.Compile(r_report);
}

}  // namespace

/* Plan C1-11: tree properties of every type in columns. Numbers and vectors are written natively
 * (vectors read where they are read: the value as it is at that place of the pass), constants of
 * any type but an object natively, anything else (text, an object, a colour) by the VM's handler
 * through the bridge, the property's number fields read again after it. A target object kept in a
 * tree property is the object of later writes, motions and visibility (found per NPC where each
 * op runs). With the bridge on and off (off: the bridged writes keep the program on the VM), with
 * helpers. */
TEST(LN_Columns, TreePropertiesOfEveryTypeEndAsOnTheLeanVM)
{
  constexpr uint32_t kOwners = 40;
  for (const bool bridged : {true, false}) {
    BridgeOn bridge(bridged);
    for (const Memory memory : {Memory::HomeVector,
                                Memory::TargetObject,
                                Memory::MoodText,
                                Memory::TintColor,
                                Memory::ChangesType,
                                Memory::ConstantText,
                                Memory::AimRotation})
    {
      std::string report;
      const std::shared_ptr<LN_Program> program = MemoryTree(memory, &report);
      ASSERT_NE(program, nullptr) << report;
      std::string refusal;
      const std::shared_ptr<const LN_ColumnProgram> columns = LN_CompileColumnProgram(
          *program, LN_Event::OnFixedUpdate, &refusal);
      const bool lowers = bridged || memory == Memory::HomeVector;
      EXPECT_EQ(columns != nullptr, lowers) << int(memory) << " " << refusal << "\n"
                                            << LN_DumpProgramText(*program);
      const std::string what = std::string(bridged ? "bridge on, " : "bridge off, ") +
                               "memory " + std::to_string(int(memory));
      const auto world = [&](SemRig &rig) {
        AddVictims(rig, kOwners, Victim::ByProperty);
        for (uint32_t index = 0; index < kOwners; index++) {
          rig.SetFloat(rig.Object(index), "pf2", 0.5f + float(index % 4));
        }
        rig.BindIds();
      };
      for (const uint32_t helpers : {0u, 3u}) {
        const uint64_t lanes = ExpectColumnsMatch(program,
                                                  LN_Event::OnFixedUpdate,
                                                  kOwners,
                                                  10,
                                                  what + (helpers ? " (3 helpers)" : ""),
                                                  helpers,
                                                  nullptr,
                                                  world,
                                                  2);
        if (lowers) {
          EXPECT_GT(lanes, 0u) << what;
        }
      }
    }
  }
}

namespace {

/** Switches the flush's ahead-of-time apply (plan C1-11 C-4) for a test. */
struct PreApplyScope {
  explicit PreApplyScope(const int mode)
  {
    LN_SetColumnPreApplyModeForTests(mode);
  }
  ~PreApplyScope()
  {
    LN_SetColumnPreApplyModeForTests(-1);
  }
};

/** The crowd of the pre-apply test: on every step its own "pf3" (twice: the second write
 * supersedes the first once the flush merges), "score" changed (clamped or not), a move between,
 * "pi1" and "pb1". `constant` (a second tree's): "pf3" only, to a constant. `bridged`: first a
 * generic Set Game Property of its "pf3" (a bridged op: its command is applied at the NPC's key,
 * before the native writes). */
std::shared_ptr<LN_Program> OwnWriterTree(const bool clamped,
                                          const bool constant,
                                          const bool bridged = false)
{
  TreeBuilder b;
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  const auto live = [&](const char *property) -> GenNode & {
    GenNode &read = b.Add("LogicNativeGetGamePropertyFloat");
    b.String(read, "Property", property);
    return read;
  };
  GenNode &first = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(first, "Property", "pf3");
  if (constant) {
    b.Float(first, "Value", 5.0f);
    b.Link(step, "Out", first, "Flow");
    return b.Compile(nullptr);
  }
  b.Link(live("pf0"), "Value", first, "Value");
  if (bridged) {
    GenNode &generic = b.Add("LogicNativeSetGameProperty");
    b.String(generic, "Property", "pf3");
    b.Link(live("pf1"), "Value", generic, "Value");
    b.Link(step, "Out", generic, "Flow");
    b.Link(generic, "Done", first, "Flow");
  }
  else {
    b.Link(step, "Out", first, "Flow");
  }
  GenNode &score = b.Add(
      "LogicNativeModifyProperty", int16_t(blender::NODE_MATH_ADD), clamped ? 1 : 0);
  b.String(score, "Property", "score");
  b.Link(live("pf1"), "Value", score, "Value");
  if (clamped) {
    b.Float(score, "Min", -3.0f);
    b.Float(score, "Max", 40.0f);
  }
  b.Link(first, "Done", score, "Flow");
  GenNode &push = b.Add("LogicNativeApplyMovement");
  b.Vector(push, "Vector", 0.02f, 0.01f, 0.0f);
  b.Bool(push, "Local", false);
  b.Link(score, "Done", push, "Flow");
  GenNode &count = b.Add("LogicNativeSetGamePropertyInt");
  b.String(count, "Property", "pi1");
  b.Int(count, "Value", 7);
  b.Link(push, "Done", count, "Flow");
  GenNode &again = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(again, "Property", "pf3");
  b.Link(live("pf2"), "Value", again, "Value");
  b.Link(count, "Done", again, "Flow");
  GenNode &flag = b.Add("LogicNativeSetGamePropertyBool");
  b.String(flag, "Property", "pb1");
  GenNode &truth = b.Add("LogicNativeGetGamePropertyBool");
  b.String(truth, "Property", "pb0");
  b.Link(truth, "Value", flag, "Value");
  b.Link(again, "Done", flag, "Flow");
  return b.Compile(nullptr);
}

enum class Acting { Poke, Modify, Copy, Remove };

/** Another tree acting on the crowd NPC its object's "target" names, every step: Poke writes its
 * "pf3", Modify adds to its "score", Copy copies its "score" into its own object, Remove removes
 * it. */
std::shared_ptr<LN_Program> ActingTree(const Acting acting)
{
  TreeBuilder b;
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode &name = b.Add("LogicNativeGetGamePropertyString");
  b.String(name, "Property", "target");
  GenNode &found = b.Add("LogicNativeObjectByName");
  b.Link(name, "Value", found, "Name");
  GenNode *node = nullptr;
  switch (acting) {
    case Acting::Poke:
      node = &b.Add("LogicNativeSetGamePropertyFloat");
      b.String(*node, "Property", "pf3");
      b.Float(*node, "Value", 99.0f);
      b.Link(found, "Object", *node, "Object");
      break;
    case Acting::Modify:
      node = &b.Add("LogicNativeModifyProperty", int16_t(blender::NODE_MATH_ADD), 0);
      b.String(*node, "Property", "score");
      b.Float(*node, "Value", 1000.0f);
      b.Link(found, "Object", *node, "Object");
      break;
    case Acting::Copy:
      node = &b.Add("LogicNativeCopyProperty");
      b.String(*node, "Property", "score");
      b.Link(found, "Object", *node, "Source");
      break;
    case Acting::Remove:
      node = &b.Add("LogicNativeRemoveObject");
      b.Link(found, "Object", *node, "Object");
      break;
  }
  b.Link(step, "Out", *node, "Flow");
  return b.Compile(nullptr);
}

}  // namespace

/* Plan C1-11 C-4: the lanes' own number writes and Modify Property changes of existing number
 * properties are applied ahead of the flush, spread over the helpers, wherever nothing else in the
 * step touches those properties: every world ends exactly as on the lean VM, tick by tick. Around
 * them: missing and text properties (the flush makes or replaces them: never ahead), trees that
 * write, change, copy or remove a crowd NPC's properties before and after its own writes (that NPC
 * never ahead), objects with two trees writing one property (groups: never ahead), a bridged op of
 * the NPC writing its own "pf3" before its native writes (never ahead), the merge leaving the
 * first "pf3" write out (it stays out). */
TEST(LN_Columns, OwnPropertyWritesAppliedAheadEndAsOnTheLeanVM)
{
  PreApplyScope force(2);
  BridgeOn bridge;
  const std::shared_ptr<LN_Program> second = OwnWriterTree(false, true);
  ASSERT_NE(second, nullptr);
  for (const bool clamped : {false, true}) {
    const std::shared_ptr<LN_Program> own_crowd = OwnWriterTree(clamped, false);
    const std::shared_ptr<LN_Program> bridged_crowd = OwnWriterTree(clamped, false, true);
    ASSERT_NE(own_crowd, nullptr);
    ASSERT_NE(bridged_crowd, nullptr);
    for (const std::shared_ptr<LN_Program> &program : {own_crowd, bridged_crowd}) {
      std::string refusal;
      ASSERT_NE(LN_CompileColumnProgram(*program, LN_Event::OnFixedUpdate, &refusal), nullptr)
          << refusal;
    }
    for (const uint32_t owners : {40u, 600u}) {
      /* World 7: the crowd's own bridged write of "pf3" (its command names its NPC). */
      for (int world = 0; world < 8; world++) {
        const std::shared_ptr<LN_Program> &crowd = world == 7 ? bridged_crowd : own_crowd;
        const auto extra = [&](SemRig &rig) {
          for (uint32_t index = 0; index < owners; index++) {
            KX_GameObject &object = rig.Object(index);
            rig.SetFloat(object, "pf2", float(index % 7) - 2.0f);
            /* World 1: missing and text properties on some NPCs. */
            if (world == 1 && index % 5 == 0) {
              rig.SetString(object, "pf3", "text");
            }
            else {
              rig.SetFloat(object, "pf3", 1.0f);
            }
            if (!(world == 1 && index % 3 == 0)) {
              rig.SetFloat(object, "score", float(index % 4));
            }
            if (world == 1 && index % 4 == 1) {
              rig.SetFloat(object, "pi1", 0.5f); /* an Int write converts in place */
            }
            else {
              rig.SetInt(object, "pi1", 0);
            }
            rig.SetBool(object, "pb1", false);
          }
          /* Worlds 2-5: trees acting on NPCs 3 and 8, before and after their own writes. */
          if (world >= 2 && world <= 5) {
            const std::shared_ptr<LN_Program> acting = ActingTree(Acting(world - 2));
            struct Placement {
              const char *target;
              uint32_t scene_index;
              bool first;
            };
            for (const Placement &placement :
                 {Placement{"Obj3", 7, false}, Placement{"Obj8", 15, true}})
            {
              KX_GameObject &object = rig.AddObject("Acting" + std::to_string(rig.ObjectCount()),
                                                    MT_Vector3(1.0f, -2.0f, 0.0f));
              rig.SetString(object, "target", placement.target);
              rig.SetFloat(object, "score", -1.0f);
              rig.AddTree(acting, rig.ObjectCount() - 1, placement.scene_index, placement.first);
            }
          }
          /* World 6: every NPC has a second tree writing "pf3" (groups). */
          if (world == 6) {
            rig.AddSecondTrees(second, owners);
          }
          rig.BindIds();
        };
        for (const uint32_t helpers : {0u, 3u}) {
          const std::string what = std::string(clamped ? "clamped, " : "") + "world " +
                                   std::to_string(world) + ", " + std::to_string(owners) +
                                   " NPCs, " + std::to_string(helpers) + " helpers";
          const uint64_t before = LN_ColumnPreAppliedLaneCount();
          EXPECT_GT(ExpectColumnsMatch(crowd, LN_Event::OnFixedUpdate, owners, 8, what, helpers,
                                       nullptr, extra, 2),
                    0u)
              << what;
          const uint64_t applied = LN_ColumnPreAppliedLaneCount() - before;
          if (world == 6 || world == 7) {
            /* Every NPC's object is in a group, or its own bridged command writes "pf3". */
            EXPECT_EQ(applied, 0u) << what;
          }
          else {
            /* Most NPCs, every tick (some worlds leave a few out). */
            if (!DirectForced()) {
              EXPECT_GT(applied, uint64_t(owners) * 8 / 2) << what;
            }
          }
        }
      }
    }
  }
}

/* Plan C1-11 C-4: what a recorded command may do to game properties (the ahead-of-time apply
 * rules them out by it): property writes, changes and copies name their objects, Remove Object
 * its object, moves and sounds nothing, scene, save and menu commands anything. */
TEST(LN_Columns, CommandsSayWhoseGamePropertiesTheyMayTouch)
{
  using CT = LN_CommandBuffer::CommandType;
  KX_GameObject *const a = reinterpret_cast<KX_GameObject *>(uintptr_t(0x1000));
  KX_GameObject *const c = reinterpret_cast<KX_GameObject *>(uintptr_t(0x3000));
  const auto objects_of = [&](const CT type, std::vector<const void *> &r_objects) {
    LN_CommandBuffer buffer;
    LN_CommandBuffer::Command command;
    command.type = type;
    command.object = a;
    command.runtime_tree = reinterpret_cast<LN_RuntimeTree *>(c);
    std::vector<LN_CommandBuffer::Command> commands;
    commands.push_back(command);
    buffer.AppendLegacyCommandsForTests(std::move(commands));
    r_objects.clear();
    return buffer.AllCommandsPropertyObjects(r_objects);
  };
  std::vector<const void *> objects;
  for (const CT type : {CT::SetGameProperty, CT::ModifyGameProperty, CT::RemoveObject}) {
    EXPECT_TRUE(objects_of(type, objects)) << int(type);
    EXPECT_EQ(objects, std::vector<const void *>{a}) << int(type);
  }
  EXPECT_TRUE(objects_of(CT::CopyProperty, objects));
  EXPECT_EQ(objects, (std::vector<const void *>{a, c}));
  for (const CT type : {CT::ApplyMovement,
                        CT::SetWorldPosition,
                        CT::PlaySound,
                        CT::SendEvent,
                        CT::Print,
                        CT::SetTreeProperty,
                        CT::AddObject,
                        CT::SetVisibility})
  {
    EXPECT_TRUE(objects_of(type, objects)) << int(type);
    EXPECT_TRUE(objects.empty()) << int(type);
  }
  for (const CT type : {CT::SaveGame,
                        CT::LoadGame,
                        CT::LoadScene,
                        CT::SetScene,
                        CT::RestartGame,
                        CT::OpenObjectMenu,
                        CT::SetOverlayCollection,
                        CT::SubsystemCommand,
                        CT::InstallLogicTree})
  {
    EXPECT_FALSE(objects_of(type, objects)) << int(type);
  }
}

namespace {

/** Plan C2 P1: trees of `programs` in `order` (one an object, "Obj0".., scene index = place), on
 * the lean VM and in columns with the shortest run 1, `ticks` Logic + Physics steps: the worlds
 * and the first 300 trees' states must be the same after every tick. Returns the lanes run. */
uint64_t ExpectMixedColumnsMatch(const std::vector<std::shared_ptr<LN_Program>> &programs,
                                 const std::vector<size_t> &order,
                                 const int ticks,
                                 const std::string &what,
                                 const uint32_t helpers,
                                 const size_t wave_min = 64,
                                 const std::shared_ptr<LN_Program> &target_tree = nullptr,
                                 const size_t min_run = 1,
                                 size_t *r_last_lanes = nullptr)
{
  ColumnScope scope(1);
  LN_SetColumnCountVerifyForTests(1);
  const uint64_t mismatches = LN_ColumnCountMismatchCount();
  struct VerifyOff {
    ~VerifyOff()
    {
      LN_SetColumnCountVerifyForTests(-1);
    }
  } verify_off;
  SemRig lean(programs[0], kMemoOn, 0);
  SemRig cols(programs[0], kMemoOn, 0);
  for (SemRig *rig : {&lean, &cols}) {
    for (size_t index = 0; index < order.size(); index++) {
      rig->AddObject("Obj" + std::to_string(index), MT_Vector3(0.0f, 0.0f, 0.0f));
      /* (Scene index from 1: the Target's tree has 0, before them.) */
      rig->AddTree(programs[order[index]], index, uint32_t(index + 1), false);
    }
    rig->AddObject("Target", MT_Vector3(3.0f, 1.0f, 0.5f));
    if (target_tree != nullptr) {
      /* The Target's own tree, recorded before the others (a pending move of a named object). */
      rig->AddTree(target_tree, rig->ObjectCount() - 1, 0, true);
    }
    Spread(*rig, uint32_t(order.size()));
    rig->BindIds();
    rig->PublishEachTick(true);
    rig->SetDump(false);
  }
  cols.SetColumns(true, helpers, min_run);
  cols.SetMinWaveLanes(wave_min);
  uint64_t lanes = 0;
  for (int tick = 0; tick < ticks; tick++) {
    lean.Step();
    cols.Step();
    /* (Plan C2 P6: forced direct, the column passes are the trees the column system ran.) */
    lanes += cols.LastColumnLanes() + (DirectForced() ? cols.LastColumnPasses() : 0);
    if (r_last_lanes != nullptr) {
      *r_last_lanes = cols.LastColumnLanes();
    }
    std::string a = WorldOf(lean);
    std::string b = WorldOf(cols);
    for (size_t index = 0; index < lean.TreeCount() && index < 300; index++) {
      a += lean.Tree(index).DumpStateForTests(uint64_t(tick)).meaning;
      b += cols.Tree(index).DumpStateForTests(uint64_t(tick)).meaning;
    }
    if (a != b) {
      ADD_FAILURE() << what << ", tick " << tick << ": " << diff::FirstDifference(a, b);
      return lanes;
    }
    if (LN_ColumnCountMismatchCount() != mismatches) {
      ADD_FAILURE() << what << ", tick " << tick << ": the flush's write counts differ";
      return lanes;
    }
  }
  return lanes;
}

/** On Logic + Physics Step: pf1 = sine of pf0, then Set Cursor Visibility (no column kernel, no
 * bridge: it stays on the old system). */
std::shared_ptr<LN_Program> CursorTree()
{
  TreeBuilder b;
  GenNode &event = b.Add("LogicNativeOnFixedUpdate");
  GenNode &sine = b.Add("LogicNativeMath", int16_t(blender::NODE_MATH_SINE));
  GenNode &read = b.Add("LogicNativeGetGamePropertyFloat");
  b.String(read, "Property", "pf0");
  b.Link(read, "Value", sine, "A");
  GenNode &set = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(set, "Property", "pf1");
  b.Link(sine, "Result", set, "Value");
  b.Link(event, "Out", set, "Flow");
  GenNode &cursor = b.Add("LogicNativeSetCursorVisibility");
  b.Link(set, "Done", cursor, "Flow");
  std::string report;
  return b.Compile(&report);
}

}  // namespace

/* Plan C2 P1: runs of 1 (and of 2 to 4) among trees of other programs end as the old system: the
 * brain on every step (4 and 15 chains), on every second step, a bridged writer of an own
 * property, a tree acting on the Target by name (moves, motions, colour: bridged too), and a tree
 * that stays on the old system, in turn (A B C ...), in blocks of 1 to 4, and shuffled; with no
 * helper and with three. */
TEST(LN_Columns, ShortRunsAmongOtherProgramsEndAsTheOldSystem)
{
  BridgeOn bridge;
  std::vector<std::shared_ptr<LN_Program>> programs;
  for (const int chains : {4, 15}) {
    BrainOfOptions options;
    options.step_every = 1;
    options.chains = chains;
    options.measure_distance = chains == 4;
    programs.push_back(BrainOf(options));
  }
  {
    BrainOfOptions options;
    options.step_every = 2;
    options.chains = 2;
    programs.push_back(BrainOf(options));
  }
  std::string report;
  programs.push_back(MadeWritesTree("nbn", false, &report));
  programs.push_back(ActsTree(Acted::Named, true, &report));
  programs.push_back(CursorTree());
  for (size_t index = 0; index < programs.size(); index++) {
    ASSERT_NE(programs[index], nullptr) << "program " << index << ": " << report;
  }
  std::string refusal;
  ASSERT_EQ(LN_CompileColumnProgram(*programs.back(), LN_Event::OnFixedUpdate, &refusal), nullptr);
  std::vector<std::pair<std::string, std::vector<size_t>>> orders;
  {
    std::vector<size_t> order;
    for (size_t index = 0; index < 60; index++) {
      order.push_back(index % programs.size());
    }
    orders.emplace_back("in turn", order);
  }
  {
    std::mt19937 random(7);
    std::vector<size_t> order;
    while (order.size() < 60) {
      const size_t program = random() % programs.size();
      for (size_t k = 0, n = 1 + random() % 4; k < n; k++) {
        order.push_back(program);
      }
    }
    orders.emplace_back("blocks", order);
    std::shuffle(order.begin(), order.end(), random);
    orders.emplace_back("shuffled", order);
  }
  for (const auto &[name, order] : orders) {
    for (const uint32_t helpers : {0u, 3u}) {
      const std::string what = name + ", helpers " + std::to_string(helpers);
      EXPECT_GT(ExpectMixedColumnsMatch(programs, order, 8, what, helpers), 0u) << what;
    }
  }
}

/* Plan C2 P1: a program lowered when its tree registers (LN_ColumnRunner::PrepareProgram) is not
 * lowered again in the tick it first runs; without it the first tick lowers it. */
TEST(LN_Columns, ProgramsPreparedAtRegistrationAreNotLoweredInTheTick)
{
  if (DirectForced()) {
    GTEST_SKIP() << "every program a direct program: no run to plan";
  }
  ColumnScope scope(1);
  BrainOfOptions options;
  options.step_every = 1;
  for (const bool prepare : {true, false}) {
    const std::shared_ptr<LN_Program> program = BrainOf(options);
    ASSERT_NE(program, nullptr);
    SemRig rig(program, kMemoOn, 20);
    Spread(rig, 20);
    rig.SetDump(false);
    rig.SetColumns(true, 0, 1);
    const uint64_t before = LN_ColumnRunner::CompiledCountForTests();
    if (prepare) {
      rig.PrepareColumns();
      EXPECT_EQ(LN_ColumnRunner::CompiledCountForTests(), before + 1);
    }
    const uint64_t ready = LN_ColumnRunner::CompiledCountForTests();
    rig.Step();
    rig.Step();
    EXPECT_GT(rig.LastColumnLanes(), 0u);
    EXPECT_EQ(LN_ColumnRunner::CompiledCountForTests(), ready + (prepare ? 0 : 1))
        << (prepare ? "prepared" : "not prepared");
    /* Preparing again lowers nothing. */
    rig.PrepareColumns();
    EXPECT_EQ(LN_ColumnRunner::CompiledCountForTests(), ready + (prepare ? 0 : 1));
  }
}

/* An object a column program names (the Target every NPC measures) runs no lane, even when it has
 * a tree of the same program among the crowd's: in a run, its move would be a column write the
 * later NPCs' reads of it could not see, while on the old system their reads apply it. With the
 * shortest run 1 and 16, the Target first, in the middle and last. (Plan C2 P1: no test held a
 * named object with a tree inside the crowd's run; a planted "no barriers" was missed.) */
TEST(LN_Columns, AnObjectAColumnProgramNamesRunsOnTheOldSystemInItsPlace)
{
  TreeBuilder b;
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode &distance = b.Add("LogicNativeGetDistance");
  b.ObjectNamed(distance, "Object B", &diff::TargetBlenderObject());
  GenNode &store = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(store, "Property", "dist");
  b.Link(distance, "Distance", store, "Value");
  b.Link(step, "Out", store, "Flow");
  GenNode &move = b.Add("LogicNativeApplyMovement");
  b.Vector(move, "Vector", 0.3f, 0.1f, 0.0f);
  b.Bool(move, "Local", false);
  b.Link(store, "Done", move, "Flow");
  std::string report;
  const std::shared_ptr<LN_Program> program = b.Compile(&report);
  ASSERT_NE(program, nullptr) << report;
  std::string refusal;
  ASSERT_NE(LN_CompileColumnProgram(*program, LN_Event::OnFixedUpdate, &refusal), nullptr)
      << refusal;
  constexpr uint32_t kObjects = 41;
  for (const size_t min_run : {size_t(1), size_t(16)}) {
    for (const uint32_t target_at : {0u, 20u, 40u}) {
      ColumnScope scope(1);
      SemRig lean(program, kMemoOn, 0);
      SemRig cols(program, kMemoOn, 0);
      for (SemRig *rig : {&lean, &cols}) {
        for (uint32_t index = 0; index < kObjects; index++) {
          const bool target = index == target_at;
          rig->AddObject(target ? std::string("Target") : "Obj" + std::to_string(index),
                         MT_Vector3(float(index % 7) - 3.0f, float(index % 5), 0.0f));
          rig->AddTree(program, index, index, false);
        }
        rig->BindIds();
        rig->PublishEachTick(true);
        rig->SetDump(false);
      }
      cols.SetColumns(true, 0, min_run);
      const std::string what = "min " + std::to_string(min_run) + ", Target at " +
                               std::to_string(target_at);
      uint64_t lanes = 0;
      for (int tick = 0; tick < 5; tick++) {
        lean.Step();
        cols.Step();
        /* (Plan C2 P6: forced direct, the column passes are the trees the column system ran.) */
    lanes += cols.LastColumnLanes() + (DirectForced() ? cols.LastColumnPasses() : 0);
        const std::string a = WorldOf(lean), c = WorldOf(cols);
        ASSERT_EQ(a, c) << what << ", tick " << tick << ": " << diff::FirstDifference(a, c);
      }
      if (!DirectForced()) {
        EXPECT_GT(lanes, 0u) << what;
      }
    }
  }
}

/* Plan C2 P1 (found by the checkpoint: research.py mixed1k ran at 11.6 fps instead of 655): many
 * runs of 1 measuring a Target whose own tree moved it first. Each run's read of the Target asks
 * whether a lane has a move of it pending, and the lane map was made again from every lane after
 * each run (O(runs x lanes)). Each lane is looked at once a tick now; the worlds end as the old
 * system's. */
TEST(LN_Columns, ManyRunsOfOneLookAtTheLaneMapOnceATick)
{
  if (DirectForced()) {
    GTEST_SKIP() << "every program a direct program: no run to plan";
  }
  const auto measurer = [](const float speed) {
    TreeBuilder b;
    GenNode &step = b.Add("LogicNativeOnFixedUpdate");
    GenNode &distance = b.Add("LogicNativeGetDistance");
    b.ObjectNamed(distance, "Object B", &diff::TargetBlenderObject());
    GenNode &store = b.Add("LogicNativeSetGamePropertyFloat");
    b.String(store, "Property", "dist");
    b.Link(distance, "Distance", store, "Value");
    b.Link(step, "Out", store, "Flow");
    GenNode &move = b.Add("LogicNativeApplyMovement");
    b.Vector(move, "Vector", speed, 0.1f, 0.0f);
    b.Bool(move, "Local", false);
    b.Link(store, "Done", move, "Flow");
    std::string report;
    return b.Compile(&report);
  };
  std::vector<std::shared_ptr<LN_Program>> crowd;
  for (int k = 0; k < 10; k++) {
    crowd.push_back(measurer(0.01f * float(k + 1)));
    ASSERT_NE(crowd.back(), nullptr);
  }
  /* The Target's own tree, recorded first: it moves the Target (a pending move every step). */
  TreeBuilder mb;
  GenNode &step = mb.Add("LogicNativeOnFixedUpdate");
  GenNode &walk = mb.Add("LogicNativeApplyMovement");
  mb.Vector(walk, "Vector", 0.0f, 0.05f, 0.0f);
  mb.Bool(walk, "Local", false);
  mb.Link(step, "Out", walk, "Flow");
  std::string report;
  const std::shared_ptr<LN_Program> mover = mb.Compile(&report);
  ASSERT_NE(mover, nullptr) << report;
  constexpr uint32_t kNpcs = 300;
  ColumnScope scope(1);
  SemRig lean(mover, kMemoOn, 0);
  SemRig cols(mover, kMemoOn, 0);
  for (SemRig *rig : {&lean, &cols}) {
    rig->AddObject("Target", MT_Vector3(1.0f, 2.0f, 0.0f));
    rig->AddTree(mover, 0, 0, false);
    for (uint32_t index = 1; index <= kNpcs; index++) {
      rig->AddObject("Obj" + std::to_string(index),
                     MT_Vector3(float(index % 13) - 6.0f, float(index % 7), 0.0f));
      rig->AddTree(crowd[index % 10], index, index, false);
    }
    rig->BindIds();
    rig->PublishEachTick(true);
    rig->SetDump(false);
  }
  cols.SetColumns(true, 0, 1);
  for (int tick = 0; tick < 4; tick++) {
    lean.Step();
    const uint64_t work_before = LN_ColumnRunner::LaneMapWorkForTests();
    cols.Step();
    const uint64_t work = LN_ColumnRunner::LaneMapWorkForTests() - work_before;
    const std::string a = WorldOf(lean), c = WorldOf(cols);
    ASSERT_EQ(a, c) << "tick " << tick << ": " << diff::FirstDifference(a, c);
    if (tick > 0) {
      EXPECT_GT(cols.LastColumnLanes(), kNpcs / 2) << "tick " << tick;
      EXPECT_LE(work, uint64_t(cols.LastColumnLanes())) << "tick " << tick;
    }
  }
}

/* Plan C2 P1: waves. Consecutive short runs of native programs start in order here, run their
 * chunks together on the pool and end in order: the brains (4 and 15 chains, on every and on
 * every second step), measurers of the Target (named, moved by its own tree first) moving
 * themselves, in turn, in blocks of 1 to 4 and shuffled, with a tree of the old system every few
 * trees (a wave stops at it); 1 and 3 helpers, the shortest wave 2 lanes. The worlds and states
 * end as the old system's after every tick, and waves ran. */
TEST(LN_Columns, WavesOfShortRunsEndAsTheOldSystem)
{
  std::vector<std::shared_ptr<LN_Program>> programs;
  for (const int every : {1, 2}) {
    for (const int chains : {4, 15}) {
      BrainOfOptions options;
      options.step_every = every;
      options.chains = chains;
      programs.push_back(BrainOf(options));
    }
  }
  for (int k = 0; k < 4; k++) {
    TreeBuilder b;
    GenNode &step = b.Add("LogicNativeOnFixedUpdate");
    GenNode &distance = b.Add("LogicNativeGetDistance");
    b.ObjectNamed(distance, "Object B", &diff::TargetBlenderObject());
    GenNode &store = b.Add("LogicNativeSetGamePropertyFloat");
    b.String(store, "Property", "dist");
    b.Link(distance, "Distance", store, "Value");
    b.Link(step, "Out", store, "Flow");
    GenNode &move = b.Add("LogicNativeApplyMovement");
    b.Vector(move, "Vector", 0.02f * float(k + 1), -0.01f, 0.0f);
    b.Bool(move, "Local", k % 2 == 0);
    b.Link(store, "Done", move, "Flow");
    std::string report;
    programs.push_back(b.Compile(&report));
  }
  {
    /* A finder: Get Object By Name "Obj3" (a lane's object), measured: its lanes go back to the
     * old system when its run starts (which must come after Obj3's run, wave or not). */
    TreeBuilder b;
    GenNode &event = b.Add("LogicNativeOnFixedUpdate");
    GenNode &find = b.Add("LogicNativeObjectByName");
    b.String(find, "Name", "Obj3");
    GenNode &distance = b.Add("LogicNativeGetDistance");
    b.Link(find, "Object", distance, "Object B");
    GenNode &store = b.Add("LogicNativeSetGamePropertyFloat");
    b.String(store, "Property", "found");
    b.Link(distance, "Distance", store, "Value");
    b.Link(event, "Out", store, "Flow");
    GenNode &move = b.Add("LogicNativeApplyMovement");
    b.Vector(move, "Vector", 0.0f, 0.03f, 0.0f);
    b.Bool(move, "Local", false);
    b.Link(store, "Done", move, "Flow");
    std::string report;
    programs.push_back(b.Compile(&report));
  }
  const size_t cursor = programs.size();
  programs.push_back(CursorTree());
  {
    /* A watcher on the old system (Set Cursor Visibility), measuring "Obj7" (a lane of a wave):
     * its read applies Obj7's pending move, found through the lane map. */
    TreeBuilder b;
    GenNode &event = b.Add("LogicNativeOnFixedUpdate");
    GenNode &target = b.Add("LogicNativeObjectByName");
    b.String(target, "Name", "Obj7");
    GenNode &distance = b.Add("LogicNativeGetDistance");
    b.Link(target, "Object", distance, "Object B");
    GenNode &store = b.Add("LogicNativeSetGamePropertyFloat");
    b.String(store, "Property", "seen");
    b.Link(distance, "Distance", store, "Value");
    b.Link(event, "Out", store, "Flow");
    GenNode &hide = b.Add("LogicNativeSetCursorVisibility");
    b.Link(store, "Done", hide, "Flow");
    std::string report;
    programs.push_back(b.Compile(&report));
  }
  for (size_t index = 0; index < programs.size(); index++) {
    ASSERT_NE(programs[index], nullptr) << "program " << index;
  }
  TreeBuilder mb;
  GenNode &step = mb.Add("LogicNativeOnFixedUpdate");
  GenNode &walk = mb.Add("LogicNativeApplyMovement");
  mb.Vector(walk, "Vector", 0.05f, 0.03f, 0.0f);
  mb.Bool(walk, "Local", false);
  mb.Link(step, "Out", walk, "Flow");
  std::string report;
  const std::shared_ptr<LN_Program> mover = mb.Compile(&report);
  ASSERT_NE(mover, nullptr) << report;
  std::vector<std::pair<std::string, std::vector<size_t>>> orders;
  std::mt19937 random(11);
  {
    /* The native programs in turn (Obj3 and Obj7 in the first wave), the finder at 12, the
     * watcher right after the first wave, the cursor tree every 23 trees. */
    const size_t finder = cursor - 1, watcher = cursor + 1;
    std::vector<size_t> order;
    for (size_t index = 0; index < 160; index++) {
      order.push_back(index == 12 ? finder :
                      index == 20 ? watcher :
                      index % 23 == 22 ? cursor :
                                         index % finder);
    }
    orders.emplace_back("in turn", order);
  }
  {
    std::vector<size_t> order;
    while (order.size() < 160) {
      const size_t program = random() % programs.size();
      for (size_t k = 0, n = 1 + random() % 4; k < n; k++) {
        order.push_back(program);
      }
    }
    orders.emplace_back("blocks", order);
    std::shuffle(order.begin(), order.end(), random);
    orders.emplace_back("shuffled", order);
  }
  const uint64_t waves_before = LN_ColumnRunner::WaveCountForTests();
  for (const auto &[name, order] : orders) {
    for (const uint32_t helpers : {1u, 3u}) {
      const std::string what = name + ", helpers " + std::to_string(helpers);
      EXPECT_GT(ExpectMixedColumnsMatch(programs, order, 8, what, helpers, 2, mover), 0u) << what;
    }
  }
  if (!DirectForced()) {
    EXPECT_GT(LN_ColumnRunner::WaveCountForTests(), waves_before) << "no wave ran";
  }
}

/* Plan C2 P1's size floors (the D3 gate): with helpers a small program runs in columns only from a
 * few trees of it in a row (LN_ColumnSizeFloor: the brain without chains, 15 column nodes, from 4;
 * the big brain from 1), fewer stay on the old system; without helpers, with a shortest run asked
 * for (SetMinRunLanes) and in the All mode there is no floor. The worlds are the old system's in
 * every case. */
TEST(LN_Columns, SmallProgramsNeedAFewTreesInARow)
{
  if (std::getenv("UPBGE_LN_COLUMN_MIN_RUN") != nullptr) {
    GTEST_SKIP() << "UPBGE_LN_COLUMN_MIN_RUN asks for a shortest run: no size floor";
  }
  struct Case {
    int chains;
    size_t floor;
  };
  for (const Case &c : {Case{0, 4}, Case{15, 1}}) {
    BrainOfOptions options;
    options.chains = c.chains;
    options.step_every = 1;
    const std::shared_ptr<LN_Program> program = BrainOf(options);
    ASSERT_NE(program, nullptr);
    std::string refusal;
    const std::shared_ptr<const LN_ColumnProgram> columns = LN_CompileColumnProgram(
        *program, LN_Event::OnFixedUpdate, &refusal);
    ASSERT_NE(columns, nullptr) << refusal;
    EXPECT_EQ(LN_ColumnSizeFloor(*columns), c.floor) << LN_DescribeColumnProgram(*columns);
    struct Setup {
      int mode;           /* ColumnScope: 1 on, 2 All */
      size_t min_run;     /* SemRig::SetColumns: 0 = the runner's own */
      uint32_t helpers;
      uint32_t owners;
      size_t expect_lanes; /* per tick */
    };
    std::vector<Setup> setups = {
        {1, 0, 3, uint32_t(c.floor), c.floor},
        {1, 1, 3, 1, 1},
        {2, 0, 3, 1, 1},
        {1, 0, 0, 1, 1},
    };
    if (c.floor > 1) {
      setups.push_back({1, 0, 3, uint32_t(c.floor - 1), 0});
    }
    for (const Setup &setup : setups) {
      const std::string what = "chains " + std::to_string(c.chains) + ", mode " +
                               std::to_string(setup.mode) + ", shortest run " +
                               std::to_string(setup.min_run) + ", " +
                               std::to_string(setup.helpers) + " helpers, " +
                               std::to_string(setup.owners) + " trees";
      ColumnScope scope(setup.mode);
      SemRig lean(program, kMemoOn, setup.owners);
      SemRig cols(program, kMemoOn, setup.owners);
      Spread(lean, setup.owners);
      Spread(cols, setup.owners);
      lean.SetDump(false);
      cols.SetDump(false);
      cols.SetColumns(true, setup.helpers, setup.min_run);
      for (int tick = 0; tick < 12; tick++) {
        lean.Step();
        cols.Step();
        if (tick > 0) {
          if (!DirectForced()) {
            EXPECT_EQ(cols.LastColumnLanes(), setup.expect_lanes) << what << ", tick " << tick;
          }
        }
        const std::string a = WorldOf(lean);
        const std::string b = WorldOf(cols);
        if (a != b) {
          ADD_FAILURE() << what << ", tick " << tick << ": " << diff::FirstDifference(a, b);
          break;
        }
      }
    }
  }
}

/* Plan C2 P1: runs in a row whose trees average LN_COLUMN_TAKE_ALL_AVERAGE_NODES column nodes are
 * all taken, a small program's runs of 1 included (left out, they cut the wave apart and make the
 * flush list the lanes' writes); smaller trees take the size floors; without helpers there is no
 * floor. Every world as the old system's. */
TEST(LN_Columns, LargerTreesInARowTakeEveryRunAndSmallerOnesTakeTheFloors)
{
  if (std::getenv("UPBGE_LN_COLUMN_MIN_RUN") != nullptr) {
    GTEST_SKIP() << "UPBGE_LN_COLUMN_MIN_RUN asks for a shortest run: no size floor";
  }
  std::vector<std::shared_ptr<LN_Program>> programs;
  std::vector<size_t> nodes;
  for (const int chains : {0, 1, 15}) {
    BrainOfOptions options;
    options.step_every = 1;
    options.chains = chains;
    programs.push_back(BrainOf(options));
    ASSERT_NE(programs.back(), nullptr);
    const std::shared_ptr<const LN_ColumnProgram> columns = LN_CompileColumnProgram(
        *programs.back(), LN_Event::OnFixedUpdate);
    ASSERT_NE(columns, nullptr);
    nodes.push_back(LN_ColumnNodeCount(*columns));
  }
  /* 15, 20 and 90 column nodes: floors 4, 2 and 1. */
  ASSERT_LT(nodes[0], 16u);
  ASSERT_LT(nodes[1], LN_COLUMN_TAKE_ALL_AVERAGE_NODES);
  ASSERT_GE(nodes[1], 16u);
  ASSERT_GE(nodes[2], 2 * LN_COLUMN_TAKE_ALL_AVERAGE_NODES);
  struct Case {
    std::string name;
    std::vector<size_t> order;
    uint32_t helpers;
    size_t expect_lanes;
  };
  std::vector<Case> cases;
  {
    /* Large, tiny, large, small...: the average is well over the line. */
    std::vector<size_t> order;
    for (size_t index = 0; index < 96; index++) {
      order.push_back(index % 2 == 0 ? 2 : index % 4 == 1 ? 0 : 1);
    }
    cases.push_back({"larger trees in a row", order, 3, order.size()});
    cases.push_back({"larger trees in a row, no helpers", order, 0, order.size()});
  }
  {
    /* Tiny and small in turn: under the line, runs of 1 below their floors. */
    std::vector<size_t> order;
    for (size_t index = 0; index < 96; index++) {
      order.push_back(index % 2);
    }
    cases.push_back({"smaller trees in a row", order, 3, 0});
    /* Without helpers no floor: every run of 1. */
    cases.push_back({"smaller trees in a row, no helpers", order, 0, order.size()});
  }
  {
    /* Tiny in blocks of 4 and small in blocks of 2: every run meets its floor. */
    std::vector<size_t> order;
    while (order.size() < 96) {
      for (int k = 0; k < 4; k++) {
        order.push_back(0);
      }
      for (int k = 0; k < 2; k++) {
        order.push_back(1);
      }
    }
    cases.push_back({"blocks at the floors", order, 3, order.size()});
    cases.push_back({"blocks at the floors, no helpers", order, 0, order.size()});
  }
  {
    /* Tiny in blocks of 3: one short of its floor. */
    std::vector<size_t> order;
    while (order.size() < 96) {
      for (int k = 0; k < 3; k++) {
        order.push_back(0);
      }
      order.push_back(1);
      order.push_back(1);
    }
    size_t small = 0;
    for (const size_t program : order) {
      small += program == 1 ? 1 : 0;
    }
    cases.push_back({"tiny blocks under the floor", order, 3, small});
  }
  for (const Case &c : cases) {
    size_t last = SIZE_MAX;
    ExpectMixedColumnsMatch(programs, c.order, 6, c.name, c.helpers, 64, nullptr, 0, &last);
    if (!DirectForced()) {
      EXPECT_EQ(last, c.expect_lanes) << c.name;
    }
  }
}

/* Plan C2 P1: Plan keeps its "no run is possible" answer while the tree list and the programs are
 * the same (it touched every tree to find it again); a tree given another program makes it plan
 * again: the middle tree's big brain makes the three trees in a row average more than
 * LN_COLUMN_TAKE_ALL_AVERAGE_NODES, so all three are lanes from then on. Worlds as the old
 * system's. */
TEST(LN_Columns, AKeptNoRunAnswerEndsWhenATreeGetsAnotherProgram)
{
  if (DirectForced()) {
    GTEST_SKIP() << "every program a direct program: no run to plan";
  }
  if (std::getenv("UPBGE_LN_COLUMN_MIN_RUN") != nullptr) {
    GTEST_SKIP() << "UPBGE_LN_COLUMN_MIN_RUN asks for a shortest run: no size floor";
  }
  BrainOfOptions options;
  options.step_every = 1;
  options.chains = 0;
  const std::shared_ptr<LN_Program> tiny = BrainOf(options);
  options.chains = 15;
  const std::shared_ptr<LN_Program> big = BrainOf(options);
  ASSERT_NE(tiny, nullptr);
  ASSERT_NE(big, nullptr);
  ColumnScope scope(1);
  SemRig lean(tiny, kMemoOn, 3);
  SemRig cols(tiny, kMemoOn, 3);
  for (SemRig *rig : {&lean, &cols}) {
    Spread(*rig, 3);
    rig->SetDump(false);
  }
  /* Helpers: the size floors apply (3 tiny trees, floor 4: no run). */
  cols.SetColumns(true, 3, 0);
  for (int tick = 0; tick < 16; tick++) {
    if (tick == 6) {
      lean.Tree(1).SetProgram(big);
      cols.Tree(1).SetProgram(big);
    }
    lean.Step();
    cols.Step();
    if (tick > 0 && tick < 6) {
      if (!DirectForced()) {
        EXPECT_EQ(cols.LastColumnLanes(), 0u) << "tick " << tick;
      }
      if (!DirectForced()) {
        EXPECT_NE(cols.LastColumnRefusal().find("more for small programs"), std::string::npos)
            << cols.LastColumnRefusal();
      }
    }
    if (tick >= 8) {
      EXPECT_EQ(cols.LastColumnLanes(), 3u) << "tick " << tick;
    }
    const std::string a = WorldOf(lean);
    const std::string b = WorldOf(cols);
    if (a != b) {
      ADD_FAILURE() << "tick " << tick << ": " << diff::FirstDifference(a, b);
      break;
    }
  }
}

/* ---- Plan C2 P2: every tree a lane ---- */

namespace {

/** A named "Ship" (a constant object for Get Distance's Object B). */
blender::Object &ShipBlenderObject()
{
  static blender::Object object;
  static const bool named = (SetCString(object.id.name, "OBShip"), true);
  (void)named;
  return object;
}

std::shared_ptr<LN_Program> P2Mover(const float x, const float y, const bool local)
{
  TreeBuilder b;
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode &walk = b.Add("LogicNativeApplyMovement");
  b.Vector(walk, "Vector", x, y, 0.0f);
  b.Bool(walk, "Local", local);
  b.Link(step, "Out", walk, "Flow");
  std::string report;
  return b.Compile(&report);
}

/** A child's tree (a turret on a cart): what it reads before its own local move. */
enum class P2Child {
  /** research.py's brain (the owner's distance to the origin, a branch, local moves). */
  Brain,
  /** The distance from the owner to the named Target, then the owner's pose. */
  MeasureTarget,
  /** The distance from the owner to the named Ship (its parent in some scenes). */
  MeasureShip,
  /** Get Parent, the parent's pose. */
  ReadParent,
  /** The owner's pose only (nothing its run's start applies). */
  PoseOnly,
  /** The owner's pose, then the distance from the named Ship to the Target (not from the owner:
   * the start applies the Ship's chain, which under the Ship holds the turret's parent). */
  PoseThenShipToTarget,
  /** Get Parent, an Apply Movement of the parent. */
  PushParent,
};

std::shared_ptr<LN_Program> P2ChildTree(const P2Child kind)
{
  if (kind == P2Child::Brain) {
    BrainOfOptions options;
    options.step_every = 1;
    options.chains = 1;
    return BrainOf(options);
  }
  TreeBuilder b;
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode *last = nullptr;
  const auto then = [&](GenNode &node) {
    if (last == nullptr) {
      b.Link(step, "Out", node, "Flow");
    }
    else {
      b.Link(*last, "Done", node, "Flow");
    }
    last = &node;
  };
  if (kind == P2Child::MeasureTarget || kind == P2Child::MeasureShip) {
    GenNode &distance = b.Add("LogicNativeGetDistance");
    b.ObjectNamed(distance,
                  "Object B",
                  kind == P2Child::MeasureShip ? &ShipBlenderObject() : &diff::TargetBlenderObject());
    GenNode &seen = b.Add("LogicNativeSetGamePropertyFloat");
    b.String(seen, "Property", "seen");
    b.Link(distance, "Distance", seen, "Value");
    then(seen);
  }
  if (kind == P2Child::PushParent) {
    GenNode &parent = b.Add("LogicNativeGetParent");
    GenNode &push = b.Add("LogicNativeApplyMovement");
    b.Link(parent, "Parent Object", push, "Object");
    b.Vector(push, "Vector", 0.03f, -0.01f, 0.0f);
    b.Bool(push, "Local", false);
    then(push);
    std::string report;
    return b.Compile(&report);
  }
  if (kind == P2Child::ReadParent) {
    GenNode &parent = b.Add("LogicNativeGetParent");
    GenNode &pose = b.Add("LogicNativeGetObjectAttribute", 0);
    b.Link(parent, "Parent Object", pose, "Object");
    GenNode &split = b.Add("LogicNativeSeparateXYZ");
    b.Link(pose, "Vector", split, "Vector");
    GenNode &keep = b.Add("LogicNativeSetGamePropertyFloat");
    b.String(keep, "Property", "ppx");
    b.Link(split, "X", keep, "Value");
    then(keep);
  }
  GenNode &own = b.Add("LogicNativeGetObjectAttribute", 0);
  GenNode &split = b.Add("LogicNativeSeparateXYZ");
  b.Link(own, "Vector", split, "Vector");
  GenNode &px = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(px, "Property", "px");
  b.Link(split, "X", px, "Value");
  then(px);
  if (kind == P2Child::PoseThenShipToTarget) {
    GenNode &distance = b.Add("LogicNativeGetDistance");
    b.ObjectNamed(distance, "Object A", &ShipBlenderObject());
    b.ObjectNamed(distance, "Object B", &diff::TargetBlenderObject());
    GenNode &seen = b.Add("LogicNativeSetGamePropertyFloat");
    b.String(seen, "Property", "seen");
    b.Link(distance, "Distance", seen, "Value");
    then(seen);
  }
  GenNode &move = b.Add("LogicNativeApplyMovement");
  b.Vector(move, "Vector", 0.05f, 0.02f, 0.0f);
  b.Bool(move, "Local", true);
  then(move);
  std::string report;
  return b.Compile(&report);
}

/** One object of a P2 scene: its parent (an index, -1 none) and its trees in their order. */
struct P2Object {
  std::string name;
  int parent = -1;
  std::vector<std::shared_ptr<LN_Program>> trees;
};

/** The scene on the lean VM and in columns (runs of 1 allowed, waves of 2 lanes), every tick:
 * the worlds and the first 300 trees' states the same. Returns the lanes run. */
uint64_t ExpectP2Matches(const std::vector<P2Object> &objects,
                         const std::string &what,
                         const uint32_t helpers,
                         const std::function<void(SemRig &)> &extra = {},
                         const int ticks = 8)
{
  ColumnScope scope(1);
  LN_SetColumnCountVerifyForTests(1);
  const uint64_t mismatches = LN_ColumnCountMismatchCount();
  struct VerifyOff {
    ~VerifyOff()
    {
      LN_SetColumnCountVerifyForTests(-1);
    }
  } verify_off;
  std::shared_ptr<LN_Program> any;
  for (const P2Object &object : objects) {
    if (!object.trees.empty()) {
      any = object.trees[0];
      break;
    }
  }
  SemRig lean(any, kMemoOn, 0);
  SemRig cols(any, kMemoOn, 0);
  for (SemRig *rig : {&lean, &cols}) {
    for (const P2Object &object : objects) {
      rig->AddObject(object.name, MT_Vector3(0.0f, 0.0f, 0.0f));
    }
    Spread(*rig, uint32_t(objects.size()));
    for (size_t index = 0; index < objects.size(); index++) {
      if (objects[index].parent >= 0) {
        rig->Parent(index, size_t(objects[index].parent));
      }
    }
    for (size_t index = 0; index < objects.size(); index++) {
      for (size_t k = 0; k < objects[index].trees.size(); k++) {
        rig->AddTreeOf(objects[index].trees[k], index, uint32_t(index + 1), uint32_t(k));
      }
    }
    rig->MarkSharedObjects();
    rig->BindIds();
    rig->PublishEachTick(true);
    rig->SetDump(false);
    if (extra) {
      extra(*rig);
    }
  }
  cols.SetColumns(true, helpers, 1);
  cols.SetMinWaveLanes(2);
  uint64_t lanes = 0;
  for (int tick = 0; tick < ticks; tick++) {
    lean.Step();
    cols.Step();
    /* (Plan C2 P6: forced direct, the column passes are the trees the column system ran.) */
    lanes += cols.LastColumnLanes() + (DirectForced() ? cols.LastColumnPasses() : 0);
    std::string a = WorldOf(lean);
    std::string b = WorldOf(cols);
    for (size_t index = 0; index < lean.TreeCount() && index < 300; index++) {
      a += lean.Tree(index).DumpStateForTests(uint64_t(tick)).meaning;
      b += cols.Tree(index).DumpStateForTests(uint64_t(tick)).meaning;
    }
    if (a != b) {
      ADD_FAILURE() << what << ", tick " << tick << ": " << diff::FirstDifference(a, b);
      return lanes;
    }
    if (LN_ColumnCountMismatchCount() != mismatches) {
      ADD_FAILURE() << what << ", tick " << tick << ": the flush's write counts differ";
      return lanes;
    }
  }
  return lanes;
}

}  // namespace

/* Plan C2 P2: objects with a parent are lanes. Carts that move (a tree of their own, before or
 * after their turrets', or none), turrets on them whose trees read their own distance, a named
 * object, their parent or their pose only, all carts first or each cart with its turret, the
 * same program on carts and turrets (a run must end before a turret of an earlier cart), deep
 * chains, a ship the turrets measure (named: its pending moves under them give the turrets back
 * to the VM) and the old system's result every tick. */
TEST(LN_Columns, ParentedLanesEndAsTheOldSystem)
{
  const std::shared_ptr<LN_Program> mover = P2Mover(0.05f, 0.03f, false);
  ASSERT_NE(mover, nullptr);
  std::map<P2Child, std::shared_ptr<LN_Program>> child;
  for (const P2Child kind : {P2Child::Brain,
                             P2Child::MeasureTarget,
                             P2Child::MeasureShip,
                             P2Child::ReadParent,
                             P2Child::PoseOnly,
                             P2Child::PoseThenShipToTarget})
  {
    child[kind] = P2ChildTree(kind);
    ASSERT_NE(child[kind], nullptr) << int(kind);
    std::string refusal;
    ASSERT_NE(LN_CompileColumnProgram(*child[kind], LN_Event::OnFixedUpdate, &refusal), nullptr)
        << int(kind) << ": " << refusal;
  }
  constexpr int kCarts = 12;
  uint64_t splits_before = 0, fallbacks_before = 0;
  LN_ColumnDependCounts(splits_before, fallbacks_before);
  for (const uint32_t helpers : {0u, 3u}) {
    for (const P2Child kind :
         {P2Child::Brain, P2Child::MeasureTarget, P2Child::ReadParent, P2Child::PoseOnly})
    {
      const std::string name = "child " + std::to_string(int(kind)) + ", helpers " +
                               std::to_string(helpers);
      /* Each cart, then its turret. */
      std::vector<P2Object> in_turn;
      for (int k = 0; k < kCarts; k++) {
        in_turn.push_back({"Cart" + std::to_string(k), -1, {mover}});
        in_turn.push_back({"Turret" + std::to_string(k), int(in_turn.size()) - 1, {child[kind]}});
      }
      in_turn.push_back({"Target", -1, {}});
      EXPECT_GT(ExpectP2Matches(in_turn, name + ", in turn", helpers), 0u) << name;
      /* All carts, then all turrets (one run of turrets whose carts moved before it). */
      std::vector<P2Object> sorted;
      for (int k = 0; k < kCarts; k++) {
        sorted.push_back({"Cart" + std::to_string(k), -1, {mover}});
      }
      for (int k = 0; k < kCarts; k++) {
        sorted.push_back({"Turret" + std::to_string(k), k, {child[kind]}});
      }
      sorted.push_back({"Target", -1, {}});
      EXPECT_GT(ExpectP2Matches(sorted, name + ", sorted", helpers), 0u) << name;
      /* Each turret before its cart. */
      std::vector<P2Object> child_first;
      for (int k = 0; k < kCarts; k++) {
        child_first.push_back({"Turret" + std::to_string(k), int(child_first.size()) + 1,
                               {child[kind]}});
        child_first.push_back({"Cart" + std::to_string(k), -1, {mover}});
      }
      child_first.push_back({"Target", -1, {}});
      EXPECT_GT(ExpectP2Matches(child_first, name + ", turret first", helpers), 0u) << name;
      /* Carts without trees (nothing pending under the turrets). */
      std::vector<P2Object> still;
      for (int k = 0; k < kCarts; k++) {
        still.push_back({"Cart" + std::to_string(k), -1, {}});
        still.push_back({"Turret" + std::to_string(k), int(still.size()) - 1, {child[kind]}});
      }
      still.push_back({"Target", -1, {}});
      EXPECT_GT(ExpectP2Matches(still, name + ", still carts", helpers), 0u) << name;
      /* Cart, turret, barrel: three levels, each with the same tree (a run of one program over
       * the chain must end before each child of an earlier lane). */
      std::vector<P2Object> chains;
      for (int k = 0; k < 6; k++) {
        const int cart = int(chains.size());
        chains.push_back({"Cart" + std::to_string(k), -1, {child[kind]}});
        chains.push_back({"Turret" + std::to_string(k), cart, {child[kind]}});
        chains.push_back({"Barrel" + std::to_string(k), cart + 1, {child[kind]}});
      }
      chains.push_back({"Target", -1, {mover}});
      EXPECT_GT(ExpectP2Matches(chains, name + ", chains", helpers), 0u) << name;
    }
    /* A ship the turrets measure: named, so its own tree runs alone; its moves pending under the
     * turrets after it give them to the VM where the run's start would apply them before a read
     * the VM makes with them pending (the turret's pose before a distance from the Ship), before
     * it they run in columns. */
    for (const P2Child kind : {P2Child::MeasureShip, P2Child::PoseThenShipToTarget}) {
      for (const bool ship_first : {true, false}) {
        std::vector<P2Object> ship;
        if (ship_first) {
          ship.push_back({"Ship", -1, {mover}});
        }
        const int ship_index = ship_first ? 0 : 2 * kCarts;
        for (int k = 0; k < 2 * kCarts; k++) {
          ship.push_back({"Turret" + std::to_string(k), ship_index, {child[kind]}});
        }
        if (!ship_first) {
          ship.push_back({"Ship", -1, {mover}});
        }
        ship.push_back({"Target", -1, {}});
        const std::string name = "ship " + std::to_string(int(kind)) +
                                 (ship_first ? " first" : " last") + ", helpers " +
                                 std::to_string(helpers);
        EXPECT_GT(ExpectP2Matches(ship, name, helpers), 0u) << name;
      }
    }
  }
  uint64_t splits = 0, fallbacks = 0;
  LN_ColumnDependCounts(splits, fallbacks);
  if (!DirectForced()) {
    EXPECT_GT(splits, splits_before) << "no run was split lane by lane";
  }
  if (!DirectForced()) {
    EXPECT_GT(fallbacks, fallbacks_before) << "no lane was given back for its ship's moves";
  }
}

/* Plan C2 P2: menu copies are lanes, each alone in its place: a copy's names find its copy's
 * objects (the counterpart rule), so a run of copies and others of one program would measure the
 * first tree's Target for all. Copies and others of one program alternate here. */
TEST(LN_Columns, MenuCopiesRunAloneAndMeasureTheirCopysObjects)
{
  const std::shared_ptr<LN_Program> measure = P2ChildTree(P2Child::MeasureTarget);
  ASSERT_NE(measure, nullptr);
  for (const uint32_t helpers : {0u, 3u}) {
    std::vector<P2Object> objects;
    for (int k = 0; k < 16; k++) {
      objects.push_back({(k % 3 == 1 ? "HudCopy" : "Hud") + std::to_string(k), -1, {measure}});
    }
    objects.push_back({"Target", -1, {}});
    objects.push_back({"Target.001", -1, {}});
    const auto copies = [](SemRig &rig) {
      KX_GameObject *target = &rig.Object(rig.ObjectCount() - 2);
      KX_GameObject *copy_target = &rig.Object(rig.ObjectCount() - 1);
      const auto is_copy = [](KX_GameObject *object) {
        return object != nullptr && object->GetName().find("HudCopy") == 0;
      };
      rig.SetOverlayCounterpart([=](KX_GameObject *object, KX_GameObject *viewer) {
        return object == target && is_copy(viewer) ? copy_target : object;
      });
      rig.SetOverlayCopies(is_copy);
    };
    const std::string name = "copies, helpers " + std::to_string(helpers);
    EXPECT_GT(ExpectP2Matches(objects, name, helpers, copies), 0u) << name;
  }
}

/* Plan C2 P2: an object's trees that no group takes run each alone in its place: an object with a
 * tree on the VM beside one that lowers, parented objects with two trees, and objects whose two
 * trees write one property no object has yet (an Int and a Float: the flush's merge keeps only
 * the later one, so the property is made a Float, as the VM's two recorded commands; listed for
 * the merge since P2, in groups too). */
TEST(LN_Columns, AnObjectsTreesRunEachInItsPlace)
{
  const auto writer = [](const bool as_int) {
    TreeBuilder b;
    GenNode &step = b.Add("LogicNativeOnFixedUpdate");
    GenNode &set = b.Add(as_int ? "LogicNativeSetGamePropertyInt" :
                                  "LogicNativeSetGamePropertyFloat");
    b.String(set, "Property", "fresh");
    if (as_int) {
      b.Int(set, "Value", 3);
    }
    else {
      b.Float(set, "Value", 2.5f);
    }
    b.Link(step, "Out", set, "Flow");
    /* (Nothing after it: a move would end the merge's run, and the VM would merge nothing.) */
    std::string report;
    return b.Compile(&report);
  };
  const std::shared_ptr<LN_Program> int_writer = writer(true);
  const std::shared_ptr<LN_Program> float_writer = writer(false);
  const std::shared_ptr<LN_Program> refused = CursorTree();
  const std::shared_ptr<LN_Program> brain = P2ChildTree(P2Child::Brain);
  ASSERT_NE(int_writer, nullptr);
  ASSERT_NE(float_writer, nullptr);
  ASSERT_NE(refused, nullptr);
  ASSERT_NE(brain, nullptr);
  for (const uint32_t helpers : {0u, 3u}) {
    const std::string h = ", helpers " + std::to_string(helpers);
    /* A tree on the VM beside one that lowers (no group: not all its trees are lanes). */
    std::vector<P2Object> beside;
    for (int k = 0; k < 40; k++) {
      beside.push_back(
          {"Obj" + std::to_string(k), -1, k % 2 == 0 ? std::vector{refused, brain} :
                                                       std::vector{brain, refused}});
    }
    EXPECT_GT(ExpectP2Matches(beside, "beside a VM tree" + h, helpers), 0u);
    /* A mover and a brain reading its own distance beside a VM tree: the brain's run starts after
     * the mover's moves are known (no wave holds both: a wave's later run starts first). */
    const std::shared_ptr<LN_Program> mover = P2Mover(0.04f, -0.03f, false);
    std::vector<P2Object> three;
    for (int k = 0; k < 40; k++) {
      three.push_back({"Obj" + std::to_string(k), -1, {refused, mover, brain}});
    }
    EXPECT_GT(ExpectP2Matches(three, "mover, brain and a VM tree" + h, helpers), 0u);
    /* Two trees of one object both setting its world position (a last-write-wins transform write
     * the merge keeps once: the pose is set once, its world stamp moves once), as a group of roots
     * and parented (no group). */
    TreeBuilder placer_builder;
    {
      GenNode &step = placer_builder.Add("LogicNativeOnFixedUpdate");
      GenNode &place = placer_builder.Add("LogicNativeSetObjectAttribute", 0, 15);
      placer_builder.Vector(place, "Value", 1.0f, 2.0f, 0.5f);
      placer_builder.Link(step, "Out", place, "Flow");
    }
    std::string placer_report;
    const std::shared_ptr<LN_Program> placer = placer_builder.Compile(&placer_report);
    ASSERT_NE(placer, nullptr) << placer_report;
    for (const bool parented : {false, true}) {
      std::vector<P2Object> placed;
      if (parented) {
        placed.push_back({"Base", -1, {}});
      }
      for (int k = 0; k < 80; k++) {
        placed.push_back({"Obj" + std::to_string(k), parented ? 0 : -1, {placer, placer}});
      }
      EXPECT_GT(ExpectP2Matches(placed, std::string("two placers") + (parented ? ", parented" : "") + h,
                                helpers),
                0u);
    }
    /* Objects under a base, each pushing the base, then measuring itself: no group (a group would
     * run every object's push before the first object's measure, which the VM makes with only
     * its own push pending). */
    const std::shared_ptr<LN_Program> pusher = P2ChildTree(P2Child::PushParent);
    ASSERT_NE(pusher, nullptr);
    std::string refusal;
    ASSERT_NE(LN_CompileColumnProgram(*pusher, LN_Event::OnFixedUpdate, &refusal), nullptr)
        << refusal;
    std::vector<P2Object> under;
    under.push_back({"Base", -1, {}});
    for (int k = 0; k < 24; k++) {
      under.push_back({"Obj" + std::to_string(k), 0, {pusher, brain}});
    }
    EXPECT_GT(ExpectP2Matches(under, "push the base, then measure" + h, helpers), 0u);
    /* Two trees writing one new property, 80 objects (the flush merges 64 and more), as groups
     * of roots and as parented objects (no group). */
    for (const bool parented : {false, true}) {
      std::vector<P2Object> two;
      if (parented) {
        two.push_back({"Base", -1, {}});
      }
      for (int k = 0; k < 80; k++) {
        two.push_back({"Obj" + std::to_string(k), parented ? 0 : -1, {int_writer, float_writer}});
      }
      EXPECT_GT(ExpectP2Matches(two, std::string("two writers") + (parented ? ", parented" : "") + h,
                                helpers),
                0u);
      /* The own number writes applied ahead (forced): never for an object with two lanes. */
      LN_SetColumnPreApplyModeForTests(2);
      EXPECT_GT(ExpectP2Matches(two,
                                std::string("two writers, pre-apply") +
                                    (parented ? ", parented" : "") + h,
                                helpers),
                0u);
      LN_SetColumnPreApplyModeForTests(-1);
    }
  }
}

/* Plan C2 P3: a crowd's sensing ray (crowd2's): a Raycast on the step's flow, Local, ahead, its
 * Has Result, Distance, Hit Point and Hit Normal written. `with_move`: the NPC moves before. */
static std::shared_ptr<LN_Program> RayTree(const bool with_move, std::string *r_report = nullptr)
{
  TreeBuilder b;
  b.SetRealSockets(true);
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode *flow = &step;
  const char *flow_out = "Out";
  if (with_move) {
    GenNode &move = b.Add("LogicNativeApplyMovement");
    b.Vector(move, "Vector", 0.1f, 0.0f, 0.0f);
    b.Bool(move, "Local", true);
    b.Link(step, "Out", move, "Flow");
    flow = &move;
    flow_out = "Done";
  }
  GenNode &ray = b.Add("LogicNativeRaycast");
  b.Vector(ray, "Direction", 1.0f, 0.0f, 0.0f);
  b.Float(ray, "Max Distance", 4.0f);
  b.Bool(ray, "Local", true);
  b.Link(*flow, flow_out, ray, "Flow");
  GenNode &seen = b.Add("LogicNativeSetGamePropertyBool");
  b.String(seen, "Property", "seen");
  b.Link(ray, "Has Result", seen, "Value");
  b.Link(ray, "Done", seen, "Flow");
  GenNode &rd = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(rd, "Property", "rd");
  b.Link(ray, "Distance", rd, "Value");
  b.Link(seen, "Done", rd, "Flow");
  GenNode &split = b.Add("LogicNativeSeparateXYZ");
  b.Link(ray, "Point", split, "Vector");
  GenNode &pz = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(pz, "Property", "pz");
  b.Link(split, "Z", pz, "Value");
  b.Link(rd, "Done", pz, "Flow");
  GenNode &nsplit = b.Add("LogicNativeSeparateXYZ");
  b.Link(ray, "Normal", nsplit, "Vector");
  GenNode &nz = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(nz, "Property", "nz");
  b.Link(nsplit, "Z", nz, "Value");
  b.Link(pz, "Done", nz, "Flow");
  return b.Compile(r_report);
}

TEST(LN_Columns, DISABLED_RayTreeListing)
{
  std::string report;
  const std::shared_ptr<LN_Program> program = RayTree(false, &report);
  ASSERT_NE(program, nullptr) << report;
  std::printf("%s\n", LN_DumpProgramText(*program).c_str());
  std::string refusal;
  const auto columns = LN_CompileColumnProgram(*program, LN_Event::OnFixedUpdate, &refusal);
  std::printf("%s\n", columns ? LN_DescribeColumnProgram(*columns).c_str() : refusal.c_str());
}

/* Plan C2 P3: a Raycast with constant settings (an event's flow, empty ignore inputs) runs as a
 * column node, its Has Result, Done, Distance, Hit Point and Hit Normal read from it; a ray from
 * the owner read after a move of it stays bridged (the VM casts from the moved owner); with the
 * switch off it is bridged as before. The rig has no physics world (every cast reports it, as
 * the VM's does): the worlds are the VM's either way. */
TEST(LN_Columns, NativeRaysLowerWhereTheVMsCastIsKept)
{
  struct NativeRaysScope {
    explicit NativeRaysScope(const int mode)
    {
      LN_SetColumnNativeRaysForTests(mode);
    }
    ~NativeRaysScope()
    {
      LN_SetColumnNativeRaysForTests(-1);
    }
  };
  std::string report;
  const std::shared_ptr<LN_Program> ray = RayTree(false, &report);
  ASSERT_NE(ray, nullptr) << report;
  const std::shared_ptr<LN_Program> moved = RayTree(true, &report);
  ASSERT_NE(moved, nullptr) << report;
  const auto describe = [](const std::shared_ptr<LN_Program> &program) {
    std::string refusal;
    const auto columns = LN_CompileColumnProgram(*program, LN_Event::OnFixedUpdate, &refusal);
    return columns != nullptr ? LN_DescribeColumnProgram(*columns) : "refused: " + refusal;
  };
  {
    NativeRaysScope scope(1);
    const std::string text = describe(ray);
    EXPECT_NE(text.find("native_rays=1"), std::string::npos) << text;
    EXPECT_NE(text.find("bridged=0"), std::string::npos) << text;
    /* After a move of the owner: not native; the bridge refuses it too (its own rule), so the
     * trees stay on the VM, as before P3 (UPBGE_LN_COLUMN_BRIDGE=0: refused without a bridge). */
    const char *bridge = std::getenv("UPBGE_LN_COLUMN_BRIDGE");
    const bool bridge_on = bridge == nullptr || std::strcmp(bridge, "0") != 0;
    const std::string after_move = describe(moved);
    EXPECT_EQ(after_move.find("native_rays=1"), std::string::npos) << after_move;
    if (bridge_on) {
      EXPECT_NE(after_move.find("after a move"), std::string::npos) << after_move;
    }
    for (const uint32_t helpers : {0u, 3u}) {
      EXPECT_GT(ExpectColumnsMatch(ray, LN_Event::OnFixedUpdate, 40, 6, "native ray", helpers), 0u);
      ExpectColumnsMatch(moved, LN_Event::OnFixedUpdate, 40, 6, "ray after a move", helpers);
    }
  }
  {
    NativeRaysScope scope(0);
    const std::string text = describe(ray);
    EXPECT_EQ(text.find("native_rays=1"), std::string::npos) << text;
    const char *bridge = std::getenv("UPBGE_LN_COLUMN_BRIDGE");
    if (bridge == nullptr || std::strcmp(bridge, "0") != 0) {
      EXPECT_NE(text.find("bridged=1"), std::string::npos) << text;
      EXPECT_GT(ExpectColumnsMatch(ray, LN_Event::OnFixedUpdate, 40, 6, "bridged ray"), 0u);
    }
  }
}

/* Plan C2 P4: the flush writes a Set Tree Property's value again only where another op of the
 * tree's program writes that property too (an Add Object result, Inventory, Assign Geometry Nodes
 * Modifier): elsewhere the op wrote the value already and nothing reads tree properties between
 * the flush's writes. The program's answer per property, and worlds and tree states as the VM's
 * (two writes of one property in one step, the last one kept). */
TEST(LN_Columns, ATreePropertysFlushRewriteIsSkippedWhereOnlySetTreePropertyWritesIt)
{
  TreeBuilder b;
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode &a = b.Add("LogicNativeGetGamePropertyFloat");
  b.String(a, "Property", "pf0");
  GenNode &c = b.Add("LogicNativeGetGamePropertyFloat");
  b.String(c, "Property", "pf1");
  /* (Set Tree Property has no outputs: a Sequence orders the three ops.) */
  GenNode &sequence = b.Add("LogicNativeSequence", 0, 0, 4);
  b.Link(step, "Out", sequence, "Flow");
  GenNode &first = b.Add("LogicNativeSetTreeProperty");
  b.String(first, "Property", "flag");
  b.Link(a, "Value", first, "Value");
  b.Link(sequence, "Then 0", first, "Flow");
  GenNode &second = b.Add("LogicNativeSetTreeProperty");
  b.String(second, "Property", "flag");
  b.Link(c, "Value", second, "Value");
  b.Link(sequence, "Then 1", second, "Flow");
  GenNode &spawn = b.Add("LogicNativeAddObject");
  b.ObjectNamed(spawn, "Object to Add", &SparkBlenderObject());
  b.Link(sequence, "Then 2", spawn, "Flow");
  /* (Its result is kept in a tree property only when its Object output is read.) */
  GenNode &last = b.Add("LogicNativeSetTreeProperty");
  b.String(last, "Property", "last");
  b.Link(spawn, "Object", last, "Value");
  b.Link(sequence, "Then 3", last, "Flow");
  std::string report;
  const std::shared_ptr<LN_Program> program = b.Compile(&report);
  ASSERT_NE(program, nullptr) << report;
  const std::vector<LN_TreePropertyRef> &refs = program->GetTreePropertyRefs();
  bool saw_flag = false, saw_add = false;
  for (uint32_t ref = 0; ref < refs.size(); ref++) {
    if (refs[ref].name == "flag" || refs[ref].name == "last") {
      saw_flag = true;
      EXPECT_TRUE(program->TreePropertyOnlySetBySetTreeProperty(ref));
    }
    else {
      saw_add = true; /* Add Object's result */
      EXPECT_FALSE(program->TreePropertyOnlySetBySetTreeProperty(ref)) << refs[ref].name;
    }
  }
  EXPECT_TRUE(saw_flag);
  EXPECT_TRUE(saw_add);
  ExpectColumnsMatch(program, LN_Event::OnFixedUpdate, 20, 5, "two tree writes and an add");
}

/* Plan C2 P5: whether a program's event may use the bridge is decided by the queries its own ops
 * reach: another event's On UI Event query refused the step event's bridge of the whole tree
 * (GameUIExample's and InventoryTest's Game trees: an Input Action read on steps beside menu
 * routes). The step event lowers (the input read bridged on the main thread), the frame event's
 * UI route stays refused (P7), and the worlds are the VM's. */
TEST(LN_Columns, AnEventsBridgeIsDecidedByTheQueriesItsOwnOpsReach)
{
  TreeBuilder b;
  b.SetRealSockets(true);
  GenNode &ui = b.Add("LogicNativeOnUIEvent");
  GenNode &clicked = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(clicked, "Property", "ui");
  b.Float(clicked, "Value", 1.0f);
  b.Link(ui, "Out", clicked, "Flow");
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode &action = b.Add("LogicNativeInputAction");
  b.String(action, "Action", "Jump");
  GenNode &branch = b.Add("LogicNativeBranch");
  b.Link(action, "Active", branch, "Condition");
  b.Link(step, "Out", branch, "Flow");
  GenNode &jump = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(jump, "Property", "jump");
  b.Float(jump, "Value", 1.0f);
  b.Link(branch, "True", jump, "Flow");
  GenNode &walk = b.Add("LogicNativeApplyMovement");
  b.Vector(walk, "Vector", 0.1f, 0.0f, 0.0f);
  b.Link(branch, "False", walk, "Flow");
  std::string report;
  const std::shared_ptr<LN_Program> program = b.Compile(&report);
  ASSERT_NE(program, nullptr) << report;
  const char *bridge = std::getenv("UPBGE_LN_COLUMN_BRIDGE");
  if (bridge != nullptr && std::strcmp(bridge, "0") == 0) {
    GTEST_SKIP() << "the bridge is off";
  }
  std::string refusal;
  const auto steps = LN_CompileColumnProgram(*program, LN_Event::OnFixedUpdate, &refusal);
  ASSERT_NE(steps, nullptr) << refusal;
  EXPECT_NE(LN_DescribeColumnProgram(*steps).find("bridged=1"), std::string::npos);
  std::string frame_refusal;
  EXPECT_EQ(LN_CompileColumnProgram(*program, LN_Event::OnUpdate, &frame_refusal), nullptr);
  EXPECT_GT(ExpectColumnsMatch(program, LN_Event::OnFixedUpdate, 30, 6, "input beside a menu"), 0u);
}

/* Plan C2 P5: direct programs (every program forced direct here) end as the old system: the
 * runner's own walk of the blocks (gates with their time and stride, a Loop body and its
 * Completed route, Delays) with the ops through the shared handlers, recording in the trees'
 * places, among trees of other programs, with and without helpers. And the Auto mode's rule: a
 * refused event with a compiled route (the old system runs it faster) stays there. */
TEST(LN_Columns, DirectProgramsEndAsTheOldSystem)
{
  std::string report;
  {
    DirectScope force(2);
    const uint64_t before = LN_ColumnDirectLaneCount();
    for (uint32_t seed = 1; seed <= 120; seed++) {
      const bool on_steps = seed % 2 == 0;
      RandomColumnTree tree(seed * 7919u, on_steps);
      const std::shared_ptr<LN_Program> program = tree.Compile();
      if (program == nullptr) {
        continue;
      }
      const LN_Event event = on_steps ? LN_Event::OnFixedUpdate : LN_Event::OnUpdate;
      ExpectColumnsMatch(program,
                         event,
                         seed % 3 == 0 ? 40 : 12,
                         8,
                         "direct seed " + std::to_string(seed),
                         seed % 3 == 0 ? 3 : 0);
      if (HasFailure()) {
        return;
      }
    }
    BrainOfOptions gated;
    gated.step_every = 2;
    struct Shape {
      std::string what;
      std::shared_ptr<LN_Program> program;
      LN_Event event;
    };
    std::vector<Shape> shapes;
    shapes.push_back({"gated brain", BrainOf(gated), LN_Event::OnFixedUpdate});
    shapes.push_back({"loop", LoopTree(3, &report), LN_Event::OnFixedUpdate});
    shapes.push_back({"time nodes", TimeNodesTree(false, 3, 1, &report), LN_Event::OnFixedUpdate});
    shapes.push_back({"time nodes on frames", TimeNodesTree(true, 1, 0, &report),
                      LN_Event::OnUpdate});
    shapes.push_back({"bridged", BridgedTree(&report), LN_Event::OnUpdate});
    shapes.push_back({"cursor", CursorTree(), LN_Event::OnFixedUpdate});
    for (const Shape &shape : shapes) {
      ASSERT_NE(shape.program, nullptr) << shape.what << ": " << report;
      EXPECT_GT(ExpectColumnsMatch(shape.program, shape.event, 30, 40, shape.what), 0u)
          << shape.what;
      EXPECT_GT(ExpectColumnsMatch(shape.program, shape.event, 300, 12, shape.what + " helpers", 3),
                0u)
          << shape.what;
    }
    /* Objects with two direct trees each (a mover, then a reader of Obj5's distance): no group
     * (a group would run every mover before Obj4's reader, which the old system runs with Obj5
     * not moved yet: its read would apply Obj5's move). */
    TreeBuilder rb;
    GenNode &rstep = rb.Add("LogicNativeOnFixedUpdate");
    GenNode &rdistance = rb.Add("LogicNativeGetDistance", 0, 0);
    rb.ObjectNamed(rdistance, "Object B", &diff::FinderBlenderObject("Obj5"));
    GenNode &rset = rb.Add("LogicNativeSetGamePropertyFloat");
    rb.String(rset, "Property", "pf1");
    rb.Link(rdistance, "Distance", rset, "Value");
    rb.Link(rstep, "Out", rset, "Flow");
    const std::shared_ptr<LN_Program> reader = rb.Compile(&report);
    ASSERT_NE(reader, nullptr) << report;
    const std::shared_ptr<LN_Program> mover = P2Mover(0.05f, 0.03f, false);
    ASSERT_NE(mover, nullptr);
    std::vector<P2Object> pairs;
    for (int k = 0; k < 12; k++) {
      pairs.push_back({"Obj" + std::to_string(k), -1, {mover, reader}});
    }
    EXPECT_GT(ExpectP2Matches(pairs, "direct mover and reader", 0), 0u);
    EXPECT_GT(LN_ColumnDirectLaneCount(), before);
  }
  /* A refused event that has a compiled route: a move, then the distance from the owner kept in
   * pf1 (the compiler refuses a distance first read after a move of the owner; both ops are
   * native, so the event has a route). Since plan C2 P6 a direct pass runs the route as the old
   * system does, so the Auto mode takes it too (before, it stayed on the old system), and it ends
   * the same. */
  TreeBuilder b;
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode &move = b.Add("LogicNativeApplyMovement");
  b.Vector(move, "Vector", 0.25f, -0.5f, 0.0f);
  b.Link(step, "Out", move, "Flow");
  GenNode &distance = b.Add("LogicNativeGetDistance", 0, 0);
  b.ObjectNamed(distance, "Object B", &diff::TargetBlenderObject());
  GenNode &keep = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(keep, "Property", "pf1");
  b.Link(distance, "Distance", keep, "Value");
  b.Link(move, "Done", keep, "Flow");
  const std::shared_ptr<LN_Program> late = b.Compile(&report);
  ASSERT_NE(late, nullptr) << report;
  for (const int mode : {1, 2}) {
    DirectScope as_set(mode);
    std::string why;
    const std::shared_ptr<const LN_ColumnProgram> columns = LN_CompileColumnProgram(
        *late, LN_Event::OnFixedUpdate, &why, true);
    ASSERT_NE(columns, nullptr) << why;
    const std::string text = LN_DescribeColumnProgram(*columns);
    EXPECT_NE(text.find("has_route=1"), std::string::npos) << text;
    /* (Force lowers nothing natively: no compiler answer.) */
    EXPECT_EQ(why.find("after a move") != std::string::npos, mode == 1) << why;
    const uint64_t direct = LN_ColumnDirectLaneCount();
    ExpectColumnsMatch(late, LN_Event::OnFixedUpdate, 30, 10, "late distance (route)");
    EXPECT_GT(LN_ColumnDirectLaneCount(), direct) << "mode " << mode;
  }
}

/* Plan C2 P5's speed check (DISABLED_): a refused program (the brain's bookkeeping chains, then
 * Set Cursor Visibility, which has no column kernel; no compiled route) on 1, 16 and 1,000
 * objects, the old system against direct programs, one thread; and 1,000 objects whose trees are
 * that program and the native brain in turn. ns a tree, the best of UPBGE_LN_BENCH_ROUNDS (400). */
TEST(LN_Columns, DISABLED_DirectBench)
{
  const char *rounds_text = std::getenv("UPBGE_LN_BENCH_ROUNDS");
  const int rounds = rounds_text != nullptr ? std::max(1, std::atoi(rounds_text)) : 400;
  ColumnScope scope(1);
  const auto measure = [&](SemRig &rig, const size_t npcs) {
    rig.SetDump(false);
    rig.SetCapturePrints(false);
    rig.Update();
    rig.Update();
    double best = 1.0e30;
    SemRig::PhaseTimes best_phase = {1.0e30, 1.0e30, 1.0e30, 1.0e30};
    for (int round = 0; round < rounds; round++) {
      rig.ResetTimes();
      rig.Update();
      const SemRig::PhaseTimes &t = rig.Times();
      best = std::min(best, t.capture_ms + t.record_ms + t.flush_ms);
      best_phase.capture_ms = std::min(best_phase.capture_ms, t.capture_ms);
      best_phase.record_ms = std::min(best_phase.record_ms, t.record_ms);
      best_phase.flush_ms = std::min(best_phase.flush_ms, t.flush_ms);
      best_phase.plan_ms = std::min(best_phase.plan_ms, t.plan_ms);
    }
    if (std::getenv("UPBGE_LN_BENCH_PHASES") != nullptr) {
      std::printf("    [plan %.0f, capture %.0f, record %.0f, flush %.0f]\n",
                  best_phase.plan_ms * 1.0e6 / double(npcs),
                  best_phase.capture_ms * 1.0e6 / double(npcs),
                  best_phase.record_ms * 1.0e6 / double(npcs),
                  best_phase.flush_ms * 1.0e6 / double(npcs));
    }
    return best * 1.0e6 / double(npcs);
  };
  const auto refused_program = [](const int chains) {
    TreeBuilder b;
    GenNode &event = b.Add("LogicNativeOnUpdate");
    GenNode *flow = &event;
    const char *out = "Out";
    for (int chain = 0; chain < chains; chain++) {
      GenNode &read = b.Add("LogicNativeGetGamePropertyFloat");
      b.String(read, "Property", ("t" + std::to_string(chain)).c_str());
      GenNode &add = b.Add("LogicNativeMath", int16_t(blender::NODE_MATH_ADD));
      b.Link(read, "Value", add, "A");
      b.Float(add, "B", 0.01f);
      GenNode &write = b.Add("LogicNativeSetGamePropertyFloat");
      b.String(write, "Property", ("t" + std::to_string(chain)).c_str());
      b.Link(add, "Result", write, "Value");
      b.Link(*flow, out, write, "Flow");
      flow = &write;
      out = "Done";
    }
    GenNode &cursor = b.Add("LogicNativeSetCursorVisibility");
    b.Link(*flow, out, cursor, "Flow");
    std::string report;
    return b.Compile(&report);
  };
  for (const int chains : {4, 15}) {
    const std::shared_ptr<LN_Program> program = refused_program(chains);
    ASSERT_NE(program, nullptr);
    for (const uint32_t owners : {1u, 16u, 1000u}) {
      std::string line = "DIRECT refused" + std::to_string(chains) + " x" +
                         std::to_string(owners) + ":";
      for (const int way : {0, 1, 2}) {
        /* 0: columns off; 1: columns on, direct programs off; 2: direct programs. */
        DirectScope mode(way == 2 ? 1 : 0);
        SemRig rig(program, kMemoOn, owners);
        Spread(rig, owners);
        rig.SetColumns(way != 0, 0, 1);
        char text[64];
        std::snprintf(text,
                      sizeof(text),
                      "  %s %5.0f",
                      way == 0 ? "old" : way == 1 ? "columns" : "direct",
                      measure(rig, owners));
        line += text;
      }
      std::printf("%s\n", line.c_str());
    }
  }
  BrainOfOptions options;
  const std::shared_ptr<LN_Program> brain = BrainOf(options);
  const std::shared_ptr<LN_Program> refused = refused_program(4);
  std::string line = "DIRECT 1000 objects, brain and refused in turn:";
  for (const int way : {0, 1, 2}) {
    /* 0: the old system; 1: columns, refused trees on the old system; 2: columns, direct. */
    DirectScope mode(way == 2 ? 1 : 0);
    SemRig rig(brain, kMemoOn, 0);
    for (uint32_t index = 0; index < 1000; index++) {
      rig.AddObject("Obj" + std::to_string(index), MT_Vector3(0.0f, 0.0f, 0.0f));
      rig.AddTree(index % 2 == 0 ? brain : refused, index, index, false);
    }
    Spread(rig, 1000);
    rig.SetColumns(way != 0, 0, 1);
    char text[64];
    std::snprintf(text,
                  sizeof(text),
                  "  %s %5.0f",
                  way == 0 ? "old" : way == 1 ? "columns" : "columns+direct",
                  measure(rig, 1000));
    line += text;
  }
  std::printf("%s\n", line.c_str());
}

/* Plan C2 P5: Apply Force To Target of the owner in columns (its handler's math, one code:
 * ColumnForceToTarget, on the owner's live pose after a move of it): the forces (the rig notes
 * them), Reached, When Reached and Done end as on the old system; a target near enough to count
 * as reached, a stop distance that keeps some forces back; with and without helpers. */
TEST(LN_Columns, ForceToTargetEndsAsOnTheLeanVM)
{
  std::string report;
  const std::shared_ptr<LN_Program> push = OtherTree(Other::Push, true);
  ASSERT_NE(push, nullptr);
  for (const int shape : {0, 1, 2, 3}) {
    const std::string what = shape == 0 ? "force to target" :
                             shape == 1 ? "force to target, wide reach" :
                             shape == 2 ? "force to target, long stop" :
                                          "force to target, distance after";
    TreeBuilder b;
    GenNode &step = b.Add("LogicNativeOnFixedUpdate");
    GenNode &move = b.Add("LogicNativeApplyMovement");
    b.Vector(move, "Vector", 0.05f, 0.02f, 0.0f);
    if (shape != 3) {
      b.Link(step, "Out", move, "Flow");
    }
    GenNode &x = b.Add("LogicNativeGetGamePropertyFloat");
    b.String(x, "Property", "pf0");
    GenNode &y = b.Add("LogicNativeGetGamePropertyFloat");
    b.String(y, "Property", "pf1");
    GenNode &target = b.Add("LogicNativeCombineXYZ");
    b.Link(x, "Value", target, "X");
    b.Link(y, "Value", target, "Y");
    GenNode &force = b.Add("LogicNativeApplyForceToTarget");
    if (shape == 2) {
      /* Only where pb0 (Reached, written below) holds: the op runs for some NPCs only. */
      GenNode &flag = b.Add("LogicNativeGetGamePropertyBool");
      b.String(flag, "Property", "pb0");
      GenNode &branch = b.Add("LogicNativeBranch");
      b.Link(flag, "Value", branch, "Condition");
      b.Link(move, "Done", branch, "Flow");
      b.Link(branch, "False", force, "Flow");
    }
    else if (shape == 3) {
      /* (No move first: only the force's read of the owner's pose comes before the distance,
       * NoteOwnerLiveRead's rule, which makes the run start quietly.) */
      b.Link(step, "Out", force, "Flow");
    }
    else {
      b.Link(move, "Done", force, "Flow");
    }
    b.Link(target, "Vector", force, "Target Position");
    GenNode &strength = b.Add("LogicNativeGetGamePropertyFloat");
    b.String(strength, "Property", "pf2");
    b.Link(strength, "Value", force, "Force");
    b.Float(force, "Reached Threshold", shape == 1 ? 6.0f : 2.5f);
    b.Float(force, "Stop Distance", shape == 2 ? 4.0f : 0.5f);
    GenNode &mark = b.Add("LogicNativeSetGamePropertyInt");
    b.String(mark, "Property", "pi0");
    b.Int(mark, "Value", 7);
    b.Link(force, "When Reached", mark, "Flow");
    GenNode &reached = b.Add("LogicNativeSetGamePropertyBool");
    b.String(reached, "Property", "pb0");
    b.Link(force, "Reached", reached, "Value");
    b.Link(force, "Done", reached, "Flow");
    if (shape == 3) {
      /* A distance from the owner after the force (the VM's read applies a push of the owner
       * there, after the force read the pose; a run would apply it at its start): since plan C2
       * fix 1 the run starts quietly, Obj7 (pushed before the crowd every step) recording on the
       * VM in its place. */
      GenNode &distance = b.Add("LogicNativeGetDistance", 0, 0);
      b.ObjectNamed(distance, "Object B", &diff::TargetBlenderObject());
      GenNode &keep = b.Add("LogicNativeSetGamePropertyFloat");
      b.String(keep, "Property", "pf3");
      b.Link(distance, "Distance", keep, "Value");
      b.Link(reached, "Done", keep, "Flow");
    }
    const std::shared_ptr<LN_Program> program = b.Compile(&report);
    ASSERT_NE(program, nullptr) << report;
    std::string refusal;
    const bool lowers = LN_CompileColumnProgram(*program, LN_Event::OnFixedUpdate, &refusal) !=
                        nullptr;
    EXPECT_TRUE(lowers) << what << ": " << refusal;
    const auto extra = [&](SemRig &rig) {
      if (shape != 3) {
        return;
      }
      KX_GameObject &object = rig.AddObject("Pusher" + std::to_string(rig.ObjectCount()),
                                            MT_Vector3(1.0f, -2.0f, 0.0f));
      rig.SetString(object, "target", "Obj7");
      rig.AddTree(push, rig.ObjectCount() - 1, 1, true);
    };
    EXPECT_GT(ExpectColumnsMatch(program, LN_Event::OnFixedUpdate, 40, 30, what, 0, nullptr, extra),
              0u)
        << what;
    EXPECT_GT(ExpectColumnsMatch(
                  program, LN_Event::OnFixedUpdate, 600, 6, what + " helpers", 3, nullptr, extra),
              0u)
        << what;
    /* The comparison saw forces (the rig notes them; no physics here). */
    SemRig rig(program, kMemoOn, 40);
    Spread(rig, 40);
    rig.SetDump(false);
    rig.SetColumns(true, 0, 1);
    for (int tick = 0; tick < 3; tick++) {
      rig.Tick(LN_Event::OnFixedUpdate);
    }
    EXPECT_NE(rig.Forces().find(" force "), std::string::npos) << what;
  }
}

/* Plan C2 P5: Predict Motion in columns through the bridge (its handler NPC by NPC, its outputs
 * bridged reads of its state): of the owner before a move of it, of the Target (named; a tree
 * pushes it first, so the run's first tree plays on the VM), of a position (from properties); of
 * the owner after a move of it (refused natively: a direct program). Predicted Position, Speed and
 * Valid written; with and without helpers. */
TEST(LN_Columns, PredictMotionEndsAsOnTheLeanVM)
{
  BridgeOn bridge;
  std::string report;
  const std::shared_ptr<LN_Program> push = OtherTree(Other::Push, true);
  ASSERT_NE(push, nullptr);
  for (const int shape : {0, 1, 2, 3, 4}) {
    const std::string what = shape == 0 ? "predict the owner" :
                             shape == 1 ? "predict the Target" :
                             shape == 2 ? "predict a position" :
                             shape == 3 ? "predict the owner after a move" :
                                          "predict the Target found by name";
    TreeBuilder b;
    GenNode &step = b.Add("LogicNativeOnFixedUpdate");
    GenNode &move = b.Add("LogicNativeApplyMovement");
    b.Vector(move, "Vector", 0.05f, 0.02f, 0.0f);
    GenNode &predict = b.Add("LogicNativePredictMotion", int16_t(shape == 2 ? 1 : 0));
    if (shape == 1) {
      b.ObjectNamed(predict, "Object", &diff::TargetBlenderObject());
    }
    if (shape == 4) {
      /* An object found while playing (refused natively: the VM's handler applies its pending
       * moves where the run could not). */
      GenNode &found = b.Add("LogicNativeObjectByName");
      b.String(found, "Name", "Target");
      b.Link(found, "Object", predict, "Object");
    }
    if (shape == 2) {
      GenNode &x = b.Add("LogicNativeGetGamePropertyFloat");
      b.String(x, "Property", "pf0");
      GenNode &y = b.Add("LogicNativeGetGamePropertyFloat");
      b.String(y, "Property", "pf1");
      GenNode &position = b.Add("LogicNativeCombineXYZ");
      b.Link(x, "Value", position, "X");
      b.Link(y, "Value", position, "Y");
      b.Link(position, "Vector", predict, "Position");
    }
    b.Float(predict, "Prediction Time", 0.5f);
    GenNode &split = b.Add("LogicNativeSeparateXYZ");
    b.Link(predict, "Predicted Position", split, "Vector");
    GenNode &w1 = b.Add("LogicNativeSetGamePropertyFloat");
    b.String(w1, "Property", "pf3");
    b.Link(split, "X", w1, "Value");
    GenNode &w2 = b.Add("LogicNativeSetGamePropertyFloat");
    b.String(w2, "Property", "pf4");
    b.Link(predict, "Speed", w2, "Value");
    GenNode &w3 = b.Add("LogicNativeSetGamePropertyBool");
    b.String(w3, "Property", "pb1");
    b.Link(predict, "Valid", w3, "Value");
    if (shape == 3) {
      b.Link(step, "Out", move, "Flow");
      b.Link(move, "Done", predict, "Flow");
    }
    else {
      b.Link(step, "Out", predict, "Flow");
    }
    b.Link(predict, "Updated", w1, "Flow");
    b.Link(w1, "Done", w2, "Flow");
    b.Link(w2, "Done", w3, "Flow");
    if (shape != 3) {
      b.Link(w3, "Done", move, "Flow");
    }
    const std::shared_ptr<LN_Program> program = b.Compile(&report);
    ASSERT_NE(program, nullptr) << report;
    std::string refusal;
    const bool lowers = LN_CompileColumnProgram(*program, LN_Event::OnFixedUpdate, &refusal) !=
                        nullptr;
    EXPECT_EQ(lowers, shape != 3 && shape != 4) << what << ": " << refusal;
    /* The Target (named) or Obj7 (an NPC: its own pending move, which the VM's handler applies
     * at the op) pushed before the crowd every step. */
    const auto extra = [&](SemRig &rig) {
      if (shape == 2) {
        return;
      }
      KX_GameObject &object = rig.AddObject("Pusher" + std::to_string(rig.ObjectCount()),
                                            MT_Vector3(1.0f, -2.0f, 0.0f));
      const bool target = shape == 1 || shape == 4;
      rig.SetString(object, "target", target ? "Target" : "Obj7");
      rig.AddTree(push, rig.ObjectCount() - 1, target ? 0 : 1, true);
    };
    EXPECT_GT(
        ExpectColumnsMatch(program, LN_Event::OnFixedUpdate, 40, 30, what, 0, nullptr, extra), 0u)
        << what;
    EXPECT_GT(ExpectColumnsMatch(
                  program, LN_Event::OnFixedUpdate, 600, 6, what + " helpers", 3, nullptr, extra),
              0u)
        << what;
  }
}

/* Plan C2 P8: Run Logic Tree passes (one forced pass after the step's trees, as the manager runs
 * them) through the column system's pass (ColumnExecuteForcedUpdate): the gated brain and the
 * time nodes on steps, two trees in five forced every step, against the old system; with and
 * without helpers. */
TEST(LN_Columns, RunLogicTreePassesEndAsTheOldSystem)
{
  std::string report;
  BrainOfOptions gated;
  gated.step_every = 3;
  const std::vector<std::pair<std::string, std::shared_ptr<LN_Program>>> programs = {
      {"gated brain", BrainOf(gated)}, {"time nodes", TimeNodesTree(false, 3, 1, &report)}};
  constexpr uint32_t kOwners = 20;
  for (const auto &[what, program] : programs) {
    ASSERT_NE(program, nullptr) << what << ": " << report;
    for (const uint32_t helpers : {0u, 3u}) {
      ColumnScope scope(1);
      SemRig lean(program, kMemoOn, kOwners);
      SemRig cols(program, kMemoOn, kOwners);
      for (SemRig *rig : {&lean, &cols}) {
        rig->AddObject("Target", MT_Vector3(3.0f, 1.0f, 0.5f));
        Spread(*rig, kOwners);
        rig->PublishEachTick(true);
        rig->SetDump(false);
      }
      cols.SetColumns(true, helpers, 1);
      const uint64_t passes = LN_ColumnDirectLaneCount();
      for (int tick = 0; tick < 30; tick++) {
        const auto forced = [&](SemRig &rig) {
          return [&rig, tick](LN_CommandBuffer &buffer, const LN_TickContext &context) {
            rig.Force(size_t(tick % 5), buffer, context);
            rig.Force(size_t((tick * 3 + 2) % kOwners), buffer, context);
          };
        };
        lean.Tick(LN_Event::OnFixedUpdate, forced(lean));
        cols.Tick(LN_Event::OnFixedUpdate, forced(cols));
        std::string a = WorldOf(lean);
        std::string b = WorldOf(cols);
        for (size_t index = 0; index < lean.TreeCount(); index++) {
          a += lean.Tree(index).DumpStateForTests(uint64_t(tick)).meaning;
          b += cols.Tree(index).DumpStateForTests(uint64_t(tick)).meaning;
        }
        ASSERT_EQ(a, b) << what << ", " << helpers << " helpers, tick " << tick << ": "
                        << diff::FirstDifference(a, b);
      }
      /* The forced passes took the column system's pass (the old system's with it off). */
      if (LN_GetColumnDirectMode() != LN_ColumnDirectMode::Off) {
        EXPECT_GE(LN_ColumnDirectLaneCount() - passes, 60u) << what;
      }
    }
  }
}

namespace {

/** The farm game's crop (game_scenes.py farm, 2026-11-06): on Every 30 Steps (spread) its growth
 * "pf0" + 1; at 14 or more growth 0, its scale and colour back and the Target's (the Barn's)
 * "pf2" + 1 (a harvest). When ripe it grows tall and turns gold (World Scale, Object Color):
 * `edge`, on the Rising edge of growth >= 9, a sensor route that runs every step, so the tree never
 * sleeps; else on a Branch of growth == 8 inside the slowed route (the tree sleeps between its
 * steps). */
std::shared_ptr<LN_Program> CropTree(const bool edge, std::string *r_report)
{
  TreeBuilder b;
  GenNode &every = b.Add("LogicNativeOnFixedUpdate", 30, 0);
  GenNode &growth = b.Add("LogicNativeGetGamePropertyFloat");
  b.String(growth, "Property", "pf0");
  GenNode &grow = b.Add("LogicNativeModifyProperty", int16_t(blender::NODE_MATH_ADD), 0);
  b.String(grow, "Property", "pf0");
  b.Float(grow, "Value", 1.0f);
  b.Link(every, "Out", grow, "Flow");
  const auto look = [&](GenNode &after, const char *out, const float height, const float red,
                        const float green, const float blue) -> GenNode & {
    GenNode &scale = b.Add("LogicNativeSetObjectAttribute", 5, 15); /* World Scale */
    b.Vector(scale, "Scale", 1.0f, 1.0f, height);
    b.Link(after, out, scale, "Flow");
    GenNode &rgba = b.Add("LogicNativeColorRGBA");
    b.Float(rgba, "R", red);
    b.Float(rgba, "G", green);
    b.Float(rgba, "B", blue);
    b.Float(rgba, "A", 1.0f);
    GenNode &color = b.Add("LogicNativeSetObjectAttribute", 11, 15); /* Object Color */
    b.Link(rgba, "Color", color, "Color");
    b.Link(scale, "Done", color, "Flow");
    return color;
  };
  if (edge) {
    GenNode &ripe = b.Add("LogicNativeCompare", 4); /* Greater or Equal */
    b.Link(growth, "Value", ripe, "A");
    b.Float(ripe, "B", 9.0f);
    GenNode &rising = b.Add("LogicNativeBooleanEdge");
    b.Link(ripe, "Result", rising, "Condition");
    look(rising, "Rising", 2.5f, 1.0f, 0.8f, 0.1f);
  }
  else {
    GenNode &ripe = b.Add("LogicNativeCompare", 0); /* Equal */
    b.Link(growth, "Value", ripe, "A");
    b.Float(ripe, "B", 8.0f);
    GenNode &check = b.Add("LogicNativeBranch");
    b.Link(grow, "Done", check, "Flow");
    b.Link(ripe, "Result", check, "Condition");
    look(check, "True", 2.5f, 1.0f, 0.8f, 0.1f);
  }
  GenNode &full = b.Add("LogicNativeCompare", 4);
  b.Link(growth, "Value", full, "A");
  b.Float(full, "B", 14.0f);
  GenNode &cut = b.Add("LogicNativeBranch");
  b.Link(grow, "Done", cut, "Flow");
  b.Link(full, "Result", cut, "Condition");
  GenNode &reset = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(reset, "Property", "pf0");
  b.Float(reset, "Value", 0.0f);
  b.Link(cut, "True", reset, "Flow");
  GenNode &back = look(reset, "Done", 1.0f, 0.2f, 0.8f, 0.2f);
  GenNode &harvest = b.Add("LogicNativeModifyProperty", int16_t(blender::NODE_MATH_ADD), 0);
  b.ObjectNamed(harvest, "Object", &diff::TargetBlenderObject());
  b.String(harvest, "Property", "pf2");
  b.Float(harvest, "Value", 1.0f);
  b.Link(back, "Done", harvest, "Flow");
  return b.Compile(r_report);
}

/** Every crop's growth at a different place of its cycle (as the farm's start values). */
void PlantCrops(SemRig &rig, const uint32_t owners)
{
  for (uint32_t index = 0; index < owners; index++) {
    rig.SetFloat(rig.Object(index), "pf0", float(index % 14));
  }
}

}  // namespace

/* The farm's crops end as on the old system, with the Boolean Edge (every step) and with the
 * Branch (asleep between their steps), over two whole cycles of Every 30 Steps. */
TEST(LN_Columns, FarmCropsEndAsTheOldSystem)
{
  for (const bool edge : {true, false}) {
    std::string report;
    const std::shared_ptr<LN_Program> program = CropTree(edge, &report);
    ASSERT_NE(program, nullptr) << report;
    std::string refusal;
    ASSERT_NE(LN_CompileColumnProgram(*program, LN_Event::OnFixedUpdate, &refusal), nullptr)
        << refusal;
    for (const uint32_t helpers : {0u, 3u}) {
      const std::string what = std::string(edge ? "edge" : "branch") + ", helpers " +
                               std::to_string(helpers);
      EXPECT_GT(ExpectColumnsMatch(program,
                                   LN_Event::OnFixedUpdate,
                                   300,
                                   70,
                                   what,
                                   helpers,
                                   nullptr,
                                   [](SemRig &rig) { PlantCrops(rig, 300); }),
                0u)
          << what;
    }
  }
}

/* The farm's crops, ns a crop and step over whole cycles of Every 30 Steps (plan / capture /
 * record / flush), old system against columns, one thread and nine. UPBGE_LN_BENCH_OWNERS (default
 * 10,000), UPBGE_LN_BENCH_ROUNDS (cycles, default 6), UPBGE_LN_BENCH_SETUP. Run with
 * --gtest_also_run_disabled_tests. */
TEST(LN_Columns, DISABLED_FarmBench)
{
  const char *owners_text = std::getenv("UPBGE_LN_BENCH_OWNERS");
  const uint32_t kOwners = owners_text != nullptr ? uint32_t(std::max(1, std::atoi(owners_text))) :
                                                    10000u;
  const char *rounds_text = std::getenv("UPBGE_LN_BENCH_ROUNDS");
  const int rounds = rounds_text != nullptr ? std::max(1, std::atoi(rounds_text)) : 6;
  const char *only_setup = std::getenv("UPBGE_LN_BENCH_SETUP");
  ColumnScope scope(1);
  struct Setup {
    const char *name;
    bool columns;
    uint32_t helpers;
  };
  for (const bool edge : {true, false}) {
    std::string report;
    const std::shared_ptr<LN_Program> program = CropTree(edge, &report);
    ASSERT_NE(program, nullptr) << report;
    for (const Setup &setup : {Setup{"old, 1 thread", false, 0},
                               Setup{"columns, 1 thread", true, 0},
                               Setup{"old batch, 9", false, 8},
                               Setup{"columns, 9", true, 8}})
    {
      if (only_setup != nullptr && std::strstr(setup.name, only_setup) == nullptr) {
        continue;
      }
      SemRig rig(program, kMemoOn, kOwners);
      /* The Barn: the first object, so the rig's name lookup (a scan of the object list, where the
       * game's manager has a table) finds it at once. */
      rig.Object(0).SetName("Target");
      rig.NoteObjectListChanged();
      rig.SetDump(false);
      rig.SetCapturePrints(false);
      PlantCrops(rig, kOwners);
      if (setup.columns) {
        rig.SetColumns(true, setup.helpers, 1);
      }
      else if (setup.helpers > 0) {
        rig.SetBatchRecording(36, setup.helpers);
        rig.SetParallelCapture(setup.helpers);
      }
      for (int step = 0; step < 30; step++) {
        rig.Step();
      }
      double best[4] = {1.0e9, 1.0e9, 1.0e9, 1.0e9};
      double best_total = 1.0e9;
      for (int round = 0; round < rounds; round++) {
        rig.ResetTimes();
        for (int step = 0; step < 30; step++) {
          rig.Step();
        }
        const SemRig::PhaseTimes &t = rig.Times();
        const double phase[4] = {t.plan_ms, t.capture_ms - t.plan_ms, t.record_ms, t.flush_ms};
        for (int k = 0; k < 4; k++) {
          best[k] = std::min(best[k], phase[k]);
        }
        best_total = std::min(best_total, t.capture_ms + t.record_ms + t.flush_ms);
      }
      const double scale = 1.0e6 / (double(kOwners) * 30.0);
      std::printf("FARM_BENCH %-6s %-18s plan %5.1f  capture %5.1f  record %5.1f  flush %5.1f  "
                  "total %5.1f ns a crop and step\n",
                  edge ? "edge" : "branch",
                  setup.name,
                  best[0] * scale,
                  best[1] * scale,
                  best[2] * scale,
                  best[3] * scale,
                  best_total * scale);
    }
  }
}

/* The play report (LN_Manager::WriteGameReport, the Logic Nodes sidebar's Performance panel):
 * what the column system does with a program's events, a rewrite hint for a refusal, and the
 * nodes that keep a slowed tree awake. */
TEST(LN_Columns, ThePlayReportSaysHowTreesRunAndWhatKeepsThemAwake)
{
  std::string report;
  const std::shared_ptr<LN_Program> edge = CropTree(true, &report);
  ASSERT_NE(edge, nullptr) << report;
  const std::shared_ptr<LN_Program> branch = CropTree(false, &report);
  ASSERT_NE(branch, nullptr) << report;
  /* The Boolean Edge's route runs every step: the crop never sleeps; with the Branch it does. */
  EXPECT_FALSE(LN_NodesKeepingTreesAwake(*edge).empty());
  EXPECT_TRUE(LN_NodesKeepingTreesAwake(*branch).empty());
  /* A tree that moves its object and then measures from it is refused (a column pass). */
  TreeBuilder b;
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode &move = b.Add("LogicNativeApplyMovement");
  b.Vector(move, "Vector", 0.5f, 0.0f, 0.0f);
  b.Link(step, "Out", move, "Flow");
  GenNode &distance = b.Add("LogicNativeGetDistance", 0, 0);
  b.ObjectNamed(distance, "Object B", &diff::TargetBlenderObject());
  GenNode &keep = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(keep, "Property", "pf0");
  b.Link(distance, "Distance", keep, "Value");
  b.Link(move, "Done", keep, "Flow");
  const std::shared_ptr<LN_Program> late = b.Compile(&report);
  ASSERT_NE(late, nullptr) << report;
  if (DirectForced()) {
    /* (Every program a column pass: no verdict is Wide.) */
    return;
  }
  {
    ColumnScope scope(1);
    LN_ColumnRunner runner;
    const LN_ColumnVerdict wide = runner.Verdict(*edge, LN_Event::OnFixedUpdate);
    EXPECT_EQ(wide.kind, LN_ColumnVerdict::Kind::Wide) << wide.reason;
    EXPECT_EQ(runner.Verdict(*edge, LN_Event::OnUpdate).kind, LN_ColumnVerdict::Kind::None);
    const LN_ColumnVerdict pass = runner.Verdict(*late, LN_Event::OnFixedUpdate);
    ASSERT_EQ(pass.kind, LN_ColumnVerdict::Kind::Pass);
    EXPECT_EQ(pass.reason, "a distance from the owner first read after a move of it");
    EXPECT_EQ(LN_ColumnRefusalHint(pass.reason),
              "Read Get Distance before the nodes that move this object in the same step.");
  }
  {
    ColumnScope scope(0);
    LN_ColumnRunner runner;
    EXPECT_EQ(runner.Verdict(*edge, LN_Event::OnFixedUpdate).kind, LN_ColumnVerdict::Kind::Off);
  }
  EXPECT_TRUE(LN_ColumnRefusalHint("something no hint knows").empty());
}

namespace {

/* On Logic + Physics Step, an object with a tree that changes the Target's game properties every
 * step ("tp" += 1.25, "tb" toggled through its own "flip"), recorded before the crowd (`first`)
 * or after it. */
void AddTargetChanger(SemRig &rig, const bool first)
{
  KX_GameObject &target = rig.Object(rig.ObjectCount() - 1);
  rig.SetFloat(target, "tp", 0.25f);
  rig.SetBool(target, "tb", false);
  TreeBuilder b;
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode &add = b.Add("LogicNativeModifyProperty");
  b.ObjectNamed(add, "Object", &diff::TargetBlenderObject());
  b.String(add, "Property", "tp");
  b.Float(add, "Value", 1.25f);
  b.Link(step, "Out", add, "Flow");
  GenNode &flip = b.Add("LogicNativeGetGamePropertyBool");
  b.String(flip, "Property", "flip");
  GenNode &branch = b.Add("LogicNativeBranch");
  b.Link(flip, "Value", branch, "Condition");
  b.Link(add, "Done", branch, "Flow");
  for (const bool on : {true, false}) {
    GenNode &own = b.Add("LogicNativeSetGamePropertyBool");
    b.String(own, "Property", "flip");
    b.Bool(own, "Value", !on);
    b.Link(branch, on ? "True" : "False", own, "Flow");
    GenNode &set = b.Add("LogicNativeSetGamePropertyBool");
    b.ObjectNamed(set, "Object", &diff::TargetBlenderObject());
    b.String(set, "Property", "tb");
    b.Bool(set, "Value", on);
    b.Link(own, "Done", set, "Flow");
  }
  std::string report;
  const std::shared_ptr<LN_Program> changer = b.Compile(&report);
  ASSERT_NE(changer, nullptr) << report;
  KX_GameObject &object = rig.AddObject("Changer" + std::to_string(rig.ObjectCount()),
                                        MT_Vector3(-4.0f, 2.0f, 0.0f));
  rig.SetBool(object, "flip", false);
  rig.AddTree(changer, rig.ObjectCount() - 1, first ? 0 : 999, first);
}

}  // namespace

/* Plan C2 fix 1 (the town's lamps and villagers): Get Game Property of a named object, read once
 * a run by the VM's own code (another object's property changes only at the flush), as a Float,
 * an Int and a Bool, a missing one through its read's fallback, and inside a Boolean Edge (a
 * stateful node's input: a bridged read was refused there). Native, so on the helpers too. */
TEST(LN_Columns, AnotherObjectsPropertyIsReadOnceARunAndMayFeedAnEdge)
{
  TreeBuilder b;
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode &hour = b.Add("LogicNativeGetGamePropertyFloat");
  b.ObjectNamed(hour, "Object", &diff::TargetBlenderObject());
  b.String(hour, "Property", "tp");
  GenNode &hour_int = b.Add("LogicNativeGetGamePropertyInt");
  b.ObjectNamed(hour_int, "Object", &diff::TargetBlenderObject());
  b.String(hour_int, "Property", "tp");
  GenNode &flag = b.Add("LogicNativeGetGamePropertyBool");
  b.ObjectNamed(flag, "Object", &diff::TargetBlenderObject());
  b.String(flag, "Property", "tb");
  GenNode &missing = b.Add("LogicNativeGetGamePropertyFloat");
  b.ObjectNamed(missing, "Object", &diff::TargetBlenderObject());
  b.String(missing, "Property", "nothing here");
  GenNode &own = b.Add("LogicNativeGetGamePropertyFloat");
  b.String(own, "Property", "pf0");
  GenNode &sum = b.Add("LogicNativeMath", int16_t(blender::NODE_MATH_ADD));
  b.Link(hour, "Value", sum, "A");
  b.Link(own, "Value", sum, "B");
  GenNode &night = b.Add("LogicNativeCompare", 2); /* Greater Than */
  b.Link(sum, "Result", night, "A");
  b.Float(night, "B", 4.0f);
  GenNode &edge = b.Add("LogicNativeBooleanEdge");
  b.Link(night, "Result", edge, "Condition");
  GenNode &count = b.Add("LogicNativeModifyProperty");
  b.String(count, "Property", "pf2");
  b.Float(count, "Value", 1.0f);
  b.Link(edge, "Rising", count, "Flow");
  GenNode &keep = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(keep, "Property", "pf1");
  b.Link(sum, "Result", keep, "Value");
  b.Link(step, "Out", keep, "Flow");
  GenNode &whole = b.Add("LogicNativeSetGamePropertyInt");
  b.String(whole, "Property", "pi0");
  b.Link(hour_int, "Value", whole, "Value");
  b.Link(keep, "Done", whole, "Flow");
  GenNode &gone = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(gone, "Property", "pf3");
  b.Link(missing, "Value", gone, "Value");
  b.Link(whole, "Done", gone, "Flow");
  GenNode &branch = b.Add("LogicNativeBranch");
  b.Link(flag, "Value", branch, "Condition");
  b.Link(gone, "Done", branch, "Flow");
  GenNode &mark = b.Add("LogicNativeSetGamePropertyBool");
  b.String(mark, "Property", "pb0");
  b.Bool(mark, "Value", true);
  b.Link(branch, "True", mark, "Flow");
  GenNode &unmark = b.Add("LogicNativeSetGamePropertyBool");
  b.String(unmark, "Property", "pb0");
  b.Bool(unmark, "Value", false);
  b.Link(branch, "False", unmark, "Flow");
  std::string report;
  const std::shared_ptr<LN_Program> program = b.Compile(&report);
  ASSERT_NE(program, nullptr) << report;
  std::string refusal;
  const std::shared_ptr<const LN_ColumnProgram> columns = LN_CompileColumnProgram(
      *program, LN_Event::OnFixedUpdate, &refusal);
  ASSERT_NE(columns, nullptr) << refusal << "\n" << LN_DumpProgramText(*program);
  const std::string text = LN_DescribeColumnProgram(*columns);
  EXPECT_NE(text.find("bridged=0"), std::string::npos) << text;
  EXPECT_NE(text.find("run_invariants=4"), std::string::npos) << text;
  for (const bool first : {true, false}) {
    for (const uint32_t helpers : {0u, 3u}) {
      const auto changer = [first](SemRig &rig) { AddTargetChanger(rig, first); };
      EXPECT_GT(ExpectColumnsMatch(program,
                                   LN_Event::OnFixedUpdate,
                                   helpers != 0 ? 600 : 40,
                                   12,
                                   std::string("another object's property, changed ") +
                                       (first ? "before" : "after") + " the crowd" +
                                       (helpers != 0 ? " (3 helpers)" : ""),
                                   helpers,
                                   nullptr,
                                   changer),
                0u);
    }
  }
}

/* Plan C2 fix 1 (the shooter's enemies, the tower defense's towers): an owner move on one route of
 * a Branch and a distance read, an Add Object at the owner or a local ray on the other: no NPC's
 * pass runs both, so the read is the VM's. On the same route they stay refused. */
TEST(LN_Columns, AMoveOnAnotherRouteLeavesTheReadsAsTheVMReadsThem)
{
  for (const bool same_route : {false, true}) {
    TreeBuilder b;
    GenNode &step = b.Add("LogicNativeOnFixedUpdate");
    GenNode &dead = b.Add("LogicNativeGetGamePropertyBool");
    b.String(dead, "Property", "pb0");
    GenNode &branch = b.Add("LogicNativeBranch");
    b.Link(dead, "Value", branch, "Condition");
    b.Link(step, "Out", branch, "Flow");
    /* True: back to the spawn point (a teleport), alive again. */
    GenNode &spawn = b.Add("LogicNativeSetObjectAttribute", 0, 15);
    b.Vector(spawn, "Value", 6.0f, -3.0f, 0.0f);
    b.Link(branch, "True", spawn, "Flow");
    GenNode &alive = b.Add("LogicNativeSetGamePropertyBool");
    b.String(alive, "Property", "pb0");
    b.Bool(alive, "Value", false);
    b.Link(spawn, "Done", alive, "Flow");
    /* False (or after the teleport): close to the Target dies, else walks. */
    GenNode &distance = b.Add("LogicNativeGetDistance", 0, 0);
    b.ObjectNamed(distance, "Object B", &diff::TargetBlenderObject());
    GenNode &close = b.Add("LogicNativeCompare", 3); /* Less Than */
    b.Link(distance, "Distance", close, "A");
    b.Float(close, "B", 3.0f);
    GenNode &hit = b.Add("LogicNativeBranch");
    b.Link(close, "Result", hit, "Condition");
    b.Link(same_route ? alive : branch, same_route ? "Done" : "False", hit, "Flow");
    GenNode &die = b.Add("LogicNativeSetGamePropertyBool");
    b.String(die, "Property", "pb0");
    b.Bool(die, "Value", true);
    b.Link(hit, "True", die, "Flow");
    GenNode &walk = b.Add("LogicNativeApplyMovement");
    b.Vector(walk, "Vector", 0.4f, 0.15f, 0.0f);
    b.Link(hit, "False", walk, "Flow");
    GenNode &keep = b.Add("LogicNativeSetGamePropertyFloat");
    b.String(keep, "Property", "pf1");
    b.Link(distance, "Distance", keep, "Value");
    b.Link(hit, "False", keep, "Flow");
    std::string report;
    const std::shared_ptr<LN_Program> program = b.Compile(&report);
    ASSERT_NE(program, nullptr) << report;
    std::string refusal;
    const bool lowers = LN_CompileColumnProgram(*program, LN_Event::OnFixedUpdate, &refusal) !=
                        nullptr;
    if (same_route) {
      EXPECT_FALSE(lowers);
      EXPECT_EQ(refusal, "a distance from the owner first read after a move of it");
      continue;
    }
    ASSERT_TRUE(lowers) << refusal << "\n" << LN_DumpProgramText(*program);
    EXPECT_GT(ExpectColumnsMatch(program, LN_Event::OnFixedUpdate, 40, 30, "teleport or measure"),
              0u);
    EXPECT_GT(ExpectColumnsMatch(
                  program, LN_Event::OnFixedUpdate, 600, 10, "teleport or measure (3 helpers)", 3),
              0u);
  }
  /* Add Object at the owner: on the other route of a turn (the tower's idle spin and its shot). */
  {
    BridgeOn bridge;
    for (const bool same_route : {false, true}) {
      TreeBuilder b;
      GenNode &step = b.Add("LogicNativeOnFixedUpdate");
      GenNode &ready = b.Add("LogicNativeGetGamePropertyBool");
      b.String(ready, "Property", "pb0");
      GenNode &branch = b.Add("LogicNativeBranch");
      b.Link(ready, "Value", branch, "Condition");
      b.Link(step, "Out", branch, "Flow");
      GenNode &spin = b.Add("LogicNativeApplyRotation");
      b.Rotation(spin, "Rotation", 0.0f, 0.0f, 0.02f);
      b.Link(branch, "False", spin, "Flow");
      GenNode &shot = b.Add("LogicNativeAddObject");
      b.ObjectNamed(shot, "Object to Add", &SparkBlenderObject());
      b.Link(same_route ? spin : branch, same_route ? "Done" : "True", shot, "Flow");
      std::string report;
      const std::shared_ptr<LN_Program> program = b.Compile(&report);
      ASSERT_NE(program, nullptr) << report;
      std::string refusal;
      const bool lowers = LN_CompileColumnProgram(*program, LN_Event::OnFixedUpdate, &refusal) !=
                          nullptr;
      EXPECT_EQ(lowers, !same_route) << refusal << "\n" << LN_DumpProgramText(*program);
      if (same_route) {
        EXPECT_EQ(refusal, "Add Object at the owner after a move of it");
      }
    }
    /* Next to a write to a named object (the enemy scores for the Player): a copy cannot be
     * it. Next to an object found by a name (here a constant one) it stays refused. */
    for (const bool by_name : {false, true}) {
      TreeBuilder b;
      GenNode &step = b.Add("LogicNativeOnFixedUpdate");
      GenNode &shot = b.Add("LogicNativeAddObject");
      b.ObjectNamed(shot, "Object to Add", &SparkBlenderObject());
      b.Link(step, "Out", shot, "Flow");
      GenNode &score = b.Add("LogicNativeModifyProperty");
      if (by_name) {
        GenNode &find = b.Add("LogicNativeObjectByName");
        b.String(find, "Name", "Target");
        b.Link(find, "Object", score, "Object");
      }
      else {
        b.ObjectNamed(score, "Object", &diff::TargetBlenderObject());
      }
      b.String(score, "Property", "pf2");
      b.Float(score, "Value", 10.0f);
      b.Link(shot, "Out", score, "Flow");
      std::string report;
      const std::shared_ptr<LN_Program> program = b.Compile(&report);
      ASSERT_NE(program, nullptr) << report;
      std::string refusal;
      const bool lowers = LN_CompileColumnProgram(*program, LN_Event::OnFixedUpdate, &refusal) !=
                          nullptr;
      EXPECT_EQ(lowers, !by_name) << refusal << "\n" << LN_DumpProgramText(*program);
      if (by_name) {
        EXPECT_EQ(refusal, "Add Object next to an object found while playing");
      }
    }
  }
}

/* Plan C2 fix 1 (the town's villagers, the physics game's crates): On Value Changed of a bool, an
 * int and a float runs natively (the VM's own step NPC by NPC, ValueChangeStep), its If Changed
 * route and its Old / New outputs (bridged reads of the state it wrote) end as on the old system;
 * its input from another object's property (a run invariant) keeps the program on the helpers. */
TEST(LN_Columns, OnValueChangedOfNumbersRunsNatively)
{
  BridgeOn bridge;
  for (const bool direct_property : {false, true}) {
    TreeBuilder b;
    GenNode &step = b.Add("LogicNativeOnFixedUpdate");
    GenNode &own = b.Add("LogicNativeGetGamePropertyFloat");
    b.String(own, "Property", "pf0");
    GenNode &hour = b.Add("LogicNativeGetGamePropertyFloat");
    b.ObjectNamed(hour, "Object", &diff::TargetBlenderObject());
    b.String(hour, "Property", "tp");
    GenNode &sum = b.Add("LogicNativeMath", int16_t(blender::NODE_MATH_ADD));
    b.Link(own, "Value", sum, "A");
    b.Link(hour, "Value", sum, "B");
    /* A bool (night) and a float (the sum itself); `direct_property`: the Target's "tp" linked
     * straight in (a generic property value: through the bridge). */
    GenNode &night = b.Add("LogicNativeCompare", 2); /* Greater Than */
    b.Link(sum, "Result", night, "A");
    b.Float(night, "B", 3.0f);
    GenNode *changed[2];
    changed[0] = &b.Add("LogicNativeValueChanged");
    b.Link(night, "Result", *changed[0], "Value");
    changed[1] = &b.Add("LogicNativeValueChanged");
    if (direct_property) {
      b.Link(hour, "Value", *changed[1], "Value");
    }
    else {
      b.Link(sum, "Result", *changed[1], "Value");
    }
    const char *counters[2] = {"pf2", "pf3"};
    for (int k = 0; k < 2; k++) {
      GenNode &count = b.Add("LogicNativeModifyProperty");
      b.String(count, "Property", counters[k]);
      b.Float(count, "Value", 1.0f);
      b.Link(*changed[k], "If Changed", count, "Flow");
    }
    /* A second op on the float's route: the change is read twice in the step (the second read
     * is the step's kept result, not a new comparison). */
    GenNode &again = b.Add("LogicNativeSetGamePropertyFloat");
    b.String(again, "Property", "pf4");
    b.Link(sum, "Result", again, "Value");
    b.Link(*changed[1], "If Changed", again, "Flow");
    if (direct_property) {
      /* The bool's Old value, read where it changed (a bridged read of the state it wrote; its
       * reach includes the change's input, so it stays on the main thread). */
      GenNode &old = b.Add("LogicNativeSetGamePropertyBool");
      b.String(old, "Property", "pb1");
      b.Link(*changed[0], "Old", old, "Value");
      b.Link(*changed[0], "If Changed", old, "Flow");
    }
    GenNode &keep = b.Add("LogicNativeSetGamePropertyFloat");
    b.String(keep, "Property", "pf1");
    b.Link(sum, "Result", keep, "Value");
    b.Link(step, "Out", keep, "Flow");
    std::string report;
    const std::shared_ptr<LN_Program> program = b.Compile(&report);
    ASSERT_NE(program, nullptr) << report;
    std::string refusal;
    const std::shared_ptr<const LN_ColumnProgram> columns = LN_CompileColumnProgram(
        *program, LN_Event::OnFixedUpdate, &refusal);
    ASSERT_NE(columns, nullptr) << refusal << "\n" << LN_DumpProgramText(*program);
    if (!direct_property) {
      EXPECT_NE(LN_DescribeColumnProgram(*columns).find("main_thread=0"), std::string::npos)
          << LN_DescribeColumnProgram(*columns);
    }
    for (const uint32_t helpers : {0u, 3u}) {
      const auto changer = [](SemRig &rig) { AddTargetChanger(rig, true); };
      EXPECT_GT(ExpectColumnsMatch(program,
                                   LN_Event::OnFixedUpdate,
                                   helpers != 0 ? 600 : 40,
                                   14,
                                   std::string("on value changed") +
                                       (direct_property ? " of a property" : "") +
                                       (helpers != 0 ? " (3 helpers)" : ""),
                                   helpers,
                                   nullptr,
                                   changer),
                0u);
    }
  }
}

/* Plan C2 fix 1 (the platformer's foes): a patrol whose wall and ledge rays decide between a turn
 * (one route) and a step forward (the other), and a touch check of the Player behind them (stomped:
 * coins for the Player, hidden). With the stomp's teleport it stays refused (see below). */
TEST(LN_Columns, APatrolTurningOrWalkingByItsRaysLowers)
{
  BridgeOn bridge;
  for (const bool teleport : {false, true}) {
    TreeBuilder b;
    b.SetRealSockets(true); /* (the rays' collision layers are ints) */
    GenNode &step = b.Add("LogicNativeOnFixedUpdate");
    GenNode &wall = b.Add("LogicNativeRaycast");
    b.Vector(wall, "Direction", 1.0f, 0.0f, 0.0f);
    b.Float(wall, "Max Distance", 0.8f);
    b.Bool(wall, "Local", true);
    b.Link(step, "Out", wall, "Flow");
    GenNode &ledge = b.Add("LogicNativeRaycast");
    b.Vector(ledge, "Direction", 0.8f, 0.0f, -1.2f);
    b.Float(ledge, "Max Distance", 1.6f);
    b.Bool(ledge, "Local", true);
    b.Link(wall, "Done", ledge, "Flow");
    GenNode &turn = b.Add("LogicNativeBranch");
    b.Link(wall, "Has Result", turn, "Condition");
    b.Link(ledge, "Done", turn, "Flow");
    GenNode &back = b.Add("LogicNativeApplyRotation");
    b.Rotation(back, "Rotation", 0.0f, 0.0f, 3.14159f);
    b.Bool(back, "Local", true);
    b.Link(turn, "True", back, "Flow");
    GenNode &edge = b.Add("LogicNativeBranch");
    b.Link(ledge, "Has Result", edge, "Condition");
    b.Link(turn, "False", edge, "Flow");
    GenNode &back2 = b.Add("LogicNativeApplyRotation");
    b.Rotation(back2, "Rotation", 0.0f, 0.0f, 3.14159f);
    b.Bool(back2, "Local", true);
    b.Link(edge, "False", back2, "Flow");
    GenNode &walk = b.Add("LogicNativeTranslate");
    b.Vector(walk, "Vector", 1.0f, 0.0f, 0.0f);
    b.Float(walk, "Speed", 2.0f);
    b.Bool(walk, "Local", true);
    b.Link(edge, "True", walk, "Flow");
    GenNode &distance = b.Add("LogicNativeGetDistance", 0, 0);
    b.ObjectNamed(distance, "Object B", &diff::TargetBlenderObject());
    GenNode &near = b.Add("LogicNativeCompare", 3); /* Less Than */
    b.Link(distance, "Distance", near, "A");
    b.Float(near, "B", 1.0f);
    GenNode &touch = b.Add("LogicNativeBranch");
    b.Link(near, "Result", touch, "Condition");
    b.Link(ledge, "Done", touch, "Flow");
    /* Stomped (the Player above): coins for the Player, hidden and sent away; else a life lost. */
    GenNode &player_pose = b.Add("LogicNativeGetObjectAttribute", 0);
    b.ObjectNamed(player_pose, "Object", &diff::TargetBlenderObject());
    GenNode &player_split = b.Add("LogicNativeSeparateXYZ");
    b.Link(player_pose, "Vector", player_split, "Vector");
    GenNode &own_pose = b.Add("LogicNativeGetObjectAttribute", 0);
    GenNode &own_split = b.Add("LogicNativeSeparateXYZ");
    b.Link(own_pose, "Vector", own_split, "Vector");
    GenNode &raised = b.Add("LogicNativeMath", int16_t(blender::NODE_MATH_ADD));
    b.Link(own_split, "Z", raised, "A");
    b.Float(raised, "B", 0.4f);
    GenNode &higher = b.Add("LogicNativeCompare", 2); /* Greater Than */
    b.Link(player_split, "Z", higher, "A");
    b.Link(raised, "Result", higher, "B");
    GenNode &above = b.Add("LogicNativeBranch");
    b.Link(higher, "Result", above, "Condition");
    b.Link(touch, "True", above, "Flow");
    GenNode &coins = b.Add("LogicNativeModifyProperty");
    b.ObjectNamed(coins, "Object", &diff::TargetBlenderObject());
    b.String(coins, "Property", "pf2");
    b.Float(coins, "Value", 5.0f);
    b.Link(above, "True", coins, "Flow");
    GenNode &hide = b.Add("LogicNativeSetObjectAttribute", 12, 15);
    b.Bool(hide, "Visible", false);
    b.Link(coins, "Done", hide, "Flow");
    if (teleport) {
      GenNode &away = b.Add("LogicNativeSetObjectAttribute", 0, 15);
      b.Vector(away, "Value", 0.0f, 30.0f, 1.0f);
      b.Bool(away, "Teleport", true);
      b.Link(coins, "Done", away, "Flow");
    }
    GenNode &hurt = b.Add("LogicNativeModifyProperty");
    b.ObjectNamed(hurt, "Object", &diff::TargetBlenderObject());
    b.String(hurt, "Property", "pf3");
    b.Float(hurt, "Value", 1.0f);
    b.Link(above, "False", hurt, "Flow");
    std::string report;
    const std::shared_ptr<LN_Program> program = b.Compile(&report);
    ASSERT_NE(program, nullptr) << report;
    std::string refusal;
    const auto columns = LN_CompileColumnProgram(*program, LN_Event::OnFixedUpdate, &refusal);
    if (teleport) {
      /* The compiler orders the stomp's routes before the turn's: the teleport comes before
       * the ledge ray's read, which the VM casts again from where the teleport put the foe. */
      EXPECT_EQ(columns, nullptr);
      EXPECT_NE(refusal.find("it reads a ray or a shape cast from the owner"), std::string::npos)
          << refusal;
      continue;
    }
    ASSERT_NE(columns, nullptr) << refusal << "\n" << LN_DumpProgramText(*program);
    EXPECT_GT(ExpectColumnsMatch(program, LN_Event::OnFixedUpdate, 40, 10, "patrol"), 0u);
  }
}

/* Plan C2 fix 1 (the physics game's crates): a Boolean Edge of Colliding (a bridged read the
 * stateful edge's input refused) lowers: the VM reads it at the edge's first evaluation of the
 * tick, the run at each read site, from the tree's per-tick collision cache. The rig has no
 * physics (never colliding): the run is compared, the contacts are the game's (research.py). */
TEST(LN_Columns, ABooleanEdgeOfCollidingLowers)
{
  BridgeOn bridge;
  TreeBuilder b;
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode &touch = b.Add("LogicNativeCollision");
  b.String(touch, "Property", "ball");
  GenNode &edge = b.Add("LogicNativeBooleanEdge");
  b.Link(touch, "Colliding", edge, "Condition");
  GenNode &count = b.Add("LogicNativeModifyProperty");
  b.String(count, "Property", "pf2");
  b.Float(count, "Value", 1.0f);
  b.Link(edge, "Rising", count, "Flow");
  GenNode &off = b.Add("LogicNativeSetGamePropertyBool");
  b.String(off, "Property", "pb0");
  b.Bool(off, "Value", false);
  b.Link(edge, "Falling", off, "Flow");
  GenNode &keep = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(keep, "Property", "pf1");
  b.Float(keep, "Value", 2.5f);
  b.Link(step, "Out", keep, "Flow");
  std::string report;
  const std::shared_ptr<LN_Program> program = b.Compile(&report);
  ASSERT_NE(program, nullptr) << report;
  std::string refusal;
  ASSERT_NE(LN_CompileColumnProgram(*program, LN_Event::OnFixedUpdate, &refusal), nullptr)
      << refusal << "\n" << LN_DumpProgramText(*program);
  EXPECT_GT(ExpectColumnsMatch(program, LN_Event::OnFixedUpdate, 40, 10, "edge of colliding"), 0u);
  /* Colliding of an object found by name: the VM's code reads more than a per-tick cache there
   * (the name's lookup), so inside the edge it stays refused. */
  TreeBuilder c;
  GenNode &step2 = c.Add("LogicNativeOnFixedUpdate");
  GenNode &find = c.Add("LogicNativeObjectByName");
  c.String(find, "Name", "Target");
  GenNode &touch2 = c.Add("LogicNativeCollision");
  c.Link(find, "Object", touch2, "Object");
  GenNode &edge2 = c.Add("LogicNativeBooleanEdge");
  c.Link(touch2, "Colliding", edge2, "Condition");
  GenNode &count2 = c.Add("LogicNativeModifyProperty");
  c.String(count2, "Property", "pf2");
  c.Float(count2, "Value", 1.0f);
  c.Link(edge2, "Rising", count2, "Flow");
  GenNode &keep2 = c.Add("LogicNativeSetGamePropertyFloat");
  c.String(keep2, "Property", "pf1");
  c.Float(keep2, "Value", 2.5f);
  c.Link(step2, "Out", keep2, "Flow");
  const std::shared_ptr<LN_Program> found = c.Compile(&report);
  ASSERT_NE(found, nullptr) << report;
  EXPECT_EQ(LN_CompileColumnProgram(*found, LN_Event::OnFixedUpdate, &refusal), nullptr);
  EXPECT_NE(refusal.find("inside another stateful node's input"), std::string::npos) << refusal;
}
