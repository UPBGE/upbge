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

/** \file gameengine/GameLogic/SCA_IController.cpp
 *  \ingroup gamelogic
 */

#include "SCA_IController.h"
#include "SCA_IActuator.h"
#include "SCA_ISensor.h"
#include "EXP_ListWrapper.h"

#include "CM_Message.h"

#include <algorithm>

SCA_IController::SCA_IController(SCA_IObject *gameobj)
	:SCA_ILogicBrick(gameobj),
	m_statemask(0),
	m_justActivated(false)
{
}

SCA_IController::~SCA_IController()
{
}

std::vector<SCA_ISensor *>& SCA_IController::GetLinkedSensors()
{
	return m_linkedsensors;
}

std::vector<SCA_IActuator *>& SCA_IController::GetLinkedActuators()
{
	return m_linkedactuators;
}

void SCA_IController::UnlinkAllSensors()
{
	for (SCA_ISensor *sensor : m_linkedsensors) {
		if (IsActive()) {
			sensor->DecLink();
		}
		sensor->UnlinkController(this);
	}
	m_linkedsensors.clear();
}

void SCA_IController::UnlinkAllActuators()
{
	for (SCA_IActuator *actuator : m_linkedactuators) {
		if (IsActive()) {
			actuator->DecLink();
		}
		actuator->UnlinkController(this);
	}
	m_linkedactuators.clear();
}

void SCA_IController::LinkToActuator(SCA_IActuator *actua)
{
	m_linkedactuators.push_back(actua);
	if (IsActive()) {
		actua->IncLink();
	}
}

void SCA_IController::UnlinkActuator(SCA_IActuator *actua)
{
	std::vector<SCA_IActuator *>::iterator it = std::find(m_linkedactuators.begin(), m_linkedactuators.end(), actua);
	if (it != m_linkedactuators.end()) {
		m_linkedactuators.erase(it);
		if (IsActive()) {
			actua->DecLink();
		}
	}
	else {
		CM_LogicBrickWarning(this, "missing link from controller " << m_gameobj->GetName() << ":" << GetName()
			<< " to actuator " << actua->GetParent()->GetName() << ":" << actua->GetName());
	}
}

void SCA_IController::LinkToSensor(SCA_ISensor *sensor)
{
	m_linkedsensors.push_back(sensor);
	if (IsActive()) {
		sensor->IncLink();
	}
}

void SCA_IController::UnlinkSensor(SCA_ISensor *sensor)
{
	std::vector<SCA_ISensor *>::iterator it = std::find(m_linkedsensors.begin(), m_linkedsensors.end(), sensor);
	if (it != m_linkedsensors.end()) {
		m_linkedsensors.erase(it);
		if (IsActive()) {
			sensor->DecLink();
		}
	}
	else {
		CM_LogicBrickWarning(this, "missing link from controller " << m_gameobj->GetName() << ":" << GetName()
			<< " to sensor " << sensor->GetParent()->GetName() << ":" << sensor->GetName());
	}
}

void SCA_IController::SetState(unsigned int state)
{
	m_statemask = state;
}

void SCA_IController::ApplyState(unsigned int state)
{
	if (m_statemask & state) {
		if (!IsActive()) {
			// reactive the controller, all the links to actuator are valid again
			for (SCA_IActuator *actuator : m_linkedactuators) {
				actuator->IncLink();
			}

			for (SCA_ISensor *sensor : m_linkedsensors) {
				sensor->IncLink();
			}
			SetActive(true);
			m_justActivated = true;
		}
	}
	else if (IsActive()) {
		for (SCA_IActuator *actuator : m_linkedactuators) {
			actuator->DecLink();
		}

		for (SCA_ISensor *sensor : m_linkedsensors) {
			sensor->DecLink();
		}
		SetActive(false);
		m_justActivated = false;
	}
}

void SCA_IController::Deactivate()
{
	// the controller can only be part of a sensor m_newControllers list
	Delink();
}

bool SCA_IController::IsJustActivated()
{
	return m_justActivated;
}

void SCA_IController::ClrJustActivated()
{
	m_justActivated = false;
}

void SCA_IController::SetBookmark(bool bookmark)
{
	m_bookmark = bookmark;
}

void SCA_IController::Activate(SG_DList& head)
{
	if (QEmpty()) {
		if (m_bookmark) {
			m_gameobj->m_activeBookmarkedControllers.QAddBack(this);
			head.AddFront(&m_gameobj->m_activeBookmarkedControllers);
		}
		else {
			InsertActiveQList(m_gameobj->m_activeControllers);
			head.AddBack(&m_gameobj->m_activeControllers);
		}
	}
}

#ifdef WITH_PYTHON

/* Python api */

PyTypeObject SCA_IController::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"SCA_IController",
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
	&SCA_ILogicBrick::Type,
	0, 0, 0, 0, 0, 0,
	py_base_new
};

PyMethodDef SCA_IController::Methods[] = {
	{nullptr, nullptr} // Sentinel
};

PyAttributeDef SCA_IController::Attributes[] = {
	EXP_PYATTRIBUTE_RO_FUNCTION("state", SCA_IController, pyattr_get_state),
	EXP_PYATTRIBUTE_RO_FUNCTION("sensors", SCA_IController, pyattr_get_sensors),
	EXP_PYATTRIBUTE_RO_FUNCTION("actuators", SCA_IController, pyattr_get_actuators),
	EXP_PYATTRIBUTE_BOOL_RW("useHighPriority", SCA_IController, m_bookmark),
	EXP_PYATTRIBUTE_NULL // Sentinel
};

PyObject *SCA_IController::pyattr_get_state(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	SCA_IController *self = static_cast<SCA_IController *>(self_v);
	return PyLong_FromLong(self->m_statemask);
}

static int sca_icontroller_get_sensors_size_cb(void *self_v)
{
	return ((SCA_IController *)self_v)->GetLinkedSensors().size();
}

static PyObject *sca_icontroller_get_sensors_item_cb(void *self_v, int index)
{
	return ((SCA_IController *)self_v)->GetLinkedSensors()[index]->GetProxy();
}

static const std::string sca_icontroller_get_sensors_item_name_cb(void *self_v, int index)
{
	return ((SCA_IController *)self_v)->GetLinkedSensors()[index]->GetName();
}

PyObject *SCA_IController::pyattr_get_sensors(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	return (new EXP_ListWrapper(self_v,
	                         ((SCA_IController *)self_v)->GetProxy(),
	                         nullptr,
	                         sca_icontroller_get_sensors_size_cb,
	                         sca_icontroller_get_sensors_item_cb,
	                         sca_icontroller_get_sensors_item_name_cb,
	                         nullptr))->NewProxy(true);
}

static int sca_icontroller_get_actuators_size_cb(void *self_v)
{
	return ((SCA_IController *)self_v)->GetLinkedActuators().size();
}

static PyObject *sca_icontroller_get_actuators_item_cb(void *self_v, int index)
{
	return ((SCA_IController *)self_v)->GetLinkedActuators()[index]->GetProxy();
}

static const std::string sca_icontroller_get_actuators_item_name_cb(void *self_v, int index)
{
	return ((SCA_IController *)self_v)->GetLinkedActuators()[index]->GetName();
}

PyObject *SCA_IController::pyattr_get_actuators(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	return (new EXP_ListWrapper(self_v,
	                         ((SCA_IController *)self_v)->GetProxy(),
	                         nullptr,
	                         sca_icontroller_get_actuators_size_cb,
	                         sca_icontroller_get_actuators_item_cb,
	                         sca_icontroller_get_actuators_item_name_cb,
	                         nullptr))->NewProxy(true);
}
#endif // WITH_PYTHON
