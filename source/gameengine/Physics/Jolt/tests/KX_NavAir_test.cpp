/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file KX_NavAir_test.cpp
 *  The air map (navigation phase 8, 2026-11-19): where a flyer fits, searches through it,
 *  water, rebuilt pieces, the same on any thread count. Levels are triangles made here (the
 *  game's space, z up), closed boxes numbered as solid pieces as Jolt's boxes are. */

#include "testing/testing.h"

#include <vector>

#include "KX_NavAir.h"
#include "KX_NavBuild.h"
#include "LN_JobPool.h"

namespace {

struct AirLevel {
  KX_NavGeometry geometry;
  int pieces = 0;

  void Triangle(const MT_Vector3 &a, const MT_Vector3 &b, const MT_Vector3 &c, const int piece)
  {
    const int base = int(geometry.verts.size() / 3);
    for (const MT_Vector3 *v : {&a, &b, &c}) {
      geometry.verts.push_back(float((*v)[0]));
      geometry.verts.push_back(float((*v)[1]));
      geometry.verts.push_back(float((*v)[2]));
    }
    geometry.tris.push_back(base);
    geometry.tris.push_back(base + 2);
    geometry.tris.push_back(base + 1);
    geometry.areas.push_back(0);
    geometry.pieces.push_back(piece);
  }
  void Quad(const MT_Vector3 &a,
            const MT_Vector3 &b,
            const MT_Vector3 &c,
            const MT_Vector3 &d,
            const int piece = -1)
  {
    Triangle(a, b, c, piece);
    Triangle(a, c, d, piece);
  }
  /** A closed box (its six faces, one solid piece). */
  void Box(const MT_Vector3 &l, const MT_Vector3 &h)
  {
    const int piece = pieces++;
    const MT_Vector3 p[8] = {MT_Vector3(l[0], l[1], l[2]),
                             MT_Vector3(h[0], l[1], l[2]),
                             MT_Vector3(h[0], h[1], l[2]),
                             MT_Vector3(l[0], h[1], l[2]),
                             MT_Vector3(l[0], l[1], h[2]),
                             MT_Vector3(h[0], l[1], h[2]),
                             MT_Vector3(h[0], h[1], h[2]),
                             MT_Vector3(l[0], h[1], h[2])};
    Quad(p[4], p[5], p[6], p[7], piece);
    Quad(p[3], p[2], p[1], p[0], piece);
    Quad(p[0], p[1], p[5], p[4], piece);
    Quad(p[1], p[2], p[6], p[5], piece);
    Quad(p[2], p[3], p[7], p[6], piece);
    Quad(p[3], p[0], p[4], p[7], piece);
  }
  /** An open flat floor at height z (a surface, no piece: a terrain's), in 4 m squares. */
  void Floor(const float x0, const float y0, const float x1, const float y1, const float z)
  {
    for (float x = x0; x < x1 - 1e-3f; x += 4.0f) {
      for (float y = y0; y < y1 - 1e-3f; y += 4.0f) {
        Quad(MT_Vector3(x, y, z),
             MT_Vector3(std::min(x + 4.0f, x1), y, z),
             MT_Vector3(std::min(x + 4.0f, x1), std::min(y + 4.0f, y1), z),
             MT_Vector3(x, std::min(y + 4.0f, y1), z));
      }
    }
  }
};

KX_NavAirMap::Settings AirSettings(const float radius = 0.4f, const float voxel = 0.4f)
{
  KX_NavAirMap::Settings s;
  s.radius = radius;
  s.voxel = voxel;
  s.headroom = 10.0f;
  return s;
}

/** Every segment of a path flyable. */
bool PathFree(const KX_NavAirMap &map,
              const MT_Vector3 &from,
              const std::vector<MT_Vector3> &points,
              const bool water_only = false)
{
  MT_Vector3 last = from;
  for (const MT_Vector3 &p : points) {
    if (!map.LineFree(last, p, water_only)) {
      return false;
    }
    last = p;
  }
  return true;
}

}  // namespace

/** No level: open sky, everything free, the way straight. */
TEST(KX_NavAir, NoLevelIsOpenSky)
{
  KX_NavAirMap map(AirSettings());
  map.Build(KX_NavGeometry(), {}, nullptr);
  EXPECT_TRUE(map.OpenSky());
  EXPECT_TRUE(map.Free(MT_Vector3(100, -50, 3)));
  std::vector<MT_Vector3> path;
  bool complete = false;
  ASSERT_TRUE(map.FindPath(MT_Vector3(0, 0, 0), MT_Vector3(10, 5, 2), path, &complete));
  EXPECT_TRUE(complete);
  ASSERT_EQ(path.size(), 1u);
  EXPECT_EQ(path[0], MT_Vector3(10, 5, 2));
}

/** A wall from the floor to well above (a gap at its end): the way goes round through the gap,
 * every leg of it flyable, never through the wall. The wall is solid inside, the air beside it
 * free beyond the flyer's radius. */
TEST(KX_NavAir, AWallIsFlownRound)
{
  AirLevel level;
  level.Box(MT_Vector3(-10, -10, -1), MT_Vector3(10, 10, 0));
  level.Box(MT_Vector3(-0.5f, -10, 0), MT_Vector3(0.5f, 6, 8));
  KX_NavAirMap map(AirSettings());
  map.Build(level.geometry, {}, nullptr);
  ASSERT_FALSE(map.OpenSky());
  EXPECT_FALSE(map.Free(MT_Vector3(0, 0, 4)));
  EXPECT_FALSE(map.Free(MT_Vector3(0.7f, 0, 4)));
  EXPECT_TRUE(map.Free(MT_Vector3(1.6f, 0, 4)));
  EXPECT_FALSE(map.LineFree(MT_Vector3(-3, 0, 4), MT_Vector3(3, 0, 4)));
  const MT_Vector3 from(-3, 0, 4), to(3, 0, 4);
  std::vector<MT_Vector3> path;
  bool complete = false;
  ASSERT_TRUE(map.FindPath(from, to, path, &complete));
  EXPECT_TRUE(complete);
  EXPECT_TRUE(PathFree(map, from, path));
  /* Round the wall's end at y 6 (or over its top at 8, the headroom is 10 m). */
  bool round = false;
  for (const MT_Vector3 &p : path) {
    round = round || p.y() > 6.0f || p.z() > 8.0f;
  }
  EXPECT_TRUE(round);
  EXPECT_LT((path.back() - to).length(), 1e-4f);
}

/** A window 1.6 m square in a wall that fills the box (no way round, the top sealed by a
 * ceiling): a flyer of radius 0.4 goes through it; one of radius 1.0 does not fit (no complete
 * way, the search ends at the wall). */
TEST(KX_NavAir, AWindowIsFlownThroughByWhatFits)
{
  AirLevel level;
  level.Box(MT_Vector3(-10, -6, -1), MT_Vector3(10, 6, 0));
  level.Box(MT_Vector3(-10, -6, 6), MT_Vector3(10, 6, 7));
  level.Box(MT_Vector3(-10, -7, -1), MT_Vector3(10, -6, 7));
  level.Box(MT_Vector3(-10, 6, -1), MT_Vector3(10, 7, 7));
  /* The wall at x 0 with the window y -0.8..0.8, z 2.2..3.8. */
  level.Box(MT_Vector3(-0.3f, -6, 0), MT_Vector3(0.3f, -0.8f, 6));
  level.Box(MT_Vector3(-0.3f, 0.8f, 0), MT_Vector3(0.3f, 6, 6));
  level.Box(MT_Vector3(-0.3f, -0.8f, 0), MT_Vector3(0.3f, 0.8f, 2.2f));
  level.Box(MT_Vector3(-0.3f, -0.8f, 3.8f), MT_Vector3(0.3f, 0.8f, 6));
  for (const float radius : {0.4f, 1.0f}) {
    KX_NavAirMap map(AirSettings(radius, 0.2f));
    map.Build(level.geometry, {}, nullptr);
    const MT_Vector3 from(-6, -3, 2), to(6, 3, 4);
    std::vector<MT_Vector3> path;
    bool complete = false;
    const bool found = map.FindPath(from, to, path, &complete);
    if (radius < 0.5f) {
      ASSERT_TRUE(found);
      EXPECT_TRUE(complete);
      EXPECT_TRUE(PathFree(map, from, path));
      /* It crossed x 0 inside the window. */
      MT_Vector3 last = from;
      bool through = false;
      for (const MT_Vector3 &p : path) {
        if ((last.x() < 0.0f) != (p.x() < 0.0f)) {
          const float t = float(-last.x() / (p.x() - last.x()));
          const MT_Vector3 at = last + (p - last) * t;
          through = std::fabs(float(at.y())) < 0.8f && at.z() > 2.2f && at.z() < 3.8f;
        }
        last = p;
      }
      EXPECT_TRUE(through);
    }
    else {
      EXPECT_FALSE(found && complete) << "a flyer of radius 1 fit a window 1.6 m square";
      for (const MT_Vector3 &p : path) {
        EXPECT_LT(p.x(), 0.0f);
      }
    }
  }
}

/** A closed box is solid inside (not only its shell), and a point buried in the ground comes out
 * on top of it (not into a free space under it). */
TEST(KX_NavAir, ABoxIsSolidAndABuriedPointComesOutOnTop)
{
  AirLevel level;
  level.Box(MT_Vector3(-10, -10, -4), MT_Vector3(10, 10, 0));
  level.Box(MT_Vector3(2, 2, 0), MT_Vector3(8, 8, 6));
  KX_NavAirMap map(AirSettings());
  map.Build(level.geometry, {}, nullptr);
  EXPECT_FALSE(map.Free(MT_Vector3(5, 5, 3)));
  EXPECT_FALSE(map.Free(MT_Vector3(0, 0, -2)));
  MT_Vector3 out;
  ASSERT_TRUE(map.NearestFree(MT_Vector3(-5, -5, -1), 4.0f, out));
  EXPECT_GT(out.z(), 0.3f);
  EXPECT_NEAR(out.x(), -5.0f, 1e-4f);
  ASSERT_TRUE(map.NearestFree(MT_Vector3(5, 5, 5.5f), 4.0f, out));
  EXPECT_TRUE(map.Free(out));
  EXPECT_GT(out.z(), 6.0f);
}

/** A flyer touching a tall wall is within its reach (solids grow by the radius): the nearest
 * free point is beside it, not the wall's top; its way along the wall starts beside it and
 * stays low. Nothing near (beside the map, below its top): straight up out of it. */
TEST(KX_NavAir, AFlyerTouchingAWallStartsBesideIt)
{
  AirLevel level;
  level.Box(MT_Vector3(-20, -20, -1), MT_Vector3(20, 20, 0));
  level.Box(MT_Vector3(-0.5f, -20, 0), MT_Vector3(0.5f, 20, 25));
  KX_NavAirMap map(AirSettings(0.4f, 0.2f));
  map.Build(level.geometry, {}, nullptr);
  const MT_Vector3 touching(0.85f, -6, 3);
  ASSERT_FALSE(map.Free(touching));
  MT_Vector3 out;
  ASSERT_TRUE(map.NearestFree(touching, 2.0f, out, false, 64.0f));
  EXPECT_LT((out - touching).length(), 0.5f);
  std::vector<MT_Vector3> path;
  bool complete = false;
  ASSERT_TRUE(map.FindPath(touching, MT_Vector3(0.85f, 6, 3), path, &complete));
  EXPECT_TRUE(complete);
  for (const MT_Vector3 &p : path) {
    EXPECT_LT(p.z(), 5.0f);
  }
  /* Beside the map: up out of it, past the top. */
  ASSERT_TRUE(map.NearestFree(MT_Vector3(30, 0, 2), 2.0f, out, false, 64.0f));
  EXPECT_GT(out.z(), map.Max().z());
  EXPECT_NEAR(out.x(), 30.0f, 1e-4f);
}

/** A terrain (an open surface, no solid piece) with a ridge: the space under it is sealed off
 * (the map's sides are solid below its top): a way across goes over the ridge, never under the
 * ground. */
TEST(KX_NavAir, UnderAnOpenGroundIsSealed)
{
  AirLevel level;
  level.Floor(-20, -10, -2, 10, 0.0f);
  level.Floor(2, -10, 20, 10, 0.0f);
  /* The ridge: two slopes up to 6 m (open triangles too). */
  level.Quad(MT_Vector3(-2, -10, 0), MT_Vector3(0, -10, 6), MT_Vector3(0, 10, 6), MT_Vector3(-2, 10, 0));
  level.Quad(MT_Vector3(0, -10, 6), MT_Vector3(2, -10, 0), MT_Vector3(2, 10, 0), MT_Vector3(0, 10, 6));
  KX_NavAirMap map(AirSettings());
  map.Build(level.geometry, {}, nullptr);
  const MT_Vector3 from(-10, 0, 1), to(10, 0, 1);
  std::vector<MT_Vector3> path;
  bool complete = false;
  ASSERT_TRUE(map.FindPath(from, to, path, &complete));
  EXPECT_TRUE(complete);
  EXPECT_TRUE(PathFree(map, from, path));
  float highest = -1e9f, lowest = 1e9f;
  for (const MT_Vector3 &p : path) {
    highest = std::max(highest, float(p.z()));
    lowest = std::min(lowest, float(p.z()));
  }
  EXPECT_GT(highest, 6.0f);
  EXPECT_GT(lowest, 0.0f);
}

/** Water: a tank's volume. A swimmer's points are water; its way keeps to the water round a rock
 * standing in it (never over it through the air). */
TEST(KX_NavAir, SwimmersKeepToTheWater)
{
  AirLevel level;
  level.Box(MT_Vector3(-10, -6, -6), MT_Vector3(10, 6, -4));
  /* A rock from the tank's floor to above the water, its middle third. */
  level.Box(MT_Vector3(-1, -6, -4), MT_Vector3(1, 2, 3));
  KX_NavWaterVolume water;
  water.center = MT_Vector3(0, 0, -2);
  water.half = MT_Vector3(10, 6, 2);
  KX_NavAirMap map(AirSettings(0.3f, 0.3f));
  map.Build(level.geometry, {water}, nullptr);
  EXPECT_TRUE(map.HasWater());
  EXPECT_TRUE(map.Free(MT_Vector3(-5, 0, -2), true));
  EXPECT_FALSE(map.Free(MT_Vector3(-5, 0, 1), true));
  EXPECT_TRUE(map.Free(MT_Vector3(-5, 0, 1), false));
  const MT_Vector3 from(-6, -3, -2), to(6, -3, -2);
  std::vector<MT_Vector3> path;
  bool complete = false;
  ASSERT_TRUE(map.FindPath(from, to, path, &complete, true));
  EXPECT_TRUE(complete);
  EXPECT_TRUE(PathFree(map, from, path, true));
  bool round = false;
  for (const MT_Vector3 &p : path) {
    EXPECT_LT(p.z(), 0.0f);
    round = round || p.y() > 2.0f;
  }
  EXPECT_TRUE(round);
}

/** A piece put in the way and taken out again: the bricks round it are built again, the way
 * changes and changes back. */
TEST(KX_NavAir, ARebuiltPlaceChangesTheWay)
{
  AirLevel level;
  level.Box(MT_Vector3(-10, -6, -1), MT_Vector3(10, 6, 0));
  level.Box(MT_Vector3(-10, -6, 6), MT_Vector3(10, 6, 7));
  KX_NavAirMap map(AirSettings());
  map.Build(level.geometry, {}, nullptr);
  const MT_Vector3 from(-6, 0, 3), to(6, 0, 3);
  EXPECT_TRUE(map.LineFree(from, to));
  /* A wall across the middle, a gap at y 4..6. */
  AirLevel changed = level;
  changed.Box(MT_Vector3(-0.5f, -6, 0), MT_Vector3(0.5f, 4, 6));
  map.Rebuild(MT_Vector3(-0.5f, -6, 0), MT_Vector3(0.5f, 4, 6), changed.geometry, nullptr);
  EXPECT_FALSE(map.LineFree(from, to));
  std::vector<MT_Vector3> path;
  bool complete = false;
  ASSERT_TRUE(map.FindPath(from, to, path, &complete));
  EXPECT_TRUE(complete);
  EXPECT_TRUE(PathFree(map, from, path));
  /* Taken out again. */
  map.Rebuild(MT_Vector3(-0.5f, -6, 0), MT_Vector3(0.5f, 4, 6), level.geometry, nullptr);
  EXPECT_TRUE(map.LineFree(from, to));
}

/** Built on 0 and 8 helper threads: the same map (every voxel of a box of points). */
TEST(KX_NavAir, TheMapIsTheSameOnAnyNumberOfThreads)
{
  AirLevel level;
  level.Box(MT_Vector3(-20, -20, -1), MT_Vector3(20, 20, 0));
  for (int i = 0; i < 12; i++) {
    const float x = -16.0f + float(i % 4) * 9.0f, y = -16.0f + float(i / 4) * 11.0f;
    level.Box(MT_Vector3(x, y, 0), MT_Vector3(x + 3.0f, y + 2.0f, 2.0f + float(i % 5)));
  }
  KX_NavAirMap alone(AirSettings()), many(AirSettings());
  LN_JobPool pool;
  pool.SetHelperCount(8);
  alone.Build(level.geometry, {}, nullptr);
  many.Build(level.geometry, {}, &pool);
  EXPECT_EQ(alone.BrickCount(), many.BrickCount());
  for (float x = -19.5f; x < 20.0f; x += 0.7f) {
    for (float y = -19.5f; y < 20.0f; y += 0.7f) {
      for (float z = 0.1f; z < 9.0f; z += 0.9f) {
        ASSERT_EQ(alone.Free(MT_Vector3(x, y, z)), many.Free(MT_Vector3(x, y, z)))
            << x << " " << y << " " << z;
      }
    }
  }
}

/** A sealed room nobody can reach: the search stops at its node budget and gives the nearest it
 * came, not complete. */
TEST(KX_NavAir, AnUnreachableGoalEndsShort)
{
  AirLevel level;
  level.Box(MT_Vector3(-10, -10, -1), MT_Vector3(10, 10, 0));
  /* A closed hut at x 4..8: walls, roof (its inside free air, sealed). */
  level.Box(MT_Vector3(4, -2, 0), MT_Vector3(4.4f, 2, 4));
  level.Box(MT_Vector3(7.6f, -2, 0), MT_Vector3(8, 2, 4));
  level.Box(MT_Vector3(4, -2, 0), MT_Vector3(8, -1.6f, 4));
  level.Box(MT_Vector3(4, 1.6f, 0), MT_Vector3(8, 2, 4));
  level.Box(MT_Vector3(4, -2, 4), MT_Vector3(8, 2, 4.4f));
  KX_NavAirMap map(AirSettings(0.3f, 0.3f));
  map.Build(level.geometry, {}, nullptr);
  ASSERT_TRUE(map.Free(MT_Vector3(6, 0, 2)));
  std::vector<MT_Vector3> path;
  bool complete = true;
  map.FindPath(MT_Vector3(-6, 0, 2), MT_Vector3(6, 0, 2), path, &complete, false, 3000);
  EXPECT_FALSE(complete);
  for (const MT_Vector3 &p : path) {
    EXPECT_FALSE(p.x() > 4.4f && p.x() < 7.6f && std::fabs(float(p.y())) < 1.6f && p.z() < 4.0f);
  }
}


/** Long ways across a city of tall blocks (160 m, 0.5 m voxels: walls far too big for a plain
 * voxel search's budget) are found whole within the engine's node budget (20,000), every
 * segment flyable (a plain voxel search ran out of nodes along the first wall): the brick
 * search crosses the free air in big steps, the fine search only joins its ends. */
TEST(KX_NavAir, LongWaysAcrossACityAreFound)
{
  AirLevel level;
  level.Box(MT_Vector3(-82, -82, -1), MT_Vector3(82, 82, 0));
  uint32_t seed = 12345;
  auto next = [&seed]() {
    seed = seed * 1664525u + 1013904223u;
    return float(seed >> 8) / float(1u << 24);
  };
  /* Blocks of 9 to 13 m on a 16 m grid, moved up to 2 m off it (no street runs straight
   * through), 5 to 40 m tall. */
  for (int i = 0; i < 10; i++) {
    for (int j = 0; j < 10; j++) {
      const float x = -78.0f + float(i) * 16.0f + 2.0f * next(),
                  y = -78.0f + float(j) * 16.0f + 2.0f * next();
      level.Box(MT_Vector3(x, y, 0),
                MT_Vector3(x + 9.0f + 4.0f * next(),
                           y + 9.0f + 4.0f * next(),
                           5.0f + 35.0f * next()));
    }
  }
  KX_NavAirMap map(AirSettings(0.5f, 0.5f));
  map.Build(level.geometry, {}, nullptr);
  /* Street crossings at 1.5 m. */
  auto crossing = [](const int i, const int j) {
    return MT_Vector3(-80.0f + float(i) * 16.0f, -80.0f + float(j) * 16.0f, 1.5f);
  };
  int found = 0;
  for (int k = 0; k < 12; k++) {
    const MT_Vector3 from = crossing(int(next() * 3.0f), int(next() * 11.0f));
    const MT_Vector3 to = crossing(8 + int(next() * 3.0f), int(next() * 11.0f));
    ASSERT_TRUE(map.Free(from));
    ASSERT_TRUE(map.Free(to));
    std::vector<MT_Vector3> path;
    bool complete = false;
    const bool started = map.FindPath(from, to, path, &complete, false, 20000);
    ASSERT_TRUE(started) << k;
    EXPECT_TRUE(complete) << k;
    EXPECT_TRUE(PathFree(map, from, path)) << k;
    ASSERT_FALSE(path.empty());
    EXPECT_LT(float((path.back() - to).length()), 0.01f) << k;
    found += complete ? 1 : 0;
  }
  EXPECT_EQ(found, 12);
}

/* -------------------------------------------------------------------- */
/** \name Walls and ceilings (navigation phase 9, 2026-11-19)
 * \{ */

namespace {

constexpr int kFloorsWalls = KX_NavAirMap::SurfaceFloor | KX_NavAirMap::SurfaceWall;
constexpr int kEverySurface = kFloorsWalls | KX_NavAirMap::SurfaceCeiling;

/** A closed room 12 x 8 x 4 m (inside x -6..6, y -4..4, z 0..4) and a wall across it from wall
 * to wall, 2.5 m high at x 0 (a crawler goes over it, nothing goes round). */
AirLevel RoomWithAWall()
{
  AirLevel level;
  level.Box(MT_Vector3(-7, -5, -1), MT_Vector3(7, 5, 0));
  level.Box(MT_Vector3(-7, -5, 4), MT_Vector3(7, 5, 5));
  level.Box(MT_Vector3(-7, -5, 0), MT_Vector3(-6, 5, 4));
  level.Box(MT_Vector3(6, -5, 0), MT_Vector3(7, 5, 4));
  level.Box(MT_Vector3(-6, -5, 0), MT_Vector3(6, -4, 4));
  level.Box(MT_Vector3(-6, 4, 0), MT_Vector3(6, 5, 4));
  level.Box(MT_Vector3(-0.2f, -4, 0), MT_Vector3(0.2f, 4, 2.5f));
  return level;
}

KX_NavAirMap CrawlerMap(const AirLevel &level, LN_JobPool *pool = nullptr)
{
  KX_NavAirMap map(AirSettings(0.3f, 0.2f));
  map.Build(level.geometry, {}, pool);
  map.EnableSurfaces(level.geometry, 0.7f, pool);
  return map;
}

}  // namespace

/** The shell: floors, walls and ceilings each where they are, nothing in the open air, nothing
 * on the map's own sides (no level there). */
TEST(KX_NavAir, TheShellIsFloorsWallsAndCeilings)
{
  const AirLevel level = RoomWithAWall();
  const KX_NavAirMap map = CrawlerMap(level);
  ASSERT_GT(map.SurfaceBrickCount(), 0u);
  MT_Vector3 p;
  ASSERT_TRUE(map.NearestShell(MT_Vector3(-3, 0, 0.3f), 1.0f, kEverySurface, p));
  EXPECT_EQ(map.SurfaceKindAt(p), KX_NavAirMap::SurfaceFloor);
  EXPECT_LT(p.z(), 1.0f);
  ASSERT_TRUE(map.NearestShell(MT_Vector3(-5.7f, 0, 2), 1.0f, kEverySurface, p));
  EXPECT_EQ(map.SurfaceKindAt(p), KX_NavAirMap::SurfaceWall);
  ASSERT_TRUE(map.NearestShell(MT_Vector3(-3, 0, 3.7f), 1.0f, kEverySurface, p));
  EXPECT_EQ(map.SurfaceKindAt(p), KX_NavAirMap::SurfaceCeiling);
  /* The wall's top is a floor. */
  ASSERT_TRUE(map.NearestShell(MT_Vector3(0, 0, 2.8f), 1.0f, kEverySurface, p));
  EXPECT_EQ(map.SurfaceKindAt(p), KX_NavAirMap::SurfaceFloor);
  EXPECT_GT(p.z(), 2.5f);
  /* The open air in the room: none. Outside the walls, inside them: none. */
  EXPECT_EQ(map.SurfaceKindAt(MT_Vector3(-3, 0, 2)), 0);
  EXPECT_EQ(map.SurfaceKindAt(MT_Vector3(-6.5f, 0, 2)), 0);
  EXPECT_EQ(map.SurfaceKindAt(MT_Vector3(-7.0f + 0.05f, 0, 2)), 0);
  /* Only ceilings asked: the nearest is the ceiling's even from near the floor. */
  ASSERT_TRUE(map.NearestShell(MT_Vector3(-3, 0, 2.5f), 3.0f, KX_NavAirMap::SurfaceCeiling, p));
  EXPECT_GT(p.z(), 3.0f);
}

/** An open floor (the map's sides are its edges): nothing crawls on the map's own sides (no
 * level there), only on the floor. */
TEST(KX_NavAir, NothingCrawlsOnTheMapsSides)
{
  AirLevel level;
  level.Box(MT_Vector3(-5, -5, -1), MT_Vector3(5, 5, 0));
  const KX_NavAirMap map = CrawlerMap(level);
  for (float z = 0.6f; z < 8.0f; z += 0.2f) {
    for (float t = -4.9f; t < 5.0f; t += 0.7f) {
      EXPECT_EQ(map.SurfaceKindAt(MT_Vector3(4.9f, t, z)), 0) << t << " " << z;
      EXPECT_EQ(map.SurfaceKindAt(MT_Vector3(t, -4.9f, z)), 0) << t << " " << z;
    }
  }
  MT_Vector3 p;
  ASSERT_TRUE(map.NearestShell(MT_Vector3(0, 0, 0.3f), 0.5f, kEverySurface, p));
  EXPECT_EQ(map.SurfaceKindAt(p), KX_NavAirMap::SurfaceFloor);
}

/** The way over the wall: up its side, over its top, down the other side (complete, every
 * corner on a floor or a wall); walking floors only, there is none. */
TEST(KX_NavAir, ACrawlersWayGoesOverAWall)
{
  const AirLevel level = RoomWithAWall();
  const KX_NavAirMap map = CrawlerMap(level);
  const MT_Vector3 from(-4, 0, 0.5f), to(4, 0, 0.5f);
  std::vector<MT_Vector3> path;
  bool complete = false;
  ASSERT_TRUE(map.FindSurfacePath(from, to, kFloorsWalls, {}, path, &complete));
  EXPECT_TRUE(complete);
  float top = -1e9f;
  for (const MT_Vector3 &q : path) {
    top = std::max(top, float(q.z()));
    EXPECT_NE(map.SurfaceKindAt(q) & kFloorsWalls, 0) << q.x() << " " << q.y() << " " << q.z();
  }
  EXPECT_GT(top, 2.5f);
  EXPECT_LT(top, 3.5f);
  /* Each leg along the shell (a step to a voxel next to it may cut a corner's voxel: crawlers
   * move round it on the true surface). */
  MT_Vector3 last = path.front();
  for (size_t i = 1; i < path.size(); i++) {
    if (float((path[i] - last).length()) > 0.2f * 1.75f) {
      EXPECT_TRUE(map.SurfaceLineFree(last, path[i], kFloorsWalls)) << i;
    }
    last = path[i];
  }
  path.clear();
  complete = true;
  map.FindSurfacePath(from, to, KX_NavAirMap::SurfaceFloor, {}, path, &complete);
  EXPECT_FALSE(complete);
}

/** A goal in the middle of a ceiling 8 m up (no other surface within the 4 m a goal snaps):
 * reached over a wall and the ceiling when ceilings are allowed; with walls only, no way ends
 * there. */
TEST(KX_NavAir, AGoalOnTheCeilingNeedsCeilings)
{
  AirLevel level;
  level.Box(MT_Vector3(-11, -11, -1), MT_Vector3(11, 11, 0));
  level.Box(MT_Vector3(-11, -11, 8), MT_Vector3(11, 11, 9));
  level.Box(MT_Vector3(-11, -11, 0), MT_Vector3(-10, 11, 8));
  level.Box(MT_Vector3(10, -11, 0), MT_Vector3(11, 11, 8));
  level.Box(MT_Vector3(-10, -11, 0), MT_Vector3(10, -10, 8));
  level.Box(MT_Vector3(-10, 10, 0), MT_Vector3(10, 11, 8));
  const KX_NavAirMap map = CrawlerMap(level);
  const MT_Vector3 from(-5, 0, 0.5f), to(0, 0, 7.6f);
  std::vector<MT_Vector3> path;
  bool complete = false;
  ASSERT_TRUE(map.FindSurfacePath(from, to, kEverySurface, {}, path, &complete));
  EXPECT_TRUE(complete);
  EXPECT_EQ(map.SurfaceKindAt(path.back()), KX_NavAirMap::SurfaceCeiling);
  EXPECT_LT((path.back() - to).length(), 0.5f);
  path.clear();
  map.FindSurfacePath(from, to, kFloorsWalls, {}, path, &complete);
  EXPECT_FALSE(complete);
  for (const MT_Vector3 &q : path) {
    EXPECT_NE(map.SurfaceKindAt(q), KX_NavAirMap::SurfaceCeiling);
  }
}

/** Dearer walls: a crawler goes round a low wall's end on the floor instead of over it; cheap
 * walls: over it. */
TEST(KX_NavAir, WallCostsChooseTheWay)
{
  AirLevel level;
  level.Box(MT_Vector3(-10, -10, -1), MT_Vector3(10, 10, 0));
  /* A wall 2 m high from y -10 to 6 (over it about 11 m, round its end about 17 m). */
  level.Box(MT_Vector3(-0.2f, -10, 0), MT_Vector3(0.2f, 6, 2));
  const KX_NavAirMap map = CrawlerMap(level);
  const MT_Vector3 from(-3, -2, 0.5f), to(3, -2, 0.5f);
  std::vector<MT_Vector3> cheap, dear;
  bool complete = false;
  ASSERT_TRUE(map.FindSurfacePath(from, to, kFloorsWalls, {}, cheap, &complete));
  EXPECT_TRUE(complete);
  KX_NavAirMap::SurfaceCosts costs;
  costs.wall = 6.0f;
  ASSERT_TRUE(map.FindSurfacePath(from, to, kFloorsWalls, costs, dear, &complete));
  EXPECT_TRUE(complete);
  float cheap_top = 0.0f, dear_top = 0.0f, dear_north = -1e9f;
  for (const MT_Vector3 &q : cheap) {
    cheap_top = std::max(cheap_top, float(q.z()));
  }
  for (const MT_Vector3 &q : dear) {
    dear_top = std::max(dear_top, float(q.z()));
    dear_north = std::max(dear_north, float(q.y()));
  }
  EXPECT_GT(cheap_top, 2.0f);
  EXPECT_LT(dear_top, 1.5f);
  EXPECT_GT(dear_north, 6.0f);
}

/** The true surface: above a box's top, straight up; beside its edge, out of the edge
 * (round it); on a wall, out of the wall. */
TEST(KX_NavAir, TheTrueSurfaceRoundsEdges)
{
  const AirLevel level = RoomWithAWall();
  const KX_NavAirMap map = CrawlerMap(level);
  MT_Vector3 point, normal;
  ASSERT_TRUE(map.NearestSurface(MT_Vector3(0, 0, 2.7f), 1.0f, point, normal));
  EXPECT_NEAR(point.z(), 2.5f, 1e-4f);
  EXPECT_NEAR(normal.z(), 1.0f, 1e-4f);
  ASSERT_TRUE(map.NearestSurface(MT_Vector3(0.4f, 0, 2.7f), 1.0f, point, normal));
  EXPECT_NEAR(point.x(), 0.2f, 1e-4f);
  EXPECT_NEAR(point.z(), 2.5f, 1e-4f);
  EXPECT_NEAR(normal.x(), 0.7071f, 1e-3f);
  EXPECT_NEAR(normal.z(), 0.7071f, 1e-3f);
  ASSERT_TRUE(map.NearestSurface(MT_Vector3(0.5f, 1, 1.2f), 1.0f, point, normal));
  EXPECT_NEAR(normal.x(), 1.0f, 1e-4f);
  EXPECT_FALSE(map.NearestSurface(MT_Vector3(-3, 0, 2), 0.5f, point, normal));
}

/** Built on 0 and 8 helper threads: the same shell (every voxel's kind on a grid of points). */
TEST(KX_NavAir, TheShellIsTheSameOnAnyNumberOfThreads)
{
  const AirLevel level = RoomWithAWall();
  LN_JobPool pool;
  pool.SetHelperCount(8);
  const KX_NavAirMap alone = CrawlerMap(level), many = CrawlerMap(level, &pool);
  EXPECT_EQ(alone.SurfaceBrickCount(), many.SurfaceBrickCount());
  for (float x = -6.9f; x < 7.0f; x += 0.13f) {
    for (float y = -4.9f; y < 5.0f; y += 0.37f) {
      for (float z = -0.9f; z < 5.0f; z += 0.11f) {
        ASSERT_EQ(alone.SurfaceKindAt(MT_Vector3(x, y, z)), many.SurfaceKindAt(MT_Vector3(x, y, z)))
            << x << " " << y << " " << z;
      }
    }
  }
}

/** A box set on the floor while playing (a rebuilt place): its sides become walls and its top a
 * floor; taken away again, the floor is back. */
TEST(KX_NavAir, ARebuiltPlaceChangesTheShell)
{
  AirLevel level = RoomWithAWall();
  KX_NavAirMap map = CrawlerMap(level);
  MT_Vector3 p;
  ASSERT_TRUE(map.NearestShell(MT_Vector3(-3, 0, 0.3f), 0.5f, kEverySurface, p));
  EXPECT_EQ(map.SurfaceKindAt(p), KX_NavAirMap::SurfaceFloor);
  const size_t before = level.geometry.tris.size();
  level.Box(MT_Vector3(-3.5f, -0.5f, 0), MT_Vector3(-2.5f, 0.5f, 1.5f));
  map.Rebuild(MT_Vector3(-3.5f, -0.5f, 0), MT_Vector3(-2.5f, 0.5f, 1.5f), level.geometry, nullptr);
  map.SetSurfaceGeometry(level.geometry);
  EXPECT_EQ(map.SurfaceKindAt(MT_Vector3(-3, 0, 0.5f)), 0);
  ASSERT_TRUE(map.NearestShell(MT_Vector3(-3, 0, 1.7f), 0.6f, kEverySurface, p));
  EXPECT_EQ(map.SurfaceKindAt(p), KX_NavAirMap::SurfaceFloor);
  EXPECT_GT(p.z(), 1.5f);
  MT_Vector3 point, normal;
  ASSERT_TRUE(map.NearestSurface(MT_Vector3(-3, 0, 1.8f), 1.0f, point, normal));
  EXPECT_NEAR(point.z(), 1.5f, 1e-4f);
  /* Taken away. */
  level.geometry.verts.resize(level.geometry.verts.size() - (level.geometry.tris.size() - before) * 3);
  level.geometry.tris.resize(before);
  level.geometry.areas.resize(before / 3);
  level.geometry.pieces.resize(before / 3);
  map.Rebuild(MT_Vector3(-3.5f, -0.5f, 0), MT_Vector3(-2.5f, 0.5f, 1.5f), level.geometry, nullptr);
  map.SetSurfaceGeometry(level.geometry);
  ASSERT_TRUE(map.NearestShell(MT_Vector3(-3, 0, 0.3f), 0.5f, kEverySurface, p));
  EXPECT_EQ(map.SurfaceKindAt(p), KX_NavAirMap::SurfaceFloor);
  EXPECT_LT(p.z(), 1.0f);
}

/** \} */
