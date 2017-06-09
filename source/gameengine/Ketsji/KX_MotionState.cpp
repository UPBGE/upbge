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

/** \file gameengine/Ketsji/KX_MotionState.cpp
 *  \ingroup ketsji
 */

#include "KX_MotionState.h"
#include "SG_Node.h"

KX_MotionState::KX_MotionState(SG_Node *node)
	:m_node(node)
{
}

KX_MotionState::~KX_MotionState()
{
}

mt::vec3 KX_MotionState::GetWorldPosition() const
{
	return m_node->GetWorldPosition();
}

mt::vec3 KX_MotionState::GetWorldScaling() const
{
	return m_node->GetWorldScaling();
}

mt::mat3 KX_MotionState::GetWorldOrientation() const
{
	return m_node->GetWorldOrientation();
}

void KX_MotionState::SetWorldOrientation(const mt::mat3& ori)
{
	m_node->SetLocalOrientation(ori);
}

void KX_MotionState::SetWorldPosition(const mt::vec3& pos)
{
	m_node->SetLocalPosition(pos);
}

void KX_MotionState::SetWorldOrientation(const mt::quat& quat)
{
	m_node->SetLocalOrientation(quat.ToMatrix());
}

void KX_MotionState::CalculateWorldTransformations()
{
	//Not needed, will be done in KX_Scene::UpdateParents() after the physics simulation
	//bool parentUpdated = false;
	//m_node->ComputeWorldTransforms(nullptr, parentUpdated);
}


