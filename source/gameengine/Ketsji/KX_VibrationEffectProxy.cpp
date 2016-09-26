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
* The Original Code is: all of this file.
*
* Contributor(s): none yet.
*
* ***** END GPL LICENSE BLOCK *****
*/

/** \file gameengine/Ketsji/KX_VibrationEffectProxy.cpp
*  \ingroup ketsji
*/

#ifdef WITH_SDL
#ifdef WITH_PYTHON

#include "KX_VibrationEffectProxy.h"
#include "DEV_JoystickPrivate.h"
#include <thread>

#define SDL_CHECK(x) ((x) != (void *)0)

KX_VibrationEffectProxy::KX_VibrationEffectProxy(DEV_Joystick *joystick)
	:m_joystick(joystick)
{
	m_type = SDL_HAPTIC_LEFTRIGHT;
	m_periodicDirectionType = SDL_HAPTIC_POLAR;
	m_periodicDirection0 = 10000;
	m_periodicDirection1 = 8000;
	m_periodicPeriod = 1000;
	m_periodicMagnitude = 20000;
	m_periodicLength = 1000;
	m_periodicAttackLength = 500;
	m_periodicAttackLevel = 0;
	m_periodicFadeLength = 500;
	m_periodicFadeLevel = 0;
	m_conditionType = NULL;
}

KX_VibrationEffectProxy::~KX_VibrationEffectProxy()
{
}

// stuff for cvalue related things
static STR_String VibEffectName = "VibrationEffect";

STR_String& KX_VibrationEffectProxy::GetName()
{
	return VibEffectName;
}

PyTypeObject KX_VibrationEffectProxy::Type = {
	PyVarObject_HEAD_INIT(NULL, 0)
	"KX_VibrationEffectProxy",
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
	&CValue::Type,
	0, 0, 0, 0, 0, 0,
	py_base_new
};

PyMethodDef KX_VibrationEffectProxy::Methods[] = {
	KX_PYMETHODTABLE_NOARGS(KX_VibrationEffectProxy, play),
	{ NULL, NULL } //Sentinel
};

PyAttributeDef KX_VibrationEffectProxy::Attributes[] = {
	//KX_PYATTRIBUTE_RW_FUNCTION("x", KX_VibrationEffectProxy, pyattr_get_, pyattr_set_),
	KX_PYATTRIBUTE_INT_RW("type", 0, (1 << 5) - 1, true, KX_VibrationEffectProxy, m_type),
	KX_PYATTRIBUTE_INT_RW("periodicDirectionType", 0, 2, true, KX_VibrationEffectProxy, m_periodicDirectionType),
	KX_PYATTRIBUTE_INT_RW("periodicDirection0", INT_MIN, INT_MAX, true, KX_VibrationEffectProxy, m_periodicDirection0),
	KX_PYATTRIBUTE_INT_RW("periodicDirection1", INT_MIN, INT_MAX, true, KX_VibrationEffectProxy, m_periodicDirection1),
	KX_PYATTRIBUTE_INT_RW("periodicMagnitude", 0, 32767, true, KX_VibrationEffectProxy, m_periodicMagnitude),
	KX_PYATTRIBUTE_INT_RW("periodicLength", 0, INT_MAX, true, KX_VibrationEffectProxy, m_periodicLength),
	KX_PYATTRIBUTE_INT_RW("periodicAttackLength", 0, INT_MAX, true, KX_VibrationEffectProxy, m_periodicAttackLength),
	KX_PYATTRIBUTE_INT_RW("periodicAttackLevel", 0, INT_MAX, true, KX_VibrationEffectProxy, m_periodicAttackLevel),
	KX_PYATTRIBUTE_INT_RW("periodicFadeLength", 0, INT_MAX, true, KX_VibrationEffectProxy, m_periodicFadeLength),
	KX_PYATTRIBUTE_INT_RW("periodicFadeLevel", 0, INT_MAX, true, KX_VibrationEffectProxy, m_periodicFadeLevel),
	KX_PYATTRIBUTE_INT_RW("conditionType", (1 << 6) - 1, (1 << 10) - 1, true, KX_VibrationEffectProxy, m_conditionType),
	{ NULL }    //Sentinel
};

//PyObject *KX_VibrationEffectProxy::pyattr_get_(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
//{
//	KX_VibrationEffectProxy *self = static_cast<KX_VibrationEffectProxy *>(self_v);
//	return PyFloat_FromDouble(self->);
//}
//
//int KX_VibrationEffectProxy::pyattr_set_(void *self_v, const struct KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
//{
//	KX_VibrationEffectProxy *self = static_cast<KX_VibrationEffectProxy *>(self_v);
//	if (PyFloat_Check(value)) {
//		float val = PyFloat_AsDouble(value);
//		self->
//		return PY_SET_ATTR_SUCCESS;
//	}
//	return PY_SET_ATTR_FAIL;
//}

static void PlayAndDestroy(SDL_Haptic *haptic, int effectid, int length)
{
        SDL_HapticRunEffect(haptic, effectid, 1);
        SDL_Delay(length);
        SDL_HapticDestroyEffect(haptic, effectid);
}

KX_PYMETHODDEF_DOC_NOARGS(KX_VibrationEffectProxy, play, "play()")
{
	
	SDL_Haptic *haptic;
	SDL_HapticEffect effect;
	int effect_id;
	SDL_Joystick *joy = SDL_GameControllerGetJoystick(m_joystick->GetPrivate()->m_gamecontroller);

	// Open the device
	if (SDL_CHECK(SDL_HapticOpen)) {
		haptic = SDL_HapticOpenFromJoystick(joy);
	}
	if (haptic == NULL) {
		PyErr_SetString(PyExc_ValueError, "No haptic/vibration support"); // Most likely joystick isn't haptic
		return NULL;
	}

	// See if it can do sine waves
	if ((SDL_HapticQuery(haptic) & m_type) == 0) {		
		PyErr_SetString(PyExc_ValueError, "Effect type not supported. Most common is SDL_HAPTIC_LEFTRIGHT: (1 << 2)");
		return NULL;
	}

	// Create the effect
	memset(&effect, 0, sizeof(SDL_HapticEffect)); // 0 is safe default
	effect.type = m_type;
	effect.periodic.direction.type = m_periodicDirectionType; // Coordinates
	effect.periodic.direction.dir[0] = m_periodicDirection0; // Force comes from where?
	effect.periodic.direction.dir[1] = m_periodicDirection1;
	effect.periodic.period = m_periodicPeriod; // ... ms
	effect.periodic.magnitude = m_periodicMagnitude; // .../32767 strength
	effect.periodic.length = m_periodicLength; // 5 seconds long
	effect.periodic.attack_length = m_periodicAttackLength; // Takes ... second to get max strength
	effect.periodic.attack_level = m_periodicAttackLevel;
	effect.periodic.fade_length = m_periodicFadeLength; // Takes ... second to fade away
	effect.periodic.fade_level = m_periodicFadeLevel;
	if (m_conditionType) {
		effect.condition.type = m_conditionType;
	}

	// Upload the effect
	effect_id = SDL_HapticNewEffect(haptic, &effect);

	// Test the effect	
	std::thread wd(PlayAndDestroy, haptic, effect_id, effect.periodic.length);
	wd.detach();

	Py_RETURN_NONE;
}

#endif // WITH_PYTHON
#endif // WITH_SDL
