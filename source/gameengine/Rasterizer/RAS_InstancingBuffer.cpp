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

/** \file gameengine/Rasterizer/RAS_InstancingBuffer.cpp
 *  \ingroup bgerast
 */

#include "RAS_InstancingBuffer.h"
#include "RAS_Rasterizer.h"
#include "RAS_MeshUser.h"

extern "C" {
// To avoid include BKE_DerivedMesh.h.
typedef int (*DMSetMaterial)(int mat_nr, void *attribs);
	#include "GPU_buffers.h"
}

RAS_InstancingBuffer::RAS_InstancingBuffer(Attrib attribs)
	:m_vbo(nullptr),
	m_attribs(attribs)
{
}

RAS_InstancingBuffer::~RAS_InstancingBuffer()
{
	if (m_vbo) {
		GPU_buffer_free(m_vbo);
	}
}

void RAS_InstancingBuffer::Realloc(unsigned int size)
{
	// Offset of next memory block.
	uintptr_t offset = 0;

	// Compute memory block offsets.

	m_matrixOffset = offset;
	offset += MATRIX_MEMORY_SIZE * size;

	m_positionOffset = offset;
	offset += POSITION_MEMORY_SIZE * size;

	if (m_attribs & COLOR_ATTRIB) {
		m_colorOffset = offset;
		offset += COLOR_MEMORY_SIZE * size;
	}
	if (m_attribs & LAYER_ATTRIB) {
		m_layerOffset = offset;
		offset += LAYER_MEMORY_SIZE * size;
	}

	if (m_vbo) {
		GPU_buffer_free(m_vbo);
	}
	// Use next offset as memory size.
	m_vbo = GPU_buffer_alloc(offset);
}

void RAS_InstancingBuffer::Bind()
{
	GPU_buffer_bind(m_vbo, GPU_BINDING_ARRAY);
}

void RAS_InstancingBuffer::Unbind()
{
	GPU_buffer_unbind(m_vbo, GPU_BINDING_ARRAY);
}

void RAS_InstancingBuffer::Update(RAS_Rasterizer *rasty, int drawingmode, const RAS_MeshSlotList &meshSlots)
{
	const intptr_t buffer = (intptr_t)GPU_buffer_lock_stream(m_vbo, GPU_BINDING_ARRAY);
	const unsigned int count = meshSlots.size();

	// Pack matrix and position.
	for (unsigned int i = 0; i < count; ++i) {
		RAS_MeshSlot *ms = meshSlots[i];
		float mat[16];
		rasty->SetClientObject(ms->m_meshUser->GetClientObject());
		rasty->GetTransform(ms->m_meshUser->GetMatrix(), drawingmode, mat);

		float (&matrixData)[9] = *(float (*)[9])(buffer + m_matrixOffset + MATRIX_MEMORY_SIZE * i);
		matrixData[0] = mat[0];
		matrixData[1] = mat[4];
		matrixData[2] = mat[8];
		matrixData[3] = mat[1];
		matrixData[4] = mat[5];
		matrixData[5] = mat[9];
		matrixData[6] = mat[2];
		matrixData[7] = mat[6];
		matrixData[8] = mat[10];

		float (&positionData)[3] = *(float (*)[3])(buffer + m_positionOffset + POSITION_MEMORY_SIZE * i);
		positionData[0] = mat[12];
		positionData[1] = mat[13];
		positionData[2] = mat[14];
	}

	// Pack color.
	if (m_attribs & COLOR_ATTRIB) {
		for (unsigned int i = 0; i < count; ++i) {
			RAS_MeshSlot *ms = meshSlots[i];

			float (&colorData)[4] = *(float (*)[4])(buffer + m_colorOffset + COLOR_MEMORY_SIZE * i);
			ms->m_meshUser->GetColor().Pack(colorData);
		}
	}

	// Pack layer.
	if (m_attribs & LAYER_ATTRIB) {
		for (unsigned int i = 0; i < count; ++i) {
			RAS_MeshSlot *ms = meshSlots[i];

			unsigned int &layerData = *(unsigned int *)(buffer + m_layerOffset + LAYER_MEMORY_SIZE * i);
			layerData = ms->m_meshUser->GetLayer();
		}
	}

	GPU_buffer_unlock(m_vbo, GPU_BINDING_ARRAY);
}
