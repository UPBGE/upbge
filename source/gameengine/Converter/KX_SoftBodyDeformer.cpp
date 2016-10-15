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

#ifdef WITH_BULLET

#include "CcdPhysicsEnvironment.h"
#include "CcdPhysicsController.h"
#include "BulletSoftBody/btSoftBody.h"

#include "btBulletDynamicsCommon.h"

void KX_SoftBodyDeformer::Relink(std::map<void *, void *>& map)
{
	void *h_obj = map[m_gameobj];

	if (h_obj) {
		m_gameobj = (BL_DeformableGameObject *)h_obj;
		m_pMeshObject = m_gameobj->GetMesh(0);
	}
	else {
		m_gameobj = NULL;
		m_pMeshObject = NULL;
	}
}

bool KX_SoftBodyDeformer::Apply(RAS_IPolyMaterial *polymat, RAS_MeshMaterial *meshmat)
{
	CcdPhysicsController *ctrl = (CcdPhysicsController *)m_gameobj->GetPhysicsController();
	if (!ctrl)
		return false;

	btSoftBody *softBody = ctrl->GetSoftBody();
	if (!softBody)
		return false;

	// update the vertex in m_transverts
	Update();

	RAS_MeshSlot *slot = meshmat->m_slots[(void *)m_gameobj->getClientInfo()];
	if (!slot) {
		return false;
	}

	RAS_IDisplayArray *array = slot->GetDisplayArray();
	RAS_IDisplayArray *origarray = meshmat->m_baseslot->GetDisplayArray();

	btSoftBody::tNodeArray&   nodes(softBody->m_nodes);

	for (unsigned int i = 0, size = array->GetVertexCount(); i < size; ++i) {
		RAS_ITexVert *v = array->GetVertex(i);
		const RAS_TexVertInfo& vinfo = origarray->GetVertexInfo(i);
		/* The physics converter write the soft body index only in the original
		 * vertex array because at this moment it doesn't know which is the
		 * game object. It didn't cause any issues because it's always the same
		 * vertex order.
		 */
		const unsigned int softbodyindex = vinfo.getSoftBodyIndex();

		MT_Vector3 pt(
		    nodes[softbodyindex].m_x.getX(),
		    nodes[softbodyindex].m_x.getY(),
		    nodes[softbodyindex].m_x.getZ());
		v->SetXYZ(pt);

		MT_Vector3 normal(
		    nodes[softbodyindex].m_n.getX(),
		    nodes[softbodyindex].m_n.getY(),
		    nodes[softbodyindex].m_n.getZ());
		v->SetNormal(normal);

		if (!m_gameobj->GetAutoUpdateBounds()) {
			continue;
		}

		const MT_Vector3& scale = m_gameobj->NodeGetWorldScaling();
		const MT_Vector3& invertscale = MT_Vector3(1.0f / scale.x(), 1.0f / scale.y(), 1.0f / scale.z());
		const MT_Vector3& pos = m_gameobj->NodeGetWorldPosition();
		const MT_Matrix3x3& rot = m_gameobj->NodeGetWorldOrientation();

		// Extract object transform from the vertex position.
		pt = (pt - pos) * rot * invertscale;

		// if the AABB need an update.
		if (m_needUpdateAABB) {
			m_aabbMin = m_aabbMax = pt;
			m_needUpdateAABB = false;
		}
		else {
			m_aabbMin.x() = std::min(m_aabbMin.x(), pt.x());
			m_aabbMin.y() = std::min(m_aabbMin.y(), pt.y());
			m_aabbMin.z() = std::min(m_aabbMin.z(), pt.z());
			m_aabbMax.x() = std::max(m_aabbMax.x(), pt.x());
			m_aabbMax.y() = std::max(m_aabbMax.y(), pt.y());
			m_aabbMax.z() = std::max(m_aabbMax.z(), pt.z());
		}
	}

	array->UpdateFrom(origarray, origarray->GetModifiedFlag() &
					 (RAS_IDisplayArray::TANGENT_MODIFIED |
					  RAS_IDisplayArray::UVS_MODIFIED |
					  RAS_IDisplayArray::COLORS_MODIFIED));

	return true;
}

#endif
