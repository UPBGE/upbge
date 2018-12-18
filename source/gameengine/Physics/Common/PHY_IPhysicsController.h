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

#ifndef __PHY_IPHYSICSCONTROLLER_H__
#define __PHY_IPHYSICSCONTROLLER_H__

#include <vector>

#include "PHY_IController.h"

class PHY_IMotionState;
class PHY_IPhysicsEnvironment;

class KX_GameObject;
class RAS_Mesh;

/**
 * PHY_IPhysicsController is the abstract simplified Interface to a physical object.
 * It contains the IMotionState and IDeformableMesh Interfaces.
 */
class PHY_IPhysicsController : public PHY_IController
{
public:
	virtual ~PHY_IPhysicsController()
	{
	}
	/**
	 * SynchronizeMotionStates ynchronizes dynas, kinematic and deformable entities (and do 'late binding')
	 */
	virtual bool SynchronizeMotionStates(float time) = 0;
	/**
	 * WriteMotionStateToDynamics ynchronizes dynas, kinematic and deformable entities (and do 'late binding')
	 */

	virtual void WriteMotionStateToDynamics(bool nondynaonly) = 0;
	virtual void WriteDynamicsToMotionState() = 0;
	virtual class PHY_IMotionState *GetMotionState() = 0;
	// controller replication
	virtual void PostProcessReplica(class PHY_IMotionState *motionstate, class PHY_IPhysicsController *parentctrl) = 0;
	virtual void SetPhysicsEnvironment(class PHY_IPhysicsEnvironment *env) = 0;

	// kinematic methods
	virtual void RelativeTranslate(const mt::vec3& dloc, bool local) = 0;
	virtual void RelativeRotate(const mt::mat3 &, bool local) = 0;
	virtual mt::mat3 GetOrientation() = 0;
	virtual void SetOrientation(const mt::mat3& orn) = 0;
	virtual void SetPosition(const mt::vec3& pos) = 0;
	virtual mt::vec3 GetPosition() const = 0;
	virtual void SetScaling(const mt::vec3& scale) = 0;
	virtual void SetTransform() = 0;

	virtual float GetMass() = 0;
	virtual void SetMass(float newmass) = 0;

	// physics methods
	virtual void ApplyImpulse(const mt::vec3& attach, const mt::vec3& impulse, bool local) = 0;
	virtual void ApplyTorque(const mt::vec3& torque, bool local) = 0;
	virtual void ApplyForce(const mt::vec3& force, bool local) = 0;
	virtual void SetAngularVelocity(const mt::vec3& ang_vel, bool local) = 0;
	virtual void SetLinearVelocity(const mt::vec3& lin_vel, bool local) = 0;

	virtual float GetLinearDamping() const = 0;
	virtual float GetAngularDamping() const = 0;
	virtual void SetLinearDamping(float damping) = 0;
	virtual void SetAngularDamping(float damping) = 0;
	virtual void SetDamping(float linear, float angular) = 0;
	virtual void SetGravity(const mt::vec3 &gravity) = 0;

	virtual void RefreshCollisions() = 0;
	virtual void SuspendPhysics(bool freeConstraints) = 0;
	virtual void RestorePhysics() = 0;
	virtual void SuspendDynamics(bool ghost = false) = 0;
	virtual void RestoreDynamics() = 0;

	virtual void SetActive(bool active) = 0;

	virtual unsigned short GetCollisionGroup() const = 0;
	virtual unsigned short GetCollisionMask() const = 0;
	virtual void SetCollisionGroup(unsigned short group) = 0;
	virtual void SetCollisionMask(unsigned short mask) = 0;

	// reading out information from physics
	virtual mt::vec3 GetLinearVelocity() = 0;
	virtual mt::vec3 GetAngularVelocity() = 0;
	virtual mt::vec3 GetVelocity(const mt::vec3& pos) = 0;
	virtual mt::vec3 GetLocalInertia() = 0;
	virtual mt::vec3 GetGravity() = 0;

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

	virtual bool IsDynamic() = 0;
	virtual bool IsCompound() = 0;
	virtual bool IsDynamicsSuspended() const = 0;
	virtual bool IsPhysicsSuspended() = 0;

	virtual bool ReinstancePhysicsShape(KX_GameObject *from_gameobj, RAS_Mesh *from_meshobj, bool dupli = false) = 0;
	virtual bool ReplacePhysicsShape(PHY_IPhysicsController *phyctrl) = 0;
};

#endif  /* __PHY_IPHYSICSCONTROLLER_H__ */
