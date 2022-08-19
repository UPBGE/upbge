/*
 * Armature sensor
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

/** \file gameengine/Ketsji/SCA_ArmatureSensor.cpp
 *  \ingroup ketsji
 */

#include "SCA_ArmatureSensor.h"

#include "BL_ArmatureObject.h"

SCA_ArmatureSensor::SCA_ArmatureSensor(class SCA_EventManager *eventmgr,
                                       SCA_IObject *gameobj,
                                       const std::string &posechannel,
                                       const std::string &constraintname,
                                       int type,
                                       float value)
    : SCA_ISensor(gameobj, eventmgr),
      m_constraint(nullptr),
      m_posechannel(posechannel),
      m_constraintname(constraintname),
      m_type(type),
      m_value(value)
{
  FindConstraint();
}

void SCA_ArmatureSensor::Init()
{
  m_lastresult = m_invert ? true : false;
  m_result = false;
  m_reset = true;
}

void SCA_ArmatureSensor::FindConstraint()
{
  m_constraint = nullptr;

  if (m_gameobj->GetGameObjectType() == SCA_IObject::OBJ_ARMATURE) {
    BL_ArmatureObject *armobj = (BL_ArmatureObject *)m_gameobj;
    // get the persistent pose structure
    bPose *pose = armobj->GetPose();
    bPoseChannel *pchan;
    bConstraint *pcon;
    // and locate the constraint
    for (pchan = (bPoseChannel *)pose->chanbase.first; pchan;
         pchan = (bPoseChannel *)pchan->next) {
      if (pchan->name == m_posechannel) {
        // now locate the constraint
        for (pcon = (bConstraint *)pchan->constraints.first; pcon;
             pcon = (bConstraint *)pcon->next) {
          if (pcon->name == m_constraintname) {
            if (pcon->flag & CONSTRAINT_DISABLE)
              /* this constraint is not valid, can't use it */
              break;
            m_constraint = pcon;
            break;
          }
        }
        break;
      }
    }
  }
}

EXP_Value *SCA_ArmatureSensor::GetReplica()
{
  SCA_ArmatureSensor *replica = new SCA_ArmatureSensor(*this);
  // m_range_expr must be recalculated on replica!
  replica->ProcessReplica();
  return replica;
}

void SCA_ArmatureSensor::ReParent(SCA_IObject *parent)
{
  SCA_ISensor::ReParent(parent);
  // must remap the constraint
  FindConstraint();
}

bool SCA_ArmatureSensor::IsPositiveTrigger()
{
  return (m_invert) ? !m_result : m_result;
}

SCA_ArmatureSensor::~SCA_ArmatureSensor()
{
}

bool SCA_ArmatureSensor::Evaluate()
{
  bool reset = m_reset && m_level;

  m_reset = false;
  if (!m_constraint)
    return false;
  switch (m_type) {
    case SENS_ARM_STATE_CHANGED:
      m_result = !(m_constraint->flag & CONSTRAINT_OFF);
      break;
    case SENS_ARM_LIN_ERROR_BELOW:
      m_result = (m_constraint->lin_error < m_value);
      break;
    case SENS_ARM_LIN_ERROR_ABOVE:
      m_result = (m_constraint->lin_error > m_value);
      break;
    case SENS_ARM_ROT_ERROR_BELOW:
      m_result = (m_constraint->rot_error < m_value);
      break;
    case SENS_ARM_ROT_ERROR_ABOVE:
      m_result = (m_constraint->rot_error > m_value);
      break;
  }
  if (m_lastresult != m_result) {
    m_lastresult = m_result;
    return true;
  }
  return (reset) ? true : false;
}

#ifdef WITH_PYTHON

/* ------------------------------------------------------------------------- */
/* Python functions                                                          */
/* ------------------------------------------------------------------------- */

/* Integration hooks ------------------------------------------------------- */
PyTypeObject SCA_ArmatureSensor::Type = {PyVarObject_HEAD_INIT(nullptr, 0) "SCA_ArmatureSensor",
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

PyMethodDef SCA_ArmatureSensor::Methods[] = {
    {nullptr, nullptr}  // Sentinel
};

PyAttributeDef SCA_ArmatureSensor::Attributes[] = {
    EXP_PYATTRIBUTE_RO_FUNCTION("constraint", SCA_ArmatureSensor, pyattr_get_constraint),
    EXP_PYATTRIBUTE_FLOAT_RW("value", -FLT_MAX, FLT_MAX, SCA_ArmatureSensor, m_value),
    EXP_PYATTRIBUTE_INT_RW("type", 0, SENS_ARM_MAXTYPE, false, SCA_ArmatureSensor, m_type),
    EXP_PYATTRIBUTE_NULL  // Sentinel
};

PyObject *SCA_ArmatureSensor::pyattr_get_constraint(EXP_PyObjectPlus *self,
                                                    const struct EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_ArmatureSensor *sensor = static_cast<SCA_ArmatureSensor *>(self);
  if (sensor->m_gameobj->GetGameObjectType() == SCA_IObject::OBJ_ARMATURE) {
    BL_ArmatureObject *armobj = (BL_ArmatureObject *)sensor->m_gameobj;
    BL_ArmatureConstraint *constraint = armobj->GetConstraint(sensor->m_posechannel,
                                                              sensor->m_constraintname);
    if (constraint)
      return constraint->GetProxy();
  }
  Py_RETURN_NONE;
}

#endif  // WITH_PYTHON
