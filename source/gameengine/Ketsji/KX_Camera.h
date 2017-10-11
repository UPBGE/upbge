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

#ifndef __KX_CAMERA_H__
#define __KX_CAMERA_H__

#include "KX_GameObject.h"

#include "SG_Frustum.h"

#include "RAS_CameraData.h"

#ifdef WITH_PYTHON
/* utility conversion function */
bool ConvertPythonToCamera(KX_Scene *scene, PyObject *value, KX_Camera **object, bool py_none_ok, const char *error_prefix);
#endif

class KX_Camera : public KX_GameObject
{
	Py_Header(KX_Camera)
protected:
	friend class KX_Scene;
	/** Camera parameters (clips distances, focal length). These
	 * params are closely tied to Blender. In the gameengine, only the
	 * projection and modelview matrices are relevant. There's a
	 * conversion being done in the engine class. Why is it stored
	 * here? It doesn't really have a function here. */
	RAS_CameraData	m_camdata;

	/**
	 * Storage for the projection matrix that is passed to the
	 * rasterizer. */
	mt::mat4 m_projection_matrix;

	/**
	 * Storage for the modelview matrix that is passed to the
	 * rasterizer. */
	mt::mat4 m_modelview_matrix;

	/**
	 * true if the view frustum (modelview/projection matrix)
	 * has changed - the clip planes (m_planes) will have to be
	 * regenerated.
	 */
	bool         m_dirty;
	/**
	 * true if the frustum planes have been normalized.
	 */
	bool         m_normalized;
	
	/**
	 * View Frustum clip planes.
	 */
	mt::vec4   m_planes[6];
	
	/**
	 * This camera is frustum culling.
	 * Some cameras (ie if the game was started from a non camera view should not cull.)
	 */
	bool         m_frustum_culling;
	
	/**
	 * true if this camera has a valid projection matrix.
	 */
	bool         m_set_projection_matrix;

	/** Distance factor for level of detail*/
	float m_lodDistanceFactor;

	/// Enable object activity culling for this camera.
	bool m_activityCulling;

	/**
	 * Show Debug Camera Frustum?
	 */
	bool m_showDebugCameraFrustum;

	SG_Frustum m_frustum;

	void ExtractFrustum();

public:

	enum { INSIDE, INTERSECT, OUTSIDE };

	KX_Camera(void* sgReplicationInfo,SG_Callbacks callbacks,const RAS_CameraData& camdata, bool frustum_culling = true);
	virtual ~KX_Camera();

	/** 
	 * Inherited from EXP_Value -- return a new copy of this
	 * instance allocated on the heap. Ownership of the new 
	 * object belongs with the caller.
	 */
	virtual	EXP_Value*
	GetReplica(
	);
	virtual void ProcessReplica();

	mt::mat3x4		GetWorldToCamera() const;
	mt::mat3x4		GetCameraToWorld() const;

	/** Sets the projection matrix that is used by the rasterizer. */
	void				SetProjectionMatrix(const mt::mat4 & mat);

	/** Sets the modelview matrix that is used by the rasterizer. */
	void				SetModelviewMatrix(const mt::mat4 & mat);
		
	/** Gets the projection matrix that is used by the rasterizer. */
	const mt::mat4&		GetProjectionMatrix() const;
	
	/** returns true if this camera has been set a projection matrix. */
	bool				hasValidProjectionMatrix() const;
	
	/** Sets the validity of the projection matrix.  Call this if you change camera
	 *  data (eg lens, near plane, far plane) and require the projection matrix to be
	 *  recalculated.
	 */
	void				InvalidateProjectionMatrix(bool valid = false);
	
	/** Gets the modelview matrix that is used by the rasterizer. 
	 *  \warning If the Camera is a dynamic object then this method may return garbage.  Use GetWorldToCamera() instead.
	 */
	const mt::mat4&		GetModelviewMatrix() const;

	/** Gets the aperture. */
	float				GetLens() const;
	/** Gets the ortho scale. */
	float				GetScale() const;
	/** Gets the horizontal size of the sensor - for camera matching */
	float				GetSensorWidth() const;
	/** Gets the vertical size of the sensor - for camera matching */
	float				GetSensorHeight() const;
	/** Gets the mode FOV is calculating from sensor dimensions */
	short				GetSensorFit() const;
	/** Gets the horizontal shift of the sensor - for camera matching */
	float				GetShiftHorizontal() const;
	/** Gets the vertical shift of the sensor - for camera matching */
	float				GetShiftVertical() const;
	/** Gets the near clip distance. */
	float				GetCameraNear() const;
	/** Gets the far clip distance. */
	float				GetCameraFar() const;
	/** Gets the focal length (only used for stereo rendering) */
	float				GetFocalLength() const;
	float GetZoom() const;
	/** Gets all camera data. */
	RAS_CameraData*		GetCameraData();

	/** Get/Set show camera frustum */
	void SetShowCameraFrustum(bool show);
	bool GetShowCameraFrustum() const;

	/** Get level of detail distance factor */
	float GetLodDistanceFactor() const;
	/** Set level of detail distance factor */
	void SetLodDistanceFactor(float lodfactor);

	bool GetActivityCulling() const;
	void SetActivityCulling(bool enable);

	const SG_Frustum& GetFrustum();

	/**
	 * Gets this camera's culling status.
	 */
	bool GetFrustumCulling() const;
	
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

	virtual ObjectTypes GetObjectType() const { return OBJECT_TYPE_CAMERA; }

#ifdef WITH_PYTHON
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
	
	bool pyattr_get_perspective();
	void pyattr_set_perspective(bool value);

	float pyattr_get_lens();
	void pyattr_set_lens(float value);
	float pyattr_get_fov();
	void pyattr_set_fov(float value);
	float pyattr_get_ortho_scale();
	void pyattr_set_ortho_scale(float value);
	float pyattr_get_near();
	void pyattr_set_near(float value);
	float pyattr_get_far();
	void pyattr_set_far(float value);
	float pyattr_get_shift_x();
	void pyattr_set_shift_x(float value);
	float pyattr_get_shift_y();
	void pyattr_set_shift_y(float value);

	bool pyattr_get_use_viewport();
	void pyattr_set_use_viewport(bool value);
	
	mt::mat4 pyattr_get_projection_matrix();
	void pyattr_set_projection_matrix(const mt::mat4& value);
	
	mt::mat4 pyattr_get_modelview_matrix();
	mt::mat4 pyattr_get_camera_to_world();
	mt::mat4 pyattr_get_world_to_camera();
	
	int pyattr_get_INSIDE();
	int pyattr_get_OUTSIDE();
	int pyattr_get_INTERSECT();
#endif
};

#endif  /* __KX_CAMERA_H__ */
