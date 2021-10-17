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

/** \file BL_ArmatureChannel.h
 *  \ingroup bgeconv
 */

#pragma once

#include "DNA_action_types.h"

#include "EXP_Value.h"

class BL_ArmatureObject;
struct bPoseChannel;

class BL_ArmatureChannel : public EXP_Value {
  // use Py_HeaderPtr since we use generic pointer in proxy
  Py_HeaderPtr;

 private:
  friend class BL_ArmatureObject;
  struct bPoseChannel *m_posechannel;
  BL_ArmatureObject *m_armature;

 public:
  BL_ArmatureChannel(class BL_ArmatureObject *armature, struct bPoseChannel *posechannel);
  virtual ~BL_ArmatureChannel();

  virtual std::string GetName()
  {
    return m_posechannel->name;
  }

#ifdef WITH_PYTHON
  // Python access
  static PyObject *py_attr_getattr(EXP_PyObjectPlus *self,
                                   const struct EXP_PYATTRIBUTE_DEF *attrdef);
  static int py_attr_setattr(EXP_PyObjectPlus *self,
                             const struct EXP_PYATTRIBUTE_DEF *attrdef,
                             PyObject *value);
  static PyObject *py_attr_get_joint_rotation(EXP_PyObjectPlus *self,
                                              const struct EXP_PYATTRIBUTE_DEF *attrdef);
  static int py_attr_set_joint_rotation(EXP_PyObjectPlus *self,
                                        const struct EXP_PYATTRIBUTE_DEF *attrdef,
                                        PyObject *value);
#endif /* WITH_PYTHON */
};

/* this is a factory class to access bBone data field in the GE.
 * It's not supposed to be instantiated, we only need it for the Attributes and Method array.
 * The actual proxy object will be manually created using NewProxyPtr */
class BL_ArmatureBone : public EXP_PyObjectPlus {
  // use Py_HeaderPtr since we use generic pointer in proxy
  Py_HeaderPtr;

 private:
  // make constructor private to make sure no one tries to instantiate this class
  BL_ArmatureBone()
  {
  }
  virtual ~BL_ArmatureBone()
  {
  }

 public:
#ifdef WITH_PYTHON
  static PyObject *py_bone_repr(PyObject *self);
  static PyObject *py_bone_get_parent(EXP_PyObjectPlus *self,
                                      const struct EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *py_bone_get_children(EXP_PyObjectPlus *self,
                                        const struct EXP_PYATTRIBUTE_DEF *attrdef);
#endif
};
