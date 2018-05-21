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

#include "KX_VertexProxy.h"
#include "KX_Mesh.h"

#include "RAS_DisplayArray.h"

#include "KX_PyMath.h"

#include "EXP_ListWrapper.h"

#include <boost/format.hpp>

PyTypeObject KX_VertexProxy::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"KX_VertexProxy",
	sizeof(EXP_PyObjectPlus_Proxy),
	0,
	py_base_dealloc,
	0,
	0,
	0,
	0,
	py_base_repr,
	0, 0, 0, 0, 0, 0, 0, 0, 0,
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	0, 0, 0, 0, 0, 0, 0,
	Methods,
	0,
	0,
	&EXP_Value::Type,
	0, 0, 0, 0, 0, 0,
	py_base_new
};

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
	{nullptr, nullptr} //Sentinel
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

	EXP_PYATTRIBUTE_NULL //Sentinel
};

PyObject *KX_VertexProxy::pyattr_get_x(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
	return PyFloat_FromDouble(self->m_array->GetPosition(self->m_vertexIndex).x);
}

PyObject *KX_VertexProxy::pyattr_get_y(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
	return PyFloat_FromDouble(self->m_array->GetPosition(self->m_vertexIndex).y);
}

PyObject *KX_VertexProxy::pyattr_get_z(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
	return PyFloat_FromDouble(self->m_array->GetPosition(self->m_vertexIndex).z);
}

PyObject *KX_VertexProxy::pyattr_get_r(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
	return PyFloat_FromDouble(self->m_array->GetColor(self->m_vertexIndex, 0)[0] / 255.0);
}

PyObject *KX_VertexProxy::pyattr_get_g(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
	return PyFloat_FromDouble(self->m_array->GetColor(self->m_vertexIndex, 0)[1] / 255.0);
}

PyObject *KX_VertexProxy::pyattr_get_b(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
	return PyFloat_FromDouble(self->m_array->GetColor(self->m_vertexIndex, 0)[2] / 255.0);
}

PyObject *KX_VertexProxy::pyattr_get_a(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
	return PyFloat_FromDouble(self->m_array->GetColor(self->m_vertexIndex, 0)[3] / 255.0);
}

PyObject *KX_VertexProxy::pyattr_get_u(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
	return PyFloat_FromDouble(self->m_array->GetUv(self->m_vertexIndex, 0).x);
}

PyObject *KX_VertexProxy::pyattr_get_v(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
	return PyFloat_FromDouble(self->m_array->GetUv(self->m_vertexIndex, 0).y);
}

PyObject *KX_VertexProxy::pyattr_get_u2(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
	return (self->m_array->GetFormat().uvSize > 1) ? PyFloat_FromDouble(self->m_array->GetUv(self->m_vertexIndex, 1).x) : PyFloat_FromDouble(0.0f);
}

PyObject *KX_VertexProxy::pyattr_get_v2(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
	return (self->m_array->GetFormat().uvSize > 1) ? PyFloat_FromDouble(self->m_array->GetUv(self->m_vertexIndex, 1).y) : PyFloat_FromDouble(0.0f);
}

PyObject *KX_VertexProxy::pyattr_get_XYZ(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
	return PyObjectFrom(self->m_array->GetPosition(self->m_vertexIndex));
}

PyObject *KX_VertexProxy::pyattr_get_UV(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
	return PyObjectFrom(self->m_array->GetUv(self->m_vertexIndex, 0));
}

unsigned int KX_VertexProxy::py_get_uvs_size()
{
	return m_array->GetFormat().uvSize;
}

PyObject *KX_VertexProxy::py_get_uvs_item(unsigned int index)
{
	return PyObjectFrom(m_array->GetUv(m_vertexIndex, index));
}

bool KX_VertexProxy::py_set_uvs_item(unsigned int index, PyObject *item)
{
	mt::vec2_packed uv;
	if (!PyVecTo(item, uv)) {
		return false;
	}

	m_array->SetUv(m_vertexIndex, index, uv);
	m_array->NotifyUpdate(RAS_DisplayArray::UVS_MODIFIED);

	return true;
}

PyObject *KX_VertexProxy::pyattr_get_uvs(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	return (new EXP_ListWrapper<KX_VertexProxy, &KX_VertexProxy::py_get_uvs_size, &KX_VertexProxy::py_get_uvs_item,
				&KX_VertexProxy::py_set_uvs_item>(self_v))->NewProxy(true);
}

unsigned int KX_VertexProxy::py_get_colors_size()
{
	return m_array->GetFormat().colorSize;
}

PyObject *KX_VertexProxy::py_get_colors_item(unsigned int index)
{
	const unsigned char *rgba = m_array->GetColor(m_vertexIndex, index);
	mt::vec4 color(rgba[0], rgba[1], rgba[2], rgba[3]);
	color /= 255.0f;
	return PyObjectFrom(color);
}

bool KX_VertexProxy::py_set_colors_item(unsigned int index, PyObject *item)
{
	mt::vec4 color;
	if (!PyVecTo(item, color)) {
		return false;
	}

	m_array->SetColor(m_vertexIndex, index, color);
	m_array->NotifyUpdate(RAS_DisplayArray::COLORS_MODIFIED);

	return true;
}

PyObject *KX_VertexProxy::pyattr_get_colors(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	return (new EXP_ListWrapper<KX_VertexProxy, &KX_VertexProxy::py_get_colors_size, &KX_VertexProxy::py_get_colors_item,
				&KX_VertexProxy::py_set_colors_item>(self_v))->NewProxy(true);
}

PyObject *KX_VertexProxy::pyattr_get_color(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
	const unsigned char *colp = self->m_array->GetColor(self->m_vertexIndex, 0);
	mt::vec4 color(colp[0], colp[1], colp[2], colp[3]);
	color /= 255.0f;
	return PyObjectFrom(color);
}

PyObject *KX_VertexProxy::pyattr_get_normal(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
	return PyObjectFrom(self->m_array->GetNormal(self->m_vertexIndex));
}

int KX_VertexProxy::pyattr_set_x(EXP_PyObjectPlus *self_v, const struct EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
	if (PyFloat_Check(value)) {
		float val = PyFloat_AsDouble(value);
		self->m_array->GetPosition(self->m_vertexIndex).x = val;
		self->m_array->NotifyUpdate(RAS_DisplayArray::POSITION_MODIFIED);
		return PY_SET_ATTR_SUCCESS;
	}
	return PY_SET_ATTR_FAIL;
}

int KX_VertexProxy::pyattr_set_y(EXP_PyObjectPlus *self_v, const struct EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
	if (PyFloat_Check(value)) {
		float val = PyFloat_AsDouble(value);
		self->m_array->GetPosition(self->m_vertexIndex).y = val;
		self->m_array->NotifyUpdate(RAS_DisplayArray::POSITION_MODIFIED);
		return PY_SET_ATTR_SUCCESS;
	}
	return PY_SET_ATTR_FAIL;
}

int KX_VertexProxy::pyattr_set_z(EXP_PyObjectPlus *self_v, const struct EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
	if (PyFloat_Check(value)) {
		float val = PyFloat_AsDouble(value);
		self->m_array->GetPosition(self->m_vertexIndex).z = val;
		self->m_array->NotifyUpdate(RAS_DisplayArray::POSITION_MODIFIED);
		return PY_SET_ATTR_SUCCESS;
	}
	return PY_SET_ATTR_FAIL;
}

int KX_VertexProxy::pyattr_set_u(EXP_PyObjectPlus *self_v, const struct EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
	if (PyFloat_Check(value)) {
		float val = PyFloat_AsDouble(value);
		self->m_array->GetUv(self->m_vertexIndex, 0).x = val;
		self->m_array->NotifyUpdate(RAS_DisplayArray::UVS_MODIFIED);
		return PY_SET_ATTR_SUCCESS;
	}
	return PY_SET_ATTR_FAIL;
}

int KX_VertexProxy::pyattr_set_v(EXP_PyObjectPlus *self_v, const struct EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
	if (PyFloat_Check(value)) {
		float val = PyFloat_AsDouble(value);
		self->m_array->GetUv(self->m_vertexIndex, 0).y = val;
		self->m_array->NotifyUpdate(RAS_DisplayArray::UVS_MODIFIED);
		return PY_SET_ATTR_SUCCESS;
	}
	return PY_SET_ATTR_FAIL;
}

int KX_VertexProxy::pyattr_set_u2(EXP_PyObjectPlus *self_v, const struct EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
	if (PyFloat_Check(value)) {
		if (self->m_array->GetFormat().uvSize > 1) {
			float val = PyFloat_AsDouble(value);
			self->m_array->GetUv(self->m_vertexIndex, 1).x = val;
			self->m_array->NotifyUpdate(RAS_DisplayArray::UVS_MODIFIED);
		}
		return PY_SET_ATTR_SUCCESS;
	}
	return PY_SET_ATTR_FAIL;
}

int KX_VertexProxy::pyattr_set_v2(EXP_PyObjectPlus *self_v, const struct EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
	if (PyFloat_Check(value)) {
		if (self->m_array->GetFormat().uvSize > 1) {
			float val = PyFloat_AsDouble(value);
			self->m_array->GetUv(self->m_vertexIndex, 1).y = val;
			self->m_array->NotifyUpdate(RAS_DisplayArray::UVS_MODIFIED);
		}
		return PY_SET_ATTR_SUCCESS;
	}
	return PY_SET_ATTR_FAIL;
}

int KX_VertexProxy::pyattr_set_r(EXP_PyObjectPlus *self_v, const struct EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
	if (PyFloat_Check(value)) {
		float val = PyFloat_AsDouble(value);
		self->m_array->GetColor(self->m_vertexIndex, 0)[0] = (unsigned char)(val * 255.0f);
		self->m_array->NotifyUpdate(RAS_DisplayArray::COLORS_MODIFIED);
		return PY_SET_ATTR_SUCCESS;
	}
	return PY_SET_ATTR_FAIL;
}

int KX_VertexProxy::pyattr_set_g(EXP_PyObjectPlus *self_v, const struct EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
	if (PyFloat_Check(value)) {
		float val = PyFloat_AsDouble(value);
		self->m_array->GetColor(self->m_vertexIndex, 0)[1] = (unsigned char)(val * 255.0f);
		self->m_array->NotifyUpdate(RAS_DisplayArray::COLORS_MODIFIED);
		return PY_SET_ATTR_SUCCESS;
	}
	return PY_SET_ATTR_FAIL;
}

int KX_VertexProxy::pyattr_set_b(EXP_PyObjectPlus *self_v, const struct EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
	if (PyFloat_Check(value)) {
		float val = PyFloat_AsDouble(value);
		self->m_array->GetColor(self->m_vertexIndex, 0)[2] = (unsigned char)(val * 255.0f);
		self->m_array->NotifyUpdate(RAS_DisplayArray::COLORS_MODIFIED);
		return PY_SET_ATTR_SUCCESS;
	}
	return PY_SET_ATTR_FAIL;
}

int KX_VertexProxy::pyattr_set_a(EXP_PyObjectPlus *self_v, const struct EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
	if (PyFloat_Check(value)) {
		float val = PyFloat_AsDouble(value);
		self->m_array->GetColor(self->m_vertexIndex, 0)[3] = (unsigned char)(val * 255.0f);
		self->m_array->NotifyUpdate(RAS_DisplayArray::COLORS_MODIFIED);
		return PY_SET_ATTR_SUCCESS;
	}
	return PY_SET_ATTR_FAIL;
}

int KX_VertexProxy::pyattr_set_XYZ(EXP_PyObjectPlus *self_v, const struct EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
	mt::vec3_packed vec;
	if (PyVecTo(value, vec)) {
		self->m_array->SetPosition(self->m_vertexIndex, vec);
		self->m_array->NotifyUpdate(RAS_DisplayArray::POSITION_MODIFIED);
		return PY_SET_ATTR_SUCCESS;
	}
	return PY_SET_ATTR_FAIL;
}

int KX_VertexProxy::pyattr_set_UV(EXP_PyObjectPlus *self_v, const struct EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
	mt::vec2_packed vec;
	if (PyVecTo(value, vec)) {
		self->m_array->SetUv(self->m_vertexIndex, 0, vec);
		self->m_array->NotifyUpdate(RAS_DisplayArray::UVS_MODIFIED);
		return PY_SET_ATTR_SUCCESS;
	}
	return PY_SET_ATTR_FAIL;
}

int KX_VertexProxy::pyattr_set_uvs(EXP_PyObjectPlus *self_v, const struct EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
	if (PySequence_Check(value)) {
		mt::vec2_packed vec;
		for (int i = 0; i < PySequence_Size(value) && i < self->m_array->GetFormat().uvSize; ++i) {
			if (PyVecTo(PySequence_GetItem(value, i), vec)) {
				self->m_array->SetUv(self->m_vertexIndex, i, vec);
			}
			else {
				PyErr_SetString(PyExc_AttributeError, ((boost::format("list[%d] was not a vector") % i).str().c_str()));
				return PY_SET_ATTR_FAIL;
			}
		}

		self->m_array->NotifyUpdate(RAS_DisplayArray::UVS_MODIFIED);
		return PY_SET_ATTR_SUCCESS;
	}
	return PY_SET_ATTR_FAIL;
}

int KX_VertexProxy::pyattr_set_color(EXP_PyObjectPlus *self_v, const struct EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
	mt::vec4 vec;
	if (PyVecTo(value, vec)) {
		self->m_array->SetColor(self->m_vertexIndex, 0, vec);
		self->m_array->NotifyUpdate(RAS_DisplayArray::COLORS_MODIFIED);
		return PY_SET_ATTR_SUCCESS;
	}
	return PY_SET_ATTR_FAIL;
}

int KX_VertexProxy::pyattr_set_colors(EXP_PyObjectPlus *self_v, const struct EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
	if (PySequence_Check(value)) {
		mt::vec4 vec;
		for (int i = 0; i < PySequence_Size(value) && i < self->m_array->GetFormat().colorSize; ++i) {
			if (PyVecTo(PySequence_GetItem(value, i), vec)) {
				self->m_array->SetColor(self->m_vertexIndex, i, vec);
			}
			else {
				PyErr_SetString(PyExc_AttributeError, ((boost::format("list[%d] was not a vector") % i).str().c_str()));
				return PY_SET_ATTR_FAIL;
			}
		}

		self->m_array->NotifyUpdate(RAS_DisplayArray::COLORS_MODIFIED);
		return PY_SET_ATTR_SUCCESS;
	}
	return PY_SET_ATTR_FAIL;
}

int KX_VertexProxy::pyattr_set_normal(EXP_PyObjectPlus *self_v, const struct EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_VertexProxy *self = static_cast<KX_VertexProxy *>(self_v);
	mt::vec3_packed vec;
	if (PyVecTo(value, vec)) {
		self->m_array->SetNormal(self->m_vertexIndex, vec);
		self->m_array->NotifyUpdate(RAS_DisplayArray::NORMAL_MODIFIED);
		return PY_SET_ATTR_SUCCESS;
	}
	return PY_SET_ATTR_FAIL;
}

KX_VertexProxy::KX_VertexProxy(RAS_DisplayArray *array, unsigned int vertexIndex)
	:m_vertexIndex(vertexIndex),
	m_array(array)
{
}

KX_VertexProxy::~KX_VertexProxy()
{
}

unsigned int KX_VertexProxy::GetVertexIndex() const
{
	return m_vertexIndex;
}

RAS_DisplayArray *KX_VertexProxy::GetDisplayArray() const
{
	return m_array;
}

// stuff for cvalue related things
std::string KX_VertexProxy::GetName() const
{
	return "vertex";
}

// stuff for python integration
PyObject *KX_VertexProxy::PyGetXYZ()
{
	return PyObjectFrom(m_array->GetPosition(m_vertexIndex));
}

PyObject *KX_VertexProxy::PySetXYZ(PyObject *value)
{
	mt::vec3_packed vec;
	if (!PyVecTo(value, vec)) {
		return nullptr;
	}

	m_array->SetPosition(m_vertexIndex, vec);
	m_array->NotifyUpdate(RAS_DisplayArray::POSITION_MODIFIED);
	Py_RETURN_NONE;
}

PyObject *KX_VertexProxy::PyGetNormal()
{
	return PyObjectFrom(m_array->GetNormal(m_vertexIndex));
}

PyObject *KX_VertexProxy::PySetNormal(PyObject *value)
{
	mt::vec3_packed vec;
	if (!PyVecTo(value, vec)) {
		return nullptr;
	}

	m_array->SetNormal(m_vertexIndex, vec);
	m_array->NotifyUpdate(RAS_DisplayArray::NORMAL_MODIFIED);
	Py_RETURN_NONE;
}

PyObject *KX_VertexProxy::PyGetRGBA()
{
	const unsigned int rgba = m_array->GetRawColor(m_vertexIndex, 0);
	return PyLong_FromLong(rgba);
}

PyObject *KX_VertexProxy::PySetRGBA(PyObject *value)
{
	if (PyLong_Check(value)) {
		int rgba = PyLong_AsLong(value);
		m_array->SetColor(m_vertexIndex, 0, rgba);
		m_array->NotifyUpdate(true);
		Py_RETURN_NONE;
	}
	else {
		mt::vec4 vec;
		if (PyVecTo(value, vec)) {
			m_array->SetColor(m_vertexIndex, 0, vec);
			m_array->NotifyUpdate(RAS_DisplayArray::COLORS_MODIFIED);
			Py_RETURN_NONE;
		}
	}

	PyErr_SetString(PyExc_TypeError, "vert.setRGBA(value): KX_VertexProxy, expected a 4D vector or an int");
	return nullptr;
}

PyObject *KX_VertexProxy::PyGetUV1()
{
	return PyObjectFrom(m_array->GetUv(m_vertexIndex, 0));
}

PyObject *KX_VertexProxy::PySetUV1(PyObject *value)
{
	mt::vec2_packed vec;
	if (!PyVecTo(value, vec)) {
		return nullptr;
	}

	m_array->SetUv(m_vertexIndex, 0, vec);
	m_array->NotifyUpdate(RAS_DisplayArray::UVS_MODIFIED);
	Py_RETURN_NONE;
}

PyObject *KX_VertexProxy::PyGetUV2()
{
	return (m_array->GetFormat().uvSize > 1) ? PyObjectFrom(m_array->GetUv(m_vertexIndex, 1)) : PyObjectFrom(mt::zero2);
}

PyObject *KX_VertexProxy::PySetUV2(PyObject *args)
{
	mt::vec2_packed vec;
	if (!PyVecTo(args, vec)) {
		return nullptr;
	}

	if (m_array->GetFormat().uvSize > 1) {
		m_array->SetUv(m_vertexIndex, 1, vec);
		m_array->NotifyUpdate(RAS_DisplayArray::UVS_MODIFIED);
	}
	Py_RETURN_NONE;
}

#endif // WITH_PYTHON
