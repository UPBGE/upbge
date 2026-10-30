/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include <cfloat>
#include <cmath>

#include "BLI_math_geom.h"
#include "BLI_math_matrix.h"

#include "KX_DrawCulling.h"

namespace {

using namespace KX_DrawCulling;

/** A camera at the origin looking down -Z: 90 degrees wide and high, Clip Start 1, End 100. */
View front_view()
{
  float camera[4][4];
  blender::unit_m4(camera);
  float projection[4][4];
  blender::perspective_m4(projection, -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, 100.0f);
  return MakeView(camera, projection);
}

Light sun(const float x, const float y, const float z)
{
  Light light;
  light.directional = true;
  light.vector[0] = x;
  light.vector[1] = y;
  light.vector[2] = z;
  return light;
}

Light lamp(const float x, const float y, const float z)
{
  Light light;
  light.vector[0] = x;
  light.vector[1] = y;
  light.vector[2] = z;
  return light;
}

TEST(KX_DrawCulling, ViewHoldsTheWholeViewVolume)
{
  const View view = front_view();
  EXPECT_NEAR(view.far_depth, 100.0f, 1e-3f);
  EXPECT_NEAR(view.axis[2], -1.0f, 1e-6f);
  /* The corners' average, and the far corners (100, 100, -100) at the sphere's edge. */
  EXPECT_NEAR(view.center[2], -50.5f, 1e-2f);
  EXPECT_NEAR(view.radius, std::sqrt(100.0f * 100.0f * 2.0f + 49.5f * 49.5f), 1e-1f);
}

TEST(KX_DrawCulling, OnlyWhatIsWhollyBeyondTheFarPlane)
{
  const View view = front_view();
  const float far_away[3] = {0.0f, 0.0f, -120.0f};
  EXPECT_TRUE(BeyondFarPlane(view, far_away, 10.0f));
  /* Its near side crosses the far plane. */
  EXPECT_FALSE(BeyondFarPlane(view, far_away, 25.0f));
  /* Far behind the camera: Clip End says nothing about it. */
  const float behind[3] = {0.0f, 0.0f, 300.0f};
  EXPECT_FALSE(BeyondFarPlane(view, behind, 1.0f));
}

TEST(KX_DrawCulling, MovedAndScaledCameras)
{
  /* Looking down +X from (10, 0, 0), the camera's object scaled by 3 (a camera's scale changes
   * nothing it shows). */
  float camera[4][4];
  blender::unit_m4(camera);
  camera[0][0] = 0.0f;
  camera[0][2] = 3.0f;
  camera[2][0] = -3.0f;
  camera[2][2] = 0.0f;
  camera[1][1] = 3.0f;
  camera[3][0] = 10.0f;
  float projection[4][4];
  blender::perspective_m4(projection, -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, 100.0f);
  const View view = MakeView(camera, projection);
  EXPECT_NEAR(view.axis[0], 1.0f, 1e-6f);
  const float ahead[3] = {125.0f, 0.0f, 0.0f};
  EXPECT_TRUE(BeyondFarPlane(view, ahead, 10.0f));
  const float near_far_plane[3] = {105.0f, 0.0f, 0.0f};
  EXPECT_FALSE(BeyondFarPlane(view, near_far_plane, 10.0f));

  /* Orthographic: the far plane is Clip End too. */
  blender::orthographic_m4(projection, -10.0f, 10.0f, -10.0f, 10.0f, 0.1f, 50.0f);
  const View ortho = MakeView(camera, projection);
  EXPECT_NEAR(ortho.far_depth, 50.0f, 1e-3f);
  const float past_ortho[3] = {70.0f, 0.0f, 0.0f};
  EXPECT_TRUE(BeyondFarPlane(ortho, past_ortho, 5.0f));
}

TEST(KX_DrawCulling, AnOddProjectionCullsNothing)
{
  float camera[4][4];
  blender::unit_m4(camera);
  float projection[4][4] = {};
  const View view = MakeView(camera, projection);
  const float far_away[3] = {0.0f, 0.0f, -1.0e6f};
  EXPECT_FALSE(BeyondFarPlane(view, far_away, 1.0f));
}

TEST(KX_DrawCulling, SunShadowsReachTheViewOnlyTowardIt)
{
  const View view = front_view();
  const float tower[3] = {0.0f, 0.0f, -400.0f};
  /* Light traveling away from the camera: the shadow falls farther away. */
  EXPECT_FALSE(ShadowMayReachView(view, sun(0.0f, 0.0f, -1.0f), tower, 5.0f));
  /* Toward the camera: the shadow comes back into view. */
  EXPECT_TRUE(ShadowMayReachView(view, sun(0.0f, 0.0f, 1.0f), tower, 5.0f));
  /* Sideways, far from the view volume. */
  EXPECT_FALSE(ShadowMayReachView(view, sun(1.0f, 0.0f, 0.0f), tower, 5.0f));
  /* A wide sun (a soft shadow spreading at 45 degrees) is never ruled out. */
  Light soft = sun(1.0f, 0.0f, 0.0f);
  soft.spread = 1.0f;
  EXPECT_TRUE(ShadowMayReachView(view, soft, tower, 5.0f));
  /* Slanted toward the view: the shadow's line passes through the view volume. */
  const float slant = 1.0f / std::sqrt(2.0f);
  const float aside[3] = {-300.0f, 0.0f, -350.0f};
  EXPECT_TRUE(ShadowMayReachView(view, sun(slant, 0.0f, slant), aside, 5.0f));
}

TEST(KX_DrawCulling, LampShadowsPointAwayFromTheLamp)
{
  const View view = front_view();
  const float tower[3] = {0.0f, 0.0f, -300.0f};
  /* Behind the tower: its shadow points at the camera. */
  EXPECT_TRUE(ShadowMayReachView(view, lamp(0.0f, 0.0f, -500.0f), tower, 5.0f));
  /* Between the camera and the tower: the shadow points away. */
  EXPECT_FALSE(ShadowMayReachView(view, lamp(0.0f, 0.0f, -100.0f), tower, 5.0f));
  /* A lamp touching the object lights it from every side. */
  Light big = lamp(0.0f, 0.0f, -310.0f);
  big.spread = 6.0f;
  EXPECT_TRUE(ShadowMayReachView(view, big, tower, 5.0f));
}

TEST(KX_DrawCulling, DrawDistanceWithHysteresis)
{
  const View view = front_view();
  const float far_prop[3] = {0.0f, 0.0f, -60.0f};
  EXPECT_TRUE(BeyondDrawDistance(view, far_prop, 5.0f, 50.0f, false));
  const float edge_prop[3] = {0.0f, 0.0f, -54.0f};
  /* 49 m away: shown while it was shown, still culled while it was culled (inside 95%). */
  EXPECT_FALSE(BeyondDrawDistance(view, edge_prop, 5.0f, 50.0f, false));
  EXPECT_TRUE(BeyondDrawDistance(view, edge_prop, 5.0f, 50.0f, true));
  const float near_prop[3] = {0.0f, 0.0f, -50.0f};
  EXPECT_FALSE(BeyondDrawDistance(view, near_prop, 5.0f, 50.0f, true));
  /* Distance is measured in every direction, behind the camera too. */
  const float behind[3] = {0.0f, 0.0f, 80.0f};
  EXPECT_TRUE(BeyondDrawDistance(view, behind, 5.0f, 50.0f, false));
}

}  // namespace
