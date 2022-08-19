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

/** \file gameengine/GameLogic/SCA_JoystickSensor.cpp
 *  \ingroup gamelogic
 */

#include "SCA_JoystickSensor.h"

#include "CM_Message.h"
#include "SCA_JoystickManager.h"

#include "BLI_compiler_attrs.h"

SCA_JoystickSensor::SCA_JoystickSensor(class SCA_JoystickManager *eventmgr,
                                       SCA_IObject *gameobj,
                                       short int joyindex,
                                       short int joymode,
                                       int axis,
                                       int axisf,
                                       int prec,
                                       int button,
                                       bool allevents)
    : SCA_ISensor(gameobj, eventmgr),
      m_axis(axis),
      m_axisf(axisf),
      m_button(button),
      m_precision(prec),
      m_joymode(joymode),
      m_joyindex(joyindex),
      m_bAllEvents(allevents)
{
  Init();
}

void SCA_JoystickSensor::Init()
{
  m_istrig = (m_invert) ? 1 : 0;
  m_istrig_prev = 0;
  m_reset = true;
}

SCA_JoystickSensor::~SCA_JoystickSensor()
{
}

EXP_Value *SCA_JoystickSensor::GetReplica()
{
  SCA_JoystickSensor *replica = new SCA_JoystickSensor(*this);
  // this will copy properties and so on...
  replica->ProcessReplica();
  replica->Init();
  return replica;
}

bool SCA_JoystickSensor::IsPositiveTrigger()
{
  bool result = m_istrig;
  if (m_invert) {
    result = !result;
  }
  return result;
}

bool SCA_JoystickSensor::Evaluate()
{
  DEV_Joystick *js = ((SCA_JoystickManager *)m_eventmgr)->GetJoystickDevice(m_joyindex);
  bool result = false;
  bool reset = m_reset && m_level;
  int axis_single_index = m_axis;

  if (js == nullptr) { /* no joystick - don't do anything */
    return false;
  }

  m_reset = false;

  switch (m_joymode) {
    case KX_JOYSENSORMODE_AXIS: {
      /* what is what!
       *  m_axisf == JOYAXIS_RIGHT, JOYAXIS_UP, JOYAXIS_DOWN, JOYAXIS_LEFT
       *  m_axisf == 1 == up
       *  m_axisf == 2 == left
       *  m_axisf == 3 == down
       *
       *  numberof== m_axis (1-4), range is half of JOYAXIS_MAX since
       *      it assumes the axis joysticks are axis parirs (0,1), (2,3), etc
       *      also note that this starts at 1 where functions its used
       *      with expect a zero index.
       */

      if (!js->IsTrigAxis() && !reset) { /* No events from SDL? - don't bother */
        return false;
      }

      js->cSetPrecision(m_precision);
      if (m_bAllEvents) {
        if (js->aAxisPairIsPositive(m_axis - 1)) { /* use zero based axis index internally */
          m_istrig = 1;
          result = true;
        }
        else {
          if (m_istrig) {
            m_istrig = 0;
            result = true;
          }
        }
      }
      else {
        if (js->aAxisPairDirectionIsPositive(m_axis - 1,
                                             m_axisf)) { /* use zero based axis index internally */
          m_istrig = 1;
          result = true;
        }
        else {
          if (m_istrig) {
            m_istrig = 0;
            result = true;
          }
        }
      }
      break;
    }
    case KX_JOYSENSORMODE_SHOULDER_TRIGGER: {
      axis_single_index = m_axis + 4;
    }
      ATTR_FALLTHROUGH;
    case KX_JOYSENSORMODE_AXIS_SINGLE: {
      /* Like KX_JOYSENSORMODE_AXIS but don't pair up axis */
      if (!js->IsTrigAxis() && !reset) { /* No events from SDL? - don't bother */
        return false;
      }

      /* No need for 'm_bAllEvents' check here since were only checking 1 axis */
      js->cSetPrecision(m_precision);
      if (js->aAxisIsPositive(axis_single_index - 1)) { /* use zero based axis index internally */
        m_istrig = 1;
        result = true;
      }
      else {
        if (m_istrig) {
          m_istrig = 0;
          result = true;
        }
      }
      break;
    }
    case KX_JOYSENSORMODE_BUTTON: {
      /* what is what!
       *  m_button = the actual button in question
       */
      if (!js->IsTrigButton() && !reset) { /* No events from SDL? - don't bother */
        return false;
      }

      if ((m_bAllEvents && js->aAnyButtonPressIsPositive()) ||
          (!m_bAllEvents && js->aButtonPressIsPositive(m_button))) {
        m_istrig = 1;
        result = true;
      }
      else {
        if (m_istrig) {
          m_istrig = 0;
          result = true;
        }
      }
      break;
    }
    /* test for ball anyone ?*/
    default: {
      CM_LogicBrickError(this, "invalid switch statement");
      break;
    }
  }

  /* if not all events are enabled, only send a positive pulse when
   * the button state changes */
  if (!m_bAllEvents) {
    if (m_istrig_prev == m_istrig) {
      result = false;
    }
    else {
      m_istrig_prev = m_istrig;
    }
  }

  if (reset) {
    result = true;
  }

  return result;
}

#ifdef WITH_PYTHON

/* ------------------------------------------------------------------------- */
/* Python functions                                                          */
/* ------------------------------------------------------------------------- */

/* Integration hooks ------------------------------------------------------- */
PyTypeObject SCA_JoystickSensor::Type = {PyVarObject_HEAD_INIT(nullptr, 0) "SCA_JoystickSensor",
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
                                         &SCA_ISensor::Type,
                                         0,
                                         0,
                                         0,
                                         0,
                                         0,
                                         0,
                                         py_base_new};

PyMethodDef SCA_JoystickSensor::Methods[] = {
    {"getButtonActiveList",
     (PyCFunction)SCA_JoystickSensor::sPyGetButtonActiveList,
     METH_NOARGS,
     (const char *)GetButtonActiveList_doc},
    {"getButtonStatus",
     (PyCFunction)SCA_JoystickSensor::sPyGetButtonStatus,
     METH_VARARGS,
     (const char *)GetButtonStatus_doc},
    {nullptr, nullptr}  // Sentinel
};

PyAttributeDef SCA_JoystickSensor::Attributes[] = {
    EXP_PYATTRIBUTE_SHORT_RW("index", 0, JOYINDEX_MAX - 1, true, SCA_JoystickSensor, m_joyindex),
    EXP_PYATTRIBUTE_INT_RW("threshold", 0, 32768, true, SCA_JoystickSensor, m_precision),
    EXP_PYATTRIBUTE_INT_RW(
        "button", 0, KX_JOYSENS_BUTTON_MAX - 1, false, SCA_JoystickSensor, m_button),
    EXP_PYATTRIBUTE_INT_LIST_RW_CHECK(
        "axis", 0, 3, true, SCA_JoystickSensor, m_axis, 2, CheckAxis),
    EXP_PYATTRIBUTE_RO_FUNCTION("hat", SCA_JoystickSensor, pyattr_check_hat),
    EXP_PYATTRIBUTE_RO_FUNCTION("axisValues", SCA_JoystickSensor, pyattr_get_axis_values),
    EXP_PYATTRIBUTE_RO_FUNCTION("axisSingle", SCA_JoystickSensor, pyattr_get_axis_single),
    EXP_PYATTRIBUTE_RO_FUNCTION("hatValues", SCA_JoystickSensor, pyattr_get_hat_values),
    EXP_PYATTRIBUTE_RO_FUNCTION("hatSingle", SCA_JoystickSensor, pyattr_get_hat_single),
    EXP_PYATTRIBUTE_RO_FUNCTION("numAxis", SCA_JoystickSensor, pyattr_get_num_axis),
    EXP_PYATTRIBUTE_RO_FUNCTION("numButtons", SCA_JoystickSensor, pyattr_get_num_buttons),
    EXP_PYATTRIBUTE_RO_FUNCTION("numHats", SCA_JoystickSensor, pyattr_get_num_hats),
    EXP_PYATTRIBUTE_RO_FUNCTION("connected", SCA_JoystickSensor, pyattr_get_connected),
    EXP_PYATTRIBUTE_NULL  // Sentinel
};

/* get button active list  -------------------------------------------------- */
const char SCA_JoystickSensor::GetButtonActiveList_doc[] =
    "getButtonActiveList\n"
    "\tReturns a list containing the indices of the button currently pressed.\n";
PyObject *SCA_JoystickSensor::PyGetButtonActiveList()
{
  DEV_Joystick *joy = ((SCA_JoystickManager *)m_eventmgr)->GetJoystickDevice(m_joyindex);
  PyObject *ls = PyList_New(0);
  PyObject *value;
  int i;

  if (joy) {
    for (i = 0; i < JOYBUT_MAX; i++) {
      if (joy->aButtonPressIsPositive(i)) {
        value = PyLong_FromLong(i);
        PyList_Append(ls, value);
        Py_DECREF(value);
      }
    }
  }
  return ls;
}

/* get button status  -------------------------------------------------- */
const char SCA_JoystickSensor::GetButtonStatus_doc[] =
    "getButtonStatus(buttonIndex)\n"
    "\tReturns a bool of the current pressed state of the specified button.\n";
PyObject *SCA_JoystickSensor::PyGetButtonStatus(PyObject *args)
{
  DEV_Joystick *joy = ((SCA_JoystickManager *)m_eventmgr)->GetJoystickDevice(m_joyindex);
  int index;

  if (!PyArg_ParseTuple(args, "i:getButtonStatus", &index)) {
    return nullptr;
  }
  if (joy && index >= 0 && index < JOYBUT_MAX) {
    return PyBool_FromLong(joy->aButtonPressIsPositive(index) ? 1 : 0);
  }
  return PyBool_FromLong(0);
}

PyObject *SCA_JoystickSensor::pyattr_get_axis_values(EXP_PyObjectPlus *self_v,
                                                     const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_JoystickSensor *self = static_cast<SCA_JoystickSensor *>(self_v);
  DEV_Joystick *joy =
      ((SCA_JoystickManager *)self->m_eventmgr)->GetJoystickDevice(self->m_joyindex);

  int axis_index = (joy ? JOYAXIS_MAX : 0);
  PyObject *list = PyList_New(axis_index);

  while (axis_index--) {
    PyList_SET_ITEM(list, axis_index, PyLong_FromLong(joy->GetAxisPosition(axis_index)));
  }

  return list;
}

PyObject *SCA_JoystickSensor::pyattr_get_axis_single(EXP_PyObjectPlus *self_v,
                                                     const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_JoystickSensor *self = static_cast<SCA_JoystickSensor *>(self_v);
  DEV_Joystick *joy =
      ((SCA_JoystickManager *)self->m_eventmgr)->GetJoystickDevice(self->m_joyindex);

  if (self->m_joymode != KX_JOYSENSORMODE_AXIS_SINGLE) {
    PyErr_SetString(PyExc_AttributeError,
                    "val = sensor.axisSingle: Joystick Sensor, not 'Single Axis' type");
    return nullptr;
  }

  return PyLong_FromLong(joy ? joy->GetAxisPosition(self->m_axis - 1) : 0);
}

PyObject *SCA_JoystickSensor::pyattr_check_hat(EXP_PyObjectPlus *self_v,
                                               const EXP_PYATTRIBUTE_DEF *attrdef)
{
  EXP_ShowDeprecationWarning("SCA_JoystickSensor.hat", "SCA_JoystickSensor.button");
  return nullptr;
}

PyObject *SCA_JoystickSensor::pyattr_get_hat_values(EXP_PyObjectPlus *self_v,
                                                    const EXP_PYATTRIBUTE_DEF *attrdef)
{
  EXP_ShowDeprecationWarning("SCA_JoystickSensor.hat", "SCA_JoystickSensor.button");
  return nullptr;
}

PyObject *SCA_JoystickSensor::pyattr_get_hat_single(EXP_PyObjectPlus *self_v,
                                                    const EXP_PYATTRIBUTE_DEF *attrdef)
{
  EXP_ShowDeprecationWarning("SCA_JoystickSensor.hatSingle", "SCA_JoystickSensor.button");
  return nullptr;
}

PyObject *SCA_JoystickSensor::pyattr_get_num_axis(EXP_PyObjectPlus *self_v,
                                                  const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_JoystickSensor *self = static_cast<SCA_JoystickSensor *>(self_v);
  DEV_Joystick *joy =
      ((SCA_JoystickManager *)self->m_eventmgr)->GetJoystickDevice(self->m_joyindex);
  return PyLong_FromLong(joy ? JOYAXIS_MAX : 0);
}

PyObject *SCA_JoystickSensor::pyattr_get_num_buttons(EXP_PyObjectPlus *self_v,
                                                     const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_JoystickSensor *self = static_cast<SCA_JoystickSensor *>(self_v);
  DEV_Joystick *joy =
      ((SCA_JoystickManager *)self->m_eventmgr)->GetJoystickDevice(self->m_joyindex);
  return PyLong_FromLong(joy ? JOYBUT_MAX : 0);
}

PyObject *SCA_JoystickSensor::pyattr_get_num_hats(EXP_PyObjectPlus *self_v,
                                                  const EXP_PYATTRIBUTE_DEF *attrdef)
{
  EXP_ShowDeprecationWarning("SCA_JoystickSensor.numHats", "SCA_JoystickSensor.numButtons");
  return nullptr;
}

PyObject *SCA_JoystickSensor::pyattr_get_connected(EXP_PyObjectPlus *self_v,
                                                   const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_JoystickSensor *self = static_cast<SCA_JoystickSensor *>(self_v);
  DEV_Joystick *joy =
      ((SCA_JoystickManager *)self->m_eventmgr)->GetJoystickDevice(self->m_joyindex);
  return PyBool_FromLong(joy ? joy->Connected() : 0);
}

#endif
