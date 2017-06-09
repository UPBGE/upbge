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

RAS_InstancingBuffer::RAS_InstancingBuffer()
	:m_vbo(nullptr),
	m_matrixOffset(nullptr),
	m_positionOffset(nullptr),
	m_colorOffset(nullptr),
	m_stride(sizeof(RAS_InstancingBuffer::InstancingObject))
{
	m_matrixOffset = (void *)((InstancingObject *)nullptr)->matrix;
	m_positionOffset = (void *)((InstancingObject *)nullptr)->position;
	m_colorOffset = (void *)((InstancingObject *)nullptr)->color;
}

RAS_InstancingBuffer::~RAS_InstancingBuffer()
{
	if (m_vbo) {
		GPU_buffer_free(m_vbo);
	}
}

void RAS_InstancingBuffer::Realloc(unsigned int size)
{
	if (m_vbo) {
		GPU_buffer_free(m_vbo);
	}
	m_vbo = GPU_buffer_alloc(m_stride * size);
}

void RAS_InstancingBuffer::Bind()
{
	GPU_buffer_bind(m_vbo, GPU_BINDING_ARRAY);
}

void RAS_InstancingBuffer::Unbind()
{
	GPU_buffer_unbind(m_vbo, GPU_BINDING_ARRAY);
}

void RAS_InstancingBuffer::Update(RAS_Rasterizer *rasty, int drawingmode, RAS_MeshSlotList &meshSlots)
{
	InstancingObject *buffer = (InstancingObject *)GPU_buffer_lock_stream(m_vbo, GPU_BINDING_ARRAY);

	for (unsigned int i = 0, size = meshSlots.size(); i < size; ++i) {
		RAS_MeshSlot *ms = meshSlots[i];
		InstancingObject& data = buffer[i];
		float mat[16];
		rasty->SetClientObject(ms->m_meshUser->GetClientObject());
		rasty->GetTransform(ms->m_meshUser->GetMatrix(), drawingmode, mat);
		data.matrix[0] = mat[0];
		data.matrix[1] = mat[4];
		data.matrix[2] = mat[8];
		data.matrix[3] = mat[1];
		data.matrix[4] = mat[5];
		data.matrix[5] = mat[9];
		data.matrix[6] = mat[2];
		data.matrix[7] = mat[6];
		data.matrix[8] = mat[10];
		data.position[0] = mat[12];
		data.position[1] = mat[13];
		data.position[2] = mat[14];

		const mt::vec4& color = ms->m_meshUser->GetColor();
		data.color[0] = color[0] * 255.0f;
		data.color[1] = color[1] * 255.0f;
		data.color[2] = color[2] * 255.0f;
		data.color[3] = color[3] * 255.0f;
	}

	GPU_buffer_unlock(m_vbo, GPU_BINDING_ARRAY);
}
