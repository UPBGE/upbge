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

#include "RAS_StorageVBO.h"
#include "RAS_DisplayArray.h"
#include "RAS_MeshObject.h"
#include "RAS_Deformer.h"

#include "GPU_glew.h"

VBO::VBO(RAS_IDisplayArray *array, bool instancing)
	:m_data(array),
	m_useVao(!instancing && GLEW_ARB_vertex_array_object)
{
	m_stride = m_data->GetVertexMemorySize();

	m_mode = m_data->GetOpenGLPrimitiveType();

	// Generate Buffers
	glGenBuffersARB(1, &m_ibo);
	glGenBuffersARB(1, &m_vbo_id);

	for (unsigned short i = 0; i < RAS_Rasterizer::RAS_DRAW_MAX; ++i) {
		m_vaos[i] = 0;
		m_vaoInitialized[i] = false;
	}

	// Fill the buffers with initial data
	UpdateSize();

	// Establish offsets
	m_vertex_offset = m_data->GetVertexXYZOffset();
	m_normal_offset = m_data->GetVertexNormalOffset();
	m_tangent_offset = m_data->GetVertexTangentOffset();
	m_color_offset = m_data->GetVertexColorOffset();
	m_uv_offset = m_data->GetVertexUVOffset();
}

VBO::~VBO()
{
	glDeleteBuffersARB(1, &m_ibo);
	glDeleteBuffersARB(1, &m_vbo_id);
	for (unsigned short i = 0; i < RAS_Rasterizer::RAS_DRAW_MAX; ++i) {
		if (m_vaos[i]) {
			glDeleteVertexArrays(1, &m_vaos[i]);
		}
	}
}

void VBO::UpdateVertexData()
{
	glBindBufferARB(GL_ARRAY_BUFFER_ARB, m_vbo_id);
	glBufferSubDataARB(GL_ARRAY_BUFFER_ARB, 0, m_stride * m_size, m_data->GetVertexPointer());
	glBindBufferARB(GL_ARRAY_BUFFER_ARB, 0);
}

void VBO::UpdateSize()
{
	m_size = m_data->GetVertexCount();
	m_indices = m_data->GetPrimitiveIndexCount();

	glBindBufferARB(GL_ARRAY_BUFFER_ARB, m_vbo_id);
	glBufferDataARB(GL_ARRAY_BUFFER_ARB, m_stride * m_size, m_data->GetVertexPointer(), GL_DYNAMIC_DRAW_ARB);
	glBindBufferARB(GL_ARRAY_BUFFER_ARB, 0);

	glBindBufferARB(GL_ELEMENT_ARRAY_BUFFER_ARB, m_ibo);
	glBufferDataARB(GL_ELEMENT_ARRAY_BUFFER_ARB, m_indices * sizeof(GLuint), m_data->GetPrimitiveIndexPointer(), GL_DYNAMIC_DRAW_ARB);
	glBindBufferARB(GL_ELEMENT_ARRAY_BUFFER_ARB, 0);
}

unsigned int *VBO::GetIndexMap()
{
	void *buffer = glMapBufferRange(GL_ELEMENT_ARRAY_BUFFER, 0, m_indices * sizeof(GLuint), GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);

	return (unsigned int *)buffer;
}

void VBO::FlushIndexMap()
{
	glUnmapBuffer(GL_ELEMENT_ARRAY_BUFFER);
}

void VBO::Bind(RAS_Rasterizer::StorageAttribs *storageAttribs, RAS_Rasterizer::DrawType drawingmode)
{
	if (m_useVao) {
		if (!m_vaos[drawingmode]) {
			// Generate Vertex Array Object
			glGenVertexArrays(1, &m_vaos[drawingmode]);
		}
		glBindVertexArray(m_vaos[drawingmode]);
		if (m_vaoInitialized[drawingmode]) {
			return;
		}
		m_vaoInitialized[drawingmode] = true;
	}

	bool wireframe = (drawingmode == RAS_Rasterizer::RAS_WIREFRAME);

	// Bind buffers
	glBindBufferARB(GL_ELEMENT_ARRAY_BUFFER_ARB, m_ibo);
	glBindBufferARB(GL_ARRAY_BUFFER_ARB, m_vbo_id);

	// Vertexes
	glEnableClientState(GL_VERTEX_ARRAY);
	glVertexPointer(3, GL_FLOAT, m_stride, m_vertex_offset);

	// Normals
	glEnableClientState(GL_NORMAL_ARRAY);
	glNormalPointer(GL_FLOAT, m_stride, m_normal_offset);

	// Colors
	if (!wireframe) {
		glEnableClientState(GL_COLOR_ARRAY);
		glColorPointer(4, GL_UNSIGNED_BYTE, m_stride, m_color_offset);
	}

	for (const std::pair<int, RAS_Rasterizer::TexCoGen> pair : storageAttribs->texcos) {
		const int unit = pair.first;
		switch (pair.second) {
			case RAS_Rasterizer::RAS_TEXCO_ORCO:
			case RAS_Rasterizer::RAS_TEXCO_GLOB:
			{
				glClientActiveTexture(GL_TEXTURE0_ARB + unit);
				glEnableClientState(GL_TEXTURE_COORD_ARRAY);
				glTexCoordPointer(3, GL_FLOAT, m_stride, m_vertex_offset);
				break;
			}
			case RAS_Rasterizer::RAS_TEXCO_UV:
			{
				glClientActiveTexture(GL_TEXTURE0_ARB + unit);
				glEnableClientState(GL_TEXTURE_COORD_ARRAY);
				glTexCoordPointer(2, GL_FLOAT, m_stride, (void *)((intptr_t)m_uv_offset + (sizeof(GLfloat) * 2 * unit)));
				break;
			}
			case RAS_Rasterizer::RAS_TEXCO_NORM:
			{
				glClientActiveTexture(GL_TEXTURE0_ARB + unit);
				glEnableClientState(GL_TEXTURE_COORD_ARRAY);
				glTexCoordPointer(3, GL_FLOAT, m_stride, m_normal_offset);
				break;
			}
			case RAS_Rasterizer::RAS_TEXTANGENT:
			{
				glClientActiveTexture(GL_TEXTURE0_ARB + unit);
				glEnableClientState(GL_TEXTURE_COORD_ARRAY);
				glTexCoordPointer(4, GL_FLOAT, m_stride, m_tangent_offset);
				break;
			}
			default:
				break;
		}
	}
	glClientActiveTextureARB(GL_TEXTURE0_ARB);

	for (const std::pair<int, RAS_Rasterizer::TexCoGen> pair : storageAttribs->attribs) {
		const int unit = pair.first;
		switch (pair.second) {
			case RAS_Rasterizer::RAS_TEXCO_ORCO:
			case RAS_Rasterizer::RAS_TEXCO_GLOB:
			{
				glVertexAttribPointerARB(unit, 3, GL_FLOAT, GL_FALSE, m_stride, m_vertex_offset);
				glEnableVertexAttribArrayARB(unit);
				break;
			}
			case RAS_Rasterizer::RAS_TEXCO_UV:
			{
				glVertexAttribPointerARB(unit, 2, GL_FLOAT, GL_FALSE, m_stride, (void *)((intptr_t)m_uv_offset + storageAttribs->layers[unit] * sizeof(GLfloat) * 2));
				glEnableVertexAttribArrayARB(unit);
				break;
			}
			case RAS_Rasterizer::RAS_TEXCO_NORM:
			{
				glVertexAttribPointerARB(unit, 2, GL_FLOAT, GL_FALSE, m_stride, m_normal_offset);
				glEnableVertexAttribArrayARB(unit);
				break;
			}
			case RAS_Rasterizer::RAS_TEXTANGENT:
			{
				glVertexAttribPointerARB(unit, 4, GL_FLOAT, GL_FALSE, m_stride, m_tangent_offset);
				glEnableVertexAttribArrayARB(unit);
				break;
			}
			case RAS_Rasterizer::RAS_TEXCO_VCOL:
			{
				glVertexAttribPointerARB(unit, 4, GL_UNSIGNED_BYTE, GL_TRUE, m_stride, (void *)((intptr_t)m_color_offset + storageAttribs->layers[unit] * sizeof(GLuint)));
				glEnableVertexAttribArrayARB(unit);
				break;
			}
			default:
				break;
		}
	}

	/* VAO don't track the VBO state and the attributes don't need a bound VBO to be used in a render.
	 * So we unbind the VBO here because they will not be unbound in VBO::Unbind. */
	if (m_useVao) {
		glBindBufferARB(GL_ARRAY_BUFFER_ARB, 0);
	}
}

void VBO::Unbind(RAS_Rasterizer::StorageAttribs *storageAttribs, RAS_Rasterizer::DrawType drawingmode)
{
	if (m_useVao) {
		glBindVertexArray(0);
		return;
	}

	bool wireframe = (drawingmode == RAS_Rasterizer::RAS_WIREFRAME);

	glDisableClientState(GL_VERTEX_ARRAY);
	glDisableClientState(GL_NORMAL_ARRAY);
	if (!wireframe) {
		glDisableClientState(GL_COLOR_ARRAY);
	}
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);

	for (const std::pair<int, RAS_Rasterizer::TexCoGen> pair : storageAttribs->texcos) {
		glClientActiveTextureARB(GL_TEXTURE0_ARB + pair.first);
		glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	}
	glClientActiveTextureARB(GL_TEXTURE0_ARB);

	for (const std::pair<int, RAS_Rasterizer::TexCoGen> pair : storageAttribs->attribs) {
		glDisableVertexAttribArrayARB(pair.first);
	}

	glBindBufferARB(GL_ARRAY_BUFFER_ARB, 0);
	glBindBufferARB(GL_ELEMENT_ARRAY_BUFFER_ARB, 0);
}

void VBO::Draw()
{
	glDrawElements(m_mode, m_indices, GL_UNSIGNED_INT, 0);
}

void VBO::DrawInstancing(unsigned int numinstance)
{
	glDrawElementsInstancedARB(m_mode, m_indices, GL_UNSIGNED_INT, 0, numinstance);
}

void VBO::DrawBatching(const std::vector<void *>& indices, const std::vector<int>& counts)
{
	glMultiDrawElements(m_mode, counts.data(), GL_UNSIGNED_INT, (const void **)indices.data(), counts.size());
}

RAS_StorageVBO::RAS_StorageVBO(RAS_Rasterizer::StorageAttribs *storageAttribs)
	:m_storageAttribs(storageAttribs)
{
}

RAS_StorageVBO::~RAS_StorageVBO()
{
}

RAS_IStorageInfo *RAS_StorageVBO::GetStorageInfo(RAS_IDisplayArray *array, bool instancing)
{
	VBO *vbo = new VBO(array, instancing);
	return vbo;
}

void RAS_StorageVBO::BindPrimitives(RAS_Rasterizer::DrawType drawingMode, VBO *vbo)
{
	vbo->Bind(m_storageAttribs, drawingMode);
}

void RAS_StorageVBO::UnbindPrimitives(RAS_Rasterizer::DrawType drawingMode, VBO *vbo)
{
	vbo->Unbind(m_storageAttribs, drawingMode);
}

void RAS_StorageVBO::IndexPrimitives(VBO *vbo)
{
	vbo->Draw();
}

void RAS_StorageVBO::IndexPrimitivesInstancing(VBO *vbo, unsigned int numslots)
{
	vbo->DrawInstancing(numslots);
}

void RAS_StorageVBO::IndexPrimitivesBatching(VBO *vbo, const std::vector<void *>& indices,
											 const std::vector<int>& counts)
{
	vbo->DrawBatching(indices, counts);
}
