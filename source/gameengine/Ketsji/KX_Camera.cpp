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
 * Camera in the gameengine. Cameras are also used for views.
 */

/** \file gameengine/Ketsji/KX_Camera.cpp
 *  \ingroup ketsji
 */


#include "KX_Camera.h"
#include "KX_Scene.h"
#include "KX_Globals.h"
#include "KX_PyMath.h"
#include "KX_RayCast.h"

#include "RAS_ICanvas.h"

#include "GPU_glew.h"

#include <BLI_math_rotation.h>

KX_Camera::View::View()
	:projectionDirty(true),
	frustumDirty(true)
{
}

KX_Camera::KX_Camera(void *sgReplicationInfo,
                     SG_Callbacks callbacks,
                     const RAS_CameraData& camdata,
                     bool frustum_culling)
	:
	KX_GameObject(sgReplicationInfo, callbacks),
	m_camdata(camdata),
	m_frustum_culling(frustum_culling),
	m_lodDistanceFactor(1.0f),
	m_activityCulling(false),
	m_showDebugCameraFrustum(false)
{
}


KX_Camera::~KX_Camera()
{
}


EXP_Value *KX_Camera::GetReplica()
{
	KX_Camera *replica = new KX_Camera(*this);

	// this will copy properties and so on...
	replica->ProcessReplica();

	return replica;
}

void KX_Camera::ProcessReplica()
{
	KX_GameObject::ProcessReplica();
}

mt::mat3x4 KX_Camera::GetWorldToCamera() const
{
	return GetCameraToWorld().Inverse();
}



mt::mat3x4 KX_Camera::GetCameraToWorld() const
{
	return mt::mat3x4(NodeGetWorldOrientation(), NodeGetWorldPosition());
}

/**
 * Sets the projection matrix that is used by the rasterizer.
 */
void KX_Camera::SetProjectionMatrix(const mt::mat4 & mat, RAS_Rasterizer::StereoEye eye)
{
	View& view = m_views[eye];
	view.projection = mat;
	view.projectionDirty = false;
	// Ask to rebuild the frustum as the projection changed.
	view.frustumDirty = true;
}

/**
 * Sets the modelview matrix that is used by the rasterizer.
 */
void KX_Camera::SetModelviewMatrix(const mt::mat4 & mat, RAS_Rasterizer::StereoEye eye)
{
	View& view = m_views[eye];
	view.modelview = mat;
	view.frustumDirty = true;
}

/**
 * Gets the projection matrix that is used by the rasterizer.
 */
const mt::mat4& KX_Camera::GetProjectionMatrix(RAS_Rasterizer::StereoEye eye) const
{
	return m_views[eye].projection;
}

/**
 * Gets the modelview matrix that is used by the rasterizer.
 */
const mt::mat4& KX_Camera::GetModelviewMatrix(RAS_Rasterizer::StereoEye eye) const
{
	return m_views[eye].modelview;
}

bool KX_Camera::HasValidProjectionMatrix(RAS_Rasterizer::StereoEye eye) const
{
	return !m_views[eye].projectionDirty;
}

void KX_Camera::InvalidateProjectionMatrix()
{
	for (unsigned short i = 0; i < RAS_Rasterizer::RAS_STEREO_MAXEYE; ++i) {
		m_views[i].projectionDirty = true;
	}
}

void KX_Camera::UpdateView(RAS_Rasterizer* rasty, KX_Scene* scene, RAS_Rasterizer::StereoMode stereoMode,
		RAS_Rasterizer::StereoEye eye, const RAS_Rect& viewport, const RAS_Rect& area)
{
	View& view = m_views[eye];

	// Update modelview everytime.
	view.modelview = rasty->GetViewMatrix(stereoMode, eye, GetWorldToCamera(), m_camdata.m_perspective);

	// Update projection when setting changed.
	if (view.projectionDirty) {
		// First compute the frame frustum.
		ComputeFrameFrustum(viewport, area, scene->GetFramingType());

		if (m_camdata.m_perspective) {
			view.projection = rasty->GetFrustumMatrix(stereoMode, eye, m_camdata.m_focallength,
				m_frameFrustum.x1, m_frameFrustum.x2, m_frameFrustum.y1, m_frameFrustum.y2,
				m_frameFrustum.camnear, m_frameFrustum.camfar);
		}
		else {
			view.projection = rasty->GetOrthoMatrix(
				m_frameFrustum.x1, m_frameFrustum.x2, m_frameFrustum.y1, m_frameFrustum.y2,
				m_frameFrustum.camnear, m_frameFrustum.camfar);
		}

		view.projectionDirty = false;
	}
	// Ask to rebuild the frustum as the projection and/or modelview changed.
	view.frustumDirty = true;
}

/**
 * These getters retrieve the clip data and the focal length
 */
float KX_Camera::GetLens() const
{
	return m_camdata.m_lens;
}

float KX_Camera::GetScale() const
{
	return m_camdata.m_scale;
}

/**
 * Gets the horizontal size of the sensor - for camera matching.
 */
float KX_Camera::GetSensorWidth() const
{
	return m_camdata.m_sensor_x;
}

/**
 * Gets the vertical size of the sensor - for camera matching.
 */
float KX_Camera::GetSensorHeight() const
{
	return m_camdata.m_sensor_y;
}
/** Gets the mode FOV is calculating from sensor dimensions */
short KX_Camera::GetSensorFit() const
{
	return m_camdata.m_sensor_fit;
}

/**
 * Gets the horizontal shift of the sensor - for camera matching.
 */
float KX_Camera::GetShiftHorizontal() const
{
	return m_camdata.m_shift_x;
}

/**
 * Gets the vertical shift of the sensor - for camera matching.
 */
float KX_Camera::GetShiftVertical() const
{
	return m_camdata.m_shift_y;
}

float KX_Camera::GetCameraNear() const
{
	return m_camdata.m_clipstart;
}



float KX_Camera::GetCameraFar() const
{
	return m_camdata.m_clipend;
}

float KX_Camera::GetFocalLength() const
{
	return m_camdata.m_focallength;
}

float KX_Camera::GetZoom() const
{
	return m_camdata.m_zoom;
}

RAS_CameraData *KX_Camera::GetCameraData()
{
	return &m_camdata;
}

void KX_Camera::SetShowCameraFrustum(bool show)
{
	m_showDebugCameraFrustum = show;
}

bool KX_Camera::GetShowCameraFrustum() const
{
	return m_showDebugCameraFrustum;
}

float KX_Camera::GetLodDistanceFactor() const
{
	return m_lodDistanceFactor;
}

void KX_Camera::SetLodDistanceFactor(float lodfactor)
{
	m_lodDistanceFactor = lodfactor;
}

bool KX_Camera::GetActivityCulling() const
{
	return m_activityCulling;
}

void KX_Camera::SetActivityCulling(bool enable)
{
	m_activityCulling = enable;
}

void KX_Camera::ExtractFrustum(RAS_Rasterizer::StereoEye eye)
{
	View& view = m_views[eye];
	if (view.frustumDirty) {
		view.frustum = SG_Frustum(view.projection * view.modelview);
		view.frustumDirty = false;
	}
}

void KX_Camera::ComputeFrameFrustum(const RAS_Rect& viewport, const RAS_Rect& area, const RAS_FrameSettings& settings)
{
	if (m_camdata.m_perspective) {
		RAS_FramingManager::ComputeFrustum(
			settings,
			area,
			viewport,
			m_camdata.m_lens,
			m_camdata.m_sensor_x,
			m_camdata.m_sensor_y,
			m_camdata.m_sensor_fit,
			m_camdata.m_shift_x,
			m_camdata.m_shift_y,
			m_camdata.m_clipstart,
			m_camdata.m_clipend,
			m_frameFrustum);
	}
	else {
		RAS_FramingManager::ComputeOrtho(
			settings,
			area,
			viewport,
			m_camdata.m_scale,
			m_camdata.m_clipstart,
			m_camdata.m_clipend,
			m_camdata.m_sensor_fit,
			m_camdata.m_shift_x,
			m_camdata.m_shift_y,
			m_frameFrustum);
	}

	if (!m_camdata.m_useViewport) {
		m_frameFrustum.x1 *= m_camdata.m_zoom;
		m_frameFrustum.x2 *= m_camdata.m_zoom;
		m_frameFrustum.y1 *= m_camdata.m_zoom;
		m_frameFrustum.y2 *= m_camdata.m_zoom;
	}
}

const RAS_FrameFrustum& KX_Camera::GetFrameFrustum() const
{
	return m_frameFrustum;
}

const SG_Frustum& KX_Camera::GetFrustum(RAS_Rasterizer::StereoEye eye)
{
	ExtractFrustum(eye);
	return m_views[eye].frustum;
}

bool KX_Camera::GetFrustumCulling() const
{
	return m_frustum_culling;
}

void KX_Camera::EnableViewport(bool viewport)
{
	InvalidateProjectionMatrix(); // We need to reset projection matrix
	m_camdata.m_useViewport = viewport;
}

void KX_Camera::SetViewport(int left, int bottom, int right, int top)
{
	InvalidateProjectionMatrix();
	m_camdata.m_viewport = RAS_Rect(left, right, bottom, top);
}

bool KX_Camera::UseViewport() const
{
	return m_camdata.m_useViewport;
}

const RAS_Rect& KX_Camera::GetViewport() const
{
	return m_camdata.m_viewport;
}

#ifdef WITH_PYTHON
//----------------------------------------------------------------------------
//Python


PyMethodDef KX_Camera::Methods[] = {
	EXP_PYMETHODTABLE(KX_Camera, sphereInsideFrustum),
	EXP_PYMETHODTABLE_O(KX_Camera, boxInsideFrustum),
	EXP_PYMETHODTABLE_O(KX_Camera, pointInsideFrustum),
	EXP_PYMETHODTABLE_NOARGS(KX_Camera, getCameraToWorld),
	EXP_PYMETHODTABLE_NOARGS(KX_Camera, getWorldToCamera),
	EXP_PYMETHODTABLE(KX_Camera, setViewport),
	EXP_PYMETHODTABLE_NOARGS(KX_Camera, setOnTop),
	EXP_PYMETHODTABLE_O(KX_Camera, getScreenPosition),
	EXP_PYMETHODTABLE(KX_Camera, getScreenVect),
	EXP_PYMETHODTABLE(KX_Camera, getScreenRay),
	{nullptr, nullptr} //Sentinel
};

PyAttributeDef KX_Camera::Attributes[] = {

	EXP_PYATTRIBUTE_BOOL_RW("frustum_culling", KX_Camera, m_frustum_culling),
	EXP_PYATTRIBUTE_BOOL_RW("activityCulling", KX_Camera, m_activityCulling),
	EXP_PYATTRIBUTE_RW_FUNCTION("perspective", KX_Camera, pyattr_get_perspective, pyattr_set_perspective),

	EXP_PYATTRIBUTE_RW_FUNCTION("lens", KX_Camera,  pyattr_get_lens, pyattr_set_lens),
	EXP_PYATTRIBUTE_RW_FUNCTION("fov",  KX_Camera,  pyattr_get_fov,  pyattr_set_fov),
	EXP_PYATTRIBUTE_RW_FUNCTION("ortho_scale",  KX_Camera,  pyattr_get_ortho_scale, pyattr_set_ortho_scale),
	EXP_PYATTRIBUTE_RW_FUNCTION("near", KX_Camera,  pyattr_get_near, pyattr_set_near),
	EXP_PYATTRIBUTE_RW_FUNCTION("far",  KX_Camera,  pyattr_get_far,  pyattr_set_far),
	EXP_PYATTRIBUTE_RW_FUNCTION("shift_x",  KX_Camera,  pyattr_get_shift_x, pyattr_set_shift_x),
	EXP_PYATTRIBUTE_RW_FUNCTION("shift_y",  KX_Camera,  pyattr_get_shift_y,  pyattr_set_shift_y),
	EXP_PYATTRIBUTE_FLOAT_RW("lodDistanceFactor", 0.0f, FLT_MAX, KX_Camera, m_lodDistanceFactor),

	EXP_PYATTRIBUTE_RW_FUNCTION("useViewport",  KX_Camera,  pyattr_get_use_viewport,  pyattr_set_use_viewport),

	EXP_PYATTRIBUTE_RW_FUNCTION("projection_matrix", KX_Camera, pyattr_get_projectionMatrixLeft, pyattr_set_projectionMatrixLeft),
	EXP_PYATTRIBUTE_RW_FUNCTION("projectionMatrixLeft", KX_Camera, pyattr_get_projectionMatrixLeft, pyattr_set_projectionMatrixLeft),
	EXP_PYATTRIBUTE_RW_FUNCTION("projectionMatrixRight", KX_Camera, pyattr_get_projectionMatrixRight, pyattr_set_projectionMatrixRight),
	EXP_PYATTRIBUTE_RO_FUNCTION("modelview_matrix", KX_Camera,  pyattr_get_modelview_matrix),
	EXP_PYATTRIBUTE_RO_FUNCTION("camera_to_world",  KX_Camera,  pyattr_get_camera_to_world),
	EXP_PYATTRIBUTE_RO_FUNCTION("world_to_camera",  KX_Camera,  pyattr_get_world_to_camera),

	/* Grrr, functions for constants? */
	EXP_PYATTRIBUTE_RO_FUNCTION("INSIDE",   KX_Camera, pyattr_get_INSIDE),
	EXP_PYATTRIBUTE_RO_FUNCTION("OUTSIDE",  KX_Camera, pyattr_get_OUTSIDE),
	EXP_PYATTRIBUTE_RO_FUNCTION("INTERSECT",    KX_Camera, pyattr_get_INTERSECT),

	EXP_PYATTRIBUTE_NULL    //Sentinel
};

PyTypeObject KX_Camera::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"KX_Camera",
	sizeof(EXP_PyObjectPlus_Proxy),
	0,
	py_base_dealloc,
	0,
	0,
	0,
	0,
	py_base_repr,
	0,
	&KX_GameObject::Sequence,
	&KX_GameObject::Mapping,
	0, 0, 0,
	nullptr,
	nullptr,
	0,
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	0, 0, 0, 0, 0, 0, 0,
	Methods,
	0,
	0,
	&KX_GameObject::Type,
	0, 0, 0, 0, 0, 0,
	py_base_new
};

EXP_PYMETHODDEF_DOC_VARARGS(KX_Camera, sphereInsideFrustum,
                            "sphereInsideFrustum(center, radius) -> Integer\n"
                            "\treturns INSIDE, OUTSIDE or INTERSECT if the given sphere is\n"
                            "\tinside/outside/intersects this camera's viewing frustum.\n\n"
                            "\tcenter = the center of the sphere (in world coordinates.)\n"
                            "\tradius = the radius of the sphere\n\n"
                            "\tExample:\n"
                            "\timport bge.logic\n\n"
                            "\tco = bge.logic.getCurrentController()\n"
                            "\tcam = co.GetOwner()\n\n"
                            "\t# A sphere of radius 4.0 located at [x, y, z] = [1.0, 1.0, 1.0]\n"
                            "\tif (cam.sphereInsideFrustum([1.0, 1.0, 1.0], 4) != cam.OUTSIDE):\n"
                            "\t\t# Sphere is inside frustum !\n"
                            "\t\t# Do something useful !\n"
                            "\telse:\n"
                            "\t\t# Sphere is outside frustum\n"
                            )
{
	PyObject *pycenter;
	float radius;
	if (PyArg_ParseTuple(args, "Of:sphereInsideFrustum", &pycenter, &radius)) {
		mt::vec3 center;
		if (PyVecTo(pycenter, center)) {
			return PyLong_FromLong(GetFrustum(RAS_Rasterizer::RAS_STEREO_LEFTEYE).SphereInsideFrustum(center, radius)); /* new ref */
		}
	}

	PyErr_SetString(PyExc_TypeError, "camera.sphereInsideFrustum(center, radius): KX_Camera, expected arguments: (center, radius)");

	return nullptr;
}

EXP_PYMETHODDEF_DOC_O(KX_Camera, boxInsideFrustum,
                      "boxInsideFrustum(box) -> Integer\n"
                      "\treturns INSIDE, OUTSIDE or INTERSECT if the given box is\n"
                      "\tinside/outside/intersects this camera's viewing frustum.\n\n"
                      "\tbox = a list of the eight (8) corners of the box (in world coordinates.)\n\n"
                      "\tExample:\n"
                      "\timport bge.logic\n\n"
                      "\tco = bge.logic.getCurrentController()\n"
                      "\tcam = co.GetOwner()\n\n"
                      "\tbox = []\n"
                      "\tbox.append([-1.0, -1.0, -1.0])\n"
                      "\tbox.append([-1.0, -1.0,  1.0])\n"
                      "\tbox.append([-1.0,  1.0, -1.0])\n"
                      "\tbox.append([-1.0,  1.0,  1.0])\n"
                      "\tbox.append([ 1.0, -1.0, -1.0])\n"
                      "\tbox.append([ 1.0, -1.0,  1.0])\n"
                      "\tbox.append([ 1.0,  1.0, -1.0])\n"
                      "\tbox.append([ 1.0,  1.0,  1.0])\n\n"
                      "\tif (cam.boxInsideFrustum(box) != cam.OUTSIDE):\n"
                      "\t\t# Box is inside/intersects frustum !\n"
                      "\t\t# Do something useful !\n"
                      "\telse:\n"
                      "\t\t# Box is outside the frustum !\n"
                      )
{
	unsigned int num_points = PySequence_Size(value);
	if (num_points != 8) {
		PyErr_Format(PyExc_TypeError, "camera.boxInsideFrustum(box): KX_Camera, expected eight (8) points, got %d", num_points);
		return nullptr;
	}

	std::array<mt::vec3, 8> box;
	for (unsigned int p = 0; p < 8; p++)
	{
		PyObject *item = PySequence_GetItem(value, p); /* new ref */
		bool error = !PyVecTo(item, box[p]);
		Py_DECREF(item);
		if (error) {
			return nullptr;
		}
	}

	return PyLong_FromLong(GetFrustum(RAS_Rasterizer::RAS_STEREO_LEFTEYE).BoxInsideFrustum(box)); /* new ref */
}

EXP_PYMETHODDEF_DOC_O(KX_Camera, pointInsideFrustum,
                      "pointInsideFrustum(point) -> Bool\n"
                      "\treturns 1 if the given point is inside this camera's viewing frustum.\n\n"
                      "\tpoint = The point to test (in world coordinates.)\n\n"
                      "\tExample:\n"
                      "\timport bge.logic\n\n"
                      "\tco = bge.logic.getCurrentController()\n"
                      "\tcam = co.GetOwner()\n\n"
                      "\t# Test point [0.0, 0.0, 0.0]"
                      "\tif (cam.pointInsideFrustum([0.0, 0.0, 0.0])):\n"
                      "\t\t# Point is inside frustum !\n"
                      "\t\t# Do something useful !\n"
                      "\telse:\n"
                      "\t\t# Box is outside the frustum !\n"
                      )
{
	mt::vec3 point;
	if (PyVecTo(value, point)) {
		return PyLong_FromLong(GetFrustum(RAS_Rasterizer::RAS_STEREO_LEFTEYE).PointInsideFrustum(point)); /* new ref */
	}

	PyErr_SetString(PyExc_TypeError, "camera.pointInsideFrustum(point): KX_Camera, expected point argument.");
	return nullptr;
}

EXP_PYMETHODDEF_DOC_NOARGS(KX_Camera, getCameraToWorld,
                           "getCameraToWorld() -> Matrix4x4\n"
                           "\treturns the camera to world transformation matrix, as a list of four lists of four values.\n\n"
                           "\tie: [[1.0, 0.0, 0.0, 0.0], [0.0, 1.0, 0.0, 0.0], [0.0, 0.0, 1.0, 0.0], [0.0, 0.0, 0.0, 1.0]])\n"
                           )
{
	return PyObjectFrom(mt::mat4::FromAffineTransform(GetCameraToWorld())); /* new ref */
}

EXP_PYMETHODDEF_DOC_NOARGS(KX_Camera, getWorldToCamera,
                           "getWorldToCamera() -> Matrix4x4\n"
                           "\treturns the world to camera transformation matrix, as a list of four lists of four values.\n\n"
                           "\tie: [[1.0, 0.0, 0.0, 0.0], [0.0, 1.0, 0.0, 0.0], [0.0, 0.0, 1.0, 0.0], [0.0, 0.0, 0.0, 1.0]])\n"
                           )
{
	return PyObjectFrom(mt::mat4::FromAffineTransform(GetWorldToCamera())); /* new ref */
}

EXP_PYMETHODDEF_DOC_VARARGS(KX_Camera, setViewport,
                            "setViewport(left, bottom, right, top)\n"
                            "Sets this camera's viewport\n")
{
	int left, bottom, right, top;
	if (!PyArg_ParseTuple(args, "iiii:setViewport", &left, &bottom, &right, &top)) {
		return nullptr;
	}

	SetViewport(left, bottom, right, top);
	Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC_NOARGS(KX_Camera, setOnTop,
                           "setOnTop()\n"
                           "Sets this camera's viewport on top\n")
{
	GetScene()->SetCameraOnTop(this);
	Py_RETURN_NONE;
}

PyObject *KX_Camera::pyattr_get_perspective(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_Camera *self = static_cast<KX_Camera *>(self_v);
	return PyBool_FromLong(self->m_camdata.m_perspective);
}

int KX_Camera::pyattr_set_perspective(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_Camera *self = static_cast<KX_Camera *>(self_v);
	int param = PyObject_IsTrue(value);
	if (param == -1) {
		PyErr_SetString(PyExc_AttributeError, "camera.perspective = bool: KX_Camera, expected True/False or 0/1");
		return PY_SET_ATTR_FAIL;
	}

	self->m_camdata.m_perspective = param;
	self->InvalidateProjectionMatrix();
	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_Camera::pyattr_get_lens(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_Camera *self = static_cast<KX_Camera *>(self_v);
	return PyFloat_FromDouble(self->m_camdata.m_lens);
}

int KX_Camera::pyattr_set_lens(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_Camera *self = static_cast<KX_Camera *>(self_v);
	float param = PyFloat_AsDouble(value);
	if (param == -1) {
		PyErr_SetString(PyExc_AttributeError, "camera.lens = float: KX_Camera, expected a float greater than zero");
		return PY_SET_ATTR_FAIL;
	}

	self->m_camdata.m_lens = param;
	self->InvalidateProjectionMatrix();
	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_Camera::pyattr_get_fov(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_Camera *self = static_cast<KX_Camera *>(self_v);

	float lens = self->m_camdata.m_lens;
	float width = self->m_camdata.m_sensor_x;
	float fov = 2.0f * atanf(0.5f * width / lens);

	return PyFloat_FromDouble(RAD2DEGF(fov));
}

int KX_Camera::pyattr_set_fov(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_Camera *self = static_cast<KX_Camera *>(self_v);
	float fov = PyFloat_AsDouble(value);
	if (fov <= 0.0f) {
		PyErr_SetString(PyExc_AttributeError, "camera.fov = float: KX_Camera, expected a float greater than zero");
		return PY_SET_ATTR_FAIL;
	}

	float width = self->m_camdata.m_sensor_x;
	float lens = width / (2.0f * tanf(0.5f * DEG2RADF(fov)));

	self->m_camdata.m_lens = lens;
	self->InvalidateProjectionMatrix();
	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_Camera::pyattr_get_ortho_scale(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_Camera *self = static_cast<KX_Camera *>(self_v);
	return PyFloat_FromDouble(self->m_camdata.m_scale);
}

int KX_Camera::pyattr_set_ortho_scale(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_Camera *self = static_cast<KX_Camera *>(self_v);
	float param = PyFloat_AsDouble(value);
	if (param == -1) {
		PyErr_SetString(PyExc_AttributeError, "camera.ortho_scale = float: KX_Camera, expected a float greater than zero");
		return PY_SET_ATTR_FAIL;
	}

	self->m_camdata.m_scale = param;
	self->InvalidateProjectionMatrix();
	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_Camera::pyattr_get_near(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_Camera *self = static_cast<KX_Camera *>(self_v);
	return PyFloat_FromDouble(self->m_camdata.m_clipstart);
}

int KX_Camera::pyattr_set_near(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_Camera *self = static_cast<KX_Camera *>(self_v);
	float param = PyFloat_AsDouble(value);
	if (param == -1) {
		PyErr_SetString(PyExc_AttributeError, "camera.near = float: KX_Camera, expected a float greater than zero");
		return PY_SET_ATTR_FAIL;
	}

	self->m_camdata.m_clipstart = param;
	self->InvalidateProjectionMatrix();
	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_Camera::pyattr_get_far(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_Camera *self = static_cast<KX_Camera *>(self_v);
	return PyFloat_FromDouble(self->m_camdata.m_clipend);
}

int KX_Camera::pyattr_set_far(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_Camera *self = static_cast<KX_Camera *>(self_v);
	float param = PyFloat_AsDouble(value);
	if (param == -1) {
		PyErr_SetString(PyExc_AttributeError, "camera.far = float: KX_Camera, expected a float greater than zero");
		return PY_SET_ATTR_FAIL;
	}

	self->m_camdata.m_clipend = param;
	self->InvalidateProjectionMatrix();
	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_Camera::pyattr_get_shift_x(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_Camera *self = static_cast<KX_Camera *>(self_v);
	return PyFloat_FromDouble(self->m_camdata.m_shift_x);
}

int KX_Camera::pyattr_set_shift_x(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_Camera *self = static_cast<KX_Camera *>(self_v);
	float param = PyFloat_AsDouble(value);
	if (param == -1) {
		PyErr_SetString(PyExc_AttributeError, "camera.shift_x = float: KX_Camera, expected a float greater than zero");
		return PY_SET_ATTR_FAIL;
	}

	self->m_camdata.m_shift_x = param;
	self->InvalidateProjectionMatrix();
	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_Camera::pyattr_get_shift_y(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_Camera *self = static_cast<KX_Camera *>(self_v);
	return PyFloat_FromDouble(self->m_camdata.m_shift_y);
}

int KX_Camera::pyattr_set_shift_y(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_Camera *self = static_cast<KX_Camera *>(self_v);
	float param = PyFloat_AsDouble(value);
	if (param == -1) {
		PyErr_SetString(PyExc_AttributeError, "camera.shift_y = float: KX_Camera, expected a float greater than zero");
		return PY_SET_ATTR_FAIL;
	}

	self->m_camdata.m_shift_y = param;
	self->InvalidateProjectionMatrix();
	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_Camera::pyattr_get_use_viewport(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_Camera *self = static_cast<KX_Camera *>(self_v);
	return PyBool_FromLong(self->UseViewport());
}

int KX_Camera::pyattr_set_use_viewport(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_Camera *self = static_cast<KX_Camera *>(self_v);
	int param = PyObject_IsTrue(value);
	if (param == -1) {
		PyErr_SetString(PyExc_AttributeError, "camera.useViewport = bool: KX_Camera, expected True or False");
		return PY_SET_ATTR_FAIL;
	}
	self->EnableViewport((bool)param);
	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_Camera::pyattr_get_projectionMatrixLeft(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_Camera *self = static_cast<KX_Camera *>(self_v);
	return PyObjectFrom(self->GetProjectionMatrix(RAS_Rasterizer::RAS_STEREO_LEFTEYE));
}

int KX_Camera::pyattr_set_projectionMatrixLeft(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_Camera *self = static_cast<KX_Camera *>(self_v);
	mt::mat4 mat;
	if (!PyMatTo(value, mat)) {
		return PY_SET_ATTR_FAIL;
	}

	self->SetProjectionMatrix(mat, RAS_Rasterizer::RAS_STEREO_LEFTEYE);
	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_Camera::pyattr_get_projectionMatrixRight(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_Camera *self = static_cast<KX_Camera *>(self_v);
	return PyObjectFrom(self->GetProjectionMatrix(RAS_Rasterizer::RAS_STEREO_RIGHTEYE));
}

int KX_Camera::pyattr_set_projectionMatrixRight(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_Camera *self = static_cast<KX_Camera *>(self_v);
	mt::mat4 mat;
	if (!PyMatTo(value, mat)) {
		return PY_SET_ATTR_FAIL;
	}

	self->SetProjectionMatrix(mat, RAS_Rasterizer::RAS_STEREO_RIGHTEYE);
	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_Camera::pyattr_get_modelview_matrix(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_Camera *self = static_cast<KX_Camera *>(self_v);
	return PyObjectFrom(mt::mat4::FromAffineTransform(self->GetWorldToCamera()));
}

PyObject *KX_Camera::pyattr_get_camera_to_world(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_Camera *self = static_cast<KX_Camera *>(self_v);
	return PyObjectFrom(mt::mat4::FromAffineTransform(self->GetCameraToWorld()));
}

PyObject *KX_Camera::pyattr_get_world_to_camera(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_Camera *self = static_cast<KX_Camera *>(self_v);
	return PyObjectFrom(mt::mat4::FromAffineTransform(self->GetWorldToCamera()));
}


PyObject *KX_Camera::pyattr_get_INSIDE(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	return PyLong_FromLong(INSIDE);
}
PyObject *KX_Camera::pyattr_get_OUTSIDE(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	return PyLong_FromLong(OUTSIDE);
}
PyObject *KX_Camera::pyattr_get_INTERSECT(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	return PyLong_FromLong(INTERSECT);
}


bool ConvertPythonToCamera(KX_Scene *scene, PyObject *value, KX_Camera **object, bool py_none_ok, const char *error_prefix)
{
	if (value == nullptr) {
		PyErr_Format(PyExc_TypeError, "%s, python pointer nullptr, should never happen", error_prefix);
		*object = nullptr;
		return false;
	}

	if (value == Py_None) {
		*object = nullptr;

		if (py_none_ok) {
			return true;
		}
		else {
			PyErr_Format(PyExc_TypeError, "%s, expected KX_Camera or a KX_Camera name, None is invalid", error_prefix);
			return false;
		}
	}

	if (PyUnicode_Check(value)) {
		std::string value_str = _PyUnicode_AsString(value);
		*object = scene->GetCameraList()->FindValue(value_str);

		if (*object) {
			return true;
		}
		else {
			PyErr_Format(PyExc_ValueError,
			             "%s, requested name \"%s\" did not match any KX_Camera in this scene",
			             error_prefix, _PyUnicode_AsString(value));
			return false;
		}
	}

	if (PyObject_TypeCheck(value, &KX_Camera::Type)) {
		*object = static_cast<KX_Camera *>EXP_PROXY_REF(value);

		/* sets the error */
		if (*object == nullptr) {
			PyErr_Format(PyExc_SystemError, "%s, " EXP_PROXY_ERROR_MSG, error_prefix);
			return false;
		}

		return true;
	}

	*object = nullptr;

	if (py_none_ok) {
		PyErr_Format(PyExc_TypeError, "%s, expect a KX_Camera, a string or None", error_prefix);
	}
	else {
		PyErr_Format(PyExc_TypeError, "%s, expect a KX_Camera or a string", error_prefix);
	}

	return false;
}

EXP_PYMETHODDEF_DOC_O(KX_Camera, getScreenPosition,
                      "getScreenPosition()\n"
                      )

{
	mt::vec3 vect;
	KX_GameObject *obj = nullptr;

	if (!PyVecTo(value, vect)) {
		PyErr_Clear();

		if (ConvertPythonToGameObject(GetScene()->GetLogicManager(), value, &obj, false, "")) {
			PyErr_Clear();
			vect = mt::vec3(obj->NodeGetWorldPosition());
		}
		else {
			PyErr_SetString(PyExc_TypeError, "Error in getScreenPosition. Expected a Vector3 or a KX_GameObject or a string for a name of a KX_GameObject");
			return nullptr;
		}
	}

	const GLint *viewport;
	GLdouble win[3];
	GLdouble dmodelmatrix[16];
	GLdouble dprojmatrix[16];

	const mt::mat4 modelmatrix = mt::mat4::FromAffineTransform(GetWorldToCamera());
	const mt::mat4& projmatrix = GetProjectionMatrix(RAS_Rasterizer::RAS_STEREO_LEFTEYE);

	for (unsigned short i = 0; i < 16; ++i) {
		dmodelmatrix[i] = modelmatrix[i];
		dprojmatrix[i] = projmatrix[i];
	}

	viewport = KX_GetActiveEngine()->GetCanvas()->GetViewPort();

	gluProject(vect[0], vect[1], vect[2], dmodelmatrix, dprojmatrix, viewport, &win[0], &win[1], &win[2]);

	vect[0] =  (win[0] - viewport[0]) / viewport[2];
	vect[1] =  (win[1] - viewport[1]) / viewport[3];

	vect[1] = 1.0f - vect[1]; //to follow Blender window coordinate system (Top-Down)

	PyObject *ret = PyTuple_New(2);
	if (ret) {
		PyTuple_SET_ITEM(ret, 0, PyFloat_FromDouble(vect[0]));
		PyTuple_SET_ITEM(ret, 1, PyFloat_FromDouble(vect[1]));
		return ret;
	}

	return nullptr;
}

EXP_PYMETHODDEF_DOC_VARARGS(KX_Camera, getScreenVect,
                            "getScreenVect()\n"
                            )
{
	float x, y;
	if (!PyArg_ParseTuple(args, "ff:getScreenVect", &x, &y)) {
		return nullptr;
	}

	y = 1.0 - y; //to follow Blender window coordinate system (Top-Down)

	const mt::mat4 modelmatrix = mt::mat4::FromAffineTransform(GetWorldToCamera());

	RAS_ICanvas *canvas = KX_GetActiveEngine()->GetCanvas();
	const int width = canvas->GetWidth();
	const int height = canvas->GetHeight();

	const mt::vec3 vect(x * width, y * height, 0.0f);
	const mt::vec3 screenpos = mt::mat4::UnProject(vect, modelmatrix, GetProjectionMatrix(RAS_Rasterizer::RAS_STEREO_LEFTEYE), width, height);
	const mt::vec3 ret = (NodeGetLocalPosition() - screenpos).Normalized();

	return PyObjectFrom(ret);
}

EXP_PYMETHODDEF_DOC_VARARGS(KX_Camera, getScreenRay,
                            "getScreenRay()\n"
                            )
{
	float x, y, dist;
	char *propName = nullptr;

	if (!PyArg_ParseTuple(args, "fff|s:getScreenRay", &x, &y, &dist, &propName)) {
		return nullptr;
	}

	y = 1.0 - y; //to follow Blender window coordinate system (Top-Down)

	const mt::mat4 modelmatrix = mt::mat4::FromAffineTransform(GetWorldToCamera());

	RAS_ICanvas *canvas = KX_GetActiveEngine()->GetCanvas();
	const int width = canvas->GetWidth();
	const int height = canvas->GetHeight();

	mt::vec3 fromPoint;
	mt::vec3 toPoint;

	// Unproject a point in near plane.
	const mt::vec3 point(x * width, y * height, 0.0f);
	const mt::vec3 screenpos = mt::mat4::UnProject(point, modelmatrix, GetProjectionMatrix(RAS_Rasterizer::RAS_STEREO_LEFTEYE), width, height);

	// For perpspective the vector is from camera center to unprojected point.
	if (m_camdata.m_perspective) {
		fromPoint = NodeGetWorldPosition();
		toPoint = screenpos;
	}
	// For orthographic the vector is the same as the -Z rotation axis but start from unprojected point.
	else {
		fromPoint = screenpos;
		toPoint = fromPoint - NodeGetWorldOrientation().GetColumn(2);
	}

	if (dist != 0.0f) {
		toPoint = fromPoint + dist * (toPoint - fromPoint).SafeNormalized(mt::axisX3);
	}

	PHY_IPhysicsEnvironment *pe = GetScene()->GetPhysicsEnvironment();
	PHY_IPhysicsController *spc = m_physicsController.get();
	KX_GameObject *parent = GetParent();
	if (!spc && parent) {
		spc = parent->GetPhysicsController();
	}

	RayCastData rayData("", false, (1u << OB_MAX_COL_MASKS) - 1);
	KX_RayCast::Callback<KX_Camera, RayCastData> callback(this, spc, &rayData);
	if (KX_RayCast::RayTest(pe, fromPoint, toPoint, callback) && rayData.m_hitObject) {
		return rayData.m_hitObject->GetProxy();
	}

	Py_RETURN_NONE;
}
#endif
