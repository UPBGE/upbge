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

/** \file gameengine/Converter/BL_ModifierDeformer.cpp
 *  \ingroup bgeconv
 */

#ifdef _MSC_VER
#  pragma warning (disable:4786)
#endif

#include "MEM_guardedalloc.h"
#include "BL_ModifierDeformer.h"
#include "STR_HashedString.h"
#include "RAS_IPolygonMaterial.h"
#include "RAS_MeshObject.h"
#include "PHY_IGraphicController.h"

#include "DNA_armature_types.h"
#include "DNA_action_types.h"
#include "DNA_key_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_curve_types.h"
#include "DNA_modifier_types.h"
#include "DNA_scene_types.h"
#include "BLI_utildefines.h"
#include "BKE_armature.h"
#include "BKE_action.h"
#include "BKE_key.h"
#include "BKE_ipo.h"
#include "MT_Vector3.h"

extern "C" {
	#include "BKE_customdata.h"
	#include "BKE_DerivedMesh.h"
	#include "BKE_lattice.h"
	#include "BKE_modifier.h"
}

#include "BLI_blenlib.h"
#include "BLI_math.h"

BL_ModifierDeformer::~BL_ModifierDeformer()
{
	if (m_dm) {
		// deformedOnly is used as a user counter
		if (--m_dm->deformedOnly == 0) {
			m_dm->needsFree = 1;
			m_dm->release(m_dm);
		}
	}
}

RAS_Deformer *BL_ModifierDeformer::GetReplica()
{
	BL_ModifierDeformer *result;

	result = new BL_ModifierDeformer(*this);
	result->ProcessReplica();
	return result;
}

void BL_ModifierDeformer::ProcessReplica()
{
	/* Note! - This is not inherited from PyObjectPlus */
	BL_ShapeDeformer::ProcessReplica();
	if (m_dm) {
		// by default try to reuse mesh, deformedOnly is used as a user count
		m_dm->deformedOnly++;
	}
	// this will force an update and if the mesh cannot be reused, a new one will be created
	m_lastModifierUpdate = -1.0;
}

bool BL_ModifierDeformer::HasCompatibleDeformer(Object *ob)
{
	if (!ob->modifiers.first)
		return false;
	// soft body cannot use mesh modifiers
	if ((ob->gameflag & OB_SOFT_BODY) != 0)
		return false;
	ModifierData *md;
	for (md = (ModifierData *)ob->modifiers.first; md; md = md->next) {
		if (modifier_dependsOnTime(md))
			continue;
		if (!(md->mode & eModifierMode_Realtime))
			continue;
		/* armature modifier are handled by SkinDeformer, not ModifierDeformer */
		if (md->type == eModifierType_Armature)
			continue;
		return true;
	}
	return false;
}

bool BL_ModifierDeformer::HasArmatureDeformer(Object *ob)
{
	if (!ob->modifiers.first)
		return false;

	ModifierData *md = (ModifierData *)ob->modifiers.first;
	if (md->type == eModifierType_Armature)
		return true;

	return false;
}

// return a deformed mesh that supports mapping (with a valid CD_ORIGINDEX layer)
DerivedMesh *BL_ModifierDeformer::GetPhysicsMesh()
{
	/* we need to compute the deformed mesh taking into account the current
	 * shape and skin deformers, we cannot just call mesh_create_derived_physics()
	 * because that would use the m_transvers already deformed previously by BL_ModifierDeformer::Update(),
	 * so restart from scratch by forcing a full update the shape/skin deformers
	 * (will do nothing if there is no such deformer) */
	BL_ShapeDeformer::ForceUpdate();
	BL_ShapeDeformer::Update();
	// now apply the modifiers but without those that don't support mapping
	Object *blendobj = m_gameobj->GetBlendObject();
	/* hack: the modifiers require that the mesh is attached to the object
	 * It may not be the case here because of replace mesh actuator */
	Mesh *oldmesh = (Mesh *)blendobj->data;
	blendobj->data = m_bmesh;
	DerivedMesh *dm = mesh_create_derived_physics(m_scene, blendobj, m_transverts, CD_MASK_MESH);
	/* restore object data */
	blendobj->data = oldmesh;

	// Some meshes with modifiers returns 0 polys, call DM_ensure_tessface avoid this.
	DM_ensure_tessface(dm);

	/* m_transverts is correct here (takes into account deform only modifiers) */
	/* the derived mesh returned by this function must be released by the caller !!! */
	return dm;
}

bool BL_ModifierDeformer::Update(void)
{
	bool bShapeUpdate = BL_ShapeDeformer::Update();

	if (bShapeUpdate || m_lastModifierUpdate != m_gameobj->GetLastFrame()) {
		// static derived mesh are not updated
		if (m_dm == NULL || m_bDynamic) {
			// Set to true if it's the first time Update() function is called.
			const bool initialize = (m_dm == NULL);
			/* execute the modifiers */
			Object *blendobj = m_gameobj->GetBlendObject();
			/* hack: the modifiers require that the mesh is attached to the object
			 * It may not be the case here because of replace mesh actuator */
			Mesh *oldmesh = (Mesh *)blendobj->data;
			blendobj->data = m_bmesh;
			/* execute the modifiers */
			DerivedMesh *dm = mesh_create_derived_no_virtual(m_scene, blendobj, m_transverts, CD_MASK_MESH);
			/* restore object data */
			blendobj->data = oldmesh;
			/* free the current derived mesh and replace, (dm should never be NULL) */
			if (m_dm != NULL) {
				// HACK! use deformedOnly as a user counter
				if (--m_dm->deformedOnly == 0) {
					m_dm->needsFree = 1;
					m_dm->release(m_dm);
				}
			}
			m_dm = dm;
			// get rid of temporary data
			m_dm->needsFree = 0;
			m_dm->release(m_dm);
			// HACK! use deformedOnly as a user counter
			m_dm->deformedOnly = 1;
			DM_update_materials(m_dm, blendobj);

			// Some meshes with modifiers returns 0 polys, call DM_ensure_tessface avoid this.
			DM_ensure_tessface(m_dm);

			// Update object's AABB.
			if (initialize || m_gameobj->GetAutoUpdateBounds()) {
				float min[3], max[3];
				INIT_MINMAX(min, max);
				m_dm->getMinMax(m_dm, min, max);
				m_aabbMin = MT_Vector3(min);
				m_aabbMax = MT_Vector3(max);
			}
		}
		m_lastModifierUpdate = m_gameobj->GetLastFrame();
		bShapeUpdate = true;

		int nmat = m_pMeshObject->NumMaterials();
		for (int imat = 0; imat < nmat; imat++) {
			RAS_MeshMaterial *mmat = m_pMeshObject->GetMeshMaterial(imat);
			RAS_MeshSlot *slot = mmat->m_slots[(void *)m_gameobj->getClientInfo()];
			if (!slot) {
				continue;
			}
			slot->m_pDerivedMesh = m_dm;
		}
	}
	return bShapeUpdate;
}

bool BL_ModifierDeformer::Apply(RAS_IPolyMaterial *mat)
{
	if (!Update())
		return false;

	return true;
}
