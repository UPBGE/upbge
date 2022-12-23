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

/** \file SCA_RaySensor.h
 *  \ingroup ketsji
 *  \brief Cast a ray and feel for objects
 */

#pragma once

#include "BLI_utildefines.h"

#include "KX_Scene.h" /* only for scene replace */
#include "MT_Vector3.h"
#include "SCA_IScene.h" /* only for scene replace */
#include "SCA_ISensor.h"

struct KX_ClientObjectInfo;
class KX_RayCast;

class SCA_RaySensor : public SCA_ISensor {
  Py_Header std::string m_propertyname;
  bool m_bFindMaterial;
  bool m_bXRay;
  float m_distance;
  class KX_Scene *m_scene;
  bool m_bTriggered;
  int m_axis;
  int m_mask;
  bool m_rayHit;
  float m_hitPosition[3];
  SCA_IObject *m_hitObject;
  float m_hitNormal[3];
  float m_rayDirection[3];
  std::string m_hitMaterial;

 public:
  SCA_RaySensor(class SCA_EventManager *eventmgr,
                SCA_IObject *gameobj,
                const std::string &propname,
                bool bFindMaterial,
                bool bXRay,
                double distance,
                int axis,
                int mask,
                class KX_Scene *ketsjiScene);
  virtual ~SCA_RaySensor();
  virtual EXP_Value *GetReplica();

  virtual bool Evaluate();
  virtual bool IsPositiveTrigger();
  virtual void Init();

  /// \see KX_RayCast
  bool RayHit(KX_ClientObjectInfo *client, KX_RayCast *result, void */*data*/);
  /// \see KX_RayCast
  bool NeedRayCast(KX_ClientObjectInfo *client, void */*data*/);

  virtual void Replace_IScene(SCA_IScene *val)
  {
    m_scene = static_cast<KX_Scene *>(val);
  }

  // Python Interface
  // odd order, see: SENS_RAY_X_AXIS
  enum RayAxis {
    KX_RAY_AXIS_POS_X = 1,
    KX_RAY_AXIS_POS_Y = 0,
    KX_RAY_AXIS_POS_Z = 2,
    KX_RAY_AXIS_NEG_X = 3,
    KX_RAY_AXIS_NEG_Y = 4,
    KX_RAY_AXIS_NEG_Z = 5,
  };

#ifdef WITH_PYTHON

  /* Attributes */
  static PyObject *pyattr_get_hitobject(EXP_PyObjectPlus *self_v,
                                        const EXP_PYATTRIBUTE_DEF *attrdef);

#endif /* WITH_PYTHON */
};
