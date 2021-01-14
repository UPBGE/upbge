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

/** \file gameengine/Ketsji/SCA_DynamicActuator.cpp
 *  \ingroup ketsji
 * Adjust dynamics settings for this object
 */

/* Previously existed as:
 * \source\gameengine\GameLogic\SCA_DynamicActuator.cpp
 * Please look here for revision history. */

#include "SCA_DynamicActuator.h"

#include "PHY_IPhysicsController.h"

#ifdef WITH_PYTHON

/* ------------------------------------------------------------------------- */
/* Python functions                                                          */
/* ------------------------------------------------------------------------- */

/* Integration hooks ------------------------------------------------------- */

PyTypeObject SCA_DynamicActuator::Type = {PyVarObject_HEAD_INIT(nullptr, 0) "SCA_DynamicActuator",
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

PyMethodDef SCA_DynamicActuator::Methods[] = {
    {nullptr, nullptr}  // Sentinel
};

PyAttributeDef SCA_DynamicActuator::Attributes[] = {
    EXP_PYATTRIBUTE_SHORT_RW("mode", 0, 4, false, SCA_DynamicActuator, m_dyn_operation),
    EXP_PYATTRIBUTE_FLOAT_RW("mass", 0.0f, FLT_MAX, SCA_DynamicActuator, m_setmass),
    EXP_PYATTRIBUTE_NULL  // Sentinel
};

#endif  // WITH_PYTHON

/* ------------------------------------------------------------------------- */
/* Native functions                                                          */
/* ------------------------------------------------------------------------- */

SCA_DynamicActuator::SCA_DynamicActuator(SCA_IObject *gameobj,
                                         short dyn_operation,
                                         float setmass,
                                         bool suspend_children_phys,
                                         bool restore_children_phys,
                                         bool suspend_constraint)
    :

      SCA_IActuator(gameobj, KX_ACT_DYNAMIC),
      m_dyn_operation(dyn_operation),
      m_setmass(setmass),
      m_suspend_children_phys(suspend_children_phys),
      m_restore_children_phys(restore_children_phys),
      m_suspend_constraints(suspend_constraint)
{
} /* End of constructor */

SCA_DynamicActuator::~SCA_DynamicActuator()
{
  // there's nothing to be done here, really....
} /* end of destructor */

bool SCA_DynamicActuator::Update()
{
  // bool result = false;	/*unused*/
  KX_GameObject *obj = (KX_GameObject *)GetParent();
  bool bNegativeEvent = IsNegativeEvent();
  PHY_IPhysicsController *controller;
  RemoveAllEvents();

  if (bNegativeEvent)
    return false;  // do nothing on negative events

  if (!obj)
    return false;  // object not accessible, shouldnt happen
  controller = obj->GetPhysicsController();
  if (!controller)
    return false;  // no physic object

  switch (m_dyn_operation) {
    case KX_DYN_RESTORE_DYNAMICS:
      // Child objects must be static, so we block changing to dynamic
      if (!obj->GetParent())
        controller->RestoreDynamics();
      break;
    case KX_DYN_DISABLE_DYNAMICS:
      controller->SuspendDynamics();
      break;
    case KX_DYN_ENABLE_RIGID_BODY:
      controller->SetRigidBody(true);
      break;
    case KX_DYN_DISABLE_RIGID_BODY:
      controller->SetRigidBody(false);
      break;
    case KX_DYN_SET_MASS:
      controller->SetMass(m_setmass);
      break;
    case KX_DYN_RESTORE_PHYSICS: {
      obj->RestorePhysics(m_restore_children_phys);
      break;
    }
    case KX_DYN_DISABLE_PHYSICS: {
      obj->SuspendPhysics(m_suspend_constraints, m_suspend_children_phys);
      break;
    }
  }

  return false;
}

EXP_Value *SCA_DynamicActuator::GetReplica()
{
  SCA_DynamicActuator *replica = new SCA_DynamicActuator(*this);

  if (replica == nullptr)
    return nullptr;

  replica->ProcessReplica();
  return replica;
};

/* eof */
