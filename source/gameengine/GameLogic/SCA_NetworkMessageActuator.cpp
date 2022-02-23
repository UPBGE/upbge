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
 * Ketsji Logic Extension: Network Message Actuator generic implementation
 */

/** \file gameengine/Ketsji/KXNetwork/SCA_NetworkMessageActuator.cpp
 *  \ingroup ketsjinet
 */

#include "SCA_NetworkMessageActuator.h"

#include "KX_NetworkMessageScene.h"

SCA_NetworkMessageActuator::SCA_NetworkMessageActuator(
    SCA_IObject *gameobj,                  // the actuator controlling object
    KX_NetworkMessageScene *networkscene,  // needed for replication
    const std::string &toPropName,
    const std::string &subject,
    int bodyType,
    const std::string &body)
    : SCA_IActuator(gameobj, KX_ACT_MESSAGE),
      m_networkscene(networkscene),
      m_toPropName(toPropName),
      m_subject(subject),
      m_bPropBody(bodyType),
      m_body(body)
{
}

SCA_NetworkMessageActuator::~SCA_NetworkMessageActuator()
{
}

/// returns true if the actuators needs to be running over several frames
bool SCA_NetworkMessageActuator::Update()
{
  bool bNegativeEvent = IsNegativeEvent();
  RemoveAllEvents();

  if (bNegativeEvent) {
    return false;  // do nothing on negative events
  }

  // ACT_MESG_PROP in DNA_actuator_types.h
  if (m_bPropBody) {
    m_networkscene->SendMessage(
        m_toPropName, GetParent(), m_subject, GetParent()->GetPropertyText(m_body));
  }
  else {
    m_networkscene->SendMessage(m_toPropName, GetParent(), m_subject, m_body);
  }
  return false;
}

EXP_Value *SCA_NetworkMessageActuator::GetReplica()
{
  SCA_NetworkMessageActuator *replica = new SCA_NetworkMessageActuator(*this);
  replica->ProcessReplica();

  return replica;
}

#ifdef WITH_PYTHON

/* -------------------------------------------------------------------- */
/* Python interface --------------------------------------------------- */
/* -------------------------------------------------------------------- */

/* Integration hooks -------------------------------------------------- */
PyTypeObject SCA_NetworkMessageActuator::Type = {
    PyVarObject_HEAD_INIT(nullptr, 0) "SCA_NetworkMessageActuator",
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

PyMethodDef SCA_NetworkMessageActuator::Methods[] = {
    {nullptr, nullptr}  // Sentinel
};

PyAttributeDef SCA_NetworkMessageActuator::Attributes[] = {
    EXP_PYATTRIBUTE_STRING_RW(
        "propName", 0, MAX_PROP_NAME, false, SCA_NetworkMessageActuator, m_toPropName),
    EXP_PYATTRIBUTE_STRING_RW("subject", 0, 100, false, SCA_NetworkMessageActuator, m_subject),
    EXP_PYATTRIBUTE_BOOL_RW("usePropBody", SCA_NetworkMessageActuator, m_bPropBody),
    EXP_PYATTRIBUTE_STRING_RW("body", 0, 16384, false, SCA_NetworkMessageActuator, m_body),
    EXP_PYATTRIBUTE_NULL  // Sentinel
};

#endif  // WITH_PYTHON
