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

/** \file JoltVehicle.cpp
 *  \ingroup physjolt
 */

#include "JoltVehicle.h"

#include <Jolt/Jolt.h>

JPH_SUPPRESS_WARNINGS

#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Vehicle/VehicleCollisionTester.h>

#include "JoltMathUtils.h"
#include "JoltPhysicsController.h"
#include "JoltPhysicsEnvironment.h"
#include "PHY_IMotionState.h"

/* -------------------------------------------------------------------- */
/** \name JoltVehicle — Construction / Destruction
 * \{ */

JoltVehicle::JoltVehicle(JoltPhysicsController *chassisCtrl,
                         JoltPhysicsEnvironment *env,
                         int constraintId)
    : m_chassisCtrl(chassisCtrl),
      m_env(env),
      m_vehicleConstraint(nullptr),
      m_constraintId(constraintId),
      m_rayCastMask(0xFFFF),
      m_rightIndex(0),
      m_upIndex(1),
      m_forwardIndex(2),
      m_built(false)
{
}

JoltVehicle::~JoltVehicle()
{
  if (m_vehicleConstraint && m_env) {
    m_env->GetPhysicsSystem()->RemoveStepListener(m_vehicleConstraint);
    m_env->GetPhysicsSystem()->RemoveConstraint(m_vehicleConstraint);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name PHY_IVehicle — Add Wheel
 * \{ */

void JoltVehicle::AddWheel(PHY_IMotionState *motionState,
                           MT_Vector3 connectionPoint,
                           MT_Vector3 downDirection,
                           MT_Vector3 axleDirection,
                           float suspensionRestLength,
                           float wheelRadius,
                           bool hasSteering)
{
  WheelInfo wheel;
  wheel.motionState = motionState;
  wheel.connectionPoint = connectionPoint;
  wheel.downDirection = downDirection;
  wheel.axleDirection = axleDirection;
  wheel.suspensionRestLength = suspensionRestLength;
  wheel.wheelRadius = wheelRadius;
  wheel.hasSteering = hasSteering;
  wheel.steeringValue = 0.0f;
  wheel.engineForce = 0.0f;
  wheel.braking = 0.0f;
  wheel.friction = 1.0f;
  wheel.suspensionStiffness = 5.88f;
  wheel.suspensionDamping = 0.88f;
  wheel.suspensionCompression = 0.83f;
  wheel.rollInfluence = 0.1f;

  m_wheels.push_back(wheel);
}

bool JoltVehicle::Build()
{
  if (m_built || m_wheels.empty() || !m_chassisCtrl || !m_env) {
    return false;
  }

  JPH::PhysicsSystem *physSystem = m_env->GetPhysicsSystem();
  JPH::BodyID chassisBodyID = m_chassisCtrl->GetBodyID();
  if (chassisBodyID.IsInvalid()) {
    return false;
  }

  /* Build vehicle constraint settings. */
  JPH::VehicleConstraintSettings vehicleSettings;

  /* Use WheeledVehicleController. */
  JPH::WheeledVehicleControllerSettings *controllerSettings =
      new JPH::WheeledVehicleControllerSettings();

  /* Add wheels. */
  vehicleSettings.mWheels.resize(m_wheels.size());
  for (size_t i = 0; i < m_wheels.size(); ++i) {
    const WheelInfo &winfo = m_wheels[i];

    JPH::WheelSettingsWV *ws = new JPH::WheelSettingsWV();
    /* Convert connection point from Blender Z-up to Jolt Y-up. */
    ws->mPosition = JoltMath::ToJolt(winfo.connectionPoint);
    ws->mSuspensionDirection = JoltMath::ToJolt(winfo.downDirection);
    ws->mSteeringAxis = -ws->mSuspensionDirection;  /* Steering around the up direction. */
    ws->mWheelUp = -ws->mSuspensionDirection;
    ws->mWheelForward = JoltMath::ToJolt(winfo.axleDirection).Cross(ws->mWheelUp).Normalized();
    ws->mSuspensionMinLength = 0.0f;
    ws->mSuspensionMaxLength = winfo.suspensionRestLength;
    ws->mSuspensionSpring.mFrequency = winfo.suspensionStiffness;
    ws->mSuspensionSpring.mDamping = winfo.suspensionDamping;
    ws->mRadius = winfo.wheelRadius;
    ws->mWidth = winfo.wheelRadius * 0.4f;  /* Reasonable default width. */
    ws->mMaxSteerAngle = winfo.hasSteering ? JPH::DegreesToRadians(70.0f) : 0.0f;

    vehicleSettings.mWheels[i] = ws;
  }

  vehicleSettings.mController = controllerSettings;

  /* Create collision tester (raycast-based, matching Bullet's approach).
   * Use the chassis controller's collision group/mask for the ObjectLayer. */
  JPH::VehicleCollisionTesterRay *collisionTester = new JPH::VehicleCollisionTesterRay(
      JoltMakeObjectLayer(m_chassisCtrl->GetCollisionGroup(), m_chassisCtrl->GetCollisionMask(), JOLT_BP_DYNAMIC));

  /* Lock the chassis body to create constraint. */
  const JPH::BodyLockInterface &lockInterface = physSystem->GetBodyLockInterface();
  JPH::BodyLockWrite lock(lockInterface, chassisBodyID);
  if (!lock.Succeeded()) {
    return false;
  }
  JPH::Body &chassisBody = lock.GetBody();

  m_vehicleConstraint = new JPH::VehicleConstraint(chassisBody, vehicleSettings);
  m_vehicleConstraint->SetVehicleCollisionTester(collisionTester);

  physSystem->AddConstraint(m_vehicleConstraint);
  physSystem->AddStepListener(m_vehicleConstraint);

  m_built = true;
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name PHY_IVehicle — Wheel Queries
 * \{ */

int JoltVehicle::GetNumWheels() const
{
  return (int)m_wheels.size();
}

MT_Vector3 JoltVehicle::GetWheelPosition(int wheelIndex) const
{
  if (!m_vehicleConstraint || wheelIndex < 0 || wheelIndex >= (int)m_wheels.size()) {
    return MT_Vector3(0, 0, 0);
  }
  JPH::Mat44 worldTransform = m_vehicleConstraint->GetWheelWorldTransform(
      wheelIndex, JPH::Vec3::sAxisY(), JPH::Vec3::sAxisX());
  return JoltMath::ToMT(JPH::Vec3(worldTransform.GetTranslation()));
}

MT_Quaternion JoltVehicle::GetWheelOrientationQuaternion(int wheelIndex) const
{
  if (!m_vehicleConstraint || wheelIndex < 0 || wheelIndex >= (int)m_wheels.size()) {
    return MT_Quaternion(0, 0, 0, 1);
  }
  JPH::Mat44 worldTransform = m_vehicleConstraint->GetWheelWorldTransform(
      wheelIndex, JPH::Vec3::sAxisY(), JPH::Vec3::sAxisX());
  JPH::Quat q = worldTransform.GetRotation().GetQuaternion();
  /* Convert from Jolt Y-up to Blender Z-up quaternion. */
  /* For quaternion, we use a simplified conversion. */
  return MT_Quaternion(q.GetX(), -q.GetZ(), q.GetY(), q.GetW());
}

float JoltVehicle::GetWheelRotation(int wheelIndex) const
{
  if (!m_vehicleConstraint || wheelIndex < 0 || wheelIndex >= (int)m_wheels.size()) {
    return 0.0f;
  }
  return m_vehicleConstraint->GetWheels()[wheelIndex]->GetRotationAngle();
}

int JoltVehicle::GetUserConstraintId() const
{
  return m_constraintId;
}

int JoltVehicle::GetUserConstraintType() const
{
  return PHY_VEHICLE_CONSTRAINT;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name PHY_IVehicle — Steering / Engine / Braking
 * \{ */

void JoltVehicle::SetSteeringValue(float steering, int wheelIndex)
{
  if (wheelIndex < 0 || wheelIndex >= (int)m_wheels.size()) {
    return;
  }
  m_wheels[wheelIndex].steeringValue = steering;

  if (m_vehicleConstraint) {
    JPH::WheeledVehicleController *controller =
        static_cast<JPH::WheeledVehicleController *>(m_vehicleConstraint->GetController());
    /* Jolt uses steer angle in radians. Convert from normalized [-1,1] to angle. */
    JPH::WheelWV *wheel = static_cast<JPH::WheelWV *>(
        m_vehicleConstraint->GetWheels()[wheelIndex]);
    wheel->SetSteerAngle(steering);
  }
}

void JoltVehicle::ApplyEngineForce(float force, int wheelIndex)
{
  if (wheelIndex < 0 || wheelIndex >= (int)m_wheels.size()) {
    return;
  }
  m_wheels[wheelIndex].engineForce = force;

  if (m_vehicleConstraint) {
    JPH::WheeledVehicleController *controller =
        static_cast<JPH::WheeledVehicleController *>(m_vehicleConstraint->GetController());
    /* Apply engine torque to the wheel. */
    controller->SetDriverInput(force > 0.0f ? 1.0f : (force < 0.0f ? -1.0f : 0.0f),
                               0.0f,
                               0.0f,
                               false);
  }
}

void JoltVehicle::ApplyBraking(float braking, int wheelIndex)
{
  if (wheelIndex < 0 || wheelIndex >= (int)m_wheels.size()) {
    return;
  }
  m_wheels[wheelIndex].braking = braking;

  if (m_vehicleConstraint) {
    JPH::WheeledVehicleController *controller =
        static_cast<JPH::WheeledVehicleController *>(m_vehicleConstraint->GetController());
    controller->SetDriverInput(0.0f, 0.0f, braking, false);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name PHY_IVehicle — Wheel Properties
 * \{ */

void JoltVehicle::SetWheelFriction(float friction, int wheelIndex)
{
  if (wheelIndex < 0 || wheelIndex >= (int)m_wheels.size()) {
    return;
  }
  m_wheels[wheelIndex].friction = friction;
}

void JoltVehicle::SetSuspensionStiffness(float stiffness, int wheelIndex)
{
  if (wheelIndex < 0 || wheelIndex >= (int)m_wheels.size()) {
    return;
  }
  m_wheels[wheelIndex].suspensionStiffness = stiffness;

  if (m_vehicleConstraint) {
    const_cast<JPH::WheelSettings *>(m_vehicleConstraint->GetWheels()[wheelIndex]->GetSettings())
        ->mSuspensionSpring.mFrequency = stiffness;
  }
}

void JoltVehicle::SetSuspensionDamping(float damping, int wheelIndex)
{
  if (wheelIndex < 0 || wheelIndex >= (int)m_wheels.size()) {
    return;
  }
  m_wheels[wheelIndex].suspensionDamping = damping;

  if (m_vehicleConstraint) {
    const_cast<JPH::WheelSettings *>(m_vehicleConstraint->GetWheels()[wheelIndex]->GetSettings())
        ->mSuspensionSpring.mDamping = damping;
  }
}

void JoltVehicle::SetSuspensionCompression(float compression, int wheelIndex)
{
  if (wheelIndex < 0 || wheelIndex >= (int)m_wheels.size()) {
    return;
  }
  m_wheels[wheelIndex].suspensionCompression = compression;
}

void JoltVehicle::SetRollInfluence(float rollInfluence, int wheelIndex)
{
  if (wheelIndex < 0 || wheelIndex >= (int)m_wheels.size()) {
    return;
  }
  m_wheels[wheelIndex].rollInfluence = rollInfluence;
}

void JoltVehicle::SetCoordinateSystem(int rightIndex, int upIndex, int forwardIndex)
{
  m_rightIndex = rightIndex;
  m_upIndex = upIndex;
  m_forwardIndex = forwardIndex;
}

void JoltVehicle::SetRayCastMask(short mask)
{
  m_rayCastMask = mask;
}

short JoltVehicle::GetRayCastMask() const
{
  return m_rayCastMask;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name JoltVehicle — Sync Wheels
 * \{ */

void JoltVehicle::SyncWheels()
{
  if (!m_vehicleConstraint) {
    return;
  }

  for (int i = 0; i < (int)m_wheels.size(); ++i) {
    WheelInfo &winfo = m_wheels[i];
    if (!winfo.motionState) {
      continue;
    }

    JPH::Mat44 worldTransform = m_vehicleConstraint->GetWheelWorldTransform(
        i, JPH::Vec3::sAxisY(), JPH::Vec3::sAxisX());

    /* Convert position from Jolt Y-up to Blender Z-up. */
    MT_Vector3 pos = JoltMath::ToMT(JPH::Vec3(worldTransform.GetTranslation()));
    winfo.motionState->SetWorldPosition(pos);

    /* Convert orientation. */
    JPH::Quat q = worldTransform.GetRotation().GetQuaternion();
    MT_Quaternion mtQ(q.GetX(), -q.GetZ(), q.GetY(), q.GetW());
    winfo.motionState->SetWorldOrientation(mtQ);
  }
}

/** \} */
