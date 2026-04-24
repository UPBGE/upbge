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

#include <algorithm>
#include <cmath>
#include <memory>

#include <Jolt/Jolt.h>

JPH_SUPPRESS_WARNINGS

#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Vehicle/VehicleAntiRollBar.h>
#include <Jolt/Physics/Vehicle/VehicleCollisionTester.h>
#include <Jolt/Physics/Vehicle/VehicleDifferential.h>

#include "CM_Message.h"
#include "JoltMathUtils.h"
#include "JoltPhysicsController.h"
#include "JoltPhysicsEnvironment.h"
#include "PHY_IMotionState.h"

namespace {

constexpr float kSimpleDriveFixedGearRatio = 2.27f;
constexpr float kSimpleDriveSteeringReductionStartSpeed = 5.0f;
constexpr float kSimpleDriveSteeringReductionEndSpeed = 27.77778f;
constexpr float kSimpleDriveBaseSteeringReduction = 0.55f;
constexpr float kSimpleDriveSteeringReductionStrengthMultiplier = 1.5f;

/* Cap for the Jolt position-step override derived from the single UPBGE
 * "Solver Iterations" slider. Jolt default is 2; 4 is already generous and
 * keeps the per-island position-solve cost bounded regardless of how high
 * the user pushes the velocity override. */
constexpr int kMaxPositionSolverIterations = 4;

struct JoltWheelCollisionTesterConfig {
  int mode;
  float sphereRadius;
  JPH::ObjectLayer objectLayer;
};

static int JoltNormalizeWheelCollisionMode(int collisionMode)
{
  switch (collisionMode) {
    case PHY_VEHICLE_WHEEL_COLLISION_RAY:
    case PHY_VEHICLE_WHEEL_COLLISION_SPHERE:
    case PHY_VEHICLE_WHEEL_COLLISION_CYLINDER:
      return collisionMode;
    default:
      return PHY_VEHICLE_WHEEL_COLLISION_CYLINDER;
  }
}

static float JoltGetBodyMassNoLock(JoltPhysicsEnvironment *env, JPH::BodyID bodyID)
{
  if (!env || bodyID.IsInvalid()) {
    return 0.0f;
  }

  const JPH::BodyLockInterface &lockInterface = env->GetPhysicsSystem()->GetBodyLockInterfaceNoLock();
  JPH::BodyLockRead lock(lockInterface, bodyID);
  if (!lock.Succeeded()) {
    return 0.0f;
  }

  const JPH::Body &body = lock.GetBody();
  if (!body.IsDynamic() || !body.GetMotionProperties()) {
    return 0.0f;
  }

  const float inverse_mass = body.GetMotionProperties()->GetInverseMass();
  return inverse_mass > 0.0f ? (1.0f / inverse_mass) : 0.0f;
}

static MT_Matrix3x3 JoltWheelOrientationToMT(const JPH::Mat44 &worldTransform)
{
  return MT_Matrix3x3(JoltMath::ToMT(worldTransform.GetRotation().GetQuaternion()));
}

class JoltPerWheelVehicleCollisionTester : public JPH::VehicleCollisionTester {
 public:
  JPH_OVERRIDE_NEW_DELETE

  JoltPerWheelVehicleCollisionTester(JPH::ObjectLayer objectLayer,
                                     JPH::Vec3Arg up,
                                     const std::vector<JoltWheelCollisionTesterConfig> &wheelConfigs)
      : JPH::VehicleCollisionTester(objectLayer)
  {
    m_wheelTesters.reserve(wheelConfigs.size());
    for (const JoltWheelCollisionTesterConfig &config : wheelConfigs) {
      switch (JoltNormalizeWheelCollisionMode(config.mode)) {
        case PHY_VEHICLE_WHEEL_COLLISION_RAY:
          m_wheelTesters.push_back(
              std::make_unique<JPH::VehicleCollisionTesterRay>(config.objectLayer, up));
          break;
        case PHY_VEHICLE_WHEEL_COLLISION_SPHERE:
          m_wheelTesters.push_back(std::make_unique<JPH::VehicleCollisionTesterCastSphere>(
              config.objectLayer, std::max(0.01f, config.sphereRadius), up));
          break;
        default:
          m_wheelTesters.push_back(
              std::make_unique<JPH::VehicleCollisionTesterCastCylinder>(config.objectLayer));
          break;
      }
    }
  }

  virtual bool Collide(JPH::PhysicsSystem &inPhysicsSystem,
                       const JPH::VehicleConstraint &inVehicleConstraint,
                       JPH::uint inWheelIndex,
                       JPH::RVec3Arg inOrigin,
                       JPH::Vec3Arg inDirection,
                       const JPH::BodyID &inVehicleBodyID,
                       JPH::Body *&outBody,
                       JPH::SubShapeID &outSubShapeID,
                       JPH::RVec3 &outContactPosition,
                       JPH::Vec3 &outContactNormal,
                       float &outSuspensionLength) const override
  {
    JPH::VehicleCollisionTester *tester = GetWheelTester(inWheelIndex);
    return tester && tester->Collide(inPhysicsSystem,
                                     inVehicleConstraint,
                                     inWheelIndex,
                                     inOrigin,
                                     inDirection,
                                     inVehicleBodyID,
                                     outBody,
                                     outSubShapeID,
                                     outContactPosition,
                                     outContactNormal,
                                     outSuspensionLength);
  }

  virtual void PredictContactProperties(JPH::PhysicsSystem &inPhysicsSystem,
                                        const JPH::VehicleConstraint &inVehicleConstraint,
                                        JPH::uint inWheelIndex,
                                        JPH::RVec3Arg inOrigin,
                                        JPH::Vec3Arg inDirection,
                                        const JPH::BodyID &inVehicleBodyID,
                                        JPH::Body *&ioBody,
                                        JPH::SubShapeID &ioSubShapeID,
                                        JPH::RVec3 &ioContactPosition,
                                        JPH::Vec3 &ioContactNormal,
                                        float &ioSuspensionLength) const override
  {
    JPH::VehicleCollisionTester *tester = GetWheelTester(inWheelIndex);
    if (tester) {
      tester->PredictContactProperties(inPhysicsSystem,
                                       inVehicleConstraint,
                                       inWheelIndex,
                                       inOrigin,
                                       inDirection,
                                       inVehicleBodyID,
                                       ioBody,
                                       ioSubShapeID,
                                       ioContactPosition,
                                       ioContactNormal,
                                       ioSuspensionLength);
    }
  }

 private:
  JPH::VehicleCollisionTester *GetWheelTester(JPH::uint wheelIndex) const
  {
    if (m_wheelTesters.empty()) {
      return nullptr;
    }
    if (wheelIndex >= m_wheelTesters.size()) {
      wheelIndex = JPH::uint(m_wheelTesters.size() - 1);
    }
    return m_wheelTesters[wheelIndex].get();
  }

  std::vector<std::unique_ptr<JPH::VehicleCollisionTester>> m_wheelTesters;
};

}  // namespace

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
      m_built(false),
      m_maxPitchRollAngle(JPH::JPH_PI),
      m_solverIterations(0),
      m_wheelWidth(0.0f),
      m_engineTorque(500.0f),
      m_differentialRatio(3.42f),
      m_limitedSlipRatio(1.4f),
      m_frontAntiRoll(0.0f),
      m_rearAntiRoll(0.0f),
      m_simpleDriveSteeringSpeed(0.15f),
      m_simpleDriveHighSpeedSteeringReduction(1.0f),
      m_simpleDriveFilteredRightInput(0.0f),
      m_simpleDriveSteeringStartInput(0.0f),
      m_simpleDriveSteeringTargetInput(0.0f),
      m_simpleDriveSteeringElapsedTime(0.0f),
      m_simpleDriveSteeringMoveDuration(0.0f),
      m_motorcycleMode(false),
      m_mcEnableLeanController(true),
      m_mcEnableLeanSteeringLimit(true),
      m_mcOverrideFrontSuspensionForcePoint(false),
      m_mcOverrideRearSuspensionForcePoint(false),
      m_mcMaxLeanAngle(0.785398163f),
      m_mcLeanSpringConstant(5000.0f),
      m_mcLeanSpringDamping(1000.0f),
      m_mcLeanSpringIntegrationCoef(0.0f),
      m_mcLeanSpringIntegrationDecay(4.0f),
      m_mcLeanSmoothingFactor(0.8f),
      m_forwardInput(0.0f),
      m_rightInput(0.0f),
      m_brakeInput(0.0f),
      m_handBrakeInput(0.0f),
      m_useDriverInput(false)
{
}

JoltVehicle::~JoltVehicle()
{
  ResetConstraint();

  for (WheelInfo &wheel : m_wheels) {
    delete wheel.motionState;
    wheel.motionState = nullptr;
  }
}

JPH::Vec3 JoltVehicle::AxisVector(int axis)
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

void JoltVehicle::ConfigureLongitudinalFriction(JPH::LinearCurve &curve, float friction)
{
  /* Match Jolt's default 3-point longitudinal friction curve shape:
   *   {0, 0}, {0.06, peak}, {0.2, sliding}
   * Blender default friction (10.5) maps to Jolt defaults (peak=1.2, sliding=1.0).
   * Higher friction scales proportionally; capped at 8.0. */
  const float scale = std::clamp(friction / 8.75f, 0.01f, 8.0f);
  const float peak = scale;
  const float sliding = scale * (1.0f / 1.2f);

  curve = JPH::LinearCurve();
  curve.Reserve(3);
  curve.AddPoint(0.0f, 0.0f);
  curve.AddPoint(0.06f, peak);
  curve.AddPoint(0.2f, sliding);
}

void JoltVehicle::ConfigureLateralFriction(JPH::LinearCurve &curve, float friction)
{
  /* Match Jolt's default 3-point lateral friction curve shape:
   *   {0, 0}, {3, peak}, {20, sliding}
   * Same scaling as longitudinal. */
  const float scale = std::clamp(friction / 8.75f, 0.01f, 8.0f);
  const float peak = scale;
  const float sliding = scale * (1.0f / 1.2f);

  curve = JPH::LinearCurve();
  curve.Reserve(3);
  curve.AddPoint(0.0f, 0.0f);
  curve.AddPoint(3.0f, peak);
  curve.AddPoint(20.0f, sliding);
}

bool JoltVehicle::UseMotorcycleSuspensionForcePoint(const WheelInfo &wheel) const
{
  if (!m_motorcycleMode) {
    return true;
  }

  const float forward_position = JoltMath::ToJolt(wheel.connectionPoint)
                                     .Dot(AxisVector(m_forwardIndex));
  constexpr float kWheelPositionEpsilon = 1.0e-4f;
  if (forward_position > kWheelPositionEpsilon) {
    return m_mcOverrideFrontSuspensionForcePoint;
  }
  if (forward_position < -kWheelPositionEpsilon) {
    return m_mcOverrideRearSuspensionForcePoint;
  }

  return wheel.hasSteering ? m_mcOverrideFrontSuspensionForcePoint :
                             m_mcOverrideRearSuspensionForcePoint;
}

void JoltVehicle::ConfigureSuspensionForcePoint(const WheelInfo &wheel,
                                                JPH::WheelSettingsWV &settings) const
{
  const float rest_length = std::max(0.0f, wheel.suspensionRestLength);
  if (m_motorcycleMode) {
    settings.mEnableSuspensionForcePoint = UseMotorcycleSuspensionForcePoint(wheel);
    settings.mSuspensionForcePoint =
        settings.mPosition + settings.mSuspensionDirection * rest_length;
    return;
  }

  settings.mEnableSuspensionForcePoint = true;
  const float roll_influence = std::clamp(wheel.rollInfluence, 0.0f, 1.0f);
  settings.mSuspensionForcePoint =
      settings.mPosition + settings.mSuspensionDirection * rest_length * roll_influence;
}

float JoltVehicle::GetResolvedWheelMaxEngineForce(const WheelInfo &wheel) const
{
  if (wheel.maxEngineForce > 1.0e-4f) {
    return wheel.maxEngineForce;
  }
  if (std::abs(wheel.engineForce) > 1.0e-4f) {
    return std::abs(wheel.engineForce);
  }
  return 0.0f;
}

float JoltVehicle::GetResolvedWheelMaxBrakeTorque(const WheelInfo &wheel) const
{
  if (wheel.maxBrakeTorque > 1.0e-4f) {
    return wheel.maxBrakeTorque;
  }
  if (std::abs(wheel.braking) > 1.0e-4f) {
    return std::abs(wheel.braking);
  }
  return 0.0f;
}

float JoltVehicle::GetResolvedWheelMaxSteerAngle(const WheelInfo &wheel) const
{
  if (!wheel.hasSteering) {
    return 0.0f;
  }
  if (wheel.maxSteerAngle > 1.0e-4f) {
    return wheel.maxSteerAngle;
  }
  if (std::abs(wheel.steeringValue) > 1.0e-4f) {
    return std::abs(wheel.steeringValue);
  }
  return 0.0f;
}

float JoltVehicle::GetEffectiveEngineTorque() const
{
  if (m_engineTorque > 1.0e-4f) {
    return m_engineTorque;
  }

  const bool has_traction_flags = std::any_of(
      m_wheels.begin(), m_wheels.end(), [](const WheelInfo &wheel) { return wheel.useTraction; });

  if (!has_traction_flags) {
    return 1.0f;
  }

  float total_wheel_torque = 0.0f;
  for (const WheelInfo &wheel : m_wheels) {
    if (!wheel.useTraction) {
      continue;
    }

    const float drive_force = GetResolvedWheelMaxEngineForce(wheel);
    const float effective_force = drive_force > 1.0e-4f ? drive_force : 1.0f;
    total_wheel_torque += effective_force * std::max(0.01f, wheel.wheelRadius);
  }

  if (total_wheel_torque <= 1.0e-4f) {
    return 1.0f;
  }

  /* Jolt default first gear ratio is 2.27. Avoid creating a temporary
   * VehicleTransmissionSettings (which allocates a vector) on every frame. */
  constexpr float first_gear_ratio = 2.27f;
  const float total_ratio =
      std::max(1.0e-3f, first_gear_ratio * std::max(0.1f, std::abs(m_differentialRatio)));
  return std::max(1.0f, total_wheel_torque / total_ratio);
}

JPH::Vec3 JoltVehicle::GetVehicleRightVector() const
{
  return AxisVector(m_rightIndex);
}

float JoltVehicle::GetWheelSideValue(const WheelInfo &wheel, const MT_Vector3 &vehicle_right) const
{
  float side = wheel.connectionPoint.dot(vehicle_right);
  if (std::abs(side) < 1.0e-4f) {
    side = wheel.axleDirection.dot(vehicle_right);
  }
  return side;
}

float JoltVehicle::GetAxleDetectionTolerance() const
{
  float radius_sum = 0.0f;
  int radius_count = 0;

  for (const WheelInfo &wheel : m_wheels) {
    if (wheel.wheelRadius > 1.0e-4f) {
      radius_sum += wheel.wheelRadius;
      ++radius_count;
    }
  }

  const float average_radius =
      radius_count > 0 ? (radius_sum / float(radius_count)) : 0.3f;
  return std::clamp(average_radius * 0.25f, 0.05f, 0.2f);
}

std::vector<JoltVehicle::AxleGroup> JoltVehicle::DetectAxleGroups(bool drivenOnly) const
{
  struct WheelPlacement {
    int wheelIndex;
    float forwardPosition;
    float sidePosition;
    float driveWeight;
  };

  if (m_wheels.empty()) {
    return {};
  }

  const MT_Vector3 vehicle_right = JoltMath::ToMT(GetVehicleRightVector());
  const MT_Vector3 vehicle_forward = JoltMath::ToMT(AxisVector(m_forwardIndex));
  if (drivenOnly &&
      !std::any_of(
          m_wheels.begin(), m_wheels.end(), [](const WheelInfo &wheel) { return wheel.useTraction; }))
  {
    return {};
  }

  std::vector<WheelPlacement> placements;
  placements.reserve(m_wheels.size());

  for (int i = 0; i < (int)m_wheels.size(); ++i) {
    const WheelInfo &wheel = m_wheels[i];
    if (drivenOnly && !wheel.useTraction) {
      continue;
    }

    const float drive_force = GetResolvedWheelMaxEngineForce(wheel);
    placements.push_back(
        {i,
         wheel.connectionPoint.dot(vehicle_forward),
         GetWheelSideValue(wheel, vehicle_right),
         drive_force > 1.0e-4f ? drive_force : 1.0f});
  }

  if (placements.empty()) {
    return {};
  }

  std::sort(placements.begin(),
            placements.end(),
            [](const WheelPlacement &a, const WheelPlacement &b) {
              if (a.forwardPosition != b.forwardPosition) {
                return a.forwardPosition > b.forwardPosition;
              }
              return a.sidePosition < b.sidePosition;
            });

  const float axle_tolerance = GetAxleDetectionTolerance();
  std::vector<AxleGroup> axles;

  for (const WheelPlacement &placement : placements) {
    if (axles.empty() ||
        std::abs(placement.forwardPosition - axles.back().forwardPosition) > axle_tolerance)
    {
      axles.push_back(AxleGroup());
    }

    AxleGroup &axle = axles.back();
    axle.forwardPosition =
        (axle.forwardPosition * float(axle.wheelCount) + placement.forwardPosition) /
        float(axle.wheelCount + 1);
    ++axle.wheelCount;

    if (placement.sidePosition < 0.0f) {
      if (axle.leftWheel < 0 || placement.sidePosition < axle.leftSidePosition) {
        axle.leftWheel = placement.wheelIndex;
        axle.leftSidePosition = placement.sidePosition;
      }
      axle.leftWeight += placement.driveWeight;
    }
    else {
      if (axle.rightWheel < 0 || placement.sidePosition > axle.rightSidePosition) {
        axle.rightWheel = placement.wheelIndex;
        axle.rightSidePosition = placement.sidePosition;
      }
      axle.rightWeight += placement.driveWeight;
    }
  }

  return axles;
}

JPH::Vec3 JoltVehicle::GetWheelModelRight()
{
  /* Wheel authoring already defines each wheel's axle direction via the wheel basis. */
  return JPH::Vec3::sAxisX();
}

JPH::Vec3 JoltVehicle::GetWheelModelUp()
{
  return JPH::Vec3::sAxisZ();
}

JPH::Mat44 JoltVehicle::GetWheelWorldTransform(int wheelIndex) const
{
  return m_vehicleConstraint->GetWheelWorldTransform(
      wheelIndex, GetWheelModelRight(), GetWheelModelUp());
}

void JoltVehicle::CacheWheelVisualOrientationOffsets()
{
  if (!m_vehicleConstraint) {
    return;
  }

  for (int wheelIndex = 0; wheelIndex < (int)m_wheels.size(); ++wheelIndex) {
    WheelInfo &winfo = m_wheels[wheelIndex];
    if (!winfo.motionState) {
      winfo.visualOrientationOffset = MT_Matrix3x3::Identity();
      continue;
    }

    const MT_Matrix3x3 authoredOrientation = winfo.motionState->GetWorldOrientation();
    const MT_Matrix3x3 neutralOrientation =
        JoltWheelOrientationToMT(GetWheelWorldTransform(wheelIndex));
    winfo.visualOrientationOffset = neutralOrientation.inverse() * authoredOrientation;
  }
}

float JoltVehicle::ComputeSpringDamping(const WheelInfo &wheel, const JPH::WheelWV *joltWheel) const
{
  (void)joltWheel;
  return std::max(0.0f, wheel.suspensionCompression);
}

float JoltVehicle::GetResolvedWheelWidth(const WheelInfo &wheel) const
{
  if (wheel.wheelWidth > 0.0f) {
    return wheel.wheelWidth;
  }
  if (m_wheelWidth > 0.0f) {
    return m_wheelWidth;
  }
  return std::max(0.01f, std::max(0.0f, wheel.wheelRadius) * 0.4f);
}

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
  wheel.downDirection = downDirection.safe_normalized();
  wheel.axleDirection = axleDirection.safe_normalized();
  wheel.suspensionRestLength = std::max(0.0f, suspensionRestLength);
  wheel.wheelRadius = std::max(0.01f, wheelRadius);
  wheel.hasSteering = hasSteering;
  wheel.lastSuspensionLength = wheel.suspensionRestLength;
  m_wheels.push_back(wheel);

  if (m_built) {
    ResetConstraint();
  }
}

bool JoltVehicle::Build()
{
  if (m_built) {
    return true;
  }
  if (m_wheels.empty() || !m_chassisCtrl || !m_env) {
    return false;
  }

  JPH::PhysicsSystem *physSystem = m_env->GetPhysicsSystem();
  JPH::BodyID chassisBodyID = m_chassisCtrl->GetBodyID();
  if (chassisBodyID.IsInvalid()) {
    return false;
  }

  /* The chassis-level ray mask is not used for collision testing: each
   * wheel has its own per-wheel collision tester with its own layer.
   * Use 0xFFFF as a safe default for the base-class object layer. */
  const JPH::ObjectLayer object_layer = JoltMakeObjectLayer(
      m_chassisCtrl->GetCollisionGroup(), 0xFFFF, JOLT_BP_DYNAMIC);

  /* Build vehicle constraint settings. */
  JPH::VehicleConstraintSettings vehicleSettings;
  vehicleSettings.mUp = AxisVector(m_upIndex);
  vehicleSettings.mForward = AxisVector(m_forwardIndex);
  vehicleSettings.mMaxPitchRollAngle = m_maxPitchRollAngle;
  /* Decoupled mapping: a single UI value drives both Jolt overrides, but the
   * position-step override is capped separately because Jolt position steps
   * are far more expensive per iteration than velocity steps and diminish
   * quickly (Jolt default is 2 position / 10 velocity). Treating them as one
   * linear knob wastes CPU at high slider values. */
  const int raw_iter = std::clamp(m_solverIterations, 0, 255);
  const unsigned int velocity_iterations = (unsigned int)raw_iter;
  const unsigned int position_iterations =
      raw_iter == 0 ? 0u : (unsigned int)std::min(raw_iter, kMaxPositionSolverIterations);
  vehicleSettings.mNumVelocityStepsOverride = velocity_iterations;
  vehicleSettings.mNumPositionStepsOverride = position_iterations;

  /* Use WheeledVehicleController in Simple Drive mode. Simple Drive is the
   * only supported mode in UPBGE: it replaces Jolt's automatic gearbox with
   * a single fixed manual gear so throttle directly drives wheel torque.
   *
   * For motorcycles, we substitute MotorcycleControllerSettings which derives
   * from WheeledVehicleControllerSettings and adds a lean spring to
   * auto-balance the bike. All drivetrain / transmission / differential
   * configuration below still applies. */
  JPH::WheeledVehicleControllerSettings *controllerSettings = nullptr;
  if (m_motorcycleMode) {
    JPH::MotorcycleControllerSettings *mcSettings = new JPH::MotorcycleControllerSettings();
    mcSettings->mMaxLeanAngle = std::max(0.0f, m_mcMaxLeanAngle);
    mcSettings->mLeanSpringConstant = std::max(0.0f, m_mcLeanSpringConstant);
    mcSettings->mLeanSpringDamping = std::max(0.0f, m_mcLeanSpringDamping);
    mcSettings->mLeanSpringIntegrationCoefficient = std::max(0.0f, m_mcLeanSpringIntegrationCoef);
    mcSettings->mLeanSpringIntegrationCoefficientDecay =
        std::max(0.0f, m_mcLeanSpringIntegrationDecay);
    mcSettings->mLeanSmoothingFactor = std::clamp(m_mcLeanSmoothingFactor, 0.0f, 1.0f);
    controllerSettings = mcSettings;
  }
  else {
    controllerSettings = new JPH::WheeledVehicleControllerSettings();
  }
  controllerSettings->mEngine.mMaxTorque =
      std::max(1.0f, GetEffectiveEngineTorque() / kSimpleDriveFixedGearRatio);
  controllerSettings->mEngine.mMinRPM = 0.0f;
  controllerSettings->mTransmission.mMode = JPH::ETransmissionMode::Manual;
  controllerSettings->mTransmission.mGearRatios = {kSimpleDriveFixedGearRatio};
  controllerSettings->mTransmission.mReverseGearRatios = {-kSimpleDriveFixedGearRatio};
  controllerSettings->mDifferentialLimitedSlipRatio = std::max(1.0f, m_limitedSlipRatio);
  ConfigureDifferentials(controllerSettings);

  /* Add wheels. */
  vehicleSettings.mWheels.resize(m_wheels.size());
  std::vector<JoltWheelCollisionTesterConfig> collision_tester_configs;
  collision_tester_configs.reserve(m_wheels.size());
  const float wheel_count = std::max(1.0f, float(m_wheels.size()));
  const float chassis_mass = std::max(0.0f, JoltGetBodyMassNoLock(m_env, chassisBodyID));
  for (size_t i = 0; i < m_wheels.size(); ++i) {
    const WheelInfo &winfo = m_wheels[i];

    JPH::WheelSettingsWV *ws = new JPH::WheelSettingsWV();
    ws->mPosition = JoltMath::ToJolt(winfo.connectionPoint);

    const JPH::Vec3 suspensionDirection = JoltMath::ToJolt(winfo.downDirection.safe_normalized());
    ws->mSuspensionDirection = suspensionDirection;
    ws->mSteeringAxis = -suspensionDirection;
    ws->mWheelUp = -ws->mSuspensionDirection;
    const JPH::Vec3 wheelRight = JoltMath::ToJolt(winfo.axleDirection.safe_normalized());
    JPH::Vec3 wheelForward = ws->mWheelUp.Cross(wheelRight);
    if (wheelForward.LengthSq() < 1.0e-6f) {
      wheelForward = vehicleSettings.mForward;
    }
    ws->mWheelForward = wheelForward.Normalized();

    const float suspensionTravel = std::max(0.0f, winfo.suspensionTravel);
    const float restLength = std::max(0.0f, winfo.suspensionRestLength);
    /* Center the travel range around rest length: if rest < travel/2, the
     * old formula clamped min to 0 but kept max unchanged, leaving the
     * spring off-center in its working range. Clamping half-travel to
     * rest keeps the rest position exactly in the middle of [min, max]. */
    const float suspensionHalfTravel = std::min(restLength, suspensionTravel * 0.5f);
    ws->mSuspensionMinLength = restLength - suspensionHalfTravel;
    ws->mSuspensionMaxLength = restLength + suspensionHalfTravel;
    ConfigureSuspensionForcePoint(winfo, *ws);
    ws->mSuspensionSpring.mMode = JPH::ESpringMode::StiffnessAndDamping;
    const float suspension_stiffness = std::max(0.0f, winfo.suspensionStiffness);
    ws->mSuspensionSpring.mStiffness = suspension_stiffness;
    ws->mSuspensionSpring.mDamping = ComputeSpringDamping(winfo, nullptr);
    ws->mRadius = std::max(0.01f, winfo.wheelRadius);
    ws->mWidth = GetResolvedWheelWidth(winfo);
    float wheel_inertia = std::max(0.01f, winfo.inertia);
    if (chassis_mass > 1.0e-4f) {
      const float max_reasonable_inertia =
          std::max(0.01f, 0.5f * (chassis_mass / wheel_count) * ws->mRadius * ws->mRadius);
      wheel_inertia = std::min(wheel_inertia, max_reasonable_inertia);
    }
    ws->mInertia = wheel_inertia;
    ws->mAngularDamping = std::max(0.0f, winfo.angularDamping);
    ws->mMaxSteerAngle = GetResolvedWheelMaxSteerAngle(winfo);
    ws->mMaxBrakeTorque = GetResolvedWheelMaxBrakeTorque(winfo);
    ws->mMaxHandBrakeTorque = winfo.useHandBrake ?
                                  std::max(0.0f, winfo.handBrakeTorque) :
                                  0.0f;
    ConfigureLongitudinalFriction(ws->mLongitudinalFriction, winfo.longitudinalFriction);
    ConfigureLateralFriction(ws->mLateralFriction, winfo.lateralFriction);

    vehicleSettings.mWheels[i] = ws;
    const JPH::ObjectLayer wheel_layer = JoltMakeObjectLayer(
        m_chassisCtrl->GetCollisionGroup(), winfo.rayMask, JOLT_BP_DYNAMIC);
    collision_tester_configs.push_back(
        {winfo.collisionMode, std::max(0.01f, winfo.wheelRadius), wheel_layer});
  }

  vehicleSettings.mController = controllerSettings;
  ConfigureAntiRollBars(vehicleSettings);

  JPH::Ref<JoltPerWheelVehicleCollisionTester> collisionTester = new JoltPerWheelVehicleCollisionTester(
      object_layer, vehicleSettings.mUp, collision_tester_configs);

  /* Lock the chassis body to create the constraint. Keep the lock scope
   * tight: ApplyPostBuildState() below calls into helpers that take a
   * BodyLockRead on the same body (JoltGetBodyMassNoLock), so the write
   * lock must be released before we touch the body again. */
  JPH::Ref<JPH::VehicleConstraint> vehicleConstraintRef;
  {
    const JPH::BodyLockInterface &lockInterface = physSystem->GetBodyLockInterface();
    JPH::BodyLockWrite lock(lockInterface, chassisBodyID);
    if (!lock.Succeeded()) {
      return false;
    }
    JPH::Body &chassisBody = lock.GetBody();

    /* Wrap immediately in a Ref<> so that if anything in between throws the
     * refcount-0 constraint object is released instead of leaked. */
    vehicleConstraintRef = new JPH::VehicleConstraint(chassisBody, vehicleSettings);
    vehicleConstraintRef->SetVehicleCollisionTester(collisionTester);
  }

  m_vehicleConstraint = vehicleConstraintRef.GetPtr();
  physSystem->AddConstraint(m_vehicleConstraint);
  physSystem->AddStepListener(m_vehicleConstraint);

  m_built = true;
  ApplyPostBuildState();
  return true;
}

int JoltVehicle::GetNumWheels() const
{
  return (int)m_wheels.size();
}

MT_Vector3 JoltVehicle::GetWheelPosition(int wheelIndex) const
{
  if (!m_vehicleConstraint || wheelIndex < 0 || wheelIndex >= (int)m_wheels.size()) {
    return MT_Vector3(0, 0, 0);
  }
  JPH::Mat44 worldTransform = GetWheelWorldTransform(wheelIndex);
  return JoltMath::ToMT(JPH::Vec3(worldTransform.GetTranslation()));
}

MT_Quaternion JoltVehicle::GetWheelOrientationQuaternion(int wheelIndex) const
{
  if (!m_vehicleConstraint || wheelIndex < 0 || wheelIndex >= (int)m_wheels.size()) {
    return MT_Quaternion(0, 0, 0, 1);
  }

  const MT_Matrix3x3 wheelOrientation =
      JoltWheelOrientationToMT(GetWheelWorldTransform(wheelIndex)) *
      m_wheels[wheelIndex].visualOrientationOffset;
  return wheelOrientation.getRotation();
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

void JoltVehicle::SetSteeringValue(float steering, int wheelIndex)
{
  if (wheelIndex < 0 || wheelIndex >= (int)m_wheels.size()) {
    return;
  }
  m_wheels[wheelIndex].steeringValue = steering;

  if (m_vehicleConstraint) {
    ApplyRuntimeWheelSettings(wheelIndex);
  }

  if (m_chassisCtrl && std::abs(steering) > 1.0e-4f) {
    m_chassisCtrl->SetActive(true);
  }
}

void JoltVehicle::ApplyEngineForce(float force, int wheelIndex)
{
  if (wheelIndex < 0 || wheelIndex >= (int)m_wheels.size()) {
    return;
  }
  m_wheels[wheelIndex].engineForce = force;

  if (m_chassisCtrl && std::abs(force) > 1.0e-4f) {
    m_chassisCtrl->SetActive(true);
  }
}

void JoltVehicle::ApplyBraking(float braking, int wheelIndex)
{
  if (wheelIndex < 0 || wheelIndex >= (int)m_wheels.size()) {
    return;
  }
  m_wheels[wheelIndex].braking = braking;

  if (m_vehicleConstraint) {
    ApplyRuntimeWheelSettings(wheelIndex);
  }

  if (m_chassisCtrl && std::abs(braking) > 1.0e-4f) {
    m_chassisCtrl->SetActive(true);
  }
}

void JoltVehicle::ApplyHandBraking(float braking, int wheelIndex)
{
  if (wheelIndex < 0 || wheelIndex >= (int)m_wheels.size()) {
    return;
  }
  m_wheels[wheelIndex].handBraking = braking;

  if (m_vehicleConstraint) {
    ApplyRuntimeWheelSettings(wheelIndex);
  }

  if (m_chassisCtrl && std::abs(braking) > 1.0e-4f) {
    m_chassisCtrl->SetActive(true);
  }
}

void JoltVehicle::SetForwardInput(float forward)
{
  m_forwardInput = std::clamp(forward, -1.0f, 1.0f);
  m_useDriverInput = true;
  if (m_chassisCtrl && std::abs(forward) > 1.0e-4f) {
    m_chassisCtrl->SetActive(true);
  }
}

void JoltVehicle::SetRightInput(float right)
{
  m_rightInput = std::clamp(right, -1.0f, 1.0f);
  m_useDriverInput = true;
  if (m_chassisCtrl && std::abs(right) > 1.0e-4f) {
    m_chassisCtrl->SetActive(true);
  }
}

void JoltVehicle::SetBrakeInput(float brake)
{
  m_brakeInput = std::clamp(brake, 0.0f, 1.0f);
  m_useDriverInput = true;
  if (m_chassisCtrl && brake > 1.0e-4f) {
    m_chassisCtrl->SetActive(true);
  }
}

void JoltVehicle::SetHandBrakeInput(float handBrake)
{
  m_handBrakeInput = std::clamp(handBrake, 0.0f, 1.0f);
  m_useDriverInput = true;
  if (m_chassisCtrl && handBrake > 1.0e-4f) {
    m_chassisCtrl->SetActive(true);
  }
}

void JoltVehicle::SetWheelFriction(float friction, int wheelIndex)
{
  if (wheelIndex < 0 || wheelIndex >= (int)m_wheels.size()) {
    return;
  }
  m_wheels[wheelIndex].longitudinalFriction = friction;
  m_wheels[wheelIndex].lateralFriction = friction;

  if (m_vehicleConstraint) {
    ApplyRuntimeWheelSettings(wheelIndex);
  }
}

void JoltVehicle::SetWheelLongitudinalFriction(float friction, int wheelIndex)
{
  if (wheelIndex < 0 || wheelIndex >= (int)m_wheels.size()) {
    return;
  }

  m_wheels[wheelIndex].longitudinalFriction = friction;

  if (m_vehicleConstraint) {
    ApplyRuntimeWheelSettings(wheelIndex);
  }
}

void JoltVehicle::SetWheelLateralFriction(float friction, int wheelIndex)
{
  if (wheelIndex < 0 || wheelIndex >= (int)m_wheels.size()) {
    return;
  }

  m_wheels[wheelIndex].lateralFriction = friction;

  if (m_vehicleConstraint) {
    ApplyRuntimeWheelSettings(wheelIndex);
  }
}

void JoltVehicle::SetWheelUseTraction(int wheelIndex, bool useTraction)
{
  if (wheelIndex < 0 || wheelIndex >= (int)m_wheels.size()) {
    return;
  }

  WheelInfo &wheel = m_wheels[wheelIndex];
  if (wheel.useTraction == useTraction) {
    return;
  }

  wheel.useTraction = useTraction;
  if (m_vehicleConstraint) {
    ApplyLiveDrivetrainUpdate();
  }
}

void JoltVehicle::SetWheelMaxEngineForce(float force, int wheelIndex)
{
  if (wheelIndex < 0 || wheelIndex >= (int)m_wheels.size()) {
    return;
  }

  m_wheels[wheelIndex].maxEngineForce = std::max(0.0f, force);
  if (m_vehicleConstraint) {
    ApplyLiveDrivetrainUpdate();
  }
}

void JoltVehicle::SetWheelMaxBrakeTorque(float torque, int wheelIndex)
{
  if (wheelIndex < 0 || wheelIndex >= (int)m_wheels.size()) {
    return;
  }

  m_wheels[wheelIndex].maxBrakeTorque = std::max(0.0f, torque);
  if (m_vehicleConstraint) {
    ApplyRuntimeWheelSettings(wheelIndex);
  }
}

void JoltVehicle::SetWheelMaxSteerAngle(float angle, int wheelIndex)
{
  if (wheelIndex < 0 || wheelIndex >= (int)m_wheels.size()) {
    return;
  }

  m_wheels[wheelIndex].maxSteerAngle = std::max(0.0f, angle);
  if (m_vehicleConstraint) {
    ApplyRuntimeWheelSettings(wheelIndex);
  }
}

void JoltVehicle::SetSuspensionStiffness(float stiffness, int wheelIndex)
{
  if (wheelIndex < 0 || wheelIndex >= (int)m_wheels.size()) {
    return;
  }
  m_wheels[wheelIndex].suspensionStiffness = stiffness;

  if (m_vehicleConstraint) {
    ApplyRuntimeWheelSettings(wheelIndex);
  }
}

void JoltVehicle::SetSuspensionDamping(float damping, int wheelIndex)
{
  /* Jolt exposes a single suspension damping slot (WheelSettingsWV::
   * mSuspensionSpring.mDamping) which UPBGE drives through
   * SetSuspensionCompression. The Bullet-era "damping_relaxation" property
   * has no independent counterpart in Jolt and is silently inert here.
   *
   * Emit a one-time diagnostic so artists tuning the "Damping Relaxation"
   * slider on a Jolt vehicle know the value is being ignored. Use a static
   * guard so we do not spam per-wheel / per-frame. */
  (void)damping;
  (void)wheelIndex;
  static bool s_warning_emitted = false;
  if (!s_warning_emitted) {
    s_warning_emitted = true;
    CM_FunctionWarning(
        "Jolt vehicles use a single suspension damping value; the "
        "'Damping Relaxation' wheel property is ignored. Tune "
        "'Damping Compression' instead.");
  }
}

void JoltVehicle::SetSuspensionCompression(float compression, int wheelIndex)
{
  if (wheelIndex < 0 || wheelIndex >= (int)m_wheels.size()) {
    return;
  }
  m_wheels[wheelIndex].suspensionCompression = compression;

  if (m_vehicleConstraint) {
    ApplyRuntimeWheelSettings(wheelIndex);
  }
}

void JoltVehicle::SetRollInfluence(float rollInfluence, int wheelIndex)
{
  if (wheelIndex < 0 || wheelIndex >= (int)m_wheels.size()) {
    return;
  }
  m_wheels[wheelIndex].rollInfluence = rollInfluence;

  if (m_vehicleConstraint) {
    ApplyRuntimeWheelSettings(wheelIndex);
  }
}

void JoltVehicle::SetSuspensionTravel(float suspensionTravel, int wheelIndex)
{
  if (wheelIndex < 0 || wheelIndex >= (int)m_wheels.size()) {
    return;
  }
  m_wheels[wheelIndex].suspensionTravel = suspensionTravel;

  if (m_vehicleConstraint) {
    ApplyRuntimeWheelSettings(wheelIndex);
  }
}

void JoltVehicle::SetWheelWidth(float width, int wheelIndex)
{
  if (wheelIndex < 0 || wheelIndex >= (int)m_wheels.size()) {
    return;
  }

  m_wheels[wheelIndex].wheelWidth = std::max(0.0f, width);

  if (m_vehicleConstraint) {
    ApplyRuntimeWheelSettings(wheelIndex);
  }
}

void JoltVehicle::SetWheelCollisionMode(int collisionMode, int wheelIndex)
{
  if (wheelIndex < 0 || wheelIndex >= (int)m_wheels.size()) {
    return;
  }

  m_wheels[wheelIndex].collisionMode = JoltNormalizeWheelCollisionMode(collisionMode);

  if (m_built) {
    ResetConstraint();
  }
}

void JoltVehicle::SetCoordinateSystem(int rightIndex, int upIndex, int forwardIndex)
{
  m_rightIndex = rightIndex;
  m_upIndex = upIndex;
  m_forwardIndex = forwardIndex;

  if (m_built) {
    ResetConstraint();
  }
}

void JoltVehicle::SetRayCastMask(short mask)
{
  /* Chassis-level ray mask is not used for Jolt collision testing;
   * each wheel has its own per-wheel ray mask instead.  Store the
   * value for GetRayCastMask() but do not trigger a rebuild. */
  m_rayCastMask = mask;
}

short JoltVehicle::GetRayCastMask() const
{
  return m_rayCastMask;
}

void JoltVehicle::SetWheelRayCastMask(short mask, int wheelIndex)
{
  if (wheelIndex < 0 || wheelIndex >= (int)m_wheels.size()) {
    return;
  }

  m_wheels[wheelIndex].rayMask = mask;

  if (m_built) {
    ResetConstraint();
  }
}

bool JoltVehicle::NeedsWheelAxleFlip() const
{
  return false;
}

bool JoltVehicle::UsesController(const JoltPhysicsController *ctrl) const
{
  return m_chassisCtrl == ctrl;
}

void JoltVehicle::SetEnvironment(JoltPhysicsEnvironment *env)
{
  m_env = env;
}

void JoltVehicle::ResetConstraint()
{
  if (!m_vehicleConstraint) {
    m_built = false;
    return;
  }

  if (m_env) {
    JPH::PhysicsSystem *physSystem = m_env->GetPhysicsSystem();
    physSystem->RemoveStepListener(m_vehicleConstraint);
    physSystem->RemoveConstraint(m_vehicleConstraint);
  }

  m_vehicleConstraint = nullptr;
  m_built = false;
}

void JoltVehicle::SetMaxPitchRollAngle(float angle)
{
  m_maxPitchRollAngle = angle;
  if (m_vehicleConstraint) {
    m_vehicleConstraint->SetMaxPitchRollAngle(angle);
  }
}

void JoltVehicle::SetSolverIterations(int iterations)
{
  /* Live mutation of the VehicleConstraint overrides writes to plain uint8
   * fields inside Jolt. This must only be called between physics steps (the
   * normal UPBGE game-logic update window). Do not invoke from a
   * PhysicsStepListener or while the solver is running. */
  m_solverIterations = std::clamp(iterations, 0, 255);
  if (m_vehicleConstraint) {
    const unsigned int velocity_iterations = (unsigned int)m_solverIterations;
    const unsigned int position_iterations =
        m_solverIterations == 0 ?
            0u :
            (unsigned int)std::min(m_solverIterations, kMaxPositionSolverIterations);
    m_vehicleConstraint->SetNumVelocityStepsOverride(velocity_iterations);
    m_vehicleConstraint->SetNumPositionStepsOverride(position_iterations);
  }
}

void JoltVehicle::SetWheelDefaults(float width, float inertia, float angularDamping)
{
  m_wheelWidth = width;
  /* Apply as defaults to all wheels that haven't been individually configured. */
  for (auto &w : m_wheels) {
    w.inertia = inertia;
    w.angularDamping = angularDamping;
  }
  if (m_vehicleConstraint) {
    for (int i = 0; i < (int)m_wheels.size(); ++i) {
      ApplyRuntimeWheelSettings(i);
    }
  }
}

void JoltVehicle::SetDrivetrain(float handbrakeTorque,
                                float engineTorque,
                                float differentialRatio,
                                float limitedSlipRatio)
{
  /* handbrakeTorque is now per-wheel; apply as default to all handbrake wheels. */
  for (auto &w : m_wheels) {
    if (w.useHandBrake) {
      w.handBrakeTorque = handbrakeTorque;
    }
  }
  m_engineTorque = engineTorque;
  m_differentialRatio = differentialRatio;
  m_limitedSlipRatio = limitedSlipRatio;
  if (m_vehicleConstraint) {
    ApplyLiveDrivetrainUpdate();
    for (int i = 0; i < (int)m_wheels.size(); ++i) {
      ApplyRuntimeWheelSettings(i);
    }
  }
}

void JoltVehicle::SetAntiRoll(float front, float rear)
{
  m_frontAntiRoll = front;
  m_rearAntiRoll = rear;
  if (m_vehicleConstraint) {
    ReconfigureLiveAntiRollBars();
  }
}

void JoltVehicle::SetSimpleDriveMode(bool enabled)
{
  /* Simple Drive is the only supported drive mode in UPBGE's Jolt vehicle
   * backend. The flag is kept in the interface for API compatibility but
   * has no effect: passing true is the current behavior, passing false
   * emits a one-time diagnostic to surface the deprecation. */
  if (!enabled) {
    static bool s_warning_emitted = false;
    if (!s_warning_emitted) {
      s_warning_emitted = true;
      CM_FunctionWarning(
          "Non-Simple-Drive mode was removed from the Jolt vehicle "
          "backend; SetSimpleDriveMode(false) has no effect.");
    }
  }
}

void JoltVehicle::SetSimpleDriveSteering(float steeringSpeed, float speedReduction)
{
  m_simpleDriveSteeringSpeed = std::max(0.0f, steeringSpeed);
  m_simpleDriveHighSpeedSteeringReduction = std::clamp(speedReduction, 0.0f, 1.0f);
}

void JoltVehicle::SetWheelUseHandBrake(int wheelIndex, bool useHandBrake)
{
  if (wheelIndex < 0 || wheelIndex >= (int)m_wheels.size()) {
    return;
  }
  m_wheels[wheelIndex].useHandBrake = useHandBrake;
  if (m_vehicleConstraint) {
    ApplyRuntimeWheelSettings(wheelIndex);
  }
}

void JoltVehicle::SetWheelInertia(float inertia, int wheelIndex)
{
  if (wheelIndex < 0 || wheelIndex >= (int)m_wheels.size()) {
    return;
  }
  m_wheels[wheelIndex].inertia = inertia;
  if (m_vehicleConstraint) {
    ApplyRuntimeWheelSettings(wheelIndex);
  }
}

void JoltVehicle::SetWheelAngularDamping(float damping, int wheelIndex)
{
  if (wheelIndex < 0 || wheelIndex >= (int)m_wheels.size()) {
    return;
  }
  m_wheels[wheelIndex].angularDamping = damping;
  if (m_vehicleConstraint) {
    ApplyRuntimeWheelSettings(wheelIndex);
  }
}

void JoltVehicle::SetWheelHandBrakeTorque(float torque, int wheelIndex)
{
  if (wheelIndex < 0 || wheelIndex >= (int)m_wheels.size()) {
    return;
  }
  m_wheels[wheelIndex].handBrakeTorque = torque;
  if (m_vehicleConstraint) {
    ApplyRuntimeWheelSettings(wheelIndex);
  }
}

void JoltVehicle::SetMaxBrakeTorque(float torque)
{
  /* Jolt uses per-wheel brake torque exclusively; chassis-level
   * brake is a Bullet concept.  Keep the override as a no-op. */
  (void)torque;
}

void JoltVehicle::SetMotorcycleMode(bool enabled)
{
  /* Build() picks the controller type based on this flag. Switching it after
   * the constraint is built is not supported: the controller kind is baked
   * into the VehicleConstraint on construction. */
  m_motorcycleMode = enabled;
}

void JoltVehicle::SetMotorcycleLean(float maxAngle,
                                    float springConstant,
                                    float springDamping,
                                    float integrationCoef,
                                    float integrationDecay,
                                    float smoothingFactor)
{
  m_mcMaxLeanAngle = std::max(0.0f, maxAngle);
  m_mcLeanSpringConstant = std::max(0.0f, springConstant);
  m_mcLeanSpringDamping = std::max(0.0f, springDamping);
  m_mcLeanSpringIntegrationCoef = std::max(0.0f, integrationCoef);
  m_mcLeanSpringIntegrationDecay = std::max(0.0f, integrationDecay);
  m_mcLeanSmoothingFactor = std::clamp(smoothingFactor, 0.0f, 1.0f);

  /* If the constraint is already live, push the spring values onto it so
   * edits at runtime take effect without rebuild. mMaxLeanAngle has no live
   * setter in Jolt, so it only applies on rebuild. */
  if (m_vehicleConstraint) {
    if (auto *mc = dynamic_cast<JPH::MotorcycleController *>(
            m_vehicleConstraint->GetController())) {
      mc->SetLeanSpringConstant(m_mcLeanSpringConstant);
      mc->SetLeanSpringDamping(m_mcLeanSpringDamping);
      mc->SetLeanSpringIntegrationCoefficient(m_mcLeanSpringIntegrationCoef);
      mc->SetLeanSpringIntegrationCoefficientDecay(m_mcLeanSpringIntegrationDecay);
      mc->SetLeanSmoothingFactor(m_mcLeanSmoothingFactor);
    }
  }
}

void JoltVehicle::SetMotorcycleLeanFlags(bool enableController, bool enableSteeringLimit)
{
  m_mcEnableLeanController = enableController;
  m_mcEnableLeanSteeringLimit = enableSteeringLimit;
  if (m_vehicleConstraint) {
    if (auto *mc = dynamic_cast<JPH::MotorcycleController *>(
            m_vehicleConstraint->GetController())) {
      mc->EnableLeanController(m_mcEnableLeanController);
      mc->EnableLeanSteeringLimit(m_mcEnableLeanSteeringLimit);
    }
  }
}

void JoltVehicle::SetMotorcycleSuspensionForcePointOverride(bool enableFront, bool enableRear)
{
  m_mcOverrideFrontSuspensionForcePoint = enableFront;
  m_mcOverrideRearSuspensionForcePoint = enableRear;

  if (!m_vehicleConstraint || !m_motorcycleMode) {
    return;
  }

  for (int wheel_index = 0; wheel_index < (int)m_wheels.size(); ++wheel_index) {
    ApplyRuntimeWheelSettings(wheel_index);
  }
}

void JoltVehicle::ApplyRuntimeWheelSettings(int wheelIndex)
{
  if (!m_vehicleConstraint || wheelIndex < 0 || wheelIndex >= (int)m_wheels.size()) {
    return;
  }

  WheelInfo &winfo = m_wheels[wheelIndex];
  JPH::WheelWV *wheel = static_cast<JPH::WheelWV *>(m_vehicleConstraint->GetWheels()[wheelIndex]);
  JPH::WheelSettingsWV *settings = const_cast<JPH::WheelSettingsWV *>(wheel->GetSettings());

  const float rest_length = std::max(0.0f, winfo.suspensionRestLength);
  /* Centered travel range: see matching block in Build(). */
  const float half_travel = std::min(rest_length, std::max(0.0f, winfo.suspensionTravel) * 0.5f);
  settings->mSuspensionMinLength = rest_length - half_travel;
  settings->mSuspensionMaxLength = rest_length + half_travel;
  settings->mSuspensionSpring.mMode = JPH::ESpringMode::StiffnessAndDamping;
  const float suspension_stiffness = std::max(0.0f, winfo.suspensionStiffness);
  settings->mSuspensionSpring.mStiffness = suspension_stiffness;
  settings->mSuspensionSpring.mDamping = ComputeSpringDamping(winfo, wheel);
  ConfigureSuspensionForcePoint(winfo, *settings);
  settings->mRadius = std::max(0.01f, winfo.wheelRadius);
  settings->mWidth = GetResolvedWheelWidth(winfo);
  float wheel_inertia = std::max(0.01f, winfo.inertia);
  const float chassis_mass =
      std::max(0.0f, JoltGetBodyMassNoLock(m_env, m_chassisCtrl->GetBodyID()));
  if (chassis_mass > 1.0e-4f) {
    const float wheel_count = std::max(1.0f, float(m_wheels.size()));
    const float max_reasonable_inertia =
        std::max(0.01f,
                 0.5f * (chassis_mass / wheel_count) * settings->mRadius * settings->mRadius);
    wheel_inertia = std::min(wheel_inertia, max_reasonable_inertia);
  }
  settings->mInertia = wheel_inertia;
  settings->mAngularDamping = std::max(0.0f, winfo.angularDamping);
  settings->mMaxSteerAngle = GetResolvedWheelMaxSteerAngle(winfo);
  settings->mMaxBrakeTorque = GetResolvedWheelMaxBrakeTorque(winfo);
  settings->mMaxHandBrakeTorque = winfo.useHandBrake ?
                                      std::max(0.0f, winfo.handBrakeTorque) :
                                      0.0f;
  ConfigureLongitudinalFriction(settings->mLongitudinalFriction, winfo.longitudinalFriction);
  ConfigureLateralFriction(settings->mLateralFriction, winfo.lateralFriction);
  winfo.lastSuspensionLength = wheel->GetSuspensionLength();
}

void JoltVehicle::ApplyPostBuildState()
{
  for (int i = 0; i < (int)m_wheels.size(); ++i) {
    ApplyRuntimeWheelSettings(i);
  }

  CacheWheelVisualOrientationOffsets();

  /* For motorcycles, apply the runtime lean-controller toggles. These are
   * not part of the settings struct, they must be set on the live
   * controller after the constraint is created. */
  if (m_motorcycleMode && m_vehicleConstraint) {
    if (auto *mc = dynamic_cast<JPH::MotorcycleController *>(
            m_vehicleConstraint->GetController())) {
      mc->EnableLeanController(m_mcEnableLeanController);
      mc->EnableLeanSteeringLimit(m_mcEnableLeanSteeringLimit);
    }
  }
}

void JoltVehicle::ClearPerWheelForces()
{
  for (WheelInfo &winfo : m_wheels) {
    winfo.engineForce = 0.0f;
    winfo.braking = 0.0f;
    winfo.handBraking = 0.0f;
    winfo.steeringValue = 0.0f;
  }
}

void JoltVehicle::ConfigureDifferentials(JPH::WheeledVehicleControllerSettings *controllerSettings) const
{
  if (m_wheels.size() < 2) {
    return;
  }

  const std::vector<AxleGroup> axles = DetectAxleGroups(true);
  if (axles.empty()) {
    return;
  }

  auto add_differential = [&](int left_wheel,
                              float left_weight,
                              int right_wheel,
                              float right_weight,
                              float engine_torque_ratio) {
    if (left_wheel < 0 && right_wheel < 0) {
      return;
    }
    JPH::VehicleDifferentialSettings differential;
    differential.mLeftWheel = left_wheel;
    differential.mRightWheel = right_wheel;
    /* Simple Drive neutralizes the configurable differential ratio because
     * drive strength is already scaled via engine torque / fixed gear. */
    differential.mDifferentialRatio = 1.0f;
    differential.mLimitedSlipRatio = std::max(1.0f, m_limitedSlipRatio);
    if (left_wheel < 0) {
      differential.mLeftRightSplit = 1.0f;
    }
    else if (right_wheel < 0) {
      differential.mLeftRightSplit = 0.0f;
    }
    else {
      differential.mLeftRightSplit =
          right_weight / std::max(1.0e-4f, left_weight + right_weight);
    }
    differential.mEngineTorqueRatio = engine_torque_ratio;
    controllerSettings->mDifferentials.push_back(differential);
  };

  float total_drive_weight = 0.0f;
  for (const AxleGroup &axle : axles) {
    if (!axle.HasAnyWheel()) {
      continue;
    }
    total_drive_weight += std::max(1.0e-4f, axle.TotalWeight());
  }

  if (total_drive_weight <= 1.0e-4f) {
    return;
  }

  for (const AxleGroup &axle : axles) {
    if (!axle.HasAnyWheel()) {
      continue;
    }

    const float axle_total = std::max(1.0e-4f, axle.TotalWeight());
    add_differential(axle.leftWheel,
                     axle.leftWeight,
                     axle.rightWheel,
                     axle.rightWeight,
                     axle_total / total_drive_weight);
  }
}

void JoltVehicle::ConfigureAntiRollBars(JPH::VehicleConstraintSettings &vehicleSettings) const
{
  const std::vector<AxleGroup> axles = DetectAxleGroups(false);
  if (axles.empty()) {
    return;
  }

  auto add_anti_roll = [&](int left_wheel, int right_wheel, float stiffness) {
    if (left_wheel < 0 || right_wheel < 0 || stiffness <= 0.0f) {
      return;
    }
    JPH::VehicleAntiRollBar bar;
    bar.mLeftWheel = left_wheel;
    bar.mRightWheel = right_wheel;
    bar.mStiffness = stiffness;
    vehicleSettings.mAntiRollBars.push_back(bar);
  };

  /* Special-case a single axle: with only one axle the front/rear split is
   * degenerate and the axle would always be classified as "rear" (because
   * axle_split equals its forwardPosition). Apply both configured values by
   * averaging any non-zero contributions so a bike/trike using only the
   * "front" anti-roll property still produces stiffness. */
  if (axles.size() == 1) {
    const AxleGroup &axle = axles.front();
    if (axle.HasWheelPair()) {
      float stiffness = 0.0f;
      int contribution_count = 0;
      if (m_frontAntiRoll > 0.0f) {
        stiffness += m_frontAntiRoll;
        ++contribution_count;
      }
      if (m_rearAntiRoll > 0.0f) {
        stiffness += m_rearAntiRoll;
        ++contribution_count;
      }
      if (contribution_count > 0) {
        stiffness /= float(contribution_count);
        add_anti_roll(axle.leftWheel, axle.rightWheel, stiffness);
      }
    }
    return;
  }

  const float axle_split = 0.5f * (axles.front().forwardPosition + axles.back().forwardPosition);
  int front_axle_count = 0;
  int rear_axle_count = 0;

  for (const AxleGroup &axle : axles) {
    if (!axle.HasWheelPair()) {
      continue;
    }

    if (axle.forwardPosition > axle_split) {
      ++front_axle_count;
    }
    else {
      ++rear_axle_count;
    }
  }

  const float front_stiffness =
      front_axle_count > 0 ? (m_frontAntiRoll / float(front_axle_count)) : 0.0f;
  const float rear_stiffness =
      rear_axle_count > 0 ? (m_rearAntiRoll / float(rear_axle_count)) : 0.0f;

  for (const AxleGroup &axle : axles) {
    if (!axle.HasWheelPair()) {
      continue;
    }

    add_anti_roll(axle.leftWheel,
                  axle.rightWheel,
                  axle.forwardPosition > axle_split ? front_stiffness : rear_stiffness);
  }
}

void JoltVehicle::ApplyLiveDrivetrainUpdate()
{
  if (!m_vehicleConstraint) {
    return;
  }

  JPH::WheeledVehicleController *controller =
      static_cast<JPH::WheeledVehicleController *>(m_vehicleConstraint->GetController());
  if (!controller) {
    return;
  }

  const JPH::VehicleTransmissionSettings defaultTransmission;

  controller->GetEngine().mMaxTorque =
      std::max(1.0f, GetEffectiveEngineTorque() / kSimpleDriveFixedGearRatio);
  controller->GetEngine().mMinRPM = 0.0f;
  controller->GetEngine().SetCurrentRPM(controller->GetEngine().GetCurrentRPM());

  JPH::VehicleTransmission &transmission = controller->GetTransmission();
  transmission.mMode = JPH::ETransmissionMode::Manual;
  transmission.mGearRatios = {kSimpleDriveFixedGearRatio};
  transmission.mReverseGearRatios = {-kSimpleDriveFixedGearRatio};
  transmission.mClutchStrength = defaultTransmission.mClutchStrength;

  int currentGear = transmission.GetCurrentGear();
  if (currentGear == 0) {
    currentGear = 1;
  }
  transmission.Set(currentGear < 0 ? -1 : 1, 0.0f);

  controller->SetDifferentialLimitedSlipRatio(std::max(1.0f, m_limitedSlipRatio));

  /* Rebuild differentials on the live controller using the existing
   * ConfigureDifferentials helper with a temporary settings object. */
  JPH::WheeledVehicleControllerSettings tempSettings;
  ConfigureDifferentials(&tempSettings);

  auto &liveDiffs = controller->GetDifferentials();
  liveDiffs.clear();
  for (const auto &diff : tempSettings.mDifferentials) {
    liveDiffs.push_back(diff);
  }
}

void JoltVehicle::ReconfigureLiveAntiRollBars()
{
  if (!m_vehicleConstraint) {
    return;
  }

  /* Rebuild anti-roll bars on the live constraint using the existing
   * ConfigureAntiRollBars helper with a temporary settings object. */
  JPH::VehicleConstraintSettings tempSettings;
  ConfigureAntiRollBars(tempSettings);

  auto &bars = m_vehicleConstraint->GetAntiRollBars();
  bars.clear();
  for (const auto &bar : tempSettings.mAntiRollBars) {
    bars.push_back(bar);
  }
}

float JoltVehicle::GetConfiguredMaxEngineForce(const WheelInfo &wheel) const
{
  /* Return the explicitly configured max engine force for normalization.
   * Unlike GetResolvedWheelMaxEngineForce, this never falls back to the
   * current per-frame engineForce value (which would always normalize to 1.0). */
  if (wheel.maxEngineForce > 1.0e-4f) {
    return wheel.maxEngineForce;
  }
  /* Derive from chassis engine torque. Simple Drive uses a fixed internal
   * reduction ratio and neutralizes the configurable differential ratio, so
   * derive force directly as torque / radius. */
  if (m_engineTorque > 1.0e-4f) {
    return m_engineTorque / std::max(0.01f, wheel.wheelRadius);
  }
  return 0.0f;
}

float JoltVehicle::GetConfiguredMaxSteerAngle(const WheelInfo &wheel) const
{
  if (!wheel.hasSteering) {
    return 0.0f;
  }
  if (wheel.maxSteerAngle > 1.0e-4f) {
    return wheel.maxSteerAngle;
  }
  return 0.0f;
}

float JoltVehicle::GetConfiguredMaxBrakeTorque(const WheelInfo &wheel) const
{
  if (wheel.maxBrakeTorque > 1.0e-4f) {
    return wheel.maxBrakeTorque;
  }
  return 0.0f;
}

float JoltVehicle::UpdateSimpleDriveSteeringInput(float rightInput, float deltaTime)
{
  constexpr float steering_epsilon = 1.0e-4f;

  float target_input = std::clamp(rightInput, -1.0f, 1.0f);

  if (m_chassisCtrl) {
    const MT_Vector3 linear_velocity = m_chassisCtrl->GetLinearVelocity();
    const float speed_range = kSimpleDriveSteeringReductionEndSpeed -
                              kSimpleDriveSteeringReductionStartSpeed;
    float speed_factor = 0.0f;
    if (speed_range > 1.0e-4f) {
      speed_factor = std::clamp((linear_velocity.length() - kSimpleDriveSteeringReductionStartSpeed) /
                                    speed_range,
                                0.0f,
                                1.0f);
      speed_factor = speed_factor * speed_factor * (3.0f - 2.0f * speed_factor);
    }

    const float steering_scale = std::clamp(
      1.0f - kSimpleDriveBaseSteeringReduction *
            kSimpleDriveSteeringReductionStrengthMultiplier *
            m_simpleDriveHighSpeedSteeringReduction * speed_factor,
      0.0f,
      1.0f);
    target_input *= steering_scale;
  }

  const float steering_time = std::max(0.0f, m_simpleDriveSteeringSpeed);
  if (steering_time <= steering_epsilon || deltaTime <= 0.0f) {
    m_simpleDriveFilteredRightInput = target_input;
    m_simpleDriveSteeringStartInput = target_input;
    m_simpleDriveSteeringTargetInput = target_input;
    m_simpleDriveSteeringElapsedTime = 0.0f;
    m_simpleDriveSteeringMoveDuration = steering_time;
    return target_input;
  }

  if (std::abs(target_input - m_simpleDriveSteeringTargetInput) > steering_epsilon ||
      std::abs(steering_time - m_simpleDriveSteeringMoveDuration) > steering_epsilon) {
    m_simpleDriveSteeringStartInput = m_simpleDriveFilteredRightInput;
    m_simpleDriveSteeringTargetInput = target_input;
    m_simpleDriveSteeringElapsedTime = 0.0f;
    m_simpleDriveSteeringMoveDuration = steering_time;
  }

  /* Steering Speed is treated as time to reach the latest target, so a full
   * right-to-left reversal finishes in the same configured time as any other
   * steering direction change. */
  m_simpleDriveSteeringElapsedTime = std::min(m_simpleDriveSteeringElapsedTime + deltaTime,
                                              m_simpleDriveSteeringMoveDuration);

  const float interpolation_factor =
      std::clamp(m_simpleDriveSteeringElapsedTime / m_simpleDriveSteeringMoveDuration,
                 0.0f,
                 1.0f);
  m_simpleDriveFilteredRightInput = std::clamp(
      m_simpleDriveSteeringStartInput +
          (m_simpleDriveSteeringTargetInput - m_simpleDriveSteeringStartInput) *
              interpolation_factor,
      -1.0f,
      1.0f);
  return m_simpleDriveFilteredRightInput;
}

void JoltVehicle::PreStep(float deltaTime)
{
  if (!Build()) {
    return;
  }

  constexpr float input_epsilon = 1.0e-4f;

  JPH::WheeledVehicleController *controller =
      static_cast<JPH::WheeledVehicleController *>(m_vehicleConstraint->GetController());
  if (controller) {
    float forward_input = std::clamp(m_forwardInput, -1.0f, 1.0f);
    float right_input = std::clamp(m_rightInput, -1.0f, 1.0f);
    float brake_input = std::clamp(m_brakeInput, 0.0f, 1.0f);
    float hand_brake_input = std::clamp(m_handBrakeInput, 0.0f, 1.0f);

    /* Convert per-wheel forces into normalized inputs using a weighted average
     * across all wheels that have a non-zero force set this frame.
     * Weights are proportional to each wheel's configured maximum, so wheels
     * with higher drive capacity contribute more to the final input. */
    float fwd_weighted_sum = 0.0f, fwd_total_weight = 0.0f;
    float steer_weighted_sum = 0.0f, steer_total_weight = 0.0f;

    for (int i = 0; i < (int)m_wheels.size(); ++i) {
      const WheelInfo &winfo = m_wheels[i];

      /* Engine force → forward input (weighted average). */
      if (std::abs(winfo.engineForce) > input_epsilon) {
        const float max_force = GetConfiguredMaxEngineForce(winfo);
        if (max_force > input_epsilon) {
          const float normalized =
              std::clamp(winfo.engineForce / max_force, -1.0f, 1.0f);
          fwd_weighted_sum += normalized * max_force;
          fwd_total_weight += max_force;
        }
      }

      /* Steering → right input (weighted average). */
      if (std::abs(winfo.steeringValue) > input_epsilon) {
        const float max_steer = GetConfiguredMaxSteerAngle(winfo);
        if (max_steer > input_epsilon) {
          const float normalized =
              std::clamp(winfo.steeringValue / max_steer, -1.0f, 1.0f);
          steer_weighted_sum += normalized * max_steer;
          steer_total_weight += max_steer;
        }
      }

      /* Brake → brake input (strongest braking wins). */
      const float max_brake = GetConfiguredMaxBrakeTorque(winfo);
      if (max_brake > input_epsilon && winfo.braking > input_epsilon) {
        brake_input = std::max(
            brake_input, std::clamp(winfo.braking / max_brake, 0.0f, 1.0f));
      }

      /* Handbrake → handbrake input (strongest wins). */
      const float max_handbrake = winfo.useHandBrake ?
                                      std::max(0.0f, winfo.handBrakeTorque) :
                                      0.0f;
      if (max_handbrake > input_epsilon && winfo.handBraking > input_epsilon) {
        hand_brake_input = std::max(
            hand_brake_input,
            std::clamp(winfo.handBraking / max_handbrake, 0.0f, 1.0f));
      }
    }

    /* Apply weighted average for forward and steering per-wheel inputs.
     * If any normalized driver-input setter (SetForwardInput / SetRightInput /
     * SetBrakeInput / SetHandBrakeInput) was called this frame, the normalized
     * path owns forward + steering and the per-wheel fallback is ignored so
     * the two input models cannot fight. Per-wheel brake / handbrake are
     * still merged additively via max() below because they have
     * independent per-wheel semantics. */
    if (!m_useDriverInput) {
      if (fwd_total_weight > input_epsilon) {
        forward_input = std::clamp(fwd_weighted_sum / fwd_total_weight, -1.0f, 1.0f);
      }
      if (steer_total_weight > input_epsilon) {
        right_input = std::clamp(steer_weighted_sum / steer_total_weight, -1.0f, 1.0f);
      }
    }

    forward_input = std::clamp(forward_input, -1.0f, 1.0f);
    right_input = std::clamp(right_input, -1.0f, 1.0f);
    brake_input = std::clamp(brake_input, 0.0f, 1.0f);
    hand_brake_input = std::clamp(hand_brake_input, 0.0f, 1.0f);

    /* Handbrake overrides throttle: cancel engine drive when handbrake is
     * active.  Normal brake does NOT cancel throttle so that threshold
     * braking (simultaneous gas + brake) remains possible. */
    if (hand_brake_input > input_epsilon) {
      forward_input = 0.0f;
    }

    controller->GetEngine().mMaxTorque =
      std::max(1.0f, GetEffectiveEngineTorque() / kSimpleDriveFixedGearRatio);
    controller->SetDifferentialLimitedSlipRatio(std::max(1.0f, m_limitedSlipRatio));
    for (JPH::VehicleDifferentialSettings &differential : controller->GetDifferentials()) {
      differential.mLimitedSlipRatio = std::max(1.0f, m_limitedSlipRatio);
    }

    right_input = UpdateSimpleDriveSteeringInput(right_input, deltaTime);

    /* Simple Drive mode keeps Jolt's native wheel / tire / suspension
     * solver but replaces the automatic gearbox with a fixed manual gear.
     * The clutch is fully engaged only while throttle is held; releasing
     * throttle immediately disconnects the engine so wheel spin decays from
     * ground contact, wheel inertia and angular damping only. */
    JPH::VehicleTransmission &transmission = controller->GetTransmission();
    const float drive_input = std::abs(forward_input);

    int currentGear = transmission.GetCurrentGear();
    if (forward_input > input_epsilon) {
      currentGear = 1;
    }
    else if (forward_input < -input_epsilon) {
      currentGear = -1;
    }
    else if (currentGear == 0) {
      currentGear = 1;
    }

    transmission.Set(currentGear, drive_input > input_epsilon ? 1.0f : 0.0f);
    controller->SetDriverInput(drive_input, right_input, brake_input, hand_brake_input);

    /* Reset all inputs after consumption so they don't persist if the
     * actuator doesn't fire next frame (fixes "sticky" driver inputs). */
    m_forwardInput = 0.0f;
    m_rightInput = 0.0f;
    m_brakeInput = 0.0f;
    m_handBrakeInput = 0.0f;
    m_useDriverInput = false;
  }

  ClearPerWheelForces();
}

void JoltVehicle::SyncWheels()
{
  if (!Build() || !m_vehicleConstraint) {
    return;
  }

  for (int i = 0; i < (int)m_wheels.size(); ++i) {
    WheelInfo &winfo = m_wheels[i];
    if (!winfo.motionState) {
      continue;
    }

    JPH::Mat44 worldTransform = GetWheelWorldTransform(i);

    MT_Vector3 pos = JoltMath::ToMT(JPH::Vec3(worldTransform.GetTranslation()));
    winfo.motionState->SetWorldPosition(pos);

    const MT_Matrix3x3 wheelOrientation =
        JoltWheelOrientationToMT(worldTransform) * winfo.visualOrientationOffset;
    winfo.motionState->SetWorldOrientation(wheelOrientation);
  }
}
