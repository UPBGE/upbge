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
*
* The Original Code is: all of this file.
*
* Contributor(s): none yet.
*
* ***** END GPL LICENSE BLOCK *****
*/

/** \file SCA_VibrationActuator.h
*  \ingroup GameLogic
*/

#ifndef __SCA_VIBRATIONACTUATOR_H__
#define __SCA_VIBRATIONACTUATOR_H__

#include "SCA_IActuator.h"

class SCA_VibrationActuator : public SCA_IActuator
{
	Py_Header

private:

	int m_joyindex;
	short m_mode;
	float m_strength_left;
	float m_strength_right;
	int m_duration;
	float m_endtime;

public:

	enum KX_ACT_VIBRATION_MODE {
		KX_ACT_VIBRATION_NONE = 0,
		KX_ACT_VIBRATION_PLAY,
		KX_ACT_VIBRATION_STOP,
		KX_ACT_VIBRATION_MAX
	};

	SCA_VibrationActuator(SCA_IObject *gameobj, short mode, int joyindex, float strength_left, float strength_right, int duration);

	virtual	~SCA_VibrationActuator(void);

	virtual CValue*	GetReplica(void);

	virtual bool Update();

#ifdef WITH_PYTHON
	KX_PYMETHOD_DOC_NOARGS(SCA_VibrationActuator, startVibration);
	KX_PYMETHOD_DOC_NOARGS(SCA_VibrationActuator, stopVibration);

	static PyObject *pyattr_get_statusVibration(void *self, const struct KX_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_hasVibration(void *self, const struct KX_PYATTRIBUTE_DEF *attrdef);
#endif  /* WITH_PYTHON */

};

#endif

