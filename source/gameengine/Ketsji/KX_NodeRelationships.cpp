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

/** \file gameengine/Ketsji/KX_NodeRelationships.cpp
 *  \ingroup ketsji
 */

#include "KX_NodeRelationships.h"

#include "SG_Node.h"

#include "BLI_utildefines.h"

KX_NormalParentRelation::~KX_NormalParentRelation()
{
}

KX_NormalParentRelation::KX_NormalParentRelation()
{
}

bool KX_NormalParentRelation::UpdateChildCoordinates(SG_Node *child, const SG_Node *parent, bool& parentUpdated)
{
	BLI_assert(child != nullptr);

	if (!parentUpdated && !child->IsModified()) {
		return false;
	}


	parentUpdated = true;

	if (parent) {
		const mt::mat3x4 trans = parent->GetWorldTransform() * child->GetLocalTransform();
		const mt::vec3 scale = trans.ScaleVector3D();
		const mt::vec3 invscale(1.0f / scale.x, 1.0f / scale.y, 1.0f / scale.z);
		const mt::vec3 pos = trans.TranslationVector3D();
		const mt::mat3 rot = trans.RotationMatrix().Scale(invscale);

		child->SetWorldScale(scale);
		child->SetWorldPosition(pos);
		child->SetWorldOrientation(rot);

	}
	else {
		child->SetWorldFromLocalTransform();
	}

	child->ClearModified();

	return true;
}

SG_ParentRelation *KX_NormalParentRelation::NewCopy()
{
	return new KX_NormalParentRelation();
}

KX_VertexParentRelation::~KX_VertexParentRelation()
{
}

KX_VertexParentRelation::KX_VertexParentRelation()
{
}

bool KX_VertexParentRelation::UpdateChildCoordinates(SG_Node *child, const SG_Node *parent, bool& parentUpdated)
{
	BLI_assert(child != nullptr);

	if (!parentUpdated && !child->IsModified()) {
		return false;
	}

	child->SetWorldScale(child->GetLocalScale());

	if (parent) {
		child->SetWorldPosition(child->GetLocalPosition() + parent->GetWorldPosition());
	}
	else {
		child->SetWorldPosition(child->GetLocalPosition());
	}

	child->SetWorldOrientation(child->GetLocalOrientation());
	child->ClearModified();

	return true;
}

SG_ParentRelation *KX_VertexParentRelation::NewCopy()
{
	return new KX_VertexParentRelation();
}

bool KX_VertexParentRelation::IsVertexRelation()
{
	return true;
}

KX_SlowParentRelation::KX_SlowParentRelation(float relaxation)
	:m_relax(relaxation),
	m_initialized(false)
{
}

KX_SlowParentRelation::~KX_SlowParentRelation()
{
}

bool KX_SlowParentRelation::UpdateChildCoordinates(SG_Node *child, const SG_Node *parent, bool& parentUpdated)
{
	BLI_assert(child != nullptr);

	// The child will move even if the parent is not.
	parentUpdated = true;

	if (parent) {
		/* This is a slow parent relation
		 * first compute the normal child world coordinates. */
		const mt::mat3x4 ntrans = parent->GetWorldTransform() * child->GetLocalTransform();
		const mt::vec3 nscale = ntrans.ScaleVector3D();
		const mt::vec3 ninvscale(1.0f / nscale.x, 1.0f / nscale.y, 1.0f / nscale.z);
		const mt::vec3 npos = ntrans.TranslationVector3D();
		const mt::mat3 nrot = ntrans.RotationMatrix().Scale(ninvscale);

		if (m_initialized) {
			// Get the current world transform.
			const mt::vec3& cscale = child->GetWorldScaling();
			const mt::vec3& cpos = child->GetWorldPosition();
			const mt::mat3& crot = child->GetWorldOrientation();

			/* Now 'interpolate' the normal coordinates with the last
			 * world coordinates to get the new world coordinates. */
			const float weight = 1.0f / (m_relax + 1.0f);
			const mt::vec3 scale = (m_relax * cscale + nscale) * weight;
			const mt::vec3 pos = (m_relax * cpos + npos) * weight;

			// For rotation we must go through quaternion.
			const mt::quat cquat = mt::quat::FromMatrix(crot).Normalized();
			const mt::quat nquat = mt::quat::FromMatrix(nrot).Normalized();
			const mt::mat3 rot = mt::quat::Slerp(cquat, nquat, weight).ToMatrix();

			child->SetWorldScale(scale);
			child->SetWorldPosition(pos);
			child->SetWorldOrientation(rot);
		}
		else {
			child->SetWorldFromLocalTransform();
			m_initialized = true;
		}
	}
	else {
		child->SetWorldFromLocalTransform();
	}

	child->ClearModified();
	// This node must always be updated, so reschedule it for next time.
	child->Reschedule();

	return true;
}

SG_ParentRelation *KX_SlowParentRelation::NewCopy()
{
	return new KX_SlowParentRelation(m_relax);
}

float KX_SlowParentRelation::GetTimeOffset()
{
	return m_relax;
}

void KX_SlowParentRelation::SetTimeOffset(float relaxation)
{
	m_relax = relaxation;
}


bool KX_SlowParentRelation::IsSlowRelation()
{
	return true;
}
