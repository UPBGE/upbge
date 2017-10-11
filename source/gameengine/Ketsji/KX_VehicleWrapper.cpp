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
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/Ketsji/KX_VehicleWrapper.cpp
 *  \ingroup ketsji
 */

#include "KX_VehicleWrapper.h"
#include "PHY_IVehicle.h"
#include "KX_PyMath.h"
#include "KX_GameObject.h"
#include "KX_MotionState.h"
#include "KX_Globals.h"

#include "DNA_object_types.h" // for OB_MAX_COL_MASKS

KX_VehicleWrapper::KX_VehicleWrapper(PHY_IVehicle *vehicle)
	:m_vehicle(vehicle)
{
}

KX_VehicleWrapper::~KX_VehicleWrapper()
{
}

std::string KX_VehicleWrapper::GetName() const
{
	return "KX_VehicleWrapper";
}

#ifdef WITH_PYTHON


static bool raise_exc_wheel(PHY_IVehicle *vehicle, int i, const char *method)
{
	if (i < 0 || i >= vehicle->GetNumWheels()) {
		PyErr_Format(PyExc_ValueError,
		             "%s(...): wheel index %d out of range (0 to %d).", method, i, vehicle->GetNumWheels() - 1);
		return true;
	}
	else {
		return false;
	}
}

#define WHEEL_INDEX_CHECK_OR_RETURN(i, method) \
	if (raise_exc_wheel(m_vehicle, i, method)) {return nullptr;} (void)0


PyObject *KX_VehicleWrapper::PyAddWheel(PyObject *args)
{

	PyObject *pylistPos, *pylistDir, *pylistAxleDir;
	PyObject *wheelGameObject;
	float suspensionRestLength, wheelRadius;
	int hasSteering;


	if (PyArg_ParseTuple(args, "OOOOffi:addWheel", &wheelGameObject, &pylistPos, &pylistDir, &pylistAxleDir, &suspensionRestLength, &wheelRadius, &hasSteering)) {
		KX_GameObject *gameOb;
		if (!ConvertPythonToGameObject(KX_GetActiveScene(), wheelGameObject, &gameOb, false, "vehicle.addWheel(...): KX_VehicleWrapper (first argument)")) {
			return nullptr;
		}

		if (gameOb->GetNode()) {
			mt::vec3 attachPos, attachDir, attachAxle;
			if (!PyVecTo(pylistPos, attachPos)) {
				PyErr_SetString(PyExc_AttributeError,
				                "addWheel(...) Unable to add wheel. attachPos must be a vector with 3 elements.");
				return nullptr;
			}
			if (!PyVecTo(pylistDir, attachDir)) {
				PyErr_SetString(PyExc_AttributeError,
				                "addWheel(...) Unable to add wheel. downDir must be a vector with 3 elements.");
				return nullptr;
			}
			if (!PyVecTo(pylistAxleDir, attachAxle)) {
				PyErr_SetString(PyExc_AttributeError,
				                "addWheel(...) Unable to add wheel. axleDir must be a vector with 3 elements.");
				return nullptr;
			}

			//someone reverse some conventions inside Bullet (axle winding)
			attachAxle = -attachAxle;

			if (wheelRadius <= 0) {
				PyErr_SetString(PyExc_AttributeError,
				                "addWheel(...) Unable to add wheel. wheelRadius must be positive.");
				return nullptr;
			}

			PHY_IMotionState *motionState = new KX_MotionState(gameOb->GetNode());
			m_vehicle->AddWheel(motionState, attachPos, attachDir, attachAxle, suspensionRestLength, wheelRadius, hasSteering);
		}

	}
	else {
		return nullptr;
	}
	Py_RETURN_NONE;
}


PyObject *KX_VehicleWrapper::PyGetWheelPosition(PyObject *args)
{

	int wheelIndex;

	if (PyArg_ParseTuple(args, "i:getWheelPosition", &wheelIndex)) {
		WHEEL_INDEX_CHECK_OR_RETURN(wheelIndex, "getWheelPosition");

		return PyObjectFrom(m_vehicle->GetWheelPosition(wheelIndex));
	}
	return nullptr;
}

PyObject *KX_VehicleWrapper::PyGetWheelRotation(PyObject *args)
{
	int wheelIndex;
	if (PyArg_ParseTuple(args, "i:getWheelRotation", &wheelIndex)) {
		WHEEL_INDEX_CHECK_OR_RETURN(wheelIndex, "getWheelRotation");

		return PyFloat_FromDouble(m_vehicle->GetWheelRotation(wheelIndex));
	}
	return nullptr;
}

PyObject *KX_VehicleWrapper::PyGetWheelOrientationQuaternion(PyObject *args)
{
	int wheelIndex;
	if (PyArg_ParseTuple(args, "i:getWheelOrientationQuaternion", &wheelIndex)) {
		WHEEL_INDEX_CHECK_OR_RETURN(wheelIndex, "getWheelOrientationQuaternion");

		const mt::quat quat = m_vehicle->GetWheelOrientationQuaternion(wheelIndex);
		const mt::mat3 ornmat = quat.ToMatrix();
		return PyObjectFrom(ornmat);
	}
	return nullptr;

}


PyObject *KX_VehicleWrapper::PyGetNumWheels(PyObject *args)
{
	return PyLong_FromLong(m_vehicle->GetNumWheels());
}


PyObject *KX_VehicleWrapper::PyGetConstraintId(PyObject *args)
{
	return PyLong_FromLong(m_vehicle->GetUserConstraintId());
}


PyObject *KX_VehicleWrapper::PyApplyEngineForce(PyObject *args)
{
	float force;
	int wheelIndex;

	if (PyArg_ParseTuple(args, "fi:applyEngineForce", &force, &wheelIndex)) {
		WHEEL_INDEX_CHECK_OR_RETURN(wheelIndex, "applyEngineForce");

		force *= -1.f;//someone reverse some conventions inside Bullet (axle winding)
		m_vehicle->ApplyEngineForce(force, wheelIndex);
	}
	else {
		return nullptr;
	}
	Py_RETURN_NONE;
}

PyObject *KX_VehicleWrapper::PySetTyreFriction(PyObject *args)
{
	float wheelFriction;
	int wheelIndex;

	if (PyArg_ParseTuple(args, "fi:setTyreFriction", &wheelFriction, &wheelIndex)) {
		WHEEL_INDEX_CHECK_OR_RETURN(wheelIndex, "setTyreFriction");

		m_vehicle->SetWheelFriction(wheelFriction, wheelIndex);
	}
	else {
		return nullptr;
	}
	Py_RETURN_NONE;
}

PyObject *KX_VehicleWrapper::PySetSuspensionStiffness(PyObject *args)
{
	float suspensionStiffness;
	int wheelIndex;

	if (PyArg_ParseTuple(args, "fi:setSuspensionStiffness", &suspensionStiffness, &wheelIndex)) {
		WHEEL_INDEX_CHECK_OR_RETURN(wheelIndex, "setSuspensionStiffness");

		m_vehicle->SetSuspensionStiffness(suspensionStiffness, wheelIndex);
	}
	else {
		return nullptr;
	}
	Py_RETURN_NONE;
}

PyObject *KX_VehicleWrapper::PySetSuspensionDamping(PyObject *args)
{
	float suspensionDamping;
	int wheelIndex;

	if (PyArg_ParseTuple(args, "fi:setSuspensionDamping", &suspensionDamping, &wheelIndex)) {
		WHEEL_INDEX_CHECK_OR_RETURN(wheelIndex, "setSuspensionDamping");

		m_vehicle->SetSuspensionDamping(suspensionDamping, wheelIndex);
	}
	else {
		return nullptr;
	}
	Py_RETURN_NONE;
}

PyObject *KX_VehicleWrapper::PySetSuspensionCompression(PyObject *args)
{
	float suspensionCompression;
	int wheelIndex;

	if (PyArg_ParseTuple(args, "fi:setSuspensionCompression", &suspensionCompression, &wheelIndex)) {
		WHEEL_INDEX_CHECK_OR_RETURN(wheelIndex, "setSuspensionCompression");

		m_vehicle->SetSuspensionCompression(suspensionCompression, wheelIndex);
	}
	else {
		return nullptr;
	}
	Py_RETURN_NONE;
}

PyObject *KX_VehicleWrapper::PySetRollInfluence(PyObject *args)
{
	float rollInfluence;
	int wheelIndex;

	if (PyArg_ParseTuple(args, "fi:setRollInfluence", &rollInfluence, &wheelIndex)) {
		WHEEL_INDEX_CHECK_OR_RETURN(wheelIndex, "setRollInfluence");

		m_vehicle->SetRollInfluence(rollInfluence, wheelIndex);
	}
	else {
		return nullptr;
	}
	Py_RETURN_NONE;
}


PyObject *KX_VehicleWrapper::PyApplyBraking(PyObject *args)
{
	float braking;
	int wheelIndex;

	if (PyArg_ParseTuple(args, "fi:applyBraking", &braking, &wheelIndex)) {
		WHEEL_INDEX_CHECK_OR_RETURN(wheelIndex, "applyBraking");

		m_vehicle->ApplyBraking(braking, wheelIndex);
	}
	else {
		return nullptr;
	}
	Py_RETURN_NONE;
}


PyObject *KX_VehicleWrapper::PySetSteeringValue(PyObject *args)
{
	float steeringValue;
	int wheelIndex;

	if (PyArg_ParseTuple(args, "fi:setSteeringValue", &steeringValue, &wheelIndex)) {
		WHEEL_INDEX_CHECK_OR_RETURN(wheelIndex, "setSteeringValue");

		m_vehicle->SetSteeringValue(steeringValue, wheelIndex);
	}
	else {
		return nullptr;
	}
	Py_RETURN_NONE;
}


PyObject *KX_VehicleWrapper::PyGetConstraintType(PyObject *args)
{
	return PyLong_FromLong(m_vehicle->GetUserConstraintType());
}





//python specific stuff
PyTypeObject KX_VehicleWrapper::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"KX_VehicleWrapper",
	sizeof(EXP_PyObjectPlus_Proxy),
	0,
	py_base_dealloc,
	0,
	0,
	0,
	0,
	py_base_repr,
	0, 0, 0, 0, 0, 0, 0, 0, 0,
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	0, 0, 0, 0, 0, 0, 0,
	Methods,
	0,
	0,
	&EXP_PyObjectPlus::Type,
	0, 0, 0, 0, 0, 0,
	py_base_new
};

PyMethodDef KX_VehicleWrapper::Methods[] = {
	{"addWheel", (PyCFunction)KX_VehicleWrapper::sPyAddWheel, METH_VARARGS},
	{"getNumWheels", (PyCFunction)KX_VehicleWrapper::sPyGetNumWheels, METH_VARARGS},
	{"getWheelOrientationQuaternion", (PyCFunction)KX_VehicleWrapper::sPyGetWheelOrientationQuaternion, METH_VARARGS},
	{"getWheelRotation", (PyCFunction)KX_VehicleWrapper::sPyGetWheelRotation, METH_VARARGS},
	{"getWheelPosition", (PyCFunction)KX_VehicleWrapper::sPyGetWheelPosition, METH_VARARGS},
	{"getConstraintId", (PyCFunction)KX_VehicleWrapper::sPyGetConstraintId, METH_VARARGS},
	{"getConstraintType", (PyCFunction)KX_VehicleWrapper::sPyGetConstraintType, METH_VARARGS},
	{"setSteeringValue", (PyCFunction)KX_VehicleWrapper::sPySetSteeringValue, METH_VARARGS},
	{"applyEngineForce", (PyCFunction)KX_VehicleWrapper::sPyApplyEngineForce, METH_VARARGS},
	{"applyBraking", (PyCFunction)KX_VehicleWrapper::sPyApplyBraking, METH_VARARGS},
	{"setTyreFriction", (PyCFunction)KX_VehicleWrapper::sPySetTyreFriction, METH_VARARGS},
	{"setSuspensionStiffness", (PyCFunction)KX_VehicleWrapper::sPySetSuspensionStiffness, METH_VARARGS},
	{"setSuspensionDamping", (PyCFunction)KX_VehicleWrapper::sPySetSuspensionDamping, METH_VARARGS},
	{"setSuspensionCompression", (PyCFunction)KX_VehicleWrapper::sPySetSuspensionCompression, METH_VARARGS},
	{"setRollInfluence", (PyCFunction)KX_VehicleWrapper::sPySetRollInfluence, METH_VARARGS},
	{nullptr, nullptr} //Sentinel
};

#define MASK_MAX ((1 << OB_MAX_COL_MASKS) - 1)

EXP_Attribute KX_VehicleWrapper::Attributes[] = {
	EXP_ATTRIBUTE_RW_FUNCTION_RANGE("rayMask", pyattr_get_ray_mask, pyattr_set_ray_mask, 1, MASK_MAX, false),
	EXP_ATTRIBUTE_RO_FUNCTION("constraint_id", pyattr_get_constraintId),
	EXP_ATTRIBUTE_RO_FUNCTION("constraint_type", pyattr_get_constraintType),
	EXP_ATTRIBUTE_NULL	//Sentinel
};

#undef MASK_MAX

int KX_VehicleWrapper::pyattr_get_constraintId()
{
	return m_vehicle->GetUserConstraintId();
}

int KX_VehicleWrapper::pyattr_get_constraintType()
{
	return PHY_VEHICLE_CONSTRAINT;
}

int KX_VehicleWrapper::pyattr_get_ray_mask()
{
	return m_vehicle->GetRayCastMask();
}

void KX_VehicleWrapper::pyattr_set_ray_mask(int value)
{
	m_vehicle->SetRayCastMask(value);
}

#endif // WITH_PYTHON
