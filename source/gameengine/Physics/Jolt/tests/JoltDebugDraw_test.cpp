/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** Physics Visualization on Jolt (JoltDebugDraw, 2026-11-18): every body in the world is drawn as
 * its collision shape (before: as its bounding box), bodies outside the world are not drawn
 * (Add Object's templates, suspended physics) except the Sensor-type objects Logic Nodes query
 * (where their objects are), colors say what a body is, triangle meshes only near the camera and
 * within a budget, and the contacts, constraints, wheels and a character's Ground and Wall
 * normals. A real Jolt world; the lines go into a list. */

#include "testing/testing.h"

#include <Jolt/Jolt.h>
JPH_SUPPRESS_WARNINGS

#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>

#include "DNA_scene_types.h"

#include "JoltCharacter.h"
#include "JoltDebugDraw.h"
#include "JoltDefaultMotionState.h"
#include "JoltMathUtils.h"
#include "JoltPhysicsController.h"
#include "JoltPhysicsEnvironment.h"
#include "JoltVehicle.h"
#include "KX_ClientObjectInfo.h"
#include "KX_GameObject.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace {

constexpr float kStep = 1.0f / 60.0f;

/** A body with its controller and object (positions are Blender's, Z up). */
struct Thing {
  Thing(JoltPhysicsEnvironment &environment,
        JPH::RefConst<JPH::Shape> shape,
        const MT_Vector3 &position,
        const JPH::EMotionType type,
        const bool add = true,
        const bool sensor = false)
      : client_info(&object,
                    sensor                             ? KX_ClientObjectInfo::SENSOR :
                    type == JPH::EMotionType::Static ? KX_ClientObjectInfo::STATIC :
                                                         KX_ClientObjectInfo::ACTOR)
  {
    const JoltBroadPhaseLayer layer = type == JPH::EMotionType::Static ? JOLT_BP_STATIC :
                                                                         JOLT_BP_DYNAMIC;
    JPH::BodyCreationSettings settings(shape,
                                       JoltMath::ToJolt(position),
                                       JPH::Quat::sIdentity(),
                                       type,
                                       JoltMakeObjectLayer(1, 1, layer));
    settings.mIsSensor = sensor;
    if (type == JPH::EMotionType::Dynamic) {
      settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
      settings.mMassPropertiesOverride.mMass = 10.0f;
    }
    JPH::BodyInterface &bodies = environment.GetBodyInterface();
    body_id = bodies.CreateBody(settings)->GetID();
    if (add) {
      bodies.AddBody(body_id,
                     type == JPH::EMotionType::Static ? JPH::EActivation::DontActivate :
                                                        JPH::EActivation::Activate);
    }
    motion_state = new JoltDefaultMotionState();
    motion_state->SetWorldPosition(position);
    motion_state->SetWorldOrientation(MT_Matrix3x3::Identity());
    controller.SetMotionState(motion_state);
    controller.SetEnvironment(&environment);
    controller.SetBodyID(body_id);
    controller.SetDynamic(type == JPH::EMotionType::Dynamic);
    controller.SetKinematic(type == JPH::EMotionType::Kinematic);
    controller.SetOriginalMotionType(type);
    controller.SetBroadPhaseCategory(layer);
    controller.SetShape(shape, nullptr);
    controller.SetCollisionGroup(1);
    controller.SetCollisionMask(1);
    controller.SetSensorFlag(sensor);
    controller.SetSensorPhysicsType(sensor);
    controller.SetNewClientInfo(&client_info);
    object.SetPhysicsController(&controller);
    environment.AddController(&controller);
    bodies.SetUserData(body_id, reinterpret_cast<JPH::uint64>(&client_info));
  }

  ~Thing()
  {
    object.SetPhysicsController(nullptr);
    controller.SetNewClientInfo(nullptr);
  }

  KX_GameObject object;
  KX_ClientObjectInfo client_info;
  JoltPhysicsController controller;
  JPH::BodyID body_id;
  JoltDefaultMotionState *motion_state = nullptr;
};

JPH::RefConst<JPH::Shape> Box(const float x, const float y, const float z)
{
  /* Blender half extents (Z up) as a Jolt box (Y up). */
  return new JPH::BoxShape(JPH::Vec3(x, z, y), 0.0f);
}

struct World {
  explicit World(const bool visualize = true)
      : environment(&scene, 0, 4096, 8192, 8192, 32, visualize)
  {
    environment.SetGravity(0.0f, 0.0f, -9.81f);
  }

  Thing &Add(JPH::RefConst<JPH::Shape> shape,
             const MT_Vector3 &position,
             const JPH::EMotionType type = JPH::EMotionType::Static,
             const bool add = true,
             const bool sensor = false)
  {
    things.push_back(std::make_unique<Thing>(environment, shape, position, type, add, sensor));
    return *things.back();
  }

  void Step(const int steps = 1)
  {
    for (int i = 0; i < steps; i++) {
      ASSERT_TRUE(environment.ProceedDeltaTime(0.0, kStep, kStep));
    }
  }

  blender::Scene scene{};
  JoltPhysicsEnvironment environment;
  std::vector<std::unique_ptr<Thing>> things;
};

struct Line {
  MT_Vector3 from, to;
  MT_Vector4 color;
};

std::vector<Line> Draw(JoltPhysicsEnvironment &environment,
                       const int mode = 1,
                       const MT_Vector3 *view_center = nullptr)
{
  std::vector<Line> lines;
  JoltDebugDraw::Settings settings;
  settings.debug_mode = mode;
  settings.view_center = view_center;
  JoltDebugDraw::DrawWorld(
      environment, settings, [&](const MT_Vector3 &a, const MT_Vector3 &b, const MT_Vector4 &c) {
        lines.push_back({a, b, c});
      });
  return lines;
}

bool SameColor(const MT_Vector4 &a, const MT_Vector4 &b)
{
  return (a - b).length() < 1.0e-6f;
}

/** The ends of the lines of \a color with both ends within \a radius of \a center. */
std::vector<MT_Vector3> EndsNear(const std::vector<Line> &lines,
                                 const MT_Vector3 &center,
                                 const float radius,
                                 const MT_Vector4 *color = nullptr)
{
  std::vector<MT_Vector3> ends;
  for (const Line &line : lines) {
    if ((line.from - center).length() <= radius && (line.to - center).length() <= radius &&
        (color == nullptr || SameColor(line.color, *color)))
    {
      ends.push_back(line.from - center);
      ends.push_back(line.to - center);
    }
  }
  return ends;
}

size_t CountColor(const std::vector<Line> &lines, const MT_Vector4 &color)
{
  return size_t(std::count_if(lines.begin(), lines.end(), [&](const Line &line) {
    return SameColor(line.color, color);
  }));
}

/** Distance from \a p to the segment from (0, 0, -h) to (0, 0, h). */
float DistanceToAxis(const MT_Vector3 &p, const float h)
{
  const float z = std::clamp(p.z(), -h, h);
  return (p - MT_Vector3(0.0f, 0.0f, z)).length();
}

}  // namespace

/** Box, sphere, capsule, cylinder and convex hull (a cone) are drawn as themselves: every line
 * end lies on the shape's surface (its corners, its round), never a bounding box. */
TEST(JoltDebugDraw, EveryShapeIsDrawnAsItIs)
{
  World world(false);
  world.Add(Box(1.0f, 2.0f, 3.0f), MT_Vector3(0.0f, 0.0f, 0.0f));
  world.Add(new JPH::SphereShape(0.5f), MT_Vector3(10.0f, 0.0f, 0.0f));
  world.Add(new JPH::CapsuleShape(0.6f, 0.3f), MT_Vector3(20.0f, 0.0f, 0.0f));
  world.Add(new JPH::CylinderShape(0.8f, 0.4f, 0.0f), MT_Vector3(30.0f, 0.0f, 0.0f));
  /* A cone as the converter builds it: a hull of a ring and an apex. */
  JPH::Array<JPH::Vec3> cone;
  for (int i = 0; i < 8; i++) {
    const float angle = float(i) * JPH::JPH_PI / 4.0f;
    cone.push_back(JPH::Vec3(std::cos(angle), -1.0f, std::sin(angle)));
  }
  cone.push_back(JPH::Vec3(0.0f, 1.0f, 0.0f));
  const JPH::RefConst<JPH::Shape> hull = JPH::ConvexHullShapeSettings(cone, 0.0f).Create().Get();
  world.Add(hull, MT_Vector3(40.0f, 0.0f, 0.0f));

  const std::vector<Line> lines = Draw(world.environment);
  const MT_Vector4 &green = JoltDebugDraw::StaticColor();
  /* Nothing but shapes (no bounding boxes without the AABB bit). */
  EXPECT_EQ(CountColor(lines, JoltDebugDraw::AabbColor()), 0u);

  const std::vector<MT_Vector3> box = EndsNear(lines, MT_Vector3(0, 0, 0), 5.0f, &green);
  ASSERT_EQ(box.size(), 24u); /* 12 edges */
  for (const MT_Vector3 &p : box) {
    EXPECT_NEAR(std::abs(p.x()), 1.0f, 1.0e-5f);
    EXPECT_NEAR(std::abs(p.y()), 2.0f, 1.0e-5f);
    EXPECT_NEAR(std::abs(p.z()), 3.0f, 1.0e-5f);
  }

  const std::vector<MT_Vector3> sphere = EndsNear(lines, MT_Vector3(10, 0, 0), 5.0f, &green);
  ASSERT_GT(sphere.size(), 100u);
  for (const MT_Vector3 &p : sphere) {
    EXPECT_NEAR(p.length(), 0.5f, 1.0e-5f);
  }

  /* The capsule along Blender's Z: every end 0.3 from its axis segment, 0.9 up and down. */
  const std::vector<MT_Vector3> capsule = EndsNear(lines, MT_Vector3(20, 0, 0), 5.0f, &green);
  ASSERT_GT(capsule.size(), 100u);
  float top = -1.0f, bottom = 1.0f, side = 0.0f;
  for (const MT_Vector3 &p : capsule) {
    EXPECT_NEAR(DistanceToAxis(p, 0.6f), 0.3f, 1.0e-5f);
    top = std::max(top, p.z());
    bottom = std::min(bottom, p.z());
    side = std::max(side, std::abs(p.x()));
  }
  EXPECT_NEAR(top, 0.9f, 1.0e-5f);
  EXPECT_NEAR(bottom, -0.9f, 1.0e-5f);
  EXPECT_NEAR(side, 0.3f, 1.0e-5f);

  const std::vector<MT_Vector3> cylinder = EndsNear(lines, MT_Vector3(30, 0, 0), 5.0f, &green);
  ASSERT_GT(cylinder.size(), 50u);
  for (const MT_Vector3 &p : cylinder) {
    EXPECT_NEAR(std::hypot(p.x(), p.y()), 0.4f, 1.0e-5f);
    EXPECT_NEAR(std::abs(p.z()), 0.8f, 1.0e-5f);
  }

  /* The hull's ends are its points (placed by its center of mass). */
  const std::vector<MT_Vector3> cone_ends = EndsNear(lines, MT_Vector3(40, 0, 0), 5.0f, &green);
  ASSERT_GT(cone_ends.size(), 16u);
  for (const MT_Vector3 &p : cone_ends) {
    float nearest = FLT_MAX;
    for (const JPH::Vec3 &point : cone) {
      nearest = std::min(nearest, (p - JoltMath::ToMT(point)).length());
    }
    EXPECT_LT(nearest, 1.0e-4f) << p.x() << " " << p.y() << " " << p.z();
  }
}

/** A compound's parts are drawn where they are (moved and turned), a scaled shape at its scale;
 * the AABB bit adds each body's bounding box. */
TEST(JoltDebugDraw, CompoundPartsAndScaledShapesAreWhereTheyAre)
{
  World world(false);
  JPH::StaticCompoundShapeSettings compound;
  compound.AddShape(JoltMath::ToJolt(MT_Vector3(2.0f, 0.0f, 0.0f)),
                    JPH::Quat::sIdentity(),
                    new JPH::BoxShape(JPH::Vec3::sReplicate(0.5f), 0.0f));
  compound.AddShape(JoltMath::ToJolt(MT_Vector3(-2.0f, 0.0f, 0.0f)),
                    JPH::Quat::sRotation(JPH::Vec3::sAxisY(), JPH::JPH_PI / 4.0f),
                    new JPH::BoxShape(JPH::Vec3::sReplicate(0.5f), 0.0f));
  world.Add(compound.Create().Get(), MT_Vector3(0.0f, 0.0f, 0.0f));
  world.Add(new JPH::ScaledShape(new JPH::SphereShape(0.5f), JPH::Vec3::sReplicate(2.0f)),
            MT_Vector3(10.0f, 0.0f, 0.0f));

  const std::vector<Line> lines = Draw(world.environment);
  const std::vector<MT_Vector3> right = EndsNear(lines, MT_Vector3(2, 0, 0), 1.0f);
  ASSERT_EQ(right.size(), 24u);
  for (const MT_Vector3 &p : right) {
    EXPECT_NEAR(std::abs(p.x()), 0.5f, 1.0e-5f);
    EXPECT_NEAR(std::abs(p.y()), 0.5f, 1.0e-5f);
    EXPECT_NEAR(std::abs(p.z()), 0.5f, 1.0e-5f);
  }
  /* The turned box: its corners 0.866 from its middle, its top and bottom flat. */
  const std::vector<MT_Vector3> left = EndsNear(lines, MT_Vector3(-2, 0, 0), 1.0f);
  ASSERT_EQ(left.size(), 24u);
  bool turned = false;
  for (const MT_Vector3 &p : left) {
    EXPECT_NEAR(p.length(), std::sqrt(0.75f), 1.0e-5f);
    EXPECT_NEAR(std::abs(p.z()), 0.5f, 1.0e-5f);
    turned |= std::abs(std::abs(p.x()) - 0.5f) > 0.1f;
  }
  EXPECT_TRUE(turned);
  const std::vector<MT_Vector3> scaled = EndsNear(lines, MT_Vector3(10, 0, 0), 3.0f);
  ASSERT_GT(scaled.size(), 100u);
  for (const MT_Vector3 &p : scaled) {
    EXPECT_NEAR(p.length(), 1.0f, 1.0e-5f);
  }

  /* With the AABB bit (Bullet's DBG_DrawAabb), the bounding boxes too. */
  const std::vector<Line> with_boxes = Draw(world.environment, 1 | JoltDebugDraw::kDrawAabbBit);
  EXPECT_EQ(CountColor(with_boxes, JoltDebugDraw::AabbColor()), 24u);
  EXPECT_EQ(with_boxes.size(), lines.size() + 24u);
}

/** A body outside the world is not drawn: Add Object's template (made, never added), an object
 * whose physics is suspended (taken out). A Sensor-type object Logic Nodes query outside the
 * world is drawn where its object is (its body's own pose is stale), in the sensor color. */
TEST(JoltDebugDraw, BodiesOutsideTheWorldAreNotDrawn)
{
  World world(false);
  world.Add(Box(0.5f, 0.5f, 0.5f), MT_Vector3(0.0f, 0.0f, 0.0f));
  world.Add(Box(0.5f, 0.5f, 0.5f), MT_Vector3(10.0f, 0.0f, 0.0f), JPH::EMotionType::Dynamic,
            /*add=*/false);
  Thing &suspended = world.Add(Box(0.5f, 0.5f, 0.5f), MT_Vector3(20.0f, 0.0f, 0.0f),
                               JPH::EMotionType::Dynamic);
  Thing &sensor = world.Add(Box(0.5f, 0.5f, 0.5f), MT_Vector3(30.0f, 0.0f, 0.0f),
                            JPH::EMotionType::Static, /*add=*/false, /*sensor=*/true);
  sensor.controller.SetLogicObjectSensorActive(true);
  sensor.controller.SetLogicCollisionQueryActive(true);
  /* Its object moved on, its body stayed (nothing moves a body outside the world). */
  sensor.motion_state->SetWorldPosition(MT_Vector3(35.0f, 0.0f, 0.0f));

  std::vector<Line> lines = Draw(world.environment);
  EXPECT_EQ(EndsNear(lines, MT_Vector3(0, 0, 0), 2.0f).size(), 24u);
  EXPECT_EQ(EndsNear(lines, MT_Vector3(10, 0, 0), 2.0f).size(), 0u) << "a template";
  EXPECT_EQ(EndsNear(lines, MT_Vector3(20, 0, 0), 2.0f).size(), 24u);
  EXPECT_EQ(EndsNear(lines, MT_Vector3(30, 0, 0), 2.0f).size(), 0u) << "a sensor's stale body";
  EXPECT_EQ(EndsNear(lines, MT_Vector3(35, 0, 0), 2.0f, &JoltDebugDraw::SensorColor()).size(),
            24u);

  suspended.controller.SuspendPhysics(false);
  lines = Draw(world.environment);
  EXPECT_EQ(EndsNear(lines, MT_Vector3(20, 0, 0), 2.0f).size(), 0u) << "suspended physics";
  suspended.controller.RestorePhysics();
  lines = Draw(world.environment);
  EXPECT_EQ(EndsNear(lines, MT_Vector3(20, 0, 0), 2.0f).size(), 24u);

  /* A sensor Logic Nodes do not query (inactive layer) is not drawn either. */
  sensor.controller.SetLogicCollisionQueryActive(false);
  lines = Draw(world.environment);
  EXPECT_EQ(EndsNear(lines, MT_Vector3(35, 0, 0), 2.0f).size(), 0u);
}

/** The colors say what a body is: static green (before, static bodies were never active and so
 * drawn grey), Dynamic orange, at rest grey, Kinematic blue, a sensor yellow, a Character's body
 * magenta. */
TEST(JoltDebugDraw, ColorsSayWhatABodyIs)
{
  World world(false);
  world.Add(Box(0.5f, 0.5f, 0.5f), MT_Vector3(0, 0, 0));
  Thing &moving = world.Add(Box(0.5f, 0.5f, 0.5f), MT_Vector3(10, 0, 0), JPH::EMotionType::Dynamic);
  Thing &resting = world.Add(
      Box(0.5f, 0.5f, 0.5f), MT_Vector3(20, 0, 0), JPH::EMotionType::Dynamic);
  world.Add(Box(0.5f, 0.5f, 0.5f), MT_Vector3(30, 0, 0), JPH::EMotionType::Kinematic);
  world.Add(Box(0.5f, 0.5f, 0.5f), MT_Vector3(40, 0, 0), JPH::EMotionType::Static, true, true);
  Thing &hero = world.Add(new JPH::CapsuleShape(0.6f, 0.3f), MT_Vector3(50, 0, 0),
                          JPH::EMotionType::Kinematic);
  ASSERT_NE(world.environment.CreateCharacter(&hero.controller, 0.3f), nullptr);
  world.environment.GetBodyInterface().ActivateBody(moving.body_id);
  world.environment.GetBodyInterface().DeactivateBody(resting.body_id);

  const std::vector<Line> lines = Draw(world.environment);
  const auto color_at = [&](const float x, const MT_Vector4 &color) {
    return EndsNear(lines, MT_Vector3(x, 0, 0), 2.0f, &color).size();
  };
  EXPECT_EQ(color_at(0, JoltDebugDraw::StaticColor()), 24u);
  EXPECT_EQ(color_at(10, JoltDebugDraw::DynamicColor()), 24u);
  EXPECT_EQ(color_at(20, JoltDebugDraw::SleepingColor()), 24u);
  EXPECT_EQ(color_at(30, JoltDebugDraw::KinematicColor()), 24u);
  EXPECT_EQ(color_at(40, JoltDebugDraw::SensorColor()), 24u);
  EXPECT_GT(color_at(50, JoltDebugDraw::CharacterColor()), 100u);
}

/** A triangle mesh is drawn near the camera only (kMeshDrawRange), and every draw takes at most
 * kMaxTrianglesPerDraw triangles. */
TEST(JoltDebugDraw, MeshesAreDrawnNearTheCameraAndWithinABudget)
{
  World world(false);
  /* A 380 x 380 m grid of 2 m squares: 72,200 triangles. */
  constexpr int kCells = 190;
  JPH::VertexList vertices;
  JPH::IndexedTriangleList triangles;
  for (int j = 0; j <= kCells; j++) {
    for (int i = 0; i <= kCells; i++) {
      vertices.push_back(JPH::Float3(float(i - kCells / 2) * 2.0f, 0.0f, float(j - kCells / 2) * 2.0f));
    }
  }
  for (int j = 0; j < kCells; j++) {
    for (int i = 0; i < kCells; i++) {
      const JPH::uint32 a = j * (kCells + 1) + i, b = a + 1, c = a + kCells + 1, d = c + 1;
      triangles.push_back(JPH::IndexedTriangle(a, c, b));
      triangles.push_back(JPH::IndexedTriangle(b, c, d));
    }
  }
  world.Add(JPH::MeshShapeSettings(std::move(vertices), std::move(triangles)).Create().Get(),
            MT_Vector3(0.0f, 0.0f, 0.0f));

  const MT_Vector3 camera(30.0f, 20.0f, 5.0f);
  const std::vector<Line> near = Draw(world.environment, 1, &camera);
  ASSERT_GT(near.size(), 3000u);
  /* (Jolt gives a mesh's triangles by blocks of its tree: a few a cell or two past the edge.) */
  for (const Line &line : near) {
    for (const MT_Vector3 &p : {line.from, line.to}) {
      EXPECT_LE(std::abs(p.x() - camera.x()), JoltDebugDraw::kMeshDrawRange + 8.0f);
      EXPECT_LE(std::abs(p.y() - camera.y()), JoltDebugDraw::kMeshDrawRange + 8.0f);
    }
  }
  /* About the 100 m square around the camera of the 380 m grid. */
  EXPECT_LT(near.size(), size_t(72200 * 3) / 8);
  /* Without a camera: all of it, but no more than the budget (3 lines a triangle). */
  const std::vector<Line> all = Draw(world.environment);
  EXPECT_EQ(all.size(), size_t(JoltDebugDraw::kMaxTrianglesPerDraw) * 3);
}

/** With Physics Visualization on, the last step's contacts are drawn (a cross and the normal);
 * off, none are kept. */
TEST(JoltDebugDraw, TheLastStepsContactsAreDrawn)
{
  for (const bool visualize : {true, false}) {
    SCOPED_TRACE(visualize);
    World world(visualize);
    world.Add(Box(10.0f, 10.0f, 0.5f), MT_Vector3(0.0f, 0.0f, -0.5f));
    world.Add(Box(0.5f, 0.5f, 0.5f), MT_Vector3(0.0f, 0.0f, 0.5f), JPH::EMotionType::Dynamic);
    world.Step(5);
    std::vector<JoltPhysicsEnvironment::DebugContact> contacts;
    world.environment.CopyDebugContacts(contacts);
    if (!visualize) {
      EXPECT_TRUE(contacts.empty());
      continue;
    }
    /* The box's four corners on the floor, of the last step only (not five steps' worth). */
    ASSERT_GE(contacts.size(), 4u);
    EXPECT_LE(contacts.size(), 8u);
    for (const JoltPhysicsEnvironment::DebugContact &contact : contacts) {
      const MT_Vector3 point = JoltMath::ToMT(JPH::Vec3(contact.point));
      EXPECT_NEAR(std::abs(point.x()), 0.5f, 0.02f);
      EXPECT_NEAR(std::abs(point.y()), 0.5f, 0.02f);
      EXPECT_NEAR(point.z(), 0.0f, 0.02f);
      EXPECT_NEAR(std::abs(JoltMath::ToMT(contact.normal).z()), 1.0f, 1.0e-3f);
    }
    const std::vector<Line> lines = Draw(world.environment);
    EXPECT_EQ(CountColor(lines, JoltDebugDraw::ContactColor()), contacts.size() * 4);
    /* Turned off, the kept contacts go, and no step keeps any (no cost while it is off). */
    world.environment.SetDebugMode(0);
    world.environment.CopyDebugContacts(contacts);
    EXPECT_TRUE(contacts.empty());
    world.Step(3);
    world.environment.CopyDebugContacts(contacts);
    EXPECT_TRUE(contacts.empty());
  }
}

/** A constraint: the line between its anchors and its frame's axes at the first anchor. */
TEST(JoltDebugDraw, AConstraintIsDrawnBetweenItsAnchors)
{
  World world(false);
  Thing &first = world.Add(Box(0.5f, 0.5f, 0.5f), MT_Vector3(0.0f, 0.0f, 2.0f),
                           JPH::EMotionType::Dynamic);
  Thing &second = world.Add(Box(0.5f, 0.5f, 0.5f), MT_Vector3(2.0f, 0.0f, 2.0f),
                            JPH::EMotionType::Dynamic);
  /* A point-to-point constraint at the first body's +X side (1 m out). */
  ASSERT_NE(world.environment.CreateConstraint(&first.controller,
                                               &second.controller,
                                               PHY_POINT2POINT_CONSTRAINT,
                                               1.0f,
                                               0.0f,
                                               0.0f,
                                               1.0f,
                                               0.0f,
                                               0.0f),
            nullptr);
  const std::vector<Line> lines = Draw(world.environment);
  const MT_Vector4 &violet = JoltDebugDraw::ConstraintColor();
  ASSERT_GE(CountColor(lines, violet), 2u);
  /* Both anchors at (1, 0, 2): the second body's anchor is its pivot in its own space. */
  bool found = false;
  for (const Line &line : lines) {
    if (SameColor(line.color, violet) && (line.from - MT_Vector3(1, 0, 2)).length() < 1.0e-4f &&
        (line.to - MT_Vector3(1, 0, 2)).length() < 1.0e-4f)
    {
      found = true;
    }
  }
  EXPECT_TRUE(found) << "the line between the anchors";
  /* Three axes of kConstraintAxisLength at the anchor. */
  int axes = 0;
  for (const Line &line : lines) {
    if ((line.from - MT_Vector3(1, 0, 2)).length() < 1.0e-4f &&
        std::abs((line.to - line.from).length() - JoltDebugDraw::kConstraintAxisLength) < 1.0e-4f)
    {
      axes++;
    }
  }
  EXPECT_EQ(axes, 3);
}

/** A vehicle's wheels: a ring of the wheel's radius where the wheel is, a line to the ground
 * where it touches. */
TEST(JoltDebugDraw, AVehiclesWheelsAreDrawnWhereTheyAre)
{
  World world(false);
  world.Add(Box(20.0f, 20.0f, 0.5f), MT_Vector3(0.0f, 0.0f, -0.5f));
  Thing &chassis = world.Add(Box(1.0f, 2.0f, 0.3f), MT_Vector3(0.0f, 0.0f, 1.0f),
                             JPH::EMotionType::Dynamic);
  JoltVehicle *vehicle = static_cast<JoltVehicle *>(
      world.environment.CreateVehicle(&chassis.controller));
  ASSERT_NE(vehicle, nullptr);
  for (const float x : {-1.0f, 1.0f}) {
    for (const float y : {-1.5f, 1.5f}) {
      vehicle->AddWheel(new JoltDefaultMotionState(),
                        MT_Vector3(x, y, -0.2f),
                        MT_Vector3(0.0f, 0.0f, -1.0f),
                        MT_Vector3(-1.0f, 0.0f, 0.0f),
                        0.4f,
                        0.35f,
                        y > 0.0f);
    }
  }
  ASSERT_TRUE(vehicle->Build());
  world.Step(30);
  const std::vector<Line> lines = Draw(world.environment);
  const MT_Vector4 &cyan = JoltDebugDraw::WheelColor();
  ASSERT_EQ(CountColor(lines, cyan), size_t(4 * (JoltDebugDraw::kCircleSegments + 1)));
  /* Each wheel's ring: its ends 0.35 from the wheel's center (the spoke's start). */
  for (int wheel = 0; wheel < 4; wheel++) {
    const MT_Vector3 center = vehicle->GetWheelPosition(wheel);
    const std::vector<MT_Vector3> ring = EndsNear(lines, center, 0.36f, &cyan);
    ASSERT_EQ(ring.size(), size_t(2 * (JoltDebugDraw::kCircleSegments + 1)));
    for (const MT_Vector3 &p : ring) {
      const float d = p.length();
      EXPECT_TRUE(std::abs(d - 0.35f) < 1.0e-4f || d < 1.0e-4f) << d;
    }
  }
  /* Standing on the floor: each wheel's line to its contact. */
  EXPECT_EQ(CountColor(lines, JoltDebugDraw::ContactColor()), 4u);
  /* (The environment deletes its vehicles.) */
}

/** A character walked into a wall: its Ground Normal arrow from where it stands (up), its Wall
 * Normal arrow from its middle (away from the wall), its touching contacts. */
TEST(JoltDebugDraw, ACharactersGroundAndWallNormalsAreArrows)
{
  World world(false);
  world.Add(Box(20.0f, 20.0f, 0.5f), MT_Vector3(0.0f, 0.0f, -0.5f));
  world.Add(Box(0.2f, 3.0f, 1.5f), MT_Vector3(1.7f, 0.0f, 1.5f));
  Thing &hero = world.Add(new JPH::CapsuleShape(0.6f, 0.3f), MT_Vector3(0.0f, 0.0f, 0.9f),
                          JPH::EMotionType::Kinematic);
  JoltCharacter *character = world.environment.CreateCharacter(&hero.controller, 0.3f);
  ASSERT_NE(character, nullptr);
  world.Step(5);
  character->SetWalkDirection(MT_Vector3(2.0f, 0.0f, 0.0f));
  world.Step(40);
  ASSERT_TRUE(character->OnGround());
  ASSERT_TRUE(character->OnWall());

  const std::vector<Line> lines = Draw(world.environment);
  const float length = JoltDebugDraw::kCharacterArrowLength;
  /* An arrow: its shaft and two head lines. */
  std::vector<Line> ground, wall;
  for (const Line &line : lines) {
    if (SameColor(line.color, JoltDebugDraw::GroundNormalColor())) {
      ground.push_back(line);
    }
    if (SameColor(line.color, JoltDebugDraw::WallNormalColor())) {
      wall.push_back(line);
    }
  }
  ASSERT_EQ(ground.size(), 3u);
  ASSERT_EQ(wall.size(), 3u);
  EXPECT_NEAR(ground[0].from.z(), 0.0f, 0.05f);
  EXPECT_NEAR((ground[0].to - ground[0].from).z(), length, 1.0e-4f);
  const MT_Vector3 middle = hero.controller.GetMotionState()->GetWorldPosition();
  EXPECT_NEAR((wall[0].from - middle).length(), 0.0f, 0.02f);
  EXPECT_NEAR((wall[0].to - wall[0].from).x(), -length, 1.0e-4f);
  /* Its contacts (the floor, the wall): crosses with normals. */
  EXPECT_GE(CountColor(lines, JoltDebugDraw::ContactColor()), 8u);
}
