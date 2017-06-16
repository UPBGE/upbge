#include "CcdConstraint.h"

#include "btBulletDynamicsCommon.h"

CcdConstraint::CcdConstraint(btTypedConstraint *constraint, bool disableCollision)
	:m_constraint(constraint),
	m_disableCollision(disableCollision),
	m_enabled(true)
{
}

CcdConstraint::~CcdConstraint()
{
}

bool CcdConstraint::GetDisableCollision() const
{
	return m_disableCollision;
}

bool CcdConstraint::GetEnabled() const
{
	return m_enabled;
}

void CcdConstraint::SetEnabled(bool enabled)
{
	m_enabled = enabled;
}

void CcdConstraint::SetParam(int param, float value0, float value1)
{
	if (!m_constraint)
		return;

	switch (m_constraint->getUserConstraintType())
	{
		case PHY_GENERIC_6DOF_CONSTRAINT:
		{
			switch (param)
			{
				case 0: case 1: case 2: case 3: case 4: case 5:
				{
					//param = 0..5 are constraint limits, with low/high limit value
					btGeneric6DofConstraint *genCons = (btGeneric6DofConstraint *)m_constraint;
					genCons->setLimit(param, value0, value1);
					break;
				}
				case 6: case 7: case 8:
				{
					//param = 6,7,8 are translational motors, with value0=target velocity, value1 = max motor force
					btGeneric6DofConstraint *genCons = (btGeneric6DofConstraint *)m_constraint;
					int transMotorIndex = param - 6;
					btTranslationalLimitMotor *transMotor = genCons->getTranslationalLimitMotor();
					transMotor->m_targetVelocity[transMotorIndex] = value0;
					transMotor->m_maxMotorForce[transMotorIndex] = value1;
					transMotor->m_enableMotor[transMotorIndex] = (value1 > 0.0f);
					break;
				}
				case 9: case 10: case 11:
				{
					//param = 9,10,11 are rotational motors, with value0=target velocity, value1 = max motor force
					btGeneric6DofConstraint *genCons = (btGeneric6DofConstraint *)m_constraint;
					int angMotorIndex = param - 9;
					btRotationalLimitMotor *rotMotor = genCons->getRotationalLimitMotor(angMotorIndex);
					rotMotor->m_enableMotor = (value1 > 0.0f);
					rotMotor->m_targetVelocity = value0;
					rotMotor->m_maxMotorForce = value1;
					break;
				}

				case 12: case 13: case 14: case 15: case 16: case 17:
				{
					//param 12-17 are for motorized springs on each of the degrees of freedom
					btGeneric6DofSpringConstraint *genCons = (btGeneric6DofSpringConstraint *)m_constraint;
					int springIndex = param - 12;
					if (value0 != 0.0f) {
						bool springEnabled = true;
						genCons->setStiffness(springIndex, value0);
						genCons->setDamping(springIndex, value1);
						genCons->enableSpring(springIndex, springEnabled);
						genCons->setEquilibriumPoint(springIndex);
					}
					else {
						bool springEnabled = false;
						genCons->enableSpring(springIndex, springEnabled);
					}
					break;
				}

				default:
				{
				}
			};
			break;
		};
		case PHY_CONE_TWIST_CONSTRAINT:
		{
			switch (param)
			{
				case 3: case 4: case 5:
				{
					//param = 3,4,5 are constraint limits, high limit values
					btConeTwistConstraint *coneTwist = (btConeTwistConstraint *)m_constraint;
					if (value1 < 0.0f)
						coneTwist->setLimit(param, btScalar(BT_LARGE_FLOAT));
					else
						coneTwist->setLimit(param, value1);
					break;
				}
				default:
				{
				}
			};
			break;
		};
		case PHY_ANGULAR_CONSTRAINT:
		case PHY_LINEHINGE_CONSTRAINT:
		{
			switch (param)
			{
				case 3:
				{
					//param = 3 is a constraint limit, with low/high limit value
					btHingeConstraint *hingeCons = (btHingeConstraint *)m_constraint;
					hingeCons->setLimit(value0, value1);
					break;
				}
				default:
				{
				}
			}
			break;
		};
		default:
		{
		};
	};
}

float CcdConstraint::GetParam(int param)
{
	if (!m_constraint)
		return 0.0f;

	switch (m_constraint->getUserConstraintType())
	{
		case PHY_GENERIC_6DOF_CONSTRAINT:
		{
			switch (param)
			{
				case 0: case 1: case 2:
				{
					//param = 0..2 are linear constraint values
					btGeneric6DofConstraint *genCons = (btGeneric6DofConstraint *)m_constraint;
					genCons->calculateTransforms();
					return genCons->getRelativePivotPosition(param);
					break;
				}
				case 3: case 4: case 5:
				{
					//param = 3..5 are relative constraint (Euler) angles
					btGeneric6DofConstraint *genCons = (btGeneric6DofConstraint *)m_constraint;
					genCons->calculateTransforms();
					return genCons->getAngle(param - 3);
					break;
				}
				default:
				{
				}
			}
			break;
		};
		default:
		{
		};
	};
	return 0.0f;
}

int CcdConstraint::GetIdentifier() const
{
	return m_constraint->getUserConstraintId();
}

PHY_ConstraintType CcdConstraint::GetType() const
{
	return (PHY_ConstraintType)m_constraint->getUserConstraintType();
}
