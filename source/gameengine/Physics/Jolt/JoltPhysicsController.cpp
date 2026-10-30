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

/** \file JoltPhysicsController.cpp
 *  \ingroup physjolt
 */

#include "JoltPhysicsController.h"

#include <algorithm>
#include <array>
#include <optional>

#include <Jolt/Jolt.h>

JPH_SUPPRESS_WARNINGS

#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/MotionType.h>
#include <Jolt/Physics/Collision/Shape/MutableCompoundShape.h>
#include <Jolt/Physics/Collision/Shape/OffsetCenterOfMassShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/SoftBody/SoftBodyMotionProperties.h>

#include "JoltDefaultMotionState.h"
#include "JoltMathUtils.h"
#include "JoltPhysicsEnvironment.h"
#include "JoltShapeBuilder.h"
#include "JoltSoftBody.h"
#include "KX_ClientObjectInfo.h"
#include "KX_GameObject.h"
#include "KX_Scene.h"
#include "PHY_IMotionState.h"
#include "RAS_MeshObject.h"
#include "SG_Node.h"

#include "BKE_object.hh"
#include "BLI_bounds.hh"
#include "DNA_object_force_types.h"
#include "DNA_object_types.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace {

constexpr float JOLT_DEFAULT_MAX_LINEAR_VELOCITY = 500.0f;
constexpr float JOLT_DEFAULT_MAX_ANGULAR_VELOCITY = 0.25f * JPH::JPH_PI * 60.0f;

bool JoltPositionChanged(const JPH::RVec3 &current, const JPH::RVec3 &target)
{
  const double dx = double(current.GetX()) - double(target.GetX());
  const double dy = double(current.GetY()) - double(target.GetY());
  const double dz = double(current.GetZ()) - double(target.GetZ());
  return (dx * dx + dy * dy + dz * dz) > 1.0e-18;
}

bool JoltPositionIsFinite(const JPH::RVec3 &position)
{
  return std::isfinite(double(position.GetX())) && std::isfinite(double(position.GetY())) &&
         std::isfinite(double(position.GetZ()));
}

bool JoltQuaternionIsFinite(const JPH::Quat &rotation)
{
  return std::isfinite(rotation.GetX()) && std::isfinite(rotation.GetY()) &&
         std::isfinite(rotation.GetZ()) && std::isfinite(rotation.GetW()) &&
         rotation.LengthSq() > std::numeric_limits<float>::epsilon();
}

bool JoltRotationChanged(const JPH::Quat &current, const JPH::Quat &target)
{
  JPH::Quat normalizedCurrent = current.Normalized();
  JPH::Quat normalizedTarget = target.Normalized();
  if (normalizedCurrent.Dot(normalizedTarget) < 0.0f) {
    normalizedTarget = -normalizedTarget;
  }

  /* A dot-product threshold near one loses small rotations to float rounding
   * and made slow Kinematic rotation advance in visible pulses. Component-space
   * distance remains sign-invariant after the hemisphere correction above. */
  constexpr float toleranceSquared = 1.0e-12f;
  return (normalizedCurrent - normalizedTarget).LengthSq() > toleranceSquared;
}

/** Exactly the same scene-graph pose (NaN never matches). */
bool JoltSamePose(const MT_Vector3 &position,
                  const MT_Matrix3x3 &orientation,
                  const MT_Vector3 &otherPosition,
                  const MT_Matrix3x3 &otherOrientation)
{
  for (int i = 0; i < 3; i++) {
    if (position[i] != otherPosition[i] || orientation[i][0] != otherOrientation[i][0] ||
        orientation[i][1] != otherOrientation[i][1] ||
        orientation[i][2] != otherOrientation[i][2])
    {
      return false;
    }
  }
  return true;
}

bool JoltScaleIsValid(const MT_Vector3 &scale)
{
  return std::isfinite(scale[0]) && std::isfinite(scale[1]) && std::isfinite(scale[2]) &&
         std::abs(scale[0]) >= std::numeric_limits<float>::epsilon() &&
         std::abs(scale[1]) >= std::numeric_limits<float>::epsilon() &&
         std::abs(scale[2]) >= std::numeric_limits<float>::epsilon();
}

bool JoltScalesMatch(JPH::Vec3Arg a, JPH::Vec3Arg b)
{
  constexpr float tolerance = 1.0e-5f;
  for (int axis = 0; axis < 3; ++axis) {
    const float magnitude = std::max({1.0f, std::abs(a[axis]), std::abs(b[axis])});
    if (std::abs(a[axis] - b[axis]) > tolerance * magnitude) {
      return false;
    }
  }
  return true;
}

float JoltEffectiveLinearVelocityMax(const float value)
{
  return value > 0.0f ? value : JOLT_DEFAULT_MAX_LINEAR_VELOCITY;
}

float JoltEffectiveAngularVelocityMax(const float value)
{
  return value > 0.0f ? value : JOLT_DEFAULT_MAX_ANGULAR_VELOCITY;
}

bool JoltGameStateVectorIsFinite(const MT_Vector3 &value)
{
  return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]);
}

bool JoltGameStateVectorIsZero(const MT_Vector3 &value)
{
  constexpr float tolerance = 1.0e-6f;
  return std::abs(value[0]) <= tolerance && std::abs(value[1]) <= tolerance &&
         std::abs(value[2]) <= tolerance;
}

bool JoltGameStateVelocityFits(const MT_Vector3 &value, const float maximum)
{
  const double lengthSquared = double(value[0]) * double(value[0]) +
                               double(value[1]) * double(value[1]) +
                               double(value[2]) * double(value[2]);
  const double tolerance = std::max(1.0, double(maximum)) * 1.0e-5;
  const double acceptedMaximum = double(maximum) + tolerance;
  return lengthSquared <= acceptedMaximum * acceptedMaximum;
}

bool JoltGameStateFloatMatches(const float a, const float b)
{
  const float scale = std::max({1.0f, std::abs(a), std::abs(b)});
  return std::abs(a - b) <= 1.0e-5f * scale;
}

bool JoltGameStateModeIsValid(const PHY_DynamicsMode mode)
{
  switch (mode) {
    case PHY_DynamicsMode::Dynamic:
    case PHY_DynamicsMode::StaticCollider:
    case PHY_DynamicsMode::Ghost:
    case PHY_DynamicsMode::NoCollision:
      return true;
  }
  return false;
}

bool JoltGameStateRuntimeMotionTypeIsValid(const PHY_RuntimeMotionType type)
{
  switch (type) {
    case PHY_RuntimeMotionType::Authored:
    case PHY_RuntimeMotionType::Dynamic:
    case PHY_RuntimeMotionType::Kinematic:
      return true;
  }
  return false;
}

JPH::EAllowedDOFs JoltGameStateAllowedDOFs(const bool rotationEnabled,
                                           const std::array<bool, 6> &axisLocks)
{
  JPH::EAllowedDOFs dofs = JPH::EAllowedDOFs::All;
  if (axisLocks[0]) {
    dofs &= ~JPH::EAllowedDOFs::TranslationX;
  }
  if (axisLocks[1]) {
    dofs &= ~JPH::EAllowedDOFs::TranslationZ; /* Blender Y -> Jolt Z. */
  }
  if (axisLocks[2]) {
    dofs &= ~JPH::EAllowedDOFs::TranslationY; /* Blender Z -> Jolt Y. */
  }
  if (!rotationEnabled || axisLocks[3]) {
    dofs &= ~JPH::EAllowedDOFs::RotationX;
  }
  if (!rotationEnabled || axisLocks[4]) {
    dofs &= ~JPH::EAllowedDOFs::RotationZ; /* Blender Y -> Jolt Z. */
  }
  if (!rotationEnabled || axisLocks[5]) {
    dofs &= ~JPH::EAllowedDOFs::RotationY; /* Blender Z -> Jolt Y. */
  }
  return dofs;
}

JPH::EMotionType JoltGameStateBaseMotionType(const PHY_GameStateRigidBodyState &state,
                                             const JPH::EMotionType originalMotionType)
{
  switch (state.runtime_motion_type) {
    case PHY_RuntimeMotionType::Authored:
      return originalMotionType;
    case PHY_RuntimeMotionType::Dynamic:
      return JPH::EMotionType::Dynamic;
    case PHY_RuntimeMotionType::Kinematic:
      return JPH::EMotionType::Kinematic;
  }
  return JPH::EMotionType::Static;
}

JPH::EMotionType JoltGameStateMotionType(const PHY_GameStateRigidBodyState &state,
                                         const JPH::EMotionType originalMotionType)
{
  if (ELEM(state.mode, PHY_DynamicsMode::StaticCollider, PHY_DynamicsMode::Ghost)) {
    return JPH::EMotionType::Static;
  }
  return JoltGameStateBaseMotionType(state, originalMotionType);
}

bool JoltGameStateEffectiveSensor(const PHY_GameStateRigidBodyState &state)
{
  if (state.mode == PHY_DynamicsMode::Ghost) {
    return true;
  }
  if (state.mode == PHY_DynamicsMode::StaticCollider) {
    return false;
  }
  return state.ghost;
}

bool JoltGameStateValueIsValid(const PHY_GameStateRigidBodyState &state)
{
  if (!JoltGameStateModeIsValid(state.mode) ||
      !JoltGameStateRuntimeMotionTypeIsValid(state.runtime_motion_type) ||
      !JoltGameStateVectorIsFinite(state.linear_velocity) ||
      !JoltGameStateVectorIsFinite(state.angular_velocity) ||
      !JoltGameStateVectorIsFinite(state.pending_force) ||
      !JoltGameStateVectorIsFinite(state.pending_torque) || !std::isfinite(state.mass) ||
      !std::isfinite(state.friction) || !std::isfinite(state.restitution) ||
      !std::isfinite(state.linear_damping) || !std::isfinite(state.angular_damping) ||
      !std::isfinite(state.linear_velocity_min) || !std::isfinite(state.linear_velocity_max) ||
      !std::isfinite(state.angular_velocity_min) || !std::isfinite(state.angular_velocity_max) ||
      !std::isfinite(state.gravity_factor) || state.collision_group > JOLT_COLLISION_LAYER_MASK ||
      state.mass < 0.0f || state.friction < 0.0f || state.restitution < 0.0f ||
      state.restitution > 1.0f || state.linear_damping < 0.0f || state.linear_damping > 1.0f ||
      state.angular_damping < 0.0f || state.angular_damping > 1.0f ||
      state.linear_velocity_min < -1.0f || state.linear_velocity_max < -1.0f ||
      state.angular_velocity_min < -1.0f || state.angular_velocity_max < -1.0f)
  {
    return false;
  }

  if ((state.runtime_motion_type == PHY_RuntimeMotionType::Dynamic && !state.dynamic_body) ||
      (state.runtime_motion_type == PHY_RuntimeMotionType::Kinematic && state.dynamic_body) ||
      (state.runtime_motion_type == PHY_RuntimeMotionType::Kinematic && state.rotation_enabled) ||
      (!state.dynamic_body && state.ccd_enabled) ||
      (state.dynamic_body && state.ccd_enabled != state.dynamic_ccd_enabled))
  {
    return false;
  }

  if (!state.dynamic_body) {
    return !state.awake && JoltGameStateVectorIsZero(state.linear_velocity) &&
           JoltGameStateVectorIsZero(state.angular_velocity) &&
           JoltGameStateVectorIsZero(state.pending_force) &&
           JoltGameStateVectorIsZero(state.pending_torque);
  }

  if (state.mass <= 0.0f ||
      JoltGameStateAllowedDOFs(state.rotation_enabled, state.axis_locks) ==
          JPH::EAllowedDOFs::None ||
      !JoltGameStateVelocityFits(state.linear_velocity,
                                 JoltEffectiveLinearVelocityMax(state.linear_velocity_max)) ||
      !JoltGameStateVelocityFits(state.angular_velocity,
                                 JoltEffectiveAngularVelocityMax(state.angular_velocity_max)))
  {
    return false;
  }

  const bool bodyCanBeAwake = !state.physics_suspended && state.mode == PHY_DynamicsMode::Dynamic;
  if (state.awake && !bodyCanBeAwake) {
    return false;
  }

  /* Jolt deliberately clears motion when a body enters a static suspended mode. */
  if (ELEM(state.mode, PHY_DynamicsMode::StaticCollider, PHY_DynamicsMode::Ghost) &&
      (!JoltGameStateVectorIsZero(state.linear_velocity) ||
       !JoltGameStateVectorIsZero(state.angular_velocity) ||
       !JoltGameStateVectorIsZero(state.pending_force) ||
       !JoltGameStateVectorIsZero(state.pending_torque)))
  {
    return false;
  }
  return true;
}

const JPH::StaticCompoundShape *JoltAsStaticCompoundShape(const JPH::Shape *shape)
{
  if (!shape) {
    return nullptr;
  }

  if (shape->GetSubType() == JPH::EShapeSubType::OffsetCenterOfMass) {
    shape = static_cast<const JPH::OffsetCenterOfMassShape *>(shape)->GetInnerShape();
  }

  if (shape && shape->GetSubType() == JPH::EShapeSubType::StaticCompound) {
    return static_cast<const JPH::StaticCompoundShape *>(shape);
  }

  return nullptr;
}

JPH::Vec3 JoltCompoundSubShapeLocalPosition(const JPH::StaticCompoundShape &compound,
                                            const JPH::CompoundShape::SubShape &subShape)
{
  return subShape.GetPositionCOM() + compound.GetCenterOfMass() -
         subShape.GetRotation() * subShape.mShape->GetCenterOfMass();
}

void JoltAddExistingShapeToCompoundSettings(JPH::StaticCompoundShapeSettings &settings,
                                            const JPH::Shape *shape)
{
  const JPH::StaticCompoundShape *compound = JoltAsStaticCompoundShape(shape);
  if (!compound) {
    if (shape) {
      settings.AddShape(JPH::Vec3::sZero(), JPH::Quat::sIdentity(), shape);
    }
    return;
  }

  for (JPH::uint i = 0; i < compound->GetNumSubShapes(); ++i) {
    const JPH::CompoundShape::SubShape &subShape = compound->GetSubShape(i);
    settings.AddShape(JoltCompoundSubShapeLocalPosition(*compound, subShape),
                      subShape.GetRotation(),
                      subShape.mShape,
                      subShape.mUserData);
  }
}

bool JoltRootRelativeTransform(const KX_GameObject &root,
                               const KX_GameObject &child,
                               JPH::Vec3 &relativePosition,
                               JPH::Quat &relativeRotation)
{
  MT_Vector3 inverseScale = root.NodeGetWorldScaling();
  for (int axis = 0; axis < 3; ++axis) {
    if (std::abs(inverseScale[axis]) <= std::numeric_limits<MT_Scalar>::epsilon()) {
      return false;
    }
    inverseScale[axis] = MT_Scalar(1.0f) / inverseScale[axis];
  }

  const MT_Matrix3x3 inverseRootRotation = root.NodeGetWorldOrientation().transposed();
  const MT_Vector3 localPosition = (inverseRootRotation *
                                    (child.NodeGetWorldPosition() - root.NodeGetWorldPosition())) *
                                   inverseScale;
  const MT_Matrix3x3 localRotation = inverseRootRotation * child.NodeGetWorldOrientation();
  for (int row = 0; row < 3; ++row) {
    if (!std::isfinite(localPosition[row])) {
      return false;
    }
    for (int column = 0; column < 3; ++column) {
      if (!std::isfinite(localRotation[row][column])) {
        return false;
      }
    }
  }
  const MT_Scalar determinant = localRotation.determinant();
  if (!std::isfinite(determinant) ||
      std::abs(determinant) <= std::numeric_limits<MT_Scalar>::epsilon())
  {
    return false;
  }
  relativePosition = JoltMath::ToJolt(localPosition);
  relativeRotation = JoltMath::ToJolt(localRotation.getRotation());
  return true;
}

void JoltFindCompoundChildRecursive(const SG_Node *node,
                                    const KX_GameObject &rootObject,
                                    KX_GameObject *sourceObject,
                                    const blender::Object *blenderObject,
                                    JPH::Vec3Arg relativePosition,
                                    JPH::QuatArg relativeRotation,
                                    KX_GameObject *&bestObject,
                                    float &bestScore,
                                    bool &foundSource)
{
  if (!node || foundSource) {
    return;
  }

  for (SG_Node *childNode : node->GetSGChildren()) {
    KX_GameObject *childObject = static_cast<KX_GameObject *>(childNode->GetSGClientObject());
    if (childObject) {
      if (childObject == sourceObject &&
          (!blenderObject || childObject->GetBlenderObject() == blenderObject))
      {
        bestObject = childObject;
        foundSource = true;
        return;
      }
      if (childObject->GetBlenderObject() == blenderObject) {
        JPH::Vec3 candidatePosition;
        JPH::Quat candidateRotation;
        if (JoltRootRelativeTransform(
                rootObject, *childObject, candidatePosition, candidateRotation))
        {
          const float positionError = (candidatePosition - relativePosition).LengthSq();
          const float rotationError = 1.0f - std::abs(candidateRotation.Normalized().Dot(
                                                 relativeRotation.Normalized()));
          const float score = positionError + rotationError;
          if (score < bestScore) {
            bestScore = score;
            bestObject = childObject;
          }
        }
      }
    }
    JoltFindCompoundChildRecursive(childNode,
                                   rootObject,
                                   sourceObject,
                                   blenderObject,
                                   relativePosition,
                                   relativeRotation,
                                   bestObject,
                                   bestScore,
                                   foundSource);
    if (foundSource) {
      return;
    }
  }
}

JPH::RefConst<JPH::Shape> JoltShapeWithCenterOfMass(JPH::RefConst<JPH::Shape> shape,
                                                    JPH::Vec3Arg centerOfMass)
{
  if (!shape) {
    return shape;
  }

  const JPH::Vec3 offset = centerOfMass - shape->GetCenterOfMass();
  if (offset.IsNearZero()) {
    return shape;
  }

  return new JPH::OffsetCenterOfMassShape(shape.GetPtr(), offset);
}

JPH::Vec3 JoltVehicleAxisVector(int axis)
{
  switch (axis) {
    case 0:
      return JPH::Vec3::sAxisX();
    case 1:
      return -JPH::Vec3::sAxisZ();
    case 2:
      return JPH::Vec3::sAxisY();
    case 3:
      return -JPH::Vec3::sAxisX();
    case 4:
      return JPH::Vec3::sAxisZ();
    case 5:
      return -JPH::Vec3::sAxisY();
    default:
      return JPH::Vec3::sAxisX();
  }
}

JPH::RefConst<JPH::Shape> JoltApplyVehicleCenterOfMassOffset(JPH::RefConst<JPH::Shape> shape,
                                                             const blender::Object *blenderobject)
{
  if (!shape || !blenderobject || !(blenderobject->gameflag2 & blender::OB_HAS_VEHICLE) ||
      !blenderobject->vehicle)
  {
    return shape;
  }

  const blender::GameVehicleSettings *vehicle_settings = blenderobject->vehicle;
  if (vehicle_settings->vehicle_type != blender::OB_VEHICLE_TYPE_CHASSIS &&
      vehicle_settings->vehicle_type != blender::OB_VEHICLE_TYPE_MOTORCYCLE_CHASSIS)
  {
    return shape;
  }

  const float normalized_offset = std::clamp(vehicle_settings->center_of_mass_offset, -1.0f, 1.0f);
  if (std::abs(normalized_offset) <= 1.0e-6f) {
    return shape;
  }

  const JPH::AABox bounds = shape->GetLocalBounds();
  if (!bounds.IsValid()) {
    return shape;
  }

  const JPH::Vec3 up = JoltVehicleAxisVector(vehicle_settings->up_axis);
  const JPH::Vec3 size = bounds.mMax - bounds.mMin;
  const float half_height = 0.5f * (std::abs(up.GetX()) * size.GetX() +
                                    std::abs(up.GetY()) * size.GetY() +
                                    std::abs(up.GetZ()) * size.GetZ());
  if (half_height <= 1.0e-6f) {
    return shape;
  }

  return new JPH::OffsetCenterOfMassShape(shape.GetPtr(), up * (normalized_offset * half_height));
}

bool JoltBoundsTypeUsesMesh(const char boundsType)
{
  using namespace blender;
  return boundsType == OB_BOUND_CONVEX_HULL || boundsType == OB_BOUND_TRIANGLE_MESH;
}

char JoltRuntimeBoundsType(const blender::Object *blenderobject, const RAS_MeshObject *meshobj)
{
  using namespace blender;

  const bool is_dynamic = (blenderobject->gameflag & OB_DYNAMIC) != 0;
  const bool is_character = (blenderobject->gameflag & OB_CHARACTER) != 0;
  const bool is_soft_body = (blenderobject->gameflag & OB_SOFT_BODY) != 0;
  const bool is_rigid_body = (blenderobject->gameflag & OB_RIGID_BODY) != 0;
  const bool is_sensor = (blenderobject->gameflag & OB_SENSOR) != 0;
  const bool is_kinematic = (blenderobject->gameflag & OB_KINEMATIC) != 0 && !is_dynamic &&
                            !is_character && !is_soft_body && !is_sensor;

  if (!(blenderobject->gameflag & OB_BOUNDS)) {
    if (is_soft_body) {
      return OB_BOUND_TRIANGLE_MESH;
    }
    if (is_character) {
      return OB_BOUND_SPHERE;
    }
    if (is_rigid_body && !(blenderobject->gameflag2 & OB_HAS_VEHICLE)) {
      return OB_BOUND_BOX;
    }
    if (is_kinematic) {
      return OB_BOUND_BOX;
    }
    if (is_dynamic) {
      return OB_BOUND_SPHERE;
    }
    return OB_BOUND_TRIANGLE_MESH;
  }

  const char boundsType = blenderobject->collision_boundtype;
  if (JoltBoundsTypeUsesMesh(boundsType) && (blenderobject->type != OB_MESH || !meshobj)) {
    return OB_BOUND_SPHERE;
  }
  return boundsType;
}

JPH::RefConst<JPH::Shape> JoltBuildRuntimeCollisionShape(KX_GameObject *gameobj,
                                                         RAS_MeshObject *meshobj,
                                                         const bool dynamic_shape,
                                                         const float margin,
                                                         const unsigned char queryDetailFlags,
                                                         JoltShapeQueryDataPtr &r_queryData)
{
  using namespace blender;

  if (!gameobj) {
    return nullptr;
  }

  const Object *blenderobject = gameobj->GetBlenderObject();
  if (!blenderobject) {
    return nullptr;
  }

  char boundsType = JoltRuntimeBoundsType(blenderobject, meshobj);
  KX_Scene *scene = gameobj->GetScene();
  if (JoltBoundsTypeUsesMesh(boundsType) && (!meshobj || !scene)) {
    boundsType = OB_BOUND_SPHERE;
  }

  float bounds_extends[3] = {1.0f, 1.0f, 1.0f};
  if (const std::optional<Bounds<float3>> bl_bounds = BKE_object_boundbox_eval_cached_get(
          blenderobject))
  {
    const std::array<float3, 8> corners = bounds::corners(*bl_bounds);
    bounds_extends[0] = 0.5f * std::abs(corners[0][0] - corners[4][0]);
    bounds_extends[1] = 0.5f * std::abs(corners[0][1] - corners[2][1]);
    bounds_extends[2] = 0.5f * std::abs(corners[0][2] - corners[1][2]);
  }

  JoltShapeBuilder shapeBuilder;
  shapeBuilder.SetMargin(margin);
  shapeBuilder.SetRayQueryDetailRequirements(queryDetailFlags);

  switch (boundsType) {
    case OB_BOUND_SPHERE:
      shapeBuilder.SetShapeType(PHY_SHAPE_SPHERE);
      shapeBuilder.SetRadius(std::max({bounds_extends[0], bounds_extends[1], bounds_extends[2]}));
      break;
    case OB_BOUND_BOX:
      shapeBuilder.SetShapeType(PHY_SHAPE_BOX);
      shapeBuilder.SetHalfExtents(bounds_extends[0], bounds_extends[1], bounds_extends[2]);
      break;
    case OB_BOUND_CYLINDER: {
      const float radius = std::max(bounds_extends[0], bounds_extends[1]);
      shapeBuilder.SetShapeType(PHY_SHAPE_CYLINDER);
      shapeBuilder.SetHalfExtents(radius, radius, bounds_extends[2]);
      break;
    }
    case OB_BOUND_CONE:
      shapeBuilder.SetShapeType(PHY_SHAPE_CONE);
      shapeBuilder.SetRadius(std::max(bounds_extends[0], bounds_extends[1]));
      shapeBuilder.SetHeight(2.0f * bounds_extends[2]);
      break;
    case OB_BOUND_CONVEX_HULL:
      shapeBuilder.SetMesh(scene, meshobj, true);
      break;
    case OB_BOUND_CAPSULE: {
      const float radius = std::max(bounds_extends[0], bounds_extends[1]);
      shapeBuilder.SetShapeType(PHY_SHAPE_CAPSULE);
      shapeBuilder.SetRadius(radius);
      shapeBuilder.SetHeight(std::max(2.0f * (bounds_extends[2] - radius), 0.0f));
      break;
    }
    case OB_BOUND_TRIANGLE_MESH:
      shapeBuilder.SetMesh(scene, meshobj, false);
      break;
    case OB_BOUND_EMPTY:
      shapeBuilder.SetShapeType(PHY_SHAPE_EMPTY);
      break;
    default:
      return nullptr;
  }

  JPH::RefConst<JPH::Shape> shape = shapeBuilder.Build(dynamic_shape,
                                                       gameobj->NodeGetWorldScaling());
  r_queryData = shapeBuilder.GetShapeQueryData();
  return JoltApplyVehicleCenterOfMassOffset(shape, blenderobject);
}

}  // namespace

JoltPhysicsController::JoltPhysicsController() {}

JoltPhysicsController::~JoltPhysicsController()
{
  /* For soft body controllers, route cleanup through the physics environment so
   * runtime teardown also removes the soft body from m_softBodies and unregisters
   * any no-pin-collision map entry. */
  if (m_softBody) {
    if (m_softBody->GetController() == this) {
      if (m_physicsEnv) {
        m_physicsEnv->RemoveSoftBody(m_softBody);
      }
      else {
        delete m_softBody;
      }
    }
    m_softBody = nullptr;
    m_bodyID = JPH::BodyID();
  }
  /* Remove body from the physics system if we still have a valid environment. */
  else if (m_physicsEnv && !m_bodyID.IsInvalid()) {
    /* If physics is currently updating, defer body destruction to prevent
     * crashes from destroying bodies while physics jobs reference them. */
    if (m_physicsEnv->IsPhysicsUpdating()) {
      JoltDeferredOp op;
      op.type = JoltDeferredOpType::DestroyBody;
      op.bodyID = m_bodyID;
      op.controller = this;
      m_physicsEnv->QueueDeferredOperation(op);
      m_bodyID = JPH::BodyID(); /* Invalidate to prevent double-free. */
    }
    else {
      m_physicsEnv->RemoveConstraintsForBody(m_bodyID);
      JPH::BodyInterface &bi = m_physicsEnv->GetBodyInterface();
      if (bi.IsAdded(m_bodyID)) {
        bi.RemoveBody(m_bodyID);
      }
      bi.DestroyBody(m_bodyID);
      m_bodyID = JPH::BodyID(); /* Invalidate to prevent double-free. */
    }
  }

  /* Remove ourselves from the environment's controller list. */
  if (m_physicsEnv) {
    m_physicsEnv->RemoveController(this);
    m_physicsEnv = nullptr;
  }

  delete m_motionState;
  m_motionState = nullptr;
}

/* -------------------------------------------------------------------- */
/** \name Motion State Synchronization
 * \{ */

bool JoltPhysicsController::SynchronizeMotionStates(float time)
{
  if (!m_physicsEnv || m_bodyID.IsInvalid() || !m_motionState) {
    return false;
  }

  /* Static bodies never move due to physics — skip the Jolt query entirely.
   * Sleeping dynamic bodies haven't changed either. */
  if (!m_isDynamic || m_transformParentingKinematic) {
    return false;
  }
  JPH::BodyInterface &bodyInterface = m_physicsEnv->GetBodyInterfaceNoLock();
  if (!bodyInterface.IsActive(m_bodyID)) {
    return false;
  }

  WriteDynamicsToMotionState();
  return true;
}

void JoltPhysicsController::UpdateSoftBody()
{
  /* Mesh deformation is handled by JoltSoftBody::UpdateMesh(),
   * called each frame from JoltPhysicsEnvironment::UpdateSoftBodies().
   * Nothing to do here. */
}

void JoltPhysicsController::SetSoftBodyTransform(const MT_Vector3 &pos, const MT_Matrix3x3 &ori)
{
  if (!m_physicsEnv || m_bodyID.IsInvalid() || !m_softBody) {
    return;
  }

  /* Full-copy spawning creates a new soft body from the template first, then
   * repositions/reorients the game object to the actuator reference.
   * Soft-body orientation is baked into local particle coordinates (body
   * rotation should remain identity), so we must rotate/rebase that local
   * data instead of applying a body rotation.
   *
   * Keep this as a soft-body-only path (same semantics as Bullet backend).
   * Rigid-body replicas are synchronized by SetTransform() immediately after. */
  m_softBody->ApplySpawnTransform(pos, ori);
}

bool JoltPhysicsController::HasValidGameStateBody() const
{
  if (!m_physicsEnv || m_bodyID.IsInvalid()) {
    return false;
  }
  JPH::BodyLockRead lock(m_physicsEnv->GetPhysicsSystem()->GetBodyLockInterface(), m_bodyID);
  if (!lock.Succeeded()) {
    return false;
  }
  const bool body_is_soft = lock.GetBody().IsSoftBody();
  if (m_softBody != nullptr) {
    return body_is_soft;
  }
  return !body_is_soft && m_shape && !m_isCompoundChild;
}

bool JoltPhysicsController::PrepareGameStateScaling(const MT_Vector3 &targetScale,
                                                    const MT_Vector3 &currentScale)
{
  m_gameStatePreparedShape = nullptr;
  if (!HasValidGameStateBody() || m_softBody != nullptr || m_isCompound) {
    return false;
  }

  if (!JoltScaleIsValid(targetScale) || !JoltScaleIsValid(currentScale)) {
    return false;
  }

  const JPH::Vec3 target(targetScale[0], targetScale[2], targetScale[1]);
  const JPH::Vec3 current(currentScale[0], currentScale[2], currentScale[1]);

  /* ScaleShape may bake an invalid ScaledShape scale (for example, a non-uniform sphere) into a
   * compound. Such a result cannot be unwrapped on the next Load. Retain the shape associated with
   * the first known world scale and always derive checkpoint shapes from that stable pair. */
  if (!m_gameStateScalingStateValid || m_shape.GetPtr() != m_gameStateKnownShape.GetPtr() ||
      !JoltScalesMatch(current, m_gameStateKnownScale))
  {
    m_gameStateScalingBaseShape = m_shape;
    m_gameStateScalingBaseScale = current;
    m_gameStateKnownShape = m_shape;
    m_gameStateKnownScale = current;
    m_gameStateScalingStateValid = true;
  }

  const JPH::Vec3 relative = target / m_gameStateScalingBaseScale;
  if (!std::isfinite(relative.GetX()) || !std::isfinite(relative.GetY()) ||
      !std::isfinite(relative.GetZ()))
  {
    return false;
  }
  const JPH::Shape::ShapeResult result = m_gameStateScalingBaseShape->ScaleShape(relative);
  if (result.HasError()) {
    return false;
  }
  m_gameStatePreparedShape = result.Get();
  m_gameStatePreparedScale = target;
  return m_gameStatePreparedShape != nullptr;
}

void JoltPhysicsController::CommitPreparedGameStateScaling()
{
  if (!m_gameStatePreparedShape) {
    return;
  }
  JPH::RefConst<JPH::Shape> shape = std::move(m_gameStatePreparedShape);
  m_gameStatePreparedShape = nullptr;
  m_shape = shape;
  m_gameStateKnownShape = shape;
  m_gameStateKnownScale = m_gameStatePreparedScale;
  m_physicsEnv->RemoveLogicActiveContactsForBody(m_bodyID);
  m_physicsEnv->GetBodyInterface().SetShape(m_bodyID, shape, true, JPH::EActivation::Activate);
}

void JoltPhysicsController::ClearPreparedGameStateScaling()
{
  m_gameStatePreparedShape = nullptr;
}

bool JoltPhysicsController::CaptureGameStateRigidBody(PHY_GameStateRigidBodyState &r_state) const
{
  r_state = PHY_GameStateRigidBodyState();
  if (!m_physicsEnv || m_bodyID.IsInvalid() || m_physicsEnv->IsPhysicsUpdating() || m_softBody ||
      m_isCompoundChild || !m_shape)
  {
    return false;
  }

  JPH::BodyLockRead lock(m_physicsEnv->GetPhysicsSystem()->GetBodyLockInterface(), m_bodyID);
  if (!lock.Succeeded()) {
    return false;
  }
  const JPH::Body &body = lock.GetBody();
  if (!body.IsRigidBody() || body.IsSoftBody()) {
    return false;
  }

  const JPH::EMotionType runtimeMotionType = GetRuntimeMotionType();
  if ((m_isDynamic && m_isKinematic) ||
      (runtimeMotionType == JPH::EMotionType::Static &&
       m_originalMotionType != JPH::EMotionType::Static) ||
      !JoltGameStateModeIsValid(m_dynamicsMode) ||
      m_dynamicsSuspended != (m_dynamicsMode != PHY_DynamicsMode::Dynamic) ||
      m_bodyRemovedOnSuspend != (m_dynamicsMode == PHY_DynamicsMode::NoCollision))
  {
    return false;
  }

  r_state.dynamic_body = runtimeMotionType == JPH::EMotionType::Dynamic;
  r_state.mode = m_dynamicsMode;
  r_state.runtime_motion_type = runtimeMotionType == m_originalMotionType ?
                                      PHY_RuntimeMotionType::Authored :
                                      (runtimeMotionType == JPH::EMotionType::Dynamic ?
                                           PHY_RuntimeMotionType::Dynamic :
                                           PHY_RuntimeMotionType::Kinematic);
  r_state.ghost = m_dynamicsSuspended ? m_savedIsSensor : m_isSensor;
  r_state.physics_suspended = m_physicsSuspended;
  /* Kinematic velocity is derived again from the scene-graph target on the
   * next fixed step. Persisting it as dynamic velocity would be both wrong and
   * incompatible with the backend-neutral static-state representation. */
  r_state.awake = runtimeMotionType == JPH::EMotionType::Kinematic ? false : body.IsActive();
  r_state.friction = body.GetFriction();
  r_state.restitution = body.GetRestitution();
  r_state.linear_velocity_min = m_linVelMin;
  r_state.linear_velocity_max = m_linVelMax;
  r_state.angular_velocity_min = m_angVelMin;
  r_state.angular_velocity_max = m_angVelMax;
  r_state.rotation_enabled = runtimeMotionType == JPH::EMotionType::Dynamic && m_isRigidBody;
  r_state.dynamic_ccd_enabled = m_dynamicMotionQuality == JPH::EMotionQuality::LinearCast;
  r_state.axis_locks = {m_lockTranslationX,
                        m_lockTranslationY,
                        m_lockTranslationZ,
                        m_lockRotationX,
                        m_lockRotationY,
                        m_lockRotationZ};
  r_state.collision_group = m_collisionGroup & JOLT_COLLISION_LAYER_MASK;
  r_state.collision_mask = m_collisionMask;

  const bool effectiveSensor = JoltGameStateEffectiveSensor(r_state);
  JoltBroadPhaseLayer effectiveCategory = m_bpCategory;
  if (m_dynamicsMode == PHY_DynamicsMode::StaticCollider) {
    effectiveCategory = JOLT_BP_STATIC;
  }
  else if (m_dynamicsMode == PHY_DynamicsMode::Ghost) {
    effectiveCategory = JOLT_BP_SENSOR;
  }
  if (body.IsSensor() != effectiveSensor || m_isSensor != effectiveSensor ||
      JoltGetCategory(body.GetObjectLayer()) != effectiveCategory ||
      JoltGetCollisionLayers(body.GetObjectLayer()) != r_state.collision_group ||
      body.GetMotionType() != JoltGameStateMotionType(r_state, m_originalMotionType))
  {
    return false;
  }

  const JPH::MotionProperties *motion = body.GetMotionPropertiesUnchecked();
  if (runtimeMotionType == JPH::EMotionType::Kinematic) {
    if (!motion) {
      return false;
    }
    r_state.allow_sleeping = body.GetAllowSleeping();
    /* Jolt's LinearCast branch explicitly excludes Kinematic bodies because
     * their authored target cannot be stopped. Keep the serialized value false
     * rather than promising CCD that the solver does not perform. */
    r_state.ccd_enabled = false;
  }
  if (runtimeMotionType == JPH::EMotionType::Dynamic) {
    if (!motion ||
        motion->GetAllowedDOFs() !=
            JoltGameStateAllowedDOFs(r_state.rotation_enabled, r_state.axis_locks) ||
        !JoltGameStateFloatMatches(motion->GetMaxLinearVelocity(),
                                   JoltEffectiveLinearVelocityMax(r_state.linear_velocity_max)) ||
        !JoltGameStateFloatMatches(motion->GetMaxAngularVelocity(),
                                   JoltEffectiveAngularVelocityMax(r_state.angular_velocity_max)))
    {
      return false;
    }

    r_state.linear_velocity = JoltMath::ToMT(motion->GetLinearVelocity());
    r_state.angular_velocity = JoltMath::ToMT(motion->GetAngularVelocity());
    r_state.pending_force = JoltMath::ToMT(motion->GetAccumulatedForce());
    r_state.pending_torque = JoltMath::ToMT(motion->GetAccumulatedTorque());

    const float inverseMass = motion->GetInverseMassUnchecked();
    if (inverseMass > 0.0f && std::isfinite(inverseMass)) {
      r_state.mass = 1.0f / inverseMass;
    }
    else {
      r_state.mass = m_configuredMass;
      if (!(r_state.mass > 0.0f) || !std::isfinite(r_state.mass)) {
        r_state.mass = m_massPropertiesTemplate.mMass;
      }
      if ((!(r_state.mass > 0.0f) || !std::isfinite(r_state.mass)) && m_shape) {
        r_state.mass = m_shape->GetMassProperties().mMass;
      }
    }

    r_state.linear_damping = motion->GetLinearDamping();
    r_state.angular_damping = motion->GetAngularDamping();
    r_state.gravity_factor = motion->GetGravityFactor();
    r_state.allow_sleeping = body.GetAllowSleeping();
    r_state.ccd_enabled = motion->GetMotionQuality() == JPH::EMotionQuality::LinearCast;
  }

  return JoltGameStateValueIsValid(r_state);
}

bool JoltPhysicsController::PrepareGameStateRigidBody(const PHY_GameStateRigidBodyState &state)
{
  ClearPreparedGameStateRigidBody();
  /* Active transform parenting is fine here: Load Game ends it (restoring the parent) before it
   * commits the prepared state. Commit still refuses while it is active. */
  if (!JoltGameStateValueIsValid(state) || !m_physicsEnv || m_bodyID.IsInvalid() ||
      m_physicsEnv->IsPhysicsUpdating() || m_softBody || m_isCompound || m_isCompoundChild ||
      (m_transformParentingActive && m_transformParentingCompound) || !m_shape)
  {
    return false;
  }

  const JPH::EMotionType targetBaseMotionType =
      JoltGameStateBaseMotionType(state, m_originalMotionType);
  const bool targetSensorPhysicsType =
      state.runtime_motion_type == PHY_RuntimeMotionType::Authored &&
      m_originalIsSensorPhysicsType;
  if (state.dynamic_body != (targetBaseMotionType == JPH::EMotionType::Dynamic) ||
      (targetBaseMotionType != JPH::EMotionType::Dynamic &&
       (state.rotation_enabled || state.ccd_enabled)) ||
      (targetBaseMotionType == JPH::EMotionType::Static && state.dynamic_ccd_enabled) ||
      (state.runtime_motion_type != PHY_RuntimeMotionType::Authored &&
       !m_runtimePhysicsTypeSupported) ||
      (targetSensorPhysicsType &&
       (!state.ghost || state.mode == PHY_DynamicsMode::StaticCollider)) ||
      (targetBaseMotionType != JPH::EMotionType::Static && m_shape->MustBeStatic()))
  {
    return false;
  }

  JPH::BodyLockRead lock(m_physicsEnv->GetPhysicsSystem()->GetBodyLockInterface(), m_bodyID);
  if (!lock.Succeeded() || !lock.GetBody().IsRigidBody() || lock.GetBody().IsSoftBody() ||
      (targetBaseMotionType != JPH::EMotionType::Static &&
       !lock.GetBody().GetMotionPropertiesUnchecked()))
  {
    return false;
  }

  m_gameStatePreparedRigidBody = state;
  m_gameStatePreparedRigidBodyID = m_bodyID;
  m_gameStatePreparedRigidBodyValid = true;
  return true;
}

void JoltPhysicsController::CommitPreparedGameStateRigidBody()
{
  if (!m_gameStatePreparedRigidBodyValid) {
    return;
  }

  const PHY_GameStateRigidBodyState state = m_gameStatePreparedRigidBody;
  const JPH::BodyID preparedBodyID = m_gameStatePreparedRigidBodyID;
  ClearPreparedGameStateRigidBody();
  if (!m_physicsEnv || m_physicsEnv->IsPhysicsUpdating() || m_bodyID.IsInvalid() ||
      m_bodyID != preparedBodyID || m_softBody || m_isCompound || m_isCompoundChild ||
      m_transformParentingActive || !m_shape)
  {
    return;
  }

  const JPH::EMotionType targetBaseMotionType =
      JoltGameStateBaseMotionType(state, m_originalMotionType);
  const bool targetSensorPhysicsType =
      state.runtime_motion_type == PHY_RuntimeMotionType::Authored &&
      m_originalIsSensorPhysicsType;
  if (state.dynamic_body != (targetBaseMotionType == JPH::EMotionType::Dynamic) ||
      (targetBaseMotionType != JPH::EMotionType::Dynamic &&
       (state.rotation_enabled || state.ccd_enabled)) ||
      (targetBaseMotionType == JPH::EMotionType::Static && state.dynamic_ccd_enabled) ||
      (state.runtime_motion_type != PHY_RuntimeMotionType::Authored &&
       !m_runtimePhysicsTypeSupported) ||
      (targetSensorPhysicsType &&
       (!state.ghost || state.mode == PHY_DynamicsMode::StaticCollider)) ||
      (targetBaseMotionType != JPH::EMotionType::Static && m_shape->MustBeStatic()))
  {
    return;
  }

  {
    JPH::BodyLockRead lock(m_physicsEnv->GetPhysicsSystem()->GetBodyLockInterface(), m_bodyID);
    if (!lock.Succeeded() || !lock.GetBody().IsRigidBody() || lock.GetBody().IsSoftBody() ||
        (targetBaseMotionType != JPH::EMotionType::Static &&
         !lock.GetBody().GetMotionPropertiesUnchecked()))
    {
      return;
    }
  }

  JPH::BodyInterface &bodyInterface = m_physicsEnv->GetBodyInterface();
  const JPH::EMotionType targetMotionType = JoltGameStateMotionType(state, m_originalMotionType);
  const bool targetSensor = JoltGameStateEffectiveSensor(state);
  const bool targetIsDynamic = targetBaseMotionType == JPH::EMotionType::Dynamic;
  const bool targetIsKinematic = targetBaseMotionType == JPH::EMotionType::Kinematic;
  JoltBroadPhaseLayer targetCategory =
      state.runtime_motion_type == PHY_RuntimeMotionType::Authored ?
          m_originalBroadPhaseCategory :
          JOLT_BP_DYNAMIC;
  if (state.mode == PHY_DynamicsMode::StaticCollider) {
    targetCategory = JOLT_BP_STATIC;
  }
  else if (state.mode == PHY_DynamicsMode::Ghost) {
    targetCategory = JOLT_BP_SENSOR;
  }

  bool targetAdded = !state.physics_suspended && state.mode != PHY_DynamicsMode::NoCollision;
  /* Authored Sensor objects remain query-only until a collision sensor registers them. */
  if (state.mode == PHY_DynamicsMode::Dynamic && targetCategory == JOLT_BP_SENSOR &&
      !Registered())
  {
    targetAdded = false;
  }

  m_physicsEnv->RemoveLogicActiveContactsForBody(m_bodyID);
  if (bodyInterface.IsActive(m_bodyID)) {
    bodyInterface.DeactivateBody(m_bodyID);
  }
  if (!targetAdded) {
    m_physicsEnv->RemovePendingRigidBodyBodyAdd(m_bodyID);
    if (bodyInterface.IsAdded(m_bodyID)) {
      bodyInterface.RemoveBody(m_bodyID);
    }
  }

  const JPH::EMotionType previousMotionType = bodyInterface.GetMotionType(m_bodyID);
  bodyInterface.SetMotionType(m_bodyID, targetMotionType, JPH::EActivation::DontActivate);
  if (previousMotionType != targetMotionType) {
    m_physicsEnv->NotifyConstraintBodyMotionTypeChanged(m_bodyID);
  }
  if (targetIsDynamic) {
    bodyInterface.SetMotionQuality(m_bodyID,
                                   state.ccd_enabled ? JPH::EMotionQuality::LinearCast :
                                                       JPH::EMotionQuality::Discrete);
  }
  else if (targetIsKinematic) {
    bodyInterface.SetMotionQuality(m_bodyID, JPH::EMotionQuality::Discrete);
  }

  {
    JPH::BodyLockWrite lock(m_physicsEnv->GetPhysicsSystem()->GetBodyLockInterface(), m_bodyID);
    if (!lock.Succeeded()) {
      return;
    }
    JPH::Body &body = lock.GetBody();
    body.SetFriction(state.friction);
    body.SetRestitution(state.restitution);
    body.SetIsSensor(targetSensor);
    if (targetIsKinematic) {
      body.SetCollideKinematicVsNonDynamic(targetSensor);
    }

    if (state.dynamic_body) {
      JPH::MotionProperties *motion = body.GetMotionPropertiesUnchecked();
      if (!motion) {
        return;
      }
      motion->SetMassProperties(JoltGameStateAllowedDOFs(state.rotation_enabled, state.axis_locks),
                                MassPropertiesForMass(state.mass));
      motion->SetMaxLinearVelocity(JoltEffectiveLinearVelocityMax(state.linear_velocity_max));
      motion->SetMaxAngularVelocity(JoltEffectiveAngularVelocityMax(state.angular_velocity_max));
      motion->SetLinearDamping(state.linear_damping);
      motion->SetAngularDamping(state.angular_damping);
      motion->SetGravityFactor(state.gravity_factor);
      body.SetAllowSleeping(state.allow_sleeping);
      motion->SetLinearVelocityClamped(JoltMath::ToJolt(state.linear_velocity));
      motion->SetAngularVelocityClamped(JoltMath::ToJolt(state.angular_velocity));
      motion->ResetForce();
      motion->ResetTorque();
      if (targetMotionType == JPH::EMotionType::Dynamic) {
        body.AddForce(JoltMath::ToJolt(state.pending_force));
        body.AddTorque(JoltMath::ToJolt(state.pending_torque));
      }
    }
    else if (targetIsKinematic) {
      /* Clear any in-flight velocity from the state being replaced. The body
       * pose is rebased to the restored scene graph below so Load is a teleport,
       * not one frame of artificial Kinematic motion through the level. */
      JPH::MotionProperties *motion = body.GetMotionPropertiesUnchecked();
      if (!motion) {
        return;
      }
      motion->SetLinearVelocityClamped(JPH::Vec3::sZero());
      motion->SetAngularVelocityClamped(JPH::Vec3::sZero());
      motion->ResetForce();
      motion->ResetTorque();
      body.SetAllowSleeping(state.allow_sleeping);
    }
  }

  bodyInterface.SetObjectLayer(
      m_bodyID, JoltMakeObjectLayer(state.collision_group, state.collision_mask, targetCategory));
  bodyInterface.InvalidateContactCache(m_bodyID);

  m_collisionGroup = state.collision_group;
  m_collisionMask = state.collision_mask;
  m_linVelMin = state.linear_velocity_min;
  m_linVelMax = state.linear_velocity_max;
  m_angVelMin = state.angular_velocity_min;
  m_angVelMax = state.angular_velocity_max;
  m_isDynamic = targetIsDynamic;
  m_isKinematic = targetIsKinematic;
  m_isRigidBody = targetIsDynamic && state.rotation_enabled;
  m_isSensorPhysicsType = targetSensorPhysicsType;
  m_lockTranslationX = state.axis_locks[0];
  m_lockTranslationY = state.axis_locks[1];
  m_lockTranslationZ = state.axis_locks[2];
  m_lockRotationX = state.axis_locks[3];
  m_lockRotationY = state.axis_locks[4];
  m_lockRotationZ = state.axis_locks[5];
  m_dynamicsMode = state.mode;
  m_dynamicsSuspended = state.mode != PHY_DynamicsMode::Dynamic;
  m_bodyRemovedOnSuspend = state.mode == PHY_DynamicsMode::NoCollision;
  m_physicsSuspended = state.physics_suspended;
  m_savedMotionType = targetBaseMotionType;
  m_kinematicNeedsStop = false;
  m_savedIsSensor = state.ghost;
  m_isSensor = targetSensor;
  m_bpCategory = state.runtime_motion_type == PHY_RuntimeMotionType::Authored ?
                     m_originalBroadPhaseCategory :
                     JOLT_BP_DYNAMIC;
  m_dynamicMotionQuality = state.dynamic_ccd_enabled ? JPH::EMotionQuality::LinearCast :
                                                      JPH::EMotionQuality::Discrete;
  if (state.dynamic_body) {
    m_configuredMass = state.mass;
  }

  if (targetIsKinematic) {
    SynchronizeKinematicPose();
  }

  if (targetAdded && !bodyInterface.IsAdded(m_bodyID)) {
    bodyInterface.AddBody(m_bodyID, JPH::EActivation::DontActivate);
    m_physicsEnv->NotifyRigidBodyBodyAdded();
  }
  if (targetAdded && state.awake && targetMotionType == JPH::EMotionType::Dynamic) {
    bodyInterface.ActivateBody(m_bodyID);
  }
  else if (bodyInterface.IsActive(m_bodyID)) {
    bodyInterface.DeactivateBody(m_bodyID);
  }

  SynchronizeClientPhysicsType();
  NotifyFhSettingsChanged();
  NotifyBuoyancySettingsChanged();
}

void JoltPhysicsController::ClearPreparedGameStateRigidBody()
{
  m_gameStatePreparedRigidBodyValid = false;
  m_gameStatePreparedRigidBodyID = JPH::BodyID();
}

void JoltPhysicsController::InvalidateGameStateScalingState()
{
  m_gameStateScalingBaseShape = nullptr;
  m_gameStateKnownShape = nullptr;
  m_gameStatePreparedShape = nullptr;
  m_gameStateScalingStateValid = false;
}

void JoltPhysicsController::EnsureScalingStateFromMotionState()
{
  if (!m_shape || !m_motionState ||
      (m_gameStateScalingStateValid && m_gameStateKnownShape.GetPtr() == m_shape.GetPtr()))
  {
    return;
  }

  const MT_Vector3 scale = m_motionState->GetWorldScaling();
  if (!JoltScaleIsValid(scale)) {
    return;
  }

  const JPH::Vec3 joltScale(scale[0], scale[2], scale[1]);
  m_gameStateScalingBaseShape = m_shape;
  m_gameStateKnownShape = m_shape;
  m_gameStateScalingBaseScale = joltScale;
  m_gameStateKnownScale = joltScale;
  m_gameStateScalingStateValid = true;
}

void JoltPhysicsController::CopyScalingStateToReplica(JoltPhysicsController &replica) const
{
  replica.InvalidateGameStateScalingState();
  if (!m_shape) {
    return;
  }

  if (m_gameStateScalingStateValid && m_gameStateScalingBaseShape && m_gameStateKnownShape &&
      m_gameStateKnownShape.GetPtr() == m_shape.GetPtr())
  {
    replica.m_gameStateScalingBaseShape = m_gameStateScalingBaseShape;
    replica.m_gameStateKnownShape = m_gameStateKnownShape;
    replica.m_gameStateScalingBaseScale = m_gameStateScalingBaseScale;
    replica.m_gameStateKnownScale = m_gameStateKnownScale;
    replica.m_gameStateScalingStateValid = true;
    return;
  }

  if (!m_motionState) {
    return;
  }
  const MT_Vector3 scale = m_motionState->GetWorldScaling();
  if (!JoltScaleIsValid(scale)) {
    return;
  }

  const JPH::Vec3 joltScale(scale[0], scale[2], scale[1]);
  replica.m_gameStateScalingBaseShape = m_shape;
  replica.m_gameStateKnownShape = m_shape;
  replica.m_gameStateScalingBaseScale = joltScale;
  replica.m_gameStateKnownScale = joltScale;
  replica.m_gameStateScalingStateValid = true;
}

void JoltPhysicsController::RemoveSoftBodyModifier(blender::Object *ob)
{
  if (m_softBody) {
    m_softBody->CleanupModifier(ob);
  }
}

void JoltPhysicsController::WriteMotionStateToDynamics(bool nondynaonly, float timeStep)
{
  if (!m_physicsEnv || m_bodyID.IsInvalid() || !m_motionState) {
    return;
  }

  /* Soft body particle positions are managed entirely by the Jolt solver and
   * UpdatePinnedVertices().  Calling SetPositionAndRotation on a soft body
   * translates ALL particles by the delta between the stored (stale) motion-
   * state position and the current COM, dragging the body back to its spawn
   * location every frame and preventing any world-space movement. */
  if (m_softBody) {
    return;
  }

  /* Use cached flag to skip dynamic bodies without touching Jolt. */
  if (nondynaonly && m_isDynamic && !m_transformParentingKinematic) {
    m_staticSyncedEpoch = 0;
    return;
  }

  /* A suspended Kinematic body has temporarily become static or has been
   * removed from the world. Do not issue MoveKinematic against that state. */
  if (m_isKinematic && (m_dynamicsSuspended || m_physicsSuspended)) {
    m_staticSyncedEpoch = 0;
    return;
  }

  if (m_isKinematic) {
    /* Parenting can change world scale without calling the child's local-scale
     * setter. Keep the collision shape synchronized lazily at the fixed-step
     * boundary and allocate a new shape only when scale actually changed. */
    EnsureScalingStateFromMotionState();
    const MT_Vector3 worldScale = m_motionState->GetWorldScaling();
    if (JoltScaleIsValid(worldScale) && m_gameStateScalingStateValid) {
      const JPH::Vec3 targetScale(worldScale[0], worldScale[2], worldScale[1]);
      if (!JoltScalesMatch(targetScale, m_gameStateKnownScale)) {
        SetScaling(worldScale);
      }
    }
  }

  /* No-lock interface is safe here: called from the main thread while
   * PhysicsSystem::Update() is not running. */
  JPH::BodyInterface &bi = m_physicsEnv->GetBodyInterfaceNoLock();

  MT_Vector3 pos = m_motionState->GetWorldPosition();
  MT_Matrix3x3 ori = m_motionState->GetWorldOrientation();

  /* A static body already found at this exact pose needs nothing (most of a level). */
  const bool staticBody = !m_isKinematic && !m_isDynamic;
  const uint32_t poseEpoch = m_physicsEnv->GetBodyPoseEpoch();
  if (staticBody && m_staticSyncedEpoch == poseEpoch && m_staticSyncedBody == m_bodyID &&
      JoltSamePose(pos, ori, m_staticSyncedPosition, m_staticSyncedOrientation))
  {
    return;
  }
  m_staticSyncedEpoch = 0;

  MT_Quaternion quat = ori.getRotation();

  const JPH::RVec3 targetPos = JoltMath::ToJolt(pos);
  JPH::Quat targetRot = JoltMath::ToJolt(quat);
  if (!JoltPositionIsFinite(targetPos) || !JoltQuaternionIsFinite(targetRot)) {
    return;
  }
  targetRot = targetRot.Normalized();

  JPH::RVec3 currentPos;
  JPH::Quat currentRot;
  bi.GetPositionAndRotation(m_bodyID, currentPos, currentRot);

  const bool targetChanged = JoltPositionChanged(currentPos, targetPos) ||
                             JoltRotationChanged(currentRot, targetRot);

  if (m_isKinematic) {
    /* MoveKinematic derives the velocity that carries the body from its
     * current pose to the scene-graph target over this exact physics step.
     * That velocity is what allows contacts to transfer momentum to Dynamic
     * bodies, unlike a SetPositionAndRotation teleport. A second write to an
     * unchanged target clears the velocity after a movement step; afterwards
     * no Jolt call is made until the target changes again. */
    if (std::isfinite(timeStep) && timeStep > 0.0f && (targetChanged || m_kinematicNeedsStop)) {
      bi.MoveKinematic(m_bodyID, targetPos, targetRot, timeStep);
      m_kinematicNeedsStop = targetChanged;
    }
    return;
  }

  /* Skip no-op transform writes, including harmless conversion noise. */
  if (targetChanged) {
    m_physicsEnv->RemoveLogicActiveContactsForBody(m_bodyID);
    bi.SetPositionAndRotation(m_bodyID, targetPos, targetRot, JPH::EActivation::DontActivate);
  }
  if (staticBody) {
    m_staticSyncedPosition = pos;
    m_staticSyncedOrientation = ori;
    m_staticSyncedBody = m_bodyID;
    m_staticSyncedEpoch = poseEpoch;
  }
}

bool JoltPhysicsController::SynchronizeKinematicPose()
{
  if (!m_isKinematic) {
    return false;
  }
  return SynchronizeSceneGraphPose(true);
}

bool JoltPhysicsController::SynchronizeSceneGraphPose(const bool clearVelocity)
{
  m_staticSyncedEpoch = 0;
  if (!m_physicsEnv || m_bodyID.IsInvalid() || !m_motionState) {
    return false;
  }

  /* A parent can change a child's world scale while the child is absent from
   * the broadphase. Rebuild only if the effective scale actually changed. */
  EnsureScalingStateFromMotionState();
  const MT_Vector3 worldScale = m_motionState->GetWorldScaling();
  if (JoltScaleIsValid(worldScale) && m_gameStateScalingStateValid) {
    const JPH::Vec3 targetScale(worldScale[0], worldScale[2], worldScale[1]);
    if (!JoltScalesMatch(targetScale, m_gameStateKnownScale)) {
      SetScaling(worldScale);
    }
  }

  const JPH::RVec3 targetPosition = JoltMath::ToJolt(m_motionState->GetWorldPosition());
  JPH::Quat targetRotation = JoltMath::ToJolt(m_motionState->GetWorldOrientation().getRotation());
  if (!JoltPositionIsFinite(targetPosition) || !JoltQuaternionIsFinite(targetRotation)) {
    return false;
  }
  targetRotation = targetRotation.Normalized();

  JPH::BodyInterface &bodyInterface = m_physicsEnv->GetBodyInterface();
  JPH::RVec3 currentPosition;
  JPH::Quat currentRotation;
  bodyInterface.GetPositionAndRotation(m_bodyID, currentPosition, currentRotation);
  const bool poseChanged = JoltPositionChanged(currentPosition, targetPosition) ||
                           JoltRotationChanged(currentRotation, targetRotation);
  if (poseChanged) {
    m_physicsEnv->RemoveLogicActiveContactsForBody(m_bodyID);
    /* An already-added body must wake so the teleported broadphase bounds can
     * create contacts with sleeping Dynamic bodies on the next step. Removed
     * bodies are activated by their later add/restore operation. */
    const JPH::EActivation activation = bodyInterface.IsAdded(m_bodyID) ?
                                            JPH::EActivation::Activate :
                                            JPH::EActivation::DontActivate;
    bodyInterface.SetPositionAndRotation(m_bodyID, targetPosition, targetRotation, activation);
  }

  if (clearVelocity) {
    /* SetPositionAndRotation does not clear Kinematic velocity. An authored
     * Kinematic body restored after a teleport therefore needs an explicit
     * stop. A temporary Kinematic body keeps its last follow velocity when it
     * returns to Dynamic motion. */
    JPH::BodyLockWrite lock(m_physicsEnv->GetPhysicsSystem()->GetBodyLockInterface(), m_bodyID);
    if (!lock.Succeeded()) {
      return false;
    }
    JPH::MotionProperties *motion = lock.GetBody().GetMotionPropertiesUnchecked();
    if (!motion) {
      return false;
    }
    motion->SetLinearVelocityClamped(JPH::Vec3::sZero());
    motion->SetAngularVelocityClamped(JPH::Vec3::sZero());
    m_kinematicNeedsStop = false;
  }
  return true;
}

void JoltPhysicsController::WriteDynamicsToMotionState()
{
  if (!m_physicsEnv || m_bodyID.IsInvalid() || !m_motionState) {
    return;
  }

  /* No-lock interface is safe here: called from the main thread while
   * PhysicsSystem::Update() is not running. */
  JPH::BodyInterface &bi = m_physicsEnv->GetBodyInterfaceNoLock();
  JPH::RVec3 joltPos;
  JPH::Quat joltRot;
  bi.GetPositionAndRotation(m_bodyID, joltPos, joltRot);

  MT_Vector3 pos = JoltMath::ToMT(JPH::Vec3(joltPos));

  /* Soft bodies keep a persistent COM->origin offset so the game-object origin
   * stays at the authored spawn position on startup while still following the
   * simulated body translation afterward. Jolt reports identity body rotation
   * for soft bodies, so don't overwrite the object's orientation here. */
  if (m_softBody) {
    pos -= m_softBody->GetBodyOriginOffset();

    /* KX_MotionState::SetWorldPosition writes local position directly.
     * For parented objects we must convert world->local first to avoid writing
     * world coordinates into child local space (startup teleport on child soft bodies). */
    KX_ClientObjectInfo *info = static_cast<KX_ClientObjectInfo *>(m_newClientInfo);
    KX_GameObject *gameobj = (info) ? info->m_gameobject : nullptr;
    if (gameobj && gameobj->GetParent()) {
      gameobj->NodeSetWorldPosition(pos);
    }
    else {
      m_motionState->SetWorldPosition(pos);
    }
    return;
  }

  MT_Quaternion quat = JoltMath::ToMT(joltRot);

  m_motionState->SetWorldPosition(pos);
  m_motionState->SetWorldOrientation(quat);
}

PHY_IMotionState *JoltPhysicsController::GetMotionState()
{
  return m_motionState;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Object Lifecycle
 * \{ */

void JoltPhysicsController::PostProcessReplica(PHY_IMotionState *motionstate,
                                               PHY_IPhysicsController *parentctrl)
{
  (void)parentctrl;
  m_motionState = motionstate;
  EnsureScalingStateFromMotionState();
  /* Replicas are live scene objects even when their source controller belongs to
   * an inactive-layer template. */
  SetLogicObjectSensorActive(true);

  /* GetReplica() temporarily copies the source controller's soft-body pointer so
   * we can defer clone creation. Never keep ownership of that pointer in the
   * replica controller itself: on any early return, retaining it would make the
   * replica destructor delete the source/template soft body. */
  JoltSoftBody *sourceSoftBody = m_softBody;
  m_softBody = nullptr;

  if (!m_physicsEnv || !m_motionState) {
    m_pendingSoftBodySource = nullptr;
    return;
  }

  if (sourceSoftBody) {
    /* ---- Soft body replica path ----
     * Soft-body cloning does not require reading BodyCreationSettings from the
     * source Jolt body. This is important for parented template soft bodies that
     * are intentionally removed from the active world (SuspendDynamics): locking
     * such bodies can fail depending on state, but cloning from JoltSoftBody data
     * remains valid.
     *
     * Invalidate copied BodyID so failure paths can't ever destroy the source
     * body from this replica controller. CloneIntoReplica will create a fresh
     * body later in SetTransform(). */
    m_bodyID = JPH::BodyID();

    /* Mark the hidden-layer template inactive so it stops writing to the shared
     * Blender modifier while replicas are alive. */
    sourceSoftBody->SetActive(false);

    m_pendingSoftBodySource = sourceSoftBody;
    return;
  }

  if (m_bodyID.IsInvalid()) {
    return;
  }

  /* Clone the Jolt body for the replicated game object.
   * Read source creation settings first, then release the body lock before
   * creating/adding the replica body. Holding a body lock while CreateBody /
   * AddBody acquires internal locks can deadlock.
   *
   * No-lock interfaces are safe here: replica creation runs on the main thread
   * while PhysicsSystem::Update() is not running.
   *
   * CRITICAL: We must invalidate m_bodyID BEFORE attempting CreateBody.
   * GetReplica() copies the source's m_bodyID to us. If CreateBody fails,
   * we must NOT retain the source's BodyID - that would cause a double-free
   * when both controllers are destroyed. */
  const JPH::BodyID sourceBodyID = m_bodyID;
  m_bodyID = JPH::BodyID(); /* Invalidate our copy so nobody else tries. */

  JPH::BodyCreationSettings bcs;
  {
    const JPH::BodyLockInterface &lockInterface =
        m_physicsEnv->GetPhysicsSystem()->GetBodyLockInterfaceNoLock();
    JPH::BodyLockRead lock(lockInterface, sourceBodyID);
    if (!lock.Succeeded()) {
      return;
    }

    bcs = lock.GetBody().GetBodyCreationSettings();
  }

  /* ---- Rigid body replica path ---- */
  MT_Vector3 pos = m_motionState->GetWorldPosition();
  MT_Matrix3x3 ori = m_motionState->GetWorldOrientation();
  MT_Quaternion quat = ori.getRotation();

  /* The source body can be in a temporary runtime state (transform parenting,
   * Ghost, or Static Collider), while GetReplica() deliberately resets the
   * replica controller to its authored state. Keep the cloned Jolt body in the
   * same authored state as its controller. */
  bcs.mMotionType = m_originalMotionType;
  bcs.mIsSensor = m_isSensor;
  bcs.mCollideKinematicVsNonDynamic = m_isKinematic && m_isSensor;
  const bool resetReplicaVelocity = m_replicaResetVelocity;
  m_replicaResetVelocity = false;
  if (m_originalMotionType == JPH::EMotionType::Dynamic) {
    /* GetBodyCreationSettings reflects the source's current runtime mode.
     * Normalize properties that define the replica's authored Dynamic type so
     * the Jolt body and controller cannot disagree after a type override. */
    bcs.mAllowedDOFs = BuildAllowedDOFs();
    bcs.mMotionQuality = m_dynamicMotionQuality;
    if (resetReplicaVelocity) {
      bcs.mLinearVelocity = JPH::Vec3::sZero();
      bcs.mAngularVelocity = JPH::Vec3::sZero();
    }
  }
  else if (m_isKinematic) {
    /* A replica has its own scene-graph target. Never inherit the source
     * Kinematic body's in-flight velocity (or a transient suspended Static
     * motion type), otherwise a newly spawned stationary replica drifts for
     * one step before its first target write. */
    bcs.mMotionType = JPH::EMotionType::Kinematic;
    bcs.mAllowDynamicOrKinematic = true;
    bcs.mLinearVelocity = JPH::Vec3::sZero();
    bcs.mAngularVelocity = JPH::Vec3::sZero();
    bcs.mMotionQuality = JPH::EMotionQuality::Discrete;
    m_kinematicNeedsStop = false;
  }
  else if (m_originalMotionType == JPH::EMotionType::Static) {
    /* A temporarily Kinematic static source may carry an in-flight target
     * velocity. Static replicas must not retain it for a later mode change. */
    bcs.mLinearVelocity = JPH::Vec3::sZero();
    bcs.mAngularVelocity = JPH::Vec3::sZero();
  }
  bcs.mPosition = JoltMath::ToJolt(pos);
  bcs.mRotation = JoltMath::ToJolt(quat);
  bcs.mObjectLayer = JoltMakeObjectLayer(m_collisionGroup, m_collisionMask, m_bpCategory);

  JPH::BodyInterface &bi = m_physicsEnv->GetBodyInterfaceNoLock();
  JPH::Body *newBody = bi.CreateBody(bcs);
  if (!newBody) {
    /* Body creation failed (e.g., maxBodies limit exceeded).
     * m_bodyID is already invalid - safe for destructor. */
    return;
  }

  m_bodyID = newBody->GetID();
  newBody->SetCollisionGroup(JPH::CollisionGroup(m_physicsEnv->GetConstraintGroupFilter(),
                                                 (JPH::CollisionGroup::GroupID)GetCollisionGroup(),
                                                 m_bodyID.GetIndexAndSequenceNumber()));
  newBody->SetUserData(reinterpret_cast<JPH::uint64>(m_newClientInfo));
  /* Like original Sensor physics objects (and Bullet), Sensor replicas enter the
   * broadphase only if a legacy collision sensor/callback registers them. C++ Logic
   * Nodes and buoyancy use the unadded body as a query shape. Ghost bodies are Jolt
   * sensors too, but like their originals they always collide. */
  if (!m_isSensorPhysicsType || Registered()) {
    bi.AddBody(m_bodyID, JPH::EActivation::Activate);
    m_physicsEnv->NotifyRigidBodyBodyAdded();
  }
}

void JoltPhysicsController::SetPhysicsEnvironment(PHY_IPhysicsEnvironment *env)
{
  m_physicsEnv = static_cast<JoltPhysicsEnvironment *>(env);
}

void JoltPhysicsController::NotifyFhSettingsChanged()
{
  if (m_physicsEnv) {
    m_physicsEnv->InvalidateFhControllersCache();
  }
}

void JoltPhysicsController::NotifyBuoyancySettingsChanged()
{
  if (m_physicsEnv) {
    m_physicsEnv->InvalidateBuoyancyVolumesCache();
  }
}

void JoltPhysicsController::SetLogicObjectSensorActive(const bool active)
{
  const bool changed = m_logicObjectSensorActive != active;
  m_logicObjectSensorActive = active;
  SetLogicCollisionQueryActive(active);
  if (changed) {
    NotifyBuoyancySettingsChanged();
  }
}

void JoltPhysicsController::SetLogicCollisionQueryActive(const bool active)
{
  if (m_logicCollisionQueryActive == active) {
    return;
  }

  m_logicCollisionQueryActive = active;
  if (m_physicsEnv) {
    m_physicsEnv->RemoveLogicActiveContactsForController(this);
  }
}

void JoltPhysicsController::SetLogicObjectSensorIncludeStatic(const bool includeStatic)
{
  if (m_logicObjectSensorIncludeStatic == includeStatic) {
    return;
  }

  m_logicObjectSensorIncludeStatic = includeStatic;
  if (m_physicsEnv) {
    m_physicsEnv->RemoveLogicActiveContactsForController(this);
  }
}

uint32_t JoltPhysicsController::RegisterCompoundChildBinding(KX_GameObject *childObject,
                                                             const JPH::Vec3 &relativePos,
                                                             const JPH::Quat &relativeRot)
{
  if (!childObject) {
    return 0;
  }

  CompoundChildBinding binding;
  binding.blenderObject = childObject->GetBlenderObject();
  binding.sourceObject = childObject;
  binding.relativePosition = relativePos;
  binding.relativeRotation = relativeRot.Normalized();
  binding.active = true;
  for (size_t index = 0; index < m_compoundChildBindings.size(); ++index) {
    if (!m_compoundChildBindings[index].active) {
      m_compoundChildBindings[index] = binding;
      return uint32_t(index + 1);
    }
  }
  if (m_compoundChildBindings.size() >= size_t(std::numeric_limits<uint32_t>::max())) {
    return 0;
  }
  m_compoundChildBindings.push_back(binding);
  return uint32_t(m_compoundChildBindings.size());
}

void JoltPhysicsController::RemoveCompoundChildBinding(const uint32_t userData)
{
  if (userData == 0 || userData > m_compoundChildBindings.size()) {
    return;
  }

  CompoundChildBinding &binding = m_compoundChildBindings[userData - 1];
  binding.active = false;
  binding.blenderObject = nullptr;
  binding.sourceObject = nullptr;
}

uint32_t JoltPhysicsController::FindCompoundChildBinding(KX_GameObject *childObject) const
{
  if (!childObject) {
    return 0;
  }
  for (size_t index = 0; index < m_compoundChildBindings.size(); ++index) {
    const CompoundChildBinding &binding = m_compoundChildBindings[index];
    if (binding.active && binding.sourceObject == childObject) {
      return uint32_t(index + 1);
    }
  }
  return 0;
}

KX_GameObject *JoltPhysicsController::ResolveCompoundChildObject(
    const JPH::SubShapeID &subShapeID) const
{
  const JPH::StaticCompoundShape *compound = JoltAsStaticCompoundShape(m_shape.GetPtr());
  if (!compound || compound->GetNumSubShapes() == 0) {
    return nullptr;
  }

  JPH::SubShapeID remainder;
  const JPH::uint32 subShapeIndex = compound->GetSubShapeIndexFromID(subShapeID, remainder);
  if (subShapeIndex >= compound->GetNumSubShapes()) {
    return nullptr;
  }
  const JPH::uint32 userData = compound->GetSubShape(subShapeIndex).mUserData;
  if (userData == 0 || userData > m_compoundChildBindings.size()) {
    return nullptr;
  }

  const CompoundChildBinding &binding = m_compoundChildBindings[userData - 1];
  return ResolveCompoundChildBinding(binding);
}

KX_GameObject *JoltPhysicsController::ResolveCompoundChildBinding(
    const CompoundChildBinding &binding) const
{
  if (!binding.active || (!binding.sourceObject && !binding.blenderObject)) {
    return nullptr;
  }

  const KX_ClientObjectInfo *rootInfo = static_cast<const KX_ClientObjectInfo *>(m_newClientInfo);
  KX_GameObject *rootObject = rootInfo ? rootInfo->m_gameobject : nullptr;
  if (!rootObject || !rootObject->GetSGNode()) {
    return nullptr;
  }

  KX_GameObject *bestObject = nullptr;
  float bestScore = std::numeric_limits<float>::max();
  bool foundSource = false;
  JoltFindCompoundChildRecursive(rootObject->GetSGNode(),
                                 *rootObject,
                                 binding.sourceObject,
                                 binding.blenderObject,
                                 binding.relativePosition,
                                 binding.relativeRotation,
                                 bestObject,
                                 bestScore,
                                 foundSource);
  return bestObject;
}

bool JoltPhysicsController::OwnsCompoundChildObject(KX_GameObject *childObject) const
{
  if (!childObject) {
    return false;
  }
  for (const CompoundChildBinding &binding : m_compoundChildBindings) {
    if (!binding.active) {
      continue;
    }
    if (ResolveCompoundChildBinding(binding) == childObject) {
      return true;
    }
  }
  return false;
}

void JoltPhysicsController::SetCompoundChildFlag(const bool compoundChild)
{
  if (m_isCompoundChild == compoundChild) {
    return;
  }
  m_isCompoundChild = compoundChild;
  if (m_physicsEnv) {
    m_physicsEnv->RemoveLogicActiveContactsForController(this);
  }
}

void JoltPhysicsController::SetMassPropertiesTemplate(const JPH::MassProperties &massProperties)
{
  m_massPropertiesTemplate = massProperties;
  m_hasMassPropertiesTemplate = massProperties.mMass > 0.0f;
  m_configuredMass = m_hasMassPropertiesTemplate ? massProperties.mMass : 0.0f;
}

JPH::MassProperties JoltPhysicsController::MassPropertiesForMass(const float mass) const
{
  JPH::MassProperties massProperties;
  if (m_hasMassPropertiesTemplate) {
    massProperties = m_massPropertiesTemplate;
  }
  else if (m_shape) {
    massProperties = m_shape->GetMassProperties();
  }

  if (massProperties.mMass > 0.0f) {
    massProperties.ScaleToMass(mass);
  }
  else {
    massProperties.mMass = mass;
  }
  return massProperties;
}

JPH::EAllowedDOFs JoltPhysicsController::BuildAllowedDOFs() const
{
  return JoltGameStateAllowedDOFs(m_isRigidBody,
                                  {m_lockTranslationX,
                                   m_lockTranslationY,
                                   m_lockTranslationZ,
                                   m_lockRotationX,
                                   m_lockRotationY,
                                   m_lockRotationZ});
}

void JoltPhysicsController::ApplyAllowedDOFs()
{
  if (!m_physicsEnv || m_bodyID.IsInvalid() || m_isKinematic) {
    return;
  }

  const JPH::EAllowedDOFs dofs = BuildAllowedDOFs();
  if (dofs == JPH::EAllowedDOFs::None) {
    return;
  }

  {
    JPH::BodyLockWrite lock(m_physicsEnv->GetPhysicsSystem()->GetBodyLockInterface(), m_bodyID);
    if (!lock.Succeeded()) {
      return;
    }
    JPH::Body &body = lock.GetBody();
    JPH::MotionProperties *motion = body.GetMotionPropertiesUnchecked();
    if (!motion) {
      return;
    }

    const float invMass = motion->GetInverseMassUnchecked();
    const float mass = invMass > 0.0f ?
                           1.0f / invMass :
                           std::max(m_configuredMass,
                                    std::max(m_massPropertiesTemplate.mMass, 1.0e-6f));
    motion->SetMassProperties(dofs, MassPropertiesForMass(mass));
    motion->SetLinearVelocityClamped(motion->GetLinearVelocity());
    motion->SetAngularVelocityClamped(motion->GetAngularVelocity());
  }

  ActivateBodyIfAdded();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Transform Operations
 * \{ */

void JoltPhysicsController::RelativeTranslate(const MT_Vector3 &dloc, bool local)
{
  m_staticSyncedEpoch = 0;
  if (!m_physicsEnv || m_bodyID.IsInvalid() || m_isKinematic) {
    return;
  }

  JPH::BodyInterface &bi = m_physicsEnv->GetBodyInterface();
  JPH::RVec3 curPos = bi.GetPosition(m_bodyID);

  JPH::Vec3 delta;
  if (local) {
    JPH::Quat rot = bi.GetRotation(m_bodyID);
    delta = rot * JoltMath::ToJolt(dloc);
  }
  else {
    delta = JoltMath::ToJolt(dloc);
  }

  if (!delta.IsNearZero()) {
    m_physicsEnv->RemoveLogicActiveContactsForBody(m_bodyID);
  }
  bi.SetPosition(m_bodyID, curPos + delta, JPH::EActivation::Activate);
}

void JoltPhysicsController::RelativeRotate(const MT_Matrix3x3 &drot, bool local)
{
  m_staticSyncedEpoch = 0;
  if (!m_physicsEnv || m_bodyID.IsInvalid() || m_isKinematic) {
    return;
  }

  JPH::BodyInterface &bi = m_physicsEnv->GetBodyInterface();
  JPH::Quat curRot = bi.GetRotation(m_bodyID);

  MT_Quaternion mtQuat = drot.getRotation();
  JPH::Quat deltaRot = JoltMath::ToJolt(mtQuat);

  JPH::Quat newRot;
  if (local) {
    newRot = curRot * deltaRot;
  }
  else {
    newRot = deltaRot * curRot;
  }

  const JPH::Quat targetRot = newRot.Normalized();
  if (JoltRotationChanged(curRot, targetRot)) {
    m_physicsEnv->RemoveLogicActiveContactsForBody(m_bodyID);
  }
  bi.SetRotation(m_bodyID, targetRot, JPH::EActivation::Activate);
}

MT_Matrix3x3 JoltPhysicsController::GetOrientation()
{
  if (!m_physicsEnv || m_bodyID.IsInvalid()) {
    return MT_Matrix3x3::Identity();
  }

  JPH::BodyInterface &bi = m_physicsEnv->GetBodyInterface();
  JPH::Quat rot = bi.GetRotation(m_bodyID);
  MT_Quaternion mtQuat = JoltMath::ToMT(rot);
  return MT_Matrix3x3(mtQuat);
}

void JoltPhysicsController::SetOrientation(const MT_Matrix3x3 &orn)
{
  m_staticSyncedEpoch = 0;
  if (!m_physicsEnv || m_bodyID.IsInvalid() || m_isKinematic) {
    return;
  }

  MT_Quaternion quat = orn.getRotation();

  JPH::BodyInterface &bi = m_physicsEnv->GetBodyInterface();
  const JPH::Quat targetRot = JoltMath::ToJolt(quat);
  if (JoltRotationChanged(bi.GetRotation(m_bodyID), targetRot)) {
    m_physicsEnv->RemoveLogicActiveContactsForBody(m_bodyID);
  }
  bi.SetRotation(m_bodyID, targetRot, JPH::EActivation::Activate);
}

void JoltPhysicsController::SetPosition(const MT_Vector3 &pos)
{
  m_staticSyncedEpoch = 0;
  if (!m_physicsEnv || m_bodyID.IsInvalid() || m_isKinematic) {
    return;
  }

  JPH::BodyInterface &bi = m_physicsEnv->GetBodyInterface();
  const JPH::RVec3 targetPos = JoltMath::ToJolt(pos);
  if (JoltPositionChanged(bi.GetPosition(m_bodyID), targetPos)) {
    m_physicsEnv->RemoveLogicActiveContactsForBody(m_bodyID);
  }
  bi.SetPosition(m_bodyID, targetPos, JPH::EActivation::Activate);
}

void JoltPhysicsController::GetPosition(MT_Vector3 &pos) const
{
  if (!m_physicsEnv || m_bodyID.IsInvalid()) {
    pos = MT_Vector3(0.0f, 0.0f, 0.0f);
    return;
  }

  JPH::BodyInterface &bi = m_physicsEnv->GetBodyInterface();
  JPH::RVec3 joltPos = bi.GetPosition(m_bodyID);
  pos = JoltMath::ToMT(JPH::Vec3(joltPos));
}

void JoltPhysicsController::SetScaling(const MT_Vector3 &scale)
{
  if (!m_physicsEnv || m_bodyID.IsInvalid() || !m_shape || !JoltScaleIsValid(scale)) {
    return;
  }

  EnsureScalingStateFromMotionState();

  /* Jolt ScaledShape has no SetScale — create new ScaledShape wrapping original.
   * For replicas, the shape may already be a ScaledShape with the source object's
   * scale baked in. We need to unwrap it to get the base shape, then apply the
   * new scale. Otherwise ScaleShape would multiply scales (e.g., 3x * 1x = 3x
   * instead of replacing with 1x). */
  JPH::Vec3 joltScale(scale[0], scale[2], scale[1]);

  JPH::RefConst<JPH::Shape> baseShape;
  JPH::Vec3 shapeScale;
  if (m_gameStateScalingStateValid) {
    baseShape = m_gameStateScalingBaseShape;
    shapeScale = joltScale / m_gameStateScalingBaseScale;
  }
  else {
    /* Get the base shape (unwrap ScaledShape if present). */
    baseShape = m_shape;
    if (m_shape->GetSubType() == JPH::EShapeSubType::Scaled) {
      const JPH::ScaledShape *scaledShape = static_cast<const JPH::ScaledShape *>(
          m_shape.GetPtr());
      baseShape = scaledShape->GetInnerShape();
    }
    shapeScale = joltScale;
  }

  /* Apply the new scale to the base shape. */
  JPH::Shape::ShapeResult result = baseShape->ScaleShape(shapeScale);
  if (result.HasError()) {
    return;
  }

  JPH::RefConst<JPH::Shape> newShape = result.Get();

  /* Update the stored shape reference. */
  m_shape = newShape;
  if (m_gameStateScalingStateValid) {
    m_gameStateKnownShape = newShape;
    m_gameStateKnownScale = joltScale;
  }

  JPH::BodyInterface &bi = m_physicsEnv->GetBodyInterface();
  m_physicsEnv->RemoveLogicActiveContactsForBody(m_bodyID);
  bi.SetShape(m_bodyID, newShape, true, JPH::EActivation::Activate);
}

void JoltPhysicsController::SetTransform()
{
  m_staticSyncedEpoch = 0;
  /* --- Deferred soft body clone creation ---
   * PostProcessReplica stores the source here because the replica's world
   * position is only correct AFTER KX_Scene::AddReplicaObject calls
   * UpdateWorldData() + SetTransform() on all nodes.  We consume the pending
   * clone here so CloneIntoReplica reads the right spawn position. */
  if (m_pendingSoftBodySource && m_physicsEnv && m_motionState) {
    JoltSoftBody *originalSb = m_pendingSoftBodySource;
    m_pendingSoftBodySource = nullptr;

    /* By the time SetTransform() is called, UpdateWorldData() has already run and
     * the scene graph parent-child relationships are fully established.  Look up
     * the replica's parent controller here rather than using the stale value from
     * PostProcessReplica (where AddChild() had not been called yet). */
    PHY_IPhysicsController *pc = nullptr;
    KX_ClientObjectInfo *info = static_cast<KX_ClientObjectInfo *>(m_newClientInfo);
    if (info && info->m_gameobject) {
      KX_GameObject *parent = info->m_gameobject->GetParent();
      if (parent) {
        pc = parent->GetPhysicsController();
      }
    }

    JoltSoftBody *clonedSb = originalSb->CloneIntoReplica(this, m_motionState, pc);
    if (clonedSb) {
      if (info && info->m_gameobject) {
        clonedSb->SetGameObject(info->m_gameobject);
      }

      m_softBody = clonedSb;
      m_bodyID = clonedSb->GetBodyID();
      SetDynamic(true);

      m_physicsEnv->AddSoftBodyReplica(clonedSb, clonedSb->GetPinController());
    }
    return;
  }

  if (!m_physicsEnv || m_bodyID.IsInvalid() || !m_motionState) {
    return;
  }

  /* Game-object transforms are the authoritative target for an authored
   * Kinematic body. The next fixed step applies that target via MoveKinematic
   * so Jolt can derive a collision velocity; never teleport it here. */
  if (m_isKinematic) {
    return;
  }

  MT_Vector3 pos = m_motionState->GetWorldPosition();
  MT_Matrix3x3 ori = m_motionState->GetWorldOrientation();
  MT_Quaternion quat = ori.getRotation();

  if (m_softBody) {
    /* Keep soft-body body rotation at identity: orientation is represented by
     * local particle coordinates, not body quaternion. Also preserve the COM
     * offset so WriteDynamicsToMotionState() <-> SetTransform() stay inverse. */
    pos += m_softBody->GetBodyOriginOffset();
    quat = MT_Quaternion(0.0f, 0.0f, 0.0f, 1.0f);
  }

  JPH::BodyInterface &bi = m_physicsEnv->GetBodyInterface();
  const JPH::RVec3 targetPos = JoltMath::ToJolt(pos);
  const JPH::Quat targetRot = JoltMath::ToJolt(quat);
  JPH::RVec3 currentPos;
  JPH::Quat currentRot;
  bi.GetPositionAndRotation(m_bodyID, currentPos, currentRot);
  if (JoltPositionChanged(currentPos, targetPos) || JoltRotationChanged(currentRot, targetRot)) {
    m_physicsEnv->RemoveLogicActiveContactsForBody(m_bodyID);
  }
  bi.SetPositionAndRotation(m_bodyID, targetPos, targetRot, JPH::EActivation::Activate);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Mass & Friction
 * \{ */

MT_Scalar JoltPhysicsController::GetMass()
{
  /* Kinematic bodies have solver-internal mass properties because Jolt needs a
   * MotionProperties allocation, but contacts treat them as infinite mass and
   * UPBGE does not expose a configurable Kinematic mass. Match Static's public
   * convention by reporting zero. */
  if (m_isKinematic || !m_physicsEnv || m_bodyID.IsInvalid()) {
    return 0.0f;
  }

  JPH::BodyLockRead lock(m_physicsEnv->GetPhysicsSystem()->GetBodyLockInterface(), m_bodyID);
  if (!lock.Succeeded()) {
    return 0.0f;
  }
  const JPH::Body &body = lock.GetBody();
  const JPH::MotionProperties *motion = body.GetMotionPropertiesUnchecked();
  if (motion == nullptr) {
    return 0.0f;
  }
  const float invMass = motion->GetInverseMassUnchecked();
  if (invMass > 0.0f && std::isfinite(invMass)) {
    return 1.0f / invMass;
  }
  return m_isDynamic && m_configuredMass > 0.0f && std::isfinite(m_configuredMass) ?
             m_configuredMass :
             0.0f;
}

void JoltPhysicsController::SetMass(MT_Scalar newmass)
{
  if (!m_physicsEnv || m_bodyID.IsInvalid() || !m_isDynamic || !std::isfinite(float(newmass)) ||
      newmass <= 0.0f)
  {
    return;
  }

  JPH::BodyLockWrite lock(m_physicsEnv->GetPhysicsSystem()->GetBodyLockInterface(), m_bodyID);
  if (!lock.Succeeded()) {
    return;
  }
  JPH::Body &body = lock.GetBody();
  JPH::MotionProperties *motion = body.GetMotionPropertiesUnchecked();
  if (!motion) {
    return;
  }
  motion->SetMassProperties(motion->GetAllowedDOFs(), MassPropertiesForMass(float(newmass)));
  m_configuredMass = float(newmass);
}

void JoltPhysicsController::SetShapePreservingMassPropertiesAndCenterOfMass(
    JPH::RefConst<JPH::Shape> shape, JoltShapeQueryDataPtr queryData)
{
  if (!shape) {
    return;
  }

  const JPH::Vec3 centerOfMass = m_shape ? m_shape->GetCenterOfMass() : shape->GetCenterOfMass();
  shape = JoltShapeWithCenterOfMass(shape, centerOfMass);

  InvalidateGameStateScalingState();

  m_shape = shape;
  m_shapeQueryData = std::move(queryData);
  EnsureScalingStateFromMotionState();
  if (!m_physicsEnv || m_bodyID.IsInvalid()) {
    return;
  }

  JPH::BodyInterface &bi = m_physicsEnv->GetBodyInterface();
  m_physicsEnv->RemoveLogicActiveContactsForBody(m_bodyID);
  bi.SetShape(m_bodyID, shape, false, JPH::EActivation::Activate);
}

MT_Scalar JoltPhysicsController::GetFriction()
{
  if (!m_physicsEnv || m_bodyID.IsInvalid()) {
    return 0.5f;
  }
  return m_physicsEnv->GetBodyInterface().GetFriction(m_bodyID);
}

void JoltPhysicsController::SetFriction(MT_Scalar friction)
{
  if (!m_physicsEnv || m_bodyID.IsInvalid()) {
    return;
  }
  m_physicsEnv->GetBodyInterface().SetFriction(m_bodyID, (float)friction);
  m_physicsEnv->GetBodyInterface().InvalidateContactCache(m_bodyID);
}

MT_Scalar JoltPhysicsController::GetRestitution()
{
  if (!m_physicsEnv || m_bodyID.IsInvalid()) {
    return 0.0f;
  }
  return m_physicsEnv->GetBodyInterface().GetRestitution(m_bodyID);
}

void JoltPhysicsController::SetRestitution(MT_Scalar restitution)
{
  if (!m_physicsEnv || m_bodyID.IsInvalid()) {
    return;
  }
  m_physicsEnv->GetBodyInterface().SetRestitution(m_bodyID, (float)restitution);
  m_physicsEnv->GetBodyInterface().InvalidateContactCache(m_bodyID);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Forces & Velocities
 * \{ */

void JoltPhysicsController::ApplyImpulse(const MT_Vector3 &attach,
                                         const MT_Vector3 &impulse,
                                         bool local)
{
  if (!m_physicsEnv || m_bodyID.IsInvalid() || m_isKinematic) {
    return;
  }

  JPH::BodyInterface &bi = m_physicsEnv->GetBodyInterface();
  JPH::Vec3 joltImpulse = JoltMath::ToJolt(impulse);
  JPH::RVec3 joltPoint;

  if (local) {
    JPH::Quat rot = bi.GetRotation(m_bodyID);
    joltImpulse = rot * joltImpulse;
    JPH::RVec3 bodyPos = bi.GetPosition(m_bodyID);
    joltPoint = bodyPos + rot * JoltMath::ToJolt(attach);
  }
  else {
    joltPoint = bi.GetPosition(m_bodyID) + JoltMath::ToJolt(attach);
  }

  bi.AddImpulse(m_bodyID, joltImpulse, joltPoint);
}

void JoltPhysicsController::ApplyTorque(const MT_Vector3 &torque, bool local)
{
  if (!m_physicsEnv || m_bodyID.IsInvalid() || m_isKinematic) {
    return;
  }

  JPH::BodyInterface &bi = m_physicsEnv->GetBodyInterface();
  JPH::Vec3 joltTorque = JoltMath::ToJolt(torque);

  if (local) {
    JPH::Quat rot = bi.GetRotation(m_bodyID);
    joltTorque = rot * joltTorque;
  }

  bi.AddTorque(m_bodyID, joltTorque);
}

void JoltPhysicsController::ApplyForce(const MT_Vector3 &force, bool local)
{
  if (!m_physicsEnv || m_bodyID.IsInvalid() || m_isKinematic) {
    return;
  }

  JPH::BodyInterface &bi = m_physicsEnv->GetBodyInterface();
  JPH::Vec3 joltForce = JoltMath::ToJolt(force);

  if (local) {
    JPH::Quat rot = bi.GetRotation(m_bodyID);
    joltForce = rot * joltForce;
  }

  bi.AddForce(m_bodyID, joltForce);
}

void JoltPhysicsController::SetAngularVelocity(const MT_Vector3 &ang_vel, bool local)
{
  if (!m_physicsEnv || m_bodyID.IsInvalid() || m_isKinematic) {
    return;
  }

  JPH::BodyInterface &bi = m_physicsEnv->GetBodyInterface();
  JPH::Vec3 joltAngVel = JoltMath::ToJolt(ang_vel);

  if (local) {
    JPH::Quat rot = bi.GetRotation(m_bodyID);
    joltAngVel = rot * joltAngVel;
  }

  bi.SetAngularVelocity(m_bodyID, joltAngVel);
}

void JoltPhysicsController::SetLinearVelocity(const MT_Vector3 &lin_vel, bool local)
{
  if (!m_physicsEnv || m_bodyID.IsInvalid() || m_isKinematic) {
    return;
  }

  JPH::BodyInterface &bi = m_physicsEnv->GetBodyInterface();
  JPH::Vec3 joltLinVel = JoltMath::ToJolt(lin_vel);

  if (m_softBody) {
    /* Jolt stores soft-body particle velocities in body-local space.
     * Convert world-space input (local=false) back to body-local so spawned
     * full-duplication soft bodies move in the same direction as normal
     * replicas. For local=true, lin_vel is already in local space. */
    if (!local) {
      JPH::Quat rot = bi.GetRotation(m_bodyID);
      joltLinVel = rot.Conjugated() * joltLinVel;
    }

    /* Jolt's BodyInterface::SetLinearVelocity has an IsRigidBody() guard and
     * is a no-op for soft bodies.  Soft body velocity is per-particle:
     * set all SoftBodyVertex::mVelocity entries uniformly so the whole
     * cloth/rope starts moving at the requested speed on spawn. */
    const JPH::BodyLockInterface &lockIf =
        m_physicsEnv->GetPhysicsSystem()->GetBodyLockInterfaceNoLock();
    JPH::BodyLockWrite lock(lockIf, m_bodyID);
    if (lock.Succeeded() && lock.GetBody().IsSoftBody()) {
      JPH::SoftBodyMotionProperties *mp = static_cast<JPH::SoftBodyMotionProperties *>(
          lock.GetBody().GetMotionProperties());
      for (JPH::SoftBodyVertex &v : mp->GetVertices()) {
        if (v.mInvMass == 0.0f) {
          continue; /* Pinned (kinematic) vertices are position-controlled; injecting
                     * velocity would fight UpdatePinnedVertices and overstress
                     * constraints between pinned / unpinned regions on spawn. */
        }
        v.mVelocity = joltLinVel;
      }
    }
    return;
  }

  if (local) {
    JPH::Quat rot = bi.GetRotation(m_bodyID);
    joltLinVel = rot * joltLinVel;
  }

  bi.SetLinearVelocity(m_bodyID, joltLinVel);
}

MT_Vector3 JoltPhysicsController::GetLinearVelocity()
{
  if (!m_physicsEnv || m_bodyID.IsInvalid()) {
    return MT_Vector3(0.0f, 0.0f, 0.0f);
  }

  JPH::BodyInterface &bi = m_physicsEnv->GetBodyInterface();
  return JoltMath::ToMT(bi.GetLinearVelocity(m_bodyID));
}

MT_Vector3 JoltPhysicsController::GetAngularVelocity()
{
  if (!m_physicsEnv || m_bodyID.IsInvalid()) {
    return MT_Vector3(0.0f, 0.0f, 0.0f);
  }

  JPH::BodyLockRead lock(m_physicsEnv->GetPhysicsSystem()->GetBodyLockInterface(), m_bodyID);
  if (!lock.Succeeded()) {
    return MT_Vector3(0.0f, 0.0f, 0.0f);
  }
  const JPH::Body &body = lock.GetBody();
  return JoltMath::ToMT(body.GetAngularVelocity());
}

MT_Vector3 JoltPhysicsController::GetVelocity(const MT_Vector3 &posin)
{
  if (!m_physicsEnv || m_bodyID.IsInvalid()) {
    return MT_Vector3(0.0f, 0.0f, 0.0f);
  }

  JPH::BodyLockRead lock(m_physicsEnv->GetPhysicsSystem()->GetBodyLockInterface(), m_bodyID);
  if (!lock.Succeeded()) {
    return MT_Vector3(0.0f, 0.0f, 0.0f);
  }
  const JPH::Body &body = lock.GetBody();
  /* PHY_IPhysicsController receives an offset relative to the game-object
   * origin, whereas Jolt's point-velocity API takes an absolute world point. */
  const JPH::RVec3 point = body.GetPosition() + JoltMath::ToJolt(posin);
  JPH::Vec3 pointVel = body.GetPointVelocity(point);
  return JoltMath::ToMT(pointVel);
}

MT_Vector3 JoltPhysicsController::GetLocalInertia()
{
  if (!m_physicsEnv || m_bodyID.IsInvalid()) {
    return MT_Vector3(1.0f, 1.0f, 1.0f);
  }

  JPH::BodyLockRead lock(m_physicsEnv->GetPhysicsSystem()->GetBodyLockInterface(), m_bodyID);
  if (!lock.Succeeded()) {
    return MT_Vector3(1.0f, 1.0f, 1.0f);
  }
  const JPH::Body &body = lock.GetBody();
  if (!body.IsDynamic()) {
    return MT_Vector3(1.0f, 1.0f, 1.0f);
  }
  JPH::Mat44 invInertia = body.GetMotionProperties()->GetLocalSpaceInverseInertia();
  /* Return the diagonal of the inverse inertia, inverted back to inertia. */
  float ix = (invInertia(0, 0) > 0.0f) ? (1.0f / invInertia(0, 0)) : 0.0f;
  float iy = (invInertia(1, 1) > 0.0f) ? (1.0f / invInertia(1, 1)) : 0.0f;
  float iz = (invInertia(2, 2) > 0.0f) ? (1.0f / invInertia(2, 2)) : 0.0f;
  /* Jolt inertia is in Y-up, convert diagonal to Blender Z-up. */
  return MT_Vector3(ix, iz, iy);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Gravity & Damping
 * \{ */

MT_Vector3 JoltPhysicsController::GetGravity()
{
  if (!m_physicsEnv || m_bodyID.IsInvalid()) {
    return MT_Vector3(0.0f, 0.0f, -9.81f);
  }

  MT_Vector3 worldGrav;
  m_physicsEnv->GetGravity(worldGrav);

  JPH::BodyLockRead lock(m_physicsEnv->GetPhysicsSystem()->GetBodyLockInterface(), m_bodyID);
  if (!lock.Succeeded()) {
    return worldGrav;
  }
  const JPH::Body &body = lock.GetBody();
  if (!body.IsDynamic()) {
    return worldGrav;
  }
  float gravFactor = body.GetMotionProperties()->GetGravityFactor();
  return worldGrav * gravFactor;
}

void JoltPhysicsController::SetGravity(const MT_Vector3 &grav)
{
  if (!m_physicsEnv || m_bodyID.IsInvalid()) {
    return;
  }

  MT_Vector3 worldGrav;
  m_physicsEnv->GetGravity(worldGrav);

  /* Compute gravity factor as ratio of desired to world gravity. */
  float worldMag = worldGrav.length();
  float factor = (worldMag > 0.0001f) ? (grav.length() / worldMag) : 0.0f;
  /* Check direction: if opposite, negate. */
  if (worldMag > 0.0001f && grav.length() > 0.0001f) {
    MT_Vector3 worldDir = worldGrav.normalized();
    MT_Vector3 gravDir = grav.normalized();
    if (worldDir.dot(gravDir) < 0.0f) {
      factor = -factor;
    }
  }

  JPH::BodyLockWrite lock(m_physicsEnv->GetPhysicsSystem()->GetBodyLockInterface(), m_bodyID);
  if (!lock.Succeeded()) {
    return;
  }
  JPH::Body &body = lock.GetBody();
  if (body.IsDynamic()) {
    body.GetMotionProperties()->SetGravityFactor(factor);
  }
}

float JoltPhysicsController::GetGravityFactor() const
{
  if (!m_physicsEnv || m_bodyID.IsInvalid()) {
    return 1.0f;
  }
  return m_physicsEnv->GetBodyInterface().GetGravityFactor(m_bodyID);
}

void JoltPhysicsController::SetGravityFactor(float factor)
{
  if (!m_physicsEnv || m_bodyID.IsInvalid()) {
    return;
  }
  m_physicsEnv->GetBodyInterface().SetGravityFactor(m_bodyID, factor);
}

float JoltPhysicsController::GetLinearDamping() const
{
  if (!m_physicsEnv || m_bodyID.IsInvalid()) {
    return 0.0f;
  }

  JPH::BodyLockRead lock(m_physicsEnv->GetPhysicsSystem()->GetBodyLockInterface(), m_bodyID);
  if (!lock.Succeeded()) {
    return 0.0f;
  }
  const JPH::Body &body = lock.GetBody();
  const JPH::MotionProperties *motion = body.GetMotionPropertiesUnchecked();
  if (motion == nullptr) {
    return 0.0f;
  }
  return motion->GetLinearDamping();
}

float JoltPhysicsController::GetAngularDamping() const
{
  if (!m_physicsEnv || m_bodyID.IsInvalid()) {
    return 0.0f;
  }

  JPH::BodyLockRead lock(m_physicsEnv->GetPhysicsSystem()->GetBodyLockInterface(), m_bodyID);
  if (!lock.Succeeded()) {
    return 0.0f;
  }
  const JPH::Body &body = lock.GetBody();
  const JPH::MotionProperties *motion = body.GetMotionPropertiesUnchecked();
  if (motion == nullptr) {
    return 0.0f;
  }
  return motion->GetAngularDamping();
}

void JoltPhysicsController::SetDamping(float linear, float angular)
{
  SetLinearDamping(linear);
  SetAngularDamping(angular);
}

void JoltPhysicsController::SetLinearDamping(float damping)
{
  if (!m_physicsEnv || m_bodyID.IsInvalid()) {
    return;
  }

  JPH::BodyLockWrite lock(m_physicsEnv->GetPhysicsSystem()->GetBodyLockInterface(), m_bodyID);
  if (!lock.Succeeded()) {
    return;
  }
  JPH::Body &body = lock.GetBody();
  JPH::MotionProperties *motion = body.GetMotionPropertiesUnchecked();
  if (motion) {
    motion->SetLinearDamping(std::clamp(damping, 0.0f, 1.0f));
  }
}

void JoltPhysicsController::SetAngularDamping(float damping)
{
  if (!m_physicsEnv || m_bodyID.IsInvalid()) {
    return;
  }

  JPH::BodyLockWrite lock(m_physicsEnv->GetPhysicsSystem()->GetBodyLockInterface(), m_bodyID);
  if (!lock.Succeeded()) {
    return;
  }
  JPH::Body &body = lock.GetBody();
  JPH::MotionProperties *motion = body.GetMotionPropertiesUnchecked();
  if (motion) {
    motion->SetAngularDamping(std::clamp(damping, 0.0f, 1.0f));
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Activation & Suspension
 * \{ */

void JoltPhysicsController::RefreshCollisions()
{
  ActivateBodyIfAdded();
}

void JoltPhysicsController::ActivateBodyIfAdded()
{
  if (!m_physicsEnv || m_bodyID.IsInvalid()) {
    return;
  }

  /* Jolt's BodyInterface::ActivateBody only validates that the BodyID exists.
   * In release builds it can therefore put a body removed from the broadphase
   * back into the active-body list. The next simulation step would then try to
   * update bounds through an invalid broadphase location. */
  JPH::BodyInterface &bodyInterface = m_physicsEnv->GetBodyInterface();
  if (bodyInterface.IsAdded(m_bodyID)) {
    bodyInterface.ActivateBody(m_bodyID);
  }
}

void JoltPhysicsController::SuspendPhysics(bool freeConstraints)
{
  if (m_physicsSuspended || !m_physicsEnv || m_bodyID.IsInvalid()) {
    return;
  }

  /* If physics is currently updating, defer removal. */
  if (m_physicsEnv->IsPhysicsUpdating()) {
    JoltDeferredOp op;
    op.type = JoltDeferredOpType::RemoveBody;
    op.bodyID = m_bodyID;
    op.controller = this;
    m_physicsEnv->QueueDeferredOperation(op);
    m_physicsSuspended = true;
    if (m_transformParentingActive && m_transformParentingKinematic) {
      m_parentingSavedBodyAdded = false;
    }
    NotifyFhSettingsChanged();
    NotifyBuoyancySettingsChanged();
    return;
  }

  JPH::BodyInterface &bi = m_physicsEnv->GetBodyInterface();
  m_physicsEnv->RemovePendingRigidBodyBodyAdd(m_bodyID);
  m_physicsEnv->RemoveLogicActiveContactsForBody(m_bodyID);
  if (bi.IsAdded(m_bodyID)) {
    bi.RemoveBody(m_bodyID);
  }
  m_physicsSuspended = true;
  if (m_transformParentingActive && m_transformParentingKinematic) {
    m_parentingSavedBodyAdded = false;
  }
  NotifyFhSettingsChanged();
  NotifyBuoyancySettingsChanged();
}

void JoltPhysicsController::RestorePhysics()
{
  if (!m_physicsSuspended || !m_physicsEnv || m_bodyID.IsInvalid()) {
    return;
  }

  /* No Collision owns the independent reason this body is absent. Clear only the physics
   * suspension; RestoreDynamics will add the body if gameplay later enables dynamics. */
  if (m_dynamicsSuspended && m_bodyRemovedOnSuspend) {
    m_physicsSuspended = false;
    NotifyFhSettingsChanged();
    NotifyBuoyancySettingsChanged();
    return;
  }

  /* A dedicated Sensor is a demand-driven query body. Restoring physics must not opt it into the
   * broadphase unless a collision sensor currently owns a registration. */
  if (m_isSensorPhysicsType && !Registered()) {
    m_physicsSuspended = false;
    NotifyFhSettingsChanged();
    NotifyBuoyancySettingsChanged();
    return;
  }

  /* If physics is currently updating, defer body re-addition. */
  if (m_physicsEnv->IsPhysicsUpdating()) {
    JoltDeferredOp op;
    op.type = JoltDeferredOpType::AddBody;
    op.bodyID = m_bodyID;
    op.controller = this;
    m_physicsEnv->QueueDeferredOperation(op);
    m_physicsSuspended = false;
    if (m_transformParentingActive && m_transformParentingKinematic) {
      m_parentingSavedBodyAdded = true;
    }
    NotifyFhSettingsChanged();
    NotifyBuoyancySettingsChanged();
    return;
  }

  if (m_isKinematic) {
    SynchronizeKinematicPose();
  }
  m_physicsEnv->GetBodyInterface().AddBody(m_bodyID, JPH::EActivation::Activate);
  m_physicsEnv->NotifyRigidBodyBodyAdded();
  m_physicsSuspended = false;
  if (m_transformParentingActive && m_transformParentingKinematic) {
    m_parentingSavedBodyAdded = true;
  }
  NotifyFhSettingsChanged();
  NotifyBuoyancySettingsChanged();
}

void JoltPhysicsController::SuspendDynamics(bool ghost)
{
  if (m_transformParentingActive) {
    m_parentingSavedDynamicsSuspended = true;
    m_parentingSavedDynamicsMode = ghost ? PHY_DynamicsMode::Ghost : PHY_DynamicsMode::NoCollision;
    return;
  }
  SetSuspendedDynamicsMode(ghost ? PHY_DynamicsMode::Ghost : PHY_DynamicsMode::NoCollision, ghost);
}

void JoltPhysicsController::RestoreDynamics()
{
  if (m_transformParentingActive) {
    m_parentingSavedDynamicsSuspended = false;
    m_parentingSavedDynamicsMode = PHY_DynamicsMode::Dynamic;
    return;
  }
  SetSuspendedDynamicsMode(PHY_DynamicsMode::Dynamic, false);
}

bool JoltPhysicsController::CanSetDynamicsMode(const PHY_DynamicsMode mode,
                                               const bool enabled) const
{
  /* Sensor is a dedicated UPBGE physics type, not merely a Static body with its Jolt sensor flag
   * set. It owns a demand-driven query lifecycle and must never become a solid body while still
   * carrying that identity. No Collision and restoration remain valid reversible states. */
  if (m_isSensorPhysicsType) {
    if (mode == PHY_DynamicsMode::StaticCollider) {
      return false;
    }
    if (mode == PHY_DynamicsMode::Ghost) {
      return enabled;
    }
  }

  return mode != PHY_DynamicsMode::Ghost || m_ghostToggleSupported;
}

void JoltPhysicsController::SetDynamicsMode(PHY_DynamicsMode mode, bool enabled)
{
  if (!CanSetDynamicsMode(mode, enabled)) {
    return;
  }

  if (m_transformParentingActive) {
    if (mode == PHY_DynamicsMode::Ghost) {
      m_parentingSavedIsSensor = enabled;
    }
    else if (enabled && mode != PHY_DynamicsMode::Dynamic) {
      m_parentingSavedDynamicsSuspended = true;
      m_parentingSavedDynamicsMode = mode;
    }
    else {
      m_parentingSavedDynamicsSuspended = false;
      m_parentingSavedDynamicsMode = PHY_DynamicsMode::Dynamic;
    }
    return;
  }
  if (mode == PHY_DynamicsMode::Ghost) {
    SetGhostFlag(enabled);
    return;
  }

  SetSuspendedDynamicsMode(mode, false);
}

void JoltPhysicsController::CaptureOriginalPhysicsType()
{
  m_originalIsRigidBody = m_isRigidBody;
  m_originalIsSensorPhysicsType = m_isSensorPhysicsType;
  m_originalIsSensor = m_isSensor;
  m_originalBroadPhaseCategory = m_bpCategory;
  m_dynamicMotionQuality = JPH::EMotionQuality::Discrete;
  if (m_physicsEnv && !m_bodyID.IsInvalid()) {
    JPH::BodyLockRead lock(m_physicsEnv->GetPhysicsSystem()->GetBodyLockInterface(), m_bodyID);
    if (lock.Succeeded()) {
      const JPH::MotionProperties *motion = lock.GetBody().GetMotionPropertiesUnchecked();
      if (motion) {
        m_dynamicMotionQuality = motion->GetMotionQuality();
      }
    }
  }
  m_originalPhysicsTypeCaptured = true;
  m_savedMotionType = m_originalMotionType;
  m_savedIsSensor = m_originalIsSensor;
}

void JoltPhysicsController::SynchronizeClientPhysicsType()
{
  KX_ClientObjectInfo *info = static_cast<KX_ClientObjectInfo *>(m_newClientInfo);
  if (!info || m_isSensorPhysicsType || info->isSensor()) {
    return;
  }

  /* Actor-targeted collision sensors use this runtime identity in addition to
   * Jolt's motion type. Keep it coherent when a regular Static body becomes a
   * Dynamic/Rigid/Kinematic body, and when authored Static state is restored. */
  info->m_type = (m_isDynamic || m_isKinematic) ? KX_ClientObjectInfo::ACTOR :
                                                  KX_ClientObjectInfo::STATIC;
}

JPH::EMotionType JoltPhysicsController::GetRuntimeMotionType() const
{
  if (m_isKinematic) {
    return JPH::EMotionType::Kinematic;
  }
  return m_isDynamic ? JPH::EMotionType::Dynamic : JPH::EMotionType::Static;
}

bool JoltPhysicsController::CanApplyRuntimePhysicsType(
    const JPH::EMotionType targetMotionType, const bool requireRuntimePhysicsTypeSupport) const
{
  if (!m_physicsEnv || m_bodyID.IsInvalid() || m_softBody || m_isCompound || m_isCompoundChild ||
      m_transformParentingActive || !m_shape)
  {
    return false;
  }

  /* A body that will become moving needs both the Jolt motion-properties
   * allocation made at conversion time and a shape that Jolt permits to move.
   * Deliberately do not allocate or rebuild either at runtime: doing so would
   * make one node execution unpredictably expensive and is unsafe for static
   * triangle/height-field shapes. */
  if (targetMotionType != JPH::EMotionType::Static &&
      ((requireRuntimePhysicsTypeSupport && !m_runtimePhysicsTypeSupported) ||
       m_shape->MustBeStatic()))
  {
    return false;
  }
  if (targetMotionType == JPH::EMotionType::Kinematic) {
    if (m_motionState == nullptr) {
      return false;
    }
    const JPH::RVec3 targetPosition = JoltMath::ToJolt(m_motionState->GetWorldPosition());
    const JPH::Quat targetRotation = JoltMath::ToJolt(
        m_motionState->GetWorldOrientation().getRotation());
    if (!JoltPositionIsFinite(targetPosition) || !JoltQuaternionIsFinite(targetRotation)) {
      return false;
    }
  }

  JPH::BodyLockRead lock(m_physicsEnv->GetPhysicsSystem()->GetBodyLockInterface(), m_bodyID);
  if (!lock.Succeeded()) {
    return false;
  }
  const JPH::Body &body = lock.GetBody();
  return body.IsRigidBody() && !body.IsSoftBody() &&
         (targetMotionType == JPH::EMotionType::Static || body.CanBeKinematicOrDynamic());
}

bool JoltPhysicsController::ApplyRuntimePhysicsTypeState(
    const bool requireRuntimePhysicsTypeSupport)
{
  if (!m_physicsEnv || m_bodyID.IsInvalid() || m_physicsEnv->IsPhysicsUpdating()) {
    return false;
  }

  const JPH::EMotionType targetMotionType = GetRuntimeMotionType();
  if (!CanApplyRuntimePhysicsType(targetMotionType, requireRuntimePhysicsTypeSupport)) {
    return false;
  }

  JPH::BodyInterface &bi = m_physicsEnv->GetBodyInterface();
  /* Authored Sensor objects stay out of the broadphase until a collision
   * sensor registers them. That existing demand-driven query contract must
   * survive Restore Original Physics Type. */
  const bool targetAdded = !m_physicsSuspended &&
                           !(m_bpCategory == JOLT_BP_SENSOR && !Registered());

  m_physicsEnv->RemoveLogicActiveContactsForBody(m_bodyID);
  if (!targetAdded) {
    m_physicsEnv->RemovePendingRigidBodyBodyAdd(m_bodyID);
    if (bi.IsAdded(m_bodyID)) {
      bi.RemoveBody(m_bodyID);
    }
  }
  else if (!bi.IsAdded(m_bodyID)) {
    bi.AddBody(m_bodyID, JPH::EActivation::DontActivate);
    m_physicsEnv->NotifyRigidBodyBodyAdded();
  }

  const JPH::EMotionType previousMotionType = bi.GetMotionType(m_bodyID);
  const JPH::EActivation activation = targetAdded && targetMotionType != JPH::EMotionType::Static ?
                                          JPH::EActivation::Activate :
                                          JPH::EActivation::DontActivate;
  bi.SetMotionType(m_bodyID, targetMotionType, activation);
  if (previousMotionType != targetMotionType) {
    m_physicsEnv->NotifyConstraintBodyMotionTypeChanged(m_bodyID);
  }
  /* LinearCast only affects Dynamic bodies in Jolt. Keep Kinematic bodies out
   * of the active CCD list, but retain the Dynamic preference for a later
   * Set Physics State or Restore Original Physics Type transition. */
  if (targetMotionType == JPH::EMotionType::Dynamic) {
    bi.SetMotionQuality(m_bodyID, m_dynamicMotionQuality);
  }
  else if (targetMotionType == JPH::EMotionType::Kinematic) {
    bi.SetMotionQuality(m_bodyID, JPH::EMotionQuality::Discrete);
  }

  {
    JPH::BodyLockWrite lock(m_physicsEnv->GetPhysicsSystem()->GetBodyLockInterface(), m_bodyID);
    if (!lock.Succeeded()) {
      return false;
    }
    JPH::Body &body = lock.GetBody();
    body.SetIsSensor(m_isSensor);
    body.SetCollideKinematicVsNonDynamic(m_isKinematic && m_isSensor);
  }

  bi.SetObjectLayer(m_bodyID,
                    JoltMakeObjectLayer(m_collisionGroup, m_collisionMask, m_bpCategory));
  bi.InvalidateContactCache(m_bodyID);

  if (m_isKinematic) {
    /* Entering Kinematic is a teleport/rebase, never one frame of inherited
     * Dynamic velocity. Subsequent scene-graph changes use MoveKinematic at
     * the fixed-step boundary and therefore generate correct push velocity. */
    if (!SynchronizeKinematicPose()) {
      return false;
    }
  }
  else if (m_isDynamic) {
    /* Dynamic and Rigid Body share Jolt's Dynamic motion type; the difference
     * is whether the authored rotation degrees of freedom are enabled. */
    ApplyAllowedDOFs();
  }

  m_savedMotionType = targetMotionType;
  m_savedIsSensor = m_isSensor;
  m_kinematicNeedsStop = false;
  SynchronizeClientPhysicsType();
  NotifyFhSettingsChanged();
  NotifyBuoyancySettingsChanged();
  return true;
}

void JoltPhysicsController::ApplyDeferredRuntimePhysicsType(const bool restoreOriginalPhysicsType)
{
  /* Deferred operations are processed immediately after PhysicsSystem::Update.
   * Do not requeue here: controller destruction or a later command may have
   * made the stored request obsolete. */
  (void)ApplyRuntimePhysicsTypeState(!restoreOriginalPhysicsType);
}

bool JoltPhysicsController::SetRuntimePhysicsType(const PHY_RuntimePhysicsType type)
{
  JPH::EMotionType targetMotionType = JPH::EMotionType::Dynamic;
  bool targetRigidBody = false;
  switch (type) {
    case PHY_RuntimePhysicsType::Dynamic:
      break;
    case PHY_RuntimePhysicsType::RigidBody:
      targetRigidBody = true;
      break;
    case PHY_RuntimePhysicsType::Kinematic:
      targetMotionType = JPH::EMotionType::Kinematic;
      break;
  }

  if (!CanApplyRuntimePhysicsType(targetMotionType, true)) {
    return false;
  }

  /* Physics type and Ghost are independent. Leaving a temporary Static/Ghost/
   * No Collision state restores the saved Ghost flag; otherwise preserve the
   * current flag while changing motion ownership. */
  const bool targetSensor = m_dynamicsSuspended ? m_savedIsSensor : m_isSensor;

  m_isDynamic = targetMotionType == JPH::EMotionType::Dynamic;
  m_isKinematic = targetMotionType == JPH::EMotionType::Kinematic;
  m_isRigidBody = targetRigidBody;
  m_isSensorPhysicsType = false;
  m_isSensor = targetSensor;
  m_bpCategory = JOLT_BP_DYNAMIC;
  m_dynamicsSuspended = false;
  m_bodyRemovedOnSuspend = false;
  m_dynamicsMode = PHY_DynamicsMode::Dynamic;
  m_savedMotionType = targetMotionType;
  m_savedIsSensor = targetSensor;

  if (m_physicsEnv->IsPhysicsUpdating()) {
    JoltDeferredOp op;
    op.type = JoltDeferredOpType::SetRuntimePhysicsType;
    op.bodyID = m_bodyID;
    op.controller = this;
    m_physicsEnv->QueueDeferredOperation(op);
    NotifyFhSettingsChanged();
    NotifyBuoyancySettingsChanged();
    return true;
  }
  return ApplyRuntimePhysicsTypeState(true);
}

bool JoltPhysicsController::RestoreOriginalPhysicsType()
{
  if (!CanApplyRuntimePhysicsType(m_originalMotionType, false)) {
    return false;
  }

  m_isDynamic = m_originalMotionType == JPH::EMotionType::Dynamic;
  m_isKinematic = m_originalMotionType == JPH::EMotionType::Kinematic;
  m_isRigidBody = m_originalIsRigidBody;
  m_isSensorPhysicsType = m_originalIsSensorPhysicsType;
  m_isSensor = m_originalIsSensor;
  m_bpCategory = m_originalBroadPhaseCategory;
  m_dynamicsSuspended = false;
  m_bodyRemovedOnSuspend = false;
  m_dynamicsMode = PHY_DynamicsMode::Dynamic;
  m_savedMotionType = m_originalMotionType;
  m_savedIsSensor = m_originalIsSensor;

  if (m_physicsEnv->IsPhysicsUpdating()) {
    JoltDeferredOp op;
    op.type = JoltDeferredOpType::SetRuntimePhysicsType;
    op.bodyID = m_bodyID;
    op.restoreOriginalPhysicsType = true;
    op.controller = this;
    m_physicsEnv->QueueDeferredOperation(op);
    NotifyFhSettingsChanged();
    NotifyBuoyancySettingsChanged();
    return true;
  }
  return ApplyRuntimePhysicsTypeState(false);
}

JoltPhysicsController::TransformParentingBehavior JoltPhysicsController::
    ResolveTransformParentingBehavior() const
{
  if ((m_isDynamic && !m_isKinematic) || (m_shape != nullptr && m_shape->MustBeStatic()) ||
      !CanUseKinematicTransformParenting())
  {
    return TransformParentingBehavior::NoCollision;
  }
  if (m_isSensor) {
    return TransformParentingBehavior::Sensor;
  }
  return TransformParentingBehavior::KinematicCollider;
}

bool JoltPhysicsController::CanUseKinematicTransformParenting() const
{
  if (m_physicsEnv == nullptr || m_bodyID.IsInvalid()) {
    return false;
  }

  JPH::BodyLockRead lock(m_physicsEnv->GetPhysicsSystem()->GetBodyLockInterface(), m_bodyID);
  return lock.Succeeded() && lock.GetBody().GetMotionPropertiesUnchecked() != nullptr;
}

bool JoltPhysicsController::SupportsTransformParenting() const
{
  if (m_physicsEnv == nullptr || m_bodyID.IsInvalid() || m_softBody != nullptr ||
      m_isCompoundChild || m_physicsEnv->IsPhysicsUpdating() ||
      m_physicsEnv->HasConstraintsForBody(m_bodyID))
  {
    return false;
  }

  const TransformParentingBehavior behavior = ResolveTransformParentingBehavior();
  return behavior == TransformParentingBehavior::NoCollision ||
         (m_shape != nullptr && !m_shape->MustBeStatic());
}

bool JoltPhysicsController::BeginTransformParenting()
{
  if (m_transformParentingActive) {
    return !m_transformParentingCompound;
  }
  if (!SupportsTransformParenting() || m_physicsEnv->HasConstraintsForBody(m_bodyID)) {
    return false;
  }

  m_parentingSavedDynamicsMode = m_dynamicsMode;
  m_parentingSavedDynamicsSuspended = m_dynamicsSuspended;
  m_parentingSavedIsKinematic = m_isKinematic;
  m_parentingSavedIsRigidBody = m_isRigidBody;
  m_parentingSavedIsSensor = m_isSensor;
  m_parentingSavedBroadPhaseCategory = m_bpCategory;
  m_parentingSavedBodyAdded = m_physicsEnv->GetBodyInterface().IsAdded(m_bodyID);

  m_transformParentingBehavior = ResolveTransformParentingBehavior();

  m_transformParentingActive = true;
  m_transformParentingCompound = false;
  if (m_transformParentingBehavior == TransformParentingBehavior::NoCollision) {
    SetSuspendedDynamicsMode(PHY_DynamicsMode::NoCollision, false);
    return true;
  }

  if (m_dynamicsSuspended) {
    SetSuspendedDynamicsMode(PHY_DynamicsMode::Dynamic, false);
  }

  JPH::BodyInterface &bi = m_physicsEnv->GetBodyInterface();
  m_parentingSavedMotionType = bi.GetMotionType(m_bodyID);
  m_parentingSavedBodyAdded = bi.IsAdded(m_bodyID);
  m_transformParentingKinematic = true;
  m_isKinematic = true;
  m_isSensor = m_transformParentingBehavior == TransformParentingBehavior::Sensor;
  m_bpCategory = m_isSensor ? JOLT_BP_SENSOR : JOLT_BP_DYNAMIC;

  m_physicsEnv->RemoveLogicActiveContactsForBody(m_bodyID);
  if (!m_physicsSuspended && !bi.IsAdded(m_bodyID)) {
    bi.AddBody(m_bodyID, JPH::EActivation::DontActivate);
    m_physicsEnv->NotifyRigidBodyBodyAdded();
  }
  bi.SetMotionType(m_bodyID, JPH::EMotionType::Kinematic, JPH::EActivation::Activate);
  m_physicsEnv->NotifyConstraintBodyMotionTypeChanged(m_bodyID);
  {
    JPH::BodyLockWrite lock(m_physicsEnv->GetPhysicsSystem()->GetBodyLockInterface(), m_bodyID);
    if (!lock.Succeeded()) {
      EndTransformParenting();
      return false;
    }
    JPH::Body &body = lock.GetBody();
    body.SetIsSensor(m_isSensor);
    body.SetCollideKinematicVsNonDynamic(m_isSensor);
  }
  bi.SetObjectLayer(m_bodyID,
                    JoltMakeObjectLayer(m_collisionGroup, m_collisionMask, m_bpCategory));
  NotifyFhSettingsChanged();
  NotifyBuoyancySettingsChanged();
  if (!SynchronizeKinematicPose()) {
    EndTransformParenting();
    return false;
  }
  return true;
}

bool JoltPhysicsController::SupportsCompoundParenting() const
{
  return m_physicsEnv != nullptr && !m_bodyID.IsInvalid() && m_shape != nullptr &&
         m_softBody == nullptr && !m_isCompound && !m_isCompoundChild &&
         !m_physicsEnv->IsPhysicsUpdating() && !m_physicsEnv->HasConstraintsForBody(m_bodyID);
}

bool JoltPhysicsController::BeginCompoundParenting()
{
  if (m_transformParentingActive) {
    return m_transformParentingCompound;
  }
  if (!SupportsCompoundParenting() || m_physicsEnv->HasConstraintsForBody(m_bodyID)) {
    return false;
  }

  m_parentingSavedDynamicsMode = m_dynamicsMode;
  m_parentingSavedDynamicsSuspended = m_dynamicsSuspended;
  m_parentingSavedIsKinematic = m_isKinematic;
  m_parentingSavedIsRigidBody = m_isRigidBody;
  m_parentingSavedIsSensor = m_isSensor;
  m_parentingSavedBroadPhaseCategory = m_bpCategory;
  m_parentingSavedBodyAdded = m_physicsEnv->GetBodyInterface().IsAdded(m_bodyID);

  m_transformParentingBehavior = TransformParentingBehavior::NoCollision;
  m_transformParentingActive = true;
  m_transformParentingCompound = true;
  m_transformParentingKinematic = false;
  SetSuspendedDynamicsMode(PHY_DynamicsMode::NoCollision, false);
  return true;
}

void JoltPhysicsController::EndTransformParenting()
{
  if (!m_transformParentingActive || m_physicsEnv == nullptr || m_bodyID.IsInvalid()) {
    m_transformParentingActive = false;
    m_transformParentingCompound = false;
    m_transformParentingKinematic = false;
    m_transformParentingBehavior = TransformParentingBehavior::NoCollision;
    return;
  }

  const bool was_parenting_kinematic = m_transformParentingKinematic;
  if (was_parenting_kinematic) {
    SynchronizeSceneGraphPose(m_parentingSavedIsKinematic);
    if (!m_parentingSavedIsKinematic) {
      m_kinematicNeedsStop = false;
    }
    JPH::BodyInterface &bi = m_physicsEnv->GetBodyInterface();
    m_physicsEnv->RemoveLogicActiveContactsForBody(m_bodyID);
    m_isKinematic = m_parentingSavedIsKinematic;
    m_isRigidBody = m_parentingSavedIsRigidBody;
    m_isSensor = m_parentingSavedIsSensor;
    m_bpCategory = m_parentingSavedBroadPhaseCategory;
    bi.SetMotionType(m_bodyID, m_parentingSavedMotionType, JPH::EActivation::Activate);
    m_physicsEnv->NotifyConstraintBodyMotionTypeChanged(m_bodyID);
    {
      JPH::BodyLockWrite lock(m_physicsEnv->GetPhysicsSystem()->GetBodyLockInterface(), m_bodyID);
      if (lock.Succeeded()) {
        JPH::Body &body = lock.GetBody();
        body.SetIsSensor(m_parentingSavedIsSensor);
        body.SetCollideKinematicVsNonDynamic(m_parentingSavedIsSensor && m_isKinematic);
      }
    }
    bi.SetObjectLayer(m_bodyID,
                      JoltMakeObjectLayer(
                          m_collisionGroup, m_collisionMask, m_parentingSavedBroadPhaseCategory));
    if (!m_parentingSavedBodyAdded && bi.IsAdded(m_bodyID)) {
      bi.RemoveBody(m_bodyID);
    }
  }
  else if (m_transformParentingBehavior == TransformParentingBehavior::NoCollision) {
    /* NoCollision removes the body from simulation while the scene graph owns
     * its world pose. Move the dormant body to that final pose before normal
     * dynamics can add it back to the broadphase. */
    SynchronizeSceneGraphPose(false);
  }

  m_transformParentingActive = false;
  m_transformParentingCompound = false;
  m_transformParentingKinematic = false;
  m_transformParentingBehavior = TransformParentingBehavior::NoCollision;

  if (m_parentingSavedDynamicsSuspended) {
    SetSuspendedDynamicsMode(m_parentingSavedDynamicsMode,
                             m_parentingSavedDynamicsMode == PHY_DynamicsMode::Ghost);
  }
  else {
    SetSuspendedDynamicsMode(PHY_DynamicsMode::Dynamic, false);
  }
  if (!was_parenting_kinematic && !m_parentingSavedBodyAdded && !m_physicsSuspended &&
      m_physicsEnv->GetBodyInterface().IsAdded(m_bodyID))
  {
    m_physicsEnv->GetBodyInterface().RemoveBody(m_bodyID);
  }
  if (was_parenting_kinematic) {
    ApplyAllowedDOFs();
  }
  NotifyFhSettingsChanged();
  NotifyBuoyancySettingsChanged();
}

bool JoltPhysicsController::IsTransformParentingActive() const
{
  return m_transformParentingActive;
}

void JoltPhysicsController::SetGhostFlag(bool enabled)
{
  if (!m_physicsEnv || m_bodyID.IsInvalid()) {
    return;
  }

  bool effectiveSensor = enabled;
  if (m_dynamicsSuspended) {
    m_savedIsSensor = enabled;
    if (m_dynamicsMode == PHY_DynamicsMode::StaticCollider) {
      effectiveSensor = false;
    }
    else if (m_dynamicsMode == PHY_DynamicsMode::Ghost) {
      effectiveSensor = true;
    }
  }
  SetSensorFlag(effectiveSensor);

  if (m_physicsEnv->IsPhysicsUpdating()) {
    JoltDeferredOp op;
    op.type = JoltDeferredOpType::SetSensorFlag;
    op.bodyID = m_bodyID;
    op.ghost = effectiveSensor;
    op.controller = this;
    m_physicsEnv->QueueDeferredOperation(op);
    return;
  }

  JPH::BodyInterface &bi = m_physicsEnv->GetBodyInterface();
  bool changed = false;
  {
    JPH::BodyLockWrite lock(m_physicsEnv->GetPhysicsSystem()->GetBodyLockInterface(), m_bodyID);
    if (lock.Succeeded()) {
      JPH::Body &body = lock.GetBody();
      changed = body.IsSensor() != effectiveSensor;
      body.SetIsSensor(effectiveSensor);
      if (m_isKinematic) {
        body.SetCollideKinematicVsNonDynamic(effectiveSensor);
      }
    }
  }

  if (changed) {
    m_physicsEnv->RemoveLogicActiveContactsForBody(m_bodyID);
  }
  if (effectiveSensor && bi.IsAdded(m_bodyID)) {
    bi.ActivateBody(m_bodyID);
  }
}

void JoltPhysicsController::SetSuspendedDynamicsMode(PHY_DynamicsMode mode, bool ghost)
{
  if (!m_physicsEnv || m_bodyID.IsInvalid()) {
    return;
  }

  JPH::BodyInterface &bi = m_physicsEnv->GetBodyInterface();
  const bool was_suspended = m_dynamicsSuspended;

  if (!was_suspended) {
    /* Controller state is the authoritative logical state. During a physics
     * update a preceding Set Physics State request has intentionally updated
     * this state before its Jolt mutation is deferred. Reading the body here
     * would otherwise save the old type and make a later Restore Previous
     * Physics State undo that preceding request. */
    m_savedMotionType = GetRuntimeMotionType();
    m_savedIsSensor = m_isSensor;
  }

  if (mode == PHY_DynamicsMode::Dynamic) {
    if (!was_suspended) {
      m_dynamicsMode = PHY_DynamicsMode::Dynamic;
      return;
    }

    if (m_physicsEnv->IsPhysicsUpdating()) {
      JoltDeferredOp op;
      op.type = JoltDeferredOpType::RestoreDynamics;
      op.bodyID = m_bodyID;
      op.ghost = m_savedIsSensor;
      op.physicsSuspended = m_physicsSuspended;
      op.motionType = m_savedMotionType;
      op.controller = this;
      op.collisionGroup = m_collisionGroup;
      op.collisionMask = m_collisionMask;
      op.bpCategory = m_bpCategory;
      m_physicsEnv->QueueDeferredOperation(op);
      m_dynamicsSuspended = false;
      m_bodyRemovedOnSuspend = false;
      m_dynamicsMode = PHY_DynamicsMode::Dynamic;
      SetSensorFlag(m_savedIsSensor);
      NotifyFhSettingsChanged();
      NotifyBuoyancySettingsChanged();
      return;
    }

    m_physicsEnv->RemoveLogicActiveContactsForBody(m_bodyID);
    if (m_isKinematic) {
      SynchronizeKinematicPose();
    }
    if (m_bodyRemovedOnSuspend && !m_physicsSuspended && !bi.IsAdded(m_bodyID) &&
        (!m_isSensorPhysicsType || Registered()))
    {
      bi.AddBody(m_bodyID, JPH::EActivation::Activate);
      m_physicsEnv->NotifyRigidBodyBodyAdded();
    }
    bi.SetMotionType(m_bodyID, m_savedMotionType, JPH::EActivation::Activate);
    m_physicsEnv->NotifyConstraintBodyMotionTypeChanged(m_bodyID);
    {
      JPH::BodyLockWrite lock(m_physicsEnv->GetPhysicsSystem()->GetBodyLockInterface(), m_bodyID);
      if (lock.Succeeded()) {
        lock.GetBody().SetIsSensor(m_savedIsSensor);
        if (m_isKinematic) {
          lock.GetBody().SetCollideKinematicVsNonDynamic(m_savedIsSensor);
        }
      }
    }
    bi.SetObjectLayer(m_bodyID,
                      JoltMakeObjectLayer(m_collisionGroup, m_collisionMask, m_bpCategory));
    m_dynamicsSuspended = false;
    m_bodyRemovedOnSuspend = false;
    m_dynamicsMode = PHY_DynamicsMode::Dynamic;
    SetSensorFlag(m_savedIsSensor);
    NotifyFhSettingsChanged();
    NotifyBuoyancySettingsChanged();
    return;
  }

  if (m_physicsEnv->IsPhysicsUpdating()) {
    JoltDeferredOp op;
    op.type = JoltDeferredOpType::SuspendDynamics;
    op.bodyID = m_bodyID;
    op.dynamicsMode = mode;
    op.physicsSuspended = m_physicsSuspended;
    op.controller = this;
    op.collisionGroup = m_collisionGroup;
    op.collisionMask = m_collisionMask;
    m_physicsEnv->QueueDeferredOperation(op);
    m_dynamicsSuspended = true;
    m_bodyRemovedOnSuspend = (mode == PHY_DynamicsMode::NoCollision);
    m_dynamicsMode = mode;
    if (mode != PHY_DynamicsMode::NoCollision) {
      SetSensorFlag(ghost);
    }
    NotifyFhSettingsChanged();
    NotifyBuoyancySettingsChanged();
    return;
  }

  m_physicsEnv->RemoveLogicActiveContactsForBody(m_bodyID);

  if (mode == PHY_DynamicsMode::NoCollision) {
    m_physicsEnv->RemovePendingRigidBodyBodyAdd(m_bodyID);
    if (bi.IsAdded(m_bodyID)) {
      bi.RemoveBody(m_bodyID);
    }
    m_bodyRemovedOnSuspend = true;
    m_dynamicsSuspended = true;
    m_dynamicsMode = mode;
    NotifyFhSettingsChanged();
    NotifyBuoyancySettingsChanged();
    return;
  }

  if (!m_physicsSuspended && !bi.IsAdded(m_bodyID)) {
    bi.AddBody(m_bodyID, JPH::EActivation::DontActivate);
    m_physicsEnv->NotifyRigidBodyBodyAdded();
  }

  const bool sensor = ghost;
  const JoltBroadPhaseLayer category = ghost ? JOLT_BP_SENSOR : JOLT_BP_STATIC;

  bi.SetMotionType(m_bodyID, JPH::EMotionType::Static, JPH::EActivation::DontActivate);
  m_physicsEnv->NotifyConstraintBodyMotionTypeChanged(m_bodyID);
  {
    JPH::BodyLockWrite lock(m_physicsEnv->GetPhysicsSystem()->GetBodyLockInterface(), m_bodyID);
    if (lock.Succeeded()) {
      lock.GetBody().SetIsSensor(sensor);
      if (m_isKinematic) {
        lock.GetBody().SetCollideKinematicVsNonDynamic(sensor);
      }
    }
  }
  bi.SetObjectLayer(m_bodyID, JoltMakeObjectLayer(m_collisionGroup, m_collisionMask, category));
  m_dynamicsSuspended = true;
  m_bodyRemovedOnSuspend = false;
  m_dynamicsMode = mode;
  SetSensorFlag(sensor);
  NotifyFhSettingsChanged();
  NotifyBuoyancySettingsChanged();
}

void JoltPhysicsController::SetActive(bool active)
{
  if (!m_physicsEnv || m_bodyID.IsInvalid()) {
    return;
  }

  if (active) {
    ActivateBodyIfAdded();
  }
  else {
    m_physicsEnv->GetBodyInterface().DeactivateBody(m_bodyID);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Collision Groups
 * \{ */

unsigned short JoltPhysicsController::GetCollisionGroup() const
{
  return m_collisionGroup & JOLT_COLLISION_LAYER_MASK;
}

unsigned short JoltPhysicsController::GetCollisionMask() const
{
  return m_collisionMask;
}

void JoltPhysicsController::SetCollisionGroup(unsigned short group)
{
  group &= JOLT_COLLISION_LAYER_MASK;
  const bool changed = (m_collisionGroup != group);
  m_collisionGroup = group;
  if (m_physicsEnv && !m_bodyID.IsInvalid()) {
    /* If physics is currently updating, defer layer change. */
    if (m_physicsEnv->IsPhysicsUpdating()) {
      JoltDeferredOp op;
      op.type = JoltDeferredOpType::SetObjectLayer;
      op.bodyID = m_bodyID;
      op.objectLayer = JoltMakeObjectLayer(m_collisionGroup, m_collisionMask, m_bpCategory);
      op.controller = this;
      m_physicsEnv->QueueDeferredOperation(op);
      return;
    }

    JPH::BodyInterface &bi = m_physicsEnv->GetBodyInterface();
    /* CollisionGroup fields (GroupFilter, SubGroupID) are reserved for
     * constraint "Disable Collisions" filtering — do not overwrite them.
     * Primary collection filtering uses ObjectLayer exclusively. */
    if (changed && bi.IsAdded(m_bodyID)) {
      m_physicsEnv->RemoveLogicActiveContactsForBody(m_bodyID);
    }
    bi.SetObjectLayer(m_bodyID,
                      JoltMakeObjectLayer(m_collisionGroup, m_collisionMask, m_bpCategory));
  }
}

void JoltPhysicsController::SetCollisionMask(unsigned short mask)
{
  /* Jolt collection filtering uses ObjectLayer collection bits only. Keep the
   * legacy mask value for Python/API compatibility, but changing it must not
   * churn Jolt object layers or cached contacts. */
  m_collisionMask = mask;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Replication & Shape
 * \{ */

void JoltPhysicsController::SetRigidBody(bool rigid)
{
  if (m_transformParentingKinematic && !m_parentingSavedIsKinematic) {
    /* A Dynamic body temporarily follows the parent as Kinematic. Preserve a
     * gameplay request for its eventual Dynamic state without changing the
     * temporary transform-ownership contract. */
    m_parentingSavedIsRigidBody = rigid;
    return;
  }
  /* Kinematic orientation is authored by the game transform. Enabling or
   * disabling "physics rotation" cannot change that ownership and must not
   * leave misleading state for API queries or game-state persistence. */
  if (m_isKinematic) {
    return;
  }
  m_isRigidBody = rigid;
  ApplyAllowedDOFs();
}

bool JoltPhysicsController::GetAllowSleeping() const
{
  if (!m_physicsEnv || m_bodyID.IsInvalid()) {
    return true;
  }

  JPH::BodyLockRead lock(m_physicsEnv->GetPhysicsSystem()->GetBodyLockInterface(), m_bodyID);
  if (!lock.Succeeded() || lock.GetBody().GetMotionPropertiesUnchecked() == nullptr) {
    return true;
  }
  return lock.GetBody().GetAllowSleeping();
}

void JoltPhysicsController::SetAllowSleeping(bool allow)
{
  if (!m_physicsEnv || m_bodyID.IsInvalid()) {
    return;
  }

  JPH::BodyLockWrite lock(m_physicsEnv->GetPhysicsSystem()->GetBodyLockInterface(), m_bodyID);
  if (!lock.Succeeded() || lock.GetBody().GetMotionPropertiesUnchecked() == nullptr) {
    return;
  }
  lock.GetBody().SetAllowSleeping(allow);
}

void JoltPhysicsController::SetRigidBodyAxisLockState(const bool lockTranslationX,
                                                      const bool lockTranslationY,
                                                      const bool lockTranslationZ,
                                                      const bool lockRotationX,
                                                      const bool lockRotationY,
                                                      const bool lockRotationZ)
{
  m_lockTranslationX = lockTranslationX;
  m_lockTranslationY = lockTranslationY;
  m_lockTranslationZ = lockTranslationZ;
  m_lockRotationX = lockRotationX;
  m_lockRotationY = lockRotationY;
  m_lockRotationZ = lockRotationZ;
}

void JoltPhysicsController::SetRigidBodyAxisLocks(const bool lockTranslationX,
                                                  const bool lockTranslationY,
                                                  const bool lockTranslationZ,
                                                  const bool lockRotationX,
                                                  const bool lockRotationY,
                                                  const bool lockRotationZ)
{
  /* Axis locks constrain solver-produced Dynamic motion. A Kinematic body is
   * transform-driven, so accepting these values would imply restrictions that
   * MoveKinematic deliberately does not apply. */
  if (m_isKinematic) {
    return;
  }
  SetRigidBodyAxisLockState(lockTranslationX,
                            lockTranslationY,
                            lockTranslationZ,
                            lockRotationX,
                            lockRotationY,
                            lockRotationZ);
  ApplyAllowedDOFs();
}

void JoltPhysicsController::GetRigidBodyAxisLocks(bool &lockTranslationX,
                                                  bool &lockTranslationY,
                                                  bool &lockTranslationZ,
                                                  bool &lockRotationX,
                                                  bool &lockRotationY,
                                                  bool &lockRotationZ) const
{
  if (m_isKinematic) {
    lockTranslationX = false;
    lockTranslationY = false;
    lockTranslationZ = false;
    lockRotationX = false;
    lockRotationY = false;
    lockRotationZ = false;
    return;
  }
  lockTranslationX = m_lockTranslationX;
  lockTranslationY = m_lockTranslationY;
  lockTranslationZ = m_lockTranslationZ;
  lockRotationX = m_lockRotationX;
  lockRotationY = m_lockRotationY;
  lockRotationZ = m_lockRotationZ;
}

bool JoltPhysicsController::GetRigidBodyRotationEnabled() const
{
  return !m_isKinematic && m_isRigidBody;
}

void JoltPhysicsController::SimulationTick(float timestep)
{
  (void)timestep;

  if (!m_physicsEnv || m_bodyID.IsInvalid() || !m_isDynamic || m_dynamicsSuspended) {
    return;
  }

  /* No-lock interface is safe here: called from the main thread while
   * PhysicsSystem::Update() is not running. */
  JPH::BodyInterface &bi = m_physicsEnv->GetBodyInterfaceNoLock();

  /* Clamp minimum linear velocity.
   * Maximum linear velocity is enforced natively by Jolt. */
  if (m_linVelMin > 0.0f) {
    JPH::Vec3 linVel = bi.GetLinearVelocity(m_bodyID);
    float len = linVel.Length();

    if (len > 1e-6f && len < m_linVelMin) {
      bi.SetLinearVelocity(m_bodyID, linVel * (m_linVelMin / len));
    }
  }

  /* Clamp minimum angular velocity.
   * Maximum angular velocity is enforced natively by Jolt. */
  if (m_angVelMin > 0.0f) {
    JPH::Vec3 angVel = bi.GetAngularVelocity(m_bodyID);
    float len = angVel.Length();

    if (len > 1e-6f && len < m_angVelMin) {
      bi.SetAngularVelocity(m_bodyID, angVel * (m_angVelMin / len));
    }
  }
}

PHY_IPhysicsController *JoltPhysicsController::GetReplica()
{
  JoltPhysicsController *replica = new JoltPhysicsController();
  const JPH::EMotionType sourceRuntimeMotionType = GetRuntimeMotionType();

  /* Conversion freezes the authored physics type explicitly. Keep a robust
   * fallback for programmatically created controllers as well: a replica must
   * never inherit transient transform-parenting or suspension state just
   * because such a controller did not go through the Blender converter. */
  JPH::EMotionType replicaOriginalMotionType = GetRuntimeMotionType();
  bool replicaOriginalIsRigidBody = m_isRigidBody;
  bool replicaOriginalIsSensorPhysicsType = m_isSensorPhysicsType;
  bool replicaOriginalIsSensor = m_isSensor;
  JoltBroadPhaseLayer replicaOriginalBroadPhaseCategory = m_bpCategory;
  if (m_dynamicsSuspended) {
    replicaOriginalMotionType = m_savedMotionType;
    replicaOriginalIsSensor = m_savedIsSensor;
  }
  if (m_transformParentingActive) {
    replicaOriginalMotionType = m_parentingSavedMotionType;
    replicaOriginalIsRigidBody = m_parentingSavedIsRigidBody;
    replicaOriginalIsSensor = m_parentingSavedIsSensor;
    replicaOriginalBroadPhaseCategory = m_parentingSavedBroadPhaseCategory;
  }
  if (m_originalPhysicsTypeCaptured) {
    replicaOriginalMotionType = m_originalMotionType;
    replicaOriginalIsRigidBody = m_originalIsRigidBody;
    replicaOriginalIsSensorPhysicsType = m_originalIsSensorPhysicsType;
    replicaOriginalIsSensor = m_originalIsSensor;
    replicaOriginalBroadPhaseCategory = m_originalBroadPhaseCategory;
  }

  replica->m_physicsEnv = m_physicsEnv;
  replica->m_newClientInfo = m_newClientInfo;
  replica->m_shape = m_shape;
  replica->m_shapeQueryData = m_shapeQueryData;
  CopyScalingStateToReplica(*replica);
  replica->m_collisionGroup = m_collisionGroup;
  replica->m_collisionMask = m_collisionMask;
  replica->m_margin = m_margin;
  replica->m_radius = m_radius;
  replica->m_fhEnabled = m_fhEnabled;
  replica->m_fhRotEnabled = m_fhRotEnabled;
  replica->m_fhNormal = m_fhNormal;
  replica->m_fhSpring = m_fhSpring;
  replica->m_fhDamping = m_fhDamping;
  replica->m_fhDistance = m_fhDistance;
  replica->m_buoyancyVolumeEnabled = m_buoyancyVolumeEnabled;
  replica->m_buoyancy = m_buoyancy;
  replica->m_buoyancyLinearDrag = m_buoyancyLinearDrag;
  replica->m_buoyancyAngularDrag = m_buoyancyAngularDrag;
  replica->m_buoyancyFluidVelocity = m_buoyancyFluidVelocity;
  replica->m_linVelMin = m_linVelMin;
  replica->m_linVelMax = m_linVelMax;
  replica->m_angVelMin = m_angVelMin;
  replica->m_angVelMax = m_angVelMax;
  replica->m_massPropertiesTemplate = m_massPropertiesTemplate;
  replica->m_hasMassPropertiesTemplate = m_hasMassPropertiesTemplate;
  replica->m_configuredMass = m_configuredMass;
  /* Replication creates a new gameplay instance, so it starts from authored
   * state rather than copying a transient Set Physics State override. */
  replica->m_isDynamic = replicaOriginalMotionType == JPH::EMotionType::Dynamic;
  replica->m_isKinematic = replicaOriginalMotionType == JPH::EMotionType::Kinematic;
  replica->m_isRigidBody = replicaOriginalIsRigidBody;
  replica->m_isSensorPhysicsType = replicaOriginalIsSensorPhysicsType;
  replica->m_isSensor = replicaOriginalIsSensor;
  replica->m_ghostToggleSupported = m_ghostToggleSupported;
  replica->m_logicObjectSensorActive = m_logicObjectSensorActive;
  replica->m_logicCollisionQueryActive = m_logicCollisionQueryActive;
  replica->m_logicObjectSensorIncludeStatic = m_logicObjectSensorIncludeStatic;
  replica->m_compoundChildBindings = m_compoundChildBindings;
  replica->m_isCompound = m_isCompound;
  replica->m_lockTranslationX = m_lockTranslationX;
  replica->m_lockTranslationY = m_lockTranslationY;
  replica->m_lockTranslationZ = m_lockTranslationZ;
  replica->m_lockRotationX = m_lockRotationX;
  replica->m_lockRotationY = m_lockRotationY;
  replica->m_lockRotationZ = m_lockRotationZ;
  replica->m_bpCategory = replicaOriginalBroadPhaseCategory;
  replica->m_originalMotionType = replicaOriginalMotionType;
  replica->m_originalIsRigidBody = replicaOriginalIsRigidBody;
  replica->m_originalIsSensorPhysicsType = replicaOriginalIsSensorPhysicsType;
  replica->m_originalIsSensor = replicaOriginalIsSensor;
  replica->m_originalBroadPhaseCategory = replicaOriginalBroadPhaseCategory;
  replica->m_originalPhysicsTypeCaptured = true;
  replica->m_dynamicMotionQuality = m_dynamicMotionQuality;
  replica->m_runtimePhysicsTypeSupported = m_runtimePhysicsTypeSupported;
  replica->m_replicaResetVelocity = sourceRuntimeMotionType != replicaOriginalMotionType;
  replica->m_savedMotionType = replicaOriginalMotionType;
  replica->m_savedIsSensor = replicaOriginalIsSensor;
  replica->m_bodyID = m_bodyID;     /* Will be replaced in PostProcessReplica. */
  replica->m_softBody = m_softBody; /* Temporarily carried so PostProcessReplica can clone it. */

  if (m_physicsEnv) {
    m_physicsEnv->AddController(replica);
  }

  return replica;
}

PHY_IPhysicsController *JoltPhysicsController::GetReplicaForSensors()
{
  JoltPhysicsController *replica = new JoltPhysicsController();
  replica->m_physicsEnv = m_physicsEnv;
  replica->m_newClientInfo = m_newClientInfo;
  replica->m_shape = m_shape;
  replica->m_shapeQueryData = m_shapeQueryData;
  CopyScalingStateToReplica(*replica);
  replica->m_collisionGroup = m_collisionGroup;
  replica->m_collisionMask = m_collisionMask;
  replica->m_massPropertiesTemplate = m_massPropertiesTemplate;
  replica->m_hasMassPropertiesTemplate = m_hasMassPropertiesTemplate;
  replica->m_configuredMass = m_configuredMass;
  replica->m_buoyancyVolumeEnabled = m_buoyancyVolumeEnabled;
  replica->m_buoyancy = m_buoyancy;
  replica->m_buoyancyLinearDrag = m_buoyancyLinearDrag;
  replica->m_buoyancyAngularDrag = m_buoyancyAngularDrag;
  replica->m_buoyancyFluidVelocity = m_buoyancyFluidVelocity;
  replica->m_logicObjectSensorActive = m_logicObjectSensorActive;
  replica->m_logicCollisionQueryActive = m_logicCollisionQueryActive;
  replica->m_logicObjectSensorIncludeStatic = m_logicObjectSensorIncludeStatic;
  replica->m_isSensorPhysicsType = true;
  replica->m_isSensor = true;
  replica->m_ghostToggleSupported = false;
  replica->m_lockTranslationX = m_lockTranslationX;
  replica->m_lockTranslationY = m_lockTranslationY;
  replica->m_lockTranslationZ = m_lockTranslationZ;
  replica->m_lockRotationX = m_lockRotationX;
  replica->m_lockRotationY = m_lockRotationY;
  replica->m_lockRotationZ = m_lockRotationZ;
  replica->m_bpCategory = JOLT_BP_SENSOR;
  replica->m_originalMotionType = JPH::EMotionType::Kinematic;
  replica->m_originalIsSensorPhysicsType = true;
  replica->m_originalIsSensor = true;
  replica->m_originalBroadPhaseCategory = JOLT_BP_SENSOR;
  replica->m_originalPhysicsTypeCaptured = true;
  replica->m_savedMotionType = JPH::EMotionType::Kinematic;
  replica->m_savedIsSensor = true;
  replica->m_bodyID = m_bodyID;

  if (m_physicsEnv) {
    m_physicsEnv->AddController(replica);
  }

  return replica;
}

float JoltPhysicsController::GetMargin() const
{
  return m_margin;
}

void JoltPhysicsController::SetMargin(float margin)
{
  m_margin = margin;
}

float JoltPhysicsController::GetRadius() const
{
  return m_radius;
}

void JoltPhysicsController::SetRadius(float radius)
{
  m_radius = radius;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Velocity Clamping
 * \{ */

float JoltPhysicsController::GetLinVelocityMin() const
{
  return m_linVelMin;
}

void JoltPhysicsController::SetLinVelocityMin(float val)
{
  m_linVelMin = val;
}

float JoltPhysicsController::GetLinVelocityMax() const
{
  return m_linVelMax;
}

void JoltPhysicsController::SetLinVelocityMax(float val)
{
  m_linVelMax = val;

  if (!m_physicsEnv || m_bodyID.IsInvalid()) {
    return;
  }

  JPH::BodyInterface &bi = m_physicsEnv->GetBodyInterface();
  bi.SetMaxLinearVelocity(m_bodyID, JoltEffectiveLinearVelocityMax(val));

  if (val > 0.0f) {
    bi.SetLinearVelocity(m_bodyID, bi.GetLinearVelocity(m_bodyID));
  }
}

float JoltPhysicsController::GetAngularVelocityMin() const
{
  return m_angVelMin;
}

void JoltPhysicsController::SetAngularVelocityMin(float val)
{
  m_angVelMin = val;
}

float JoltPhysicsController::GetAngularVelocityMax() const
{
  return m_angVelMax;
}

void JoltPhysicsController::SetAngularVelocityMax(float val)
{
  m_angVelMax = val;

  if (!m_physicsEnv || m_bodyID.IsInvalid()) {
    return;
  }

  JPH::BodyInterface &bi = m_physicsEnv->GetBodyInterface();
  bi.SetMaxAngularVelocity(m_bodyID, JoltEffectiveAngularVelocityMax(val));

  if (val > 0.0f) {
    bi.SetAngularVelocity(m_bodyID, bi.GetAngularVelocity(m_bodyID));
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Compound Shape
 * \{ */

void JoltPhysicsController::AddCompoundChild(PHY_IPhysicsController *child)
{
  TryAddCompoundChild(child);
}

bool JoltPhysicsController::TryAddCompoundChild(PHY_IPhysicsController *child)
{
  JoltPhysicsController *childCtrl = dynamic_cast<JoltPhysicsController *>(child);
  if (!m_isCompound || !m_physicsEnv || m_physicsEnv->IsPhysicsUpdating() ||
      m_bodyID.IsInvalid() || !m_shape || !childCtrl || childCtrl == this ||
      childCtrl->m_physicsEnv != m_physicsEnv || childCtrl->m_bodyID.IsInvalid() ||
      !childCtrl->GetShape() || childCtrl->m_softBody)
  {
    return false;
  }

  /* Build new compound from current shape + child shape. */
  JPH::StaticCompoundShapeSettings compoundSettings;
  JoltAddExistingShapeToCompoundSettings(compoundSettings, m_shape.GetPtr());

  KX_ClientObjectInfo *parentInfo = static_cast<KX_ClientObjectInfo *>(m_newClientInfo);
  KX_ClientObjectInfo *childInfo = static_cast<KX_ClientObjectInfo *>(
      childCtrl->GetNewClientInfo());
  KX_GameObject *parentObject = parentInfo ? parentInfo->m_gameobject : nullptr;
  KX_GameObject *childObject = childInfo ? childInfo->m_gameobject : nullptr;
  if (!parentObject || !childObject) {
    return false;
  }

  if (FindCompoundChildBinding(childObject) != 0) {
    childCtrl->SetCompoundChildFlag(true);
    return true;
  }
  if (childCtrl->m_isCompound || childCtrl->m_isCompoundChild) {
    return false;
  }

  JPH::Vec3 relativePosition;
  JPH::Quat relativeRotation;
  if (!JoltRootRelativeTransform(*parentObject, *childObject, relativePosition, relativeRotation))
  {
    return false;
  }

  const JPH::uint32 childUserData = RegisterCompoundChildBinding(
      childObject, relativePosition, relativeRotation);
  if (childUserData == 0) {
    return false;
  }

  compoundSettings.AddShape(
      relativePosition, relativeRotation, childCtrl->GetShape(), childUserData);

  JPH::Shape::ShapeResult result = compoundSettings.Create();
  if (!result.HasError()) {
    if ((m_isDynamic || m_isKinematic) && result.Get()->MustBeStatic()) {
      RemoveCompoundChildBinding(childUserData);
      return false;
    }
    SetShapePreservingMassPropertiesAndCenterOfMass(
        result.Get(), JoltMergeShapeQueryData(m_shapeQueryData, childCtrl->m_shapeQueryData));
    childCtrl->SetCompoundChildFlag(true);
    return true;
  }
  RemoveCompoundChildBinding(childUserData);
  return false;
}

void JoltPhysicsController::RemoveCompoundChild(PHY_IPhysicsController *child)
{
  JoltPhysicsController *childCtrl = dynamic_cast<JoltPhysicsController *>(child);
  if (!childCtrl || childCtrl->m_physicsEnv != m_physicsEnv) {
    return;
  }
  KX_ClientObjectInfo *childInfo = static_cast<KX_ClientObjectInfo *>(
      childCtrl->GetNewClientInfo());
  KX_GameObject *childObject = childInfo ? childInfo->m_gameobject : nullptr;
  const JPH::uint32 childUserData = FindCompoundChildBinding(childObject);
  const JPH::StaticCompoundShape *compound = JoltAsStaticCompoundShape(m_shape.GetPtr());
  if (childUserData == 0 || !compound) {
    childCtrl->SetCompoundChildFlag(false);
    return;
  }

  JPH::StaticCompoundShapeSettings compoundSettings;
  JPH::uint remainingShapeCount = 0;
  for (JPH::uint index = 0; index < compound->GetNumSubShapes(); ++index) {
    const JPH::CompoundShape::SubShape &subShape = compound->GetSubShape(index);
    if (subShape.mUserData == childUserData) {
      continue;
    }
    compoundSettings.AddShape(JoltCompoundSubShapeLocalPosition(*compound, subShape),
                              subShape.GetRotation(),
                              subShape.mShape,
                              subShape.mUserData);
    ++remainingShapeCount;
  }
  if (remainingShapeCount == 0) {
    return;
  }

  JPH::Shape::ShapeResult result = compoundSettings.Create();
  if (result.HasError()) {
    return;
  }
  SetShapePreservingMassPropertiesAndCenterOfMass(result.Get(), m_shapeQueryData);
  RemoveCompoundChildBinding(childUserData);
  childCtrl->SetCompoundChildFlag(false);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name State Queries
 * \{ */

bool JoltPhysicsController::IsDynamic()
{
  return m_isDynamic;
}

bool JoltPhysicsController::IsCompound()
{
  return m_isCompound;
}

bool JoltPhysicsController::IsDynamicsSuspended() const
{
  return m_dynamicsSuspended;
}

bool JoltPhysicsController::IsPhysicsSuspended()
{
  return m_physicsSuspended;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shape Reinstancing
 * \{ */

bool JoltPhysicsController::ReinstancePhysicsShape(KX_GameObject *from_gameobj,
                                                   RAS_MeshObject *from_meshobj,
                                                   bool dupli,
                                                   bool evaluatedMesh)
{
  (void)dupli;
  (void)evaluatedMesh;

  if (!m_physicsEnv || m_bodyID.IsInvalid() || !from_gameobj || m_softBody || m_isCompound) {
    return false;
  }

  const bool dynamic_shape = m_isDynamic || m_isKinematic || m_isSensor;
  JoltShapeQueryDataPtr newQueryData;
  JPH::RefConst<JPH::Shape> newShape = JoltBuildRuntimeCollisionShape(
      from_gameobj,
      from_meshobj,
      dynamic_shape,
      m_margin,
      m_physicsEnv->GetRayQueryDetailRequirements(),
      newQueryData);
  if (!newShape) {
    return false;
  }

  JPH::BodyInterface &bi = m_physicsEnv->GetBodyInterface();
  m_physicsEnv->RemoveLogicActiveContactsForBody(m_bodyID);
  bi.SetShape(m_bodyID, newShape, true, JPH::EActivation::Activate);
  InvalidateGameStateScalingState();
  m_shape = newShape;
  m_shapeQueryData = std::move(newQueryData);
  EnsureScalingStateFromMotionState();

  return true;
}

bool JoltPhysicsController::ReplacePhysicsShape(PHY_IPhysicsController *phyctrl)
{
  if (!m_physicsEnv || m_bodyID.IsInvalid()) {
    return false;
  }

  JoltPhysicsController *other = static_cast<JoltPhysicsController *>(phyctrl);
  if (!other || !other->m_shape) {
    return false;
  }

  JPH::RefConst<JPH::Shape> replacementShape = other->m_shape;
  JoltShapeQueryDataPtr replacementQueryData = other->m_shapeQueryData;
  if ((m_isDynamic || m_isKinematic) && replacementShape->MustBeStatic()) {
    /* A MeshShape (or a compound containing one) cannot be installed on a
     * moving Jolt body. Rebuild from the source object's authored collision
     * geometry using the same convex fallback as normal conversion. This is a
     * rare explicit API operation, so its allocation cost is preferable to
     * leaving a body in an unsupported state. */
    /* Rebuilding only the root object's mesh would silently drop a compound
     * source's child shapes. Refuse that case until a full compound-to-convex
     * conversion is available. */
    if (other->m_isCompound) {
      return false;
    }
    KX_ClientObjectInfo *sourceInfo = static_cast<KX_ClientObjectInfo *>(
        other->GetNewClientInfo());
    KX_GameObject *sourceObject = sourceInfo ? sourceInfo->m_gameobject : nullptr;
    if (!sourceObject) {
      return false;
    }
    RAS_MeshObject *sourceMesh = sourceObject->GetMeshCount() > 0 ? sourceObject->GetMesh(0) :
                                                                    nullptr;
    replacementShape = JoltBuildRuntimeCollisionShape(
        sourceObject,
        sourceMesh,
        true,
        other->GetMargin(),
        m_physicsEnv->GetRayQueryDetailRequirements(),
        replacementQueryData);
    if (!replacementShape || replacementShape->MustBeStatic()) {
      return false;
    }
  }

  JPH::BodyInterface &bi = m_physicsEnv->GetBodyInterface();
  m_physicsEnv->RemoveLogicActiveContactsForBody(m_bodyID);
  bi.SetShape(m_bodyID, replacementShape, true, JPH::EActivation::Activate);
  InvalidateGameStateScalingState();
  m_shape = replacementShape;
  m_shapeQueryData = std::move(replacementQueryData);
  EnsureScalingStateFromMotionState();
  return true;
}

void JoltPhysicsController::ReplicateConstraints(KX_GameObject *gameobj,
                                                 std::vector<KX_GameObject *> constobj)
{
  if (gameobj->GetConstraints().empty() || !gameobj->GetPhysicsController()) {
    return;
  }

  PHY_IPhysicsEnvironment *physEnv = GetPhysicsEnvironment();

  std::vector<blender::bRigidBodyJointConstraint *> constraints = gameobj->GetConstraints();
  for (blender::bRigidBodyJointConstraint *dat : constraints) {
    for (KX_GameObject *member : constobj) {
      if (dat->tar->id.name + 2 == member->GetName() && member->GetPhysicsController()) {
        physEnv->SetupObjectConstraints(gameobj, member, dat, true);
      }
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name CCD
 * \{ */

void JoltPhysicsController::SetCcdMotionThreshold(float val)
{
  if (!m_physicsEnv || m_bodyID.IsInvalid()) {
    return;
  }

  /* Jolt 5.6 only executes LinearCast CCD for Dynamic non-sensor bodies.
   * Kinematic targets cannot be stopped, so enabling this flag would consume
   * active-CCD bookkeeping without changing collision detection. */
  if (m_isKinematic) {
    m_physicsEnv->GetBodyInterface().SetMotionQuality(m_bodyID, JPH::EMotionQuality::Discrete);
    return;
  }

  if (!m_isDynamic) {
    return;
  }

  /* Enable CCD (LinearCast) if threshold > 0, otherwise use Discrete. */
  m_dynamicMotionQuality = (val > 0.0f) ? JPH::EMotionQuality::LinearCast :
                                          JPH::EMotionQuality::Discrete;

  /* MotionQuality can only be set via BodyInterface::SetMotionQuality. */
  m_physicsEnv->GetBodyInterface().SetMotionQuality(m_bodyID, m_dynamicMotionQuality);
}

bool JoltPhysicsController::GetCcdEnabled() const
{
  if (m_isKinematic || !m_physicsEnv || m_bodyID.IsInvalid()) {
    return false;
  }
  return m_physicsEnv->GetBodyInterface().GetMotionQuality(m_bodyID) ==
         JPH::EMotionQuality::LinearCast;
}

void JoltPhysicsController::SetCcdSweptSphereRadius(float val)
{
  /* No direct Jolt mapping; CCD is automatic with LinearCast quality. */
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Client Info
 * \{ */

void *JoltPhysicsController::GetNewClientInfo()
{
  return m_newClientInfo;
}

void JoltPhysicsController::SetNewClientInfo(void *clientinfo)
{
  m_newClientInfo = clientinfo;
  SynchronizeClientPhysicsType();

  /* Also store in body user data for fast lookup from ContactListener. */
  if (m_physicsEnv && !m_bodyID.IsInvalid()) {
    JPH::BodyLockWrite lock(m_physicsEnv->GetPhysicsSystem()->GetBodyLockInterface(), m_bodyID);
    if (lock.Succeeded()) {
      lock.GetBody().SetUserData(reinterpret_cast<JPH::uint64>(clientinfo));
    }
  }
}

/** \} */
