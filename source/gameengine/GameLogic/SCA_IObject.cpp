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

/** \file gameengine/GameLogic/SCA_IObject.cpp
 *  \ingroup gamelogic
 */

#include "SCA_IObject.h"

#include "CM_List.h"
#include "SCA_IActuator.h"
#include "SCA_ISensor.h"

SG_QList SCA_IObject::m_activeBookmarkedControllers;

SCA_IObject::SCA_IObject()
    : KX_PythonProxy(),
      m_logicSuspended(false),
      m_initState(0),
      m_state(0),
      m_backupState(0),
      m_firstState(nullptr)
{
}

SCA_IObject::~SCA_IObject()
{
  for (SCA_ISensor *sensor : m_sensors) {
    // Use Delete for sensor to ensure proper cleaning.
    sensor->Delete();
  }

  for (SCA_IController *controller : m_controllers) {
    // Use Delete for controller to ensure proper cleaning (expression controller).
    controller->Delete();
  }

  for (SCA_IActuator *actuator : m_registeredActuators) {
    actuator->UnlinkObject(this);
  }
  for (SCA_IActuator *actuator : m_actuators) {
    actuator->Delete();
  }

  for (SCA_IObject *object : m_registeredObjects) {
    object->UnlinkObject(this);
  }
}

SCA_ControllerList &SCA_IObject::GetControllers()
{
  return m_controllers;
}
SCA_SensorList &SCA_IObject::GetSensors()
{
  return m_sensors;
}
SCA_ActuatorList &SCA_IObject::GetActuators()
{
  return m_actuators;
}
SG_QList &SCA_IObject::GetActiveActuators()
{
  return m_activeActuators;
}

SG_QList &SCA_IObject::GetActiveControllers()
{
  return m_activeControllers;
}

SG_QList &SCA_IObject::GetActiveBookmarkedControllers()
{
  return m_activeBookmarkedControllers;
}

void SCA_IObject::AddSensor(SCA_ISensor *act)
{
  act->AddRef();
  m_sensors.push_back(act);
}

void SCA_IObject::ReserveSensor(int num)
{
  m_sensors.reserve(num);
}

void SCA_IObject::AddController(SCA_IController *act)
{
  act->AddRef();
  m_controllers.push_back(act);
}

void SCA_IObject::ReserveController(int num)
{
  m_controllers.reserve(num);
}

void SCA_IObject::AddActuator(SCA_IActuator *act)
{
  act->AddRef();
  m_actuators.push_back(act);
}

void SCA_IObject::ReserveActuator(int num)
{
  m_actuators.reserve(num);
}

void SCA_IObject::RegisterActuator(SCA_IActuator *act)
{
  // Don't increase ref count, it would create dead lock
  m_registeredActuators.push_back(act);
}

void SCA_IObject::UnregisterActuator(SCA_IActuator *act)
{
  CM_ListRemoveIfFound(m_registeredActuators, act);
}

void SCA_IObject::RegisterObject(SCA_IObject *obj)
{
  /* One object may be registered multiple times via constraint target
   * store multiple reference, this will serve as registration counter */
  m_registeredObjects.push_back(obj);
}

void SCA_IObject::UnregisterObject(SCA_IObject *obj)
{
  CM_ListRemoveIfFound(m_registeredObjects, obj);
}

bool SCA_IObject::UnlinkObject(SCA_IObject *clientobj)
{
  return false;
}

void SCA_IObject::ReParentLogic()
{
  SCA_ActuatorList &oldactuators = GetActuators();
  for (unsigned short i = 0, size = oldactuators.size(); i < size; ++i) {
    SCA_IActuator *newactuator = static_cast<SCA_IActuator *>(oldactuators[i]->GetReplica());
    newactuator->ReParent(this);
    // Actuators are initially not connected to any controller
    newactuator->SetActive(false);
    newactuator->ClrLink();
    oldactuators[i] = newactuator;
  }

  SCA_ControllerList &oldcontrollers = GetControllers();
  for (unsigned short i = 0, size = oldcontrollers.size(); i < size; ++i) {
    SCA_IController *newcontroller = static_cast<SCA_IController *>(
        oldcontrollers[i]->GetReplica());
    newcontroller->ReParent(this);
    newcontroller->SetActive(false);
    oldcontrollers[i] = newcontroller;
  }
  // Convert sensors last so that actuators are already available for Actuator sensor
  SCA_SensorList &oldsensors = GetSensors();
  for (unsigned short i = 0, size = oldsensors.size(); i < size; ++i) {
    SCA_ISensor *newsensor = static_cast<SCA_ISensor *>(oldsensors[i]->GetReplica());
    newsensor->ReParent(this);
    newsensor->SetActive(false);
    // sensors are initially not connected to any controller
    newsensor->ClrLink();
    oldsensors[i] = newsensor;
  }

  // A new object cannot be client of any actuator
  m_registeredActuators.clear();
  m_registeredObjects.clear();
}

SCA_ISensor *SCA_IObject::FindSensor(const std::string &sensorname)
{
  for (SCA_ISensor *sensor : m_sensors) {
    if (sensor->GetName() == sensorname) {
      return sensor;
    }
  }
  return nullptr;
}

SCA_IController *SCA_IObject::FindController(const std::string &controllername)
{
  for (SCA_IController *controller : m_controllers) {
    if (controller->GetName() == controllername) {
      return controller;
    }
  }
  return nullptr;
}

SCA_IActuator *SCA_IObject::FindActuator(const std::string &actuatorname)
{
  for (SCA_IActuator *actuator : m_actuators) {
    if (actuator->GetName() == actuatorname) {
      return actuator;
    }
  }
  return nullptr;
}

void SCA_IObject::SuspendLogic()
{
  if (!m_logicSuspended) {
    m_logicSuspended = true;
    /* flag suspend for all sensors */
    for (SCA_ISensor *sensor : m_sensors) {
      sensor->Suspend();
    }
    m_backupState = GetState();
    /* Suspend sensors is not enough to stop logic activity
     * then we change logic state to a state which is probably not used */
    SetState((1 << 30));
  }
}

void SCA_IObject::ResumeLogic(void)
{
  if (m_logicSuspended) {
    m_logicSuspended = false;
    /* unflag suspend for all sensors */
    for (SCA_ISensor *sensor : m_sensors) {
      sensor->Resume();
    }
    SetState(m_backupState);
  }
}

void SCA_IObject::SetInitState(unsigned int initState)
{
  m_initState = initState;
}

void SCA_IObject::ResetState()
{
  SetState(m_initState);
}

void SCA_IObject::SetState(unsigned int state)
{
  /* 1) set the new state bits that are 1
   * 2) clr the new state bits that are 0
   * This to ensure continuity if a sensor is attached to two states
   * that are switching state: no need to deactive and reactive the sensor
   */

  const unsigned int tmpstate = m_state | state;
  if (tmpstate != m_state) {
    // Update the status of the controllers.
    for (SCA_IController *controller : m_controllers) {
      controller->ApplyState(tmpstate);
    }
  }
  m_state = state;
  if (m_state != tmpstate) {
    for (SCA_IController *controller : m_controllers) {
      controller->ApplyState(m_state);
    }
  }
}

unsigned int SCA_IObject::GetState()
{
  return m_state;
}

SG_QList **SCA_IObject::GetFirstState()
{
  return &m_firstState;
}

void SCA_IObject::SetFirstState(SG_QList *firstState)
{
  m_firstState = firstState;
}

int SCA_IObject::GetGameObjectType() const
{
  return -1;
}

#ifdef WITH_PYTHON

/* ------------------------------------------------------------------------- */
/* Python functions                                                          */
/* ------------------------------------------------------------------------- */

/* Integration hooks ------------------------------------------------------- */
PyTypeObject SCA_IObject::Type = {PyVarObject_HEAD_INIT(nullptr, 0) "SCA_IObject",
                                  sizeof(EXP_PyObjectPlus_Proxy),
                                  0,
                                  py_base_dealloc,
                                  0,
                                  0,
                                  0,
                                  0,
                                  py_base_repr,
                                  0,
                                  0,
                                  0,
                                  0,
                                  0,
                                  0,
                                  0,
                                  0,
                                  0,
                                  Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
                                  0,
                                  0,
                                  0,
                                  0,
                                  0,
                                  0,
                                  0,
                                  Methods,
                                  0,
                                  0,
                                  &EXP_Value::Type,
                                  0,
                                  0,
                                  0,
                                  0,
                                  0,
                                  0,
                                  py_base_new};

PyMethodDef SCA_IObject::Methods[] = {
    {nullptr, nullptr}  // Sentinel
};

PyAttributeDef SCA_IObject::Attributes[] = {
    EXP_PYATTRIBUTE_NULL  // Sentinel
};

#endif  // WITH_PYTHON
