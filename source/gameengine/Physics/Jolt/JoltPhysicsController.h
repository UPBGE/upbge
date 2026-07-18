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

/** \file JoltPhysicsController.h
 *  \ingroup physjolt
 *  \brief Jolt Physics backend implementing PHY_IPhysicsController.
 */

#pragma once

#include "JoltPhysicsConfig.h"
#include "JoltShapeQueryData.h"

#include <Jolt/Jolt.h>

JPH_SUPPRESS_WARNINGS

#include <Jolt/Physics/Body/AllowedDOFs.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Body/MassProperties.h>
#include <Jolt/Physics/Body/MotionType.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>

#include "PHY_IPhysicsController.h"
#include "JoltPhysicsEnvironment.h"

#include <cstdint>
#include <utility>
#include <vector>

class JoltSoftBody;
class PHY_IMotionState;

/**
 * JoltPhysicsController wraps a Jolt Body and implements the PHY_IPhysicsController interface.
 * Each game object with physics has one JoltPhysicsController.
 */
class JoltPhysicsController : public PHY_IPhysicsController {
 public:
  JoltPhysicsController();
  virtual ~JoltPhysicsController();

  JPH::BodyID GetBodyID() const
  {
    return m_bodyID;
  }
  void SetBodyID(JPH::BodyID bodyID)
  {
    m_bodyID = bodyID;
  }

  JoltPhysicsEnvironment *GetPhysicsEnvironment() const
  {
    return m_physicsEnv;
  }

  /* ---- PHY_IPhysicsController interface ---- */

  virtual bool SynchronizeMotionStates(float time) override;
  virtual void UpdateSoftBody() override;
  virtual void SetSoftBodyTransform(const MT_Vector3 &pos, const MT_Matrix3x3 &ori) override;
  virtual void RemoveSoftBodyModifier(blender::Object *ob) override;
  virtual void WriteMotionStateToDynamics(bool nondynaonly) override;
  virtual void WriteDynamicsToMotionState() override;
  virtual class PHY_IMotionState *GetMotionState() override;

  virtual void PostProcessReplica(class PHY_IMotionState *motionstate,
                                  class PHY_IPhysicsController *parentctrl) override;
  virtual void SetPhysicsEnvironment(class PHY_IPhysicsEnvironment *env) override;

  virtual void RelativeTranslate(const MT_Vector3 &dloc, bool local) override;
  virtual void RelativeRotate(const MT_Matrix3x3 &drot, bool local) override;
  virtual MT_Matrix3x3 GetOrientation() override;
  virtual void SetOrientation(const MT_Matrix3x3 &orn) override;
  virtual void SetPosition(const MT_Vector3 &pos) override;
  virtual void GetPosition(MT_Vector3 &pos) const override;
  virtual void SetScaling(const MT_Vector3 &scale) override;
  virtual void SetTransform() override;

  virtual MT_Scalar GetMass() override;
  virtual void SetMass(MT_Scalar newmass) override;
  virtual MT_Scalar GetFriction() override;
  virtual void SetFriction(MT_Scalar friction) override;
  virtual MT_Scalar GetRestitution() override;
  virtual void SetRestitution(MT_Scalar restitution) override;

  virtual void ApplyImpulse(const MT_Vector3 &attach, const MT_Vector3 &impulse, bool local) override;
  virtual void ApplyTorque(const MT_Vector3 &torque, bool local) override;
  virtual void ApplyForce(const MT_Vector3 &force, bool local) override;
  virtual void SetAngularVelocity(const MT_Vector3 &ang_vel, bool local) override;
  virtual void SetLinearVelocity(const MT_Vector3 &lin_vel, bool local) override;
  virtual MT_Vector3 GetLinearVelocity() override;
  virtual MT_Vector3 GetAngularVelocity() override;
  virtual MT_Vector3 GetVelocity(const MT_Vector3 &posin) override;
  virtual MT_Vector3 GetLocalInertia() override;

  virtual MT_Vector3 GetGravity() override;
  virtual void SetGravity(const MT_Vector3 &grav) override;
  virtual float GetGravityFactor() const override;
  virtual void SetGravityFactor(float factor) override;

  virtual float GetLinearDamping() const override;
  virtual float GetAngularDamping() const override;
  virtual void SetDamping(float linear, float angular) override;
  virtual void SetLinearDamping(float damping) override;
  virtual void SetAngularDamping(float damping) override;

  virtual void RefreshCollisions() override;
  virtual void SuspendPhysics(bool freeConstraints) override;
  virtual void RestorePhysics() override;
  virtual void SuspendDynamics(bool ghost) override;
  virtual void RestoreDynamics() override;
  virtual void SetDynamicsMode(PHY_DynamicsMode mode, bool enabled = true) override;

  virtual void SetActive(bool active) override;

  virtual unsigned short GetCollisionGroup() const override;
  virtual unsigned short GetCollisionMask() const override;
  virtual void SetCollisionGroup(unsigned short group) override;
  virtual void SetCollisionMask(unsigned short mask) override;

  virtual void SetRigidBody(bool rigid) override;
  virtual bool GetAllowSleeping() const override;
  virtual void SetAllowSleeping(bool allow) override;
  virtual void SetRigidBodyAxisLocks(bool lockTranslationX,
                                     bool lockTranslationY,
                                     bool lockTranslationZ,
                                     bool lockRotationX,
                                     bool lockRotationY,
                                     bool lockRotationZ) override;
  virtual void GetRigidBodyAxisLocks(bool &lockTranslationX,
                                     bool &lockTranslationY,
                                     bool &lockTranslationZ,
                                     bool &lockRotationX,
                                     bool &lockRotationY,
                                     bool &lockRotationZ) const override;
  virtual bool GetRigidBodyRotationEnabled() const override;
  virtual bool GetCcdEnabled() const override;
  virtual PHY_IPhysicsController *GetReplica() override;
  virtual PHY_IPhysicsController *GetReplicaForSensors() override;

  virtual float GetMargin() const override;
  virtual void SetMargin(float margin) override;
  virtual float GetRadius() const override;
  virtual void SetRadius(float margin) override;

  virtual float GetLinVelocityMin() const override;
  virtual void SetLinVelocityMin(float val) override;
  virtual float GetLinVelocityMax() const override;
  virtual void SetLinVelocityMax(float val) override;
  virtual float GetAngularVelocityMin() const override;
  virtual void SetAngularVelocityMin(float val) override;
  virtual float GetAngularVelocityMax() const override;
  virtual void SetAngularVelocityMax(float val) override;

  virtual void AddCompoundChild(PHY_IPhysicsController *child) override;
  virtual void RemoveCompoundChild(PHY_IPhysicsController *child) override;

  virtual bool IsDynamic() override;
  virtual bool IsCompound() override;
  virtual bool IsDynamicsSuspended() const override;
  virtual bool IsPhysicsSuspended() override;

  virtual bool ReinstancePhysicsShape(KX_GameObject *from_gameobj,
                                      RAS_MeshObject *from_meshobj,
                                      bool dupli = false,
                                      bool evaluatedMesh = false) override;
  virtual bool ReplacePhysicsShape(PHY_IPhysicsController *phyctrl) override;
  virtual void ReplicateConstraints(KX_GameObject *gameobj,
                                    std::vector<KX_GameObject *> constobj) override;

  virtual void SetCcdMotionThreshold(float val) override;
  virtual void SetCcdSweptSphereRadius(float val) override;

  virtual void *GetNewClientInfo() override;
  virtual void SetNewClientInfo(void *clientinfo) override;

  /** Called each physics tick to apply UPBGE minimum velocity clamps.
   * Maximum linear/angular velocity is handled natively by Jolt. */
  void SimulationTick(float timestep);

  /* ---- Jolt-specific helpers ---- */

  void SetMotionState(PHY_IMotionState *ms) { m_motionState = ms; }
  PHY_IMotionState *GetMotionState() const { return m_motionState; }
  void SetEnvironment(JoltPhysicsEnvironment *env) { m_physicsEnv = env; }
  void SetDynamic(bool dyna)
  {
    if (m_isDynamic != dyna) {
      m_isDynamic = dyna;
      NotifyFhSettingsChanged();
    }
  }
  void SetRigidBodyFlag(bool rigid) { m_isRigidBody = rigid; }
  void SetRigidBodyAxisLockState(bool lockTranslationX,
                                 bool lockTranslationY,
                                 bool lockTranslationZ,
                                 bool lockRotationX,
                                 bool lockRotationY,
                                 bool lockRotationZ);
  void SetMassPropertiesTemplate(const JPH::MassProperties &massProperties);
  void SetSensorFlag(bool sensor)
  {
    if (m_isSensor != sensor) {
      m_isSensor = sensor;
      NotifyBuoyancySettingsChanged();
    }
  }
  void SetBroadPhaseCategory(JoltBroadPhaseLayer cat) { m_bpCategory = cat; }
  JoltBroadPhaseLayer GetBroadPhaseCategory() const { return m_bpCategory; }
  void SetCompoundFlag(bool compound) { m_isCompound = compound; }
  void SetOriginalMotionType(JPH::EMotionType mt) { m_originalMotionType = mt; }
  JPH::EMotionType GetOriginalMotionType() const { return m_originalMotionType; }
  void SetShape(JPH::RefConst<JPH::Shape> shape, JoltShapeQueryDataPtr queryData)
  {
    m_shape = shape;
    m_shapeQueryData = std::move(queryData);
  }
  void SetShapePreservingMassPropertiesAndCenterOfMass(JPH::RefConst<JPH::Shape> shape,
                                                        JoltShapeQueryDataPtr queryData);
  JPH::RefConst<JPH::Shape> GetShape() const { return m_shape; }
  const JoltShapeQueryDataPtr &GetShapeQueryData() const { return m_shapeQueryData; }
  bool IsSensor() const { return m_isSensor; }
  bool IsRigidBodyFlag() const { return m_isRigidBody; }
  void SetLogicObjectSensorActive(bool active);
  bool IsLogicObjectSensorActive() const
  {
    return m_isSensor && m_logicObjectSensorActive && IsLogicCollisionQueryActive();
  }
  void SetLogicCollisionQueryActive(bool active);
  bool IsLogicCollisionQueryActive() const
  {
    return m_logicCollisionQueryActive && !m_physicsSuspended && !m_isCompoundChild &&
           (m_isSensor ||
            (m_parentedCollisionQuery && m_dynamicsSuspended &&
             m_dynamicsMode == PHY_DynamicsMode::NoCollision));
  }
  bool IsParentedCollisionQuery() const
  {
    return m_parentedCollisionQuery && IsLogicCollisionQueryActive();
  }
  void SetLogicObjectSensorIncludeStatic(bool includeStatic);
  bool GetLogicObjectSensorIncludeStatic() const
  {
    return m_logicObjectSensorIncludeStatic;
  }

  /** Store a stable Jolt compound-slot token for a logical child object. */
  uint32_t RegisterCompoundChildBinding(KX_GameObject *childObject,
                                        const JPH::Vec3 &relativePos,
                                        const JPH::Quat &relativeRot);
  void RemoveCompoundChildBinding(uint32_t userData);
  uint32_t FindCompoundChildBinding(KX_GameObject *childObject) const;
  KX_GameObject *ResolveCompoundChildObject(const JPH::SubShapeID &subShapeID) const;
  bool OwnsCompoundChildObject(KX_GameObject *childObject) const;
  void SetCompoundChildFlag(bool compoundChild);

  /* FH (Floating Height) spring parameters. */
  void SetFhEnabled(bool enabled)
  {
    if (m_fhEnabled != enabled) {
      m_fhEnabled = enabled;
      NotifyFhSettingsChanged();
    }
  }
  bool GetFhEnabled() const { return m_fhEnabled; }
  void SetFhRotEnabled(bool enabled)
  {
    if (m_fhRotEnabled != enabled) {
      m_fhRotEnabled = enabled;
    }
  }
  bool GetFhRotEnabled() const { return m_fhRotEnabled; }
  void SetFhSpring(float k)
  {
    if (m_fhSpring != k) {
      m_fhSpring = k;
      NotifyFhSettingsChanged();
    }
  }
  float GetFhSpring() const { return m_fhSpring; }
  void SetFhDamping(float d) { m_fhDamping = d; }
  float GetFhDamping() const { return m_fhDamping; }
  void SetFhDistance(float dist)
  {
    if (m_fhDistance != dist) {
      m_fhDistance = dist;
      NotifyFhSettingsChanged();
    }
  }
  float GetFhDistance() const { return m_fhDistance; }
  void SetFhNormal(bool n) { m_fhNormal = n; }
  bool GetFhNormal() const { return m_fhNormal; }
  bool UsesFhSpring() const
  {
    return m_isDynamic && !m_dynamicsSuspended && !m_physicsSuspended &&
           m_fhEnabled && m_fhSpring > 0.0f && m_fhDistance > 0.0f;
  }

  /* Jolt buoyancy volume parameters for Sensor physics type. */
  void SetBuoyancyVolumeEnabled(bool enabled)
  {
    if (m_buoyancyVolumeEnabled != enabled) {
      m_buoyancyVolumeEnabled = enabled;
      NotifyBuoyancySettingsChanged();
    }
  }
  bool GetBuoyancyVolumeEnabled() const { return m_buoyancyVolumeEnabled; }
  void SetBuoyancy(float buoyancy) { m_buoyancy = buoyancy; }
  float GetBuoyancy() const { return m_buoyancy; }
  void SetBuoyancyLinearDrag(float drag) { m_buoyancyLinearDrag = drag; }
  float GetBuoyancyLinearDrag() const { return m_buoyancyLinearDrag; }
  void SetBuoyancyAngularDrag(float drag) { m_buoyancyAngularDrag = drag; }
  float GetBuoyancyAngularDrag() const { return m_buoyancyAngularDrag; }
  void SetBuoyancyFluidVelocity(const JPH::Vec3 &velocity) { m_buoyancyFluidVelocity = velocity; }
  const JPH::Vec3 &GetBuoyancyFluidVelocity() const { return m_buoyancyFluidVelocity; }
  bool UsesBuoyancyVolume() const
  {
    return m_isSensor && m_buoyancyVolumeEnabled && IsLogicObjectSensorActive();
  }

  bool Register()
  {
    return m_registerCount++ == 0;
  }
  bool Unregister()
  {
    return --m_registerCount == 0;
  }
  bool Registered() const { return (m_registerCount != 0); }

  void SetSoftBody(JoltSoftBody *sb) { m_softBody = sb; }
  JoltSoftBody *GetSoftBody() const { return m_softBody; }

 private:
  JPH::MassProperties MassPropertiesForMass(float mass) const;
  JPH::EAllowedDOFs BuildAllowedDOFs() const;
  void ApplyAllowedDOFs();
  void ActivateBodyIfAdded();
  void NotifyFhSettingsChanged();
  void NotifyBuoyancySettingsChanged();
  void SetGhostFlag(bool enabled);
  void SetSuspendedDynamicsMode(PHY_DynamicsMode mode, bool ghost);

  JoltSoftBody *m_softBody = nullptr;
  JoltPhysicsEnvironment *m_physicsEnv = nullptr;  /* m_softBody declared above */

  /** Deferred soft body clone: set in PostProcessReplica, consumed in SetTransform()
   *  once UpdateWorldData() has given the motion state its correct world position. */
  JoltSoftBody *m_pendingSoftBodySource = nullptr;
  PHY_IMotionState *m_motionState = nullptr;
  void *m_newClientInfo = nullptr;
  JPH::BodyID m_bodyID;
  JPH::RefConst<JPH::Shape> m_shape;
  JoltShapeQueryDataPtr m_shapeQueryData;

  unsigned short m_collisionGroup = 0xFFFF;
  unsigned short m_collisionMask = 0xFFFF;

  float m_margin = 0.04f;
  float m_radius = 0.0f;
  float m_linVelMin = -1.0f;
  float m_linVelMax = -1.0f;
  float m_angVelMin = 0.0f;
  float m_angVelMax = 0.0f;
  JPH::MassProperties m_massPropertiesTemplate;
  bool m_hasMassPropertiesTemplate = false;

  /* FH spring parameters. */
  bool m_fhEnabled = false;
  bool m_fhRotEnabled = false;
  bool m_fhNormal = false;
  float m_fhSpring = 0.0f;
  float m_fhDamping = 0.0f;
  float m_fhDistance = 0.0f;

  bool m_buoyancyVolumeEnabled = false;
  float m_buoyancy = 2.0f;
  float m_buoyancyLinearDrag = 0.5f;
  float m_buoyancyAngularDrag = 0.01f;
  JPH::Vec3 m_buoyancyFluidVelocity = JPH::Vec3::sZero();

  bool m_isDynamic = false;
  bool m_isRigidBody = false;
  bool m_isSensor = false;
  /** The Sensor physics object belongs to the active scene rather than an inactive template. */
  bool m_logicObjectSensorActive = false;
  /** The object belongs to the active scene and may serve as a demand-driven LN query shape. */
  bool m_logicCollisionQueryActive = false;
  /** Whether C++ Logic Nodes Sensor queries may enter Jolt's static/sensor broadphase trees. */
  bool m_logicObjectSensorIncludeStatic = false;
  /** NoCollision suspension was caused by transform parenting, not an explicit gameplay pause. */
  bool m_parentedCollisionQuery = false;
  /** This controller's shape is represented inside a compound parent body. */
  bool m_isCompoundChild = false;
  bool m_isCompound = false;
  bool m_lockTranslationX = false;
  bool m_lockTranslationY = false;
  bool m_lockTranslationZ = false;
  bool m_lockRotationX = false;
  bool m_lockRotationY = false;
  bool m_lockRotationZ = false;
  JoltBroadPhaseLayer m_bpCategory = JOLT_BP_STATIC;
  bool m_dynamicsSuspended = false;
  bool m_physicsSuspended = false;
  PHY_DynamicsMode m_dynamicsMode = PHY_DynamicsMode::Dynamic;
  bool m_bodyRemovedOnSuspend = false;

  JPH::EMotionType m_originalMotionType = JPH::EMotionType::Static;

  /* Saved state for SuspendDynamics/RestoreDynamics. */
  JPH::EMotionType m_savedMotionType = JPH::EMotionType::Static;
  bool m_savedIsSensor = false;

  struct CompoundChildBinding {
    blender::Object *blenderObject = nullptr;
    KX_GameObject *sourceObject = nullptr;
    JPH::Vec3 relativePosition = JPH::Vec3::sZero();
    JPH::Quat relativeRotation = JPH::Quat::sIdentity();
    bool active = false;
  };
  KX_GameObject *ResolveCompoundChildBinding(const CompoundChildBinding &binding) const;
  /** Slot zero is reserved for the compound root; Jolt stores indices into this vector. */
  std::vector<CompoundChildBinding> m_compoundChildBindings;

  int m_registerCount = 0;
};
