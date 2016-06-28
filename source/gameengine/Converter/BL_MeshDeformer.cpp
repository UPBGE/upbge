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
 * Simple deformation controller that restores a mesh to its rest position
 */

/** \file gameengine/Converter/BL_MeshDeformer.cpp
 *  \ingroup bgeconv
 */

#ifdef _MSC_VER
/* This warning tells us about truncation of __long__ stl-generated names.
 * It can occasionally cause DevStudio to have internal compiler warnings. */
#  pragma warning( disable:4786 )
#endif

#include "RAS_IPolygonMaterial.h"
#include "RAS_DisplayArray.h"
#include "BL_DeformableGameObject.h"
#include "BL_MeshDeformer.h"
#include "RAS_MeshObject.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"

#include "STR_HashedString.h"
#include "BLI_math.h"

bool BL_MeshDeformer::Apply(RAS_IPolyMaterial *)
{
	// only apply once per frame if the mesh is actually modified
	if ((m_pMeshObject->GetModifiedFlag() & RAS_MeshObject::MESH_MODIFIED) &&
		m_lastDeformUpdate != m_gameobj->GetLastFrame())
	{
		// For each material
		for (list<RAS_MeshMaterial>::iterator mit = m_pMeshObject->GetFirstMaterial();
		     mit != m_pMeshObject->GetLastMaterial(); ++mit)
		{
			RAS_MeshSlot *slot = mit->m_slots[(void *)m_gameobj->getClientInfo()];
			if (!slot) {
				continue;
			}

			RAS_IDisplayArray *array = slot->GetDisplayArray();

			//	For each vertex
			for (unsigned int i = 0, size = array->GetVertexCount(); i < size; ++i) {
				RAS_ITexVert *v = array->GetVertex(i);
				const RAS_TexVertInfo& vinfo = array->GetVertexInfo(i);
				v->SetXYZ(m_bmesh->mvert[vinfo.getOrigIndex()].co);
			}
		}

		m_lastDeformUpdate = m_gameobj->GetLastFrame();

		return true;
	}

	return false;
}

BL_MeshDeformer::~BL_MeshDeformer()
{
	if (m_transverts)
		delete[] m_transverts;
	if (m_transnors)
		delete[] m_transnors;
}

void BL_MeshDeformer::ProcessReplica()
{
	m_transverts = NULL;
	m_transnors = NULL;
	m_tvtot = 0;
	m_bDynamic = false;
	m_lastDeformUpdate = -1.0;
}

void BL_MeshDeformer::Relink(std::map<void *, void *>& map)
{
	m_gameobj = (BL_DeformableGameObject *)map[m_gameobj];
}

/**
 * \warning This function is expensive!
 */
void BL_MeshDeformer::RecalcNormals()
{
	// if we don't use a vertex array we does nothing.
	if (!UseVertexArray()) {
		return;
	}

	/* We don't normalize for performance, not doing it for faces normals
	 * gives area-weight normals which often look better anyway, and use
	 * GL_NORMALIZE so we don't have to do per vertex normalization either
	 * since the GPU can do it faster */
	list<RAS_MeshMaterial>::iterator mit;
	size_t i;

	/* set vertex normals to zero */
	memset(m_transnors, 0, sizeof(float) * 3 * m_bmesh->totvert);

	/* add face normals to vertices. */
	for (mit = m_pMeshObject->GetFirstMaterial(); mit != m_pMeshObject->GetLastMaterial(); ++mit) {
		RAS_MeshSlot *slot = mit->m_slots[(void *)m_gameobj->getClientInfo()];
		if (!slot) {
			continue;
		}

		RAS_IDisplayArray *array = slot->GetDisplayArray();

		for (i = 0; i < array->GetIndexCount(); i += 3) {
			const unsigned int indexes[3] = {array->GetIndex(i), array->GetIndex(i + 1), array->GetIndex(i + 2)};
			RAS_ITexVert *v1 = array->GetVertex(indexes[0]);
			RAS_ITexVert *v2 = array->GetVertex(indexes[1]);
			RAS_ITexVert *v3 = array->GetVertex(indexes[2]);
			const RAS_TexVertInfo& v1info = array->GetVertexInfo(indexes[0]);
			const RAS_TexVertInfo& v2info = array->GetVertexInfo(indexes[1]);
			const RAS_TexVertInfo& v3info = array->GetVertexInfo(indexes[2]);

			const float *co1 = m_transverts[v1info.getOrigIndex()];
			const float *co2 = m_transverts[v2info.getOrigIndex()];
			const float *co3 = m_transverts[v3info.getOrigIndex()];

			/* compute face normal */
			float fnor[3], n1[3], n2[3];

			n1[0] = co1[0] - co2[0];
			n2[0] = co2[0] - co3[0];
			n1[1] = co1[1] - co2[1];

			n2[1] = co2[1] - co3[1];
			n1[2] = co1[2] - co2[2];
			n2[2] = co2[2] - co3[2];

			fnor[0] = n1[1] * n2[2] - n1[2] * n2[1];
			fnor[1] = n1[2] * n2[0] - n1[0] * n2[2];
			fnor[2] = n1[0] * n2[1] - n1[1] * n2[0];
			normalize_v3(fnor);

			/* add to vertices for smooth normals */
			float *vn1 = m_transnors[v1info.getOrigIndex()];
			float *vn2 = m_transnors[v2info.getOrigIndex()];
			float *vn3 = m_transnors[v3info.getOrigIndex()];

			vn1[0] += fnor[0]; vn1[1] += fnor[1]; vn1[2] += fnor[2];
			vn2[0] += fnor[0]; vn2[1] += fnor[1]; vn2[2] += fnor[2];
			vn3[0] += fnor[0]; vn3[1] += fnor[1]; vn3[2] += fnor[2];

			/* in case of flat - just assign, the vertices are split */
			if (v1info.getFlag() & RAS_TexVertInfo::FLAT) {
				MT_Vector3 normal = MT_Vector3(fnor);
				v1->SetNormal(normal);
				v2->SetNormal(normal);
				v3->SetNormal(normal);
			}
		}
	}

	/* assign smooth vertex normals */
	for (mit = m_pMeshObject->GetFirstMaterial(); mit != m_pMeshObject->GetLastMaterial(); ++mit) {
		RAS_MeshSlot *slot = mit->m_slots[(void *)m_gameobj->getClientInfo()];
		if (!slot) {
			continue;
		}

		RAS_IDisplayArray *array = slot->GetDisplayArray();

		for (unsigned int i = 0, size = array->GetVertexCount(); i < size; ++i) {
			RAS_ITexVert *v = array->GetVertex(i);
			const RAS_TexVertInfo& vinfo = array->GetVertexInfo(i);

			if (!(vinfo.getFlag() & RAS_TexVertInfo::FLAT))
				v->SetNormal(MT_Vector3(m_transnors[vinfo.getOrigIndex()])); //.safe_normalized()
		}
	}
}

void BL_MeshDeformer::VerifyStorage()
{
	/* Ensure that we have the right number of verts assigned */
	if (m_tvtot != m_bmesh->totvert) {
		if (m_transverts)
			delete[] m_transverts;
		if (m_transnors)
			delete[] m_transnors;

		m_transverts = new float[m_bmesh->totvert][3];
		m_transnors = new float[m_bmesh->totvert][3];
		m_tvtot = m_bmesh->totvert;
	}
}

