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

/** \file KX_Camera.h
 *  \ingroup ketsji
 *  \brief Camera in the gameengine. Cameras are also used for views.
 */

#pragma once

#include "KX_GameObject.h"
#include "RAS_CameraData.h"
#include "SG_Frustum.h"

#ifdef WITH_PYTHON
/* utility conversion function */
bool ConvertPythonToCamera(KX_Scene *scene,
                           PyObject *value,
                           KX_Camera **object,
                           bool py_none_ok,
                           const char *error_prefix);
#endif

class KX_Camera : public KX_GameObject {
  Py_Header

      protected : friend class KX_Scene;
  /** Camera parameters (clips distances, focal length). These
   * params are closely tied to Blender. In the gameengine, only the
   * projection and modelview matrices are relevant. There's a
   * conversion being done in the engine class. Why is it stored
   * here? It doesn't really have a function here. */
  RAS_CameraData m_camdata;

  struct GPUViewport *m_gpuViewport;

  // Never used, I think...
//	void MoveTo(const MT_Vector3& movevec)
//	{
#if 0
		MT_Transform camtrans;
		camtrans.invert(m_trans1);
		MT_Matrix3x3 camorient = camtrans.getBasis();
		camtrans.translate(camorient.inverse()*movevec);
		m_trans1.invert(camtrans);
#endif
  //	}

  /**
   * Storage for the projection matrix that is passed to the
   * rasterizer. */
  MT_Matrix4x4 m_projection_matrix;
  // MT_Matrix4x4 m_projection_matrix1;

  /**
   * Storage for the modelview matrix that is passed to the
   * rasterizer. */
  MT_Matrix4x4 m_modelview_matrix;

  /**
   * true if the view frustum (modelview/projection matrix)
   * has changed - the clip planes (m_planes) will have to be
   * regenerated.
   */
  bool m_dirty;
  /**
   * true if the frustum planes have been normalized.
   */
  bool m_normalized;

  /**
   * View Frustum clip planes.
   */
  MT_Vector4 m_planes[6];

  /**
   * true if this camera has a valid projection matrix.
   */
  bool m_set_projection_matrix;

  /**
   * whether the camera should delete the node itself (only for shadow camera)
   */
  bool m_delete_node;

  /** Enable object activity culling for this camera. */
  bool m_activityCulling;

  /** Distance factor for level of detail*/
  float m_lodDistanceFactor;

  /**
   * Show Debug Camera Frustum?
   */
  bool m_showDebugCameraFrustum;

  SG_Frustum m_frustum;

  void ExtractFrustum();

 public:
  enum { INSIDE, INTERSECT, OUTSIDE };

  KX_Camera();
  virtual ~KX_Camera();

  struct GPUViewport *GetGPUViewport();
  void RemoveGPUViewport();

  virtual KX_PythonProxy *NewInstance();
  virtual void ProcessReplica();

  MT_Transform GetWorldToCamera() const;
  MT_Transform GetCameraToWorld() const;

  /** Sets the projection matrix that is used by the rasterizer. */
  void SetProjectionMatrix(const MT_Matrix4x4 &mat);

  /** Sets the modelview matrix that is used by the rasterizer. */
  void SetModelviewMatrix(const MT_Matrix4x4 &mat);

  /** Gets the projection matrix that is used by the rasterizer. */
  const MT_Matrix4x4 &GetProjectionMatrix() const;

  /** returns true if this camera has been set a projection matrix. */
  bool hasValidProjectionMatrix() const;

  /** Sets the validity of the projection matrix.  Call this if you change camera
   *  data (eg lens, near plane, far plane) and require the projection matrix to be
   *  recalculated.
   */
  void InvalidateProjectionMatrix(bool valid = false);

  /** Gets the modelview matrix that is used by the rasterizer.
   *  \warning If the Camera is a dynamic object then this method may return garbage.  Use
   * GetWorldToCamera() instead.
   */
  const MT_Matrix4x4 &GetModelviewMatrix() const;

  /** Gets the aperture. */
  float GetLens() const;
  /** Gets the ortho scale. */
  float GetScale() const;
  /** Gets the horizontal size of the sensor - for camera matching */
  float GetSensorWidth() const;
  /** Gets the vertical size of the sensor - for camera matching */
  float GetSensorHeight() const;
  /** Gets the mode FOV is calculating from sensor dimensions */
  short GetSensorFit() const;
  /** Gets the horizontal shift of the sensor - for camera matching */
  float GetShiftHorizontal() const;
  /** Gets the vertical shift of the sensor - for camera matching */
  float GetShiftVertical() const;
  /** Gets the near clip distance. */
  float GetCameraNear() const;
  /** Gets the far clip distance. */
  float GetCameraFar() const;
  /** Gets the focal length (only used for stereo rendering) */
  float GetFocalLength() const;
  /** Gets all camera data. */
  RAS_CameraData *GetCameraData();

  void SetCameraData(const RAS_CameraData &camdata);

  /** Get/Set show camera frustum */
  void SetShowCameraFrustum(bool show);
  bool GetShowCameraFrustum() const;

  /** Get level of detail distance factor */
  float GetLodDistanceFactor() const;
  /** Set level of detail distance factor */
  void SetLodDistanceFactor(float lodfactor);

  bool GetActivityCulling() const;
  void SetActivityCulling(bool enable);

  const SG_Frustum &GetFrustum();

  /**
   * Sets this camera's viewport status.
   */
  void EnableViewport(bool viewport);

  /**
   * Sets this camera's viewport.
   */
  void SetViewport(int left, int bottom, int right, int top);

  /**
   * Gets this camera's viewport status.
   */
  bool GetViewport() const;

  /**
   * Gets this camera's viewport left.
   */
  int GetViewportLeft() const;

  /**
   * Gets this camera's viewport bottom.
   */
  int GetViewportBottom() const;

  /**
   * Gets this camera's viewport right.
   */
  int GetViewportRight() const;

  /**
   * Gets this camera's viewport top.
   */
  int GetViewportTop() const;

  virtual int GetGameObjectType() const
  {
    return OBJ_CAMERA;
  }

  virtual void SetBlenderObject(Object *obj);

  void MarkForDeletion();

#ifdef WITH_PYTHON
  static PyObject *game_object_new(PyTypeObject *type, PyObject *args, PyObject *kwds);

  EXP_PYMETHOD_DOC_VARARGS(KX_Camera, sphereInsideFrustum);
  EXP_PYMETHOD_DOC_O(KX_Camera, boxInsideFrustum);
  EXP_PYMETHOD_DOC_O(KX_Camera, pointInsideFrustum);

  EXP_PYMETHOD_DOC_NOARGS(KX_Camera, getCameraToWorld);
  EXP_PYMETHOD_DOC_NOARGS(KX_Camera, getWorldToCamera);

  EXP_PYMETHOD_DOC_VARARGS(KX_Camera, setViewport);
  EXP_PYMETHOD_DOC_NOARGS(KX_Camera, setOnTop);

  EXP_PYMETHOD_DOC_O(KX_Camera, getScreenPosition);
  EXP_PYMETHOD_DOC_VARARGS(KX_Camera, getScreenVect);
  EXP_PYMETHOD_DOC_VARARGS(KX_Camera, getScreenRay);

  static PyObject *pyattr_get_perspective(EXP_PyObjectPlus *self_v,
                                          const EXP_PYATTRIBUTE_DEF *attrdef);
  static int pyattr_set_perspective(EXP_PyObjectPlus *self_v,
                                    const EXP_PYATTRIBUTE_DEF *attrdef,
                                    PyObject *value);

  static PyObject *pyattr_get_lens(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
  static int pyattr_set_lens(EXP_PyObjectPlus *self_v,
                             const EXP_PYATTRIBUTE_DEF *attrdef,
                             PyObject *value);
  static PyObject *pyattr_get_fov(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
  static int pyattr_set_fov(EXP_PyObjectPlus *self_v,
                            const EXP_PYATTRIBUTE_DEF *attrdef,
                            PyObject *value);
  static PyObject *pyattr_get_ortho_scale(EXP_PyObjectPlus *self_v,
                                          const EXP_PYATTRIBUTE_DEF *attrdef);
  static int pyattr_set_ortho_scale(EXP_PyObjectPlus *self_v,
                                    const EXP_PYATTRIBUTE_DEF *attrdef,
                                    PyObject *value);
  static PyObject *pyattr_get_near(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
  static int pyattr_set_near(EXP_PyObjectPlus *self_v,
                             const EXP_PYATTRIBUTE_DEF *attrdef,
                             PyObject *value);
  static PyObject *pyattr_get_far(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
  static int pyattr_set_far(EXP_PyObjectPlus *self_v,
                            const EXP_PYATTRIBUTE_DEF *attrdef,
                            PyObject *value);
  static PyObject *pyattr_get_shift_x(EXP_PyObjectPlus *self_v,
                                      const EXP_PYATTRIBUTE_DEF *attrdef);
  static int pyattr_set_shift_x(EXP_PyObjectPlus *self_v,
                                const EXP_PYATTRIBUTE_DEF *attrdef,
                                PyObject *value);
  static PyObject *pyattr_get_shift_y(EXP_PyObjectPlus *self_v,
                                      const EXP_PYATTRIBUTE_DEF *attrdef);
  static int pyattr_set_shift_y(EXP_PyObjectPlus *self_v,
                                const EXP_PYATTRIBUTE_DEF *attrdef,
                                PyObject *value);

  static PyObject *pyattr_get_use_viewport(EXP_PyObjectPlus *self_v,
                                           const EXP_PYATTRIBUTE_DEF *attrdef);
  static int pyattr_set_use_viewport(EXP_PyObjectPlus *self_v,
                                     const EXP_PYATTRIBUTE_DEF *attrdef,
                                     PyObject *value);

  static PyObject *pyattr_get_projection_matrix(EXP_PyObjectPlus *self_v,
                                                const EXP_PYATTRIBUTE_DEF *attrdef);
  static int pyattr_set_projection_matrix(EXP_PyObjectPlus *self_v,
                                          const EXP_PYATTRIBUTE_DEF *attrdef,
                                          PyObject *value);

  static PyObject *pyattr_get_modelview_matrix(EXP_PyObjectPlus *self_v,
                                               const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_camera_to_world(EXP_PyObjectPlus *self_v,
                                              const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_world_to_camera(EXP_PyObjectPlus *self_v,
                                              const EXP_PYATTRIBUTE_DEF *attrdef);

  static PyObject *pyattr_get_INSIDE(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_OUTSIDE(EXP_PyObjectPlus *self_v,
                                      const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_INTERSECT(EXP_PyObjectPlus *self_v,
                                        const EXP_PYATTRIBUTE_DEF *attrdef);
#endif
};
