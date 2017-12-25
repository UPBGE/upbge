#ifndef __KX_MESH_BUILDER_H__
#define __KX_MESH_BUILDER_H__

#include "KX_Mesh.h"
#include "RAS_DisplayArray.h"

#include "EXP_ListValue.h"

class KX_BlenderMaterial;

class KX_MeshBuilderSlot : public EXP_Value
{
	Py_Header

private:
	KX_BlenderMaterial *m_material;
	RAS_DisplayArray *m_array;
	unsigned int& m_origIndexCounter;

public:
	KX_MeshBuilderSlot(KX_BlenderMaterial *material, RAS_DisplayArray::PrimitiveType primitiveType,
			const RAS_DisplayArray::Format& format, unsigned int& origIndexCounter);
	~KX_MeshBuilderSlot();

	virtual std::string GetName();

	KX_BlenderMaterial *GetMaterial() const;
	void SetMaterial(KX_BlenderMaterial *material);

	/// Return true if the number of vertices or indices doesn't match the primitive type used.
	bool Invalid() const;

	RAS_DisplayArray *GetDisplayArray() const;

#ifdef WITH_PYTHON

	using GetSizeFunc = unsigned int (RAS_DisplayArray::*)() const;
	using GetIndexFunc = unsigned int (RAS_DisplayArray::*)(const unsigned int) const;

	template <GetSizeFunc Func>
	unsigned int get_size_cb();
	PyObject *get_item_vertices_cb(unsigned int index);
	template <GetIndexFunc Func>
	PyObject *get_item_indices_cb(unsigned int index);

	static PyObject *pyattr_get_vertices(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_indices(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_triangleIndices(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_material(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_material(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_uvCount(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_colorCount(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_primitive(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);

	EXP_PYMETHOD(KX_MeshBuilderSlot, AddVertex);
	EXP_PYMETHOD_VARARGS(KX_MeshBuilderSlot, RemoveVertex);
	EXP_PYMETHOD_O(KX_MeshBuilderSlot, AddIndex);
	EXP_PYMETHOD_O(KX_MeshBuilderSlot, AddPrimitiveIndex);
	EXP_PYMETHOD_VARARGS(KX_MeshBuilderSlot, RemovePrimitiveIndex);
	EXP_PYMETHOD_O(KX_MeshBuilderSlot, AddTriangleIndex);
	EXP_PYMETHOD_VARARGS(KX_MeshBuilderSlot, RemoveTriangleIndex);
	EXP_PYMETHOD_NOARGS(KX_MeshBuilderSlot, RecalculateNormals);

#endif  // WITH_PYTHON
};

class KX_MeshBuilder : public EXP_Value
{
	Py_Header

private:
	std::string m_name;

	EXP_ListValue<KX_MeshBuilderSlot> m_slots;
	RAS_Mesh::LayersInfo m_layersInfo;
	RAS_DisplayArray::Format m_format;

	KX_Scene *m_scene;

	/// Counter shared by all the slots to compute the original index of a new added vertex.
	unsigned int m_origIndexCounter;

public:
	KX_MeshBuilder(const std::string& name, KX_Scene *scene, const RAS_Mesh::LayersInfo& layersInfo,
			const RAS_DisplayArray::Format& format);
	~KX_MeshBuilder();

	virtual std::string GetName();

	EXP_ListValue<KX_MeshBuilderSlot>& GetSlots();

#ifdef WITH_PYTHON

	static PyObject *pyattr_get_slots(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);

	EXP_PYMETHOD(KX_MeshBuilder, AddMaterial);
	EXP_PYMETHOD_NOARGS(KX_MeshBuilder, Finish);

#endif  // WITH_PYTHON
};

#endif  // __KX_MESH_BUILDER_H__
