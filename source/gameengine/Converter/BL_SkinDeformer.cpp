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

/** \file gameengine/Converter/BL_SkinDeformer.cpp
 *  \ingroup bgeconv
 */

#ifdef _MSC_VER
#  pragma warning (disable:4786)
#endif

// Eigen3 stuff used for BGEDeformVerts
#include <Eigen/Core>
#include <Eigen/LU>

#include "BL_SkinDeformer.h"
#include <string>
#include "RAS_IPolygonMaterial.h"
#include "RAS_DisplayArray.h"
#include "RAS_MeshObject.h"
#include "RAS_MeshUser.h"
#include "RAS_BoundingBox.h"

//#include "BL_ArmatureController.h"
#include "BL_DeformableGameObject.h"
#include "DNA_armature_types.h"
#include "DNA_action_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"
#include "BLI_utildefines.h"
#include "BKE_armature.h"
#include "BKE_action.h"
#include "MT_Vector3.h"

extern "C" {
	#include "BKE_lattice.h"
	#include "BKE_deform.h"
}


#include "BLI_blenlib.h"
#include "BLI_math.h"

#define __NLA_DEFNORMALS
//#undef __NLA_DEFNORMALS

static short get_deformflags(Object *bmeshobj)
{
	short flags = ARM_DEF_VGROUP;

	ModifierData *md;
	for (md = (ModifierData *)bmeshobj->modifiers.first; md; md = md->next) {
		if (md->type == eModifierType_Armature) {
			flags |= ((ArmatureModifierData *)md)->deformflag;
			break;
		}
	}

	return flags;
}

BL_SkinDeformer::BL_SkinDeformer(BL_DeformableGameObject *gameobj,
								 Object *bmeshobj,
								 RAS_MeshObject *mesh,
								 BL_ArmatureObject *arma)
	:BL_MeshDeformer(gameobj, bmeshobj, mesh),
	m_armobj(arma),
	m_lastArmaUpdate(-1),
	m_releaseobject(false),
	m_poseApplied(false),
	m_recalcNormal(true),
	m_copyNormals(false),
	m_dfnrToPC(nullptr)
{
	copy_m4_m4(m_obmat, bmeshobj->obmat);
	m_deformflags = get_deformflags(bmeshobj);
}

BL_SkinDeformer::BL_SkinDeformer(
	BL_DeformableGameObject *gameobj,
	Object *bmeshobj_old, // Blender object that owns the new mesh
	Object *bmeshobj_new, // Blender object that owns the original mesh
	RAS_MeshObject *mesh,
	bool release_object,
	bool recalc_normal,
	BL_ArmatureObject *arma)
	:BL_MeshDeformer(gameobj, bmeshobj_old, mesh),
	m_armobj(arma),
	m_lastArmaUpdate(-1),
	m_releaseobject(release_object),
	m_recalcNormal(recalc_normal),
	m_copyNormals(false),
	m_dfnrToPC(nullptr)
{
	// this is needed to ensure correct deformation of mesh:
	// the deformation is done with Blender's armature_deform_verts() function
	// that takes an object as parameter and not a mesh. The object matrice is used
	// in the calculation, so we must use the matrix of the original object to
	// simulate a pure replacement of the mesh.
	copy_m4_m4(m_obmat, bmeshobj_new->obmat);
	m_deformflags = get_deformflags(bmeshobj_new);
}

BL_SkinDeformer::~BL_SkinDeformer()
{
	if (m_releaseobject && m_armobj)
		m_armobj->Release();
	if (m_dfnrToPC)
		delete [] m_dfnrToPC;
}

void BL_SkinDeformer::Relink(std::map<SCA_IObject *, SCA_IObject *>& map)
{
	if (m_armobj) {
		m_armobj = static_cast<BL_ArmatureObject *>(map[m_armobj]);
	}

	BL_MeshDeformer::Relink(map);
}

bool BL_SkinDeformer::Apply(RAS_MeshMaterial *meshmat, RAS_IDisplayArray *array)
{
	// if we don't use a vertex array we does nothing.
	if (!UseVertexArray() || !meshmat || !array) {
		return false;
	}

	RAS_IDisplayArray *origarray = meshmat->GetDisplayArray();

	const short modifiedFlag = origarray->GetModifiedFlag();
	// No modifications ?
	if (modifiedFlag == RAS_IDisplayArray::NONE_MODIFIED) {
		return false;
	}

	/// Update vertex data from the original mesh.
	array->UpdateFrom(origarray, modifiedFlag &
					 (RAS_IDisplayArray::TANGENT_MODIFIED |
					  RAS_IDisplayArray::UVS_MODIFIED |
					  RAS_IDisplayArray::COLORS_MODIFIED));

	// We do everything in UpdateInternal() now so we can thread it.
	// All that is left is telling the rasterizer if we've changed the mesh
	bool retval = !m_poseApplied;
	m_poseApplied = true;
	return retval;
}

RAS_Deformer *BL_SkinDeformer::GetReplica()
{
	BL_SkinDeformer *result;

	result = new BL_SkinDeformer(*this);
	/* there is m_armobj that must be fixed but we cannot do it now, it will be done in Relink */
	result->ProcessReplica();
	return result;
}

void BL_SkinDeformer::ProcessReplica()
{
	BL_MeshDeformer::ProcessReplica();
	m_lastArmaUpdate = -1.0;
	m_releaseobject = false;
	m_dfnrToPC = nullptr;
}

void BL_SkinDeformer::BlenderDeformVerts()
{
	float obmat[4][4];  // the original object matrix
	Object *par_arma = m_armobj->GetArmatureObject();

	// save matrix first
	copy_m4_m4(obmat, m_objMesh->obmat);
	// set reference matrix
	copy_m4_m4(m_objMesh->obmat, m_obmat);

	armature_deform_verts(par_arma, m_objMesh, nullptr, m_transverts, nullptr, m_bmesh->totvert, m_deformflags, nullptr, nullptr, nullptr);

	// restore matrix
	copy_m4_m4(m_objMesh->obmat, obmat);

#ifdef __NLA_DEFNORMALS
	if (m_recalcNormal)
		RecalcNormals();
#endif
}

void BL_SkinDeformer::BGEDeformVerts()
{
	Object *par_arma = m_armobj->GetArmatureObject();
	MDeformVert *dverts = m_bmesh->dvert;
	bDeformGroup *dg;
	int defbase_tot;
	Eigen::Matrix4f pre_mat, post_mat, chan_mat, norm_chan_mat;

	if (!dverts)
		return;

	defbase_tot = BLI_listbase_count(&m_objMesh->defbase);

	if (m_dfnrToPC == nullptr) {
		m_dfnrToPC = new bPoseChannel *[defbase_tot];
		int i;
		for (i = 0, dg = (bDeformGroup *)m_objMesh->defbase.first;
		     dg;
		     ++i, dg = dg->next)
		{
			m_dfnrToPC[i] = BKE_pose_channel_find_name(par_arma->pose, dg->name);

			if (m_dfnrToPC[i] && m_dfnrToPC[i]->bone->flag & BONE_NO_DEFORM)
				m_dfnrToPC[i] = nullptr;
		}
	}

	post_mat = Eigen::Matrix4f::Map((float *)m_obmat).inverse() * Eigen::Matrix4f::Map((float *)m_armobj->GetArmatureObject()->obmat);
	pre_mat = post_mat.inverse();

	MDeformVert *dv = dverts;
	MDeformWeight *dw;

	for (int i = 0; i < m_bmesh->totvert; ++i, dv++) {
		float contrib = 0.0f, weight, max_weight = -1.0f;
		bPoseChannel *pchan = nullptr;
		Eigen::Vector3f normorg(m_bmesh->mvert[i].no[0], m_bmesh->mvert[i].no[1], m_bmesh->mvert[i].no[2]);
		Eigen::Map<Eigen::Vector3f> norm = Eigen::Vector3f::Map(m_transnors[i]);
		Eigen::Vector4f vec(0.0f, 0.0f, 0.0f, 1.0f);
		Eigen::Vector4f co(m_transverts[i][0],
		                   m_transverts[i][1],
		                   m_transverts[i][2],
		                   1.0f);

		if (!dv->totweight)
			continue;

		co = pre_mat * co;

		dw = dv->dw;

		for (unsigned int j = dv->totweight; j != 0; j--, dw++) {
			const int index = dw->def_nr;

			if (index < defbase_tot && (pchan = m_dfnrToPC[index])) {
				weight = dw->weight;

				if (weight) {
					chan_mat = Eigen::Matrix4f::Map((float *)pchan->chan_mat);

					// Update Vertex Position
					vec.noalias() += (chan_mat * co - co) * weight;

					// Save the most influential channel so we can use it to update the vertex normal
					if (weight > max_weight)
					{
						max_weight = weight;
						norm_chan_mat = chan_mat;
					}

					contrib += weight;
				}
			}
		}

		// Update Vertex Normal
		norm = norm_chan_mat.topLeftCorner<3, 3>() * normorg;

		co.noalias() += vec / contrib;
		co[3] = 1.0f; // Make sure we have a 1 for the w component!

		co = post_mat * co;

		m_transverts[i][0] = co[0];
		m_transverts[i][1] = co[1];
		m_transverts[i][2] = co[2];
	}
	m_copyNormals = true;
}

void BL_SkinDeformer::UpdateTransverts()
{
	// if we don't use a vertex array we does nothing.
	if (!UseVertexArray()) {
		return;
	}

	bool first = true;
	if (m_transverts) {
		// AABB Box : min/max.
		MT_Vector3 aabbMin;
		MT_Vector3 aabbMax;

		// the vertex cache is unique to this deformer, no need to update it
		// if it wasn't updated! We must update all the materials at once
		// because we will not get here again for the other material
		for (RAS_IDisplayArray *array: m_displayArrayList) {
			// for each vertex
			// copy the untransformed data from the original mvert
			for (unsigned int i = 0, size = array->GetVertexCount(); i < size; ++i) {
				RAS_ITexVert *v = array->GetVertex(i);
				const RAS_TexVertInfo& vinfo = array->GetVertexInfo(i);
				v->SetXYZ(m_transverts[vinfo.getOrigIndex()]);
				if (m_copyNormals)
					v->SetNormal(MT_Vector3(m_transnors[vinfo.getOrigIndex()]));

				MT_Vector3 vertpos = v->xyz();

				if (!m_gameobj->GetAutoUpdateBounds()) {
					continue;
				}

				// For the first vertex of the mesh, only initialize AABB.
				if (first) {
					aabbMin = aabbMax = vertpos;
					first = false;
				}
				else {
					aabbMin.x() = std::min(aabbMin.x(), vertpos.x());
					aabbMin.y() = std::min(aabbMin.y(), vertpos.y());
					aabbMin.z() = std::min(aabbMin.z(), vertpos.z());
					aabbMax.x() = std::max(aabbMax.x(), vertpos.x());
					aabbMax.y() = std::max(aabbMax.y(), vertpos.y());
					aabbMax.z() = std::max(aabbMax.z(), vertpos.z());
				}
			}

			array->SetModifiedFlag(RAS_IDisplayArray::POSITION_MODIFIED | RAS_IDisplayArray::NORMAL_MODIFIED);
		}

		m_boundingBox->SetAabb(aabbMin, aabbMax);


		if (m_copyNormals)
			m_copyNormals = false;
	}
}

bool BL_SkinDeformer::UpdateInternal(bool shape_applied)
{
	/* See if the armature has been updated for this frame */
	if (PoseUpdated()) {
		if (!shape_applied) {
			/* store verts locally */
			VerifyStorage();
		}

		m_armobj->ApplyPose();

		if (m_armobj->GetVertDeformType() == ARM_VDEF_BGE_CPU)
			BGEDeformVerts();
		else
			BlenderDeformVerts();

		/* Update the current frame */
		m_lastArmaUpdate = m_armobj->GetLastFrame();

		m_armobj->RestorePose();
		/* dynamic vertex, cannot use display list */
		m_bDynamic = true;

		UpdateTransverts();

		m_poseApplied = false;

		/* indicate that the m_transverts and normals are up to date */
		return true;
	}

	return false;
}

bool BL_SkinDeformer::Update(void)
{
	return UpdateInternal(false);
}

/* XXX note: I propose to drop this function */
void BL_SkinDeformer::SetArmature(BL_ArmatureObject *armobj)
{
	// only used to set the object now
	m_armobj = armobj;
}
