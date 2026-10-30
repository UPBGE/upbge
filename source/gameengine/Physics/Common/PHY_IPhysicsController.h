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
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file PHY_IPhysicsController.h
 *  \ingroup phys
 */

#pragma once

#include "PHY_IController.h"

#include <array>
#include <cstdint>
#include <vector>

#include "DNA_object_types.h"
#include "MT_Vector3.h"

class PHY_IMotionState;
class PHY_IPhysicsEnvironment;

class MT_Matrix3x3;

class KX_GameObject;
class RAS_MeshObject;

enum class PHY_DynamicsMode {
  Dynamic = 0,
  StaticCollider = 1,
  Ghost = 2,
  NoCollision = 3,
};

/**
 * Explicit runtime physics types offered by the Set Physics State Logic Node.
 *
 * These are deliberately separate from #PHY_DynamicsMode. The latter is the
 * legacy, reversible collision-state overlay (Static, Ghost and No
 * Collision), while these requests change which system owns motion.
 */
enum class PHY_RuntimePhysicsType : uint8_t {
  Dynamic = 0,
  RigidBody,
  Kinematic,
};

/**
 * Base motion ownership stored by a checkpoint. `Authored` means that the
 * body should use the type configured in the blend file; the other values are
 * explicit runtime overrides.
 */
enum class PHY_RuntimeMotionType : uint8_t {
  Authored = 0,
  Dynamic,
  Kinematic,
};

/**
 * Fixed-size, backend-neutral rigid-body state used by game-state checkpoints.
 *
 * Vectors use UPBGE world coordinates. `ghost` is the independent runtime sensor flag; it is
 * intentionally separate from the suspended `Ghost` dynamics mode. The six axis locks are
 * Blender translation X/Y/Z followed by rotation X/Y/Z.
 */
struct PHY_GameStateRigidBodyState {
  bool dynamic_body = false;
  PHY_DynamicsMode mode = PHY_DynamicsMode::Dynamic;
  PHY_RuntimeMotionType runtime_motion_type = PHY_RuntimeMotionType::Authored;
  bool ghost = false;
  bool physics_suspended = false;
  bool awake = false;
  MT_Vector3 linear_velocity = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 angular_velocity = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 pending_force = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 pending_torque = MT_Vector3(0.0f, 0.0f, 0.0f);
  float mass = 0.0f;
  float friction = 0.5f;
  float restitution = 0.0f;
  float linear_damping = 0.0f;
  float angular_damping = 0.0f;
  float linear_velocity_min = -1.0f;
  float linear_velocity_max = -1.0f;
  float angular_velocity_min = 0.0f;
  float angular_velocity_max = 0.0f;
  float gravity_factor = 1.0f;
  bool allow_sleeping = true;
  bool rotation_enabled = false;
  /** Effective CCD state for the current motion type. Always false for Kinematic. */
  bool ccd_enabled = false;
  /** CCD preference to apply if this body is or later becomes Dynamic. */
  bool dynamic_ccd_enabled = false;
  std::array<bool, 6> axis_locks = {false, false, false, false, false, false};
  uint16_t collision_group = 0;
  uint16_t collision_mask = 0;
};

/**
 * PHY_IPhysicsController is the abstract simplified Interface to a physical object.
 * It contains the IMotionState and IDeformableMesh Interfaces.
 */
class PHY_IPhysicsController : public PHY_IController {
 public:
  virtual ~PHY_IPhysicsController() {}
  /**
   * SynchronizeMotionStates ynchronizes dynas, kinematic and deformable entities (and do 'late
   * binding')
   */
  virtual bool SynchronizeMotionStates(float time) = 0;

  virtual void UpdateSoftBody() = 0;
  virtual void SetSoftBodyTransform(const MT_Vector3 &pos, const MT_Matrix3x3 &ori) = 0;
  virtual bool IsSoftBody() const
  {
    return false;
  }
  /** Checkpoint support is intentionally opt-in; non-Jolt backends keep these defaults. */
  virtual bool SupportsGameStatePersistence() const
  {
    return false;
  }
  virtual bool HasValidGameStateBody() const
  {
    return false;
  }
  /** Allocate/build a replacement rigid shape before the checkpoint commit boundary. */
  virtual bool PrepareGameStateScaling(const MT_Vector3 & /*target_scale*/,
                                       const MT_Vector3 & /*current_scale*/)
  {
    return false;
  }
  /** Install a shape prepared above. This is called only after a successful full preflight. */
  virtual void CommitPreparedGameStateScaling() {}
  virtual void ClearPreparedGameStateScaling() {}
  /**
   * Capture semantic rigid-body state. Backends must read latent motion properties when a dynamic
   * body is temporarily static or absent from the broadphase.
   */
  virtual bool CaptureGameStateRigidBody(PHY_GameStateRigidBodyState & /*r_state*/) const
  {
    return false;
  }
  /**
   * Validate and stage a rigid-body restore without mutating the live body. Commit is called
   * physics-last, after hierarchy, transforms and any prepared shape change.
   */
  virtual bool PrepareGameStateRigidBody(const PHY_GameStateRigidBodyState & /*state*/)
  {
    return false;
  }
  virtual void CommitPreparedGameStateRigidBody() {}
  virtual void ClearPreparedGameStateRigidBody() {}
  virtual void RemoveSoftBodyModifier(blender::Object *ob) = 0;
  /**
   * WriteMotionStateToDynamics ynchronizes dynas, kinematic and deformable entities (and do 'late
   * binding')
   */

  /**
   * Synchronize the game transform before a physics step. `timeStep` is zero
   * for out-of-band callers that do not own a simulation step.
   */
  virtual void WriteMotionStateToDynamics(bool nondynaonly, float timeStep = 0.0f) = 0;
  virtual void WriteDynamicsToMotionState() = 0;
  virtual class PHY_IMotionState *GetMotionState() = 0;
  // controller replication
  virtual void PostProcessReplica(class PHY_IMotionState *motionstate,
                                  class PHY_IPhysicsController *parentctrl) = 0;
  virtual void SetPhysicsEnvironment(class PHY_IPhysicsEnvironment *env) = 0;

  // kinematic methods
  virtual void RelativeTranslate(const MT_Vector3 &dloc, bool local) = 0;
  virtual void RelativeRotate(const MT_Matrix3x3 &, bool local) = 0;
  virtual MT_Matrix3x3 GetOrientation() = 0;
  virtual void SetOrientation(const MT_Matrix3x3 &orn) = 0;
  virtual void SetPosition(const MT_Vector3 &pos) = 0;
  virtual void GetPosition(MT_Vector3 &pos) const = 0;
  virtual void SetScaling(const MT_Vector3 &scale) = 0;
  virtual void SetTransform() = 0;

  virtual MT_Scalar GetMass() = 0;
  virtual void SetMass(MT_Scalar newmass) = 0;

  virtual MT_Scalar GetFriction() = 0;
  virtual void SetFriction(MT_Scalar newfriction) = 0;

  virtual MT_Scalar GetRestitution()
  {
    return 0.0f;
  }
  virtual void SetRestitution(MT_Scalar /*restitution*/) {}
  virtual float GetGravityFactor() const
  {
    return 1.0f;
  }
  virtual void SetGravityFactor(float /*factor*/) {}
  virtual bool GetAllowSleeping() const
  {
    return true;
  }
  virtual void SetAllowSleeping(bool /*allow*/) {}
  virtual void SetRigidBodyAxisLocks(bool /*lockTranslationX*/,
                                     bool /*lockTranslationY*/,
                                     bool /*lockTranslationZ*/,
                                     bool /*lockRotationX*/,
                                     bool /*lockRotationY*/,
                                     bool /*lockRotationZ*/)
  {
  }
  virtual void GetRigidBodyAxisLocks(bool &lockTranslationX,
                                     bool &lockTranslationY,
                                     bool &lockTranslationZ,
                                     bool &lockRotationX,
                                     bool &lockRotationY,
                                     bool &lockRotationZ) const
  {
    lockTranslationX = false;
    lockTranslationY = false;
    lockTranslationZ = false;
    lockRotationX = false;
    lockRotationY = false;
    lockRotationZ = false;
  }
  virtual bool GetRigidBodyRotationEnabled() const
  {
    return false;
  }
  virtual bool GetCcdEnabled() const
  {
    return false;
  }

  // physics methods
  virtual void ApplyImpulse(const MT_Vector3 &attach, const MT_Vector3 &impulse, bool local) = 0;
  virtual void ApplyTorque(const MT_Vector3 &torque, bool local) = 0;
  virtual void ApplyForce(const MT_Vector3 &force, bool local) = 0;
  virtual void SetAngularVelocity(const MT_Vector3 &ang_vel, bool local) = 0;
  virtual void SetLinearVelocity(const MT_Vector3 &lin_vel, bool local) = 0;

  virtual float GetLinearDamping() const = 0;
  virtual float GetAngularDamping() const = 0;
  virtual void SetLinearDamping(float damping) = 0;
  virtual void SetAngularDamping(float damping) = 0;
  virtual void SetDamping(float linear, float angular) = 0;
  virtual void SetGravity(const MT_Vector3 &gravity) = 0;

  virtual void RefreshCollisions() = 0;
  virtual void SuspendPhysics(bool freeConstraints) = 0;
  virtual void RestorePhysics() = 0;
  virtual void SuspendDynamics(bool ghost = false) = 0;
  virtual void RestoreDynamics() = 0;
  virtual bool SupportsTransformParenting() const
  {
    return false;
  }
  virtual bool BeginTransformParenting()
  {
    return false;
  }
  /** Enter the collision-disabled child state required while this shape is owned by a compound. */
  virtual bool SupportsCompoundParenting() const
  {
    return false;
  }
  virtual bool BeginCompoundParenting()
  {
    return false;
  }
  virtual void EndTransformParenting() {}
  virtual bool IsTransformParentingActive() const
  {
    return false;
  }
  /**
   * Return whether a reversible collision-state request is coherent for this body.
   * Backends may reject combinations such as disabling Ghost on a dedicated Sensor type.
   */
  virtual bool CanSetDynamicsMode(PHY_DynamicsMode /*mode*/, bool /*enabled*/) const
  {
    return true;
  }
  virtual void SetDynamicsMode(PHY_DynamicsMode mode, bool enabled = true)
  {
    switch (mode) {
      case PHY_DynamicsMode::Dynamic:
        if (enabled) {
          RestorePhysics();
          RestoreDynamics();
        }
        else {
          SuspendDynamics(false);
        }
        break;
      case PHY_DynamicsMode::StaticCollider:
        SuspendDynamics(false);
        break;
      case PHY_DynamicsMode::Ghost:
        if (enabled) {
          SuspendDynamics(true);
        }
        else {
          RestoreDynamics();
        }
        break;
      case PHY_DynamicsMode::NoCollision:
        SuspendDynamics(false);
        break;
    }
  }

  /**
   * Change motion ownership at runtime. Backends return false when the body
   * was not created with the capabilities needed for a safe transition.
   */
  virtual bool SetRuntimePhysicsType(PHY_RuntimePhysicsType /*type*/)
  {
    return false;
  }

  /** Restore the exact game-start physics type configured for this body. */
  virtual bool RestoreOriginalPhysicsType()
  {
    return false;
  }

  virtual void SetActive(bool active) = 0;

  virtual unsigned short GetCollisionGroup() const = 0;
  virtual unsigned short GetCollisionMask() const = 0;
  virtual void SetCollisionGroup(unsigned short group) = 0;
  virtual void SetCollisionMask(unsigned short mask) = 0;

  // reading out information from physics
  virtual MT_Vector3 GetLinearVelocity() = 0;
  virtual MT_Vector3 GetAngularVelocity() = 0;
  virtual MT_Vector3 GetVelocity(const MT_Vector3 &pos) = 0;
  virtual MT_Vector3 GetLocalInertia() = 0;
  virtual MT_Vector3 GetGravity() = 0;

  // dyna's that are rigidbody are free in orientation, dyna's with non-rigidbody are restricted
  virtual void SetRigidBody(bool rigid) = 0;

  virtual PHY_IPhysicsController *GetReplica()
  {
    return nullptr;
  }
  virtual PHY_IPhysicsController *GetReplicaForSensors()
  {
    return nullptr;
  }

  virtual void SetMargin(float margin) = 0;
  virtual float GetMargin() const = 0;
  virtual float GetRadius() const = 0;
  virtual void SetRadius(float margin) = 0;

  virtual float GetLinVelocityMin() const = 0;
  virtual void SetLinVelocityMin(float val) = 0;
  virtual float GetLinVelocityMax() const = 0;
  virtual void SetLinVelocityMax(float val) = 0;

  virtual void SetAngularVelocityMin(float val) = 0;
  virtual float GetAngularVelocityMin() const = 0;
  virtual void SetAngularVelocityMax(float val) = 0;
  virtual float GetAngularVelocityMax() const = 0;

  // Shape control
  virtual void AddCompoundChild(PHY_IPhysicsController *child) = 0;
  virtual void RemoveCompoundChild(PHY_IPhysicsController *child) = 0;
  /** Checked compound mutation used by transactional game-engine parenting. */
  virtual bool TryAddCompoundChild(PHY_IPhysicsController * /*child*/)
  {
    return false;
  }

  virtual bool IsDynamic() = 0;
  /** Authored transform-driven body. Backends without this motion type return false. */
  virtual bool IsKinematic() const
  {
    return false;
  }
  /**
   * Teleport a transform-driven body to its current motion-state pose and clear
   * any derived velocity.
   *
   * This is a lifecycle operation for spawning, restoration, and resuming a
   * paused simulation. Normal gameplay transforms must not call it: they need
   * the backend's velocity-producing kinematic target path instead.
   */
  virtual void RebaseKinematicMotionState() {}
  virtual bool IsCompound() = 0;
  virtual bool IsDynamicsSuspended() const = 0;
  virtual bool IsPhysicsSuspended() = 0;

  virtual bool ReinstancePhysicsShape(KX_GameObject *from_gameobj,
                                      RAS_MeshObject *from_meshobj,
                                      bool dupli = false,
                                      bool evaluatedMesh = false) = 0;

  virtual bool ReplacePhysicsShape(PHY_IPhysicsController *phyctrl) = 0;

  /* Method to replicate rigid body joint contraints for group instances. */
  virtual void ReplicateConstraints(KX_GameObject *gameobj,
                                    std::vector<KX_GameObject *> constobj) = 0;

  // CCD methods
  virtual void SetCcdMotionThreshold(float val) = 0;
  virtual void SetCcdSweptSphereRadius(float val) = 0;
};
