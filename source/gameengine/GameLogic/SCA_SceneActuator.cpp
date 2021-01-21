/*
 * Set scene/camera stuff
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

/** \file gameengine/Ketsji/SCA_SceneActuator.cpp
 *  \ingroup ketsji
 */

#include "SCA_SceneActuator.h"

#include "KX_Camera.h"
#include "KX_Globals.h"

/* ------------------------------------------------------------------------- */
/* Native functions                                                          */
/* ------------------------------------------------------------------------- */

SCA_SceneActuator::SCA_SceneActuator(SCA_IObject *gameobj,
                                     int mode,
                                     KX_Scene *scene,
                                     KX_KetsjiEngine *ketsjiEngine,
                                     const std::string &nextSceneName,
                                     KX_Camera *camera)
    : SCA_IActuator(gameobj, KX_ACT_SCENE)
{
  m_mode = mode;
  m_scene = scene;
  m_KetsjiEngine = ketsjiEngine;
  m_camera = camera;
  m_nextSceneName = nextSceneName;
  if (m_camera)
    m_camera->RegisterActuator(this);
} /* End of constructor */

SCA_SceneActuator::~SCA_SceneActuator()
{
  if (m_camera)
    m_camera->UnregisterActuator(this);
} /* end of destructor */

EXP_Value *SCA_SceneActuator::GetReplica()
{
  SCA_SceneActuator *replica = new SCA_SceneActuator(*this);
  replica->ProcessReplica();
  return replica;
}

void SCA_SceneActuator::ProcessReplica()
{
  if (m_camera)
    m_camera->RegisterActuator(this);
  SCA_IActuator::ProcessReplica();
}

bool SCA_SceneActuator::UnlinkObject(SCA_IObject *clientobj)
{
  if (clientobj == (SCA_IObject *)m_camera) {
    // this object is being deleted, we cannot continue to track it.
    m_camera = nullptr;
    return true;
  }
  return false;
}

void SCA_SceneActuator::Relink(std::map<SCA_IObject *, SCA_IObject *> &obj_map)
{
  KX_Camera *obj = static_cast<KX_Camera *>(obj_map[m_camera]);
  if (obj) {
    if (m_camera)
      m_camera->UnregisterActuator(this);
    m_camera = obj;
    m_camera->RegisterActuator(this);
  }
}

bool SCA_SceneActuator::Update()
{
  // bool result = false;	/*unused*/
  bool bNegativeEvent = IsNegativeEvent();
  RemoveAllEvents();

  if (bNegativeEvent)
    return false;  // do nothing on negative events

  switch (m_mode) {
    case KX_SCENE_RESTART: {
      m_KetsjiEngine->ReplaceScene(m_scene->GetName(), m_scene->GetName());
      break;
    }
    case KX_SCENE_SET_CAMERA:
      if (m_camera) {
        m_scene->SetActiveCamera(m_camera);
      }
      else {
        // if no camera is set and the parent object is a camera, use it as the camera
        SCA_IObject *parent = GetParent();
        if (parent->GetGameObjectType() == SCA_IObject::OBJ_CAMERA) {
          m_scene->SetActiveCamera((KX_Camera *)parent);
        }
      }
      break;
    default:
      break;
  }

  if (!m_nextSceneName.size())
    return false;

  switch (m_mode) {
    case KX_SCENE_SET_SCENE: {
      m_KetsjiEngine->ReplaceScene(m_scene->GetName(), m_nextSceneName);
      break;
    }
    case KX_SCENE_REMOVE_SCENE: {
      m_KetsjiEngine->RemoveScene(m_nextSceneName);
      break;
    }
    default:; /* do nothing? this is an internal error !!! */
  }

  return false;
}

#ifdef WITH_PYTHON

/* ------------------------------------------------------------------------- */
/* Python functions                                                          */
/* ------------------------------------------------------------------------- */

/* Integration hooks ------------------------------------------------------- */
PyTypeObject SCA_SceneActuator::Type = {PyVarObject_HEAD_INIT(nullptr, 0) "SCA_SceneActuator",
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

PyMethodDef SCA_SceneActuator::Methods[] = {
    {nullptr, nullptr}  // Sentinel
};

PyAttributeDef SCA_SceneActuator::Attributes[] = {
    EXP_PYATTRIBUTE_STRING_RW(
        "scene", 0, MAX_ID_NAME - 2, true, SCA_SceneActuator, m_nextSceneName),
    EXP_PYATTRIBUTE_RW_FUNCTION("camera", SCA_SceneActuator, pyattr_get_camera, pyattr_set_camera),
    EXP_PYATTRIBUTE_BOOL_RW("useRestart", SCA_SceneActuator, m_restart),
    EXP_PYATTRIBUTE_INT_RW(
        "mode", KX_SCENE_NODEF + 1, KX_SCENE_MAX - 1, true, SCA_SceneActuator, m_mode),
    EXP_PYATTRIBUTE_NULL  // Sentinel
};

PyObject *SCA_SceneActuator::pyattr_get_camera(EXP_PyObjectPlus *self,
                                               const struct EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_SceneActuator *actuator = static_cast<SCA_SceneActuator *>(self);
  if (!actuator->m_camera)
    Py_RETURN_NONE;

  return actuator->m_camera->GetProxy();
}

int SCA_SceneActuator::pyattr_set_camera(EXP_PyObjectPlus *self,
                                         const struct EXP_PYATTRIBUTE_DEF *attrdef,
                                         PyObject *value)
{
  SCA_SceneActuator *actuator = static_cast<SCA_SceneActuator *>(self);
  KX_Camera *camOb;

  if (!ConvertPythonToCamera(
          KX_GetActiveScene(), value, &camOb, true, "actu.camera = value: SCA_SceneActuator"))
    return PY_SET_ATTR_FAIL;

  if (actuator->m_camera)
    actuator->m_camera->UnregisterActuator(actuator);

  if (camOb == nullptr) {
    actuator->m_camera = nullptr;
  }
  else {
    actuator->m_camera = camOb;
    actuator->m_camera->RegisterActuator(actuator);
  }

  return PY_SET_ATTR_SUCCESS;
}

#endif  // WITH_PYTHON

/* eof */
