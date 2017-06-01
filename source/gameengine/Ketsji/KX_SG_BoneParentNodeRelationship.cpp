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

/** \file gameengine/Ketsji/KX_SG_BoneParentNodeRelationship.cpp
 *  \ingroup ketsji
 */


#include <iostream>

#include "BLI_utildefines.h"

#include "KX_SG_BoneParentNodeRelationship.h"

#include "MT_Matrix4x4.h"
#include "BL_ArmatureObject.h"


/**
 * Implementation of classes defined in KX_SG_BoneParentNodeRelationship.h
 */

/** 
 * first of all KX_SG_BoneParentRelation
 */

	KX_BoneParentRelation *
KX_BoneParentRelation::
New(Bone* bone
) {
	return new KX_BoneParentRelation(bone);
}

	bool
KX_BoneParentRelation::
UpdateChildCoordinates(
	SG_Node * child,
	const SG_Node * parent,
	bool& parentUpdated
) {
	BLI_assert(child != nullptr);
	
	// This way of accessing child coordinates is a bit cumbersome
	// be nice to have non constant reference access to these values.

	const MT_Vector3 & child_scale = child->GetLocalScale();
	const MT_Vector3 & child_pos = child->GetLocalPosition();
	const MT_Matrix3x3 & child_rotation = child->GetLocalOrientation();
	// we don't know if the armature has been updated or not, assume yes
	parentUpdated = true;

	// the childs world locations which we will update.
	
	MT_Vector3 child_w_scale;
	MT_Vector3 child_w_pos;
	MT_Matrix3x3 child_w_rotation;
	
	bool valid_parent_transform = false;
	
	if (parent)
	{
		BL_ArmatureObject *armature = (BL_ArmatureObject*)(parent->GetSGClientObject());
		if (armature) {
			MT_Transform boneTrans;
			if (armature->GetBoneTransform(m_bone, boneTrans)) {
				// Get the child's transform, and the bone matrix.
				MT_Transform child_transform(child_pos + MT_Vector3(0.0f, armature->GetBoneLength(m_bone), 0.0f), child_rotation, child_scale);

				// The child's world transform is parent * child
				child_transform = parent->GetWorldTransform() * boneTrans * child_transform;

				child->SetWorldScale(child_transform.normalize());
				child->SetWorldPosition(child_transform.getOrigin());
				child->SetWorldOrientation(child_transform.getBasis());

				valid_parent_transform = true;
			}
		}
	}
	if (!valid_parent_transform) {
		child->SetWorldFromLocalTransform();
	}
	child->ClearModified();
	// this node must always be updated, so reschedule it for next time
	child->ActivateRecheduleUpdateCallback();
	return valid_parent_transform;
}

	SG_ParentRelation *
KX_BoneParentRelation::
NewCopy(
) {
	KX_BoneParentRelation* bone_parent = new KX_BoneParentRelation(m_bone);
	return bone_parent;
}

KX_BoneParentRelation::
~KX_BoneParentRelation(
) {
	//nothing to do
}


KX_BoneParentRelation::
KX_BoneParentRelation(Bone* bone
)
: m_bone(bone)
{
	// nothing to do
}
