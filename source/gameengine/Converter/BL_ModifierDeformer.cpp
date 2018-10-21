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
#include "BL_BlenderDataConversion.h"
#include <string>
#include "RAS_IMaterial.h"
#include "RAS_MaterialBucket.h"
#include "RAS_Mesh.h"
#include "RAS_MeshUser.h"
#include "RAS_BoundingBox.h"

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
		m_dm->needsFree = 1;
		m_dm->release(m_dm);
	}
}

bool BL_ModifierDeformer::HasCompatibleDeformer(Object *ob)
{
	if (!ob->modifiers.first) {
		return false;
	}
	// soft body cannot use mesh modifiers
	if ((ob->gameflag & OB_SOFT_BODY) != 0) {
		return false;
	}
	ModifierData *md;
	for (md = (ModifierData *)ob->modifiers.first; md; md = md->next) {
		if (modifier_dependsOnTime(md)) {
			continue;
		}
		if (!(md->mode & eModifierMode_Realtime)) {
			continue;
		}
		/* armature modifier are handled by SkinDeformer, not ModifierDeformer */
		if (md->type == eModifierType_Armature) {
			continue;
		}
		return true;
	}
	return false;
}

bool BL_ModifierDeformer::HasArmatureDeformer(Object *ob)
{
	if (!ob->modifiers.first) {
		return false;
	}

	ModifierData *md = (ModifierData *)ob->modifiers.first;
	if (md->type == eModifierType_Armature) {
		return true;
	}

	return false;
}

bool BL_ModifierDeformer::Update(void)
{
	bool bShapeUpdate = BL_ShapeDeformer::UpdateInternal(false);

	if (bShapeUpdate || m_lastModifierUpdate != m_lastFrame) {
		// static derived mesh are not updated
		if (m_dm == nullptr || m_bDynamic) {
			/* execute the modifiers */
			Object *blendobj = m_gameobj->GetBlenderObject();
			/* hack: the modifiers require that the mesh is attached to the object
			 * It may not be the case here because of replace mesh actuator */
			Mesh *oldmesh = (Mesh *)blendobj->data;
			blendobj->data = m_bmesh;
			/* execute the modifiers */
			DerivedMesh *dm = mesh_create_derived_no_virtual(m_scene, blendobj, (float(*)[3])m_transverts.data(), CD_MASK_MESH);
			/* restore object data */
			blendobj->data = oldmesh;
			/* free the current derived mesh and replace, (dm should never be nullptr) */
			if (m_dm) {
				m_dm->needsFree = 1;
				m_dm->release(m_dm);
			}
			m_dm = dm;
			// get rid of temporary data
			m_dm->needsFree = 0;
			m_dm->release(m_dm);
			DM_update_materials(m_dm, blendobj);

			UpdateBounds();
			UpdateTransverts();
		}
		m_lastModifierUpdate = m_lastFrame;
		bShapeUpdate = true;
	}

	return bShapeUpdate;
}

void BL_ModifierDeformer::UpdateBounds()
{
	float min[3], max[3];
	INIT_MINMAX(min, max);
	m_dm->getMinMax(m_dm, min, max);
	m_boundingBox->SetAabb(mt::vec3(min), mt::vec3(max));
}

void BL_ModifierDeformer::UpdateTransverts()
{
	if (!m_dm) {
		return;
	}

	const unsigned short nummat = m_slots.size();
	std::vector<BL_MeshMaterial> mats(nummat);

	for (unsigned short i = 0; i < nummat; ++i) {
		const DisplayArraySlot& slot = m_slots[i];
		RAS_MeshMaterial *meshmat = slot.m_meshMaterial;
		RAS_DisplayArray *array = slot.m_displayArray;
		array->Clear();

		RAS_IMaterial *mat = meshmat->GetBucket()->GetMaterial();
		mats[i] = {array, meshmat->GetBucket(), mat->IsVisible(), mat->IsTwoSided(), mat->IsCollider(), mat->IsWire()};
	}

	BL_ConvertDerivedMeshToArray(m_dm, m_bmesh, mats, m_mesh->GetLayersInfo());

	for (const DisplayArraySlot& slot : m_slots) {
		RAS_DisplayArray *array = slot.m_displayArray;
		array->NotifyUpdate(RAS_DisplayArray::SIZE_MODIFIED);
	}

	// Update object's AABB.
	if (m_gameobj->GetAutoUpdateBounds()) {
		UpdateBounds();
	}
}

void BL_ModifierDeformer::Apply(RAS_DisplayArray *array)
{
	Update();
}
