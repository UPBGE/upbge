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
#include "RAS_IRasterizer.h"
#include "glew-mx.h"

RAS_InstancingBuffer::RAS_InstancingBuffer()
	:m_matrixOffset(NULL),
	m_positionOffset(NULL),
	m_colorOffset(NULL),
	m_stride(sizeof(RAS_InstancingBuffer::InstancingObject))
{
	glGenBuffersARB(1, &m_vbo);

	m_matrixOffset = (void *)((InstancingObject *)NULL)->matrix;
	m_positionOffset = (void *)((InstancingObject *)NULL)->position;
	m_colorOffset = (void *)((InstancingObject *)NULL)->color;
}

RAS_InstancingBuffer::~RAS_InstancingBuffer()
{
	glDeleteBuffersARB(1, &m_vbo);
}

void RAS_InstancingBuffer::Bind()
{
	glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
}

void RAS_InstancingBuffer::Unbind()
{
	glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void RAS_InstancingBuffer::Update(RAS_IRasterizer *rasty, int drawingmode, RAS_MeshSlotList &meshSlots)
{
	glBufferData(GL_ARRAY_BUFFER, sizeof(InstancingObject) * meshSlots.size(), 0, GL_DYNAMIC_DRAW);
	InstancingObject *buffer = (InstancingObject *)glMapBuffer(GL_ARRAY_BUFFER, GL_WRITE_ONLY);

	for (unsigned int i = 0, size = meshSlots.size(); i < size; ++i) {
		RAS_MeshSlot *ms = meshSlots[i];
		InstancingObject& data = buffer[i];
		float mat[16];
		rasty->SetClientObject(ms->m_clientObj);
		rasty->GetTransform(ms->m_OpenGLMatrix, drawingmode, mat);
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

		data.color[0] = ms->m_RGBAcolor[0] * 255.0f;
		data.color[1] = ms->m_RGBAcolor[1] * 255.0f;
		data.color[2] = ms->m_RGBAcolor[2] * 255.0f;
		data.color[3] = ms->m_RGBAcolor[3] * 255.0f;

		// make this mesh slot culled automatically for next frame
		// it will be culled out by frustrum culling
		ms->SetCulled(true);
	}

	glUnmapBuffer(GL_ARRAY_BUFFER);
}
