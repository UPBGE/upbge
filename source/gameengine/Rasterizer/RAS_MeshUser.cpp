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
 * The Original Code is: all of this file.
 *
 * Contributor(s): Porteries Tristan.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file RAS_MeshUser.cpp
 *  \ingroup bgerast
 */

#include "RAS_MeshUser.h"
#include "RAS_DisplayArrayBucket.h"
#include "RAS_BoundingBox.h"
#include "RAS_BatchGroup.h"

RAS_MeshUser::RAS_MeshUser(void *clientobj)
	:m_frontFace(true),
	m_color(MT_Vector4(0.0f, 0.0f, 0.0f, 0.0f)),
	m_matrix(NULL),
	m_boundingBox(NULL),
	m_clientObject(clientobj),
	m_batchGroup(NULL)
{
}

RAS_MeshUser::~RAS_MeshUser()
{
	m_meshSlots.clear();

	if (m_boundingBox) {
		m_boundingBox->RemoveUser();
	}

	if (m_batchGroup) {
		// Has the side effect to deference the batch group.
		m_batchGroup->SplitMeshUser(this);
	}
}

void RAS_MeshUser::AddMeshSlot(RAS_MeshSlot *meshSlot)
{
	m_meshSlots.push_back(meshSlot);
}

bool RAS_MeshUser::GetFrontFace() const
{
	return m_frontFace;
}

const MT_Vector4& RAS_MeshUser::GetColor() const
{
	return m_color;
}

float *RAS_MeshUser::GetMatrix() const
{
	return m_matrix;
}

RAS_BoundingBox *RAS_MeshUser::GetBoundingBox() const
{
	return m_boundingBox;
}

void *RAS_MeshUser::GetClientObject() const
{
	return m_clientObject;
}

RAS_MeshSlotList& RAS_MeshUser::GetMeshSlots()
{
	return m_meshSlots;
}

RAS_BatchGroup *RAS_MeshUser::GetBatchGroup() const
{
	return m_batchGroup;
}

void RAS_MeshUser::SetFrontFace(bool frontFace)
{
	m_frontFace = frontFace;
}

void RAS_MeshUser::SetColor(const MT_Vector4& color)
{
	m_color = color;
}

void RAS_MeshUser::SetMatrix(float *matrix)
{
	m_matrix = matrix;
}

void RAS_MeshUser::SetBoundingBox(RAS_BoundingBox *boundingBox)
{
	if (m_boundingBox) {
		m_boundingBox->RemoveUser();
	}

	m_boundingBox = boundingBox;

	if (m_boundingBox) {
		m_boundingBox->AddUser();
	}
}

void RAS_MeshUser::SetBatchGroup(RAS_BatchGroup *batchGroup)
{
	if (m_batchGroup) {
		m_batchGroup->RemoveMeshUser();
	}

	m_batchGroup = batchGroup;

	if (m_batchGroup) {
		m_batchGroup->AddMeshUser();
	}
}

void RAS_MeshUser::ActivateMeshSlots()
{
	for (RAS_MeshSlotList::iterator it = m_meshSlots.begin(), end = m_meshSlots.end(); it != end; ++it) {
		RAS_MeshSlot *ms = *it;
		ms->m_displayArrayBucket->ActivateMesh(ms);
	}
}
