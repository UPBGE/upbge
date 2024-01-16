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

/** \file gameengine/Converter/BL_ArmatureConstraint.cpp
 *  \ingroup bgeconv
 */

#include "BL_ArmatureConstraint.h"

#include "BL_ArmatureObject.h"
#include "KX_Globals.h"

#include "BKE_constraint.h"
#include "BKE_context.hh"
#include "BKE_lib_id.hh"
#include "BKE_object.hh"

#ifdef WITH_PYTHON

PyTypeObject BL_ArmatureConstraint::Type = {
    PyVarObject_HEAD_INIT(nullptr, 0) "BL_ArmatureConstraint",
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

#endif  // WITH_PYTHON

BL_ArmatureConstraint::BL_ArmatureConstraint(BL_ArmatureObject *armature,
                                             bPoseChannel *posechannel,
                                             bConstraint *constraint,
                                             KX_GameObject *target,
                                             KX_GameObject *subtarget)
    : m_constraint(constraint),
      m_posechannel(posechannel),
      m_armature(armature),
      m_target(target),
      m_subtarget(subtarget),
      m_blendtarget(nullptr),
      m_blendsubtarget(nullptr)
{
  BLI_assert(m_constraint != nullptr && m_posechannel != nullptr);

  m_name = std::string(m_posechannel->name) + ":" + std::string(m_constraint->name);

  if (m_target) {
    m_target->RegisterObject(m_armature);
  }
  if (m_subtarget) {
    m_subtarget->RegisterObject(m_armature);
  }

  CopyBlenderTargets();
}

BL_ArmatureConstraint::~BL_ArmatureConstraint()
{
  if (m_target)
    m_target->UnregisterObject(m_armature);
  if (m_subtarget)
    m_subtarget->UnregisterObject(m_armature);

  // Free the fake blender object targets without freeing the pose of an armature set in these
  // objects.
  bContext *C = KX_GetActiveEngine()->GetContext();
  if (m_blendtarget) {
    m_blendtarget->pose = nullptr;
    BKE_id_free(CTX_data_main(C), &m_blendtarget->id);
  }
  if (m_blendsubtarget) {
    m_blendsubtarget->pose = nullptr;
    BKE_id_free(CTX_data_main(C), &m_blendsubtarget->id);
  }
}

EXP_Value *BL_ArmatureConstraint::GetReplica()
{
  BL_ArmatureConstraint *replica = new BL_ArmatureConstraint(*this);

  return replica;
}

void BL_ArmatureConstraint::CopyBlenderTargets()
{
  // Create the fake blender object target.
  bContext *C = KX_GetActiveEngine()->GetContext();
  if (m_target) {
    m_blendtarget = BKE_object_add_only_object(
        CTX_data_main(C), OB_EMPTY, m_target->GetName().c_str());
  }
  if (m_subtarget) {
    m_blendsubtarget = BKE_object_add_only_object(
        CTX_data_main(C), OB_EMPTY, m_subtarget->GetName().c_str());
  }

  const bConstraintTypeInfo *cti = BKE_constraint_typeinfo_get(m_constraint);
  if (cti && cti->get_constraint_targets) {
    ListBase listb = {nullptr, nullptr};
    cti->get_constraint_targets(m_constraint, &listb);
    if (listb.first) {
      bConstraintTarget *target = (bConstraintTarget *)listb.first;
      if (m_blendtarget) {
        target->tar = m_blendtarget;
      }
      if (target->next && m_blendsubtarget) {
        target->next->tar = m_blendsubtarget;
      }
    }
    if (cti->flush_constraint_targets) {
      cti->flush_constraint_targets(m_constraint, &listb, 0);
    }
  }
}

void BL_ArmatureConstraint::ReParent(BL_ArmatureObject *armature)
{
  m_armature = armature;
  if (m_target) {
    m_target->RegisterObject(armature);
  }
  if (m_subtarget) {
    m_subtarget->RegisterObject(armature);
  }

  const std::string constraintname = m_constraint->name;
  const std::string posechannelname = m_posechannel->name;
  m_constraint = nullptr;
  m_posechannel = nullptr;

  bPose *newpose = m_armature->GetPose();

  // find the corresponding constraint in the new armature object
  // and locate the constraint
  for (bPoseChannel *pchan = (bPoseChannel *)newpose->chanbase.first; pchan;
       pchan = (bPoseChannel *)pchan->next) {
    if (posechannelname == pchan->name) {
      // now locate the constraint
      for (bConstraint *pcon = (bConstraint *)pchan->constraints.first; pcon;
           pcon = (bConstraint *)pcon->next) {
        if (constraintname == pcon->name) {
          m_constraint = pcon;
          m_posechannel = pchan;
          break;
        }
      }
      break;
    }
  }

  CopyBlenderTargets();
}

void BL_ArmatureConstraint::Relink(std::map<SCA_IObject *, SCA_IObject *> &obj_map)
{
  KX_GameObject *obj = static_cast<KX_GameObject *>(obj_map[m_target]);
  if (obj) {
    m_target->UnregisterObject(m_armature);
    m_target = obj;
    m_target->RegisterObject(m_armature);
  }
  obj = static_cast<KX_GameObject *>(obj_map[m_subtarget]);
  if (obj) {
    m_subtarget->UnregisterObject(m_armature);
    m_subtarget = obj;
    m_subtarget->RegisterObject(m_armature);
  }
}

bool BL_ArmatureConstraint::UnlinkObject(SCA_IObject *clientobj)
{
  bool res = false;
  if (clientobj == m_target) {
    m_target = nullptr;
    res = true;
  }
  if (clientobj == m_subtarget) {
    m_subtarget = nullptr;
    res = true;
  }
  return res;
}

void BL_ArmatureConstraint::UpdateTarget()
{
  if (!(m_constraint->flag & CONSTRAINT_OFF) && (!m_blendtarget || m_target)) {
    if (m_blendtarget) {
      // external target, must be updated
      m_target->UpdateBlenderObjectMatrix(m_blendtarget);

      if (m_target->GetGameObjectType() == SCA_IObject::OBJ_ARMATURE) {
        // update the pose in case a bone is specified in the constraint target
        m_blendtarget->pose = static_cast<BL_ArmatureObject *>(m_target)->GetPose();
      }
    }
    if (m_blendsubtarget && m_subtarget) {
      m_subtarget->UpdateBlenderObjectMatrix(m_blendsubtarget);
      if (m_subtarget->GetGameObjectType() == SCA_IObject::OBJ_ARMATURE) {
        m_blendsubtarget->pose = static_cast<BL_ArmatureObject *>(m_subtarget)->GetPose();
      }
    }
  }
}

bool BL_ArmatureConstraint::Match(const std::string &posechannel, const std::string &constraint)
{
  return ((m_posechannel->name == posechannel) && (m_constraint->name == constraint));
}

void BL_ArmatureConstraint::SetTarget(KX_GameObject *target)
{
  if (m_blendtarget) {
    if (target != m_target) {
      m_target->UnregisterObject(m_armature);
      m_target = target;
      if (m_target)
        m_target->RegisterObject(m_armature);
    }
  }
}

void BL_ArmatureConstraint::SetSubtarget(KX_GameObject *subtarget)
{
  if (m_blendsubtarget) {
    if (subtarget != m_subtarget) {
      m_subtarget->UnregisterObject(m_armature);
      m_subtarget = subtarget;
      if (m_subtarget)
        m_subtarget->RegisterObject(m_armature);
    }
  }
}

#ifdef WITH_PYTHON

// PYTHON

PyMethodDef BL_ArmatureConstraint::Methods[] = {
    {nullptr, nullptr}  // Sentinel
};

// order of definition of attributes, must match Attributes[] array
#  define BCA_TYPE 0
#  define BCA_NAME 1
#  define BCA_ENFORCE 2
#  define BCA_HEADTAIL 3
#  define BCA_LINERROR 4
#  define BCA_ROTERROR 5
#  define BCA_TARGET 6
#  define BCA_SUBTARGET 7
#  define BCA_ACTIVE 8
#  define BCA_IKWEIGHT 9
#  define BCA_IKTYPE 10
#  define BCA_IKFLAG 11
#  define BCA_IKDIST 12
#  define BCA_IKMODE 13

PyAttributeDef BL_ArmatureConstraint::Attributes[] = {
    // Keep these attributes in order of BCA_ defines!!! used by py_attr_getattr and
    // py_attr_setattr
    EXP_PYATTRIBUTE_RO_FUNCTION("type", BL_ArmatureConstraint, py_attr_getattr),
    EXP_PYATTRIBUTE_RO_FUNCTION("name", BL_ArmatureConstraint, py_attr_getattr),
    EXP_PYATTRIBUTE_RW_FUNCTION(
        "enforce", BL_ArmatureConstraint, py_attr_getattr, py_attr_setattr),
    EXP_PYATTRIBUTE_RW_FUNCTION(
        "headtail", BL_ArmatureConstraint, py_attr_getattr, py_attr_setattr),
    EXP_PYATTRIBUTE_RO_FUNCTION("lin_error", BL_ArmatureConstraint, py_attr_getattr),
    EXP_PYATTRIBUTE_RO_FUNCTION("rot_error", BL_ArmatureConstraint, py_attr_getattr),
    EXP_PYATTRIBUTE_RW_FUNCTION("target", BL_ArmatureConstraint, py_attr_getattr, py_attr_setattr),
    EXP_PYATTRIBUTE_RW_FUNCTION(
        "subtarget", BL_ArmatureConstraint, py_attr_getattr, py_attr_setattr),
    EXP_PYATTRIBUTE_RW_FUNCTION("active", BL_ArmatureConstraint, py_attr_getattr, py_attr_setattr),
    EXP_PYATTRIBUTE_RW_FUNCTION(
        "ik_weight", BL_ArmatureConstraint, py_attr_getattr, py_attr_setattr),
    EXP_PYATTRIBUTE_RO_FUNCTION("ik_type", BL_ArmatureConstraint, py_attr_getattr),
    EXP_PYATTRIBUTE_RO_FUNCTION("ik_flag", BL_ArmatureConstraint, py_attr_getattr),
    EXP_PYATTRIBUTE_RW_FUNCTION(
        "ik_dist", BL_ArmatureConstraint, py_attr_getattr, py_attr_setattr),
    EXP_PYATTRIBUTE_RW_FUNCTION(
        "ik_mode", BL_ArmatureConstraint, py_attr_getattr, py_attr_setattr),
    EXP_PYATTRIBUTE_NULL  // Sentinel
};

PyObject *BL_ArmatureConstraint::py_attr_getattr(EXP_PyObjectPlus *self_v,
                                                 const struct EXP_PYATTRIBUTE_DEF *attrdef)
{
  BL_ArmatureConstraint *self = static_cast<BL_ArmatureConstraint *>(self_v);
  bConstraint *constraint = self->m_constraint;
  bKinematicConstraint *ikconstraint = (constraint &&
                                        constraint->type == CONSTRAINT_TYPE_KINEMATIC) ?
                                           (bKinematicConstraint *)constraint->data :
                                           nullptr;
  int attr_order = attrdef - Attributes;

  if (!constraint) {
    PyErr_SetString(PyExc_AttributeError, "constraint is nullptr");
    return nullptr;
  }

  switch (attr_order) {
    case BCA_TYPE:
      return PyLong_FromLong(constraint->type);
    case BCA_NAME:
      return PyUnicode_FromString(constraint->name);
    case BCA_ENFORCE:
      return PyFloat_FromDouble(constraint->enforce);
    case BCA_HEADTAIL:
      return PyFloat_FromDouble(constraint->headtail);
    case BCA_LINERROR:
      return PyFloat_FromDouble(constraint->lin_error);
    case BCA_ROTERROR:
      return PyFloat_FromDouble(constraint->rot_error);
    case BCA_TARGET:
      if (!self->m_target)
        Py_RETURN_NONE;
      else
        return self->m_target->GetProxy();
    case BCA_SUBTARGET:
      if (!self->m_subtarget)
        Py_RETURN_NONE;
      else
        return self->m_subtarget->GetProxy();
    case BCA_ACTIVE:
      return PyBool_FromLong((constraint->flag & CONSTRAINT_OFF) == 0);
    case BCA_IKWEIGHT:
    case BCA_IKTYPE:
    case BCA_IKFLAG:
    case BCA_IKDIST:
    case BCA_IKMODE:
      if (!ikconstraint) {
        PyErr_SetString(PyExc_AttributeError, "constraint is not of IK type");
        return nullptr;
      }
      switch (attr_order) {
        case BCA_IKWEIGHT:
          return PyFloat_FromDouble(ikconstraint->weight);
        case BCA_IKTYPE:
          return PyLong_FromLong(ikconstraint->type);
        case BCA_IKFLAG:
          return PyLong_FromLong(ikconstraint->flag);
        case BCA_IKDIST:
          return PyFloat_FromDouble(ikconstraint->dist);
        case BCA_IKMODE:
          return PyLong_FromLong(ikconstraint->mode);
      }
      // should not come here
      break;
  }
  PyErr_SetString(PyExc_AttributeError, "constraint unknown attribute");
  return nullptr;
}

int BL_ArmatureConstraint::py_attr_setattr(EXP_PyObjectPlus *self_v,
                                           const struct EXP_PYATTRIBUTE_DEF *attrdef,
                                           PyObject *value)
{
  BL_ArmatureConstraint *self = static_cast<BL_ArmatureConstraint *>(self_v);
  bConstraint *constraint = self->m_constraint;
  bKinematicConstraint *ikconstraint = (constraint &&
                                        constraint->type == CONSTRAINT_TYPE_KINEMATIC) ?
                                           (bKinematicConstraint *)constraint->data :
                                           nullptr;
  int attr_order = attrdef - Attributes;
  int ival;
  double dval;
  //	char* sval;
  SCA_LogicManager *logicmgr = KX_GetActiveScene()->GetLogicManager();
  KX_GameObject *oval;

  if (!constraint) {
    PyErr_SetString(PyExc_AttributeError, "constraint is nullptr");
    return PY_SET_ATTR_FAIL;
  }

  switch (attr_order) {
    case BCA_ENFORCE:
      dval = PyFloat_AsDouble(value);
      if (dval < 0.0 || dval > 1.0) { /* also accounts for non float */
        PyErr_SetString(
            PyExc_AttributeError,
            "constraint.enforce = float: BL_ArmatureConstraint, expected a float between 0 and 1");
        return PY_SET_ATTR_FAIL;
      }
      constraint->enforce = dval;
      return PY_SET_ATTR_SUCCESS;

    case BCA_HEADTAIL:
      dval = PyFloat_AsDouble(value);
      if (dval < 0.0 || dval > 1.0) { /* also accounts for non float */
        PyErr_SetString(PyExc_AttributeError,
                        "constraint.headtail = float: BL_ArmatureConstraint, expected a float "
                        "between 0 and 1");
        return PY_SET_ATTR_FAIL;
      }
      constraint->headtail = dval;
      return PY_SET_ATTR_SUCCESS;

    case BCA_TARGET:
      if (!ConvertPythonToGameObject(
              logicmgr, value, &oval, true, "constraint.target = value: BL_ArmatureConstraint"))
        return PY_SET_ATTR_FAIL;  // ConvertPythonToGameObject sets the error
      self->SetTarget(oval);
      return PY_SET_ATTR_SUCCESS;

    case BCA_SUBTARGET:
      if (!ConvertPythonToGameObject(
              logicmgr, value, &oval, true, "constraint.subtarget = value: BL_ArmatureConstraint"))
        return PY_SET_ATTR_FAIL;  // ConvertPythonToGameObject sets the error
      self->SetSubtarget(oval);
      return PY_SET_ATTR_SUCCESS;

    case BCA_ACTIVE:
      ival = PyObject_IsTrue(value);
      if (ival == -1) {
        PyErr_SetString(PyExc_AttributeError,
                        "constraint.active = bool: BL_ArmatureConstraint, expected True or False");
        return PY_SET_ATTR_FAIL;
      }
      self->m_constraint->flag = (self->m_constraint->flag & ~CONSTRAINT_OFF) |
                                 ((ival) ? 0 : CONSTRAINT_OFF);
      return PY_SET_ATTR_SUCCESS;

    case BCA_IKWEIGHT:
    case BCA_IKDIST:
    case BCA_IKMODE:
      if (!ikconstraint) {
        PyErr_SetString(PyExc_AttributeError, "constraint is not of IK type");
        return PY_SET_ATTR_FAIL;
      }
      switch (attr_order) {
        case BCA_IKWEIGHT:
          dval = PyFloat_AsDouble(value);
          if (dval < 0.0 || dval > 1.0) { /* also accounts for non float */
            PyErr_SetString(PyExc_AttributeError,
                            "constraint.weight = float: BL_ArmatureConstraint, expected a float "
                            "between 0 and 1");
            return PY_SET_ATTR_FAIL;
          }
          ikconstraint->weight = dval;
          return PY_SET_ATTR_SUCCESS;

        case BCA_IKDIST:
          dval = PyFloat_AsDouble(value);
          if (dval < 0.0) { /* also accounts for non float */
            PyErr_SetString(
                PyExc_AttributeError,
                "constraint.ik_dist = float: BL_ArmatureConstraint, expected a positive float");
            return PY_SET_ATTR_FAIL;
          }
          ikconstraint->dist = dval;
          return PY_SET_ATTR_SUCCESS;

        case BCA_IKMODE:
          ival = PyLong_AsLong(value);
          if (ival < 0) {
            PyErr_SetString(PyExc_AttributeError,
                            "constraint.ik_mode = integer: BL_ArmatureConstraint, expected a "
                            "positive integer");
            return PY_SET_ATTR_FAIL;
          }
          ikconstraint->mode = ival;
          return PY_SET_ATTR_SUCCESS;
      }
      // should not come here
      break;
  }

  PyErr_SetString(PyExc_AttributeError, "constraint unknown attribute");
  return PY_SET_ATTR_FAIL;
}

#endif  // WITH_PYTHON
