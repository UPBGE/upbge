/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Compiled routes (lean runtime plan, phase A33): an event whose ops are all native, with no Loop
 * and no UI event route, runs as its LN_LeanRoute instead of the block walk. Every test compares a
 * run with routes on against the same run with them off, by the whole text of the differential
 * world (recorded commands, applied world, every tree's state), or by what the rig recorded. */

#include "testing/testing.h"

#include <algorithm>
#include <string>

#include "LN_LeanProgram.h"
#include "LN_SemRig.h"
#include "LN_TreeBatch.h"

using namespace ln_test;
using namespace ln_test::sem;

namespace {

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

struct LeanScope {
  explicit LeanScope(const int mode)
  {
    LN_SetLeanModeForTests(mode);
  }
  ~LeanScope()
  {
    LN_SetLeanModeForTests(-1);
  }
};

bool HasRoute(const LN_Program &program, const LN_Event event)
{
  program.EnsureLeanProgram();
  const std::shared_ptr<const LN_LeanProgram> &lean = program.GetLeanProgram();
  return lean != nullptr && lean->events[size_t(event)].has_route;
}

/** Routes off against on, the lean VM in Auto and Force, on 1, 3 and 17 owners, and (batch safe
 * programs) in a tree batch on helpers: the same text every time. Returns the route runs. */
uint64_t ExpectRouteEqualsWalk(const std::shared_ptr<LN_Program> &program, const std::string &what)
{
  const uint64_t routes_before = LN_RouteRunCount();
  for (const int lean : {1, 2}) {
    for (const uint32_t objects : {1u, 3u, 17u}) {
      diff::RunOptions walk;
      walk.objects = objects;
      walk.frames = 12;
      walk.lean = lean;
      walk.routes = 0;
      diff::RunOptions route = walk;
      route.routes = 1;
      const diff::RunResult a = diff::RunWorld(program, walk);
      const diff::RunResult b = diff::RunWorld(program, route);
      EXPECT_EQ(a.text, b.text) << what << ", lean " << lean << ", " << objects
                                << " objects: " << diff::FirstDifference(a.text, b.text);
      EXPECT_EQ(a.warnings, b.warnings) << what;
      EXPECT_TRUE(b.failure.empty()) << what << ": " << b.failure;
      if (objects == 17 && program->GetTreeBatchProfile().eligible) {
        diff::RunOptions batch = route;
        batch.chunks = 3;
        batch.helpers = 2;
        const diff::RunResult c = diff::RunWorld(program, batch);
        EXPECT_EQ(a.text, c.text) << what << ", lean " << lean
                                  << " (batch): " << diff::FirstDifference(a.text, c.text);
      }
    }
  }
  return LN_RouteRunCount() - routes_before;
}

}  // namespace

/* The brain in every form research.py plays it (On Update and On Logic + Physics Step with Every 1,
 * 2 and 4 Spread Out, measured or stored distance, with and without the Player's visibility, with 0
 * and 4 chains) has a route, and the route ends every run exactly as the block walk ends it. */
TEST(LN_LeanRoute, EveryBrainHasARouteThatEndsAsTheBlockWalk)
{
  size_t programs = 0;
  for (const int step_every : {0, 1, 2, 4}) {
    for (const int chains : {0, 4}) {
      for (const bool measure : {true, false}) {
        for (const bool look : {false, true}) {
          BrainOfOptions options;
          options.step_every = step_every;
          options.chains = chains;
          options.measure_distance = measure;
          options.look = look;
          const std::shared_ptr<LN_Program> program = BrainOf(options);
          ASSERT_NE(program, nullptr);
          const LN_Event event = step_every > 0 ? LN_Event::OnFixedUpdate : LN_Event::OnUpdate;
          const std::string what = "brain every " + std::to_string(step_every) + " chains " +
                                   std::to_string(chains) + (measure ? " measured" : " stored") +
                                   (look ? " look" : "");
          EXPECT_TRUE(HasRoute(*program, event)) << what;
          EXPECT_GT(ExpectRouteEqualsWalk(program, what), 0u) << what << ": the route never ran";
          programs++;
        }
      }
    }
  }
  EXPECT_EQ(programs, 32u);
}

/* What a route cannot hold keeps the block walk: a Loop, an op without a native handler (Print), a
 * route behind a Delay's Done. Those programs give the same text with routes on and off. */
TEST(LN_LeanRoute, ALoopAGenericOpOrADelayGuardKeepsTheBlockWalk)
{
  {
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
    std::string report;
    const std::shared_ptr<LN_Program> program = b.Compile(&report);
    ASSERT_NE(program, nullptr) << report;
    EXPECT_FALSE(HasRoute(*program, LN_Event::OnFixedUpdate)) << "a Loop";
    ExpectRouteEqualsWalk(program, "loop");
  }
  {
    TreeBuilder b;
    GenNode &event = b.Add("LogicNativeOnFixedUpdate");
    GenNode &print = b.Add("LogicNativePrint");
    b.Link(event, "Out", print, "Flow");
    GenNode &set = b.Add("LogicNativeSetGamePropertyFloat");
    b.String(set, "Property", "after");
    b.Float(set, "Value", 1.0f);
    b.Link(print, "Done", set, "Flow");
    std::string report;
    const std::shared_ptr<LN_Program> program = b.Compile(&report);
    ASSERT_NE(program, nullptr) << report;
    EXPECT_FALSE(HasRoute(*program, LN_Event::OnFixedUpdate)) << "a Print";
    ExpectRouteEqualsWalk(program, "print");
  }
  {
    TreeBuilder b;
    GenNode &event = b.Add("LogicNativeOnFixedUpdate");
    GenNode &delay = b.Add("LogicNativeDelay");
    b.Float(delay, "Delay", 0.04f);
    b.Link(event, "Out", delay, "Flow");
    GenNode &set = b.Add("LogicNativeSetGamePropertyFloat");
    b.String(set, "Property", "after");
    b.Float(set, "Value", 2.0f);
    b.Link(delay, "Out", set, "Flow");
    std::string report;
    const std::shared_ptr<LN_Program> program = b.Compile(&report);
    ASSERT_NE(program, nullptr) << report;
    EXPECT_FALSE(HasRoute(*program, LN_Event::OnFixedUpdate)) << "a Delay";
    ExpectRouteEqualsWalk(program, "delay");
  }
}

/* Run Logic Tree passes run a slowed route (Every 4) on the steps between its own, with the time
 * since its last run: with routes on, every step's recording is the block walk's, the passes
 * included. */
TEST(LN_LeanRoute, RunLogicTreePassesRunTheRouteAsTheWalkDoes)
{
  BrainOfOptions options;
  options.step_every = 4;
  const std::shared_ptr<LN_Program> program = BrainOf(options);
  ASSERT_NE(program, nullptr);
  ASSERT_TRUE(HasRoute(*program, LN_Event::OnFixedUpdate));
  constexpr uint32_t kOwners = 12;
  LeanScope lean(2);
  std::string walked, routed;
  for (const int routes : {0, 1}) {
    RouteScope scope(routes);
    SemRig rig(program, kMemoOn, kOwners);
    for (uint32_t index = 0; index < kOwners; index++) {
      rig.Object(index).NodeSetWorldPosition(
          MT_Vector3(float(index % 5) * 2.5f, float(index % 3), 0.0f));
      rig.Object(index).NodeUpdateGS(0.0);
    }
    std::string &out = routes == 0 ? walked : routed;
    for (int step = 0; step < 20; step++) {
      rig.Tick(LN_Event::OnFixedUpdate,
               [&](LN_CommandBuffer &buffer, const LN_TickContext &context) {
                 if (step % 3 == 1) {
                   rig.Force(size_t(step % kOwners), buffer, context);
                 }
               });
      out += rig.LastRecorded();
    }
  }
  EXPECT_FALSE(walked.empty());
  EXPECT_TRUE(walked == routed) << diff::FirstDifference(walked, routed);
}

/* A route is never run where it would leave out what the run asks for: a profiled run (the profile
 * counters), and a tree whose UI event routes only run (paused). Such runs walk the blocks. */
TEST(LN_LeanRoute, ProfiledAndPausedRunsWalkTheBlocks)
{
  BrainOfOptions options;
  options.step_every = 1;
  const std::shared_ptr<LN_Program> program = BrainOf(options);
  ASSERT_NE(program, nullptr);
  ASSERT_TRUE(HasRoute(*program, LN_Event::OnFixedUpdate));
  {
    diff::RunOptions profiled;
    profiled.lean = 2;
    profiled.routes = 1;
    profiled.profiled = true;
    const uint64_t before = LN_RouteRunCount();
    diff::RunWorld(program, profiled);
    EXPECT_EQ(LN_RouteRunCount(), before) << "a profiled run ran a route";
  }
  {
    LeanScope lean(2);
    RouteScope routes(1);
    SemRig rig(program, kMemoOn, 2);
    rig.Step();
    rig.Tree(0).SetUIEventRoutesOnly(true);
    rig.Tree(1).SetUIEventRoutesOnly(true);
    const uint64_t before = LN_RouteRunCount();
    rig.Step();
    EXPECT_EQ(LN_RouteRunCount(), before) << "a paused tree ran a route";
    rig.Tree(0).SetUIEventRoutesOnly(false);
    rig.Tree(1).SetUIEventRoutesOnly(false);
    rig.Step();
    EXPECT_GT(LN_RouteRunCount(), before);
  }
}

/* A Run Logic Tree pass in the same tick as the normal pass takes the other route of a Branch: the
 * normal pass's walk carried the object across the threshold, and the pass's Get Distance (taken
 * again in its own pass) sees it. The route runs only the ops of the route this pass takes, as the
 * walk does: a guard counts only the stamps of this pass (InstructionExecutedNow's scope). */
TEST(LN_LeanRoute, ARunLogicTreePassThatTakesTheOtherBranchRunsOnlyThatRoute)
{
  TreeBuilder b;
  GenNode &event = b.Add("LogicNativeOnFixedUpdate");
  GenNode &distance = b.Add("LogicNativeGetDistance", 0, 1); /* the owner to a position */
  GenNode &near = b.Add("LogicNativeCompare", 3);            /* Less Than */
  b.Link(distance, "Distance", near, "A");
  b.Float(near, "B", 6.0f);
  GenNode &branch = b.Add("LogicNativeBranch");
  b.Link(event, "Out", branch, "Flow");
  b.Link(near, "Result", branch, "Condition");
  GenNode &count_near = b.Add("LogicNativeModifyProperty", int16_t(blender::NODE_MATH_ADD), 0);
  b.String(count_near, "Property", "near");
  b.Float(count_near, "Value", 1.0f);
  b.Link(branch, "True", count_near, "Flow");
  GenNode &walk = b.Add("LogicNativeApplyMovement");
  b.Vector(walk, "Vector", 10.0f, 0.0f, 0.0f);
  b.Bool(walk, "Local", false);
  b.Link(count_near, "Done", walk, "Flow");
  GenNode &count_far = b.Add("LogicNativeModifyProperty", int16_t(blender::NODE_MATH_ADD), 0);
  b.String(count_far, "Property", "far");
  b.Float(count_far, "Value", 1.0f);
  b.Link(branch, "False", count_far, "Flow");
  std::string report;
  const std::shared_ptr<LN_Program> program = b.Compile(&report);
  ASSERT_NE(program, nullptr) << report;
  ASSERT_TRUE(HasRoute(*program, LN_Event::OnFixedUpdate));
  LeanScope lean(2);
  std::string walked, routed;
  float near_count[2] = {0.0f, 0.0f}, far_count[2] = {0.0f, 0.0f};
  for (const int routes : {0, 1}) {
    RouteScope scope(routes);
    SemRig rig(program, kMemoOn, 1);
    rig.SetFloat(rig.Object(0), "near", 0.0f);
    rig.SetFloat(rig.Object(0), "far", 0.0f);
    rig.Object(0).NodeSetWorldPosition(MT_Vector3(3.0f, 0.0f, 0.0f));
    rig.Object(0).NodeUpdateGS(0.0);
    const uint64_t routes_before = LN_RouteRunCount();
    rig.Tick(LN_Event::OnFixedUpdate, [&](LN_CommandBuffer &buffer, const LN_TickContext &context) {
      rig.Force(0, buffer, context);
    });
    (routes == 0 ? walked : routed) = rig.LastRecorded();
    near_count[routes] = rig.Float(rig.Object(0), "near");
    far_count[routes] = rig.Float(rig.Object(0), "far");
    if (routes == 1) {
      EXPECT_GE(LN_RouteRunCount() - routes_before, 2u) << "the pass and the normal run";
    }
  }
  /* The normal pass is near (it walks), the forced pass far. */
  EXPECT_EQ(near_count[0], 1.0f);
  EXPECT_EQ(far_count[0], 1.0f);
  EXPECT_EQ(near_count[1], near_count[0]);
  EXPECT_EQ(far_count[1], far_count[0]);
  EXPECT_TRUE(walked == routed) << diff::FirstDifference(walked, routed);
}

/* The lean memo keeps no expression that reads something that may change within the tick: one Add
 * over a tree property, read before and after a Set Tree Property in one pass, gives the new value
 * the second time, on the lean VM with its memo as on the old interpreter. */
TEST(LN_LeanRoute, TheLeanMemoKeepsNothingThatReadsATreeProperty)
{
  TreeBuilder b;
  GenNode &event = b.Add("LogicNativeOnFixedUpdate");
  GenNode &get = b.Add("LogicNativeGetTreeProperty");
  b.String(get, "Property", "p");
  GenNode &add = b.Add("LogicNativeMath", int16_t(blender::NODE_MATH_ADD));
  b.Link(get, "Value", add, "A");
  b.Float(add, "B", 1.0f);
  GenNode &sequence = b.Add("LogicNativeSequence", 0, 0, 3);
  b.Link(event, "Out", sequence, "Flow");
  GenNode &first = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(first, "Property", "before");
  b.Link(sequence, "Then 0", first, "Flow");
  b.Link(add, "Result", first, "Value");
  GenNode &seed = b.Add("LogicNativeGetGamePropertyFloat");
  b.String(seed, "Property", "seed");
  GenNode &set = b.Add("LogicNativeSetTreeProperty");
  b.String(set, "Property", "p");
  b.Link(sequence, "Then 1", set, "Flow");
  b.Link(seed, "Value", set, "Value");
  GenNode &second = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(second, "Property", "after");
  b.Link(sequence, "Then 2", second, "Flow");
  b.Link(add, "Result", second, "Value");
  std::string report;
  const std::shared_ptr<LN_Program> program = b.Compile(&report);
  ASSERT_NE(program, nullptr) << report;
  for (const int lean_mode : {0, 2}) {
    SCOPED_TRACE(lean_mode == 0 ? "old interpreter" : "lean VM with its memo");
    LeanScope lean(lean_mode);
    LN_RuntimeTree::SetLeanMemoForTests(1);
    SemRig rig(program, kMemoOn, 1);
    rig.SetFloat(rig.Object(0), "seed", 5.0f);
    rig.Step();
    EXPECT_EQ(rig.Float(rig.Object(0), "before"), 1.0f);
    EXPECT_EQ(rig.Float(rig.Object(0), "after"), 6.0f) << "the second read sees the write";
    LN_RuntimeTree::SetLeanMemoForTests(-1);
  }
}
