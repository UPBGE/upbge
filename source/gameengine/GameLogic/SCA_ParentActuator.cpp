/*
 * Set or remove an objects parent
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

/** \file gameengine/Ketsji/SCA_ParentActuator.cpp
 *  \ingroup ketsji
 */

#include "SCA_ParentActuator.h"

#include "KX_GameObject.h"

/* ------------------------------------------------------------------------- */
/* Native functions                                                          */
/* ------------------------------------------------------------------------- */

SCA_ParentActuator::SCA_ParentActuator(
    SCA_IObject *gameobj, int mode, bool addToCompound, bool ghost, SCA_IObject *ob)
    : SCA_IActuator(gameobj, KX_ACT_PARENT),
      m_mode(mode),
      m_addToCompound(addToCompound),
      m_ghost(ghost),
      m_ob(ob)
{
  if (m_ob)
    m_ob->RegisterActuator(this);
}

SCA_ParentActuator::~SCA_ParentActuator()
{
  if (m_ob)
    m_ob->UnregisterActuator(this);
}

EXP_Value *SCA_ParentActuator::GetReplica()
{
  SCA_ParentActuator *replica = new SCA_ParentActuator(*this);
  // replication just copy the m_base pointer => common random generator
  replica->ProcessReplica();
  return replica;
}

void SCA_ParentActuator::ProcessReplica()
{
  if (m_ob)
    m_ob->RegisterActuator(this);
  SCA_IActuator::ProcessReplica();
}

bool SCA_ParentActuator::UnlinkObject(SCA_IObject *clientobj)
{
  if (clientobj == m_ob) {
    // this object is being deleted, we cannot continue to track it.
    m_ob = nullptr;
    return true;
  }
  return false;
}

void SCA_ParentActuator::Relink(std::map<SCA_IObject *, SCA_IObject *> &obj_map)
{
  SCA_IObject *obj = obj_map[m_ob];
  if (obj) {
    if (m_ob)
      m_ob->UnregisterActuator(this);
    m_ob = obj;
    m_ob->RegisterActuator(this);
  }
}

bool SCA_ParentActuator::Update()
{
  bool bNegativeEvent = IsNegativeEvent();
  RemoveAllEvents();

  if (bNegativeEvent)
    return false;  // do nothing on negative events

  KX_GameObject *obj = (KX_GameObject *)GetParent();
  switch (m_mode) {
    case KX_PARENT_SET:
      if (m_ob)
        obj->SetParent((KX_GameObject *)m_ob, m_addToCompound, m_ghost);
      break;
    case KX_PARENT_REMOVE:
      obj->RemoveParent();
      break;
  };

  return false;
}

#ifdef WITH_PYTHON

/* ------------------------------------------------------------------------- */
/* Python functions                                                          */
/* ------------------------------------------------------------------------- */

/* Integration hooks ------------------------------------------------------- */
PyTypeObject SCA_ParentActuator::Type = {PyVarObject_HEAD_INIT(nullptr, 0) "SCA_ParentActuator",
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

PyMethodDef SCA_ParentActuator::Methods[] = {
    {nullptr, nullptr}  // Sentinel
};

PyAttributeDef SCA_ParentActuator::Attributes[] = {
    EXP_PYATTRIBUTE_RW_FUNCTION(
        "object", SCA_ParentActuator, pyattr_get_object, pyattr_set_object),
    EXP_PYATTRIBUTE_INT_RW(
        "mode", KX_PARENT_NODEF + 1, KX_PARENT_MAX - 1, true, SCA_ParentActuator, m_mode),
    EXP_PYATTRIBUTE_BOOL_RW("compound", SCA_ParentActuator, m_addToCompound),
    EXP_PYATTRIBUTE_BOOL_RW("ghost", SCA_ParentActuator, m_ghost),
    EXP_PYATTRIBUTE_NULL  // Sentinel
};

PyObject *SCA_ParentActuator::pyattr_get_object(EXP_PyObjectPlus *self,
                                                const struct EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_ParentActuator *actuator = static_cast<SCA_ParentActuator *>(self);
  if (!actuator->m_ob)
    Py_RETURN_NONE;
  else
    return actuator->m_ob->GetProxy();
}

int SCA_ParentActuator::pyattr_set_object(EXP_PyObjectPlus *self,
                                          const struct EXP_PYATTRIBUTE_DEF *attrdef,
                                          PyObject *value)
{
  SCA_ParentActuator *actuator = static_cast<SCA_ParentActuator *>(self);
  KX_GameObject *gameobj;

  if (!ConvertPythonToGameObject(actuator->GetLogicManager(),
                                 value,
                                 &gameobj,
                                 true,
                                 "actuator.object = value: SCA_ParentActuator"))
    return PY_SET_ATTR_FAIL;  // ConvertPythonToGameObject sets the error

  if (actuator->m_ob != nullptr)
    actuator->m_ob->UnregisterActuator(actuator);

  actuator->m_ob = (SCA_IObject *)gameobj;

  if (actuator->m_ob)
    actuator->m_ob->RegisterActuator(actuator);

  return PY_SET_ATTR_SUCCESS;
}

#endif  // WITH_PYTHON

/* eof */
