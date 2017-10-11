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

EXP_Attribute KX_CharacterWrapper::Attributes[] = {
	EXP_ATTRIBUTE_RO_FUNCTION("onGround", pyattr_get_onground),
	EXP_ATTRIBUTE_RW_FUNCTION("gravity", pyattr_get_gravity, pyattr_set_gravity),
	EXP_ATTRIBUTE_RW_FUNCTION_RANGE("fallSpeed", pyattr_get_fallSpeed, pyattr_set_fallSpeed, 0.0f, FLT_MAX, false),
	EXP_ATTRIBUTE_RW_FUNCTION_RANGE("maxSlope", pyattr_get_maxSlope, pyattr_set_maxSlope, 0.0f, M_PI_2, false),
	EXP_ATTRIBUTE_RW_FUNCTION_RANGE("maxJumps", pyattr_get_max_jumps, pyattr_set_max_jumps, 0, 255, true),
	EXP_ATTRIBUTE_RO_FUNCTION("jumpCount", pyattr_get_jump_count),
	EXP_ATTRIBUTE_RW_FUNCTION("jumpSpeed", pyattr_get_jumpSpeed, pyattr_set_jumpSpeed),
	EXP_ATTRIBUTE_RW_FUNCTION("walkDirection", pyattr_get_walk_dir, pyattr_set_walk_dir),
	EXP_ATTRIBUTE_NULL	//Sentinel
};

bool KX_CharacterWrapper::pyattr_get_onground()
{
	return m_character->OnGround();
}

mt::vec3 KX_CharacterWrapper::pyattr_get_gravity()
{
	return m_character->GetGravity();
}

void KX_CharacterWrapper::pyattr_set_gravity(const mt::vec3& value)
{
	m_character->SetGravity(value);
}

float KX_CharacterWrapper::pyattr_get_fallSpeed()
{
	return m_character->GetFallSpeed();
}

void KX_CharacterWrapper::pyattr_set_fallSpeed(float value)
{
	m_character->SetFallSpeed(value);
}

float KX_CharacterWrapper::pyattr_get_maxSlope()
{
	return m_character->GetMaxSlope();
}

void KX_CharacterWrapper::pyattr_set_maxSlope(float value)
{
	m_character->SetMaxSlope(value);
}

int KX_CharacterWrapper::pyattr_get_max_jumps()
{
	return m_character->GetMaxJumps();
}

void KX_CharacterWrapper::pyattr_set_max_jumps(int value)
{
	m_character->SetMaxJumps(value);
}

int KX_CharacterWrapper::pyattr_get_jump_count()
{
	return m_character->GetJumpCount();
}

float KX_CharacterWrapper::pyattr_get_jumpSpeed()
{
	return m_character->GetJumpSpeed();
}

void KX_CharacterWrapper::pyattr_set_jumpSpeed(float value)
{
	m_character->SetJumpSpeed(value);
}

mt::vec3 KX_CharacterWrapper::pyattr_get_walk_dir()
{
	return m_character->GetWalkDirection();
}

void KX_CharacterWrapper::pyattr_set_walk_dir(const mt::vec3& value)
{
	m_character->SetWalkDirection(value);
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
