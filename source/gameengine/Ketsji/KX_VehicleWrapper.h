
/** \file KX_VehicleWrapper.h
 *  \ingroup ketsji
 */

#ifndef __KX_VEHICLEWRAPPER_H__
#define __KX_VEHICLEWRAPPER_H__

#include "EXP_Value.h"
class PHY_IVehicle;

///Python interface to physics vehicles (primarily 4-wheel cars and 2wheel bikes)
class	KX_VehicleWrapper : public EXP_Value
{
	Py_Header

public:
	KX_VehicleWrapper(PHY_IVehicle* vehicle);
	virtual ~KX_VehicleWrapper ();

	virtual std::string GetName() const;

#ifdef WITH_PYTHON
	
	EXP_PYMETHOD_VARARGS(KX_VehicleWrapper,AddWheel);
	EXP_PYMETHOD_VARARGS(KX_VehicleWrapper,GetNumWheels);
	EXP_PYMETHOD_VARARGS(KX_VehicleWrapper,GetWheelOrientationQuaternion);
	EXP_PYMETHOD_VARARGS(KX_VehicleWrapper,GetWheelRotation);
	
	EXP_PYMETHOD_VARARGS(KX_VehicleWrapper,GetWheelPosition);
	
	EXP_PYMETHOD_VARARGS(KX_VehicleWrapper,GetConstraintId);
	EXP_PYMETHOD_VARARGS(KX_VehicleWrapper,GetConstraintType);

	EXP_PYMETHOD_VARARGS(KX_VehicleWrapper,SetSteeringValue);

	EXP_PYMETHOD_VARARGS(KX_VehicleWrapper,ApplyEngineForce);

	EXP_PYMETHOD_VARARGS(KX_VehicleWrapper,ApplyBraking);

	EXP_PYMETHOD_VARARGS(KX_VehicleWrapper,SetTyreFriction);

	EXP_PYMETHOD_VARARGS(KX_VehicleWrapper,SetSuspensionStiffness);
	
	EXP_PYMETHOD_VARARGS(KX_VehicleWrapper,SetSuspensionDamping);
	
	EXP_PYMETHOD_VARARGS(KX_VehicleWrapper,SetSuspensionCompression);
	
	EXP_PYMETHOD_VARARGS(KX_VehicleWrapper,SetRollInfluence);

	static PyObject *pyattr_get_ray_mask(EXP_PyObjectPlus *self, const struct EXP_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_ray_mask(EXP_PyObjectPlus *self, const struct EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_constraintId(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_constraintType(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);

#endif  /* WITH_PYTHON */

private:
	PHY_IVehicle*			 m_vehicle;
};

#endif  /* __KX_VEHICLEWRAPPER_H__ */
