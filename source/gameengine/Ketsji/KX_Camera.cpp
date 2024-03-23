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

#include <epoxy/gl.h>
#include "GPU_matrix.hh"
#include "GPU_viewport.hh"

#include "DNA_camera_types.h"
#include "KX_Globals.h"
#include "KX_PyMath.h"
#include "KX_RayCast.h"
#include "RAS_ICanvas.h"

KX_Camera::KX_Camera()
    : KX_GameObject(),
      m_gpuViewport(nullptr),  // eevee
      m_dirty(true),
      m_normalized(false),
      m_set_projection_matrix(false),
      m_delete_node(false),
      m_activityCulling(false),
      m_lodDistanceFactor(1.0f),
      m_showDebugCameraFrustum(false)
{
  // setting a name would be nice...
  m_name = "cam";
  m_projection_matrix.setIdentity();
  m_modelview_matrix.setIdentity();
}

KX_Camera::~KX_Camera()
{
  RemoveGPUViewport();
  if (m_delete_node && m_pSGNode) {
    // for shadow camera, avoids memleak
    delete m_pSGNode;
    m_pSGNode = nullptr;
  }
}

void KX_Camera::SetBlenderObject(Object *obj)
{
  KX_GameObject::SetBlenderObject(obj);

  Camera *ca = static_cast<Camera *>(obj->data);

  RAS_CameraData camdata(ca->lens,
                         ca->ortho_scale,
                         ca->sensor_x,
                         ca->sensor_y,
                         ca->sensor_fit,
                         ca->shiftx,
                         ca->shifty,
                         ca->clip_start,
                         ca->clip_end,
                         ca->type == CAM_PERSP);

  SetName(ca->id.name + 2);
  SetLodDistanceFactor(ca->lodfactor);
  SetActivityCulling(ca->gameflag & GAME_CAM_OBJECT_ACTIVITY_CULLING);

  SetCameraData(camdata);
}

void KX_Camera::SetCameraData(const RAS_CameraData &camdata)
{
  m_camdata = camdata;
}

GPUViewport *KX_Camera::GetGPUViewport()
{
  if (!m_gpuViewport) {
    m_gpuViewport = GPU_viewport_create();
  }
  return m_gpuViewport;
}

void KX_Camera::RemoveGPUViewport()
{
  if (m_gpuViewport && m_gpuViewport != GetScene()->GetCurrentGPUViewport()) {
    GPU_viewport_free(m_gpuViewport);
    m_gpuViewport = nullptr;
  }
}

KX_PythonProxy *KX_Camera::NewInstance()
{
  return new KX_Camera(*this);
}

void KX_Camera::ProcessReplica()
{
  KX_GameObject::ProcessReplica();
  // replicated camera are always registered in the scene
  m_delete_node = false;
}

MT_Transform KX_Camera::GetWorldToCamera() const
{
  MT_Transform camtrans;
  camtrans.invert(MT_Transform(NodeGetWorldPosition(), NodeGetWorldOrientation()));

  return camtrans;
}

MT_Transform KX_Camera::GetCameraToWorld() const
{
  return MT_Transform(NodeGetWorldPosition(), NodeGetWorldOrientation());
}

/**
 * Sets the projection matrix that is used by the rasterizer.
 */
void KX_Camera::SetProjectionMatrix(const MT_Matrix4x4 &mat)
{
  m_projection_matrix = mat;
  m_dirty = true;
  m_set_projection_matrix = true;
}

/**
 * Sets the modelview matrix that is used by the rasterizer.
 */
void KX_Camera::SetModelviewMatrix(const MT_Matrix4x4 &mat)
{
  m_modelview_matrix = mat;
  m_dirty = true;
}

/**
 * Gets the projection matrix that is used by the rasterizer.
 */
const MT_Matrix4x4 &KX_Camera::GetProjectionMatrix() const
{
  return m_projection_matrix;
}

/**
 * Gets the modelview matrix that is used by the rasterizer.
 */
const MT_Matrix4x4 &KX_Camera::GetModelviewMatrix() const
{
  return m_modelview_matrix;
}

bool KX_Camera::hasValidProjectionMatrix() const
{
  return m_set_projection_matrix;
}

void KX_Camera::InvalidateProjectionMatrix(bool valid)
{
  m_set_projection_matrix = valid;
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

void KX_Camera::ExtractFrustum()
{
  if (m_dirty) {
    m_frustum = SG_Frustum(m_projection_matrix * m_modelview_matrix);
    m_dirty = false;
  }
}

const SG_Frustum &KX_Camera::GetFrustum()
{
  ExtractFrustum();
  return m_frustum;
}

void KX_Camera::EnableViewport(bool viewport)
{
  InvalidateProjectionMatrix(false);  // We need to reset projection matrix
  m_camdata.m_viewport = viewport;
}

void KX_Camera::SetViewport(int left, int bottom, int right, int top)
{
  m_camdata.m_viewportleft = left;
  m_camdata.m_viewportbottom = bottom;
  m_camdata.m_viewportright = right;
  m_camdata.m_viewporttop = top;
}

bool KX_Camera::GetViewport() const
{
  return m_camdata.m_viewport;
}

int KX_Camera::GetViewportLeft() const
{
  return m_camdata.m_viewportleft;
}

int KX_Camera::GetViewportBottom() const
{
  return m_camdata.m_viewportbottom;
}

int KX_Camera::GetViewportRight() const
{
  return m_camdata.m_viewportright;
}

int KX_Camera::GetViewportTop() const
{
  return m_camdata.m_viewporttop;
}

void KX_Camera::MarkForDeletion()
{
  m_delete_node = true;
}

#ifdef WITH_PYTHON
//----------------------------------------------------------------------------
// Python

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
    {nullptr, nullptr}  // Sentinel
};

PyAttributeDef KX_Camera::Attributes[] = {

    EXP_PYATTRIBUTE_RW_FUNCTION(
        "perspective", KX_Camera, pyattr_get_perspective, pyattr_set_perspective),

    EXP_PYATTRIBUTE_RW_FUNCTION("lens", KX_Camera, pyattr_get_lens, pyattr_set_lens),
    EXP_PYATTRIBUTE_RW_FUNCTION("fov", KX_Camera, pyattr_get_fov, pyattr_set_fov),
    EXP_PYATTRIBUTE_RW_FUNCTION(
        "ortho_scale", KX_Camera, pyattr_get_ortho_scale, pyattr_set_ortho_scale),
    EXP_PYATTRIBUTE_RW_FUNCTION("near", KX_Camera, pyattr_get_near, pyattr_set_near),
    EXP_PYATTRIBUTE_RW_FUNCTION("far", KX_Camera, pyattr_get_far, pyattr_set_far),
    EXP_PYATTRIBUTE_RW_FUNCTION("shift_x", KX_Camera, pyattr_get_shift_x, pyattr_set_shift_x),
    EXP_PYATTRIBUTE_RW_FUNCTION("shift_y", KX_Camera, pyattr_get_shift_y, pyattr_set_shift_y),
    EXP_PYATTRIBUTE_FLOAT_RW("lodDistanceFactor", 0.0f, FLT_MAX, KX_Camera, m_lodDistanceFactor),

    EXP_PYATTRIBUTE_RW_FUNCTION(
        "useViewport", KX_Camera, pyattr_get_use_viewport, pyattr_set_use_viewport),

    EXP_PYATTRIBUTE_RW_FUNCTION("projection_matrix",
                                KX_Camera,
                                pyattr_get_projection_matrix,
                                pyattr_set_projection_matrix),
    EXP_PYATTRIBUTE_RO_FUNCTION("modelview_matrix", KX_Camera, pyattr_get_modelview_matrix),
    EXP_PYATTRIBUTE_RO_FUNCTION("camera_to_world", KX_Camera, pyattr_get_camera_to_world),
    EXP_PYATTRIBUTE_RO_FUNCTION("world_to_camera", KX_Camera, pyattr_get_world_to_camera),

    EXP_PYATTRIBUTE_BOOL_RW("activityCulling", KX_Camera, m_activityCulling),

    /* Grrr, functions for constants? */
    EXP_PYATTRIBUTE_RO_FUNCTION("INSIDE", KX_Camera, pyattr_get_INSIDE),
    EXP_PYATTRIBUTE_RO_FUNCTION("OUTSIDE", KX_Camera, pyattr_get_OUTSIDE),
    EXP_PYATTRIBUTE_RO_FUNCTION("INTERSECT", KX_Camera, pyattr_get_INTERSECT),

    EXP_PYATTRIBUTE_NULL  // Sentinel
};

PyObject *KX_Camera::game_object_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
  KX_Camera *obj = new KX_Camera();

  PyObject *proxy = py_base_new(type, PyTuple_Pack(1, obj->GetProxy()), kwds);
  if (!proxy) {
    delete obj;
    return nullptr;
  }

  return proxy;
}

PyTypeObject KX_Camera::Type = {PyVarObject_HEAD_INIT(nullptr, 0) "KX_Camera",
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
                                0,
                                0,
                                0,
                                nullptr,
                                nullptr,
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
                                &KX_GameObject::Type,
                                0,
                                0,
                                0,
                                0,
                                0,
                                0,
                                game_object_new};

EXP_PYMETHODDEF_DOC_VARARGS(KX_Camera,
                            sphereInsideFrustum,
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
                            "\t\t# Sphere is outside frustum\n")
{
  PyObject *pycenter;
  float radius;
  if (PyArg_ParseTuple(args, "Of:sphereInsideFrustum", &pycenter, &radius)) {
    MT_Vector3 center;
    if (PyVecTo(pycenter, center)) {
      return PyLong_FromLong(GetFrustum().SphereInsideFrustum(center, radius)); /* new ref */
    }
  }

  PyErr_SetString(PyExc_TypeError,
                  "camera.sphereInsideFrustum(center, radius): KX_Camera, expected arguments: "
                  "(center, radius)");

  return nullptr;
}

EXP_PYMETHODDEF_DOC_O(
    KX_Camera,
    boxInsideFrustum,
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
    "\t\t# Box is outside the frustum !\n")
{
  unsigned int num_points = PySequence_Size(value);
  if (num_points != 8) {
    PyErr_Format(PyExc_TypeError,
                 "camera.boxInsideFrustum(box): KX_Camera, expected eight (8) points, got %d",
                 num_points);
    return nullptr;
  }

  std::array<MT_Vector3, 8> box;
  for (unsigned int p = 0; p < 8; p++) {
    PyObject *item = PySequence_GetItem(value, p); /* new ref */
    bool error = !PyVecTo(item, box[p]);
    Py_DECREF(item);
    if (error)
      return nullptr;
  }

  return PyLong_FromLong(GetFrustum().BoxInsideFrustum(box)); /* new ref */
}

EXP_PYMETHODDEF_DOC_O(KX_Camera,
                      pointInsideFrustum,
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
                      "\t\t# Box is outside the frustum !\n")
{
  MT_Vector3 point;
  if (PyVecTo(value, point)) {
    return PyLong_FromLong(GetFrustum().PointInsideFrustum(point)); /* new ref */
  }

  PyErr_SetString(PyExc_TypeError,
                  "camera.pointInsideFrustum(point): KX_Camera, expected point argument.");
  return nullptr;
}

EXP_PYMETHODDEF_DOC_NOARGS(
    KX_Camera,
    getCameraToWorld,
    "getCameraToWorld() -> Matrix4x4\n"
    "\treturns the camera to world transformation matrix, as a list of four "
    "lists of four values.\n\n"
    "\tie: [[1.0, 0.0, 0.0, 0.0], [0.0, 1.0, 0.0, 0.0], [0.0, 0.0, 1.0, "
    "0.0], [0.0, 0.0, 0.0, 1.0]])\n")
{
  return PyObjectFrom(MT_Matrix4x4(GetCameraToWorld())); /* new ref */
}

EXP_PYMETHODDEF_DOC_NOARGS(
    KX_Camera,
    getWorldToCamera,
    "getWorldToCamera() -> Matrix4x4\n"
    "\treturns the world to camera transformation matrix, as a list of four "
    "lists of four values.\n\n"
    "\tie: [[1.0, 0.0, 0.0, 0.0], [0.0, 1.0, 0.0, 0.0], [0.0, 0.0, 1.0, "
    "0.0], [0.0, 0.0, 0.0, 1.0]])\n")
{
  return PyObjectFrom(MT_Matrix4x4(GetWorldToCamera())); /* new ref */
}

EXP_PYMETHODDEF_DOC_VARARGS(KX_Camera,
                            setViewport,
                            "setViewport(left, bottom, right, top)\n"
                            "Sets this camera's viewport\n")
{
  int left, bottom, right, top;
  if (!PyArg_ParseTuple(args, "iiii:setViewport", &left, &bottom, &right, &top))
    return nullptr;

  SetViewport(left, bottom, right, top);
  Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC_NOARGS(KX_Camera,
                           setOnTop,
                           "setOnTop()\n"
                           "Sets this camera's viewport on top\n")
{
  GetScene()->SetCameraOnTop(this);
  Py_RETURN_NONE;
}

PyObject *KX_Camera::pyattr_get_perspective(EXP_PyObjectPlus *self_v,
                                            const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_Camera *self = static_cast<KX_Camera *>(self_v);
  return PyBool_FromLong(self->m_camdata.m_perspective);
}

int KX_Camera::pyattr_set_perspective(EXP_PyObjectPlus *self_v,
                                      const EXP_PYATTRIBUTE_DEF *attrdef,
                                      PyObject *value)
{
  KX_Camera *self = static_cast<KX_Camera *>(self_v);
  int param = PyObject_IsTrue(value);
  if (param == -1) {
    PyErr_SetString(PyExc_AttributeError,
                    "camera.perspective = bool: KX_Camera, expected True/False or 0/1");
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

int KX_Camera::pyattr_set_lens(EXP_PyObjectPlus *self_v,
                               const EXP_PYATTRIBUTE_DEF *attrdef,
                               PyObject *value)
{
  KX_Camera *self = static_cast<KX_Camera *>(self_v);
  float param = PyFloat_AsDouble(value);
  if (param == -1) {
    PyErr_SetString(PyExc_AttributeError,
                    "camera.lens = float: KX_Camera, expected a float greater than zero");
    return PY_SET_ATTR_FAIL;
  }

  self->m_camdata.m_lens = param;
  self->m_set_projection_matrix = false;
  return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_Camera::pyattr_get_fov(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_Camera *self = static_cast<KX_Camera *>(self_v);

  float lens = self->m_camdata.m_lens;
  float width = self->m_camdata.m_sensor_x;
  float fov = 2.0f * atanf(0.5f * width / lens);

  return PyFloat_FromDouble(fov * MT_DEGS_PER_RAD);
}

int KX_Camera::pyattr_set_fov(EXP_PyObjectPlus *self_v,
                              const EXP_PYATTRIBUTE_DEF *attrdef,
                              PyObject *value)
{
  KX_Camera *self = static_cast<KX_Camera *>(self_v);
  float fov = PyFloat_AsDouble(value);
  if (fov <= 0.0f) {
    PyErr_SetString(PyExc_AttributeError,
                    "camera.fov = float: KX_Camera, expected a float greater than zero");
    return PY_SET_ATTR_FAIL;
  }

  fov *= MT_RADS_PER_DEG;
  float width = self->m_camdata.m_sensor_x;
  float lens = width / (2.0f * tanf(0.5f * fov));

  self->m_camdata.m_lens = lens;
  self->m_set_projection_matrix = false;
  return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_Camera::pyattr_get_ortho_scale(EXP_PyObjectPlus *self_v,
                                            const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_Camera *self = static_cast<KX_Camera *>(self_v);
  return PyFloat_FromDouble(self->m_camdata.m_scale);
}

int KX_Camera::pyattr_set_ortho_scale(EXP_PyObjectPlus *self_v,
                                      const EXP_PYATTRIBUTE_DEF *attrdef,
                                      PyObject *value)
{
  KX_Camera *self = static_cast<KX_Camera *>(self_v);
  float param = PyFloat_AsDouble(value);
  if (param == -1) {
    PyErr_SetString(PyExc_AttributeError,
                    "camera.ortho_scale = float: KX_Camera, expected a float greater than zero");
    return PY_SET_ATTR_FAIL;
  }

  self->m_camdata.m_scale = param;
  self->m_set_projection_matrix = false;
  return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_Camera::pyattr_get_near(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_Camera *self = static_cast<KX_Camera *>(self_v);
  return PyFloat_FromDouble(self->m_camdata.m_clipstart);
}

int KX_Camera::pyattr_set_near(EXP_PyObjectPlus *self_v,
                               const EXP_PYATTRIBUTE_DEF *attrdef,
                               PyObject *value)
{
  KX_Camera *self = static_cast<KX_Camera *>(self_v);
  float param = PyFloat_AsDouble(value);
  if (param == -1) {
    PyErr_SetString(PyExc_AttributeError,
                    "camera.near = float: KX_Camera, expected a float greater than zero");
    return PY_SET_ATTR_FAIL;
  }

  self->m_camdata.m_clipstart = param;
  self->m_set_projection_matrix = false;
  return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_Camera::pyattr_get_far(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_Camera *self = static_cast<KX_Camera *>(self_v);
  return PyFloat_FromDouble(self->m_camdata.m_clipend);
}

int KX_Camera::pyattr_set_far(EXP_PyObjectPlus *self_v,
                              const EXP_PYATTRIBUTE_DEF *attrdef,
                              PyObject *value)
{
  KX_Camera *self = static_cast<KX_Camera *>(self_v);
  float param = PyFloat_AsDouble(value);
  if (param == -1) {
    PyErr_SetString(PyExc_AttributeError,
                    "camera.far = float: KX_Camera, expected a float greater than zero");
    return PY_SET_ATTR_FAIL;
  }

  self->m_camdata.m_clipend = param;
  self->m_set_projection_matrix = false;
  return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_Camera::pyattr_get_shift_x(EXP_PyObjectPlus *self_v,
                                        const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_Camera *self = static_cast<KX_Camera *>(self_v);
  return PyFloat_FromDouble(self->m_camdata.m_shift_x);
}

int KX_Camera::pyattr_set_shift_x(EXP_PyObjectPlus *self_v,
                                  const EXP_PYATTRIBUTE_DEF *attrdef,
                                  PyObject *value)
{
  KX_Camera *self = static_cast<KX_Camera *>(self_v);
  float param = PyFloat_AsDouble(value);
  if (param == -1) {
    PyErr_SetString(PyExc_AttributeError,
                    "camera.shift_x = float: KX_Camera, expected a float greater than zero");
    return PY_SET_ATTR_FAIL;
  }

  self->m_camdata.m_shift_x = param;
  self->m_set_projection_matrix = false;
  return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_Camera::pyattr_get_shift_y(EXP_PyObjectPlus *self_v,
                                        const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_Camera *self = static_cast<KX_Camera *>(self_v);
  return PyFloat_FromDouble(self->m_camdata.m_shift_y);
}

int KX_Camera::pyattr_set_shift_y(EXP_PyObjectPlus *self_v,
                                  const EXP_PYATTRIBUTE_DEF *attrdef,
                                  PyObject *value)
{
  KX_Camera *self = static_cast<KX_Camera *>(self_v);
  float param = PyFloat_AsDouble(value);
  if (param == -1) {
    PyErr_SetString(PyExc_AttributeError,
                    "camera.shift_y = float: KX_Camera, expected a float greater than zero");
    return PY_SET_ATTR_FAIL;
  }

  self->m_camdata.m_shift_y = param;
  self->m_set_projection_matrix = false;
  return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_Camera::pyattr_get_use_viewport(EXP_PyObjectPlus *self_v,
                                             const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_Camera *self = static_cast<KX_Camera *>(self_v);
  return PyBool_FromLong(self->GetViewport());
}

int KX_Camera::pyattr_set_use_viewport(EXP_PyObjectPlus *self_v,
                                       const EXP_PYATTRIBUTE_DEF *attrdef,
                                       PyObject *value)
{
  KX_Camera *self = static_cast<KX_Camera *>(self_v);
  int param = PyObject_IsTrue(value);
  if (param == -1) {
    PyErr_SetString(PyExc_AttributeError,
                    "camera.useViewport = bool: KX_Camera, expected True or False");
    return PY_SET_ATTR_FAIL;
  }
  self->EnableViewport((bool)param);
  return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_Camera::pyattr_get_projection_matrix(EXP_PyObjectPlus *self_v,
                                                  const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_Camera *self = static_cast<KX_Camera *>(self_v);
  return PyObjectFrom(self->GetProjectionMatrix());
}

int KX_Camera::pyattr_set_projection_matrix(EXP_PyObjectPlus *self_v,
                                            const EXP_PYATTRIBUTE_DEF *attrdef,
                                            PyObject *value)
{
  KX_Camera *self = static_cast<KX_Camera *>(self_v);
  MT_Matrix4x4 mat;
  if (!PyMatTo(value, mat))
    return PY_SET_ATTR_FAIL;

  self->SetProjectionMatrix(mat);
  return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_Camera::pyattr_get_modelview_matrix(EXP_PyObjectPlus *self_v,
                                                 const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_Camera *self = static_cast<KX_Camera *>(self_v);
  return PyObjectFrom(MT_Matrix4x4(self->GetWorldToCamera()));
}

PyObject *KX_Camera::pyattr_get_camera_to_world(EXP_PyObjectPlus *self_v,
                                                const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_Camera *self = static_cast<KX_Camera *>(self_v);
  return PyObjectFrom(MT_Matrix4x4(self->GetCameraToWorld()));
}

PyObject *KX_Camera::pyattr_get_world_to_camera(EXP_PyObjectPlus *self_v,
                                                const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_Camera *self = static_cast<KX_Camera *>(self_v);
  return PyObjectFrom(MT_Matrix4x4(self->GetWorldToCamera()));
}

PyObject *KX_Camera::pyattr_get_INSIDE(EXP_PyObjectPlus *self_v,
                                       const EXP_PYATTRIBUTE_DEF *attrdef)
{
  return PyLong_FromLong(INSIDE);
}
PyObject *KX_Camera::pyattr_get_OUTSIDE(EXP_PyObjectPlus *self_v,
                                        const EXP_PYATTRIBUTE_DEF *attrdef)
{
  return PyLong_FromLong(OUTSIDE);
}
PyObject *KX_Camera::pyattr_get_INTERSECT(EXP_PyObjectPlus *self_v,
                                          const EXP_PYATTRIBUTE_DEF *attrdef)
{
  return PyLong_FromLong(INTERSECT);
}

bool ConvertPythonToCamera(KX_Scene *scene,
                           PyObject *value,
                           KX_Camera **object,
                           bool py_none_ok,
                           const char *error_prefix)
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
      PyErr_Format(PyExc_TypeError,
                   "%s, expected KX_Camera or a KX_Camera name, None is invalid",
                   error_prefix);
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
                   error_prefix,
                   _PyUnicode_AsString(value));
      return false;
    }
  }

  if (PyObject_TypeCheck(value, &KX_Camera::Type)) {
    *object = static_cast<KX_Camera *> EXP_PROXY_REF(value);

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

EXP_PYMETHODDEF_DOC_O(KX_Camera, getScreenPosition, "getScreenPosition()\n")

{
  MT_Vector3 vect;
  KX_GameObject *obj = nullptr;

  if (!PyVecTo(value, vect)) {
    PyErr_Clear();

    if (ConvertPythonToGameObject(GetScene()->GetLogicManager(), value, &obj, false, "")) {
      PyErr_Clear();
      vect = MT_Vector3(obj->NodeGetWorldPosition());
    }
    else {
      PyErr_SetString(PyExc_TypeError,
                      "Error in getScreenPosition. Expected a Vector3 or a KX_GameObject or a "
                      "string for a name of a KX_GameObject");
      return nullptr;
    }
  }

  GLint viewport[4];
  GLfloat vec[3];
  GLfloat win[3];
  GLfloat modelmatrix[4][4];
  GLfloat projmatrix[4][4];

  MT_Matrix4x4 m_modelmatrix = MT_Matrix4x4(GetWorldToCamera());
  MT_Matrix4x4 m_projmatrix = this->GetProjectionMatrix();

  vect.getValue(vec);
  m_modelmatrix.getValue((float *)modelmatrix);
  m_projmatrix.getValue((float *)projmatrix);

  KX_GetActiveEngine()->GetCanvas()->GetViewportArea().Pack(viewport);

  GPU_matrix_project_3fv(vec, modelmatrix, projmatrix, viewport, win);

  vect[0] = (win[0] - viewport[0]) / viewport[2];
  vect[1] = (win[1] - viewport[1]) / viewport[3];

  vect[1] = 1.0f - vect[1];  // to follow Blender window coordinate system (Top-Down)

  /* Check if the object is behind the camera */
  /* To avoid having screenpos "twice", one in front of cam, the other
   * behind cam. Fix from Raphael */
  bool behind_cam = win[2] > 1.0;

  PyObject *ret = PyTuple_New(2);
  if (ret) {
    PyTuple_SET_ITEM(ret, 0, PyFloat_FromDouble(behind_cam ? FLT_MAX : vect[0]));
    PyTuple_SET_ITEM(ret, 1, PyFloat_FromDouble(behind_cam ? FLT_MAX : vect[1]));
    return ret;
  }

  return nullptr;
}

EXP_PYMETHODDEF_DOC_VARARGS(KX_Camera, getScreenVect, "getScreenVect()\n")
{
  float x, y;
  if (!PyArg_ParseTuple(args, "ff:getScreenVect", &x, &y))
    return nullptr;

  y = 1.0 - y;  // to follow Blender window coordinate system (Top-Down)

  GLint viewport[4];
  GLfloat vec[3];
  GLfloat win[3];
  GLfloat modelmatrixinv[4][4];
  GLfloat projmatrix[4][4];

  MT_Matrix4x4 m_modelmatrix = MT_Matrix4x4(GetWorldToCamera());
  MT_Matrix4x4 m_projmatrix = this->GetProjectionMatrix();

  m_modelmatrix.inverse().getValue((float *)modelmatrixinv);
  m_projmatrix.getValue((float *)projmatrix);

  KX_GetActiveEngine()->GetCanvas()->GetViewportArea().Pack(viewport);

  vec[0] = x * viewport[2];
  vec[1] = y * viewport[3];

  vec[0] += viewport[0];
  vec[1] += viewport[1];

  vec[2] = 0.f;

  GPU_matrix_unproject_3fv(vec, modelmatrixinv, projmatrix, viewport, win);

  MT_Vector3 campos = NodeGetWorldPosition();
  MT_Vector3 screenpos(win[0], win[1], win[2]);
  MT_Vector3 vect = campos - screenpos;
  vect.normalize();
  return PyObjectFrom(vect);
}

EXP_PYMETHODDEF_DOC_VARARGS(KX_Camera, getScreenRay, "getScreenRay()\n")
{
  MT_Vector3 vect;
  float x, y, dist;
  char *propName = nullptr;

  if (!PyArg_ParseTuple(args, "fff|s:getScreenRay", &x, &y, &dist, &propName))
    return nullptr;

  y = 1.0 - y;  // to follow Blender window coordinate system (Top-Down)

  GLint viewport[4];
  GLfloat modelmatrixinv[4][4];
  GLfloat projmatrix[4][4];

  MT_Matrix4x4 m_modelmatrix = MT_Matrix4x4(GetWorldToCamera());
  MT_Matrix4x4 m_projmatrix = this->GetProjectionMatrix();

  m_modelmatrix.inverse().getValue((float *)modelmatrixinv);
  m_projmatrix.getValue((float *)projmatrix);

  KX_GetActiveEngine()->GetCanvas()->GetViewportArea().Pack(viewport);

  MT_Vector3 fromPoint;
  MT_Vector3 toPoint;

  // Unproject a point in near plane.
  MT_Vector3 point;

  point[0] = x * viewport[2];
  point[1] = y * viewport[3];

  point[0] += viewport[0];
  point[1] += viewport[1];

  point[2] = 0.f;

  float screenpos[3];
  GPU_matrix_unproject_3fv(point.getValue(), modelmatrixinv, projmatrix, viewport, screenpos);

  // For perpspective the vector is from camera center to unprojected point.
  if (m_camdata.m_perspective) {
    fromPoint = NodeGetWorldPosition();
    toPoint = MT_Vector3(screenpos);
  }
  // For orthographic the vector is the same as the -Z rotation axis but start from unprojected
  // point.
  else {
    fromPoint = MT_Vector3(screenpos);
    toPoint = fromPoint - NodeGetWorldOrientation().getColumn(2);
  }

  if (dist != 0.0f) {
    toPoint = fromPoint + (toPoint - fromPoint).safe_normalized() * dist;
  }

  PHY_IPhysicsEnvironment *pe = GetScene()->GetPhysicsEnvironment();
  PHY_IPhysicsController *spc = m_pPhysicsController;
  KX_GameObject *parent = GetParent();
  if (!spc && parent) {
    spc = parent->GetPhysicsController();
  }

  std::string prop = propName ? (std::string)propName : "";
  RayCastData rayData(prop, false, (1u << OB_MAX_COL_MASKS) - 1);
  KX_RayCast::Callback<KX_Camera, RayCastData> callback(this, spc, &rayData);
  if (KX_RayCast::RayTest(pe, fromPoint, toPoint, callback) && rayData.m_hitObject) {
    return rayData.m_hitObject->GetProxy();
  }

  Py_RETURN_NONE;
}
#endif
