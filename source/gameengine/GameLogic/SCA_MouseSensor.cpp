/*
 * Sensor for mouse input
 *
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
 * Contributor(s): José I. Romero (cleanup and fixes)
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/GameLogic/SCA_MouseSensor.cpp
 *  \ingroup gamelogic
 */

#include "SCA_MouseSensor.h"

#include "SCA_MouseManager.h"

#include "BLI_compiler_attrs.h"

/* ------------------------------------------------------------------------- */
/* Native functions                                                          */
/* ------------------------------------------------------------------------- */

SCA_MouseSensor::SCA_MouseSensor(
    SCA_MouseManager *eventmgr, int startx, int starty, short int mousemode, SCA_IObject *gameobj)
    : SCA_ISensor(gameobj, eventmgr), m_x(startx), m_y(starty)
{
  m_mousemode = mousemode;
  m_triggermode = true;

  Init();
}

void SCA_MouseSensor::Init()
{
  m_val = (m_invert) ? 1 : 0; /* stores the latest attribute */
  m_reset = true;
}

SCA_MouseSensor::~SCA_MouseSensor()
{
  /* Nothing to be done here. */
}

EXP_Value *SCA_MouseSensor::GetReplica()
{
  SCA_MouseSensor *replica = new SCA_MouseSensor(*this);
  // this will copy properties and so on...
  replica->ProcessReplica();
  replica->Init();

  return replica;
}

bool SCA_MouseSensor::IsPositiveTrigger()
{
  bool result = (m_val != 0);
  if (m_invert)
    result = !result;

  return result;
}

bool SCA_MouseSensor::Evaluate()
{
  bool result = false;
  bool reset = m_reset && m_level;
  int previousval = m_val;
  bool forceevent = false;
  SCA_IInputDevice *mousedev = ((SCA_MouseManager *)m_eventmgr)->GetInputDevice();

  m_reset = false;
  switch (m_mousemode) {
    case KX_MOUSESENSORMODE_WHEELUP:
    case KX_MOUSESENSORMODE_WHEELDOWN: {
      forceevent = true;
      ATTR_FALLTHROUGH;
      /* pass-through */
    }
    case KX_MOUSESENSORMODE_LEFTBUTTON:
    case KX_MOUSESENSORMODE_MIDDLEBUTTON:
    case KX_MOUSESENSORMODE_RIGHTBUTTON:
    case KX_MOUSESENSORMODE_BUTTON4:
    case KX_MOUSESENSORMODE_BUTTON5:
    case KX_MOUSESENSORMODE_BUTTON6:
    case KX_MOUSESENSORMODE_BUTTON7: {
      static const SCA_IInputDevice::SCA_EnumInputs convertTable[KX_MOUSESENSORMODE_MAX] = {
          SCA_IInputDevice::NOKEY,          // KX_MOUSESENSORMODE_NODEF
          SCA_IInputDevice::LEFTMOUSE,      // KX_MOUSESENSORMODE_LEFTBUTTON
          SCA_IInputDevice::MIDDLEMOUSE,    // KX_MOUSESENSORMODE_MIDDLEBUTTON
          SCA_IInputDevice::RIGHTMOUSE,     // KX_MOUSESENSORMODE_RIGHTBUTTON
          SCA_IInputDevice::BUTTON4MOUSE,   // KX_MOUSESENSORMODE_BUTTON4
          SCA_IInputDevice::BUTTON5MOUSE,   // KX_MOUSESENSORMODE_BUTTON5
          SCA_IInputDevice::BUTTON6MOUSE,   // KX_MOUSESENSORMODE_BUTTON6
          SCA_IInputDevice::BUTTON7MOUSE,   // KX_MOUSESENSORMODE_BUTTON7
          SCA_IInputDevice::WHEELUPMOUSE,   // KX_MOUSESENSORMODE_WHEELUP
          SCA_IInputDevice::WHEELDOWNMOUSE  // KX_MOUSESENSORMODE_WHEELDOWN
      };

      const SCA_InputEvent &mevent = mousedev->GetInput(convertTable[m_mousemode]);
      if (mevent.Find(SCA_InputEvent::ACTIVE)) {
        m_val = 1;
        if (forceevent) {
          result = true;
        }
      }
      else {
        m_val = 0;
      }
      break;
    }
    case KX_MOUSESENSORMODE_MOVEMENT: {
      const SCA_InputEvent &eventX = mousedev->GetInput(SCA_IInputDevice::MOUSEX);
      const SCA_InputEvent &eventY = mousedev->GetInput(SCA_IInputDevice::MOUSEY);

      if (eventX.Find(SCA_InputEvent::ACTIVE) || eventY.Find(SCA_InputEvent::ACTIVE) ||
          eventX.Find(SCA_InputEvent::JUSTACTIVATED) || eventY.Find(SCA_InputEvent::JUSTACTIVATED) ||
          eventX.Find(SCA_InputEvent::JUSTRELEASED) || eventY.Find(SCA_InputEvent::JUSTRELEASED)) {
        m_val = 1;
      }
      else {
        m_val = 0;
      }
      break;
    }
    default:; /* error */
  }

  if (previousval != m_val) {
    result = true;
  }

  if (reset)
    // force an event
    result = true;
  return result;
}

void SCA_MouseSensor::setX(short x)
{
  m_x = x;
}

void SCA_MouseSensor::setY(short y)
{
  m_y = y;
}

#ifdef WITH_PYTHON

/* ------------------------------------------------------------------------- */
/* Python functions                                                          */
/* ------------------------------------------------------------------------- */

EXP_PYMETHODDEF_DOC_O(SCA_MouseSensor,
                      getButtonStatus,
                      "getButtonStatus(button)\n"
                      "\tGet the given button's status (KX_INPUT_NONE, KX_INPUT_NONE, "
                      "KX_INPUT_JUST_ACTIVATED, KX_INPUT_ACTIVE, KX_INPUT_JUST_RELEASED).\n")
{
  EXP_ShowDeprecationWarning("sensor.getButtonStatus(button)", "logic.mouse.events[button]");

  if (PyLong_Check(value)) {
    SCA_IInputDevice::SCA_EnumInputs button = (SCA_IInputDevice::SCA_EnumInputs)PyLong_AsLong(
        value);

    if ((button < SCA_IInputDevice::LEFTMOUSE) || (button > SCA_IInputDevice::RIGHTMOUSE)) {
      PyErr_SetString(PyExc_ValueError,
                      "sensor.getButtonStatus(int): Mouse Sensor, invalid button specified!");
      return nullptr;
    }

    SCA_IInputDevice *mousedev = ((SCA_MouseManager *)m_eventmgr)->GetInputDevice();
    const SCA_InputEvent &event = mousedev->GetInput(button);
    return PyLong_FromLong(event.m_status[event.m_status.size() - 1]);
  }

  Py_RETURN_NONE;
}

/* ------------------------------------------------------------------------- */
/* Python Integration Hooks                                                  */
/* ------------------------------------------------------------------------- */

PyTypeObject SCA_MouseSensor::Type = {PyVarObject_HEAD_INIT(nullptr, 0) "SCA_MouseSensor",
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

PyMethodDef SCA_MouseSensor::Methods[] = {
    EXP_PYMETHODTABLE_O(SCA_MouseSensor, getButtonStatus), {nullptr, nullptr}  // Sentinel
};

PyAttributeDef SCA_MouseSensor::Attributes[] = {
    EXP_PYATTRIBUTE_SHORT_RW("mode",
                             KX_MOUSESENSORMODE_NODEF,
                             KX_MOUSESENSORMODE_MAX - 1,
                             true,
                             SCA_MouseSensor,
                             m_mousemode),
    EXP_PYATTRIBUTE_SHORT_LIST_RO("position", SCA_MouseSensor, m_x, 2),
    EXP_PYATTRIBUTE_NULL  // Sentinel
};

#endif  // WITH_PYTHON

/* eof */
