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

/** \file gameengine/GameLogic/SCA_VibrationActuator.cpp
 *  \ingroup GameLogic
 */

#include "SCA_VibrationActuator.h"

#include "SCA_JoystickManager.h"

SCA_VibrationActuator::SCA_VibrationActuator(SCA_IObject *gameobj,
                                             short mode,
                                             int joyindex,
                                             float strengthLeft,
                                             float strengthRight,
                                             int duration)
    : SCA_IActuator(gameobj, KX_ACT_VIBRATION),
      m_joyindex(joyindex),
      m_mode(mode),
      m_strengthLeft(strengthLeft),
      m_strengthRight(strengthRight),
      m_duration(duration)
{
}

SCA_VibrationActuator::~SCA_VibrationActuator(void)
{
}

EXP_Value *SCA_VibrationActuator::GetReplica(void)
{
  SCA_VibrationActuator *replica = new SCA_VibrationActuator(*this);
  replica->ProcessReplica();
  return replica;
}

bool SCA_VibrationActuator::Update()
{
  /* Can't init instance in constructor because m_joystick list is not available yet */
  SCA_JoystickManager *mgr = (SCA_JoystickManager *)GetLogicManager();
  DEV_Joystick *instance = mgr->GetJoystickDevice(m_joyindex);

  if (!instance) {
    return false;
  }

  bool bPositiveEvent = IsPositiveEvent();

  RemoveAllEvents();

  if (bPositiveEvent) {
    switch (m_mode) {
      case KX_ACT_VIBRATION_PLAY: {
        instance->RumblePlay(m_strengthLeft, m_strengthRight, m_duration);
        break;
      }
      case KX_ACT_VIBRATION_STOP: {
        instance->RumbleStop();
        break;
      }
    }
  }

  return false;
}

#ifdef WITH_PYTHON

/* ------------------------------------------------------------------------- */
/* Python functions                                                          */
/* ------------------------------------------------------------------------- */

/* Integration hooks ------------------------------------------------------- */
PyTypeObject SCA_VibrationActuator::Type = {
    PyVarObject_HEAD_INIT(nullptr, 0) "SCA_VibrationActuator",
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
    &SCA_IActuator::Type,
    0,
    0,
    0,
    0,
    0,
    0,
    py_base_new};

PyMethodDef SCA_VibrationActuator::Methods[] = {
    EXP_PYMETHODTABLE_NOARGS(SCA_VibrationActuator, startVibration),
    EXP_PYMETHODTABLE_NOARGS(SCA_VibrationActuator, stopVibration),
    {nullptr, nullptr}  // Sentinel
};

PyAttributeDef SCA_VibrationActuator::Attributes[] = {
    EXP_PYATTRIBUTE_INT_RW("duration", 0, INT_MAX, true, SCA_VibrationActuator, m_duration),
    EXP_PYATTRIBUTE_INT_RW("joyindex", 0, 7, true, SCA_VibrationActuator, m_joyindex),
    EXP_PYATTRIBUTE_FLOAT_RW("strengthLeft", 0.0, 1.0, SCA_VibrationActuator, m_strengthLeft),
    EXP_PYATTRIBUTE_FLOAT_RW("strengthRight", 0.0, 1.0, SCA_VibrationActuator, m_strengthRight),
    EXP_PYATTRIBUTE_RO_FUNCTION("isVibrating", SCA_VibrationActuator, pyattr_get_isVibrating),
    EXP_PYATTRIBUTE_RO_FUNCTION("hasVibration", SCA_VibrationActuator, pyattr_get_hasVibration),
    EXP_PYATTRIBUTE_NULL  // Sentinel
};

EXP_PYMETHODDEF_DOC_NOARGS(SCA_VibrationActuator,
                           startVibration,
                           "startVibration()\n"
                           "\tStarts the joystick vibration.\n")
{
  SCA_JoystickManager *mgr = (SCA_JoystickManager *)GetLogicManager();
  DEV_Joystick *instance = mgr->GetJoystickDevice(m_joyindex);

  if (!instance) {
    Py_RETURN_NONE;
  }

  instance->RumblePlay(m_strengthLeft, m_strengthRight, m_duration);

  Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC_NOARGS(SCA_VibrationActuator,
                           stopVibration,
                           "StopVibration()\n"
                           "\tStops the joystick vibration.\n")
{
  SCA_JoystickManager *mgr = (SCA_JoystickManager *)GetLogicManager();
  DEV_Joystick *instance = mgr->GetJoystickDevice(m_joyindex);

  if (!instance) {
    Py_RETURN_NONE;
  }

  instance->RumbleStop();

  Py_RETURN_NONE;
}

PyObject *SCA_VibrationActuator::pyattr_get_isVibrating(EXP_PyObjectPlus *self_v,
                                                        const struct EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_VibrationActuator *self = static_cast<SCA_VibrationActuator *>(self_v);
  SCA_JoystickManager *mgr = (SCA_JoystickManager *)self->GetLogicManager();
  DEV_Joystick *instance = mgr->GetJoystickDevice(self->m_joyindex);

  if (!instance) {
    return Py_False;
  }

  return PyBool_FromLong(instance->GetRumbleStatus());
}

PyObject *SCA_VibrationActuator::pyattr_get_hasVibration(EXP_PyObjectPlus *self_v,
                                                         const struct EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_VibrationActuator *self = static_cast<SCA_VibrationActuator *>(self_v);
  SCA_JoystickManager *mgr = (SCA_JoystickManager *)self->GetLogicManager();
  DEV_Joystick *instance = mgr->GetJoystickDevice(self->m_joyindex);

  if (!instance) {
    return Py_False;
  }

  return PyBool_FromLong(instance->GetRumbleSupport());
}

#endif  // WITH_PYTHON
