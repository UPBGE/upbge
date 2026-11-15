/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * The aiming math (LN_AimMath.h; 2026-11-11): Aim for Arc checked by flying the throw it gives
 * (the closed form of a fall with constant gravity) and Ray to Plane against hand-worked cases.
 * The nodes around them are checked in LN_NodeValues_test (every mode and folded).
 */

#include "testing/testing.h"

#include <cmath>
#include <limits>
#include <vector>

#include "LN_AimMath.h"

namespace {

const MT_Vector3 kGravity(0.0f, 0.0f, -9.81f);

MT_Vector3 FlyTo(const MT_Vector3 &start,
                 const MT_Vector3 &velocity,
                 const MT_Vector3 &gravity,
                 const float time)
{
  return start + velocity * time + gravity * (0.5f * time * time);
}

void ExpectNear(const MT_Vector3 &actual, const MT_Vector3 &expected, const float tolerance)
{
  EXPECT_NEAR(actual.x(), expected.x(), tolerance);
  EXPECT_NEAR(actual.y(), expected.y(), tolerance);
  EXPECT_NEAR(actual.z(), expected.z(), tolerance);
}

/** How far a throw at `velocity` gets along the ground (z back to the start's height). */
float LevelRange(const MT_Vector3 &velocity)
{
  const float time = 2.0f * velocity.z() / 9.81f;
  return std::sqrt(velocity.x() * velocity.x() + velocity.y() * velocity.y()) * time;
}

struct Case {
  MT_Vector3 start;
  MT_Vector3 target;
};

}  // namespace

TEST(LN_AimMath, SpeedGivesALowAndAHighArcThatBothLandOnTheTarget)
{
  const std::vector<Case> cases = {
      {MT_Vector3(0, 0, 0), MT_Vector3(20, 0, 0)},       /* level */
      {MT_Vector3(1, 2, 3), MT_Vector3(-12, 9, 3)},      /* level, another heading */
      {MT_Vector3(0, 0, 0), MT_Vector3(15, 5, 6)},       /* above */
      {MT_Vector3(0, 0, 10), MT_Vector3(25, -3, 0)},     /* below */
      {MT_Vector3(5, 5, 0), MT_Vector3(5.5f, 5, 0.5f)},  /* close */
  };
  const float speed = 20.0f;
  for (const Case &c : cases) {
    const LN_ArcSolution low = LN_SolveArc(
        c.start, c.target, kGravity, LN_ArcSolveMode::Speed, speed, false);
    const LN_ArcSolution high = LN_SolveArc(
        c.start, c.target, kGravity, LN_ArcSolveMode::Speed, speed, true);
    ASSERT_TRUE(low.can_reach);
    ASSERT_TRUE(high.can_reach);
    EXPECT_NEAR(low.velocity.length(), speed, 1.0e-3f);
    EXPECT_NEAR(high.velocity.length(), speed, 1.0e-3f);
    ExpectNear(FlyTo(c.start, low.velocity, kGravity, low.flight_time), c.target, 2.0e-3f);
    ExpectNear(FlyTo(c.start, high.velocity, kGravity, high.flight_time), c.target, 2.0e-3f);
    /* The high arc is steeper and slower. */
    EXPECT_GT(high.velocity.z(), low.velocity.z());
    EXPECT_GT(high.flight_time, low.flight_time);
    /* Both head toward the target. */
    const MT_Vector3 flat(c.target.x() - c.start.x(), c.target.y() - c.start.y(), 0.0f);
    EXPECT_GT(MT_Vector3(low.velocity.x(), low.velocity.y(), 0.0f).dot(flat), 0.0f);
  }
}

TEST(LN_AimMath, SpeedOutOfReachThrowsAsFarAsPossibleTowardTheTarget)
{
  const MT_Vector3 start(0, 0, 0);
  const MT_Vector3 target(1000, 0, 0);
  const LN_ArcSolution best = LN_SolveArc(
      start, target, kGravity, LN_ArcSolveMode::Speed, 20.0f, false);
  EXPECT_FALSE(best.can_reach);
  EXPECT_EQ(best.flight_time, 0.0f);
  EXPECT_NEAR(best.velocity.length(), 20.0f, 1.0e-3f);
  EXPECT_GT(best.velocity.x(), 0.0f);
  /* On level ground: 45 degrees, and no other angle goes farther. */
  EXPECT_NEAR(best.velocity.x(), best.velocity.z(), 1.0e-3f);
  const float best_range = LevelRange(best.velocity);
  for (const float degrees : {30.0f, 40.0f, 44.0f, 46.0f, 50.0f, 60.0f}) {
    const float angle = degrees * float(M_PI) / 180.0f;
    const MT_Vector3 other(20.0f * std::cos(angle), 0.0f, 20.0f * std::sin(angle));
    EXPECT_LT(LevelRange(other), best_range);
  }
  /* The high arc asked for the same: there is only the farthest throw. */
  const LN_ArcSolution high = LN_SolveArc(
      start, target, kGravity, LN_ArcSolveMode::Speed, 20.0f, true);
  EXPECT_FALSE(high.can_reach);
  ExpectNear(high.velocity, best.velocity, 1.0e-6f);
}

TEST(LN_AimMath, StraightUpAndDownTargets)
{
  const MT_Vector3 start(2, 3, 0);
  /* Above, in reach: straight up, low passing on the way up, high on the way back down. */
  const MT_Vector3 above(2, 3, 5);
  const LN_ArcSolution up_low = LN_SolveArc(
      start, above, kGravity, LN_ArcSolveMode::Speed, 15.0f, false);
  const LN_ArcSolution up_high = LN_SolveArc(
      start, above, kGravity, LN_ArcSolveMode::Speed, 15.0f, true);
  ASSERT_TRUE(up_low.can_reach);
  ASSERT_TRUE(up_high.can_reach);
  ExpectNear(up_low.velocity, MT_Vector3(0, 0, 15), 1.0e-4f);
  ExpectNear(FlyTo(start, up_low.velocity, kGravity, up_low.flight_time), above, 1.0e-3f);
  ExpectNear(FlyTo(start, up_high.velocity, kGravity, up_high.flight_time), above, 1.0e-3f);
  EXPECT_GT(up_high.flight_time, up_low.flight_time);
  /* Above, out of reach: straight up anyway. */
  const LN_ArcSolution too_high = LN_SolveArc(
      start, MT_Vector3(2, 3, 50), kGravity, LN_ArcSolveMode::Speed, 15.0f, false);
  EXPECT_FALSE(too_high.can_reach);
  ExpectNear(too_high.velocity, MT_Vector3(0, 0, 15), 1.0e-4f);
  /* Below: low throws down, high throws up and falls past the start. */
  const MT_Vector3 below(2, 3, -8);
  const LN_ArcSolution down_low = LN_SolveArc(
      start, below, kGravity, LN_ArcSolveMode::Speed, 6.0f, false);
  const LN_ArcSolution down_high = LN_SolveArc(
      start, below, kGravity, LN_ArcSolveMode::Speed, 6.0f, true);
  ASSERT_TRUE(down_low.can_reach);
  ASSERT_TRUE(down_high.can_reach);
  EXPECT_LT(down_low.velocity.z(), 0.0f);
  EXPECT_GT(down_high.velocity.z(), 0.0f);
  ExpectNear(FlyTo(start, down_low.velocity, kGravity, down_low.flight_time), below, 1.0e-3f);
  ExpectNear(FlyTo(start, down_high.velocity, kGravity, down_high.flight_time), below, 1.0e-3f);
  /* The target where the throw starts: there at once. */
  const LN_ArcSolution here = LN_SolveArc(
      start, start, kGravity, LN_ArcSolveMode::Speed, 6.0f, false);
  EXPECT_TRUE(here.can_reach);
  EXPECT_EQ(here.flight_time, 0.0f);
}

TEST(LN_AimMath, ArcHeightPutsTheTopThatHighAboveTheHigherEnd)
{
  const std::vector<Case> cases = {
      {MT_Vector3(0, 0, 0), MT_Vector3(10, 0, 0)},
      {MT_Vector3(0, 0, 0), MT_Vector3(-4, 6, 3)},
      {MT_Vector3(1, 1, 5), MT_Vector3(9, -2, 0)},
      {MT_Vector3(0, 0, 0), MT_Vector3(0, 0, 4)}, /* straight above */
  };
  for (const Case &c : cases) {
    for (const float height : {0.5f, 3.0f}) {
      const LN_ArcSolution throw_ = LN_SolveArc(
          c.start, c.target, kGravity, LN_ArcSolveMode::ArcHeight, height, false);
      ASSERT_TRUE(throw_.can_reach);
      ExpectNear(
          FlyTo(c.start, throw_.velocity, kGravity, throw_.flight_time), c.target, 2.0e-3f);
      /* The top: where the vertical speed is zero. */
      const float top_time = throw_.velocity.z() / 9.81f;
      const float top = FlyTo(c.start, throw_.velocity, kGravity, top_time).z();
      EXPECT_NEAR(top, std::max(c.start.z(), c.target.z()) + height, 2.0e-3f);
    }
  }
  /* No height, no rise: only a target lower or level ahead is reached by falling. */
  const LN_ArcSolution drop = LN_SolveArc(MT_Vector3(0, 0, 5),
                                          MT_Vector3(3, 0, 0),
                                          kGravity,
                                          LN_ArcSolveMode::ArcHeight,
                                          0.0f,
                                          false);
  ASSERT_TRUE(drop.can_reach);
  EXPECT_NEAR(drop.velocity.z(), 0.0f, 1.0e-6f);
  const LN_ArcSolution level = LN_SolveArc(
      MT_Vector3(0, 0, 0), MT_Vector3(3, 0, 0), kGravity, LN_ArcSolveMode::ArcHeight, 0.0f, false);
  EXPECT_FALSE(level.can_reach);
}

TEST(LN_AimMath, GravityMayPointAnyWay)
{
  const MT_Vector3 gravity(3.0f, 0.0f, -4.0f); /* down and sideways, 5 long */
  const MT_Vector3 start(0, 0, 0);
  const MT_Vector3 target(-6, 8, 1);
  for (const bool high : {false, true}) {
    const LN_ArcSolution throw_ = LN_SolveArc(
        start, target, gravity, LN_ArcSolveMode::Speed, 15.0f, high);
    ASSERT_TRUE(throw_.can_reach);
    ExpectNear(FlyTo(start, throw_.velocity, gravity, throw_.flight_time), target, 2.0e-3f);
  }
  const LN_ArcSolution by_height = LN_SolveArc(
      start, target, gravity, LN_ArcSolveMode::ArcHeight, 2.0f, false);
  ASSERT_TRUE(by_height.can_reach);
  ExpectNear(FlyTo(start, by_height.velocity, gravity, by_height.flight_time), target, 2.0e-3f);
}

TEST(LN_AimMath, WithoutGravityThrowsInAStraightLine)
{
  const MT_Vector3 zero(0, 0, 0);
  const LN_ArcSolution line = LN_SolveArc(
      MT_Vector3(1, 0, 0), MT_Vector3(1, 8, 6), zero, LN_ArcSolveMode::Speed, 5.0f, false);
  ASSERT_TRUE(line.can_reach);
  ExpectNear(line.velocity, MT_Vector3(0, 4, 3), 1.0e-5f);
  EXPECT_NEAR(line.flight_time, 2.0f, 1.0e-5f);
  /* An arc needs gravity. */
  EXPECT_FALSE(LN_SolveArc(MT_Vector3(1, 0, 0),
                           MT_Vector3(1, 8, 6),
                           zero,
                           LN_ArcSolveMode::ArcHeight,
                           2.0f,
                           false)
                   .can_reach);
  /* No speed: only the start itself. */
  EXPECT_FALSE(LN_SolveArc(zero, MT_Vector3(1, 0, 0), zero, LN_ArcSolveMode::Speed, 0.0f, false)
                   .can_reach);
}

TEST(LN_AimMath, InputsThatAreNotNumbersReachNothing)
{
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  const MT_Vector3 ok(1, 2, 3);
  for (const LN_ArcSolution &none :
       {LN_SolveArc(MT_Vector3(nan, 0, 0), ok, kGravity, LN_ArcSolveMode::Speed, 10, false),
        LN_SolveArc(ok, MT_Vector3(0, inf, 0), kGravity, LN_ArcSolveMode::Speed, 10, false),
        LN_SolveArc(ok, ok, MT_Vector3(0, 0, nan), LN_ArcSolveMode::Speed, 10, false),
        LN_SolveArc(MT_Vector3(0, 0, 0), ok, kGravity, LN_ArcSolveMode::Speed, -1, false),
        LN_SolveArc(MT_Vector3(0, 0, 0), ok, kGravity, LN_ArcSolveMode::ArcHeight, nan, false)})
  {
    EXPECT_FALSE(none.can_reach);
    EXPECT_EQ(none.flight_time, 0.0f);
    ExpectNear(none.velocity, MT_Vector3(0, 0, 0), 0.0f);
  }
}

TEST(LN_AimMath, RayToPlaneMeetsTheGroundAheadAndNothingElse)
{
  /* A cursor ray down at 45 degrees onto the ground z = 0. */
  const MT_Vector3 origin(0, 0, 10);
  const MT_Vector3 down(1, 0, -1);
  const MT_Vector3 point(0, 0, 0);
  const MT_Vector3 up(0, 0, 1);
  const float t = LN_RayPlaneDistance(origin - point, down, up);
  EXPECT_NEAR(t, 10.0f * std::sqrt(2.0f), 1.0e-4f);
  ExpectNear(LN_RayPlanePoint(origin, down, t), MT_Vector3(10, 0, 0), 1.0e-4f);
  ExpectNear(LN_RayPlaneNormal(down, up, t), MT_Vector3(0, 0, 1), 0.0f);
  /* Any length of direction and normal; a plane through another point. */
  const MT_Vector3 raised(2, 3, 4);
  const float t2 = LN_RayPlaneDistance(origin - raised, MT_Vector3(0, 0, -3), MT_Vector3(0, 0, 7));
  EXPECT_NEAR(t2, 6.0f, 1.0e-5f);
  ExpectNear(LN_RayPlanePoint(origin, MT_Vector3(0, 0, -3), t2), MT_Vector3(0, 0, 4), 1.0e-5f);
  /* From below: the normal turns toward the ray's start. */
  const MT_Vector3 under(0, 0, -5);
  const float t3 = LN_RayPlaneDistance(under - point, up, up);
  EXPECT_NEAR(t3, 5.0f, 1.0e-6f);
  ExpectNear(LN_RayPlaneNormal(up, up, t3), MT_Vector3(0, 0, -1), 0.0f);
  /* A ray on the plane's start point meets it at 0. */
  EXPECT_EQ(LN_RayPlaneDistance(MT_Vector3(0, 0, 0), down, up), 0.0f);
  /* Parallel, pointing away, zero vectors, numbers that are not: none (-1), zero outputs. */
  EXPECT_EQ(LN_RayPlaneDistance(origin, MT_Vector3(1, 0, 0), up), -1.0f);
  EXPECT_EQ(LN_RayPlaneDistance(origin, MT_Vector3(0, 0, 1), up), -1.0f);
  EXPECT_EQ(LN_RayPlaneDistance(origin, MT_Vector3(0, 0, 0), up), -1.0f);
  EXPECT_EQ(LN_RayPlaneDistance(origin, down, MT_Vector3(0, 0, 0)), -1.0f);
  const float nan = std::numeric_limits<float>::quiet_NaN();
  EXPECT_EQ(LN_RayPlaneDistance(MT_Vector3(nan, 0, 0), down, up), -1.0f);
  ExpectNear(LN_RayPlanePoint(origin, down, -1.0f), MT_Vector3(0, 0, 0), 0.0f);
  ExpectNear(LN_RayPlaneNormal(down, up, -1.0f), MT_Vector3(0, 0, 0), 0.0f);
}
