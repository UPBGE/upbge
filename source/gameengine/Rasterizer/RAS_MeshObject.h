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

/** \file RAS_MeshObject.h
 *  \ingroup bgerast
 */

#ifndef __RAS_MESHOBJECT_H__
#define __RAS_MESHOBJECT_H__

#ifdef _MSC_VER
/* disable the STL warnings ("debug information length > 255") */
#  pragma warning (disable:4786)
#endif

#include <vector>
#include <list>

#include "RAS_MaterialBucket.h"
#include "MT_Transform.h"
#include "STR_String.h"

struct Mesh;
class RAS_Deformer;
class RAS_Polygon;

/* RAS_MeshObject is a mesh used for rendering. It stores polygons,
 * but the actual vertices and index arrays are stored in material
 * buckets, referenced by the list of RAS_MeshMaterials. */

class RAS_MeshObject
{
private:
	bool m_bMeshModified;
	bool m_aabbModified;
	MT_Point3 m_aabbMax;
	MT_Point3 m_aabbMin;

	STR_String m_name;
	static STR_String s_emptyname;

	vector<RAS_Polygon *> m_Polygons;

	/* polygon sorting */
	struct polygonSlot;
	struct backtofront;
	struct fronttoback;

	void UpdateAabb();

protected:
	vector<int> m_cacheWeightIndex;
	list<RAS_MeshMaterial> m_materials;
	Mesh *m_mesh;

public:
	// for now, meshes need to be in a certain layer (to avoid sorting on lights in realtime)
	RAS_MeshObject(Mesh *mesh);
	virtual ~RAS_MeshObject();

	// materials
	int NumMaterials();
	const STR_String& GetMaterialName(unsigned int matid);
	const STR_String& GetTextureName(unsigned int matid);

	RAS_MeshMaterial *GetMeshMaterial(unsigned int matid);
	RAS_MeshMaterial *GetMeshMaterial(RAS_IPolyMaterial *mat);
	/// Return the material position in the mesh, like in blender.
	int GetBlenderMaterialId(RAS_IPolyMaterial *mat);

	list<RAS_MeshMaterial>::iterator GetFirstMaterial();
	list<RAS_MeshMaterial>::iterator GetLastMaterial();

	// name
	void SetName(const char *name);
	STR_String& GetName();

	// modification state
	bool MeshModified();
	void SetMeshModified(bool v)
	{
		m_bMeshModified = v;
	}

	bool AabbModfified() const
	{
		return m_aabbModified;
	}
	void SetAabbModified(bool v)
	{
		m_aabbModified = v;
	}

	// original blender mesh
	Mesh *GetMesh()
	{
		return m_mesh;
	}

	// mesh construction
	void AddMaterial(RAS_MaterialBucket *bucket, unsigned int index);
	virtual RAS_Polygon *AddPolygon(RAS_MaterialBucket *bucket, int numverts);
	virtual void AddVertex(RAS_Polygon *poly, int i,
						   const MT_Point3& xyz,
						   const MT_Point2 uvs[RAS_TexVert::MAX_UNIT],
						   const MT_Vector4& tangent,
						   const unsigned int rgbacolor,
						   const MT_Vector3& normal,
						   bool flat,
						   int origindex);

	// vertex and polygon acces
	int NumVertices(RAS_IPolyMaterial *mat);
	RAS_TexVert *GetVertex(unsigned int matid, unsigned int index);
	const float *GetVertexLocation(unsigned int orig_index);

	int NumPolygons();
	RAS_Polygon *GetPolygon(int num) const;

	// buckets
	virtual void AddMeshUser(void *clientobj, RAS_MeshSlotList& meshslots, RAS_Deformer *deformer);

	void RemoveFromBuckets(void *clientobj);
	void EndConversion();

	// colors
	void SetVertexColor(RAS_IPolyMaterial *mat, MT_Vector4 rgba);

	// polygon sorting by Z for alpha
	void SortPolygons(RAS_MeshSlot& ms, const MT_Transform &transform);

	bool HasColliderPolygon();

	void GetAabb(MT_Point3 &aabbMin, MT_Point3 &aabbMax);

	// for construction to find shared vertices
	struct SharedVertex
	{
		RAS_DisplayArray *m_darray;
		int m_offset;
	};

	vector<vector<SharedVertex> > m_sharedvertex_map;


#ifdef WITH_CXX_GUARDEDALLOC
	MEM_CXX_CLASS_ALLOC_FUNCS("GE:RAS_MeshObject")
#endif
};

#endif  // __RAS_MESHOBJECT_H__
