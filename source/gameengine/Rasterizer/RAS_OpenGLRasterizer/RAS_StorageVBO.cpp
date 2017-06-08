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
	m_useVao(!instancing)
{
	m_size = m_data->GetVertexCount();
	m_indices = m_data->GetIndexCount();
	m_stride = m_data->GetVertexMemorySize();

	m_mode = m_data->GetOpenGLPrimitiveType();

	// Generate Buffers
	glGenBuffers(1, &m_ibo);
	glGenBuffers(1, &m_vbo_id);

	for (unsigned short i = 0; i < RAS_Rasterizer::RAS_DRAW_MAX; ++i) {
		m_vaos[i] = 0;
		m_vaoInitialized[i] = false;
	}

	// Fill the buffers with initial data
	AllocData();

	// Establish offsets
	m_vertex_offset = m_data->GetVertexXYZOffset();
	m_normal_offset = m_data->GetVertexNormalOffset();
	m_tangent_offset = m_data->GetVertexTangentOffset();
	m_color_offset = m_data->GetVertexColorOffset();
	m_uv_offset = m_data->GetVertexUVOffset();
}

VBO::~VBO()
{
	glDeleteBuffers(1, &m_ibo);
	glDeleteBuffers(1, &m_vbo_id);
	for (unsigned short i = 0; i < RAS_Rasterizer::RAS_DRAW_MAX; ++i) {
		if (m_vaos[i]) {
			glDeleteVertexArrays(1, &m_vaos[i]);
		}
	}
}

void VBO::UpdateVertexData()
{
	UpdateData();
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

void VBO::UpdateData()
{
	glBindBuffer(GL_ARRAY_BUFFER, m_vbo_id);
	glBufferSubData(GL_ARRAY_BUFFER, 0, m_stride * m_size, m_data->GetVertexPointer());
	glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void VBO::AllocData()
{
	glBindBuffer(GL_ARRAY_BUFFER, m_vbo_id);
	glBufferData(GL_ARRAY_BUFFER, m_stride * m_size, m_data->GetVertexPointer(), GL_DYNAMIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ibo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, m_indices * sizeof(GLuint), m_data->GetIndexPointer(), GL_DYNAMIC_DRAW);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

static int vertattribloc;
static int normalattribloc;

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

	int program;
	glGetIntegerv(GL_CURRENT_PROGRAM, &program);

	// Bind buffers
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ibo);
	glBindBuffer(GL_ARRAY_BUFFER, m_vbo_id);

	// Vertexes
	char *vertattrib = "pos";
	vertattribloc = glGetAttribLocation(program, vertattrib);
	glEnableVertexAttribArray(vertattribloc);
	glVertexAttribPointer(vertattribloc, 3,	GL_FLOAT, GL_FALSE,	m_stride, m_vertex_offset);

	// Normals
	char *normalattrib = "nor";
	normalattribloc = glGetAttribLocation(program, normalattrib);
	glEnableVertexAttribArray(normalattribloc);
	glVertexAttribPointer(normalattribloc, 3, GL_FLOAT, GL_FALSE, m_stride, m_normal_offset);

	// Colors
	/*if (!wireframe) {
		glEnableClientState(GL_COLOR_ARRAY);
		glColorPointer(4, GL_UNSIGNED_BYTE, m_stride, m_color_offset);
	}*/

	/*for (unsigned short unit = 0, size = storageAttribs->texcos.size(); unit < size; ++unit) {
		switch (storageAttribs->texcos[unit]) {
			case RAS_Rasterizer::RAS_TEXCO_ORCO:
			case RAS_Rasterizer::RAS_TEXCO_GLOB:
			{
				glClientActiveTexture(GL_TEXTURE0 + unit);
				glEnableClientState(GL_TEXTURE_COORD_ARRAY);
				glTexCoordPointer(3, GL_FLOAT, m_stride, m_vertex_offset);
				break;
			}
			case RAS_Rasterizer::RAS_TEXCO_UV:
			{
				glClientActiveTexture(GL_TEXTURE0 + unit);
				glEnableClientState(GL_TEXTURE_COORD_ARRAY);
				glTexCoordPointer(2, GL_FLOAT, m_stride, (void *)((intptr_t)m_uv_offset + (sizeof(GLfloat) * 2 * unit)));
				break;
			}
			case RAS_Rasterizer::RAS_TEXCO_NORM:
			{
				glClientActiveTexture(GL_TEXTURE0 + unit);
				glEnableClientState(GL_TEXTURE_COORD_ARRAY);
				glTexCoordPointer(3, GL_FLOAT, m_stride, m_normal_offset);
				break;
			}
			case RAS_Rasterizer::RAS_TEXTANGENT:
			{
				glClientActiveTexture(GL_TEXTURE0 + unit);
				glEnableClientState(GL_TEXTURE_COORD_ARRAY);
				glTexCoordPointer(4, GL_FLOAT, m_stride, m_tangent_offset);
				break;
			}
			default:
				break;
		}
	}
	glClientActiveTexture(GL_TEXTURE0);

	for (unsigned short unit = 0, size = storageAttribs->attribs.size(); unit < size; ++unit) {
		switch (storageAttribs->attribs[unit]) {
			case RAS_Rasterizer::RAS_TEXCO_ORCO:
			case RAS_Rasterizer::RAS_TEXCO_GLOB:
			{
				glVertexAttribPointer(unit, 3, GL_FLOAT, GL_FALSE, m_stride, m_vertex_offset);
				glEnableVertexAttribArray(unit);
				break;
			}
			case RAS_Rasterizer::RAS_TEXCO_UV:
			{
				glVertexAttribPointer(unit, 2, GL_FLOAT, GL_FALSE, m_stride, (void *)((intptr_t)m_uv_offset + storageAttribs->layers[unit] * sizeof(GLfloat) * 2));
				glEnableVertexAttribArray(unit);
				break;
			}
			case RAS_Rasterizer::RAS_TEXCO_NORM:
			{
				glVertexAttribPointer(unit, 2, GL_FLOAT, GL_FALSE, m_stride, m_normal_offset);
				glEnableVertexAttribArray(unit);
				break;
			}
			case RAS_Rasterizer::RAS_TEXTANGENT:
			{
				glVertexAttribPointer(unit, 4, GL_FLOAT, GL_FALSE, m_stride, m_tangent_offset);
				glEnableVertexAttribArray(unit);
				break;
			}
			case RAS_Rasterizer::RAS_TEXCO_VCOL:
			{
				glVertexAttribPointer(unit, 4, GL_UNSIGNED_BYTE, GL_TRUE, m_stride, (void *)((intptr_t)m_color_offset + storageAttribs->layers[unit] * sizeof(GLuint)));
				glEnableVertexAttribArray(unit);
				break;
			}
			default:
				break;
		}
	}*/

	/* VAO don't track the VBO state and the attributes don't need a bound VBO to be used in a render.
	 * So we unbind the VBO here because they will not be unbound in VBO::Unbind. */
	if (m_useVao) {
		glBindBuffer(GL_ARRAY_BUFFER, 0);
	}
}

void VBO::Unbind(RAS_Rasterizer::StorageAttribs *storageAttribs, RAS_Rasterizer::DrawType drawingmode)
{
	if (m_useVao) {
		glBindVertexArray(0);
		return;
	}

	//bool wireframe = (drawingmode == RAS_Rasterizer::RAS_WIREFRAME);

	glDisableVertexAttribArray(vertattribloc);
	glDisableVertexAttribArray(normalattribloc);
	/*if (!wireframe) {
		glDisableClientState(GL_COLOR_ARRAY);
	}
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);

	for (unsigned short unit = 0, size = storageAttribs->texcos.size(); unit < size; ++unit) {
		if (storageAttribs->texcos[unit] != RAS_Rasterizer::RAS_TEXCO_DISABLE) {
			glClientActiveTexture(GL_TEXTURE0 + unit);
			glDisableClientState(GL_TEXTURE_COORD_ARRAY);
		}
	}
	glClientActiveTexture(GL_TEXTURE0);

	for (unsigned short unit = 0, size = storageAttribs->attribs.size(); unit < size; ++unit) {
		if (storageAttribs->attribs[unit] != RAS_Rasterizer::RAS_TEXCO_DISABLE) {
			glDisableVertexAttribArray(unit);
		}
	}*/

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

void VBO::Draw()
{
	glDrawElements(m_mode, m_indices, GL_UNSIGNED_INT, 0);
}

void VBO::DrawInstancing(unsigned int numinstance)
{
	glDrawElementsInstanced(m_mode, m_indices, GL_UNSIGNED_INT, 0, numinstance);
}

void VBO::DrawBatching(const std::vector<void *>& indices, const std::vector<int>& counts)
{
	glMultiDrawElements(m_mode, counts.data(), GL_UNSIGNED_INT, (void **)indices.data(), counts.size());
}

RAS_StorageVBO::RAS_StorageVBO(RAS_Rasterizer::StorageAttribs *storageAttribs)
	:m_drawingmode(RAS_Rasterizer::RAS_TEXTURED),
	m_storageAttribs(storageAttribs)
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

void RAS_StorageVBO::BindPrimitives(VBO *vbo)
{
	vbo->Bind(m_storageAttribs, m_drawingmode);
}

void RAS_StorageVBO::UnbindPrimitives(VBO *vbo)
{
	vbo->Unbind(m_storageAttribs, m_drawingmode);
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
