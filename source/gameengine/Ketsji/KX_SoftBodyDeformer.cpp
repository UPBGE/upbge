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

/** \file gameengine/Converter/KX_SoftBodyDeformer.cpp
 *  \ingroup bgeconv
 */


#ifdef _MSC_VER
#  pragma warning (disable:4786)
#endif //WIN32

#include "BLI_utildefines.h"

#include "KX_SoftBodyDeformer.h"
#include "RAS_MeshObject.h"
#include "RAS_DisplayArray.h"
#include "RAS_BoundingBoxManager.h"

#ifdef WITH_BULLET

#include "CcdPhysicsEnvironment.h"
#include "CcdPhysicsController.h"
#include "BulletSoftBody/btSoftBody.h"

#include "btBulletDynamicsCommon.h"

KX_SoftBodyDeformer::KX_SoftBodyDeformer(RAS_MeshObject *pMeshObject, BL_DeformableGameObject *gameobj)
	:RAS_Deformer(pMeshObject),
	m_gameobj(gameobj),
	m_needUpdateAabb(true)
{
	KX_Scene *scene = m_gameobj->GetScene();
	RAS_BoundingBoxManager *boundingBoxManager = scene->GetBoundingBoxManager();
	m_boundingBox = boundingBoxManager->CreateBoundingBox();
	// Set AABB default to mesh bounding box AABB.
	m_boundingBox->CopyAabb(m_mesh->GetBoundingBox());
}

KX_SoftBodyDeformer::~KX_SoftBodyDeformer()
{
}

void KX_SoftBodyDeformer::Relink(std::map<SCA_IObject *, SCA_IObject *>& map)
{
	BL_DeformableGameObject *obj = static_cast<BL_DeformableGameObject *>(map[m_gameobj]);

	if (obj) {
		m_gameobj = obj;
		m_mesh = m_gameobj->GetMeshList().front();
	}
	else {
		m_gameobj = nullptr;
		m_mesh = nullptr;
	}
}

void KX_SoftBodyDeformer::Apply(RAS_MeshMaterial *meshmat, RAS_IDisplayArray *array)
{
	CcdPhysicsController *ctrl = (CcdPhysicsController *)m_gameobj->GetPhysicsController();
	if (!ctrl)
		return;

	btSoftBody *softBody = ctrl->GetSoftBody();
	if (!softBody)
		return;

	// update the vertex in m_transverts
	Update();

	RAS_IDisplayArray *origarray = meshmat->GetDisplayArray();

	btSoftBody::tNodeArray&   nodes(softBody->m_nodes);
	const std::vector<unsigned int>& indices = ctrl->GetSoftBodyIndices();

	if (m_needUpdateAabb) {
		m_boundingBox->SetAabb(MT_Vector3(0.0f, 0.0f, 0.0f), MT_Vector3(0.0f, 0.0f, 0.0f));
		m_needUpdateAabb = false;
	}

	// AABB Box : min/max.
	MT_Vector3 aabbMin(FLT_MAX, FLT_MAX, FLT_MAX);
	MT_Vector3 aabbMax(-FLT_MAX, -FLT_MAX, -FLT_MAX);

	for (unsigned int i = 0, size = array->GetVertexCount(); i < size; ++i) {
		RAS_Vertex v = array->GetVertex(i);
		const RAS_VertexInfo& vinfo = array->GetVertexInfo(i);

		const unsigned int index = indices[vinfo.GetOrigIndex()];
		const MT_Vector3 pt(ToMoto(nodes[index].m_x));
		v.SetXYZ(pt);

		const MT_Vector3 normal(ToMoto(nodes[index].m_n));
		v.SetNormal(normal);

		if (!m_gameobj->GetAutoUpdateBounds()) {
			continue;
		}

		const MT_Vector3& scale = m_gameobj->NodeGetWorldScaling();
		const MT_Vector3& invertscale = MT_Vector3(1.0f / scale.x(), 1.0f / scale.y(), 1.0f / scale.z());
		const MT_Vector3& pos = m_gameobj->NodeGetWorldPosition();
		const MT_Matrix3x3& rot = m_gameobj->NodeGetWorldOrientation();

		// Extract object transform from the vertex position.
		const MT_Vector3 ptWorld = (pt - pos) * rot * invertscale;
		// if the AABB need an update.
		aabbMin.x() = std::min(aabbMin.x(), ptWorld.x());
		aabbMin.y() = std::min(aabbMin.y(), ptWorld.y());
		aabbMin.z() = std::min(aabbMin.z(), ptWorld.z());
		aabbMax.x() = std::max(aabbMax.x(), ptWorld.x());
		aabbMax.y() = std::max(aabbMax.y(), ptWorld.y());
		aabbMax.z() = std::max(aabbMax.z(), ptWorld.z());
	}

	array->UpdateFrom(origarray, origarray->GetModifiedFlag() &
					 (RAS_IDisplayArray::TANGENT_MODIFIED |
					  RAS_IDisplayArray::UVS_MODIFIED |
					  RAS_IDisplayArray::COLORS_MODIFIED));

	m_boundingBox->ExtendAabb(aabbMin, aabbMax);

	array->AppendModifiedFlag(RAS_IDisplayArray::POSITION_MODIFIED | RAS_IDisplayArray::NORMAL_MODIFIED);
}

#endif
