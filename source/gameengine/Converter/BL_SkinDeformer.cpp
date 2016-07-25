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
#include "STR_HashedString.h"
#include "RAS_IPolygonMaterial.h"
#include "RAS_DisplayArray.h"
#include "RAS_MeshObject.h"

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

#include "GPU_material.h"
#include "GPU_extensions.h"

#include <GL/glew.h>

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
	m_dfnrToPC(NULL),
	m_poseMatrices(NULL)
{
	copy_m4_m4(m_obmat, bmeshobj->obmat);
	m_deformflags = get_deformflags(bmeshobj);

	VerifyHardwareSkinning();
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
	m_dfnrToPC(NULL),
	m_poseMatrices(NULL)
{
	// this is needed to ensure correct deformation of mesh:
	// the deformation is done with Blender's armature_deform_verts() function
	// that takes an object as parameter and not a mesh. The object matrice is used
	// in the calculation, so we must use the matrix of the original object to
	// simulate a pure replacement of the mesh.
	copy_m4_m4(m_obmat, bmeshobj_new->obmat);
	m_deformflags = get_deformflags(bmeshobj_new);

	VerifyHardwareSkinning();
}

void BL_SkinDeformer::VerifyHardwareSkinning()
{
	if (m_armobj && m_armobj->GetVertDeformType() == ARM_VDEF_BGE_GPU) {
		// Check to see if we can do this skinning in hardware, and fallback to software if we cannot.
		// Each matrix is sixteen components, and we don't want to use more than half of the available components.
		// We consider a maximum of 128 bones too.
		int defbase_tot = BLI_countlist(&m_objMesh->defbase);
		int max = GPU_max_vertex_uniform_components() / 32;
		if (max > 128)
			max = 128;

		if (defbase_tot > max) {
			printf("Skinned mesh with %d bones not supported by hardware (max: %d): %s. Using software skinning.\n",
			       defbase_tot, max, m_objMesh->id.name);
			m_armobj->SetVertDeformType(ARM_VDEF_BGE_CPU);
		}
	}
}

BL_SkinDeformer::~BL_SkinDeformer()
{
	if (m_releaseobject && m_armobj)
		m_armobj->Release();
	if (m_dfnrToPC)
		delete [] m_dfnrToPC;
	if (m_poseMatrices)
		delete [] m_poseMatrices;

	map<void*, SkinVertData*>::iterator it;
	for (it = m_skinVertData.begin(); it != m_skinVertData.end(); ++it) {
		delete [] it->second;
	}
	m_skinVertData.clear();
}

void BL_SkinDeformer::Relink(std::map<void *, void *>& map)
{
	if (m_armobj) {
		m_armobj = (BL_ArmatureObject *)map[m_armobj];
	}

	BL_MeshDeformer::Relink(map);
}

bool BL_SkinDeformer::Apply(RAS_IPolyMaterial *mat)
{
	// if we don't use a vertex array we does nothing.
	if (!UseVertexArray() || !mat) {
		return false;
	}

	const short modifiedFlag = m_pMeshObject->GetModifiedFlag();
	// No modifications ?
	if (modifiedFlag == 0) {
		return false;
	}

	RAS_MeshMaterial *mmat = m_pMeshObject->GetMeshMaterial(mat);
	RAS_MeshSlot *slot = mmat->m_slots[(void *)m_gameobj->getClientInfo()];
	if (!slot) {
		return false;
	}

	RAS_DisplayArray *array = slot->GetDisplayArray();
	RAS_DisplayArray *origarray = mmat->m_baseslot->GetDisplayArray();

	/// Update vertex data from the original mesh.
	array->UpdateFrom(origarray, modifiedFlag &
					 (RAS_MeshObject::TANGENT_MODIFIED |
					  RAS_MeshObject::UVS_MODIFIED |
					  RAS_MeshObject::COLORS_MODIFIED));

	// We do everything in UpdateInternal() now so we can thread it.
	// All that is left is telling the rasterizer if we've changed the mesh
	bool retval = !m_poseApplied;
	m_poseApplied = true;
	bool retval2 = m_bDynamic;
	m_bDynamic = false;
	if (retval || retval2)
		return true;
	else
		return false;
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
	m_dfnrToPC = NULL;
}

void BL_SkinDeformer::BlenderDeformVerts()
{
	float obmat[4][4];  // the original object matrix
	Object *par_arma = m_armobj->GetArmatureObject();

	// save matrix first
	copy_m4_m4(obmat, m_objMesh->obmat);
	// set reference matrix
	copy_m4_m4(m_objMesh->obmat, m_obmat);

	armature_deform_verts(par_arma, m_objMesh, NULL, m_transverts, NULL, m_bmesh->totvert, m_deformflags, NULL, NULL);

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

	if (m_dfnrToPC == NULL) {
		m_dfnrToPC = new bPoseChannel *[defbase_tot];
		int i;
		for (i = 0, dg = (bDeformGroup *)m_objMesh->defbase.first;
		     dg;
		     ++i, dg = dg->next)
		{
			m_dfnrToPC[i] = BKE_pose_channel_find_name(par_arma->pose, dg->name);

			if (m_dfnrToPC[i] && m_dfnrToPC[i]->bone->flag & BONE_NO_DEFORM)
				m_dfnrToPC[i] = NULL;
		}
	}

	post_mat = Eigen::Matrix4f::Map((float *)m_obmat).inverse() * Eigen::Matrix4f::Map((float *)m_armobj->GetArmatureObject()->obmat);
	pre_mat = post_mat.inverse();

	MDeformVert *dv = dverts;
	MDeformWeight *dw;

	for (int i = 0; i < m_bmesh->totvert; ++i, dv++) {
		float contrib = 0.0f, weight, max_weight = -1.0f;
		bPoseChannel *pchan = NULL;
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
		norm = norm_chan_mat.topLeftCorner<3, 3>() * norm;

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

	RAS_MeshMaterial *mmat;
	RAS_MeshSlot *slot;
	size_t i, nmat, imat;
	bool first = true;
	if (m_bDynamic && m_transverts) {
		// the vertex cache is unique to this deformer, no need to update it
		// if it wasn't updated! We must update all the materials at once
		// because we will not get here again for the other material
		nmat = m_pMeshObject->NumMaterials();
		for (imat = 0; imat < nmat; imat++) {
			mmat = m_pMeshObject->GetMeshMaterial(imat);
			slot = mmat->m_slots[(void *)m_gameobj->getClientInfo()];
			if (!slot) {
				continue;
			}

			RAS_DisplayArray *array = slot->GetDisplayArray();

			// for each vertex
			// copy the untransformed data from the original mvert
			for (i = 0; i < array->m_vertex.size(); i++) {
				RAS_TexVert& v = array->m_vertex[i];
				v.SetXYZ(m_transverts[v.getOrigIndex()]);
				if (m_copyNormals)
					v.SetNormal(MT_Vector3(m_transnors[v.getOrigIndex()]));

				MT_Vector3 vertpos = v.xyz();

				if (!m_gameobj->GetAutoUpdateBounds()) {
					continue;
				}

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

		if (m_copyNormals)
			m_copyNormals = false;
	}
}

bool BL_SkinDeformer::UpdateInternal(bool shape_applied)
{
	/* See if the armature has been updated for this frame */
	if (PoseUpdated()) {
		int vert_deformer = m_armobj->GetVertDeformType();

		if (!shape_applied) {
			/* store verts locally */
			VerifyStorage();

			/* duplicate */
			for (int v = 0; v < m_bmesh->totvert; v++) {
				copy_v3_v3(m_transverts[v], m_bmesh->mvert[v].co);
				normal_short_to_float_v3(m_transnors[v], m_bmesh->mvert[v].no);
			}
		}

		m_armobj->ApplyPose();

		if (vert_deformer == ARM_VDEF_BGE_CPU)
			BGEDeformVerts();
		else if (vert_deformer == ARM_VDEF_BLENDER)
			BlenderDeformVerts();

		/* Update the current frame */
		m_lastArmaUpdate = m_armobj->GetLastFrame();

		m_armobj->RestorePose();
		/* dynamic vertex, cannot use display list */
		m_bDynamic = vert_deformer != ARM_VDEF_BGE_GPU;;

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

void BL_SkinDeformer::HandleGPUUniforms(RAS_IRasterizer *rasty, RAS_MeshSlot &ms)
{
	if (m_armobj->GetVertDeformType() != ARM_VDEF_BGE_GPU)
		return;

	int defbase_tot = BLI_countlist(&m_objMesh->defbase);

	if (m_dfnrToPC == NULL)
	{
		bDeformGroup *dg;
		Object *par_arma = m_armobj->GetArmatureObject();

		m_dfnrToPC = new bPoseChannel*[defbase_tot];
		m_poseMatrices = new float[16*defbase_tot];

		int i;
		for (i=0, dg=(bDeformGroup*)m_objMesh->defbase.first;
			dg;
			++i, dg = dg->next)
		{
			m_dfnrToPC[i] = BKE_pose_channel_find_name(par_arma->pose, dg->name);

			if (m_dfnrToPC[i] && m_dfnrToPC[i]->bone->flag & BONE_NO_DEFORM)
				m_dfnrToPC[i] = NULL;
		}
	}

	for (int i = 0; i < defbase_tot; ++i) {
		if (m_dfnrToPC[i] != NULL)
			memcpy(m_poseMatrices+(i*16), m_dfnrToPC[i]->chan_mat, 16*sizeof(float));
	}


	GPUShader *shader = (GPUShader *)rasty->GetCurrentProgram();
	m_shader = shader;

	int loc = GPU_shader_get_uniform(shader, "useshwskin");
	GPU_shader_uniform_int(shader, loc, 1);

	loc = GPU_shader_get_uniform(shader, "bonematrices");
	GPU_shader_uniform_vector(shader, loc, 16, defbase_tot, m_poseMatrices);
}


void BL_SkinDeformer::BeginHandleGPUAttribs(RAS_DisplayArray *array)
{
	if (m_armobj->GetVertDeformType() != ARM_VDEF_BGE_GPU)
		return;

	SkinVertData *skinverts = m_skinVertData[array];

	if (skinverts == NULL) {
		m_skinVertData[array] = skinverts = new SkinVertData[array->m_vertex.size()];

		vector<RAS_TexVert>::iterator vit;
		int i = 0;
		for (vit = array->m_vertex.begin(); vit != array->m_vertex.end(); ++i, ++vit) {
			MDeformVert* dv = &m_bmesh->dvert[vit->getOrigIndex()];
			if (dv->totweight <= 4) {
				// We have no more than four weights, just copy them over.
				skinverts[i].num_bones = dv->totweight;

				MDeformWeight *dw = dv->dw;
				for (int j = 0; j < dv->totweight && j < 4; ++j, ++dw) {
					skinverts[i].indexes[j] = dw->def_nr;
					skinverts[i].weights[j] = dw->weight;
				}
			} else {
				// We have more than four weights, pick the four most influential bones
				float indexes[4] = {-1.f};
				float weights[4] = {0.f};

				int j;
				for (j = 0; j < 4; ++j) {
					float maxval = -1;
					float maxidx = -1;

					MDeformWeight *dw = dv->dw;
					for (int k = 0; k < dv->totweight; ++k, ++dw) {
						if (dw->weight > maxval && !ELEM4(dw->def_nr, indexes[0], indexes[1], indexes[2], indexes[3])) {
							maxval = dw->weight;
							maxidx = dw->def_nr;
						}
					}

					if (maxval <= 0) break;

					indexes[j] = maxidx;
					weights[j] = maxval;

					dw = dv->dw;
				}

				skinverts[i].num_bones = j;

				normalize_vn(weights, 4);

				memcpy(skinverts[i].indexes, indexes, 4*sizeof(float));
				memcpy(skinverts[i].weights, weights, 4*sizeof(float));
			}
		}
	}

	GPU_material_bind_hwskinning_attrib(GPUMaterial *material, skinverts[0].weights, skinverts[0].indexes, skinverts[0].num_bones );
}

void BL_SkinDeformer::EndHandleGPUAttribs()
{
	if (m_armobj->GetVertDeformType() != ARM_VDEF_BGE_GPU)
		return;

	GPU_material_unbind_hwskinning_attrib(GPUMaterial *material)
}
