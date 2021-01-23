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

/** \file SCA_CollectionActuator.h
 *  \ingroup ketsji
 */

#pragma once

#include "SCA_IActuator.h"

struct Collection;
class KX_Camera;
class KX_Scene;

class SCA_CollectionActuator : public SCA_IActuator {
  Py_Header

      private :

      KX_Scene *m_kxscene;
  Collection *m_collection;
  KX_Camera *m_camera;
  int m_mode;  // suspend/resume/addOverlayCollection/RemoveOverlayCollection

  bool m_useLogic;
  bool m_usePhysics;
  bool m_useVisibility;

 public:
  enum SCA_SceneActuatorMode {
    KX_COLLECTION_NODEF = 0,
    KX_COLLECTION_SUSPEND,
    KX_COLLECTION_RESUME,
    KX_COLLECTION_ADD_OVERLAY,
    KX_COLLECTION_REMOVE_OVERLAY,
    KX_COLLECTION_MAX
  };

  SCA_CollectionActuator(SCA_IObject *gameobj,
                         KX_Scene *scene,
                         KX_Camera *cam,
                         Collection *collection,
                         int m_mode,
                         bool use_logic,
                         bool use_physics,
                         bool use_visibility);
  virtual ~SCA_CollectionActuator();

  virtual EXP_Value *GetReplica();
  virtual void ProcessReplica();
  virtual bool UnlinkObject(SCA_IObject *clientobj);
  virtual void Relink(std::map<SCA_IObject *, SCA_IObject *> &obj_map);

  virtual bool Update();

#ifdef WITH_PYTHON

  /* --------------------------------------------------------------------- */
  /* Python interface ---------------------------------------------------- */
  /* --------------------------------------------------------------------- */

  // static PyObject *pyattr_get_camera(EXP_PyObjectPlus *self, const struct EXP_PYATTRIBUTE_DEF
  // *attrdef); static int pyattr_set_camera(EXP_PyObjectPlus *self, const struct
  // EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);

#endif /* WITH_PYTHON */

}; /* end of class SCA_CollectionActuator */
