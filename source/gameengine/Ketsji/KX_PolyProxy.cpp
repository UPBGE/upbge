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

#  include "KX_PolyProxy.h"

#  include "EXP_ListWrapper.h"
#  include "KX_BlenderMaterial.h"
#  include "KX_MeshProxy.h"
#  include "KX_PyMath.h"
#  include "KX_VertexProxy.h"
#  include "RAS_IDisplayArray.h"
#  include "RAS_MeshObject.h"
#  include "RAS_Polygon.h"

RAS_Polygon *KX_PolyProxy::GetPolygon()
{
  return m_polygon;
}

KX_MeshProxy *KX_PolyProxy::GetMeshProxy()
{
  return m_meshProxy;
}

PyTypeObject KX_PolyProxy::Type = {PyVarObject_HEAD_INIT(nullptr, 0) "KX_PolyProxy",
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
    {nullptr, nullptr}  // Sentinel
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
    EXP_PYATTRIBUTE_NULL  // Sentinel
};

KX_PolyProxy::KX_PolyProxy(KX_MeshProxy *meshProxy, RAS_MeshObject *mesh, RAS_Polygon *polygon)
    : m_meshProxy(meshProxy), m_polygon(polygon), m_mesh(mesh)
{
  Py_INCREF(m_meshProxy->GetProxy());
}

KX_PolyProxy::~KX_PolyProxy()
{
  Py_DECREF(m_meshProxy->GetProxy());
}

// stuff for cvalue related things
std::string KX_PolyProxy::GetName()
{
  return "polygone";
}

// stuff for python integration

PyObject *KX_PolyProxy::pyattr_get_material_name(EXP_PyObjectPlus *self_v,
                                                 const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_PolyProxy *self = static_cast<KX_PolyProxy *>(self_v);
  return self->PygetMaterialName();
}

PyObject *KX_PolyProxy::pyattr_get_texture_name(EXP_PyObjectPlus *self_v,
                                                const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_PolyProxy *self = static_cast<KX_PolyProxy *>(self_v);
  return self->PygetTextureName();
}

PyObject *KX_PolyProxy::pyattr_get_material(EXP_PyObjectPlus *self_v,
                                            const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_PolyProxy *self = static_cast<KX_PolyProxy *>(self_v);
  return self->PygetMaterial();
}

PyObject *KX_PolyProxy::pyattr_get_material_id(EXP_PyObjectPlus *self_v,
                                               const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_PolyProxy *self = static_cast<KX_PolyProxy *>(self_v);
  return self->PygetMaterialIndex();
}

PyObject *KX_PolyProxy::pyattr_get_v1(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_PolyProxy *self = static_cast<KX_PolyProxy *>(self_v);

  return PyLong_FromLong(self->m_polygon->GetVertexOffset(0));
}

PyObject *KX_PolyProxy::pyattr_get_v2(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_PolyProxy *self = static_cast<KX_PolyProxy *>(self_v);

  return PyLong_FromLong(self->m_polygon->GetVertexOffset(1));
}

PyObject *KX_PolyProxy::pyattr_get_v3(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_PolyProxy *self = static_cast<KX_PolyProxy *>(self_v);

  return PyLong_FromLong(self->m_polygon->GetVertexOffset(2));
}

PyObject *KX_PolyProxy::pyattr_get_v4(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_PolyProxy *self = static_cast<KX_PolyProxy *>(self_v);

  if (3 < self->m_polygon->VertexCount()) {
    return PyLong_FromLong(self->m_polygon->GetVertexOffset(3));
  }
  return PyLong_FromLong(0);
}

PyObject *KX_PolyProxy::pyattr_get_visible(EXP_PyObjectPlus *self_v,
                                           const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_PolyProxy *self = static_cast<KX_PolyProxy *>(self_v);
  return self->PyisVisible();
}

PyObject *KX_PolyProxy::pyattr_get_collide(EXP_PyObjectPlus *self_v,
                                           const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_PolyProxy *self = static_cast<KX_PolyProxy *>(self_v);
  return self->PyisCollider();
}

static int kx_poly_proxy_get_vertices_size_cb(void *self_v)
{
  return ((KX_PolyProxy *)self_v)->GetPolygon()->VertexCount();
}

static PyObject *kx_poly_proxy_get_vertices_item_cb(void *self_v, int index)
{
  KX_PolyProxy *self = static_cast<KX_PolyProxy *>(self_v);
  RAS_Polygon *polygon = self->GetPolygon();
  int vertindex = polygon->GetVertexOffset(index);
  RAS_IDisplayArray *array = polygon->GetDisplayArray();
  KX_VertexProxy *vert = new KX_VertexProxy(array, array->GetVertex(vertindex));

  return vert->GetProxy();
}

PyObject *KX_PolyProxy::pyattr_get_vertices(EXP_PyObjectPlus *self_v,
                                            const EXP_PYATTRIBUTE_DEF *attrdef)
{
  return (new EXP_ListWrapper(self_v,
                              ((KX_PolyProxy *)self_v)->GetProxy(),
                              nullptr,
                              kx_poly_proxy_get_vertices_size_cb,
                              kx_poly_proxy_get_vertices_item_cb,
                              nullptr,
                              nullptr))
      ->NewProxy(true);
}

EXP_PYMETHODDEF_DOC_NOARGS(
    KX_PolyProxy,
    getMaterialIndex,
    "getMaterialIndex() : return the material index of the polygon in the mesh\n")
{
  RAS_MaterialBucket *polyBucket = m_polygon->GetMaterial();
  unsigned int matid;
  for (matid = 0; matid < (unsigned int)m_mesh->NumMaterials(); matid++) {
    RAS_MeshMaterial *meshMat = m_mesh->GetMeshMaterial(matid);
    if (meshMat->GetBucket() == polyBucket)
      // found it
      break;
  }
  return PyLong_FromLong(matid);
}

EXP_PYMETHODDEF_DOC_NOARGS(
    KX_PolyProxy,
    getNumVertex,
    "getNumVertex() : returns the number of vertex of the polygon, 3 or 4\n")
{
  return PyLong_FromLong(m_polygon->VertexCount());
}

EXP_PYMETHODDEF_DOC_NOARGS(KX_PolyProxy,
                           isVisible,
                           "isVisible() : returns whether the polygon is visible or not\n")
{
  return PyLong_FromLong(m_polygon->IsVisible());
}

EXP_PYMETHODDEF_DOC_NOARGS(
    KX_PolyProxy,
    isCollider,
    "isCollider() : returns whether the polygon is receives collision or not\n")
{
  return PyLong_FromLong(m_polygon->IsCollider());
}

EXP_PYMETHODDEF_DOC_NOARGS(
    KX_PolyProxy,
    getMaterialName,
    "getMaterialName() : returns the polygon material name, \"NoMaterial\" if no material\n")
{
  return PyUnicode_FromStdString(m_polygon->GetMaterial()->GetPolyMaterial()->GetName());
}

EXP_PYMETHODDEF_DOC_NOARGS(
    KX_PolyProxy,
    getTextureName,
    "getTexturelName() : returns the polygon texture name, \"nullptr\" if no texture\n")
{
  return PyUnicode_FromStdString(m_polygon->GetMaterial()->GetPolyMaterial()->GetTextureName());
}

EXP_PYMETHODDEF_DOC(KX_PolyProxy,
                    getVertexIndex,
                    "getVertexIndex(vertex) : returns the mesh vertex index of a polygon vertex\n"
                    "vertex: index of the vertex in the polygon: 0->3\n"
                    "return value can be used to retrieve the vertex details through mesh proxy\n"
                    "Note: getVertexIndex(3) on a triangle polygon returns 0\n")
{
  int index;
  if (!PyArg_ParseTuple(args, "i:getVertexIndex", &index)) {
    return nullptr;
  }
  if (index < 0 || index > 3) {
    PyErr_SetString(PyExc_AttributeError,
                    "poly.getVertexIndex(int): KX_PolyProxy, expected an index between 0-3");
    return nullptr;
  }
  if (index < m_polygon->VertexCount()) {
    return PyLong_FromLong(m_polygon->GetVertexOffset(index));
  }
  return PyLong_FromLong(0);
}

EXP_PYMETHODDEF_DOC_NOARGS(KX_PolyProxy, getMesh, "getMesh() : returns a mesh proxy\n")
{
  return m_meshProxy->GetProxy();
}

EXP_PYMETHODDEF_DOC_NOARGS(KX_PolyProxy, getMaterial, "getMaterial() : returns a material\n")
{
  RAS_IPolyMaterial *polymat = m_polygon->GetMaterial()->GetPolyMaterial();
  KX_BlenderMaterial *mat = static_cast<KX_BlenderMaterial *>(polymat);
  return mat->GetProxy();
}

#endif  // WITH_PYTHON
