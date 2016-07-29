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

/** \file gameengine/Ketsji/KX_VibrationActuator.cpp
*  \ingroup ketsji
*/


#include "KX_VibrationActuator.h"
#include "SCA_JoystickManager.h"
#include "DEV_JoystickPrivate.h"
#include <iostream> //std::cout
#ifdef WITH_SDL
	#include "SDL.h"
#endif // WITH_SDL

KX_VibrationActuator::KX_VibrationActuator(SCA_IObject* gameobj, int joyindex, float strength, int duration)
	: SCA_IActuator(gameobj, KX_ACT_VIBRATION),
	m_joyindex(joyindex),
	m_strength(strength),
	m_duration(duration)
{
}

KX_VibrationActuator::~KX_VibrationActuator(void)
{
}

CValue* KX_VibrationActuator::GetReplica(void)
{
	KX_VibrationActuator* replica = new KX_VibrationActuator(*this);
	replica->ProcessReplica();
	return replica;
}

bool KX_VibrationActuator::Update()
{
#ifdef WITH_SDL

	/* Can't init instance in constructor because m_joystick list is not available yet */
	SCA_JoystickManager *mgr = (SCA_JoystickManager *)GetLogicManager();
	DEV_Joystick *instance = mgr->GetJoystickDevice(m_joyindex) ? mgr->GetJoystickDevice(m_joyindex) : NULL;

	if (!instance) {
		return false;
	}
	bool bNegativeEvent = IsNegativeEvent();

	if (bNegativeEvent) {
		return false;
	}
	
	SDL_Haptic *haptic;

	// Open the device
	haptic = instance->GetPrivate()->m_haptic;
	if (haptic == NULL) {
		return false;
	}

	// Initialize simple rumble
	if (SDL_HapticRumbleInit(haptic) != 0) {
		return false;
	}

	// Play effect at strength for m_duration frame
	if (SDL_HapticRumblePlay(haptic, m_strength, m_duration) != 0)
		return false;

	RemoveAllEvents();

	return true;

#endif // WITH_SDL
}

#ifdef WITH_PYTHON

/* ------------------------------------------------------------------------- */
/* Python functions                                                          */
/* ------------------------------------------------------------------------- */



/* Integration hooks ------------------------------------------------------- */
PyTypeObject KX_VibrationActuator::Type = {
	PyVarObject_HEAD_INIT(NULL, 0)
	"KX_VibrationActuator",
	sizeof(PyObjectPlus_Proxy),
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
	&SCA_IActuator::Type,
	0, 0, 0, 0, 0, 0,
	py_base_new
};

PyMethodDef KX_VibrationActuator::Methods[] = {
	{ NULL, NULL } //Sentinel
};

PyAttributeDef KX_VibrationActuator::Attributes[] = {
	KX_PYATTRIBUTE_INT_RW("duration", 0, INT_MAX, true, KX_VibrationActuator, m_duration),
	KX_PYATTRIBUTE_INT_RW("joyindex", 0, 7, true, KX_VibrationActuator, m_joyindex),
	KX_PYATTRIBUTE_FLOAT_RW("strength", 0.0, 1.0, KX_VibrationActuator, m_strength),
	{ NULL }	//Sentinel
};

#endif // WITH_PYTHON
