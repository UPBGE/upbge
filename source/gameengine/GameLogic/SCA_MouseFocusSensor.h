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

/** \file SCA_MouseFocusSensor.h
 *  \ingroup ketsji
 *  \brief SCA_MouseFocusSensor determines mouse in/out/over events.
 */

#pragma once

#include "BLI_utildefines.h"

#include "KX_Scene.h"
#include "SCA_MouseSensor.h"

class KX_Camera;
class KX_KetsjiEngine;
class KX_RayCast;

struct KX_ClientObjectInfo;

/**
 * The mouse focus sensor extends the basic SCA_MouseSensor. It has
 * been placed in KX because it needs access to the rasterizer and
 * SuMO.
 *
 * - extend the valid modes?
 * - */
class SCA_MouseFocusSensor : public SCA_MouseSensor {

  Py_Header

      public :

      SCA_MouseFocusSensor(class SCA_MouseManager *eventmgr,
                           int startx,
                           int starty,
                           short int mousemode,
                           int focusmode,
                           bool bCollisionPulse,
                           const std::string &propname,
                           bool bFindMaterial,
                           bool bXRay,
                           int mask,
                           KX_Scene *kxscene,
                           KX_KetsjiEngine *kxengine,
                           SCA_IObject *gameobj);

  virtual ~SCA_MouseFocusSensor()
  {
  }
  virtual EXP_Value *GetReplica()
  {
    EXP_Value *replica = new SCA_MouseFocusSensor(*this);
    // this will copy properties and so on...
    replica->ProcessReplica();
    return replica;
  };

  virtual void Replace_IScene(SCA_IScene *val)
  {
    m_kxscene = static_cast<KX_Scene *>(val);
  };

  /**
   * \attention Overrides default evaluate.
   */
  virtual bool Evaluate();
  virtual void Init();

  virtual bool IsPositiveTrigger()
  {
    bool result = m_positive_event;
    if (m_invert)
      result = !result;
    return result;
  };

  /// \see KX_RayCast
  bool RayHit(KX_ClientObjectInfo *client, KX_RayCast *result, void */*data*/);
  /// \see KX_RayCast
  bool NeedRayCast(KX_ClientObjectInfo *client, void */*data*/);

  const MT_Vector3 &RaySource() const;
  const MT_Vector3 &RayTarget() const;
  const MT_Vector3 &HitPosition() const;
  const MT_Vector3 &HitNormal() const;
  const MT_Vector2 &HitUV() const;

#ifdef WITH_PYTHON

  /* --------------------------------------------------------------------- */
  /* Python interface ---------------------------------------------------- */
  /* --------------------------------------------------------------------- */

  /* attributes */
  static PyObject *pyattr_get_ray_source(EXP_PyObjectPlus *self_v,
                                         const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_ray_target(EXP_PyObjectPlus *self_v,
                                         const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_ray_direction(EXP_PyObjectPlus *self_v,
                                            const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_hit_object(EXP_PyObjectPlus *self_v,
                                         const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_hit_position(EXP_PyObjectPlus *self_v,
                                           const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_hit_normal(EXP_PyObjectPlus *self_v,
                                         const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_hit_uv(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);

#endif /* WITH_PYTHON */

  /* --------------------------------------------------------------------- */
  SCA_IObject *m_hitObject;
  void *m_hitObject_Last; /* only use for comparison, never access */

 private:
  /**
   * The focus mode. 1 for handling focus, 0 for not handling, 2 for focus on any object
   */
  int m_focusmode;

  /**
   * Flags whether the previous test showed a mouse-over.
   */
  bool m_mouse_over_in_previous_frame;

  /**
   * Flags whether changes in hit object should trigger a pulse
   */
  bool m_bCollisionPulse;

  /**
   * Flags get through other objects
   */
  bool m_bXRay;

  int m_mask;

  /**
   * Flags material
   */
  bool m_bFindMaterial;

  /**
   * Property or material name
   */
  std::string m_propertyname;

  /**
   * Flags whether the previous test evaluated positive.
   */
  bool m_positive_event;

  /**
   * Tests whether the object is in mouse focus for this camera
   */
  bool ParentObjectHasFocusCamera(KX_Camera *cam);

  /**
   * Tests whether the object is in mouse focus in this scene.
   */
  bool ParentObjectHasFocus(void);

  /**
   * (in game world coordinates) the place where the object was hit.
   */
  MT_Vector3 m_hitPosition;

  /**
   * (in game world coordinates) the position to which to shoot the ray.
   */
  MT_Vector3 m_prevTargetPoint;

  /**
   * (in game world coordinates) the position from which to shoot the ray.
   */
  MT_Vector3 m_prevSourcePoint;

  /**
   * (in game world coordinates) the face normal of the vertex where
   * the object was hit.  */
  MT_Vector3 m_hitNormal;

  /**
   * UV texture coordinate of the hit point if any, (0,0) otherwise
   */
  MT_Vector2 m_hitUV;

  /**
   * The KX scene that holds the camera. The camera position
   * determines a part of the start location of the picking ray.  */
  KX_Scene *m_kxscene;

  /**
   * The KX engine is needed for computing the viewport */
  KX_KetsjiEngine *m_kxengine;
};
