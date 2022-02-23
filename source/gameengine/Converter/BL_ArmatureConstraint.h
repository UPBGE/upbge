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

/** \file BL_ArmatureConstraint.h
 *  \ingroup bgeconv
 */

#pragma once

#include <map>

#include "DNA_constraint_types.h"

#include "EXP_Value.h"

class SCA_IObject;
class KX_GameObject;
class BL_ArmatureObject;
struct bConstraint;
struct bPoseChannel;
struct Object;

class BL_ArmatureConstraint : public EXP_Value {
  Py_Header

      private : struct bConstraint *m_constraint;
  struct bPoseChannel *m_posechannel;
  class BL_ArmatureObject *m_armature;
  std::string m_name;
  KX_GameObject *m_target;
  KX_GameObject *m_subtarget;
  struct Object *m_blendtarget;
  struct Object *m_blendsubtarget;

 public:
  BL_ArmatureConstraint(class BL_ArmatureObject *armature,
                        struct bPoseChannel *posechannel,
                        struct bConstraint *constraint,
                        KX_GameObject *target,
                        KX_GameObject *subtarget);
  virtual ~BL_ArmatureConstraint();

  virtual EXP_Value *GetReplica();
  void CopyBlenderTargets();
  void ReParent(BL_ArmatureObject *armature);
  void Relink(std::map<SCA_IObject *, SCA_IObject *> &map);
  bool UnlinkObject(SCA_IObject *clientobj);

  void UpdateTarget();

  bool Match(const std::string &posechannel, const std::string &constraint);
  virtual std::string GetName()
  {
    return m_name;
  }

  void SetConstraintFlag(int flag)
  {
    if (m_constraint)
      m_constraint->flag |= flag;
  }
  void ClrConstraintFlag(int flag)
  {
    if (m_constraint)
      m_constraint->flag &= ~flag;
  }
  void SetWeight(float weight)
  {
    if (m_constraint && m_constraint->type == CONSTRAINT_TYPE_KINEMATIC && m_constraint->data) {
      bKinematicConstraint *con = (bKinematicConstraint *)m_constraint->data;
      con->weight = weight;
    }
  }
  void SetInfluence(float influence)
  {
    if (m_constraint)
      m_constraint->enforce = influence;
  }
  void SetTarget(KX_GameObject *target);
  void SetSubtarget(KX_GameObject *subtarget);

#ifdef WITH_PYTHON

  // Python access
  static PyObject *py_attr_getattr(EXP_PyObjectPlus *self,
                                   const struct EXP_PYATTRIBUTE_DEF *attrdef);
  static int py_attr_setattr(EXP_PyObjectPlus *self,
                             const struct EXP_PYATTRIBUTE_DEF *attrdef,
                             PyObject *value);
#endif /* WITH_PYTHON */
};
