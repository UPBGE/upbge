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

/** \file KX_VertexProxy.h
 *  \ingroup ketsji
 */

#ifndef __KX_VERTEXPROXY_H__
#define __KX_VERTEXPROXY_H__

#ifdef WITH_PYTHON

#include "EXP_Value.h"

class RAS_DisplayArray;

class KX_VertexProxy : public EXP_Value
{
	Py_Header

protected:
	unsigned int m_vertexIndex;
	RAS_DisplayArray *m_array;

public:
	KX_VertexProxy(RAS_DisplayArray *array, unsigned int vertexIndex);
	virtual ~KX_VertexProxy();

	unsigned int GetVertexIndex() const;
	RAS_DisplayArray *GetDisplayArray() const;

	// stuff for cvalue related things
	virtual std::string GetName() const;

	static PyObject *pyattr_get_x(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_y(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_z(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_r(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_g(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_b(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_a(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_u(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_v(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_u2(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_v2(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_XYZ(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_UV(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_color(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_colors(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_normal(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_uvs(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_x(EXP_PyObjectPlus *self, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static int pyattr_set_y(EXP_PyObjectPlus *self, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static int pyattr_set_z(EXP_PyObjectPlus *self, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static int pyattr_set_u(EXP_PyObjectPlus *self, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static int pyattr_set_v(EXP_PyObjectPlus *self, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static int pyattr_set_u2(EXP_PyObjectPlus *self, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static int pyattr_set_v2(EXP_PyObjectPlus *self, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static int pyattr_set_r(EXP_PyObjectPlus *self, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static int pyattr_set_g(EXP_PyObjectPlus *self, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static int pyattr_set_b(EXP_PyObjectPlus *self, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static int pyattr_set_a(EXP_PyObjectPlus *self, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static int pyattr_set_XYZ(EXP_PyObjectPlus *self, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static int pyattr_set_UV(EXP_PyObjectPlus *self, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static int pyattr_set_color(EXP_PyObjectPlus *self, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static int pyattr_set_colors(EXP_PyObjectPlus *self, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static int pyattr_set_normal(EXP_PyObjectPlus *self, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static int pyattr_set_uvs(EXP_PyObjectPlus *self, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);

	unsigned int py_get_uvs_size();
	PyObject *py_get_uvs_item(unsigned int index);
	bool py_set_uvs_item(unsigned int index, PyObject *item);
	unsigned int py_get_colors_size();
	PyObject *py_get_colors_item(unsigned int index);
	bool py_set_colors_item(unsigned int index, PyObject *item);

	EXP_PYMETHOD_NOARGS(KX_VertexProxy, GetXYZ);
	EXP_PYMETHOD_O(KX_VertexProxy, SetXYZ);
	EXP_PYMETHOD_NOARGS(KX_VertexProxy, GetUV1);
	EXP_PYMETHOD_O(KX_VertexProxy, SetUV1);

	EXP_PYMETHOD_NOARGS(KX_VertexProxy, GetUV2);
	EXP_PYMETHOD_VARARGS(KX_VertexProxy, SetUV2);

	EXP_PYMETHOD_NOARGS(KX_VertexProxy, GetRGBA);
	EXP_PYMETHOD_O(KX_VertexProxy, SetRGBA);
	EXP_PYMETHOD_NOARGS(KX_VertexProxy, GetNormal);
	EXP_PYMETHOD_O(KX_VertexProxy, SetNormal);
};

#endif  // WITH_PYTHON

#endif  // __KX_VERTEXPROXY_H__
