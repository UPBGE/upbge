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

/** \file gameengine/Ketsji/KX_BoneParentNodeRelationship.cpp
 *  \ingroup ketsji
 */

#include "KX_BoneParentNodeRelationship.h"

#include "BL_ArmatureObject.h"

#include "mathfu.h"

#include "BLI_utildefines.h"

KX_BoneParentRelation::KX_BoneParentRelation(Bone *bone)
	:m_bone(bone)
{
}

KX_BoneParentRelation::~KX_BoneParentRelation()
{
}

bool KX_BoneParentRelation::UpdateChildCoordinates(SG_Node *child, const SG_Node *parent, bool& parentUpdated)
{
	BLI_assert(child != nullptr);

	// We don't know if the armature has been updated or not, assume yes.
	parentUpdated = true;

	// The childs world locations which we will update.
	bool valid_parent_transform = false;

	if (parent) {
		BL_ArmatureObject *armature = (BL_ArmatureObject *)(parent->GetObject());
		if (armature) {
			mt::mat3x4 bonetrans;
			if (armature->GetBoneTransform(m_bone, bonetrans)) {
				const mt::vec3& cscale = child->GetLocalScale();
				const mt::vec3& cpos = child->GetLocalPosition();
				const mt::mat3& crot = child->GetLocalOrientation();

				// The child's world transform is parent * child
				const mt::mat3x4 ptrans = parent->GetWorldTransform() * bonetrans;

				// Get the child's transform, and the bone matrix.
				const mt::mat3x4 trans = ptrans *
				                         mt::mat3x4(crot, cpos + mt::vec3(0.0f, armature->GetBoneLength(m_bone), 0.0f), cscale);

				// Recompute the child transform components from the transform.
				const mt::vec3 scale = trans.ScaleVector3D();
				const mt::vec3 invscale(1.0f / scale.x, 1.0f / scale.y, 1.0f / scale.z);
				const mt::vec3 pos = trans.TranslationVector3D();
				const mt::mat3 rot = trans.RotationMatrix().Scale(invscale);

				child->SetWorldScale(scale);
				child->SetWorldPosition(pos);
				child->SetWorldOrientation(rot);

				valid_parent_transform = true;
			}
		}
	}

	if (!valid_parent_transform) {
		child->SetWorldFromLocalTransform();
	}

	child->ClearModified();
	// This node must always be updated, so reschedule it for next time.
	child->Reschedule();
	return valid_parent_transform;
}

SG_ParentRelation *KX_BoneParentRelation::NewCopy()
{
	KX_BoneParentRelation *bone_parent = new KX_BoneParentRelation(m_bone);
	return bone_parent;
}

