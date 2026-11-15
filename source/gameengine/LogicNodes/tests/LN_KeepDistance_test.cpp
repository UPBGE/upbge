/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Keep Distance's push (LN_KeepDistanceAcceleration, 2026-11-12) against numbers worked out by
 * hand: the falloffs, the balance between surfaces, a dense mesh pushing as a coarse one, the
 * gaps measured again between checks, the damping and its limit. The node end to end, with real
 * Jolt queries: Physics/Jolt/tests/JoltLogicNodesQueries_test.cpp (JoltLogicNodes.KeepDistance*).
 */

#include "testing/testing.h"

#include "LN_KeepDistance.h"

namespace {

LN_KeepDistancePatch Patch(const MT_Vector3 &away, const float gap)
{
  LN_KeepDistancePatch patch;
  patch.away = away;
  patch.gap = gap;
  patch.point = -away * gap;
  return patch;
}

LN_KeepDistanceSettings Settings(const float distance = 1.0f,
                                 const float damping = 0.0f,
                                 const LN_KeepDistanceMode mode = LN_KeepDistanceMode::Push,
                                 const float reach = 2.0f)
{
  LN_KeepDistanceSettings settings;
  settings.mode = mode;
  settings.distance = distance;
  settings.reach = reach;
  settings.damping = damping;
  return settings;
}

LN_KeepDistanceSettings Pull(const float damping = 0.0f)
{
  return Settings(1.0f, damping, LN_KeepDistanceMode::Pull);
}

LN_KeepDistanceSettings Hold(const float damping = 0.0f, const float reach = 2.0f)
{
  return Settings(1.0f, damping, LN_KeepDistanceMode::Hold, reach);
}

MT_Vector3 Accelerate(const std::vector<LN_KeepDistancePatch> &patches,
                      const LN_KeepDistanceSettings &settings,
                      const MT_Vector3 &velocity = MT_Vector3(0.0f, 0.0f, 0.0f),
                      int *r_nearest = nullptr)
{
  return LN_KeepDistanceAcceleration(
      patches, MT_Vector3(0.0f, 0.0f, 0.0f), settings, 20.0f, velocity, 1.0f / 60.0f, r_nearest);
}

const MT_Vector3 kNone(0.0f, 0.0f, 0.0f);
const MT_Vector3 kUp(0.0f, 0.0f, 1.0f);
const MT_Vector3 kDown(0.0f, 0.0f, -1.0f);
const MT_Vector3 kEast(1.0f, 0.0f, 0.0f);

void ExpectVector(const MT_Vector3 &actual, const MT_Vector3 &expected, const float tolerance = 1e-4f)
{
  EXPECT_NEAR(actual.x(), expected.x(), tolerance);
  EXPECT_NEAR(actual.y(), expected.y(), tolerance);
  EXPECT_NEAR(actual.z(), expected.z(), tolerance);
}

}  // namespace

TEST(LN_KeepDistance, OneSurfacePushesAwayHarderTheCloserLinearOrSmooth)
{
  /* A floor 0.3 m below, Distance 1, Strength 20: Linear 20 x 0.7, Smooth 20 x 0.7^2. */
  const std::vector<LN_KeepDistancePatch> floor = {Patch(kUp, 0.3f)};
  int nearest = -2;
  ExpectVector(
      LN_KeepDistanceAcceleration(floor, kNone, Settings(), 20.0f, kNone, 1.0f / 60.0f, &nearest),
      kUp * 14.0f);
  EXPECT_EQ(nearest, 0);
  LN_KeepDistanceSettings smooth = Settings();
  smooth.smooth = true;
  ExpectVector(
      LN_KeepDistanceAcceleration(floor, kNone, smooth, 20.0f, kNone, 1.0f / 60.0f, nullptr),
      kUp * 9.8f);
  /* Touching: all of it; at or past Distance: nothing (the nearest is still reported). */
  ExpectVector(LN_KeepDistanceAcceleration(
                   {Patch(kUp, 0.0f)}, kNone, Settings(), 20.0f, kNone, 1.0f / 60.0f, nullptr),
               kUp * 20.0f);
  ExpectVector(
      LN_KeepDistanceAcceleration(
          {Patch(kUp, 1.0f)}, kNone, Settings(), 20.0f, kNone, 1.0f / 60.0f, &nearest),
      kNone);
  EXPECT_EQ(nearest, 0);
  /* Distance 0: only what touches. */
  ExpectVector(LN_KeepDistanceAcceleration(
                   {Patch(kUp, 0.0f)}, kNone, Settings(0.0f), 20.0f, kNone, 1.0f / 60.0f, nullptr),
               kUp * 20.0f);
  ExpectVector(LN_KeepDistanceAcceleration(
                   {Patch(kUp, 0.1f)}, kNone, Settings(0.0f), 20.0f, kNone, 1.0f / 60.0f, nullptr),
               kNone);
}

TEST(LN_KeepDistance, SurfacesOnBothSidesBalanceAndADenseMeshPushesAsACoarseOne)
{
  /* Between two equal walls: no push. */
  ExpectVector(LN_KeepDistanceAcceleration({Patch(kUp, 0.4f), Patch(kDown, 0.4f)},
                                           kNone,
                                           Settings(),
                                           20.0f,
                                           kNone,
                                           1.0f / 60.0f,
                                           nullptr),
               kNone);
  /* Nearer the floor (0.2) than the ceiling (0.8): weights 0.8 and 0.2, so the direction's
   * balance is 0.6, times the nearest weight 0.8, times 20: 9.6 up. */
  ExpectVector(LN_KeepDistanceAcceleration({Patch(kUp, 0.2f), Patch(kDown, 0.8f)},
                                           kNone,
                                           Settings(),
                                           20.0f,
                                           kNone,
                                           1.0f / 60.0f,
                                           nullptr),
               kUp * 9.6f);
  /* A floor seen as 50 patches pushes as one patch. */
  std::vector<LN_KeepDistancePatch> dense(50, Patch(kUp, 0.3f));
  ExpectVector(
      LN_KeepDistanceAcceleration(dense, kNone, Settings(), 20.0f, kNone, 1.0f / 60.0f, nullptr),
      kUp * 14.0f);
  /* A corner: half way between the two walls, a little weaker than one (balance 0.707). */
  const MT_Vector3 corner = LN_KeepDistanceAcceleration(
      {Patch(kUp, 0.3f), Patch(kEast, 0.3f)}, kNone, Settings(), 20.0f, kNone, 1.0f / 60.0f, nullptr);
  ExpectVector(corner, (kUp + kEast) * 7.0f);
}

TEST(LN_KeepDistance, BetweenChecksTheGapsAreMeasuredAgainFromTheMovement)
{
  /* Found 0.5 above the floor; the object moved 0.2 down since: 0.3 now, as a fresh check. */
  const std::vector<LN_KeepDistancePatch> floor = {Patch(kUp, 0.5f)};
  ExpectVector(LN_KeepDistanceAcceleration(
                   floor, MT_Vector3(0.0f, 0.0f, -0.2f), Settings(), 20.0f, kNone, 0.016f, nullptr),
               kUp * 14.0f);
  /* Moving along the floor changes nothing; moving through it is touching (0). */
  ExpectVector(LN_KeepDistanceAcceleration(
                   floor, MT_Vector3(3.0f, 0.0f, 0.0f), Settings(), 20.0f, kNone, 0.016f, nullptr),
               kUp * 10.0f);
  ExpectVector(LN_KeepDistanceAcceleration(
                   floor, MT_Vector3(0.0f, 0.0f, -2.0f), Settings(), 20.0f, kNone, 0.016f, nullptr),
               kUp * 20.0f);
}

TEST(LN_KeepDistance, DampingBrakesAlongThePushOnlyAndNeverReversesTheMovement)
{
  const std::vector<LN_KeepDistancePatch> floor = {Patch(kUp, 0.3f)};
  /* Falling at 2 m/s: brake 2 x 1 x sqrt(20 / 1) x 2 x 0.7 = 12.522 more up. */
  const float brake = 2.0f * std::sqrt(20.0f) * 2.0f * 0.7f;
  ExpectVector(LN_KeepDistanceAcceleration(floor,
                                           kNone,
                                           Settings(1.0f, 1.0f),
                                           20.0f,
                                           MT_Vector3(0.0f, 0.0f, -2.0f),
                                           1.0f / 60.0f,
                                           nullptr),
               kUp * (14.0f + brake),
               1e-3f);
  /* Rising away from it is braked too; sideways movement is not. */
  ExpectVector(LN_KeepDistanceAcceleration(floor,
                                           kNone,
                                           Settings(1.0f, 1.0f),
                                           20.0f,
                                           MT_Vector3(5.0f, 0.0f, 2.0f),
                                           1.0f / 60.0f,
                                           nullptr),
               kUp * (14.0f - brake),
               1e-3f);
  /* A huge Damping stops the movement along the push within the step, no more. */
  ExpectVector(LN_KeepDistanceAcceleration(floor,
                                           kNone,
                                           Settings(1.0f, 100.0f),
                                           20.0f,
                                           MT_Vector3(0.0f, 0.0f, -0.01f),
                                           0.1f,
                                           nullptr),
               kUp * (14.0f + 0.1f),
               1e-3f);
  /* Nothing near: no brake either. */
  ExpectVector(LN_KeepDistanceAcceleration({Patch(kUp, 2.0f)},
                                           kNone,
                                           Settings(1.0f, 1.0f),
                                           20.0f,
                                           MT_Vector3(0.0f, 0.0f, -3.0f),
                                           1.0f / 60.0f,
                                           nullptr),
               kNone);
}

TEST(LN_KeepDistance, PullGoesTowardTheNearestSurfaceOnlyHarderTheCloser)
{
  /* A floor 0.3 below and a wall 0.6 away (west of the object, pushing east): the floor only,
   * Strength 20 x 0.7 down (Smooth: x 0.49); the push would go up and east. */
  const std::vector<LN_KeepDistancePatch> both = {Patch(kEast, 0.6f), Patch(kUp, 0.3f)};
  int nearest = -2;
  ExpectVector(Accelerate(both, Pull(), kNone, &nearest), kDown * 14.0f);
  EXPECT_EQ(nearest, 1);
  LN_KeepDistanceSettings smooth = Pull();
  smooth.smooth = true;
  ExpectVector(Accelerate(both, smooth), kDown * 9.8f);
  /* Touching: all of it (it holds on); at or past Distance: nothing. */
  ExpectVector(Accelerate({Patch(kUp, 0.0f)}, Pull()), kDown * 20.0f);
  ExpectVector(Accelerate({Patch(kUp, 1.0f)}, Pull()), kNone);
  ExpectVector(Accelerate({Patch(kUp, 3.0f)}, Pull(1.0f), MT_Vector3(0.0f, 0.0f, -2.0f)), kNone);
  /* Two equal walls: the first of the nearest (as the check ordered them), never between. */
  const MT_Vector3 corridor = Accelerate({Patch(kEast, 0.4f), Patch(-kEast, 0.4f)}, Pull());
  ExpectVector(corridor, -kEast * 12.0f);
  /* Damping brakes the approach (falling toward the floor at 2 m/s: 14 - 12.522) and holds it
   * back when it leaves (14 + 12.522). */
  const float brake = 2.0f * std::sqrt(20.0f) * 2.0f * 0.7f;
  ExpectVector(Accelerate({Patch(kUp, 0.3f)}, Pull(1.0f), MT_Vector3(0.0f, 0.0f, -2.0f)),
               kDown * (14.0f - brake),
               1e-3f);
  ExpectVector(Accelerate({Patch(kUp, 0.3f)}, Pull(1.0f), MT_Vector3(3.0f, 0.0f, 2.0f)),
               kDown * (14.0f + brake),
               1e-3f);
}

TEST(LN_KeepDistance, HoldPushesInsideDistanceAndPullsBackOutToReach)
{
  /* Inside Distance: the push (one floor 0.3 below: 14 up; two walls balance). */
  ExpectVector(Accelerate({Patch(kUp, 0.3f)}, Hold()), kUp * 14.0f);
  ExpectVector(Accelerate({Patch(kUp, 0.4f), Patch(kDown, 0.4f)}, Hold()), kNone);
  /* Farther: back toward the nearest, 20 x (gap - 1) / (2 - 1); Smooth that squared. */
  ExpectVector(Accelerate({Patch(kUp, 1.5f)}, Hold()), kDown * 10.0f);
  ExpectVector(Accelerate({Patch(kUp, 1.9f), Patch(kEast, 1.5f)}, Hold()), -kEast * 10.0f);
  LN_KeepDistanceSettings smooth = Hold();
  smooth.smooth = true;
  ExpectVector(Accelerate({Patch(kUp, 1.5f)}, smooth), kDown * 5.0f);
  /* At Distance: nothing; past Reach: nothing; a longer Reach pulls from farther. */
  ExpectVector(Accelerate({Patch(kUp, 1.0f)}, Hold()), kNone);
  ExpectVector(Accelerate({Patch(kUp, 2.5f)}, Hold()), kNone);
  ExpectVector(Accelerate({Patch(kUp, 2.5f)}, Hold(0.0f, 4.0f)), kDown * (20.0f * 1.5f / 3.0f));
  /* A Reach short of Distance: no pull at all (Hold is Push with its damping). */
  ExpectVector(Accelerate({Patch(kUp, 1.5f)}, Hold(0.0f, 0.5f)), kNone);
}

TEST(LN_KeepDistance, HoldBrakesFullyAroundDistanceAndLessTowardReach)
{
  /* The spring Damping 1 brakes critically: 20 / (Reach / 2) = 20 per meter at Reach 2. */
  const float c = 2.0f * std::sqrt(20.0f);
  /* Rising through Distance at 1 m/s: no pull there, all of the brake (8.944 down). */
  ExpectVector(Accelerate({Patch(kUp, 1.0f)}, Hold(1.0f), MT_Vector3(0.0f, 0.0f, 1.0f)),
               kDown * c,
               1e-3f);
  /* Falling at 2 m/s 0.9 above the floor: Push brakes faded with its weight (0.1), Hold fully. */
  const MT_Vector3 falling(0.0f, 0.0f, -2.0f);
  ExpectVector(Accelerate({Patch(kUp, 0.9f)}, Settings(1.0f, 1.0f), falling),
               kUp * (2.0f + c * 2.0f * 0.1f),
               1e-3f);
  ExpectVector(Accelerate({Patch(kUp, 0.9f)}, Hold(1.0f), falling),
               kUp * (2.0f + c * 2.0f),
               1e-3f);
  /* Half way to Reach: pulled 10 down, braked half (falling toward it: less pull). */
  ExpectVector(Accelerate({Patch(kUp, 1.5f)}, Hold(1.0f), falling),
               kDown * (10.0f - c * 2.0f * 0.5f),
               1e-3f);
  /* Past Reach: nothing, no brake either. */
  ExpectVector(Accelerate({Patch(kUp, 2.5f)}, Hold(1.0f), falling), kNone);
  /* Reach 4: the spring 20 / 2 per meter. */
  ExpectVector(Accelerate({Patch(kUp, 1.0f)}, Hold(1.0f, 4.0f), MT_Vector3(0.0f, 0.0f, 1.0f)),
               kDown * (2.0f * std::sqrt(10.0f)),
               1e-3f);
}
