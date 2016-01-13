/*
 * Detects if an object has moved
 *
 *
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

/** \file gameengine/Ketsji/KX_MovementSensor.cpp
 *  \ingroup ketsji
 */


#include "KX_MovementSensor.h"
#include "SCA_EventManager.h"
#include "SCA_LogicManager.h"
#include "SCA_IObject.h"
#include "KX_GameObject.h"
#include "DNA_sensor_types.h"

#include <stdio.h>


KX_MovementSensor::KX_MovementSensor(SCA_EventManager *eventmgr,
									 SCA_IObject *gameobj,
									 int axis, bool localflag)
	:SCA_ISensor(gameobj, eventmgr),
	m_localflag(localflag),
	m_axis((MovementAxis)axis)
{
	Init();
}

void KX_MovementSensor::Init()
{
	m_previousPosition = MT_Point3(0.0f, 0.0f, 0.0f);
	m_positionHasChanged = false;
}

KX_MovementSensor::~KX_MovementSensor()
{
}

CValue *KX_MovementSensor::GetReplica()
{
	KX_MovementSensor *replica = new KX_MovementSensor(*this);
	replica->ProcessReplica();
	replica->Init();

	return replica;
}

bool KX_MovementSensor::IsPositiveTrigger()
{
	bool result = m_positionHasChanged;

	if (m_invert) {
		result = !result;
	}

	return result;
}

bool KX_MovementSensor::Evaluate()
{
	KX_GameObject *obj = (KX_GameObject *)GetParent();
	MT_Point3 currentposition;

	if (m_localflag) {
		currentposition = obj->NodeGetLocalOrientation().inverse() * obj->NodeGetLocalPosition();
	}
	else {
		currentposition = obj->NodeGetWorldPosition();
	}

	float treshold = 0.001f;

	m_positionHasChanged = false;

	switch (m_axis)
	{
		case SENS_MOVEMENT_X_AXIS: // X
		{
			m_positionHasChanged = ((currentposition.x() - m_previousPosition.x()) > treshold);
			break;
		}
		case SENS_MOVEMENT_Y_AXIS: // Y
		{
			m_positionHasChanged = ((currentposition.y() - m_previousPosition.y()) > treshold);
			break;
		}
		case SENS_MOVEMENT_Z_AXIS: // Z
		{
			m_positionHasChanged = ((currentposition.z() - m_previousPosition.z()) > treshold);
			break;
		}
		case SENS_MOVEMENT_NEG_X_AXIS: // -X
		{
			m_positionHasChanged = ((currentposition.x() - m_previousPosition.x()) < -treshold);
			break;
		}
		case SENS_MOVEMENT_NEG_Y_AXIS: // -Y
		{
			m_positionHasChanged = ((currentposition.y() - m_previousPosition.y()) < -treshold);
			break;
		}
		case SENS_MOVEMENT_NEG_Z_AXIS: // -Z
		{
			m_positionHasChanged = ((currentposition.z() - m_previousPosition.z()) < -treshold);
			break;
		}
		case SENS_MOVEMENT_ALL_AXIS: // ALL
		{
			if ((fabs(currentposition.x() - m_previousPosition.x()) > treshold) ||
				(fabs(currentposition.y() - m_previousPosition.y()) > treshold) ||
				(fabs(currentposition.z() - m_previousPosition.z()) > treshold))
			{
				m_positionHasChanged = true;
			}
			break;
		}
	}

	m_previousPosition = currentposition;

	return m_positionHasChanged;
}

#ifdef WITH_PYTHON

/* ------------------------------------------------------------------------- */
/* Python functions                                                          */
/* ------------------------------------------------------------------------- */

/* Integration hooks ------------------------------------------------------- */
PyTypeObject KX_MovementSensor::Type = {
	PyVarObject_HEAD_INIT(NULL, 0)
	"KX_MovementSensor",
	sizeof(PyObjectPlus_Proxy),
	0,
	py_base_dealloc,
	0,
	0,
	0,
	0,
	py_base_repr,
	0,0,0,0,0,0,0,0,0,
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	0,0,0,0,0,0,0,
	Methods,
	0,
	0,
	&SCA_ISensor::Type,
	0,0,0,0,0,0,
	py_base_new
};

PyMethodDef KX_MovementSensor::Methods[] = {
	{NULL, NULL} // Sentinel
};

PyAttributeDef KX_MovementSensor::Attributes[] = {
	{NULL} // Sentinel
};

#endif

