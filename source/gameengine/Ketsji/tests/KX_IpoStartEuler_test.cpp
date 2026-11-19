/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* KX_IpoStartEuler (2026-11-17): the X, Y and Z angles an action keeps for the rotation channels
 * it does not key. Blender keeps the numbers typed in the Rotation fields; the engine took
 * another set of numbers for the same turn when the typed Y was beyond +-90 or the turn pointed
 * straight up or down with both X and Z typed, and an action keying only Z played 180 (or 45)
 * degrees off. The game check ~/Desktop/CPP_LN_TESTS/euler_check/ plays it. */

#include "testing/testing.h"

#include <cmath>
#include <cstdint>
#include <cstring>

#include "KX_IpoController.h"

namespace {

constexpr double kDegree = M_PI / 180.0;

MT_Matrix3x3 Turn(const double x, const double y, const double z)
{
  return MT_Matrix3x3(MT_Vector3(float(x * kDegree), float(y * kDegree), float(z * kDegree)));
}

/** The largest difference of an entry between `m` and the turn of `angles`. */
float RebuildError(const MT_Matrix3x3 &m, const MT_Vector3 &angles)
{
  const MT_Matrix3x3 rebuilt(angles);
  float error = 0.0f;
  for (int row = 0; row < 3; row++) {
    for (int column = 0; column < 3; column++) {
      error = std::max(error, std::fabs(m[row][column] - rebuilt[row][column]));
    }
  }
  return error;
}

bool SameBits(const MT_Vector3 &a, const MT_Vector3 &b)
{
  for (int axis = 0; axis < 3; axis++) {
    const float x = a[axis], y = b[axis];
    if (std::memcmp(&x, &y, sizeof(float)) != 0) {
      return false;
    }
  }
  return true;
}

MT_Vector3 OldStart(const MT_Matrix3x3 &m)
{
  MT_Scalar x, y, z;
  m.getEuler(x, y, z);
  return MT_Vector3(x, y, z);
}

/** `angle` plus the whole turns that bring it nearest `reference`. */
float Near(const float angle, const float reference)
{
  const float turn = float(2.0 * M_PI);
  return angle - turn * std::round((angle - reference) / turn);
}

}  // namespace

/* Typed angles that already are the engine's (Y within +-89, X and Z within +-179): the old
 * start angles, to the bit; typed angles whole turns away from them (370 for 10) too. */
TEST(KX_IpoStartEuler, TypedAnglesTheEngineAlreadyGivesKeepTheOldBits)
{
  size_t checked = 0;
  for (int x = -179; x <= 179; x += 11) {
    for (int z = -179; z <= 179; z += 13) {
      for (int y = -89; y <= 89; y += 7) {
        for (const int whole_turns : {0, 1, -2}) {
          const float typed[3] = {float((x + 360.0 * whole_turns) * kDegree),
                                  float(y * kDegree),
                                  float((z - 360.0 * whole_turns) * kDegree)};
          const MT_Matrix3x3 turn = Turn(x, y, z);
          ASSERT_TRUE(SameBits(KX_IpoStartEuler(turn, typed), OldStart(turn)))
              << x << " " << y << " " << z << " + " << whole_turns << " turns";
          checked++;
        }
      }
    }
  }
  EXPECT_GT(checked, size_t(20000));
}

/* Typed angles that are another set for the object's turn (Y beyond +-90, straight up or down
 * with X and Z): the typed ones, as Blender keeps them; they give the turn. */
TEST(KX_IpoStartEuler, AnObjectTheGameDidNotTurnKeepsItsTypedAngles)
{
  const double cases[][3] = {{0, 180, 0},   {30, 120, 10},  {30, -90, 45},  {-20, 200, 0},
                             {170, 95, -30}, {0, -90, 30},   {45, 90, 45},   {10, -270, 5},
                             {-120, 135, 60}, {90, -90, -90}, {370, 180, -400}};
  for (const auto &c : cases) {
    const float typed[3] = {float(c[0] * kDegree), float(c[1] * kDegree), float(c[2] * kDegree)};
    const MT_Matrix3x3 turn = Turn(c[0], c[1], c[2]);
    const MT_Vector3 start = KX_IpoStartEuler(turn, typed);
    EXPECT_TRUE(SameBits(start, MT_Vector3(typed[0], typed[1], typed[2])))
        << c[0] << " " << c[1] << " " << c[2] << " gave " << start.x() / kDegree << " "
        << start.y() / kDegree << " " << start.z() / kDegree;
    EXPECT_LT(RebuildError(turn, start), 1.0e-5f);
  }
}

/* An object the game turned before the action: the set of angles giving its turn closest to the
 * typed ones, never another turn. A flipped object (typed Y 180) turned 1 degree around Z keeps
 * Y 180; one typed straight down with X 30 and Z 45, then turned 10 degrees around the vertical,
 * keeps the typed Z (X takes the turn). */
TEST(KX_IpoStartEuler, AnObjectTheGameTurnedTakesTheClosestSetGivingItsTurn)
{
  {
    const float typed[3] = {0.0f, float(180.0 * kDegree), 0.0f};
    const MT_Matrix3x3 turn = Turn(0, 0, 1) * Turn(0, 180, 0);
    const MT_Vector3 start = KX_IpoStartEuler(turn, typed);
    EXPECT_LT(RebuildError(turn, start), 1.0e-5f);
    EXPECT_NEAR(start.x() / kDegree, 0.0, 1.0e-3);
    EXPECT_NEAR(start.y() / kDegree, 180.0, 1.0e-3);
    EXPECT_NEAR(start.z() / kDegree, 1.0, 1.0e-3);
  }
  {
    const float typed[3] = {float(30.0 * kDegree), float(-90.0 * kDegree), float(45.0 * kDegree)};
    const MT_Matrix3x3 turn = Turn(0, 0, 10) * Turn(30, -90, 45);
    const MT_Vector3 start = KX_IpoStartEuler(turn, typed);
    EXPECT_LT(RebuildError(turn, start), 1.0e-5f);
    EXPECT_NEAR(start.x() / kDegree, 40.0, 1.0e-3);
    EXPECT_NEAR(start.y() / kDegree, -90.0, 1.0e-3);
    EXPECT_FLOAT_EQ(start.z(), typed[2]);
  }
  {
    /* Typed Y beyond 90 with the engine's Y not 0 (its other set differs from the typed one in
     * all three angles): turned 2 degrees around Z, it keeps X 30 and Y 120. */
    const float typed[3] = {float(30.0 * kDegree), float(120.0 * kDegree), float(10.0 * kDegree)};
    const MT_Matrix3x3 turn = Turn(0, 0, 2) * Turn(30, 120, 10);
    const MT_Vector3 start = KX_IpoStartEuler(turn, typed);
    EXPECT_LT(RebuildError(turn, start), 1.0e-5f);
    EXPECT_NEAR(start.x() / kDegree, 30.0, 1.0e-3);
    EXPECT_NEAR(start.y() / kDegree, 120.0, 1.0e-3);
    EXPECT_NEAR(start.z() / kDegree, 12.0, 1.0e-3);
  }
  /* Every typed rotation, turned or not by the game: angles giving the object's turn; not
   * turned: the typed ones (whole turns apart at most); turned a little (at most 1 degree
   * around each axis) and at least 30 degrees from straight up or down: within 5 degrees of the
   * typed ones (the other set is half a turn away). */
  uint32_t state = 12345;
  const auto next = [&]() {
    state = state * 1664525u + 1013904223u;
    return double(state >> 8) / double(1u << 24);
  };
  for (int k = 0; k < 20000; k++) {
    const double x = next() * 720.0 - 360.0, z = next() * 720.0 - 360.0;
    /* A third of the cases straight up or down (or a hair off). */
    const double y = k % 3 == 0 ? (next() < 0.5 ? -90.0 : 90.0) + (next() - 0.5) * 1.0e-3 :
                                  next() * 720.0 - 360.0;
    const float typed[3] = {float(x * kDegree), float(y * kDegree), float(z * kDegree)};
    const MT_Matrix3x3 authored = Turn(x, y, z);
    const MT_Vector3 start = KX_IpoStartEuler(authored, typed);
    ASSERT_LT(RebuildError(authored, start), 1.0e-4f) << x << " " << y << " " << z;
    for (int axis = 0; axis < 3; axis++) {
      ASSERT_NEAR(Near(start[axis], typed[axis]), typed[axis], 1.0e-4f)
          << x << " " << y << " " << z << " axis " << axis;
    }
    const MT_Matrix3x3 turned = Turn(next() * 40.0 - 20.0, next() * 40.0 - 20.0,
                                     next() * 40.0 - 20.0) *
                                authored;
    ASSERT_LT(RebuildError(turned, KX_IpoStartEuler(turned, typed)), 1.0e-4f)
        << x << " " << y << " " << z << " turned";
    const MT_Matrix3x3 nudged = Turn(next() * 2.0 - 1.0, next() * 2.0 - 1.0,
                                     next() * 2.0 - 1.0) *
                                authored;
    const MT_Vector3 nudged_start = KX_IpoStartEuler(nudged, typed);
    ASSERT_LT(RebuildError(nudged, nudged_start), 1.0e-4f)
        << x << " " << y << " " << z << " nudged";
    if (std::fabs(std::sin(y * kDegree)) <= 0.866) {
      for (int axis = 0; axis < 3; axis++) {
        ASSERT_NEAR(Near(nudged_start[axis], typed[axis]), typed[axis], float(5.0 * kDegree))
            << x << " " << y << " " << z << " nudged, axis " << axis;
      }
    }
  }
}

/* No typed rotation (another rotation mode), or one that is no number: the old start angles. */
TEST(KX_IpoStartEuler, NoTypedAnglesKeepTheOldOnes)
{
  const MT_Matrix3x3 turn = Turn(0, 180, 0);
  EXPECT_TRUE(SameBits(KX_IpoStartEuler(turn, nullptr), OldStart(turn)));
  const float not_a_number[3] = {0.0f, NAN, 0.0f};
  EXPECT_TRUE(SameBits(KX_IpoStartEuler(turn, not_a_number), OldStart(turn)));
}
