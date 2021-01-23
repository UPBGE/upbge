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

/** \file BL_ArmatureActuator.h
 *  \ingroup bgeconv
 */

#pragma once

#include "BL_ArmatureConstraint.h"
#include "SCA_IActuator.h"

/**
 * This class is the conversion of the Pose channel constraint.
 * It makes a link between the pose constraint and the KX scene.
 * The main purpose is to give access to the constraint target
 * to link it to a game object.
 * It also allows to activate/deactivate constraints during the game.
 * Later it will also be possible to create constraint on the fly
 */

class BL_ArmatureActuator : public SCA_IActuator {
  Py_Header public : BL_ArmatureActuator(SCA_IObject *gameobj,
                                         int type,
                                         const char *posechannel,
                                         const char *constraintname,
                                         KX_GameObject *targetobj,
                                         KX_GameObject *subtargetobj,
                                         float weight,
                                         float influence);

  virtual ~BL_ArmatureActuator();

  virtual EXP_Value *GetReplica()
  {
    BL_ArmatureActuator *replica = new BL_ArmatureActuator(*this);
    replica->ProcessReplica();
    return replica;
  };
  virtual void ProcessReplica();
  virtual bool UnlinkObject(SCA_IObject *clientobj);
  virtual void Relink(std::map<SCA_IObject *, SCA_IObject *> &obj_map);
  virtual bool Update(double curtime);
  virtual void ReParent(SCA_IObject *parent);

#ifdef WITH_PYTHON

  /* These are used to get and set m_target */
  static PyObject *pyattr_get_constraint(EXP_PyObjectPlus *self,
                                         const struct EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_object(EXP_PyObjectPlus *self,
                                     const struct EXP_PYATTRIBUTE_DEF *attrdef);
  static int pyattr_set_object(EXP_PyObjectPlus *self,
                               const struct EXP_PYATTRIBUTE_DEF *attrdef,
                               PyObject *value);

#endif /* WITH_PYTHON */

 private:
  // identify the constraint that this actuator controls
  void FindConstraint();

  BL_ArmatureConstraint *m_constraint;
  KX_GameObject *m_gametarget;
  KX_GameObject *m_gamesubtarget;
  std::string m_posechannel;
  std::string m_constraintname;
  float m_weight;
  float m_influence;
  int m_type;
};
