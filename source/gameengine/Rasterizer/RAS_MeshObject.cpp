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

/** \file gameengine/Rasterizer/RAS_MeshObject.cpp
 *  \ingroup bgerast
 */

#include "DNA_key_types.h"
#include "DNA_mesh_types.h"

#include "RAS_MeshObject.h"
#include "RAS_MeshUser.h"
#include "RAS_Polygon.h"
#include "RAS_IPolygonMaterial.h"
#include "RAS_DisplayArray.h"
#include "RAS_Deformer.h"
#include "MT_Vector3.h"

#include <algorithm>

// polygon sorting

struct RAS_MeshObject::polygonSlot
{
	float m_z;
	int m_index[4];

	polygonSlot()
	{
	}

	/* pnorm is the normal from the plane equation that the distance from is
	 * used to sort again. */
	void get(const RAS_TexVert *vertexarray, const unsigned int *indexarray,
	         int offset, int nvert, const MT_Vector3& pnorm)
	{
		MT_Vector3 center(0.0f, 0.0f, 0.0f);
		int i;

		for (i = 0; i < nvert; i++) {
			m_index[i] = indexarray[offset + i];
			center += vertexarray[m_index[i]].getXYZ();
		}

		/* note we don't divide center by the number of vertices, since all
		* polygons have the same number of vertices, and that we leave out
		* the 4-th component of the plane equation since it is constant. */
		m_z = MT_dot(pnorm, center);
	}

	void set(unsigned int *indexarray, int offset, int nvert)
	{
		int i;

		for (i = 0; i < nvert; i++)
			indexarray[offset + i] = m_index[i];
	}
};

struct RAS_MeshObject::backtofront
{
	bool operator()(const polygonSlot &a, const polygonSlot &b) const
	{
		return a.m_z < b.m_z;
	}
};

struct RAS_MeshObject::fronttoback
{
	bool operator()(const polygonSlot &a, const polygonSlot &b) const
	{
		return a.m_z > b.m_z;
	}
};

// mesh object

STR_String RAS_MeshObject::s_emptyname = "";

RAS_MeshObject::RAS_MeshObject(Mesh *mesh)
	:m_modifiedFlag(MESH_MODIFIED),
	m_needUpdateAabb(true),
	m_mesh(mesh)
{
	if (m_mesh && m_mesh->key) {
		KeyBlock *kb;
		int count = 0;
		// initialize weight cache for shape objects
		// count how many keys in this mesh
		for (kb = (KeyBlock *)m_mesh->key->block.first; kb; kb = (KeyBlock *)kb->next)
			count++;
		m_cacheWeightIndex.resize(count, -1);
	}
}

RAS_MeshObject::~RAS_MeshObject()
{
	std::vector<RAS_Polygon *>::iterator it;

	for (it = m_Polygons.begin(); it != m_Polygons.end(); it++)
		delete (*it);

	m_sharedvertex_map.clear();
	m_Polygons.clear();
	m_materials.clear();
}

void RAS_MeshObject::UpdateAabb()
{
	bool first = true;
	unsigned int nmat = NumMaterials();
	for (unsigned int imat = 0; imat < nmat; ++imat) {
		RAS_MeshMaterial *mmat = GetMeshMaterial(imat);

		RAS_MeshSlot *slot = mmat->m_baseslot;
		if (!slot)
			continue;

		RAS_DisplayArray *array = slot->GetDisplayArray();
		// for each vertex
		for (unsigned int i = 0; i < array->m_vertex.size(); ++i) {
			RAS_TexVert& v = array->m_vertex[i];
			MT_Vector3 vertpos = v.xyz();

			// For the first vertex of the mesh, only initialize AABB.
			if (first) {
				m_aabbMin = m_aabbMax = vertpos;
				first = false;
			}
			else {
				m_aabbMin.x() = std::min(m_aabbMin.x(), vertpos.x());
				m_aabbMin.y() = std::min(m_aabbMin.y(), vertpos.y());
				m_aabbMin.z() = std::min(m_aabbMin.z(), vertpos.z());
				m_aabbMax.x() = std::max(m_aabbMax.x(), vertpos.x());
				m_aabbMax.y() = std::max(m_aabbMax.y(), vertpos.y());
				m_aabbMax.z() = std::max(m_aabbMax.z(), vertpos.z());
			}
		}
	}
}

void RAS_MeshObject::GetAabb(MT_Vector3 &aabbMin, MT_Vector3 &aabbMax)
{
	if (m_needUpdateAabb) {
		UpdateAabb();
		m_needUpdateAabb = false;
	}

	aabbMin = m_aabbMin;
	aabbMax = m_aabbMax;
}

int RAS_MeshObject::NumMaterials()
{
	return m_materials.size();
}

const STR_String& RAS_MeshObject::GetMaterialName(unsigned int matid)
{
	RAS_MeshMaterial *mmat = GetMeshMaterial(matid);

	if (mmat)
		return mmat->m_bucket->GetPolyMaterial()->GetMaterialName();

	return s_emptyname;
}

RAS_MeshMaterial *RAS_MeshObject::GetMeshMaterial(unsigned int matid)
{
	if ((m_materials.empty() == false) && (matid < m_materials.size())) {
		std::list<RAS_MeshMaterial>::iterator it = m_materials.begin();
		while (matid--) {
			++it;
		}
		return &*it;
	}

	return NULL;
}

int RAS_MeshObject::NumPolygons()
{
	return m_Polygons.size();
}

RAS_Polygon *RAS_MeshObject::GetPolygon(int num) const
{
	return m_Polygons[num];
}

std::list<RAS_MeshMaterial>::iterator RAS_MeshObject::GetFirstMaterial()
{
	return m_materials.begin();
}

std::list<RAS_MeshMaterial>::iterator RAS_MeshObject::GetLastMaterial()
{
	return m_materials.end();
}

void RAS_MeshObject::SetName(const char *name)
{
	m_name = name;
}

STR_String& RAS_MeshObject::GetName()
{
	return m_name;
}

short RAS_MeshObject::GetModifiedFlag() const
{
	return m_modifiedFlag;
}

void RAS_MeshObject::AppendModifiedFlag(short flag)
{
	SetModifiedFlag(m_modifiedFlag | flag);
}

void RAS_MeshObject::SetModifiedFlag(short flag)
{
	m_modifiedFlag = flag;
	if (m_modifiedFlag & AABB_MODIFIED) {
		m_needUpdateAabb = true;
	}
}

const STR_String& RAS_MeshObject::GetTextureName(unsigned int matid)
{
	RAS_MeshMaterial *mmat = GetMeshMaterial(matid);

	if (mmat)
		return mmat->m_bucket->GetPolyMaterial()->GetTextureName();

	return s_emptyname;
}

RAS_MeshMaterial *RAS_MeshObject::GetMeshMaterial(RAS_IPolyMaterial *mat)
{
	// find a mesh material
	for (std::list<RAS_MeshMaterial>::iterator mit = m_materials.begin(); mit != m_materials.end(); mit++) {
		if (mit->m_bucket->GetPolyMaterial() == mat)
			return &*mit;
	}

	return NULL;
}

int RAS_MeshObject::GetBlenderMaterialId(RAS_IPolyMaterial *mat)
{
	// find a mesh material
	for (std::list<RAS_MeshMaterial>::iterator mit = m_materials.begin(); mit != m_materials.end(); ++mit) {
		if (mit->m_bucket->GetPolyMaterial() == mat)
			return mit->m_index;
	}

	return -1;
}

void RAS_MeshObject::AddMaterial(RAS_MaterialBucket *bucket, unsigned int index)
{
	RAS_MeshMaterial *mmat = GetMeshMaterial(bucket->GetPolyMaterial());

	// none found, create a new one
	if (!mmat) {
		RAS_MeshMaterial meshmat;
		meshmat.m_bucket = bucket;
		meshmat.m_baseslot = meshmat.m_bucket->AddMesh(this);
		meshmat.m_index = index;
		m_materials.push_back(meshmat);
	}
}

void RAS_MeshObject::AddLine(RAS_MaterialBucket *bucket, unsigned int v1, unsigned int v2)
{
	// find a mesh material
	RAS_MeshMaterial *mmat = GetMeshMaterial(bucket->GetPolyMaterial());
	// add it to the bucket, this also adds new display arrays
	RAS_MeshSlot *slot = mmat->m_baseslot;

	// create a new polygon
	RAS_DisplayArray *darray = slot->GetDisplayArray();
	darray->m_type = RAS_DisplayArray::LINES;
	slot->AddPolygonVertex(v1);
	slot->AddPolygonVertex(v2);
}

RAS_Polygon *RAS_MeshObject::AddPolygon(RAS_MaterialBucket *bucket, int numverts, unsigned int indices[4],
										bool visible, bool collider, bool twoside)
{
	// find a mesh material
	RAS_MeshMaterial *mmat = GetMeshMaterial(bucket->GetPolyMaterial());
	// add it to the bucket, this also adds new display arrays
	RAS_MeshSlot *slot = mmat->m_baseslot;

	// create a new polygon
	RAS_DisplayArray *darray = slot->GetDisplayArray();
	RAS_Polygon *poly = new RAS_Polygon(bucket, darray, numverts);
	m_Polygons.push_back(poly);

	poly->SetVisible(visible);
	poly->SetCollider(collider);
	poly->SetTwoside(twoside);

	if (visible && !bucket->IsWire()) {
		// Add the first triangle.
		slot->AddPolygonVertex(indices[0]);
		slot->AddPolygonVertex(indices[1]);
		slot->AddPolygonVertex(indices[2]);

		poly->SetVertexOffset(0, indices[0]);
		poly->SetVertexOffset(1, indices[1]);
		poly->SetVertexOffset(2, indices[2]);

		if (numverts == 4) {
			// Add the second triangle.
			slot->AddPolygonVertex(indices[0]);
			slot->AddPolygonVertex(indices[2]);
			slot->AddPolygonVertex(indices[3]);

			poly->SetVertexOffset(3, indices[3]);
		}
	}

	return poly;
}

void RAS_MeshObject::SetVertexColor(RAS_IPolyMaterial *mat, MT_Vector4 rgba)
{
	RAS_MeshMaterial *mmat = GetMeshMaterial(mat);
	RAS_MeshSlot *slot = mmat->m_baseslot;
	RAS_DisplayArray *array = slot->GetDisplayArray();

	for (unsigned int i = 0; i < array->m_vertex.size(); i++) {
		array->m_vertex[i].SetRGBA(rgba);
	}
}

unsigned int RAS_MeshObject::AddVertex(RAS_MaterialBucket *bucket, int i,
                               const MT_Vector3& xyz,
                               const MT_Vector2 uvs[RAS_TexVert::MAX_UNIT],
                               const MT_Vector4& tangent,
                               const unsigned int rgba,
                               const MT_Vector3& normal,
                               bool flat,
                               int origindex)
{
	RAS_TexVert texvert(xyz, uvs, tangent, rgba, normal, flat, origindex);

	RAS_MeshMaterial *mmat = GetMeshMaterial(bucket->GetPolyMaterial());
	RAS_MeshSlot *slot = mmat->m_baseslot;
	RAS_DisplayArray *darray = slot->GetDisplayArray();

	{	/* Shared Vertex! */
		/* find vertices shared between faces, with the restriction
		 * that they exist in the same display array, and have the
		 * same uv coordinate etc */
		std::vector<SharedVertex>& sharedmap = m_sharedvertex_map[origindex];
		std::vector<SharedVertex>::iterator it;

		for (it = sharedmap.begin(); it != sharedmap.end(); it++) {
			if (it->m_darray != darray)
				continue;
			if (!it->m_darray->m_vertex[it->m_offset].closeTo(&texvert))
				continue;

			// found one, add it and we're done
			return it->m_offset;
		}
	}

	// no shared vertex found, add a new one
	int offset = slot->AddVertex(texvert);

	{ 	// Shared Vertex!
		SharedVertex shared;
		shared.m_darray = darray;
		shared.m_offset = offset;
		m_sharedvertex_map[origindex].push_back(shared);
	}

	return offset;
}

int RAS_MeshObject::NumVertices(RAS_IPolyMaterial *mat)
{
	RAS_MeshMaterial *mmat = GetMeshMaterial(mat);
	RAS_MeshSlot *slot = mmat->m_baseslot;
	return slot->GetDisplayArray()->m_vertex.size();
}

RAS_TexVert *RAS_MeshObject::GetVertex(unsigned int matid, unsigned int index)
{
	RAS_MeshMaterial *mmat = GetMeshMaterial(matid);

	if (!mmat)
		return NULL;

	RAS_MeshSlot *slot = mmat->m_baseslot;
	RAS_DisplayArray *array = slot->GetDisplayArray();

	if (index < array->m_vertex.size()) {
		return &array->m_vertex[index];
	}

	return NULL;
}

const float *RAS_MeshObject::GetVertexLocation(unsigned int orig_index)
{
	std::vector<SharedVertex>& sharedmap = m_sharedvertex_map[orig_index];
	std::vector<SharedVertex>::iterator it = sharedmap.begin();
	return it->m_darray->m_vertex[it->m_offset].getXYZ();
}

RAS_MeshUser* RAS_MeshObject::AddMeshUser(void *clientobj, RAS_Deformer *deformer)
{
	RAS_MeshUser *meshUser = new RAS_MeshUser(clientobj);
	for (std::list<RAS_MeshMaterial>::iterator it = m_materials.begin(); it != m_materials.end(); ++it) {
		RAS_MeshSlot *ms = it->m_bucket->CopyMesh(it->m_baseslot);
		ms->SetMeshUser(meshUser);
		ms->SetDeformer(deformer);
		it->m_slots[clientobj] = ms;
		meshUser->AddMeshSlot(ms);
	}
	return meshUser;
}

void RAS_MeshObject::RemoveFromBuckets(void *clientobj)
{
	for (std::list<RAS_MeshMaterial>::iterator it = m_materials.begin(); it != m_materials.end(); ++it) {
		std::map<void *, RAS_MeshSlot *>::iterator msit = it->m_slots.find(clientobj);

		if (msit == it->m_slots.end()) {
			continue;
		}

		RAS_MeshSlot *ms = msit->second;

		it->m_bucket->RemoveMesh(ms);
		it->m_slots.erase(clientobj);
	}
}

void RAS_MeshObject::EndConversion()
{
#if 0
	m_sharedvertex_map.clear(); // SharedVertex
	vector<vector<SharedVertex> >   shared_null(0);
	shared_null.swap(m_sharedvertex_map);   /* really free the memory */
#endif
}

void RAS_MeshObject::SortPolygons(RAS_MeshSlot *ms, const MT_Transform &transform)
{
	// Limitations: sorting is quite simple, and handles many
	// cases wrong, partially due to polygons being sorted per
	// bucket.
	//
	// a) mixed triangles/quads are sorted wrong
	// b) mixed materials are sorted wrong
	// c) more than 65k faces are sorted wrong
	// d) intersecting objects are sorted wrong
	// e) intersecting polygons are sorted wrong
	//
	// a) can be solved by making all faces either triangles or quads
	// if they need to be z-sorted. c) could be solved by allowing
	// larger buckets, b) and d) cannot be solved easily if we want
	// to avoid excessive state changes while drawing. e) would
	// require splitting polygons.

	RAS_DisplayArray *array = ms->GetDisplayArray();

	unsigned int nvert = 3;
	unsigned int totpoly = array->m_index.size() / nvert;

	if (totpoly <= 1)
		return;

	// Extract camera Z plane...
	const MT_Vector3 pnorm(transform.getBasis()[2]);
	// unneeded: const MT_Scalar pval = transform.getOrigin()[2];

	std::vector<polygonSlot> poly_slots(totpoly);

	// get indices and z into temporary array
	for (unsigned int j = 0; j < totpoly; j++)
		poly_slots[j].get(array->m_vertex.data(), array->m_index.data(), j * nvert, nvert, pnorm);

	// sort (stable_sort might be better, if flickering happens?)
	std::sort(poly_slots.begin(), poly_slots.end(), backtofront());

	// get indices from temporary array again
	for (unsigned int j = 0; j < totpoly; j++)
		poly_slots[j].set(array->m_index.data(), j * nvert, nvert);
}


bool RAS_MeshObject::HasColliderPolygon()
{
	int numpolys = NumPolygons();
	for (int p = 0; p < numpolys; p++) {
		if (m_Polygons[p]->IsCollider())
			return true;
	}

	return false;
}
