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

/** \file gameengine/Ketsji/KX_PolyProxy.cpp
 *  \ingroup ketsji
 */


#ifdef WITH_PYTHON

#include "KX_PolyProxy.h"
#include "KX_Mesh.h"
#include "RAS_Mesh.h"
#include "RAS_MaterialBucket.h"
#include "RAS_DisplayArray.h"
#include "KX_VertexProxy.h"
#include "BL_Material.h"
#include "EXP_ListWrapper.h"

#include "KX_PyMath.h"

PyTypeObject KX_PolyProxy::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"KX_PolyProxy",
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

PyMethodDef KX_PolyProxy::Methods[] = {
	EXP_PYMETHODTABLE_NOARGS(KX_PolyProxy, getMaterialIndex),
	EXP_PYMETHODTABLE_NOARGS(KX_PolyProxy, getNumVertex),
	EXP_PYMETHODTABLE_NOARGS(KX_PolyProxy, isVisible),
	EXP_PYMETHODTABLE_NOARGS(KX_PolyProxy, isCollider),
	EXP_PYMETHODTABLE_NOARGS(KX_PolyProxy, getMaterialName),
	EXP_PYMETHODTABLE_NOARGS(KX_PolyProxy, getTextureName),
	EXP_PYMETHODTABLE(KX_PolyProxy, getVertexIndex),
	EXP_PYMETHODTABLE_NOARGS(KX_PolyProxy, getMesh),
	EXP_PYMETHODTABLE_NOARGS(KX_PolyProxy, getMaterial),
	{nullptr, nullptr} //Sentinel
};

PyAttributeDef KX_PolyProxy::Attributes[] = {
	EXP_PYATTRIBUTE_RO_FUNCTION("material_name", KX_PolyProxy, pyattr_get_material_name),
	EXP_PYATTRIBUTE_RO_FUNCTION("texture_name", KX_PolyProxy, pyattr_get_texture_name),
	EXP_PYATTRIBUTE_RO_FUNCTION("material", KX_PolyProxy, pyattr_get_material),
	EXP_PYATTRIBUTE_RO_FUNCTION("material_id", KX_PolyProxy, pyattr_get_material_id),
	EXP_PYATTRIBUTE_RO_FUNCTION("v1", KX_PolyProxy, pyattr_get_v1),
	EXP_PYATTRIBUTE_RO_FUNCTION("v2", KX_PolyProxy, pyattr_get_v2),
	EXP_PYATTRIBUTE_RO_FUNCTION("v3", KX_PolyProxy, pyattr_get_v3),
	EXP_PYATTRIBUTE_RO_FUNCTION("v4", KX_PolyProxy, pyattr_get_v4),
	EXP_PYATTRIBUTE_RO_FUNCTION("visible", KX_PolyProxy, pyattr_get_visible),
	EXP_PYATTRIBUTE_RO_FUNCTION("collide", KX_PolyProxy, pyattr_get_collide),
	EXP_PYATTRIBUTE_RO_FUNCTION("vertices", KX_PolyProxy, pyattr_get_vertices),
	EXP_PYATTRIBUTE_NULL    //Sentinel
};

KX_PolyProxy::KX_PolyProxy(KX_Mesh *mesh, const RAS_Mesh::PolygonInfo& polygon)
	:m_mesh(mesh),
	m_polygon(polygon)
{
}

KX_PolyProxy::~KX_PolyProxy()
{
}


// stuff for cvalue related things
std::string KX_PolyProxy::GetName()
{
	return "polygone";
}

const RAS_Mesh::PolygonInfo& KX_PolyProxy::GetPolygon() const
{
	return m_polygon;
}

// stuff for python integration

PyObject *KX_PolyProxy::pyattr_get_material_name(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_PolyProxy *self = static_cast<KX_PolyProxy *>(self_v);
	return self->PygetMaterialName();
}

PyObject *KX_PolyProxy::pyattr_get_texture_name(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_PolyProxy *self = static_cast<KX_PolyProxy *>(self_v);
	return self->PygetTextureName();
}

PyObject *KX_PolyProxy::pyattr_get_material(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_PolyProxy *self = static_cast<KX_PolyProxy *>(self_v);
	return self->PygetMaterial();
}

PyObject *KX_PolyProxy::pyattr_get_material_id(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_PolyProxy *self = static_cast<KX_PolyProxy *>(self_v);
	return self->PygetMaterialIndex();
}

PyObject *KX_PolyProxy::pyattr_get_v1(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_PolyProxy *self = static_cast<KX_PolyProxy *>(self_v);

	return PyLong_FromLong(self->m_polygon.indices[0]);
}

PyObject *KX_PolyProxy::pyattr_get_v2(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_PolyProxy *self = static_cast<KX_PolyProxy *>(self_v);

	return PyLong_FromLong(self->m_polygon.indices[1]);
}

PyObject *KX_PolyProxy::pyattr_get_v3(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_PolyProxy *self = static_cast<KX_PolyProxy *>(self_v);

	return PyLong_FromLong(self->m_polygon.indices[2]);
}

PyObject *KX_PolyProxy::pyattr_get_v4(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	return PyLong_FromLong(0);
}

PyObject *KX_PolyProxy::pyattr_get_visible(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_PolyProxy *self = static_cast<KX_PolyProxy *>(self_v);
	return self->PyisVisible();
}

PyObject *KX_PolyProxy::pyattr_get_collide(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_PolyProxy *self = static_cast<KX_PolyProxy *>(self_v);
	return self->PyisCollider();
}

unsigned int KX_PolyProxy::py_get_vertices_size()
{
	return 3;
}

PyObject *KX_PolyProxy::py_get_vertices_item(unsigned int index)
{
	KX_VertexProxy *vert = new KX_VertexProxy(m_polygon.array, m_polygon.indices[index]);

	return vert->NewProxy(true);
}

PyObject *KX_PolyProxy::pyattr_get_vertices(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	return (new EXP_ListWrapper<KX_PolyProxy, &KX_PolyProxy::py_get_vertices_size, &KX_PolyProxy::py_get_vertices_item>(self_v))->NewProxy(true);
}

EXP_PYMETHODDEF_DOC_NOARGS(KX_PolyProxy, getMaterialIndex,
                           "getMaterialIndex() : return the material index of the polygon in the mesh\n")
{
	return PyLong_FromLong(m_polygon.matId);
}

EXP_PYMETHODDEF_DOC_NOARGS(KX_PolyProxy, getNumVertex,
                           "getNumVertex() : returns the number of vertex of the polygon\n")
{
	return PyLong_FromLong(3);
}

EXP_PYMETHODDEF_DOC_NOARGS(KX_PolyProxy, isVisible,
                           "isVisible() : returns whether the polygon is visible or not\n")
{
	return PyLong_FromLong(m_polygon.flags & RAS_Mesh::PolygonInfo::VISIBLE);
}

EXP_PYMETHODDEF_DOC_NOARGS(KX_PolyProxy, isCollider,
                           "isCollider() : returns whether the polygon is receives collision or not\n")
{
	return PyLong_FromLong(m_polygon.flags & RAS_Mesh::PolygonInfo::COLLIDER);
}

EXP_PYMETHODDEF_DOC_NOARGS(KX_PolyProxy, getMaterialName,
                           "getMaterialName() : returns the polygon material name, \"\" if no material\n")
{
	return PyUnicode_FromStdString(m_mesh->GetMaterialName(m_polygon.matId));
}

EXP_PYMETHODDEF_DOC_NOARGS(KX_PolyProxy, getTextureName,
                           "getTexturelName() : returns the polygon texture name, \"\" if no texture\n")
{
	return PyUnicode_FromStdString(m_mesh->GetTextureName(m_polygon.matId));
}

EXP_PYMETHODDEF_DOC(KX_PolyProxy, getVertexIndex,
                    "getVertexIndex(vertex) : returns the mesh vertex index of a polygon vertex\n"
                    "vertex: index of the vertex in the polygon: 0->2\n"
                    "return value can be used to retrieve the vertex details through mesh proxy\n")
{
	int index;
	if (!PyArg_ParseTuple(args, "i:getVertexIndex", &index)) {
		return nullptr;
	}
	if (index < 0 || index > 3) {
		PyErr_SetString(PyExc_AttributeError, "poly.getVertexIndex(int): KX_PolyProxy, expected an index between 0-2");
		return nullptr;
	}
	if (index < 3) {
		return PyLong_FromLong(m_polygon.indices[index]);
	}
	return PyLong_FromLong(0);
}

EXP_PYMETHODDEF_DOC_NOARGS(KX_PolyProxy, getMesh,
                           "getMesh() : returns a mesh proxy\n")
{
	return m_mesh->GetProxy();
}

EXP_PYMETHODDEF_DOC_NOARGS(KX_PolyProxy, getMaterial,
                           "getMaterial() : returns a material\n")
{
	RAS_MeshMaterial *meshmat = m_mesh->GetMeshMaterial(m_polygon.matId);
	BL_Material *mat = static_cast<BL_Material *>(meshmat->GetBucket()->GetMaterial());
	return mat->GetProxy();
}

#endif // WITH_PYTHON
