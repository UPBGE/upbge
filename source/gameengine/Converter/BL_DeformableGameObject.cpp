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

/** \file gameengine/Converter/BL_DeformableGameObject.cpp
 *  \ingroup bgeconv
 */

#include "BL_DeformableGameObject.h"
#include "BL_ShapeDeformer.h"

BL_DeformableGameObject::BL_DeformableGameObject(void *sgReplicationInfo, SG_Callbacks callbacks)
	:KX_GameObject(sgReplicationInfo, callbacks),
	m_pDeformer(nullptr),
	m_lastframe(0.0)
{
}

BL_DeformableGameObject::~BL_DeformableGameObject()
{
	if (m_pDeformer) {
		delete m_pDeformer;
	}
}

CValue *BL_DeformableGameObject::GetReplica()
{
	BL_DeformableGameObject *replica = new BL_DeformableGameObject(*this);
	replica->ProcessReplica();
	return replica;
}

void BL_DeformableGameObject::ProcessReplica()
{
	KX_GameObject::ProcessReplica();

	if (m_pDeformer) {
		m_pDeformer = m_pDeformer->GetReplica();
	}
}

void BL_DeformableGameObject::Relink(std::map<SCA_IObject *, SCA_IObject *>& map)
{
	if (m_pDeformer) {
		m_pDeformer->Relink(map);
	}
	KX_GameObject::Relink(map);
}

double BL_DeformableGameObject::GetLastFrame() const
{
	return m_lastframe;
}

void BL_DeformableGameObject::UpdateLastFrame(double deltatime)
{
	m_lastframe += deltatime;
}

bool BL_DeformableGameObject::GetShape(std::vector<float> &shape)
{
	shape.clear();
	BL_ShapeDeformer *shape_deformer = dynamic_cast<BL_ShapeDeformer *>(m_pDeformer);
	if (shape_deformer) {
		// this check is normally superfluous: a shape deformer can only be created if the mesh
		// has relative keys
		Key *key = shape_deformer->GetKey();
		if (key && key->type == KEY_RELATIVE) {
			KeyBlock *kb;
			for (kb = (KeyBlock *)key->block.first; kb; kb = (KeyBlock *)kb->next) {
				shape.push_back(kb->curval);
			}
		}
	}
	return !shape.empty();
}

void BL_DeformableGameObject::SetDeformer(RAS_Deformer *deformer)
{
	// Make sure that the object doesn't already have a mesh user.
	BLI_assert(m_meshUser == nullptr);
	m_pDeformer = deformer;
}

RAS_Deformer *BL_DeformableGameObject::GetDeformer()
{
	return m_pDeformer;
}

bool BL_DeformableGameObject::IsDeformable() const
{
	return true;
}
