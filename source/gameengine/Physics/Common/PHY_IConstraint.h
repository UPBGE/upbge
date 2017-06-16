#ifndef __PHY_ICONSTRAINT_H__
#define __PHY_ICONSTRAINT_H__

#include "PHY_DynamicTypes.h" // For PHY_ConstraintType.

class PHY_IConstraint
{
public:
	PHY_IConstraint() = default;
	virtual ~PHY_IConstraint() = default;

	virtual void SetParam(int param, float value, float value1) = 0;
	virtual float GetParam(int param) = 0;

	virtual int GetIdentifier() const = 0;
	virtual PHY_ConstraintType GetType() const = 0;
};

#endif  // __PHY_ICONSTRAINT_H__
