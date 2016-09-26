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

/** \file KX_VibrationEffectProxy.h
*  \ingroup ketsji
*/

#ifndef __KX_VIBRATIONEFFECTPROXY_H__
#define __KX_VIBRATIONEFFECTPROXY_H__

#ifdef WITH_SDL
#ifdef WITH_PYTHON

#include "SCA_IObject.h"
#include "DEV_Joystick.h"

class KX_VibrationEffectProxy : public CValue
{
	Py_Header

private:

	DEV_Joystick *m_joystick;
	SDL_Haptic *m_haptic;
	SDL_HapticEffect m_effect;
	int m_effectId;

	int m_type;
	int m_periodicDirectionType;
	int m_periodicDirection0;
	int m_periodicDirection1;
	int m_periodicPeriod;
	int m_periodicMagnitude;
	int m_periodicLength;
	int m_periodicAttackLength;
	int m_periodicAttackLevel;
	int m_periodicFadeLength;
	int m_periodicFadeLevel;
	int m_conditionType;


public:
	KX_VibrationEffectProxy(DEV_Joystick *joystick);
	virtual ~KX_VibrationEffectProxy();

	// stuff for cvalue related things
	STR_String& GetName();

	//static PyObject *pyattr_get_(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	//static int pyattr_set_x(void *self, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);

	KX_PYMETHOD_DOC_NOARGS(KX_VibrationEffectProxy, play);
};

#endif  // WITH_PYTHON
#endif // WITH_SDL

#endif  // __KX_VIBRATIONEFFECTPROXY_H__
