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

#include <Jolt/Jolt.h>

JPH_SUPPRESS_WARNINGS

#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/MotionType.h>
#include <Jolt/Physics/Collision/Shape/MutableCompoundShape.h>
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

#include <algorithm>
#include <cmath>
#include <cstdio>

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

  /* Keep behavior identical while skipping truly no-op transform writes. */
  if (currentPos.GetX() == targetPos.GetX() && currentPos.GetY() == targetPos.GetY() &&
      currentPos.GetZ() == targetPos.GetZ() && currentRot.GetX() == targetRot.GetX() &&
      currentRot.GetY() == targetRot.GetY() && currentRot.GetZ() == targetRot.GetZ() &&
      currentRot.GetW() == targetRot.GetW()) {
    return;
  }

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

  JPH::BodyInterface &bi = m_physicsEnv->GetBodyInterfaceNoLock();
  JPH::Body *newBody = bi.CreateBody(bcs);
  if (!newBody) {
    /* Body creation failed (e.g., maxBodies limit exceeded).
     * m_bodyID is already invalid - safe for destructor. */
    return;
  }

  m_bodyID = newBody->GetID();
  newBody->SetUserData(reinterpret_cast<JPH::uint64>(m_newClientInfo));
  bi.AddBody(m_bodyID, JPH::EActivation::Activate);
  m_physicsEnv->NotifyRigidBodyBodyAdded();
}

void JoltPhysicsController::SetPhysicsEnvironment(PHY_IPhysicsEnvironment *env)
{
  m_physicsEnv = static_cast<JoltPhysicsEnvironment *>(env);
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

  bi.SetRotation(m_bodyID, newRot.Normalized(), JPH::EActivation::Activate);
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
  bi.SetRotation(m_bodyID, JoltMath::ToJolt(quat), JPH::EActivation::Activate);
}

void JoltPhysicsController::SetPosition(const MT_Vector3 &pos)
{
  if (!m_physicsEnv || m_bodyID.IsInvalid()) {
    return;
  }

  JPH::BodyInterface &bi = m_physicsEnv->GetBodyInterface();
  bi.SetPosition(m_bodyID, JoltMath::ToJolt(pos), JPH::EActivation::Activate);
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
  bi.SetPositionAndRotation(
      m_bodyID, JoltMath::ToJolt(pos), JoltMath::ToJolt(quat), JPH::EActivation::Activate);
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
  if (!m_physicsEnv || m_bodyID.IsInvalid()) {
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
  JPH::MassProperties mp;
  mp.mMass = (float)newmass;
  mp.mInertia = body.GetMotionProperties()->GetLocalSpaceInverseInertia().Inversed();
  mp.ScaleToMass((float)newmass);
  body.GetMotionProperties()->SetMassProperties(JPH::EAllowedDOFs::All, mp);
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
    body.GetMotionProperties()->SetLinearDamping(damping);
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
    body.GetMotionProperties()->SetAngularDamping(damping);
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
    return;
  }

  JPH::BodyInterface &bi = m_physicsEnv->GetBodyInterface();
  m_physicsEnv->RemovePendingRigidBodyBodyAdd(m_bodyID);
  if (bi.IsAdded(m_bodyID)) {
    bi.RemoveBody(m_bodyID);
  }
  m_physicsSuspended = true;
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
    return;
  }

  m_physicsEnv->GetBodyInterface().AddBody(m_bodyID, JPH::EActivation::Activate);
  m_physicsEnv->NotifyRigidBodyBodyAdded();
  m_physicsSuspended = false;
}

void JoltPhysicsController::SuspendDynamics(bool ghost)
{
  if (m_dynamicsSuspended || !m_physicsEnv || m_bodyID.IsInvalid()) {
    return;
  }

  JPH::BodyInterface &bi = m_physicsEnv->GetBodyInterface();

  /* Save current state. */
  m_savedMotionType = bi.GetMotionType(m_bodyID);
  m_savedIsSensor = false;

  {
    JPH::BodyLockRead lock(m_physicsEnv->GetPhysicsSystem()->GetBodyLockInterface(), m_bodyID);
    if (lock.Succeeded()) {
      m_savedIsSensor = lock.GetBody().IsSensor();
    }
  }

  /* Check if physics is currently updating - defer if so. */
  if (m_physicsEnv->IsPhysicsUpdating()) {
    JoltDeferredOp op;
    op.type = JoltDeferredOpType::SuspendDynamics;
    op.bodyID = m_bodyID;
    op.ghost = ghost;
    op.controller = this;
    op.collisionGroup = m_collisionGroup;
    op.collisionMask = m_collisionMask;
    m_physicsEnv->QueueDeferredOperation(op);
    m_dynamicsSuspended = true;
    m_bodyRemovedOnSuspend = !ghost;
    return;
  }

  if (ghost) {
    /* Ghost mode: make body a static sensor (collisions detected but not resolved). */
    bi.SetMotionType(m_bodyID, JPH::EMotionType::Static, JPH::EActivation::DontActivate);
    {
      JPH::BodyLockWrite lock(m_physicsEnv->GetPhysicsSystem()->GetBodyLockInterface(), m_bodyID);
      if (lock.Succeeded()) {
        lock.GetBody().SetIsSensor(true);
      }
    }
    if (bi.IsAdded(m_bodyID)) {
      bi.SetObjectLayer(m_bodyID, JoltMakeObjectLayer(m_collisionGroup, m_collisionMask, JOLT_BP_SENSOR));
    }
    m_bodyRemovedOnSuspend = false;
  }
  else {
    /* Non-ghost: remove the body from the physics world entirely so
     * the parented child has no collision at all.  The body is kept
     * alive (not destroyed) so RestoreDynamics / RemoveParent can
     * re-add it later. */
    if (bi.IsAdded(m_bodyID)) {
      bi.RemoveBody(m_bodyID);
    }
    else {
      m_physicsEnv->RemovePendingRigidBodyBodyAdd(m_bodyID);
    }
    m_bodyRemovedOnSuspend = true;
  }

  m_dynamicsSuspended = true;
}

void JoltPhysicsController::RestoreDynamics()
{
  if (!m_dynamicsSuspended || !m_physicsEnv || m_bodyID.IsInvalid()) {
    return;
  }

  /* Check if physics is currently updating - defer if so. */
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
    return;
  }

  JPH::BodyInterface &bi = m_physicsEnv->GetBodyInterface();

  /* If body was removed from the world on suspend, add it back first. */
  if (m_bodyRemovedOnSuspend) {
    if (!bi.IsAdded(m_bodyID)) {
      bi.AddBody(m_bodyID, JPH::EActivation::Activate);
      m_physicsEnv->NotifyRigidBodyBodyAdded();
    }
  }

  /* Restore motion type. */
  bi.SetMotionType(m_bodyID, m_savedMotionType, JPH::EActivation::Activate);

  /* Restore sensor flag. */
  {
    JPH::BodyLockWrite lock(m_physicsEnv->GetPhysicsSystem()->GetBodyLockInterface(), m_bodyID);
    if (lock.Succeeded()) {
      lock.GetBody().SetIsSensor(m_savedIsSensor);
    }
  }

  /* Restore the original broadphase category in the ObjectLayer. */
  if (bi.IsAdded(m_bodyID)) {
    bi.SetObjectLayer(m_bodyID, JoltMakeObjectLayer(m_collisionGroup, m_collisionMask, m_bpCategory));
  }

  m_dynamicsSuspended = false;
  m_bodyRemovedOnSuspend = false;
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
  return m_collisionGroup;
}

unsigned short JoltPhysicsController::GetCollisionMask() const
{
  return m_collisionMask;
}

void JoltPhysicsController::SetCollisionGroup(unsigned short group)
{
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
     * Primary group/mask filtering uses ObjectLayer exclusively. */
    if (bi.IsAdded(m_bodyID)) {
      bi.SetObjectLayer(m_bodyID, JoltMakeObjectLayer(m_collisionGroup, m_collisionMask, m_bpCategory));
    }
  }
}

void JoltPhysicsController::SetCollisionMask(unsigned short mask)
{
  m_collisionMask = mask;
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
     * Primary group/mask filtering uses ObjectLayer exclusively. */
    if (bi.IsAdded(m_bodyID)) {
      bi.SetObjectLayer(m_bodyID, JoltMakeObjectLayer(m_collisionGroup, m_collisionMask, m_bpCategory));
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Replication & Shape
 * \{ */

void JoltPhysicsController::SetRigidBody(bool rigid)
{
  m_isRigidBody = rigid;
  /* Non-rigid dynamic objects have locked rotation enforced via
   * AllowedDOFs in ConvertObject at body creation time. */
}

void JoltPhysicsController::SimulationTick(float timestep)
{
  if (!m_physicsEnv || m_bodyID.IsInvalid() || !m_isDynamic) {
    return;
  }

  /* No-lock interface is safe here: called from the main thread while
   * PhysicsSystem::Update() is not running. */
  JPH::BodyInterface &bi = m_physicsEnv->GetBodyInterfaceNoLock();

  /* Clamp linear velocity. */
  if (m_linVelMax > 0.0f || m_linVelMin > 0.0f) {
    JPH::Vec3 linVel = bi.GetLinearVelocity(m_bodyID);
    float len = linVel.Length();

    if (m_linVelMax > 0.0f && len > m_linVelMax) {
      bi.SetLinearVelocity(m_bodyID, linVel * (m_linVelMax / len));
    }
    else if (m_linVelMin > 0.0f && len > 1e-6f && len < m_linVelMin) {
      bi.SetLinearVelocity(m_bodyID, linVel * (m_linVelMin / len));
    }
  }

  /* Clamp angular velocity. */
  if (m_angVelMax > 0.0f || m_angVelMin > 0.0f) {
    JPH::Vec3 angVel = bi.GetAngularVelocity(m_bodyID);
    float len = angVel.Length();

    if (m_angVelMax > 0.0f && len > m_angVelMax) {
      bi.SetAngularVelocity(m_bodyID, angVel * (m_angVelMax / len));
    }
    else if (m_angVelMin > 0.0f && len > 1e-6f && len < m_angVelMin) {
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
  replica->m_linVelMin = m_linVelMin;
  replica->m_linVelMax = m_linVelMax;
  replica->m_angVelMin = m_angVelMin;
  replica->m_angVelMax = m_angVelMax;
  replica->m_isDynamic = m_isDynamic;
  replica->m_isRigidBody = m_isRigidBody;
  replica->m_isSensor = m_isSensor;
  replica->m_isCompound = m_isCompound;
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
  replica->m_collisionGroup = m_collisionGroup;
  replica->m_collisionMask = m_collisionMask;
  replica->m_isSensor = true;
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

  /* Get the current shape and check if it's a mutable compound. If not,
   * wrap it in one. Then add the child shape with its relative transform. */
  JPH::BodyInterface &bi = m_physicsEnv->GetBodyInterface();

  /* Build new compound from current shape + child shape. */
  JPH::StaticCompoundShapeSettings compoundSettings;
  if (m_shape) {
    compoundSettings.AddShape(JPH::Vec3::sZero(), JPH::Quat::sIdentity(), m_shape);
  }

  /* Compute child's relative transform. */
  MT_Vector3 myPos, childPos;
  GetPosition(myPos);
  childCtrl->GetPosition(childPos);
  MT_Vector3 relPos = childPos - myPos;

  compoundSettings.AddShape(
      JoltMath::ToJolt(relPos),
      JPH::Quat::sIdentity(),
      childCtrl->GetShape());

  JPH::Shape::ShapeResult result = compoundSettings.Create();
  if (!result.HasError()) {
    m_shape = result.Get();
    bi.SetShape(m_bodyID, m_shape, true, JPH::EActivation::Activate);
  }
}

void JoltPhysicsController::RemoveCompoundChild(PHY_IPhysicsController *child)
{
  /* Removing a sub-shape from a static compound requires rebuilding it.
   * For now, we simply log that this is not yet fully supported.
   * A complete implementation would iterate the compound sub-shapes
   * and rebuild without the removed child. */
  (void)child;
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
  if (!m_physicsEnv || m_bodyID.IsInvalid() || !from_meshobj) {
    return false;
  }

  /* Rebuild the collision shape from the (potentially updated) mesh data. */
  JoltShapeBuilder shapeBuilder;
  shapeBuilder.SetMargin(m_margin);

  /* Determine if we need a convex hull or triangle mesh based on dynamic state. */
  bool useConvex = m_isDynamic;
  shapeBuilder.SetMesh(from_gameobj->GetScene(), from_meshobj, useConvex);

  MT_Vector3 scale = from_gameobj->NodeGetWorldScaling();
  JPH::RefConst<JPH::Shape> newShape = shapeBuilder.Build(useConvex, scale);
  if (!newShape) {
    return false;
  }

  /* Replace the shape on the body. */
  JPH::BodyInterface &bi = m_physicsEnv->GetBodyInterface();
  bi.SetShape(m_bodyID, newShape, true, JPH::EActivation::Activate);
  m_shape = newShape;

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
  bi.SetShape(m_bodyID, other->m_shape, true, JPH::EActivation::Activate);
  m_shape = other->m_shape;
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
