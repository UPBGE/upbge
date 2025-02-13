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

/** \file gameengine/Ketsji/KX_VertexProxy.cpp
 *  \ingroup ketsji
 */

#ifdef WITH_PYTHON

#  include "KX_VertexProxy.h"

#include <fmt/format.h>

#  include "EXP_ListWrapper.h"
#  include "KX_MeshProxy.h"
#  include "KX_PyMath.h"
#  include "RAS_IDisplayArray.h"
#  include "RAS_IVertex.h"

PyTypeObject KX_VertexProxy::Type = {PyVarObject_HEAD_INIT(nullptr, 0) "KX_VertexProxy",
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
                                     &EXP_Value::Type,
                                     0,
                                     0,
                                     0,
                                     0,
                                     0,
                                     0,
                                     py_base_new};

PyMethodDef KX_VertexProxy::Methods[] = {
    {"getXYZ", (PyCFunction)KX_VertexProxy::sPyGetXYZ, METH_NOARGS},
    {"setXYZ", (PyCFunction)KX_VertexProxy::sPySetXYZ, METH_O},
    {"getUV", (PyCFunction)KX_VertexProxy::sPyGetUV1, METH_NOARGS},
    {"setUV", (PyCFunction)KX_VertexProxy::sPySetUV1, METH_O},

    {"getUV2", (PyCFunction)KX_VertexProxy::sPyGetUV2, METH_NOARGS},
    {"setUV2", (PyCFunction)KX_VertexProxy::sPySetUV2, METH_VARARGS},

    {"getRGBA", (PyCFunction)KX_VertexProxy::sPyGetRGBA, METH_NOARGS},
    {"setRGBA", (PyCFunction)KX_VertexProxy::sPySetRGBA, METH_O},
    {"getNormal", (PyCFunction)KX_VertexProxy::sPyGetNormal, METH_NOARGS},
    {"setNormal", (PyCFunction)KX_VertexProxy::sPySetNormal, METH_O},
    {nullptr, nullptr}  // Sentinel
};

PyAttributeDef KX_VertexProxy::Attributes[] = {
    EXP_PYATTRIBUTE_RW_FUNCTION("x", KX_VertexProxy, pyattr_get_x, pyattr_set_x),
    EXP_PYATTRIBUTE_RW_FUNCTION("y", KX_VertexProxy, pyattr_get_y, pyattr_set_y),
    EXP_PYATTRIBUTE_RW_FUNCTION("z", KX_VertexProxy, pyattr_get_z, pyattr_set_z),

    EXP_PYATTRIBUTE_RW_FUNCTION("r", KX_VertexProxy, pyattr_get_r, pyattr_set_r),
    EXP_PYATTRIBUTE_RW_FUNCTION("g", KX_VertexProxy, pyattr_get_g, pyattr_set_g),
    EXP_PYATTRIBUTE_RW_FUNCTION("b", KX_VertexProxy, pyattr_get_b, pyattr_set_b),
    EXP_PYATTRIBUTE_RW_FUNCTION("a", KX_VertexProxy, pyattr_get_a, pyattr_set_a),

    EXP_PYATTRIBUTE_RW_FUNCTION("u", KX_VertexProxy, pyattr_get_u, pyattr_set_u),
    EXP_PYATTRIBUTE_RW_FUNCTION("v", KX_VertexProxy, pyattr_get_v, pyattr_set_v),

    EXP_PYATTRIBUTE_RW_FUNCTION("u2", KX_VertexProxy, pyattr_get_u2, pyattr_set_u2),
    EXP_PYATTRIBUTE_RW_FUNCTION("v2", KX_VertexProxy, pyattr_get_v2, pyattr_set_v2),

    EXP_PYATTRIBUTE_RW_FUNCTION("XYZ", KX_VertexProxy, pyattr_get_XYZ, pyattr_set_XYZ),
    EXP_PYATTRIBUTE_RW_FUNCTION("UV", KX_VertexProxy, pyattr_get_UV, pyattr_set_UV),
    EXP_PYATTRIBUTE_RW_FUNCTION("uvs", KX_VertexProxy, pyattr_get_uvs, pyattr_set_uvs),

    EXP_PYATTRIBUTE_RW_FUNCTION("color", KX_VertexProxy, pyattr_get_color, pyattr_set_color),
    EXP_PYATTRIBUTE_RW_FUNCTION("colors", KX_VertexProxy, pyattr_get_colors, pyattr_set_colors),
    EXP_PYATTRIBUTE_RW_FUNCTION("normal", KX_VertexProxy, pyattr_get_normal, pyattr_set_normal),

    EXP_PYATTRIBUTE_NULL  // Sentinel
};

PyObject *KX_VertexProxy::pyattr_get_x(EXP_PyObjectPlus *self_v,
                                       const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
  return PyFloat_FromDouble(self->m_vertex->getXYZ()[0]);
}

PyObject *KX_VertexProxy::pyattr_get_y(EXP_PyObjectPlus *self_v,
                                       const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
  return PyFloat_FromDouble(self->m_vertex->getXYZ()[1]);
}

PyObject *KX_VertexProxy::pyattr_get_z(EXP_PyObjectPlus *self_v,
                                       const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
  return PyFloat_FromDouble(self->m_vertex->getXYZ()[2]);
}

PyObject *KX_VertexProxy::pyattr_get_r(EXP_PyObjectPlus *self_v,
                                       const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
  return PyFloat_FromDouble(self->m_vertex->getRGBA(0)[0] / 255.0);
}

PyObject *KX_VertexProxy::pyattr_get_g(EXP_PyObjectPlus *self_v,
                                       const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
  return PyFloat_FromDouble(self->m_vertex->getRGBA(0)[1] / 255.0);
}

PyObject *KX_VertexProxy::pyattr_get_b(EXP_PyObjectPlus *self_v,
                                       const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
  return PyFloat_FromDouble(self->m_vertex->getRGBA(0)[2] / 255.0);
}

PyObject *KX_VertexProxy::pyattr_get_a(EXP_PyObjectPlus *self_v,
                                       const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
  return PyFloat_FromDouble(self->m_vertex->getRGBA(0)[3] / 255.0);
}

PyObject *KX_VertexProxy::pyattr_get_u(EXP_PyObjectPlus *self_v,
                                       const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
  return PyFloat_FromDouble(self->m_vertex->getUV(0)[0]);
}

PyObject *KX_VertexProxy::pyattr_get_v(EXP_PyObjectPlus *self_v,
                                       const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
  return PyFloat_FromDouble(self->m_vertex->getUV(0)[1]);
}

PyObject *KX_VertexProxy::pyattr_get_u2(EXP_PyObjectPlus *self_v,
                                        const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
  return (self->m_vertex->getUvSize() > 1) ? PyFloat_FromDouble(self->m_vertex->getUV(1)[0]) :
                                             PyFloat_FromDouble(0.0f);
}

PyObject *KX_VertexProxy::pyattr_get_v2(EXP_PyObjectPlus *self_v,
                                        const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
  return (self->m_vertex->getUvSize() > 1) ? PyFloat_FromDouble(self->m_vertex->getUV(1)[1]) :
                                             PyFloat_FromDouble(0.0f);
}

PyObject *KX_VertexProxy::pyattr_get_XYZ(EXP_PyObjectPlus *self_v,
                                         const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
  return PyObjectFrom(MT_Vector3(self->m_vertex->getXYZ()));
}

PyObject *KX_VertexProxy::pyattr_get_UV(EXP_PyObjectPlus *self_v,
                                        const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
  return PyObjectFrom(MT_Vector2(self->m_vertex->getUV(0)));
}

static int kx_vertex_proxy_get_uvs_size_cb(void *self_v)
{
  return ((KX_VertexProxy *)self_v)->GetVertex()->getUvSize();
}

static PyObject *kx_vertex_proxy_get_uvs_item_cb(void *self_v, int index)
{
  MT_Vector2 uv = MT_Vector2(((KX_VertexProxy *)self_v)->GetVertex()->getUV(index));
  return PyObjectFrom(uv);
}

static bool kx_vertex_proxy_set_uvs_item_cb(void *self_v, int index, PyObject *item)
{
  MT_Vector2 uv;
  if (!PyVecTo(item, uv)) {
    return false;
  }

  KX_VertexProxy *self = ((KX_VertexProxy *)self_v);
  self->GetVertex()->SetUV(index, uv);
  self->GetDisplayArray()->AppendModifiedFlag(RAS_IDisplayArray::UVS_MODIFIED);

  return true;
}

PyObject *KX_VertexProxy::pyattr_get_uvs(EXP_PyObjectPlus *self_v,
                                         const EXP_PYATTRIBUTE_DEF *attrdef)
{
  return (new EXP_ListWrapper(self_v,
                              ((KX_VertexProxy *)self_v)->GetProxy(),
                              nullptr,
                              kx_vertex_proxy_get_uvs_size_cb,
                              kx_vertex_proxy_get_uvs_item_cb,
                              nullptr,
                              kx_vertex_proxy_set_uvs_item_cb))
      ->NewProxy(true);
}

static int kx_vertex_proxy_get_colors_size_cb(void *self_v)
{
  return ((KX_VertexProxy *)self_v)->GetVertex()->getColorSize();
}

static PyObject *kx_vertex_proxy_get_colors_item_cb(void *self_v, int index)
{
  MT_Vector4 color = MT_Vector4(((KX_VertexProxy *)self_v)->GetVertex()->getRGBA(index));
  color /= 255.0f;
  return PyObjectFrom(color);
}

static bool kx_vertex_proxy_set_colors_item_cb(void *self_v, int index, PyObject *item)
{
  MT_Vector4 color;
  if (!PyVecTo(item, color)) {
    return false;
  }

  KX_VertexProxy *self = ((KX_VertexProxy *)self_v);
  self->GetVertex()->SetRGBA(index, color);
  self->GetDisplayArray()->AppendModifiedFlag(RAS_IDisplayArray::COLORS_MODIFIED);

  return true;
}

PyObject *KX_VertexProxy::pyattr_get_colors(EXP_PyObjectPlus *self_v,
                                            const EXP_PYATTRIBUTE_DEF *attrdef)
{
  return (new EXP_ListWrapper(self_v,
                              ((KX_VertexProxy *)self_v)->GetProxy(),
                              nullptr,
                              kx_vertex_proxy_get_colors_size_cb,
                              kx_vertex_proxy_get_colors_item_cb,
                              nullptr,
                              kx_vertex_proxy_set_colors_item_cb))
      ->NewProxy(true);
}

PyObject *KX_VertexProxy::pyattr_get_color(EXP_PyObjectPlus *self_v,
                                           const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
  const unsigned char *colp = self->m_vertex->getRGBA(0);
  MT_Vector4 color(colp);
  color /= 255.0f;
  return PyObjectFrom(color);
}

PyObject *KX_VertexProxy::pyattr_get_normal(EXP_PyObjectPlus *self_v,
                                            const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
  return PyObjectFrom(MT_Vector3(self->m_vertex->getNormal()));
}

int KX_VertexProxy::pyattr_set_x(EXP_PyObjectPlus *self_v,
                                 const struct EXP_PYATTRIBUTE_DEF *attrdef,
                                 PyObject *value)
{
  KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
  if (PyFloat_Check(value)) {
    float val = PyFloat_AsDouble(value);
    MT_Vector3 pos(self->m_vertex->getXYZ());
    pos.x() = val;
    self->m_vertex->SetXYZ(pos);
    self->m_array->AppendModifiedFlag(RAS_IDisplayArray::POSITION_MODIFIED);
    return PY_SET_ATTR_SUCCESS;
  }
  return PY_SET_ATTR_FAIL;
}

int KX_VertexProxy::pyattr_set_y(EXP_PyObjectPlus *self_v,
                                 const struct EXP_PYATTRIBUTE_DEF *attrdef,
                                 PyObject *value)
{
  KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
  if (PyFloat_Check(value)) {
    float val = PyFloat_AsDouble(value);
    MT_Vector3 pos(self->m_vertex->getXYZ());
    pos.y() = val;
    self->m_vertex->SetXYZ(pos);
    self->m_array->AppendModifiedFlag(RAS_IDisplayArray::POSITION_MODIFIED);
    return PY_SET_ATTR_SUCCESS;
  }
  return PY_SET_ATTR_FAIL;
}

int KX_VertexProxy::pyattr_set_z(EXP_PyObjectPlus *self_v,
                                 const struct EXP_PYATTRIBUTE_DEF *attrdef,
                                 PyObject *value)
{
  KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
  if (PyFloat_Check(value)) {
    float val = PyFloat_AsDouble(value);
    MT_Vector3 pos(self->m_vertex->getXYZ());
    pos.z() = val;
    self->m_vertex->SetXYZ(pos);
    self->m_array->AppendModifiedFlag(RAS_IDisplayArray::POSITION_MODIFIED);
    return PY_SET_ATTR_SUCCESS;
  }
  return PY_SET_ATTR_FAIL;
}

int KX_VertexProxy::pyattr_set_u(EXP_PyObjectPlus *self_v,
                                 const struct EXP_PYATTRIBUTE_DEF *attrdef,
                                 PyObject *value)
{
  KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
  if (PyFloat_Check(value)) {
    float val = PyFloat_AsDouble(value);
    MT_Vector2 uv = MT_Vector2(self->m_vertex->getUV(0));
    uv[0] = val;
    self->m_vertex->SetUV(0, uv);
    self->m_array->AppendModifiedFlag(RAS_IDisplayArray::UVS_MODIFIED);
    return PY_SET_ATTR_SUCCESS;
  }
  return PY_SET_ATTR_FAIL;
}

int KX_VertexProxy::pyattr_set_v(EXP_PyObjectPlus *self_v,
                                 const struct EXP_PYATTRIBUTE_DEF *attrdef,
                                 PyObject *value)
{
  KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
  if (PyFloat_Check(value)) {
    float val = PyFloat_AsDouble(value);
    MT_Vector2 uv = MT_Vector2(self->m_vertex->getUV(0));
    uv[1] = val;
    self->m_vertex->SetUV(0, uv);
    self->m_array->AppendModifiedFlag(RAS_IDisplayArray::UVS_MODIFIED);
    return PY_SET_ATTR_SUCCESS;
  }
  return PY_SET_ATTR_FAIL;
}

int KX_VertexProxy::pyattr_set_u2(EXP_PyObjectPlus *self_v,
                                  const struct EXP_PYATTRIBUTE_DEF *attrdef,
                                  PyObject *value)
{
  KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
  if (PyFloat_Check(value)) {
    if (self->GetVertex()->getUvSize() > 1) {
      float val = PyFloat_AsDouble(value);
      MT_Vector2 uv = MT_Vector2(self->m_vertex->getUV(1));
      uv[0] = val;
      self->m_vertex->SetUV(1, uv);
      self->m_array->AppendModifiedFlag(RAS_IDisplayArray::UVS_MODIFIED);
    }
    return PY_SET_ATTR_SUCCESS;
  }
  return PY_SET_ATTR_FAIL;
}

int KX_VertexProxy::pyattr_set_v2(EXP_PyObjectPlus *self_v,
                                  const struct EXP_PYATTRIBUTE_DEF *attrdef,
                                  PyObject *value)
{
  KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
  if (PyFloat_Check(value)) {
    if (self->GetVertex()->getUvSize() > 1) {
      float val = PyFloat_AsDouble(value);
      MT_Vector2 uv = MT_Vector2(self->m_vertex->getUV(1));
      uv[1] = val;
      self->m_vertex->SetUV(1, uv);
      self->m_array->AppendModifiedFlag(RAS_IDisplayArray::UVS_MODIFIED);
    }
    return PY_SET_ATTR_SUCCESS;
  }
  return PY_SET_ATTR_FAIL;
}

int KX_VertexProxy::pyattr_set_r(EXP_PyObjectPlus *self_v,
                                 const struct EXP_PYATTRIBUTE_DEF *attrdef,
                                 PyObject *value)
{
  KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
  if (PyFloat_Check(value)) {
    float val = PyFloat_AsDouble(value);
    unsigned int icol = self->m_vertex->getRawRGBA(0);
    unsigned char *cp = (unsigned char *)&icol;
    val *= 255.0f;
    cp[0] = (unsigned char)val;
    self->m_vertex->SetRGBA(0, icol);
    self->m_array->AppendModifiedFlag(RAS_IDisplayArray::COLORS_MODIFIED);
    return PY_SET_ATTR_SUCCESS;
  }
  return PY_SET_ATTR_FAIL;
}

int KX_VertexProxy::pyattr_set_g(EXP_PyObjectPlus *self_v,
                                 const struct EXP_PYATTRIBUTE_DEF *attrdef,
                                 PyObject *value)
{
  KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
  if (PyFloat_Check(value)) {
    float val = PyFloat_AsDouble(value);
    unsigned int icol = self->m_vertex->getRawRGBA(0);
    unsigned char *cp = (unsigned char *)&icol;
    val *= 255.0f;
    cp[1] = (unsigned char)val;
    self->m_vertex->SetRGBA(0, icol);
    self->m_array->AppendModifiedFlag(RAS_IDisplayArray::COLORS_MODIFIED);
    return PY_SET_ATTR_SUCCESS;
  }
  return PY_SET_ATTR_FAIL;
}

int KX_VertexProxy::pyattr_set_b(EXP_PyObjectPlus *self_v,
                                 const struct EXP_PYATTRIBUTE_DEF *attrdef,
                                 PyObject *value)
{
  KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
  if (PyFloat_Check(value)) {
    float val = PyFloat_AsDouble(value);
    unsigned int icol = self->m_vertex->getRawRGBA(0);
    unsigned char *cp = (unsigned char *)&icol;
    val *= 255.0f;
    cp[2] = (unsigned char)val;
    self->m_vertex->SetRGBA(0, icol);
    self->m_array->AppendModifiedFlag(RAS_IDisplayArray::COLORS_MODIFIED);
    return PY_SET_ATTR_SUCCESS;
  }
  return PY_SET_ATTR_FAIL;
}

int KX_VertexProxy::pyattr_set_a(EXP_PyObjectPlus *self_v,
                                 const struct EXP_PYATTRIBUTE_DEF *attrdef,
                                 PyObject *value)
{
  KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
  if (PyFloat_Check(value)) {
    float val = PyFloat_AsDouble(value);
    unsigned int icol = self->m_vertex->getRawRGBA(0);
    unsigned char *cp = (unsigned char *)&icol;
    val *= 255.0f;
    cp[3] = (unsigned char)val;
    self->m_vertex->SetRGBA(0, icol);
    self->m_array->AppendModifiedFlag(RAS_IDisplayArray::COLORS_MODIFIED);
    return PY_SET_ATTR_SUCCESS;
  }
  return PY_SET_ATTR_FAIL;
}

int KX_VertexProxy::pyattr_set_XYZ(EXP_PyObjectPlus *self_v,
                                   const struct EXP_PYATTRIBUTE_DEF *attrdef,
                                   PyObject *value)
{
  KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
  if (PySequence_Check(value)) {
    MT_Vector3 vec;
    if (PyVecTo(value, vec)) {
      self->m_vertex->SetXYZ(vec);
      self->m_array->AppendModifiedFlag(RAS_IDisplayArray::POSITION_MODIFIED);
      return PY_SET_ATTR_SUCCESS;
    }
  }
  return PY_SET_ATTR_FAIL;
}

int KX_VertexProxy::pyattr_set_UV(EXP_PyObjectPlus *self_v,
                                  const struct EXP_PYATTRIBUTE_DEF *attrdef,
                                  PyObject *value)
{
  KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
  if (PySequence_Check(value)) {
    MT_Vector2 vec;
    if (PyVecTo(value, vec)) {
      self->m_vertex->SetUV(0, vec);
      self->m_array->AppendModifiedFlag(RAS_IDisplayArray::UVS_MODIFIED);
      return PY_SET_ATTR_SUCCESS;
    }
  }
  return PY_SET_ATTR_FAIL;
}

int KX_VertexProxy::pyattr_set_uvs(EXP_PyObjectPlus *self_v,
                                   const struct EXP_PYATTRIBUTE_DEF *attrdef,
                                   PyObject *value)
{
  KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
  if (PySequence_Check(value)) {
    MT_Vector2 vec;
    for (int i = 0; i < PySequence_Size(value) && i < self->GetVertex()->getUvSize(); ++i) {
      if (PyVecTo(PySequence_GetItem(value, i), vec)) {
        self->m_vertex->SetUV(i, vec);
      }
      else {
        PyErr_SetString(PyExc_AttributeError,
                        (fmt::format("list[{}] was not a vector", i).c_str()));
        return PY_SET_ATTR_FAIL;
      }
    }

    self->m_array->AppendModifiedFlag(RAS_IDisplayArray::UVS_MODIFIED);
    return PY_SET_ATTR_SUCCESS;
  }
  return PY_SET_ATTR_FAIL;
}

int KX_VertexProxy::pyattr_set_color(EXP_PyObjectPlus *self_v,
                                     const struct EXP_PYATTRIBUTE_DEF *attrdef,
                                     PyObject *value)
{
  KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
  if (PySequence_Check(value)) {
    MT_Vector4 vec;
    if (PyVecTo(value, vec)) {
      self->m_vertex->SetRGBA(0, vec);
      self->m_array->AppendModifiedFlag(RAS_IDisplayArray::COLORS_MODIFIED);
      return PY_SET_ATTR_SUCCESS;
    }
  }
  return PY_SET_ATTR_FAIL;
}

int KX_VertexProxy::pyattr_set_colors(EXP_PyObjectPlus *self_v,
                                      const struct EXP_PYATTRIBUTE_DEF *attrdef,
                                      PyObject *value)
{
  KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
  if (PySequence_Check(value)) {
    MT_Vector4 vec;
    for (int i = 0; i < PySequence_Size(value) && i < self->GetVertex()->getColorSize(); ++i) {
      if (PyVecTo(PySequence_GetItem(value, i), vec)) {
        self->m_vertex->SetRGBA(i, vec);
      }
      else {
        PyErr_SetString(PyExc_AttributeError,
                        (fmt::format("list[{}] was not a vector", i).c_str()));
        return PY_SET_ATTR_FAIL;
      }
    }

    self->m_array->AppendModifiedFlag(RAS_IDisplayArray::COLORS_MODIFIED);
    return PY_SET_ATTR_SUCCESS;
  }
  return PY_SET_ATTR_FAIL;
}

int KX_VertexProxy::pyattr_set_normal(EXP_PyObjectPlus *self_v,
                                      const struct EXP_PYATTRIBUTE_DEF *attrdef,
                                      PyObject *value)
{
  KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
  if (PySequence_Check(value)) {
    MT_Vector3 vec;
    if (PyVecTo(value, vec)) {
      self->m_vertex->SetNormal(vec);
      self->m_array->AppendModifiedFlag(RAS_IDisplayArray::NORMAL_MODIFIED);
      return PY_SET_ATTR_SUCCESS;
    }
  }
  return PY_SET_ATTR_FAIL;
}

KX_VertexProxy::KX_VertexProxy(RAS_IDisplayArray *array, RAS_IVertex *vertex)
    : m_vertex(vertex), m_array(array)
{
}

KX_VertexProxy::~KX_VertexProxy()
{
}

RAS_IVertex *KX_VertexProxy::GetVertex()
{
  return m_vertex;
}

RAS_IDisplayArray *KX_VertexProxy::GetDisplayArray()
{
  return m_array;
}

// stuff for cvalue related things
std::string KX_VertexProxy::GetName()
{
  return "vertex";
}

// stuff for python integration
PyObject *KX_VertexProxy::PyGetXYZ()
{
  return PyObjectFrom(MT_Vector3(m_vertex->getXYZ()));
}

PyObject *KX_VertexProxy::PySetXYZ(PyObject *value)
{
  MT_Vector3 vec;
  if (!PyVecTo(value, vec))
    return nullptr;

  m_vertex->SetXYZ(vec);
  m_array->AppendModifiedFlag(RAS_IDisplayArray::POSITION_MODIFIED);
  Py_RETURN_NONE;
}

PyObject *KX_VertexProxy::PyGetNormal()
{
  return PyObjectFrom(MT_Vector3(m_vertex->getNormal()));
}

PyObject *KX_VertexProxy::PySetNormal(PyObject *value)
{
  MT_Vector3 vec;
  if (!PyVecTo(value, vec))
    return nullptr;

  m_vertex->SetNormal(vec);
  m_array->AppendModifiedFlag(RAS_IDisplayArray::NORMAL_MODIFIED);
  Py_RETURN_NONE;
}

PyObject *KX_VertexProxy::PyGetRGBA()
{
  const unsigned int rgba = m_vertex->getRawRGBA(0);
  return PyLong_FromLong(rgba);
}

PyObject *KX_VertexProxy::PySetRGBA(PyObject *value)
{
  if (PyLong_Check(value)) {
    int rgba = PyLong_AsLong(value);
    m_vertex->SetRGBA(0, rgba);
    m_array->AppendModifiedFlag(true);
    Py_RETURN_NONE;
  }
  else {
    MT_Vector4 vec;
    if (PyVecTo(value, vec)) {
      m_vertex->SetRGBA(0, vec);
      m_array->AppendModifiedFlag(RAS_IDisplayArray::COLORS_MODIFIED);
      Py_RETURN_NONE;
    }
  }

  PyErr_SetString(PyExc_TypeError,
                  "vert.setRGBA(value): KX_VertexProxy, expected a 4D vector or an int");
  return nullptr;
}

PyObject *KX_VertexProxy::PyGetUV1()
{
  return PyObjectFrom(MT_Vector2(m_vertex->getUV(0)));
}

PyObject *KX_VertexProxy::PySetUV1(PyObject *value)
{
  MT_Vector2 vec;
  if (!PyVecTo(value, vec))
    return nullptr;

  m_vertex->SetUV(0, vec);
  m_array->AppendModifiedFlag(RAS_IDisplayArray::UVS_MODIFIED);
  Py_RETURN_NONE;
}

PyObject *KX_VertexProxy::PyGetUV2()
{
  return (m_vertex->getUvSize() > 1) ? PyObjectFrom(MT_Vector2(m_vertex->getUV(1))) :
                                       PyObjectFrom(MT_Vector2(0.0f, 0.0f));
}

PyObject *KX_VertexProxy::PySetUV2(PyObject *args)
{
  MT_Vector2 vec;
  if (!PyVecTo(args, vec))
    return nullptr;

  if (m_vertex->getUvSize() > 1) {
    m_vertex->SetUV(1, vec);
    m_array->AppendModifiedFlag(RAS_IDisplayArray::UVS_MODIFIED);
  }
  Py_RETURN_NONE;
}

#endif  // WITH_PYTHON
