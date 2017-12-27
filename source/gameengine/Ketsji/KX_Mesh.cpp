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

/** \file gameengine/Ketsji/KX_Mesh.cpp
 *  \ingroup ketsji
 */

#include "KX_Mesh.h"
#include "KX_Scene.h"
#include "KX_Globals.h"
#include "KX_KetsjiEngine.h"

#include "BL_Converter.h"

#include "RAS_IPolygonMaterial.h"
#include "RAS_DisplayArray.h"
#include "RAS_BucketManager.h"
#include "SCA_LogicManager.h"

#include "KX_VertexProxy.h"
#include "KX_PolyProxy.h"

#include "KX_BlenderMaterial.h"

#include "KX_PyMath.h"

#include "SCA_LogicManager.h"

#include "EXP_PyObjectPlus.h"
#include "EXP_ListWrapper.h"

KX_Mesh::KX_Mesh(KX_Scene *scene, Mesh *mesh, const RAS_Mesh::LayersInfo& layersInfo)
	:RAS_Mesh(mesh, layersInfo),
	m_scene(scene)
{
}

KX_Mesh::KX_Mesh(const KX_Mesh& other)
	:RAS_Mesh(other),
	m_scene(other.m_scene)
{
}

KX_Mesh::~KX_Mesh()
{
}

void KX_Mesh::ReplaceScene(KX_Scene *scene)
{
	m_scene = scene;
}

#ifdef WITH_PYTHON

PyTypeObject KX_Mesh::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"KX_Mesh",
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

PyMethodDef KX_Mesh::Methods[] = {
	{"getMaterialName", (PyCFunction) KX_Mesh::sPyGetMaterialName, METH_VARARGS},
	{"getTextureName", (PyCFunction) KX_Mesh::sPyGetTextureName, METH_VARARGS},
	{"getVertexArrayLength", (PyCFunction) KX_Mesh::sPyGetVertexArrayLength, METH_VARARGS},
	{"getVertex", (PyCFunction) KX_Mesh::sPyGetVertex, METH_VARARGS},
	{"getPolygon", (PyCFunction) KX_Mesh::sPyGetPolygon, METH_VARARGS},
	{"transform", (PyCFunction) KX_Mesh::sPyTransform, METH_VARARGS},
	{"transformUV", (PyCFunction) KX_Mesh::sPyTransformUV, METH_VARARGS},
	{"replaceMaterial", (PyCFunction) KX_Mesh::sPyReplaceMaterial, METH_VARARGS},
	{"copy", (PyCFunction) KX_Mesh::sPyCopy, METH_NOARGS},
	{nullptr, nullptr} //Sentinel
};

PyAttributeDef KX_Mesh::Attributes[] = {
	EXP_PYATTRIBUTE_RO_FUNCTION("materials",     KX_Mesh, pyattr_get_materials),
	EXP_PYATTRIBUTE_RO_FUNCTION("numPolygons",   KX_Mesh, pyattr_get_numPolygons),
	EXP_PYATTRIBUTE_RO_FUNCTION("numMaterials",  KX_Mesh, pyattr_get_numMaterials),
	EXP_PYATTRIBUTE_RO_FUNCTION("polygons",      KX_Mesh, pyattr_get_polygons),

	EXP_PYATTRIBUTE_NULL    //Sentinel
};

std::string KX_Mesh::GetName()
{
	return RAS_Mesh::GetName();
}

PyObject *KX_Mesh::PyGetMaterialName(PyObject *args, PyObject *kwds)
{
	int matid = 1;
	std::string matname;

	if (PyArg_ParseTuple(args, "i:getMaterialName", &matid)) {
		matname = GetMaterialName(matid);
	}
	else {
		return nullptr;
	}

	return PyUnicode_FromStdString(matname);
}

PyObject *KX_Mesh::PyGetTextureName(PyObject *args, PyObject *kwds)
{
	int matid = 1;
	std::string matname;

	if (PyArg_ParseTuple(args, "i:getTextureName", &matid)) {
		matname = GetTextureName(matid);
	}
	else {
		return nullptr;
	}

	return PyUnicode_FromStdString(matname);
}

PyObject *KX_Mesh::PyGetVertexArrayLength(PyObject *args, PyObject *kwds)
{
	int matid = 0;
	int length = 0;

	if (!PyArg_ParseTuple(args, "i:getVertexArrayLength", &matid))
		return nullptr;

	RAS_IDisplayArray *array = GetDisplayArray(matid);
	if (array) {
		length = array->GetVertexCount();
	}

	return PyLong_FromLong(length);
}

PyObject *KX_Mesh::PyGetVertex(PyObject *args, PyObject *kwds)
{
	int vertexindex;
	int matindex;

	if (!PyArg_ParseTuple(args, "ii:getVertex", &matindex, &vertexindex))
		return nullptr;

	RAS_IDisplayArray *array = GetDisplayArray(matindex);
	if (vertexindex < 0 || vertexindex >= array->GetVertexCount()) {
		PyErr_SetString(PyExc_ValueError, "mesh.getVertex(mat_idx, vert_idx): KX_Mesh, could not get a vertex at the given indices");
		return nullptr;
	}

	RAS_Vertex vertex = array->GetVertex(vertexindex);

	return (new KX_VertexProxy(array, vertex))->NewProxy(true);
}

PyObject *KX_Mesh::PyGetPolygon(PyObject *args, PyObject *kwds)
{
	int polyindex = 1;

	if (!PyArg_ParseTuple(args, "i:getPolygon", &polyindex))
		return nullptr;

	if (polyindex < 0 || polyindex >= m_numPolygons) {
		PyErr_SetString(PyExc_AttributeError, "mesh.getPolygon(int): KX_Mesh, invalid polygon index");
		return nullptr;
	}

	const RAS_Mesh::PolygonInfo polygon = GetPolygon(polyindex);
	KX_PolyProxy *polyProxy = new KX_PolyProxy(this, polygon);
	return polyProxy->NewProxy(true);
}

PyObject *KX_Mesh::PyTransform(PyObject *args, PyObject *kwds)
{
	int matindex;
	PyObject *pymat;
	bool ok = false;

	mt::mat4 transform;

	if (!PyArg_ParseTuple(args, "iO:transform", &matindex, &pymat) ||
	    !PyMatTo(pymat, transform))
	{
		return nullptr;
	}

	mt::mat4 ntransform = transform;
	ntransform(0, 3) = ntransform(1, 3) = ntransform(2, 3) = 0.0f;

	/* transform mesh verts */
	for (unsigned short i = 0, num = m_materials.size(); i < num; ++i) {
		if (matindex == -1) {
			/* always transform */
		}
		else if (matindex == i) {
			/* we found the right index! */
		}
		else {
			continue;
		}

		RAS_IDisplayArray *array = m_materials[i]->GetDisplayArray();
		ok = true;

		for (unsigned int j = 0, size = array->GetVertexCount(); j < size; ++j) {
			RAS_Vertex vert = array->GetVertex(j);
			vert.Transform(transform, ntransform);
		}

		array->NotifyUpdate(RAS_IDisplayArray::POSITION_MODIFIED |
							RAS_IDisplayArray::NORMAL_MODIFIED |
							RAS_IDisplayArray::TANGENT_MODIFIED);

		/* if we set a material index, quit when done */
		if (matindex != -1) {
			break;
		}
	}

	if (ok == false) {
		PyErr_Format(PyExc_ValueError,
		             "mesh.transform(...): invalid material index %d", matindex);
		return nullptr;
	}

	Py_RETURN_NONE;
}

PyObject *KX_Mesh::PyTransformUV(PyObject *args, PyObject *kwds)
{
	int matindex;
	PyObject *pymat;
	int uvindex = -1;
	int uvindex_from = -1;
	bool ok = false;

	mt::mat4 transform;

	if (!PyArg_ParseTuple(args, "iO|iii:transformUV", &matindex, &pymat, &uvindex, &uvindex_from) ||
	    !PyMatTo(pymat, transform))
	{
		return nullptr;
	}

	if (uvindex < -1 || uvindex > RAS_Texture::MaxUnits) {
		PyErr_Format(PyExc_ValueError,
		             "mesh.transformUV(...): invalid uv_index %d", uvindex);
		return nullptr;
	}
	if (uvindex_from < -1 || uvindex_from > RAS_Texture::MaxUnits) {
		PyErr_Format(PyExc_ValueError,
		             "mesh.transformUV(...): invalid uv_index_from %d", uvindex);
		return nullptr;
	}
	if (uvindex_from == uvindex) {
		uvindex_from = -1;
	}

	/* transform mesh verts */
	for (unsigned short i = 0, num = m_materials.size(); i < num; ++i) {
		if (matindex == -1) {
			/* always transform */
		}
		else if (matindex == i) {
			/* we found the right index! */
		}
		else {
			continue;
		}

		RAS_IDisplayArray *array = m_materials[i]->GetDisplayArray();
		ok = true;

		for (unsigned int j = 0, size = array->GetVertexCount(); j < size; ++j) {
			RAS_Vertex vert = array->GetVertex(j);
			if (uvindex_from != -1) {
				vert.SetUV(uvindex, vert.GetUv(uvindex_from));
			}

			if (uvindex >= 0) {
				vert.TransformUv(uvindex, transform);
			}
			else if (uvindex == -1) {
				for (int k = 0; k < RAS_Texture::MaxUnits; ++k) {
					vert.TransformUv(k, transform);
				}
			}
		}

		array->NotifyUpdate(RAS_IDisplayArray::UVS_MODIFIED);

		/* if we set a material index, quit when done */
		if (matindex != -1) {
			break;
		}
	}

	if (ok == false) {
		PyErr_Format(PyExc_ValueError,
		             "mesh.transformUV(...): invalid material index %d", matindex);
		return nullptr;
	}

	Py_RETURN_NONE;
}

PyObject *KX_Mesh::PyReplaceMaterial(PyObject *args, PyObject *kwds)
{
	unsigned short matindex;
	PyObject *pymat;
	KX_BlenderMaterial *mat;

	if (!PyArg_ParseTuple(args, "hO:replaceMaterial", &matindex, &pymat) ||
		!ConvertPythonToMaterial(pymat, &mat, false, "mesh.replaceMaterial(...): invalid material")) {
		return nullptr;
	}


	RAS_MeshMaterial *meshmat = GetMeshMaterial(matindex);
	if (!meshmat) {
		PyErr_Format(PyExc_ValueError, "Invalid material index %d", matindex);
		return nullptr;
	}

	KX_Scene *scene = (KX_Scene *)meshmat->GetBucket()->GetPolyMaterial()->GetScene();
	if (scene != mat->GetScene()) {
		PyErr_Format(PyExc_ValueError, "Mesh successor scene doesn't match current mesh scene");
		return nullptr;
	}

	RAS_BucketManager *bucketmgr = scene->GetBucketManager();
	bool created = false;
	RAS_MaterialBucket *bucket = bucketmgr->FindBucket(mat, created);

	// Must never create the material bucket.
	BLI_assert(created == false);

	meshmat->ReplaceMaterial(bucket);

	Py_RETURN_NONE;
}

PyObject *KX_Mesh::PyCopy()
{
	KX_Mesh *dupli = new KX_Mesh(*this);
	// Create bounding box.
	dupli->EndConversion(m_scene->GetBoundingBoxManager());

	// Transfer ownership to converter.
	KX_GetActiveEngine()->GetConverter()->RegisterMesh(m_scene, dupli);

	return dupli->GetProxy();
}

PyObject *KX_Mesh::pyattr_get_materials(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_Mesh *self = static_cast<KX_Mesh *>(self_v);

	const unsigned short tot = self->m_materials.size();

	PyObject *materials = PyList_New(tot);

	for (unsigned short i = 0; i < tot; ++i) {
		RAS_MeshMaterial *mmat = self->m_materials[i];
		RAS_IPolyMaterial *polymat = mmat->GetBucket()->GetPolyMaterial();
		KX_BlenderMaterial *mat = static_cast<KX_BlenderMaterial *>(polymat);
		PyList_SET_ITEM(materials, i, mat->GetProxy());
	}
	return materials;
}

PyObject *KX_Mesh::pyattr_get_numMaterials(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_Mesh *self = static_cast<KX_Mesh *> (self_v);
	return PyLong_FromLong(self->m_materials.size());
}

PyObject *KX_Mesh::pyattr_get_numPolygons(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_Mesh *self = static_cast<KX_Mesh *> (self_v);
	return PyLong_FromLong(self->m_numPolygons);
}

static int kx_mesh_proxy_get_polygons_size_cb(void *self_v)
{
	return ((KX_Mesh *)self_v)->GetNumPolygons();
}

static PyObject *kx_mesh_proxy_get_polygons_item_cb(void *self_v, int index)
{
	KX_Mesh *self = static_cast<KX_Mesh *>(self_v);
	const RAS_Mesh::PolygonInfo polygon = self->GetPolygon(index);

	KX_PolyProxy *polyProxy = new KX_PolyProxy(self, polygon);
	return polyProxy->NewProxy(true);
}

PyObject *KX_Mesh::pyattr_get_polygons(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	return (new EXP_ListWrapper(self_v,
		((KX_Mesh *)self_v)->GetProxy(),
		nullptr,
		kx_mesh_proxy_get_polygons_size_cb,
		kx_mesh_proxy_get_polygons_item_cb,
		nullptr,
		nullptr))->NewProxy(true);
}

/* a close copy of ConvertPythonToGameObject but for meshes */
bool ConvertPythonToMesh(SCA_LogicManager *logicmgr, PyObject *value, KX_Mesh **object, bool py_none_ok, const char *error_prefix)
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
			PyErr_Format(PyExc_TypeError, "%s, expected KX_Mesh or a KX_Mesh name, None is invalid", error_prefix);
			return false;
		}
	}

	if (PyUnicode_Check(value)) {
		*object = (KX_Mesh *)logicmgr->GetMeshByName(std::string(_PyUnicode_AsString(value)));
		
		if (*object) {
			return true;
		}
		else {
			PyErr_Format(PyExc_ValueError, "%s, requested name \"%s\" did not match any KX_Mesh in this scene", error_prefix, _PyUnicode_AsString(value));
			return false;
		}
	}

	if (PyObject_TypeCheck(value, &KX_Mesh::Type)) {
		KX_Mesh *kx_mesh = static_cast<KX_Mesh *>EXP_PROXY_REF(value);

		/* sets the error */
		if (kx_mesh == nullptr) {
			PyErr_Format(PyExc_SystemError, "%s, " EXP_PROXY_ERROR_MSG, error_prefix);
			return false;
		}

		*object = kx_mesh;
		return true;
	}

	*object = nullptr;

	if (py_none_ok) {
		PyErr_Format(PyExc_TypeError, "%s, expect a KX_Mesh, a string or None", error_prefix);
	}
	else {
		PyErr_Format(PyExc_TypeError, "%s, expect a KX_Mesh or a string", error_prefix);
	}

	return false;
}

#endif // WITH_PYTHON
