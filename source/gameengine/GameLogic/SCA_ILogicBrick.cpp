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

/** \file gameengine/GameLogic/SCA_ILogicBrick.cpp
 *  \ingroup gamelogic
 */

#include "SCA_ILogicBrick.h"

SCA_ILogicBrick::SCA_ILogicBrick(SCA_IObject *gameobj)
    : EXP_Value(),
      m_gameobj(gameobj),
      m_logicManager(nullptr),
      m_Execute_Priority(0),
      m_Execute_Ueber_Priority(0),
      m_bActive(false),
      m_eventval(0)
{
}

SCA_ILogicBrick::~SCA_ILogicBrick()
{
  RemoveEvent();
}

void SCA_ILogicBrick::SetExecutePriority(int execute_Priority)
{
  m_Execute_Priority = execute_Priority;
}

void SCA_ILogicBrick::SetUeberExecutePriority(int execute_Priority)
{
  m_Execute_Ueber_Priority = execute_Priority;
}

void SCA_ILogicBrick::ReParent(SCA_IObject *parent)
{
  m_gameobj = parent;
}

void SCA_ILogicBrick::Relink(std::map<SCA_IObject *, SCA_IObject *> &obj_map)
{
  // nothing to do
}

std::string SCA_ILogicBrick::GetName()
{
  return m_name;
}

void SCA_ILogicBrick::SetName(const std::string &name)
{
  m_name = name;
}

void SCA_ILogicBrick::SetLogicManager(SCA_LogicManager *logicmgr)
{
  m_logicManager = logicmgr;
}

SCA_LogicManager *SCA_ILogicBrick::GetLogicManager()
{
  return m_logicManager;
}

void SCA_ILogicBrick::RemoveEvent()
{
  if (m_eventval) {
    m_eventval->Release();
    m_eventval = nullptr;
  }
}

#ifdef WITH_PYTHON

/* python stuff */

PyTypeObject SCA_ILogicBrick::Type = {PyVarObject_HEAD_INIT(nullptr, 0) "SCA_ILogicBrick",
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

PyMethodDef SCA_ILogicBrick::Methods[] = {
    {nullptr, nullptr}  // Sentinel
};

PyAttributeDef SCA_ILogicBrick::Attributes[] = {
    EXP_PYATTRIBUTE_RO_FUNCTION("owner", SCA_ILogicBrick, pyattr_get_owner),
    EXP_PYATTRIBUTE_INT_RW(
        "executePriority", 0, 100000, false, SCA_ILogicBrick, m_Execute_Priority),
    EXP_PYATTRIBUTE_NULL  // Sentinel
};

int SCA_ILogicBrick::CheckProperty(EXP_PyObjectPlus *self, const PyAttributeDef *attrdef)
{
  if (attrdef->m_type != EXP_PYATTRIBUTE_TYPE_STRING || attrdef->m_length != 1) {
    PyErr_SetString(PyExc_AttributeError,
                    "inconsistent check function for attribute type, report to blender.org");
    return 1;
  }
  SCA_ILogicBrick *brick = reinterpret_cast<SCA_ILogicBrick *>(self);
  std::string *var = reinterpret_cast<std::string *>((char *)self + attrdef->m_offset);
  EXP_Value *prop = brick->GetParent()->FindIdentifier(*var);
  bool error = prop->IsError();
  prop->Release();
  if (error) {
    PyErr_SetString(PyExc_ValueError, "string does not correspond to a property");
    return 1;
  }
  return 0;
}

/*Attribute functions */
PyObject *SCA_ILogicBrick::pyattr_get_owner(EXP_PyObjectPlus *self_v,
                                            const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_ILogicBrick *self = static_cast<SCA_ILogicBrick *>(self_v);
  EXP_Value *parent = self->GetParent();

  if (parent)
    return parent->GetProxy();

  Py_RETURN_NONE;
}

#endif  // WITH_PYTHON
