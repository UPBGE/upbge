/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** NPC navigation (2026-11-18): the Detour navmesh wrapper (KX_NavMesh), ORCA (KX_NavOrca.h),
 * the scene's own navmesh built while playing (KX_NavBuild) and the navigation step
 * (KX_Navigation): paths, avoidance, the hand-over to Characters and bodies, links, areas, shared
 * searches, the same outcome on any number of threads. Objects are the Logic Nodes rigs'
 * PositionedGameObject; bodies and Characters live in a real Jolt world. */

#include "testing/testing.h"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "DNA_scene_types.h"
#include "EXP_StringValue.h"
#include "JoltCharacter.h"
#include "JoltMathUtils.h"
#include "JoltPhysicsController.h"
#include "JoltPhysicsEnvironment.h"
#include "KX_ClientObjectInfo.h"
#include "KX_GameObject.h"
#include "KX_MotionState.h"
#include "KX_NavBuild.h"
#include "KX_NavMesh.h"
#include "KX_NavOrca.h"
#include "KX_Navigation.h"
#include "LN_Columns.h"
#include "LN_JobPool.h"
#include "LN_RuntimeTree.h"
#include "LN_SemRig.h"
#include "LN_TestTrees.h"

using namespace ln_test;
using namespace ln_test::sem;

namespace {

/* -------------------------------------------------------------------- */
/** \name A level: triangles the navmesh is built of
 * \{ */

/** Triangles of the game's space (Z up), counter-clockwise seen from above for floors. */
struct Level {
  struct Box {
    MT_Vector3 min, max;
    int area = 0;
    bool enabled = true;
  };
  /** Floors: (min, max) of flat rectangles at max.z, each with an area. */
  std::vector<Box> floors;
  /** Solid boxes (walls, pillars): their five faces (no bottom). */
  std::vector<Box> boxes;

  void Emit(const MT_Vector3 &a,
            const MT_Vector3 &b,
            const MT_Vector3 &c,
            const int area,
            KX_NavGeometry &g) const
  {
    const int base = int(g.verts.size() / 3);
    for (const MT_Vector3 *v : {&a, &b, &c}) {
      g.verts.push_back(float((*v)[0]));
      g.verts.push_back(float((*v)[1]));
      g.verts.push_back(float((*v)[2]));
    }
    /* KX_NavCollectSceneGeometry's order: the face's points reversed (Recast's handedness). */
    g.tris.push_back(base);
    g.tris.push_back(base + 2);
    g.tris.push_back(base + 1);
    g.areas.push_back(uint8_t(area));
  }
  void Quad(const MT_Vector3 &a,
            const MT_Vector3 &b,
            const MT_Vector3 &c,
            const MT_Vector3 &d,
            const int area,
            KX_NavGeometry &g) const
  {
    Emit(a, b, c, area, g);
    Emit(a, c, d, area, g);
  }
  /** The geometry source: everything (a test level is small). */
  void Collect(const MT_Vector3 & /*min*/, const MT_Vector3 & /*max*/, KX_NavGeometry &g) const
  {
    for (const Box &f : floors) {
      if (!f.enabled) {
        continue;
      }
      const float z = float(f.max[2]);
      /* Split in 4 m squares: Recast rasterizes long thin triangles badly at tile borders. */
      for (float x = float(f.min[0]); x < float(f.max[0]) - 1e-3f; x += 4.0f) {
        for (float y = float(f.min[1]); y < float(f.max[1]) - 1e-3f; y += 4.0f) {
          const float x1 = std::min(x + 4.0f, float(f.max[0]));
          const float y1 = std::min(y + 4.0f, float(f.max[1]));
          Quad(MT_Vector3(x, y, z),
               MT_Vector3(x1, y, z),
               MT_Vector3(x1, y1, z),
               MT_Vector3(x, y1, z),
               f.area,
               g);
        }
      }
    }
    for (const Box &b : boxes) {
      if (!b.enabled) {
        continue;
      }
      const MT_Vector3 &l = b.min, &h = b.max;
      const MT_Vector3 p[8] = {MT_Vector3(l[0], l[1], l[2]),
                               MT_Vector3(h[0], l[1], l[2]),
                               MT_Vector3(h[0], h[1], l[2]),
                               MT_Vector3(l[0], h[1], l[2]),
                               MT_Vector3(l[0], l[1], h[2]),
                               MT_Vector3(h[0], l[1], h[2]),
                               MT_Vector3(h[0], h[1], h[2]),
                               MT_Vector3(l[0], h[1], h[2])};
      Quad(p[4], p[5], p[6], p[7], 0, g); /* top */
      Quad(p[0], p[1], p[5], p[4], 0, g);
      Quad(p[1], p[2], p[6], p[5], 0, g);
      Quad(p[2], p[3], p[7], p[6], 0, g);
      Quad(p[3], p[0], p[4], p[7], 0, g);
    }
  }
};

/** Navigation settings of a test: 0.2 m cells, agents of 0.3 m. */
KX_Navigation::Settings TestSettings(const int build_mode = blender::NAVMESH_BUILD_LEVEL)
{
  KX_Navigation::Settings s;
  s.cell_size = 0.2f;
  s.cell_height = 0.1f;
  s.agent_radius = 0.3f;
  s.agent_height = 1.8f;
  s.agent_climb = 0.4f;
  s.agent_max_slope = 0.785398f;
  s.region_min_size = 2.0f;
  s.region_merge_size = 10.0f;
  s.build_mode = build_mode;
  return s;
}

/** A navigation with `level` as the scene's level. */
std::unique_ptr<KX_Navigation> MakeNavigation(const Level &level,
                                             const KX_Navigation::Settings &settings)
{
  std::unique_ptr<KX_Navigation> navigation = std::make_unique<KX_Navigation>(nullptr);
  navigation->SetSettings(settings);
  navigation->SetGeometrySourceForTests(
      [&level](const MT_Vector3 &min, const MT_Vector3 &max, KX_NavGeometry &g) {
        level.Collect(min, max, g);
      });
  return navigation;
}

/** A 20 x 20 m floor with a wall across its middle (x from -10 to 6: a gap at x 6..10). */
Level WallLevel()
{
  Level level;
  level.floors.push_back({MT_Vector3(-10, -10, 0), MT_Vector3(10, 10, 0)});
  level.boxes.push_back({MT_Vector3(-10, -0.5, 0), MT_Vector3(6, 0.5, 2)});
  return level;
}

float PathLength(const std::vector<MT_Vector3> &corners)
{
  float length = 0.0f;
  for (size_t i = 1; i < corners.size(); i++) {
    length += float((corners[i] - corners[i - 1]).length());
  }
  return length;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Jolt bodies for the objects
 * \{ */

struct Body {
  Body(JoltPhysicsEnvironment &environment,
       KX_GameObject &object,
       const JPH::RefConst<JPH::Shape> &shape,
       const JPH::EMotionType motion)
      : object(object),
        client_info(&object,
                    motion == JPH::EMotionType::Static ? KX_ClientObjectInfo::STATIC :
                                                         KX_ClientObjectInfo::ACTOR)
  {
    const JoltBroadPhaseLayer category = motion == JPH::EMotionType::Static ? JOLT_BP_STATIC :
                                                                              JOLT_BP_DYNAMIC;
    JPH::BodyCreationSettings settings(
        shape,
        JoltMath::ToJoltR(object.NodeGetWorldPosition()),
        JoltMath::ToJolt(object.NodeGetWorldOrientation().getRotation()),
        motion,
        JoltMakeObjectLayer(1, 1, category));
    body_id = environment.GetBodyInterface().CreateAndAddBody(settings,
                                                              JPH::EActivation::Activate);
    controller.SetEnvironment(&environment);
    controller.SetBodyID(body_id);
    controller.SetDynamic(motion == JPH::EMotionType::Dynamic);
    controller.SetKinematic(motion == JPH::EMotionType::Kinematic);
    controller.SetOriginalMotionType(motion);
    controller.SetBroadPhaseCategory(category);
    controller.SetShape(shape, nullptr);
    controller.SetNewClientInfo(&client_info);
    controller.SetCollisionGroup(1);
    object.SetPhysicsController(&controller);
    environment.GetBodyInterface().SetUserData(body_id,
                                               reinterpret_cast<JPH::uint64>(&client_info));
  }
  ~Body()
  {
    object.SetPhysicsController(nullptr);
    controller.SetNewClientInfo(nullptr);
  }
  KX_GameObject &object;
  KX_ClientObjectInfo client_info;
  JoltPhysicsController controller;
  JPH::BodyID body_id;
};

class World {
 public:
  World() : environment(&scene, 1, 512, 512, 512, 8, false)
  {
    /* Down in the game's space (Jolt's own is Y up). */
    environment.GetPhysicsSystem()->SetGravity(JoltMath::ToJolt(MT_Vector3(0.0f, 0.0f, -9.81f)));
  }
  Body &Add(KX_GameObject &object,
            const JPH::RefConst<JPH::Shape> &shape,
            const JPH::EMotionType motion)
  {
    bodies.push_back(std::make_unique<Body>(environment, object, shape, motion));
    if (motion != JPH::EMotionType::Static) {
      /* Its object follows it (the world pose after each step). */
      bodies.back()->controller.SetMotionState(new KX_MotionState(object.GetSGNode()));
      if (motion == JPH::EMotionType::Dynamic) {
        environment.AddController(&bodies.back()->controller);
      }
    }
    return *bodies.back();
  }
  /** A static box (half extents in the game's space). */
  Body &StaticBox(KX_GameObject &object, const MT_Vector3 &half)
  {
    return Add(object,
               new JPH::BoxShape(JPH::Vec3(half.x(), half.z(), half.y()), 0.0f),
               JPH::EMotionType::Static);
  }
  /** A Character: a 1.8 m capsule of 0.3 m radius, its origin in its middle. */
  JoltCharacter *Character(KX_GameObject &object)
  {
    Body &body = Add(object, new JPH::CapsuleShape(0.6f, 0.3f), JPH::EMotionType::Kinematic);
    return environment.CreateCharacter(&body.controller, 0.3f);
  }
  void Step(const std::vector<KX_GameObject *> &moved)
  {
    ASSERT_TRUE(environment.ProceedDeltaTime(0.0, 1.0 / 60.0, 1.0 / 60.0));
    for (KX_GameObject *object : moved) {
      object->NodeUpdateGS(0.0);
    }
  }
  ~World()
  {
    for (std::unique_ptr<Body> &body : bodies) {
      environment.RemoveController(&body->controller);
    }
  }
  blender::Scene scene{};
  JoltPhysicsEnvironment environment;
  std::deque<std::unique_ptr<Body>> bodies;
};

/** Objects that stay where they are put (deque: their addresses hold). */
class Objects {
 public:
  KX_GameObject &Add(const MT_Vector3 &position)
  {
    items.emplace_back(position);
    return items.back().object;
  }
  std::deque<PositionedGameObject> items;
};

KX_NavRequest GoTo(KX_GameObject &object, const MT_Vector3 &destination, const float speed = 3.0f)
{
  KX_NavRequest request;
  request.object = &object;
  request.destination = destination;
  request.speed = speed;
  request.arrive_distance = 0.3f;
  return request;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name The navmesh wrapper
 * \{ */

/** A navmesh object's polygons (the editor bake's data): a 3 x 3 grid of 4 m quads without the
 * middle one, each quad with detail triangles. */
KX_NavMeshPolygons GridWithHole()
{
  KX_NavMeshPolygons p;
  p.nvp = 6;
  /* Vertices 4 x 4, Recast's space (x, up, z). */
  for (int z = 0; z < 4; z++) {
    for (int x = 0; x < 4; x++) {
      p.verts.insert(p.verts.end(), {float(x * 4), 0.0f, float(z * 4)});
    }
  }
  for (int z = 0; z < 3; z++) {
    for (int x = 0; x < 3; x++) {
      if (x == 1 && z == 1) {
        continue;
      }
      const uint16_t a = uint16_t(z * 4 + x), b = uint16_t(a + 1), c = uint16_t(a + 5),
                     d = uint16_t(a + 4);
      /* Recast's polygons wind clockwise seen from above (its +y). */
      std::vector<uint16_t> poly = {a, d, c, b, 0xffff, 0xffff};
      poly.resize(12, 0xffff);
      p.polys.insert(p.polys.end(), poly.begin(), poly.end());
    }
  }
  /* Neighbours as buildMeshAdjacency would set them. */
  const int npolys = int(p.polys.size() / 12);
  for (int i = 0; i < npolys; i++) {
    for (int j = 0; j < 4; j++) {
      const uint16_t va = p.polys[size_t(i) * 12 + j], vb = p.polys[size_t(i) * 12 + (j + 1) % 4];
      for (int k = 0; k < npolys; k++) {
        if (k == i) {
          continue;
        }
        for (int m = 0; m < 4; m++) {
          const uint16_t wa = p.polys[size_t(k) * 12 + m],
                         wb = p.polys[size_t(k) * 12 + (m + 1) % 4];
          if (wa == vb && wb == va) {
            p.polys[size_t(i) * 12 + 6 + j] = uint16_t(k);
          }
        }
      }
    }
  }
  /* Detail: two triangles per quad, no extra vertices. */
  for (int i = 0; i < npolys; i++) {
    p.detail_meshes.insert(p.detail_meshes.end(), {0u, 0u, uint32_t(i * 2), 2u});
    p.detail_tris.insert(p.detail_tris.end(), {0, 1, 2, 0, 2, 3});
  }
  p.walkable_radius = 0.3f;
  p.walkable_height = 1.8f;
  p.walkable_climb = 0.4f;
  return p;
}

TEST(KX_NavMesh, PolygonsOfANavmeshObjectBecomeADetourNavmesh)
{
  std::string error;
  std::unique_ptr<KX_NavMesh> navmesh = KX_NavMesh::FromPolygons(GridWithHole(), error);
  ASSERT_NE(navmesh, nullptr) << error;
  EXPECT_EQ(navmesh->PolygonCount(), 8);
  /* Recast's (x, up, z) is the game's (x, z, y). */
  MT_Vector3 point;
  ASSERT_TRUE(navmesh->NearestPoint(MT_Vector3(2.0f, 2.0f, 0.5f), point));
  EXPECT_NEAR(point.x(), 2.0f, 1e-3f);
  EXPECT_NEAR(point.y(), 2.0f, 1e-3f);
  EXPECT_NEAR(point.z(), 0.0f, 1e-3f);
  /* Far from it: no point (snapping looks 2 m around, 4 up and down). */
  EXPECT_FALSE(navmesh->NearestPoint(MT_Vector3(30.0f, 30.0f, 0.0f), point));

  /* Around the hole: a corner on its edge, longer than straight. */
  std::vector<MT_Vector3> corners;
  bool complete = false;
  ASSERT_TRUE(navmesh->FindPath(MT_Vector3(1, 1, 0), MT_Vector3(11, 11, 0), corners, &complete));
  EXPECT_TRUE(complete);
  ASSERT_GE(corners.size(), 3u);
  EXPECT_GT(PathLength(corners), (MT_Vector3(11, 11, 0) - MT_Vector3(1, 1, 0)).length() + 0.5f);
  for (const MT_Vector3 &c : corners) {
    const bool in_hole = c.x() > 4.01f && c.x() < 7.99f && c.y() > 4.01f && c.y() < 7.99f;
    EXPECT_FALSE(in_hole) << c.x() << " " << c.y();
  }

  /* A straight walk into the hole stops at its edge. */
  float fraction = 0.0f;
  MT_Vector3 hit, normal;
  ASSERT_TRUE(navmesh->Raycast(MT_Vector3(6, 1, 0), MT_Vector3(6, 11, 0), fraction, hit, normal));
  EXPECT_NEAR(hit.y(), 4.0f, 0.02f);
  EXPECT_NEAR(normal.y(), -1.0f, 1e-3f);
  /* Along the side: free. */
  EXPECT_FALSE(navmesh->Raycast(MT_Vector3(1, 1, 0), MT_Vector3(1, 11, 0), fraction, hit, normal));

  MT_Vector3 up;
  ASSERT_TRUE(navmesh->SurfaceNormal(MT_Vector3(2, 2, 0.3f), up));
  EXPECT_NEAR(up.z(), 1.0f, 1e-4f);

  /* Random points: the same seed, the same point; within the radius; reachable. */
  MT_Vector3 r1, r2, r3;
  ASSERT_TRUE(navmesh->RandomPoint(MT_Vector3(2, 2, 0), 3.0f, 42, r1));
  ASSERT_TRUE(navmesh->RandomPoint(MT_Vector3(2, 2, 0), 3.0f, 42, r2));
  ASSERT_TRUE(navmesh->RandomPoint(MT_Vector3(2, 2, 0), 3.0f, 43, r3));
  EXPECT_EQ(r1, r2);
  EXPECT_NE(r1, r3);

  /* The walls: the outer border (12 edges) and the hole's (4). */
  int walls = 0;
  navmesh->ForEachWallEdge([&](const MT_Vector3 &, const MT_Vector3 &) { walls++; });
  EXPECT_EQ(walls, 16);
}

TEST(KX_NavMesh, ThreadsAskAtOnceAndAgree)
{
  std::string error;
  std::unique_ptr<KX_NavMesh> navmesh = KX_NavMesh::FromPolygons(GridWithHole(), error);
  ASSERT_NE(navmesh, nullptr) << error;
  auto ask = [&](const int i, std::vector<MT_Vector3> &corners) {
    const float a = float(i % 11) + 0.5f, b = float((i * 7) % 11) + 0.5f;
    navmesh->FindPath(MT_Vector3(a, 0.5f, 0), MT_Vector3(b, 11.5f, 0), corners);
  };
  std::vector<std::vector<MT_Vector3>> alone(200);
  for (int i = 0; i < 200; i++) {
    ask(i, alone[size_t(i)]);
  }
  std::vector<std::vector<MT_Vector3>> together(200);
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; t++) {
    threads.emplace_back([&, t]() {
      for (int i = t; i < 200; i += 8) {
        ask(i, together[size_t(i)]);
      }
    });
  }
  for (std::thread &thread : threads) {
    thread.join();
  }
  for (int i = 0; i < 200; i++) {
    EXPECT_EQ(alone[size_t(i)], together[size_t(i)]) << i;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name ORCA
 * \{ */

/** Two agents walking into each other: both turn aside, never overlap, both get by. */
TEST(KX_NavOrca, TwoAgentsHeadOnPassEachOther)
{
  using namespace kx_orca;
  /* A little apart sideways: exactly head-on both would wait (the navigation step turns every
   * agent's wish a little for that). */
  Vec2 p[2] = {Vec2(-5.0f, 0.02f), Vec2(5.0f, -0.02f)};
  Vec2 v[2] = {Vec2(), Vec2()};
  const Vec2 goal[2] = {Vec2(5.0f, 0.0f), Vec2(-5.0f, 0.0f)};
  const float radius = 0.4f, speed = 1.5f, dt = 1.0f / 60.0f;
  std::vector<Line> lines, scratch;
  float closest = 100.0f;
  for (int step = 0; step < 900; step++) {
    Vec2 next[2];
    for (int i = 0; i < 2; i++) {
      const int o = 1 - i;
      Neighbour n;
      n.position = p[o];
      n.velocity = v[o];
      n.radius = radius;
      Vec2 to = goal[i] - p[i];
      const float l = length(to);
      const Vec2 preferred = l > speed * dt ? to / l * speed : to / dt;
      next[i] = solve(p[i], v[i], preferred, radius, speed, &n, 1, nullptr, 0, Params{2.0f, 0.5f, dt}, lines, scratch);
    }
    for (int i = 0; i < 2; i++) {
      v[i] = next[i];
      p[i] = p[i] + v[i] * dt;
    }
    closest = std::min(closest, length(p[0] - p[1]));
  }
  EXPECT_GT(closest, 2.0f * radius - 0.01f);
  EXPECT_LT(length(p[0] - goal[0]), 0.1f);
  EXPECT_LT(length(p[1] - goal[1]), 0.1f);
}

/** A wall line: an agent heading into a wall slows to stop at the wall's radius (a navmesh's
 * walls: 0, they are already the agent's radius in). */
TEST(KX_NavOrca, AWallIsNeverCrossed)
{
  using namespace kx_orca;
  Vec2 p(0.0f, 0.0f), v;
  const Wall wall{Vec2(-5.0f, 1.0f), Vec2(5.0f, 1.0f)};
  std::vector<Line> lines, scratch;
  Params params;
  params.wall_radius = 0.4f;
  for (int step = 0; step < 300; step++) {
    v = solve(p, v, Vec2(0.0f, 2.0f), 0.4f, 2.0f, nullptr, 0, &wall, 1, params, lines, scratch);
    p = p + v * (1.0f / 60.0f);
    ASSERT_LT(p.y, 1.0f - 0.4f + 0.02f);
  }
  EXPECT_GT(p.y, 0.5f);
}

/** An agent that won't move (responsibility 1) is walked around by the other alone. */
TEST(KX_NavOrca, ANpcStandingStillIsWalkedAround)
{
  using namespace kx_orca;
  Vec2 p(-4.0f, 0.05f), v;
  Neighbour still;
  still.position = Vec2(0.0f, 0.0f);
  still.radius = 0.4f;
  still.responsibility = 1.0f;
  std::vector<Line> lines, scratch;
  float closest = 100.0f;
  for (int step = 0; step < 600; step++) {
    v = solve(p, v, Vec2(1.5f, 0.0f), 0.4f, 1.5f, &still, 1, nullptr, 0, Params{}, lines, scratch);
    p = p + v * (1.0f / 60.0f);
    closest = std::min(closest, length(p - still.position));
  }
  EXPECT_GT(closest, 0.79f);
  EXPECT_GT(p.x, 3.0f);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name The scene's navmesh built while playing
 * \{ */

TEST(KX_NavBuild, TheWholeLevelIsBuiltAtTheFirstStep)
{
  Level level = WallLevel();
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  navigation->Step(1.0f / 60.0f, nullptr);
  KX_NavMesh *navmesh = navigation->GetSceneNavMesh();
  ASSERT_NE(navmesh, nullptr);
  /* The first agent size's navmesh is made by the first agent: none yet. */
  navigation->Step(1.0f / 60.0f, nullptr);
  ASSERT_GT(navmesh->PolygonCount(), 0);
  /* Across the wall: through its gap (x 6..10), not over it. */
  std::vector<MT_Vector3> corners;
  bool complete = false;
  ASSERT_TRUE(navmesh->FindPath(MT_Vector3(-5, -5, 0), MT_Vector3(-5, 5, 0), corners, &complete));
  EXPECT_TRUE(complete);
  bool through_gap = false;
  for (const MT_Vector3 &c : corners) {
    through_gap = through_gap || c.x() > 5.9f;
  }
  EXPECT_TRUE(through_gap);
  EXPECT_GT(PathLength(corners), 20.0f);
  /* The wall's top is too high to step on: no point there. */
  MT_Vector3 top;
  EXPECT_FALSE(navmesh->NearestPoint(MT_Vector3(-5, 0, 2.2f), top) && top.z() > 1.0f);
}

/** A still obstacle is carved out (paths go around it); moved away, the way is free again. */
TEST(KX_NavBuild, StillObstaclesAreCarvedOut)
{
  Level level;
  level.floors.push_back({MT_Vector3(-10, -10, 0), MT_Vector3(10, 10, 0)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  Objects objects;
  KX_GameObject &crate = objects.Add(MT_Vector3(0, 0, 0.5f));
  navigation->SetObstacle(&crate, true);
  /* A crate in the tests has no mesh: its box is the default obstacle box (radius 1). */
  navigation->Step(1.0f / 60.0f, nullptr);
  KX_NavMesh *navmesh = navigation->GetSceneNavMesh();
  ASSERT_NE(navmesh, nullptr);
  navigation->Step(1.0f / 60.0f, nullptr);
  navigation->Step(1.0f / 60.0f, nullptr);
  float fraction = 0.0f;
  MT_Vector3 hit, normal;
  EXPECT_TRUE(navmesh->Raycast(MT_Vector3(-5, 0, 0), MT_Vector3(5, 0, 0), fraction, hit, normal));
  EXPECT_NEAR(hit.x(), -1.0f - 0.3f, 0.35f);
  /* Taken away: carved no more. */
  navigation->SetObstacle(&crate, false);
  navigation->Step(1.0f / 60.0f, nullptr);
  EXPECT_FALSE(navmesh->Raycast(MT_Vector3(-5, 0, 0), MT_Vector3(5, 0, 0), fraction, hit, normal));
}

/** A level piece added while playing: its tiles are made again, paths see it. */
TEST(KX_NavBuild, ChangedLevelPiecesRebuildTheirTiles)
{
  Level level;
  level.floors.push_back({MT_Vector3(-10, -10, 0), MT_Vector3(10, 10, 0)});
  level.boxes.push_back({MT_Vector3(-10, -0.5, 0), MT_Vector3(10, 0.5, 2)});
  level.boxes.back().enabled = false;
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  navigation->Step(1.0f / 60.0f, nullptr);
  KX_NavMesh *navmesh = navigation->GetSceneNavMesh();
  ASSERT_NE(navmesh, nullptr);
  navigation->Step(1.0f / 60.0f, nullptr);
  std::vector<MT_Vector3> corners;
  bool complete = false;
  ASSERT_TRUE(navmesh->FindPath(MT_Vector3(0, -5, 0), MT_Vector3(0, 5, 0), corners, &complete));
  EXPECT_TRUE(complete);
  const uint64_t stamp = navmesh->GetTileStamp();
  /* The wall appears across the whole floor: the two halves part. */
  level.boxes.back().enabled = true;
  navigation->NoteStaticChanged(MT_Vector3(-10, -0.5, 0), MT_Vector3(10, 0.5, 2));
  navigation->Step(1.0f / 60.0f, nullptr);
  EXPECT_NE(navmesh->GetTileStamp(), stamp);
  ASSERT_TRUE(navmesh->FindPath(MT_Vector3(0, -5, 0), MT_Vector3(0, 5, 0), corners, &complete));
  EXPECT_FALSE(complete);
}

/** Around NPCs: tiles only near the agents; left behind, they go (kept), come back at once. */
TEST(KX_NavBuild, AroundNpcsBuildsOnlyNearTheAgents)
{
  Level level;
  level.floors.push_back({MT_Vector3(-200, -20, 0), MT_Vector3(200, 20, 0)});
  KX_Navigation::Settings settings = TestSettings(blender::NAVMESH_BUILD_AROUND_NPCS);
  settings.build_radius = 15.0f;
  settings.remove_radius = 25.0f;
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, settings);
  Objects objects;
  KX_GameObject &npc = objects.Add(MT_Vector3(-150, 0, 0));
  navigation->Submit(GoTo(npc, MT_Vector3(-150, 0, 0)));
  navigation->Step(1.0f / 60.0f, nullptr);
  KX_NavBuildStats first = KX_NavBuildGetStats(*navigation->GetBuildStateForTests());
  EXPECT_GT(first.tiles_alive, 0u);
  /* 15 m around: at 12.8 m tiles at most 4 x 4 (the world is 400 m long). */
  EXPECT_LE(first.tiles_alive, 16u);
  KX_NavMesh *navmesh = navigation->GetSceneNavMesh();
  MT_Vector3 point;
  EXPECT_TRUE(navmesh->NearestPoint(MT_Vector3(-150, 0, 0), point));
  EXPECT_FALSE(navmesh->NearestPoint(MT_Vector3(100, 0, 0), point));
  /* Teleported far away: its new place is built (a few a step), the old one dropped. */
  npc.NodeSetWorldPosition(MT_Vector3(100, 0, 0));
  npc.NodeUpdateGS(0.0);
  for (int i = 0; i < 4; i++) {
    navigation->Submit(GoTo(npc, MT_Vector3(100, 0, 0)));
    navigation->Step(1.0f / 60.0f, nullptr);
  }
  EXPECT_TRUE(navmesh->NearestPoint(MT_Vector3(100, 0, 0), point));
  EXPECT_FALSE(navmesh->NearestPoint(MT_Vector3(-150, 0, 0), point));
  KX_NavBuildStats second = KX_NavBuildGetStats(*navigation->GetBuildStateForTests());
  EXPECT_GT(second.tiles_removed, 0u);
  /* Back: the kept layers come back without voxelizing. */
  npc.NodeSetWorldPosition(MT_Vector3(-150, 0, 0));
  npc.NodeUpdateGS(0.0);
  navigation->Submit(GoTo(npc, MT_Vector3(-150, 0, 0)));
  navigation->Step(1.0f / 60.0f, nullptr);
  KX_NavBuildStats third = KX_NavBuildGetStats(*navigation->GetBuildStateForTests());
  EXPECT_GT(third.tiles_restored, 0u);
  EXPECT_EQ(third.tiles_built, second.tiles_built);
  EXPECT_TRUE(navmesh->NearestPoint(MT_Vector3(-150, 0, 0), point));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Agents
 * \{ */

/** An NPC without a body slides along the navmesh, around the wall, and stops at its goal. */
TEST(KX_Navigation, AnNpcWalksAroundTheWallToItsGoal)
{
  Level level = WallLevel();
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  Objects objects;
  /* Its origin 1 m above the floor: it keeps that height. */
  KX_GameObject &npc = objects.Add(MT_Vector3(-5, -5, 1));
  const MT_Vector3 goal(-5, 5, 0);
  bool arrived = false;
  float lowest_y_on_wall_side = -1e9f;
  for (int step = 0; step < 60 * 20 && !arrived; step++) {
    navigation->Submit(GoTo(npc, goal));
    navigation->Step(1.0f / 60.0f, nullptr);
    const MT_Vector3 p = npc.NodeGetWorldPosition();
    /* Never through the wall (y -0.5..0.5 for x < 6). */
    if (p.x() < 5.5f) {
      EXPECT_FALSE(std::fabs(p.y()) < 0.6f) << step << ": " << p.x() << " " << p.y();
    }
    lowest_y_on_wall_side = std::max(lowest_y_on_wall_side, float(p.y()));
    KX_NavAgentInfo info;
    ASSERT_TRUE(navigation->GetAgentInfo(&npc, info));
    arrived = info.arrived;
  }
  EXPECT_TRUE(arrived);
  const MT_Vector3 p = npc.NodeGetWorldPosition();
  EXPECT_NEAR(p.x(), goal.x(), 0.35f);
  EXPECT_NEAR(p.y(), goal.y(), 0.35f);
  EXPECT_NEAR(p.z(), 1.0f, 0.05f);
  /* Not asked any more: it stands. */
  navigation->Step(1.0f / 60.0f, nullptr);
  const MT_Vector3 still = npc.NodeGetWorldPosition();
  navigation->Step(1.0f / 60.0f, nullptr);
  EXPECT_EQ(still, npc.NodeGetWorldPosition());
  KX_NavAgentInfo info;
  ASSERT_TRUE(navigation->GetAgentInfo(&npc, info));
  EXPECT_FALSE(info.active);
}

/** The level from a real Jolt world (CollectStaticTriangles, as the game's Build at Game Start
 * reads it): only static solid bodies, in the game's space (Z up, the floor's top at z 0, its top
 * faces counter-clockwise seen from above), only those meeting the box asked for. */
TEST(KX_NavBuild, TheLevelIsReadFromJoltsStaticBodiesInTheGamesSpace)
{
  Objects objects;
  World world;
  world.StaticBox(objects.Add(MT_Vector3(0, 0, -0.5f)), MT_Vector3(10, 10, 0.5f));
  world.StaticBox(objects.Add(MT_Vector3(40, 0, 1)), MT_Vector3(1, 1, 1));
  world.Add(objects.Add(MT_Vector3(0, 0, 3)),
            new JPH::BoxShape(JPH::Vec3(0.5f, 0.5f, 0.5f)),
            JPH::EMotionType::Dynamic);
  world.environment.GetPhysicsSystem()->OptimizeBroadPhase();
  KX_NavGeometry g;
  KX_NavCollectPhysicsGeometry(
      &world.environment,
      MT_Vector3(-20, -20, -5),
      MT_Vector3(20, 20, 5),
      [](const std::string &) { return 0; },
      [](const KX_GameObject *) { return false; },
      g);
  ASSERT_EQ(g.tris.size(), size_t(12 * 3)); /* the floor's box only: 12 triangles */
  int up_faces = 0;
  for (size_t t = 0; t < g.tris.size() / 3; t++) {
    MT_Vector3 v[3];
    /* As handed to Recast (the game's counter-clockwise faces reversed): turn back. */
    const int order[3] = {g.tris[t * 3], g.tris[t * 3 + 2], g.tris[t * 3 + 1]};
    for (int k = 0; k < 3; k++) {
      const float *f = &g.verts[size_t(order[k]) * 3];
      v[k] = MT_Vector3(f[0], f[1], f[2]);
      EXPECT_LE(std::fabs(v[k].x()), 10.0f + 1e-4f);
      EXPECT_LE(std::fabs(v[k].y()), 10.0f + 1e-4f);
      EXPECT_GE(v[k].z(), -1.0f - 1e-4f);
      EXPECT_LE(v[k].z(), 0.0f + 1e-4f);
    }
    const MT_Vector3 normal = (v[1] - v[0]).cross(v[2] - v[0]);
    if (std::fabs(v[0].z()) < 1e-4f && std::fabs(v[1].z()) < 1e-4f && std::fabs(v[2].z()) < 1e-4f)
    {
      EXPECT_GT(normal.z(), 0.0f);
      up_faces++;
    }
  }
  EXPECT_EQ(up_faces, 2);
  /* A static body's bounds in the game's space; a dynamic one has none (no level piece). */
  MT_Vector3 lo, hi;
  ASSERT_TRUE(world.environment.GetStaticBodyBounds(&world.bodies[1]->controller, lo, hi));
  for (int axis = 0; axis < 3; axis++) {
    EXPECT_NEAR(lo[axis], MT_Vector3(39, -1, 0)[axis], 1e-3f) << axis;
    EXPECT_NEAR(hi[axis], MT_Vector3(41, 1, 2)[axis], 1e-3f) << axis;
  }
  EXPECT_FALSE(world.environment.GetStaticBodyBounds(&world.bodies[2]->controller, lo, hi));
  /* A box that meets nothing static: nothing. */
  KX_NavGeometry none;
  KX_NavCollectPhysicsGeometry(
      &world.environment,
      MT_Vector3(-20, 30, -5),
      MT_Vector3(20, 35, 5),
      [](const std::string &) { return 0; },
      [](const KX_GameObject *) { return false; },
      none);
  EXPECT_TRUE(none.tris.empty());
}

/** The scene's navmesh built from a real Jolt world's static floor and wall: an NPC walks round
 * the wall (Build at Game Start's whole path but the scene). */
TEST(KX_Navigation, AnNpcWalksRoundARealJoltWall)
{
  Objects objects;
  World world;
  world.StaticBox(objects.Add(MT_Vector3(0, 0, -0.5f)), MT_Vector3(10, 10, 0.5f));
  world.StaticBox(objects.Add(MT_Vector3(-2, 0, 1)), MT_Vector3(8, 0.5f, 1));
  world.environment.GetPhysicsSystem()->OptimizeBroadPhase();
  std::unique_ptr<KX_Navigation> navigation = std::make_unique<KX_Navigation>(nullptr);
  navigation->SetSettings(TestSettings());
  PHY_IPhysicsEnvironment *physics = &world.environment;
  navigation->SetGeometrySourceForTests(
      [physics](const MT_Vector3 &min, const MT_Vector3 &max, KX_NavGeometry &g) {
        KX_NavCollectPhysicsGeometry(
            physics,
            min,
            max,
            [](const std::string &) { return 0; },
            [](const KX_GameObject *) { return false; },
            g);
      });
  KX_GameObject &npc = objects.Add(MT_Vector3(-5, -5, 0.5f));
  bool arrived = false;
  for (int step = 0; step < 60 * 20 && !arrived; step++) {
    navigation->Submit(GoTo(npc, MT_Vector3(-5, 5, 0)));
    navigation->Step(1.0f / 60.0f, nullptr);
    const MT_Vector3 p = npc.NodeGetWorldPosition();
    if (p.x() < 5.5f) {
      ASSERT_FALSE(std::fabs(p.y()) < 0.6f) << step << ": " << p.x() << " " << p.y();
    }
    KX_NavAgentInfo info;
    ASSERT_TRUE(navigation->GetAgentInfo(&npc, info));
    arrived = info.arrived;
  }
  EXPECT_TRUE(arrived);
  EXPECT_NEAR(npc.NodeGetWorldPosition().y(), 5.0f, 0.35f);
  EXPECT_NEAR(npc.NodeGetWorldPosition().z(), 0.5f, 0.05f);
}

/** A level piece removed while playing (a destroyed gate) opens the way: its tiles are built
 * again (NoteStaticChanged, as the scene calls it for a static object removed or added). */
TEST(KX_Navigation, ARemovedLevelPieceOpensTheWay)
{
  Objects objects;
  World world;
  world.StaticBox(objects.Add(MT_Vector3(0, 0, -0.5f)), MT_Vector3(10, 10, 0.5f));
  /* A wall right across the floor: the far side cannot be reached. */
  Body &gate = world.StaticBox(objects.Add(MT_Vector3(0, 0, 1)), MT_Vector3(10, 0.5f, 1));
  world.environment.GetPhysicsSystem()->OptimizeBroadPhase();
  std::unique_ptr<KX_Navigation> navigation = std::make_unique<KX_Navigation>(nullptr);
  navigation->SetSettings(TestSettings());
  PHY_IPhysicsEnvironment *physics = &world.environment;
  navigation->SetGeometrySourceForTests(
      [physics](const MT_Vector3 &min, const MT_Vector3 &max, KX_NavGeometry &g) {
        KX_NavCollectPhysicsGeometry(
            physics,
            min,
            max,
            [](const std::string &) { return 0; },
            [](const KX_GameObject *) { return false; },
            g);
      });
  KX_GameObject &npc = objects.Add(MT_Vector3(0, -5, 0.5f));
  const MT_Vector3 goal(0, 5, 0);
  for (int step = 0; step < 120; step++) {
    navigation->Submit(GoTo(npc, goal));
    navigation->Step(1.0f / 60.0f, nullptr);
  }
  KX_NavAgentInfo info;
  ASSERT_TRUE(navigation->GetAgentInfo(&npc, info));
  EXPECT_FALSE(info.path_complete);
  EXPECT_LT(npc.NodeGetWorldPosition().y(), -0.5f);
  /* The gate goes. */
  MT_Vector3 lo, hi;
  ASSERT_TRUE(world.environment.GetStaticBodyBounds(&gate.controller, lo, hi));
  world.environment.GetBodyInterface().RemoveBody(gate.body_id);
  navigation->NoteStaticChanged(lo, hi);
  bool arrived = false;
  for (int step = 0; step < 60 * 10 && !arrived; step++) {
    navigation->Submit(GoTo(npc, goal));
    navigation->Step(1.0f / 60.0f, nullptr);
    navigation->GetAgentInfo(&npc, info);
    arrived = info.arrived;
  }
  EXPECT_TRUE(arrived);
  EXPECT_NEAR(npc.NodeGetWorldPosition().y(), 5.0f, 0.35f);
  /* Back in the world for the World's own teardown. */
  world.environment.GetBodyInterface().AddBody(gate.body_id, JPH::EActivation::DontActivate);
}

/** Two crowds crossing: nobody walks through anybody, everybody arrives. */
void RunCrossingCrowds(const uint32_t helpers, std::vector<MT_Vector3> &r_positions, int &r_steps)
{
  Level level;
  level.floors.push_back({MT_Vector3(-20, -20, 0), MT_Vector3(20, 20, 0)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  LN_JobPool pool;
  pool.SetHelperCount(helpers);
  Objects objects;
  std::vector<KX_GameObject *> npcs;
  std::vector<MT_Vector3> goals;
  for (int i = 0; i < 40; i++) {
    const int row = i % 10, side = i / 20, col = (i / 10) % 2;
    const float x = side == 0 ? -12.0f - col * 1.2f : 12.0f + col * 1.2f;
    const float y = -6.0f + row * 1.3f;
    npcs.push_back(&objects.Add(MT_Vector3(x, y, 0)));
    goals.push_back(MT_Vector3(-x, y, 0));
  }
  int steps = 0;
  bool all = false;
  float closest = 100.0f;
  for (; steps < 60 * 40 && !all; steps++) {
    for (size_t i = 0; i < npcs.size(); i++) {
      navigation->Submit(GoTo(*npcs[i], goals[i], 2.0f));
    }
    navigation->Step(1.0f / 60.0f, helpers > 0 ? &pool : nullptr);
    all = true;
    for (size_t i = 0; i < npcs.size(); i++) {
      KX_NavAgentInfo info;
      navigation->GetAgentInfo(npcs[i], info);
      all = all && info.arrived;
      for (size_t j = i + 1; j < npcs.size(); j++) {
        const MT_Vector3 d = npcs[i]->NodeGetWorldPosition() - npcs[j]->NodeGetWorldPosition();
        closest = std::min(closest, float(std::hypot(d.x(), d.y())));
      }
    }
  }
  EXPECT_TRUE(all) << steps;
  /* Two radii of 0.3 m: a little press is allowed, never through. */
  EXPECT_GT(closest, 0.45f);
  r_positions.clear();
  for (KX_GameObject *npc : npcs) {
    r_positions.push_back(npc->NodeGetWorldPosition());
  }
  r_steps = steps;
}

TEST(KX_Navigation, CrowdsCrossWithoutWalkingThroughEachOther)
{
  std::vector<MT_Vector3> positions;
  int steps = 0;
  RunCrossingCrowds(0, positions, steps);
}

/** The same outcome on 0, 3 and 15 helper threads, bit for bit. */
TEST(KX_Navigation, TheSameOnAnyNumberOfThreads)
{
  std::vector<MT_Vector3> alone, few, many;
  int steps_alone = 0, steps_few = 0, steps_many = 0;
  RunCrossingCrowds(0, alone, steps_alone);
  RunCrossingCrowds(3, few, steps_few);
  RunCrossingCrowds(15, many, steps_many);
  EXPECT_EQ(steps_alone, steps_few);
  EXPECT_EQ(steps_alone, steps_many);
  ASSERT_EQ(alone.size(), many.size());
  for (size_t i = 0; i < alone.size(); i++) {
    EXPECT_EQ(alone[i], few[i]) << i;
    EXPECT_EQ(alone[i], many[i]) << i;
  }
}

/** A Character walks (Jolt moves it, colliding), around the wall, to its goal; asked no more, it
 * stops walking. */
TEST(KX_Navigation, ACharacterWalksItsPath)
{
  Level level = WallLevel();
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  /* The objects outlive the world (its bodies let go of them when it goes). */
  Objects objects;
  World world;
  KX_GameObject &floor = objects.Add(MT_Vector3(0, 0, -0.5f));
  world.StaticBox(floor, MT_Vector3(10, 10, 0.5f));
  KX_GameObject &wall = objects.Add(MT_Vector3(-2, 0, 1));
  world.StaticBox(wall, MT_Vector3(8, 0.5f, 1));
  KX_GameObject &hero = objects.Add(MT_Vector3(-5, -5, 0.95f));
  JoltCharacter *character = world.Character(hero);
  ASSERT_NE(character, nullptr);
  navigation->SetPhysicsEnvironmentForTests(&world.environment);
  const MT_Vector3 goal(-5, 5, 0);
  bool arrived = false;
  for (int step = 0; step < 60 * 25 && !arrived; step++) {
    navigation->Submit(GoTo(hero, goal, 4.0f));
    navigation->Step(1.0f / 60.0f, nullptr);
    world.Step({&hero});
    KX_NavAgentInfo info;
    ASSERT_TRUE(navigation->GetAgentInfo(&hero, info));
    arrived = info.arrived;
  }
  EXPECT_TRUE(arrived);
  EXPECT_NEAR(hero.NodeGetWorldPosition().x(), -5.0f, 0.5f);
  EXPECT_NEAR(hero.NodeGetWorldPosition().y(), 5.0f, 0.5f);
  /* Arrived: it stops walking (and stays stopped when not asked). */
  EXPECT_EQ(character->GetWalkDirection(), MT_Vector3(0, 0, 0));
  navigation->Step(1.0f / 60.0f, nullptr);
  EXPECT_EQ(character->GetWalkDirection(), MT_Vector3(0, 0, 0));
}

/** A dynamic body moved as Dynamic: its velocity (its own vertical speed kept). */
TEST(KX_Navigation, ADynamicBodyGetsItsVelocity)
{
  Level level;
  level.floors.push_back({MT_Vector3(-10, -10, 0), MT_Vector3(10, 10, 0)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  /* The objects outlive the world (its bodies let go of them when it goes). */
  Objects objects;
  World world;
  KX_GameObject &floor = objects.Add(MT_Vector3(0, 0, -0.5f));
  world.StaticBox(floor, MT_Vector3(10, 10, 0.5f));
  KX_GameObject &ball = objects.Add(MT_Vector3(-5, 0, 0.3f));
  world.Add(ball, new JPH::BoxShape(JPH::Vec3(0.3f, 0.3f, 0.3f)), JPH::EMotionType::Dynamic);
  navigation->SetPhysicsEnvironmentForTests(&world.environment);
  for (int step = 0; step < 60 * 8; step++) {
    KX_NavRequest request = GoTo(ball, MT_Vector3(5, 0, 0), 2.0f);
    request.move_dynamic = true;
    navigation->Submit(request);
    navigation->Step(1.0f / 60.0f, nullptr);
    world.Step({&ball});
  }
  EXPECT_NEAR(ball.NodeGetWorldPosition().x(), 5.0f, 0.5f);
}

/** A link joins two floors the navmesh can't: the path takes it, the NPC crosses it (the step it
 * starts says so), and arrives. */
TEST(KX_Navigation, NpcsTakeLinks)
{
  Level level;
  level.floors.push_back({MT_Vector3(-10, -10, 0), MT_Vector3(-1, 10, 0)});
  level.floors.push_back({MT_Vector3(1, -10, 1.5f), MT_Vector3(10, 10, 1.5f)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  Objects objects;
  KX_GameObject &link_object = objects.Add(MT_Vector3(-1.5f, 0, 0));
  KX_NavLink link;
  link.object = &link_object;
  link.start = MT_Vector3(-1.5f, 0, 0);
  link.end = MT_Vector3(1.5f, 0, 1.5f);
  link.kind = "jump";
  link.user_id = 1;
  navigation->SetLinksForTests({link});
  KX_GameObject &npc = objects.Add(MT_Vector3(-6, 3, 0));
  bool started = false, arrived = false;
  KX_GameObject *seen = nullptr;
  std::string kind;
  for (int step = 0; step < 60 * 20 && !arrived; step++) {
    navigation->Submit(GoTo(npc, MT_Vector3(6, -3, 1.5f)));
    navigation->Step(1.0f / 60.0f, nullptr);
    KX_NavAgentInfo info;
    ASSERT_TRUE(navigation->GetAgentInfo(&npc, info));
    if (info.link_started) {
      started = true;
      seen = info.link_object;
      kind = info.link_kind;
    }
    arrived = info.arrived;
  }
  EXPECT_TRUE(started);
  EXPECT_EQ(seen, &link_object);
  EXPECT_EQ(kind, "jump");
  EXPECT_TRUE(arrived);
  EXPECT_NEAR(npc.NodeGetWorldPosition().z(), 1.5f, 0.1f);
}

/** A link onto a platform standing on the floor (the floor's edge right below the link's end):
 * the NPC jumps once and stays up (it was put back on the floor's edge and jumped again). */
TEST(KX_Navigation, ALinkOntoAPlatformLandsForGood)
{
  Level level;
  level.floors.push_back({MT_Vector3(-10, -10, 0), MT_Vector3(10, 10, 0)});
  level.boxes.push_back({MT_Vector3(2, -3, 0), MT_Vector3(8, 3, 1.5f)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  Objects objects;
  KX_GameObject &link_object = objects.Add(MT_Vector3(0, 0, 0));
  KX_NavLink link;
  link.object = &link_object;
  link.start = MT_Vector3(0, 0, 0);
  link.end = MT_Vector3(3, 0, 1.5f);
  link.user_id = 1;
  navigation->SetLinksForTests({link});
  KX_GameObject &npc = objects.Add(MT_Vector3(-6, 0, 0.5f));
  int starts = 0;
  bool landed = false, arrived = false;
  for (int step = 0; step < 60 * 20 && !arrived; step++) {
    navigation->Submit(GoTo(npc, MT_Vector3(6, 0, 1.5f)));
    navigation->Step(1.0f / 60.0f, nullptr);
    KX_NavAgentInfo info;
    ASSERT_TRUE(navigation->GetAgentInfo(&npc, info));
    starts += info.link_started ? 1 : 0;
    const MT_Vector3 p = npc.NodeGetWorldPosition();
    if (landed) {
      ASSERT_GT(p.z(), 1.9f) << step << ": back down at " << p.x();
    }
    landed = landed || (!info.on_link && starts > 0 && p.z() > 1.9f);
    arrived = info.arrived;
  }
  EXPECT_EQ(starts, 1);
  EXPECT_TRUE(arrived);
  EXPECT_NEAR(npc.NodeGetWorldPosition().x(), 6.0f, 0.35f);
  EXPECT_NEAR(npc.NodeGetWorldPosition().z(), 2.0f, 0.1f);
}

/* Until Arrived, Stop and When Reached (2026-11-18). */

KX_NavRequest HeldGoTo(KX_GameObject &object, const MT_Vector3 &destination)
{
  KX_NavRequest request = GoTo(object, destination);
  request.until_arrived = true;
  return request;
}

/** One request with Until Arrived is enough: the NPC walks to its goal without being asked
 * again, When Reached pulses once, then it stands there with Arrived on. */
TEST(KX_Navigation, UntilArrivedOneRequestIsEnough)
{
  Level level = WallLevel();
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  Objects objects;
  KX_GameObject &npc = objects.Add(MT_Vector3(-5, -5, 0));
  navigation->Submit(HeldGoTo(npc, MT_Vector3(-5, 5, 0)));
  int reached = 0, arrived_steps = 0;
  for (int step = 0; step < 60 * 20; step++) {
    navigation->Step(1.0f / 60.0f, nullptr);
    KX_NavAgentInfo info;
    ASSERT_TRUE(navigation->GetAgentInfo(&npc, info));
    reached += info.reached ? 1 : 0;
    arrived_steps += info.arrived ? 1 : 0;
  }
  EXPECT_EQ(reached, 1);
  EXPECT_GT(arrived_steps, 60);
  KX_NavAgentInfo info;
  navigation->GetAgentInfo(&npc, info);
  EXPECT_TRUE(info.arrived);
  EXPECT_FALSE(info.moving);
  EXPECT_FALSE(info.active);
  EXPECT_NEAR(npc.NodeGetWorldPosition().x(), -5.0f, 0.35f);
  EXPECT_NEAR(npc.NodeGetWorldPosition().y(), 5.0f, 0.35f);
  KX_NavRequest held;
  EXPECT_FALSE(navigation->GetHeldOrder(&npc, held));
}

/** Stop ends a held order where the NPC is; a new order starts it again and pulses When Reached
 * again (also where it already stands). */
TEST(KX_Navigation, StopEndsAHeldOrder)
{
  Level level;
  level.floors.push_back({MT_Vector3(-10, -10, 0), MT_Vector3(10, 10, 0)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  Objects objects;
  KX_GameObject &npc = objects.Add(MT_Vector3(-8, 0, 0));
  navigation->Submit(HeldGoTo(npc, MT_Vector3(8, 0, 0)));
  for (int step = 0; step < 90; step++) {
    navigation->Step(1.0f / 60.0f, nullptr);
  }
  KX_NavRequest held;
  ASSERT_TRUE(navigation->GetHeldOrder(&npc, held));
  KX_NavRequest stop;
  stop.command = KX_NavRequest::Command::Stop;
  stop.object = &npc;
  navigation->Submit(stop);
  navigation->Step(1.0f / 60.0f, nullptr);
  const MT_Vector3 stopped = npc.NodeGetWorldPosition();
  EXPECT_LT(stopped.x(), 0.0f);
  for (int step = 0; step < 60; step++) {
    navigation->Step(1.0f / 60.0f, nullptr);
  }
  EXPECT_EQ(npc.NodeGetWorldPosition(), stopped);
  EXPECT_FALSE(navigation->GetHeldOrder(&npc, held));
  KX_NavAgentInfo info;
  navigation->GetAgentInfo(&npc, info);
  EXPECT_FALSE(info.active);
  EXPECT_FALSE(info.arrived);
  /* An order to where it stands: arrived at once, When Reached pulses. */
  navigation->Submit(HeldGoTo(npc, stopped));
  navigation->Step(1.0f / 60.0f, nullptr);
  navigation->GetAgentInfo(&npc, info);
  EXPECT_TRUE(info.reached);
  EXPECT_TRUE(info.arrived);
}

/** Asked every step: When Reached pulses once an arrival, again after it walked off (its target
 * moved away) and arrived again. */
TEST(KX_Navigation, WhenReachedPulsesOnceAnArrival)
{
  Level level;
  level.floors.push_back({MT_Vector3(-10, -10, 0), MT_Vector3(10, 10, 0)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  Objects objects;
  KX_GameObject &npc = objects.Add(MT_Vector3(-6, 0, 0));
  KX_GameObject &leader = objects.Add(MT_Vector3(-2, 0, 0));
  std::vector<int> pulses;
  for (int step = 0; step < 60 * 12; step++) {
    if (step == 60 * 5) {
      leader.NodeSetWorldPosition(MT_Vector3(6, 0, 0));
      leader.NodeUpdateGS(0.0);
    }
    KX_NavRequest request = GoTo(npc, MT_Vector3(0, 0, 0));
    request.target = &leader;
    request.arrive_distance = 1.0f;
    navigation->Submit(request);
    navigation->Step(1.0f / 60.0f, nullptr);
    KX_NavAgentInfo info;
    navigation->GetAgentInfo(&npc, info);
    if (info.reached) {
      pulses.push_back(step);
    }
  }
  ASSERT_EQ(pulses.size(), 2u);
  EXPECT_LT(pulses[0], 60 * 5);
  EXPECT_GT(pulses[1], 60 * 5);
}

/** Stopped (or no longer asked) while crossing a link, the NPC is carried to the link's end
 * first: it never hangs in the air. */
TEST(KX_Navigation, AStopOnALinkFinishesTheLink)
{
  for (const bool held : {true, false}) {
    SCOPED_TRACE(held ? "a held order stopped" : "no longer asked");
    Level level;
    level.floors.push_back({MT_Vector3(-10, -10, 0), MT_Vector3(10, 10, 0)});
    level.boxes.push_back({MT_Vector3(2, -3, 0), MT_Vector3(8, 3, 1.5f)});
    std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
    Objects objects;
    KX_GameObject &link_object = objects.Add(MT_Vector3(0, 0, 0));
    KX_NavLink link;
    link.object = &link_object;
    link.start = MT_Vector3(0, 0, 0);
    link.end = MT_Vector3(3, 0, 1.5f);
    link.user_id = 1;
    navigation->SetLinksForTests({link});
    KX_GameObject &npc = objects.Add(MT_Vector3(-6, 0, 0.5f));
    if (held) {
      navigation->Submit(HeldGoTo(npc, MT_Vector3(6, 0, 1.5f)));
    }
    bool stopped = false;
    for (int step = 0; step < 60 * 10; step++) {
      KX_NavAgentInfo info;
      navigation->GetAgentInfo(&npc, info);
      if (!stopped && info.on_link && !info.link_started) {
        stopped = true;
        if (held) {
          KX_NavRequest stop;
          stop.command = KX_NavRequest::Command::Stop;
          stop.object = &npc;
          navigation->Submit(stop);
        }
      }
      if (!held && !stopped) {
        navigation->Submit(GoTo(npc, MT_Vector3(6, 0, 1.5f)));
      }
      navigation->Step(1.0f / 60.0f, nullptr);
    }
    ASSERT_TRUE(stopped);
    /* At the link's end, on the platform, standing. */
    EXPECT_NEAR(npc.NodeGetWorldPosition().x(), 3.0f, 0.3f);
    EXPECT_NEAR(npc.NodeGetWorldPosition().z(), 2.0f, 0.1f);
    KX_NavAgentInfo info;
    navigation->GetAgentInfo(&npc, info);
    EXPECT_FALSE(info.active);
    EXPECT_FALSE(info.on_link);
  }
}

/** Save Game's API: a held order read back and given again (Load Game) goes on from where the NPC
 * now stands; none given ends it. A held order after an object that is removed ends. */
TEST(KX_Navigation, AHeldOrderIsReadAndRestored)
{
  Level level;
  level.floors.push_back({MT_Vector3(-10, -10, 0), MT_Vector3(10, 10, 0)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  Objects objects;
  KX_GameObject &npc = objects.Add(MT_Vector3(-8, 0, 0));
  navigation->Submit(HeldGoTo(npc, MT_Vector3(8, 0, 0)));
  for (int step = 0; step < 60; step++) {
    navigation->Step(1.0f / 60.0f, nullptr);
  }
  KX_NavRequest saved;
  ASSERT_TRUE(navigation->GetHeldOrder(&npc, saved));
  EXPECT_TRUE(saved.until_arrived);
  EXPECT_EQ(saved.destination, MT_Vector3(8, 0, 0));
  /* "Load": back where it was saved... a new navigation, the order given again. */
  std::unique_ptr<KX_Navigation> loaded = MakeNavigation(level, TestSettings());
  npc.NodeSetWorldPosition(MT_Vector3(-8, 0, 0));
  npc.NodeUpdateGS(0.0);
  loaded->RestoreHeldOrder(&npc, &saved);
  int reached = 0;
  for (int step = 0; step < 60 * 10; step++) {
    loaded->Step(1.0f / 60.0f, nullptr);
    KX_NavAgentInfo info;
    loaded->GetAgentInfo(&npc, info);
    reached += info.reached ? 1 : 0;
  }
  EXPECT_EQ(reached, 1);
  EXPECT_NEAR(npc.NodeGetWorldPosition().x(), 8.0f, 0.35f);
  /* None given: the held order ends. */
  loaded->Submit(HeldGoTo(npc, MT_Vector3(-8, 0, 0)));
  loaded->Step(1.0f / 60.0f, nullptr);
  loaded->RestoreHeldOrder(&npc, nullptr);
  const MT_Vector3 here = npc.NodeGetWorldPosition();
  for (int step = 0; step < 30; step++) {
    loaded->Step(1.0f / 60.0f, nullptr);
  }
  EXPECT_LT((npc.NodeGetWorldPosition() - here).length(), 0.1f);
  /* After an object that goes: the order ends (no walk to the Destination field). */
  KX_GameObject &leader = objects.Add(MT_Vector3(6, 6, 0));
  KX_NavRequest follow = HeldGoTo(npc, MT_Vector3(0, 0, 0));
  follow.target = &leader;
  loaded->Submit(follow);
  for (int step = 0; step < 30; step++) {
    loaded->Step(1.0f / 60.0f, nullptr);
  }
  loaded->ForgetObject(&leader);
  loaded->Step(1.0f / 60.0f, nullptr);
  const MT_Vector3 left = npc.NodeGetWorldPosition();
  for (int step = 0; step < 30; step++) {
    loaded->Step(1.0f / 60.0f, nullptr);
  }
  EXPECT_LT((npc.NodeGetWorldPosition() - left).length(), 0.1f);
  KX_NavRequest none;
  EXPECT_FALSE(loaded->GetHeldOrder(&npc, none));
}

/** Areas: a costly area is walked around, a closed one never crossed. */
TEST(KX_Navigation, AreasCostAndClose)
{
  Level level;
  level.floors.push_back({MT_Vector3(-10, -10, 0), MT_Vector3(10, -2, 0)});
  level.floors.push_back({MT_Vector3(-10, 2, 0), MT_Vector3(10, 10, 0)});
  /* Water across the middle, ground only at its east end (x 6..10). */
  level.floors.push_back({MT_Vector3(-10, -2, 0), MT_Vector3(6, 2, 0)});
  level.floors.back().area = 0;
  level.floors.push_back({MT_Vector3(6, -2, 0), MT_Vector3(10, 2, 0)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  const int water = navigation->AreaId("water");
  EXPECT_EQ(water, 1);
  level.floors[2].area = water;
  Objects objects;
  KX_GameObject &swimmer = objects.Add(MT_Vector3(-6, -6, 0));
  KX_GameObject &walker = objects.Add(MT_Vector3(-6, -6.5f, 0));
  navigation->SetAreaCost(&walker, "water", 50.0f, false);
  auto widest_x = [&](KX_GameObject &npc) {
    float x = -1e9f;
    for (int step = 0; step < 60 * 20; step++) {
      KX_NavRequest request = GoTo(npc, MT_Vector3(-6, 6, 0), 3.0f);
      request.avoid = false;
      navigation->Submit(request);
      navigation->Step(1.0f / 60.0f, nullptr);
      x = std::max(x, float(npc.NodeGetWorldPosition().x()));
      KX_NavAgentInfo info;
      navigation->GetAgentInfo(&npc, info);
      if (info.arrived) {
        break;
      }
    }
    return x;
  };
  /* Water is ground to the swimmer: straight across. */
  EXPECT_LT(widest_x(swimmer), -3.0f);
  /* Fifty times dearer to the walker: round by the east end. */
  EXPECT_GT(widest_x(walker), 5.0f);
  /* Closed: the corridor only (it is there; open the east end... it is ground). */
  navigation->SetAreaCost(&swimmer, "water", 1.0f, true);
  swimmer.NodeSetWorldPosition(MT_Vector3(-6, -6, 0));
  swimmer.NodeUpdateGS(0.0);
  EXPECT_GT(widest_x(swimmer), 5.0f);
}

/** An area whose surface lies on the ground (a pond's thin slab on the floor: Recast merges the
 * two into one span, the higher area kept) stays the named area: closed, it is walked round; to
 * an NPC for whom it is open, straight across. */
TEST(KX_Navigation, AnAreaLyingOnTheGroundKeepsItsName)
{
  Level level;
  level.floors.push_back({MT_Vector3(-10, -10, 0), MT_Vector3(10, 10, 0)});
  /* The pond: x -2..2, y -6..10, 0.1 above the floor (the gap south of it is ground). */
  level.floors.push_back({MT_Vector3(-2, -6, 0.1f), MT_Vector3(2, 10, 0.1f)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  level.floors[1].area = navigation->AreaId("water");
  Objects objects;
  KX_GameObject &wader = objects.Add(MT_Vector3(-6, 2, 0));
  KX_GameObject &swimmer = objects.Add(MT_Vector3(-6, 4, 0));
  navigation->SetAreaCost(&wader, "water", 1.0f, true);
  float wader_low = 1e9f, swimmer_off = 0.0f;
  bool wader_wet = false;
  for (int step = 0; step < 60 * 20; step++) {
    KX_NavRequest a = GoTo(wader, MT_Vector3(6, 2, 0), 3.0f);
    KX_NavRequest b = GoTo(swimmer, MT_Vector3(6, 4, 0), 3.0f);
    a.avoid = b.avoid = false;
    navigation->Submit(a);
    navigation->Submit(b);
    navigation->Step(1.0f / 60.0f, nullptr);
    const MT_Vector3 p = wader.NodeGetWorldPosition();
    wader_low = std::min(wader_low, float(p.y()));
    wader_wet = wader_wet || (std::fabs(p.x()) < 1.9f && p.y() > -5.9f);
    swimmer_off = std::max(swimmer_off, float(std::fabs(swimmer.NodeGetWorldPosition().y() - 4)));
  }
  EXPECT_FALSE(wader_wet);
  EXPECT_LT(wader_low, -6.0f);
  EXPECT_NEAR(wader.NodeGetWorldPosition().x(), 6.0f, 0.35f);
  EXPECT_LT(swimmer_off, 0.3f);
  EXPECT_NEAR(swimmer.NodeGetWorldPosition().x(), 6.0f, 0.35f);
}

/** The same with the game's geometry: a pond box (its top, sides and bottom) lying on a Jolt
 * floor, its area the object's nav_area game property. */
TEST(KX_Navigation, APondBoxOnAJoltFloorKeepsItsArea)
{
  Objects objects;
  World world;
  world.StaticBox(objects.Add(MT_Vector3(0, 0, -0.5f)), MT_Vector3(10, 10, 0.5f));
  KX_GameObject &pond = objects.Add(MT_Vector3(0, 2, 0.05f));
  world.StaticBox(pond, MT_Vector3(2, 8, 0.05f));
  pond.SetProperty("nav_area", new EXP_StringValue("water", "nav_area"));
  world.environment.GetPhysicsSystem()->OptimizeBroadPhase();
  std::unique_ptr<KX_Navigation> navigation = std::make_unique<KX_Navigation>(nullptr);
  navigation->SetSettings(TestSettings());
  PHY_IPhysicsEnvironment *physics = &world.environment;
  KX_Navigation *nav = navigation.get();
  navigation->SetGeometrySourceForTests(
      [physics, nav](const MT_Vector3 &min, const MT_Vector3 &max, KX_NavGeometry &g) {
        KX_NavCollectPhysicsGeometry(
            physics,
            min,
            max,
            [nav](const std::string &name) { return nav->AreaId(name); },
            [](const KX_GameObject *) { return false; },
            g);
      });
  KX_GameObject &wader = objects.Add(MT_Vector3(-6, 2, 0.5f));
  navigation->SetAreaCost(&wader, "water", 1.0f, true);
  bool wet = false;
  float low = 1e9f;
  for (int step = 0; step < 60 * 20; step++) {
    KX_NavRequest a = GoTo(wader, MT_Vector3(6, 2, 0), 3.0f);
    a.avoid = false;
    navigation->Submit(a);
    navigation->Step(1.0f / 60.0f, nullptr);
    const MT_Vector3 p = wader.NodeGetWorldPosition();
    low = std::min(low, float(p.y()));
    wet = wet || (std::fabs(p.x()) < 1.9f && p.y() > -5.9f);
  }
  EXPECT_FALSE(wet);
  EXPECT_LT(low, -6.0f);
  EXPECT_NEAR(wader.NodeGetWorldPosition().x(), 6.0f, 0.35f);
}

/** Many NPCs after one goal: one search from the goal serves them; all get there. */
TEST(KX_Navigation, AHordeSharesOneSearch)
{
  Level level = WallLevel();
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  Objects objects;
  std::vector<KX_GameObject *> horde;
  for (int i = 0; i < 24; i++) {
    horde.push_back(&objects.Add(MT_Vector3(-8.0f + (i % 8) * 1.5f, -3.0f - (i / 8) * 1.5f, 0)));
  }
  const MT_Vector3 player(-5, 6, 0);
  int arrived = 0;
  for (int step = 0; step < 60 * 30 && arrived < int(horde.size()); step++) {
    for (KX_GameObject *npc : horde) {
      KX_NavRequest request = GoTo(*npc, player, 3.0f);
      request.arrive_distance = 3.0f;
      navigation->Submit(request);
    }
    navigation->Step(1.0f / 60.0f, nullptr);
    if (step == 0) {
      EXPECT_GE(navigation->GetLastSharedGroupCount(), 1);
    }
    arrived = 0;
    for (KX_GameObject *npc : horde) {
      KX_NavAgentInfo info;
      navigation->GetAgentInfo(npc, info);
      arrived += info.arrived ? 1 : 0;
    }
  }
  EXPECT_GE(navigation->GetSharedSearchCount(), 1u);
  EXPECT_EQ(arrived, int(horde.size()));
}

/** Steering modes: Wander stays near home, Flee gets away, Formation keeps its place. */
TEST(KX_Navigation, SteeringModes)
{
  Level level;
  level.floors.push_back({MT_Vector3(-20, -20, 0), MT_Vector3(20, 20, 0)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  Objects objects;
  KX_GameObject &wanderer = objects.Add(MT_Vector3(0, 0, 0));
  KX_GameObject &coward = objects.Add(MT_Vector3(3, 0, 0));
  KX_GameObject &threat = objects.Add(MT_Vector3(1, 0, 0));
  KX_GameObject &leader = objects.Add(MT_Vector3(-10, -10, 0));
  KX_GameObject &soldier = objects.Add(MT_Vector3(-15, -15, 0));
  std::set<std::pair<int, int>> cells;
  for (int step = 0; step < 60 * 30; step++) {
    KX_NavRequest wander = GoTo(wanderer, MT_Vector3(0, 0, 0), 3.0f);
    wander.mode = KX_NavRequest::Mode::Wander;
    wander.radius = 6.0f;
    wander.seed = 7;
    navigation->Submit(wander);
    KX_NavRequest flee = GoTo(coward, MT_Vector3(0, 0, 0), 3.0f);
    flee.mode = KX_NavRequest::Mode::Flee;
    flee.target = &threat;
    flee.radius = 10.0f;
    navigation->Submit(flee);
    KX_NavRequest place = GoTo(soldier, MT_Vector3(0, 0, 0), 3.0f);
    place.mode = KX_NavRequest::Mode::Formation;
    place.target = &leader;
    place.offset = MT_Vector3(-2, -1, 0);
    navigation->Submit(place);
    navigation->Step(1.0f / 60.0f, nullptr);
    const MT_Vector3 p = wanderer.NodeGetWorldPosition();
    EXPECT_LT(std::hypot(p.x(), p.y()), 6.5f);
    cells.insert({int(std::floor(p.x())), int(std::floor(p.y()))});
  }
  /* It goes places. */
  EXPECT_GT(cells.size(), 10u);
  EXPECT_GE((coward.NodeGetWorldPosition() - threat.NodeGetWorldPosition()).length(), 9.5f);
  EXPECT_LT((soldier.NodeGetWorldPosition() - MT_Vector3(-12, -11, 0)).length(), 0.5f);
}

/** Far from the camera (the crowd's detail distance) agents avoid others only every 4th step: in
 * between they walk as they want to. */
TEST(KX_Navigation, FarAgentsAvoidLessOften)
{
  Level level;
  level.floors.push_back({MT_Vector3(-20, -20, 0), MT_Vector3(20, 20, 0)});
  KX_Navigation::Settings settings = TestSettings();
  settings.detail_distance = 10.0f;
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, settings);
  navigation->SetCameraForTests(MT_Vector3(100, 100, 0));
  Objects objects;
  KX_GameObject &a = objects.Add(MT_Vector3(-5, 0.1f, 0));
  KX_GameObject &b = objects.Add(MT_Vector3(5, 0, 0));
  int steps_as_wanted = 0, steps_moving = 0;
  for (int step = 0; step < 120; step++) {
    navigation->Submit(GoTo(a, MT_Vector3(5, 0, 0), 2.0f));
    navigation->Submit(GoTo(b, MT_Vector3(-5, 0, 0), 2.0f));
    navigation->Step(1.0f / 60.0f, nullptr);
    KX_NavAgentInfo info;
    navigation->GetAgentInfo(&a, info);
    if (info.moving) {
      steps_moving++;
      steps_as_wanted += (info.velocity - info.desired_velocity).length() < 1e-5f ? 1 : 0;
    }
  }
  EXPECT_GT(steps_moving, 60);
  /* Three of four steps take the wanted velocity as it is. */
  EXPECT_GE(steps_as_wanted, steps_moving * 2 / 3);
}

/** Benchmark (2026-11-18): the navigation step for a crowd, on 1 thread and with helpers.
 * A 200 x 200 m floor with a grid of pillars, NAV_BENCH_AGENTS NPCs (default 1,000) in two
 * groups crossing to the other side (half a horde after one goal, half each to its own), with
 * avoidance; the median of NAV_BENCH_ROUNDS (5) rounds of 300 steps after 60 of warm-up. Also
 * the start-up build of the whole level. Run with --gtest_also_run_disabled_tests. */
TEST(KX_Navigation, DISABLED_CrowdCost)
{
  const char *agents_text = getenv("NAV_BENCH_AGENTS");
  const char *rounds_text = getenv("NAV_BENCH_ROUNDS");
  const int count = agents_text ? std::max(1, atoi(agents_text)) : 1000;
  const int rounds = rounds_text ? std::max(1, atoi(rounds_text)) : 5;
  Level level;
  level.floors.push_back({MT_Vector3(-100, -100, 0), MT_Vector3(100, 100, 0)});
  for (int x = -80; x <= 80; x += 20) {
    for (int y = -60; y <= 60; y += 20) {
      level.boxes.push_back({MT_Vector3(x - 2, y - 2, 0), MT_Vector3(x + 2, y + 2, 3)});
    }
  }
  for (const uint32_t helpers : {0u, 7u, 15u}) {
    std::vector<double> step_ms, build_ms;
    for (int round = 0; round < rounds; round++) {
      std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
      LN_JobPool pool;
      pool.SetHelperCount(helpers);
      LN_JobPool *jobs = helpers > 0 ? &pool : nullptr;
      Objects objects;
      std::vector<KX_GameObject *> npcs;
      std::vector<MT_Vector3> goals;
      const int side = int(std::ceil(std::sqrt(double(count))));
      for (int i = 0; i < count; i++) {
        const float x = -90.0f + 180.0f * float(i % side) / float(side);
        const float y = -95.0f + 30.0f * float(i / side) / float(side);
        npcs.push_back(&objects.Add(MT_Vector3(x, y, 0)));
        goals.push_back(i % 2 == 0 ? MT_Vector3(0, 90, 0) : MT_Vector3(-x, 90 - (y + 95), 0));
      }
      auto step = [&]() {
        for (size_t i = 0; i < npcs.size(); i++) {
          navigation->Submit(GoTo(*npcs[i], goals[i], 3.0f));
        }
        navigation->Step(1.0f / 60.0f, jobs);
      };
      const auto t0 = std::chrono::steady_clock::now();
      step(); /* the whole level is built at the first step */
      build_ms.push_back(
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
      for (int s = 0; s < 60; s++) {
        step();
      }
      const auto t1 = std::chrono::steady_clock::now();
      for (int s = 0; s < 300; s++) {
        step();
      }
      step_ms.push_back(
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count() /
          300.0);
    }
    std::sort(step_ms.begin(), step_ms.end());
    std::sort(build_ms.begin(), build_ms.end());
    printf("navigation bench: %d NPCs, %u helpers: %.3f ms a step (median of %d), first step "
           "(the level's build + the searches) %.1f ms\n",
           count,
           helpers,
           step_ms[step_ms.size() / 2],
           rounds,
           build_ms[build_ms.size() / 2]);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name The Logic Nodes: Navigate, Follow Path, Get Navigation Info, On Navigation Link,
 * Navmesh Query, Set Navigation Attribute in the rig, the navigation step after each tick (as
 * the manager runs it after the flush).
 * \{ */

/** The trees' navigation for the test's life. */
struct NavigationSeam {
  explicit NavigationSeam(KX_Navigation *navigation)
  {
    LN_SetNavigationForTests(navigation);
  }
  ~NavigationSeam()
  {
    LN_SetNavigationForTests(nullptr);
  }
};

/** A tree of the rig: its nodes after an On Logic + Physics Step (every `every` steps), outputs
 * stored into the owner's game properties one after the other. */
class NavTree {
 public:
  explicit NavTree(const int every = 1)
  {
    step = &b.Add("LogicNativeOnFixedUpdate", int16_t(every));
    flow = step;
    flow_pin = "Out";
  }
  diff::GenNode &Add(const char *idname, const int16_t c1 = 0, const int16_t c2 = 0)
  {
    return b.Add(idname, c1, c2);
  }
  /** Continues the step's chain through `node` (its Flow in, its Done out). */
  void Chain(diff::GenNode &node, const char *done = "Done")
  {
    b.Link(*flow, flow_pin, node, "Flow");
    flow = &node;
    flow_pin = done;
  }
  void Store(diff::GenNode &node, const char *pin, const char *property, const char *setter)
  {
    diff::GenNode &writer = b.Add(setter);
    b.String(writer, "Property", property);
    b.Link(node, pin, writer, "Value");
    Chain(writer);
  }
  void StoreFloat(diff::GenNode &node, const char *pin, const char *property)
  {
    Store(node, pin, property, "LogicNativeSetGamePropertyFloat");
  }
  void StoreBool(diff::GenNode &node, const char *pin, const char *property)
  {
    Store(node, pin, property, "LogicNativeSetGamePropertyBool");
  }
  void StoreText(diff::GenNode &node, const char *pin, const char *property)
  {
    Store(node, pin, property, "LogicNativeSetGamePropertyString");
  }
  void StoreVector(diff::GenNode &node, const char *pin, const std::string &property)
  {
    diff::GenNode &split = b.Add("LogicNativeSeparateXYZ");
    b.Link(node, pin, split, "Vector");
    for (const char *axis : {"X", "Y", "Z"}) {
      names.push_back(property + axis);
      StoreFloat(split, axis, names.back().c_str());
    }
  }
  void StoreCount(diff::GenNode &node, const char *pin, const char *property)
  {
    diff::GenNode &length = b.Add("LogicNativeListLength");
    b.Link(node, pin, length, "List");
    Store(length, "Length", property, "LogicNativeSetGamePropertyInt");
  }
  /** A pulse output into a Bool property set true (the test sets it false again). */
  void Flag(diff::GenNode &node, const char *pulse, const char *property)
  {
    diff::GenNode &writer = b.Add("LogicNativeSetGamePropertyBool");
    b.String(writer, "Property", property);
    b.Bool(writer, "Value", true);
    b.Link(node, pulse, writer, "Flow");
  }
  std::shared_ptr<LN_Program> Compile()
  {
    std::string report;
    std::shared_ptr<LN_Program> program = b.Compile(&report);
    EXPECT_NE(program, nullptr) << report;
    return program;
  }

  diff::TreeBuilder b;
  diff::GenNode *step = nullptr;
  diff::GenNode *flow = nullptr;
  const char *flow_pin = nullptr;
  std::deque<std::string> names;
};

/** A Navigate (Go To `goal`, 3 m/s, arrived within 0.3 m, Look At fast) and Get Navigation
 * Info's outputs stored; When Reached raises "reached". */
std::shared_ptr<LN_Program> NavigateTree(const MT_Vector3 &goal,
                                         const int every = 1,
                                         const bool until_arrived = false)
{
  NavTree tree(every);
  diff::GenNode &navigate = tree.Add(
      "LogicNativeNavigate", 0, until_arrived ? LN_NAVIGATE_UNTIL_ARRIVED : 0);
  tree.b.Vector(navigate, "Destination", float(goal[0]), float(goal[1]), float(goal[2]));
  tree.b.Float(navigate, "Lin Speed", 3.0f);
  tree.b.Float(navigate, "Reach Threshold", 0.3f);
  tree.b.Float(navigate, "Rot Speed", 20.0f);
  if (until_arrived) {
    /* One pulse: the step -> Once -> Navigate; the reads below go on every step. */
    diff::GenNode &once = tree.Add("LogicNativeOnce");
    tree.b.Link(*tree.step, "Out", once, "Flow");
    tree.b.Link(once, "Out", navigate, "Flow");
  }
  else {
    tree.Chain(navigate);
  }
  tree.Flag(navigate, "When Reached", "reached");
  diff::GenNode &info = tree.Add("LogicNativeGetNavigationInfo");
  tree.StoreBool(info, "Moving", "moving");
  tree.StoreBool(info, "Arrived", "arrived");
  tree.StoreBool(info, "Has Path", "has_path");
  tree.StoreBool(info, "Path Complete", "complete");
  tree.StoreBool(info, "No Path", "no_path");
  tree.StoreFloat(info, "Remaining Distance", "remaining");
  tree.StoreFloat(info, "Speed", "speed");
  tree.StoreVector(info, "Velocity", "vel");
  tree.StoreVector(info, "Goal", "goal");
  tree.StoreVector(info, "Next Corner", "corner");
  return tree.Compile();
}

MT_Vector3 ReadVector(SemRig &rig, KX_GameObject &object, const std::string &property)
{
  return MT_Vector3(rig.Float(object, (property + "X").c_str()),
                    rig.Float(object, (property + "Y").c_str()),
                    rig.Float(object, (property + "Z").c_str()));
}

void Place(KX_GameObject &object, const MT_Vector3 &position)
{
  object.NodeSetWorldPosition(position);
  object.NodeUpdateGS(0.0);
}

/** Navigate in a tree walks its owner around the wall to its goal (the step after each tick
 * moves it), facing the way it walks; Get Navigation Info tells the way, When Reached pulses. */
TEST(KX_NavigationNodes, NavigateWalksAroundTheWallAndTellsItsProgress)
{
  const MT_Vector3 goal(-5, 5, 0);
  const std::shared_ptr<LN_Program> program = NavigateTree(goal);
  ASSERT_NE(program, nullptr);
  for (const SemMode &mode : {kProduction, kMemoOff}) {
    SCOPED_TRACE(mode.name);
    Level level = WallLevel();
    std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
    NavigationSeam seam(navigation.get());
    SemRig rig(program, mode);
    KX_GameObject &npc = rig.Object(0);
    Place(npc, MT_Vector3(-5, -5, 1));
    bool was_moving = false, had_path = false, faced = false, full_speed = false;
    int reached_pulses = 0, steps = 0, arrived_at = -1;
    float first_remaining = -1.0f;
    for (; steps < 60 * 20; steps++) {
      rig.SetBool(npc, "reached", false);
      rig.Step();
      navigation->Step(1.0f / 60.0f, nullptr);
      reached_pulses += rig.Float(npc, "reached") == 1.0f ? 1 : 0;
      const MT_Vector3 p = npc.NodeGetWorldPosition();
      if (p.x() < 5.5f) {
        ASSERT_FALSE(std::fabs(p.y()) < 0.6f) << steps;
      }
      if (rig.Float(npc, "moving") == 1.0f) {
        was_moving = true;
        if (first_remaining < 0.0f) {
          first_remaining = rig.Float(npc, "remaining");
        }
        /* Speed: how fast it walks (Lin Speed once it got going on a straight stretch). */
        full_speed = full_speed || std::fabs(rig.Float(npc, "speed") - 3.0f) < 1e-3f;
        /* Look At: its front (+Y, where it faced at the start) along the velocity of the step
         * before, on a leg that goes another way (east, then west round the wall). */
        const MT_Vector3 v = ReadVector(rig, npc, "vel");
        const MT_Vector3 front = npc.NodeGetWorldOrientation() * MT_Vector3(0, 1, 0);
        if (steps > 30 && v.length() > 1.0f && std::fabs(v.normalized().y()) < 0.3f) {
          faced = faced || front.dot(v.normalized()) > 0.98f;
        }
      }
      had_path = had_path || rig.Float(npc, "has_path") == 1.0f;
      if (rig.Float(npc, "arrived") == 1.0f && reached_pulses > 0 && arrived_at < 0) {
        arrived_at = steps;
      }
      /* A minute standing there, still asked: no more When Reached. */
      if (arrived_at >= 0 && steps > arrived_at + 60) {
        break;
      }
    }
    EXPECT_TRUE(was_moving);
    EXPECT_TRUE(had_path);
    EXPECT_TRUE(faced);
    EXPECT_EQ(reached_pulses, 1);
    EXPECT_EQ(rig.Float(npc, "arrived"), 1.0f);
    EXPECT_EQ(rig.Float(npc, "complete"), 1.0f);
    EXPECT_EQ(rig.Float(npc, "no_path"), 0.0f);
    EXPECT_TRUE(full_speed);
    EXPECT_EQ(rig.Float(npc, "speed"), 0.0f);
    /* Round the wall's end at x = 6: much longer than the 10 m straight line. */
    EXPECT_GT(first_remaining, 20.0f);
    const MT_Vector3 told_goal = ReadVector(rig, npc, "goal");
    EXPECT_NEAR(told_goal.x(), goal.x(), 1e-4f);
    EXPECT_NEAR(told_goal.y(), goal.y(), 1e-4f);
    const MT_Vector3 p = npc.NodeGetWorldPosition();
    EXPECT_NEAR(p.x(), goal.x(), 0.35f);
    EXPECT_NEAR(p.y(), goal.y(), 0.35f);
    EXPECT_NEAR(p.z(), 1.0f, 0.05f);
  }
}

/** Behind Every N Steps the tree asks every 4th step: the NPC still walks every step (the
 * request holds), as fast as asked every step. */
TEST(KX_NavigationNodes, NavigateBehindEveryNStepsKeepsWalking)
{
  const MT_Vector3 goal(5, -5, 0);
  int arrival[2] = {0, 0};
  for (const int every : {1, 4}) {
    SCOPED_TRACE(every);
    const std::shared_ptr<LN_Program> program = NavigateTree(goal, every);
    ASSERT_NE(program, nullptr);
    Level level;
    level.floors.push_back({MT_Vector3(-10, -10, 0), MT_Vector3(10, 10, 0)});
    std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
    NavigationSeam seam(navigation.get());
    SemRig rig(program);
    KX_GameObject &npc = rig.Object(0);
    Place(npc, MT_Vector3(-5, -5, 0));
    int moved_steps = 0, steps = 0;
    for (; steps < 60 * 10; steps++) {
      const MT_Vector3 before = npc.NodeGetWorldPosition();
      rig.Step();
      navigation->Step(1.0f / 60.0f, nullptr);
      if (steps >= 4 && steps < 4 + 120) {
        moved_steps += (npc.NodeGetWorldPosition() - before).length() > 1e-3f ? 1 : 0;
      }
      if ((npc.NodeGetWorldPosition() - goal).length() < 0.35f) {
        break;
      }
    }
    EXPECT_EQ(moved_steps, 120);
    arrival[every == 1 ? 0 : 1] = steps;
    EXPECT_LT(steps, 60 * 10);
  }
  /* Ten metres at 3 m/s: about 200 steps either way (the gated tree starts up to 3 steps late). */
  EXPECT_LE(std::abs(arrival[0] - arrival[1]), 8) << arrival[0] << " " << arrival[1];
}

/** Navmesh Query's five questions on the scene's own navmesh, around the wall. */
TEST(KX_NavigationNodes, NavmeshQueryAnswersOnTheScenesNavmesh)
{
  using Q = LN_NavmeshQueryMode;
  NavTree tree;
  auto query = [&](const Q mode, const char *prefix) -> diff::GenNode & {
    diff::GenNode &node = tree.Add("LogicNativeNavmeshQuery", int16_t(mode));
    std::string name = std::string(prefix) + "_found";
    tree.names.push_back(name);
    tree.StoreBool(node, "Found", tree.names.back().c_str());
    return node;
  };
  diff::GenNode &path = query(Q::FindPath, "path");
  tree.b.Vector(path, "From", -5, -5, 0);
  tree.b.Vector(path, "To", -5, 5, 0);
  tree.StoreFloat(path, "Length", "path_length");
  tree.StoreCount(path, "Points", "path_points");
  tree.StoreVector(path, "Point", "path_end");
  diff::GenNode &off = query(Q::FindPath, "off");
  tree.b.Vector(off, "From", -5, -5, 0);
  tree.b.Vector(off, "To", 50, 50, 0);
  diff::GenNode &nearest = query(Q::NearestPoint, "near");
  tree.b.Vector(nearest, "Position", -4, -3, 3);
  tree.StoreVector(nearest, "Point", "near_point");
  diff::GenNode &random = query(Q::RandomPoint, "rand");
  tree.b.Vector(random, "Position", 0, -5, 0);
  tree.b.Float(random, "Radius", 3.0f);
  tree.StoreVector(random, "Point", "rand_point");
  diff::GenNode &reach = query(Q::IsReachable, "reach");
  tree.b.Vector(reach, "From", -5, -5, 0);
  tree.b.Vector(reach, "To", -5, 5, 0);
  tree.StoreFloat(reach, "Length", "reach_length");
  diff::GenNode &blocked = query(Q::WalkableLine, "blocked");
  tree.b.Vector(blocked, "From", -5, -5, 0);
  tree.b.Vector(blocked, "To", -5, 5, 0);
  tree.StoreVector(blocked, "Point", "blocked_point");
  tree.StoreVector(blocked, "Normal", "blocked_normal");
  diff::GenNode &clear = query(Q::WalkableLine, "clear");
  tree.b.Vector(clear, "From", -5, -5, 0);
  tree.b.Vector(clear, "To", 5, -5, 0);
  tree.StoreFloat(clear, "Length", "clear_length");
  const std::shared_ptr<LN_Program> program = tree.Compile();
  ASSERT_NE(program, nullptr);

  Level level = WallLevel();
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  NavigationSeam seam(navigation.get());
  SemRig rig(program);
  KX_GameObject &owner = rig.Object(0);
  /* Before the first navigation step there is no navmesh: nothing found. */
  rig.Step();
  EXPECT_EQ(rig.Float(owner, "path_found"), 0.0f);
  EXPECT_EQ(rig.Float(owner, "near_found"), 0.0f);
  navigation->Step(1.0f / 60.0f, nullptr);
  std::set<std::pair<float, float>> random_points;
  for (int step = 0; step < 6; step++) {
    rig.Step();
    navigation->Step(1.0f / 60.0f, nullptr);
    const MT_Vector3 r = ReadVector(rig, owner, "rand_point");
    EXPECT_EQ(rig.Float(owner, "rand_found"), 1.0f);
    EXPECT_LE(std::hypot(r.x() - 0.0f, r.y() + 5.0f), 3.0f + 1e-3f);
    EXPECT_NEAR(r.z(), 0.0f, 0.3f);
    random_points.insert({float(r.x()), float(r.y())});
  }
  /* A new point each step. */
  EXPECT_GE(random_points.size(), 5u);
  EXPECT_EQ(rig.Float(owner, "path_found"), 1.0f);
  EXPECT_GT(rig.Float(owner, "path_length"), 20.0f);
  EXPECT_GE(rig.Float(owner, "path_points", -1.0f), 3.0f);
  const MT_Vector3 end = ReadVector(rig, owner, "path_end");
  EXPECT_NEAR(end.x(), -5.0f, 0.05f);
  EXPECT_NEAR(end.y(), 5.0f, 0.05f);
  /* Off the navmesh: no way all the way there. */
  EXPECT_EQ(rig.Float(owner, "off_found"), 0.0f);
  EXPECT_EQ(rig.Float(owner, "near_found"), 1.0f);
  const MT_Vector3 near_point = ReadVector(rig, owner, "near_point");
  EXPECT_NEAR(near_point.x(), -4.0f, 0.05f);
  EXPECT_NEAR(near_point.y(), -3.0f, 0.05f);
  EXPECT_NEAR(near_point.z(), 0.0f, 0.3f);
  EXPECT_EQ(rig.Float(owner, "reach_found"), 1.0f);
  EXPECT_NEAR(rig.Float(owner, "reach_length"), rig.Float(owner, "path_length"), 1e-4f);
  /* The wall stops the straight walk at its eroded edge (0.5 + the agent's 0.3 m). */
  EXPECT_EQ(rig.Float(owner, "blocked_found"), 0.0f);
  const MT_Vector3 stop = ReadVector(rig, owner, "blocked_point");
  EXPECT_NEAR(stop.y(), -0.8f, 0.25f);
  const MT_Vector3 normal = ReadVector(rig, owner, "blocked_normal");
  EXPECT_NEAR(normal.y(), -1.0f, 0.05f);
  EXPECT_EQ(rig.Float(owner, "clear_found"), 1.0f);
  EXPECT_NEAR(rig.Float(owner, "clear_length"), 10.0f, 1e-3f);
}

/** On Navigation Link pulses once, in the step after the NPC started across a link, with the
 * link's object and kind; Get Navigation Info's On Link is on while it crosses. */
TEST(KX_NavigationNodes, OnNavigationLinkPulsesOnceWithTheLink)
{
  NavTree tree;
  diff::GenNode &navigate = tree.Add("LogicNativeNavigate");
  tree.b.Vector(navigate, "Destination", 6, -3, 1.5f);
  tree.b.Float(navigate, "Lin Speed", 3.0f);
  tree.b.Float(navigate, "Reach Threshold", 0.3f);
  tree.Chain(navigate);
  diff::GenNode &info = tree.Add("LogicNativeGetNavigationInfo");
  tree.StoreBool(info, "On Link", "on_link");
  tree.StoreBool(info, "Arrived", "arrived");
  diff::GenNode &event = tree.Add("LogicNativeOnNavigationLink");
  tree.Flag(event, "On Link Start", "started");
  diff::GenNode &kind = tree.b.Add("LogicNativeSetGamePropertyString");
  tree.b.String(kind, "Property", "kind");
  tree.b.Link(event, "On Link Start", kind, "Flow");
  tree.b.Link(event, "Link Kind", kind, "Value");
  diff::GenNode &which = tree.b.Add("LogicNativeSetGamePropertyString");
  tree.b.String(which, "Property", "which");
  tree.b.Link(kind, "Done", which, "Flow");
  tree.b.Link(event, "Link Object", which, "Value");
  const std::shared_ptr<LN_Program> program = tree.Compile();
  ASSERT_NE(program, nullptr);

  Level level;
  level.floors.push_back({MT_Vector3(-10, -10, 0), MT_Vector3(-1, 10, 0)});
  level.floors.push_back({MT_Vector3(1, -10, 1.5f), MT_Vector3(10, 10, 1.5f)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  NavigationSeam seam(navigation.get());
  SemRig rig(program);
  KX_GameObject &npc = rig.Object(0);
  Place(npc, MT_Vector3(-6, 3, 0));
  KX_GameObject &link_object = rig.AddObject("Ledge", MT_Vector3(-1.5f, 0, 0));
  KX_NavLink link;
  link.object = &link_object;
  link.start = MT_Vector3(-1.5f, 0, 0);
  link.end = MT_Vector3(1.5f, 0, 1.5f);
  link.kind = "jump";
  link.user_id = 1;
  navigation->SetLinksForTests({link});
  int pulses = 0, on_link_steps = 0;
  for (int step = 0; step < 60 * 20; step++) {
    rig.SetBool(npc, "started", false);
    rig.Step();
    navigation->Step(1.0f / 60.0f, nullptr);
    pulses += rig.Float(npc, "started") == 1.0f ? 1 : 0;
    on_link_steps += rig.Float(npc, "on_link") == 1.0f ? 1 : 0;
    if (rig.Float(npc, "arrived") == 1.0f) {
      break;
    }
  }
  EXPECT_EQ(pulses, 1);
  EXPECT_GT(on_link_steps, 3);
  EXPECT_EQ(rig.Text(npc, "kind"), "jump");
  EXPECT_EQ(rig.Text(npc, "which"), "Ledge");
  EXPECT_EQ(rig.Float(npc, "arrived"), 1.0f);
  EXPECT_NEAR(npc.NodeGetWorldPosition().z(), 1.5f, 0.1f);
}

/** Set Navigation Attribute: an area closed for the NPC (it walks round), an object made an
 * obstacle and not, the navmesh made again in a box after a level piece went. */
TEST(KX_NavigationNodes, SetNavigationAttributeChangesTheNavigation)
{
  using A = LN_NavigationAttribute;
  blender::Object crate_blender{};
  std::snprintf(crate_blender.id.name, sizeof(crate_blender.id.name), "OBCrate");
  NavTree tree;
  diff::GenNode &once = tree.Add("LogicNativeOnce");
  tree.b.Link(*tree.step, "Out", once, "Flow");
  diff::GenNode &close = tree.Add("LogicNativeSetNavigationAttribute", int16_t(A::AreaClosed));
  tree.b.String(close, "Area", "water");
  tree.b.Bool(close, "Closed", true);
  tree.b.Link(once, "Out", close, "Flow");
  diff::GenNode &crate_on = tree.Add("LogicNativeSetNavigationAttribute", int16_t(A::Obstacle));
  tree.b.ObjectNamed(crate_on, "Object", &crate_blender);
  tree.b.Bool(crate_on, "Enabled", true);
  tree.b.Link(close, "Done", crate_on, "Flow");
  diff::GenNode &navigate = tree.Add("LogicNativeNavigate");
  tree.b.Vector(navigate, "Destination", -6, 6, 0);
  tree.b.Float(navigate, "Lin Speed", 3.0f);
  tree.b.Float(navigate, "Reach Threshold", 0.3f);
  tree.b.Bool(navigate, "Avoid Others", false);
  tree.Chain(navigate);
  /* The game property "rebuild" asks for the box around (8, 0) to be made again. */
  diff::GenNode &wish = tree.Add("LogicNativeGetGamePropertyBool");
  tree.b.String(wish, "Property", "rebuild");
  diff::GenNode &branch = tree.Add("LogicNativeBranch");
  tree.b.Link(wish, "Value", branch, "Condition");
  tree.Chain(branch, "True");
  diff::GenNode &rebuild = tree.Add("LogicNativeSetNavigationAttribute", int16_t(A::RebuildArea));
  tree.b.Vector(rebuild, "Center", 8, 0, 0);
  tree.b.Vector(rebuild, "Size", 6, 6, 4);
  tree.Chain(rebuild);
  diff::GenNode &solid = tree.Add("LogicNativeSetNavigationAttribute", int16_t(A::Obstacle));
  tree.b.ObjectNamed(solid, "Object", &crate_blender);
  tree.b.Bool(solid, "Enabled", false);
  tree.Chain(solid);
  const std::shared_ptr<LN_Program> program = tree.Compile();
  ASSERT_NE(program, nullptr);

  Level level;
  level.floors.push_back({MT_Vector3(-10, -10, 0), MT_Vector3(10, -2, 0)});
  level.floors.push_back({MT_Vector3(-10, 2, 0), MT_Vector3(10, 10, 0)});
  level.floors.push_back({MT_Vector3(-10, -2, 0), MT_Vector3(6, 2, 0)});
  level.floors.push_back({MT_Vector3(6, -2, 0), MT_Vector3(10, 2, 0)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  NavigationSeam seam(navigation.get());
  level.floors[2].area = navigation->AreaId("water");
  SemRig rig(program);
  KX_GameObject &npc = rig.Object(0);
  KX_GameObject &crate = rig.AddObject("Crate", MT_Vector3(-8, 8, 0));
  Place(npc, MT_Vector3(-6, -6, 0));
  rig.SetBool(npc, "rebuild", false);
  float widest_x = -1e9f;
  bool arrived = false;
  for (int step = 0; step < 60 * 20 && !arrived; step++) {
    rig.Step();
    navigation->Step(1.0f / 60.0f, nullptr);
    widest_x = std::max(widest_x, float(npc.NodeGetWorldPosition().x()));
    KX_NavAgentInfo info;
    arrived = navigation->GetAgentInfo(&npc, info) && info.arrived;
  }
  EXPECT_TRUE(arrived);
  /* The water is closed to it: round by the east end. */
  EXPECT_GT(widest_x, 5.0f);
  EXPECT_TRUE(navigation->IsObstacle(&crate));

  /* The east bridge goes; the tree asks for its box to be made again (and for the crate not to
   * be an obstacle any more): the navmesh there is gone. */
  level.floors[3].enabled = false;
  const KX_NavMesh *before = navigation->FindSceneNavMesh();
  ASSERT_NE(before, nullptr);
  MT_Vector3 snapped;
  ASSERT_TRUE(before->NearestPoint(MT_Vector3(8, 0, 0), snapped));
  EXPECT_LT((snapped - MT_Vector3(8, 0, 0)).length(), 0.3f);
  rig.SetBool(npc, "rebuild", true);
  rig.Step();
  navigation->Step(1.0f / 60.0f, nullptr);
  rig.SetBool(npc, "rebuild", false);
  for (int step = 0; step < 4; step++) {
    rig.Step();
    navigation->Step(1.0f / 60.0f, nullptr);
  }
  const KX_NavMesh *after = navigation->FindSceneNavMesh();
  ASSERT_NE(after, nullptr);
  MT_Vector3 moved;
  if (after->NearestPoint(MT_Vector3(8, 0, 0), moved)) {
    EXPECT_GT((moved - MT_Vector3(8, 0, 0)).length(), 1.0f);
  }
  EXPECT_FALSE(navigation->IsObstacle(&crate));
}

/** Until Arrived: one pulse (a Once) is enough; When Reached pulses once, though nothing runs
 * Navigate any more; Get Navigation Info's Arrived stays on after. */
TEST(KX_NavigationNodes, UntilArrivedNeedsOnePulse)
{
  const MT_Vector3 goal(-5, 5, 0);
  const std::shared_ptr<LN_Program> program = NavigateTree(goal, 1, true);
  ASSERT_NE(program, nullptr);
  for (const SemMode &mode : {kProduction, kMemoOff}) {
    SCOPED_TRACE(mode.name);
    Level level = WallLevel();
    std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
    NavigationSeam seam(navigation.get());
    SemRig rig(program, mode);
    KX_GameObject &npc = rig.Object(0);
    Place(npc, MT_Vector3(-5, -5, 1));
    int reached = 0, done = 0;
    for (int step = 0; step < 60 * 20; step++) {
      rig.SetBool(npc, "reached", false);
      rig.Step();
      navigation->Step(1.0f / 60.0f, nullptr);
      reached += rig.Float(npc, "reached") == 1.0f ? 1 : 0;
      KX_NavAgentInfo info;
      navigation->GetAgentInfo(&npc, info);
      done += info.active ? 0 : 1;
      const MT_Vector3 p = npc.NodeGetWorldPosition();
      if (p.x() < 5.5f) {
        ASSERT_FALSE(std::fabs(p.y()) < 0.6f) << step;
      }
    }
    EXPECT_EQ(reached, 1);
    EXPECT_GT(done, 60);
    EXPECT_EQ(rig.Float(npc, "arrived"), 1.0f);
    EXPECT_NEAR(npc.NodeGetWorldPosition().x(), goal.x(), 0.35f);
    EXPECT_NEAR(npc.NodeGetWorldPosition().y(), goal.y(), 0.35f);
  }
}

/** Navigate's Stop input ends an Until Arrived order where the NPC is: a game property raised at
 * step 60 runs it through a Branch. */
TEST(KX_NavigationNodes, TheStopInputEndsAnOrder)
{
  NavTree tree;
  diff::GenNode &once = tree.Add("LogicNativeOnce");
  tree.b.Link(*tree.step, "Out", once, "Flow");
  diff::GenNode &navigate = tree.Add("LogicNativeNavigate", 0, LN_NAVIGATE_UNTIL_ARRIVED);
  tree.b.Vector(navigate, "Destination", 8, 0, 0);
  tree.b.Float(navigate, "Lin Speed", 3.0f);
  tree.b.Float(navigate, "Reach Threshold", 0.3f);
  tree.b.Link(once, "Out", navigate, "Flow");
  tree.Flag(navigate, "When Reached", "reached");
  diff::GenNode &halt = tree.Add("LogicNativeGetGamePropertyBool");
  tree.b.String(halt, "Property", "halt");
  diff::GenNode &branch = tree.Add("LogicNativeBranch");
  tree.b.Link(*tree.step, "Out", branch, "Flow");
  tree.b.Link(halt, "Value", branch, "Condition");
  tree.b.Link(branch, "True", navigate, "Stop");
  const std::shared_ptr<LN_Program> program = tree.Compile();
  ASSERT_NE(program, nullptr);
  Level level;
  level.floors.push_back({MT_Vector3(-10, -10, 0), MT_Vector3(10, 10, 0)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  NavigationSeam seam(navigation.get());
  SemRig rig(program);
  KX_GameObject &npc = rig.Object(0);
  Place(npc, MT_Vector3(-8, 0, 0));
  rig.SetBool(npc, "halt", false);
  rig.SetBool(npc, "reached", false);
  MT_Vector3 at_stop;
  for (int step = 0; step < 60 * 8; step++) {
    if (step == 60) {
      rig.SetBool(npc, "halt", true);
    }
    rig.Step();
    navigation->Step(1.0f / 60.0f, nullptr);
    if (step == 61) {
      at_stop = npc.NodeGetWorldPosition();
      EXPECT_GT(at_stop.x(), -8.0f);
    }
  }
  EXPECT_LT(npc.NodeGetWorldPosition().x(), 0.0f);
  EXPECT_LT((npc.NodeGetWorldPosition() - at_stop).length(), 0.05f);
  EXPECT_EQ(rig.Float(npc, "reached"), 0.0f);
}

/** Navigate's Wander stays within Radius of its home and goes places. */
TEST(KX_NavigationNodes, NavigateWanderStaysNearHome)
{
  NavTree tree;
  diff::GenNode &navigate = tree.Add("LogicNativeNavigate", 1 /* Wander */);
  tree.b.Vector(navigate, "Destination", 0, 0, 0);
  tree.b.Float(navigate, "Lin Speed", 3.0f);
  tree.b.Float(navigate, "Radius", 6.0f);
  tree.Chain(navigate);
  const std::shared_ptr<LN_Program> program = tree.Compile();
  ASSERT_NE(program, nullptr);
  Level level;
  level.floors.push_back({MT_Vector3(-20, -20, 0), MT_Vector3(20, 20, 0)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  NavigationSeam seam(navigation.get());
  SemRig rig(program);
  KX_GameObject &npc = rig.Object(0);
  std::set<std::pair<int, int>> cells;
  for (int step = 0; step < 60 * 30; step++) {
    rig.Step();
    navigation->Step(1.0f / 60.0f, nullptr);
    const MT_Vector3 p = npc.NodeGetWorldPosition();
    ASSERT_LT(std::hypot(p.x(), p.y()), 6.5f) << step;
    cells.insert({int(std::floor(p.x())), int(std::floor(p.y()))});
  }
  EXPECT_GT(cells.size(), 10u);
}

/** Follow Path walks its points one after the other (with no navmesh object straight from point
 * to point) and stops after the last. */
TEST(KX_NavigationNodes, FollowPathWalksItsPointsInOrder)
{
  NavTree tree;
  diff::GenNode &list = tree.Add("LogicNativeListFromItems", 5); /* the Vectors input */
  const float points[3][2] = {{4, 0}, {4, 4}, {0, 4}};
  for (const auto &point : points) {
    diff::GenNode &v = tree.Add("LogicNativeCombineXYZ");
    tree.b.Float(v, "X", point[0]);
    tree.b.Float(v, "Y", point[1]);
    tree.b.Link(v, "Vector", list, "Vectors");
  }
  diff::GenNode &follow = tree.Add("LogicNativeFollowPath");
  tree.b.Link(list, "Vectors", follow, "Path Points");
  tree.b.Float(follow, "Lin Speed", 3.0f);
  tree.b.Float(follow, "Reach Threshold", 0.2f);
  tree.Chain(follow);
  const std::shared_ptr<LN_Program> program = tree.Compile();
  ASSERT_NE(program, nullptr);
  Level level;
  level.floors.push_back({MT_Vector3(-10, -10, 0), MT_Vector3(10, 10, 0)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  NavigationSeam seam(navigation.get());
  SemRig rig(program);
  KX_GameObject &npc = rig.Object(0);
  /* The order it comes near each point in. */
  std::vector<int> visits;
  for (int step = 0; step < 60 * 10; step++) {
    rig.Step();
    navigation->Step(1.0f / 60.0f, nullptr);
    const MT_Vector3 p = npc.NodeGetWorldPosition();
    for (int i = 0; i < 3; i++) {
      if (std::hypot(p.x() - points[i][0], p.y() - points[i][1]) < 0.3f &&
          (visits.empty() || visits.back() != i))
      {
        visits.push_back(i);
      }
    }
    /* Straight lines between the points: never off the square's sides. */
    ASSERT_GT(p.x(), -0.3f) << step;
    ASSERT_LT(p.x(), 4.3f) << step;
    ASSERT_GT(p.y(), -0.3f) << step;
    ASSERT_LT(p.y(), 4.3f) << step;
  }
  EXPECT_EQ(visits, std::vector<int>({0, 1, 2}));
  /* After the last point it stands there. */
  EXPECT_NEAR(npc.NodeGetWorldPosition().x(), 0.0f, 0.25f);
  EXPECT_NEAR(npc.NodeGetWorldPosition().y(), 4.0f, 0.25f);
}

/** A crowd's brains: 120 NPCs each running Navigate (after the horde's goal, Look At) and Get
 * Navigation Info run wide on this thread and with 3 helpers exactly as tree by tree, every pose
 * and every stored output bit for bit over 90 steps; the run is wide (lanes counted). */
TEST(KX_NavigationNodes, ACrowdsBrainsRunWideAsTheirColumnPasses)
{
  const uint32_t owners = 120;
  for (const bool until_arrived : {false, true}) {
  SCOPED_TRACE(until_arrived ? "Until Arrived" : "asked every step");
  const std::shared_ptr<LN_Program> program = NavigateTree(MT_Vector3(0, 8, 0), 1, until_arrived);
  ASSERT_NE(program, nullptr);
  {
    std::string refusal;
    const std::shared_ptr<const LN_ColumnProgram> columns = LN_CompileColumnProgram(
        *program, LN_Event::OnFixedUpdate, &refusal, false);
    EXPECT_NE(columns, nullptr) << refusal;
  }
  std::string reference;
  for (const int columns : {0, 1, 4}) {
    SCOPED_TRACE("columns " + std::to_string(columns));
    Level level = WallLevel();
    std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
    NavigationSeam seam(navigation.get());
    SemRig rig(program, kProduction, owners);
    if (columns != 0) {
      rig.SetColumns(true, uint32_t(columns - 1));
    }
    for (uint32_t index = 0; index < owners; index++) {
      Place(rig.Object(index),
            MT_Vector3(-9.0f + float(index % 15) * 1.2f, -9.0f + float(index / 15) * 1.0f, 0));
    }
    const uint64_t lanes_before = LN_ColumnLaneCount();
    std::string text;
    for (int step = 0; step < 90; step++) {
      rig.Step();
      navigation->Step(1.0f / 60.0f, nullptr);
      for (uint32_t index = 0; index < owners; index++) {
        KX_GameObject &npc = rig.Object(index);
        const MT_Vector3 p = npc.NodeGetWorldPosition();
        const MT_Matrix3x3 o = npc.NodeGetWorldOrientation();
        for (const float value : {float(p[0]), float(p[1]), float(p[2]), float(o[0][1]),
                                  rig.Float(npc, "remaining"), rig.Float(npc, "velX"),
                                  rig.Float(npc, "cornerY"), rig.Float(npc, "moving")})
        {
          text += Bits(value) + " ";
        }
      }
      text += "\n";
    }
    if (columns == 0) {
      reference = text;
    }
    else {
      EXPECT_TRUE(text == reference) << "a wide run's NPCs differ from the column passes'";
      /* (UPBGE_LN_COLUMN_DIRECT=force: every tree a column pass, no lanes to count.) */
      if (LN_GetColumnDirectMode() != LN_ColumnDirectMode::Force) {
        EXPECT_GT(LN_ColumnLaneCount() - lanes_before, uint64_t(owners) * 80)
            << rig.LastColumnRefusal();
      }
    }
  }
  }
}

/** \} */

}  // namespace
