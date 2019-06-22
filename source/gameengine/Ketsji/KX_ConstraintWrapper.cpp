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

/** \file gameengine/Ketsji/KX_ConstraintWrapper.cpp
 *  \ingroup ketsji
 */


#include "KX_ConstraintWrapper.h"
#include "PHY_IConstraint.h"

KX_ConstraintWrapper::KX_ConstraintWrapper(PHY_IConstraint *constraint)
	:m_constraint(constraint)
{
}
KX_ConstraintWrapper::~KX_ConstraintWrapper()
{
}

std::string KX_ConstraintWrapper::GetName() const
{
	return "KX_ConstraintWrapper";
}

#ifdef WITH_PYTHON

PyObject *KX_ConstraintWrapper::PyGetConstraintId()
{
	return PyLong_FromLong(m_constraint->GetIdentifier());
}


PyObject *KX_ConstraintWrapper::PyGetParam(PyObject *args, PyObject *kwds)
{
	int dof;
	float value;

	if (!PyArg_ParseTuple(args, "i:getParam", &dof)) {
		return nullptr;
	}

	value = m_constraint->GetParam(dof);
	return PyFloat_FromDouble(value);

}

PyObject *KX_ConstraintWrapper::PySetParam(PyObject *args, PyObject *kwds)
{
	int dof;
	float minLimit, maxLimit;

	if (!PyArg_ParseTuple(args, "iff:setParam", &dof, &minLimit, &maxLimit)) {
		return nullptr;
	}

	m_constraint->SetParam(dof, minLimit, maxLimit);
	Py_RETURN_NONE;
}


//python specific stuff
PyTypeObject KX_ConstraintWrapper::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"KX_ConstraintWrapper",
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

PyMethodDef KX_ConstraintWrapper::Methods[] = {
	{"getConstraintId", (PyCFunction)KX_ConstraintWrapper::sPyGetConstraintId, METH_NOARGS},
	{"setParam", (PyCFunction)KX_ConstraintWrapper::sPySetParam, METH_VARARGS},
	{"getParam", (PyCFunction)KX_ConstraintWrapper::sPyGetParam, METH_VARARGS},
	{nullptr, nullptr} //Sentinel
};

PyAttributeDef KX_ConstraintWrapper::Attributes[] = {
	EXP_PYATTRIBUTE_RO_FUNCTION("constraint_id", KX_ConstraintWrapper, pyattr_get_constraintId),
	EXP_PYATTRIBUTE_RO_FUNCTION("constraint_type", KX_ConstraintWrapper, pyattr_get_constraintType),
	EXP_PYATTRIBUTE_RW_FUNCTION("breakingThreshold", KX_ConstraintWrapper, pyattr_get_breakingThreshold, pyattr_set_breakingThreshold),
	EXP_PYATTRIBUTE_RW_FUNCTION("enabled", KX_ConstraintWrapper, pyattr_get_enabled, pyattr_set_enabled),
	EXP_PYATTRIBUTE_NULL    //Sentinel
};

PyObject *KX_ConstraintWrapper::pyattr_get_constraintId(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_ConstraintWrapper *self = static_cast<KX_ConstraintWrapper *>(self_v);
	return PyLong_FromLong(self->m_constraint->GetIdentifier());
}

PyObject *KX_ConstraintWrapper::pyattr_get_constraintType(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_ConstraintWrapper *self = static_cast<KX_ConstraintWrapper *>(self_v);
	return PyLong_FromLong(self->m_constraint->GetType());
}

PyObject *KX_ConstraintWrapper::pyattr_get_breakingThreshold(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_ConstraintWrapper *self = static_cast<KX_ConstraintWrapper *>(self_v);
	return PyFloat_FromDouble(self->m_constraint->GetBreakingThreshold());
}

int KX_ConstraintWrapper::pyattr_set_breakingThreshold(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_ConstraintWrapper *self = static_cast<KX_ConstraintWrapper *>(self_v);
	float val = PyFloat_AsDouble(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "constraint.%s = float: KX_ConstraintWrapper, expected a float", attrdef->m_name.c_str());
		return PY_SET_ATTR_FAIL;
	}

	self->m_constraint->SetBreakingThreshold(val);
	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_ConstraintWrapper::pyattr_get_enabled(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_ConstraintWrapper *self = static_cast<KX_ConstraintWrapper *>(self_v);
	return PyBool_FromLong(self->m_constraint->GetEnabled());
}

int KX_ConstraintWrapper::pyattr_set_enabled(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_ConstraintWrapper *self = static_cast<KX_ConstraintWrapper *>(self_v);
	int val = PyObject_IsTrue(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "constraint.%s = bool: KX_ConstraintWrapper, expected True or False", attrdef->m_name.c_str());
		return PY_SET_ATTR_FAIL;
	}

	self->m_constraint->SetEnabled(val);
	return PY_SET_ATTR_SUCCESS;
}

#endif // WITH_PYTHON
