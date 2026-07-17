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

/** \file JoltConstraint.cpp
 *  \ingroup physjolt
 */

#include "JoltConstraint.h"

#include <algorithm>

#include <Jolt/Jolt.h>

JPH_SUPPRESS_WARNINGS

#include <Jolt/Physics/Constraints/ConeConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/SixDOFConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>

#include "JoltPhysicsEnvironment.h"

#include <cfloat>
#include <cmath>

/* -------------------------------------------------------------------- */
/** \name JoltConstraint — Construction / Destruction
 * \{ */

JoltConstraint::JoltConstraint(JPH::Constraint *constraint,
                               PHY_ConstraintType type,
                               int id,
                               bool disableCollision,
                               JoltPhysicsEnvironment *env)
    : m_constraint(constraint),
      m_env(env),
      m_type(type),
      m_id(id),
      m_disableCollision(disableCollision),
      m_active(true),
      m_breakingThreshold(FLT_MAX)
{
}

JoltConstraint::~JoltConstraint()
{
  /* Constraint lifetime is managed by PhysicsSystem. The constraint is
   * removed from the system in RemoveConstraintById() before deletion. */
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name PHY_IConstraint — Enable / Disable
 * \{ */

bool JoltConstraint::GetEnabled() const
{
  if (!m_constraint) {
    return false;
  }
  return m_constraint->GetEnabled();
}

void JoltConstraint::SetEnabled(bool enabled)
{
  if (!m_constraint || m_constraint->GetEnabled() == enabled) {
    return;
  }
  m_constraint->SetEnabled(enabled);

  if (m_env) {
    /* Enabling or disabling changes the island topology and invalidates cached
     * impulses in the surviving connected constraints. */
    JPH::BodyInterface &bi = m_env->GetBodyInterface();
    JPH::TwoBodyConstraint *tbc = static_cast<JPH::TwoBodyConstraint *>(m_constraint);
    const JPH::BodyID bodyID1 = tbc->GetBody1()->GetID();
    const JPH::BodyID bodyID2 = tbc->GetBody2()->GetID();
    m_env->NotifyConstraintTopologyChanged(bodyID1, bodyID2);
    if (!bodyID1.IsInvalid()) {
      bi.ActivateBody(bodyID1);
    }
    if (!bodyID2.IsInvalid()) {
      bi.ActivateBody(bodyID2);
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name PHY_IConstraint — SetParam / GetParam
 *
 * Param indices follow Bullet's convention for Python API compatibility:
 *   0-5:   Constraint limits (low/high for each DOF)
 *   6-8:   Translational motors (target velocity, max force)
 *   9-11:  Rotational motors (target velocity, max torque)
 *   12-17: Motorized springs (stiffness, damping)
 * \{ */

void JoltConstraint::SetParam(int param, float value0, float value1)
{
  if (!m_constraint) {
    return;
  }

  switch (m_constraint->GetSubType()) {
    case JPH::EConstraintSubType::SixDOF: {
      JPH::SixDOFConstraint *sixdof = static_cast<JPH::SixDOFConstraint *>(m_constraint);
      if (param >= 0 && param <= 2) {
        /* param 0-2: translation limits X,Y,Z.
         * value0 = low limit, value1 = high limit.
         * Bullet convention: min > max means "free" (unconstrained).
         * Jolt convention: min = -FLT_MAX, max = FLT_MAX for free. */
        float lo = value0, hi = value1;
        if (!std::isfinite(lo) || !std::isfinite(hi) || lo > hi) {
          lo = -FLT_MAX;
          hi = FLT_MAX;
        }
        JPH::Vec3 limMin = sixdof->GetTranslationLimitsMin();
        JPH::Vec3 limMax = sixdof->GetTranslationLimitsMax();
        limMin.SetComponent(param, lo);
        limMax.SetComponent(param, hi);
        sixdof->SetTranslationLimits(limMin, limMax);
      }
      else if (param >= 3 && param <= 5) {
        /* param 3-5: rotation limits X,Y,Z.
         * value0 = low limit, value1 = high limit.
         * Bullet convention: min > max means "free" (unconstrained).
         * Jolt convention: min = -FLT_MAX, max = FLT_MAX for free. */
        float lo = value0, hi = value1;
        if (!std::isfinite(lo) || !std::isfinite(hi) || lo > hi) {
          lo = -FLT_MAX;
          hi = FLT_MAX;
        }
        else {
          lo = std::clamp(lo, -JPH::JPH_PI, JPH::JPH_PI);
          hi = std::clamp(hi, -JPH::JPH_PI, JPH::JPH_PI);
        }
        JPH::Vec3 limMin = sixdof->GetRotationLimitsMin();
        JPH::Vec3 limMax = sixdof->GetRotationLimitsMax();
        limMin.SetComponent(param - 3, lo);
        limMax.SetComponent(param - 3, hi);
        sixdof->SetRotationLimits(limMin, limMax);
      }
      else if (param >= 6 && param <= 8) {
        /* param 6-8: translational motors (target velocity, max force). */
        JPH::SixDOFConstraint::EAxis axis = static_cast<JPH::SixDOFConstraint::EAxis>(param - 6);
        if (value1 > 0.0f) {
          JPH::MotorSettings &motor = sixdof->GetMotorSettings(axis);
          motor.mMinForceLimit = -value1;
          motor.mMaxForceLimit = value1;
          sixdof->SetMotorState(axis, JPH::EMotorState::Velocity);
          sixdof->SetTargetVelocityCS(JPH::Vec3(
              (param == 6) ? value0 : 0.0f,
              (param == 7) ? value0 : 0.0f,
              (param == 8) ? value0 : 0.0f));
        }
        else {
          sixdof->SetMotorState(axis, JPH::EMotorState::Off);
        }
      }
      else if (param >= 9 && param <= 11) {
        /* param 9-11: rotational motors (target angular velocity, max torque). */
        JPH::SixDOFConstraint::EAxis axis = static_cast<JPH::SixDOFConstraint::EAxis>(
            param - 9 + 3);  /* +3 to get rotation axes */
        if (value1 > 0.0f) {
          JPH::MotorSettings &motor = sixdof->GetMotorSettings(axis);
          motor.mMinTorqueLimit = -value1;
          motor.mMaxTorqueLimit = value1;
          sixdof->SetMotorState(axis, JPH::EMotorState::Velocity);
          sixdof->SetTargetAngularVelocityCS(JPH::Vec3(
              (param == 9) ? value0 : 0.0f,
              (param == 10) ? value0 : 0.0f,
              (param == 11) ? value0 : 0.0f));
        }
        else {
          sixdof->SetMotorState(axis, JPH::EMotorState::Off);
        }
      }
      else if (param >= 12 && param <= 17) {
        /* param 12-17: motorized springs (stiffness=value0, damping=value1). */
        JPH::SixDOFConstraint::EAxis axis = static_cast<JPH::SixDOFConstraint::EAxis>(param - 12);
        const float stiffness = std::isfinite(value0) ? std::max(value0, 0.0f) : 0.0f;
        if (stiffness > 0.0f) {
          JPH::MotorSettings &motor = sixdof->GetMotorSettings(axis);
          motor.mSpringSettings = JPH::SpringSettings(
              JPH::ESpringMode::StiffnessAndDamping,
              stiffness,
              std::isfinite(value1) ? std::max(value1, 0.0f) : 0.0f);
          sixdof->SetMotorState(axis, JPH::EMotorState::Position);
        }
        else {
          sixdof->SetMotorState(axis, JPH::EMotorState::Off);
        }
      }
      break;
    }
    case JPH::EConstraintSubType::Slider: {
      if (param == 0) {
        JPH::SliderConstraint *slider = static_cast<JPH::SliderConstraint *>(m_constraint);
        if (!std::isfinite(value0) || !std::isfinite(value1) || value0 > value1) {
          slider->SetLimits(-FLT_MAX, FLT_MAX);
        }
        else if (value0 < value1 && value0 <= 0.0f && value1 >= 0.0f) {
          slider->SetLimits(value0, value1);
        }
      }
      break;
    }
    case JPH::EConstraintSubType::Cone: {
      if (param >= 3 && param <= 5) {
        /* Cone twist limits: param 3=swing span1, 4=swing span2, 5=twist span. */
        JPH::ConeConstraint *cone = static_cast<JPH::ConeConstraint *>(m_constraint);
        /* Jolt ConeConstraint has a single half-cone angle. Use max of the spans. */
        if (param == 3 || param == 4) {
          cone->SetHalfConeAngle(std::max(std::abs(value0), std::abs(value1)));
        }
        /* Twist is not directly supported in ConeConstraint; would need SwingTwist. */
      }
      break;
    }
    case JPH::EConstraintSubType::Hinge: {
      if (param == 3) {
        /* Hinge limits: value0 = low limit, value1 = high limit.
         * Bullet convention: min > max means "free" (unconstrained).
         * Jolt HingeConstraint::SetLimits asserts min <= max, so we
         * must convert to full range [-π, π] for the "free" case. */
        JPH::HingeConstraint *hinge = static_cast<JPH::HingeConstraint *>(m_constraint);
        if (value0 > value1) {
          hinge->SetLimits(-JPH::JPH_PI, JPH::JPH_PI);
        }
        else {
          hinge->SetLimits(value0, value1);
        }
      }
      break;
    }
    default:
      break;
  }
}

float JoltConstraint::GetParam(int param)
{
  if (!m_constraint) {
    return 0.0f;
  }

  switch (m_constraint->GetSubType()) {
    case JPH::EConstraintSubType::SixDOF: {
      JPH::SixDOFConstraint *sixdof = static_cast<JPH::SixDOFConstraint *>(m_constraint);
      if (param >= 0 && param <= 2) {
        /* param 0-2: current translation limit values (return min). */
        return sixdof->GetLimitsMin(
            static_cast<JPH::SixDOFConstraint::EAxis>(param));
      }
      else if (param >= 3 && param <= 5) {
        /* param 3-5: current rotation limit values (return min). */
        return sixdof->GetLimitsMin(
            static_cast<JPH::SixDOFConstraint::EAxis>(param));
      }
      break;
    }
    case JPH::EConstraintSubType::Slider: {
      if (param == 0) {
        return static_cast<JPH::SliderConstraint *>(m_constraint)->GetLimitsMin();
      }
      break;
    }
    default:
      break;
  }
  return 0.0f;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name PHY_IConstraint — Breaking Threshold
 * \{ */

float JoltConstraint::GetBreakingThreshold() const
{
  return m_breakingThreshold;
}

void JoltConstraint::SetBreakingThreshold(float threshold)
{
  m_breakingThreshold = threshold;
  if (m_env) {
    m_env->NotifyConstraintBreakingThresholdChanged();
  }
}

bool JoltConstraint::CheckBreaking() const
{
  if (!m_constraint || m_breakingThreshold >= FLT_MAX || !m_constraint->GetEnabled() ||
      !m_constraint->IsActive()) {
    return false;
  }

  return GetAppliedImpulse() > m_breakingThreshold;
}

float JoltConstraint::GetAppliedImpulse() const
{
  if (!m_constraint || !m_constraint->GetEnabled() || !m_constraint->IsActive()) {
    return 0.0f;
  }

  switch (m_constraint->GetSubType()) {
    case JPH::EConstraintSubType::Point: {
      JPH::PointConstraint *pc = static_cast<JPH::PointConstraint *>(m_constraint);
      return pc->GetTotalLambdaPosition().Length();
    }
    case JPH::EConstraintSubType::Hinge: {
      JPH::HingeConstraint *hc = static_cast<JPH::HingeConstraint *>(m_constraint);
      return hc->GetTotalLambdaPosition().Length() +
             hc->GetTotalLambdaRotation().Length() +
             std::abs(hc->GetTotalLambdaRotationLimits()) +
             std::abs(hc->GetTotalLambdaMotor());
    }
    case JPH::EConstraintSubType::Cone: {
      JPH::ConeConstraint *cone = static_cast<JPH::ConeConstraint *>(m_constraint);
      return cone->GetTotalLambdaPosition().Length() +
             std::abs(cone->GetTotalLambdaRotation());
    }
    case JPH::EConstraintSubType::SixDOF: {
      JPH::SixDOFConstraint *sixdof = static_cast<JPH::SixDOFConstraint *>(m_constraint);
      return sixdof->GetTotalLambdaPosition().Length() +
             sixdof->GetTotalLambdaRotation().Length() +
             sixdof->GetTotalLambdaMotorTranslation().Length() +
             sixdof->GetTotalLambdaMotorRotation().Length();
    }
    case JPH::EConstraintSubType::Fixed: {
      JPH::FixedConstraint *fixed = static_cast<JPH::FixedConstraint *>(m_constraint);
      return fixed->GetTotalLambdaPosition().Length() +
             fixed->GetTotalLambdaRotation().Length();
    }
    case JPH::EConstraintSubType::Slider: {
      JPH::SliderConstraint *slider = static_cast<JPH::SliderConstraint *>(m_constraint);
      return slider->GetTotalLambdaPosition().Length() +
             std::abs(slider->GetTotalLambdaPositionLimits()) +
             slider->GetTotalLambdaRotation().Length() +
             std::abs(slider->GetTotalLambdaMotor());
    }
    default: {
      return 0.0f;
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name PHY_IConstraint — Solver / Identity
 * \{ */

void JoltConstraint::SetSolverIterations(int iterations)
{
  SetSolverIterations(iterations, iterations);
}

void JoltConstraint::SetSolverIterations(int velocityIterations, int positionIterations)
{
  if (m_constraint) {
    m_constraint->SetNumVelocityStepsOverride(
        (unsigned int)std::clamp(velocityIterations, 0, 255));
    m_constraint->SetNumPositionStepsOverride(
        (unsigned int)std::clamp(positionIterations, 0, 255));
  }
}

int JoltConstraint::GetIdentifier() const
{
  return m_id;
}

PHY_ConstraintType JoltConstraint::GetType() const
{
  return m_type;
}

/** \} */
