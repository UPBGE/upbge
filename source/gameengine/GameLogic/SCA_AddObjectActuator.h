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

/** \file SCA_AddObjectActuator.h
 *  \ingroup ketsji
 *  \attention Previously existed as: source/gameengine/GameLogic/SCA_AddObjectActuator.h
 * Please look here for revision history.
 */

#pragma once

/* Actuator tree */
#include "MT_Vector3.h"
#include "SCA_IActuator.h"
#include "SCA_LogicManager.h"

class KX_Scene;
class KX_GameObject;

class SCA_AddObjectActuator : public SCA_IActuator {
  Py_Header

      /// Time field: lifetime of the new object
      float m_timeProp;

  /// Original object reference (object to replicate)
  KX_GameObject *m_OriginalObject;

  /// Full Object copy
  bool m_duplicateObject;

  /// Object will be added to the following scene
  KX_Scene *m_scene;

  /// Linear velocity upon creation of the object.
  float m_linear_velocity[3];
  /// Apply the velocity locally
  bool m_localLinvFlag;

  /// Angular velocity upon creation of the object.
  float m_angular_velocity[3];
  /// Apply the velocity locally
  bool m_localAngvFlag;

  KX_GameObject *m_lastCreatedObject;

 public:
  /**
   * This class also has the default constructors
   * available. Use with care!
   */

  SCA_AddObjectActuator(KX_GameObject *gameobj,
                        KX_GameObject *original,
                        float time,
                        KX_Scene *scene,
                        const float *linvel,
                        bool linv_local,
                        const float *angvel,
                        bool angv_local,
                        bool duplicateObject);

  ~SCA_AddObjectActuator(void);

  EXP_Value *GetReplica();

  virtual void ProcessReplica();

  virtual void Replace_IScene(SCA_IScene *val);

  virtual bool UnlinkObject(SCA_IObject *clientobj);

  virtual void Relink(std::map<SCA_IObject *, SCA_IObject *> &obj_map);

  virtual bool Update();

  KX_GameObject *GetLastCreatedObject() const;

  void InstantAddObject();

#ifdef WITH_PYTHON

  EXP_PYMETHOD_DOC_NOARGS(SCA_AddObjectActuator, InstantAddObject);

  static PyObject *pyattr_get_object(EXP_PyObjectPlus *self,
                                     const struct EXP_PYATTRIBUTE_DEF *attrdef);
  static int pyattr_set_object(EXP_PyObjectPlus *self,
                               const struct EXP_PYATTRIBUTE_DEF *attrdef,
                               PyObject *value);
  static PyObject *pyattr_get_objectLastCreated(EXP_PyObjectPlus *self,
                                                const struct EXP_PYATTRIBUTE_DEF *attrdef);

#endif /* WITH_PYTHON */

}; /* end of class SCA_AddObjectActuator : public KX_EditObjectActuator */
