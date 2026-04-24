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

#include <Jolt/Physics/Vehicle/MotorcycleController.h>
#include <Jolt/Physics/Vehicle/VehicleConstraint.h>
#include <Jolt/Physics/Vehicle/WheeledVehicleController.h>

#include "PHY_IVehicle.h"

#include "MT_Matrix3x3.h"
#include "MT_Quaternion.h"
#include "MT_Vector3.h"

#include <vector>

class JoltPhysicsController;
class JoltPhysicsEnvironment;
class PHY_IMotionState;

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
  virtual void ApplyHandBraking(float braking, int wheelIndex) override;

  virtual void SetWheelFriction(float friction, int wheelIndex) override;
  virtual void SetWheelLongitudinalFriction(float friction, int wheelIndex) override;
  virtual void SetWheelLateralFriction(float friction, int wheelIndex) override;
  virtual void SetSuspensionStiffness(float stiffness, int wheelIndex) override;
  virtual void SetSuspensionDamping(float damping, int wheelIndex) override;
  virtual void SetSuspensionCompression(float compression, int wheelIndex) override;
  virtual void SetRollInfluence(float rollInfluence, int wheelIndex) override;
  virtual void SetSuspensionTravel(float suspensionTravel, int wheelIndex) override;
  virtual void SetWheelWidth(float width, int wheelIndex) override;
  virtual void SetWheelCollisionMode(int collisionMode, int wheelIndex) override;

  virtual void SetCoordinateSystem(int rightIndex, int upIndex, int forwardIndex) override;

  virtual void SetRayCastMask(short mask) override;
  virtual short GetRayCastMask() const override;
  virtual void SetWheelRayCastMask(short mask, int wheelIndex) override;
  virtual bool NeedsWheelAxleFlip() const override;

  /* ---- Jolt-specific methods ---- */

  /** Finalize the vehicle after all wheels are added. Must be called before simulation. */
  bool Build();

  /** Apply cached engine / brake forces before the simulation step. */
  void PreStep(float deltaTime);

  /** Sync wheel motion states for rendering. Called each frame. */
  void SyncWheels();

  JPH::VehicleConstraint *GetVehicleConstraint() { return m_vehicleConstraint; }

  bool UsesController(const JoltPhysicsController *ctrl) const;
  void SetEnvironment(JoltPhysicsEnvironment *env);
  void ResetConstraint();

  virtual void SetMaxPitchRollAngle(float angle) override;
  virtual void SetSolverIterations(int iterations) override;
  virtual void SetWheelDefaults(float width, float inertia, float angularDamping) override;
  virtual void SetDrivetrain(float handbrakeTorque,
                             float engineTorque,
                             float differentialRatio,
                             float limitedSlipRatio) override;
  virtual void SetSimpleDriveMode(bool enabled) override;
  virtual void SetSimpleDriveSteering(float steeringSpeed, float speedReduction) override;
  virtual void SetAntiRoll(float front, float rear) override;
  virtual void SetWheelUseHandBrake(int wheelIndex, bool useHandBrake) override;
  virtual void SetWheelUseTraction(int wheelIndex, bool useTraction) override;
  virtual void SetWheelInertia(float inertia, int wheelIndex) override;
  virtual void SetWheelAngularDamping(float damping, int wheelIndex) override;
  virtual void SetWheelHandBrakeTorque(float torque, int wheelIndex) override;
  virtual void SetWheelMaxEngineForce(float force, int wheelIndex) override;
  virtual void SetWheelMaxBrakeTorque(float torque, int wheelIndex) override;
  virtual void SetWheelMaxSteerAngle(float angle, int wheelIndex) override;
  virtual void SetMaxBrakeTorque(float torque) override;

  virtual void SetForwardInput(float forward) override;
  virtual void SetRightInput(float right) override;
  virtual void SetBrakeInput(float brake) override;
  virtual void SetHandBrakeInput(float handBrake) override;

  virtual void SetMotorcycleMode(bool enabled) override;
  virtual void SetMotorcycleLean(float maxAngle,
                                 float springConstant,
                                 float springDamping,
                                 float integrationCoef,
                                 float integrationDecay,
                                 float smoothingFactor) override;
  virtual void SetMotorcycleLeanFlags(bool enableController,
                                      bool enableSteeringLimit) override;
  virtual void SetMotorcycleSuspensionForcePointOverride(bool enableFront,
                                                         bool enableRear) override;
 private:
  struct WheelInfo {
    PHY_IMotionState *motionState = nullptr;
    MT_Vector3 connectionPoint = MT_Vector3(0.0f, 0.0f, 0.0f);
    MT_Vector3 downDirection = MT_Vector3(0.0f, 0.0f, -1.0f);
    MT_Vector3 axleDirection = MT_Vector3(1.0f, 0.0f, 0.0f);
    MT_Matrix3x3 visualOrientationOffset = MT_Matrix3x3::Identity();
    float suspensionRestLength = 0.15f;
    float wheelRadius = 0.5f;
    float wheelWidth = 0.0f;
    int collisionMode = PHY_VEHICLE_WHEEL_COLLISION_CYLINDER;
    short rayMask = 1; /* Bit 0 only: collide with group 0 by default. */
    bool hasSteering = false;
    bool useHandBrake = false;
    bool useTraction = false;
    float maxEngineForce = 0.0f;
    float maxBrakeTorque = 0.0f;
    float maxSteerAngle = 0.0f;
    float steeringValue = 0.0f;
    float engineForce = 0.0f;
    float braking = 0.0f;
    float handBraking = 0.0f;
    float longitudinalFriction = 10.5f;
    float lateralFriction = 10.5f;
    float suspensionStiffness = 20.0f;
    float suspensionCompression = 4.4f;
    float rollInfluence = 0.1f;
    float suspensionTravel = 0.3f;
    float lastSuspensionLength = 0.15f;
    float inertia = 0.9f;
    float angularDamping = 0.2f;
    float handBrakeTorque = 4000.0f;
  };

  struct AxleGroup {
    float forwardPosition = 0.0f;
    float leftSidePosition = 0.0f;
    float rightSidePosition = 0.0f;
    float leftWeight = 0.0f;
    float rightWeight = 0.0f;
    int leftWheel = -1;
    int rightWheel = -1;
    int wheelCount = 0;

    float TotalWeight() const
    {
      return leftWeight + rightWeight;
    }

    bool HasAnyWheel() const
    {
      return leftWheel >= 0 || rightWheel >= 0;
    }

    bool HasWheelPair() const
    {
      return leftWheel >= 0 && rightWheel >= 0;
    }
  };

  static JPH::Vec3 AxisVector(int axis);
  static void ConfigureLongitudinalFriction(JPH::LinearCurve &curve, float friction);
  static void ConfigureLateralFriction(JPH::LinearCurve &curve, float friction);
  bool UseMotorcycleSuspensionForcePoint(const WheelInfo &wheel) const;
  void ConfigureSuspensionForcePoint(const WheelInfo &wheel, JPH::WheelSettingsWV &settings) const;
  float GetResolvedWheelMaxEngineForce(const WheelInfo &wheel) const;
  float GetResolvedWheelMaxBrakeTorque(const WheelInfo &wheel) const;
  float GetResolvedWheelMaxSteerAngle(const WheelInfo &wheel) const;
  float GetEffectiveEngineTorque() const;

  JPH::Vec3 GetVehicleRightVector() const;
  float GetWheelSideValue(const WheelInfo &wheel, const MT_Vector3 &vehicleRight) const;
  float GetAxleDetectionTolerance() const;
  std::vector<AxleGroup> DetectAxleGroups(bool drivenOnly) const;
  static JPH::Vec3 GetWheelModelRight();
  static JPH::Vec3 GetWheelModelUp();
  JPH::Mat44 GetWheelWorldTransform(int wheelIndex) const;
  void CacheWheelVisualOrientationOffsets();
  float ComputeSpringDamping(const WheelInfo &wheel, const JPH::WheelWV *joltWheel) const;
  float GetResolvedWheelWidth(const WheelInfo &wheel) const;
  void ApplyRuntimeWheelSettings(int wheelIndex);
  void ApplyPostBuildState();
  void ClearPerWheelForces();
  void ConfigureDifferentials(JPH::WheeledVehicleControllerSettings *controllerSettings) const;
  void ConfigureAntiRollBars(JPH::VehicleConstraintSettings &vehicleSettings) const;
  void ApplyLiveDrivetrainUpdate();
  void ReconfigureLiveAntiRollBars();
  float GetConfiguredMaxEngineForce(const WheelInfo &wheel) const;
  float GetConfiguredMaxSteerAngle(const WheelInfo &wheel) const;
  float GetConfiguredMaxBrakeTorque(const WheelInfo &wheel) const;
  float UpdateSimpleDriveSteeringInput(float rightInput, float deltaTime);

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
  float m_maxPitchRollAngle;
  int m_solverIterations;
  float m_wheelWidth;
  float m_engineTorque;
  float m_differentialRatio;
  float m_limitedSlipRatio;
  float m_frontAntiRoll;
  float m_rearAntiRoll;
  float m_simpleDriveSteeringSpeed;
  float m_simpleDriveHighSpeedSteeringReduction;
  float m_simpleDriveFilteredRightInput;
  float m_simpleDriveSteeringStartInput;
  float m_simpleDriveSteeringTargetInput;
  float m_simpleDriveSteeringElapsedTime;
  float m_simpleDriveSteeringMoveDuration;

  /* Motorcycle-specific (Jolt MotorcycleController). When m_motorcycleMode is
   * true, Build() creates a MotorcycleControllerSettings instead of a plain
   * WheeledVehicleControllerSettings. The lean parameters below mirror Jolt's
   * MotorcycleControllerSettings 1:1. */
  bool m_motorcycleMode;
  bool m_mcEnableLeanController;
  bool m_mcEnableLeanSteeringLimit;
  bool m_mcOverrideFrontSuspensionForcePoint;
  bool m_mcOverrideRearSuspensionForcePoint;
  float m_mcMaxLeanAngle;
  float m_mcLeanSpringConstant;
  float m_mcLeanSpringDamping;
  float m_mcLeanSpringIntegrationCoef;
  float m_mcLeanSpringIntegrationDecay;
  float m_mcLeanSmoothingFactor;

  // Normalized driver inputs (set each frame by actuator, cleared in PreStep).
  float m_forwardInput;
  float m_rightInput;
  float m_brakeInput;
  float m_handBrakeInput;
  /* True when any SetForwardInput / SetRightInput / SetBrakeInput /
   * SetHandBrakeInput setter was called this frame. When true, PreStep
   * ignores per-wheel forward/steering fallback so the two input paths
   * cannot fight each other. Cleared inside PreStep after consumption. */
  bool m_useDriverInput;
};
