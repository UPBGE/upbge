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
 * Contributor(s): lordloki.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/GameLogic/SCA_LedActuator.cpp
 *  \ingroup GameLogic
 */

#include "SCA_LedActuator.h"

#include "DEV_Joystick.h"

SCA_LedActuator::SCA_LedActuator(SCA_IObject *gameobj, int joyindex, const float color[3])
    : SCA_IActuator(gameobj, KX_ACT_LED), m_joyindex(joyindex)
{
  m_color[0] = color[0];
  m_color[1] = color[1];
  m_color[2] = color[2];
}

SCA_LedActuator::~SCA_LedActuator() {}

EXP_Value *SCA_LedActuator::GetReplica()
{
  SCA_LedActuator *replica = new SCA_LedActuator(*this);
  replica->ProcessReplica();
  return replica;
}

/* [0, 1] float to a byte; NaN goes to 0 because every comparison with it is false. */
static unsigned char unit_to_byte(float v)
{
  return (unsigned char)((v > 0.0f) ? ((v < 1.0f) ? v * 255.0f + 0.5f : 255.0f) : 0.0f);
}

bool SCA_LedActuator::Update()
{
  const bool positive = IsPositiveEvent();
  RemoveAllEvents();

  DEV_Joystick *joystick = DEV_Joystick::GetInstance(m_joyindex);
  if (positive && joystick) {
    /* SetLED() does nothing on a controller without an LED, and SDL skips a write that repeats
     * the current color. */
    joystick->SetLED(unit_to_byte(m_color[0]), unit_to_byte(m_color[1]), unit_to_byte(m_color[2]));
  }
  return false;
}

#ifdef WITH_PYTHON

PyTypeObject SCA_LedActuator::Type = {PyVarObject_HEAD_INIT(nullptr, 0) "SCA_LedActuator",
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

PyMethodDef SCA_LedActuator::Methods[] = {
    {nullptr, nullptr}  // Sentinel
};

PyAttributeDef SCA_LedActuator::Attributes[] = {
    EXP_PYATTRIBUTE_INT_RW("joyindex", 0, JOYINDEX_MAX - 1, true, SCA_LedActuator, m_joyindex),
    EXP_PYATTRIBUTE_FLOAT_ARRAY_RW("color", 0.0f, 1.0f, SCA_LedActuator, m_color, 3),
    EXP_PYATTRIBUTE_NULL  // Sentinel
};

#endif  // WITH_PYTHON
