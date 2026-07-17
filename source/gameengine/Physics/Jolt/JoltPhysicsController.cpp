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

bool JoltRotationChanged(const JPH::Quat &current, const JPH::Quat &target)
{
  return std::abs(current.Normalized().Dot(target.Normalized())) < 0.999999f;
}

float JoltEffectiveLinearVelocityMax(const float value)
{
  return value > 0.0f ? value : JOLT_DEFAULT_MAX_LINEAR_VELOCITY;
}

float JoltEffectiveAngularVelocityMax(const float value)
{
  return value > 0.0f ? value : JOLT_DEFAULT_MAX_ANGULAR_VELOCITY;
}

const JPH::StaticCompoundShape *JoltAsStaticCompoundShape(const JPH::Shape *shape)
{
  if (!shape) {
    return nullptr;
  }

  if (shape->GetSubType() == JPH::EShapeSubType::OffsetCenterOfMass) {
    shape = static_cast<const JPH::OffsetCenterOfMassShape *>(shape)
                ->GetInnerShape();
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
  const MT_Vector3 localPosition =
      inverseRootRotation *
      ((child.NodeGetWorldPosition() - root.NodeGetWorldPosition()) * inverseScale);
  const MT_Matrix3x3 localRotation =
      inverseRootRotation * child.NodeGetWorldOrientation();
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
          const float rotationError =
              1.0f - std::abs(candidateRotation.Normalized().Dot(relativeRotation.Normalized()));
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

JPH::RefConst<JPH::Shape> JoltApplyVehicleCenterOfMassOffset(
    JPH::RefConst<JPH::Shape> shape, const blender::Object *blenderobject)
{
  if (!shape || !blenderobject || !(blenderobject->gameflag2 & blender::OB_HAS_VEHICLE) ||
      !blenderobject->vehicle) {
    return shape;
  }

  const blender::GameVehicleSettings *vehicle_settings = blenderobject->vehicle;
  if (vehicle_settings->vehicle_type != blender::OB_VEHICLE_TYPE_CHASSIS &&
      vehicle_settings->vehicle_type != blender::OB_VEHICLE_TYPE_MOTORCYCLE_CHASSIS) {
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
          blenderobject)) {
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

JoltPhysicsController::JoltPhysicsController()
{
}

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
  if (!m_isDynamic) {
    return false;
  }
  if (!m_physicsEnv->GetBodyInterfaceNoLock().IsActive(m_bodyID)) {
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

void JoltPhysicsController::RemoveSoftBodyModifier(blender::Object *ob)
{
  if (m_softBody) {
    m_softBody->CleanupModifier(ob);
  }
}

void JoltPhysicsController::WriteMotionStateToDynamics(bool nondynaonly)
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
  if (nondynaonly && m_isDynamic) {
    return;
  }

  /* No-lock interface is safe here: called from the main thread while
   * PhysicsSystem::Update() is not running. */
  JPH::BodyInterface &bi = m_physicsEnv->GetBodyInterfaceNoLock();

  MT_Vector3 pos = m_motionState->GetWorldPosition();
  MT_Matrix3x3 ori = m_motionState->GetWorldOrientation();

  MT_Quaternion quat = ori.getRotation();

  const JPH::RVec3 targetPos = JoltMath::ToJolt(pos);
  const JPH::Quat targetRot = JoltMath::ToJolt(quat);

  JPH::RVec3 currentPos;
  JPH::Quat currentRot;
  bi.GetPositionAndRotation(m_bodyID, currentPos, currentRot);

  /* Skip no-op transform writes, including harmless conversion noise. */
  if (!JoltPositionChanged(currentPos, targetPos) &&
      !JoltRotationChanged(currentRot, targetRot))
  {
    return;
  }

  m_physicsEnv->RemoveLogicActiveContactsForBody(m_bodyID);
  bi.SetPositionAndRotation(
      m_bodyID, targetPos, targetRot, JPH::EActivation::DontActivate);
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
  m_bodyID = JPH::BodyID();  /* Invalidate our copy so nobody else tries. */

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
  newBody->SetCollisionGroup(JPH::CollisionGroup(
      m_physicsEnv->GetConstraintGroupFilter(),
      (JPH::CollisionGroup::GroupID)GetCollisionGroup(),
      m_bodyID.GetIndexAndSequenceNumber()));
  newBody->SetUserData(reinterpret_cast<JPH::uint64>(m_newClientInfo));
  /* Like original Sensor physics objects (and Bullet), Sensor replicas enter the
   * broadphase only if a legacy collision sensor/callback registers them. C++ Logic
   * Nodes and buoyancy use the unadded body as a query shape. */
  if (!m_isSensor || Registered()) {
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

uint32_t JoltPhysicsController::RegisterCompoundChildBinding(
    KX_GameObject *childObject,
    const JPH::Vec3 &relativePos,
    const JPH::Quat &relativeRot)
{
  if (!childObject ||
      m_compoundChildBindings.size() >= size_t(std::numeric_limits<uint32_t>::max()))
  {
    return 0;
  }

  CompoundChildBinding binding;
  binding.blenderObject = childObject->GetBlenderObject();
  binding.sourceObject = childObject;
  binding.relativePosition = relativePos;
  binding.relativeRotation = relativeRot.Normalized();
  binding.active = true;
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
  JPH::EAllowedDOFs dofs = JPH::EAllowedDOFs::All;
  if (m_lockTranslationX) {
    dofs &= ~JPH::EAllowedDOFs::TranslationX;
  }
  if (m_lockTranslationY) {
    dofs &= ~JPH::EAllowedDOFs::TranslationZ;  /* Blender Y -> Jolt Z */
  }
  if (m_lockTranslationZ) {
    dofs &= ~JPH::EAllowedDOFs::TranslationY;  /* Blender Z -> Jolt Y */
  }

  if (!m_isRigidBody || m_lockRotationX) {
    dofs &= ~JPH::EAllowedDOFs::RotationX;
  }
  if (!m_isRigidBody || m_lockRotationY) {
    dofs &= ~JPH::EAllowedDOFs::RotationZ;  /* Blender Y -> Jolt Z */
  }
  if (!m_isRigidBody || m_lockRotationZ) {
    dofs &= ~JPH::EAllowedDOFs::RotationY;  /* Blender Z -> Jolt Y */
  }
  return dofs;
}

void JoltPhysicsController::ApplyAllowedDOFs()
{
  if (!m_physicsEnv || m_bodyID.IsInvalid()) {
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
    if (!body.IsDynamic()) {
      return;
    }

    JPH::MotionProperties *motion = body.GetMotionProperties();
    const float invMass = motion->GetInverseMassUnchecked();
    const float mass = invMass > 0.0f ? 1.0f / invMass :
                                       std::max(m_massPropertiesTemplate.mMass, 1.0e-6f);
    motion->SetMassProperties(dofs, MassPropertiesForMass(mass));
    motion->SetLinearVelocityClamped(motion->GetLinearVelocity());
    motion->SetAngularVelocityClamped(motion->GetAngularVelocity());
  }

  m_physicsEnv->GetBodyInterface().ActivateBody(m_bodyID);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Transform Operations
 * \{ */

void JoltPhysicsController::RelativeTranslate(const MT_Vector3 &dloc, bool local)
{
  if (!m_physicsEnv || m_bodyID.IsInvalid()) {
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
  if (!m_physicsEnv || m_bodyID.IsInvalid()) {
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
  if (!m_physicsEnv || m_bodyID.IsInvalid()) {
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
  if (!m_physicsEnv || m_bodyID.IsInvalid()) {
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
  if (!m_physicsEnv || m_bodyID.IsInvalid() || !m_shape) {
    return;
  }

  /* Jolt ScaledShape has no SetScale — create new ScaledShape wrapping original.
   * For replicas, the shape may already be a ScaledShape with the source object's
   * scale baked in. We need to unwrap it to get the base shape, then apply the
   * new scale. Otherwise ScaleShape would multiply scales (e.g., 3x * 1x = 3x
   * instead of replacing with 1x). */
  JPH::Vec3 joltScale(scale[0], scale[2], scale[1]);

  /* Get the base shape (unwrap ScaledShape if present). */
  JPH::RefConst<JPH::Shape> baseShape = m_shape;
  if (m_shape->GetSubType() == JPH::EShapeSubType::Scaled) {
    const JPH::ScaledShape *scaledShape = static_cast<const JPH::ScaledShape *>(m_shape.GetPtr());
    baseShape = scaledShape->GetInnerShape();
  }

  /* Apply the new scale to the base shape. */
  JPH::Shape::ShapeResult result = baseShape->ScaleShape(joltScale);
  if (result.HasError()) {
    return;
  }

  JPH::RefConst<JPH::Shape> newShape = result.Get();

  /* Update the stored shape reference. */
  m_shape = newShape;

  JPH::BodyInterface &bi = m_physicsEnv->GetBodyInterface();
  m_physicsEnv->RemoveLogicActiveContactsForBody(m_bodyID);
  bi.SetShape(m_bodyID, newShape, true, JPH::EActivation::Activate);
}

void JoltPhysicsController::SetTransform()
{
  /* --- Deferred soft body clone creation ---
   * PostProcessReplica stores the source here because the replica's world
   * position is only correct AFTER KX_Scene::AddReplicaObject calls
   * UpdateWorldData() + SetTransform() on all nodes.  We consume the pending
   * clone here so CloneIntoReplica reads the right spawn position. */
  if (m_pendingSoftBodySource && m_physicsEnv && m_motionState) {
    JoltSoftBody *originalSb = m_pendingSoftBodySource;
    m_pendingSoftBodySource  = nullptr;

    /* By the time SetTransform() is called, UpdateWorldData() has already run and
     * the scene graph parent-child relationships are fully established.  Look up
     * the replica's parent controller here rather than using the stale value from
     * PostProcessReplica (where AddChild() had not been called yet). */
    PHY_IPhysicsController *pc = nullptr;
    KX_ClientObjectInfo *info  = static_cast<KX_ClientObjectInfo *>(m_newClientInfo);
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
      m_bodyID   = clonedSb->GetBodyID();
      SetDynamic(true);

      m_physicsEnv->AddSoftBodyReplica(clonedSb, clonedSb->GetPinController());
    }
    return;
  }

  if (!m_physicsEnv || m_bodyID.IsInvalid() || !m_motionState) {
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
  if (!m_physicsEnv || m_bodyID.IsInvalid()) {
    return 0.0f;
  }

  JPH::BodyLockRead lock(m_physicsEnv->GetPhysicsSystem()->GetBodyLockInterface(), m_bodyID);
  if (!lock.Succeeded()) {
    return 0.0f;
  }
  const JPH::Body &body = lock.GetBody();
  if (!body.IsDynamic()) {
    return 0.0f;
  }
  float invMass = body.GetMotionProperties()->GetInverseMass();
  return (invMass > 0.0f) ? (1.0f / invMass) : 0.0f;
}

void JoltPhysicsController::SetMass(MT_Scalar newmass)
{
  if (!m_physicsEnv || m_bodyID.IsInvalid() || newmass <= 0.0f) {
    return;
  }

  JPH::BodyLockWrite lock(m_physicsEnv->GetPhysicsSystem()->GetBodyLockInterface(), m_bodyID);
  if (!lock.Succeeded()) {
    return;
  }
  JPH::Body &body = lock.GetBody();
  if (!body.IsDynamic()) {
    return;
  }
  JPH::MotionProperties *motion = body.GetMotionProperties();
  motion->SetMassProperties(motion->GetAllowedDOFs(), MassPropertiesForMass(float(newmass)));
}

void JoltPhysicsController::SetShapePreservingMassPropertiesAndCenterOfMass(
    JPH::RefConst<JPH::Shape> shape, JoltShapeQueryDataPtr queryData)
{
  if (!shape) {
    return;
  }

  const JPH::Vec3 centerOfMass = m_shape ? m_shape->GetCenterOfMass() :
                                             shape->GetCenterOfMass();
  shape = JoltShapeWithCenterOfMass(shape, centerOfMass);

  m_shape = shape;
  m_shapeQueryData = std::move(queryData);
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
  if (!m_physicsEnv || m_bodyID.IsInvalid()) {
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
  if (!m_physicsEnv || m_bodyID.IsInvalid()) {
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
  if (!m_physicsEnv || m_bodyID.IsInvalid()) {
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
  if (!m_physicsEnv || m_bodyID.IsInvalid()) {
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
  if (!m_physicsEnv || m_bodyID.IsInvalid()) {
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
      JPH::SoftBodyMotionProperties *mp =
          static_cast<JPH::SoftBodyMotionProperties *>(lock.GetBody().GetMotionProperties());
      for (JPH::SoftBodyVertex &v : mp->GetVertices()) {
        if (v.mInvMass == 0.0f) {
          continue;  /* Pinned (kinematic) vertices are position-controlled; injecting
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
  JPH::Vec3 pointVel = body.GetPointVelocity(JoltMath::ToJolt(posin));
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
  if (!body.IsDynamic()) {
    return 0.0f;
  }
  return body.GetMotionProperties()->GetLinearDamping();
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
  if (!body.IsDynamic()) {
    return 0.0f;
  }
  return body.GetMotionProperties()->GetAngularDamping();
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
  if (body.IsDynamic()) {
    body.GetMotionProperties()->SetLinearDamping(std::clamp(damping, 0.0f, 1.0f));
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
  if (body.IsDynamic()) {
    body.GetMotionProperties()->SetAngularDamping(std::clamp(damping, 0.0f, 1.0f));
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Activation & Suspension
 * \{ */

void JoltPhysicsController::RefreshCollisions()
{
  if (!m_physicsEnv || m_bodyID.IsInvalid()) {
    return;
  }
  m_physicsEnv->GetBodyInterface().ActivateBody(m_bodyID);
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
  NotifyFhSettingsChanged();
  NotifyBuoyancySettingsChanged();
}

void JoltPhysicsController::RestorePhysics()
{
  if (!m_physicsSuspended || !m_physicsEnv || m_bodyID.IsInvalid()) {
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
    NotifyFhSettingsChanged();
    NotifyBuoyancySettingsChanged();
    return;
  }

  m_physicsEnv->GetBodyInterface().AddBody(m_bodyID, JPH::EActivation::Activate);
  m_physicsEnv->NotifyRigidBodyBodyAdded();
  m_physicsSuspended = false;
  NotifyFhSettingsChanged();
  NotifyBuoyancySettingsChanged();
}

void JoltPhysicsController::SuspendDynamics(bool ghost)
{
  SetSuspendedDynamicsMode(ghost ? PHY_DynamicsMode::Ghost : PHY_DynamicsMode::NoCollision,
                           ghost);
}

void JoltPhysicsController::RestoreDynamics()
{
  SetSuspendedDynamicsMode(PHY_DynamicsMode::Dynamic, false);
}

void JoltPhysicsController::SetDynamicsMode(PHY_DynamicsMode mode, bool enabled)
{
  if (mode == PHY_DynamicsMode::Ghost) {
    SetGhostFlag(enabled);
    return;
  }

  SetSuspendedDynamicsMode(mode, false);
}

void JoltPhysicsController::SetGhostFlag(bool enabled)
{
  if (!m_physicsEnv || m_bodyID.IsInvalid()) {
    return;
  }

  SetSensorFlag(enabled);
  if (m_dynamicsSuspended) {
    m_savedIsSensor = enabled;
  }

  if (m_physicsEnv->IsPhysicsUpdating()) {
    JoltDeferredOp op;
    op.type = JoltDeferredOpType::SetSensorFlag;
    op.bodyID = m_bodyID;
    op.ghost = enabled;
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
      changed = body.IsSensor() != enabled;
      body.SetIsSensor(enabled);
    }
  }

  if (changed) {
    m_physicsEnv->RemoveLogicActiveContactsForBody(m_bodyID);
  }
  if (enabled && bi.IsAdded(m_bodyID)) {
    bi.ActivateBody(m_bodyID);
  }
}

void JoltPhysicsController::SetSuspendedDynamicsMode(PHY_DynamicsMode mode, bool ghost)
{
  if (!m_physicsEnv || m_bodyID.IsInvalid()) {
    return;
  }

  KX_ClientObjectInfo *clientInfo = static_cast<KX_ClientObjectInfo *>(m_newClientInfo);
  KX_GameObject *gameObject = clientInfo ? clientInfo->m_gameobject : nullptr;
  const bool parentedCollisionQuery =
      mode == PHY_DynamicsMode::NoCollision && gameObject && gameObject->GetParent();
  if (m_parentedCollisionQuery != parentedCollisionQuery) {
    m_parentedCollisionQuery = parentedCollisionQuery;
    m_physicsEnv->RemoveLogicActiveContactsForController(this);
  }

  JPH::BodyInterface &bi = m_physicsEnv->GetBodyInterface();
  const bool was_suspended = m_dynamicsSuspended;

  if (!was_suspended) {
    m_savedMotionType = bi.GetMotionType(m_bodyID);
    m_savedIsSensor = m_isSensor;
    JPH::BodyLockRead lock(m_physicsEnv->GetPhysicsSystem()->GetBodyLockInterface(), m_bodyID);
    if (lock.Succeeded()) {
      m_savedIsSensor = lock.GetBody().IsSensor();
    }
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
    if (m_bodyRemovedOnSuspend && !bi.IsAdded(m_bodyID)) {
      bi.AddBody(m_bodyID, JPH::EActivation::Activate);
      m_physicsEnv->NotifyRigidBodyBodyAdded();
    }
    bi.SetMotionType(m_bodyID, m_savedMotionType, JPH::EActivation::Activate);
    m_physicsEnv->NotifyConstraintBodyMotionTypeChanged(m_bodyID);
    {
      JPH::BodyLockWrite lock(m_physicsEnv->GetPhysicsSystem()->GetBodyLockInterface(), m_bodyID);
      if (lock.Succeeded()) {
        lock.GetBody().SetIsSensor(m_savedIsSensor);
      }
    }
    if (bi.IsAdded(m_bodyID)) {
      bi.SetObjectLayer(m_bodyID,
                        JoltMakeObjectLayer(m_collisionGroup, m_collisionMask, m_bpCategory));
    }
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

  if (!bi.IsAdded(m_bodyID)) {
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
    m_physicsEnv->GetBodyInterface().ActivateBody(m_bodyID);
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
    if (bi.IsAdded(m_bodyID)) {
      if (changed) {
        m_physicsEnv->RemoveLogicActiveContactsForBody(m_bodyID);
      }
      bi.SetObjectLayer(m_bodyID, JoltMakeObjectLayer(m_collisionGroup, m_collisionMask, m_bpCategory));
    }
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
  lockTranslationX = m_lockTranslationX;
  lockTranslationY = m_lockTranslationY;
  lockTranslationZ = m_lockTranslationZ;
  lockRotationX = m_lockRotationX;
  lockRotationY = m_lockRotationY;
  lockRotationZ = m_lockRotationZ;
}

bool JoltPhysicsController::GetRigidBodyRotationEnabled() const
{
  return m_isRigidBody;
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
  replica->m_physicsEnv = m_physicsEnv;
  replica->m_newClientInfo = m_newClientInfo;
  replica->m_shape = m_shape;
  replica->m_shapeQueryData = m_shapeQueryData;
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
  replica->m_isDynamic = m_isDynamic;
  replica->m_isRigidBody = m_isRigidBody;
  replica->m_isSensor = m_isSensor;
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
  replica->m_bpCategory = m_bpCategory;
  replica->m_originalMotionType = m_originalMotionType;
  replica->m_bodyID  = m_bodyID;   /* Will be replaced in PostProcessReplica. */
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
  replica->m_collisionGroup = m_collisionGroup;
  replica->m_collisionMask = m_collisionMask;
  replica->m_massPropertiesTemplate = m_massPropertiesTemplate;
  replica->m_hasMassPropertiesTemplate = m_hasMassPropertiesTemplate;
  replica->m_buoyancyVolumeEnabled = m_buoyancyVolumeEnabled;
  replica->m_buoyancy = m_buoyancy;
  replica->m_buoyancyLinearDrag = m_buoyancyLinearDrag;
  replica->m_buoyancyAngularDrag = m_buoyancyAngularDrag;
  replica->m_buoyancyFluidVelocity = m_buoyancyFluidVelocity;
  replica->m_logicObjectSensorActive = m_logicObjectSensorActive;
  replica->m_logicCollisionQueryActive = m_logicCollisionQueryActive;
  replica->m_logicObjectSensorIncludeStatic = m_logicObjectSensorIncludeStatic;
  replica->m_isSensor = true;
  replica->m_lockTranslationX = m_lockTranslationX;
  replica->m_lockTranslationY = m_lockTranslationY;
  replica->m_lockTranslationZ = m_lockTranslationZ;
  replica->m_lockRotationX = m_lockRotationX;
  replica->m_lockRotationY = m_lockRotationY;
  replica->m_lockRotationZ = m_lockRotationZ;
  replica->m_bpCategory = JOLT_BP_SENSOR;
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
  if (!m_physicsEnv || m_bodyID.IsInvalid()) {
    return;
  }

  JoltPhysicsController *childCtrl = static_cast<JoltPhysicsController *>(child);
  if (!childCtrl || !childCtrl->GetShape()) {
    return;
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
    return;
  }

  JPH::Vec3 relativePosition;
  JPH::Quat relativeRotation;
  if (!JoltRootRelativeTransform(
          *parentObject, *childObject, relativePosition, relativeRotation))
  {
    return;
  }

  const JPH::uint32 childUserData = RegisterCompoundChildBinding(
      childObject, relativePosition, relativeRotation);
  if (childUserData == 0) {
    return;
  }

  compoundSettings.AddShape(
      relativePosition, relativeRotation, childCtrl->GetShape(), childUserData);

  JPH::Shape::ShapeResult result = compoundSettings.Create();
  if (!result.HasError()) {
    SetShapePreservingMassPropertiesAndCenterOfMass(
        result.Get(), JoltMergeShapeQueryData(m_shapeQueryData, childCtrl->m_shapeQueryData));
    childCtrl->SetCompoundChildFlag(true);
  }
  else {
    RemoveCompoundChildBinding(childUserData);
  }
}

void JoltPhysicsController::RemoveCompoundChild(PHY_IPhysicsController *child)
{
  JoltPhysicsController *childCtrl = static_cast<JoltPhysicsController *>(child);
  if (!childCtrl) {
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

  const bool dynamic_shape = m_isDynamic || m_isSensor;
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
  m_shape = newShape;
  m_shapeQueryData = std::move(newQueryData);

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

  JPH::BodyInterface &bi = m_physicsEnv->GetBodyInterface();
  m_physicsEnv->RemoveLogicActiveContactsForBody(m_bodyID);
  bi.SetShape(m_bodyID, other->m_shape, true, JPH::EActivation::Activate);
  m_shape = other->m_shape;
  m_shapeQueryData = other->m_shapeQueryData;
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

  /* Enable CCD (LinearCast) if threshold > 0, otherwise use Discrete. */
  JPH::EMotionQuality quality = (val > 0.0f) ? JPH::EMotionQuality::LinearCast :
                                                JPH::EMotionQuality::Discrete;

  /* MotionQuality can only be set via BodyInterface::SetMotionQuality. */
  m_physicsEnv->GetBodyInterface().SetMotionQuality(m_bodyID, quality);
}

bool JoltPhysicsController::GetCcdEnabled() const
{
  if (!m_physicsEnv || m_bodyID.IsInvalid()) {
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

  /* Also store in body user data for fast lookup from ContactListener. */
  if (m_physicsEnv && !m_bodyID.IsInvalid()) {
    JPH::BodyLockWrite lock(m_physicsEnv->GetPhysicsSystem()->GetBodyLockInterface(), m_bodyID);
    if (lock.Succeeded()) {
      lock.GetBody().SetUserData(reinterpret_cast<JPH::uint64>(clientinfo));
    }
  }
}

/** \} */
