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
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <tuple>
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
  /** Solid boxes (walls, pillars): their six faces, each box a closed solid piece (as Jolt's
   * boxes are: KX_NavGeometry::pieces). */
  std::vector<Box> boxes;

  void Emit(const MT_Vector3 &a,
            const MT_Vector3 &b,
            const MT_Vector3 &c,
            const int area,
            KX_NavGeometry &g,
            const int piece = -1) const
  {
    g.pieces.push_back(piece);
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
            KX_NavGeometry &g,
            const int piece = -1) const
  {
    Emit(a, b, c, area, g, piece);
    Emit(a, c, d, area, g, piece);
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
    for (size_t i = 0; i < boxes.size(); i++) {
      const Box &b = boxes[i];
      if (!b.enabled) {
        continue;
      }
      const int piece = int(i);
      const MT_Vector3 &l = b.min, &h = b.max;
      const MT_Vector3 p[8] = {MT_Vector3(l[0], l[1], l[2]),
                               MT_Vector3(h[0], l[1], l[2]),
                               MT_Vector3(h[0], h[1], l[2]),
                               MT_Vector3(l[0], h[1], l[2]),
                               MT_Vector3(l[0], l[1], h[2]),
                               MT_Vector3(h[0], l[1], h[2]),
                               MT_Vector3(h[0], h[1], h[2]),
                               MT_Vector3(l[0], h[1], h[2])};
      Quad(p[4], p[5], p[6], p[7], 0, g, piece); /* top */
      Quad(p[3], p[2], p[1], p[0], 0, g, piece); /* bottom */
      Quad(p[0], p[1], p[5], p[4], 0, g, piece);
      Quad(p[1], p[2], p[6], p[5], 0, g, piece);
      Quad(p[2], p[3], p[7], p[6], 0, g, piece);
      Quad(p[3], p[0], p[4], p[7], 0, g, piece);
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

/** A corner (2026-11-19, the guard of the navigation example 17 that stood 28 s): a wall's west
 * face up to the corner (0, 0), its top on from there sloping a little down (walkable to the
 * west and above). An agent a hair below the corner on the west side, heading along the top's
 * line (its way on, the corner dropped within 1 cm), clips the corner by that hair: the west
 * face's line held it (it slid down the face, 4 cm/s along its way). Near a corner a wall lets go: it goes at full speed. Far
 * from the corner the face holds as before; an inner corner (the face going on) never lets go;
 * a wall the agent stands behind holds nothing. */
TEST(KX_NavOrca, ACornerLetsGoWithinAHair)
{
  using namespace kx_orca;
  const Vec2 corner(0.0f, 0.0f);
  const Vec2 top_end(1.6f, -0.2f);
  const Vec2 top_in = Vec2(0.2f, 1.6f) / length(Vec2(0.2f, 1.6f));
  Params params;
  params.wall_corner_release = 0.03f;
  std::vector<Line> lines, scratch;
  const Vec2 way = (top_end - Vec2(-0.007f, -0.006f)) / length(top_end - Vec2(-0.007f, -0.006f));
  const Vec2 wish = way * 1.6f;
  auto progress = [&](const Vec2 &p, const bool convex) {
    Wall walls[2] = {{Vec2(0.0f, -5.0f), corner, Vec2(-1.0f, 0.0f)},
                     {corner, top_end, top_in}};
    walls[0].b_convex = convex;
    walls[1].a_convex = convex;
    /* (Its progress along its way: a held one slides down the face, as the guard did.) */
    return dot(solve(p, wish, wish, 0.35f, 1.6f, nullptr, 0, walls, 2, params, lines, scratch),
               way);
  };
  EXPECT_GT(progress(Vec2(-0.007f, -0.006f), true), 1.5f);
  /* (The face's corner not known as one: held as before.) */
  EXPECT_LT(progress(Vec2(-0.007f, -0.006f), false), 0.1f);
  /* Far below the corner, the face holds. */
  EXPECT_LT(progress(Vec2(-0.007f, -1.0f), true), 0.1f);
  /* Behind the top's line (round the corner, below it, beside it): the top holds nothing. */
  {
    const Wall top{corner, top_end, top_in};
    const Vec2 p(0.8f, -0.5f);
    const Vec2 up(0.0f, 1.6f);
    const Vec2 v = solve(p, up, up, 0.35f, 1.6f, nullptr, 0, &top, 1, params, lines, scratch);
    EXPECT_NEAR(v.y, 1.6f, 1e-3f);
  }
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
  /* The box's side widened by the agents' radius (2026-11-19: it was the side itself). */
  EXPECT_NEAR(hit.x(), -1.0f - 0.3f, 0.15f);
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

/** Around NPCs: a goal far beyond the built area (200 m off, the build radius 15 m, nobody
 * near it): the NPC heads for it over what is built round itself, round a wall on the way,
 * the tiles ahead built as it goes, and arrives; Path Complete stays off until the goal's
 * tiles exist, No Path never on. 2026-11-19: it stood still for good, No Path (the examples). */
TEST(KX_Navigation, AroundNpcsAnNpcWalksToAFarGoal)
{
  Level level;
  level.floors.push_back({MT_Vector3(-30, -20, 0), MT_Vector3(220, 20, 0)});
  /* A wall across the way 100 m on, a gap at its north end. */
  level.boxes.push_back({MT_Vector3(99.5f, -20, 0), MT_Vector3(100.5f, 12, 3)});
  KX_Navigation::Settings settings = TestSettings(blender::NAVMESH_BUILD_AROUND_NPCS);
  settings.build_radius = 15.0f;
  settings.remove_radius = 25.0f;
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, settings);
  Objects objects;
  KX_GameObject &npc = objects.Add(MT_Vector3(0, 0, 0));
  const MT_Vector3 goal(200, 0, 0);
  bool arrived = false, ever_no_path = false, complete_early = false;
  int step = 0;
  for (; step < 60 * 90 && !arrived; step++) {
    navigation->Submit(GoTo(npc, goal, 6.0f));
    navigation->Step(1.0f / 60.0f, nullptr);
    const MT_Vector3 p = npc.NodeGetWorldPosition();
    ASSERT_FALSE(std::fabs(p.x() - 100.0f) < 0.6f && p.y() < 11.5f) << step;
    KX_NavAgentInfo info;
    ASSERT_TRUE(navigation->GetAgentInfo(&npc, info));
    ever_no_path = ever_no_path || (step > 2 && info.no_path);
    complete_early = complete_early || (info.path_complete && p.x() < 150.0f);
    arrived = info.arrived;
  }
  printf("Around NPCs, a goal 200 m off: %s in %.1f s at (%.1f %.1f)\n",
         arrived ? "arrived" : "NOT arrived", step / 60.0f, npc.NodeGetWorldPosition().x(),
         npc.NodeGetWorldPosition().y());
  EXPECT_TRUE(arrived);
  EXPECT_FALSE(ever_no_path);
  EXPECT_FALSE(complete_early);
}

/** A point reached with a small Reach Threshold, moved straight (no navmesh: Follow Path's way,
 * the move clamped at the threshold), at the examples' places: it arrives as it stops (no second
 * standing there), and a new point within the crowd's "as near as it gets" reach (2 m on for a
 * walker, 8 m and up for a 3 m platform) is walked to, never arrived at once. 2026-11-19: the
 * rounding of 16 - 15.9 kept it from arriving for a second; then, its press time full, the next
 * point counted as arrived at once and Follow Path skipped it (the examples' platform). */
TEST(KX_Navigation, ANewGoalNearAPointJustReachedIsWalkedTo)
{
  for (const float radius : {0.3f, 1.5f}) {
    Level level;
    level.floors.push_back({MT_Vector3(-20, -20, 0), MT_Vector3(30, 30, 0)});
    std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
    Objects objects;
    KX_GameObject &npc = objects.Add(MT_Vector3(6, 6, 1.5f));
    navigation->SetAgentSizeForTests(&npc, radius, 0.1f);
    const MT_Vector3 first(16, 6, 1.5f);
    const MT_Vector3 second = radius > 1.0f ? MT_Vector3(16, 14, 3.0f) : MT_Vector3(16, 8, 1.5f);
    auto order = [&](const MT_Vector3 &goal) {
      KX_NavRequest request = GoTo(npc, goal, 2.0f);
      request.arrive_distance = 0.1f;
      request.avoid = false;
      request.use_scene_navmesh = false;
      return request;
    };
    bool arrived = false;
    int stopped_at = -1, step = 0;
    for (; step < 60 * 10 && !arrived; step++) {
      navigation->Submit(order(first));
      navigation->Step(1.0f / 60.0f, nullptr);
      if (stopped_at < 0 && float((npc.NodeGetWorldPosition() - first).length()) < 0.1001f) {
        stopped_at = step;
      }
      KX_NavAgentInfo info;
      navigation->GetAgentInfo(&npc, info);
      arrived = info.arrived;
    }
    ASSERT_TRUE(arrived);
    printf("radius %.1f: arrived %d steps after it stopped\n", radius, step - 1 - stopped_at);
    EXPECT_LE(step - 1 - stopped_at, 2) << radius;
    /* The new point: not arrived in the step it is asked, and walked to. */
    navigation->Submit(order(second));
    navigation->Step(1.0f / 60.0f, nullptr);
    KX_NavAgentInfo info;
    navigation->GetAgentInfo(&npc, info);
    EXPECT_FALSE(info.arrived) << radius;
    for (int more = 0; more < 60 * 10 && !info.arrived; more++) {
      navigation->Submit(order(second));
      navigation->Step(1.0f / 60.0f, nullptr);
      navigation->GetAgentInfo(&npc, info);
    }
    const float off = float((npc.NodeGetWorldPosition() - second).length());
    printf("radius %.1f: at the second point %.2f m off\n", radius, off);
    EXPECT_LT(off, 0.15f) << radius;
  }
  /* Kept from its point by another NPC standing on it (it arrives as near as it gets, after a
   * second of pressing), then a new point 2 m back: its press was for the old point, it walks. */
  Level level;
  level.floors.push_back({MT_Vector3(-20, -20, 0), MT_Vector3(20, 20, 0)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  Objects objects;
  KX_GameObject &npc = objects.Add(MT_Vector3(-6, 0, 0));
  KX_GameObject &stander = objects.Add(MT_Vector3(0, 0, 0));
  const MT_Vector3 counter(0, 0, 0);
  bool arrived = false;
  for (int step = 0; step < 60 * 10 && !arrived; step++) {
    navigation->Submit(GoTo(stander, counter));
    navigation->Submit(GoTo(npc, counter));
    navigation->Step(1.0f / 60.0f, nullptr);
    KX_NavAgentInfo info;
    navigation->GetAgentInfo(&npc, info);
    arrived = info.arrived;
  }
  ASSERT_TRUE(arrived);
  const MT_Vector3 next = npc.NodeGetWorldPosition() - MT_Vector3(2, 0, 0);
  navigation->Submit(GoTo(stander, counter));
  navigation->Submit(GoTo(npc, next));
  navigation->Step(1.0f / 60.0f, nullptr);
  KX_NavAgentInfo info;
  navigation->GetAgentInfo(&npc, info);
  EXPECT_FALSE(info.arrived) << "a point 2 m back counted as reached at once";
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

/** How an NPC walking to its goal alone went: the step it arrived (-1: never) and the steps it
 * crawled far from its goal (moved less than a third of its speed). */
struct CornerRun {
  int arrived_step = -1;
  int crawl_steps = 0;
  int longest_crawl = 0;
};

CornerRun RunToGoal(const Level &level,
                    const float radius,
                    const MT_Vector3 &start,
                    const MT_Vector3 &goal,
                    const float speed,
                    const int max_steps)
{
  KX_Navigation::Settings settings = TestSettings();
  settings.agent_radius = radius;
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, settings);
  Objects objects;
  KX_GameObject &npc = objects.Add(start);
  CornerRun run;
  int crawl = 0;
  MT_Vector3 previous = start;
  for (int step = 0; step < max_steps; step++) {
    KX_NavRequest request = GoTo(npc, goal, speed);
    request.arrive_distance = 0.6f;
    navigation->Submit(request);
    navigation->Step(1.0f / 60.0f, nullptr);
    const MT_Vector3 p = npc.NodeGetWorldPosition();
    KX_NavAgentInfo info;
    navigation->GetAgentInfo(&npc, info);
    if (info.arrived) {
      run.arrived_step = step;
      break;
    }
    const float moved = float((p - previous).length()) * 60.0f;
    previous = p;
    /* Past its start-up, away from the goal (it slows for the goal itself). */
    if (step > 30 && float((goal - p).length()) > 2.0f && moved < speed / 3.0f) {
      run.crawl_steps++;
      run.longest_crawl = std::max(run.longest_crawl, ++crawl);
    }
    else {
      crawl = 0;
    }
  }
  return run;
}

/* An NPC rounding the corners of walls keeps its pace (2026-11-19). Avoidance held a wall's
 * line as a hard limit: aiming at a corner (a point on that line) slowed it to a crawl for
 * 1.5 s at each corner, a segment it stood behind held it too, and within 1 cm of a corner (the
 * corner dropped, the way to the next one clipping the corner) it stood for 28 s: a guard of the
 * navigation example 17. The example's walls round that guard's way, and the wall level. */
TEST(KX_Navigation, AnNpcRoundsWallCornersWithoutStopping)
{
  Level stealth;
  stealth.floors.push_back({MT_Vector3(-26, -18, 0), MT_Vector3(26, 18, 0)});
  stealth.boxes.push_back({MT_Vector3(13, -18, 0), MT_Vector3(14, -6, 3)});
  stealth.boxes.push_back({MT_Vector3(13, 0, 0), MT_Vector3(14, 18, 3)});
  stealth.boxes.push_back({MT_Vector3(0, -10, 0), MT_Vector3(1, 10, 3)});
  stealth.boxes.push_back({MT_Vector3(5, -9, 0), MT_Vector3(13, -8, 3)});
  const struct {
    const Level *level;
    float radius;
    MT_Vector3 start, goal;
    float speed;
    /* Its way's length over its speed, and some. */
    int max_steps;
  } cases[] = {
      {&stealth, 0.35f, MT_Vector3(5, -13, 0), MT_Vector3(22, -13, 0), 1.6f, 60 * 22},
      {&stealth, 0.35f, MT_Vector3(22, -13, 0), MT_Vector3(5, -13, 0), 1.6f, 60 * 22},
      {&stealth, 0.3f, MT_Vector3(8, -14, 0), MT_Vector3(9, -6, 0), 3.0f, 60 * 8},
      {&stealth, 0.5f, MT_Vector3(20, 14, 0), MT_Vector3(6, -12, 0), 2.5f, 60 * 16},
  };
  int number = 0;
  for (const auto &c : cases) {
    SCOPED_TRACE("case " + std::to_string(number++));
    const CornerRun run = RunToGoal(*c.level, c.radius, c.start, c.goal, c.speed, c.max_steps);
    EXPECT_GE(run.arrived_step, 0);
    EXPECT_LE(run.longest_crawl, 6) << run.crawl_steps << " crawling steps";
    EXPECT_LE(run.crawl_steps, 12);
  }
  Level wall = WallLevel();
  for (const float radius : {0.3f, 0.45f}) {
    for (const float speed : {1.5f, 3.0f, 6.0f}) {
      SCOPED_TRACE("wall level, radius " + std::to_string(radius) + ", speed " +
                   std::to_string(speed));
      const CornerRun run = RunToGoal(
          wall, radius, MT_Vector3(-5, -5, 0), MT_Vector3(-5, 5, 0), speed, 60 * 20);
      EXPECT_GE(run.arrived_step, 0);
      /* (Round one wall's end: never slower than a third of its speed; aimed at the corner
       * itself, or held by the wall up to its corner, it was, 1 to 7 steps.) */
      EXPECT_EQ(run.crawl_steps, 0);
    }
  }
  /* Across a field of pillars (1 m, every 3 m, every other row shifted), eight ways, walking and
   * running: 12 of the 16 stood for good before (a pillar's far wall held them back). A runner
   * weaving at 6 m/s brakes for the pillars in its way now and then. */
  Level pillars;
  pillars.floors.push_back({MT_Vector3(-20, -20, 0), MT_Vector3(20, 20, 0)});
  for (int i = -5; i <= 5; i++) {
    for (int j = -5; j <= 5; j++) {
      const float x = float(i) * 3.0f + float(j % 2) * 1.5f, y = float(j) * 3.0f;
      pillars.boxes.push_back(
          {MT_Vector3(x - 0.5f, y - 0.5f, 0), MT_Vector3(x + 0.5f, y + 0.5f, 3)});
    }
  }
  for (int k = 0; k < 8; k++) {
    const float a = float(k) * 0.785398f;
    const MT_Vector3 start(17.0f * std::cos(a), 17.0f * std::sin(a), 0.0f);
    for (const float speed : {1.6f, 6.0f}) {
      SCOPED_TRACE("pillars, way " + std::to_string(k) + ", speed " + std::to_string(speed));
      const CornerRun run = RunToGoal(
          pillars, 0.35f, start, -start, speed, speed < 2.0f ? 60 * 32 : 60 * 13);
      EXPECT_GE(run.arrived_step, 0);
      EXPECT_LE(run.longest_crawl, speed < 2.0f ? 6 : 16) << run.crawl_steps << " crawling steps";
      EXPECT_LE(run.crawl_steps, speed < 2.0f ? 12 : 90);
    }
  }
}

/* A goal inside a solid (the middle of a pillar, a chest, a table) is reached at the navmesh's
 * point nearest to it, once the way leads there (2026-11-19: the NPC stood beside it for good,
 * When Reached never came). A goal deeper than the usual snap reach is not: it stands as near as
 * it gets, not arrived. */
TEST(KX_Navigation, AGoalInsideASolidIsReachedBesideIt)
{
  Level level;
  level.floors.push_back({MT_Vector3(-20, -20, 0), MT_Vector3(20, 20, 0)});
  /* A pillar, a table-sized box, a block 10 m across. */
  level.boxes.push_back({MT_Vector3(-0.5, 4.5, 0), MT_Vector3(0.5, 5.5, 3)});
  level.boxes.push_back({MT_Vector3(5, -1, 0), MT_Vector3(8, 1, 1)});
  level.boxes.push_back({MT_Vector3(-15, -15, 0), MT_Vector3(-5, -5, 3)});
  /* (How far it stands: the goal's depth in the solid, the radius and a cell or three.) */
  const struct {
    MT_Vector3 goal;
    bool reached;
    float stands_within;
  } cases[] = {
      {MT_Vector3(0, 5, 0), true, 1.4f},
      {MT_Vector3(6.5, 0, 0), true, 1.9f},
      {MT_Vector3(-6.5, -10, 0), true, 2.4f},
      {MT_Vector3(-10, -10, 0), false, 5.9f},
  };
  for (const auto &c : cases) {
    SCOPED_TRACE("goal " + std::to_string(c.goal.x()) + " " + std::to_string(c.goal.y()));
    std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
    Objects objects;
    KX_GameObject &npc = objects.Add(MT_Vector3(0, -3, 0));
    bool arrived = false;
    int reached = 0;
    for (int step = 0; step < 60 * 12; step++) {
      navigation->Submit(GoTo(npc, c.goal, 3.0f));
      navigation->Step(1.0f / 60.0f, nullptr);
      KX_NavAgentInfo info;
      ASSERT_TRUE(navigation->GetAgentInfo(&npc, info));
      arrived = info.arrived;
      reached += info.reached ? 1 : 0;
    }
    EXPECT_EQ(arrived, c.reached);
    EXPECT_EQ(reached, c.reached ? 1 : 0);
    /* Either way it stands next to the solid, nearest the goal. */
    const MT_Vector3 p = npc.NodeGetWorldPosition();
    const MT_Vector3 off(c.goal.x() - p.x(), c.goal.y() - p.y(), 0.0f);
    EXPECT_LT(float(off.length()), c.stands_within);
  }
}

/* -------------------------------------------------------------------- */
/** \name Jump links (2026-11-19)
 * \{ */

/** Floor A (x -12..-1), a 2 m gap, floor B (x 1..12, y -6..0.5) and a platform 1.2 m high
 * beside it (x 1..12, y 0.5..6), a 3 m wall on A (x -12..-8 at y 0), all at z 0. Nothing below
 * the gap. */
Level JumpLevel()
{
  Level level;
  level.floors.push_back({MT_Vector3(-12, -6, 0), MT_Vector3(-1, 6, 0)});
  level.floors.push_back({MT_Vector3(1, -6, 0), MT_Vector3(12, 0.5, 0)});
  level.boxes.push_back({MT_Vector3(1, 0.5, -0.5), MT_Vector3(12, 6, 1.2)});
  level.boxes.push_back({MT_Vector3(-12, -0.25, 0), MT_Vector3(-8, 0.25, 3)});
  return level;
}

KX_Navigation::Settings JumpSettings()
{
  KX_Navigation::Settings s = TestSettings();
  s.jump_links = true;
  s.jump_gap = 3.0f;
  s.jump_up = 1.5f;
  s.jump_drop = 4.0f;
  s.jump_spacing = 2.0f;
  return s;
}

struct FoundLinks {
  std::vector<KX_NavLinkInfo> links;
  size_t Count(const std::function<bool(const KX_NavLinkInfo &)> &which) const
  {
    return size_t(std::count_if(links.begin(), links.end(), which));
  }
};

FoundLinks LinksOf(KX_Navigation &navigation)
{
  FoundLinks found;
  KX_NavBuildForEachFoundLink(*navigation.GetBuildStateForTests(),
                              [&](uint32_t, const KX_NavLinkInfo &link) {
                                found.links.push_back(link);
                              });
  return found;
}

/* The build finds the jumps a level offers (2026-11-19, Jump Links Automatic): across the gap
 * both ways, up onto the platform and down from it (also across the gap), none over the 3 m
 * wall, none longer or higher than the limits, one per Spacing along a ledge; tighter limits
 * find fewer; the same links on any number of threads. */
TEST(KX_NavJumpLinks, TheBuildFindsTheJumpsALevelOffers)
{
  Level level = JumpLevel();
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, JumpSettings());
  navigation->Step(1.0f / 60.0f, nullptr);
  const FoundLinks found = LinksOf(*navigation);
  auto across = [](const KX_NavLinkInfo &l) {
    return std::fabs(float(l.end[2] - l.start[2])) < 0.3f && (l.start[0] < 0) != (l.end[0] < 0);
  };
  auto up = [](const KX_NavLinkInfo &l) { return float(l.end[2] - l.start[2]) > 0.8f; };
  auto down = [](const KX_NavLinkInfo &l) { return float(l.end[2] - l.start[2]) < -0.8f; };
  EXPECT_GE(found.Count([&](const KX_NavLinkInfo &l) { return across(l) && l.start[0] < 0; }), 2u);
  EXPECT_GE(found.Count([&](const KX_NavLinkInfo &l) { return across(l) && l.start[0] > 0; }), 2u);
  EXPECT_GE(found.Count([&](const KX_NavLinkInfo &l) { return up(l) && l.start[0] > 0; }), 2u);
  EXPECT_GE(found.Count([&](const KX_NavLinkInfo &l) { return down(l) && l.drop; }), 2u);
  /* Up across the gap onto the platform too, and down from it to A. */
  EXPECT_GE(found.Count([&](const KX_NavLinkInfo &l) { return up(l) && l.start[0] < 0; }), 1u);
  EXPECT_GE(found.Count([&](const KX_NavLinkInfo &l) { return down(l) && l.end[0] < 0; }), 1u);
  for (const KX_NavLinkInfo &l : found.links) {
    /* The gap is the space between the ledges (the 2 m gap: the jump from end to end is about
     * 3.25 m, the navmesh keeping 0.6 m from each ledge); up and down the platform's side: 0. */
    EXPECT_LE(l.gap, 3.0f + 1e-3f);
    if (across(l) || (l.start[0] < 0) != (l.end[0] < 0)) {
      EXPECT_NEAR(l.gap, 2.0f, 0.15f);
    }
    else {
      EXPECT_NEAR(l.gap, 0.0f, 0.15f);
    }
    EXPECT_LE(float(l.end[2] - l.start[2]), 0.4f + 1.5f + 0.05f);
    EXPECT_GE(float(l.end[2] - l.start[2]), -4.0f - 0.05f);
    EXPECT_EQ(l.drop, float(l.end[2] - l.start[2]) < -0.4f);
    /* Never over the wall (x -12..-8 at y 0). */
    EXPECT_FALSE(l.start[0] < -7.5f && (l.start[1] < 0) != (l.end[1] < 0))
        << l.start[0] << " " << l.start[1] << " -> " << l.end[0] << " " << l.end[1];
  }
  /* One per Spacing along a ledge: no two links of one kind start within it with ends alike
   * (in one tile: 12.8 m tiles from the origin, each tile thinned by itself). */
  for (size_t i = 0; i < found.links.size(); i++) {
    for (size_t j = i + 1; j < found.links.size(); j++) {
      const KX_NavLinkInfo &a = found.links[i], &b = found.links[j];
      const MT_Vector3 ds(a.start[0] - b.start[0], a.start[1] - b.start[1], 0.0f);
      if (a.drop == b.drop && std::fabs(float(a.end[2] - b.end[2])) < 0.5f &&
          float((a.end - b.end).length()) < 2.5f && (a.start[0] < 0) == (b.start[0] < 0) &&
          (a.start[1] < 0) == (b.start[1] < 0))
      {
        EXPECT_GE(float(ds.length()), 2.0f - 1e-3f);
      }
    }
  }
  /* The gap's tiles made again (Rebuild Area): the same links (a search judging the walk over
   * the links already there found each "about as short" and dropped it). */
  {
    Objects objects;
    KX_NavRequest rebuild;
    rebuild.object = &objects.Add(MT_Vector3(0, 0, 0));
    rebuild.command = KX_NavRequest::Command::RebuildArea;
    rebuild.destination = MT_Vector3(0, 0, 0);
    rebuild.offset = MT_Vector3(3, 3, 3);
    navigation->Submit(rebuild);
    const uint64_t searched =
        KX_NavBuildGetStats(*navigation->GetBuildState()).tiles_jump_searched;
    navigation->Step(1.0f / 60.0f, nullptr);
    navigation->Step(1.0f / 60.0f, nullptr);
    EXPECT_GT(KX_NavBuildGetStats(*navigation->GetBuildState()).tiles_jump_searched, searched);
    const FoundLinks again = LinksOf(*navigation);
    EXPECT_EQ(again.links.size(), found.links.size());
  }
  /* Tighter limits: a 1.8 m gap and 0.5 m up find no jump across and none up. */
  {
    KX_Navigation::Settings s = JumpSettings();
    s.jump_gap = 1.8f;
    s.jump_up = 0.5f;
    std::unique_ptr<KX_Navigation> tight = MakeNavigation(level, s);
    tight->Step(1.0f / 60.0f, nullptr);
    const FoundLinks few = LinksOf(*tight);
    EXPECT_EQ(few.Count(across), 0u);
    EXPECT_EQ(few.Count(up), 0u);
  }
  /* The same on 1 and 4 threads. */
  {
    LN_JobPool pool;
    pool.SetHelperCount(3);
    std::unique_ptr<KX_Navigation> threaded = MakeNavigation(level, JumpSettings());
    threaded->Step(1.0f / 60.0f, &pool);
    const FoundLinks again = LinksOf(*threaded);
    ASSERT_EQ(again.links.size(), found.links.size());
    for (size_t i = 0; i < found.links.size(); i++) {
      EXPECT_EQ(again.links[i].start, found.links[i].start);
      EXPECT_EQ(again.links[i].end, found.links[i].end);
    }
  }
  /* Every link's landing joined (Detour left the ends of links leaving a tile unjoined when the
   * tile had no room set aside: 6 of these 19). */
  EXPECT_EQ(KX_NavBuildCountUnjoinedLinks(*navigation->GetBuildState()), 0u);
  /* Off: none. */
  std::unique_ptr<KX_Navigation> off = MakeNavigation(level, TestSettings());
  off->Step(1.0f / 60.0f, nullptr);
  EXPECT_EQ(LinksOf(*off).links.size(), 0u);
}

/* A jump where walking is about as short is no link (Havok's rule): a 0.8 m ledge (x 0..10) with
 * a steep ramp up its face in the middle (x -1..0, y -1..1): walking round by the ramp is short
 * within about 2.7 m of it (no link starts there), long farther away (links). */
TEST(KX_NavJumpLinks, NoJumpWhereWalkingIsAboutAsShort)
{
  Level level;
  level.floors.push_back({MT_Vector3(-12, -10, 0), MT_Vector3(0, 10, 0)});
  level.boxes.push_back({MT_Vector3(0, -10, -0.5), MT_Vector3(10, 10, 0.8)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, JumpSettings());
  navigation->SetGeometrySourceForTests(
      [&level](const MT_Vector3 &min, const MT_Vector3 &max, KX_NavGeometry &g) {
        level.Collect(min, max, g);
        /* The ramp: 0.8 m up over 1 m (39 degrees, under the 45 the NPCs walk). */
        level.Quad(MT_Vector3(-1, -1, 0),
                   MT_Vector3(0, -1, 0.8f),
                   MT_Vector3(0, 1, 0.8f),
                   MT_Vector3(-1, 1, 0),
                   0,
                   g);
      });
  navigation->Step(1.0f / 60.0f, nullptr);
  const FoundLinks found = LinksOf(*navigation);
  auto at_ledge = [](const KX_NavLinkInfo &l) {
    return std::fabs(float(l.start[0])) < 1.0f && std::fabs(float(l.end[0])) < 1.5f;
  };
  EXPECT_EQ(found.Count([&](const KX_NavLinkInfo &l) {
              return at_ledge(l) && std::fabs(float(l.start[1])) < 2.0f;
            }),
            0u);
  EXPECT_GE(found.Count([&](const KX_NavLinkInfo &l) {
              return at_ledge(l) && !l.drop && std::fabs(float(l.start[1])) > 4.0f;
            }),
            4u);
  EXPECT_GE(found.Count([&](const KX_NavLinkInfo &l) {
              return at_ledge(l) && l.drop && std::fabs(float(l.start[1])) > 4.0f;
            }),
            4u);
}

/** Go To `goal` with `limits` (gap, up, drop; none: unlimited) set by Set Navigation Attribute;
 * true when it arrives within `seconds`. `r_link_kind`: the kind of a link it crossed. */
bool WalkJumpLevel(const MT_Vector3 &start,
                   const MT_Vector3 &goal,
                   const MT_Vector3 *limits,
                   const float seconds,
                   std::string *r_link_kind = nullptr,
                   float *r_lowest_over_platform = nullptr)
{
  Level level = JumpLevel();
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, JumpSettings());
  Objects objects;
  KX_GameObject &npc = objects.Add(start);
  if (limits != nullptr) {
    KX_NavRequest set;
    set.command = KX_NavRequest::Command::SetJumpLimits;
    set.object = &npc;
    set.enabled = true;
    set.offset = *limits;
    navigation->Submit(set);
  }
  for (int step = 0; step < int(seconds * 60.0f); step++) {
    navigation->Submit(GoTo(npc, goal, 4.0f));
    navigation->Step(1.0f / 60.0f, nullptr);
    KX_NavAgentInfo info;
    navigation->GetAgentInfo(&npc, info);
    if (info.link_started && r_link_kind != nullptr) {
      *r_link_kind = info.link_kind;
    }
    const MT_Vector3 p = npc.NodeGetWorldPosition();
    if (r_lowest_over_platform != nullptr && info.on_link && p.x() > 1.1f && p.y() > 0.55f) {
      *r_lowest_over_platform = std::min(*r_lowest_over_platform, float(p.z()));
    }
    if (info.arrived) {
      return true;
    }
  }
  return false;
}

/* NPCs take the links (2026-11-19): across the gap, up onto the platform, down from it; an NPC
 * whose jump limits are too short for a link never takes it (no path there), one within them
 * does. */
TEST(KX_NavJumpLinks, NpcsJumpWithinTheirLimits)
{
  std::string kind;
  EXPECT_TRUE(WalkJumpLevel(MT_Vector3(-5, -3, 0), MT_Vector3(5, -3, 0), nullptr, 12, &kind));
  EXPECT_EQ(kind, "jump");
  EXPECT_TRUE(WalkJumpLevel(MT_Vector3(5, -3, 0), MT_Vector3(5, 3, 1.2f), nullptr, 12));
  kind.clear();
  /* Down over its arc: above the platform until past its edge (a straight line went through
   * the corner). */
  float lowest = 1e9f;
  EXPECT_TRUE(WalkJumpLevel(
      MT_Vector3(5, 3, 1.2f), MT_Vector3(5, -3, 0), nullptr, 12, &kind, &lowest));
  EXPECT_EQ(kind, "drop");
  EXPECT_GT(lowest, 1.15f);
  EXPECT_LT(lowest, 1e8f);
  /* The 2 m gap: 1.8 m at most: never across. 2.2 m: across. */
  const MT_Vector3 short_gap(1.8f, 1.5f, 4.0f), long_gap(2.2f, 1.5f, 4.0f);
  EXPECT_FALSE(WalkJumpLevel(MT_Vector3(-5, -3, 0), MT_Vector3(5, -3, 0), &short_gap, 12));
  EXPECT_TRUE(WalkJumpLevel(MT_Vector3(-5, -3, 0), MT_Vector3(5, -3, 0), &long_gap, 12));
  /* 0.5 m up at most (above its 0.4 m climb): never onto the 1.2 m platform; it may drop. */
  const MT_Vector3 low(3.0f, 0.5f, 4.0f), no_drop(3.0f, 1.5f, 0.5f);
  EXPECT_FALSE(WalkJumpLevel(MT_Vector3(5, -3, 0), MT_Vector3(5, 3, 1.2f), &low, 10));
  EXPECT_TRUE(WalkJumpLevel(MT_Vector3(5, 3, 1.2f), MT_Vector3(5, -3, 0), &low, 12));
  EXPECT_FALSE(WalkJumpLevel(MT_Vector3(5, 3, 1.2f), MT_Vector3(5, -3, 0), &no_drop, 10));
}

/* Cost (2026-11-19): a 160 m field of 400 platforms 6.5 m wide with 1.5 m gaps, at 0..2 m
 * heights; the whole level built with and without Jump Links, 0 and 7 helpers. */
TEST(KX_NavJumpLinks, DISABLED_FindCost)
{
  Level level;
  uint32_t seed = 7;
  auto random = [&]() {
    seed = seed * 1664525u + 1013904223u;
    return float(seed >> 8) / float(1u << 24);
  };
  for (int y = 0; y < 20; y++) {
    for (int x = 0; x < 20; x++) {
      const float x0 = -80.0f + x * 8.0f, y0 = -80.0f + y * 8.0f;
      const float top = std::floor(random() * 4.0f) * 0.5f;
      level.boxes.push_back({MT_Vector3(x0, y0, -1.0f), MT_Vector3(x0 + 6.5f, y0 + 6.5f, top)});
    }
  }
  for (const bool jumps : {false, true}) {
    for (const uint32_t helpers : {0u, 7u}) {
      KX_Navigation::Settings s = jumps ? JumpSettings() : TestSettings();
      LN_JobPool pool;
      pool.SetHelperCount(helpers);
      std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, s);
      const auto t0 = std::chrono::steady_clock::now();
      navigation->Step(1.0f / 60.0f, helpers ? &pool : nullptr);
      const double ms =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
              .count();
      const KX_NavBuildStats stats = KX_NavBuildGetStats(*navigation->GetBuildState());
      printf("jump links %s, %u helpers: build %.1f ms, %llu links, %llu tiles searched\n",
             jumps ? "on " : "off",
             helpers,
             ms,
             (unsigned long long)stats.found_links,
             (unsigned long long)stats.tiles_jump_searched);
    }
  }
}

/** JumpLevel's floors, platform and wall as static Jolt boxes. */
void JumpLevelBodies(World &world, Objects &objects)
{
  world.StaticBox(objects.Add(MT_Vector3(-6.5f, 0, -0.5f)), MT_Vector3(5.5f, 6, 0.5f));
  world.StaticBox(objects.Add(MT_Vector3(6.5f, -2.75f, -0.5f)), MT_Vector3(5.5f, 3.25f, 0.5f));
  world.StaticBox(objects.Add(MT_Vector3(6.5f, 3.25f, 0.35f)), MT_Vector3(5.5f, 2.75f, 0.85f));
  world.StaticBox(objects.Add(MT_Vector3(-10, 0, 1.5f)), MT_Vector3(2, 0.25f, 1.5f));
  world.environment.GetPhysicsSystem()->OptimizeBroadPhase();
}

/* A Character's jump limits are its own (2026-11-19): its jump speed and gravity. 3 m/s (a
 * 0.46 m jump, 2.7 m far at a run) never takes the 2 m gap (a jump of 3.25 m from edge to edge
 * of the navmesh); 6 m/s does; Set Navigation Attribute's limits replace them. */
TEST(KX_NavJumpLinks, ACharacterJumpsAsItsJumpSpeedAllows)
{
  for (const int run : {0, 1, 2}) {
    const float jump_speed = run == 0 ? 3.0f : 6.0f;
    SCOPED_TRACE(run == 0 ? "3 m/s" : (run == 1 ? "6 m/s" : "6 m/s, limited to 1 m"));
    Level level = JumpLevel();
    std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, JumpSettings());
    Objects objects;
    World world;
    JumpLevelBodies(world, objects);
    navigation->SetPhysicsEnvironmentForTests(&world.environment);
    KX_GameObject &npc = objects.Add(MT_Vector3(-5, -3, 0.95f));
    JoltCharacter *character = world.Character(npc);
    ASSERT_NE(character, nullptr);
    character->SetJumpSpeed(jump_speed);
    if (run == 2) {
      KX_NavRequest set;
      set.command = KX_NavRequest::Command::SetJumpLimits;
      set.object = &npc;
      set.offset = MT_Vector3(1.0f, 1.5f, 4.0f);
      navigation->Submit(set);
    }
    bool arrived = false;
    for (int step = 0; step < 60 * 15 && !arrived; step++) {
      navigation->Submit(GoTo(npc, MT_Vector3(5, -3, 0), 3.0f));
      navigation->Step(1.0f / 60.0f, nullptr);
      world.Step({&npc});
      KX_NavAgentInfo info;
      ASSERT_TRUE(navigation->GetAgentInfo(&npc, info));
      arrived = info.arrived;
    }
    EXPECT_EQ(arrived, run == 1);
    if (run != 1) {
      EXPECT_LT(npc.NodeGetWorldPosition().x(), -1.0f);
    }
  }
}

/** A physics jump's walk: `npc` (a Character or a Dynamic body in `world`) goes to `goal`;
 * returns whether it arrived; what its crossings did. */
struct PhysicsWalk {
  bool arrived = false;
  int starts = 0, ends = 0, landed = 0;
  std::vector<MT_Vector3> start_positions;
  /* The highest it rose above where it took off, over every crossing; the steps on links. */
  float highest_rise = -1e9f;
  int link_steps = 0;
};
PhysicsWalk WalkByPhysics(KX_Navigation &navigation,
                          World &world,
                          KX_GameObject &npc,
                          const MT_Vector3 &goal,
                          const float seconds,
                          const bool dynamic,
                          const std::function<void(int, const KX_NavAgentInfo &)> &each = nullptr)
{
  PhysicsWalk walk;
  float takeoff = 0.0f;
  for (int step = 0; step < int(seconds * 60.0f) && !walk.arrived; step++) {
    KX_NavRequest request = GoTo(npc, goal, 3.0f);
    request.move_dynamic = dynamic;
    navigation.Submit(request);
    navigation.Step(1.0f / 60.0f, nullptr);
    world.Step({&npc});
    KX_NavAgentInfo info;
    EXPECT_TRUE(navigation.GetAgentInfo(&npc, info));
    const MT_Vector3 p = npc.NodeGetWorldPosition();
    if (info.link_started) {
      walk.starts++;
      walk.start_positions.push_back(p);
      takeoff = float(p.z());
    }
    if (info.on_link) {
      walk.link_steps++;
      walk.highest_rise = std::max(walk.highest_rise, float(p.z()) - takeoff);
    }
    if (info.link_ended) {
      walk.ends++;
      walk.landed += info.link_landed ? 1 : 0;
    }
    if (each) {
      each(step, info);
    }
    walk.arrived = info.arrived;
  }
  return walk;
}

/* Physics jumps (2026-11-19, Jumps: Physics): a Character jumps the links itself (launched,
 * steered through the air, landing on its own feet), across the gap, up onto the platform and
 * down again; it rises above the ledges it jumps between; each crossing ends landed on the
 * link's end. With Carried (the older files' setting) it crosses too. */
TEST(KX_NavJumpLinks, ACharacterJumpsByPhysics)
{
  for (const bool physics : {true, false}) {
    SCOPED_TRACE(physics ? "Physics" : "Carried");
    Level level = JumpLevel();
    KX_Navigation::Settings s = JumpSettings();
    s.physics_jumps = physics;
    std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, s);
    Objects objects;
    World world;
    JumpLevelBodies(world, objects);
    navigation->SetPhysicsEnvironmentForTests(&world.environment);
    KX_GameObject &npc = objects.Add(MT_Vector3(-5, -3, 0.95f));
    JoltCharacter *character = world.Character(npc);
    ASSERT_NE(character, nullptr);
    character->SetJumpSpeed(7.0f);
    const PhysicsWalk across = WalkByPhysics(
        *navigation, world, npc, MT_Vector3(5, -3, 0), 15, false);
    EXPECT_TRUE(across.arrived);
    EXPECT_EQ(across.starts, 1);
    EXPECT_EQ(across.landed, 1);
    EXPECT_GT(across.highest_rise, 0.3f);
    EXPECT_NEAR(npc.NodeGetWorldPosition().z(), 0.95f, 0.1f);
    const PhysicsWalk up = WalkByPhysics(
        *navigation, world, npc, MT_Vector3(5, 3, 1.2f), 15, false);
    EXPECT_TRUE(up.arrived);
    EXPECT_EQ(up.landed, 1);
    EXPECT_GT(up.highest_rise, 1.2f);
    EXPECT_NEAR(npc.NodeGetWorldPosition().z(), 1.2f + 0.95f, 0.1f);
    const PhysicsWalk down = WalkByPhysics(
        *navigation, world, npc, MT_Vector3(5, -3, 0), 15, false);
    EXPECT_TRUE(down.arrived);
    EXPECT_EQ(down.landed, 1);
    EXPECT_NEAR(npc.NodeGetWorldPosition().z(), 0.95f, 0.1f);
  }
}

/* A physics jump knocked off its way (2026-11-19): pushed back hard just after the launch, the
 * Character lands back where it took off: the crossing ends not landed, that link is closed to
 * it a while, and it jumps another one to its goal. */
TEST(KX_NavJumpLinks, AKnockedJumperFindsAnotherWay)
{
  Level level = JumpLevel();
  KX_Navigation::Settings s = JumpSettings();
  s.physics_jumps = true;
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, s);
  Objects objects;
  World world;
  JumpLevelBodies(world, objects);
  navigation->SetPhysicsEnvironmentForTests(&world.environment);
  KX_GameObject &npc = objects.Add(MT_Vector3(-5, -3, 0.95f));
  JoltCharacter *character = world.Character(npc);
  ASSERT_NE(character, nullptr);
  character->SetJumpSpeed(7.0f);
  int knocked_at = -1;
  const PhysicsWalk walk = WalkByPhysics(
      *navigation,
      world,
      npc,
      MT_Vector3(5, -3, 0),
      25,
      false,
      [&](const int step, const KX_NavAgentInfo &info) {
        if (info.link_started && knocked_at < 0) {
          knocked_at = step;
        }
        if (knocked_at >= 0 && step == knocked_at + 2) {
          /* Straight back west, harder than its run east. */
          character->AddImpulse(MT_Vector3(-9.0f, 0.0f, 0.0f), true);
        }
      });
  EXPECT_TRUE(walk.arrived);
  ASSERT_GE(walk.starts, 2);
  EXPECT_EQ(walk.ends, walk.starts);
  EXPECT_EQ(walk.landed, walk.ends - 1);
  /* Another link: its start more than a metre from the first. */
  const MT_Vector3 d = walk.start_positions[1] - walk.start_positions[0];
  EXPECT_GT(float(MT_Vector3(d.x(), d.y(), 0).length()), 1.0f);
  EXPECT_NEAR(npc.NodeGetWorldPosition().z(), 0.95f, 0.1f);
}

/* A body moved as Dynamic jumps by its velocity (2026-11-19): a ball across the gap. */
TEST(KX_NavJumpLinks, ADynamicBodyJumpsByItsVelocity)
{
  Level level = JumpLevel();
  KX_Navigation::Settings s = JumpSettings();
  s.physics_jumps = true;
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, s);
  Objects objects;
  World world;
  JumpLevelBodies(world, objects);
  navigation->SetPhysicsEnvironmentForTests(&world.environment);
  KX_GameObject &npc = objects.Add(MT_Vector3(-5, -3, 0.3f));
  world.Add(npc, new JPH::SphereShape(0.3f), JPH::EMotionType::Dynamic);
  const PhysicsWalk walk = WalkByPhysics(
      *navigation, world, npc, MT_Vector3(5, -3, 0), 15, true);
  EXPECT_TRUE(walk.arrived);
  EXPECT_EQ(walk.starts, 1);
  EXPECT_EQ(walk.landed, 1);
  EXPECT_GT(walk.highest_rise, 0.3f);
  EXPECT_NEAR(npc.NodeGetWorldPosition().z(), 0.3f, 0.1f);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vehicles (2026-11-19)
 * \{ */

/** A ship's shape in its own space: a deck 12 x 4 m, its top 1 m above its origin. */
Level ShipShape()
{
  Level ship;
  ship.boxes.push_back({MT_Vector3(-6, -2, -1), MT_Vector3(6, 2, 1)});
  return ship;
}

/** `object` placed at `position`, turned `yaw` degrees about up. */
void PlaceTurned(KX_GameObject &object, const MT_Vector3 &position, const float yaw)
{
  object.NodeSetWorldPosition(position);
  object.NodeSetGlobalOrientation(
      MT_Matrix3x3(MT_Quaternion(MT_Vector3(0, 0, 1), yaw * float(M_PI) / 180.0f)));
  object.NodeUpdateGS(0.0);
}

MT_Vector3 LocalOf(const KX_GameObject &frame, const MT_Vector3 &world)
{
  MT_Transform pose(frame.NodeGetWorldPosition(), frame.NodeGetWorldOrientation());
  MT_Transform inverse;
  inverse.invert(pose);
  return inverse(world);
}

MT_Vector3 WorldOf(const KX_GameObject &frame, const MT_Vector3 &local)
{
  return MT_Transform(frame.NodeGetWorldPosition(), frame.NodeGetWorldOrientation())(local);
}

/* An NPC on a moving, turning ship (2026-11-19): it stands on the ship (its frame), walks the
 * ship's own navmesh to a place on the deck while the ship sails 3 m/s and turns 18 degrees a
 * second, never off the deck, and arrives; standing there it goes along. A second ship of the
 * same shape shares the first's navmesh build. */
TEST(KX_NavVehicles, AnNpcWalksTheDeckOfAMovingShip)
{
  Level sea;
  sea.floors.push_back({MT_Vector3(-60, -60, -10), MT_Vector3(60, 60, -10)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(sea, TestSettings());
  Objects objects;
  KX_GameObject &ship = objects.Add(MT_Vector3(0, 0, 0));
  KX_GameObject &twin = objects.Add(MT_Vector3(0, 30, 0));
  const Level shape = ShipShape();
  navigation->SetVehiclesForTests({&ship, &twin});
  navigation->SetVehicleGeometryForTests([&shape](KX_GameObject *, KX_NavGeometry &g) {
    shape.Collect(MT_Vector3(0, 0, 0), MT_Vector3(0, 0, 0), g);
  });
  KX_GameObject &npc = objects.Add(WorldOf(ship, MT_Vector3(-4.5f, -0.5f, 1.0f)));
  const MT_Vector3 local_goal(4.5f, 0.5f, 1.0f);
  float yaw = 0.0f;
  MT_Vector3 place(0, 0, 0);
  bool arrived = false;
  int steps = 0;
  for (; steps < 60 * 15 && !arrived; steps++) {
    /* The ship sails on (logic moves it before the step). */
    place += MT_Vector3(std::cos(yaw * float(M_PI) / 180.0f), std::sin(yaw * float(M_PI) / 180.0f), 0) * 0.05f;
    yaw += 0.3f;
    PlaceTurned(ship, place, yaw);
    navigation->Submit(GoTo(npc, WorldOf(ship, local_goal), 2.0f));
    navigation->Step(1.0f / 60.0f, nullptr);
    npc.NodeUpdateGS(0.0);
    const MT_Vector3 local = LocalOf(ship, npc.NodeGetWorldPosition());
    ASSERT_LE(std::fabs(float(local.x())), 6.0f) << "step " << steps;
    ASSERT_LE(std::fabs(float(local.y())), 2.0f) << "step " << steps;
    ASSERT_NEAR(float(local.z()), 1.0f, 0.2f) << "step " << steps;
    KX_NavAgentInfo info;
    ASSERT_TRUE(navigation->GetAgentInfo(&npc, info));
    arrived = info.arrived;
  }
  EXPECT_TRUE(arrived);
  EXPECT_EQ(navigation->AgentFrame(&npc), &ship);
  EXPECT_EQ(navigation->VehicleTypeCountForTests(), 1u);
  const MT_Vector3 there = LocalOf(ship, npc.NodeGetWorldPosition());
  EXPECT_NEAR(float(there.x()), 4.5f, 0.5f);
  EXPECT_NEAR(float(there.y()), 0.5f, 0.5f);
  /* Standing there (no longer asked), it goes along for 5 s more. */
  for (int step = 0; step < 300; step++) {
    place += MT_Vector3(std::cos(yaw * float(M_PI) / 180.0f), std::sin(yaw * float(M_PI) / 180.0f), 0) * 0.05f;
    yaw += 0.3f;
    PlaceTurned(ship, place, yaw);
    navigation->Step(1.0f / 60.0f, nullptr);
    npc.NodeUpdateGS(0.0);
  }
  const MT_Vector3 later = LocalOf(ship, npc.NodeGetWorldPosition());
  EXPECT_NEAR(float(later.x()), float(there.x()), 0.05f);
  EXPECT_NEAR(float(later.y()), float(there.y()), 0.05f);
}

/** A root whose moves reach its children (the tests' own root tells them nothing changed). */
class MovingRootRelation : public SG_ParentRelation {
 public:
  bool UpdateChildCoordinates(SG_Node *child,
                              const SG_Node * /*parent*/,
                              bool &parentUpdated) override
  {
    if (child == nullptr) {
      return false;
    }
    child->SetWorldFromLocalTransform();
    parentUpdated = true;
    return true;
  }
  SG_ParentRelation *NewCopy() override
  {
    return new MovingRootRelation();
  }
};

/** A child of `parent` at `local` (a vehicle's part: a gangway's end, a cab's door); the parent's
 * moves reach it. */
KX_GameObject &AddChildObject(Objects &objects, KX_GameObject &parent, const MT_Vector3 &local)
{
  PositionedGameObject *parent_item = nullptr;
  for (PositionedGameObject &item : objects.items) {
    if (&item.object == &parent) {
      parent_item = &item;
    }
  }
  parent_item->node.SetParentRelation(new MovingRootRelation());
  objects.items.emplace_back();
  PositionedGameObject &child = objects.items.back();
  child.node.SetParentRelation(new KX_NormalParentRelation());
  parent_item->node.AddChild(&child.node);
  child.object.NodeSetLocalPosition(local);
  parent.NodeUpdateGS(0.0);
  return child.object;
}

/* Boarding a ship by its gangway (2026-11-19): the NPC's goal is on the deck; the ship is away
 * (the gangway's ends 23 m apart: closed): it walks to the gangway on the dock and waits there
 * (Waiting, at the dock's end), not arrived. The ship docks: it crosses (its frame the ship),
 * walks to its place on deck and arrives; the ship sails off with it aboard; back again, the
 * NPC goes ashore to a goal on the dock. */
TEST(KX_NavVehicles, AnNpcBoardsByTheGangwayWhenTheShipIsIn)
{
  Level dock;
  dock.floors.push_back({MT_Vector3(-20, -10, 0), MT_Vector3(0, 10, 0)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(dock, TestSettings());
  Objects objects;
  /* The ship: its deck's top at z 0 (its origin 1 m below), 16 m out (docked: 7 m). */
  KX_GameObject &ship = objects.Add(MT_Vector3(23, 0, -1));
  const Level shape = ShipShape();
  navigation->SetVehiclesForTests({&ship});
  navigation->SetVehicleGeometryForTests([&shape](KX_GameObject *, KX_NavGeometry &g) {
    shape.Collect(MT_Vector3(0, 0, 0), MT_Vector3(0, 0, 0), g);
  });
  KX_GameObject &shore_end = objects.Add(MT_Vector3(-0.6f, 0, 0));
  KX_GameObject &ship_end = AddChildObject(objects, ship, MT_Vector3(-5.4f, 0, 1));
  KX_NavLink gangway;
  gangway.object = &shore_end;
  gangway.end_object = &ship_end;
  gangway.kind = "gangway";
  gangway.both_ways = true;
  gangway.reach = 2.5f;
  gangway.user_id = 1;
  navigation->SetLinksForTests({gangway});
  KX_GameObject &npc = objects.Add(MT_Vector3(-8, 3, 0));
  const MT_Vector3 on_deck(3.0f, 1.0f, 1.0f);
  auto step = [&](const MT_Vector3 &goal) {
    navigation->Submit(GoTo(npc, goal, 3.0f));
    navigation->Step(1.0f / 60.0f, nullptr);
    npc.NodeUpdateGS(0.0);
    KX_NavAgentInfo info;
    EXPECT_TRUE(navigation->GetAgentInfo(&npc, info));
    return info;
  };
  /* Away: it waits at the gangway. */
  KX_NavAgentInfo info;
  for (int i = 0; i < 60 * 8; i++) {
    info = step(WorldOf(ship, on_deck));
  }
  EXPECT_TRUE(info.waiting);
  EXPECT_EQ(info.waiting_at, &shore_end);
  EXPECT_FALSE(info.arrived);
  EXPECT_LT((npc.NodeGetWorldPosition() - MT_Vector3(-0.6f, 0, 0)).length(), 1.0f);
  /* In: over the gangway, onto its place on deck. */
  float x = 23.0f;
  bool crossed = false;
  for (int i = 0; i < 60 * 15 && !info.arrived; i++) {
    x = std::max(x - 0.1f, 7.0f);
    PlaceTurned(ship, MT_Vector3(x, 0, -1), 0.0f);
    info = step(WorldOf(ship, on_deck));
    crossed = crossed || (info.link_started && info.link_object == &shore_end);
  }
  EXPECT_TRUE(crossed);
  EXPECT_TRUE(info.arrived);
  EXPECT_EQ(info.vehicle, &ship);
  const MT_Vector3 aboard = LocalOf(ship, npc.NodeGetWorldPosition());
  EXPECT_NEAR(float(aboard.x()), 3.0f, 0.5f);
  /* The ship sails 20 m out with it aboard (it stands at its place). */
  for (int i = 0; i < 200; i++) {
    x += 0.1f;
    PlaceTurned(ship, MT_Vector3(x, 0, -1), 0.0f);
    navigation->Step(1.0f / 60.0f, nullptr);
    npc.NodeUpdateGS(0.0);
  }
  const MT_Vector3 sailed = LocalOf(ship, npc.NodeGetWorldPosition());
  EXPECT_NEAR(float(sailed.x()), float(aboard.x()), 0.05f);
  EXPECT_NEAR(float(sailed.y()), float(aboard.y()), 0.05f);
  /* Ashore again: it waits at the ship's end while away, crosses when the ship is in. */
  const MT_Vector3 ashore(-10, -4, 0);
  for (int i = 0; i < 60 * 4; i++) {
    info = step(ashore);
  }
  EXPECT_TRUE(info.waiting);
  EXPECT_EQ(info.waiting_at, &ship_end);
  for (int i = 0; i < 60 * 20 && !info.arrived; i++) {
    x = std::max(x - 0.1f, 7.0f);
    PlaceTurned(ship, MT_Vector3(x, 0, -1), 0.0f);
    info = step(ashore);
  }
  EXPECT_TRUE(info.arrived);
  EXPECT_EQ(info.vehicle, nullptr);
  EXPECT_LT((npc.NodeGetWorldPosition() - ashore).length(), 0.5f);
}

/* Docking links (2026-11-19): no gangway; the ship comes in 0.5 m from the quay, its deck at the
 * quay's height. Moving, it has no docking links; standing still half a second, it gets them
 * ("step" links along its side facing the quay); an NPC on the quay with a goal on deck boards
 * by one. The ship moves off: its docking links go. */
TEST(KX_NavVehicles, AShipStandingAtTheQuayGetsDockingLinks)
{
  Level quay;
  quay.floors.push_back({MT_Vector3(-20, -10, 0), MT_Vector3(0, 10, 0)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(quay, TestSettings());
  Objects objects;
  KX_GameObject &ship = objects.Add(MT_Vector3(20, 0, -1));
  const Level shape = ShipShape();
  navigation->SetVehiclesForTests({&ship});
  navigation->SetVehicleGeometryForTests([&shape](KX_GameObject *, KX_NavGeometry &g) {
    shape.Collect(MT_Vector3(0, 0, 0), MT_Vector3(0, 0, 0), g);
  });
  navigation->SetLinksForTests({});
  KX_GameObject &npc = objects.Add(MT_Vector3(-8, 3, 0));
  auto docks = [&]() {
    size_t n = 0;
    for (const KX_NavFrameLink &l : navigation->GetFrameLinks()) {
      n += l.automatic ? 1 : 0;
    }
    return n;
  };
  /* Coming in (6.5 m out: its side 0.5 m from the quay's edge): none while it moves. */
  float x = 20.0f;
  for (int i = 0; i < 200; i++) {
    x = std::max(x - 0.1f, 6.5f);
    PlaceTurned(ship, MT_Vector3(x, 0, -1), 0.0f);
    navigation->Step(1.0f / 60.0f, nullptr);
    if (x > 6.5f) {
      EXPECT_EQ(docks(), 0u);
    }
  }
  /* Standing: docking links along its side (its 4 m side: one or two), on the quay's side. */
  EXPECT_GE(docks(), 1u);
  for (const KX_NavFrameLink &l : navigation->GetFrameLinks()) {
    EXPECT_LT(float(l.start.x()), 1.5f);
    EXPECT_LT(float(l.end.x()), 0.0f);
    EXPECT_NEAR(float(l.end.z()), float(l.start.z()), 0.3f);
  }
  /* Boarding by one. */
  KX_NavAgentInfo info;
  std::string kind;
  for (int i = 0; i < 60 * 15 && !info.arrived; i++) {
    navigation->Submit(GoTo(npc, WorldOf(ship, MT_Vector3(2, 1, 1)), 3.0f));
    navigation->Step(1.0f / 60.0f, nullptr);
    npc.NodeUpdateGS(0.0);
    ASSERT_TRUE(navigation->GetAgentInfo(&npc, info));
    if (info.link_started) {
      kind = info.link_kind;
    }
  }
  EXPECT_TRUE(info.arrived);
  EXPECT_EQ(info.vehicle, &ship);
  EXPECT_EQ(kind, "step");
  /* It casts off: its docking links go. */
  for (int i = 0; i < 10; i++) {
    x += 0.1f;
    PlaceTurned(ship, MT_Vector3(x, 0, -1), 0.0f);
    navigation->Step(1.0f / 60.0f, nullptr);
  }
  EXPECT_EQ(docks(), 0u);
}

/* A real Jolt vehicle (2026-11-19): a Kinematic ship body whose shape the navigation reads from
 * Jolt (CollectBodyTriangles, in the ship's space); a Character on its deck walks to a place on
 * deck while the ship moves 2 m/s (Jolt carries the Character: what it stands on moves). */
TEST(KX_NavVehicles, ACharacterWalksTheDeckOfARealJoltShip)
{
  Level sea;
  sea.floors.push_back({MT_Vector3(-60, -60, -10), MT_Vector3(60, 60, -10)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(sea, TestSettings());
  Objects objects;
  World world;
  KX_GameObject &ship = objects.Add(MT_Vector3(0, 0, 0));
  Body &hull = world.Add(
      ship, new JPH::BoxShape(JPH::Vec3(6.0f, 1.0f, 2.0f), 0.0f), JPH::EMotionType::Kinematic);
  hull.controller.SetMotionState(new KX_MotionState(ship.GetSGNode()));
  /* (Its object moves it: the step writes it to Jolt, as the game's Kinematic bodies.) */
  world.environment.AddController(&hull.controller);
  world.environment.GetPhysicsSystem()->OptimizeBroadPhase();
  navigation->SetPhysicsEnvironmentForTests(&world.environment);
  navigation->SetVehiclesForTests({&ship});
  KX_GameObject &npc = objects.Add(MT_Vector3(-4.5f, -0.5f, 1.95f));
  JoltCharacter *character = world.Character(npc);
  ASSERT_NE(character, nullptr);
  const MT_Vector3 local_goal(4.0f, 0.8f, 1.0f);
  float x = 0.0f;
  KX_NavAgentInfo info;
  for (int i = 0; i < 60 * 12 && !info.arrived; i++) {
    x += 2.0f / 60.0f;
    ship.NodeSetWorldPosition(MT_Vector3(x, 0, 0));
    ship.NodeUpdateGS(0.0);
    navigation->Submit(GoTo(npc, WorldOf(ship, local_goal), 2.0f));
    navigation->Step(1.0f / 60.0f, nullptr);
    world.Step({&npc, &ship});
    ASSERT_TRUE(navigation->GetAgentInfo(&npc, info));
  }
  EXPECT_TRUE(info.arrived);
  EXPECT_EQ(info.vehicle, &ship);
  EXPECT_EQ(navigation->VehicleTypeCountForTests(), 1u);
  const MT_Vector3 local = LocalOf(ship, npc.NodeGetWorldPosition());
  EXPECT_NEAR(float(local.x()), 4.0f, 0.5f);
  EXPECT_NEAR(float(local.y()), 0.8f, 0.5f);
  EXPECT_NEAR(float(local.z()), 1.95f, 0.15f);
}

/* A lift (2026-11-19): two floors 4 m apart, no stairs; a cab beside them with a door link to
 * each floor. An NPC on the lower floor with a goal upstairs: into the cab (at its floor), it
 * waits in the cab (the upper door's link closed), the cab goes up, it walks out to its goal. */
TEST(KX_NavVehicles, AnNpcTakesTheLiftToAnotherFloor)
{
  Level floors;
  floors.floors.push_back({MT_Vector3(-12, -6, 0), MT_Vector3(0, 6, 0)});
  floors.floors.push_back({MT_Vector3(-12, -6, 4), MT_Vector3(0, 6, 4)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(floors, TestSettings());
  Objects objects;
  KX_GameObject &cab = objects.Add(MT_Vector3(1.6f, 0, 0));
  Level cab_shape;
  cab_shape.boxes.push_back({MT_Vector3(-1.2f, -1.2f, -0.5f), MT_Vector3(1.2f, 1.2f, 0)});
  navigation->SetVehiclesForTests({&cab});
  navigation->SetVehicleGeometryForTests([&cab_shape](KX_GameObject *, KX_NavGeometry &g) {
    cab_shape.Collect(MT_Vector3(0, 0, 0), MT_Vector3(0, 0, 0), g);
  });
  KX_GameObject &door1 = objects.Add(MT_Vector3(-0.6f, 0, 0));
  KX_GameObject &door2 = objects.Add(MT_Vector3(-0.6f, 0, 4));
  KX_GameObject &inside = AddChildObject(objects, cab, MT_Vector3(-0.9f, 0, 0));
  KX_NavLink lower, upper;
  lower.object = &door1;
  lower.end_object = &inside;
  lower.kind = "door";
  lower.reach = 1.5f;
  lower.user_id = 1;
  upper = lower;
  upper.object = &door2;
  upper.user_id = 2;
  navigation->SetLinksForTests({lower, upper});
  KX_GameObject &npc = objects.Add(MT_Vector3(-8, 3, 0));
  const MT_Vector3 goal(-8, -3, 4);
  KX_NavAgentInfo info;
  bool waited_in_cab = false;
  float cab_z = 0.0f;
  for (int i = 0; i < 60 * 30 && !info.arrived; i++) {
    navigation->Submit(GoTo(npc, goal, 3.0f));
    navigation->Step(1.0f / 60.0f, nullptr);
    npc.NodeUpdateGS(0.0);
    ASSERT_TRUE(navigation->GetAgentInfo(&npc, info));
    if (info.waiting && info.vehicle == &cab) {
      waited_in_cab = true;
    }
    /* The lift's logic: up once someone waits in it. */
    if (waited_in_cab && cab_z < 4.0f) {
      cab_z = std::min(cab_z + 0.05f, 4.0f);
      PlaceTurned(cab, MT_Vector3(1.6f, 0, cab_z), 0.0f);
    }
  }
  EXPECT_TRUE(waited_in_cab);
  EXPECT_TRUE(info.arrived);
  EXPECT_EQ(info.vehicle, nullptr);
  EXPECT_LT((npc.NodeGetWorldPosition() - goal).length(), 0.5f);
}

/* Many links leaving a tile all land (2026-11-19: Detour joins a link's landing with room of the
 * start's tile set aside only for link ends inside it; a tile with many links leaving ran out
 * and left some unjoined). Twenty ladders, one way, from a floor west of the tiles' border
 * (x 0) up onto a floor east of it, and twenty back both ways, all joined. */
TEST(KX_NavJumpLinks, ManyLinksLeavingATileAllLand)
{
  Level level;
  level.floors.push_back({MT_Vector3(-10, -10, 0), MT_Vector3(-1, 10, 0)});
  level.floors.push_back({MT_Vector3(1, -10, 1.5f), MT_Vector3(10, 10, 1.5f)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  std::vector<KX_NavLink> links;
  for (int i = 0; i < 20; i++) {
    const float y = -9.0f + float(i) * 0.9f;
    KX_NavLink up;
    up.start = MT_Vector3(-1.5f, y, 0);
    up.end = MT_Vector3(1.5f, y, 1.5f);
    up.kind = "ladder";
    up.both_ways = false;
    up.user_id = uint32_t(1 + i);
    links.push_back(up);
    KX_NavLink back = up;
    back.start = MT_Vector3(1.5f, y + 0.4f, 1.5f);
    back.end = MT_Vector3(-1.5f, y + 0.4f, 0);
    back.both_ways = true;
    back.user_id = uint32_t(100 + i);
    links.push_back(back);
  }
  navigation->SetLinksForTests(links);
  navigation->Step(1.0f / 60.0f, nullptr);
  navigation->Step(1.0f / 60.0f, nullptr);
  EXPECT_EQ(KX_NavBuildCountUnjoinedLinks(*navigation->GetBuildState()), 0u);
}

/* A hand-placed jump link both ways is judged each way (2026-11-19): an NPC that cannot jump up
 * takes it down only. The level of the platform without the gap: a link from B up onto the
 * platform. */
TEST(KX_NavJumpLinks, AHandPlacedJumpIsJudgedEachWay)
{
  for (const bool limited : {false, true}) {
    SCOPED_TRACE(limited ? "0.2 m up at most" : "no limits");
    for (const bool going_up : {true, false}) {
      SCOPED_TRACE(going_up ? "up" : "down");
      Level level;
      level.floors.push_back({MT_Vector3(-6, -6, 0), MT_Vector3(6, 0.5, 0)});
      level.boxes.push_back({MT_Vector3(-6, 0.5, -0.5), MT_Vector3(6, 6, 1.2)});
      std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
      KX_NavLink link;
      link.start = MT_Vector3(0, -0.2f, 0);
      link.end = MT_Vector3(0, 1.4f, 1.2f);
      link.kind = "jump";
      link.both_ways = true;
      link.user_id = 1;
      navigation->SetLinksForTests({link});
      Objects objects;
      KX_GameObject &npc = objects.Add(going_up ? MT_Vector3(0, -4, 0) : MT_Vector3(0, 4, 1.2f));
      if (limited) {
        KX_NavRequest set;
        set.command = KX_NavRequest::Command::SetJumpLimits;
        set.object = &npc;
        set.enabled = true;
        set.offset = MT_Vector3(3.0f, 0.2f, 4.0f);
        navigation->Submit(set);
      }
      const MT_Vector3 goal = going_up ? MT_Vector3(0, 4, 1.2f) : MT_Vector3(0, -4, 0);
      bool arrived = false;
      for (int step = 0; step < 60 * 10 && !arrived; step++) {
        navigation->Submit(GoTo(npc, goal, 4.0f));
        navigation->Step(1.0f / 60.0f, nullptr);
        KX_NavAgentInfo info;
        navigation->GetAgentInfo(&npc, info);
        arrived = info.arrived;
      }
      EXPECT_EQ(arrived, !(limited && going_up));
    }
  }
}

/* A horde sharing one search (8+ NPCs after one goal) never takes a one-way link the wrong way
 * (2026-11-19: the shared search runs from the goal; a drop reversed is a jump up): NPCs whose
 * way crosses a link search their own. Ten NPCs on B go to the platform's top: up-jumps only,
 * every one arrives on top; ten on top go down to B. */
TEST(KX_NavJumpLinks, AHordeNeverTakesALinkTheWrongWay)
{
  for (const bool going_up : {true, false}) {
    SCOPED_TRACE(going_up ? "up" : "down");
    Level level = JumpLevel();
    std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, JumpSettings());
    Objects objects;
    std::vector<KX_GameObject *> npcs;
    /* Some right at the starts of the links their way takes (the jumps' on B, the drops' on the
     * platform): they start crossing at once. (Given the shared way, a link the wrong way round,
     * Detour finds no corners over it: they stood at the ledge until its topology repair, up to
     * 30 steps later.) The rest farther back. */
    const std::vector<float> beside = going_up ? std::vector<float>{1.85f, 4.3f, 6.74f, 9.19f} :
                                                 std::vector<float>{3.35f, 5.35f, 7.35f, 9.35f};
    for (const float x : beside) {
      npcs.push_back(&objects.Add(MT_Vector3(x, going_up ? -0.45f : 1.45f, going_up ? 0 : 1.2f)));
    }
    for (int i = int(beside.size()); i < 10; i++) {
      const float x = 2.6f + float(i - int(beside.size())) * 1.5f;
      npcs.push_back(
          &objects.Add(MT_Vector3(x, going_up ? -2.0f : 2.8f, going_up ? 0.0f : 1.2f)));
    }
    const MT_Vector3 goal = going_up ? MT_Vector3(8, 4, 1.2f) : MT_Vector3(8, -4, 0);
    /* The navmesh built and settled first: right after a build every NPC checks its way near
     * the new tiles and searches its own (that hid a wrong shared way). */
    for (int step = 0; step < 30; step++) {
      navigation->Step(1.0f / 60.0f, nullptr);
    }
    int arrived = 0;
    std::vector<int> first_link(npcs.size(), -1);
    for (int step = 0; step < 60 * 15 && arrived < 10; step++) {
      arrived = 0;
      for (KX_GameObject *npc : npcs) {
        KX_NavRequest request = GoTo(*npc, goal, 4.0f);
        request.arrive_distance = 1.5f;
        navigation->Submit(request);
      }
      navigation->Step(1.0f / 60.0f, nullptr);
      for (size_t i = 0; i < npcs.size(); i++) {
        KX_NavAgentInfo info;
        navigation->GetAgentInfo(npcs[i], info);
        arrived += info.arrived ? 1 : 0;
        /* Never on a link going the wrong way. */
        if (info.on_link) {
          EXPECT_EQ(info.link_kind == "drop", !going_up);
        }
        if (info.link_started && first_link[i] < 0) {
          first_link[i] = step;
        }
      }
    }
    EXPECT_EQ(arrived, 10);
    for (size_t i = 0; i < beside.size(); i++) {
      EXPECT_GE(first_link[i], 0) << "NPC " << i;
      EXPECT_LE(first_link[i], 4) << "NPC " << i;
    }
    /* (The ten searched as one: the shared search ran.) */
    EXPECT_GE(navigation->GetSharedSearchCount(), 1u);
  }
}

/** \} */

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

/* -------------------------------------------------------------------- */
/** \name A world that changes (phase 6, 2026-11-19)
 * \{ */

/** A navigation whose level is a real Jolt world's static bodies (as the game reads it), with the
 * physics' notes of changed pieces. */
std::unique_ptr<KX_Navigation> MakeJoltNavigation(World &world)
{
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
            [](const std::string &) { return 0; },
            [nav](const KX_GameObject *object) { return nav->IsExcludedFromLevel(object); },
            g);
      });
  navigation->SetPhysicsEnvironmentForTests(physics);
  return navigation;
}

/** A gate slid aside while playing (a static body moved by the game, never added or removed):
 * nothing is rebuilt while it slides, its tiles are once it came to rest, where it was and where
 * it now stands; the NPC walks through the opening, never through the gate's new place. */
TEST(KX_Navigation, AMovedLevelPieceIsBuiltWhereItComesToRest)
{
  Objects objects;
  World world;
  world.StaticBox(objects.Add(MT_Vector3(0, 0, -0.5f)), MT_Vector3(10, 10, 0.5f));
  /* Right across the floor: the far side cannot be reached. */
  Body &gate = world.StaticBox(objects.Add(MT_Vector3(0, 0, 1)), MT_Vector3(10, 0.5f, 1));
  world.environment.GetPhysicsSystem()->OptimizeBroadPhase();
  std::unique_ptr<KX_Navigation> navigation = MakeJoltNavigation(world);
  KX_GameObject &npc = objects.Add(MT_Vector3(-6, -5, 0.5f));
  const MT_Vector3 goal(-6, 5, 0);
  for (int step = 0; step < 120; step++) {
    navigation->Submit(GoTo(npc, goal));
    navigation->Step(1.0f / 60.0f, nullptr);
  }
  KX_NavAgentInfo info;
  ASSERT_TRUE(navigation->GetAgentInfo(&npc, info));
  EXPECT_FALSE(info.path_complete);
  KX_NavMesh *navmesh = navigation->GetSceneNavMesh();
  ASSERT_NE(navmesh, nullptr);
  const uint64_t built_before = KX_NavBuildGetStats(*navigation->GetBuildState()).tiles_built;
  /* It slides 14 m east over 28 steps (now x 4 to 24: an opening west of x 4). */
  for (int step = 1; step <= 28; step++) {
    gate.controller.SetPosition(MT_Vector3(0.5f * float(step), 0, 1));
    navigation->Submit(GoTo(npc, goal));
    navigation->Step(1.0f / 60.0f, nullptr);
    EXPECT_EQ(KX_NavBuildGetStats(*navigation->GetBuildState()).tiles_built, built_before)
        << "rebuilt while sliding, step " << step;
  }
  bool arrived = false, rebuilt = false;
  for (int step = 0; step < 60 * 12 && !arrived; step++) {
    navigation->Submit(GoTo(npc, goal));
    navigation->Step(1.0f / 60.0f, nullptr);
    if (KX_NavBuildGetStats(*navigation->GetBuildState()).tiles_built > built_before && !rebuilt) {
      rebuilt = true;
      /* Once it stood still kStillSteps steps (the notes come a step late at most). */
      EXPECT_LE(step, KX_Navigation::kStillSteps + 2);
      EXPECT_GE(step, KX_Navigation::kStillSteps - 2);
    }
    const MT_Vector3 p = npc.NodeGetWorldPosition();
    if (std::fabs(p.y()) < 0.6f) {
      EXPECT_LT(p.x(), 3.7f) << "through the gate's new place, step " << step;
    }
    ASSERT_TRUE(navigation->GetAgentInfo(&npc, info));
    arrived = info.arrived;
  }
  EXPECT_TRUE(rebuilt);
  EXPECT_TRUE(arrived);
  EXPECT_NEAR(npc.NodeGetWorldPosition().y(), 5.0f, 0.35f);
}

/** An NPC whose object has a static body of its own (a plain mesh is Static by default) is no
 * floor: the navmesh under it is the floor's, and walking (its static body moving every step)
 * rebuilds nothing. */
TEST(KX_Navigation, AnNpcsOwnStaticBodyIsNoFloor)
{
  Objects objects;
  World world;
  world.StaticBox(objects.Add(MT_Vector3(0, 0, -0.5f)), MT_Vector3(10, 10, 0.5f));
  KX_GameObject &npc = objects.Add(MT_Vector3(-5, 0, 0.5f));
  Body &body = world.StaticBox(npc, MT_Vector3(0.5f, 0.5f, 0.5f));
  world.environment.GetPhysicsSystem()->OptimizeBroadPhase();
  std::unique_ptr<KX_Navigation> navigation = MakeJoltNavigation(world);
  uint64_t built_after_start = 0;
  for (int step = 0; step < 60 * 7; step++) {
    navigation->Submit(GoTo(npc, MT_Vector3(5, 0, 0), 2.0f));
    navigation->Step(1.0f / 60.0f, nullptr);
    /* The game's sync writes the moved object's pose to its static body. */
    body.controller.SetPosition(npc.NodeGetWorldPosition());
    if (step == 30) {
      built_after_start = KX_NavBuildGetStats(*navigation->GetBuildState()).tiles_built;
    }
  }
  EXPECT_EQ(KX_NavBuildGetStats(*navigation->GetBuildState()).tiles_built, built_after_start);
  EXPECT_NEAR(npc.NodeGetWorldPosition().x(), 5.0f, 0.4f);
  /* The floor's surface where it started: no hole and no step. */
  KX_NavMesh *navmesh = navigation->GetSceneNavMesh();
  ASSERT_NE(navmesh, nullptr);
  MT_Vector3 point;
  ASSERT_TRUE(navmesh->NearestPoint(MT_Vector3(-5, 0, 0), point));
  EXPECT_NEAR(point.x(), -5.0f, 0.05f);
  EXPECT_NEAR(point.z(), 0.0f, 0.15f);
}

/** Links that come, go and move while playing (a ladder added, a bridge destroyed, a lift that
 * stops elsewhere): only the tiles they start in are made again, from their layers. */
TEST(KX_Navigation, LinksThatComeGoAndMoveChangeTheWay)
{
  Level level;
  level.floors.push_back({MT_Vector3(-10, -10, 0), MT_Vector3(-1, 10, 0)});
  level.floors.push_back({MT_Vector3(1, -10, 1.5f), MT_Vector3(10, 10, 1.5f)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  Objects objects;
  KX_GameObject &npc = objects.Add(MT_Vector3(-6, 3, 0));
  KX_NavAgentInfo info;
  auto walk = [&](const MT_Vector3 &goal, const int steps) {
    for (int step = 0; step < steps; step++) {
      navigation->Submit(GoTo(npc, goal));
      navigation->Step(1.0f / 60.0f, nullptr);
      if (navigation->GetAgentInfo(&npc, info) && info.arrived) {
        return true;
      }
    }
    return false;
  };
  /* No link: the upper floor cannot be reached. */
  EXPECT_FALSE(walk(MT_Vector3(6, -3, 1.5f), 120));
  EXPECT_FALSE(info.path_complete);
  /* A ladder comes. */
  KX_GameObject &ladder = objects.Add(MT_Vector3(-1.5f, 0, 0));
  KX_NavLink link;
  link.object = &ladder;
  link.start = MT_Vector3(-1.5f, 0, 0);
  link.end = MT_Vector3(1.5f, 0, 1.5f);
  link.kind = "ladder";
  link.user_id = 7;
  const uint64_t relinked = KX_NavBuildGetStats(*navigation->GetBuildState()).tiles_relinked;
  navigation->SetLinksForTests({link});
  EXPECT_TRUE(walk(MT_Vector3(6, -3, 1.5f), 60 * 20));
  EXPECT_GT(KX_NavBuildGetStats(*navigation->GetBuildState()).tiles_relinked, relinked);
  EXPECT_NEAR(npc.NodeGetWorldPosition().z(), 1.5f, 0.1f);
  /* It goes: back down cannot be reached any more. */
  navigation->SetLinksForTests({});
  EXPECT_FALSE(walk(MT_Vector3(-6, 3, 0), 60 * 4));
  EXPECT_NEAR(npc.NodeGetWorldPosition().z(), 1.5f, 0.1f);
  /* It comes back elsewhere (8 m north): the way down takes it there. */
  link.start = MT_Vector3(-1.5f, 8, 0);
  link.end = MT_Vector3(1.5f, 8, 1.5f);
  navigation->SetLinksForTests({link});
  bool started_north = false;
  for (int step = 0; step < 60 * 20 && !info.arrived; step++) {
    navigation->Submit(GoTo(npc, MT_Vector3(-6, 3, 0)));
    navigation->Step(1.0f / 60.0f, nullptr);
    ASSERT_TRUE(navigation->GetAgentInfo(&npc, info));
    if (info.link_started) {
      started_north = npc.NodeGetWorldPosition().y() > 6.5f;
      EXPECT_EQ(info.link_kind, "ladder");
      EXPECT_EQ(info.link_object, &ladder);
    }
  }
  EXPECT_TRUE(started_north);
  EXPECT_TRUE(info.arrived);
  EXPECT_NEAR(npc.NodeGetWorldPosition().z(), 0.0f, 0.1f);
}

/** An NPC that began to cross a link finishes the crossing when the links change under it (its
 * link went: it lands at the link's end, its kind and object as they were). */
TEST(KX_Navigation, ACrossingKeepsItsLinkWhenTheLinksChange)
{
  Level level;
  level.floors.push_back({MT_Vector3(-10, -10, 0), MT_Vector3(-1, 10, 0)});
  level.floors.push_back({MT_Vector3(1, -10, 1.5f), MT_Vector3(10, 10, 1.5f)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  Objects objects;
  KX_GameObject &bridge = objects.Add(MT_Vector3(-1.5f, 0, 0));
  KX_NavLink link;
  link.object = &bridge;
  link.start = MT_Vector3(-1.5f, 0, 0);
  link.end = MT_Vector3(1.5f, 0, 1.5f);
  link.kind = "jump";
  link.duration = 0.5f;
  link.user_id = 3;
  navigation->SetLinksForTests({link});
  KX_GameObject &npc = objects.Add(MT_Vector3(-6, 0, 0));
  KX_NavAgentInfo info;
  bool started = false;
  int crossing_steps = 0;
  for (int step = 0; step < 60 * 20; step++) {
    navigation->Submit(GoTo(npc, MT_Vector3(6, 0, 1.5f)));
    navigation->Step(1.0f / 60.0f, nullptr);
    ASSERT_TRUE(navigation->GetAgentInfo(&npc, info));
    if (info.link_started) {
      started = true;
      /* The bridge goes the step it was taken. */
      navigation->SetLinksForTests({});
    }
    else if (started && info.on_link) {
      crossing_steps++;
      EXPECT_EQ(info.link_object, &bridge);
      EXPECT_EQ(info.link_kind, "jump");
    }
    if (started && !info.on_link) {
      break;
    }
  }
  ASSERT_TRUE(started);
  EXPECT_GE(crossing_steps, 25);
  EXPECT_NEAR(npc.NodeGetWorldPosition().x(), 1.5f, 0.3f);
  EXPECT_NEAR(npc.NodeGetWorldPosition().z(), 1.5f, 0.1f);
}

/** Avoid Physics Objects: a crate dropped in a corridor is avoided while it falls and carved
 * once Jolt puts it to sleep (paths go round it); a small one is not; with the setting off,
 * nothing. Woken and pushed away, the corridor is free again. */
TEST(KX_Navigation, APhysicsObjectAtRestIsCarved)
{
  for (const bool on : {true, false}) {
    Level level;
    level.floors.push_back({MT_Vector3(-10, -10, 0), MT_Vector3(10, 10, 0)});
    std::unique_ptr<KX_Navigation> navigation = std::make_unique<KX_Navigation>(nullptr);
    KX_Navigation::Settings settings = TestSettings();
    settings.avoid_physics = on;
    settings.physics_min_size = 0.5f;
    navigation->SetSettings(settings);
    navigation->SetGeometrySourceForTests(
        [&level](const MT_Vector3 &min, const MT_Vector3 &max, KX_NavGeometry &g) {
          level.Collect(min, max, g);
        });
    Objects objects;
    World world;
    world.StaticBox(objects.Add(MT_Vector3(0, 0, -0.5f)), MT_Vector3(10, 10, 0.5f));
    KX_GameObject &crate = objects.Add(MT_Vector3(0, 0, 2.0f));
    Body &crate_body = world.Add(
        crate, new JPH::BoxShape(JPH::Vec3(0.6f, 0.6f, 0.6f)), JPH::EMotionType::Dynamic);
    KX_GameObject &pebble = objects.Add(MT_Vector3(4, 0, 2.0f));
    world.Add(pebble, new JPH::BoxShape(JPH::Vec3(0.1f, 0.1f, 0.1f)), JPH::EMotionType::Dynamic);
    world.environment.GetPhysicsSystem()->OptimizeBroadPhase();
    navigation->SetPhysicsEnvironmentForTests(&world.environment);
    navigation->SetPhysicsObjectsForTests({&crate, &pebble});
    EXPECT_EQ(navigation->IsPhysicsObstacle(&crate), on);
    EXPECT_FALSE(navigation->IsPhysicsObstacle(&pebble));
    bool asleep = false;
    for (int step = 0; step < 60 * 6 && !asleep; step++) {
      navigation->Step(1.0f / 60.0f, nullptr);
      world.Step({&crate, &pebble});
      asleep = crate.GetPhysicsController()->IsAsleep();
    }
    ASSERT_TRUE(asleep);
    navigation->Step(1.0f / 60.0f, nullptr);
    navigation->Step(1.0f / 60.0f, nullptr);
    KX_NavMesh *navmesh = navigation->GetSceneNavMesh();
    ASSERT_NE(navmesh, nullptr);
    float fraction = 0.0f;
    MT_Vector3 hit, normal;
    /* Through the crate (x -0.6..0.6, the agents' 0.3 m beside it). */
    const bool blocked = navmesh->Raycast(
        MT_Vector3(-5, 0, 0), MT_Vector3(-1.0f, 0, 0) + MT_Vector3(2.0f, 0, 0), fraction, hit,
        normal);
    EXPECT_EQ(blocked, on);
    if (on) {
      EXPECT_NEAR(hit.x(), -0.9f, 0.15f);
    }
    /* The pebble never. */
    EXPECT_FALSE(
        navmesh->Raycast(MT_Vector3(2.5f, 0, 0), MT_Vector3(5.5f, 0, 0), fraction, hit, normal));
    if (!on) {
      continue;
    }
    /* Woken and slid away: carved no more. */
    crate_body.controller.SetLinearVelocity(MT_Vector3(0, 12, 0), false);
    for (int step = 0; step < 60 * 3; step++) {
      navigation->Step(1.0f / 60.0f, nullptr);
      world.Step({&crate, &pebble});
    }
    EXPECT_GT(std::fabs(crate.NodeGetWorldPosition().y()), 3.0f);
    EXPECT_FALSE(
        navmesh->Raycast(MT_Vector3(-5, 0, 0), MT_Vector3(1, 0, 0), fraction, hit, normal));
  }
}

/** A wall appearing ahead of an NPC on its way, near or far (the 2026-11-19 probes): it walks
 * round it through the gap, never through it, never standing still. */
TEST(KX_Navigation, AWallAppearingAheadIsWalkedRound)
{
  for (const float wall_y : {-120.0f, 130.0f}) {
    Level level;
    level.floors.push_back({MT_Vector3(-6, -150, 0), MT_Vector3(6, 150, 0)});
    /* The wall leaves a gap at x 4..6. */
    level.boxes.push_back({MT_Vector3(-6, wall_y - 0.5f, 0), MT_Vector3(4, wall_y + 0.5f, 2)});
    level.boxes.back().enabled = false;
    std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
    Objects objects;
    KX_GameObject &npc = objects.Add(MT_Vector3(-3, -145, 1));
    const MT_Vector3 goal(-3, 145, 0);
    bool arrived = false;
    int still = 0, longest_still = 0;
    MT_Vector3 last = npc.NodeGetWorldPosition();
    for (int step = 0; step < 60 * 120 && !arrived; step++) {
      if (step == 60) {
        level.boxes.back().enabled = true;
        navigation->NoteStaticChanged(MT_Vector3(-6, wall_y - 0.5f, 0),
                                      MT_Vector3(4, wall_y + 0.5f, 2));
      }
      navigation->Submit(GoTo(npc, goal, 8.0f));
      navigation->Step(1.0f / 60.0f, nullptr);
      const MT_Vector3 p = npc.NodeGetWorldPosition();
      if (step > 60 && std::fabs(p.y() - wall_y) < 0.6f) {
        EXPECT_GT(p.x(), 3.7f) << "through the wall at y " << wall_y;
      }
      KX_NavAgentInfo info;
      ASSERT_TRUE(navigation->GetAgentInfo(&npc, info));
      arrived = info.arrived;
      /* (Arrived, it stands: that step is no stand-still on the way.) */
      still = !arrived && (p - last).length() < 1e-4f ? still + 1 : 0;
      longest_still = std::max(longest_still, still);
      last = p;
    }
    EXPECT_TRUE(arrived) << "wall at y " << wall_y;
    EXPECT_EQ(longest_still, 0) << "wall at y " << wall_y;
  }
}

/** A gate opening a much shorter way while an NPC walks the long way round, near it or far from
 * its whole way (the 2026-11-19 probes): it turns and takes the gate. */
TEST(KX_Navigation, AGateOpeningAShorterWayIsTaken)
{
  struct Case {
    MT_Vector3 start, goal;
    int open_step;
  };
  for (const Case &c : {Case{MT_Vector3(-40, -10, 1), MT_Vector3(-40, 40, 0), 180},
                        Case{MT_Vector3(10, -10, 1), MT_Vector3(-40, 45, 0), 60}})
  {
    float seconds[2] = {0.0f, 0.0f};
    float farthest_x[2] = {-1e9f, -1e9f};
    for (const bool open : {false, true}) {
      Level level;
      level.floors.push_back({MT_Vector3(-60, -20, 0), MT_Vector3(60, 50, 0)});
      level.boxes.push_back({MT_Vector3(-60, -0.5f, 0), MT_Vector3(-42, 0.5f, 2)});
      level.boxes.push_back({MT_Vector3(-38, -0.5f, 0), MT_Vector3(54, 0.5f, 2)});
      /* The gate, closed. The long way round: the gap at x 54..60. */
      level.boxes.push_back({MT_Vector3(-42, -0.5f, 0), MT_Vector3(-38, 0.5f, 2)});
      std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
      Objects objects;
      KX_GameObject &npc = objects.Add(c.start);
      bool arrived = false;
      int step = 0;
      for (; step < 60 * 120 && !arrived; step++) {
        if (open && step == c.open_step) {
          level.boxes.back().enabled = false;
          navigation->NoteStaticChanged(MT_Vector3(-42, -0.5f, 0), MT_Vector3(-38, 0.5f, 2));
        }
        navigation->Submit(GoTo(npc, c.goal, 8.0f));
        navigation->Step(1.0f / 60.0f, nullptr);
        farthest_x[open] = std::max(farthest_x[open], float(npc.NodeGetWorldPosition().x()));
        KX_NavAgentInfo info;
        ASSERT_TRUE(navigation->GetAgentInfo(&npc, info));
        arrived = info.arrived;
      }
      EXPECT_TRUE(arrived);
      seconds[open] = float(step) / 60.0f;
    }
    EXPECT_GT(farthest_x[false], 54.0f);
    EXPECT_LT(farthest_x[true], 30.0f);
    EXPECT_LT(seconds[true], seconds[false] - 3.0f);
  }
}

/** The same far from the NPC's way in a maze (pillars every 2 m: many small polygons, so the
 * short local shortcut search every half second cannot reach the gate): the change's place
 * itself tells the NPC a shorter way may have opened, and it searches again. */
TEST(KX_Navigation, AGateOpeningFarAwayInAMazeIsTaken)
{
  float seconds[2] = {0.0f, 0.0f};
  float farthest_x[2] = {-1e9f, -1e9f};
  for (const bool open : {false, true}) {
    Level level;
    level.floors.push_back({MT_Vector3(-60, -20, 0), MT_Vector3(60, 50, 0)});
    level.boxes.push_back({MT_Vector3(-60, -0.5f, 0), MT_Vector3(-42, 0.5f, 2)});
    level.boxes.push_back({MT_Vector3(-38, -0.5f, 0), MT_Vector3(54, 0.5f, 2)});
    /* Pillars all over the south half, round the NPC. */
    for (float x = -30.0f; x <= 50.0f; x += 2.0f) {
      for (float y = -18.0f; y <= -2.0f; y += 2.0f) {
        level.boxes.push_back({MT_Vector3(x - 0.3f, y - 0.3f, 0), MT_Vector3(x + 0.3f, y + 0.3f, 2)});
      }
    }
    level.boxes.push_back({MT_Vector3(-42, -0.5f, 0), MT_Vector3(-38, 0.5f, 2)});
    std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
    Objects objects;
    KX_GameObject &npc = objects.Add(MT_Vector3(20, -11, 1));
    const MT_Vector3 goal(-40, 45, 0);
    bool arrived = false;
    int step = 0;
    for (; step < 60 * 120 && !arrived; step++) {
      if (open && step == 30) {
        level.boxes.back().enabled = false;
        navigation->NoteStaticChanged(MT_Vector3(-42, -0.5f, 0), MT_Vector3(-38, 0.5f, 2));
      }
      navigation->Submit(GoTo(npc, goal, 8.0f));
      navigation->Step(1.0f / 60.0f, nullptr);
      farthest_x[open] = std::max(farthest_x[open], float(npc.NodeGetWorldPosition().x()));
      KX_NavAgentInfo info;
      ASSERT_TRUE(navigation->GetAgentInfo(&npc, info));
      arrived = info.arrived;
    }
    EXPECT_TRUE(arrived);
    seconds[open] = float(step) / 60.0f;
  }
  /* Closed: east to the gap at x 54. Opened half a second in: it turned back at once (it had
   * walked east 3 m). The way through the pillars is about 55 m shorter but slow at 8 m/s: the time it
   * gains swings from run to run (eight starts measured 2026-11-19: +-20% each), so only "not
   * slower by much" is asked. */
  EXPECT_GT(farthest_x[false], 54.0f);
  EXPECT_LT(farthest_x[true], 25.0f);
  EXPECT_LT(seconds[true], seconds[false] + 2.0f)
      << "closed " << seconds[false] << " s, opened " << seconds[true] << " s";
}

/** A Character says how high it climbs (PHY_ICharacter::GetClimbHeight, the navigation plans
 * with it): its Step Height, or the edge its round bottom rides up. What it says it climbs, it
 * climbs (a step 2 cm lower); a step 6 cm higher it does not: a capsule with the panel's 90
 * degrees, one with 50 degrees, a box (a flat bottom rides nothing). */
TEST(JoltCharacter, WhatItSaysItClimbsItClimbs)
{
  struct Case {
    const char *name;
    bool box;
    float slope_degrees;
    float expect;
  };
  /* A 0.3 m capsule: 90 degrees rides 0.3 (1 - cos 80) = 0.248 m; 50 degrees rides 0.107 m,
   * under its 0.15 m Step Height. */
  for (const Case &c : {Case{"capsule 90", false, 90.0f, 0.2479f},
                        Case{"capsule 50", false, 50.0f, 0.15f},
                        Case{"box 90", true, 90.0f, 0.15f}})
  {
    float said = 0.0f;
    for (const float more : {-0.02f, 0.06f}) {
      Objects objects;
      World world;
      world.StaticBox(objects.Add(MT_Vector3(0, 0, -0.5f)), MT_Vector3(10, 10, 0.5f));
      KX_GameObject &hero = objects.Add(MT_Vector3(0, 0, 0.9f));
      Body &body = world.Add(hero,
                             c.box ? JPH::RefConst<JPH::Shape>(new JPH::BoxShape(
                                         JPH::Vec3(0.3f, 0.9f, 0.3f), 0.0f)) :
                                     JPH::RefConst<JPH::Shape>(new JPH::CapsuleShape(0.6f, 0.3f)),
                             JPH::EMotionType::Kinematic);
      JoltCharacter *character = world.environment.CreateCharacter(&body.controller, 0.15f);
      ASSERT_NE(character, nullptr);
      character->SetMaxSlope(c.slope_degrees * 0.01745329f);
      character->SetStepHeight(0.15f);
      said = character->GetClimbHeight();
      const float h = said + more;
      world.StaticBox(objects.Add(MT_Vector3(3, 0, h / 2)), MT_Vector3(1.5f, 2, h / 2));
      world.environment.GetPhysicsSystem()->OptimizeBroadPhase();
      for (int step = 0; step < 120; step++) {
        character->SetWalkDirection(MT_Vector3(2, 0, 0));
        world.Step({&hero});
      }
      const MT_Vector3 p = hero.NodeGetWorldPosition();
      const bool up = p.x() > 1.6f && p.z() > 0.9f + h * 0.5f;
      EXPECT_EQ(up, more < 0.0f) << c.name << ": a step of " << h << " m";
    }
    EXPECT_NEAR(said, c.expect, 1e-3f) << c.name;
  }
}

/** A Character walks what it can: its own Step Height and Max Slope give it a navmesh of its own
 * when they are lower than the scene's. The scene climbs 0.4 m: a sliding NPC goes straight up
 * the platform's 0.3 m edge; the Character (Step Height 0.2, Max Slope 30 degrees) is routed
 * round to the stairs of 0.1 m steps and climbs them. */
TEST(KX_Navigation, ACharacterIsRoutedWhereItCanClimb)
{
  Level level;
  level.floors.push_back({MT_Vector3(-10, -10, 0), MT_Vector3(10, 10, 0)});
  level.boxes.push_back({MT_Vector3(2, -3, 0), MT_Vector3(6, 3, 0.3f)});
  level.boxes.push_back({MT_Vector3(6, -1, 0), MT_Vector3(7, 1, 0.2f)});
  level.boxes.push_back({MT_Vector3(7, -1, 0), MT_Vector3(8, 1, 0.1f)});
  /* Which way each went: straight over the platform's edge, or round it (north or south of
   * its 3 m half width) to the stairs east of it. */
  float widest_y[2] = {0.0f, 0.0f}, farthest_x[2] = {-1e9f, -1e9f};
  for (const bool is_character : {false, true}) {
    std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
    Objects objects;
    World world;
    world.StaticBox(objects.Add(MT_Vector3(0, 0, -0.5f)), MT_Vector3(10, 10, 0.5f));
    world.StaticBox(objects.Add(MT_Vector3(4, 0, 0.15f)), MT_Vector3(2, 3, 0.15f));
    world.StaticBox(objects.Add(MT_Vector3(6.5f, 0, 0.1f)), MT_Vector3(0.5f, 1, 0.1f));
    world.StaticBox(objects.Add(MT_Vector3(7.5f, 0, 0.05f)), MT_Vector3(0.5f, 1, 0.05f));
    world.environment.GetPhysicsSystem()->OptimizeBroadPhase();
    navigation->SetPhysicsEnvironmentForTests(&world.environment);
    KX_GameObject &npc = objects.Add(MT_Vector3(-5, 0, is_character ? 0.95f : 0.5f));
    if (is_character) {
      JoltCharacter *character = world.Character(npc);
      ASSERT_NE(character, nullptr);
      character->SetStepHeight(0.2f);
      character->SetMaxSlope(0.5235988f); /* 30 degrees */
    }
    const MT_Vector3 goal(4, 0, 0.3f);
    bool arrived = false;
    for (int step = 0; step < 60 * 30 && !arrived; step++) {
      navigation->Submit(GoTo(npc, goal, 3.0f));
      navigation->Step(1.0f / 60.0f, nullptr);
      if (is_character) {
        world.Step({&npc});
      }
      const MT_Vector3 p = npc.NodeGetWorldPosition();
      widest_y[is_character] = std::max(widest_y[is_character], std::fabs(float(p.y())));
      farthest_x[is_character] = std::max(farthest_x[is_character], float(p.x()));
      KX_NavAgentInfo info;
      ASSERT_TRUE(navigation->GetAgentInfo(&npc, info));
      arrived = info.arrived;
    }
    EXPECT_TRUE(arrived) << (is_character ? "Character" : "sliding NPC");
    if (is_character) {
      /* Up on the platform (its capsule's middle 0.95 m above the 0.3 m top). */
      EXPECT_GT(npc.NodeGetWorldPosition().z(), 1.15f);
    }
  }
  EXPECT_LT(widest_y[false], 1.0f);
  EXPECT_LT(farthest_x[false], 4.5f);
  EXPECT_GT(widest_y[true], 3.0f);
  EXPECT_GT(farthest_x[true], 6.5f);
}

/** \} */

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

/** A crowd in a level that changes (phase 6): a wall slid aside, a crate dropped (Avoid Physics
 * Objects: carved once asleep), a link coming; the positions after 8 s. */
void RunChangingWorld(const uint32_t helpers, std::vector<MT_Vector3> &r_positions)
{
  Objects objects;
  World world;
  world.StaticBox(objects.Add(MT_Vector3(0, 0, -0.5f)), MT_Vector3(20, 20, 0.5f));
  Body &wall = world.StaticBox(objects.Add(MT_Vector3(0, 0, 1)), MT_Vector3(12, 0.5f, 1));
  KX_GameObject &crate = objects.Add(MT_Vector3(-4, -6, 3));
  world.Add(crate, new JPH::BoxShape(JPH::Vec3(0.7f, 0.7f, 0.7f)), JPH::EMotionType::Dynamic);
  world.environment.GetPhysicsSystem()->OptimizeBroadPhase();
  std::unique_ptr<KX_Navigation> navigation = MakeJoltNavigation(world);
  KX_Navigation::Settings settings = TestSettings();
  settings.avoid_physics = true;
  navigation->SetSettings(settings);
  navigation->SetPhysicsObjectsForTests({&crate});
  LN_JobPool pool;
  pool.SetHelperCount(helpers);
  std::vector<KX_GameObject *> npcs;
  std::vector<MT_Vector3> goals;
  for (int i = 0; i < 40; i++) {
    const float x = -15.0f + float(i % 10) * 3.0f, y = -12.0f - float(i / 10) * 1.5f;
    npcs.push_back(&objects.Add(MT_Vector3(x, y, 0.5f)));
    goals.push_back(MT_Vector3(-x, 12.0f + float(i / 10) * 1.5f, 0));
  }
  KX_GameObject &link_object = objects.Add(MT_Vector3(16, -2, 0));
  for (int step = 0; step < 60 * 8; step++) {
    if (step >= 60 && step < 90) {
      /* The wall slides 15 m east over half a second (a gap opens west of x 3). */
      wall.controller.SetPosition(MT_Vector3(0.5f * float(step - 59), 0, 1));
    }
    if (step == 120) {
      KX_NavLink link;
      link.object = &link_object;
      link.start = MT_Vector3(16, -2, 0);
      link.end = MT_Vector3(16, 2, 0);
      link.user_id = 5;
      navigation->SetLinksForTests({link});
    }
    for (size_t i = 0; i < npcs.size(); i++) {
      navigation->Submit(GoTo(*npcs[i], goals[i], 3.0f));
    }
    navigation->Step(1.0f / 60.0f, helpers > 0 ? &pool : nullptr);
    world.Step({&crate});
  }
  r_positions.clear();
  for (KX_GameObject *npc : npcs) {
    r_positions.push_back(npc->NodeGetWorldPosition());
  }
}

/** The changing world's outcome on 0, 3 and 15 helper threads, bit for bit. */
TEST(KX_Navigation, AChangingWorldIsTheSameOnAnyNumberOfThreads)
{
  std::vector<MT_Vector3> alone, few, many;
  RunChangingWorld(0, alone);
  RunChangingWorld(3, few);
  RunChangingWorld(15, many);
  ASSERT_EQ(alone.size(), many.size());
  int crossed = 0;
  for (size_t i = 0; i < alone.size(); i++) {
    EXPECT_EQ(alone[i], few[i]) << i;
    EXPECT_EQ(alone[i], many[i]) << i;
    crossed += alone[i].y() > 1.0f ? 1 : 0;
  }
  /* (The wall slid aside: most crossed in 8 s.) */
  EXPECT_GT(crossed, 30);
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
 * floor, its area the object's Physics > Navigation > Area. */
TEST(KX_Navigation, APondBoxOnAJoltFloorKeepsItsArea)
{
  Objects objects;
  World world;
  world.StaticBox(objects.Add(MT_Vector3(0, 0, -0.5f)), MT_Vector3(10, 10, 0.5f));
  KX_GameObject &pond = objects.Add(MT_Vector3(0, 2, 0.05f));
  world.StaticBox(pond, MT_Vector3(2, 8, 0.05f));
  KX_NavObjectSettings pond_settings;
  pond_settings.area = "water";
  pond.SetNavSettingsForTests(pond_settings, nullptr);
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
                                         const bool until_arrived = false,
                                         const int settings = 0,
                                         const int avoidance = LN_NAVIGATE_AVOIDANCE_POLITE)
{
  NavTree tree(every);
  diff::GenNode &navigate = tree.Add(
      "LogicNativeNavigate", 0, (until_arrived ? LN_NAVIGATE_UNTIL_ARRIVED : 0) | settings);
  tree.b.Vector(navigate, "Destination", float(goal[0]), float(goal[1]), float(goal[2]));
  tree.b.Int(navigate, "Avoidance", avoidance);
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
  /* Next Point: a list of the next corner. */
  tree.StoreCount(navigate, "Next Point", "next_count");
  diff::GenNode &first = tree.Add("LogicNativeListGetItem");
  tree.b.Link(navigate, "Next Point", first, "List");
  tree.StoreVector(first, "Value", "next");
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
    int reached_pulses = 0, steps = 0, arrived_at = -1, next_told = 0;
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
        /* Next Point: one item, the corner Get Navigation Info tells. */
        EXPECT_EQ(rig.Float(npc, "next_count"), 1.0f) << steps;
        next_told += (ReadVector(rig, npc, "next") - ReadVector(rig, npc, "corner")).length() <
                             1e-4f ?
                         1 :
                         0;
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
    EXPECT_GT(next_told, 60);
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

/** Navmesh Query's air modes (phase 8): for a flyer the size of the object running the tree (not
 * an NPC: the scene's agent radius). The map of that size is asked for by the first query and
 * made at the next navigation step; till then nothing is found. Over the WallLevel's wall (2 m
 * high): the straight line low through it is not clear, high above it is; the way low goes
 * round or over; free air near a point inside the wall is outside it. */
TEST(KX_NavigationNodes, NavmeshQueryAnswersInTheAir)
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
  diff::GenNode &low = query(Q::AirClearLine, "low");
  tree.b.Vector(low, "From", -5, -5, 1);
  tree.b.Vector(low, "To", -5, 5, 1);
  diff::GenNode &high = query(Q::AirClearLine, "high");
  tree.b.Vector(high, "From", -5, -5, 4);
  tree.b.Vector(high, "To", -5, 5, 4);
  diff::GenNode &way = query(Q::AirFindPath, "way");
  tree.b.Vector(way, "From", -5, -5, 1);
  tree.b.Vector(way, "To", -5, 5, 1);
  tree.StoreFloat(way, "Length", "way_length");
  tree.StoreCount(way, "Points", "way_points");
  tree.StoreVector(way, "Point", "way_end");
  diff::GenNode &near = query(Q::AirNearestPoint, "near");
  tree.b.Vector(near, "Position", -4, 0, 1);
  tree.b.Float(near, "Radius", 5.0f);
  tree.StoreVector(near, "Point", "near_point");
  const std::shared_ptr<LN_Program> program = tree.Compile();
  ASSERT_NE(program, nullptr);

  Level level = WallLevel();
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  NavigationSeam seam(navigation.get());
  SemRig rig(program);
  KX_GameObject &owner = rig.Object(0);
  rig.Step();
  EXPECT_EQ(rig.Float(owner, "way_found"), 0.0f);
  EXPECT_EQ(rig.Float(owner, "near_found"), 0.0f);
  navigation->Step(1.0f / 60.0f, nullptr);
  ASSERT_NE(navigation->AirMapForTests(0.3f), nullptr);
  rig.Step();
  EXPECT_EQ(rig.Float(owner, "low_found"), 0.0f);
  EXPECT_EQ(rig.Float(owner, "high_found"), 1.0f);
  EXPECT_EQ(rig.Float(owner, "way_found"), 1.0f);
  EXPECT_GT(rig.Float(owner, "way_length"), 10.5f);
  EXPECT_GE(rig.Float(owner, "way_points", -1.0f), 3.0f);
  const MT_Vector3 end = ReadVector(rig, owner, "way_end");
  EXPECT_LT((end - MT_Vector3(-5, 5, 1)).length(), 0.05f);
  EXPECT_EQ(rig.Float(owner, "near_found"), 1.0f);
  const MT_Vector3 near_point = ReadVector(rig, owner, "near_point");
  EXPECT_TRUE(navigation->AirMapForTests(0.3f)->Free(near_point));
  EXPECT_FALSE(std::fabs(float(near_point.y())) < 0.5f + 0.3f && near_point.z() < 2.3f &&
               near_point.x() < 6.0f);
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
  /* On Link End (2026-11-19): its own pulse, the link's kind still given. */
  tree.Flag(event, "On Link End", "ended");
  diff::GenNode &end_kind = tree.b.Add("LogicNativeSetGamePropertyString");
  tree.b.String(end_kind, "Property", "end_kind");
  tree.b.Link(event, "On Link End", end_kind, "Flow");
  tree.b.Link(event, "Link Kind", end_kind, "Value");
  diff::GenNode &end_which = tree.b.Add("LogicNativeSetGamePropertyString");
  tree.b.String(end_which, "Property", "end_which");
  tree.b.Link(end_kind, "Done", end_which, "Flow");
  tree.b.Link(event, "Link Object", end_which, "Value");
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
  int pulses = 0, on_link_steps = 0, end_pulses = 0, start_step = -1, end_step = -1;
  for (int step = 0; step < 60 * 20; step++) {
    rig.SetBool(npc, "started", false);
    rig.SetBool(npc, "ended", false);
    rig.Step();
    navigation->Step(1.0f / 60.0f, nullptr);
    if (rig.Float(npc, "started") == 1.0f) {
      pulses++;
      start_step = step;
    }
    if (rig.Float(npc, "ended") == 1.0f) {
      end_pulses++;
      end_step = step;
    }
    on_link_steps += rig.Float(npc, "on_link") == 1.0f ? 1 : 0;
    if (rig.Float(npc, "arrived") == 1.0f) {
      break;
    }
  }
  EXPECT_EQ(pulses, 1);
  EXPECT_EQ(end_pulses, 1);
  EXPECT_GT(end_step, start_step + 3);
  EXPECT_EQ(rig.Text(npc, "end_kind"), "jump");
  EXPECT_EQ(rig.Text(npc, "end_which"), "Ledge");
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
  tree.b.Int(navigate, "Avoidance", 0);
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

/** Set Navigation Attribute's Jump Limits (2026-11-19): read from game properties while the game
 * runs (Enabled from "limited", Jump Gap from "gap"); an NPC limited to 1.5 m stands at the 2 m
 * gap, given 2.5 m it jumps across; not limited, it jumps. */
TEST(KX_NavigationNodes, JumpLimitsDecideWhichJumpsAnNpcTakes)
{
  using A = LN_NavigationAttribute;
  NavTree tree;
  diff::GenNode &limits = tree.Add("LogicNativeSetNavigationAttribute", int16_t(A::JumpLimits));
  diff::GenNode &limited = tree.Add("LogicNativeGetGamePropertyBool");
  tree.b.String(limited, "Property", "limited");
  tree.b.Link(limited, "Value", limits, "Enabled");
  diff::GenNode &gap = tree.Add("LogicNativeGetGamePropertyFloat");
  tree.b.String(gap, "Property", "gap");
  tree.b.Link(gap, "Value", limits, "Jump Gap");
  tree.b.Float(limits, "Jump Up", 1.5f);
  tree.b.Float(limits, "Drop Down", 4.0f);
  tree.Chain(limits);
  diff::GenNode &navigate = tree.Add("LogicNativeNavigate");
  tree.b.Vector(navigate, "Destination", 5, -3, 0);
  tree.b.Float(navigate, "Lin Speed", 4.0f);
  tree.b.Float(navigate, "Reach Threshold", 0.3f);
  tree.Chain(navigate);
  const std::shared_ptr<LN_Program> program = tree.Compile();
  ASSERT_NE(program, nullptr);

  for (const int run : {0, 1, 2}) {
    SCOPED_TRACE(run == 0 ? "1.5 m, then 2.5 m" : (run == 1 ? "not limited" : "limited off"));
    Level level = JumpLevel();
    std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, JumpSettings());
    NavigationSeam seam(navigation.get());
    SemRig rig(program);
    KX_GameObject &npc = rig.Object(0);
    Place(npc, MT_Vector3(-5, -3, 0));
    rig.SetBool(npc, "limited", run != 1);
    rig.SetFloat(npc, "gap", run == 0 ? 1.5f : 0.0f);
    if (run == 2) {
      /* Limited to nothing, then the limits taken off again. */
      for (int step = 0; step < 30; step++) {
        rig.Step();
        navigation->Step(1.0f / 60.0f, nullptr);
      }
      rig.SetBool(npc, "limited", false);
    }
    auto walk = [&](const int steps) {
      for (int step = 0; step < steps; step++) {
        rig.Step();
        navigation->Step(1.0f / 60.0f, nullptr);
        KX_NavAgentInfo info;
        if (navigation->GetAgentInfo(&npc, info) && info.arrived) {
          return true;
        }
      }
      return false;
    };
    if (run == 0) {
      EXPECT_FALSE(walk(60 * 6));
      EXPECT_LT(npc.NodeGetWorldPosition().x(), -1.0f);
      rig.SetFloat(npc, "gap", 2.5f);
    }
    EXPECT_TRUE(walk(60 * 10));
    EXPECT_NEAR(npc.NodeGetWorldPosition().x(), 5.0f, 0.4f);
  }
}

/** Vehicles in a tree (2026-11-19): Navigate after a marker on a ship's deck (Target Object);
 * the ship away, Get Navigation Info says Waiting, Waiting At the gangway's quay end, no
 * Vehicle; docked, it boards: Vehicle the ship, not waiting. */
TEST(KX_NavigationNodes, GetNavigationInfoTellsTheVehicleAndTheWait)
{
  blender::Object marker_blender{};
  std::snprintf(marker_blender.id.name, sizeof(marker_blender.id.name), "OBMarker");
  NavTree tree;
  diff::GenNode &navigate = tree.Add("LogicNativeNavigate");
  tree.b.ObjectNamed(navigate, "Target Object", &marker_blender);
  tree.b.Float(navigate, "Lin Speed", 3.0f);
  tree.b.Float(navigate, "Reach Threshold", 0.3f);
  tree.Chain(navigate);
  diff::GenNode &info = tree.Add("LogicNativeGetNavigationInfo");
  tree.StoreBool(info, "Waiting", "waiting");
  tree.StoreBool(info, "Arrived", "arrived");
  tree.StoreText(info, "Waiting At", "waiting_at");
  tree.StoreText(info, "Vehicle", "vehicle");
  const std::shared_ptr<LN_Program> program = tree.Compile();
  ASSERT_NE(program, nullptr);

  Level dock;
  dock.floors.push_back({MT_Vector3(-20, -10, 0), MT_Vector3(0, 10, 0)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(dock, TestSettings());
  NavigationSeam seam(navigation.get());
  SemRig rig(program);
  KX_GameObject &npc = rig.Object(0);
  Place(npc, MT_Vector3(-8, 3, 0));
  KX_GameObject &ship = rig.AddObject("Ship", MT_Vector3(23, 0, -1));
  const size_t ship_index = rig.ObjectCount() - 1;
  KX_GameObject &marker = rig.AddObject("Marker", MT_Vector3(3, 1, 1));
  rig.Parent(rig.ObjectCount() - 1, ship_index);
  marker.NodeSetLocalPosition(MT_Vector3(3, 1, 1));
  KX_GameObject &ship_end = rig.AddObject("ShipEnd", MT_Vector3(-5.4f, 0, 1));
  rig.Parent(rig.ObjectCount() - 1, ship_index);
  ship_end.NodeSetLocalPosition(MT_Vector3(-5.4f, 0, 1));
  KX_GameObject &quay_end = rig.AddObject("Gangway", MT_Vector3(-0.6f, 0, 0));
  ship.NodeUpdateGS(0.0);
  const Level shape = ShipShape();
  navigation->SetVehiclesForTests({&ship});
  navigation->SetVehicleGeometryForTests([&shape](KX_GameObject *, KX_NavGeometry &g) {
    shape.Collect(MT_Vector3(0, 0, 0), MT_Vector3(0, 0, 0), g);
  });
  KX_NavLink gangway;
  gangway.object = &quay_end;
  gangway.end_object = &ship_end;
  gangway.kind = "gangway";
  gangway.reach = 2.5f;
  gangway.user_id = 1;
  navigation->SetLinksForTests({gangway});
  for (int step = 0; step < 60 * 8; step++) {
    rig.Step();
    navigation->Step(1.0f / 60.0f, nullptr);
  }
  EXPECT_EQ(rig.Float(npc, "waiting"), 1.0f);
  EXPECT_EQ(rig.Text(npc, "waiting_at"), "Gangway");
  EXPECT_EQ(rig.Text(npc, "vehicle"), "");
  float x = 23.0f;
  for (int step = 0; step < 60 * 15 && rig.Float(npc, "arrived") != 1.0f; step++) {
    x = std::max(x - 0.1f, 7.0f);
    ship.NodeSetWorldPosition(MT_Vector3(x, 0, -1));
    ship.NodeUpdateGS(0.0);
    rig.Step();
    navigation->Step(1.0f / 60.0f, nullptr);
  }
  EXPECT_EQ(rig.Float(npc, "arrived"), 1.0f);
  EXPECT_EQ(rig.Float(npc, "waiting"), 0.0f);
  EXPECT_EQ(rig.Text(npc, "vehicle"), "Ship");
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

/** Navigate's settings (custom2 bits, its instruction's secondary_int_value) and its Avoidance
 * input reach the request: Avoidance Off / Polite / Dense, Move, Climb (2026-11-19; Avoidance an
 * input since 2026-11-20). */
TEST(KX_NavigationNodes, NavigatesSettingsReachTheRequest)
{
  struct Case {
    int settings;
    int avoidance;
    bool avoid, dense;
    KX_NavRequest::Move move;
    KX_NavRequest::Climb climb;
  };
  const int fly = LN_NAVIGATE_MOVE_FLY << LN_NAVIGATE_MOVE_SHIFT;
  const int swim = LN_NAVIGATE_MOVE_SWIM << LN_NAVIGATE_MOVE_SHIFT;
  const int walls = LN_NAVIGATE_CLIMB_WALLS << LN_NAVIGATE_CLIMB_SHIFT;
  const int ceilings = LN_NAVIGATE_CLIMB_CEILINGS << LN_NAVIGATE_CLIMB_SHIFT;
  for (const Case &c :
       {Case{0, 1, true, false, KX_NavRequest::Move::Walk, KX_NavRequest::Climb::Floors},
        Case{0, 2, true, true, KX_NavRequest::Move::Walk, KX_NavRequest::Climb::Floors},
        Case{0, 0, false, false, KX_NavRequest::Move::Walk, KX_NavRequest::Climb::Floors},
        Case{0, -3, false, false, KX_NavRequest::Move::Walk, KX_NavRequest::Climb::Floors},
        Case{0, 7, true, true, KX_NavRequest::Move::Walk, KX_NavRequest::Climb::Floors},
        Case{fly, 1, true, false, KX_NavRequest::Move::Fly, KX_NavRequest::Climb::Floors},
        Case{swim, 2, true, true, KX_NavRequest::Move::Swim, KX_NavRequest::Climb::Floors},
        Case{walls, 1, true, false, KX_NavRequest::Move::Walk, KX_NavRequest::Climb::Walls},
        Case{ceilings, 1, true, false, KX_NavRequest::Move::Walk, KX_NavRequest::Climb::Ceilings}})
  {
    const std::shared_ptr<LN_Program> program = NavigateTree(MT_Vector3(-5, 5, 0), 1, true,
                                                             c.settings, c.avoidance);
    ASSERT_NE(program, nullptr);
    for (const SemMode &mode : {kProduction, kMemoOff}) {
      SCOPED_TRACE(mode.name);
      Level level = WallLevel();
      std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
      NavigationSeam seam(navigation.get());
      SemRig rig(program, mode);
      KX_GameObject &npc = rig.Object(0);
      Place(npc, MT_Vector3(-5, -5, 1));
      rig.Step();
      navigation->Step(1.0f / 60.0f, nullptr);
      KX_NavRequest held;
      ASSERT_TRUE(navigation->GetHeldOrder(&npc, held)) << c.settings;
      EXPECT_EQ(held.avoid, c.avoid) << c.settings << " " << c.avoidance;
      EXPECT_EQ(held.dense, c.dense) << c.settings << " " << c.avoidance;
      EXPECT_EQ(held.move, c.move) << c.settings;
      EXPECT_EQ(held.climb, c.climb) << c.settings;
      EXPECT_TRUE(held.until_arrived) << c.settings;
    }
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

/** An open floor 20 m across (the NPC's navigation for the node tests below). */
std::unique_ptr<KX_Navigation> OpenFloorNavigation(Level &level)
{
  level.floors.push_back({MT_Vector3(-10, -10, 0), MT_Vector3(10, 10, 0)});
  return MakeNavigation(level, TestSettings());
}

/* When Reached is per node (2026-11-19, Unreal's request ids): an NPC driven by two Navigate
 * nodes (one runs at a time, a Branch on "go_b") pulses only the node whose order arrived (both
 * pulsed at any arrival before), and another node's order is a new order: a Navigate taking over
 * where the NPC already stands at its goal pulses once. */
TEST(KX_NavigationNodes, WhenReachedPulsesOnlyTheNodeWhoseOrderArrived)
{
  for (const bool same_goal : {false, true}) {
    SCOPED_TRACE(same_goal ? "the same goal" : "two goals");
    const MT_Vector3 goals[2] = {MT_Vector3(4, 0, 0),
                                 same_goal ? MT_Vector3(4, 0, 0) : MT_Vector3(-4, 0, 0)};
    NavTree tree;
    diff::GenNode &go_b = tree.Add("LogicNativeGetGamePropertyBool");
    tree.b.String(go_b, "Property", "go_b");
    diff::GenNode &branch = tree.Add("LogicNativeBranch");
    tree.b.Link(*tree.step, "Out", branch, "Flow");
    tree.b.Link(go_b, "Value", branch, "Condition");
    for (int i = 0; i < 2; i++) {
      diff::GenNode &navigate = tree.Add("LogicNativeNavigate");
      tree.b.Vector(navigate,
                    "Destination",
                    float(goals[i][0]),
                    float(goals[i][1]),
                    float(goals[i][2]));
      tree.b.Float(navigate, "Lin Speed", 3.0f);
      tree.b.Float(navigate, "Reach Threshold", 0.3f);
      tree.b.Link(branch, i == 0 ? "False" : "True", navigate, "Flow");
      tree.Flag(navigate, "When Reached", i == 0 ? "a" : "b");
    }
    const std::shared_ptr<LN_Program> program = tree.Compile();
    ASSERT_NE(program, nullptr);
    Level level;
    std::unique_ptr<KX_Navigation> navigation = OpenFloorNavigation(level);
    NavigationSeam seam(navigation.get());
    SemRig rig(program);
    KX_GameObject &npc = rig.Object(0);
    Place(npc, MT_Vector3(0, 0, 0));
    int pulses[2] = {0, 0};
    char last = 0;
    for (int step = 0; step < 60 * 16; step++) {
      rig.SetBool(npc, "a", false);
      rig.SetBool(npc, "b", false);
      rig.Step();
      navigation->Step(1.0f / 60.0f, nullptr);
      const bool a = rig.Float(npc, "a") == 1.0f, b = rig.Float(npc, "b") == 1.0f;
      ASSERT_FALSE(a && b) << step;
      const MT_Vector3 p = npc.NodeGetWorldPosition();
      for (int i = 0; i < 2; i++) {
        if (i == 0 ? a : b) {
          /* Where its own goal is, and the nodes take turns. */
          EXPECT_LT(std::hypot(p.x() - goals[i].x(), p.y() - goals[i].y()), 0.6f) << step;
          EXPECT_NE(last, 'a' + i) << step;
          last = char('a' + i);
          pulses[i]++;
          rig.SetBool(npc, "go_b", i == 0);
        }
      }
    }
    EXPECT_GE(pulses[0], 2);
    EXPECT_GE(pulses[1], 2);
  }
}

/** Follow Path's tree for the tests below: step -> (Branch on "run" when `gated`) -> Follow Path
 * (Way `way`, 3 m/s, Reach 0.2, Continue from "continue") -> Point Index into "index";
 * Point Reached raises "reached", Finished "finished". `points` link into its Path Points. */
std::shared_ptr<LN_Program> FollowPathTree(
    const std::function<void(NavTree &, diff::GenNode &list)> &points,
    const int16_t way,
    const bool gated,
    const bool loop = false)
{
  NavTree tree;
  diff::GenNode &list = tree.Add("LogicNativeListFromItems", 0); /* the Values input */
  points(tree, list);
  diff::GenNode &follow = tree.Add("LogicNativeFollowPath", way);
  tree.b.Link(list, "Values", follow, "Path Points");
  tree.b.Float(follow, "Lin Speed", 3.0f);
  tree.b.Float(follow, "Reach Threshold", 0.2f);
  tree.b.Bool(follow, "Loop", loop);
  diff::GenNode &keep = tree.Add("LogicNativeGetGamePropertyBool");
  tree.b.String(keep, "Property", "continue");
  tree.b.Link(keep, "Value", follow, "Continue");
  if (gated) {
    diff::GenNode &run = tree.Add("LogicNativeGetGamePropertyBool");
    tree.b.String(run, "Property", "run");
    diff::GenNode &branch = tree.Add("LogicNativeBranch");
    tree.b.Link(run, "Value", branch, "Condition");
    tree.Chain(branch, "True");
  }
  tree.Chain(follow);
  tree.Store(follow, "Point Index", "index", "LogicNativeSetGamePropertyInt");
  tree.Flag(follow, "Point Reached", "reached");
  tree.Flag(follow, "Finished", "finished");
  return tree.Compile();
}

void AddPlace(NavTree &tree, diff::GenNode &list, const float x, const float y)
{
  diff::GenNode &v = tree.Add("LogicNativeCombineXYZ");
  tree.b.Float(v, "X", x);
  tree.b.Float(v, "Y", y);
  tree.b.Link(v, "Vector", list, "Values");
}

/** An object found by the name in game property `property` (or by `name`). */
void AddObjectPoint(NavTree &tree,
                    diff::GenNode &list,
                    const char *name,
                    const char *property = nullptr)
{
  diff::GenNode &object = tree.Add("LogicNativeObjectByName");
  if (property != nullptr) {
    diff::GenNode &text = tree.Add("LogicNativeGetGamePropertyString");
    tree.b.String(text, "Property", property);
    tree.b.Link(text, "Value", object, "Name");
  }
  else {
    tree.b.String(object, "Name", name);
  }
  tree.b.Link(object, "Object", list, "Values");
}

/** A Follow Path run: each Point Reached as (step, Point Index, where it stood), Finished's
 * steps; `each(step)` before each step (the test's changes). */
struct FollowRun {
  struct Reach {
    int step;
    int index;
    MT_Vector3 at;
  };
  std::vector<Reach> reached;
  std::vector<int> finished;
  int index_before_first = 0;
};

FollowRun RunFollowPath(SemRig &rig,
                        KX_Navigation &navigation,
                        const int steps,
                        const std::function<void(int step, const FollowRun &run)> &each)
{
  FollowRun run;
  KX_GameObject &npc = rig.Object(0);
  run.index_before_first = -100;
  for (int step = 0; step < steps; step++) {
    each(step, run);
    rig.SetBool(npc, "reached", false);
    rig.SetBool(npc, "finished", false);
    rig.Step();
    navigation.Step(1.0f / 60.0f, nullptr);
    if (step == 10) {
      run.index_before_first = int(rig.Float(npc, "index"));
    }
    if (rig.Float(npc, "reached") == 1.0f) {
      run.reached.push_back({step, int(rig.Float(npc, "index")), npc.NodeGetWorldPosition()});
    }
    if (rig.Float(npc, "finished") == 1.0f) {
      run.finished.push_back(step);
    }
  }
  return run;
}

/* Follow Path's points are places or objects (2026-11-19: objects were left out without a word),
 * an object's point where it is, also when it moved (the third point moves away while the NPC
 * walks the first two). Point Reached pulses at each point with its Point Index (-1 before the
 * first), Finished once at the last. */
TEST(KX_NavigationNodes, FollowPathWalksToObjectsAndPlacesAndTellsEachPoint)
{
  const std::shared_ptr<LN_Program> program = FollowPathTree(
      [](NavTree &tree, diff::GenNode &list) {
        AddObjectPoint(tree, list, "Leader");
        AddPlace(tree, list, 4.0f, 4.0f);
        AddObjectPoint(tree, list, "Turret");
      },
      1,
      false);
  ASSERT_NE(program, nullptr);
  Level level;
  std::unique_ptr<KX_Navigation> navigation = OpenFloorNavigation(level);
  NavigationSeam seam(navigation.get());
  SemRig rig(program);
  KX_GameObject &leader = rig.AddObject("Leader", MT_Vector3(4, 0, 0));
  KX_GameObject &turret = rig.AddObject("Turret", MT_Vector3(0, 4, 0));
  (void)leader;
  const FollowRun run = RunFollowPath(rig, *navigation, 60 * 8, [&](const int step, auto &) {
    if (step == 60) {
      Place(turret, MT_Vector3(-3, 6, 0));
    }
  });
  EXPECT_EQ(run.index_before_first, -1);
  ASSERT_EQ(run.reached.size(), size_t(3));
  const MT_Vector3 expect[3] = {MT_Vector3(4, 0, 0), MT_Vector3(4, 4, 0), MT_Vector3(-3, 6, 0)};
  for (int i = 0; i < 3; i++) {
    EXPECT_EQ(run.reached[size_t(i)].index, i);
    EXPECT_LT(std::hypot(run.reached[size_t(i)].at.x() - expect[i].x(),
                         run.reached[size_t(i)].at.y() - expect[i].y()),
              0.3f)
        << i;
  }
  ASSERT_EQ(run.finished.size(), size_t(1));
  EXPECT_EQ(run.finished[0], run.reached[2].step);
}

/* Continue (2026-11-19, it meant nothing): Flow paused a second after the second point, then
 * running again: off, from the first point; on, on to the third. */
TEST(KX_NavigationNodes, FollowPathStartsAgainOrContinuesAfterAPause)
{
  const std::shared_ptr<LN_Program> program = FollowPathTree(
      [](NavTree &tree, diff::GenNode &list) {
        AddPlace(tree, list, 3.0f, 0.0f);
        AddPlace(tree, list, 3.0f, 3.0f);
        AddPlace(tree, list, 0.0f, 3.0f);
      },
      1,
      true);
  ASSERT_NE(program, nullptr);
  for (const bool keep : {false, true}) {
    SCOPED_TRACE(keep ? "Continue" : "from the first");
    Level level;
    std::unique_ptr<KX_Navigation> navigation = OpenFloorNavigation(level);
    NavigationSeam seam(navigation.get());
    SemRig rig(program);
    KX_GameObject &npc = rig.Object(0);
    rig.SetBool(npc, "continue", keep);
    rig.SetBool(npc, "run", true);
    int paused_at = -1;
    const FollowRun run = RunFollowPath(
        rig, *navigation, 60 * 8, [&](const int step, const FollowRun &so_far) {
          if (paused_at < 0 && so_far.reached.size() == 2) {
            paused_at = step;
            rig.SetBool(npc, "run", false);
          }
          if (paused_at >= 0 && step == paused_at + 60) {
            rig.SetBool(npc, "run", true);
          }
        });
    ASSERT_GE(run.reached.size(), size_t(3));
    EXPECT_EQ(run.reached[0].index, 0);
    EXPECT_EQ(run.reached[1].index, 1);
    EXPECT_EQ(run.reached[2].index, keep ? 2 : 0);
  }
}

/* A list of another length (2026-11-19): it goes on to the point it walked to where that is in
 * the new list, else from its first. The second point is an object named by "extra": named
 * "Nobody", the list has two points. */
TEST(KX_NavigationNodes, FollowPathGoesOnInAListOfAnotherLength)
{
  const std::shared_ptr<LN_Program> program = FollowPathTree(
      [](NavTree &tree, diff::GenNode &list) {
        AddPlace(tree, list, 3.0f, 0.0f);
        AddObjectPoint(tree, list, nullptr, "extra");
        AddPlace(tree, list, 0.0f, 3.0f);
      },
      1,
      false);
  ASSERT_NE(program, nullptr);
  for (const bool walking_to_object : {false, true}) {
    SCOPED_TRACE(walking_to_object ? "walking to the object that goes" :
                                     "walking to a point both lists have");
    Level level;
    std::unique_ptr<KX_Navigation> navigation = OpenFloorNavigation(level);
    NavigationSeam seam(navigation.get());
    SemRig rig(program);
    KX_GameObject &npc = rig.Object(0);
    rig.AddObject("Turret", MT_Vector3(3, 3, 0));
    rig.SetString(npc, "extra", "Turret");
    bool changed = false;
    const FollowRun run = RunFollowPath(
        rig, *navigation, 60 * 8, [&](const int /*step*/, const FollowRun &so_far) {
          /* Walking to the object (after the first point) or past it (after the second). */
          if (!changed && so_far.reached.size() == (walking_to_object ? 1u : 2u)) {
            changed = true;
            rig.SetString(npc, "extra", "Nobody");
          }
        });
    ASSERT_GE(run.reached.size(), size_t(3)) << (run.reached.size() > 1 ? run.reached[1].index : -9);
    const FollowRun::Reach &after = run.reached[walking_to_object ? 1 : 2];
    /* The object gone: from the first point (3, 0). The third point is the new list's second. */
    EXPECT_EQ(after.index, walking_to_object ? 0 : 1);
    const MT_Vector3 at = walking_to_object ? MT_Vector3(3, 0, 0) : MT_Vector3(0, 3, 0);
    EXPECT_LT(std::hypot(after.at.x() - at.x(), after.at.y() - at.y()), 0.3f);
  }
}

/* Way Navmesh (2026-11-19) walks the scene's navmesh round a wall (Straight, every older node,
 * goes straight at the point, through the wall where nothing stops it). */
TEST(KX_NavigationNodes, FollowPathOnTheNavmeshGoesRoundTheWall)
{
  for (const int16_t way : {int16_t(0), int16_t(1)}) {
    SCOPED_TRACE(way ? "Navmesh" : "Straight");
    const std::shared_ptr<LN_Program> program = FollowPathTree(
        [](NavTree &tree, diff::GenNode &list) { AddPlace(tree, list, -5.0f, 5.0f); },
        way,
        false);
    ASSERT_NE(program, nullptr);
    Level level = WallLevel();
    std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
    NavigationSeam seam(navigation.get());
    SemRig rig(program);
    KX_GameObject &npc = rig.Object(0);
    Place(npc, MT_Vector3(-5, -5, 0));
    bool through_wall = false;
    const FollowRun run = RunFollowPath(rig, *navigation, 60 * 12, [&](int, const FollowRun &) {
      const MT_Vector3 p = npc.NodeGetWorldPosition();
      through_wall = through_wall || (p.x() < 5.5f && std::fabs(p.y()) < 0.5f);
    });
    EXPECT_EQ(through_wall, way == 0);
    ASSERT_EQ(run.finished.size(), size_t(1));
  }
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


/* -------------------------------------------------------------------- */
/** \name Packed crowds (phase 7, 2026-11-19)
 * \{ */

float distance_2d(const MT_Vector3 &a, const MT_Vector3 &b)
{
  return float(std::hypot(a.x() - b.x(), a.y() - b.y()));
}

/** A room (x -14..0, y -10..10) whose east wall has one door `width` wide round y 0, a hall
 * beyond it (x 0..30): the door test of the packed-crowds research. */
Level DoorLevel(const float width = 1.2f)
{
  Level level;
  level.floors.push_back({MT_Vector3(-14, -10, 0), MT_Vector3(30, 10, 0)});
  level.boxes.push_back({MT_Vector3(-0.5f, -10, 0), MT_Vector3(0.5f, -width * 0.5f, 2)});
  level.boxes.push_back({MT_Vector3(-0.5f, width * 0.5f, 0), MT_Vector3(0.5f, 10, 2)});
  return level;
}

struct DoorRun {
  /** Seconds until the half and the whole crowd were through, people a second through the door
   * in between, the longest any NPC stood still on its way (not arrived), the deepest overlap. */
  float half_seconds = 0.0f, all_seconds = 0.0f, flow = 0.0f;
  float longest_still = 0.0f, deepest_overlap = 0.0f;
  /** The deepest any NPC's middle got inside the door's walls (0: never). */
  float in_wall = 0.0f;
  int through = 0;
};

DoorRun RunDoor(const int count,
               const bool dense,
               const uint32_t helpers,
               const float seconds,
               const float width = 1.2f,
               std::vector<MT_Vector3> *r_positions = nullptr)
{
  Level level = DoorLevel(width);
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  LN_JobPool pool;
  pool.SetHelperCount(helpers);
  Objects objects;
  std::vector<KX_GameObject *> npcs;
  std::vector<MT_Vector3> goals;
  /* 17 a row across the room (1 m apart), rows 0.9 m apart from the door back. */
  for (int i = 0; i < count; i++) {
    const float x = -1.5f - float(i / 17) * 0.9f;
    const float y = -8.0f + float(i % 17) * 1.0f;
    npcs.push_back(&objects.Add(MT_Vector3(x, y, 0)));
    /* Each its own place in the hall (no queue at a common goal). */
    goals.push_back(MT_Vector3(6.0f + float(i / 16) * 1.2f, -9.0f + float(i % 16) * 1.2f, 0));
  }
  DoorRun run;
  std::vector<float> still(npcs.size(), 0.0f);
  std::vector<MT_Vector3> last(npcs.size());
  for (size_t i = 0; i < npcs.size(); i++) {
    last[i] = npcs[i]->NodeGetWorldPosition();
  }
  const float dt = 1.0f / 60.0f;
  int first_step = -1, half_step = -1, all_step = -1;
  for (int step = 0; step < int(seconds * 60.0f) && all_step < 0; step++) {
    for (size_t i = 0; i < npcs.size(); i++) {
      KX_NavRequest request = GoTo(*npcs[i], goals[i], 1.6f);
      request.dense = dense;
      request.arrive_distance = 0.4f;
      navigation->Submit(request);
    }
    navigation->Step(dt, helpers > 0 ? &pool : nullptr);
    int through = 0;
    for (size_t i = 0; i < npcs.size(); i++) {
      const MT_Vector3 p = npcs[i]->NodeGetWorldPosition();
      through += p.x() > 0.6f ? 1 : 0;
      if (std::fabs(p.x()) < 0.5f && std::fabs(p.y()) > width * 0.5f) {
        run.in_wall = std::max(run.in_wall,
                               std::min(float(std::fabs(p.y())) - width * 0.5f,
                                        0.5f - float(std::fabs(p.x()))));
      }
      KX_NavAgentInfo info;
      navigation->GetAgentInfo(npcs[i], info);
      if (!info.arrived && (p - last[i]).length() < 1e-4f) {
        still[i] += dt;
        run.longest_still = std::max(run.longest_still, still[i]);
      }
      else {
        still[i] = 0.0f;
      }
      last[i] = p;
      for (size_t j = i + 1; j < npcs.size(); j++) {
        const MT_Vector3 q = npcs[j]->NodeGetWorldPosition();
        const float d = float(std::hypot(p.x() - q.x(), p.y() - q.y()));
        run.deepest_overlap = std::max(run.deepest_overlap, 0.6f - d);
      }
    }
    if (through > 0 && first_step < 0) {
      first_step = step;
    }
    if (through * 2 >= count && half_step < 0) {
      half_step = step;
    }
    if (through == count) {
      all_step = step;
    }
    run.through = through;
  }
  if (r_positions != nullptr) {
    r_positions->clear();
    for (KX_GameObject *npc : npcs) {
      r_positions->push_back(npc->NodeGetWorldPosition());
    }
  }
  run.half_seconds = half_step >= 0 ? float(half_step) * dt : -1.0f;
  run.all_seconds = all_step >= 0 ? float(all_step) * dt : -1.0f;
  if (half_step > first_step && first_step >= 0) {
    run.flow = float(count / 2) / (float(half_step - first_step) * dt);
  }
  return run;
}

/** The door test (the packed-crowds research's first step): 200 NPCs leave a room by one door,
 * Polite (ORCA) and Dense. Printed: how long, how many a second, the longest stand-still, the
 * deepest overlap. Measured 2026-11-19 (90 s, these settings: 0.2 m cells, radius 0.3):
 *   1.6 m: Polite 161 through, 1.8 a second, one NPC frozen 39 s; Dense all in 72 s, 2.9 a
 *          second, never frozen;
 *   2.4 m: Polite all in 49.5 s, 4.4 a second, frozen 23 s; Dense all in 41 s, 5.6 a second.
 *   After going round standing groups, crowd costs along the way and keep_right (same day):
 *   1.6 m: Polite 197 through, 2.2 a second, frozen 20 s; Dense all in 78 s, 2.9 a second;
 *   2.4 m: Polite all in 49 s, 4.3 a second, frozen 2 s; Dense all in 43 s, 5.6 a second.
 *   (1.2 m: both crawl, Dense worse: the walkable passage, 0.4 m after the radius rounded up to
 *   two cells, is narrower than a body; the scene's defaults round exactly.)
 * Dense: everyone through, more a second than Polite, never frozen, never deeply inside each
 * other. */
TEST(KX_Navigation, ADenseCrowdFlowsThroughADoor)
{
  for (const float width : {1.6f, 2.4f}) {
    const DoorRun polite = RunDoor(200, false, 0, 90.0f, width);
    const DoorRun dense = RunDoor(200, true, 0, 90.0f, width);
    for (const auto &[name, run] : {std::pair<const char *, DoorRun>{"polite", polite},
                                    std::pair<const char *, DoorRun>{"dense", dense}})
    {
      printf("door %.1f m %-6s: %d of 200 through, half in %.1f s, all in %.1f s, %.2f a second, "
             "longest stand-still %.1f s, deepest overlap %.2f m, deepest in a wall %.2f m\n",
             width, name, run.through, run.half_seconds, run.all_seconds, run.flow,
             run.longest_still, run.deepest_overlap, run.in_wall);
      /* Never inside a wall (the navmesh keeps a middle a radius from it). */
      EXPECT_EQ(run.in_wall, 0.0f) << width << " " << name;
    }
    EXPECT_EQ(dense.through, 200) << width;
    /* (Crowd numbers move by about 15% with any change of rounding: 1.1, not more.) */
    EXPECT_GT(dense.flow, polite.flow * 1.1f) << width;
    /* Polite: most through the narrow door (169 to 194 over six starts a few millimetres apart,
     * 2026-11-19), all through the wide one. */
    EXPECT_GE(polite.through, width < 2.0f ? 150 : 200) << width;
    EXPECT_LT(dense.longest_still, 2.0f) << width;
    EXPECT_LT(dense.deepest_overlap, 0.3f) << width;
  }
}

/** The door test with Characters (Jolt's CharacterVirtual moves each, its capsule collides with
 * the walls): 60 through a 1.6 m door, Polite and Dense. Measured 2026-11-19, the capsules
 * blocking each other: Polite 51.2 s, Dense 36.0 s (1.7 a second; sliding NPCs 2.9): capsules
 * cannot push each other (a Character's body is kinematic), so a crowd of them clogged a door
 * as grains do. Since item 12 (2026-11-20, the user's call) navigating Characters pass each
 * other's bodies, kept apart by the navigation and walking out of any overlap
 * (PassThroughSeparation): Polite 36.3 s, deepest overlap 0.08 m; Dense 22.0 s, 0.20 m, over
 * 10 cm in 1% of close pairs' steps (Dense's own squeeze; sliding NPCs: 24.0 s, 0.18 m). */
TEST(KX_Navigation, ADenseCrowdOfCharactersFlowsThroughADoor)
{
  float seconds[2] = {0.0f, 0.0f};
  int through_at_end[2] = {0, 0};
  for (const bool dense : {false, true}) {
    Level level = DoorLevel(1.6f);
    std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
    Objects objects;
    World world;
    world.StaticBox(objects.Add(MT_Vector3(8, 0, -0.5f)), MT_Vector3(22, 10, 0.5f));
    world.StaticBox(objects.Add(MT_Vector3(0, -5.4f, 1)), MT_Vector3(0.5f, 4.6f, 1));
    world.StaticBox(objects.Add(MT_Vector3(0, 5.4f, 1)), MT_Vector3(0.5f, 4.6f, 1));
    world.environment.GetPhysicsSystem()->OptimizeBroadPhase();
    navigation->SetPhysicsEnvironmentForTests(&world.environment);
    std::vector<KX_GameObject *> npcs;
    std::vector<MT_Vector3> goals;
    for (int i = 0; i < 60; i++) {
      KX_GameObject &npc = objects.Add(
          MT_Vector3(-1.5f - float(i / 12) * 0.9f, -5.5f + float(i % 12) * 1.0f, 0.95f));
      ASSERT_NE(world.Character(npc), nullptr);
      npcs.push_back(&npc);
      goals.push_back(MT_Vector3(6.0f + float(i / 12) * 1.2f, -6.6f + float(i % 12) * 1.2f, 0));
    }
    world.environment.GetPhysicsSystem()->OptimizeBroadPhase();
    float deepest = 0.0f;
    int step = 0, through = 0;
    for (; step < 60 * 90 && through < int(npcs.size()); step++) {
      for (size_t i = 0; i < npcs.size(); i++) {
        KX_NavRequest request = GoTo(*npcs[i], goals[i], 1.6f);
        request.dense = dense;
        request.arrive_distance = 0.4f;
        navigation->Submit(request);
      }
      navigation->Step(1.0f / 60.0f, nullptr);
      world.Step(npcs);
      through = 0;
      for (size_t i = 0; i < npcs.size(); i++) {
        const MT_Vector3 p = npcs[i]->NodeGetWorldPosition();
        ASSERT_TRUE(std::isfinite(float(p.x())) && std::isfinite(float(p.y())));
        through += p.x() > 0.6f ? 1 : 0;
        for (size_t j = i + 1; j < npcs.size(); j++) {
          deepest = std::max(deepest, 0.6f - distance_2d(p, npcs[j]->NodeGetWorldPosition()));
        }
      }
    }
    seconds[dense] = float(step) / 60.0f;
    through_at_end[dense] = through;
    printf("door of Characters %-6s: %d of 60 through in %.1f s, deepest overlap %.2f m\n",
           dense ? "dense" : "polite", through, seconds[dense], deepest);
    EXPECT_EQ(through, 60) << dense;
    /* (Polite as sliding NPCs; Dense presses, as theirs.) */
    EXPECT_LT(deepest, dense ? 0.25f : 0.15f) << dense;
    EXPECT_LT(seconds[dense], dense ? 30.0f : 45.0f) << dense;
  }
  (void)through_at_end;
}

/** Two crowds meeting head-on in a corridor (40 each way, from a room at either end into the
 * other): lanes form (keep_right), those caught face to face sidestep and, stuck, slip past
 * (head_on_size). Returns how many reached the far room within `seconds` and when all did. */
struct HeadOnRun {
  int through = 0;
  float all_seconds = -1.0f, deepest_overlap = 0.0f;
};

HeadOnRun RunHeadOn(const bool dense,
                    const int abreast,
                    const float half_width,
                    const float seconds,
                    const uint32_t helpers = 0,
                    std::vector<MT_Vector3> *r_positions = nullptr,
                    const int jitter = 0)
{
  /* The corridor (x -length..length) holds both crowds, rows 0.8 m apart from x 6 outward;
   * rooms 14 m square beyond its ends. */
  const float length = 8.0f + float(40 / abreast) * 0.8f;
  Level level;
  level.floors.push_back({MT_Vector3(-length, -half_width, 0), MT_Vector3(length, half_width, 0)});
  level.floors.push_back({MT_Vector3(-length - 14.0f, -7, 0), MT_Vector3(-length, 7, 0)});
  level.floors.push_back({MT_Vector3(length, -7, 0), MT_Vector3(length + 14.0f, 7, 0)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  LN_JobPool pool;
  pool.SetHelperCount(helpers);
  Objects objects;
  std::vector<KX_GameObject *> npcs;
  std::vector<MT_Vector3> goals;
  for (int i = 0; i < 80; i++) {
    const bool east = i < 40;
    const int k = i % 40;
    const float side = east ? -1.0f : 1.0f;
    /* (`jitter`: each start a few millimetres off, the same every time: crowds are chaotic.) */
    const float x = (6.0f + float(k / abreast) * 0.8f) * side +
                    (jitter != 0 ? float(((i * 7919 + jitter * 104729) % 101) - 50) * 0.001f :
                                   0.0f);
    const float y = -0.35f * float(abreast - 1) + float(k % abreast) * 0.7f;
    npcs.push_back(&objects.Add(MT_Vector3(x, y, 0)));
    /* A place in the far room, 1.2 m apart. */
    goals.push_back(MT_Vector3(-side * (length + 2.0f + float(k % 8) * 1.2f),
                               -4.2f + float(k / 8) * 2.1f,
                               0));
  }
  HeadOnRun run;
  for (int step = 0; step < int(seconds * 60.0f); step++) {
    for (size_t i = 0; i < npcs.size(); i++) {
      KX_NavRequest request = GoTo(*npcs[i], goals[i], 1.4f);
      request.dense = dense;
      request.arrive_distance = 0.4f;
      navigation->Submit(request);
    }
    navigation->Step(1.0f / 60.0f, helpers > 0 ? &pool : nullptr);
    int through = 0;
    for (size_t i = 0; i < npcs.size(); i++) {
      const MT_Vector3 p = npcs[i]->NodeGetWorldPosition();
      /* Through: in the far room, or arrived (as near as it gets to a place others stand at). */
      KX_NavAgentInfo info;
      navigation->GetAgentInfo(npcs[i], info);
      through += ((i < 40 ? p.x() > length : p.x() < -length) || info.arrived) ? 1 : 0;
      if (step % 10 == 0) {
        for (size_t j = i + 1; j < npcs.size(); j++) {
          run.deepest_overlap = std::max(run.deepest_overlap,
                                         0.6f - distance_2d(p, npcs[j]->NodeGetWorldPosition()));
        }
      }
    }
    run.through = through;
    if (through == int(npcs.size()) && run.all_seconds < 0.0f) {
      run.all_seconds = float(step) / 60.0f;
      if (r_positions == nullptr) {
        break;
      }
    }
  }
  if (r_positions != nullptr) {
    r_positions->clear();
    for (KX_GameObject *npc : npcs) {
      r_positions->push_back(npc->NodeGetWorldPosition());
    }
  }
  return run;
}

/** Every set-up from three starts a few millimetres apart (crowds are chaotic): everybody
 * through within 180 s, Dense never deeply inside each other. Seen 2026-11-19 (seconds until all
 * were through; the hardest set-up over seven starts):
 *   two abreast, corridor 3 m: Polite 34-35 s, Dense 42-43 s;
 *   four abreast, street 6 m: Polite 23-24 s, Dense 29-31 s;
 *   four abreast, corridor 3 m (each crowd fills it): Polite 54-110 s, Dense 35-39 s;
 *   deepest Dense overlap 0.15 m.
 * Found on the way: without keep_right Polite got 77 and 28 of 80 through in the corridors;
 * NPCs standing for good were an NPC shaking 3 mm at a wall corner that looked like walking
 * (stalls are now by distance, stall_time) and Dense NPCs on a corridor's edge pushed out by
 * their wall (its way in is now the navmesh's winding: wall_inward). A Dense sidestep and shrink
 * of face-to-face pairs, and a zipper yield, measured no better: not kept. */
TEST(KX_Navigation, TwoCrowdsMeetingHeadOnPass)
{
  struct Case {
    const char *name;
    int abreast;
    float half_width;
  };
  for (const Case &c : {Case{"two abreast, corridor 3 m", 2, 1.5f},
                        Case{"four abreast, street 6 m", 4, 3.0f},
                        Case{"four abreast, corridor 3 m (each fills it)", 4, 1.5f}})
  {
    for (const int jitter : {0, 1, 2}) {
      for (const bool dense : {false, true}) {
        const HeadOnRun run = RunHeadOn(dense, c.abreast, c.half_width, 180.0f, 0, nullptr, jitter);
        printf("head-on, %s, start %d, %s: %d of 80 through, all in %.1f s, deepest overlap "
               "%.2f m\n",
               c.name, jitter, dense ? "dense" : "polite", run.through, run.all_seconds,
               run.deepest_overlap);
        EXPECT_EQ(run.through, 80) << c.name << " start " << jitter
                                   << (dense ? " dense" : " polite");
        if (dense) {
          EXPECT_LT(run.deepest_overlap, 0.3f) << c.name << " start " << jitter;
        }
      }
    }
  }
}

/** The head-on crowds (Dense, each filling the corridor) on 0 and 4 helper threads: bit for
 * bit (keep_right reads the neighbour grid on the helpers). */
TEST(KX_Navigation, TwoCrowdsMeetingHeadOnAreTheSameOnAnyNumberOfThreads)
{
  for (const bool dense : {false, true}) {
    std::vector<MT_Vector3> alone, many;
    RunHeadOn(dense, 4, 1.5f, 15.0f, 0, &alone);
    RunHeadOn(dense, 4, 1.5f, 15.0f, 4, &many);
    ASSERT_EQ(alone.size(), many.size());
    for (size_t i = 0; i < alone.size(); i++) {
      EXPECT_EQ(alone[i], many[i]) << (dense ? "dense " : "polite ") << i;
    }
  }
}

/** A thick wall taller than an NPC is solid: no navmesh inside it. Recast rasterizes surfaces
 * only, so the floor inside a closed box kept its headroom (up to the box's top) and stayed
 * walkable: an island in the wall, which NPCs pressed against it at a gate slid into (the
 * stadium exit of the game check, 2026-11-19). Closed convex pieces are filled now. The same
 * with the level read from Jolt's own bodies (boxes, as in a game). */
TEST(KX_Navigation, NoNavmeshInsideASolidWall)
{
  for (const bool from_jolt : {false, true}) {
    Level level;
    level.floors.push_back({MT_Vector3(-10, -10, 0), MT_Vector3(10, 10, 0)});
    /* A wall 1.4 m thick, 3 m high, from y -10 to -1.2 (a gate north of it). */
    level.boxes.push_back({MT_Vector3(-0.7f, -10, 0), MT_Vector3(0.7f, -1.2f, 3)});
    KX_Navigation::Settings settings = TestSettings();
    settings.cell_size = 0.15f;
    std::unique_ptr<KX_Navigation> navigation;
    Objects objects;
    World world;
    if (from_jolt) {
      navigation = std::make_unique<KX_Navigation>(nullptr);
      navigation->SetSettings(settings);
      world.StaticBox(objects.Add(MT_Vector3(0, 0, -0.5f)), MT_Vector3(10, 10, 0.5f));
      world.StaticBox(objects.Add(MT_Vector3(0, -5.6f, 1.5f)), MT_Vector3(0.7f, 4.4f, 1.5f));
      world.environment.GetPhysicsSystem()->OptimizeBroadPhase();
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
      navigation->SetPhysicsEnvironmentForTests(physics);
    }
    else {
      navigation = MakeNavigation(level, settings);
    }
    navigation->Step(1.0f / 60.0f, nullptr);
    KX_NavMesh *navmesh = navigation->GetSceneNavMesh();
    ASSERT_NE(navmesh, nullptr) << from_jolt;
    navigation->Step(1.0f / 60.0f, nullptr);
    ASSERT_GT(navmesh->PolygonCount(), 0) << from_jolt;
    /* Along the wall's middle, at the floor: the nearest navmesh point is outside the wall (the
     * floor beside it, a radius off), never inside it. */
    for (float y = -9.0f; y <= -1.6f; y += 0.4f) {
      MT_Vector3 point;
      if (navmesh->NearestPoint(MT_Vector3(0, y, 0.0f), point) && point.z() < 1.0f) {
        EXPECT_FALSE(std::fabs(float(point.x())) < 0.7f && point.y() < -1.2f)
            << (from_jolt ? "Jolt" : "test level") << ": navmesh inside the wall at ("
            << point.x() << ", " << point.y() << ")";
      }
    }
    /* The gate and the floor beside the wall are walkable. */
    MT_Vector3 point;
    ASSERT_TRUE(navmesh->NearestPoint(MT_Vector3(0, 0, 0), point));
    EXPECT_LT(distance_2d(point, MT_Vector3(0, 0, 0)), 0.05f);
    ASSERT_TRUE(navmesh->NearestPoint(MT_Vector3(-3, -5, 0), point));
    EXPECT_LT(distance_2d(point, MT_Vector3(-3, -5, 0)), 0.05f);
  }
}

/* -------------------------------------------------------------------- */
/** \name Flying and swimming (phase 8, 2026-11-19)
 * \{ */

KX_NavRequest FlyTo(KX_GameObject &object, const MT_Vector3 &goal, const float speed = 3.0f)
{
  KX_NavRequest request = GoTo(object, goal, speed);
  request.move = KX_NavRequest::Move::Fly;
  return request;
}

/** A flyer (no physics: it moves through free air) round a wall 8 m high to a goal 4 m up
 * beyond it: it arrives, never where the air map says it does not fit, never through the wall. */
TEST(KX_Navigation, AFlyerFliesRoundAWallToItsGoal)
{
  Level level;
  level.floors.push_back({MT_Vector3(-12, -12, 0), MT_Vector3(12, 12, 0)});
  level.boxes.push_back({MT_Vector3(-0.5f, -12, 0), MT_Vector3(0.5f, 6, 8)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  Objects objects;
  KX_GameObject &bird = objects.Add(MT_Vector3(-6, 0, 3));
  const MT_Vector3 goal(6, 0, 4);
  bool arrived = false;
  bool ever_blocked = false, ever_in_wall = false;
  int step = 0;
  for (; step < 60 * 30 && !arrived; step++) {
    navigation->Submit(FlyTo(bird, goal));
    navigation->Step(1.0f / 60.0f, nullptr);
    const MT_Vector3 p = bird.NodeGetWorldPosition();
    KX_NavAirMap *map = navigation->AirMapForTests(0.3f);
    ASSERT_NE(map, nullptr);
    ever_blocked = ever_blocked || !map->Free(p);
    ever_in_wall = ever_in_wall || (std::fabs(float(p.x())) < 0.5f + 0.3f && p.y() < 6.0f &&
                                    p.z() < 8.0f);
    KX_NavAgentInfo info;
    ASSERT_TRUE(navigation->GetAgentInfo(&bird, info));
    arrived = info.arrived;
  }
  printf("a flyer round a wall: %s in %.1f s\n", arrived ? "arrived" : "NOT arrived", step / 60.0f);
  EXPECT_TRUE(arrived);
  EXPECT_FALSE(ever_blocked);
  EXPECT_FALSE(ever_in_wall);
  EXPECT_LT((bird.NodeGetWorldPosition() - goal).length(), 0.35f);
}

/** A room with one window (1.6 m square, 2.2 to 3.8 m up) in its middle wall, no other way: a
 * flyer goes through the window. */
TEST(KX_Navigation, AFlyerFliesThroughAWindow)
{
  Level level;
  level.boxes.push_back({MT_Vector3(-10, -6, -1), MT_Vector3(10, 6, 0)});
  level.boxes.push_back({MT_Vector3(-10, -6, 6), MT_Vector3(10, 6, 7)});
  level.boxes.push_back({MT_Vector3(-10, -7, -1), MT_Vector3(10, -6, 7)});
  level.boxes.push_back({MT_Vector3(-10, 6, -1), MT_Vector3(10, 7, 7)});
  level.boxes.push_back({MT_Vector3(-0.3f, -6, 0), MT_Vector3(0.3f, -0.8f, 6)});
  level.boxes.push_back({MT_Vector3(-0.3f, 0.8f, 0), MT_Vector3(0.3f, 6, 6)});
  level.boxes.push_back({MT_Vector3(-0.3f, -0.8f, 0), MT_Vector3(0.3f, 0.8f, 2.2f)});
  level.boxes.push_back({MT_Vector3(-0.3f, -0.8f, 3.8f), MT_Vector3(0.3f, 0.8f, 6)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  Objects objects;
  KX_GameObject &drone = objects.Add(MT_Vector3(-6, -3, 2));
  const MT_Vector3 goal(6, 3, 4);
  bool arrived = false, through_window = false;
  MT_Vector3 last = drone.NodeGetWorldPosition();
  for (int step = 0; step < 60 * 30 && !arrived; step++) {
    navigation->Submit(FlyTo(drone, goal, 2.0f));
    navigation->Step(1.0f / 60.0f, nullptr);
    const MT_Vector3 p = drone.NodeGetWorldPosition();
    if ((last.x() < 0.0f) != (p.x() < 0.0f)) {
      through_window = std::fabs(float(p.y())) < 0.8f && p.z() > 2.2f && p.z() < 3.8f;
    }
    last = p;
    KX_NavAgentInfo info;
    navigation->GetAgentInfo(&drone, info);
    arrived = info.arrived;
  }
  EXPECT_TRUE(arrived);
  EXPECT_TRUE(through_window);
}

/** Two flocks of 16 crossing in open air at the same height, and 16 more above them the other
 * way: 3D avoidance keeps every pair apart (never deeper than a few centimetres inside each
 * other), all arrive. The same on 0 and 4 helper threads, bit for bit. */
void RunFlocks(const uint32_t helpers, std::vector<MT_Vector3> &r_positions, int &r_arrived,
               float &r_deepest)
{
  Level level;
  level.floors.push_back({MT_Vector3(-20, -20, 0), MT_Vector3(20, 20, 0)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  LN_JobPool pool;
  pool.SetHelperCount(helpers);
  Objects objects;
  std::vector<KX_GameObject *> flyers;
  std::vector<MT_Vector3> goals;
  for (int i = 0; i < 48; i++) {
    const int group = i / 16, k = i % 16;
    const float a = -1.8f + float(k % 4) * 1.2f, b = -1.8f + float(k / 4) * 1.2f;
    MT_Vector3 start, goal;
    if (group == 0) {
      start = MT_Vector3(-10, a, 5 + b * 0.5f), goal = MT_Vector3(10, a, 5 + b * 0.5f);
    }
    else if (group == 1) {
      start = MT_Vector3(a, -10, 5 + b * 0.5f), goal = MT_Vector3(a, 10, 5 + b * 0.5f);
    }
    else {
      start = MT_Vector3(10, a, 6.5f + b * 0.5f), goal = MT_Vector3(-10, a, 6.5f + b * 0.5f);
    }
    flyers.push_back(&objects.Add(start));
    goals.push_back(goal);
  }
  r_deepest = 0.0f;
  r_arrived = 0;
  for (int step = 0; step < 60 * 30 && r_arrived < int(flyers.size()); step++) {
    for (size_t i = 0; i < flyers.size(); i++) {
      KX_NavRequest request = FlyTo(*flyers[i], goals[i], 3.0f);
      request.arrive_distance = 0.4f;
      navigation->Submit(request);
    }
    navigation->Step(1.0f / 60.0f, helpers > 0 ? &pool : nullptr);
    r_arrived = 0;
    for (size_t i = 0; i < flyers.size(); i++) {
      KX_NavAgentInfo info;
      navigation->GetAgentInfo(flyers[i], info);
      r_arrived += info.arrived ? 1 : 0;
      for (size_t j = i + 1; j < flyers.size(); j++) {
        r_deepest = std::max(r_deepest, 0.6f - float((flyers[i]->NodeGetWorldPosition() -
                                                       flyers[j]->NodeGetWorldPosition())
                                                          .length()));
      }
    }
  }
  r_positions.clear();
  for (KX_GameObject *flyer : flyers) {
    r_positions.push_back(flyer->NodeGetWorldPosition());
  }
}

TEST(KX_Navigation, FlyersAvoidEachOtherInSpace)
{
  std::vector<MT_Vector3> alone, many;
  int arrived_alone = 0, arrived_many = 0;
  float deepest_alone = 0.0f, deepest_many = 0.0f;
  RunFlocks(0, alone, arrived_alone, deepest_alone);
  RunFlocks(4, many, arrived_many, deepest_many);
  printf("flocks crossing: %d of 48 arrived, deepest overlap %.3f m\n", arrived_alone,
         deepest_alone);
  EXPECT_EQ(arrived_alone, 48);
  EXPECT_LT(deepest_alone, 0.05f);
  ASSERT_EQ(alone.size(), many.size());
  for (size_t i = 0; i < alone.size(); i++) {
    EXPECT_EQ(alone[i], many[i]) << i;
  }
}

/** Flock mode in the air: 12 birds stacked 9 m tall draw together in height too (cohesion in
 * space), never into each other. */
TEST(KX_Navigation, AFlyingFlockHoldsTogetherInHeight)
{
  Level level;
  level.floors.push_back({MT_Vector3(-30, -30, 0), MT_Vector3(30, 30, 0)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  Objects objects;
  std::vector<KX_GameObject *> birds;
  for (int i = 0; i < 12; i++) {
    birds.push_back(&objects.Add(
        MT_Vector3(float(i % 2) * 1.5f, float((i / 2) % 2) * 1.5f, 3.0f + float(i) * 0.8f)));
  }
  auto spread = [&]() {
    float lo = 1e9f, hi = -1e9f;
    for (KX_GameObject *bird : birds) {
      lo = std::min(lo, float(bird->NodeGetWorldPosition().z()));
      hi = std::max(hi, float(bird->NodeGetWorldPosition().z()));
    }
    return hi - lo;
  };
  const float before = spread();
  float closest = 1e9f;
  for (int step = 0; step < 60 * 8; step++) {
    for (KX_GameObject *bird : birds) {
      KX_NavRequest request = FlyTo(*bird, MT_Vector3(0, 0, 0), 2.0f);
      request.mode = KX_NavRequest::Mode::Flock;
      request.radius = 15.0f;
      navigation->Submit(request);
    }
    navigation->Step(1.0f / 60.0f, nullptr);
    for (size_t i = 0; i < birds.size(); i++) {
      for (size_t j = i + 1; j < birds.size(); j++) {
        closest = std::min(closest, float((birds[i]->NodeGetWorldPosition() -
                                           birds[j]->NodeGetWorldPosition())
                                              .length()));
      }
    }
  }
  printf("a flying flock: height spread %.2f m -> %.2f m, closest %.2f m\n", before, spread(),
         closest);
  EXPECT_LT(spread(), before * 0.6f);
  EXPECT_GT(closest, 0.5f);
}

/** Flock with a Target Object heads for where the target is, also after it moved (walking on
 * the navmesh and flying); the request's Destination (the node's hidden input, the origin) is
 * no goal. 2026-11-19: both flocks went to the origin (found by the navigation examples). */
TEST(KX_Navigation, AFlockFollowsItsTargetObject)
{
  for (const bool fly : {false, true}) {
    Level level;
    level.floors.push_back({MT_Vector3(-40, -40, 0), MT_Vector3(40, 40, 0)});
    std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
    Objects objects;
    KX_GameObject &target = objects.Add(MT_Vector3(20, 10, fly ? 6.0f : 0.0f));
    std::vector<KX_GameObject *> flock;
    for (int i = 0; i < 8; i++) {
      flock.push_back(&objects.Add(MT_Vector3(-20.0f + float(i % 4) * 1.5f,
                                              -10.0f + float(i / 4) * 1.5f,
                                              fly ? 4.0f : 0.0f)));
    }
    auto middle = [&]() {
      MT_Vector3 sum(0, 0, 0);
      for (KX_GameObject *bird : flock) {
        sum += bird->NodeGetWorldPosition();
      }
      return sum / float(flock.size());
    };
    for (int step = 0; step < 60 * 28; step++) {
      if (step == 60 * 12) {
        /* The target moves on: the flock turns after it. */
        target.NodeSetWorldPosition(MT_Vector3(-15, 25, fly ? 8.0f : 0.0f));
        target.NodeUpdateGS(0.0);
      }
      for (KX_GameObject *bird : flock) {
        KX_NavRequest request = fly ? FlyTo(*bird, MT_Vector3(0, 0, 0), 4.0f) :
                                      GoTo(*bird, MT_Vector3(0, 0, 0), 4.0f);
        request.mode = KX_NavRequest::Mode::Flock;
        request.radius = 6.0f;
        request.target = &target;
        request.goal = true;
        navigation->Submit(request);
      }
      navigation->Step(1.0f / 60.0f, nullptr);
      if (step == 60 * 12 - 1) {
        const float off = float((middle() - target.NodeGetWorldPosition()).length());
        printf("a %s flock after its target: %.2f m from it at 12 s\n",
               fly ? "flying" : "walking", off);
        EXPECT_LT(off, 4.0f);
      }
    }
    const float off = float((middle() - target.NodeGetWorldPosition()).length());
    const float from_origin = float(middle().length());
    printf("  after it moved: %.2f m from it (%.2f m from the origin)\n", off, from_origin);
    EXPECT_LT(off, 4.0f);
    EXPECT_GT(from_origin, 15.0f);
  }
}

/** A Dynamic body (Jolt, the scene's gravity, Move as Dynamic) flies up to a point 5 m above the
 * floor and hovers there while Navigate runs (its gravity undone each step); no longer asked, it
 * is a body like any other and falls. */
TEST(KX_Navigation, ADynamicFlyerHoversAndFallsWhenNoLongerAsked)
{
  Level level;
  level.floors.push_back({MT_Vector3(-10, -10, 0), MT_Vector3(10, 10, 0)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  Objects objects;
  World world;
  world.StaticBox(objects.Add(MT_Vector3(0, 0, -0.5f)), MT_Vector3(10, 10, 0.5f));
  KX_GameObject &drone = objects.Add(MT_Vector3(-5, 0, 0.3f));
  world.Add(drone, new JPH::BoxShape(JPH::Vec3(0.3f, 0.3f, 0.3f)), JPH::EMotionType::Dynamic);
  world.environment.GetPhysicsSystem()->OptimizeBroadPhase();
  navigation->SetPhysicsEnvironmentForTests(&world.environment);
  const MT_Vector3 goal(3, 0, 5);
  float lowest_after = 1e9f, highest_after = -1e9f;
  for (int step = 0; step < 60 * 10; step++) {
    KX_NavRequest request = FlyTo(drone, goal, 2.0f);
    request.move_dynamic = true;
    navigation->Submit(request);
    navigation->Step(1.0f / 60.0f, nullptr);
    world.Step({&drone});
    if (step >= 60 * 8) {
      lowest_after = std::min(lowest_after, float(drone.NodeGetWorldPosition().z()));
      highest_after = std::max(highest_after, float(drone.NodeGetWorldPosition().z()));
    }
  }
  printf("a Dynamic flyer: at (%.2f %.2f %.2f), hovering %.3f .. %.3f m\n",
         drone.NodeGetWorldPosition().x(), drone.NodeGetWorldPosition().y(),
         drone.NodeGetWorldPosition().z(), lowest_after, highest_after);
  EXPECT_LT((drone.NodeGetWorldPosition() - goal).length(), 0.5f);
  EXPECT_LT(highest_after - lowest_after, 0.05f);
  /* No longer asked: it falls. */
  for (int step = 0; step < 60 * 2; step++) {
    navigation->Step(1.0f / 60.0f, nullptr);
    world.Step({&drone});
  }
  EXPECT_LT(drone.NodeGetWorldPosition().z(), 1.0f);
}

/** A Dynamic flyer crossing 16 m level at 3 m keeps its height all the way (its gravity undone
 * at every step of the flight, not only while it hovers: each step's fall left in, the steering
 * would hold it a metre low). */
TEST(KX_Navigation, ADynamicFlyerFliesLevel)
{
  Level level;
  level.floors.push_back({MT_Vector3(-12, -12, 0), MT_Vector3(12, 12, 0)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  Objects objects;
  World world;
  world.StaticBox(objects.Add(MT_Vector3(0, 0, -0.5f)), MT_Vector3(12, 12, 0.5f));
  KX_GameObject &drone = objects.Add(MT_Vector3(-8, 0, 3));
  world.Add(drone, new JPH::BoxShape(JPH::Vec3(0.3f, 0.3f, 0.3f)), JPH::EMotionType::Dynamic);
  world.environment.GetPhysicsSystem()->OptimizeBroadPhase();
  navigation->SetPhysicsEnvironmentForTests(&world.environment);
  const MT_Vector3 goal(8, 0, 3);
  float lowest = 1e9f, highest = -1e9f;
  bool arrived = false;
  for (int step = 0; step < 60 * 15 && !arrived; step++) {
    KX_NavRequest request = FlyTo(drone, goal, 2.0f);
    request.move_dynamic = true;
    navigation->Submit(request);
    navigation->Step(1.0f / 60.0f, nullptr);
    world.Step({&drone});
    lowest = std::min(lowest, float(drone.NodeGetWorldPosition().z()));
    highest = std::max(highest, float(drone.NodeGetWorldPosition().z()));
    KX_NavAgentInfo info;
    navigation->GetAgentInfo(&drone, info);
    arrived = info.arrived;
  }
  printf("a Dynamic flyer level: z %.3f .. %.3f\n", lowest, highest);
  EXPECT_TRUE(arrived);
  EXPECT_GT(lowest, 2.9f);
  EXPECT_LT(highest, 3.1f);
}

/** At most four air map sizes (each maps the whole level): a fifth flyer size flies by the
 * nearest larger map, else the largest (warned once). */
TEST(KX_Navigation, AtMostFourAirMapSizes)
{
  Level level;
  level.floors.push_back({MT_Vector3(-10, -10, 0), MT_Vector3(10, 10, 0)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  KX_NavAirMap *maps[4];
  const float radii[4] = {0.2f, 0.3f, 0.45f, 0.6f};
  for (int i = 0; i < 4; i++) {
    maps[i] = navigation->AirMapForTests(radii[i]);
    ASSERT_NE(maps[i], nullptr);
    EXPECT_FLOAT_EQ(maps[i]->Radius(), radii[i]);
    for (int j = 0; j < i; j++) {
      EXPECT_NE(maps[i], maps[j]);
    }
  }
  /* (Within a quarter and 5 cm above a map's radius: that map.) */
  EXPECT_EQ(navigation->AirMapForTests(0.28f), maps[1]);
  EXPECT_EQ(navigation->AirMapForTests(0.5f), maps[3]);
  EXPECT_EQ(navigation->AirMapForTests(0.9f), maps[3]);
}

/** A Character cannot fly (its body falls by its own gravity): asked to, it stands where it is. */
TEST(KX_Navigation, ACharacterAskedToFlyStands)
{
  Level level;
  level.floors.push_back({MT_Vector3(-10, -10, 0), MT_Vector3(10, 10, 0)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  Objects objects;
  World world;
  world.StaticBox(objects.Add(MT_Vector3(0, 0, -0.5f)), MT_Vector3(10, 10, 0.5f));
  KX_GameObject &hero = objects.Add(MT_Vector3(-5, 0, 0.95f));
  ASSERT_NE(world.Character(hero), nullptr);
  world.environment.GetPhysicsSystem()->OptimizeBroadPhase();
  navigation->SetPhysicsEnvironmentForTests(&world.environment);
  for (int step = 0; step < 60 * 3; step++) {
    navigation->Submit(FlyTo(hero, MT_Vector3(5, 0, 5)));
    navigation->Step(1.0f / 60.0f, nullptr);
    world.Step({&hero});
  }
  EXPECT_NEAR(hero.NodeGetWorldPosition().x(), -5.0f, 0.1f);
}

/** A fish (no physics) swims to the tank's other end round a rock standing in it: it keeps to
 * the water (never above its surface), never inside the rock, and arrives. */
TEST(KX_Navigation, AFishSwimsRoundARockInTheWater)
{
  Level level;
  level.boxes.push_back({MT_Vector3(-10, -6, -6), MT_Vector3(10, 6, -4)});
  level.boxes.push_back({MT_Vector3(-1, -6, -4), MT_Vector3(1, 2, 3)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  KX_NavWaterVolume water;
  water.center = MT_Vector3(0, 0, -2);
  water.half = MT_Vector3(10, 6, 2);
  navigation->SetWaterForTests({water});
  Objects objects;
  KX_GameObject &fish = objects.Add(MT_Vector3(-6, -3, -2));
  const MT_Vector3 goal(6, -3, -2);
  bool arrived = false;
  float highest = -1e9f;
  bool in_rock = false;
  for (int step = 0; step < 60 * 30 && !arrived; step++) {
    KX_NavRequest request = GoTo(fish, goal, 2.0f);
    request.move = KX_NavRequest::Move::Swim;
    navigation->Submit(request);
    navigation->Step(1.0f / 60.0f, nullptr);
    const MT_Vector3 p = fish.NodeGetWorldPosition();
    highest = std::max(highest, float(p.z()));
    in_rock = in_rock || (std::fabs(float(p.x())) < 1.0f && p.y() < 2.0f);
    KX_NavAgentInfo info;
    navigation->GetAgentInfo(&fish, info);
    arrived = info.arrived;
  }
  printf("a fish round a rock: %s, highest %.2f m\n", arrived ? "arrived" : "NOT arrived",
         highest);
  EXPECT_TRUE(arrived);
  EXPECT_LT(highest, 0.0f);
  EXPECT_FALSE(in_rock);
}

/** Wandering in the air: every point it goes to is free air within the radius of home, and it
 * keeps going (several points reached). */
TEST(KX_Navigation, AFlyerWandersInTheBallRoundHome)
{
  Level level;
  level.floors.push_back({MT_Vector3(-20, -20, 0), MT_Vector3(20, 20, 0)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  Objects objects;
  KX_GameObject &bat = objects.Add(MT_Vector3(0, 0, 6));
  const MT_Vector3 home(0, 0, 6);
  float farthest = 0.0f, lowest = 1e9f;
  for (int step = 0; step < 60 * 20; step++) {
    KX_NavRequest request = FlyTo(bat, home, 3.0f);
    request.mode = KX_NavRequest::Mode::Wander;
    request.radius = 4.0f;
    request.seed = 77;
    navigation->Submit(request);
    navigation->Step(1.0f / 60.0f, nullptr);
    farthest = std::max(farthest, float((bat.NodeGetWorldPosition() - home).length()));
    lowest = std::min(lowest, float(bat.NodeGetWorldPosition().z()));
  }
  printf("a flyer wandering: farthest %.2f m from home, lowest %.2f m\n", farthest, lowest);
  EXPECT_LT(farthest, 4.5f);
  EXPECT_GT(farthest, 1.5f);
  EXPECT_GT(lowest, 0.3f);
}

/** A wall appearing on a flyer's way (a level piece added while playing): the air map is built
 * again there, the flyer searches again and goes round, never through it. */
TEST(KX_Navigation, AFlyerGoesRoundAWallThatAppears)
{
  Level level;
  level.floors.push_back({MT_Vector3(-15, -15, 0), MT_Vector3(15, 15, 0)});
  level.boxes.push_back({MT_Vector3(-0.5f, -15, 0), MT_Vector3(0.5f, 8, 12)});
  level.boxes.back().enabled = false;
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  Objects objects;
  KX_GameObject &bird = objects.Add(MT_Vector3(-10, 0, 4));
  const MT_Vector3 goal(10, 0, 4);
  bool arrived = false, through = false;
  MT_Vector3 last = bird.NodeGetWorldPosition();
  for (int step = 0; step < 60 * 40 && !arrived; step++) {
    if (step == 30) {
      level.boxes.back().enabled = true;
      navigation->NoteStaticChanged(MT_Vector3(-0.5f, -15, 0), MT_Vector3(0.5f, 8, 12));
    }
    navigation->Submit(FlyTo(bird, goal, 3.0f));
    navigation->Step(1.0f / 60.0f, nullptr);
    const MT_Vector3 p = bird.NodeGetWorldPosition();
    if (step > 30 && (last.x() < 0.0f) != (p.x() < 0.0f)) {
      through = p.y() < 8.0f && p.z() < 12.0f;
    }
    last = p;
    KX_NavAgentInfo info;
    navigation->GetAgentInfo(&bird, info);
    arrived = info.arrived;
  }
  EXPECT_TRUE(arrived);
  EXPECT_FALSE(through);
}

/** A flyer climbing steeply to a perch 12 m above it (1 m across) has not stalled (it moves in
 * space; measured along the ground it crept 1 m in 4 s), and its speed for animations is its
 * speed in space. (Speeding up from rest counts as a few hundredths of a second.) */
TEST(KX_Navigation, AFlyerClimbingStraightUpHasNotStalled)
{
  Level level;
  level.floors.push_back({MT_Vector3(-10, -10, 0), MT_Vector3(10, 10, 0)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  Objects objects;
  KX_GameObject &bat = objects.Add(MT_Vector3(0, 0, 1));
  const MT_Vector3 goal(1, 0, 13);
  float most_stalled = 0.0f, fastest = 0.0f;
  bool arrived = false, had_way = true, said_no_way = false;
  for (int step = 0; step < 60 * 10 && !arrived; step++) {
    navigation->Submit(FlyTo(bat, goal, 3.0f));
    navigation->Step(1.0f / 60.0f, nullptr);
    most_stalled = std::max(most_stalled, navigation->StallSecondsForTests(&bat));
    KX_NavAgentInfo info;
    ASSERT_TRUE(navigation->GetAgentInfo(&bat, info));
    fastest = std::max(fastest, info.speed);
    arrived = info.arrived;
    /* (Get Navigation Info: its way through the air, whole; never "no path".) */
    if (!arrived) {
      had_way = had_way && info.has_path && info.path_complete;
    }
    said_no_way = said_no_way || info.no_path;
  }
  EXPECT_TRUE(arrived);
  EXPECT_LT(most_stalled, 0.3f);
  EXPECT_GT(fastest, 2.5f);
  EXPECT_TRUE(had_way);
  EXPECT_FALSE(said_no_way);
}

/** Flee and Evade measure in space for a flyer: high above its threat (straight above it) it is
 * already far enough and stays; near it, it gets that far away in space. */
TEST(KX_Navigation, AFlyerFleesByTheDistanceInSpace)
{
  Level level;
  level.floors.push_back({MT_Vector3(-20, -20, 0), MT_Vector3(20, 20, 0)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  Objects objects;
  KX_GameObject &cat = objects.Add(MT_Vector3(0, 0, 0.5f));
  KX_GameObject &high = objects.Add(MT_Vector3(0, 0, 12));
  KX_GameObject &low = objects.Add(MT_Vector3(1, 0, 2));
  auto flee = [&](KX_GameObject &bird) {
    KX_NavRequest request = FlyTo(bird, MT_Vector3(0, 0, 0), 4.0f);
    request.mode = KX_NavRequest::Mode::Flee;
    request.target = &cat;
    request.radius = 6.0f;
    return request;
  };
  for (int step = 0; step < 60 * 6; step++) {
    navigation->Submit(flee(high));
    navigation->Submit(flee(low));
    navigation->Step(1.0f / 60.0f, nullptr);
  }
  EXPECT_LT((high.NodeGetWorldPosition() - MT_Vector3(0, 0, 12)).length(), 0.01f);
  EXPECT_GT((low.NodeGetWorldPosition() - cat.NodeGetWorldPosition()).length(), 5.9f);
  KX_NavAgentInfo info;
  ASSERT_TRUE(navigation->GetAgentInfo(&high, info));
  EXPECT_TRUE(info.arrived);
}

/** Pursuing a target that climbs: a flyer heads for where it will be in space (above it). */
TEST(KX_Navigation, AFlyerPursuesWhereAClimbingTargetWillBe)
{
  Level level;
  level.floors.push_back({MT_Vector3(-30, -30, 0), MT_Vector3(30, 30, 0)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  Objects objects;
  KX_GameObject &prey = objects.Add(MT_Vector3(0, 0, 2));
  KX_GameObject &hawk = objects.Add(MT_Vector3(-15, 0, 2));
  float lead = 0.0f;
  for (int step = 0; step < 60 && !std::isnan(lead); step++) {
    navigation->Submit(FlyTo(prey, MT_Vector3(0, 0, 20), 2.0f));
    KX_NavRequest request = FlyTo(hawk, MT_Vector3(0, 0, 0), 3.0f);
    request.mode = KX_NavRequest::Mode::Pursue;
    request.target = &prey;
    navigation->Submit(request);
    navigation->Step(1.0f / 60.0f, nullptr);
    KX_NavAgentInfo info;
    ASSERT_TRUE(navigation->GetAgentInfo(&hawk, info));
    lead = float(info.goal.z() - prey.NodeGetWorldPosition().z());
  }
  /* About its speed (2 m/s) times the time to get there (3 s at most): well above it. */
  EXPECT_GT(lead, 2.0f);
}

/** Wandering round a sealed hut it cannot enter: a point inside the hut is given up once the
 * flyer got as near as it can, and it wanders on (several points). */
TEST(KX_Navigation, AFlyerGivesUpAWanderPointItCannotReach)
{
  Level level;
  level.floors.push_back({MT_Vector3(-15, -15, 0), MT_Vector3(15, 15, 0)});
  level.boxes.push_back({MT_Vector3(-3, -3, 0), MT_Vector3(-2.6f, 3, 4)});
  level.boxes.push_back({MT_Vector3(2.6f, -3, 0), MT_Vector3(3, 3, 4)});
  level.boxes.push_back({MT_Vector3(-3, -3, 0), MT_Vector3(3, -2.6f, 4)});
  level.boxes.push_back({MT_Vector3(-3, 2.6f, 0), MT_Vector3(3, 3, 4)});
  level.boxes.push_back({MT_Vector3(-3, -3, 4), MT_Vector3(3, 3, 4.4f)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  Objects objects;
  KX_GameObject &bat = objects.Add(MT_Vector3(-8, 0, 2));
  MT_Vector3 last_goal(1e9f, 0, 0);
  int goals = 0;
  bool inside = false;
  for (int step = 0; step < 60 * 30; step++) {
    KX_NavRequest request = FlyTo(bat, MT_Vector3(0, 0, 2), 3.0f);
    request.mode = KX_NavRequest::Mode::Wander;
    request.radius = 4.0f;
    request.seed = 5;
    navigation->Submit(request);
    navigation->Step(1.0f / 60.0f, nullptr);
    KX_NavAgentInfo info;
    ASSERT_TRUE(navigation->GetAgentInfo(&bat, info));
    if ((info.goal - last_goal).length2() > 1e-6f) {
      goals++;
      last_goal = info.goal;
    }
    const MT_Vector3 p = bat.NodeGetWorldPosition();
    inside = inside || (std::fabs(float(p.x())) < 2.6f && std::fabs(float(p.y())) < 2.6f &&
                        p.z() < 4.0f);
  }
  printf("a flyer wandering round a sealed hut: %d points\n", goals);
  EXPECT_GE(goals, 5);
  EXPECT_FALSE(inside);
}

/** A Dynamic swimmer with Gravity Factor 0 (a fish, Move as Dynamic) dives to a goal deep in the
 * tank, moved in three dimensions; it never leaves the water. */
TEST(KX_Navigation, ADynamicFishWithNoGravityDives)
{
  Level level;
  level.boxes.push_back({MT_Vector3(-10, -6, -8), MT_Vector3(10, 6, -6)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  KX_NavWaterVolume water;
  water.center = MT_Vector3(0, 0, -3);
  water.half = MT_Vector3(10, 6, 3);
  navigation->SetWaterForTests({water});
  Objects objects;
  World world;
  world.StaticBox(objects.Add(MT_Vector3(0, 0, -7)), MT_Vector3(10, 6, 1));
  KX_GameObject &fish = objects.Add(MT_Vector3(-6, 0, -1));
  Body &body = world.Add(fish, new JPH::SphereShape(0.3f), JPH::EMotionType::Dynamic);
  body.controller.SetGravityFactor(0.0f);
  world.environment.GetPhysicsSystem()->OptimizeBroadPhase();
  navigation->SetPhysicsEnvironmentForTests(&world.environment);
  const MT_Vector3 goal(5, 0, -5);
  bool arrived = false;
  float highest = -1e9f;
  for (int step = 0; step < 60 * 20 && !arrived; step++) {
    KX_NavRequest request = GoTo(fish, goal, 2.0f);
    request.move = KX_NavRequest::Move::Swim;
    request.move_dynamic = true;
    navigation->Submit(request);
    navigation->Step(1.0f / 60.0f, nullptr);
    world.Step({&fish});
    highest = std::max(highest, float(fish.NodeGetWorldPosition().z()));
    KX_NavAgentInfo info;
    navigation->GetAgentInfo(&fish, info);
    arrived = info.arrived;
  }
  printf("a Dynamic fish diving: %s at (%.2f %.2f %.2f)\n", arrived ? "arrived" : "NOT arrived",
         fish.NodeGetWorldPosition().x(), fish.NodeGetWorldPosition().y(),
         fish.NodeGetWorldPosition().z());
  EXPECT_TRUE(arrived);
  EXPECT_LT((fish.NodeGetWorldPosition() - goal).length(), 0.5f);
  EXPECT_LT(highest, 0.0f);
}

/** A flyer placed beside the level, below its top (there is no way round there: the space under a
 * terrain stays sealed), climbs straight up out of it and flies in to its goal. */
TEST(KX_Navigation, AFlyerBesideTheLevelClimbsOutOfIt)
{
  Level level;
  level.floors.push_back({MT_Vector3(-10, -10, 0), MT_Vector3(10, 10, 0)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  Objects objects;
  KX_GameObject &bird = objects.Add(MT_Vector3(15, 0, 2));
  const MT_Vector3 goal(0, 0, 3);
  bool arrived = false;
  for (int step = 0; step < 60 * 60 && !arrived; step++) {
    navigation->Submit(FlyTo(bird, goal, 4.0f));
    navigation->Step(1.0f / 60.0f, nullptr);
    KX_NavAgentInfo info;
    navigation->GetAgentInfo(&bird, info);
    arrived = info.arrived;
  }
  EXPECT_TRUE(arrived);
}

/** Level pieces added outside the air map's box (a new part of the level beside it), or to a map
 * made when there was no level at all (open sky): the map is built whole again, a new box, and
 * flyers go round the new walls. */
TEST(KX_Navigation, APieceAddedOutsideTheAirMapIsMapped)
{
  for (const bool open_sky : {false, true}) {
    Level level;
    if (!open_sky) {
      level.floors.push_back({MT_Vector3(-10, -10, 0), MT_Vector3(10, 10, 0)});
    }
    /* The new part: a floor beyond and a wall across it (a gap at y 6..10). */
    level.floors.push_back({MT_Vector3(10, -10, 0), MT_Vector3(30, 10, 0)});
    level.floors.back().enabled = false;
    level.boxes.push_back({MT_Vector3(19.5f, -10, 0), MT_Vector3(20.5f, 6, 12)});
    level.boxes.back().enabled = false;
    std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
    Objects objects;
    KX_GameObject &bird = objects.Add(MT_Vector3(0, 0, 3));
    const MT_Vector3 goal(25, 0, 3);
    bool arrived = false, through = false;
    MT_Vector3 last = bird.NodeGetWorldPosition();
    for (int step = 0; step < 60 * 40 && !arrived; step++) {
      if (step == 10) {
        level.floors.back().enabled = true;
        level.boxes.back().enabled = true;
        navigation->NoteStaticChanged(MT_Vector3(10, -10, 0), MT_Vector3(30, 10, 0));
        navigation->NoteStaticChanged(MT_Vector3(19.5f, -10, 0), MT_Vector3(20.5f, 6, 12));
      }
      navigation->Submit(FlyTo(bird, goal, 4.0f));
      navigation->Step(1.0f / 60.0f, nullptr);
      const MT_Vector3 p = bird.NodeGetWorldPosition();
      if ((last.x() < 20.0f) != (p.x() < 20.0f)) {
        through = p.y() < 6.0f && p.z() < 12.0f;
      }
      last = p;
      KX_NavAgentInfo info;
      navigation->GetAgentInfo(&bird, info);
      arrived = info.arrived;
    }
    EXPECT_TRUE(arrived) << (open_sky ? "open sky" : "beside the level");
    EXPECT_FALSE(through) << (open_sky ? "open sky" : "beside the level");
    EXPECT_LT((bird.NodeGetWorldPosition() - goal).length(), 0.5f);
  }
}

/** The water rises (a flooded room): within kWaterCheckSteps the maps take it, a fish reaches a
 * point that was above the water; a tank moved beside the level is mapped whole again. */
TEST(KX_Navigation, WaterThatMovesIsFollowed)
{
  Level level;
  level.boxes.push_back({MT_Vector3(-10, -6, -6), MT_Vector3(10, 6, -4)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  KX_NavWaterVolume water;
  water.center = MT_Vector3(0, 0, -2);
  water.half = MT_Vector3(10, 6, 2);
  navigation->SetWaterForTests({water});
  Objects objects;
  KX_GameObject &fish = objects.Add(MT_Vector3(-6, 0, -2));
  const MT_Vector3 goal(5, 0, 1.5f);
  auto swim = [&](KX_GameObject &swimmer, const MT_Vector3 &to) {
    KX_NavRequest request = GoTo(swimmer, to, 2.0f);
    request.move = KX_NavRequest::Move::Swim;
    navigation->Submit(request);
  };
  float highest = -1e9f;
  for (int step = 0; step < 60 * 10; step++) {
    swim(fish, goal);
    navigation->Step(1.0f / 60.0f, nullptr);
    highest = std::max(highest, float(fish.NodeGetWorldPosition().z()));
  }
  /* (The water's top is 0: a fish keeps below it.) */
  EXPECT_LT(highest, 0.0f);
  water.center = MT_Vector3(0, 0, -1);
  water.half = MT_Vector3(10, 6, 3);
  navigation->SetWaterForTests({water});
  bool arrived = false;
  for (int step = 0; step < 60 * 20 && !arrived; step++) {
    swim(fish, goal);
    navigation->Step(1.0f / 60.0f, nullptr);
    KX_NavAgentInfo info;
    navigation->GetAgentInfo(&fish, info);
    arrived = info.arrived;
  }
  EXPECT_TRUE(arrived);
  /* The tank moved beside the level (outside the map's box): a fish there swims in it. */
  KX_NavWaterVolume moved;
  moved.center = MT_Vector3(40, 0, -2);
  moved.half = MT_Vector3(6, 6, 2);
  navigation->SetWaterForTests({moved});
  KX_GameObject &other = objects.Add(MT_Vector3(36, 0, -2));
  const MT_Vector3 other_goal(44, 0, -3);
  arrived = false;
  for (int step = 0; step < 60 * 20 && !arrived; step++) {
    swim(other, other_goal);
    navigation->Step(1.0f / 60.0f, nullptr);
    KX_NavAgentInfo info;
    navigation->GetAgentInfo(&other, info);
    arrived = info.arrived;
  }
  EXPECT_TRUE(arrived);
  EXPECT_LT((other.NodeGetWorldPosition() - other_goal).length(), 0.5f);
}

/** A flyer asked into a closed hut waits as near as it gets (its way ends short; tried again
 * less and less often); a door opening anywhere (a level piece removed) makes every way that
 * ended short search again at once: it goes in within seconds, not at its next try (up to 10 s
 * later). */
TEST(KX_Navigation, AFlyerEntersWhenADoorOpens)
{
  Level level;
  level.floors.push_back({MT_Vector3(-15, -15, 0), MT_Vector3(15, 15, 0)});
  level.boxes.push_back({MT_Vector3(3, -3, 0), MT_Vector3(3.4f, 3, 4)});
  level.boxes.push_back({MT_Vector3(8.6f, -3, 0), MT_Vector3(9, 3, 4)});
  level.boxes.push_back({MT_Vector3(3, -3, 0), MT_Vector3(9, -2.6f, 4)});
  level.boxes.push_back({MT_Vector3(3, 2.6f, 0), MT_Vector3(9, 3, 4)});
  level.boxes.push_back({MT_Vector3(3, -3, 4), MT_Vector3(9, 3, 4.4f)});
  /* The door: the west wall's middle (the wall above split round it). */
  level.boxes[0] = {MT_Vector3(3, -3, 0), MT_Vector3(3.4f, -1.2f, 4)};
  level.boxes.push_back({MT_Vector3(3, 1.2f, 0), MT_Vector3(3.4f, 3, 4)});
  level.boxes.push_back({MT_Vector3(3, -1.2f, 2.8f), MT_Vector3(3.4f, 1.2f, 4)});
  level.boxes.push_back({MT_Vector3(3, -1.2f, 0), MT_Vector3(3.4f, 1.2f, 2.8f)});
  Level::Box &door = level.boxes.back();
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  Objects objects;
  KX_GameObject &bird = objects.Add(MT_Vector3(-6, 0, 2));
  const MT_Vector3 goal(6, 0, 1.5f);
  const int opened = 60 * 25;
  int arrived_at = -1;
  bool inside_early = false;
  for (int step = 0; step < 60 * 35 && arrived_at < 0; step++) {
    if (step == opened) {
      door.enabled = false;
      navigation->NoteStaticChanged(door.min, door.max);
    }
    navigation->Submit(FlyTo(bird, goal, 4.0f));
    navigation->Step(1.0f / 60.0f, nullptr);
    const MT_Vector3 p = bird.NodeGetWorldPosition();
    if (step < opened) {
      inside_early = inside_early || (p.x() > 3.4f && p.x() < 8.6f && std::fabs(float(p.y())) <
                                                                          2.6f && p.z() < 4.0f);
    }
    KX_NavAgentInfo info;
    navigation->GetAgentInfo(&bird, info);
    if (info.arrived && step >= opened) {
      arrived_at = step;
    }
  }
  EXPECT_FALSE(inside_early);
  ASSERT_GE(arrived_at, 0);
  printf("a flyer entering when the door opened: in %.1f s\n", (arrived_at - opened) / 60.0f);
  EXPECT_LT(arrived_at - opened, 60 * 4);
}

/** Navmesh Query's air modes alone (no NPC navigates): the map of the asked size is made at the
 * next step, then the queries answer. */
TEST(KX_Navigation, AirQueriesAloneGetTheirMap)
{
  Level level;
  level.floors.push_back({MT_Vector3(-10, -10, 0), MT_Vector3(10, 10, 0)});
  level.boxes.push_back({MT_Vector3(-0.5f, -10, 0), MT_Vector3(0.5f, 6, 8)});
  std::unique_ptr<KX_Navigation> navigation =
      MakeNavigation(level, TestSettings(0 /* no navmesh built */));
  std::vector<MT_Vector3> points;
  bool complete = false;
  EXPECT_FALSE(navigation->AirFindPath(
      MT_Vector3(-5, 0, 3), MT_Vector3(5, 0, 3), 0.4f, false, points, &complete));
  navigation->Step(1.0f / 60.0f, nullptr);
  ASSERT_TRUE(navigation->AirFindPath(
      MT_Vector3(-5, 0, 3), MT_Vector3(5, 0, 3), 0.4f, false, points, &complete));
  EXPECT_TRUE(complete);
  EXPECT_GT(points.size(), 1u);
  EXPECT_FALSE(navigation->AirLineClear(MT_Vector3(-5, 0, 3), MT_Vector3(5, 0, 3), 0.4f, false));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Walls and ceilings (phase 9, 2026-11-19)
 * \{ */

KX_NavRequest CrawlTo(KX_GameObject &object,
                      const MT_Vector3 &goal,
                      const float speed = 2.0f,
                      const KX_NavRequest::Climb climb = KX_NavRequest::Climb::Walls)
{
  KX_NavRequest request = GoTo(object, goal, speed);
  request.climb = climb;
  request.look_at = true;
  request.rot_speed = 10.0f;
  return request;
}

/** A floor 16 x 10 m and a wall across it from side to side, 2.5 m high, 0.4 m thick at x 0:
 * the level's sides are the map's (nothing goes round). */
Level WallAcross(const float height = 2.5f)
{
  Level level;
  level.floors.push_back({MT_Vector3(-8, -5, 0), MT_Vector3(8, 5, 0)});
  level.boxes.push_back({MT_Vector3(-0.2f, -5, 0), MT_Vector3(0.2f, 5, height)});
  return level;
}

/** The way a crawler goes: over the wall (it climbs one side, crosses the top, climbs down),
 * never inside it, its body at its height from the surface, its up along the surface's normal
 * on the wall; arrived on the floor beyond. */
TEST(KX_Navigation, ACrawlerClimbsOverAWall)
{
  Level level = WallAcross();
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  Objects objects;
  KX_GameObject &spider = objects.Add(MT_Vector3(-5, 0, 0.3f));
  const MT_Vector3 goal(5, 0, 0);
  bool arrived = false, inside = false, stood_on_wall = false;
  float top = -1e9f, deepest = 0.0f;
  int step = 0;
  for (; step < 60 * 30 && !arrived; step++) {
    navigation->Submit(CrawlTo(spider, goal));
    navigation->Step(1.0f / 60.0f, nullptr);
    const MT_Vector3 p = spider.NodeGetWorldPosition();
    top = std::max(top, float(p.z()));
    inside = inside || (std::fabs(float(p.x())) < 0.2f && p.z() < 2.5f);
    /* (Its body's height from the nearest surface: floor, wall sides, top.) */
    const float to_floor = float(p.z());
    const float to_side = std::fabs(float(p.x())) - 0.2f;
    const float to_top = float(p.z()) - 2.5f;
    float gap = to_floor;
    if (p.z() < 2.5f) {
      gap = std::min(gap, to_side);
    }
    else if (std::fabs(float(p.x())) < 0.2f) {
      gap = to_top;
    }
    else {
      gap = std::min(gap, std::sqrt(to_side * to_side + to_top * to_top));
    }
    if (step > 30) {
      deepest = std::max(deepest, 0.3f - gap);
    }
    KX_NavAgentInfo info;
    ASSERT_TRUE(navigation->GetAgentInfo(&spider, info));
    arrived = info.arrived;
    if (p.z() > 1.0f && p.z() < 2.0f && std::fabs(float(info.surface_normal.x())) > 0.95f) {
      const MT_Vector3 up = spider.NodeGetWorldOrientation() * MT_Vector3(0, 0, 1);
      stood_on_wall = stood_on_wall || float(up.dot(info.surface_normal)) > 0.95f;
    }
  }
  printf("a crawler over a wall: %s in %.1f s, top %.2f m, deepest %.3f m\n",
         arrived ? "arrived" : "NOT arrived", step / 60.0f, top, deepest);
  EXPECT_TRUE(arrived);
  /* (About 11 m at 2 m/s: 7.4 s; creeping to the wall's foot pressed into the floor, 13.6.) */
  EXPECT_LT(step, 60 * 10);
  EXPECT_GT(top, 2.6f);
  EXPECT_FALSE(inside);
  EXPECT_LT(deepest, 0.05f);
  EXPECT_TRUE(stood_on_wall);
  EXPECT_LT((spider.NodeGetWorldPosition() - MT_Vector3(5, 0, 0.3f)).length(), 0.5f);
  /* Standing on the floor again: up is up. */
  const MT_Vector3 up = spider.NodeGetWorldOrientation() * MT_Vector3(0, 0, 1);
  EXPECT_GT(up.z(), 0.95f);
}

/** Walking the floors only (Climb Floors), the same NPC with no navmesh walks straight into the
 * wall's foot and no further; Climb Walls gets it over (above). */
TEST(KX_Navigation, ClimbFloorsNeverClimbs)
{
  Level level = WallAcross();
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  Objects objects;
  KX_GameObject &npc = objects.Add(MT_Vector3(-5, 0, 0.3f));
  float top = -1e9f;
  for (int step = 0; step < 60 * 10; step++) {
    navigation->Submit(CrawlTo(npc, MT_Vector3(5, 0, 0), 2.0f, KX_NavRequest::Climb::Floors));
    navigation->Step(1.0f / 60.0f, nullptr);
    top = std::max(top, float(npc.NodeGetWorldPosition().z()));
  }
  EXPECT_LT(top, 1.0f);
  EXPECT_LT(npc.NodeGetWorldPosition().x(), 0.0f);
}

/** A closed room 4 m high: a crawler asked to the ceiling's middle climbs a wall and hangs there
 * upside down (its up down, the surface's normal down); asked so with walls only, it never gets
 * onto the ceiling. */
TEST(KX_Navigation, ACrawlerHangsFromTheCeiling)
{
  Level level;
  level.floors.push_back({MT_Vector3(-6, -4, 0), MT_Vector3(6, 4, 0)});
  level.boxes.push_back({MT_Vector3(-7, -5, 4), MT_Vector3(7, 5, 5)});
  level.boxes.push_back({MT_Vector3(-7, -5, -1), MT_Vector3(-6, 5, 4)});
  level.boxes.push_back({MT_Vector3(6, -5, -1), MT_Vector3(7, 5, 4)});
  level.boxes.push_back({MT_Vector3(-6, -5, -1), MT_Vector3(6, -4, 4)});
  level.boxes.push_back({MT_Vector3(-6, 4, -1), MT_Vector3(6, 5, 4)});
  for (const KX_NavRequest::Climb climb :
       {KX_NavRequest::Climb::Ceilings, KX_NavRequest::Climb::Walls})
  {
    std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
    Objects objects;
    KX_GameObject &spider = objects.Add(MT_Vector3(-3, 0, 0.3f));
    const MT_Vector3 goal(0, 0, 4);
    bool arrived = false;
    float on_ceiling = 0.0f;
    for (int step = 0; step < 60 * 30 && !arrived; step++) {
      navigation->Submit(CrawlTo(spider, goal, 2.0f, climb));
      navigation->Step(1.0f / 60.0f, nullptr);
      KX_NavAgentInfo info;
      ASSERT_TRUE(navigation->GetAgentInfo(&spider, info));
      arrived = info.arrived;
      if (info.surface_normal.z() < -0.9f) {
        on_ceiling += 1.0f / 60.0f;
      }
    }
    const MT_Vector3 p = spider.NodeGetWorldPosition();
    if (climb == KX_NavRequest::Climb::Ceilings) {
      EXPECT_TRUE(arrived);
      EXPECT_LT((p - MT_Vector3(0, 0, 3.7f)).length(), 0.5f);
      KX_NavAgentInfo info;
      navigation->GetAgentInfo(&spider, info);
      EXPECT_LT(info.surface_normal.z(), -0.95f);
      const MT_Vector3 up = spider.NodeGetWorldOrientation() * MT_Vector3(0, 0, 1);
      EXPECT_LT(up.z(), -0.95f);
    }
    else {
      EXPECT_FALSE(arrived);
      EXPECT_EQ(on_ceiling, 0.0f);
    }
  }
}

/** A Dynamic crawler (real Jolt, Move as Dynamic): over the wall by its velocity, its gravity
 * undone and drawn to the surface; arrived beyond it. */
TEST(KX_Navigation, ADynamicCrawlerClimbsOverAWall)
{
  Level level = WallAcross(2.0f);
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  Objects objects;
  World world;
  world.StaticBox(objects.Add(MT_Vector3(0, 0, -0.5f)), MT_Vector3(8, 5, 0.5f));
  world.StaticBox(objects.Add(MT_Vector3(0, 0, 1.0f)), MT_Vector3(0.2f, 5, 1.0f));
  KX_GameObject &beetle = objects.Add(MT_Vector3(-4, 0, 0.3f));
  world.Add(beetle, new JPH::SphereShape(0.25f), JPH::EMotionType::Dynamic);
  world.environment.GetPhysicsSystem()->OptimizeBroadPhase();
  navigation->SetPhysicsEnvironmentForTests(&world.environment);
  const MT_Vector3 goal(4, 0, 0);
  bool arrived = false;
  float top = -1e9f;
  int step = 0;
  for (; step < 60 * 30 && !arrived; step++) {
    KX_NavRequest request = CrawlTo(beetle, goal);
    request.move_dynamic = true;
    navigation->Submit(request);
    navigation->Step(1.0f / 60.0f, nullptr);
    world.Step({&beetle});
    top = std::max(top, float(beetle.NodeGetWorldPosition().z()));
    KX_NavAgentInfo info;
    navigation->GetAgentInfo(&beetle, info);
    arrived = info.arrived;
  }
  printf("a Dynamic crawler over a wall: %s in %.1f s at (%.2f %.2f %.2f), top %.2f\n",
         arrived ? "arrived" : "NOT arrived", step / 60.0f, beetle.NodeGetWorldPosition().x(),
         beetle.NodeGetWorldPosition().y(), beetle.NodeGetWorldPosition().z(), top);
  EXPECT_TRUE(arrived);
  EXPECT_GT(top, 2.0f);
  EXPECT_GT(beetle.NodeGetWorldPosition().x(), 3.0f);
}

/** A Dynamic crawler longer than tall (a box 0.7 x 0.6 x 0.3 m: its nose stops 0.35 m from its
 * middle against the wall, its body 0.15 m off the floor) climbs over the wall too: the wall
 * touches it at its side. 2026-11-19: it pressed on the wall's foot for good (the examples). */
TEST(KX_Navigation, ALongDynamicCrawlerClimbsOverAWall)
{
  Level level = WallAcross(2.0f);
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  Objects objects;
  World world;
  world.StaticBox(objects.Add(MT_Vector3(0, 0, -0.5f)), MT_Vector3(8, 5, 0.5f));
  world.StaticBox(objects.Add(MT_Vector3(0, 0, 1.0f)), MT_Vector3(0.2f, 5, 1.0f));
  KX_GameObject &beetle = objects.Add(MT_Vector3(-4, 0, 0.16f));
  /* (Jolt's space is Y up: x, the height, then the game's y.) */
  world.Add(beetle, new JPH::BoxShape(JPH::Vec3(0.35f, 0.15f, 0.3f)),
            JPH::EMotionType::Dynamic);
  world.environment.GetPhysicsSystem()->OptimizeBroadPhase();
  navigation->SetPhysicsEnvironmentForTests(&world.environment);
  navigation->SetAgentSizeForTests(&beetle, 0.35f, 0.15f);
  const MT_Vector3 goal(4, 0, 0);
  bool arrived = false;
  float top = -1e9f;
  int step = 0;
  for (; step < 60 * 30 && !arrived; step++) {
    KX_NavRequest request = CrawlTo(beetle, goal);
    request.move_dynamic = true;
    navigation->Submit(request);
    navigation->Step(1.0f / 60.0f, nullptr);
    world.Step({&beetle});
    top = std::max(top, float(beetle.NodeGetWorldPosition().z()));
    KX_NavAgentInfo info;
    navigation->GetAgentInfo(&beetle, info);
    arrived = info.arrived;
  }
  printf("a long Dynamic crawler over a wall: %s in %.1f s at (%.2f %.2f %.2f), top %.2f\n",
         arrived ? "arrived" : "NOT arrived", step / 60.0f, beetle.NodeGetWorldPosition().x(),
         beetle.NodeGetWorldPosition().y(), beetle.NodeGetWorldPosition().z(), top);
  EXPECT_TRUE(arrived);
  EXPECT_GT(top, 2.0f);
  EXPECT_GT(beetle.NodeGetWorldPosition().x(), 3.0f);
}

/** A Character with Climb Walls: its gravity turns toward the wall it climbs, it walks up and
 * over and on to its goal; no longer asked, its gravity is the scene's again. */
TEST(KX_Navigation, ACharacterClimbsOverAWall)
{
  Level level = WallAcross(2.0f);
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  Objects objects;
  World world;
  world.StaticBox(objects.Add(MT_Vector3(0, 0, -0.5f)), MT_Vector3(8, 5, 0.5f));
  world.StaticBox(objects.Add(MT_Vector3(0, 0, 1.0f)), MT_Vector3(0.2f, 5, 1.0f));
  KX_GameObject &climber = objects.Add(MT_Vector3(-4, 0, 0.95f));
  JoltCharacter *character = world.Character(climber);
  ASSERT_NE(character, nullptr);
  world.environment.GetPhysicsSystem()->OptimizeBroadPhase();
  navigation->SetPhysicsEnvironmentForTests(&world.environment);
  const MT_Vector3 before = character->GetGravity();
  const MT_Vector3 goal(4, 0, 0);
  bool arrived = false, turned = false;
  float top = -1e9f;
  int step = 0;
  for (; step < 60 * 40 && !arrived; step++) {
    navigation->Submit(CrawlTo(climber, goal));
    navigation->Step(1.0f / 60.0f, nullptr);
    world.Step({&climber});
    top = std::max(top, float(climber.NodeGetWorldPosition().z()));
    turned = turned || std::fabs(float(character->GetGravity().x())) > 9.0f;
    KX_NavAgentInfo info;
    navigation->GetAgentInfo(&climber, info);
    arrived = info.arrived;
  }
  printf("a Character over a wall: %s in %.1f s at (%.2f %.2f %.2f), top %.2f\n",
         arrived ? "arrived" : "NOT arrived", step / 60.0f, climber.NodeGetWorldPosition().x(),
         climber.NodeGetWorldPosition().y(), climber.NodeGetWorldPosition().z(), top);
  EXPECT_TRUE(arrived);
  EXPECT_TRUE(turned);
  EXPECT_GT(top, 2.0f);
  EXPECT_GT(climber.NodeGetWorldPosition().x(), 3.0f);
  /* No longer asked: the scene's gravity again. */
  navigation->Step(1.0f / 60.0f, nullptr);
  EXPECT_LT((character->GetGravity() - before).length(), 1e-4f);
}

/** A flat crawler (0.25 m wide, its body 0.1 m off the surface: an ant) over a barrier 2 m high
 * at its pace: along the floor to the barrier's foot, up, over, down the far face, on (about 20
 * m at 1.5 m/s: 14 s; heading straight for corners off the plane it crept down the far face,
 * the wall eating its move: over 30 s in the game). */
TEST(KX_Navigation, AFlatCrawlerKeepsItsPace)
{
  Level level;
  level.floors.push_back({MT_Vector3(28, -12, 0), MT_Vector3(52, 12, 0)});
  level.boxes.push_back({MT_Vector3(39.5f, -10, 0), MT_Vector3(40.5f, 10, 2)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  Objects objects;
  KX_GameObject &ant = objects.Add(MT_Vector3(32, 0, 0.1f));
  navigation->SetAgentSizeForTests(&ant, 0.25f, 0.1f);
  bool arrived = false;
  float top = 0.0f;
  int step = 0;
  for (; step < 60 * 40 && !arrived; step++) {
    navigation->Submit(CrawlTo(ant, MT_Vector3(48, 0, 0), 1.5f));
    navigation->Step(1.0f / 60.0f, nullptr);
    top = std::max(top, float(ant.NodeGetWorldPosition().z()));
    KX_NavAgentInfo info;
    navigation->GetAgentInfo(&ant, info);
    arrived = info.arrived;
  }
  printf("a flat crawler over a barrier: %s in %.1f s, top %.2f m\n",
         arrived ? "arrived" : "NOT arrived", step / 60.0f, top);
  EXPECT_TRUE(arrived);
  EXPECT_LT(step, 60 * 18);
  EXPECT_GT(top, 2.0f);
}

/** The game check's hunter: a Character over a ridge from wall to wall, on to a goal 6 m beyond
 * it (Look At, 1.5 m/s). Standing on the floor at the ridge's far foot its capsule touches the
 * ridge nearer than the floor under it: it stands on the floor its way runs along (stood on the
 * ridge, its gravity held it pressed into the corner for good). */
TEST(KX_Navigation, ACharacterClimbsOverARidge)
{
  Level level;
  level.floors.push_back({MT_Vector3(-10, -8, 0), MT_Vector3(10, 8, 0)});
  level.boxes.push_back({MT_Vector3(-1, -8, 0), MT_Vector3(1, 8, 2)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  Objects objects;
  World world;
  world.StaticBox(objects.Add(MT_Vector3(0, 0, -0.5f)), MT_Vector3(10, 8, 0.5f));
  world.StaticBox(objects.Add(MT_Vector3(0, 0, 1.0f)), MT_Vector3(1, 8, 1));
  KX_GameObject &hunter = objects.Add(MT_Vector3(-6, 3, 0.95f));
  JoltCharacter *character = world.Character(hunter);
  ASSERT_NE(character, nullptr);
  world.environment.GetPhysicsSystem()->OptimizeBroadPhase();
  navigation->SetPhysicsEnvironmentForTests(&world.environment);
  bool arrived = false;
  int step = 0;
  for (; step < 60 * 30 && !arrived; step++) {
    KX_NavRequest request = CrawlTo(hunter, MT_Vector3(6, 3, 0), 1.5f);
    request.arrive_distance = 0.6f;
    request.rot_speed = 8.0f;
    navigation->Submit(request);
    navigation->Step(1.0f / 60.0f, nullptr);
    world.Step({&hunter});
    KX_NavAgentInfo info;
    navigation->GetAgentInfo(&hunter, info);
    arrived = info.arrived;
  }
  printf("a Character over a ridge: %s in %.1f s at (%.2f %.2f %.2f), gravity (%.2f %.2f %.2f)\n",
         arrived ? "arrived" : "NOT arrived", step / 60.0f, hunter.NodeGetWorldPosition().x(),
         hunter.NodeGetWorldPosition().y(), hunter.NodeGetWorldPosition().z(),
         character->GetGravity().x(), character->GetGravity().y(), character->GetGravity().z());
  EXPECT_TRUE(arrived);
  EXPECT_GT(hunter.NodeGetWorldPosition().x(), 5.0f);
  EXPECT_LT(character->GetGravity().z(), -9.0f);
}

/** A Character with Climb Walls walking along a corridor's wall it touches (its goal ahead on
 * the floor): its way runs along the floor and the wall alike, and it stays on the floor it
 * stands on (by the nearer surface, its capsule's side, it stood on the wall). */
TEST(KX_Navigation, ACharacterWalksAlongAWallItTouches)
{
  Level level;
  level.floors.push_back({MT_Vector3(-10, -3, 0), MT_Vector3(10, 3, 0)});
  level.boxes.push_back({MT_Vector3(-10, 1, 0), MT_Vector3(10, 1.4f, 3)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  Objects objects;
  World world;
  world.StaticBox(objects.Add(MT_Vector3(0, 0, -0.5f)), MT_Vector3(10, 3, 0.5f));
  world.StaticBox(objects.Add(MT_Vector3(0, 1.2f, 1.5f)), MT_Vector3(10, 0.2f, 1.5f));
  KX_GameObject &walker = objects.Add(MT_Vector3(-6, 0.68f, 0.95f));
  JoltCharacter *character = world.Character(walker);
  ASSERT_NE(character, nullptr);
  world.environment.GetPhysicsSystem()->OptimizeBroadPhase();
  navigation->SetPhysicsEnvironmentForTests(&world.environment);
  bool arrived = false, sideways = false;
  float highest = 0.0f;
  int step = 0;
  for (; step < 60 * 15 && !arrived; step++) {
    navigation->Submit(CrawlTo(walker, MT_Vector3(6, 0.68f, 0), 1.5f));
    navigation->Step(1.0f / 60.0f, nullptr);
    world.Step({&walker});
    sideways = sideways || character->GetGravity().z() > -9.0f;
    highest = std::max(highest, float(walker.NodeGetWorldPosition().z()));
    KX_NavAgentInfo info;
    navigation->GetAgentInfo(&walker, info);
    arrived = info.arrived;
  }
  printf("a Character along a wall it touches: %s in %.1f s, highest %.2f m\n",
         arrived ? "arrived" : "NOT arrived", step / 60.0f, highest);
  EXPECT_TRUE(arrived);
  EXPECT_FALSE(sideways);
  EXPECT_LT(highest, 1.2f);
}

/** A Character told to stop while it climbs a wall gets the scene's gravity back at once and
 * falls to the floor. */
TEST(KX_Navigation, ACharacterLetGoOnAWallFalls)
{
  Level level = WallAcross(4.0f);
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  Objects objects;
  World world;
  world.StaticBox(objects.Add(MT_Vector3(0, 0, -0.5f)), MT_Vector3(8, 5, 0.5f));
  world.StaticBox(objects.Add(MT_Vector3(0, 0, 2.0f)), MT_Vector3(0.2f, 5, 2.0f));
  KX_GameObject &climber = objects.Add(MT_Vector3(-4, 0, 0.95f));
  JoltCharacter *character = world.Character(climber);
  ASSERT_NE(character, nullptr);
  world.environment.GetPhysicsSystem()->OptimizeBroadPhase();
  navigation->SetPhysicsEnvironmentForTests(&world.environment);
  const MT_Vector3 before = character->GetGravity();
  bool on_wall = false;
  for (int step = 0; step < 60 * 20 && !on_wall; step++) {
    navigation->Submit(CrawlTo(climber, MT_Vector3(4, 0, 0)));
    navigation->Step(1.0f / 60.0f, nullptr);
    world.Step({&climber});
    on_wall = std::fabs(float(character->GetGravity().x())) > 9.0f &&
              climber.NodeGetWorldPosition().z() > 1.5f;
  }
  ASSERT_TRUE(on_wall);
  navigation->Step(1.0f / 60.0f, nullptr);
  EXPECT_LT((character->GetGravity() - before).length(), 1e-4f);
  for (int step = 0; step < 60 * 3; step++) {
    navigation->Step(1.0f / 60.0f, nullptr);
    world.Step({&climber});
  }
  EXPECT_LT(climber.NodeGetWorldPosition().z(), 1.2f);
}

/** A Dynamic crawler hanging from a ceiling is drawn back to it when moved off it (its gravity
 * undone, nothing else would bring it back). */
TEST(KX_Navigation, ADynamicCrawlerHoldsOnToTheCeiling)
{
  Level level;
  level.floors.push_back({MT_Vector3(-6, -4, 0), MT_Vector3(6, 4, 0)});
  level.boxes.push_back({MT_Vector3(-7, -5, 4), MT_Vector3(7, 5, 5)});
  level.boxes.push_back({MT_Vector3(-7, -5, -1), MT_Vector3(-6, 5, 4)});
  level.boxes.push_back({MT_Vector3(6, -5, -1), MT_Vector3(7, 5, 4)});
  level.boxes.push_back({MT_Vector3(-6, -5, -1), MT_Vector3(6, -4, 4)});
  level.boxes.push_back({MT_Vector3(-6, 4, -1), MT_Vector3(6, 5, 4)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  Objects objects;
  World world;
  world.StaticBox(objects.Add(MT_Vector3(0, 0, -0.5f)), MT_Vector3(7, 5, 0.5f));
  world.StaticBox(objects.Add(MT_Vector3(0, 0, 4.5f)), MT_Vector3(7, 5, 0.5f));
  world.StaticBox(objects.Add(MT_Vector3(-6.5f, 0, 2)), MT_Vector3(0.5f, 5, 2));
  world.StaticBox(objects.Add(MT_Vector3(6.5f, 0, 2)), MT_Vector3(0.5f, 5, 2));
  world.StaticBox(objects.Add(MT_Vector3(0, -4.5f, 2)), MT_Vector3(6, 0.5f, 2));
  world.StaticBox(objects.Add(MT_Vector3(0, 4.5f, 2)), MT_Vector3(6, 0.5f, 2));
  KX_GameObject &beetle = objects.Add(MT_Vector3(-3, 0, 0.25f));
  world.Add(beetle, new JPH::SphereShape(0.25f), JPH::EMotionType::Dynamic);
  world.environment.GetPhysicsSystem()->OptimizeBroadPhase();
  navigation->SetPhysicsEnvironmentForTests(&world.environment);
  auto run = [&](const int steps) {
    for (int step = 0; step < steps; step++) {
      KX_NavRequest request = CrawlTo(beetle, MT_Vector3(0, 0, 4), 1.5f,
                                      KX_NavRequest::Climb::Ceilings);
      request.move_dynamic = true;
      navigation->Submit(request);
      navigation->Step(1.0f / 60.0f, nullptr);
      world.Step({&beetle});
    }
  };
  run(60 * 25);
  ASSERT_GT(beetle.NodeGetWorldPosition().z(), 3.6f);
  /* Moved half a metre off the ceiling. */
  beetle.NodeSetWorldPosition(beetle.NodeGetWorldPosition() - MT_Vector3(0, 0, 0.5f));
  beetle.NodeUpdateGS(0.0);
  beetle.GetPhysicsController()->SetTransform();
  run(60 * 2);
  EXPECT_GT(beetle.NodeGetWorldPosition().z(), 3.65f);
}

/** Up a wall to a point 3 m up it and 5 m along it: straight up the wall's face at a slant, not
 * along its foot first. */
TEST(KX_Navigation, ACrawlerClimbsAWallAtASlant)
{
  Level level = WallAcross(4.0f);
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  Objects objects;
  KX_GameObject &gecko = objects.Add(MT_Vector3(-1.5f, -2.5f, 0.3f));
  const MT_Vector3 goal(-0.2f, 2.5f, 3.0f);
  bool arrived = false;
  int step = 0;
  for (; step < 60 * 20 && !arrived; step++) {
    navigation->Submit(CrawlTo(gecko, goal));
    navigation->Step(1.0f / 60.0f, nullptr);
    KX_NavAgentInfo info;
    navigation->GetAgentInfo(&gecko, info);
    arrived = info.arrived;
  }
  printf("a crawler up a wall at a slant: %s in %.1f s\n", arrived ? "arrived" : "NOT arrived",
         step / 60.0f);
  EXPECT_TRUE(arrived);
  EXPECT_LT((gecko.NodeGetWorldPosition() - MT_Vector3(-0.5f, 2.5f, 3.0f)).length(), 0.5f);
}

/** Two groups of 8 crawlers crossing over the same wall the other way: they pass on its faces
 * and top, never deeply inside each other, all arrive; the same on 0 and 4 helpers, bit for
 * bit. */
void RunCrawlers(const uint32_t helpers, std::vector<MT_Vector3> &r_positions, int &r_arrived,
                 float &r_deepest, const int jitter = 0)
{
  Level level = WallAcross(3.0f);
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  LN_JobPool pool;
  pool.SetHelperCount(helpers);
  Objects objects;
  std::vector<KX_GameObject *> crawlers;
  std::vector<MT_Vector3> goals;
  for (int i = 0; i < 16; i++) {
    const float side = i < 8 ? -1.0f : 1.0f;
    const float y = -3.5f + float(i % 8) * 1.0f;
    /* (Another start: each moved up to 0.15 m, from the jitter's number.) */
    const float dx = jitter == 0 ? 0.0f : float(((i * 7 + jitter * 13) % 11) - 5) * 0.03f;
    const float dy = jitter == 0 ? 0.0f : float(((i * 5 + jitter * 3) % 11) - 5) * 0.03f;
    crawlers.push_back(&objects.Add(MT_Vector3(side * 5.0f + dx, y + dy, 0.3f)));
    goals.push_back(MT_Vector3(-side * 5.0f, -y, 0.0f));
  }
  r_deepest = 0.0f;
  r_arrived = 0;
  for (int step = 0; step < 60 * 40 && r_arrived < int(crawlers.size()); step++) {
    for (size_t i = 0; i < crawlers.size(); i++) {
      navigation->Submit(CrawlTo(*crawlers[i], goals[i], 1.5f));
    }
    navigation->Step(1.0f / 60.0f, helpers > 0 ? &pool : nullptr);
    r_arrived = 0;
    for (size_t i = 0; i < crawlers.size(); i++) {
      KX_NavAgentInfo info;
      navigation->GetAgentInfo(crawlers[i], info);
      r_arrived += info.arrived ? 1 : 0;
      for (size_t j = i + 1; j < crawlers.size(); j++) {
        r_deepest = std::max(r_deepest,
                             0.6f - float((crawlers[i]->NodeGetWorldPosition() -
                                           crawlers[j]->NodeGetWorldPosition())
                                              .length()));
      }
    }
  }
  r_positions.clear();
  for (KX_GameObject *crawler : crawlers) {
    r_positions.push_back(crawler->NodeGetWorldPosition());
  }
}

TEST(KX_Navigation, CrawlersAvoidEachOtherOnAWall)
{
  std::vector<MT_Vector3> alone, many;
  int arrived_alone = 0, arrived_many = 0;
  float deepest_alone = 0.0f, deepest_many = 0.0f;
  RunCrawlers(0, alone, arrived_alone, deepest_alone);
  RunCrawlers(4, many, arrived_many, deepest_many);
  printf("crawlers crossing over a wall: %d of 16 arrived, deepest overlap %.3f m\n",
         arrived_alone, deepest_alone);
  EXPECT_EQ(arrived_alone, 16);
  EXPECT_LT(deepest_alone, 0.1f);
  ASSERT_EQ(alone.size(), many.size());
  for (size_t i = 0; i < alone.size(); i++) {
    EXPECT_EQ(alone[i], many[i]) << i;
  }
  /* (Crowds are chaotic: two other starts must pass too.) */
  for (const int jitter : {1, 2}) {
    std::vector<MT_Vector3> positions;
    int arrived = 0;
    float deepest = 0.0f;
    RunCrawlers(0, positions, arrived, deepest, jitter);
    printf("  from start %d: %d of 16 arrived, deepest overlap %.3f m\n", jitter, arrived,
           deepest);
    EXPECT_EQ(arrived, 16) << jitter;
    EXPECT_LT(deepest, 0.1f) << jitter;
  }
}

/** A box set down on a crawler's way while playing (a level piece added): its shell is made
 * there, the crawler searches again and climbs over it, never through it. */
TEST(KX_Navigation, ACrawlerClimbsOverABoxThatAppears)
{
  Level level;
  level.floors.push_back({MT_Vector3(-8, -5, 0), MT_Vector3(8, 5, 0)});
  level.boxes.push_back({MT_Vector3(-0.6f, -5, 0), MT_Vector3(0.6f, 5, 1.2f)});
  level.boxes.back().enabled = false;
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  Objects objects;
  KX_GameObject &spider = objects.Add(MT_Vector3(-5, 0, 0.3f));
  bool arrived = false, through = false;
  float top = -1e9f;
  for (int step = 0; step < 60 * 30 && !arrived; step++) {
    if (step == 30) {
      level.boxes.back().enabled = true;
      navigation->NoteStaticChanged(level.boxes.back().min, level.boxes.back().max);
    }
    navigation->Submit(CrawlTo(spider, MT_Vector3(5, 0, 0), 1.5f));
    navigation->Step(1.0f / 60.0f, nullptr);
    const MT_Vector3 p = spider.NodeGetWorldPosition();
    if (step > 30) {
      through = through || (std::fabs(float(p.x())) < 0.6f && p.z() < 1.2f);
      top = std::max(top, float(p.z()));
    }
    KX_NavAgentInfo info;
    navigation->GetAgentInfo(&spider, info);
    arrived = info.arrived;
  }
  EXPECT_TRUE(arrived);
  EXPECT_FALSE(through);
  EXPECT_GT(top, 1.3f);
}

/** Navigate with Climb Walls in a tree: the NPC climbs over the wall; Get Navigation Info's
 * Surface Normal tells the wall's normal while it climbs, straight up on the floors. */
TEST(KX_NavigationNodes, NavigateClimbsAndTellsItsSurface)
{
  NavTree tree;
  diff::GenNode &navigate = tree.Add(
      "LogicNativeNavigate", 0, LN_NAVIGATE_CLIMB_WALLS << LN_NAVIGATE_CLIMB_SHIFT);
  tree.b.Vector(navigate, "Destination", 5, 0, 0);
  tree.b.Float(navigate, "Lin Speed", 2.0f);
  tree.b.Float(navigate, "Reach Threshold", 0.3f);
  tree.Chain(navigate);
  diff::GenNode &info = tree.Add("LogicNativeGetNavigationInfo");
  tree.StoreBool(info, "Arrived", "arrived");
  tree.StoreVector(info, "Surface Normal", "normal");
  const std::shared_ptr<LN_Program> program = tree.Compile();
  ASSERT_NE(program, nullptr);
  Level level = WallAcross();
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  NavigationSeam seam(navigation.get());
  SemRig rig(program);
  KX_GameObject &npc = rig.Object(0);
  Place(npc, MT_Vector3(-5, 0, 0.3f));
  bool on_wall = false, arrived = false;
  for (int step = 0; step < 60 * 30 && !arrived; step++) {
    rig.Step();
    navigation->Step(1.0f / 60.0f, nullptr);
    const MT_Vector3 normal = ReadVector(rig, npc, "normal");
    on_wall = on_wall || (std::fabs(float(normal.x())) > 0.95f && npc.NodeGetWorldPosition().z() > 1.0f);
    arrived = rig.Float(npc, "arrived") == 1.0f;
  }
  EXPECT_TRUE(on_wall);
  EXPECT_TRUE(arrived);
  rig.Step();
  const MT_Vector3 normal = ReadVector(rig, npc, "normal");
  EXPECT_GT(normal.z(), 0.95f);
}

/** Cost (run by hand): the shell of the 500 m city (FlyerCrowdCost's) for crawlers of 0.3 m
 * (voxels 0.2 m): its build and memory; then 1,000 crawlers crossing a field 100 m square with
 * 80 rocks 1 to 5 m high (over them or round them), the step's time on 0 and 15 helpers. */
TEST(KX_Navigation, DISABLED_CrawlerCrowdCost)
{
  {
    Level city;
    city.floors.push_back({MT_Vector3(-250, -250, 0), MT_Vector3(250, 250, 0)});
    for (int i = 0; i < 400; i++) {
      const float x = -240.0f + float(i % 20) * 24.0f, y = -240.0f + float(i / 20) * 24.0f;
      const float w = 4.0f + float((i * 37) % 17), h = 5.0f + float((i * 53) % 36);
      city.boxes.push_back({MT_Vector3(x, y, 0), MT_Vector3(x + w, y + w * 0.8f, h)});
    }
    for (const uint32_t helpers : {0u, 15u}) {
      LN_JobPool pool;
      pool.SetHelperCount(helpers);
      KX_NavAirMap::Settings settings;
      settings.radius = 0.3f;
      settings.voxel = 0.2f;
      settings.headroom = 30.0f;
      KX_NavAirMap map(settings);
      KX_NavGeometry geometry;
      city.Collect(MT_Vector3(), MT_Vector3(), geometry);
      const auto t0 = std::chrono::steady_clock::now();
      map.Build(geometry, {}, helpers > 0 ? &pool : nullptr);
      const auto t1 = std::chrono::steady_clock::now();
      map.EnableSurfaces(geometry, 0.7f, helpers > 0 ? &pool : nullptr);
      const auto t2 = std::chrono::steady_clock::now();
      printf("city 500 m, crawlers of 0.3 m, voxels 0.2 m, %u helpers: air map %.1f ms, shell %.1f "
             "ms, %zu shell bricks (%.1f MB)\n",
             helpers, std::chrono::duration<double, std::milli>(t1 - t0).count(),
             std::chrono::duration<double, std::milli>(t2 - t1).count(), map.SurfaceBrickCount(),
             double(map.SurfaceBrickCount()) * (3 * 64 + 8 + 16) / 1e6);
    }
  }
  Level level;
  level.floors.push_back({MT_Vector3(-50, -50, 0), MT_Vector3(50, 50, 0)});
  for (int i = 0; i < 80; i++) {
    const float x = -44.0f + float((i * 37) % 88), y = -44.0f + float((i * 61) % 88);
    const float w = 1.0f + float((i * 13) % 4), h = 1.0f + float((i * 7) % 5);
    level.boxes.push_back({MT_Vector3(x, y, 0), MT_Vector3(x + w, y + w, h)});
  }
  for (const uint32_t helpers : {0u, 15u}) {
    std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings(0));
    LN_JobPool pool;
    pool.SetHelperCount(helpers);
    Objects objects;
    std::vector<KX_GameObject *> crawlers;
    std::vector<MT_Vector3> goals;
    for (int i = 0; i < 1000; i++) {
      const float a = float(i) * 0.0062832f;
      crawlers.push_back(&objects.Add(MT_Vector3(std::cos(a) * 46.0f, std::sin(a) * 46.0f, 0.3f)));
      goals.push_back(MT_Vector3(-std::cos(a) * 46.0f, -std::sin(a) * 46.0f, 0.0f));
    }
    double total = 0.0;
    int measured = 0, arrived = 0, whole = 0;
    double progress = 0.0;
    for (int step = 0; step < 600; step++) {
      for (size_t i = 0; i < crawlers.size(); i++) {
        navigation->Submit(CrawlTo(*crawlers[i], goals[i], 2.0f));
      }
      const auto a = std::chrono::steady_clock::now();
      navigation->Step(1.0f / 60.0f, helpers > 0 ? &pool : nullptr);
      const auto b = std::chrono::steady_clock::now();
      if (step >= 60) {
        total += std::chrono::duration<double, std::milli>(b - a).count();
        measured++;
      }
    }
    for (size_t i = 0; i < crawlers.size(); i++) {
      KX_NavAgentInfo info;
      navigation->GetAgentInfo(crawlers[i], info);
      arrived += info.arrived ? 1 : 0;
      whole += info.path_complete ? 1 : 0;
      const float a = float(i) * 0.0062832f;
      progress += 92.0 - (crawlers[i]->NodeGetWorldPosition() - goals[i]).length();
      (void)a;
    }
    printf("1,000 crawlers crossing a field 100 m square, %u helpers: %.2f ms a step (steps "
           "60-600), %d on whole ways, %.1f m nearer their goals on average\n",
           helpers, total / measured, whole, progress / double(crawlers.size()));
  }
}

/** \} */

/** Cost (run by hand): a city 500 m square (a floor, 400 blocks 4 to 20 m wide, 5 to 40 m high):
 * the air map's build and memory for flyers of 0.5 m; then 1,000 flyers crossing it, each to the
 * far side, the step's time on 0 and 15 helpers. */
TEST(KX_Navigation, DISABLED_FlyerCrowdCost)
{
  Level level;
  level.floors.push_back({MT_Vector3(-250, -250, 0), MT_Vector3(250, 250, 0)});
  for (int i = 0; i < 400; i++) {
    const float x = -240.0f + float(i % 20) * 24.0f, y = -240.0f + float(i / 20) * 24.0f;
    const float w = 4.0f + float((i * 37) % 17), h = 5.0f + float((i * 53) % 36);
    level.boxes.push_back({MT_Vector3(x, y, 0), MT_Vector3(x + w, y + w * 0.8f, h)});
  }
  for (const uint32_t helpers : {0u, 15u}) {
    std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings(0));
    LN_JobPool pool;
    pool.SetHelperCount(helpers);
    Objects objects;
    std::vector<KX_GameObject *> flyers;
    std::vector<MT_Vector3> goals;
    for (int i = 0; i < 1000; i++) {
      const float a = float(i) * 0.0062832f;
      const MT_Vector3 start(std::cos(a) * 230.0f, std::sin(a) * 230.0f, 10.0f + float(i % 30));
      flyers.push_back(&objects.Add(start));
      goals.push_back(MT_Vector3(-start.x(), -start.y(), 10.0f + float((i * 7) % 30)));
    }
    /* (A flyer of 0.5 m: its map is made at its first step.) */
    const auto t0 = std::chrono::steady_clock::now();
    KX_NavAirMap *map = nullptr;
    {
      KX_NavAirMap::Settings settings;
      settings.radius = 0.5f;
      settings.voxel = 0.25f;
      settings.headroom = 30.0f;
      KX_NavAirMap probe(settings);
      KX_NavGeometry geometry;
      level.Collect(MT_Vector3(), MT_Vector3(), geometry);
      probe.Build(geometry, {}, helpers > 0 ? &pool : nullptr);
      const auto t1 = std::chrono::steady_clock::now();
      printf("air map of a city 500 m square, flyers of 0.5 m, voxels 0.25 m, %u helpers: built "
             "in %.1f ms, %zu bricks, %.1f MB\n",
             helpers, std::chrono::duration<double, std::milli>(t1 - t0).count(),
             probe.BrickCount(), double(probe.MemoryBytes()) / 1e6);
    }
    (void)map;
    double total = 0.0;
    int measured = 0;
    int arrived = 0;
    for (int step = 0; step < 600; step++) {
      for (size_t i = 0; i < flyers.size(); i++) {
        KX_NavRequest request = FlyTo(*flyers[i], goals[i], 8.0f);
        navigation->Submit(request);
      }
      const auto a = std::chrono::steady_clock::now();
      navigation->Step(1.0f / 60.0f, helpers > 0 ? &pool : nullptr);
      const auto b = std::chrono::steady_clock::now();
      if (step >= 60) {
        total += std::chrono::duration<double, std::milli>(b - a).count();
        measured++;
      }
    }
    /* (How far they got toward their goals in the 10 s: at 8 m/s at most 80 m; and how many
     * fly a whole way.) */
    double progress = 0.0;
    int whole = 0;
    for (size_t i = 0; i < flyers.size(); i++) {
      KX_NavAgentInfo info;
      navigation->GetAgentInfo(flyers[i], info);
      arrived += info.arrived ? 1 : 0;
      whole += info.path_complete ? 1 : 0;
      const float a = float(i) * 0.0062832f;
      const MT_Vector3 start(std::cos(a) * 230.0f, std::sin(a) * 230.0f, 10.0f + float(i % 30));
      progress += (start - goals[i]).length() -
                  (flyers[i]->NodeGetWorldPosition() - goals[i]).length();
    }
    printf("1,000 flyers crossing the city, %u helpers: %.2f ms a step (steps 60-600), %d "
           "arrived in 10 s, %d on whole ways, %.1f m nearer their goals on average\n",
           helpers, total / measured, arrived, whole, progress / double(flyers.size()));
  }
}

/** \} */

/** The door test's crowd on 0, 3 and 15 helper threads (Dense): bit for bit. */
TEST(KX_Navigation, ADenseCrowdIsTheSameOnAnyNumberOfThreads)
{
  std::vector<MT_Vector3> alone, few, many;
  const DoorRun a = RunDoor(120, true, 0, 20.0f, 1.6f, &alone);
  const DoorRun b = RunDoor(120, true, 3, 20.0f, 1.6f, &few);
  const DoorRun c = RunDoor(120, true, 15, 20.0f, 1.6f, &many);
  EXPECT_EQ(a.through, b.through);
  EXPECT_EQ(a.through, c.through);
  ASSERT_EQ(alone.size(), many.size());
  for (size_t i = 0; i < alone.size(); i++) {
    EXPECT_EQ(alone[i], few[i]) << i;
    EXPECT_EQ(alone[i], many[i]) << i;
  }
}

/** Density-aware routes: a crowd heading for the nearer of two doors spreads to the other as it
 * gathers there (crowded polygons cost more; a way ahead through one is searched again). */
TEST(KX_Navigation, ACrowdSpreadsOverTwoDoors)
{
  Level level;
  level.floors.push_back({MT_Vector3(-14, -10, 0), MT_Vector3(30, 10, 0)});
  /* Doors 1.6 m wide round y 3 (the nearer) and y -3. */
  level.boxes.push_back({MT_Vector3(-0.5f, -10, 0), MT_Vector3(0.5f, -3.8f, 2)});
  level.boxes.push_back({MT_Vector3(-0.5f, -2.2f, 0), MT_Vector3(0.5f, 2.2f, 2)});
  level.boxes.push_back({MT_Vector3(-0.5f, 3.8f, 0), MT_Vector3(0.5f, 10, 2)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  Objects objects;
  std::vector<KX_GameObject *> npcs;
  std::vector<MT_Vector3> goals;
  for (int i = 0; i < 120; i++) {
    npcs.push_back(&objects.Add(MT_Vector3(-1.5f - float(i / 12) * 0.9f, 0.5f + float(i % 12) * 0.75f, 0)));
    goals.push_back(MT_Vector3(6.0f + float(i / 12) * 1.2f, -2.0f + float(i % 12) * 0.9f, 0));
  }
  std::vector<int> door(npcs.size(), 0);
  for (int step = 0; step < 60 * 60; step++) {
    for (size_t i = 0; i < npcs.size(); i++) {
      KX_NavRequest request = GoTo(*npcs[i], goals[i], 1.6f);
      request.arrive_distance = 0.4f;
      navigation->Submit(request);
    }
    navigation->Step(1.0f / 60.0f, nullptr);
    for (size_t i = 0; i < npcs.size(); i++) {
      const MT_Vector3 p = npcs[i]->NodeGetWorldPosition();
      if (door[i] == 0 && std::fabs(p.x()) < 0.5f) {
        door[i] = p.y() > 0.0f ? 1 : -1;
      }
    }
  }
  const int far = int(std::count(door.begin(), door.end(), -1));
  const int near = int(std::count(door.begin(), door.end(), 1));
  printf("two doors: %d by the nearer, %d by the farther\n", near, far);
  EXPECT_EQ(near + far, int(npcs.size()));
  EXPECT_GE(far, int(npcs.size()) / 8);
}

/** A crowd that gathers on an NPC's way after it searched (a door it walks to gets packed, the
 * crowd standing in a big open room before it): it searches again and takes the other door,
 * never pressing into the crowd. Both with Detour's corridor shortcuts (they search a little
 * with the crowd's costs too) and without them (an NPC with its own area costs: only the
 * crowd-ahead rule searches again). */
TEST(KX_Navigation, ACrowdGatheringAheadMakesAnNpcSearchAgain)
{
  for (const bool own_costs : {false, true}) {
    Level level;
    level.floors.push_back({MT_Vector3(-14, -10, 0), MT_Vector3(30, 10, 0)});
    /* Doors 1.6 m wide round y 3 (on the walker's line) and y -3. */
    level.boxes.push_back({MT_Vector3(-0.5f, -10, 0), MT_Vector3(0.5f, -3.8f, 2)});
    level.boxes.push_back({MT_Vector3(-0.5f, -2.2f, 0), MT_Vector3(0.5f, 2.2f, 2)});
    level.boxes.push_back({MT_Vector3(-0.5f, 3.8f, 0), MT_Vector3(0.5f, 10, 2)});
    std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
    Objects objects;
    KX_GameObject &walker = objects.Add(MT_Vector3(-13.5f, 3, 0));
    if (own_costs) {
      /* (An area the level does not have: only its costs are no longer the default.) */
      navigation->SetAreaCost(&walker, "mud", 3.0f, false);
    }
    /* The crowd starts in the room's far corner, then stands packed (0.7 m apart) before the
     * first door, one in it, four past it. */
    std::vector<MT_Vector3> places;
    for (int i = 0; i < 30; i++) {
      places.push_back(MT_Vector3(-1.2f - float(i % 6) * 0.7f, 1.6f + float(i / 6) * 0.7f, 0));
    }
    places.push_back(MT_Vector3(0, 3, 0));
    for (const float x : {1.0f, 1.7f}) {
      places.push_back(MT_Vector3(x, 2.6f, 0));
      places.push_back(MT_Vector3(x, 3.4f, 0));
    }
    std::vector<KX_GameObject *> crowd;
    for (int i = 0; i < int(places.size()); i++) {
      crowd.push_back(
          &objects.Add(MT_Vector3(-13.0f + float(i % 7) * 0.8f, -9.0f + float(i / 7) * 0.8f, 0)));
    }
    float closest = 1e9f;
    int door_taken = 0;
    bool arrived = false;
    for (int step = 0; step < 60 * 120 && !arrived; step++) {
      for (size_t i = 0; i < crowd.size(); i++) {
        KX_NavRequest request = GoTo(*crowd[i], places[i], 1.6f);
        request.arrive_distance = 0.2f;
        navigation->Submit(request);
      }
      navigation->Submit(GoTo(walker, MT_Vector3(12, 3, 0), 0.35f));
      navigation->Step(1.0f / 60.0f, nullptr);
      const MT_Vector3 p = walker.NodeGetWorldPosition();
      for (KX_GameObject *other : crowd) {
        closest = std::min(closest, distance_2d(p, other->NodeGetWorldPosition()));
      }
      if (door_taken == 0 && std::fabs(p.x()) < 0.5f) {
        door_taken = p.y() > 0.0f ? 1 : -1;
      }
      KX_NavAgentInfo info;
      navigation->GetAgentInfo(&walker, info);
      arrived = info.arrived;
    }
    const char *name = own_costs ? "own area costs" : "default costs";
    printf("a crowd gathering ahead (%s): door %d, closest to one of the crowd %.2f m, %s\n",
           name, door_taken, closest, arrived ? "arrived" : "not arrived");
    EXPECT_EQ(door_taken, -1) << name;
    EXPECT_TRUE(arrived) << name;
    EXPECT_GT(closest, 1.0f) << name;
  }
}

/** Crowd costs: a polygon costs what its most crowded agent's own crowd says (the agents within
 * 1 m of it, standing ones twice, by Weidmann's speed), nothing below three; other polygons
 * (far off, a deck above) nothing. */
TEST(KX_Navigation, TheCrowdCostIsTheLocalCrowd)
{
  Level level;
  level.floors.push_back({MT_Vector3(-10, -10, 0), MT_Vector3(10, 10, 0)});
  level.floors.push_back({MT_Vector3(-3, -3, 4), MT_Vector3(3, 3, 4)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  Objects objects;
  /* Three agents standing 0.65 m or more apart (nothing pushes them). */
  const MT_Vector3 group[] = {
      MT_Vector3(-0.3f, -0.35f, 0), MT_Vector3(-0.3f, 0.35f, 0), MT_Vector3(-0.85f, 0, 0)};
  std::vector<KX_GameObject *> standing;
  for (const MT_Vector3 &p : group) {
    standing.push_back(&objects.Add(p));
  }
  /* Two walking ones far off: fewer than kCrowdedAgents. */
  KX_GameObject &a = objects.Add(MT_Vector3(6, 6, 0));
  KX_GameObject &b = objects.Add(MT_Vector3(6.4f, 6, 0));
  for (int step = 0; step < 30; step++) {
    for (size_t i = 0; i < standing.size(); i++) {
      navigation->Submit(GoTo(*standing[i], group[i], 1.0f));
    }
    navigation->Submit(GoTo(a, MT_Vector3(6, -6, 0), 1.0f));
    navigation->Submit(GoTo(b, MT_Vector3(6.4f, -6, 0), 1.0f));
    navigation->Step(1.0f / 60.0f, nullptr);
  }
  /* Each of the group: itself and the two others, all standing: 6 within 1 m, 1.9 a square
   * meter, Weidmann's speed. */
  const float expected =
      1.0f / (1.0f - std::exp(-1.913f * (float(M_PI) / 6.0f - 1.0f / 5.4f))) - 1.0f;
  for (const MT_Vector3 &p : group) {
    EXPECT_NEAR(navigation->CrowdExtraCostForTests(p), expected, 1e-4f);
  }
  /* Far off: none. The deck above them: none. The two walkers: fewer than three, none. */
  EXPECT_EQ(navigation->CrowdExtraCostForTests(MT_Vector3(8, -8, 0)), 0.0f);
  EXPECT_EQ(navigation->CrowdExtraCostForTests(MT_Vector3(-0.45f, 0, 4)), 0.0f);
  EXPECT_EQ(navigation->CrowdExtraCostForTests(a.NodeGetWorldPosition()), 0.0f);
}

/** A crowd on another floor is no crowd: an NPC on a deck 4 m above a packed square walks
 * straight over it (the way round the deck is 12 m longer), while one on the square walks round
 * the crowd. */
TEST(KX_Navigation, ACrowdOnAnotherFloorCostsNothing)
{
  Level level;
  level.floors.push_back({MT_Vector3(-10, -10, 0), MT_Vector3(10, 10, 0)});
  /* The deck: a straight strip over the square's middle (y -1..1), a far strip (y 5..7), joined
   * at both ends. */
  level.floors.push_back({MT_Vector3(-8, -1, 4), MT_Vector3(8, 1, 4)});
  level.floors.push_back({MT_Vector3(-8, 5, 4), MT_Vector3(8, 7, 4)});
  level.floors.push_back({MT_Vector3(-10, -1, 4), MT_Vector3(-8, 7, 4)});
  level.floors.push_back({MT_Vector3(8, -1, 4), MT_Vector3(10, 7, 4)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  Objects objects;
  std::vector<KX_GameObject *> crowd;
  std::vector<MT_Vector3> places;
  for (int i = 0; i < 36; i++) {
    places.push_back(MT_Vector3(-1.75f + float(i % 6) * 0.7f, -1.75f + float(i / 6) * 0.7f, 0));
    crowd.push_back(&objects.Add(places.back()));
  }
  KX_GameObject &above = objects.Add(MT_Vector3(-9, 0, 4));
  KX_GameObject &below = objects.Add(MT_Vector3(-9, 0, 0));
  float above_far = 0.0f, below_far = 0.0f;
  for (int step = 0; step < 60 * 30; step++) {
    for (size_t i = 0; i < crowd.size(); i++) {
      KX_NavRequest request = GoTo(*crowd[i], places[i], 1.0f);
      request.arrive_distance = 0.2f;
      navigation->Submit(request);
    }
    /* They set off once the crowd stands. */
    if (step >= 120) {
      navigation->Submit(GoTo(above, MT_Vector3(9, 0, 4), 1.5f));
      navigation->Submit(GoTo(below, MT_Vector3(9, 0, 0), 1.5f));
    }
    navigation->Step(1.0f / 60.0f, nullptr);
    above_far = std::max(above_far, float(std::fabs(above.NodeGetWorldPosition().y())));
    below_far = std::max(below_far, float(std::fabs(below.NodeGetWorldPosition().y())));
  }
  printf("a crowd on another floor: above strays %.2f m, below %.2f m\n", above_far, below_far);
  EXPECT_LT(above_far, 0.6f);
  EXPECT_NEAR(above.NodeGetWorldPosition().x(), 9.0f, 0.4f);
  EXPECT_GT(above.NodeGetWorldPosition().z(), 3.5f);
  EXPECT_GT(below_far, 2.0f);
  EXPECT_NEAR(below.NodeGetWorldPosition().x(), 9.0f, 0.4f);
}

/** Stalled is getting nowhere: an NPC held in place and shaken 3 mm back and forth each step
 * (ORCA's flutter at a wall corner looked so: its speed of one step said walking, and an NPC
 * stood there for good) has stalled; one walking freely has not. */
TEST(KX_Navigation, AnNpcShakingInPlaceHasStalled)
{
  Level level;
  level.floors.push_back({MT_Vector3(-10, -10, 0), MT_Vector3(10, 10, 0)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  Objects objects;
  KX_GameObject &shaker = objects.Add(MT_Vector3(-5, 2, 0));
  KX_GameObject &walker = objects.Add(MT_Vector3(-5, -2, 0));
  for (int step = 0; step < 90; step++) {
    navigation->Submit(GoTo(shaker, MT_Vector3(5, 2, 0), 1.0f));
    navigation->Submit(GoTo(walker, MT_Vector3(5, -2, 0), 1.0f));
    navigation->Step(1.0f / 60.0f, nullptr);
    shaker.NodeSetWorldPosition(MT_Vector3(-5.0f + ((step % 2) ? 0.003f : -0.003f), 2, 0));
    shaker.NodeUpdateGS(0.0);
  }
  EXPECT_GT(navigation->StallSecondsForTests(&shaker), 1.0f);
  EXPECT_LT(navigation->StallSecondsForTests(&walker), 0.1f);
  EXPECT_GT(walker.NodeGetWorldPosition().x(), -4.0f);
}

/** A polite NPC blocked by a group standing in the open (one navmesh polygon: no search can lead
 * round it) goes round it: to the side fewer of them stand on, and on to its goal. One whose goal
 * is inside the group waits beside it (arrived), never circling. */
TEST(KX_Navigation, APoliteNpcGoesRoundAStandingGroup)
{
  struct Case {
    const char *name;
    float group_y;
    MT_Vector3 goal;
  };
  for (const Case &c : {Case{"in the middle", 0.0f, MT_Vector3(9, 0, 0)},
                        Case{"more on the left", 0.9f, MT_Vector3(9, 0, 0)},
                        Case{"at its goal", 0.0f, MT_Vector3(0, 0, 0)}})
  {
    Level level;
    level.floors.push_back({MT_Vector3(-10, -10, 0), MT_Vector3(10, 10, 0)});
    std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
    Objects objects;
    std::vector<KX_GameObject *> group;
    std::vector<MT_Vector3> places;
    for (int i = 0; i < 25; i++) {
      places.push_back(
          MT_Vector3(-1.4f + float(i % 5) * 0.7f, c.group_y - 1.4f + float(i / 5) * 0.7f, 0));
      group.push_back(&objects.Add(places.back()));
    }
    KX_GameObject &walker = objects.Add(MT_Vector3(-9, 0, 0));
    bool arrived = false;
    float south = 0.0f, north = 0.0f, closest = 1e9f;
    int step = 0;
    for (; step < 60 * 30 && !arrived; step++) {
      for (size_t i = 0; i < group.size(); i++) {
        KX_NavRequest request = GoTo(*group[i], places[i], 1.0f);
        request.arrive_distance = 0.2f;
        navigation->Submit(request);
      }
      if (step >= 60) {
        navigation->Submit(GoTo(walker, c.goal, 1.5f));
      }
      navigation->Step(1.0f / 60.0f, nullptr);
      const MT_Vector3 p = walker.NodeGetWorldPosition();
      south = std::min(south, float(p.y()));
      north = std::max(north, float(p.y()));
      for (KX_GameObject *other : group) {
        closest = std::min(closest, distance_2d(p, other->NodeGetWorldPosition()));
      }
      KX_NavAgentInfo info;
      navigation->GetAgentInfo(&walker, info);
      arrived = step >= 60 && info.arrived;
    }
    printf("round a standing group, %s: %s in %.1f s, y %.2f .. %.2f, closest %.2f m\n", c.name,
           arrived ? "arrived" : "NOT arrived", float(step) / 60.0f, south, north, closest);
    EXPECT_TRUE(arrived) << c.name;
    EXPECT_GT(closest, 0.5f) << c.name;
    if (c.goal.x() > 1.0f) {
      /* 18 m at 1.5 m/s: 12 s straight; going round costs a few seconds. */
      EXPECT_LT(float(step) / 60.0f, 20.0f) << c.name;
      EXPECT_NEAR(walker.NodeGetWorldPosition().x(), 9.0f, 0.4f) << c.name;
    }
    if (c.group_y > 0.0f) {
      /* Round the right (south) side, where fewer of them stand. */
      EXPECT_LT(south, -1.0f) << c.name;
      EXPECT_LT(north, 0.5f) << c.name;
    }
    if (c.goal.x() < 1.0f) {
      /* Waits at the group's edge: never went round it. */
      EXPECT_LT(walker.NodeGetWorldPosition().x(), -1.5f) << c.name;
      EXPECT_LT(std::max(north, -south), 1.0f) << c.name;
    }
  }
}

/** A crowd walking the same way is no reason to search again: 40 NPCs walking down a corridor
 * together keep to it (the way round, through a parallel corridor, is 6 m longer), all arrive. */
TEST(KX_Navigation, ACrowdWalkingTogetherKeepsItsWay)
{
  Level level;
  level.floors.push_back({MT_Vector3(-14, -2, 0), MT_Vector3(14, 2, 0)});
  level.floors.push_back({MT_Vector3(-14, 5, 0), MT_Vector3(14, 9, 0)});
  level.floors.push_back({MT_Vector3(-14, 2, 0), MT_Vector3(-11, 5, 0)});
  level.floors.push_back({MT_Vector3(11, 2, 0), MT_Vector3(14, 5, 0)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  Objects objects;
  std::vector<KX_GameObject *> npcs;
  std::vector<MT_Vector3> goals;
  for (int i = 0; i < 40; i++) {
    npcs.push_back(&objects.Add(MT_Vector3(-12.5f + float(i / 5) * 0.8f, -1.4f + float(i % 5) * 0.7f, 0)));
    goals.push_back(MT_Vector3(7.0f + float(i / 5) * 0.8f, -1.4f + float(i % 5) * 0.7f, 0));
  }
  float strayed = 0.0f;
  int arrived = 0;
  for (int step = 0; step < 60 * 40 && arrived < int(npcs.size()); step++) {
    arrived = 0;
    for (size_t i = 0; i < npcs.size(); i++) {
      KX_NavRequest request = GoTo(*npcs[i], goals[i], 1.4f);
      request.arrive_distance = 0.3f;
      navigation->Submit(request);
    }
    navigation->Step(1.0f / 60.0f, nullptr);
    for (size_t i = 0; i < npcs.size(); i++) {
      strayed = std::max(strayed, float(npcs[i]->NodeGetWorldPosition().y()));
      KX_NavAgentInfo info;
      navigation->GetAgentInfo(npcs[i], info);
      arrived += info.arrived ? 1 : 0;
    }
  }
  printf("a crowd walking together: %d of %zu arrived, farthest north %.2f m\n", arrived,
         npcs.size(), strayed);
  EXPECT_EQ(arrived, int(npcs.size()));
  EXPECT_LT(strayed, 2.5f);
}

/** Dense: a heavy NPC (Priority 100) walks through a corridor packed with light ones (Priority 0,
 * standing at their places) as through a crowd: shoving them aside, never stuck, they step back
 * to their places after. A polite one is held up by the standing crowd. Weight is what does it:
 * a light Dense one (Priority 0 among goblins of 100) takes clearly longer. */
TEST(KX_Navigation, AHeavyDenseNpcShovesLightOnesAside)
{
  float seconds[3] = {0.0f, 0.0f, 0.0f};
  /* 0 polite, 1 dense and heavy, 2 dense and light (goblins heavy). */
  for (const int run : {0, 1, 2}) {
    const bool dense = run > 0;
    const int ogre_priority = run == 2 ? 0 : 100;
    const int goblin_priority = run == 2 ? 100 : 0;
    Level level;
    level.floors.push_back({MT_Vector3(-12, -1.5f, 0), MT_Vector3(12, 1.5f, 0)});
    std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
    Objects objects;
    std::vector<KX_GameObject *> goblins;
    std::vector<MT_Vector3> places;
    for (int i = 0; i < 24; i++) {
      places.push_back(MT_Vector3(-4.0f + float(i / 3) * 1.0f, -0.8f + float(i % 3) * 0.8f, 0));
      goblins.push_back(&objects.Add(places.back()));
    }
    KX_GameObject &ogre = objects.Add(MT_Vector3(-10, 0, 0));
    bool arrived = false;
    int step = 0;
    for (; step < 60 * 40 && !arrived; step++) {
      for (size_t i = 0; i < goblins.size(); i++) {
        KX_NavRequest request = GoTo(*goblins[i], places[i], 1.0f);
        request.dense = dense;
        request.priority = goblin_priority;
        navigation->Submit(request);
      }
      KX_NavRequest request = GoTo(ogre, MT_Vector3(10, 0, 0), 1.6f);
      request.dense = dense;
      request.priority = ogre_priority;
      navigation->Submit(request);
      navigation->Step(1.0f / 60.0f, nullptr);
      KX_NavAgentInfo info;
      navigation->GetAgentInfo(&ogre, info);
      arrived = info.arrived;
    }
    seconds[run] = float(step) / 60.0f;
    printf("%s ogre through the goblins: %.1f s%s\n",
           run == 0 ? "polite" : (run == 1 ? "dense heavy" : "dense light"), seconds[run],
           arrived ? "" : " (not through)");
    if (run == 1) {
      EXPECT_TRUE(arrived);
      /* 20 m at 1.6 m/s: 12.5 s free; shoving costs little. */
      EXPECT_LT(seconds[run], 20.0f);
      /* Back at their places after a while. */
      for (int settle = 0; settle < 60 * 8; settle++) {
        for (size_t i = 0; i < goblins.size(); i++) {
          KX_NavRequest request = GoTo(*goblins[i], places[i], 1.0f);
          request.dense = true;
          request.priority = 0;
          navigation->Submit(request);
        }
        navigation->Step(1.0f / 60.0f, nullptr);
      }
      int home = 0;
      for (size_t i = 0; i < goblins.size(); i++) {
        home += distance_2d(goblins[i]->NodeGetWorldPosition(), places[i]) < 0.6f ? 1 : 0;
      }
      EXPECT_GE(home, 20);
    }
  }
  EXPECT_LT(seconds[1], seconds[0]);
  EXPECT_LT(seconds[1], seconds[2] * 0.8f);
}

/** A Dense crowd and the player (a Character that is no NPC, an obstacle with Avoid Physics
 * Objects): in the open the crowd parts round it, never inside it; in a corridor it blocks, the
 * crowd presses on it and, with Crowds Push Characters, pushes it along (else it stands). */
TEST(KX_Navigation, ADenseCrowdPartsRoundThePlayer)
{
  float player_moved[2] = {0.0f, 0.0f};
  for (const bool pushes : {false, true}) {
    Level level;
    level.floors.push_back({MT_Vector3(-12, -4, 0), MT_Vector3(12, 4, 0)});
    /* The corridor's walls: 1.6 m wide round y 0 from x -3 on. */
    level.boxes.push_back({MT_Vector3(-3, -4, 0), MT_Vector3(12, -0.8f, 2)});
    level.boxes.push_back({MT_Vector3(-3, 0.8f, 0), MT_Vector3(12, 4, 2)});
    std::unique_ptr<KX_Navigation> navigation = std::make_unique<KX_Navigation>(nullptr);
    KX_Navigation::Settings settings = TestSettings();
    settings.avoid_physics = true;
    settings.crowd_pushes = pushes;
    navigation->SetSettings(settings);
    navigation->SetGeometrySourceForTests(
        [&level](const MT_Vector3 &min, const MT_Vector3 &max, KX_NavGeometry &g) {
          level.Collect(min, max, g);
        });
    Objects objects;
    World world;
    world.StaticBox(objects.Add(MT_Vector3(0, 0, -0.5f)), MT_Vector3(12, 4, 0.5f));
    world.StaticBox(objects.Add(MT_Vector3(4.5f, -2.4f, 1)), MT_Vector3(7.5f, 1.6f, 1));
    world.StaticBox(objects.Add(MT_Vector3(4.5f, 2.4f, 1)), MT_Vector3(7.5f, 1.6f, 1));
    KX_GameObject &player = objects.Add(MT_Vector3(0, 0, 0.95f));
    ASSERT_NE(world.Character(player), nullptr);
    world.environment.GetPhysicsSystem()->OptimizeBroadPhase();
    navigation->SetPhysicsEnvironmentForTests(&world.environment);
    navigation->SetPhysicsObjectsForTests({&player});
    std::vector<KX_GameObject *> crowd;
    for (int i = 0; i < 30; i++) {
      crowd.push_back(&objects.Add(MT_Vector3(-9.0f + float(i / 6) * 0.8f, -2.0f + float(i % 6) * 0.8f, 0)));
    }
    const MT_Vector3 start = player.NodeGetWorldPosition();
    float deepest = 0.0f;
    for (int step = 0; step < 60 * 12; step++) {
      for (size_t i = 0; i < crowd.size(); i++) {
        KX_NavRequest request = GoTo(*crowd[i], MT_Vector3(10.0f, 0, 0), 1.6f);
        request.dense = true;
        navigation->Submit(request);
      }
      navigation->Step(1.0f / 60.0f, nullptr);
      world.Step({&player});
      for (KX_GameObject *npc : crowd) {
        /* The player's capsule 0.3 m, an NPC 0.3 m (TestSettings' agent). */
        deepest = std::max(deepest, 0.6f - distance_2d(npc->NodeGetWorldPosition(),
                                                       player.NodeGetWorldPosition()));
      }
    }
    player_moved[pushes] = distance_2d(player.NodeGetWorldPosition(), start);
    printf("crowd past the player (pushes %d): deepest %.2f m, player moved %.2f m\n", int(pushes),
           deepest, player_moved[pushes]);
    EXPECT_LT(deepest, 0.2f) << pushes;
  }
  EXPECT_LT(player_moved[false], 0.05f);
  EXPECT_GT(player_moved[true], 0.2f);
}

/** Get Navigation Info's Speed: how fast it really walks, smoothed, exactly 0 while it stands. */
TEST(KX_Navigation, SpeedIsSmoothedAndZeroWhileStanding)
{
  Level level;
  level.floors.push_back({MT_Vector3(-20, -5, 0), MT_Vector3(20, 5, 0)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  Objects objects;
  KX_GameObject &npc = objects.Add(MT_Vector3(-15, 0, 0));
  KX_NavAgentInfo info;
  float previous = 0.0f;
  bool rose_smoothly = true;
  for (int step = 0; step < 60 * 3; step++) {
    navigation->Submit(GoTo(npc, MT_Vector3(15, 0, 0), 2.0f));
    navigation->Step(1.0f / 60.0f, nullptr);
    ASSERT_TRUE(navigation->GetAgentInfo(&npc, info));
    rose_smoothly = rose_smoothly && info.speed >= previous - 1e-4f && info.speed <= 2.05f;
    previous = info.speed;
  }
  EXPECT_TRUE(rose_smoothly);
  EXPECT_NEAR(info.speed, 2.0f, 0.1f);
  /* Not asked any more: it stands; Speed drops to exactly 0 and stays there. */
  int zero_from = -1;
  for (int step = 0; step < 60; step++) {
    navigation->Step(1.0f / 60.0f, nullptr);
    navigation->GetAgentInfo(&npc, info);
    if (info.speed == 0.0f && zero_from < 0) {
      zero_from = step;
    }
    if (zero_from >= 0) {
      EXPECT_EQ(info.speed, 0.0f) << step;
    }
  }
  EXPECT_GE(zero_from, 0);
  EXPECT_LT(zero_from, 45);
}

/** NPCs whose goals lie in one big polygon share one search (a hall's places): each still goes
 * to its own place (2026-11-19: all walked to the first one's). */
TEST(KX_Navigation, ASharedSearchLeadsEachToItsOwnGoal)
{
  Level level;
  level.floors.push_back({MT_Vector3(-20, -10, 0), MT_Vector3(20, 10, 0)});
  level.boxes.push_back({MT_Vector3(-1, -10, 0), MT_Vector3(1, 6, 2)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  Objects objects;
  std::vector<KX_GameObject *> npcs;
  std::vector<MT_Vector3> goals;
  for (int i = 0; i < 16; i++) {
    npcs.push_back(&objects.Add(MT_Vector3(-15.0f + float(i % 4), -6.0f + float(i / 4), 0)));
    goals.push_back(MT_Vector3(8.0f + float(i % 4) * 2.0f, -6.0f + float(i / 4) * 2.5f, 0));
  }
  for (int step = 0; step < 60 * 30; step++) {
    for (size_t i = 0; i < npcs.size(); i++) {
      navigation->Submit(GoTo(*npcs[i], goals[i], 3.0f));
    }
    navigation->Step(1.0f / 60.0f, nullptr);
  }
  EXPECT_GE(navigation->GetSharedSearchCount(), 1u);
  for (size_t i = 0; i < npcs.size(); i++) {
    EXPECT_LT(distance_2d(npcs[i]->NodeGetWorldPosition(), goals[i]), 0.5f) << i;
  }
}

/** Timing: 10,000 Dense NPCs crossing a field in two crowds (UPBGE_LN_BENCH_ROUNDS, helpers 0
 * and 15). */
TEST(KX_Navigation, DISABLED_DenseCrowdCost)
{
  for (const uint32_t helpers : {0u, 15u}) {
    Level level;
    level.floors.push_back({MT_Vector3(-100, -100, 0), MT_Vector3(100, 100, 0)});
    std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
    LN_JobPool pool;
    pool.SetHelperCount(helpers);
    Objects objects;
    std::vector<KX_GameObject *> npcs;
    std::vector<MT_Vector3> goals;
    for (int i = 0; i < 10000; i++) {
      const float side = (i % 2) ? 1.0f : -1.0f;
      const float x = side * (20.0f + float((i / 2) % 50) * 0.8f);
      const float y = -40.0f + float((i / 2) / 50) * 0.8f;
      npcs.push_back(&objects.Add(MT_Vector3(x, y, 0)));
      goals.push_back(MT_Vector3(-x, y, 0));
    }
    double total = 0.0;
    int measured = 0;
    for (int step = 0; step < 300; step++) {
      for (size_t i = 0; i < npcs.size(); i++) {
        KX_NavRequest request = GoTo(*npcs[i], goals[i], 1.6f);
        request.dense = true;
        navigation->Submit(request);
      }
      const auto t0 = std::chrono::steady_clock::now();
      navigation->Step(1.0f / 60.0f, helpers > 0 ? &pool : nullptr);
      const auto t1 = std::chrono::steady_clock::now();
      if (step >= 60) {
        total += std::chrono::duration<double, std::milli>(t1 - t0).count();
        measured++;
      }
    }
    printf("10,000 Dense NPCs, %u helpers: %.2f ms a step\n", helpers, total / measured);
  }
}

/** \} */


/* -------------------------------------------------------------------- */
/** \name Navigation settings (2026-11-20): Physics > Navigation, closed links, the node's options
 * \{ */

/** Two floors 2 m apart joined by a door link (x -1.5 to 1.5 at y 4) and, far south, by a
 * walkway (y -10..-8): the way round is about 18 m longer. */
Level DoorAndWalkway()
{
  Level level;
  level.floors.push_back({MT_Vector3(-10, -10, 0), MT_Vector3(-1, 10, 0)});
  level.floors.push_back({MT_Vector3(1, -10, 0), MT_Vector3(10, 10, 0)});
  level.floors.push_back({MT_Vector3(-1, -10, 0), MT_Vector3(1, -8, 0)});
  return level;
}

KX_NavLink DoorLink(KX_GameObject &door, KX_GameObject &other)
{
  KX_NavLink link;
  link.object = &door;
  link.end_object = &other;
  link.start = MT_Vector3(-1.5f, 4, 0);
  link.end = MT_Vector3(1.5f, 4, 0);
  link.kind = "door";
  link.user_id = 5;
  return link;
}

/** A closed link (Link Enabled off): Go Round leaves it out (the walkway far south); Wait takes
 * it, the NPC standing at its start (Waiting, Waiting At the door) until it opens, then through;
 * a Give Up After makes it go round after that long; a link closing on an NPC's way sends it
 * round (Go Round) without a step lost. */
TEST(KX_Navigation, AClosedLinkIsGoneRoundOrWaitedAt)
{
  struct Run {
    const char *name;
    bool closed;
    bool wait;
    float wait_time;
    /* Closed this many steps into the walk (-1: from the start), opened at `open_step` (-1:
     * never). */
    int close_step;
    int open_step;
    bool round;
  };
  const Run runs[] = {
      {"open", false, false, 0.0f, -1, -1, false},
      {"go round", true, false, 0.0f, -1, -1, true},
      {"wait, then opened", true, true, 0.0f, -1, 60 * 6, false},
      {"wait, giving up", true, true, 1.5f, -1, -1, true},
      {"closed on its way", false, false, 0.0f, 40, -1, true},
  };
  for (const Run &run : runs) {
    SCOPED_TRACE(run.name);
    Level level = DoorAndWalkway();
    std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
    Objects objects;
    KX_GameObject &door = objects.Add(MT_Vector3(-1.5f, 4, 0));
    KX_GameObject &other = objects.Add(MT_Vector3(1.5f, 4, 0));
    KX_NavLink link = DoorLink(door, other);
    link.enabled = !(run.closed && run.close_step < 0);
    link.wait = run.wait;
    link.wait_time = run.wait_time;
    navigation->SetLinksForTests({link});
    KX_GameObject &npc = objects.Add(MT_Vector3(-6, 4, 0));
    KX_NavAgentInfo info;
    float lowest = 1e9f;
    int waited = 0, crossed = 0;
    bool arrived = false;
    for (int step = 0; step < 60 * 30 && !arrived; step++) {
      if (step == run.close_step) {
        link.enabled = false;
        navigation->SetLinksForTests({link});
      }
      if (step == run.open_step) {
        link.enabled = true;
        navigation->SetLinksForTests({link});
      }
      navigation->Submit(GoTo(npc, MT_Vector3(6, 4, 0)));
      navigation->Step(1.0f / 60.0f, nullptr);
      ASSERT_TRUE(navigation->GetAgentInfo(&npc, info));
      lowest = std::min(lowest, float(npc.NodeGetWorldPosition().y()));
      if (info.waiting) {
        waited++;
        EXPECT_EQ(info.waiting_at, &door);
        EXPECT_LT(npc.NodeGetWorldPosition().x(), -1.0f);
      }
      crossed += info.link_started ? 1 : 0;
      arrived = info.arrived;
    }
    EXPECT_TRUE(arrived);
    EXPECT_NEAR(npc.NodeGetWorldPosition().x(), 6.0f, 0.4f);
    if (run.round) {
      EXPECT_LT(lowest, -7.0f);
      EXPECT_EQ(crossed, 0);
    }
    else {
      EXPECT_GT(lowest, 2.0f);
      EXPECT_EQ(crossed, 1);
    }
    if (run.wait && run.wait_time == 0.0f) {
      /* It stood at the door until it opened (6 s after starting, about 4 s of it there). */
      EXPECT_GT(waited, 60 * 3);
    }
    else if (run.wait) {
      EXPECT_NEAR(float(waited), 1.5f * 60.0f, 6.0f);
    }
    else {
      EXPECT_EQ(waited, 0);
    }
  }
}

/** A link's objects are never physics obstacles (Avoid Physics Objects, 2026-11-20): a resting
 * rigid-body ladder carved out of the navmesh cut away its own foot. */
TEST(KX_Navigation, ALinksObjectsAreNeverPhysicsObstacles)
{
  Level level;
  level.floors.push_back({MT_Vector3(-10, -10, 0), MT_Vector3(10, 10, 0)});
  std::unique_ptr<KX_Navigation> navigation = std::make_unique<KX_Navigation>(nullptr);
  KX_Navigation::Settings settings = TestSettings();
  settings.avoid_physics = true;
  navigation->SetSettings(settings);
  navigation->SetGeometrySourceForTests(
      [&level](const MT_Vector3 &min, const MT_Vector3 &max, KX_NavGeometry &g) {
        level.Collect(min, max, g);
      });
  Objects objects;
  World world;
  world.StaticBox(objects.Add(MT_Vector3(0, 0, -0.5f)), MT_Vector3(10, 10, 0.5f));
  KX_GameObject &ladder = objects.Add(MT_Vector3(0, 0, 0.5f));
  world.Add(ladder, new JPH::BoxShape(JPH::Vec3(1.5f, 0.4f, 0.1f)), JPH::EMotionType::Dynamic);
  KX_GameObject &top = objects.Add(MT_Vector3(1.4f, 0, 0.6f));
  world.environment.GetPhysicsSystem()->OptimizeBroadPhase();
  navigation->SetPhysicsEnvironmentForTests(&world.environment);
  navigation->SetPhysicsObjectsForTests({&ladder});
  EXPECT_TRUE(navigation->IsPhysicsObstacle(&ladder));
  KX_NavLink link;
  link.object = &ladder;
  link.end_object = &top;
  link.start = MT_Vector3(-1.4f, 0, 0);
  link.end = MT_Vector3(1.4f, 0, 0);
  link.kind = "ladder";
  navigation->SetLinksForTests({link});
  EXPECT_FALSE(navigation->IsPhysicsObstacle(&ladder));
  EXPECT_TRUE(navigation->IsLinkObject(&top));
  /* At rest it is not carved: the navmesh still runs under it. */
  bool asleep = false;
  for (int step = 0; step < 60 * 6 && !asleep; step++) {
    navigation->Step(1.0f / 60.0f, nullptr);
    world.Step({&ladder});
    asleep = ladder.GetPhysicsController()->IsAsleep();
  }
  ASSERT_TRUE(asleep);
  navigation->Step(1.0f / 60.0f, nullptr);
  navigation->Step(1.0f / 60.0f, nullptr);
  KX_NavMesh *navmesh = navigation->GetSceneNavMesh();
  ASSERT_NE(navmesh, nullptr);
  float fraction = 0.0f;
  MT_Vector3 hit, normal;
  EXPECT_FALSE(navmesh->Raycast(MT_Vector3(0, -5, 0), MT_Vector3(0, 5, 0), fraction, hit, normal));
  /* Its link gone, it is an obstacle again (carved while it rests). */
  navigation->SetLinksForTests({});
  EXPECT_TRUE(navigation->IsPhysicsObstacle(&ladder));
  navigation->Step(1.0f / 60.0f, nullptr);
  navigation->Step(1.0f / 60.0f, nullptr);
  EXPECT_TRUE(navmesh->Raycast(MT_Vector3(0, -5, 0), MT_Vector3(0, 5, 0), fraction, hit, normal));
}

/** A link that comes with its end in the neighbouring tile (12.8 m tiles: the start at x 10.5,
 * the end at 14) joins there: the end's tile is made again too (Detour joins a landing only to
 * an end tile built knowing of the link). */
TEST(KX_Navigation, ALinkEndingInTheNextTileJoinsThere)
{
  Level level;
  level.floors.push_back({MT_Vector3(0, -6, 0), MT_Vector3(11, 6, 0)});
  level.floors.push_back({MT_Vector3(13, -6, 0), MT_Vector3(24, 6, 0)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  Objects objects;
  KX_GameObject &npc = objects.Add(MT_Vector3(4, 0, 0));
  KX_NavAgentInfo info;
  MT_Vector3 goal(20, 0, 0);
  auto walk = [&](const int steps) {
    for (int step = 0; step < steps; step++) {
      navigation->Submit(GoTo(npc, goal));
      navigation->Step(1.0f / 60.0f, nullptr);
      if (navigation->GetAgentInfo(&npc, info) && info.arrived) {
        return true;
      }
    }
    return false;
  };
  /* Both tiles built without it: no way across. */
  EXPECT_FALSE(walk(60));
  KX_GameObject &plank = objects.Add(MT_Vector3(10.5f, 0, 0));
  KX_NavLink link;
  link.object = &plank;
  link.start = MT_Vector3(10.5f, 0, 0);
  link.end = MT_Vector3(14.0f, 0, 0);
  link.kind = "gangway";
  link.user_id = 9;
  navigation->SetLinksForTests({link});
  EXPECT_TRUE(walk(60 * 15));
  EXPECT_NEAR(npc.NodeGetWorldPosition().x(), 20.0f, 0.4f);
  /* And back (both ways: Detour needs room for the way back in the end's tile). */
  goal = MT_Vector3(4, 0, 0);
  EXPECT_TRUE(walk(60 * 15));
  EXPECT_NEAR(npc.NodeGetWorldPosition().x(), 4.0f, 0.4f);
}

/** Set Navigation Attribute's Link Enabled and Link End (2026-11-20), their inputs read from
 * game properties while the game runs: the teleporter closed (Wait) keeps the NPC waiting at it,
 * Get Navigation Info says so; opened, it goes through; then the link turned to a far end (its
 * new destination, farther than Detour joins links) takes it there. */
TEST(KX_NavigationNodes, LinkEnabledAndLinkEndChangeALinkWhilePlaying)
{
  using A = LN_NavigationAttribute;
  blender::Object door_blender{};
  std::snprintf(door_blender.id.name, sizeof(door_blender.id.name), "OBDoor");
  NavTree tree;
  diff::GenNode &enable = tree.Add("LogicNativeSetNavigationAttribute", int16_t(A::LinkEnabled));
  tree.b.ObjectNamed(enable, "Object", &door_blender);
  diff::GenNode &open = tree.Add("LogicNativeGetGamePropertyBool");
  tree.b.String(open, "Property", "open");
  tree.b.Link(open, "Value", enable, "Enabled");
  tree.Chain(enable);
  diff::GenNode &end = tree.Add("LogicNativeSetNavigationAttribute", int16_t(A::LinkEnd));
  tree.b.ObjectNamed(end, "Object", &door_blender);
  diff::GenNode &end_name = tree.Add("LogicNativeGetGamePropertyString");
  tree.b.String(end_name, "Property", "end");
  diff::GenNode &by_name = tree.Add("LogicNativeObjectByName");
  tree.b.Link(end_name, "Value", by_name, "Name");
  tree.b.Link(by_name, "Object", end, "End Object");
  tree.Chain(end);
  diff::GenNode &navigate = tree.Add("LogicNativeNavigate");
  diff::GenNode &goal_x = tree.Add("LogicNativeGetGamePropertyFloat");
  tree.b.String(goal_x, "Property", "goal_x");
  diff::GenNode &goal = tree.Add("LogicNativeCombineXYZ");
  tree.b.Link(goal_x, "Value", goal, "X");
  tree.b.Float(goal, "Y", 4.0f);
  tree.b.Link(goal, "Vector", navigate, "Destination");
  tree.b.Float(navigate, "Lin Speed", 3.0f);
  tree.b.Float(navigate, "Reach Threshold", 0.3f);
  tree.Chain(navigate);
  diff::GenNode &info = tree.Add("LogicNativeGetNavigationInfo");
  tree.StoreBool(info, "Waiting", "waiting");
  tree.StoreText(info, "Waiting At", "waiting_at");
  tree.StoreBool(info, "Arrived", "arrived");
  const std::shared_ptr<LN_Program> program = tree.Compile();
  ASSERT_NE(program, nullptr);

  Level level = DoorAndWalkway();
  /* A far floor, reached only by the link once it ends there. */
  level.floors.push_back({MT_Vector3(20, -2, 0), MT_Vector3(26, 10, 0)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  NavigationSeam seam(navigation.get());
  SemRig rig(program);
  KX_GameObject &npc = rig.Object(0);
  Place(npc, MT_Vector3(-6, 4, 0));
  KX_GameObject &door = rig.AddObject("Door", MT_Vector3(-1.5f, 4, 0));
  KX_GameObject &other = rig.AddObject("Other", MT_Vector3(1.5f, 4, 0));
  rig.AddObject("Far", MT_Vector3(22, 4, 0));
  rig.SetBool(npc, "open", false);
  rig.SetString(npc, "end", "Other");
  rig.SetFloat(npc, "goal_x", 6.0f);
  /* (A teleporter: its new end is 24 m away, beyond the tiles Detour joins a link to: routed
   * by the navigation as a long link.) */
  KX_NavLink link = DoorLink(door, other);
  link.kind = "teleport";
  link.wait = true;
  navigation->SetLinksForTests({link});
  auto run = [&](const int steps) {
    for (int step = 0; step < steps; step++) {
      rig.Step();
      navigation->Step(1.0f / 60.0f, nullptr);
      if (rig.Float(npc, "arrived") == 1.0f) {
        return true;
      }
    }
    return false;
  };
  EXPECT_FALSE(run(60 * 6));
  EXPECT_EQ(rig.Float(npc, "waiting"), 1.0f);
  EXPECT_EQ(rig.Text(npc, "waiting_at"), "Door");
  EXPECT_LT(npc.NodeGetWorldPosition().x(), -1.0f);
  EXPECT_TRUE(door.GetNavChanged() & KX_NAV_CHANGED_LINK_ENABLED);
  rig.SetBool(npc, "open", true);
  EXPECT_TRUE(run(60 * 10));
  EXPECT_NEAR(npc.NodeGetWorldPosition().x(), 6.0f, 0.4f);
  /* Back west, the link now ends on the far floor: the NPC goes there through it. */
  rig.SetFloat(npc, "goal_x", -6.0f);
  EXPECT_TRUE(run(60 * 10));
  rig.SetString(npc, "end", "Far");
  rig.SetFloat(npc, "goal_x", 24.0f);
  EXPECT_TRUE(run(60 * 15));
  EXPECT_NEAR(npc.NodeGetWorldPosition().x(), 24.0f, 0.4f);
  EXPECT_EQ(door.GetNavLinkEnd()->GetName(), "Far");
}

/** Set Navigation Attribute's Vehicle and Area (2026-11-20), from game properties: the ship stops
 * being a vehicle (its NPC stands on the world again) and is one again; an object's area changes
 * (Save Object State keeps what changed). */
TEST(KX_NavigationNodes, VehicleAndAreaChangeWhilePlaying)
{
  using A = LN_NavigationAttribute;
  blender::Object ship_blender{}, floor_blender{};
  std::snprintf(ship_blender.id.name, sizeof(ship_blender.id.name), "OBShip");
  std::snprintf(floor_blender.id.name, sizeof(floor_blender.id.name), "OBFloor");
  NavTree tree;
  diff::GenNode &vehicle = tree.Add("LogicNativeSetNavigationAttribute", int16_t(A::Vehicle));
  tree.b.ObjectNamed(vehicle, "Object", &ship_blender);
  diff::GenNode &is_vehicle = tree.Add("LogicNativeGetGamePropertyBool");
  tree.b.String(is_vehicle, "Property", "vehicle");
  tree.b.Link(is_vehicle, "Value", vehicle, "Enabled");
  tree.Chain(vehicle);
  diff::GenNode &area = tree.Add("LogicNativeSetNavigationAttribute", int16_t(A::Area));
  tree.b.ObjectNamed(area, "Object", &floor_blender);
  diff::GenNode &area_name = tree.Add("LogicNativeGetGamePropertyString");
  tree.b.String(area_name, "Property", "area");
  tree.b.Link(area_name, "Value", area, "Area");
  tree.Chain(area);
  diff::GenNode &navigate = tree.Add("LogicNativeNavigate");
  tree.b.Vector(navigate, "Destination", 25, 1, 0);
  tree.b.Float(navigate, "Lin Speed", 3.0f);
  tree.b.Float(navigate, "Reach Threshold", 0.3f);
  tree.Chain(navigate);
  const std::shared_ptr<LN_Program> program = tree.Compile();
  ASSERT_NE(program, nullptr);

  Level dock;
  dock.floors.push_back({MT_Vector3(-20, -10, 0), MT_Vector3(0, 10, 0)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(dock, TestSettings());
  NavigationSeam seam(navigation.get());
  SemRig rig(program);
  KX_GameObject &npc = rig.Object(0);
  KX_GameObject &ship = rig.AddObject("Ship", MT_Vector3(23, 0, -1));
  KX_GameObject &floor = rig.AddObject("Floor", MT_Vector3(-10, 0, 0));
  ship.NodeUpdateGS(0.0);
  Place(npc, MT_Vector3(23, 1, 0.5f));
  const Level shape = ShipShape();
  navigation->SetVehiclesForTests({&ship});
  navigation->SetVehicleGeometryForTests([&shape](KX_GameObject *, KX_NavGeometry &g) {
    shape.Collect(MT_Vector3(0, 0, 0), MT_Vector3(0, 0, 0), g);
  });
  rig.SetBool(npc, "vehicle", true);
  rig.SetString(npc, "area", "");
  for (int step = 0; step < 30; step++) {
    rig.Step();
    navigation->Step(1.0f / 60.0f, nullptr);
  }
  EXPECT_EQ(navigation->AgentFrame(&npc), &ship);
  EXPECT_EQ(navigation->VehicleOf(&ship), &ship);
  /* No vehicle: the NPC stands on the world (no navmesh under it there). */
  rig.SetBool(npc, "vehicle", false);
  for (int step = 0; step < 10; step++) {
    rig.Step();
    navigation->Step(1.0f / 60.0f, nullptr);
  }
  EXPECT_EQ(navigation->VehicleOf(&ship), nullptr);
  EXPECT_EQ(navigation->AgentFrame(&npc), nullptr);
  EXPECT_FALSE(ship.GetNavSettings().vehicle);
  EXPECT_TRUE(ship.GetNavChanged() & KX_NAV_CHANGED_VEHICLE);
  /* A vehicle again. */
  rig.SetBool(npc, "vehicle", true);
  for (int step = 0; step < 60; step++) {
    rig.Step();
    navigation->Step(1.0f / 60.0f, nullptr);
  }
  EXPECT_EQ(navigation->VehicleOf(&ship), &ship);
  EXPECT_EQ(navigation->AgentFrame(&npc), &ship);
  /* The floor's area. */
  EXPECT_FALSE(floor.GetNavChanged() & KX_NAV_CHANGED_AREA);
  rig.SetString(npc, "area", "lava");
  rig.Step();
  navigation->Step(1.0f / 60.0f, nullptr);
  EXPECT_EQ(floor.GetNavSettings().area, "lava");
  EXPECT_TRUE(floor.GetNavChanged() & KX_NAV_CHANGED_AREA);
}

/** The level's areas from Physics > Navigation > Area with real Jolt bodies (the geometry
 * path, not the tests' seam): a slab's triangles carry its area, the floor's none; changed while
 * playing, the slab's tiles are made again with the new one. */
TEST(KX_Navigation, AnAreaSetWhilePlayingRebuildsItsTiles)
{
  Objects objects;
  World world;
  KX_GameObject &ground = objects.Add(MT_Vector3(0, 0, -0.5f));
  world.StaticBox(ground, MT_Vector3(10, 10, 0.5f));
  KX_GameObject &slab = objects.Add(MT_Vector3(0, 0, 0.05f));
  world.StaticBox(slab, MT_Vector3(2, 10, 0.05f));
  world.environment.GetPhysicsSystem()->OptimizeBroadPhase();
  std::unique_ptr<KX_Navigation> navigation = std::make_unique<KX_Navigation>(nullptr);
  navigation->SetSettings(TestSettings());
  navigation->SetPhysicsEnvironmentForTests(&world.environment);
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
  KX_GameObject &npc = objects.Add(MT_Vector3(-6, 0, 0.5f));
  navigation->SetAreaCost(&npc, "lava", 1.0f, true);
  auto cross = [&](const MT_Vector3 &goal) {
    bool on_slab = false;
    for (int step = 0; step < 60 * 12; step++) {
      KX_NavRequest request = GoTo(npc, goal, 3.0f);
      request.avoid = false;
      navigation->Submit(request);
      navigation->Step(1.0f / 60.0f, nullptr);
      on_slab = on_slab || std::fabs(npc.NodeGetWorldPosition().x()) < 1.5f;
      KX_NavAgentInfo info;
      if (navigation->GetAgentInfo(&npc, info) && info.arrived) {
        break;
      }
    }
    return on_slab;
  };
  /* Ground: it walks across. */
  EXPECT_TRUE(cross(MT_Vector3(6, 0, 0)));
  EXPECT_NEAR(npc.NodeGetWorldPosition().x(), 6.0f, 0.4f);
  /* Lava, closed to it: it cannot cross back. */
  slab.SetNavArea("lava");
  navigation->NoteAreaChanged(&slab);
  EXPECT_FALSE(cross(MT_Vector3(-6, 0, 0)));
  EXPECT_GT(npc.NodeGetWorldPosition().x(), 1.0f);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vehicle checks owed (Navigation_Settings_And_Vehicles_Plan.md E1, 2026-11-20)
 * \{ */

/** A Dynamic ship (moved by its physics: 3 m/s and turning, no gravity) carries a sliding NPC
 * standing on its deck without lag (the move its velocities give it, ahead), and a Character
 * rides it (Jolt's ground velocity): both stay where they stood on the deck. */
TEST(KX_NavVehicles, ADynamicShipCarriesItsNpcsWithoutLag)
{
  Level sea;
  sea.floors.push_back({MT_Vector3(-80, -80, -10), MT_Vector3(80, 80, -10)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(sea, TestSettings());
  Objects objects;
  World world;
  KX_GameObject &ship = objects.Add(MT_Vector3(0, 0, 0));
  Body &hull = world.Add(
      ship, new JPH::BoxShape(JPH::Vec3(6.0f, 1.0f, 2.0f), 0.0f), JPH::EMotionType::Dynamic);
  hull.controller.SetGravityFactor(0.0f);
  world.environment.GetPhysicsSystem()->OptimizeBroadPhase();
  navigation->SetPhysicsEnvironmentForTests(&world.environment);
  navigation->SetVehiclesForTests({&ship});
  const MT_Vector3 slider_spot(-3.0f, -0.6f, 1.0f), rider_spot(3.0f, 0.6f, 1.95f);
  KX_GameObject &slider = objects.Add(WorldOf(ship, slider_spot));
  KX_GameObject &rider = objects.Add(WorldOf(ship, rider_spot));
  ASSERT_NE(world.Character(rider), nullptr);
  /* Settled on the deck first, then the ship gets going. */
  float worst_slider = 0.0f, worst_rider = 0.0f;
  for (int step = 0; step < 60 * 8; step++) {
    if (step == 60) {
      hull.controller.SetLinearVelocity(MT_Vector3(3.0f, 0.0f, 0.0f), false);
      hull.controller.SetAngularVelocity(MT_Vector3(0.0f, 0.0f, 0.25f), false);
    }
    for (KX_GameObject *npc : {&slider, &rider}) {
      KX_NavRequest stay = GoTo(*npc, WorldOf(ship, npc == &slider ? slider_spot : rider_spot));
      stay.arrive_distance = 0.4f;
      navigation->Submit(stay);
    }
    navigation->Step(1.0f / 60.0f, nullptr);
    world.Step({&ship, &slider, &rider});
    if (step > 90) {
      worst_slider = std::max(
          worst_slider, distance_2d(LocalOf(ship, slider.NodeGetWorldPosition()), slider_spot));
      worst_rider = std::max(
          worst_rider, distance_2d(LocalOf(ship, rider.NodeGetWorldPosition()), rider_spot));
    }
  }
  EXPECT_GT(float(ship.NodeGetWorldPosition().length()), 10.0f);
  EXPECT_EQ(navigation->AgentFrame(&slider), &ship);
  EXPECT_EQ(navigation->AgentFrame(&rider), &ship);
  /* (Within its arrival distance: it stood, carried along, the whole voyage.) */
  EXPECT_LT(worst_slider, 0.45f);
  EXPECT_LT(worst_rider, 0.45f);
}

/** Around NPCs (E1): an NPC on a ship at sea makes the world build no tiles under it (the
 * seabed); once the ship stands still it does (its docking places are found there). */
TEST(KX_NavVehicles, AroundNpcsAShipAtSeaBuildsNoWorldTiles)
{
  Level sea;
  sea.floors.push_back({MT_Vector3(-200, -40, -10), MT_Vector3(200, 40, -10)});
  KX_Navigation::Settings settings = TestSettings(blender::NAVMESH_BUILD_AROUND_NPCS);
  settings.build_radius = 15.0f;
  settings.remove_radius = 25.0f;
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(sea, settings);
  Objects objects;
  KX_GameObject &ship = objects.Add(MT_Vector3(0, 0, 0));
  const Level shape = ShipShape();
  navigation->SetVehiclesForTests({&ship});
  navigation->SetVehicleGeometryForTests([&shape](KX_GameObject *, KX_NavGeometry &g) {
    shape.Collect(MT_Vector3(0, 0, 0), MT_Vector3(0, 0, 0), g);
  });
  KX_GameObject &npc = objects.Add(WorldOf(ship, MT_Vector3(-3, 0, 1)));
  float x = 0.0f;
  auto sail = [&](const int steps, const float speed) {
    for (int step = 0; step < steps; step++) {
      x += speed / 60.0f;
      PlaceTurned(ship, MT_Vector3(x, 0, 0), 0.0f);
      navigation->Submit(GoTo(npc, WorldOf(ship, MT_Vector3(-3, 0, 1))));
      navigation->Step(1.0f / 60.0f, nullptr);
      npc.NodeUpdateGS(0.0);
    }
  };
  sail(30, 0.0f);
  ASSERT_EQ(navigation->AgentFrame(&npc), &ship);
  const uint64_t before = KX_NavBuildGetStats(*navigation->GetBuildState()).tiles_built;
  /* 150 m at sea: no tile of the seabed. */
  sail(60 * 15, 10.0f);
  EXPECT_EQ(navigation->AgentFrame(&npc), &ship);
  const uint64_t at_sea = KX_NavBuildGetStats(*navigation->GetBuildState()).tiles_built;
  EXPECT_EQ(at_sea, before);
  /* Standing still: the world round it. */
  sail(60, 0.0f);
  EXPECT_GT(KX_NavBuildGetStats(*navigation->GetBuildState()).tiles_built, at_sea);
}

/** Save and Load aboard (E1): a held order to a place on a moving ship's deck read
 * (GetHeldOrder), the NPC put back elsewhere on the deck and the order restored (as Load Game
 * does): it stands on the ship again and arrives; a passenger holding an order to the deck while
 * the gangway is out of reach is restored waiting at its quay end. */
TEST(KX_NavVehicles, HeldOrdersAboardAreRestoredAboard)
{
  Level dock;
  dock.floors.push_back({MT_Vector3(-20, -10, 0), MT_Vector3(0, 10, 0)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(dock, TestSettings());
  Objects objects;
  KX_GameObject &ship = objects.Add(MT_Vector3(30, 0, -1));
  KX_GameObject &ship_end = AddChildObject(objects, ship, MT_Vector3(-5.4f, 0, 1));
  KX_GameObject &quay_end = objects.Add(MT_Vector3(-0.6f, 0, 0));
  const Level shape = ShipShape();
  navigation->SetVehiclesForTests({&ship});
  navigation->SetVehicleGeometryForTests([&shape](KX_GameObject *, KX_NavGeometry &g) {
    shape.Collect(MT_Vector3(0, 0, 0), MT_Vector3(0, 0, 0), g);
  });
  KX_NavLink gangway;
  gangway.object = &quay_end;
  gangway.end_object = &ship_end;
  gangway.kind = "gangway";
  gangway.reach = 2.5f;
  gangway.user_id = 1;
  navigation->SetLinksForTests({gangway});
  ship.NodeUpdateGS(0.0);
  KX_GameObject &sailor = objects.Add(WorldOf(ship, MT_Vector3(-4, -1, 1)));
  KX_GameObject &passenger = objects.Add(MT_Vector3(-8, 3, 0));
  /* (The goal: a marker on the deck, Target Object: it goes along with the ship.) */
  KX_GameObject &marker = AddChildObject(objects, ship, MT_Vector3(4, 1, 1));
  auto held = [&](KX_GameObject &npc) {
    KX_NavRequest request = GoTo(npc, marker.NodeGetWorldPosition(), 2.0f);
    request.target = &marker;
    request.until_arrived = true;
    return request;
  };
  navigation->Submit(held(sailor));
  navigation->Submit(held(passenger));
  float x = 30.0f;
  auto sail = [&](const int steps) {
    for (int step = 0; step < steps; step++) {
      x += 1.0f / 60.0f;
      ship.NodeSetWorldPosition(MT_Vector3(x, 0, -1));
      ship.NodeUpdateGS(0.0);
      navigation->Step(1.0f / 60.0f, nullptr);
      sailor.NodeUpdateGS(0.0);
    }
  };
  sail(30);
  /* "Save": both orders held; "Load": the sailor put back on the deck's other end, the
   * passenger at the quay's far side, their orders restored. */
  KX_NavRequest sailor_order, passenger_order;
  ASSERT_TRUE(navigation->GetHeldOrder(&sailor, sailor_order));
  ASSERT_TRUE(navigation->GetHeldOrder(&passenger, passenger_order));
  sailor.NodeSetWorldPosition(WorldOf(ship, MT_Vector3(-4.5f, 1, 1)));
  sailor.NodeUpdateGS(0.0);
  passenger.NodeSetWorldPosition(MT_Vector3(-15, -5, 0));
  passenger.NodeUpdateGS(0.0);
  navigation->RestoreHeldOrder(&sailor, &sailor_order);
  navigation->RestoreHeldOrder(&passenger, &passenger_order);
  KX_NavAgentInfo sailor_info, passenger_info;
  bool passenger_waited = false;
  for (int step = 0; step < 60 * 10; step++) {
    sail(1);
    navigation->GetAgentInfo(&sailor, sailor_info);
    navigation->GetAgentInfo(&passenger, passenger_info);
    passenger_waited = passenger_waited || passenger_info.waiting;
  }
  EXPECT_EQ(navigation->AgentFrame(&sailor), &ship);
  EXPECT_TRUE(sailor_info.arrived);
  const MT_Vector3 there = LocalOf(ship, sailor.NodeGetWorldPosition());
  EXPECT_NEAR(float(there.x()), 4.0f, 0.5f);
  EXPECT_TRUE(passenger_waited);
  EXPECT_EQ(passenger_info.waiting_at, &quay_end);
  EXPECT_EQ(navigation->AgentFrame(&passenger), nullptr);
}

/** Many vehicles (E1, a benchmark): 100 train cars and 20 ships moving at once, 1,000 NPCs aboard
 * walking their decks; the step's cost with 0 and 8 helpers (UPBGE_LN_BENCH_ROUNDS steps). */
TEST(KX_NavVehicles, DISABLED_ManyVehiclesCost)
{
  const char *rounds_text = getenv("UPBGE_LN_BENCH_ROUNDS");
  const int rounds = rounds_text ? std::max(atoi(rounds_text), 60) : 300;
  for (const unsigned helpers : {0u, 8u}) {
    Level ground;
    ground.floors.push_back({MT_Vector3(-300, -300, -10), MT_Vector3(300, 300, -10)});
    std::unique_ptr<KX_Navigation> navigation = MakeNavigation(ground, TestSettings());
    LN_JobPool pool;
    pool.SetHelperCount(helpers);
    Objects objects;
    std::vector<KX_GameObject *> vehicles;
    for (int i = 0; i < 120; i++) {
      vehicles.push_back(&objects.Add(MT_Vector3(-250.0f + (i % 20) * 25.0f,
                                                 -250.0f + (i / 20) * 25.0f, 0)));
    }
    const Level car = ShipShape();
    Level ship_shape;
    ship_shape.boxes.push_back({MT_Vector3(-9, -3, -1), MT_Vector3(9, 3, 1)});
    navigation->SetVehiclesForTests(vehicles);
    navigation->SetVehicleGeometryForTests(
        [&](KX_GameObject *vehicle, KX_NavGeometry &g) {
          const bool is_ship = (std::find(vehicles.begin(), vehicles.end(), vehicle) -
                                vehicles.begin()) >= 100;
          (is_ship ? ship_shape : car).Collect(MT_Vector3(0, 0, 0), MT_Vector3(0, 0, 0), g);
        });
    std::vector<KX_GameObject *> npcs;
    std::vector<std::pair<size_t, MT_Vector3>> spots;
    for (int i = 0; i < 1000; i++) {
      const size_t v = size_t(i % 120);
      const MT_Vector3 local(-4.0f + float((i / 120) % 8), (i % 2) ? 0.8f : -0.8f, 1.0f);
      npcs.push_back(&objects.Add(WorldOf(*vehicles[v], local)));
      spots.emplace_back(v, MT_Vector3(-local.x(), -local.y(), 1.0f));
    }
    double total = 0.0;
    int measured = 0;
    for (int step = 0; step < rounds; step++) {
      for (size_t v = 0; v < vehicles.size(); v++) {
        const float t = float(step) / 60.0f;
        PlaceTurned(*vehicles[v],
                    MT_Vector3(-250.0f + float(v % 20) * 25.0f + 3.0f * t,
                               -250.0f + float(v / 20) * 25.0f, 0),
                    v >= 100 ? 5.0f * t : 0.0f);
      }
      for (size_t i = 0; i < npcs.size(); i++) {
        navigation->Submit(
            GoTo(*npcs[i], WorldOf(*vehicles[spots[i].first], spots[i].second), 1.5f));
      }
      const auto t0 = std::chrono::steady_clock::now();
      navigation->Step(1.0f / 60.0f, helpers > 0 ? &pool : nullptr);
      const auto t1 = std::chrono::steady_clock::now();
      for (KX_GameObject *npc : npcs) {
        npc->NodeUpdateGS(0.0);
      }
      if (step >= 60) {
        total += std::chrono::duration<double, std::milli>(t1 - t0).count();
        measured++;
      }
    }
    int aboard = 0;
    for (size_t i = 0; i < npcs.size(); i++) {
      aboard += navigation->AgentFrame(npcs[i]) == vehicles[spots[i].first] ? 1 : 0;
    }
    printf("120 vehicles, 1,000 NPCs aboard (%d on their own), %u helpers: %.2f ms a step\n",
           aboard, helpers, total / std::max(measured, 1));
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vehicle follow-ups (Navigation_Settings_And_Vehicles_Plan.md E2-E6, 2026-11-20)
 * \{ */

/** A ship with two decks: the lower at z 1 (its own space), an upper deck at z 3 over its bow
 * half (x 0..6). */
Level TwoDeckShip()
{
  Level ship;
  ship.boxes.push_back({MT_Vector3(-6, -2, -1), MT_Vector3(6, 2, 1)});
  ship.boxes.push_back({MT_Vector3(0, -2, 1), MT_Vector3(6, 2, 3)});
  return ship;
}

/** E2: a ladder between a sailing, turning ship's two decks (both ends children of the ship): an
 * NPC on the lower deck climbs it to a place on the upper deck, carried in the ship's space
 * (never left behind the moving ladder); a twin ship with its own ladder at the same place
 * shares the first's navmesh (one type), its NPC crossing its own ladder (Link Object). */
TEST(KX_NavVehicles, AnNpcClimbsBetweenTheDecksOfASailingShip)
{
  Level sea;
  sea.floors.push_back({MT_Vector3(-80, -80, -10), MT_Vector3(80, 80, -10)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(sea, TestSettings());
  Objects objects;
  KX_GameObject &ship = objects.Add(MT_Vector3(0, 0, 0));
  KX_GameObject &twin = objects.Add(MT_Vector3(0, 30, 0));
  const Level shape = TwoDeckShip();
  navigation->SetVehiclesForTests({&ship, &twin});
  navigation->SetVehicleGeometryForTests([&shape](KX_GameObject *, KX_NavGeometry &g) {
    shape.Collect(MT_Vector3(0, 0, 0), MT_Vector3(0, 0, 0), g);
  });
  std::vector<KX_NavLink> links;
  KX_GameObject *ladders[2];
  for (KX_GameObject *vessel : {&ship, &twin}) {
    KX_GameObject &foot = AddChildObject(objects, *vessel, MT_Vector3(-0.7f, 0, 1));
    KX_GameObject &top = AddChildObject(objects, *vessel, MT_Vector3(0.7f, 0, 3));
    KX_NavLink ladder;
    ladder.object = &foot;
    ladder.end_object = &top;
    ladder.start = foot.NodeGetWorldPosition();
    ladder.end = top.NodeGetWorldPosition();
    ladder.kind = "ladder";
    ladder.duration = 1.0f;
    ladder.user_id = uint32_t(links.size() + 1);
    links.push_back(ladder);
    ladders[vessel == &ship ? 0 : 1] = &foot;
  }
  navigation->SetLinksForTests(links);
  /* Cargo on the first ship's lower deck (its own navmesh copy: the links go with it). */
  KX_GameObject &cargo = AddChildObject(objects, ship, MT_Vector3(-4.0f, 1.6f, 1.9f));
  navigation->SetObstacle(&cargo, true);
  KX_GameObject &npc = objects.Add(WorldOf(ship, MT_Vector3(-4.5f, -0.5f, 1.0f)));
  KX_GameObject &twin_npc = objects.Add(WorldOf(twin, MT_Vector3(-4.5f, -0.5f, 1.0f)));
  const MT_Vector3 upper(4.0f, 0.5f, 3.0f);
  float yaw = 0.0f;
  MT_Vector3 place(0, 0, 0);
  KX_GameObject *crossed[2] = {nullptr, nullptr};
  bool arrived[2] = {false, false};
  float worst = 0.0f;
  for (int step = 0; step < 60 * 20 && !(arrived[0] && arrived[1]); step++) {
    place += MT_Vector3(std::cos(yaw * float(M_PI) / 180.0f),
                        std::sin(yaw * float(M_PI) / 180.0f), 0) *
             0.05f;
    yaw += 0.3f;
    PlaceTurned(ship, place, yaw);
    PlaceTurned(twin, place + MT_Vector3(0, 30, 0), yaw);
    navigation->Submit(GoTo(npc, WorldOf(ship, upper), 2.0f));
    navigation->Submit(GoTo(twin_npc, WorldOf(twin, upper), 2.0f));
    navigation->Step(1.0f / 60.0f, nullptr);
    for (int k = 0; k < 2; k++) {
      KX_GameObject &who = k == 0 ? npc : twin_npc;
      KX_GameObject &vessel = k == 0 ? ship : twin;
      who.NodeUpdateGS(0.0);
      KX_NavAgentInfo info;
      ASSERT_TRUE(navigation->GetAgentInfo(&who, info));
      if (info.on_link) {
        crossed[k] = info.link_object;
        /* Carried in the ship's space: always near the ladder (x -0.7 .. 0.7). */
        const MT_Vector3 local = LocalOf(vessel, who.NodeGetWorldPosition());
        worst = std::max(worst, std::fabs(float(local.x())) - 0.7f);
      }
      arrived[k] = info.arrived;
    }
  }
  EXPECT_TRUE(arrived[0]);
  EXPECT_TRUE(arrived[1]);
  EXPECT_EQ(crossed[0], ladders[0]);
  EXPECT_EQ(crossed[1], ladders[1]);
  EXPECT_LT(worst, 0.3f);
  for (KX_GameObject *who : {&npc, &twin_npc}) {
    const MT_Vector3 local = LocalOf(who == &npc ? ship : twin, who->NodeGetWorldPosition());
    EXPECT_NEAR(float(local.z()), 3.0f, 0.2f);
    EXPECT_NEAR(float(local.x()), 4.0f, 0.5f);
  }
  /* The shared shape and the first ship's own copy (its cargo). */
  EXPECT_EQ(navigation->VehicleTypeCountForTests(), 2u);
}

/** E3: crates standing on a sailing ship's deck (one parented to it, Navigation > Obstacle)
 * are carved into that ship's own navmesh (a private copy: its twin keeps the shared one): the
 * NPC walks round them, never through; Rebuild Area round a docked ship reads its shape again
 * (a ramp folded down: the new shape's navmesh reaches it). */
TEST(KX_NavVehicles, CratesOnADeckAreWalkedRoundAndARampIsReadAgain)
{
  Level sea;
  sea.floors.push_back({MT_Vector3(-80, -80, -10), MT_Vector3(80, 80, -10)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(sea, TestSettings());
  Objects objects;
  KX_GameObject &ship = objects.Add(MT_Vector3(0, 0, 0));
  KX_GameObject &twin = objects.Add(MT_Vector3(0, 30, 0));
  bool ramp_down = false;
  navigation->SetVehiclesForTests({&ship, &twin});
  navigation->SetVehicleGeometryForTests([&](KX_GameObject *vehicle, KX_NavGeometry &g) {
    /* (A 6 m wide deck: the 2 m crate leaves a way on each side.) */
    Level shape;
    shape.boxes.push_back({MT_Vector3(-6, -3, -1), MT_Vector3(6, 3, 1)});
    if (ramp_down && vehicle == &ship) {
      shape.floors.push_back({MT_Vector3(6, -1, 1), MT_Vector3(10, 1, 1)});
    }
    shape.Collect(MT_Vector3(0, 0, 0), MT_Vector3(0, 0, 0), g);
  });
  /* A 2 m crate in the deck's middle (1 m beside it each side: walkable). */
  KX_GameObject &crate = AddChildObject(objects, ship, MT_Vector3(0, 0, 1.9f));
  navigation->SetObstacle(&crate, true);
  KX_GameObject &npc = objects.Add(WorldOf(ship, MT_Vector3(-5, 0, 1)));
  bool through = false, arrived = false;
  float x = 0.0f;
  for (int step = 0; step < 60 * 20 && !arrived; step++) {
    x += 2.0f / 60.0f;
    PlaceTurned(ship, MT_Vector3(x, 0, 0), 0.0f);
    PlaceTurned(twin, MT_Vector3(x, 30, 0), 0.0f);
    navigation->Submit(GoTo(npc, WorldOf(ship, MT_Vector3(5, 0, 1)), 2.0f));
    navigation->Step(1.0f / 60.0f, nullptr);
    npc.NodeUpdateGS(0.0);
    const MT_Vector3 local = LocalOf(ship, npc.NodeGetWorldPosition());
    through = through || (std::fabs(float(local.x())) < 0.8f && std::fabs(float(local.y())) < 0.8f);
    KX_NavAgentInfo info;
    ASSERT_TRUE(navigation->GetAgentInfo(&npc, info));
    arrived = info.arrived;
  }
  EXPECT_TRUE(arrived);
  EXPECT_FALSE(through);
  /* The shared shape and the ship's own copy. */
  EXPECT_EQ(navigation->VehicleTypeCountForTests(), 2u);
  /* The twin's deck middle stays walkable (its navmesh is the shared one, uncarved). */
  KX_NavAgentSize size;
  KX_NavMesh *twin_mesh = navigation->VehicleNavMeshFor(&twin, size);
  ASSERT_NE(twin_mesh, nullptr);
  MT_Vector3 point;
  ASSERT_TRUE(twin_mesh->NearestPoint(WorldOf(twin, MT_Vector3(0, 0, 1)), point));
  EXPECT_LT(distance_2d(point, WorldOf(twin, MT_Vector3(0, 0, 1))), 0.1f);
  /* The ramp folded down, Rebuild Area round the ship: its navmesh reaches the ramp's end. */
  ramp_down = true;
  KX_NavRequest rebuild;
  rebuild.object = &ship;
  rebuild.command = KX_NavRequest::Command::RebuildArea;
  rebuild.destination = ship.NodeGetWorldPosition();
  rebuild.offset = MT_Vector3(10, 10, 5);
  navigation->Submit(rebuild);
  for (int step = 0; step < 10; step++) {
    navigation->Step(1.0f / 60.0f, nullptr);
  }
  KX_NavMesh *ship_mesh = navigation->VehicleNavMeshFor(&ship, size);
  ASSERT_NE(ship_mesh, nullptr);
  ASSERT_TRUE(ship_mesh->NearestPoint(WorldOf(ship, MT_Vector3(9, 0, 1)), point));
  EXPECT_LT(distance_2d(point, WorldOf(ship, MT_Vector3(9, 0, 1))), 0.4f);
}

/** E4: ten NPCs boarding a docked ship while ten leave it by one gangway: they avoid each other
 * across the gangway's two frames (near it), all get through, hardly ever overlapping. */
TEST(KX_NavVehicles, CrowdsBoardAndLeaveByOneGangway)
{
  Level dock;
  dock.floors.push_back({MT_Vector3(-20, -10, 0), MT_Vector3(0, 10, 0)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(dock, TestSettings());
  Objects objects;
  KX_GameObject &ship = objects.Add(MT_Vector3(7, 0, -1));
  const Level shape = ShipShape();
  navigation->SetVehiclesForTests({&ship});
  navigation->SetVehicleGeometryForTests([&shape](KX_GameObject *, KX_NavGeometry &g) {
    shape.Collect(MT_Vector3(0, 0, 0), MT_Vector3(0, 0, 0), g);
  });
  KX_GameObject &shore_end = objects.Add(MT_Vector3(-0.6f, 0, 0));
  KX_GameObject &ship_end = AddChildObject(objects, ship, MT_Vector3(-5.4f, 0, 1));
  KX_NavLink gangway;
  gangway.object = &shore_end;
  gangway.end_object = &ship_end;
  gangway.kind = "gangway";
  gangway.reach = 2.5f;
  gangway.user_id = 1;
  navigation->SetLinksForTests({gangway});
  std::vector<KX_GameObject *> npcs;
  std::vector<MT_Vector3> goals;
  for (int i = 0; i < 10; i++) {
    npcs.push_back(&objects.Add(MT_Vector3(-6.0f - (i % 5) * 1.2f, -2.4f + (i / 5) * 4.8f, 0)));
    goals.push_back(WorldOf(ship, MT_Vector3(1.0f + (i % 5) * 1.0f, -1.2f + (i / 5) * 2.4f, 1)));
  }
  for (int i = 0; i < 10; i++) {
    npcs.push_back(&objects.Add(
        WorldOf(ship, MT_Vector3(-3.0f + (i % 5) * 1.0f, -1.2f + (i / 5) * 2.4f, 1))));
    goals.push_back(MT_Vector3(-6.0f - (i % 5) * 1.2f, -6.0f + (i / 5) * 12.0f, 0));
  }
  int arrived = 0, deep = 0;
  for (int step = 0; step < 60 * 60 && arrived < 20; step++) {
    for (size_t i = 0; i < npcs.size(); i++) {
      KX_NavRequest request = GoTo(*npcs[i], goals[i], 2.0f);
      request.arrive_distance = 0.5f;
      navigation->Submit(request);
    }
    navigation->Step(1.0f / 60.0f, nullptr);
    arrived = 0;
    for (size_t i = 0; i < npcs.size(); i++) {
      KX_NavAgentInfo info;
      navigation->GetAgentInfo(npcs[i], info);
      arrived += info.arrived ? 1 : 0;
      for (size_t j = i + 1; j < npcs.size(); j++) {
        KX_NavAgentInfo other;
        navigation->GetAgentInfo(npcs[j], other);
        const float d = distance_2d(npcs[i]->NodeGetWorldPosition(),
                                    npcs[j]->NodeGetWorldPosition());
        if (!info.on_link && !other.on_link &&
            std::fabs(float(npcs[i]->NodeGetWorldPosition().z() -
                            npcs[j]->NodeGetWorldPosition().z())) < 1.5f &&
            d < 0.5f)
        {
          deep++;
        }
      }
    }
  }
  printf("20 NPCs by one gangway: %d arrived, %d deep overlaps (pair x step)\n", arrived, deep);

  EXPECT_EQ(arrived, 20);
  EXPECT_LT(deep, 60);
}

/** E6: a ship sailing through a flyer's way at its height: the flyer goes round the hull, never
 * through it. */
TEST(KX_NavVehicles, AFlyerAvoidsASailingShip)
{
  Level sea;
  sea.floors.push_back({MT_Vector3(-30, -30, -1), MT_Vector3(30, 30, -1)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(sea, TestSettings());
  Objects objects;
  KX_GameObject &ship = objects.Add(MT_Vector3(-20, 0, 1));
  const Level shape = ShipShape();
  navigation->SetVehiclesForTests({&ship});
  navigation->SetVehicleGeometryForTests([&shape](KX_GameObject *, KX_NavGeometry &g) {
    shape.Collect(MT_Vector3(0, 0, 0), MT_Vector3(0, 0, 0), g);
  });
  KX_GameObject &bird = objects.Add(MT_Vector3(0, -12, 1));
  bool inside = false, arrived = false;
  float x = -20.0f;
  for (int step = 0; step < 60 * 20 && !arrived; step++) {
    x += 3.0f / 60.0f;
    PlaceTurned(ship, MT_Vector3(x, 0, 1), 0.0f);
    navigation->Submit(FlyTo(bird, MT_Vector3(0, 12, 1), 3.0f));
    navigation->Step(1.0f / 60.0f, nullptr);
    const MT_Vector3 local = LocalOf(ship, bird.NodeGetWorldPosition());
    const bool now_inside = std::fabs(float(local.x())) < 6.0f && std::fabs(float(local.y())) < 2.0f &&
                        std::fabs(float(local.z())) < 1.0f;
    inside = inside || now_inside;
    KX_NavAgentInfo info;
    ASSERT_TRUE(navigation->GetAgentInfo(&bird, info));
    arrived = info.arrived;
  }
  EXPECT_TRUE(arrived);
  EXPECT_FALSE(inside);
}

/** A ship (ShipShape: deck 12 x 4 m, its top 1 m over its origin) sailing east at 3 m/s and
 * turning 6 degrees a second; a bird (6 m/s) 12 m north of its start, and a perch, an Empty on the deck
 * 2 m forward of its middle at a bird's height over it, carried with the ship. */
struct SailingShip {
  Level sea;
  std::unique_ptr<KX_Navigation> navigation;
  Objects objects;
  Level shape = ShipShape();
  KX_GameObject *ship = nullptr, *perch = nullptr, *bird = nullptr;
  float x = -10.0f, yaw = 0.0f;

  SailingShip()
  {
    /* (The sea wide enough for 80 s of sailing: beyond its box the air map is solid.) */
    sea.floors.push_back({MT_Vector3(-60, -120, -1), MT_Vector3(260, 120, -1)});
    navigation = MakeNavigation(sea, TestSettings());
    ship = &objects.Add(MT_Vector3(x, 0, 1));
    navigation->SetVehiclesForTests({ship});
    navigation->SetVehicleGeometryForTests([this](KX_GameObject *, KX_NavGeometry &g) {
      shape.Collect(MT_Vector3(0, 0, 0), MT_Vector3(0, 0, 0), g);
    });
    perch = &objects.Add(MT_Vector3(0, 0, 0));
    bird = &objects.Add(MT_Vector3(-10, 12, 3));
    Sail(0.0f);
  }
  /** The ship and its perch moved on by `dt`. */
  void Sail(const float dt)
  {
    x += 3.0f * dt;
    yaw += 6.0f * dt;
    PlaceTurned(*ship, MT_Vector3(x, 0, 1), yaw);
    /* (The bird's middle a body over the deck: TestSettings' height 1.8, a radius of 0.9.) */
    PlaceTurned(*perch, WorldOf(*ship, MT_Vector3(2, 0, 1.9f)), yaw);
  }
  /** Inside the hull's box (below the deck's top, a little in from its sides). */
  bool InHull(const MT_Vector3 &p) const
  {
    const MT_Vector3 local = LocalOf(*ship, p);
    return std::fabs(float(local.x())) < 5.8f && std::fabs(float(local.y())) < 1.8f &&
           float(local.z()) < 0.95f && float(local.z()) > -1.0f;
  }
};

/** Plan item 11: a bird after a perch on a sailing ship's deck comes down on it from above
 * (never through the hull's sides) and rides it 20 s (the ship turning, the bird on its perch,
 * Get Navigation Info's Vehicle the ship); then a goal ashore: it takes off straight up out of
 * the hull, flies there and arrives (Vehicle none). */
TEST(KX_NavVehicles, AFlyerLandsOnASailingShipAndRides)
{
  SailingShip scene;
  KX_Navigation &navigation = *scene.navigation;
  const float dt = 1.0f / 60.0f;
  bool landed = false, in_hull = false;
  int step = 0;
  for (; step < 60 * 30 && !landed; step++) {
    scene.Sail(dt);
    KX_NavRequest request = FlyTo(*scene.bird, MT_Vector3(0, 0, 0), 6.0f);
    request.target = scene.perch;
    navigation.Submit(request);
    navigation.Step(dt, nullptr);
    in_hull = in_hull || scene.InHull(scene.bird->NodeGetWorldPosition());
    KX_NavAgentInfo info;
    ASSERT_TRUE(navigation.GetAgentInfo(scene.bird, info));
    landed = info.arrived && info.vehicle == scene.ship;
  }
  ASSERT_TRUE(landed) << "after " << step << " steps";
  EXPECT_FALSE(in_hull);
  /* Riding 20 s: on its perch however the ship turns. */
  float farthest = 0.0f;
  int reached = 0;
  for (int i = 0; i < 60 * 20; i++) {
    scene.Sail(dt);
    /* (Measured as the step starts: the bird is drawn ahead with the deck at a step's end.) */
    farthest = std::max(farthest,
                        float((scene.bird->NodeGetWorldPosition() -
                               scene.perch->NodeGetWorldPosition())
                                  .length()));
    KX_NavRequest request = FlyTo(*scene.bird, MT_Vector3(0, 0, 0), 6.0f);
    request.target = scene.perch;
    navigation.Submit(request);
    navigation.Step(dt, nullptr);
    KX_NavAgentInfo info;
    ASSERT_TRUE(navigation.GetAgentInfo(scene.bird, info));
    ASSERT_EQ(info.vehicle, scene.ship) << i;
    EXPECT_TRUE(info.arrived) << i;
    reached += info.reached ? 1 : 0;
  }
  EXPECT_LT(farthest, 0.35f);
  /* (Carried, it stays arrived: When Reached pulsed at the landing only.) */
  EXPECT_EQ(reached, 0);
  /* A goal ashore, north of the ship: straight up first (over the hull's spheres before it
   * leaves its place over the deck), never out through the hull. */
  const MT_Vector3 ashore(scene.x, 14, 2);
  const MT_Vector3 sat = LocalOf(*scene.ship, scene.bird->NodeGetWorldPosition());
  bool arrived = false, climbed_first = true, over = false;
  for (int i = 0; i < 60 * 20 && !arrived; i++) {
    scene.Sail(dt);
    navigation.Submit(FlyTo(*scene.bird, ashore, 6.0f));
    navigation.Step(dt, nullptr);
    const MT_Vector3 local = LocalOf(*scene.ship, scene.bird->NodeGetWorldPosition());
    over = over || float(local.z()) > 3.0f;
    if (!over && std::hypot(float(local.x() - sat.x()), float(local.y() - sat.y())) > 0.5f) {
      climbed_first = false;
    }
    in_hull = in_hull || scene.InHull(scene.bird->NodeGetWorldPosition());
    KX_NavAgentInfo info;
    ASSERT_TRUE(navigation.GetAgentInfo(scene.bird, info));
    arrived = info.arrived;
    if (arrived) {
      EXPECT_EQ(info.vehicle, nullptr);
    }
  }
  EXPECT_TRUE(arrived);
  EXPECT_TRUE(climbed_first);
  EXPECT_FALSE(in_hull);
}

/** Plan item 11's safeguard: a point 4 m above the deck (no flyer's height over it) is followed
 * by flying as before: the bird keeps up with it, never rides, never enters the hull. */
TEST(KX_NavVehicles, AFlyerFollowingAPointAboveTheDeckStillHovers)
{
  SailingShip scene;
  KX_Navigation &navigation = *scene.navigation;
  KX_GameObject &mark = scene.objects.Add(MT_Vector3(0, 0, 0));
  const float dt = 1.0f / 60.0f;
  bool in_hull = false, near = false;
  for (int step = 0; step < 60 * 30; step++) {
    scene.Sail(dt);
    mark.NodeSetWorldPosition(WorldOf(*scene.ship, MT_Vector3(0, 0, 5)));
    mark.NodeUpdateGS(0.0);
    KX_NavRequest request = FlyTo(*scene.bird, MT_Vector3(0, 0, 0), 6.0f);
    request.target = &mark;
    navigation.Submit(request);
    navigation.Step(dt, nullptr);
    in_hull = in_hull || scene.InHull(scene.bird->NodeGetWorldPosition());
    KX_NavAgentInfo info;
    ASSERT_TRUE(navigation.GetAgentInfo(scene.bird, info));
    ASSERT_EQ(info.vehicle, nullptr) << step;
    if (step > 60 * 20) {
      near = float((scene.bird->NodeGetWorldPosition() - mark.NodeGetWorldPosition()).length()) <
             1.5f;
    }
  }
  EXPECT_TRUE(near);
  EXPECT_FALSE(in_hull);
}

/** \} */

/** E5: a train car (Kinematic, 4 and 8 m/s) runs past a platform; a jump link (both ways) from
 * the platform's edge to the car's near side, open while their ends are within 4 m and nearing
 * (Jumps: Physics). A Character on the platform with a goal on the car jumps on (the throw
 * aimed where the car will be, steered with it in the air: at 8 m/s the car outruns any chase),
 * a Character riding the car with a goal on the platform jumps off: both land where they meant
 * to, the boarder near the car's door spot. */
TEST(KX_NavVehicles, CharactersJumpOntoAMovingTrainAndOff)
{
  for (const float car_speed : {4.0f, 8.0f}) {
  SCOPED_TRACE(car_speed);
  Level platform;
  platform.floors.push_back({MT_Vector3(-40, -8, 0), MT_Vector3(40, -1, 0)});
  KX_Navigation::Settings settings = TestSettings();
  settings.physics_jumps = true;
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(platform, settings);
  Objects objects;
  World world;
  world.StaticBox(objects.Add(MT_Vector3(0, -4.5f, -0.5f)), MT_Vector3(40, 3.5f, 0.5f));
  KX_GameObject &car = objects.Add(MT_Vector3(-22, 2.0f, 0.25f));
  Body &body = world.Add(
      car, new JPH::BoxShape(JPH::Vec3(4.0f, 0.75f, 1.5f), 0.0f), JPH::EMotionType::Kinematic);
  body.controller.SetMotionState(new KX_MotionState(car.GetSGNode()));
  world.environment.AddController(&body.controller);
  world.environment.GetPhysicsSystem()->OptimizeBroadPhase();
  navigation->SetPhysicsEnvironmentForTests(&world.environment);
  Level car_shape;
  car_shape.boxes.push_back({MT_Vector3(-4, -1.5f, -0.75f), MT_Vector3(4, 1.5f, 0.75f)});
  navigation->SetVehiclesForTests({&car});
  navigation->SetVehicleGeometryForTests([&car_shape](KX_GameObject *, KX_NavGeometry &g) {
    car_shape.Collect(MT_Vector3(0, 0, 0), MT_Vector3(0, 0, 0), g);
  });
  KX_GameObject &edge = objects.Add(MT_Vector3(0, -1.4f, 0));
  KX_GameObject &door = AddChildObject(objects, car, MT_Vector3(0, -1.1f, 0.75f));
  KX_NavLink jump;
  jump.object = &edge;
  jump.end_object = &door;
  jump.kind = "jump";
  jump.reach = 4.0f;
  jump.user_id = 1;
  navigation->SetLinksForTests({jump});
  KX_GameObject &boarder = objects.Add(MT_Vector3(-3, -4, 0.95f));
  KX_GameObject &leaver = objects.Add(WorldOf(car, MT_Vector3(2.5f, 0.5f, 1.7f)));
  for (KX_GameObject *npc : {&boarder, &leaver}) {
    JoltCharacter *character = world.Character(*npc);
    ASSERT_NE(character, nullptr);
    character->SetJumpSpeed(7.0f);
  }
  const MT_Vector3 seat(2.5f, 0.5f, 0.75f), bench(3, -5, 0);
  bool boarder_on = false, leaver_off = false;
  float boarder_miss = -1.0f;
  int boarder_jumps = 0, leaver_jumps = 0;
  for (int step = 0; step < 60 * 14; step++) {
    if (step >= 60) {
      car.NodeSetWorldPosition(car.NodeGetWorldPosition() +
                               MT_Vector3(car_speed / 60.0f, 0, 0));
      car.NodeUpdateGS(0.0);
    }
    KX_NavRequest board = GoTo(boarder, WorldOf(car, seat), 3.0f);
    KX_NavRequest leave = GoTo(leaver, bench, 3.0f);
    board.until_arrived = leave.until_arrived = false;
    navigation->Submit(board);
    navigation->Submit(leave);
    navigation->Step(1.0f / 60.0f, nullptr);
    world.Step({&car, &boarder, &leaver});
    KX_NavAgentInfo a, b;
    navigation->GetAgentInfo(&boarder, a);
    navigation->GetAgentInfo(&leaver, b);
    boarder_jumps += a.link_started ? 1 : 0;
    leaver_jumps += b.link_started ? 1 : 0;
    if (a.link_ended && a.link_landed && !boarder_on) {
      /* How near the car's door spot it came down (the car moved ~2.5 m in its flight). */
      boarder_miss = distance_2d(boarder.NodeGetWorldPosition(), door.NodeGetWorldPosition());
    }
    boarder_on = boarder_on || (a.link_ended && a.link_landed);
    leaver_off = leaver_off || (b.link_ended && b.link_landed);
  }
  printf("train %.0f m/s: boarder jumps %d, landed on %d (%.2f m from the door spot); leaver "
         "jumps %d, landed off %d\n",
         car_speed, boarder_jumps, boarder_on, boarder_miss, leaver_jumps, leaver_off);
  EXPECT_TRUE(boarder_on);
  EXPECT_LT(boarder_miss, 0.6f);
  EXPECT_TRUE(leaver_off);
  const MT_Vector3 on_car = LocalOf(car, boarder.NodeGetWorldPosition());
  EXPECT_LT(std::fabs(float(on_car.y())), 1.6f);
  EXPECT_GT(float(on_car.z()), 0.5f);
  EXPECT_EQ(navigation->AgentFrame(&boarder), &car);
  EXPECT_LT(float(leaver.NodeGetWorldPosition().y()), -1.0f);
  EXPECT_LT(float(leaver.NodeGetWorldPosition().z()), 1.5f);
  }
}

/** E7: the Visualization draws what it is asked to: the navmesh's walls and its areas' edges in
 * their colours, links by kind and state (a ladder yellow, a closed Wait link red, a closed Go
 * Round link grey), NPCs and their ways, carved obstacles; each category left out on request;
 * only within its range of the camera; never more lines than its budget (the NPCs first). */
TEST(KX_Navigation, TheVisualizationDrawsWhatItShows)
{
  struct Seen {
    std::map<std::tuple<int, int, int>, int> colors;
    size_t lines = 0;
    float farthest = 0.0f;
  };
  auto key = [](const MT_Vector4 &c) {
    return std::make_tuple(int(std::lround(float(c[0]) * 100.0f)),
                           int(std::lround(float(c[1]) * 100.0f)),
                           int(std::lround(float(c[2]) * 100.0f)));
  };
  auto run = [&](const int hide, const float range, const int budget) {
    Level level;
    level.floors.push_back({MT_Vector3(-20, -10, 0), MT_Vector3(-1, 10, 0)});
    level.floors.push_back({MT_Vector3(1, -10, 0), MT_Vector3(60, 10, 0)});
    /* (Pillars far east: many walls, past the budget.) */
    for (int i = 0; i < 12; i++) {
      for (int j = 0; j < 5; j++) {
        const MT_Vector3 c(25.0f + float(i) * 3.0f, -8.0f + float(j) * 4.0f, 0.0f);
        level.boxes.push_back({c - MT_Vector3(0.4f, 0.4f, 0.0f), c + MT_Vector3(0.4f, 0.4f, 2.0f)});
      }
    }
    KX_Navigation::Settings settings = TestSettings();
    settings.visualize = true;
    settings.vis_hide = hide;
    settings.vis_range = range;
    settings.vis_max_lines = budget;
    std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, settings);
    level.floors[1].area = navigation->AreaId("water");
    navigation->SetCameraForTests(MT_Vector3(0, 0, 10));
    Seen seen;
    navigation->SetDebugLineSinkForTests(
        [&seen, &key](const MT_Vector3 &a, const MT_Vector3 &b, const MT_Vector4 &color) {
          seen.colors[key(color)]++;
          seen.lines++;
          seen.farthest = std::max({seen.farthest, float(a.x()), float(b.x())});
        });
    Objects objects;
    KX_GameObject &ladder = objects.Add(MT_Vector3(-1.5f, 3, 0));
    KX_GameObject &door = objects.Add(MT_Vector3(-1.5f, -3, 0));
    KX_GameObject &gate = objects.Add(MT_Vector3(-1.5f, -7, 0));
    KX_NavLink a, b, c;
    a.object = &ladder;
    a.start = MT_Vector3(-1.5f, 3, 0);
    a.end = MT_Vector3(1.5f, 3, 0);
    a.kind = "ladder";
    a.user_id = 1;
    b = a;
    b.object = &door;
    b.start = MT_Vector3(-1.5f, -3, 0);
    b.end = MT_Vector3(1.5f, -3, 0);
    b.kind = "door";
    b.enabled = false;
    b.wait = true;
    b.user_id = 2;
    c = b;
    c.object = &gate;
    c.start = MT_Vector3(-1.5f, -7, 0);
    c.end = MT_Vector3(1.5f, -7, 0);
    c.wait = false;
    c.user_id = 3;
    navigation->SetLinksForTests({a, b, c});
    KX_GameObject &npc = objects.Add(MT_Vector3(-10, 0, 0));
    KX_GameObject &crate = objects.Add(MT_Vector3(-12, 5, 0));
    navigation->SetObstacle(&crate, true);
    for (int step = 0; step < 30; step++) {
      seen = Seen();
      navigation->Submit(GoTo(npc, MT_Vector3(10, 0, 0)));
      navigation->Step(1.0f / 60.0f, nullptr);
    }
    return seen;
  };
  auto has = [&](const Seen &seen, const MT_Vector4 &color) {
    return seen.colors.count(key(color)) != 0;
  };
  const MT_Vector4 wall(0.0f, 1.0f, 1.0f, 1.0f), ladder(1.0f, 0.95f, 0.2f, 1.0f),
      wait(1.0f, 0.1f, 0.1f, 1.0f), closed(0.5f, 0.5f, 0.5f, 1.0f),
      agent(0.2f, 0.8f, 1.0f, 1.0f), path(0.3f, 1.0f, 0.3f, 1.0f),
      carve(1.0f, 0.15f, 0.1f, 1.0f), ground = MT_Vector4(0.15f, 0.45f, 0.8f, 1.0f) * 0.6f;
  const Seen all = run(0, 0.0f, 100000);
  EXPECT_TRUE(has(all, wall));
  EXPECT_TRUE(has(all, ground));
  EXPECT_TRUE(has(all, ladder));
  EXPECT_TRUE(has(all, wait));
  EXPECT_TRUE(has(all, closed));
  EXPECT_TRUE(has(all, agent));
  EXPECT_TRUE(has(all, path));
  EXPECT_TRUE(has(all, carve));
  /* The water's edges: a colour of their own (none of the known ones). */
  int water_lines = 0;
  for (const auto &[color, count] : all.colors) {
    bool known = false;
    for (const MT_Vector4 &k : {wall, ground, ladder, wait, closed, agent, path, carve}) {
      known = known || color == key(k);
    }
    water_lines = known ? water_lines : std::max(water_lines, count);
  }
  EXPECT_GT(water_lines, 20);
  EXPECT_GT(all.farthest, 50.0f);
  /* Each category left out. */
  const Seen no_mesh = run(blender::NAVMESH_VIS_HIDE_NAVMESH, 0.0f, 100000);
  EXPECT_FALSE(has(no_mesh, wall));
  EXPECT_TRUE(has(no_mesh, ladder));
  const Seen no_links = run(blender::NAVMESH_VIS_HIDE_LINKS, 0.0f, 100000);
  EXPECT_FALSE(has(no_links, ladder));
  EXPECT_FALSE(has(no_links, wait));
  EXPECT_TRUE(has(no_links, wall));
  const Seen no_agents = run(blender::NAVMESH_VIS_HIDE_AGENTS | blender::NAVMESH_VIS_HIDE_PATHS |
                                 blender::NAVMESH_VIS_HIDE_OBSTACLES,
                             0.0f,
                             100000);
  EXPECT_FALSE(has(no_agents, agent));
  EXPECT_FALSE(has(no_agents, path));
  EXPECT_FALSE(has(no_agents, carve));
  /* Within 20 m of the camera only: nothing of the far end. */
  const Seen near_only = run(0, 20.0f, 100000);
  EXPECT_LT(near_only.farthest, 40.0f);
  EXPECT_LT(near_only.lines, all.lines);
  /* A budget: never more lines; the NPC still drawn. */
  EXPECT_GT(all.lines, 500u);
  const Seen capped = run(0, 0.0f, 300);
  EXPECT_LE(capped.lines, 300u);
  EXPECT_TRUE(has(capped, agent));
}

/* -------------------------------------------------------------------- */
/** \name Flyers: the rubble and the decks (plan item 11, 2026-11-20)
 * \{ */

/** A yard with a heap of rubble across it: four Obstacle boxes (2 m wide, 3 m high, 1 m deep)
 * side by side, x -4..4 at y 0, a bird 8 m south of them. */
struct RubbleYard {
  Level level;
  std::unique_ptr<KX_Navigation> navigation;
  Objects objects;
  World world;
  std::vector<KX_GameObject *> rubble;
  KX_GameObject *bird = nullptr;

  RubbleYard()
  {
    level.floors.push_back({MT_Vector3(-15, -15, 0), MT_Vector3(15, 15, 0)});
    navigation = MakeNavigation(level, TestSettings());
    for (int i = 0; i < 4; i++) {
      KX_GameObject &piece = objects.Add(MT_Vector3(-3.0f + 2.0f * float(i), 0, 1.5f));
      /* (Jolt's box: x, up, depth.) */
      world.Add(piece, new JPH::BoxShape(JPH::Vec3(1.0f, 1.5f, 0.5f), 0.0f),
                JPH::EMotionType::Kinematic);
      navigation->SetObstacle(&piece, true);
      rubble.push_back(&piece);
    }
    navigation->SetPhysicsEnvironmentForTests(&world.environment);
    bird = &objects.Add(MT_Vector3(0, -8, 1.5f));
  }
  /** Whether `p` lies in a piece of rubble grown by `grow`. */
  bool InRubble(const MT_Vector3 &p, const float grow) const
  {
    for (const KX_GameObject *piece : rubble) {
      const MT_Vector3 d = p - piece->NodeGetWorldPosition();
      if (std::fabs(float(d[0])) < 1.0f + grow && std::fabs(float(d[1])) < 0.5f + grow &&
          std::fabs(float(d[2])) < 1.5f + grow)
      {
        return true;
      }
    }
    return false;
  }
};

/** Item 11: resting rubble is solid in the air map: the bird's way to a goal behind it goes
 * round or over it (its next corner off the straight line), never into it, and it arrives.
 * Before the heap comes to rest (kAirSolidSteps) the air between is free. */
TEST(KX_Navigation, AFlyerPlansRoundRestingRubble)
{
  RubbleYard yard;
  KX_Navigation &navigation = *yard.navigation;
  const MT_Vector3 goal(0, 8, 1.5f);
  const MT_Vector3 start = yard.bird->NodeGetWorldPosition();
  /* (The air map is made at the first flyer's first step.) */
  navigation.Submit(FlyTo(*yard.bird, start));
  navigation.Step(1.0f / 60.0f, nullptr);
  EXPECT_TRUE(navigation.AirLineClear(start, goal, 0.3f, false));
  for (int step = 0; step < 15; step++) {
    navigation.Submit(FlyTo(*yard.bird, start));
    navigation.Step(1.0f / 60.0f, nullptr);
  }
  EXPECT_FALSE(navigation.AirLineClear(start, goal, 0.3f, false));
  bool bent = false, inside = false, arrived = false;
  for (int step = 0; step < 60 * 20 && !arrived; step++) {
    navigation.Submit(FlyTo(*yard.bird, goal));
    navigation.Step(1.0f / 60.0f, nullptr);
    KX_NavAgentInfo info;
    ASSERT_TRUE(navigation.GetAgentInfo(yard.bird, info));
    if (step == 30) {
      /* Round the heap (beyond its end, x 4, or above its top, 3 m). */
      bent = std::fabs(float(info.next_corner[0])) > 4.0f || float(info.next_corner[2]) > 3.0f;
    }
    inside = inside || yard.InRubble(yard.bird->NodeGetWorldPosition(), 0.0f);
    arrived = info.arrived;
  }
  EXPECT_TRUE(bent);
  EXPECT_FALSE(inside);
  EXPECT_TRUE(arrived);
}

/** Item 11: rubble that moves again (carried off, woken) is free air at once; at rest again
 * elsewhere it is solid there. */
TEST(KX_Navigation, RubbleWakingClearsTheAirMap)
{
  RubbleYard yard;
  KX_Navigation &navigation = *yard.navigation;
  const MT_Vector3 start = yard.bird->NodeGetWorldPosition();
  const MT_Vector3 goal(0, 8, 1.5f);
  auto step = [&](const int count) {
    for (int i = 0; i < count; i++) {
      navigation.Submit(FlyTo(*yard.bird, start));
      navigation.Step(1.0f / 60.0f, nullptr);
    }
  };
  step(20);
  EXPECT_FALSE(navigation.AirLineClear(start, goal, 0.3f, false));
  /* The heap carried 10 m east over a second (each piece moving), then left there. */
  for (int i = 1; i <= 60; i++) {
    for (int k = 0; k < 4; k++) {
      KX_GameObject *piece = yard.rubble[k];
      piece->NodeSetWorldPosition(MT_Vector3(-3.0f + 2.0f * float(k) + 10.0f * float(i) / 60.0f,
                                             0, 1.5f));
      piece->NodeUpdateGS(0.0);
    }
    step(1);
    if (i == 2) {
      /* (Moving: free at once, the step after.) */
      step(1);
      EXPECT_TRUE(navigation.AirLineClear(start, goal, 0.3f, false));
    }
  }
  step(20);
  EXPECT_TRUE(navigation.AirLineClear(start, goal, 0.3f, false));
  EXPECT_FALSE(navigation.AirLineClear(MT_Vector3(10, -8, 1.5f), MT_Vector3(10, 8, 1.5f), 0.3f, false));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Chasers spread round their target (plan item 10, 2026-11-20)
 * \{ */

/** A chase: `count` NPCs starting in a clump 9 m south of a target (an NPC standing at
 * `target_at` that avoids nobody), jittered by `jitter`, each step asked `mode` after it. */
struct ChaseScene {
  Level level;
  std::unique_ptr<KX_Navigation> navigation;
  Objects objects;
  KX_GameObject *target = nullptr;
  MT_Vector3 target_at;
  std::vector<KX_GameObject *> chasers;
  KX_NavRequest::Mode mode = KX_NavRequest::Mode::Pursue;
  bool dense = false;

  ChaseScene(const int count, const int jitter, const MT_Vector3 &at, const float south = -9.0f)
      : target_at(at)
  {
    level.floors.push_back({MT_Vector3(-15, -15, 0), MT_Vector3(15, 15, 0)});
    target = &objects.Add(at);
    uint64_t state = 0x9e3779b97f4a7c15ull * uint64_t(jitter + 1);
    auto random = [&]() {
      state = state * 6364136223846793005ull + 1442695040888963407ull;
      return float(state >> 40) * (1.0f / 16777216.0f) - 0.5f;
    };
    for (int i = 0; i < count; i++) {
      const float jx = jitter ? 0.3f * random() : 0.0f, jy = jitter ? 0.3f * random() : 0.0f;
      chasers.push_back(&objects.Add(
          MT_Vector3(at[0] - 1.5f + 1.0f * float(i % 4) + jx, south - float(i / 4) + jy, 0)));
    }
  }
  void Build()
  {
    navigation = MakeNavigation(level, TestSettings());
  }
  void Step(const int steps)
  {
    for (int step = 0; step < steps; step++) {
      KX_NavRequest stand = GoTo(*target, target_at);
      stand.avoid = false;
      navigation->Submit(stand);
      for (KX_GameObject *chaser : chasers) {
        KX_NavRequest request = GoTo(*chaser, MT_Vector3(0, 0, 0));
        request.mode = mode;
        request.target = target;
        request.dense = dense;
        navigation->Submit(request);
      }
      navigation->Step(1.0f / 60.0f, nullptr);
    }
  }
};

/** The angles round `center` of `points`, sorted; the widest gap between two in a row. */
float WidestGap(const MT_Vector3 &center, const std::vector<MT_Vector3> &points)
{
  std::vector<float> angles;
  for (const MT_Vector3 &p : points) {
    angles.push_back(std::atan2(float(p[1] - center[1]), float(p[0] - center[0])));
  }
  std::sort(angles.begin(), angles.end());
  float widest = 0.0f;
  for (size_t i = 0; i < angles.size(); i++) {
    const float next = i + 1 < angles.size() ? angles[i + 1] : angles[0] + 6.2831853f;
    widest = std::max(widest, next - angles[i]);
  }
  return widest;
}

/** Item 10: eight NPCs pursuing one still target (Polite) end at eight places round it, not
 * queued behind each other on the side they came from: five on the first ring (the bodies a body
 * apart), three on the next, spread; each at its place and arrived, every two at least two radii
 * apart. The target then runs 4 m east: they spread round it again. Three
 * jittered starts (crowds are chaotic). */
TEST(KX_Navigation, ChasersSpreadRoundAStillTarget)
{
  for (const int jitter : {0, 1, 2}) {
    ChaseScene scene(8, jitter, MT_Vector3(0, 0, 0));
    scene.Build();
    for (const MT_Vector3 &move : {MT_Vector3(0, 0, 0), MT_Vector3(4, 0, 0)}) {
      scene.target_at += move;
      for (int i = 0; i < 8 * 60 && move.length2() > 0.0f; i++) {
        /* (Running east at 2 m/s.) */
        scene.target->NodeSetWorldPosition(scene.target_at - move + move * (float(i + 1) / 480.0f));
        scene.target->NodeUpdateGS(0.0);
        scene.Step(1);
      }
      scene.Step(20 * 60);
      const MT_Vector3 center = scene.target->NodeGetWorldPosition();
      std::vector<MT_Vector3> inner, all;
      int outer = 0;
      for (KX_GameObject *chaser : scene.chasers) {
        float angle = 0.0f, radius = 0.0f;
        ASSERT_TRUE(scene.navigation->ChasePlaceOf(chaser, angle, radius)) << jitter;
        const MT_Vector3 at = chaser->NodeGetWorldPosition();
        const MT_Vector3 place = center + MT_Vector3(std::cos(angle), std::sin(angle), 0) * radius;
        EXPECT_LT(distance_2d(at, place), 0.45f)
            << "start " << jitter << " move " << move[0] << ": " << at[0] << " " << at[1]
            << ", place " << place[0] << " " << place[1];
        KX_NavAgentInfo info;
        ASSERT_TRUE(scene.navigation->GetAgentInfo(chaser, info));
        EXPECT_TRUE(info.arrived) << "start " << jitter << " move " << move[0];
        all.push_back(at);
        if (radius < 1.0f) {
          EXPECT_NEAR(radius, 0.75f, 1e-4f);
          inner.push_back(at);
        }
        else {
          EXPECT_NEAR(radius, 1.65f, 1e-4f);
          outer++;
        }
      }
      EXPECT_EQ(inner.size(), 5u) << jitter;
      EXPECT_EQ(outer, 3) << jitter;
      /* Round it: on the first ring every 72 degrees (each arrived within 0.3 m of its place,
       * 23 degrees at 0.75 m: never more than 125 between two; queued on one side, over 180). */
      EXPECT_LT(WidestGap(center, inner), 2.2f) << "start " << jitter << " move " << move[0];
      for (size_t a = 0; a < all.size(); a++) {
        for (size_t b = a + 1; b < all.size(); b++) {
          EXPECT_GT(distance_2d(all[a], all[b]), 0.55f) << "start " << jitter;
        }
      }
    }
  }
}

/** Item 10, found by example 02 in a game: a pack of three wolves (Reach Threshold 1.5) after a
 * player walking a circle at 2 m/s catch it as lone wolves do (When Reached within reach of it:
 * places round a moving player moved on with it and were never reached; no places while it
 * walks); when it stops, they settle round it. */
TEST(KX_Navigation, AChaserPackCatchesAWalkingTarget)
{
  ChaseScene scene(3, 0, MT_Vector3(0, 0, 0));
  scene.Build();
  KX_Navigation &navigation = *scene.navigation;
  int caught[3] = {0, 0, 0};
  for (int step = 0; step < 60 * 20; step++) {
    const float a = float(step) / 60.0f * (2.0f / 6.0f);
    scene.target->NodeSetWorldPosition(MT_Vector3(6.0f * std::cos(a), 6.0f * std::sin(a), 0));
    scene.target->NodeUpdateGS(0.0);
    for (int i = 0; i < 3; i++) {
      KX_NavRequest request = GoTo(*scene.chasers[i], MT_Vector3(0, 0, 0), 4.0f);
      request.mode = KX_NavRequest::Mode::Pursue;
      request.target = scene.target;
      request.arrive_distance = 1.5f;
      navigation.Submit(request);
    }
    navigation.Step(1.0f / 60.0f, nullptr);
    for (int i = 0; i < 3; i++) {
      KX_NavAgentInfo info;
      ASSERT_TRUE(navigation.GetAgentInfo(scene.chasers[i], info));
      caught[i] += info.reached ? 1 : 0;
    }
  }
  for (int i = 0; i < 3; i++) {
    float angle, radius;
    EXPECT_FALSE(navigation.ChasePlaceOf(scene.chasers[i], angle, radius)) << i;
    EXPECT_GE(caught[i], 1) << "wolf " << i;
  }
  /* It stops: places, and they stand round it. */
  for (int step = 0; step < 60 * 10; step++) {
    for (int i = 0; i < 3; i++) {
      KX_NavRequest request = GoTo(*scene.chasers[i], MT_Vector3(0, 0, 0), 4.0f);
      request.mode = KX_NavRequest::Mode::Pursue;
      request.target = scene.target;
      request.arrive_distance = 1.5f;
      navigation.Submit(request);
    }
    navigation.Step(1.0f / 60.0f, nullptr);
  }
  std::vector<MT_Vector3> ends;
  for (int i = 0; i < 3; i++) {
    float angle, radius;
    EXPECT_TRUE(navigation.ChasePlaceOf(scene.chasers[i], angle, radius)) << i;
    ends.push_back(scene.chasers[i]->NodeGetWorldPosition());
  }
  EXPECT_LT(WidestGap(scene.target->NodeGetWorldPosition(), ends), 3.14159265f);
}

/** Item 10: arrival is a catch: within its Reach Threshold (1.5 m) of the target, as a lone
 * chaser, or at its place. A chaser that caught a standing target before the places were given
 * (0.6 m from it) stays arrived while it walks out to its place: When Reached once, not again at
 * its place; then the three stand round the target. */
TEST(KX_Navigation, AChaserWithinReachStaysArrivedWhileItTakesItsPlace)
{
  ChaseScene scene(3, 0, MT_Vector3(0, 0, 0));
  scene.chasers[0]->NodeSetWorldPosition(MT_Vector3(0.6f, 0, 0));
  scene.chasers[0]->NodeUpdateGS(0.0);
  scene.Build();
  KX_Navigation &navigation = *scene.navigation;
  int reached[3] = {0, 0, 0};
  for (int step = 0; step < 60 * 10; step++) {
    for (int i = 0; i < 3; i++) {
      KX_NavRequest request = GoTo(*scene.chasers[i], MT_Vector3(0, 0, 0), 3.0f);
      request.mode = KX_NavRequest::Mode::Pursue;
      request.target = scene.target;
      request.arrive_distance = 1.5f;
      navigation.Submit(request);
    }
    navigation.Step(1.0f / 60.0f, nullptr);
    for (int i = 0; i < 3; i++) {
      KX_NavAgentInfo info;
      ASSERT_TRUE(navigation.GetAgentInfo(scene.chasers[i], info));
      reached[i] += info.reached ? 1 : 0;
    }
  }
  EXPECT_EQ(reached[0], 1);
  std::vector<MT_Vector3> ends;
  for (int i = 0; i < 3; i++) {
    EXPECT_GE(reached[i], 1) << i;
    float angle, radius;
    ASSERT_TRUE(navigation.ChasePlaceOf(scene.chasers[i], angle, radius));
    const MT_Vector3 place = scene.target_at +
                             MT_Vector3(std::cos(angle), std::sin(angle), 0) * radius;
    EXPECT_LT(distance_2d(scene.chasers[i]->NodeGetWorldPosition(), place), 0.5f) << i;
    ends.push_back(scene.chasers[i]->NodeGetWorldPosition());
  }
  EXPECT_LT(WidestGap(scene.target_at, ends), 2.6f);
}

/** Item 10's safeguard: a lone chaser goes straight at its target, bit for bit as Go To after
 * it (a still target: Pursue's prediction is the target itself); two chasers after two targets
 * are two lone chasers. */
TEST(KX_Navigation, OneChaserGoesStraightToItsTarget)
{
  auto run = [](const KX_NavRequest::Mode mode) {
    Level level;
    level.floors.push_back({MT_Vector3(-15, -15, 0), MT_Vector3(15, 15, 0)});
    level.boxes.push_back({MT_Vector3(-3, -4, 0), MT_Vector3(3, -3.4f, 2)});
    std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
    Objects objects;
    KX_GameObject &a = objects.Add(MT_Vector3(-6, 6, 0));
    KX_GameObject &b = objects.Add(MT_Vector3(6, 6, 0));
    KX_GameObject &chaser_a = objects.Add(MT_Vector3(0.5f, -9, 0));
    KX_GameObject &chaser_b = objects.Add(MT_Vector3(-0.5f, -9, 0));
    std::vector<MT_Vector3> track;
    for (int step = 0; step < 12 * 60; step++) {
      for (auto [chaser, target] : {std::pair(&chaser_a, &a), std::pair(&chaser_b, &b)}) {
        KX_NavRequest request = GoTo(*chaser, MT_Vector3(0, 0, 0));
        request.mode = mode;
        request.target = target;
        navigation->Submit(request);
      }
      navigation->Step(1.0f / 60.0f, nullptr);
      track.push_back(chaser_a.NodeGetWorldPosition());
      track.push_back(chaser_b.NodeGetWorldPosition());
      float angle, radius;
      EXPECT_FALSE(navigation->ChasePlaceOf(&chaser_a, angle, radius));
    }
    KX_NavAgentInfo info;
    navigation->GetAgentInfo(&chaser_a, info);
    EXPECT_TRUE(info.arrived);
    return track;
  };
  const std::vector<MT_Vector3> pursue = run(KX_NavRequest::Mode::Pursue);
  const std::vector<MT_Vector3> go_to = run(KX_NavRequest::Mode::GoTo);
  ASSERT_EQ(pursue.size(), go_to.size());
  for (size_t i = 0; i < pursue.size(); i++) {
    ASSERT_EQ(pursue[i], go_to[i]) << "step " << i / 2;
  }
}

/** Item 10's safeguard: Go To after one target and Dense pursuers give no places (several NPCs
 * walking into one portal still reach it; a horde piles on); they all come within a crowd's
 * reach of it. (Neither arrives here: the target's own body stands on the goal and they mill
 * round it, as before: what spreading is for.) */
TEST(KX_Navigation, GoToAndDenseStillConverge)
{
  for (const bool dense : {false, true}) {
    ChaseScene scene(8, 1, MT_Vector3(0, 0, 0));
    scene.mode = dense ? KX_NavRequest::Mode::Pursue : KX_NavRequest::Mode::GoTo;
    scene.dense = dense;
    scene.Build();
    for (int second = 0; second < 20; second++) {
      scene.Step(60);
      for (KX_GameObject *chaser : scene.chasers) {
        float angle, radius;
        ASSERT_FALSE(scene.navigation->ChasePlaceOf(chaser, angle, radius))
            << (dense ? "dense" : "go to");
      }
    }
    for (KX_GameObject *chaser : scene.chasers) {
      EXPECT_LT(distance_2d(chaser->NodeGetWorldPosition(), scene.target_at), 2.6f)
          << (dense ? "dense" : "go to");
    }
  }
}

/** Item 10: a target with its back to a wall: no place in the wall nor behind it; the chasers
 * stand round its open side, arrived, apart. */
TEST(KX_Navigation, ChasersLeaveTheWallSideFree)
{
  ChaseScene scene(6, 0, MT_Vector3(0, -0.6f, 0), 8.0f);
  /* A wall 0.6 m thick south of it (its face at y -1.4; the navmesh 0.4 m off it, 0.4 m from the
   * target's middle: the first ring's places south of it lie in the wall). */
  scene.level.boxes.push_back({MT_Vector3(-8, -2, 0), MT_Vector3(8, -1.4f, 2)});
  scene.Build();
  scene.Step(20 * 60);
  std::vector<MT_Vector3> all;
  for (KX_GameObject *chaser : scene.chasers) {
    float angle = 0.0f, radius = 0.0f;
    ASSERT_TRUE(scene.navigation->ChasePlaceOf(chaser, angle, radius));
    const MT_Vector3 place = scene.target_at +
                             MT_Vector3(std::cos(angle), std::sin(angle), 0) * radius;
    EXPECT_GT(place[1], -1.05f) << place[0] << " " << place[1];
    const MT_Vector3 at = chaser->NodeGetWorldPosition();
    EXPECT_LT(distance_2d(at, place), 0.45f) << at[0] << " " << at[1];
    KX_NavAgentInfo info;
    scene.navigation->GetAgentInfo(chaser, info);
    EXPECT_TRUE(info.arrived);
    all.push_back(at);
  }
  for (size_t a = 0; a < all.size(); a++) {
    for (size_t b = a + 1; b < all.size(); b++) {
      EXPECT_GT(distance_2d(all[a], all[b]), 0.55f);
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Find Spot (Navigation_Settings_And_Vehicles_Plan.md E8, 2026-11-20)
 * \{ */

/** Whether the segment a-b misses every box of `level` (the slab test). */
bool LineClearOf(const Level &level, const MT_Vector3 &a, const MT_Vector3 &b)
{
  for (const Level::Box &box : level.boxes) {
    float t0 = 0.0f, t1 = 1.0f;
    bool miss = false;
    for (int i = 0; i < 3 && !miss; i++) {
      const float d = float(b[i] - a[i]);
      if (std::fabs(d) < 1e-9f) {
        miss = float(a[i]) < float(box.min[i]) || float(a[i]) > float(box.max[i]);
        continue;
      }
      float u = (float(box.min[i]) - float(a[i])) / d, v = (float(box.max[i]) - float(a[i])) / d;
      if (u > v) {
        std::swap(u, v);
      }
      t0 = std::max(t0, u);
      t1 = std::min(t1, v);
      miss = t0 > t1;
    }
    if (!miss) {
      return false;
    }
  }
  return true;
}

/** Find Spot on a yard with two low walls between an enemy (south) and the NPC: hiding places
 * are all out of the enemy's sight, places to shoot from all in it, cover places have a wall
 * close toward the enemy; the nearest walk scores best; a place another NPC stands on is taken;
 * an area keeps to itself; the same answer on another thread. */
TEST(KX_Navigation, FindSpotFindsHidingCoverAndShootingPlaces)
{
  Level level;
  level.floors.push_back({MT_Vector3(-15, -15, 0), MT_Vector3(15, 15, 0)});
  level.floors.push_back({MT_Vector3(8, 6, 0.01f), MT_Vector3(14, 12, 0.01f)});
  level.boxes.push_back({MT_Vector3(-7, -2, 0), MT_Vector3(-2, -1.4f, 2)});
  level.boxes.push_back({MT_Vector3(2, -2, 0), MT_Vector3(7, -1.4f, 2)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  level.floors[1].area = navigation->AreaId("mud");
  Objects objects;
  KX_GameObject &npc = objects.Add(MT_Vector3(0, 6, 0));
  KX_GameObject &other = objects.Add(MT_Vector3(-12, 12, 0));
  for (int step = 0; step < 3; step++) {
    navigation->Submit(GoTo(npc, MT_Vector3(0, 6, 0)));
    navigation->Submit(GoTo(other, MT_Vector3(-12, 12, 0)));
    navigation->Step(1.0f / 60.0f, nullptr);
  }
  const KX_NavMesh *navmesh = navigation->FindSceneNavMesh();
  ASSERT_NE(navmesh, nullptr);
  const MT_Vector3 eye(0, -12, 1.6f), up(0, 0, kSpotBodyHeight), eyes(0, 0, kSpotEyeHeight);
  const auto clear = [&](const MT_Vector3 &a, const MT_Vector3 &b) {
    return LineClearOf(level, a, b);
  };
  KX_NavSpotQuery query;
  query.center = MT_Vector3(0, 0, 0);
  query.from = MT_Vector3(0, 6, 0);
  query.radius = 9.0f;
  query.samples = 96;
  query.look_from = eye;
  query.has_look = true;
  query.asker = &npc;
  std::vector<KX_NavSpot> spots;
  /* Hidden from the enemy. */
  query.visibility = 1;
  navigation->FindSpots(*navmesh, query, clear, spots);
  ASSERT_GE(spots.size(), 3u);
  for (const KX_NavSpot &spot : spots) {
    EXPECT_FALSE(LineClearOf(level, eye, spot.point + up)) << spot.point.x() << " " << spot.point.y();
  }
  for (size_t i = 1; i < spots.size(); i++) {
    EXPECT_GE(spots[i - 1].score, spots[i].score);
  }
  /* (Walk weight 1: the best is the nearest walk from (0, 6), its walk a path's length.) */
  for (const KX_NavSpot &spot : spots) {
    EXPECT_LE(spots.front().walk, spot.walk + 1e-4f);
  }
  EXPECT_GE(spots.front().walk, distance_2d(spots.front().point, query.from) - 1e-3f);
  EXPECT_LT(spots.front().walk, distance_2d(spots.front().point, query.from) + 6.0f);
  const MT_Vector3 best_hidden = spots.front().point;
  /* Seen by it. */
  query.visibility = 2;
  navigation->FindSpots(*navmesh, query, clear, spots);
  ASSERT_GE(spots.size(), 3u);
  for (const KX_NavSpot &spot : spots) {
    EXPECT_TRUE(LineClearOf(level, eye, spot.point + eyes));
  }
  /* In cover: a wall within 1.5 m toward the enemy (behind the walls, y -1.4 .. 0.2). */
  query.visibility = 0;
  query.cover = true;
  navigation->FindSpots(*navmesh, query, clear, spots);
  ASSERT_GE(spots.size(), 2u);
  for (const KX_NavSpot &spot : spots) {
    /* (Behind a wall, or beside its end: never out in the open.) */
    EXPECT_GT(spot.point.y(), -2.6f);
    EXPECT_LT(spot.point.y(), 0.3f);
    EXPECT_GT(std::fabs(float(spot.point.x())), 1.0f);
  }
  /* Taken: another NPC standing on the best hiding place. */
  query.cover = false;
  query.visibility = 1;
  other.NodeSetWorldPosition(best_hidden);
  other.NodeUpdateGS(0.0);
  navigation->FindSpots(*navmesh, query, clear, spots);
  for (const KX_NavSpot &spot : spots) {
    EXPECT_GE(distance_2d(spot.point, best_hidden), query.spacing - 1e-4f);
  }
  /* In the mud only (anywhere, no look). */
  KX_NavSpotQuery mud;
  mud.center = MT_Vector3(5, 5, 0);
  mud.from = MT_Vector3(0, 6, 0);
  mud.radius = 12.0f;
  mud.samples = 128;
  mud.area = "mud";
  navigation->FindSpots(*navmesh, mud, clear, spots);
  ASSERT_GE(spots.size(), 3u);
  for (const KX_NavSpot &spot : spots) {
    EXPECT_GT(spot.point.x(), 7.5f);
    EXPECT_GT(spot.point.y(), 5.5f);
  }
  /* The same on another thread (its own query object). */
  std::vector<KX_NavSpot> again;
  std::thread([&] { navigation->FindSpots(*navmesh, mud, clear, again); }).join();
  ASSERT_EQ(again.size(), spots.size());
  for (size_t i = 0; i < spots.size(); i++) {
    EXPECT_EQ(again[i].point, spots[i].point);
    EXPECT_EQ(again[i].score, spots[i].score);
  }
}

/** Half cover (2026-11-20): behind a wall 1.2 m high a place is in cover, hidden from the enemy
 * (a crouching body's middle) and seen by it (a standing NPC's eyes): the places to shoot from.
 * With one height for both, no place was both covered and seen (the navmesh lies a cell, here
 * 0.1 m, above the floor: heights are measured from it). */
TEST(KX_Navigation, FindSpotSeesOverLowCover)
{
  Level level;
  level.floors.push_back({MT_Vector3(-10, -15, 0), MT_Vector3(10, 6, 0)});
  level.boxes.push_back({MT_Vector3(-3, -1.4f, 0), MT_Vector3(3, -1.0f, 1.2f)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  Objects objects;
  KX_GameObject &npc = objects.Add(MT_Vector3(0, 4, 0));
  for (int step = 0; step < 3; step++) {
    navigation->Submit(GoTo(npc, MT_Vector3(0, 4, 0)));
    navigation->Step(1.0f / 60.0f, nullptr);
  }
  const KX_NavMesh *navmesh = navigation->FindSceneNavMesh();
  ASSERT_NE(navmesh, nullptr);
  const MT_Vector3 eye(0, -12, 1.6f);
  const auto clear = [&](const MT_Vector3 &a, const MT_Vector3 &b) {
    return LineClearOf(level, a, b);
  };
  KX_NavSpotQuery query;
  query.center = MT_Vector3(0, 0, 0);
  query.from = MT_Vector3(0, 4, 0);
  query.radius = 3.5f;
  query.samples = 96;
  query.look_from = eye;
  query.has_look = true;
  query.asker = &npc;
  query.cover = true;
  query.cover_distance = 1.0f;
  std::vector<KX_NavSpot> spots;
  for (const int visibility : {2, 1}) {
    query.visibility = visibility;
    navigation->FindSpots(*navmesh, query, clear, spots);
    ASSERT_GE(spots.size(), 3u) << "visibility " << visibility;
    for (const KX_NavSpot &spot : spots) {
      /* Right behind the wall, the same places both ways. */
      EXPECT_GT(spot.point.y(), -1.0f);
      EXPECT_LT(spot.point.y(), 0.05f);
      EXPECT_LT(std::fabs(float(spot.point.x())), 3.05f);
      EXPECT_FALSE(LineClearOf(level, eye, spot.point + MT_Vector3(0, 0, kSpotBodyHeight)))
          << visibility << ": " << spot.point.x() << " " << spot.point.y() << " "
          << spot.point.z();
      EXPECT_TRUE(LineClearOf(level, eye, spot.point + MT_Vector3(0, 0, kSpotEyeHeight)))
          << visibility << ": " << spot.point.x() << " " << spot.point.y() << " "
          << spot.point.z();
    }
  }
}

/** Item 13 (2026-11-20): Find Spot's lines start at the asker's own heights. Behind a wall
 * 0.75 m high a dog 0.9 m tall (its middle crouching at 0.5 m) is hidden from an enemy, a man of
 * 1.8 m (0.99 m) is not, nor an asker with no body (the fixed 1.0 m). */
TEST(KX_Navigation, FindSpotUsesTheAskersHeight)
{
  Level level;
  level.floors.push_back({MT_Vector3(-10, -15, 0), MT_Vector3(10, 6, 0)});
  level.boxes.push_back({MT_Vector3(-3, -1.4f, 0), MT_Vector3(3, -1.0f, 0.75f)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  Objects objects;
  World world;
  /* (Jolt's box: x, up, depth.) */
  KX_GameObject &dog = objects.Add(MT_Vector3(-1, 4, 0.45f));
  world.Add(dog, new JPH::BoxShape(JPH::Vec3(0.3f, 0.45f, 0.3f)), JPH::EMotionType::Kinematic);
  KX_GameObject &man = objects.Add(MT_Vector3(1, 4, 0.9f));
  world.Add(man, new JPH::BoxShape(JPH::Vec3(0.3f, 0.9f, 0.3f)), JPH::EMotionType::Kinematic);
  KX_GameObject &squad = objects.Add(MT_Vector3(0, 5, 0));
  for (int step = 0; step < 3; step++) {
    navigation->Submit(GoTo(squad, MT_Vector3(0, 5, 0)));
    navigation->Step(1.0f / 60.0f, nullptr);
  }
  EXPECT_NEAR(navigation->SpotAskerHeight(&dog), 0.9f, 1e-4f);
  EXPECT_NEAR(navigation->SpotAskerHeight(&man), 1.8f, 1e-4f);
  EXPECT_EQ(navigation->SpotAskerHeight(&squad), 0.0f);
  const KX_NavMesh *navmesh = navigation->FindSceneNavMesh();
  ASSERT_NE(navmesh, nullptr);
  const MT_Vector3 eye(0, -12, 1.6f);
  const auto clear = [&](const MT_Vector3 &a, const MT_Vector3 &b) {
    return LineClearOf(level, a, b);
  };
  KX_NavSpotQuery query;
  query.center = MT_Vector3(0, 0, 0);
  query.from = MT_Vector3(0, 4, 0);
  query.radius = 3.5f;
  query.samples = 96;
  query.look_from = eye;
  query.has_look = true;
  query.visibility = 1;
  std::vector<KX_NavSpot> spots;
  query.asker = &dog;
  navigation->FindSpots(*navmesh, query, clear, spots);
  ASSERT_GE(spots.size(), 3u);
  for (const KX_NavSpot &spot : spots) {
    /* (In the wall's shadow: up to ~1.9 m behind it at the dog's height.) */
    EXPECT_GT(spot.point.y(), -1.0f);
    EXPECT_LT(spot.point.y(), 1.2f);
    EXPECT_FALSE(LineClearOf(level, eye, spot.point + MT_Vector3(0, 0, 0.495f)));
  }
  for (KX_GameObject *asker : {&man, &squad}) {
    query.asker = asker;
    navigation->FindSpots(*navmesh, query, clear, spots);
    EXPECT_TRUE(spots.empty()) << asker->GetName() << " " << spots.size();
  }
}

/** Find Spot's cost (plan E8: "no kernel at first, measured with 300 NPCs asking it every
 * second"): 300 queries (one second of 300 NPCs, 5 a step) on a 120 m level with 48 low walls,
 * the lines counted (the game casts each through Jolt: ~2.7 us a line on one thread, the
 * Projectile Ray numbers). NAV_BENCH_ROUNDS rounds, the median. */
TEST(KX_Navigation, DISABLED_FindSpotCost)
{
  const char *rounds_text = getenv("NAV_BENCH_ROUNDS");
  const int rounds = rounds_text ? std::max(1, atoi(rounds_text)) : 5;
  Level level;
  level.floors.push_back({MT_Vector3(-60, -60, 0), MT_Vector3(60, 60, 0)});
  for (int x = -50; x <= 50; x += 15) {
    for (int y = -45; y <= 45; y += 15) {
      level.boxes.push_back({MT_Vector3(x - 2, y - 0.2f, 0), MT_Vector3(x + 2, y + 0.2f, 1.3f)});
    }
  }
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  Objects objects;
  std::vector<KX_GameObject *> npcs;
  for (int i = 0; i < 300; i++) {
    npcs.push_back(&objects.Add(MT_Vector3(-55.0f + 110.0f * float(i % 20) / 20.0f,
                                           -55.0f + 110.0f * float(i / 20) / 15.0f, 0)));
  }
  for (int step = 0; step < 3; step++) {
    for (KX_GameObject *npc : npcs) {
      navigation->Submit(GoTo(*npc, npc->NodeGetWorldPosition()));
    }
    navigation->Step(1.0f / 60.0f, nullptr);
  }
  const KX_NavMesh *navmesh = navigation->FindSceneNavMesh();
  ASSERT_NE(navmesh, nullptr);
  for (const int samples : {24, 96}) {
    std::vector<double> ms;
    size_t lines = 0, found = 0;
    for (int round = 0; round < rounds; round++) {
      lines = 0;
      found = 0;
      const auto clear = [&](const MT_Vector3 &a, const MT_Vector3 &b) {
        lines++;
        return LineClearOf(level, a, b);
      };
      std::vector<KX_NavSpot> spots;
      const auto t0 = std::chrono::steady_clock::now();
      for (KX_GameObject *npc : npcs) {
        KX_NavSpotQuery query;
        query.center = npc->NodeGetWorldPosition();
        query.from = query.center;
        query.radius = 12.0f;
        query.samples = samples;
        query.look_from = MT_Vector3(0, -58, 1.6f);
        query.has_look = true;
        query.visibility = 1;
        query.cover = true;
        query.asker = npc;
        navigation->FindSpots(*navmesh, query, clear, spots);
        found += spots.empty() ? 0 : 1;
      }
      ms.push_back(
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    std::sort(ms.begin(), ms.end());
    const double median = ms[ms.size() / 2];
    printf("Find Spot, %d samples: 300 queries %.2f ms (%.1f us each, %.2f ms a step at 5 a "
           "step), %zu lines (%.1f a query; in Jolt about %.2f ms more a step), %zu found\n",
           samples,
           median,
           median * 1000.0 / 300.0,
           median / 60.0,
           lines,
           double(lines) / 300.0,
           double(lines) / 60.0 * 0.0027,
           found);
  }
}

/** Navmesh Query's Find Spot in a tree (E8): Position, Radius and Walk Weight from game
 * properties while the game runs (the rig's physics blocks no line: Any visibility); Found,
 * Point, Score, Length and the Points' count read back; a far walk weighted negative picks a
 * place far from From instead. */
TEST(KX_NavigationNodes, NavmeshQueryFindsASpot)
{
  NavTree tree;
  diff::GenNode &query = tree.Add("LogicNativeNavmeshQuery", int16_t(LN_NavmeshQueryMode::FindSpot));
  diff::GenNode &cx = tree.Add("LogicNativeGetGamePropertyFloat");
  tree.b.String(cx, "Property", "cx");
  diff::GenNode &center = tree.Add("LogicNativeCombineXYZ");
  tree.b.Link(cx, "Value", center, "X");
  tree.b.Link(center, "Vector", query, "Position");
  diff::GenNode &radius = tree.Add("LogicNativeGetGamePropertyFloat");
  tree.b.String(radius, "Property", "radius");
  tree.b.Link(radius, "Value", query, "Radius");
  diff::GenNode &weight = tree.Add("LogicNativeGetGamePropertyFloat");
  tree.b.String(weight, "Property", "weight");
  tree.b.Link(weight, "Value", query, "Walk Weight");
  tree.b.Vector(query, "From", 0, 0, 0);
  tree.b.Int(query, "Samples", 48);
  tree.StoreBool(query, "Found", "found");
  tree.StoreVector(query, "Point", "p");
  tree.StoreFloat(query, "Score", "score");
  tree.StoreFloat(query, "Length", "walk");
  tree.StoreCount(query, "Points", "count");
  const std::shared_ptr<LN_Program> program = tree.Compile();
  ASSERT_NE(program, nullptr);

  Level level;
  level.floors.push_back({MT_Vector3(-20, -20, 0), MT_Vector3(20, 20, 0)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  NavigationSeam seam(navigation.get());
  SemRig rig(program);
  KX_GameObject &npc = rig.Object(0);
  Place(npc, MT_Vector3(0, 0, 0));
  rig.SetFloat(npc, "cx", 6.0f);
  rig.SetFloat(npc, "radius", 3.0f);
  rig.SetFloat(npc, "weight", 1.0f);
  /* (The navmesh is built at the navigation's first step.) */
  for (int step = 0; step < 3; step++) {
    rig.Step();
    navigation->Step(1.0f / 60.0f, nullptr);
  }
  EXPECT_EQ(rig.Float(npc, "found"), 1.0f);
  /* Nearest to From (0, 0): the circle's west edge round x 3. */
  EXPECT_LT(rig.Float(npc, "pX"), 4.0f);
  EXPECT_NEAR(rig.Float(npc, "walk"), rig.Float(npc, "pX"), 0.5f);
  EXPECT_NEAR(rig.Float(npc, "score"), -rig.Float(npc, "walk"), 1e-3f);
  EXPECT_GE(rig.Float(npc, "count"), 30.0f);
  /* Farther is better now, and a centre moved while playing. */
  rig.SetFloat(npc, "weight", -1.0f);
  rig.SetFloat(npc, "cx", -6.0f);
  rig.Step();
  navigation->Step(1.0f / 60.0f, nullptr);

  EXPECT_LT(rig.Float(npc, "pX"), -8.0f);
  EXPECT_GT(rig.Float(npc, "score"), 8.0f);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Navigation made easy (2026-11-20, Navigation_Ease_Of_Use_Plan.md phases 1 and 2)
 * \{ */

namespace {

bool HasProblem(const KX_Navigation &navigation, const std::string &kind)
{
  for (const KX_NavProblem &problem : navigation.GetProblems()) {
    if (problem.kind == kind) {
      return true;
    }
  }
  return false;
}

/** Two floors joined only by a link: the lower (x below -1, z 0), the upper (x above 1, z 1.5). */
Level TwoFloorsLevel()
{
  Level level;
  level.floors.push_back({MT_Vector3(-10, -10, 0), MT_Vector3(-1, 10, 0)});
  level.floors.push_back({MT_Vector3(1, -10, 1.5f), MT_Vector3(10, 10, 1.5f)});
  return level;
}

}  // namespace

/** Item 1: Build Auto builds nothing until an NPC walks the scene's navmesh (or a query asks for
 * it); an order straight at its goal asks for none. Then it is the whole level, in the step that
 * asked: the first order already goes round the wall. */
TEST(KX_Navigation, AutoBuildsWhenAnNpcNeedsTheSceneNavmesh)
{
  Level level = WallLevel();
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(
      level, TestSettings(blender::NAVMESH_BUILD_AUTO));
  Objects objects;
  KX_GameObject &npc = objects.Add(MT_Vector3(-5, -5, 0));
  for (int step = 0; step < 10; step++) {
    navigation->Step(1.0f / 60.0f, nullptr);
  }
  EXPECT_EQ(navigation->BuiltCellSize(), 0.0f);
  /* Straight at the goal (Follow Path's Straight way): no navmesh wanted. */
  KX_NavRequest straight = GoTo(npc, MT_Vector3(-5, -4, 0));
  straight.use_scene_navmesh = false;
  for (int step = 0; step < 5; step++) {
    navigation->Submit(straight);
    navigation->Step(1.0f / 60.0f, nullptr);
  }
  EXPECT_EQ(navigation->BuiltCellSize(), 0.0f);
  /* On the scene's navmesh: built in this step, its way round the wall's end (x 6..10). */
  navigation->Submit(GoTo(npc, MT_Vector3(-5, 5, 0)));
  navigation->Step(1.0f / 60.0f, nullptr);
  EXPECT_GT(navigation->BuiltCellSize(), 0.0f);
  KX_NavAgentInfo info;
  ASSERT_TRUE(navigation->GetAgentInfo(&npc, info));
  EXPECT_TRUE(info.has_path);
  EXPECT_TRUE(info.path_complete);
  EXPECT_GT(info.next_corner.x(), 5.0f);
  EXPECT_FALSE(HasProblem(*navigation, "nothing_to_walk_on"));
}

/** Item 1: a Navmesh Query finding no scene navmesh under Auto has it built at the next step. */
TEST(KX_Navigation, AutoBuildsWhenAQueryAsksForTheSceneNavmesh)
{
  Level level = WallLevel();
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(
      level, TestSettings(blender::NAVMESH_BUILD_AUTO));
  navigation->Step(1.0f / 60.0f, nullptr);
  EXPECT_EQ(navigation->FindSceneNavMesh(), nullptr);
  navigation->Step(1.0f / 60.0f, nullptr);
  EXPECT_NE(navigation->FindSceneNavMesh(), nullptr);
}

/** Item 1: the very first step's order is served (the build runs before the step's searches),
 * and the NPC arrives without ever crossing the wall. */
TEST(KX_Navigation, AutoServesTheFirstStep)
{
  Level level = WallLevel();
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(
      level, TestSettings(blender::NAVMESH_BUILD_AUTO));
  Objects objects;
  KX_GameObject &npc = objects.Add(MT_Vector3(-5, -5, 0));
  bool arrived = false;
  for (int step = 0; step < 60 * 20 && !arrived; step++) {
    navigation->Submit(GoTo(npc, MT_Vector3(-5, 5, 0)));
    navigation->Step(1.0f / 60.0f, nullptr);
    KX_NavAgentInfo info;
    ASSERT_TRUE(navigation->GetAgentInfo(&npc, info));
    if (step == 0) {
      EXPECT_TRUE(info.has_path);
      EXPECT_FALSE(info.no_path);
    }
    const MT_Vector3 p = npc.NodeGetWorldPosition();
    if (p.x() < 5.5f) {
      ASSERT_FALSE(std::fabs(p.y()) < 0.6f) << step;
    }
    arrived = info.arrived;
  }
  EXPECT_TRUE(arrived);
}

/** Item 1: Off stays a choice: the NPC walks straight at its goal (through where the wall is: no
 * physics here) and nothing is reported. */
TEST(KX_Navigation, OffWalksStraightWithoutWarning)
{
  Level level = WallLevel();
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(
      level, TestSettings(blender::NAVMESH_BUILD_OFF));
  Objects objects;
  KX_GameObject &npc = objects.Add(MT_Vector3(-5, -5, 0));
  bool crossed = false;
  for (int step = 0; step < 60 * 8; step++) {
    navigation->Submit(GoTo(npc, MT_Vector3(-5, 5, 0)));
    navigation->Step(1.0f / 60.0f, nullptr);
    const MT_Vector3 p = npc.NodeGetWorldPosition();
    crossed = crossed || (std::fabs(p.y()) < 0.3f && p.x() < 0.0f);
  }
  EXPECT_TRUE(crossed);
  EXPECT_NEAR(npc.NodeGetWorldPosition().y(), 5.0f, 0.35f);
  EXPECT_EQ(navigation->BuiltCellSize(), 0.0f);
  EXPECT_TRUE(navigation->GetProblems().empty());
}

/** Item 2: an NPC's size is its collision shape's: a Character its capsule, a body its shape's
 * half width on the plane (not the scene's fallback size, 0.6 m here); an object without a body
 * the fallback. */
TEST(KX_Navigation, AnNpcsSizeIsItsCollisionShape)
{
  Level level;
  level.floors.push_back({MT_Vector3(-10, -10, 0), MT_Vector3(10, 10, 0)});
  KX_Navigation::Settings settings = TestSettings();
  settings.agent_radius = 0.6f;
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, settings);
  Objects objects;
  World world;
  KX_GameObject &floor = objects.Add(MT_Vector3(0, 0, -0.5f));
  world.StaticBox(floor, MT_Vector3(10, 10, 0.5f));
  KX_GameObject &hero = objects.Add(MT_Vector3(-5, -5, 0.95f));
  ASSERT_NE(world.Character(hero), nullptr);
  /* A crate of 1.0 x 0.4 x 0.8 m (Jolt's box: x, up, depth). */
  KX_GameObject &crate = objects.Add(MT_Vector3(5, -5, 0.4f));
  world.Add(crate, new JPH::BoxShape(JPH::Vec3(0.5f, 0.4f, 0.2f)), JPH::EMotionType::Dynamic);
  KX_GameObject &ghost = objects.Add(MT_Vector3(0, 5, 0));
  navigation->SetPhysicsEnvironmentForTests(&world.environment);
  for (KX_GameObject *npc : {&hero, &crate, &ghost}) {
    navigation->Submit(GoTo(*npc, MT_Vector3(0, 0, 0)));
  }
  navigation->Step(1.0f / 60.0f, nullptr);
  EXPECT_NEAR(navigation->AgentWalkRadiusOf(&hero), 0.3f, 0.01f);
  EXPECT_NEAR(navigation->AgentWalkRadiusOf(&crate), 0.5f, 0.01f);
  EXPECT_NEAR(navigation->AgentWalkRadiusOf(&ghost), 0.6f, 1e-6f);
}

/** Item 2: a turned box keeps its size (its shape's own bounds, not the world's box round it). */
TEST(KX_Navigation, ATurnedBoxNpcKeepsItsSize)
{
  Level level;
  level.floors.push_back({MT_Vector3(-10, -10, 0), MT_Vector3(10, 10, 0)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  Objects objects;
  World world;
  KX_GameObject &crate = objects.Add(MT_Vector3(0, 0, 0.4f));
  /* A plank 1.0 x 0.1 m turned 45 degrees: the world's box round it is 0.39 m each side. */
  crate.NodeSetLocalOrientation(MT_Matrix3x3(MT_Vector3(0.0f, 0.0f, float(M_PI) * 0.25f)));
  crate.NodeUpdateGS(0.0);
  world.Add(crate, new JPH::BoxShape(JPH::Vec3(0.5f, 0.4f, 0.05f)), JPH::EMotionType::Dynamic);
  navigation->SetPhysicsEnvironmentForTests(&world.environment);
  navigation->Submit(GoTo(crate, MT_Vector3(5, 0, 0)));
  navigation->Step(1.0f / 60.0f, nullptr);
  EXPECT_NEAR(navigation->AgentWalkRadiusOf(&crate), 0.5f, 0.01f);
}

/** Item 2: Physics > Navigation's NPC Radius wins over the shape (more room than its body). */
TEST(KX_Navigation, TheNpcRadiusOverrideWins)
{
  Level level;
  level.floors.push_back({MT_Vector3(-10, -10, 0), MT_Vector3(10, 10, 0)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  Objects objects;
  World world;
  KX_GameObject &horse = objects.Add(MT_Vector3(0, 0, 0.95f));
  ASSERT_NE(world.Character(horse), nullptr);
  KX_NavObjectSettings settings;
  settings.npc_radius = 0.8f;
  horse.SetNavSettingsForTests(settings, nullptr);
  navigation->SetPhysicsEnvironmentForTests(&world.environment);
  navigation->Submit(GoTo(horse, MT_Vector3(5, 0, 0)));
  navigation->Step(1.0f / 60.0f, nullptr);
  EXPECT_NEAR(navigation->AgentWalkRadiusOf(&horse), 0.8f, 1e-6f);
}

/** Item 3: a Dynamic NPC is pushed by its velocity with no setting (it was slid unless Move as
 * Dynamic was on); Slide (move_dynamic off) moves it by its position. */
TEST(KX_Navigation, ADynamicNpcIsPushedByVelocityWithoutASetting)
{
  for (const bool slide : {false, true}) {
    SCOPED_TRACE(slide ? "slide" : "default");
    Level level;
    level.floors.push_back({MT_Vector3(-10, -10, 0), MT_Vector3(10, 10, 0)});
    std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
    Objects objects;
    World world;
    KX_GameObject &floor = objects.Add(MT_Vector3(0, 0, -0.5f));
    world.StaticBox(floor, MT_Vector3(10, 10, 0.5f));
    KX_GameObject &ball = objects.Add(MT_Vector3(-5, 0, 0.3f));
    Body &body = world.Add(
        ball, new JPH::BoxShape(JPH::Vec3(0.3f, 0.3f, 0.3f)), JPH::EMotionType::Dynamic);
    navigation->SetPhysicsEnvironmentForTests(&world.environment);
    float fastest = 0.0f;
    for (int step = 0; step < 60 * 8; step++) {
      KX_NavRequest request = GoTo(ball, MT_Vector3(5, 0, 0), 2.0f);
      if (slide) {
        request.move_dynamic = false;
      }
      navigation->Submit(request);
      navigation->Step(1.0f / 60.0f, nullptr);
      world.Step({&ball});
      fastest = std::max(fastest, float(body.controller.GetLinearVelocity().length()));
    }
    EXPECT_NEAR(ball.NodeGetWorldPosition().x(), 5.0f, 0.5f);
    if (slide) {
      EXPECT_LT(fastest, 0.5f);
    }
    else {
      EXPECT_GT(fastest, 1.5f);
    }
  }
}

/** Item 4: a link's end 0.9 m over the floor it leads to (a ladder's top at its tip) moves down
 * onto that floor and joins: the NPC climbs. The move is listed (not printed). */
TEST(KX_Navigation, ALinkEndAboveItsFloorJoins)
{
  Level level = TwoFloorsLevel();
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  Objects objects;
  KX_GameObject &ladder = objects.Add(MT_Vector3(-1.5f, 0, 0));
  KX_NavLink link;
  link.object = &ladder;
  link.start = MT_Vector3(-1.5f, 0, 0);
  link.end = MT_Vector3(1.5f, 0, 2.4f);
  link.kind = "ladder";
  link.user_id = 1;
  navigation->SetLinksForTests({link});
  ASSERT_EQ(navigation->GetLinks().size(), 1u);
  EXPECT_NEAR(navigation->GetLinks()[0].end.z(), 1.5f, 1e-4f);
  EXPECT_NEAR(navigation->GetLinks()[0].end_shift.z(), -0.9f, 1e-4f);
  EXPECT_EQ(navigation->GetLinks()[0].start, MT_Vector3(-1.5f, 0, 0));
  bool moved_listed = false;
  for (const KX_NavProblem &problem : navigation->GetProblems()) {
    if (problem.kind == "link_end_moved") {
      moved_listed = true;
      EXPECT_FALSE(problem.setup);
    }
  }
  EXPECT_TRUE(moved_listed);
  KX_GameObject &npc = objects.Add(MT_Vector3(-6, 3, 0));
  bool arrived = false;
  for (int step = 0; step < 60 * 20 && !arrived; step++) {
    navigation->Submit(GoTo(npc, MT_Vector3(6, -3, 1.5f)));
    navigation->Step(1.0f / 60.0f, nullptr);
    KX_NavAgentInfo info;
    ASSERT_TRUE(navigation->GetAgentInfo(&npc, info));
    arrived = info.arrived;
  }
  EXPECT_TRUE(arrived);
  EXPECT_NEAR(npc.NodeGetWorldPosition().z(), 1.5f, 0.1f);
  EXPECT_FALSE(HasProblem(*navigation, "link_end_off_floor"));
}

/** Item 4: ends on a floor (within Max Climb) stay exactly where they are. */
TEST(KX_Navigation, ALinkEndOnAFloorStays)
{
  Level level = TwoFloorsLevel();
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  Objects objects;
  KX_GameObject &ladder = objects.Add(MT_Vector3(-1.5f, 0, 0.1f));
  KX_NavLink link;
  link.object = &ladder;
  link.start = MT_Vector3(-1.5f, 0.25f, 0.1f);
  link.end = MT_Vector3(1.5f, -0.25f, 1.8f);
  link.user_id = 1;
  navigation->SetLinksForTests({link});
  ASSERT_EQ(navigation->GetLinks().size(), 1u);
  EXPECT_EQ(navigation->GetLinks()[0].start, link.start);
  EXPECT_EQ(navigation->GetLinks()[0].end, link.end);
  EXPECT_EQ(navigation->GetLinks()[0].end_shift, MT_Vector3(0, 0, 0));
  EXPECT_TRUE(navigation->GetProblems().empty());
}

/** Item 4: an end between two floors (1.45 m over the ground, 0.45 m under a shelf's top) takes the
 * floor below; one 0.45 m under a floor and far over any other goes up onto it. */
TEST(KX_Navigation, ALinkEndBetweenTwoFloorsTakesTheOneBelow)
{
  Level level;
  level.floors.push_back({MT_Vector3(-10, -10, 0), MT_Vector3(10, 10, 0)});
  /* A shelf (a solid box) whose top is at 1.9 m, and a high platform whose top is at 5 m. */
  level.boxes.push_back({MT_Vector3(0, -2, 1.8f), MT_Vector3(4, 2, 1.9f)});
  level.boxes.push_back({MT_Vector3(-8, -2, 4.9f), MT_Vector3(-4, 2, 5.0f)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  Objects objects;
  KX_GameObject &a = objects.Add(MT_Vector3(2, 0, 1.45f));
  KX_GameObject &b = objects.Add(MT_Vector3(-6, 0, 4.55f));
  KX_NavLink down;
  down.object = &a;
  down.start = MT_Vector3(8, 0, 0);
  down.end = MT_Vector3(2, 0, 1.45f);
  down.user_id = 1;
  KX_NavLink up;
  up.object = &b;
  up.start = MT_Vector3(8, 5, 0);
  up.end = MT_Vector3(-6, 0, 4.55f);
  up.user_id = 2;
  navigation->SetLinksForTests({down, up});
  ASSERT_EQ(navigation->GetLinks().size(), 2u);
  EXPECT_NEAR(navigation->GetLinks()[0].end.z(), 0.0f, 1e-4f);
  EXPECT_NEAR(navigation->GetLinks()[1].end.z(), 5.0f, 1e-4f);
}

/** Item 9: each problem is listed once per object, however many steps it holds (counted), and
 * the play report carries them. */
TEST(KX_Navigation, EveryProblemIsReportedOnce)
{
  /* Auto with nothing to walk on. */
  {
    Level empty;
    std::unique_ptr<KX_Navigation> navigation = MakeNavigation(
        empty, TestSettings(blender::NAVMESH_BUILD_AUTO));
    Objects objects;
    KX_GameObject &npc = objects.Add(MT_Vector3(0, 0, 0));
    for (int step = 0; step < 30; step++) {
      navigation->Submit(GoTo(npc, MT_Vector3(5, 0, 0)));
      navigation->Step(1.0f / 60.0f, nullptr);
    }
    const std::vector<KX_NavProblem> problems = navigation->GetProblems();
    int found = 0;
    for (const KX_NavProblem &problem : problems) {
      found += problem.kind == "nothing_to_walk_on" ? 1 : 0;
      EXPECT_TRUE(problem.setup);
    }
    EXPECT_EQ(found, 1);
    const std::string report = navigation->ReportJson();
    EXPECT_NE(report.find("\"nothing_to_walk_on\""), std::string::npos) << report;
    EXPECT_NE(report.find("\"build\": \"auto\""), std::string::npos) << report;
    /* The same problem again: counted, listed once. */
    for (int k = 0; k < 3; k++) {
      navigation->ReportProblem("again", "Guard", "'Guard' again", false);
    }
    int again = 0;
    for (const KX_NavProblem &problem : navigation->GetProblems()) {
      if (problem.kind == "again") {
        again++;
        EXPECT_EQ(problem.count, 3);
      }
    }
    EXPECT_EQ(again, 1);
  }
  /* NPCs rounded up to whole cells (0.3 m at 0.2 m cells: 0.4 m), each once; a link whose end is
   * on no floor (2 m over the ground: further than a floor is looked for). */
  {
    Level level;
    level.floors.push_back({MT_Vector3(-10, -10, 0), MT_Vector3(10, 10, 0)});
    std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
    Objects objects;
    KX_GameObject &a = objects.Add(MT_Vector3(-5, 0, 0));
    KX_GameObject &b = objects.Add(MT_Vector3(5, 0, 0));
    a.SetName("Guard A");
    b.SetName("Guard B");
    KX_GameObject &post = objects.Add(MT_Vector3(0, 5, 0));
    post.SetName("Post");
    KX_GameObject &top = objects.Add(MT_Vector3(0, 8, 2.0f));
    top.SetName("Post Top");
    KX_NavLink link;
    link.object = &post;
    link.end_object = &top;
    link.start = MT_Vector3(0, 5, 0);
    link.end = MT_Vector3(0, 8, 2.0f);
    link.user_id = 1;
    navigation->SetLinksForTests({link});
    for (int step = 0; step < 30; step++) {
      navigation->Submit(GoTo(a, MT_Vector3(-5, 5, 0)));
      navigation->Submit(GoTo(b, MT_Vector3(5, 5, 0)));
      navigation->Step(1.0f / 60.0f, nullptr);
    }
    std::map<std::string, int> seen;
    for (const KX_NavProblem &problem : navigation->GetProblems()) {
      seen[problem.kind + "/" + problem.object]++;
      EXPECT_EQ(problem.count, 1) << problem.kind << " " << problem.text;
    }
    EXPECT_EQ(seen["npc_rounded_up/Guard A"], 1);
    EXPECT_EQ(seen["npc_rounded_up/Guard B"], 1);
    EXPECT_EQ(seen["link_end_off_floor/Post"], 1);
    for (const auto &[key, count] : seen) {
      EXPECT_EQ(count, 1) << key;
    }
  }
}

/** Item 6: Auto Cells take the cell from the smallest walking NPC (radius / 2, 0.1 .. 0.3 m). */
TEST(KX_Navigation, AutoCellsFollowTheSmallestNpc)
{
  EXPECT_FLOAT_EQ(KX_Navigation::AutoCellSize(0.3f), 0.15f);
  EXPECT_FLOAT_EQ(KX_Navigation::AutoCellSize(0.05f), 0.1f);
  EXPECT_FLOAT_EQ(KX_Navigation::AutoCellSize(2.0f), 0.3f);
  Level level;
  level.floors.push_back({MT_Vector3(-10, -10, 0), MT_Vector3(10, 10, 0)});
  KX_Navigation::Settings settings = TestSettings(blender::NAVMESH_BUILD_AUTO);
  settings.auto_cells = true;
  settings.agent_radius = 0.6f;
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, settings);
  Objects objects;
  World world;
  KX_GameObject &floor = objects.Add(MT_Vector3(0, 0, -0.5f));
  world.StaticBox(floor, MT_Vector3(10, 10, 0.5f));
  KX_GameObject &hero = objects.Add(MT_Vector3(-5, -5, 0.95f));
  hero.SetName("Guard");
  ASSERT_NE(world.Character(hero), nullptr);
  KX_GameObject &big = objects.Add(MT_Vector3(5, -5, 0));
  navigation->SetPhysicsEnvironmentForTests(&world.environment);
  navigation->Submit(GoTo(hero, MT_Vector3(-5, 5, 0)));
  navigation->Submit(GoTo(big, MT_Vector3(5, 5, 0)));
  navigation->Step(1.0f / 60.0f, nullptr);
  EXPECT_NEAR(navigation->BuiltCellSize(), 0.15f, 1e-6f);
  EXPECT_NE(navigation->AutoCellReason().find("Guard"), std::string::npos)
      << navigation->AutoCellReason();
  EXPECT_FALSE(HasProblem(*navigation, "npc_rounded_up"));
}

/** Item 6: a door 0.9 m wide (1.5 NPCs of radius 0.3 m) is walked through with Auto Cells
 * (0.15 m: the radius exactly two cells); at 0.2 m cells it is closed (0.3 m counted as 0.4 m),
 * as the measurement of 2026-11-20 found, and that is reported. */
TEST(KX_Navigation, ADoorOneAndAHalfNpcsWideIsWalkable)
{
  for (const bool auto_cells : {true, false}) {
    SCOPED_TRACE(auto_cells ? "auto cells" : "0.2 m cells");
    Level level;
    level.floors.push_back({MT_Vector3(-10, -10, 0), MT_Vector3(10, 10, 0)});
    level.boxes.push_back({MT_Vector3(-10, -0.25f, 0), MT_Vector3(-0.45f, 0.25f, 2)});
    level.boxes.push_back({MT_Vector3(0.45f, -0.25f, 0), MT_Vector3(10, 0.25f, 2)});
    KX_Navigation::Settings settings = TestSettings();
    settings.auto_cells = auto_cells;
    std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, settings);
    Objects objects;
    KX_GameObject &npc = objects.Add(MT_Vector3(0.2f, -5, 0));
    npc.SetName("Guard");
    bool arrived = false;
    for (int step = 0; step < 60 * 12 && !arrived; step++) {
      navigation->Submit(GoTo(npc, MT_Vector3(-0.2f, 5, 0), 2.0f));
      navigation->Step(1.0f / 60.0f, nullptr);
      KX_NavAgentInfo info;
      ASSERT_TRUE(navigation->GetAgentInfo(&npc, info));
      arrived = info.arrived && info.path_complete;
    }
    EXPECT_EQ(arrived, auto_cells);
    EXPECT_EQ(HasProblem(*navigation, "npc_rounded_up"), !auto_cells);
  }
}

/** Item 8: When Failed pulses once an order and reason, Get Navigation Info's Reason says why:
 * Unreachable (the goal on an island: it stands at the end of its partial way). */
TEST(KX_NavigationNodes, WhenFailedPulsesOnceWithItsReason)
{
  NavTree tree;
  diff::GenNode &navigate = tree.Add("LogicNativeNavigate");
  tree.b.Vector(navigate, "Destination", 6, 0, 0);
  tree.b.Float(navigate, "Lin Speed", 3.0f);
  tree.b.Float(navigate, "Reach Threshold", 0.3f);
  tree.Chain(navigate);
  tree.Flag(navigate, "When Failed", "failed");
  tree.Flag(navigate, "When Reached", "reached");
  diff::GenNode &info = tree.Add("LogicNativeGetNavigationInfo");
  tree.StoreText(info, "Reason", "reason");
  const std::shared_ptr<LN_Program> program = tree.Compile();
  ASSERT_NE(program, nullptr);
  for (const SemMode &mode : {kProduction, kMemoOff}) {
    SCOPED_TRACE(mode.name);
    /* Two floors with a gap between them: the goal is on the other one. */
    Level level;
    level.floors.push_back({MT_Vector3(-10, -10, 0), MT_Vector3(-1, 10, 0)});
    level.floors.push_back({MT_Vector3(1, -10, 0), MT_Vector3(10, 10, 0)});
    std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
    NavigationSeam seam(navigation.get());
    SemRig rig(program, mode);
    KX_GameObject &npc = rig.Object(0);
    Place(npc, MT_Vector3(-6, 0, 0));
    rig.SetBool(npc, "reached", false);
    int failed = 0;
    int first_failed_step = -1;
    for (int step = 0; step < 60 * 10; step++) {
      rig.SetBool(npc, "failed", false);
      rig.Step();
      navigation->Step(1.0f / 60.0f, nullptr);
      if (rig.Float(npc, "failed") == 1.0f) {
        failed++;
        if (first_failed_step < 0) {
          first_failed_step = step;
        }
      }
    }
    EXPECT_EQ(failed, 1);
    EXPECT_GT(first_failed_step, 60);
    EXPECT_EQ(rig.Text(npc, "reason"), "Unreachable");
    EXPECT_EQ(rig.Float(npc, "reached"), 0.0f);
    EXPECT_LT(npc.NodeGetWorldPosition().x(), -0.5f);
  }
}

/** Item 8: When Failed is per node: another Navigate's order (a new order) re-arms it, and only
 * the node whose order failed pulses. */
TEST(KX_NavigationNodes, AnotherNodesOrderReArmsWhenFailed)
{
  NavTree tree;
  diff::GenNode &which = tree.Add("LogicNativeGetGamePropertyBool");
  tree.b.String(which, "Property", "use_b");
  diff::GenNode &branch = tree.Add("LogicNativeBranch");
  tree.b.Link(*tree.step, "Out", branch, "Flow");
  tree.b.Link(which, "Value", branch, "Condition");
  diff::GenNode &a = tree.Add("LogicNativeNavigate");
  diff::GenNode &b = tree.Add("LogicNativeNavigate");
  for (diff::GenNode *navigate : {&a, &b}) {
    tree.b.Float(*navigate, "Lin Speed", 3.0f);
    tree.b.Float(*navigate, "Reach Threshold", 0.3f);
  }
  tree.b.Vector(a, "Destination", 6, 0, 0);
  tree.b.Vector(b, "Destination", 6, 4, 0);
  tree.b.Link(branch, "False", a, "Flow");
  tree.b.Link(branch, "True", b, "Flow");
  tree.Flag(a, "When Failed", "failed_a");
  tree.Flag(b, "When Failed", "failed_b");
  const std::shared_ptr<LN_Program> program = tree.Compile();
  ASSERT_NE(program, nullptr);
  Level level;
  level.floors.push_back({MT_Vector3(-10, -10, 0), MT_Vector3(-1, 10, 0)});
  level.floors.push_back({MT_Vector3(1, -10, 0), MT_Vector3(10, 10, 0)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  NavigationSeam seam(navigation.get());
  SemRig rig(program);
  KX_GameObject &npc = rig.Object(0);
  Place(npc, MT_Vector3(-6, 0, 0));
  rig.SetBool(npc, "use_b", false);
  int failed_a = 0, failed_b = 0;
  auto run = [&](const int steps) {
    for (int step = 0; step < steps; step++) {
      rig.SetBool(npc, "failed_a", false);
      rig.SetBool(npc, "failed_b", false);
      rig.Step();
      navigation->Step(1.0f / 60.0f, nullptr);
      failed_a += rig.Float(npc, "failed_a") == 1.0f ? 1 : 0;
      failed_b += rig.Float(npc, "failed_b") == 1.0f ? 1 : 0;
    }
  };
  run(60 * 8);
  EXPECT_EQ(failed_a, 1);
  EXPECT_EQ(failed_b, 0);
  rig.SetBool(npc, "use_b", true);
  run(60 * 8);
  EXPECT_EQ(failed_a, 1);
  EXPECT_EQ(failed_b, 1);
  rig.SetBool(npc, "use_b", false);
  run(60 * 8);
  EXPECT_EQ(failed_a, 2);
  EXPECT_EQ(failed_b, 1);
}

/** Item 7 (2026-11-20): Navigate's Avoidance is an input a game property can switch while the
 * game runs: Off, Polite, Dense and back, each step's order taking the value of that step. */
TEST(KX_NavigationNodes, AvoidanceSwitchesWhilePlaying)
{
  NavTree tree;
  diff::GenNode &avoid = tree.Add("LogicNativeGetGamePropertyInt");
  tree.b.String(avoid, "Property", "avoid");
  diff::GenNode &navigate = tree.Add("LogicNativeNavigate", 0, LN_NAVIGATE_UNTIL_ARRIVED);
  tree.b.Vector(navigate, "Destination", 8, 0, 0);
  tree.b.Float(navigate, "Lin Speed", 0.5f);
  tree.b.Link(avoid, "Value", navigate, "Avoidance");
  tree.Chain(navigate);
  const std::shared_ptr<LN_Program> program = tree.Compile();
  ASSERT_NE(program, nullptr);
  for (const SemMode &mode : {kProduction, kMemoOff}) {
    SCOPED_TRACE(mode.name);
    Level level;
    level.floors.push_back({MT_Vector3(-10, -10, 0), MT_Vector3(10, 10, 0)});
    std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
    NavigationSeam seam(navigation.get());
    SemRig rig(program, mode);
    KX_GameObject &npc = rig.Object(0);
    Place(npc, MT_Vector3(-8, 0, 0));
    for (const int value : {0, 1, 2, 1, 0, 2}) {
      rig.SetInt(npc, "avoid", value);
      rig.Step();
      navigation->Step(1.0f / 60.0f, nullptr);
      KX_NavRequest held;
      ASSERT_TRUE(navigation->GetHeldOrder(&npc, held)) << value;
      EXPECT_EQ(held.avoid, value != 0) << value;
      EXPECT_EQ(held.dense, value == 2) << value;
    }
  }
}

/** Item 3: Navigate's Slide reaches the request (off: move_dynamic, so a Dynamic NPC is pushed by
 * its velocity; on: slid). */
TEST(KX_NavigationNodes, SlideReachesTheRequest)
{
  for (const bool slide : {false, true}) {
    NavTree tree;
    diff::GenNode &once = tree.Add("LogicNativeOnce");
    tree.b.Link(*tree.step, "Out", once, "Flow");
    diff::GenNode &navigate = tree.Add("LogicNativeNavigate", 0, LN_NAVIGATE_UNTIL_ARRIVED);
    tree.b.Vector(navigate, "Destination", 8, 0, 0);
    tree.b.Bool(navigate, "Slide", slide);
    tree.b.Link(once, "Out", navigate, "Flow");
    const std::shared_ptr<LN_Program> program = tree.Compile();
    ASSERT_NE(program, nullptr);
    Level level;
    level.floors.push_back({MT_Vector3(-10, -10, 0), MT_Vector3(10, 10, 0)});
    std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
    NavigationSeam seam(navigation.get());
    SemRig rig(program);
    KX_GameObject &npc = rig.Object(0);
    Place(npc, MT_Vector3(-8, 0, 0));
    rig.Step();
    navigation->Step(1.0f / 60.0f, nullptr);
    KX_NavRequest held;
    ASSERT_TRUE(navigation->GetHeldOrder(&npc, held));
    EXPECT_EQ(held.move_dynamic, !slide);
  }
}

/** \} */

/** Plan item 6's checkpoint (2026-11-20): what Auto Cells cost. The crowd bench's 200 x 200 m
 * level with its 63 blocks, built whole at cells of 0.2 m (today's), 0.15 m (radius / 2 for 0.3 m
 * NPCs), 0.2 m (radius / 1.5) and 0.1 m (the floor, radius 0.2 m and less); its first step's time
 * and 300 steps of 200 NPCs walking across. NAV_BENCH_ROUNDS rounds, the median. */
TEST(KX_Navigation, DISABLED_AutoCellsCost)
{
  const char *rounds_text = getenv("NAV_BENCH_ROUNDS");
  const int rounds = rounds_text ? std::max(1, atoi(rounds_text)) : 3;
  Level level;
  level.floors.push_back({MT_Vector3(-100, -100, 0), MT_Vector3(100, 100, 0)});
  for (int x = -80; x <= 80; x += 20) {
    for (int y = -60; y <= 60; y += 20) {
      level.boxes.push_back({MT_Vector3(x - 2, y - 2, 0), MT_Vector3(x + 2, y + 2, 3)});
    }
  }
  for (const float cell : {0.2f, 0.15f, 0.1f}) {
    std::vector<double> build_ms, step_ms;
    for (int round = 0; round < rounds; round++) {
      KX_Navigation::Settings settings = TestSettings();
      settings.cell_size = cell;
      settings.cell_height = cell * 0.5f;
      std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, settings);
      LN_JobPool pool;
      pool.SetHelperCount(7);
      Objects objects;
      std::vector<KX_GameObject *> npcs;
      for (int i = 0; i < 200; i++) {
        npcs.push_back(&objects.Add(MT_Vector3(-90.0f + 0.9f * float(i), -95.0f, 0)));
      }
      auto step = [&]() {
        for (size_t i = 0; i < npcs.size(); i++) {
          navigation->Submit(GoTo(*npcs[i], MT_Vector3(90.0f - 0.9f * float(i), 95.0f, 0), 3.0f));
        }
        navigation->Step(1.0f / 60.0f, &pool);
      };
      const auto t0 = std::chrono::steady_clock::now();
      step();
      build_ms.push_back(
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
      const auto t1 = std::chrono::steady_clock::now();
      for (int s = 0; s < 300; s++) {
        step();
      }
      step_ms.push_back(
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count() /
          300.0);
    }
    std::sort(build_ms.begin(), build_ms.end());
    std::sort(step_ms.begin(), step_ms.end());
    printf("auto cells bench: cell %.2f m: first step (the build) %.1f ms, then %.3f ms a step "
           "(200 NPCs, 7 helpers, median of %d)\n",
           cell,
           build_ms[build_ms.size() / 2],
           step_ms[step_ms.size() / 2],
           rounds);
  }
}

/* -------------------------------------------------------------------- */
/** \name Item 12: Character NPCs pass each other (2026-11-20)
 * \{ */

namespace {

/** Character NPCs (real Jolt capsules, radius 0.3 m) through a door `width` wide in the wall at
 * x 0: `per_side` from the left to the right, and as many from the right to the left when
 * `both_ways`; Dense or Polite. `jitter` moves each start a few centimetres (another start of a
 * chaotic crowd). Returns when the last one was through (-1: not within `seconds`), the longest
 * time with nobody getting through, people a second through the door until half were through,
 * and the deepest two capsules got into each other. */
struct CharacterDoorRun {
  float last_through = -1.0f, longest_wait = 0.0f, flow = 0.0f, deepest_overlap = 0.0f;
  int through = 0;
};

CharacterDoorRun RunCharacterDoor(const int per_side,
                                  const bool both_ways,
                                  const float width,
                                  const bool dense,
                                  const float seconds,
                                  const int jitter,
                                  const bool kinematic_right = false)
{
  Level level = DoorLevel(width);
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  Objects objects;
  World world;
  world.StaticBox(objects.Add(MT_Vector3(8, 0, -0.5f)), MT_Vector3(22, 10, 0.5f));
  const float wall = (10.0f - width * 0.5f) * 0.5f;
  world.StaticBox(objects.Add(MT_Vector3(0, -10.0f + wall, 1)), MT_Vector3(0.5f, wall, 1));
  world.StaticBox(objects.Add(MT_Vector3(0, 10.0f - wall, 1)), MT_Vector3(0.5f, wall, 1));
  navigation->SetPhysicsEnvironmentForTests(&world.environment);
  std::vector<KX_GameObject *> npcs;
  std::vector<MT_Vector3> goals;
  std::vector<int> side;
  uint32_t random = 0x9e3779b9u * uint32_t(jitter + 1);
  auto wobble = [&]() {
    random ^= random << 13;
    random ^= random >> 17;
    random ^= random << 5;
    return jitter == 0 ? 0.0f : (float(random % 1000) / 1000.0f - 0.5f) * 0.1f;
  };
  for (int s = 0; s < (both_ways ? 2 : 1); s++) {
    const float out = s == 0 ? -1.0f : 1.0f;
    for (int i = 0; i < per_side; i++) {
      /* 8 a row (1 m apart) across the door's middle, rows 0.9 m apart from 1.5 m off it. */
      const float x = out * (1.5f + float(i / 8) * 0.9f) + wobble();
      const float y = -3.5f + float(i % 8) * 1.0f + wobble();
      KX_GameObject &npc = objects.Add(MT_Vector3(x, y, 0.95f));
      if (s == 1 && kinematic_right) {
        /* (A Kinematic NPC, slid: a capsule as the Characters'. Its body follows its object at
         * each step as in a game: the world's step writes the controllers it holds; unheld, the
         * bodies stood where they started, posts the Characters stopped at.) */
        Body &body = world.Add(
            npc, new JPH::CapsuleShape(0.6f, 0.3f), JPH::EMotionType::Kinematic);
        world.environment.AddController(&body.controller);
      }
      else {
        EXPECT_NE(world.Character(npc), nullptr);
      }
      npcs.push_back(&npc);
      side.push_back(s);
      /* Each its own place on the other side (no queue at a common goal). */
      goals.push_back(
          MT_Vector3(-out * (5.0f + float(i / 8) * 1.2f), -4.2f + float(i % 8) * 1.2f, 0));
    }
  }
  world.environment.GetPhysicsSystem()->OptimizeBroadPhase();
  CharacterDoorRun run;
  const float dt = 1.0f / 60.0f;
  int last_crossing = 0, first_step = -1, half_step = -1;
  std::vector<bool> crossed(npcs.size(), false);
  int step = 0;
  for (; step < int(seconds * 60.0f); step++) {
    for (size_t i = 0; i < npcs.size(); i++) {
      KX_NavRequest request = GoTo(*npcs[i], goals[i], 1.6f);
      request.dense = dense;
      request.arrive_distance = 0.4f;
      navigation->Submit(request);
    }
    navigation->Step(dt, nullptr);
    world.Step(npcs);
    int through = 0;
    for (size_t i = 0; i < npcs.size(); i++) {
      const MT_Vector3 p = npcs[i]->NodeGetWorldPosition();
      EXPECT_TRUE(std::isfinite(float(p.x())));
      const bool now = side[i] == 0 ? p.x() > 0.6f : p.x() < -0.6f;
      if (now && !crossed[i]) {
        crossed[i] = true;
        run.longest_wait = std::max(run.longest_wait, float(step - last_crossing) * dt);
        last_crossing = step;
        first_step = first_step < 0 ? step : first_step;
      }
      through += crossed[i] ? 1 : 0;
      for (size_t j = i + 1; j < npcs.size(); j++) {
        run.deepest_overlap = std::max(run.deepest_overlap,
                                       0.6f - distance_2d(p, npcs[j]->NodeGetWorldPosition()));
      }
    }
    if (half_step < 0 && through * 2 >= int(npcs.size())) {
      half_step = step;
    }
    run.through = through;
    if (through == int(npcs.size())) {
      run.last_through = float(step) * dt;
      break;
    }
  }
  if (half_step > first_step && first_step >= 0) {
    run.flow = float(npcs.size() / 2) / (float(half_step - first_step) * dt);
  }
  return run;
}

}  // namespace

/** The door scenes measured for item 12 (2026-11-20, a temporary rig): Character NPCs in Dense,
 * the last one through and the longest time nobody got through. Before (capsules blocking each
 * other): 27 s (6.6), 61 s (12), 179 s (43), 205 s (55); NPCs without bodies: 20 s (1.4), 20 s
 * (1.5), 20 s (3.7), 65 s (9). Now the navigating Characters pass each other's bodies: every
 * start through within about the bodiless times, nobody standing for long. Two starts each. */
TEST(KX_Navigation, CharacterNpcsPassEachOtherAtADoor)
{
  struct Scene {
    const char *name;
    int per_side;
    bool both_ways;
    float width;
    float bodiless_last;
  };
  for (const Scene &scene : {Scene{"30 one way, 1.2 m", 30, false, 1.2f, 20.0f},
                             Scene{"20 + 20, 1.6 m", 20, true, 1.6f, 20.0f},
                             Scene{"10 + 10, 1.2 m", 10, true, 1.2f, 20.0f},
                             Scene{"20 + 20, 1.2 m", 20, true, 1.2f, 65.0f}})
  {
    for (const int jitter : {0, 1}) {
      const CharacterDoorRun run = RunCharacterDoor(
          scene.per_side, scene.both_ways, scene.width, true, 150.0f, jitter);
      printf("Characters, Dense, %-17s start %d: all through in %.1f s, longest with nobody "
             "through %.1f s, %.2f a second (%.2f a metre), deepest overlap %.2f m\n",
             scene.name, jitter, run.last_through, run.longest_wait, run.flow,
             run.flow / scene.width, run.deepest_overlap);
      SCOPED_TRACE(std::string(scene.name) + " start " + std::to_string(jitter));
      EXPECT_GT(run.last_through, 0.0f);
      /* (Chaotic crowds: within twice the bodiless time and a margin, never the minute-long
       * face-offs.) */
      EXPECT_LT(run.last_through, scene.bodiless_last * 2.0f + 10.0f);
      EXPECT_LT(run.longest_wait, 15.0f);
      /* The navigation keeps them apart (a body's width is 0.6 m). */
      EXPECT_LT(run.deepest_overlap, 0.3f);
    }
  }
}

/** Item 12's realism check: one way through a 1.2 m and a 1.6 m door, Dense; real crowds pass
 * about 1.3 to 2 people a second for every metre of door width. Printed; held loosely (crowd
 * numbers swing). */
TEST(KX_Navigation, CharacterNpcsFlowThroughADoorAsPeopleDo)
{
  for (const float width : {1.2f, 1.6f}) {
    const CharacterDoorRun run = RunCharacterDoor(40, false, width, true, 120.0f, 0);
    printf("Characters, Dense, 40 one way, %.1f m door: %.2f a second, %.2f a second a metre, "
           "all through in %.1f s\n",
           width, run.flow, run.flow / width, run.last_through);
    EXPECT_GT(run.last_through, 0.0f) << width;
    EXPECT_GT(run.flow / width, 0.9f) << width;
    EXPECT_LT(run.flow / width, 3.0f) << width;
  }
}

/** Item 12: a mixed crowd, 10 Character NPCs one way and 10 Kinematic NPCs (slid) the other,
 * through a 1.2 m door, Dense: before, each Kinematic one slid through the Characters while they
 * were stopped by it; now the Characters pass them too: all through, nobody standing long. */
TEST(KX_Navigation, ACharacterNpcPassesAWalkingKinematicNpc)
{
  for (const int jitter : {0, 1}) {
    const CharacterDoorRun run = RunCharacterDoor(10, true, 1.2f, true, 120.0f, jitter, true);
    printf("Characters and Kinematic NPCs, Dense, 10 + 10, 1.2 m, start %d: all through in "
           "%.1f s, longest with nobody through %.1f s, deepest overlap %.2f m\n",
           jitter, run.last_through, run.longest_wait, run.deepest_overlap);
    EXPECT_GT(run.last_through, 0.0f) << jitter;
    EXPECT_LT(run.last_through, 50.0f) << jitter;
    EXPECT_LT(run.longest_wait, 15.0f) << jitter;
  }
}

/** Item 12: what the navigation hands to physics as passing through each other: a Character
 * and a Kinematic NPC walking; not one that arrived and stands, nor one not asked, nor a
 * Dynamic NPC (the others push it by their mass). */
TEST(KX_Navigation, OnlyMovingNpcsPhysicsCannotPushPassThrough)
{
  Level level;
  level.floors.push_back({MT_Vector3(-10, -10, 0), MT_Vector3(10, 10, 0)});
  std::unique_ptr<KX_Navigation> navigation = MakeNavigation(level, TestSettings());
  Objects objects;
  World world;
  world.StaticBox(objects.Add(MT_Vector3(0, 0, -0.5f)), MT_Vector3(10, 10, 0.5f));
  navigation->SetPhysicsEnvironmentForTests(&world.environment);
  KX_GameObject &walker = objects.Add(MT_Vector3(-6, -4, 0.95f));
  KX_GameObject &arrived = objects.Add(MT_Vector3(-6, 0, 0.95f));
  KX_GameObject &idle = objects.Add(MT_Vector3(-6, 4, 0.95f));
  for (KX_GameObject *character : {&walker, &arrived, &idle}) {
    ASSERT_NE(world.Character(*character), nullptr);
  }
  KX_GameObject &cart = objects.Add(MT_Vector3(6, -4, 0.5f));
  Body &cart_body = world.Add(
      cart, new JPH::BoxShape(JPH::Vec3(0.3f, 0.5f, 0.3f)), JPH::EMotionType::Kinematic);
  world.environment.AddController(&cart_body.controller);
  KX_GameObject &ball = objects.Add(MT_Vector3(6, 4, 0.3f));
  Body &ball_body = world.Add(
      ball, new JPH::BoxShape(JPH::Vec3(0.3f, 0.3f, 0.3f)), JPH::EMotionType::Dynamic);
  for (int step = 0; step < 30; step++) {
    navigation->Submit(GoTo(walker, MT_Vector3(6, -8, 0)));
    navigation->Submit(GoTo(arrived, MT_Vector3(-6, 0, 0)));
    navigation->Submit(GoTo(cart, MT_Vector3(-6, -8, 0)));
    navigation->Submit(GoTo(ball, MT_Vector3(0, 8, 0)));
    navigation->Step(1.0f / 60.0f, nullptr);
    world.Step({&walker, &arrived, &idle, &cart, &ball});
  }
  auto handed = [&](KX_GameObject &object) {
    const JoltPhysicsController *controller = static_cast<const JoltPhysicsController *>(
        object.GetPhysicsController());
    return world.environment.IsNavigatingBody(controller->GetBodyID());
  };
  EXPECT_TRUE(handed(walker));
  EXPECT_TRUE(handed(cart));
  EXPECT_FALSE(handed(arrived));
  EXPECT_FALSE(handed(idle));
  EXPECT_FALSE(handed(ball));
  (void)ball_body;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Item 5: obstacles take their real shape (2026-11-20)
 * \{ */

namespace {

/** A level of one 20 m floor and a navigation built on it (0.2 m cells, NPCs of 0.3 m). */
std::unique_ptr<KX_Navigation> FloorNavigation(Level &level)
{
  level.floors.push_back({MT_Vector3(-10, -10, 0), MT_Vector3(10, 10, 0)});
  return MakeNavigation(level, TestSettings());
}

/** Where a ray along x at height `y` first meets the navmesh's edge (-10: nothing cut). */
float CutStartsAt(KX_Navigation &navigation, const float y)
{
  KX_NavMesh *navmesh = navigation.GetSceneNavMesh();
  if (navmesh == nullptr) {
    return 0.0f;
  }
  float fraction = 0.0f;
  MT_Vector3 hit, normal;
  if (!navmesh->Raycast(MT_Vector3(-6, y, 0), MT_Vector3(6, y, 0), fraction, hit, normal)) {
    return -10.0f;
  }
  return float(hit.x());
}

/** Whether `p` (on the floor) lies on the navmesh. */
bool Walkable(KX_Navigation &navigation, const MT_Vector3 &p)
{
  KX_NavMesh *navmesh = navigation.GetSceneNavMesh();
  MT_Vector3 nearest;
  return navmesh != nullptr && navmesh->NearestPoint(p, nearest) && distance_2d(nearest, p) < 0.05f;
}

void Settle(KX_Navigation &navigation, const int steps = 3)
{
  for (int step = 0; step < steps; step++) {
    navigation.Step(1.0f / 60.0f, nullptr);
  }
}

}  // namespace

/** A cylinder obstacle (radius 1 m) is cut out as a circle (Detour's cylinder), a box of the same
 * size as a square: (1.25, 1.25) off their middles, outside the circle widened by the NPCs' room
 * (0.3 m at 0.2 m cells: 0.4 m) and the outline's simplification (a quarter metre), inside the
 * square's. Through the middle both cut from 1.4 m. */
TEST(KX_Navigation, ARoundObstacleIsCutAsACylinder)
{
  for (const bool round : {true, false}) {
    SCOPED_TRACE(round ? "cylinder" : "box");
    Level level;
    std::unique_ptr<KX_Navigation> navigation = FloorNavigation(level);
    Objects objects;
    World world;
    KX_GameObject &post = objects.Add(MT_Vector3(0, 0, 0.5f));
    world.Add(post,
              round ? JPH::RefConst<JPH::Shape>(new JPH::CylinderShape(0.5f, 1.0f, 0.0f)) :
                      JPH::RefConst<JPH::Shape>(new JPH::BoxShape(JPH::Vec3(1, 0.5f, 1), 0.0f)),
              JPH::EMotionType::Static);
    navigation->SetPhysicsEnvironmentForTests(&world.environment);
    navigation->SetObstacle(&post, true);
    Settle(*navigation);
    EXPECT_NEAR(CutStartsAt(*navigation, 0.0f), -1.4f, 0.15f);
    EXPECT_FALSE(Walkable(*navigation, MT_Vector3(0.9f, 0, 0)));
    EXPECT_EQ(Walkable(*navigation, MT_Vector3(1.25f, 1.25f, 0)), round);
    EXPECT_EQ(Walkable(*navigation, MT_Vector3(-1.25f, -1.25f, 0)), round);
  }
}

/** A lying log (a capsule 3.6 m long, 0.3 m round, turned to lie along x) is cut out along its
 * length (the box round its turned corners: 1.8 m each way, 0.3 m across, both widened by the
 * NPCs' 0.4 m), not as an upright circle; a box tilted 60 degrees takes the footprint of its
 * turned corners (0.97 m across). */
TEST(KX_Navigation, ATiltedLogsFootprint)
{
  Level level;
  std::unique_ptr<KX_Navigation> navigation = FloorNavigation(level);
  Objects objects;
  World world;
  KX_GameObject &log = objects.Add(MT_Vector3(0, -4, 0.3f));
  log.NodeSetLocalOrientation(MT_Matrix3x3(MT_Vector3(0.0f, float(M_PI) * 0.5f, 0.0f)));
  log.NodeUpdateGS(0.0);
  world.Add(log, new JPH::CapsuleShape(1.5f, 0.3f), JPH::EMotionType::Static);
  /* A plank 2 m x 0.2 m x 1 m (x, y, z) tilted 60 degrees about x: seen from above 2 m by
   * 0.2 cos 60 + 1 sin 60 = 0.97 m. */
  /* (Resting on the floor: its lowest corner 0.34 m under its middle.) */
  KX_GameObject &plank = objects.Add(MT_Vector3(0, 4, 0.34f));
  plank.NodeSetLocalOrientation(MT_Matrix3x3(MT_Vector3(float(M_PI) / 3.0f, 0.0f, 0.0f)));
  plank.NodeUpdateGS(0.0);
  world.Add(plank, new JPH::BoxShape(JPH::Vec3(1.0f, 0.5f, 0.1f), 0.0f), JPH::EMotionType::Static);
  navigation->SetPhysicsEnvironmentForTests(&world.environment);
  navigation->SetObstacle(&log, true);
  navigation->SetObstacle(&plank, true);
  Settle(*navigation);
  /* Along the log: from 1.8 + 0.4 m; at its ends' line too. */
  EXPECT_NEAR(CutStartsAt(*navigation, -4.0f), -2.2f, 0.15f);
  EXPECT_FALSE(Walkable(*navigation, MT_Vector3(1.6f, -4.0f, 0)));
  /* Beside it, 1.1 m off its line: nothing cut (0.3 + 0.4 m, the outline within 0.26 m). */
  EXPECT_EQ(CutStartsAt(*navigation, -5.1f), -10.0f);
  EXPECT_EQ(CutStartsAt(*navigation, -2.9f), -10.0f);
  /* The plank: 0.4 m off its middle across is cut (half its footprint 0.48 + 0.4), 1.2 m off
   * is not; along x from 1.0 + 0.4 m. */
  EXPECT_NEAR(CutStartsAt(*navigation, 4.4f), -1.4f, 0.15f);
  EXPECT_EQ(CutStartsAt(*navigation, 5.2f), -10.0f);
  EXPECT_EQ(CutStartsAt(*navigation, 2.8f), -10.0f);
}

/** Keep Away (Physics > Navigation, the object's): the cut and the avoidance keep that much more
 * room. A post of 0.5 m: cut from 0.9 m (the NPCs' 0.4 m); with Keep Away 1 m from 1.9 m. Moving (avoided, not cut),
 * an NPC crossing its way keeps 1 m more from it. */
TEST(KX_Navigation, KeepAwayWidensTheCutAndTheAvoidance)
{
  for (const float keep : {0.0f, 1.0f}) {
    SCOPED_TRACE(keep);
    Level level;
    std::unique_ptr<KX_Navigation> navigation = FloorNavigation(level);
    Objects objects;
    World world;
    KX_GameObject &post = objects.Add(MT_Vector3(0, 0, 0.5f));
    world.Add(post, new JPH::CylinderShape(0.5f, 0.5f, 0.0f), JPH::EMotionType::Static);
    KX_NavObjectSettings settings;
    settings.keep_away = keep;
    post.SetNavSettingsForTests(settings, nullptr);
    navigation->SetPhysicsEnvironmentForTests(&world.environment);
    navigation->SetObstacle(&post, true);
    Settle(*navigation);
    EXPECT_NEAR(CutStartsAt(*navigation, 0.0f), -(0.5f + keep + 0.4f), 0.15f);
  }
  for (const float keep : {0.0f, 1.0f}) {
    SCOPED_TRACE(keep);
    Level level;
    std::unique_ptr<KX_Navigation> navigation = FloorNavigation(level);
    Objects objects;
    World world;
    KX_GameObject &cart = objects.Add(MT_Vector3(0, -3, 0.5f));
    world.Add(cart, new JPH::CylinderShape(0.5f, 0.5f, 0.0f), JPH::EMotionType::Kinematic);
    KX_NavObjectSettings settings;
    settings.keep_away = keep;
    cart.SetNavSettingsForTests(settings, nullptr);
    navigation->SetPhysicsEnvironmentForTests(&world.environment);
    navigation->SetObstacle(&cart, true);
    KX_GameObject &npc = objects.Add(MT_Vector3(-6, 0, 0));
    float nearest = 1e9f;
    for (int step = 0; step < 60 * 8; step++) {
      /* (It moves every step: avoided, never cut.) */
      Place(cart, MT_Vector3(0, -3.0f + 0.75f * float(step) / 60.0f, 0.5f));
      navigation->Submit(GoTo(npc, MT_Vector3(6, 0, 0), 1.5f));
      navigation->Step(1.0f / 60.0f, nullptr);
      nearest = std::min(nearest,
                         distance_2d(npc.NodeGetWorldPosition(), cart.NodeGetWorldPosition()));
    }
    if (keep > 0.0f) {
      EXPECT_GT(nearest, 0.5f + keep + 0.3f - 0.25f);
    }
    else {
      EXPECT_LT(nearest, 1.3f);
    }
  }
}

/** An obstacle with no shape and no mesh (an Empty) is a zone of its Keep Away. */
TEST(KX_Navigation, AnEmptyObstacleIsAZoneOfKeepAway)
{
  Level level;
  std::unique_ptr<KX_Navigation> navigation = FloorNavigation(level);
  Objects objects;
  KX_GameObject &zone = objects.Add(MT_Vector3(0, 0, 0));
  blender::Object empty{};
  std::snprintf(empty.id.name, sizeof(empty.id.name), "OBZone");
  zone.SetBlenderObject(&empty);
  KX_NavObjectSettings settings;
  settings.keep_away = 2.0f;
  zone.SetNavSettingsForTests(settings, nullptr);
  navigation->SetObstacle(&zone, true);
  Settle(*navigation);
  EXPECT_NEAR(CutStartsAt(*navigation, 0.0f), -2.4f, 0.15f);
  zone.SetBlenderObject(nullptr);
}

/** A long cart (4 m x 1 m) moving along its length: an NPC walking beside it, 1.3 m off its
 * line, passes alongside (avoided as a row of circles), not pushed out round one circle as wide
 * as the cart is long (2.3 m off its middle). */
TEST(KX_Navigation, ALongCartIsAvoidedAlongItsLength)
{
  Level level;
  std::unique_ptr<KX_Navigation> navigation = FloorNavigation(level);
  Objects objects;
  World world;
  KX_GameObject &cart = objects.Add(MT_Vector3(-2, 0, 0.5f));
  world.Add(cart, new JPH::BoxShape(JPH::Vec3(2.0f, 0.5f, 0.5f), 0.0f), JPH::EMotionType::Kinematic);
  navigation->SetPhysicsEnvironmentForTests(&world.environment);
  navigation->SetObstacle(&cart, true);
  KX_GameObject &npc = objects.Add(MT_Vector3(-8, 1.3f, 0));
  float widest = 0.0f, closest = 1e9f;
  for (int step = 0; step < 60 * 8; step++) {
    Place(cart, MT_Vector3(-2.0f + 0.25f * float(step) / 60.0f, 0, 0.5f));
    navigation->Submit(GoTo(npc, MT_Vector3(8, 1.3f, 0), 2.0f));
    navigation->Step(1.0f / 60.0f, nullptr);
    const MT_Vector3 p = npc.NodeGetWorldPosition(), c = cart.NodeGetWorldPosition();
    if (std::fabs(float(p.x() - c.x())) < 1.5f) {
      widest = std::max(widest, float(std::fabs(p.y())));
      closest = std::min(closest, float(std::fabs(p.y())));
    }
  }
  EXPECT_GT(closest, 0.5f + 0.3f - 0.1f);
  EXPECT_LT(widest, 1.8f);
  EXPECT_GT(npc.NodeGetWorldPosition().x(), 7.0f);
}

/** \} */
