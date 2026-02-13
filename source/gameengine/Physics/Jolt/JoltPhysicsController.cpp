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

#include "JoltDefaultMotionState.h"
#include "JoltMathUtils.h"
#include "JoltPhysicsEnvironment.h"
#include "JoltShapeBuilder.h"
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
  /* Remove body from the physics system if we still have a valid environment. */
  if (m_physicsEnv && !m_bodyID.IsInvalid()) {
    JPH::BodyInterface &bi = m_physicsEnv->GetBodyInterface();
    if (bi.IsAdded(m_bodyID)) {
      bi.RemoveBody(m_bodyID);
    }
    bi.DestroyBody(m_bodyID);
    m_bodyID = JPH::BodyID(); /* Invalidate to prevent double-free. */
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
  if (!m_physicsEnv || m_bodyID.IsInvalid()) {
    return;
  }

  /* Soft body vertex sync is handled by JoltSoftBody::SyncVertices()
   * which is called from JoltPhysicsEnvironment::UpdateSoftBodies(). */
}

void JoltPhysicsController::SetSoftBodyTransform(const MT_Vector3 &pos, const MT_Matrix3x3 &ori)
{
  if (!m_physicsEnv || m_bodyID.IsInvalid()) {
    return;
  }

  /* Set the soft body's world transform. */
  JPH::BodyInterface &bi = m_physicsEnv->GetBodyInterface();
  bi.SetPositionAndRotation(
      m_bodyID,
      JoltMath::ToJolt(pos),
      JoltMath::ToJolt(ori.getRotation()),
      JPH::EActivation::Activate);
}

void JoltPhysicsController::RemoveSoftBodyModifier(blender::Object *ob)
{
  /* Soft body modifiers are managed at the Blender level.
   * No Jolt-specific cleanup needed here. */
}

void JoltPhysicsController::WriteMotionStateToDynamics(bool nondynaonly)
{
  if (!m_physicsEnv || m_bodyID.IsInvalid() || !m_motionState) {
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

  bi.SetPositionAndRotation(
      m_bodyID, JoltMath::ToJolt(pos), JoltMath::ToJolt(quat), JPH::EActivation::DontActivate);
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
  m_motionState = motionstate;

  if (!m_physicsEnv || m_bodyID.IsInvalid()) {
    return;
  }

  /* Clone the Jolt body for the replicated game object. */
  JPH::BodyInterface &bi = m_physicsEnv->GetBodyInterface();
  JPH::BodyLockRead lock(m_physicsEnv->GetPhysicsSystem()->GetBodyLockInterface(), m_bodyID);
  if (!lock.Succeeded()) {
    return;
  }

  const JPH::Body &srcBody = lock.GetBody();
  JPH::BodyCreationSettings bcs = srcBody.GetBodyCreationSettings();

  /* Use the replica's motion state position. */
  MT_Vector3 pos = motionstate->GetWorldPosition();
  MT_Matrix3x3 ori = motionstate->GetWorldOrientation();
  MT_Quaternion quat = ori.getRotation();
  bcs.mPosition = JoltMath::ToJolt(pos);
  bcs.mRotation = JoltMath::ToJolt(quat);

  JPH::Body *newBody = bi.CreateBody(bcs);
  if (newBody) {
    m_bodyID = newBody->GetID();
    newBody->SetUserData(reinterpret_cast<JPH::uint64>(m_newClientInfo));
    bi.AddBody(m_bodyID, JPH::EActivation::Activate);
  }
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

  /* Jolt ScaledShape has no SetScale — create new ScaledShape wrapping original. */
  JPH::Vec3 joltScale(scale[0], scale[2], scale[1]);

  JPH::Shape::ShapeResult result = m_shape->ScaleShape(joltScale);
  if (result.HasError()) {
    return;
  }

  JPH::BodyInterface &bi = m_physicsEnv->GetBodyInterface();
  bi.SetShape(m_bodyID, result.Get(), true, JPH::EActivation::Activate);
}

void JoltPhysicsController::SetTransform()
{
  if (!m_physicsEnv || m_bodyID.IsInvalid() || !m_motionState) {
    return;
  }

  MT_Vector3 pos = m_motionState->GetWorldPosition();
  MT_Matrix3x3 ori = m_motionState->GetWorldOrientation();
  MT_Quaternion quat = ori.getRotation();

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

  m_physicsEnv->GetBodyInterface().RemoveBody(m_bodyID);
  m_physicsSuspended = true;
}

void JoltPhysicsController::RestorePhysics()
{
  if (!m_physicsSuspended || !m_physicsEnv || m_bodyID.IsInvalid()) {
    return;
  }

  m_physicsEnv->GetBodyInterface().AddBody(m_bodyID, JPH::EActivation::Activate);
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

  /* Switch to static. */
  bi.SetMotionType(m_bodyID, JPH::EMotionType::Static, JPH::EActivation::DontActivate);

  /* If ghost mode, make it a sensor (collisions detected but not resolved). */
  if (ghost) {
    JPH::BodyLockWrite lock(m_physicsEnv->GetPhysicsSystem()->GetBodyLockInterface(), m_bodyID);
    if (lock.Succeeded()) {
      lock.GetBody().SetIsSensor(true);
    }
    /* Update ObjectLayer to SENSOR broadphase category. */
    if (bi.IsAdded(m_bodyID)) {
      bi.SetObjectLayer(m_bodyID, JoltMakeObjectLayer(m_collisionGroup, m_collisionMask, JOLT_BP_SENSOR));
    }
  }
  else {
    /* Update ObjectLayer to STATIC broadphase category. */
    if (bi.IsAdded(m_bodyID)) {
      bi.SetObjectLayer(m_bodyID, JoltMakeObjectLayer(m_collisionGroup, m_collisionMask, JOLT_BP_STATIC));
    }
  }

  m_dynamicsSuspended = true;
}

void JoltPhysicsController::RestoreDynamics()
{
  if (!m_dynamicsSuspended || !m_physicsEnv || m_bodyID.IsInvalid()) {
    return;
  }

  JPH::BodyInterface &bi = m_physicsEnv->GetBodyInterface();

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
  replica->m_bodyID = m_bodyID;  /* Will be replaced in PostProcessReplica. */

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
