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

	/* This way of accessing child coordinates is a bit cumbersome
	 * be nice to have non constant reference access to these values. */

	const mt::vec3 & child_scale = child->GetLocalScale();
	const mt::vec3 & child_pos = child->GetLocalPosition();
	const mt::mat3 & child_rotation = child->GetLocalOrientation();
	// We don't know if the armature has been updated or not, assume yes.
	parentUpdated = true;

	// The childs world locations which we will update.

	mt::vec3 child_w_scale;
	mt::vec3 child_w_pos;
	mt::mat3 child_w_rotation;

	bool valid_parent_transform = false;

	if (parent) {
		BL_ArmatureObject *armature = (BL_ArmatureObject *)(parent->GetSGClientObject());
		if (armature) {
			mt::mat3x4 bone_transform;
			if (armature->GetBoneMatrix(m_bone, bone_transform)) {
				// Get the child's transform, and the bone matrix.
				mt::mat3x4 child_transform(child_rotation, child_pos + mt::vec3(0.0f, armature->GetBoneLength(m_bone), 0.0f), child_scale);

				// The child's world transform is parent * child
				mt::mat3x4 parent_transform = parent->GetWorldTransform() * bone_transform;
				child_transform = parent_transform * child_transform;

				// Recompute the child transform components from the transform.
				child_w_rotation = child_transform.RotationMatrix();
				child_w_scale = child_w_rotation.ScaleVector3D();
				child_w_rotation = child_w_rotation.Scale(mt::vec3(1.0f / child_w_scale.x, 1.0f / child_w_scale.y, 1.0f / child_w_scale.z));
				child_w_pos = child_transform.TranslationVector3D();

				valid_parent_transform = true;
			}
		}
	}

	if (valid_parent_transform) {
		child->SetWorldScale(child_w_scale);
		child->SetWorldPosition(child_w_pos);
		child->SetWorldOrientation(child_w_rotation);
	}
	else {
		child->SetWorldFromLocalTransform();
	}

	child->ClearModified();
	// This node must always be updated, so reschedule it for next time.
	child->ActivateRecheduleUpdateCallback();
	return valid_parent_transform;
}

SG_ParentRelation *KX_BoneParentRelation::NewCopy()
{
	KX_BoneParentRelation *bone_parent = new KX_BoneParentRelation(m_bone);
	return bone_parent;
}

