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

#include "MT_assert.h"

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

bool KX_SoftBodyDeformer::Apply(RAS_IPolyMaterial *polymat)
{
	CcdPhysicsController *ctrl = (CcdPhysicsController *)m_gameobj->GetPhysicsController();
	if (!ctrl)
		return false;

	btSoftBody *softBody = ctrl->GetSoftBody();
	if (!softBody)
		return false;

	//printf("apply\n");
	RAS_MeshMaterial *mmat;
	RAS_MeshSlot *slot;
	size_t i;

	// update the vertex in m_transverts
	Update();

	// The vertex cache can only be updated for this deformer:
	// Duplicated objects with more than one ploymaterial (=multiple mesh slot per object)
	// share the same mesh (=the same cache). As the rendering is done per polymaterial
	// cycling through the objects, the entire mesh cache cannot be updated in one shot.
	mmat = m_pMeshObject->GetMeshMaterial(polymat);
	if (!mmat->m_slots[(void *)m_gameobj->getClientInfo()])
		return true;

	slot = mmat->m_slots[(void *)m_gameobj->getClientInfo()];
	RAS_DisplayArray *array = slot->GetDisplayArray();
	RAS_DisplayArray *origarray = mmat->m_baseslot->GetDisplayArray();

	btSoftBody::tNodeArray&   nodes(softBody->m_nodes);

	int index = 0;
	for (i = 0; i < array->m_vertex.size(); i++, index++) {
		RAS_TexVert& v = array->m_vertex[i];
		RAS_TexVert& origvert = origarray->m_vertex[i];
		/* The physics converter write the soft body index only in the original
		 * vertex array because at this moment it doesn't know which is the
		 * game object. It didn't cause any issues because it's always the same
		 * vertex order.
		 */
		const unsigned int softbodyindex = origvert.getSoftBodyIndex();

		MT_Vector3 pt(
		    nodes[softbodyindex].m_x.getX(),
		    nodes[softbodyindex].m_x.getY(),
		    nodes[softbodyindex].m_x.getZ());
		v.SetXYZ(pt);

		MT_Vector3 normal(
		    nodes[softbodyindex].m_n.getX(),
		    nodes[softbodyindex].m_n.getY(),
		    nodes[softbodyindex].m_n.getZ());
		v.SetNormal(normal);

		/// Update vertex data from the original mesh.
		const short modifiedFlag = m_pMeshObject->GetModifiedFlag();
		// If the tangent vertex data is modified.
		if (modifiedFlag & RAS_MeshObject::TANGENT_MODIFIED) {
			v.SetTangent(origvert.getTangent());
		}
		// If the tangent vertex data is modified.
		if (modifiedFlag & RAS_MeshObject::UVS_MODIFIED) {
			for (unsigned int uv = 0; uv < 8; ++uv) {
				v.SetUV(uv, origvert.getUV(uv));
			}
		}
		// If the colors vertex data is modified.
		if (modifiedFlag & RAS_MeshObject::COLORS_MODIFIED) {
			v.SetRGBA(*((unsigned int *)origvert.getRGBA()));
		}

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

	return true;
}

#endif
