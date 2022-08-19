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

/** \file SCA_SceneActuator.h
 *  \ingroup ketsji
 */

#pragma once

#include "KX_Scene.h" /* Replace_IScene only */
#include "SCA_IActuator.h"
#include "SCA_IScene.h" /* Replace_IScene only */

class SCA_SceneActuator : public SCA_IActuator {
  Py_Header

      int m_mode;
  // (restart) has become a toggle internally... not in the interface though
  bool m_restart;
  // (set Scene) Scene
  /** The current scene. */
  class KX_Scene *m_scene;
  class KX_KetsjiEngine *m_KetsjiEngine;
  /** The scene to switch to. */
  std::string m_nextSceneName;

  // (Set Camera) Object
  class KX_Camera *m_camera;

 public:
  enum SCA_SceneActuatorMode {
    KX_SCENE_NODEF = 0,
    KX_SCENE_RESTART,
    KX_SCENE_SET_SCENE,
    KX_SCENE_SET_CAMERA,
    KX_SCENE_REMOVE_SCENE,
    KX_SCENE_MAX
  };

  SCA_SceneActuator(SCA_IObject *gameobj,
                    int mode,
                    KX_Scene *scene,
                    KX_KetsjiEngine *ketsjiEngine,
                    const std::string &nextSceneName,
                    KX_Camera *camera);
  virtual ~SCA_SceneActuator();

  virtual EXP_Value *GetReplica();
  virtual void ProcessReplica();
  virtual bool UnlinkObject(SCA_IObject *clientobj);
  virtual void Relink(std::map<SCA_IObject *, SCA_IObject *> &obj_map);

  virtual bool Update();

#ifdef WITH_PYTHON

  virtual void Replace_IScene(SCA_IScene *val)
  {
    m_scene = static_cast<KX_Scene *>(val);
  };

  /* --------------------------------------------------------------------- */
  /* Python interface ---------------------------------------------------- */
  /* --------------------------------------------------------------------- */

  static PyObject *pyattr_get_camera(EXP_PyObjectPlus *self,
                                     const struct EXP_PYATTRIBUTE_DEF *attrdef);
  static int pyattr_set_camera(EXP_PyObjectPlus *self,
                               const struct EXP_PYATTRIBUTE_DEF *attrdef,
                               PyObject *value);

#endif /* WITH_PYTHON */

}; /* end of class KXSceneActuator */
