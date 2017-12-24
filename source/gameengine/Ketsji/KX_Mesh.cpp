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

#include "RAS_IMaterial.h"
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

#include "BLI_kdopbvh.h"
#include "BLI_compiler_compat.h" // For MSVC __func__

#include "MEM_guardedalloc.h"

extern "C" {
#  include "mathutils_bvhtree.h"
}

KX_Mesh::KX_Mesh(KX_Scene *scene, Mesh *mesh, const RAS_Mesh::LayersInfo& layersInfo)
	:RAS_Mesh(mesh, layersInfo),
	m_scene(scene)
{
}

KX_Mesh::KX_Mesh(KX_Scene *scene, const std::string& name, const RAS_Mesh::LayersInfo& layersInfo)
	:RAS_Mesh(name, layersInfo),
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
	{"getMaterialName", (PyCFunction)KX_Mesh::sPyGetMaterialName, METH_VARARGS},
	{"getTextureName", (PyCFunction)KX_Mesh::sPyGetTextureName, METH_VARARGS},
	{"getVertexArrayLength", (PyCFunction)KX_Mesh::sPyGetVertexArrayLength, METH_VARARGS},
	{"getVertex", (PyCFunction)KX_Mesh::sPyGetVertex, METH_VARARGS},
	{"getPolygon", (PyCFunction)KX_Mesh::sPyGetPolygon, METH_VARARGS},
	{"transform", (PyCFunction)KX_Mesh::sPyTransform, METH_VARARGS},
	{"transformUV", (PyCFunction)KX_Mesh::sPyTransformUV, METH_VARARGS},
	{"replaceMaterial", (PyCFunction)KX_Mesh::sPyReplaceMaterial, METH_VARARGS},
	{"copy", (PyCFunction)KX_Mesh::sPyCopy, METH_NOARGS},
	{"constructBvh", (PyCFunction)KX_Mesh::sPyConstructBvh, METH_VARARGS | METH_KEYWORDS},
	{"destruct", (PyCFunction) KX_Mesh::sPyDestruct, METH_NOARGS},
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

	if (!PyArg_ParseTuple(args, "i:getVertexArrayLength", &matid)) {
		return nullptr;
	}

	RAS_DisplayArray *array = GetDisplayArray(matid);
	if (array) {
		length = array->GetVertexCount();
	}

	return PyLong_FromLong(length);
}

PyObject *KX_Mesh::PyGetVertex(PyObject *args, PyObject *kwds)
{
	int vertexindex;
	int matindex;

	if (!PyArg_ParseTuple(args, "ii:getVertex", &matindex, &vertexindex)) {
		return nullptr;
	}

	RAS_DisplayArray *array = GetDisplayArray(matindex);
	if (vertexindex < 0 || vertexindex >= array->GetVertexCount()) {
		PyErr_SetString(PyExc_ValueError, "mesh.getVertex(mat_idx, vert_idx): KX_Mesh, could not get a vertex at the given indices");
		return nullptr;
	}

	return (new KX_VertexProxy(array, vertexindex))->NewProxy(true);
}

PyObject *KX_Mesh::PyGetPolygon(PyObject *args, PyObject *kwds)
{
	int polyindex = 1;

	if (!PyArg_ParseTuple(args, "i:getPolygon", &polyindex)) {
		return nullptr;
	}

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
	    !PyMatTo(pymat, transform)) {
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

		RAS_DisplayArray *array = m_materials[i]->GetDisplayArray();
		ok = true;

		const unsigned int vertexCount = array->GetVertexCount();
		for (unsigned int j = 0; j < vertexCount; ++j) {
			array->SetPosition(j, transform * mt::vec3(array->GetPosition(j)));
			array->SetNormal(j, ntransform * mt::vec3(array->GetNormal(j)));
			array->SetTangent(j, ntransform * mt::vec4(array->GetTangent(j)));
		}

		array->NotifyUpdate(RAS_DisplayArray::POSITION_MODIFIED |
		                    RAS_DisplayArray::NORMAL_MODIFIED |
		                    RAS_DisplayArray::TANGENT_MODIFIED);

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
	    !PyMatTo(pymat, transform)) {
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

		RAS_DisplayArray *array = m_materials[i]->GetDisplayArray();
		const RAS_DisplayArray::Format& format = array->GetFormat();
		ok = true;

		for (unsigned int j = 0, size = array->GetVertexCount(); j < size; ++j) {
			if (uvindex_from != -1) {
				array->SetUv(j, uvindex, array->GetUv(j, uvindex_from));
			}

			if (0 <= uvindex && uvindex < format.uvSize) {
				const mt::vec2_packed& uv = array->GetUv(j, uvindex);
				array->SetUv(j, uvindex, (transform * mt::vec3(uv.x, uv.y, 0.0f)).xy());
			}
			else if (uvindex == -1) {
				for (unsigned short k = 0; k < format.uvSize; ++k) {
					const mt::vec2_packed& uv = array->GetUv(j, k);
					array->SetUv(j, k, (transform * mt::vec3(uv.x, uv.y, 0.0f)).xy());
				}
			}
		}

		array->NotifyUpdate(RAS_DisplayArray::UVS_MODIFIED);

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

	KX_Scene *scene = (KX_Scene *)meshmat->GetBucket()->GetMaterial()->GetScene();
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

PyObject *KX_Mesh::PyDestruct()
{
	// Transfer ownership to converter.
	KX_GetActiveEngine()->GetConverter()->UnregisterMesh(m_scene, this);

	// Here the mesh is freed.

	Py_RETURN_NONE;
}

PyObject *KX_Mesh::PyConstructBvh(PyObject *args, PyObject *kwds)
{
	float epsilon = 0.0f;
	PyObject *pymat = nullptr;

	if (!EXP_ParseTupleArgsAndKeywords(args, kwds, "|Of:constructBvh", {"transform", "epsilon", 0}, &pymat, &epsilon)) {
		return nullptr;
	}

	mt::mat4 mat = mt::mat4::Identity();
	if (pymat && !PyMatTo(pymat, mat)) {
		return nullptr;
	}

	BVHTree *tree = BLI_bvhtree_new(m_numPolygons, epsilon, 4, 6);

	unsigned int numVert = 0;
	// Compute the totale number of vertices.
	for (const PolygonRangeInfo& range : m_polygonRanges) {
		numVert += range.array->GetVertexCount();
	}
	
	const char *function_macro = __func__; //Workaround for MSVC2015
	float (*coords)[3] = (float (*)[3])MEM_mallocN(sizeof(float[3]) * numVert, function_macro);
	// Convert the vertices.
	{
		unsigned vertBase = 0;
		for (const PolygonRangeInfo& range : m_polygonRanges) {
			RAS_DisplayArray *array = range.array;
			for (unsigned int i = 0, size = array->GetVertexCount(); i < size; ++i) {
				const mt::vec3 pos = mat * mt::vec3(array->GetPosition(i));
				pos.Pack(coords[vertBase + i]);
			}
			vertBase += array->GetVertexCount();
		}
	}

	unsigned int *tris = (unsigned int *)MEM_mallocN(sizeof(unsigned int) * 3 * m_numPolygons, __func__);
	// Convert the indices.
	{
		unsigned int index = 0;
		unsigned int vertBase = 0;
		for (const PolygonRangeInfo& range : m_polygonRanges) {
			// Iterate by triangle (3 indices).
			for (; index < range.endIndex; index += 3) {
				// Get the relative triangle base index.
				const unsigned int triBase = index - range.startIndex;
				float co[3][3];

				for (unsigned short i = 0; i < 3; ++i) {
					// Get the absolute the vertex index.
					const unsigned int vertIndex = vertBase + range.array->GetTriangleIndex(triBase + i);

					tris[index + i] = vertIndex;
					copy_v3_v3(co[i], coords[vertIndex]);
				}

				BLI_bvhtree_insert(tree, index / 3, co[0], 3);
			}
			vertBase += range.array->GetVertexCount();
		}
	}

	BLI_bvhtree_balance(tree);

	return bvhtree_CreatePyObject(
		tree, epsilon,
		coords, numVert,
		(unsigned int (*)[3])tris, m_numPolygons * 3,
		nullptr, nullptr);
}

PyObject *KX_Mesh::pyattr_get_materials(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_Mesh *self = static_cast<KX_Mesh *>(self_v);

	const unsigned short tot = self->m_materials.size();

	PyObject *materials = PyList_New(tot);

	for (unsigned short i = 0; i < tot; ++i) {
		RAS_MeshMaterial *mmat = self->m_materials[i];
		KX_BlenderMaterial *mat = static_cast<KX_BlenderMaterial *>(mmat->GetBucket()->GetMaterial());
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

unsigned int KX_Mesh::py_get_polygons_size()
{
	return m_numPolygons;
}

PyObject *KX_Mesh::py_get_polygons_item(unsigned int index)
{
	const RAS_Mesh::PolygonInfo polygon = GetPolygon(index);

	KX_PolyProxy *polyProxy = new KX_PolyProxy(this, polygon);
	return polyProxy->NewProxy(true);
}

PyObject *KX_Mesh::pyattr_get_polygons(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	return (new EXP_ListWrapper<KX_Mesh, &KX_Mesh::py_get_polygons_size, &KX_Mesh::py_get_polygons_item>(self_v))->NewProxy(true);
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
