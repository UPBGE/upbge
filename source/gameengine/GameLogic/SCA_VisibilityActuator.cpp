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
 * Actuator to toggle visibility/invisibility of objects
 */

/** \file gameengine/Ketsji/SCA_VisibilityActuator.cpp
 *  \ingroup ketsji
 */

#include "SCA_VisibilityActuator.h"

#include "KX_GameObject.h"

SCA_VisibilityActuator::SCA_VisibilityActuator(SCA_IObject *gameobj,
                                               bool visible,
                                               bool occlusion,
                                               bool recursive)
    : SCA_IActuator(gameobj, KX_ACT_VISIBILITY),
      m_visible(visible),
      m_occlusion(occlusion),
      m_recursive(recursive)
{
  // intentionally empty
}

SCA_VisibilityActuator::~SCA_VisibilityActuator(void)
{
  // intentionally empty
}

EXP_Value *SCA_VisibilityActuator::GetReplica(void)
{
  SCA_VisibilityActuator *replica = new SCA_VisibilityActuator(*this);
  replica->ProcessReplica();
  return replica;
}

bool SCA_VisibilityActuator::Update()
{
  bool bNegativeEvent = IsNegativeEvent();

  RemoveAllEvents();
  if (bNegativeEvent)
    return false;

  KX_GameObject *obj = (KX_GameObject *)GetParent();

  obj->SetVisible(m_visible, m_recursive);
  obj->SetOccluder(m_occlusion, m_recursive);

  return false;
}

#ifdef WITH_PYTHON

/* ------------------------------------------------------------------------- */
/* Python functions                                                          */
/* ------------------------------------------------------------------------- */

/* Integration hooks ------------------------------------------------------- */
PyTypeObject SCA_VisibilityActuator::Type = {
    PyVarObject_HEAD_INIT(nullptr, 0) "SCA_VisibilityActuator",
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

PyMethodDef SCA_VisibilityActuator::Methods[] = {
    {nullptr, nullptr}  // Sentinel
};

PyAttributeDef SCA_VisibilityActuator::Attributes[] = {
    EXP_PYATTRIBUTE_BOOL_RW("visibility", SCA_VisibilityActuator, m_visible),
    EXP_PYATTRIBUTE_BOOL_RW("useOcclusion", SCA_VisibilityActuator, m_occlusion),
    EXP_PYATTRIBUTE_BOOL_RW("useRecursion", SCA_VisibilityActuator, m_recursive),
    EXP_PYATTRIBUTE_NULL  // Sentinel
};

#endif  // WITH_PYTHON
