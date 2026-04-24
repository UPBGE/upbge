
/** \file PHY_IVehicle.h
 *  \ingroup phys
 */

#pragma once

// PHY_IVehicle provides a generic interface for (raycast based) vehicles. Mostly targetting 4
// wheel cars and 2 wheel motorbikes.

#include "PHY_DynamicTypes.h"

#include "MT_Quaternion.h"

class PHY_IMotionState;

enum PHY_VehicleWheelCollisionMode {
  PHY_VEHICLE_WHEEL_COLLISION_RAY = 0,
  PHY_VEHICLE_WHEEL_COLLISION_SPHERE = 1,
  PHY_VEHICLE_WHEEL_COLLISION_CYLINDER = 2,
};

class PHY_IVehicle {
 public:
  virtual ~PHY_IVehicle(){};

  virtual void AddWheel(PHY_IMotionState *motionState,
                        MT_Vector3 connectionPoint,
                        MT_Vector3 downDirection,
                        MT_Vector3 axleDirection,
                        float suspensionRestLength,
                        float wheelRadius,
                        bool hasSteering) = 0;

  virtual int GetNumWheels() const = 0;

  virtual MT_Vector3 GetWheelPosition(int wheelIndex) const = 0;
  virtual MT_Quaternion GetWheelOrientationQuaternion(int wheelIndex) const = 0;
  virtual float GetWheelRotation(int wheelIndex) const = 0;

  virtual int GetUserConstraintId() const = 0;
  virtual int GetUserConstraintType() const = 0;

  // some basic steering/braking/tuning/balancing (bikes)

  virtual void SetSteeringValue(float steering, int wheelIndex) = 0;

  virtual void ApplyEngineForce(float force, int wheelIndex) = 0;

  virtual void ApplyBraking(float braking, int wheelIndex) = 0;

  virtual void ApplyHandBraking(float /*braking*/, int /*wheelIndex*/) {}

  virtual void SetWheelFriction(float friction, int wheelIndex) = 0;
  virtual void SetWheelLongitudinalFriction(float /*friction*/, int /*wheelIndex*/) {}
  virtual void SetWheelLateralFriction(float /*friction*/, int /*wheelIndex*/) {}

  virtual void SetSuspensionStiffness(float suspensionStiffness, int wheelIndex) = 0;

  virtual void SetSuspensionDamping(float suspensionStiffness, int wheelIndex) = 0;

  virtual void SetSuspensionCompression(float suspensionStiffness, int wheelIndex) = 0;

  virtual void SetRollInfluence(float rollInfluence, int wheelIndex) = 0;

  virtual void SetSuspensionTravel(float /*suspensionTravel*/, int /*wheelIndex*/) {}
  virtual void SetWheelWidth(float /*width*/, int /*wheelIndex*/) {}
  virtual void SetWheelCollisionMode(int /*collisionMode*/, int /*wheelIndex*/) {}

  virtual void SetCoordinateSystem(int rightIndex, int upIndex, int forwardIndex) = 0;

  virtual void SetRayCastMask(short mask) = 0;
  virtual short GetRayCastMask() const = 0;
  virtual void SetWheelRayCastMask(short /*mask*/, int /*wheelIndex*/) {}

  virtual void SetMaxPitchRollAngle(float /*angle*/) {}
  virtual void SetSolverIterations(int /*iterations*/) {}
  virtual void SetWheelDefaults(float /*width*/, float /*inertia*/, float /*angularDamping*/) {}
  virtual void SetDrivetrain(float /*handbrakeTorque*/,
                             float /*engineTorque*/,
                             float /*differentialRatio*/,
                             float /*limitedSlipRatio*/) {}
  virtual void SetSimpleDriveMode(bool /*enabled*/) {}
  virtual void SetSimpleDriveSteering(float /*steeringSpeed*/, float /*speedReduction*/) {}
  virtual void SetAntiRoll(float /*front*/, float /*rear*/) {}
  virtual void SetWheelUseHandBrake(int /*wheelIndex*/, bool /*useHandBrake*/) {}
  virtual void SetWheelUseTraction(int /*wheelIndex*/, bool /*useTraction*/) {}
  virtual void SetWheelInertia(float /*inertia*/, int /*wheelIndex*/) {}
  virtual void SetWheelAngularDamping(float /*damping*/, int /*wheelIndex*/) {}
  virtual void SetWheelHandBrakeTorque(float /*torque*/, int /*wheelIndex*/) {}

  virtual void SetWheelMaxEngineForce(float /*force*/, int /*wheelIndex*/) {}
  virtual void SetWheelMaxBrakeTorque(float /*torque*/, int /*wheelIndex*/) {}
  virtual void SetWheelMaxSteerAngle(float /*angle*/, int /*wheelIndex*/) {}

  virtual void SetMaxBrakeTorque(float torque) { (void)torque; }

  /* Motorcycle-specific (Jolt MotorcycleController). Default implementations
   * are no-ops so non-Jolt physics backends can ignore these calls. */
  virtual void SetMotorcycleMode(bool /*enabled*/) {}
  virtual void SetMotorcycleLean(float /*maxAngle*/,
                                 float /*springConstant*/,
                                 float /*springDamping*/,
                                 float /*integrationCoef*/,
                                 float /*integrationDecay*/,
                                 float /*smoothingFactor*/) {}
  virtual void SetMotorcycleLeanFlags(bool /*enableController*/,
                                      bool /*enableSteeringLimit*/) {}
  virtual void SetMotorcycleSuspensionForcePointOverride(bool /*enableFront*/,
                                                         bool /*enableRear*/) {}

  virtual bool NeedsWheelAxleFlip() const { return false; }

  virtual void SetForwardInput(float /*forward*/) {}
  virtual void SetRightInput(float /*right*/) {}
  virtual void SetBrakeInput(float /*brake*/) {}
  virtual void SetHandBrakeInput(float /*handBrake*/) {}
};
