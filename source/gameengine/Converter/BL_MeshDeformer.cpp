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
#include "BL_MeshDeformer.h"
#include "KX_GameObject.h"
#include "RAS_BoundingBoxManager.h"
#include "RAS_Mesh.h"
#include "RAS_MeshUser.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"

#include <string>
#include "BLI_math.h"

unsigned short BL_MeshDeformer::NeedUpdate() const
{
	for (const DisplayArraySlot& slot : m_slots) {
		const short modifiedFlag = slot.m_arrayUpdateClient.GetInvalid() &
				~(RAS_DisplayArray::POSITION_MODIFIED | RAS_DisplayArray::NORMAL_MODIFIED);
		if (modifiedFlag != RAS_DisplayArray::NONE_MODIFIED) {
			return UPDATE_DISPLAY_ARRAY;
		}
	}

	return 0;
}

void BL_MeshDeformer::Update(unsigned short reason)
{
	if (!(reason & UPDATE_DISPLAY_ARRAY)) {
		return;
	}

	// For each display array
	for (const DisplayArraySlot& slot : m_slots) {
		RAS_DisplayArray *array = slot.m_displayArray;

		for (unsigned int i = 0, size = array->GetVertexCount(); i < size; ++i) {
			const RAS_VertexInfo& vinfo = array->GetVertexInfo(i);
			array->SetPosition(i, mt::vec3_packed(m_bmesh->mvert[vinfo.GetOrigIndex()].co));
		}

		array->NotifyUpdate(RAS_DisplayArray::POSITION_MODIFIED);
	}
}

BL_MeshDeformer::BL_MeshDeformer(KX_GameObject *gameobj, Object *obj, RAS_Mesh *meshobj)
	:RAS_Deformer(meshobj),
	m_bmesh((Mesh *)(obj->data)),
	m_objMesh(obj),
	m_gameobj(gameobj),
	m_lastDeformUpdate(-1.0),
	m_lastFrame(0.0)
{
	KX_Scene *scene = m_gameobj->GetScene();
	RAS_BoundingBoxManager *boundingBoxManager = scene->GetBoundingBoxManager();
	m_boundingBox = boundingBoxManager->CreateBoundingBox();
	// Set AABB default to mesh bounding box AABB.
	m_boundingBox->CopyAabb(m_mesh->GetBoundingBox());
}

BL_MeshDeformer::~BL_MeshDeformer()
{
}

/**
 * \warning This function is expensive!
 */
void BL_MeshDeformer::RecalcNormals()
{
	/* We don't normalize for performance, not doing it for faces normals
	 * gives area-weight normals which often look better anyway, and use
	 * GL_NORMALIZE so we don't have to do per vertex normalization either
	 * since the GPU can do it faster */

	/* set vertex normals to zero */
	std::fill(m_transnors.begin(), m_transnors.end(), mt::zero3);

	for (const DisplayArraySlot& slot : m_slots) {
		RAS_DisplayArray *array = slot.m_displayArray;
		for (unsigned int i = 0, size = array->GetTriangleIndexCount(); i < size; i += 3) {
			mt::vec3_packed co[3];
			bool flat = false;

			for (unsigned short j = 0; j < 3; ++j) {
				const unsigned int index = array->GetTriangleIndex(i + j);
				const RAS_VertexInfo& vinfo = array->GetVertexInfo(index);
				const unsigned int origindex = vinfo.GetOrigIndex();

				co[j] = m_transverts[origindex];
				flat |= (vinfo.GetFlag() & RAS_VertexInfo::FLAT);
			}

			mt::vec3_packed pnorm;
			normal_tri_v3(pnorm.data, co[0].data, co[1].data, co[2].data);

			for (unsigned short j = 0; j < 3; ++j) {
				const unsigned int index = array->GetTriangleIndex(i + j);

				if (flat) {
					array->SetNormal(index, pnorm);
				}
				else {
					const RAS_VertexInfo& vinfo = array->GetVertexInfo(index);
					const unsigned int origindex = vinfo.GetOrigIndex();
					add_v3_v3(m_transnors[origindex].data, pnorm.data);
				}
			}
		}
	}

	// Assign smooth vertex normals.
	for (const DisplayArraySlot& slot : m_slots) {
		RAS_DisplayArray *array = slot.m_displayArray;
		for (unsigned int i = 0, size = array->GetVertexCount(); i < size; ++i) {
			const RAS_VertexInfo& vinfo = array->GetVertexInfo(i);

			if (!(vinfo.GetFlag() & RAS_VertexInfo::FLAT)) {
				array->SetNormal(i, m_transnors[vinfo.GetOrigIndex()]);
			}
		}
	}
}

void BL_MeshDeformer::VerifyStorage()
{
	/* Ensure that we have the right number of verts assigned */
	const unsigned int totvert = m_bmesh->totvert;
	if (m_transverts.size() != totvert) {
		m_transverts.resize(totvert);
		m_transnors.resize(totvert);
	}

	for (unsigned int v = 0; v < totvert; ++v) {
		copy_v3_v3(m_transverts[v].data, m_bmesh->mvert[v].co);
		normal_short_to_float_v3(m_transnors[v].data, m_bmesh->mvert[v].no);
	}
}

void BL_MeshDeformer::SetLastFrame(double lastFrame)
{
	m_lastFrame = lastFrame;
}

