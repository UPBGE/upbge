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

/** \file JoltConstraint.h
 *  \ingroup physjolt
 *  \brief Jolt Physics constraint wrapper implementing PHY_IConstraint.
 */

#pragma once

#include "JoltPhysicsConfig.h"

#include <Jolt/Jolt.h>

JPH_SUPPRESS_WARNINGS

#include <Jolt/Physics/Constraints/Constraint.h>

#include "PHY_IConstraint.h"

#include <string>

class JoltPhysicsEnvironment;
class KX_GameObject;
struct PHY_RigidBodyConstraintBreakEvent;

/**
 * JoltConstraint wraps a Jolt Constraint and implements the UPBGE PHY_IConstraint interface.
 *
 * Supports all UPBGE constraint types mapped to Jolt equivalents:
 *   - PHY_POINT2POINT_CONSTRAINT  → PointConstraint
 *   - PHY_LINEHINGE_CONSTRAINT    → HingeConstraint
 *   - PHY_ANGULAR_CONSTRAINT      → HingeConstraint (no position constraint)
 *   - PHY_CONE_TWIST_CONSTRAINT   → ConeConstraint / SwingTwistConstraint
 *   - PHY_GENERIC_6DOF_CONSTRAINT → SixDOFConstraint
 *   - PHY_GENERIC_6DOF_SPRING2_CONSTRAINT → SixDOFConstraint with motors/springs
 *   - PHY_FIXED_CONSTRAINT → FixedConstraint
 *   - PHY_SLIDER_CONSTRAINT → SliderConstraint
 */
class JoltConstraint : public PHY_IConstraint {
 public:
  JoltConstraint(JPH::Constraint *constraint,
                 PHY_ConstraintType type,
                 int id,
                 bool disableCollision,
                 JoltPhysicsEnvironment *env);
  virtual ~JoltConstraint();

  /* ---- PHY_IConstraint interface ---- */

  virtual bool GetEnabled() const override;
  virtual void SetEnabled(bool enabled) override;

  virtual void SetParam(int param, float value0, float value1) override;
  virtual float GetParam(int param) override;

  virtual float GetBreakingThreshold() const override;
  virtual void SetBreakingThreshold(float threshold) override;

  virtual void SetSolverIterations(int iterations) override;
  void SetSolverIterations(int velocityIterations, int positionIterations);

  virtual int GetIdentifier() const override;
  virtual PHY_ConstraintType GetType() const override;

  /* ---- Jolt-specific accessors ---- */

  JPH::Constraint *GetConstraint() { return m_constraint; }
  const JPH::Constraint *GetConstraint() const { return m_constraint; }

  bool GetDisableCollision() const { return m_disableCollision; }
  KX_GameObject *GetRigidBodyConstraintOwner() const { return m_rigidBodyConstraintOwner; }
  KX_GameObject *GetRigidBodyConstraintTarget() const { return m_rigidBodyConstraintTarget; }
  const std::string &GetRigidBodyConstraintName() const { return m_rigidBodyConstraintName; }
  /** Compatibility setter for legacy callers that only track the owner. */
  void SetRigidBodyConstraintOwner(KX_GameObject *owner) { m_rigidBodyConstraintOwner = owner; }
  void SetRigidBodyConstraintMetadata(KX_GameObject *owner,
                                      KX_GameObject *target,
                                      const std::string &name);
  bool GetActive() const { return m_active; }
  void SetActive(bool active) { m_active = active; }

  /** Check if constraint lambda exceeds breaking threshold. Called each frame. */
  bool CheckBreaking() const;

  /** Return the constraint impulse from Jolt's last solver update. */
  float GetAppliedImpulse() const;

  /** Build the automatic-break event while Jolt's constraint is still alive. */
  bool BuildRigidBodyConstraintBreakEvent(PHY_RigidBodyConstraintBreakEvent &r_event) const;

 private:
  bool GetImpulseComponents(float &r_linear_impulse,
                            float &r_angular_impulse,
                            float &r_applied_impulse) const;

  JPH::Constraint *m_constraint;
  JoltPhysicsEnvironment *m_env;
  KX_GameObject *m_rigidBodyConstraintOwner = nullptr;
  KX_GameObject *m_rigidBodyConstraintTarget = nullptr;
  std::string m_rigidBodyConstraintName;
  PHY_ConstraintType m_type;
  int m_id;
  bool m_disableCollision;
  bool m_active;
  float m_breakingThreshold;
};
