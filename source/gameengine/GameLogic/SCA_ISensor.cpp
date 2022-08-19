/*
 * Abstract class for sensor logic bricks
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

/** \file gameengine/GameLogic/SCA_ISensor.cpp
 *  \ingroup gamelogic
 */

#include "SCA_ISensor.h"

#include "CM_List.h"
#include "CM_Message.h"
#include "SCA_PythonController.h"

void SCA_ISensor::ReParent(SCA_IObject *parent)
{
  SCA_ILogicBrick::ReParent(parent);
}

SCA_ISensor::SCA_ISensor(SCA_IObject *gameobj, SCA_EventManager *eventmgr)
    : SCA_ILogicBrick(gameobj),
      m_eventmgr(eventmgr),
      m_pos_pulsemode(false),
      m_neg_pulsemode(false),
      m_skipped_ticks(0),
      m_pos_ticks(0),
      m_neg_ticks(0),
      m_invert(false),
      m_level(false),
      m_tap(false),
      m_reset(false),
      m_suspended(false),
      m_links(0),
      m_state(false),
      m_prev_state(false)
{
}

SCA_ISensor::~SCA_ISensor()
{
}

void SCA_ISensor::ProcessReplica()
{
  SCA_ILogicBrick::ProcessReplica();
  m_linkedcontrollers.clear();
}

bool SCA_ISensor::IsPositiveTrigger()
{
  bool result = false;

  if (m_eventval) {
    result = (m_eventval->GetNumber() != 0.0);
  }
  if (m_invert) {
    result = !result;
  }

  return result;
}

void SCA_ISensor::SetPulseMode(bool posmode, bool negmode, int skippedticks)
{
  m_pos_pulsemode = posmode;
  m_neg_pulsemode = negmode;
  m_skipped_ticks = skippedticks;
}

void SCA_ISensor::SetInvert(bool inv)
{
  m_invert = inv;
}

void SCA_ISensor::SetLevel(bool lvl)
{
  m_level = lvl;
}

void SCA_ISensor::SetTap(bool tap)
{
  m_tap = tap;
}

double SCA_ISensor::GetNumber()
{
  return GetState();
}

SCA_ISensor::sensortype SCA_ISensor::GetSensorType()
{
  return ST_NONE;
}

void SCA_ISensor::Suspend()
{
  m_suspended = true;
}

bool SCA_ISensor::IsSuspended()
{
  return m_suspended;
}

void SCA_ISensor::Resume()
{
  m_suspended = false;
}

bool SCA_ISensor::GetState()
{
  return m_state;
}

bool SCA_ISensor::GetPrevState()
{
  return m_prev_state;
}

int SCA_ISensor::GetPosTicks()
{
  return m_pos_ticks;
}

int SCA_ISensor::GetNegTicks()
{
  return m_neg_ticks;
}

void SCA_ISensor::ClrLink()
{
  m_links = 0;
}

void SCA_ISensor::IncLink()
{
  if (!m_links++) {
    RegisterToManager();
  }
}

bool SCA_ISensor::IsNoLink() const
{
  return !m_links;
}

void SCA_ISensor::Init()
{
  CM_LogicBrickError(
      this, "sensor " << m_name << " has no init function, please report this bug to Blender.org");
}

void SCA_ISensor::DecLink()
{
  --m_links;
  if (m_links < 0) {
    CM_LogicBrickWarning(this, "sensor " << m_name << " has negative m_links: " << m_links);
    m_links = 0;
  }
  if (!m_links) {
    // sensor is detached from all controllers, remove it from manager
    UnregisterToManager();
  }
}

void SCA_ISensor::RegisterToManager()
{
  // sensor is just activated, initialize it
  Init();
  m_state = false;
  m_eventmgr->RegisterSensor(this);
}

void SCA_ISensor::Replace_EventManager(class SCA_LogicManager *logicmgr)
{
  // True if we're used currently.
  if (m_links) {
    m_eventmgr->RemoveSensor(this);
    m_eventmgr = logicmgr->FindEventManager(m_eventmgr->GetType());
    m_eventmgr->RegisterSensor(this);
  }
  else {
    m_eventmgr = logicmgr->FindEventManager(m_eventmgr->GetType());
  }
}

void SCA_ISensor::LinkToController(SCA_IController *controller)
{
  m_linkedcontrollers.push_back(controller);
}

void SCA_ISensor::UnlinkController(SCA_IController *controller)
{
  if (!CM_ListRemoveIfFound(m_linkedcontrollers, controller)) {
    CM_LogicBrickWarning(this,
                         "missing link from sensor " << m_gameobj->GetName() << ":" << GetName()
                                                     << " to controller "
                                                     << controller->GetParent()->GetName() << ":"
                                                     << controller->GetName());
  }
}

void SCA_ISensor::UnlinkAllControllers()
{
  for (SCA_IController *controller : m_linkedcontrollers) {
    controller->UnlinkSensor(this);
  }
  m_linkedcontrollers.clear();
}

void SCA_ISensor::UnregisterToManager()
{
  m_eventmgr->RemoveSensor(this);
  m_links = 0;
}

void SCA_ISensor::ActivateControllers(class SCA_LogicManager *logicmgr)
{
  for (SCA_IController *controller : m_linkedcontrollers) {
    if (controller->IsActive()) {
      logicmgr->AddTriggeredController(controller, this);
    }
  }
}

void SCA_ISensor::Activate(class SCA_LogicManager *logicmgr)
{
  /* Calculate if a __triggering__ is wanted
   * don't evaluate a sensor that is not connected to any controller
   */
  if (m_links && !m_suspended) {
    bool result = this->Evaluate();
    // store the state for the rest of the logic system
    m_prev_state = m_state;
    m_state = this->IsPositiveTrigger();
    if (result) {
      // the sensor triggered this frame
      if (m_state || !m_tap) {
        ActivateControllers(logicmgr);
        // reset these counters so that pulse are synchronized with transition
        m_pos_ticks = 0;
        m_neg_ticks = 0;
      }
      else {
        result = false;
      }
    }
    else {
      /* First, the pulsing behavior, if pulse mode is
       * active. It seems something goes wrong if pulse mode is
       * not set :( */
      if (m_pos_pulsemode) {
        m_pos_ticks++;
        if (m_pos_ticks > m_skipped_ticks) {
          if (m_state) {
            ActivateControllers(logicmgr);
            result = true;
          }
          m_pos_ticks = 0;
        }
      }
      // negative pulse doesn't make sense in tap mode, skip
      if (m_neg_pulsemode && !m_tap) {
        m_neg_ticks++;
        if (m_neg_ticks > m_skipped_ticks) {
          if (!m_state) {
            ActivateControllers(logicmgr);
            result = true;
          }
          m_neg_ticks = 0;
        }
      }
    }
    if (m_tap) {
      // in tap mode: we send always a negative pulse immediately after a positive pulse
      if (!result) {
        // the sensor did not trigger on this frame
        if (m_prev_state) {
          // but it triggered on previous frame => send a negative pulse
          ActivateControllers(logicmgr);
          result = true;
        }
        // in any case, absence of trigger means sensor off
        m_state = false;
      }
    }
    if (!result && m_level) {
      // This level sensor is connected to at least one controller that was just made
      // active but it did not generate an event yet, do it now to those controllers only
      for (SCA_IController *controller : m_linkedcontrollers) {
        if (controller->IsJustActivated()) {
          logicmgr->AddTriggeredController(controller, this);
        }
      }
    }
  }
}

#ifdef WITH_PYTHON

/* ----------------------------------------------- */
/* Python Functions						           */
/* ----------------------------------------------- */

EXP_PYMETHODDEF_DOC_NOARGS(
    SCA_ISensor,
    reset,
    "reset()\n"
    "\tReset sensor internal state, effect depends on the type of sensor and settings.\n"
    "\tThe sensor is put in its initial state as if it was just activated.\n")
{
  Init();
  m_prev_state = false;
  Py_RETURN_NONE;
}

/* ----------------------------------------------- */
/* Python Integration Hooks					       */
/* ----------------------------------------------- */

PyTypeObject SCA_ISensor::Type = {PyVarObject_HEAD_INIT(nullptr, 0) "SCA_ISensor",
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
                                  &SCA_ILogicBrick::Type,
                                  0,
                                  0,
                                  0,
                                  0,
                                  0,
                                  0,
                                  py_base_new};

PyMethodDef SCA_ISensor::Methods[] = {
    EXP_PYMETHODTABLE_NOARGS(SCA_ISensor, reset), {nullptr, nullptr}  // Sentinel
};

PyAttributeDef SCA_ISensor::Attributes[] = {
    EXP_PYATTRIBUTE_BOOL_RW("usePosPulseMode", SCA_ISensor, m_pos_pulsemode),
    EXP_PYATTRIBUTE_BOOL_RW("useNegPulseMode", SCA_ISensor, m_neg_pulsemode),
    EXP_PYATTRIBUTE_INT_RW("skippedTicks", 0, 100000, true, SCA_ISensor, m_skipped_ticks),
    EXP_PYATTRIBUTE_BOOL_RW("invert", SCA_ISensor, m_invert),
    EXP_PYATTRIBUTE_BOOL_RW_CHECK("level", SCA_ISensor, m_level, pyattr_check_level),
    EXP_PYATTRIBUTE_BOOL_RW_CHECK("tap", SCA_ISensor, m_tap, pyattr_check_tap),
    EXP_PYATTRIBUTE_RO_FUNCTION("triggered", SCA_ISensor, pyattr_get_triggered),
    EXP_PYATTRIBUTE_RO_FUNCTION("positive", SCA_ISensor, pyattr_get_positive),
    EXP_PYATTRIBUTE_RO_FUNCTION("status", SCA_ISensor, pyattr_get_status),
    EXP_PYATTRIBUTE_RO_FUNCTION("pos_ticks", SCA_ISensor, pyattr_get_posTicks),
    EXP_PYATTRIBUTE_RO_FUNCTION("neg_ticks", SCA_ISensor, pyattr_get_negTicks),
    EXP_PYATTRIBUTE_RW_FUNCTION(
        "frequency", SCA_ISensor, pyattr_get_frequency, pyattr_set_frequency),
    EXP_PYATTRIBUTE_NULL  // Sentinel
};

PyObject *SCA_ISensor::pyattr_get_triggered(EXP_PyObjectPlus *self_v,
                                            const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_ISensor *self = static_cast<SCA_ISensor *>(self_v);
  bool retval = false;
  if (SCA_PythonController::m_sCurrentController) {
    retval = SCA_PythonController::m_sCurrentController->IsTriggered(self);
  }
  return PyBool_FromLong(retval);
}

PyObject *SCA_ISensor::pyattr_get_positive(EXP_PyObjectPlus *self_v,
                                           const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_ISensor *self = static_cast<SCA_ISensor *>(self_v);
  return PyBool_FromLong(self->GetState());
}

PyObject *SCA_ISensor::pyattr_get_status(EXP_PyObjectPlus *self_v,
                                         const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_ISensor *self = static_cast<SCA_ISensor *>(self_v);
  int status = KX_SENSOR_INACTIVE;
  if (self->GetState()) {
    if (self->GetState() == self->GetPrevState()) {
      status = KX_SENSOR_ACTIVE;
    }
    else {
      status = KX_SENSOR_JUST_ACTIVATED;
    }
  }
  else if (self->GetState() != self->GetPrevState()) {
    status = KX_SENSOR_JUST_DEACTIVATED;
  }
  return PyLong_FromLong(status);
}

PyObject *SCA_ISensor::pyattr_get_posTicks(EXP_PyObjectPlus *self_v,
                                           const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_ISensor *self = static_cast<SCA_ISensor *>(self_v);
  return PyLong_FromLong(self->GetPosTicks());
}

PyObject *SCA_ISensor::pyattr_get_negTicks(EXP_PyObjectPlus *self_v,
                                           const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_ISensor *self = static_cast<SCA_ISensor *>(self_v);
  return PyLong_FromLong(self->GetNegTicks());
}

int SCA_ISensor::pyattr_check_level(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_ISensor *self = static_cast<SCA_ISensor *>(self_v);
  if (self->m_level) {
    self->m_tap = false;
  }
  return 0;
}

int SCA_ISensor::pyattr_check_tap(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_ISensor *self = static_cast<SCA_ISensor *>(self_v);
  if (self->m_tap) {
    self->m_level = false;
  }
  return 0;
}

PyObject *SCA_ISensor::pyattr_get_frequency(EXP_PyObjectPlus *self_v,
                                            const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_ISensor *self = static_cast<SCA_ISensor *>(self_v);
  EXP_ShowDeprecationWarning("SCA_ISensor.frequency", "SCA_ISensor.skippedTicks");
  return PyLong_FromLong(self->m_skipped_ticks);
}

int SCA_ISensor::pyattr_set_frequency(EXP_PyObjectPlus *self_v,
                                      const struct EXP_PYATTRIBUTE_DEF *attrdef,
                                      PyObject *value)
{
  SCA_ISensor *self = static_cast<SCA_ISensor *>(self_v);
  EXP_ShowDeprecationWarning("SCA_ISensor.frequency", "SCA_ISensor.skippedTicks");
  if (PyLong_Check(value)) {
    self->m_skipped_ticks = PyLong_AsLong(value);
    return PY_SET_ATTR_SUCCESS;
  }
  else {
    PyErr_SetString(PyExc_TypeError, "sensor.frequency = int: Sensor, expected an integer");
    return PY_SET_ATTR_FAIL;
  }
}
#endif  // WITH_PYTHON
