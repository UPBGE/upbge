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

/** \file JoltVehicle.h
 *  \ingroup physjolt
 *  \brief Jolt Physics vehicle wrapper implementing PHY_IVehicle.
 */

#pragma once

#include <Jolt/Jolt.h>

JPH_SUPPRESS_WARNINGS

#include <Jolt/Physics/Vehicle/VehicleConstraint.h>
#include <Jolt/Physics/Vehicle/WheeledVehicleController.h>

#include "PHY_IVehicle.h"

#include "MT_Quaternion.h"
#include "MT_Vector3.h"

#include <vector>

class JoltPhysicsController;
class JoltPhysicsEnvironment;
class PHY_IMotionState;

/**
 * JoltVehicle wraps Jolt's VehicleConstraint (with WheeledVehicleController)
 * and implements UPBGE's PHY_IVehicle interface for raycast-based vehicles.
 */
class JoltVehicle : public PHY_IVehicle {
 public:
  JoltVehicle(JoltPhysicsController *chassisCtrl,
              JoltPhysicsEnvironment *env,
              int constraintId);
  virtual ~JoltVehicle();

  /* ---- PHY_IVehicle interface ---- */

  virtual void AddWheel(PHY_IMotionState *motionState,
                        MT_Vector3 connectionPoint,
                        MT_Vector3 downDirection,
                        MT_Vector3 axleDirection,
                        float suspensionRestLength,
                        float wheelRadius,
                        bool hasSteering) override;

  virtual int GetNumWheels() const override;

  virtual MT_Vector3 GetWheelPosition(int wheelIndex) const override;
  virtual MT_Quaternion GetWheelOrientationQuaternion(int wheelIndex) const override;
  virtual float GetWheelRotation(int wheelIndex) const override;

  virtual int GetUserConstraintId() const override;
  virtual int GetUserConstraintType() const override;

  virtual void SetSteeringValue(float steering, int wheelIndex) override;
  virtual void ApplyEngineForce(float force, int wheelIndex) override;
  virtual void ApplyBraking(float braking, int wheelIndex) override;

  virtual void SetWheelFriction(float friction, int wheelIndex) override;
  virtual void SetSuspensionStiffness(float stiffness, int wheelIndex) override;
  virtual void SetSuspensionDamping(float damping, int wheelIndex) override;
  virtual void SetSuspensionCompression(float compression, int wheelIndex) override;
  virtual void SetRollInfluence(float rollInfluence, int wheelIndex) override;

  virtual void SetCoordinateSystem(int rightIndex, int upIndex, int forwardIndex) override;

  virtual void SetRayCastMask(short mask) override;
  virtual short GetRayCastMask() const override;

  /* ---- Jolt-specific methods ---- */

  /** Finalize the vehicle after all wheels are added. Must be called before simulation. */
  bool Build();

  /** Sync wheel motion states for rendering. Called each frame. */
  void SyncWheels();

  JPH::VehicleConstraint *GetVehicleConstraint() { return m_vehicleConstraint; }

 private:
  struct WheelInfo {
    PHY_IMotionState *motionState;
    MT_Vector3 connectionPoint;
    MT_Vector3 downDirection;
    MT_Vector3 axleDirection;
    float suspensionRestLength;
    float wheelRadius;
    bool hasSteering;
    float steeringValue;
    float engineForce;
    float braking;
    float friction;
    float suspensionStiffness;
    float suspensionDamping;
    float suspensionCompression;
    float rollInfluence;
  };

  JoltPhysicsController *m_chassisCtrl;
  JoltPhysicsEnvironment *m_env;
  JPH::VehicleConstraint *m_vehicleConstraint;
  std::vector<WheelInfo> m_wheels;
  int m_constraintId;
  short m_rayCastMask;
  int m_rightIndex;
  int m_upIndex;
  int m_forwardIndex;
  bool m_built;
};
