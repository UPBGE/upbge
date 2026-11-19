/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Contributor(s): UPBGE Contributors
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file JoltDebugDraw.cpp
 *  \ingroup physjolt
 */

#include "JoltDebugDraw.h"

#include "JoltPhysicsConfig.h"

#include <Jolt/Jolt.h>

JPH_SUPPRESS_WARNINGS

#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/ShapeFilter.h>
#include <Jolt/Physics/Collision/TransformedShape.h>
#include <Jolt/Physics/Constraints/TwoBodyConstraint.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Vehicle/VehicleConstraint.h>

#include "JoltCharacter.h"
#include "JoltConstraint.h"
#include "JoltMathUtils.h"
#include "JoltPhysicsController.h"
#include "JoltPhysicsEnvironment.h"
#include "JoltVehicle.h"
#include "KX_ClientObjectInfo.h"
#include "KX_GameObject.h"
#include "PHY_IMotionState.h"

#include <cmath>
#include <unordered_set>
#include <vector>

namespace JoltDebugDraw {

/* -------------------------------------------------------------------- */
/** \name Colors
 * \{ */

const MT_Vector4 &SensorColor()
{
  static const MT_Vector4 color(1.0f, 1.0f, 0.0f, 1.0f);
  return color;
}
const MT_Vector4 &StaticColor()
{
  static const MT_Vector4 color(0.2f, 0.9f, 0.2f, 1.0f);
  return color;
}
const MT_Vector4 &CharacterColor()
{
  static const MT_Vector4 color(1.0f, 0.3f, 0.9f, 1.0f);
  return color;
}
const MT_Vector4 &SleepingColor()
{
  static const MT_Vector4 color(0.55f, 0.55f, 0.55f, 1.0f);
  return color;
}
const MT_Vector4 &KinematicColor()
{
  static const MT_Vector4 color(0.1f, 0.55f, 1.0f, 1.0f);
  return color;
}
const MT_Vector4 &DynamicColor()
{
  static const MT_Vector4 color(1.0f, 0.55f, 0.0f, 1.0f);
  return color;
}
const MT_Vector4 &AabbColor()
{
  static const MT_Vector4 color(0.45f, 0.45f, 0.75f, 1.0f);
  return color;
}
const MT_Vector4 &ContactColor()
{
  static const MT_Vector4 color(1.0f, 1.0f, 1.0f, 1.0f);
  return color;
}
const MT_Vector4 &ConstraintColor()
{
  static const MT_Vector4 color(0.8f, 0.5f, 1.0f, 1.0f);
  return color;
}
const MT_Vector4 &WheelColor()
{
  static const MT_Vector4 color(0.0f, 1.0f, 1.0f, 1.0f);
  return color;
}
const MT_Vector4 &GroundNormalColor()
{
  static const MT_Vector4 color(0.6f, 1.0f, 0.2f, 1.0f);
  return color;
}
const MT_Vector4 &WallNormalColor()
{
  static const MT_Vector4 color(1.0f, 0.25f, 0.25f, 1.0f);
  return color;
}

/** \} */

namespace {

/* -------------------------------------------------------------------- */
/** \name Lines
 * \{ */

struct Drawer {
  const Settings &settings;
  const LineSink &sink;
  int triangles_left = kMaxTrianglesPerDraw;

  void Line(const JPH::RVec3 &from, const JPH::RVec3 &to, const MT_Vector4 &color) const
  {
    sink(JoltMath::ToMT(JPH::Vec3(from)), JoltMath::ToMT(JPH::Vec3(to)), color);
  }

  /** An arc of radius \a radius around \a center (local), in the plane of the local unit axes
   * \a a and \a b, from angle \a start to \a end; the local frame placed at \a position and
   * turned by \a rotation. */
  void Arc(const JPH::RVec3 &position,
           const JPH::Quat &rotation,
           const JPH::Vec3 &center,
           const JPH::Vec3 &a,
           const JPH::Vec3 &b,
           const float radius,
           const float start,
           const float end,
           const int segments,
           const MT_Vector4 &color) const
  {
    const auto point = [&](const float angle) {
      return position + rotation * (center + radius * (std::cos(angle) * a + std::sin(angle) * b));
    };
    JPH::RVec3 previous = point(start);
    for (int i = 1; i <= segments; i++) {
      const JPH::RVec3 next = point(start + (end - start) * float(i) / float(segments));
      Line(previous, next, color);
      previous = next;
    }
  }

  void Ring(const JPH::RVec3 &position,
            const JPH::Quat &rotation,
            const JPH::Vec3 &center,
            const JPH::Vec3 &a,
            const JPH::Vec3 &b,
            const float radius,
            const MT_Vector4 &color) const
  {
    Arc(position, rotation, center, a, b, radius, 0.0f, 2.0f * JPH::JPH_PI, kCircleSegments, color);
  }

  /** A small cross at \a point. */
  void Cross(const JPH::RVec3 &point, const float size, const MT_Vector4 &color) const
  {
    Line(point - JPH::Vec3(size, 0, 0), point + JPH::Vec3(size, 0, 0), color);
    Line(point - JPH::Vec3(0, size, 0), point + JPH::Vec3(0, size, 0), color);
    Line(point - JPH::Vec3(0, 0, size), point + JPH::Vec3(0, 0, size), color);
  }

  /** An arrow from \a from along the unit \a direction (Jolt space), \a length long. */
  void Arrow(const JPH::RVec3 &from,
             const JPH::Vec3 &direction,
             const float length,
             const MT_Vector4 &color) const
  {
    const JPH::RVec3 tip = from + direction * length;
    Line(from, tip, color);
    JPH::Vec3 side = direction.Cross(std::abs(direction.GetY()) < 0.9f ? JPH::Vec3::sAxisY() :
                                                                         JPH::Vec3::sAxisX());
    side = side.NormalizedOr(JPH::Vec3::sAxisZ());
    const JPH::RVec3 back = tip - direction * (0.2f * length);
    Line(tip, back + side * (0.1f * length), color);
    Line(tip, back - side * (0.1f * length), color);
  }

  void Box(const JPH::RVec3 &position,
           const JPH::Quat &rotation,
           const JPH::Vec3 &center,
           const JPH::Vec3 &half,
           const MT_Vector4 &color) const
  {
    JPH::RVec3 corners[8];
    for (int i = 0; i < 8; i++) {
      const JPH::Vec3 local(i & 1 ? half.GetX() : -half.GetX(),
                            i & 2 ? half.GetY() : -half.GetY(),
                            i & 4 ? half.GetZ() : -half.GetZ());
      corners[i] = position + rotation * (center + local);
    }
    static const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                     {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const auto &edge : edges) {
      Line(corners[edge[0]], corners[edge[1]], color);
    }
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shapes
 * \{ */

/** The leaf shapes of a body's shape, with their world transforms and scales. */
class LeafCollector final : public JPH::TransformedShapeCollector {
 public:
  void AddHit(const JPH::TransformedShape &shape) override
  {
    leaves.push_back(shape);
  }
  std::vector<JPH::TransformedShape> leaves;
};

/** Every triangle Jolt gives for \a leaf (in \a box, world space), as its three edges. */
void DrawTriangles(Drawer &drawer,
                   const JPH::TransformedShape &leaf,
                   const JPH::AABox &box,
                   const MT_Vector4 &color)
{
  if (drawer.triangles_left <= 0) {
    return;
  }
  constexpr int kBatch = 256;
  static_assert(kBatch >= JPH::Shape::cGetTrianglesMinTrianglesRequested);
  JPH::Shape::GetTrianglesContext context;
  leaf.GetTrianglesStart(context, box, JPH::RVec3::sZero());
  JPH::Float3 vertices[kBatch * 3];
  int count;
  while (drawer.triangles_left > 0 &&
         (count = leaf.GetTrianglesNext(context, kBatch, vertices)) > 0)
  {
    for (int i = 0; i < count && drawer.triangles_left > 0; i++, drawer.triangles_left--) {
      const JPH::RVec3 a(JPH::Vec3(vertices[i * 3 + 0]));
      const JPH::RVec3 b(JPH::Vec3(vertices[i * 3 + 1]));
      const JPH::RVec3 c(JPH::Vec3(vertices[i * 3 + 2]));
      drawer.Line(a, b, color);
      drawer.Line(b, c, color);
      drawer.Line(c, a, color);
    }
  }
}

void DrawLeaf(Drawer &drawer, const JPH::TransformedShape &leaf, const MT_Vector4 &color)
{
  const JPH::Shape *shape = leaf.mShape;
  if (shape == nullptr) {
    return;
  }
  /* The leaf's own space: its points relative to its center of mass, scaled, turned, placed. */
  const JPH::RVec3 position = leaf.mShapePositionCOM;
  const JPH::Quat rotation = leaf.mShapeRotation;
  const JPH::Vec3 scale = leaf.GetShapeScale();
  const JPH::Vec3 x = JPH::Vec3::sAxisX(), y = JPH::Vec3::sAxisY(), z = JPH::Vec3::sAxisZ();
  const JPH::Vec3 origin = JPH::Vec3::sZero();
  switch (shape->GetSubType()) {
    case JPH::EShapeSubType::Sphere: {
      const float radius = static_cast<const JPH::SphereShape *>(shape)->GetRadius() *
                           std::abs(scale.GetX());
      drawer.Ring(position, rotation, origin, x, y, radius, color);
      drawer.Ring(position, rotation, origin, y, z, radius, color);
      drawer.Ring(position, rotation, origin, x, z, radius, color);
      return;
    }
    case JPH::EShapeSubType::Box: {
      const JPH::Vec3 half = static_cast<const JPH::BoxShape *>(shape)->GetHalfExtent() * scale;
      drawer.Box(position, rotation, origin, half.Abs(), color);
      return;
    }
    case JPH::EShapeSubType::Capsule: {
      /* Along its Y (the object's Z): two rings, four sides, and each cap's two half circles. */
      const JPH::CapsuleShape *capsule = static_cast<const JPH::CapsuleShape *>(shape);
      const float uniform = std::abs(scale.GetX());
      const float radius = capsule->GetRadius() * uniform;
      const float half = capsule->GetHalfHeightOfCylinder() * uniform;
      const JPH::Vec3 top = y * half, bottom = -y * half;
      drawer.Ring(position, rotation, top, x, z, radius, color);
      drawer.Ring(position, rotation, bottom, x, z, radius, color);
      for (const JPH::Vec3 &side : {x, -x, z, -z}) {
        drawer.Line(position + rotation * (top + side * radius),
                    position + rotation * (bottom + side * radius),
                    color);
      }
      const int half_segments = kCircleSegments / 2;
      for (const JPH::Vec3 &across : {x, z}) {
        drawer.Arc(position, rotation, top, across, y, radius, 0.0f, JPH::JPH_PI, half_segments,
                   color);
        drawer.Arc(position, rotation, bottom, across, -y, radius, 0.0f, JPH::JPH_PI,
                   half_segments, color);
      }
      return;
    }
    case JPH::EShapeSubType::Cylinder: {
      const JPH::CylinderShape *cylinder = static_cast<const JPH::CylinderShape *>(shape);
      const float radius = cylinder->GetRadius() * std::abs(scale.GetX());
      const float half = cylinder->GetHalfHeight() * std::abs(scale.GetY());
      const JPH::Vec3 top = y * half, bottom = -y * half;
      drawer.Ring(position, rotation, top, x, z, radius, color);
      drawer.Ring(position, rotation, bottom, x, z, radius, color);
      for (const JPH::Vec3 &side : {x, -x, z, -z}) {
        drawer.Line(position + rotation * (top + side * radius),
                    position + rotation * (bottom + side * radius),
                    color);
      }
      return;
    }
    case JPH::EShapeSubType::ConvexHull: {
      /* Its faces' outlines (a cone is one too). */
      const JPH::ConvexHullShape *hull = static_cast<const JPH::ConvexHullShape *>(shape);
      std::vector<JPH::uint> indices;
      for (JPH::uint face = 0; face < hull->GetNumFaces(); face++) {
        indices.resize(hull->GetNumVerticesInFace(face));
        const JPH::uint count = hull->GetFaceVertices(
            face, JPH::uint(indices.size()), indices.data());
        for (JPH::uint i = 0; i < count; i++) {
          const JPH::Vec3 a = hull->GetPoint(indices[i]) * scale;
          const JPH::Vec3 b = hull->GetPoint(indices[(i + 1) % count]) * scale;
          drawer.Line(position + rotation * a, position + rotation * b, color);
        }
      }
      return;
    }
    case JPH::EShapeSubType::Empty:
      return;
    case JPH::EShapeSubType::Mesh:
    case JPH::EShapeSubType::HeightField: {
      /* A level mesh: only near the camera. */
      JPH::AABox box = JPH::AABox::sBiggest();
      if (drawer.settings.view_center != nullptr) {
        box = JPH::AABox::sFromTwoPoints(
            JoltMath::ToJolt(*drawer.settings.view_center) - JPH::Vec3::sReplicate(kMeshDrawRange),
            JoltMath::ToJolt(*drawer.settings.view_center) + JPH::Vec3::sReplicate(kMeshDrawRange));
      }
      DrawTriangles(drawer, leaf, box, color);
      return;
    }
    default:
      /* Soft bodies, tapered shapes, anything else: the triangles Jolt gives. */
      DrawTriangles(drawer, leaf, JPH::AABox::sBiggest(), color);
      return;
  }
}

/** \a shape placed with its center of mass at \a centerOfMass, turned by \a rotation. */
void DrawShape(Drawer &drawer,
               const JPH::Shape &shape,
               const JPH::RVec3 &centerOfMass,
               const JPH::Quat &rotation,
               const MT_Vector4 &color)
{
  LeafCollector collector;
  shape.CollectTransformedShapes(JPH::AABox::sBiggest(),
                                 JPH::Vec3(centerOfMass),
                                 rotation,
                                 JPH::Vec3::sOne(),
                                 JPH::SubShapeIDCreator(),
                                 collector,
                                 JPH::ShapeFilter());
  for (const JPH::TransformedShape &leaf : collector.leaves) {
    DrawLeaf(drawer, leaf, color);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Bodies
 * \{ */

PHY_IPhysicsController *ControllerOf(const JPH::Body &body)
{
  KX_ClientObjectInfo *info = reinterpret_cast<KX_ClientObjectInfo *>(body.GetUserData());
  KX_GameObject *object = info ? info->m_gameobject : nullptr;
  return object ? object->GetPhysicsController() : nullptr;
}

const MT_Vector4 &BodyColor(const JPH::Body &body)
{
  if (body.IsSensor()) {
    return SensorColor();
  }
  if (body.IsStatic()) {
    return StaticColor();
  }
  if (const PHY_IPhysicsController *controller = ControllerOf(body)) {
    if (controller->IsCharacter()) {
      return CharacterColor();
    }
  }
  if (!body.IsActive()) {
    return SleepingColor();
  }
  return body.IsKinematic() ? KinematicColor() : DynamicColor();
}

void DrawBodies(Drawer &drawer, JoltPhysicsEnvironment &env)
{
  JPH::PhysicsSystem *system = env.GetPhysicsSystem();
  const JPH::BodyLockInterface &locks = system->GetBodyLockInterface();

  /* Sensor-type objects Logic Nodes query outside the world: drawn where their objects are, as
   * the queries meet them (JoltQueryTransformedShape). */
  std::unordered_set<JPH::uint32> offWorldSensors;
  for (JoltPhysicsController *sensor : env.GetQuerySensors()) {
    if (sensor != nullptr && sensor->IsLogicObjectSensorActive() &&
        !sensor->GetBodyID().IsInvalid())
    {
      offWorldSensors.insert(sensor->GetBodyID().GetIndexAndSequenceNumber());
    }
  }

  const bool drawAabbs = (drawer.settings.debug_mode & kDrawAabbBit) != 0;
  JPH::BodyIDVector bodyIDs;
  system->GetBodies(bodyIDs);
  for (const JPH::BodyID &id : bodyIDs) {
    JPH::BodyLockRead lock(locks, id);
    if (!lock.Succeeded()) {
      continue;
    }
    const JPH::Body &body = lock.GetBody();
    const JPH::Shape *shape = body.GetShape();
    if (shape == nullptr) {
      continue;
    }
    if (body.IsInBroadPhase()) {
      DrawShape(drawer, *shape, body.GetCenterOfMassPosition(), body.GetRotation(),
                BodyColor(body));
      if (drawAabbs) {
        const JPH::AABox bounds = body.GetWorldSpaceBounds();
        drawer.Box(JPH::RVec3(bounds.GetCenter()), JPH::Quat::sIdentity(), JPH::Vec3::sZero(),
                   bounds.GetExtent(), AabbColor());
      }
      continue;
    }
    /* Outside the world: Add Object's templates, suspended physics, compound children are not
     * drawn; only the sensors the queries test. */
    if (offWorldSensors.count(id.GetIndexAndSequenceNumber()) == 0) {
      continue;
    }
    const JoltPhysicsController *controller = static_cast<const JoltPhysicsController *>(
        ControllerOf(body));
    const PHY_IMotionState *motionState = controller ? controller->GetMotionState() : nullptr;
    if (motionState == nullptr) {
      continue;
    }
    const JPH::Quat rotation =
        JoltMath::ToJolt(motionState->GetWorldOrientation().getRotation()).Normalized();
    const JPH::RVec3 origin = JoltMath::ToJoltR(motionState->GetWorldPosition());
    DrawShape(drawer, *shape, origin + rotation * shape->GetCenterOfMass(), rotation,
              SensorColor());
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Contacts, constraints, wheels, characters
 * \{ */

void DrawContact(const Drawer &drawer, const JPH::RVec3 &point, const JPH::Vec3 &normal)
{
  drawer.Cross(point, kContactSize, ContactColor());
  drawer.Line(point, point + normal.NormalizedOr(JPH::Vec3::sZero()) * kContactNormalLength,
              ContactColor());
}

void DrawContacts(const Drawer &drawer, JoltPhysicsEnvironment &env)
{
  std::vector<JoltPhysicsEnvironment::DebugContact> contacts;
  env.CopyDebugContacts(contacts);
  for (const JoltPhysicsEnvironment::DebugContact &contact : contacts) {
    DrawContact(drawer, contact.point, contact.normal);
  }
}

void DrawConstraints(const Drawer &drawer, JoltPhysicsEnvironment &env)
{
  for (const auto &entry : env.GetConstraintsById()) {
    const JPH::Constraint *constraint = entry.second ? entry.second->GetConstraint() : nullptr;
    if (constraint == nullptr || !constraint->GetEnabled() ||
        constraint->GetType() != JPH::EConstraintType::TwoBodyConstraint)
    {
      continue;
    }
    const JPH::TwoBodyConstraint *twoBody = static_cast<const JPH::TwoBodyConstraint *>(
        constraint);
    const JPH::Body *body1 = twoBody->GetBody1();
    const JPH::Body *body2 = twoBody->GetBody2();
    if (body1 == nullptr || body2 == nullptr) {
      continue;
    }
    /* Each body's anchor: its frame of the constraint (the hinge's axis, a slider's direction)
     * as red, green and blue axes; the line between the anchors (apart when the constraint
     * gives way or springs); thin lines to the bodies' centers. */
    const JPH::RMat44 frame1 = body1->GetCenterOfMassTransform() *
                               twoBody->GetConstraintToBody1Matrix();
    const JPH::RMat44 frame2 = body2->GetCenterOfMassTransform() *
                               twoBody->GetConstraintToBody2Matrix();
    const JPH::RVec3 anchor1 = frame1.GetTranslation();
    const JPH::RVec3 anchor2 = frame2.GetTranslation();
    drawer.Line(anchor1, anchor2, ConstraintColor());
    drawer.Line(body1->GetCenterOfMassPosition(), anchor1, ConstraintColor());
    if (body2 != &JPH::Body::sFixedToWorld) {
      drawer.Line(body2->GetCenterOfMassPosition(), anchor2, ConstraintColor());
    }
    static const MT_Vector4 axisColors[3] = {MT_Vector4(1.0f, 0.2f, 0.2f, 1.0f),
                                             MT_Vector4(0.2f, 1.0f, 0.2f, 1.0f),
                                             MT_Vector4(0.3f, 0.3f, 1.0f, 1.0f)};
    const JPH::Vec3 axes[3] = {frame1.GetAxisX(), frame1.GetAxisY(), frame1.GetAxisZ()};
    for (int i = 0; i < 3; i++) {
      drawer.Line(anchor1,
                  anchor1 + axes[i].NormalizedOr(JPH::Vec3::sZero()) * kConstraintAxisLength,
                  axisColors[i]);
    }
  }
}

void DrawWheels(const Drawer &drawer, JoltPhysicsEnvironment &env)
{
  for (JoltVehicle *vehicle : env.GetVehicles()) {
    JPH::VehicleConstraint *constraint = vehicle ? vehicle->GetVehicleConstraint() : nullptr;
    if (constraint == nullptr) {
      continue;
    }
    const JPH::Wheels &wheels = constraint->GetWheels();
    for (JPH::uint i = 0; i < JPH::uint(wheels.size()); i++) {
      const JPH::Wheel *wheel = wheels[i];
      if (wheel == nullptr || wheel->GetSettings() == nullptr) {
        continue;
      }
      /* The wheel's model space: X its axle, Y up. */
      const JPH::RMat44 transform = constraint->GetWheelWorldTransform(
          i, JPH::Vec3::sAxisX(), JPH::Vec3::sAxisY());
      const JPH::RVec3 center = transform.GetTranslation();
      const float radius = wheel->GetSettings()->mRadius;
      drawer.Ring(center, transform.GetQuaternion(), JPH::Vec3::sZero(), JPH::Vec3::sAxisY(),
                  JPH::Vec3::sAxisZ(), radius, WheelColor());
      /* A spoke, so its turn shows. */
      drawer.Line(center, center + transform.GetAxisY() * radius, WheelColor());
      if (wheel->HasContact()) {
        drawer.Line(center, wheel->GetContactPosition(), ContactColor());
      }
    }
  }
}

void DrawCharacters(const Drawer &drawer, JoltPhysicsEnvironment &env)
{
  std::vector<JoltCharacterContact> contacts;
  for (JoltCharacter *character : env.GetCharacters()) {
    JPH::CharacterVirtual *virtualCharacter = character ? character->GetCharacterVirtual() :
                                                          nullptr;
    if (virtualCharacter == nullptr) {
      continue;
    }
    /* (Its shape is its body's, drawn with the bodies.) */
    if (character->OnGround()) {
      const JPH::Vec3 normal = JoltMath::ToJolt(character->GetGroundNormal());
      if (!normal.IsNearZero()) {
        drawer.Arrow(virtualCharacter->GetGroundPosition(), normal.Normalized(),
                     kCharacterArrowLength, GroundNormalColor());
      }
    }
    if (character->OnWall()) {
      const JPH::Vec3 normal = JoltMath::ToJolt(character->GetWallNormal());
      if (!normal.IsNearZero()) {
        drawer.Arrow(virtualCharacter->GetPosition(), normal.Normalized(), kCharacterArrowLength,
                     WallNormalColor());
      }
    }
    /* What it touches (its contacts are no body contacts): the normals toward it. */
    character->GetTouchingContacts(contacts);
    for (const JoltCharacterContact &contact : contacts) {
      DrawContact(drawer, contact.position, -contact.normal);
    }
  }
}

/** \} */

}  // namespace

void DrawWorld(JoltPhysicsEnvironment &env, const Settings &settings, const LineSink &sink)
{
  if (env.GetPhysicsSystem() == nullptr || !sink) {
    return;
  }
  Drawer drawer{settings, sink};
  DrawBodies(drawer, env);
  DrawContacts(drawer, env);
  DrawConstraints(drawer, env);
  DrawWheels(drawer, env);
  DrawCharacters(drawer, env);
}

}  // namespace JoltDebugDraw
