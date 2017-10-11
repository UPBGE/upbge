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

EXP_Attribute KX_VertexProxy::Attributes[] = {
	EXP_ATTRIBUTE_RW_FUNCTION("x", pyattr_get_x, pyattr_set_x),
	EXP_ATTRIBUTE_RW_FUNCTION("y", pyattr_get_y, pyattr_set_y),
	EXP_ATTRIBUTE_RW_FUNCTION("z", pyattr_get_z, pyattr_set_z),

	EXP_ATTRIBUTE_RW_FUNCTION("r", pyattr_get_r, pyattr_set_r),
	EXP_ATTRIBUTE_RW_FUNCTION("g", pyattr_get_g, pyattr_set_g),
	EXP_ATTRIBUTE_RW_FUNCTION("b", pyattr_get_b, pyattr_set_b),
	EXP_ATTRIBUTE_RW_FUNCTION("a", pyattr_get_a, pyattr_set_a),

	EXP_ATTRIBUTE_RW_FUNCTION("u", pyattr_get_u, pyattr_set_u),
	EXP_ATTRIBUTE_RW_FUNCTION("v", pyattr_get_v, pyattr_set_v),

	EXP_ATTRIBUTE_RW_FUNCTION("u2", pyattr_get_u2, pyattr_set_u2),
	EXP_ATTRIBUTE_RW_FUNCTION("v2", pyattr_get_v2, pyattr_set_v2),

	EXP_ATTRIBUTE_RW_FUNCTION("XYZ", pyattr_get_XYZ, pyattr_set_XYZ),
	EXP_ATTRIBUTE_RW_FUNCTION("UV", pyattr_get_UV, pyattr_set_UV),
	EXP_ATTRIBUTE_RW_FUNCTION("uvs", pyattr_get_uvs, pyattr_set_uvs),

	EXP_ATTRIBUTE_RW_FUNCTION("color", pyattr_get_color, pyattr_set_color),
	EXP_ATTRIBUTE_RW_FUNCTION("colors", pyattr_get_colors, pyattr_set_colors),
	EXP_ATTRIBUTE_RW_FUNCTION("normal", pyattr_get_normal, pyattr_set_normal),

	EXP_ATTRIBUTE_NULL //Sentinel
};

float KX_VertexProxy::pyattr_get_x()
{
	return m_array->GetPosition(m_vertexIndex).x;
}

float KX_VertexProxy::pyattr_get_y()
{
	return m_array->GetPosition(m_vertexIndex).y;
}

float KX_VertexProxy::pyattr_get_z()
{
	return m_array->GetPosition(m_vertexIndex).z;
}

float KX_VertexProxy::pyattr_get_r()
{
	return m_array->GetColor(m_vertexIndex, 0)[0] / 255.0f;
}

float KX_VertexProxy::pyattr_get_g()
{
	return m_array->GetColor(m_vertexIndex, 0)[1] / 255.0f;
}

float KX_VertexProxy::pyattr_get_b()
{
	return m_array->GetColor(m_vertexIndex, 0)[2] / 255.0f;
}

float KX_VertexProxy::pyattr_get_a()
{
	return m_array->GetColor(m_vertexIndex, 0)[3] / 255.0f;
}

float KX_VertexProxy::pyattr_get_u()
{
	return m_array->GetUv(m_vertexIndex, 0).x;
}

float KX_VertexProxy::pyattr_get_v()
{
	return m_array->GetUv(m_vertexIndex, 0).y;
}

float KX_VertexProxy::pyattr_get_u2()
{
	return (m_array->GetFormat().uvSize > 1) ? m_array->GetUv(m_vertexIndex, 1).x : 0.0f;
}

float KX_VertexProxy::pyattr_get_v2()
{
	return (m_array->GetFormat().uvSize > 1) ? m_array->GetUv(m_vertexIndex, 1).y : 0.0f;
}

mt::vec3_packed KX_VertexProxy::pyattr_get_XYZ()
{
	return m_array->GetPosition(m_vertexIndex);
}

mt::vec2_packed KX_VertexProxy::pyattr_get_UV()
{
	return m_array->GetUv(m_vertexIndex, 0);
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

EXP_BaseListWrapper *KX_VertexProxy::pyattr_get_uvs()
{
	return (new EXP_ListWrapper<KX_VertexProxy, &KX_VertexProxy::py_get_uvs_size, &KX_VertexProxy::py_get_uvs_item,
				&KX_VertexProxy::py_set_uvs_item>(this));
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

EXP_BaseListWrapper *KX_VertexProxy::pyattr_get_colors()
{
	return (new EXP_ListWrapper<KX_VertexProxy, &KX_VertexProxy::py_get_colors_size, &KX_VertexProxy::py_get_colors_item,
				&KX_VertexProxy::py_set_colors_item>(this));
}

mt::vec4 KX_VertexProxy::pyattr_get_color()
{
	const unsigned char *colp = m_array->GetColor(m_vertexIndex, 0);
	mt::vec4 color(colp[0], colp[1], colp[2], colp[3]);
	return color / 255.0f;
}

mt::vec3_packed KX_VertexProxy::pyattr_get_normal()
{
	return m_array->GetNormal(m_vertexIndex);
}

void KX_VertexProxy::pyattr_set_x(float value)
{
	m_array->GetPosition(m_vertexIndex).x = value;
	m_array->NotifyUpdate(RAS_DisplayArray::POSITION_MODIFIED);
}

void KX_VertexProxy::pyattr_set_y(float value)
{
	m_array->GetPosition(m_vertexIndex).y = value;
	m_array->NotifyUpdate(RAS_DisplayArray::POSITION_MODIFIED);
}

void KX_VertexProxy::pyattr_set_z(float value)
{
	m_array->GetPosition(m_vertexIndex).z = value;
	m_array->NotifyUpdate(RAS_DisplayArray::POSITION_MODIFIED);
}

void KX_VertexProxy::pyattr_set_u(float value)
{
	m_array->GetUv(m_vertexIndex, 0).x = value;
	m_array->NotifyUpdate(RAS_DisplayArray::UVS_MODIFIED);
}

void KX_VertexProxy::pyattr_set_v(float value)
{
	m_array->GetUv(m_vertexIndex, 0).y = value;
	m_array->NotifyUpdate(RAS_DisplayArray::UVS_MODIFIED);
}

bool KX_VertexProxy::pyattr_set_u2(float value)
{
	if (m_array->GetFormat().uvSize == 1) {
		return false;
	}

	m_array->GetUv(m_vertexIndex, 1).x = value;
	m_array->NotifyUpdate(RAS_DisplayArray::UVS_MODIFIED);
	return true;
}

bool KX_VertexProxy::pyattr_set_v2(float value)
{
	if (m_array->GetFormat().uvSize == 1) {
		return false;
	}

	m_array->GetUv(m_vertexIndex, 1).y = value;
	m_array->NotifyUpdate(RAS_DisplayArray::UVS_MODIFIED);
	return true;
}

void KX_VertexProxy::pyattr_set_r(float value)
{
	m_array->GetColor(m_vertexIndex, 0)[0] = (unsigned char)(value * 255.0f);
	m_array->NotifyUpdate(RAS_DisplayArray::COLORS_MODIFIED);
}

void KX_VertexProxy::pyattr_set_g(float value)
{
	m_array->GetColor(m_vertexIndex, 0)[1] = (unsigned char)(value * 255.0f);
	m_array->NotifyUpdate(RAS_DisplayArray::COLORS_MODIFIED);
}

void KX_VertexProxy::pyattr_set_b(float value)
{
	m_array->GetColor(m_vertexIndex, 0)[2] = (unsigned char)(value * 255.0f);
	m_array->NotifyUpdate(RAS_DisplayArray::COLORS_MODIFIED);
}

void KX_VertexProxy::pyattr_set_a(float value)
{
	m_array->GetColor(m_vertexIndex, 0)[3] = (unsigned char)(value * 255.0f);
	m_array->NotifyUpdate(RAS_DisplayArray::COLORS_MODIFIED);
}

void KX_VertexProxy::pyattr_set_XYZ(const mt::vec3_packed& value)
{
	m_array->SetPosition(m_vertexIndex, value);
	m_array->NotifyUpdate(RAS_DisplayArray::POSITION_MODIFIED);
}

void KX_VertexProxy::pyattr_set_UV(const mt::vec2_packed& value)
{
	m_array->SetUv(m_vertexIndex, 0, value);
	m_array->NotifyUpdate(RAS_DisplayArray::UVS_MODIFIED);
}

bool KX_VertexProxy::pyattr_set_uvs(PyObject *value, const EXP_Attribute *attrdef)
{
	if (PySequence_Check(value)) {
		mt::vec2_packed vec;
		for (int i = 0; i < PySequence_Size(value) && i < m_array->GetFormat().uvSize; ++i) {
			if (PyVecTo(PySequence_GetItem(value, i), vec)) {
				m_array->SetUv(m_vertexIndex, i, vec);
			}
			else {
				attrdef->PrintError((boost::format("list[%d] was not a vector") % i).str().c_str());
				return false;
			}
		}

		m_array->NotifyUpdate(RAS_DisplayArray::UVS_MODIFIED);
		return PY_SET_ATTR_SUCCESS;
	}
	return false;
}

void KX_VertexProxy::pyattr_set_color(const mt::vec4& value)
{
	m_array->SetColor(m_vertexIndex, 0, value);
	m_array->NotifyUpdate(RAS_DisplayArray::COLORS_MODIFIED);
}

bool KX_VertexProxy::pyattr_set_colors(PyObject *value, const EXP_Attribute *attrdef)
{
	if (PySequence_Check(value)) {
		mt::vec4 vec;
		for (int i = 0; i < PySequence_Size(value) && i < m_array->GetFormat().colorSize; ++i) {
			if (PyVecTo(PySequence_GetItem(value, i), vec)) {
				m_array->SetColor(m_vertexIndex, i, vec);
			}
			else {
				attrdef->PrintError((boost::format("list[%d] was not a vector") % i).str().c_str());
				return false;
			}
		}

		m_array->NotifyUpdate(RAS_DisplayArray::COLORS_MODIFIED);
		return PY_SET_ATTR_SUCCESS;
	}
	return false;
}

void KX_VertexProxy::pyattr_set_normal(const mt::vec3_packed& value)
{
	m_array->SetNormal(m_vertexIndex, value);
	m_array->NotifyUpdate(RAS_DisplayArray::NORMAL_MODIFIED);
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
