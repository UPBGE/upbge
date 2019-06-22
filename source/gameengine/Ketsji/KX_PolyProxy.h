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

/** \file KX_PolyProxy.h
 *  \ingroup ketsji
 */

#ifndef __KX_POLYPROXY_H__
#define __KX_POLYPROXY_H__

#ifdef WITH_PYTHON

#include "EXP_Value.h"
#include "RAS_Mesh.h"

class KX_Mesh;

class KX_PolyProxy : public EXP_Value
{
	Py_Header
protected:
	KX_Mesh *m_mesh;
	RAS_Mesh::PolygonInfo m_polygon;

public:
	KX_PolyProxy(KX_Mesh *mesh, const RAS_Mesh::PolygonInfo& polygon);
	virtual ~KX_PolyProxy();

	// stuff for cvalue related things
	virtual std::string GetName() const;

	const RAS_Mesh::PolygonInfo& GetPolygon() const;

	// stuff for python integration
	static PyObject *pyattr_get_material_name(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_texture_name(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_material(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_material_id(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_v1(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_v2(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_v3(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_v4(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_visible(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_collide(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_vertices(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);

	unsigned int py_get_vertices_size();
	PyObject *py_get_vertices_item(unsigned int index);

	EXP_PYMETHOD_DOC_NOARGS(KX_PolyProxy, getMaterialIndex)
	EXP_PYMETHOD_DOC_NOARGS(KX_PolyProxy, getNumVertex)
	EXP_PYMETHOD_DOC_NOARGS(KX_PolyProxy, isVisible)
	EXP_PYMETHOD_DOC_NOARGS(KX_PolyProxy, isCollider)
	EXP_PYMETHOD_DOC_NOARGS(KX_PolyProxy, getMaterialName)
	EXP_PYMETHOD_DOC_NOARGS(KX_PolyProxy, getTextureName)
	EXP_PYMETHOD_DOC(KX_PolyProxy, getVertexIndex)
	EXP_PYMETHOD_DOC_NOARGS(KX_PolyProxy, getMesh)
	EXP_PYMETHOD_DOC_NOARGS(KX_PolyProxy, getMaterial)

};

#endif  /* WITH_PYTHON */

#endif  /* __KX_POLYPROXY_H__ */
