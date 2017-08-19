
/** \file PHY_IVehicle.h
 *  \ingroup phys
 */

#ifndef __PHY_IVEHICLE_H__
#define __PHY_IVEHICLE_H__

//PHY_IVehicle provides a generic interface for (raycast based) vehicles. Mostly targetting 4 wheel cars and 2 wheel motorbikes.

#include "PHY_DynamicTypes.h"

class PHY_IMotionState;

class PHY_IVehicle
{
public:
	virtual ~PHY_IVehicle()
	{
	};

	virtual void AddWheel(
	    PHY_IMotionState *motionState,
	    const mt::vec3 &connectionPoint,
	    const mt::vec3 &downDirection,
	    const mt::vec3 &axleDirection,
	    float suspensionRestLength,
	    float wheelRadius,
	    bool hasSteering) = 0;

	virtual int GetNumWheels() const = 0;

	virtual mt::vec3 GetWheelPosition(int wheelIndex) const = 0;
	virtual mt::quat GetWheelOrientationQuaternion(int wheelIndex) const = 0;
	virtual float GetWheelRotation(int wheelIndex) const = 0;

	virtual int GetUserConstraintId() const = 0;
	virtual int GetUserConstraintType() const = 0;

	// some basic steering/braking/tuning/balancing (bikes)

	virtual void SetSteeringValue(float steering, int wheelIndex) = 0;

	virtual void ApplyEngineForce(float force, int wheelIndex) = 0;

	virtual void ApplyBraking(float braking, int wheelIndex) = 0;

	virtual void SetWheelFriction(float friction, int wheelIndex) = 0;

	virtual void SetSuspensionStiffness(float suspensionStiffness, int wheelIndex) = 0;

	virtual void SetSuspensionDamping(float suspensionStiffness, int wheelIndex) = 0;

	virtual void SetSuspensionCompression(float suspensionStiffness, int wheelIndex) = 0;

	virtual void SetRollInfluence(float rollInfluence, int wheelIndex) = 0;

	virtual void SetCoordinateSystem(int rightIndex, int upIndex, int forwardIndex) = 0;

	virtual void SetRayCastMask(short mask) = 0;
	virtual short GetRayCastMask() const = 0;
};

#endif  /* __PHY_IVEHICLE_H__ */
