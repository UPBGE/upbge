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
#include "RAS_Deformer.h"

#include "BLI_hash.h"

RAS_MeshUser::RAS_MeshUser(void *clientobj, RAS_BoundingBox *boundingBox, RAS_Deformer *deformer)
	:m_layer((1 << 20) - 1),
	m_passIndex(0),
	m_random(BLI_hash_int_2d((uintptr_t)clientobj, 0) / ((float)0xFFFFFFFF)),
	m_frontFace(true),
	m_color(mt::zero4),
	m_boundingBox(boundingBox),
	m_clientObject(clientobj),
	m_batchGroup(nullptr),
	m_deformer(deformer)
{
	BLI_assert(m_boundingBox);
	m_boundingBox->AddUser();
}

RAS_MeshUser::~RAS_MeshUser()
{
	m_meshSlots.clear();

	m_boundingBox->RemoveUser();

	if (m_batchGroup) {
		// Has the side effect to deference the batch group.
		m_batchGroup->SplitMeshUser(this);
	}
}

void RAS_MeshUser::NewMeshSlot(RAS_DisplayArrayBucket *arrayBucket)
{
	m_meshSlots.emplace_back(this, arrayBucket);
}

unsigned int RAS_MeshUser::GetLayer() const
{
	return m_layer;
}

short RAS_MeshUser::GetPassIndex() const
{
	return m_passIndex;
}

float RAS_MeshUser::GetRandom() const
{
	return m_random;
}

bool RAS_MeshUser::GetFrontFace() const
{
	return m_frontFace;
}

const mt::vec4& RAS_MeshUser::GetColor() const
{
	return m_color;
}

const mt::mat4& RAS_MeshUser::GetMatrix() const
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

std::vector<RAS_MeshSlot>& RAS_MeshUser::GetMeshSlots()
{
	return m_meshSlots;
}

RAS_BatchGroup *RAS_MeshUser::GetBatchGroup() const
{
	return m_batchGroup;
}

RAS_Deformer *RAS_MeshUser::GetDeformer()
{
	return m_deformer.get();
}

void RAS_MeshUser::SetLayer(unsigned int layer)
{
	m_layer = layer;
}

void RAS_MeshUser::SetPassIndex(short index)
{
	m_passIndex = index;
}

void RAS_MeshUser::SetFrontFace(bool frontFace)
{
	m_frontFace = frontFace;
}

void RAS_MeshUser::SetColor(const mt::vec4& color)
{
	m_color = color;
}

void RAS_MeshUser::SetMatrix(const mt::mat4& matrix)
{
	m_matrix = matrix;
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
	for (RAS_MeshSlot& ms : m_meshSlots) {
		ms.m_displayArrayBucket->ActivateMesh(&ms);
	}
}
