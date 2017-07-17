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
	glGenBuffers(1, &m_ibo);
	glGenBuffers(1, &m_vbo_id);

	// Fill the buffers with initial data
	AllocData();
}

RAS_StorageVbo::~RAS_StorageVbo()
{
	glDeleteBuffers(1, &m_ibo);
	glDeleteBuffers(1, &m_vbo_id);
}

void RAS_StorageVbo::BindVertexBuffer()
{
	glBindBuffer(GL_ARRAY_BUFFER, m_vbo_id);
}

void RAS_StorageVbo::UnbindVertexBuffer()
{
	glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void RAS_StorageVbo::BindIndexBuffer()
{
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ibo);
}

void RAS_StorageVbo::UnbindIndexBuffer()
{
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

void RAS_StorageVbo::AllocData()
{
	BindVertexBuffer();
	glBufferData(GL_ARRAY_BUFFER, m_stride * m_size, m_data->GetVertexPointer(), GL_DYNAMIC_DRAW);
	UnbindVertexBuffer();

	BindIndexBuffer();
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, m_indices * sizeof(GLuint), m_data->GetIndexPointer(), GL_DYNAMIC_DRAW);
	UnbindIndexBuffer();
}

void RAS_StorageVbo::UpdateVertexData()
{
	BindVertexBuffer();
	glBufferSubData(GL_ARRAY_BUFFER, 0, m_stride * m_size, m_data->GetVertexPointer());
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
	glDrawElementsInstanced(m_mode, m_indices, GL_UNSIGNED_INT, 0, numinstance);
}

void RAS_StorageVbo::IndexPrimitivesBatching(const std::vector<void *>& indices, const std::vector<int>& counts)
{
	glMultiDrawElements(m_mode, counts.data(), GL_UNSIGNED_INT, (const void **)indices.data(), counts.size());
}
