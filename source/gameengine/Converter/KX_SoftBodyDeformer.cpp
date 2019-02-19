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

KX_SoftBodyDeformer::KX_SoftBodyDeformer(RAS_MeshObject *pMeshObject, BL_DeformableGameObject *gameobj)
	:RAS_Deformer(pMeshObject),
	m_gameobj(gameobj),
	m_needUpdateAabb(true)
{
}

KX_SoftBodyDeformer::~KX_SoftBodyDeformer()
{
}

void KX_SoftBodyDeformer::Relink(std::map<SCA_IObject *, SCA_IObject *>& map)
{
	BL_DeformableGameObject *obj = static_cast<BL_DeformableGameObject *>(map[m_gameobj]);

	if (obj) {
		m_gameobj = obj;
		m_mesh = m_gameobj->GetMesh(0);
	}
	else {
		m_gameobj = nullptr;
		m_mesh = nullptr;
	}
}

bool KX_SoftBodyDeformer::Apply(RAS_MeshMaterial *meshmat, RAS_IDisplayArray *array)
{
	CcdPhysicsController *ctrl = (CcdPhysicsController *)m_gameobj->GetPhysicsController();
	if (!ctrl)
		return false;

	btSoftBody *softBody = ctrl->GetSoftBody();
	if (!softBody)
		return false;

	// update the vertex in m_transverts
	Update();

	RAS_IDisplayArray *origarray = meshmat->GetDisplayArray();

	btSoftBody::tNodeArray&   nodes(softBody->m_nodes);

	if (m_needUpdateAabb) {
		m_needUpdateAabb = false;
	}

	// AABB Box : min/max.
	MT_Vector3 aabbMin;
	MT_Vector3 aabbMax;

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
		if (i == 0) {
			aabbMin = aabbMax = pt;
		}
		else {
			aabbMin.x() = std::min(aabbMin.x(), pt.x());
			aabbMin.y() = std::min(aabbMin.y(), pt.y());
			aabbMin.z() = std::min(aabbMin.z(), pt.z());
			aabbMax.x() = std::max(aabbMax.x(), pt.x());
			aabbMax.y() = std::max(aabbMax.y(), pt.y());
			aabbMax.z() = std::max(aabbMax.z(), pt.z());
		}
	}

	array->UpdateFrom(origarray, origarray->GetModifiedFlag() &
					 (RAS_IDisplayArray::TANGENT_MODIFIED |
					  RAS_IDisplayArray::UVS_MODIFIED |
					  RAS_IDisplayArray::COLORS_MODIFIED));

	array->SetModifiedFlag(RAS_IDisplayArray::POSITION_MODIFIED | RAS_IDisplayArray::NORMAL_MODIFIED);

	return true;
}

#endif
