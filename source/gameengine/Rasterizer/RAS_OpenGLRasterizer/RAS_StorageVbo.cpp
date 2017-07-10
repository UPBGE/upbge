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

#include "RAS_StorageVbo.h"
#include "RAS_DisplayArray.h"

RAS_StorageVbo::RAS_StorageVbo(RAS_IDisplayArray *array)
	:m_data(array)
{
	m_size = m_data->GetVertexCount();
	m_indices = m_data->GetIndexCount();
	m_stride = m_data->GetVertexMemorySize();

	m_mode = m_data->GetOpenGLPrimitiveType();

	// Generate Buffers
	glGenBuffersARB(1, &m_ibo);
	glGenBuffersARB(1, &m_vbo_id);

	// Fill the buffers with initial data
	AllocData();
}

RAS_StorageVbo::~RAS_StorageVbo()
{
	glDeleteBuffersARB(1, &m_ibo);
	glDeleteBuffersARB(1, &m_vbo_id);
}

void RAS_StorageVbo::BindVertexBuffer()
{
	glBindBufferARB(GL_ARRAY_BUFFER_ARB, m_vbo_id);
}

void RAS_StorageVbo::UnbindVertexBuffer()
{
	glBindBufferARB(GL_ARRAY_BUFFER_ARB, 0);
}

void RAS_StorageVbo::BindIndexBuffer()
{
	glBindBufferARB(GL_ELEMENT_ARRAY_BUFFER_ARB, m_ibo);
}

void RAS_StorageVbo::UnbindIndexBuffer()
{
	glBindBufferARB(GL_ELEMENT_ARRAY_BUFFER_ARB, 0);
}

void RAS_StorageVbo::AllocData()
{
	BindVertexBuffer();
	glBufferDataARB(GL_ARRAY_BUFFER_ARB, m_stride * m_size, m_data->GetVertexPointer(), GL_DYNAMIC_DRAW_ARB);
	UnbindVertexBuffer();

	BindIndexBuffer();
	glBufferDataARB(GL_ELEMENT_ARRAY_BUFFER_ARB, m_indices * sizeof(GLuint), m_data->GetIndexPointer(), GL_DYNAMIC_DRAW_ARB);
	UnbindIndexBuffer();
}

void RAS_StorageVbo::UpdateVertexData()
{
	BindVertexBuffer();
	glBufferSubDataARB(GL_ARRAY_BUFFER_ARB, 0, m_stride * m_size, m_data->GetVertexPointer());
	UnbindVertexBuffer();
}

unsigned int *RAS_StorageVbo::GetIndexMap()
{
	void *buffer = glMapBufferRange(GL_ELEMENT_ARRAY_BUFFER, 0, m_indices * sizeof(GLuint), GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);

	return (unsigned int *)buffer;
}

void RAS_StorageVbo::FlushIndexMap()
{
	glUnmapBuffer(GL_ELEMENT_ARRAY_BUFFER);
}

void RAS_StorageVbo::IndexPrimitives()
{
	glDrawElements(m_mode, m_indices, GL_UNSIGNED_INT, 0);
}

void RAS_StorageVbo::IndexPrimitivesInstancing(unsigned int numinstance)
{
	glDrawElementsInstancedARB(m_mode, m_indices, GL_UNSIGNED_INT, 0, numinstance);
}

void RAS_StorageVbo::IndexPrimitivesBatching(const std::vector<void *>& indices, const std::vector<int>& counts)
{
	glMultiDrawElements(m_mode, counts.data(), GL_UNSIGNED_INT, (const void **)indices.data(), counts.size());
}
