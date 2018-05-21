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

/** \file gameengine/Ketsji/KX_CharacterWrapper.cpp
 *  \ingroup ketsji
 */

#include "KX_CharacterWrapper.h"
#include "KX_PyMath.h"

#include "PHY_ICharacter.h"

#include "BLI_utildefines.h"
#include "BLI_math_base.h"

KX_CharacterWrapper::KX_CharacterWrapper(PHY_ICharacter *character) :
	m_character(character)
{
}

KX_CharacterWrapper::~KX_CharacterWrapper()
{
}

std::string KX_CharacterWrapper::GetName() const
{
	return "KX_CharacterWrapper";
}

#ifdef WITH_PYTHON

PyTypeObject KX_CharacterWrapper::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"KX_CharacterWrapper",
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

PyAttributeDef KX_CharacterWrapper::Attributes[] = {
	EXP_PYATTRIBUTE_RO_FUNCTION("onGround", KX_CharacterWrapper, pyattr_get_onground),
	EXP_PYATTRIBUTE_RW_FUNCTION("gravity", KX_CharacterWrapper, pyattr_get_gravity, pyattr_set_gravity),
	EXP_PYATTRIBUTE_RW_FUNCTION("fallSpeed", KX_CharacterWrapper, pyattr_get_fallSpeed, pyattr_set_fallSpeed),
	EXP_PYATTRIBUTE_RW_FUNCTION("maxJumps", KX_CharacterWrapper, pyattr_get_max_jumps, pyattr_set_max_jumps),
	EXP_PYATTRIBUTE_RW_FUNCTION("maxSlope", KX_CharacterWrapper, pyattr_get_maxSlope, pyattr_set_maxSlope),
	EXP_PYATTRIBUTE_RO_FUNCTION("jumpCount", KX_CharacterWrapper, pyattr_get_jump_count),
	EXP_PYATTRIBUTE_RW_FUNCTION("jumpSpeed", KX_CharacterWrapper, pyattr_get_jumpSpeed, pyattr_set_jumpSpeed),
	EXP_PYATTRIBUTE_RW_FUNCTION("walkDirection", KX_CharacterWrapper, pyattr_get_walk_dir, pyattr_set_walk_dir),
	EXP_PYATTRIBUTE_NULL    //Sentinel
};

PyObject *KX_CharacterWrapper::pyattr_get_onground(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_CharacterWrapper *self = static_cast<KX_CharacterWrapper *>(self_v);

	return PyBool_FromLong(self->m_character->OnGround());
}

PyObject *KX_CharacterWrapper::pyattr_get_gravity(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_CharacterWrapper *self = static_cast<KX_CharacterWrapper *>(self_v);

	return PyObjectFrom(self->m_character->GetGravity());
}

int KX_CharacterWrapper::pyattr_set_gravity(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_CharacterWrapper *self = static_cast<KX_CharacterWrapper *>(self_v);
	mt::vec3 gravity;
	if (!PyVecTo(value, gravity)) {
		return PY_SET_ATTR_FAIL;
	}

	self->m_character->SetGravity(gravity);
	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_CharacterWrapper::pyattr_get_fallSpeed(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_CharacterWrapper *self = static_cast<KX_CharacterWrapper *>(self_v);
	return PyFloat_FromDouble(self->m_character->GetFallSpeed());
}

int KX_CharacterWrapper::pyattr_set_fallSpeed(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_CharacterWrapper *self = static_cast<KX_CharacterWrapper *>(self_v);
	const float param = PyFloat_AsDouble(value);

	if (param == -1 || param < 0.0f) {
		PyErr_SetString(PyExc_ValueError, "KX_CharacterWrapper.fallSpeed: expected a positive float");
		return PY_SET_ATTR_FAIL;
	}

	self->m_character->SetFallSpeed(param);
	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_CharacterWrapper::pyattr_get_maxSlope(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_CharacterWrapper *self = static_cast<KX_CharacterWrapper *>(self_v);
	return PyFloat_FromDouble(self->m_character->GetMaxSlope());
}

int KX_CharacterWrapper::pyattr_set_maxSlope(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_CharacterWrapper *self = static_cast<KX_CharacterWrapper *>(self_v);
	const float param = PyFloat_AsDouble(value);

	if (param == -1 || param < 0.0f || param > M_PI_2) {
		PyErr_SetString(PyExc_ValueError, "KX_CharacterWrapper.maxSlope: expected a float between 0 and half pi");
		return PY_SET_ATTR_FAIL;
	}

	self->m_character->SetMaxSlope(param);
	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_CharacterWrapper::pyattr_get_max_jumps(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_CharacterWrapper *self = static_cast<KX_CharacterWrapper *>(self_v);

	return PyLong_FromLong(self->m_character->GetMaxJumps());
}

int KX_CharacterWrapper::pyattr_set_max_jumps(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_CharacterWrapper *self = static_cast<KX_CharacterWrapper *>(self_v);
	long param = PyLong_AsLong(value);

	if (param == -1) {
		PyErr_SetString(PyExc_ValueError, "KX_CharacterWrapper.maxJumps: expected an integer");
		return PY_SET_ATTR_FAIL;
	}

	CLAMP(param, 0, 255);

	self->m_character->SetMaxJumps(param);
	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_CharacterWrapper::pyattr_get_jump_count(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_CharacterWrapper *self = static_cast<KX_CharacterWrapper *>(self_v);

	return PyLong_FromLong(self->m_character->GetJumpCount());
}

PyObject *KX_CharacterWrapper::pyattr_get_jumpSpeed(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_CharacterWrapper *self = static_cast<KX_CharacterWrapper *>(self_v);
	return PyFloat_FromDouble(self->m_character->GetJumpSpeed());
}

int KX_CharacterWrapper::pyattr_set_jumpSpeed(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_CharacterWrapper *self = static_cast<KX_CharacterWrapper *>(self_v);
	const float param = PyFloat_AsDouble(value);

	if (param == -1) {
		PyErr_SetString(PyExc_ValueError, "KX_CharacterWrapper.gravity: expected a float");
		return PY_SET_ATTR_FAIL;
	}

	self->m_character->SetJumpSpeed(param);
	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_CharacterWrapper::pyattr_get_walk_dir(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_CharacterWrapper *self = static_cast<KX_CharacterWrapper *>(self_v);

	return PyObjectFrom(self->m_character->GetWalkDirection());
}

int KX_CharacterWrapper::pyattr_set_walk_dir(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_CharacterWrapper *self = static_cast<KX_CharacterWrapper *>(self_v);
	mt::vec3 dir;
	if (!PyVecTo(value, dir)) {
		PyErr_SetString(PyExc_TypeError, "KX_CharacterWrapper.walkDirection: expected a vector");
		return PY_SET_ATTR_FAIL;
	}

	self->m_character->SetWalkDirection(dir);
	return PY_SET_ATTR_SUCCESS;
}

PyMethodDef KX_CharacterWrapper::Methods[] = {
	EXP_PYMETHODTABLE_NOARGS(KX_CharacterWrapper, jump),
	EXP_PYMETHODTABLE(KX_CharacterWrapper, setVelocity),
	EXP_PYMETHODTABLE_NOARGS(KX_CharacterWrapper, reset),
	{nullptr, nullptr} //Sentinel
};

EXP_PYMETHODDEF_DOC_NOARGS(KX_CharacterWrapper, jump,
                           "jump()\n"
                           "makes the character jump.\n")
{
	m_character->Jump();

	Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(KX_CharacterWrapper, setVelocity,
                    "setVelocity(velocity, time, local=False)\n"
                    "set the character velocity for time period.\n")
{
	PyObject *pyvect;
	float time;
	int local = 0;

	if (!PyArg_ParseTuple(args, "Of|i:setVelocity", &pyvect, &time, &local)) {
		return nullptr;
	}

	mt::vec3 velocity;
	if (!PyVecTo(pyvect, velocity)) {
		return nullptr;
	}

	m_character->SetVelocity(velocity, time, local);

	Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC_NOARGS(KX_CharacterWrapper, reset,
                           "reset()\n"
                           "reset the character velocity and walk direction.\n")
{
	m_character->Reset();

	Py_RETURN_NONE;
}

#endif // WITH_PYTHON
