#ifndef __CCD_CONSTRAINT_H__
#define __CCD_CONSTRAINT_H__

#include "PHY_IConstraint.h"

class btTypedConstraint;

class CcdConstraint : public PHY_IConstraint
{
private:
	btTypedConstraint *m_constraint;

	/// Disable collision between constrained objects?
	bool m_disableCollision;
	/// The constraint is added in dynamic world?
	bool m_active;

public:
	CcdConstraint(btTypedConstraint *constraint, bool disableCollision);
	virtual ~CcdConstraint();

	bool GetDisableCollision() const;
	bool GetActive() const;
	void SetActive(bool active);

	virtual bool GetEnabled() const;
	virtual void SetEnabled(bool enabled);

	virtual void SetParam(int param, float value0, float value1);
	virtual float GetParam(int param);

	virtual float GetBreakingThreshold() const;
	virtual void SetBreakingThreshold(float threshold);

	virtual int GetIdentifier() const;
	virtual PHY_ConstraintType GetType() const;
};

#endif  // __CCD_CONSTRAINT_H__
