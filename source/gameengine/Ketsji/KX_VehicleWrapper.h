
/** \file KX_VehicleWrapper.h
 *  \ingroup ketsji
 */

#ifndef __KX_VEHICLEWRAPPER_H__
#define __KX_VEHICLEWRAPPER_H__

#include "EXP_Value.h"
class PHY_IVehicle;

///Python interface to physics vehicles (primarily 4-wheel cars and 2wheel bikes)
class	KX_VehicleWrapper : public CValue
{
	Py_Header

public:
	KX_VehicleWrapper(PHY_IVehicle* vehicle);
	virtual ~KX_VehicleWrapper ();

	virtual std::string GetName();

#ifdef WITH_PYTHON
	
	KX_PYMETHOD_VARARGS(KX_VehicleWrapper,AddWheel);
	KX_PYMETHOD_VARARGS(KX_VehicleWrapper,GetNumWheels);
	KX_PYMETHOD_VARARGS(KX_VehicleWrapper,GetWheelOrientationQuaternion);
	KX_PYMETHOD_VARARGS(KX_VehicleWrapper,GetWheelRotation);
	
	KX_PYMETHOD_VARARGS(KX_VehicleWrapper,GetWheelPosition);
	
	KX_PYMETHOD_VARARGS(KX_VehicleWrapper,GetConstraintId);
	KX_PYMETHOD_VARARGS(KX_VehicleWrapper,GetConstraintType);

	KX_PYMETHOD_VARARGS(KX_VehicleWrapper,SetSteeringValue);

	KX_PYMETHOD_VARARGS(KX_VehicleWrapper,ApplyEngineForce);

	KX_PYMETHOD_VARARGS(KX_VehicleWrapper,ApplyBraking);

	KX_PYMETHOD_VARARGS(KX_VehicleWrapper,SetTyreFriction);

	KX_PYMETHOD_VARARGS(KX_VehicleWrapper,SetSuspensionStiffness);
	
	KX_PYMETHOD_VARARGS(KX_VehicleWrapper,SetSuspensionDamping);
	
	KX_PYMETHOD_VARARGS(KX_VehicleWrapper,SetSuspensionCompression);
	
	KX_PYMETHOD_VARARGS(KX_VehicleWrapper,SetRollInfluence);

	static PyObject *pyattr_get_ray_mask(PyObjectPlus *self, const struct KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_ray_mask(PyObjectPlus *self, const struct KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_constraintId(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_constraintType(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);

#endif  /* WITH_PYTHON */

private:
	PHY_IVehicle*			 m_vehicle;
};

#endif  /* __KX_VEHICLEWRAPPER_H__ */
