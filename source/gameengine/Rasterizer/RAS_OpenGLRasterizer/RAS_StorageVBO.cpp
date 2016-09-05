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

#include "glew-mx.h"

VBO::VBO(RAS_DisplayArrayBucket *arrayBucket)
	:m_vaoInitialized(false)
{
	m_data = arrayBucket->GetDisplayArray();
	m_size = m_data->GetVertexCount();
	m_indices = m_data->GetIndexCount();
	m_stride = m_data->GetVertexMemorySize();

	m_mode = m_data->GetOpenGLPrimitiveType();

	m_useVao = arrayBucket->UseVao() && GLEW_ARB_vertex_array_object;

	// Generate Buffers
	glGenBuffersARB(1, &m_ibo);
	glGenBuffersARB(1, &m_vbo_id);
	// Generate Vertex Array Object
	glGenVertexArrays(1, &m_vao);

	// Fill the buffers with initial data
	UpdateIndices();
	UpdateData();

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
	if (m_vao) {
		glDeleteVertexArrays(1, &m_vao);
	}
}

void VBO::UpdateData()
{
	glBindBufferARB(GL_ARRAY_BUFFER_ARB, m_vbo_id);
	glBufferData(GL_ARRAY_BUFFER, m_stride * m_size, m_data->GetVertexPointer(), GL_STATIC_DRAW);
	glBindBufferARB(GL_ARRAY_BUFFER_ARB, 0);
}

void VBO::UpdateIndices()
{
	glBindBufferARB(GL_ELEMENT_ARRAY_BUFFER_ARB, m_ibo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, m_indices * sizeof(GLuint),
	             m_data->GetIndexPointer(), GL_STATIC_DRAW);
	glBindBufferARB(GL_ELEMENT_ARRAY_BUFFER, 0);
}

void VBO::SetMeshModified(RAS_IRasterizer::DrawType drawType, bool modified)
{
	if (modified) {
		m_vaoInitialized = false;
	}
}

void VBO::Bind(int texco_num, RAS_IRasterizer::TexCoGen *texco, int attrib_num, RAS_IRasterizer::TexCoGen *attrib,
			   int *attrib_layer, RAS_IRasterizer::DrawType drawingmode)
{
	if (m_useVao) {
		glBindVertexArray(m_vao);
		if (m_vaoInitialized) {
			return;
		}
		m_vaoInitialized = true;
	}

	bool wireframe = (drawingmode == RAS_IRasterizer::RAS_WIREFRAME);
	int unit;

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

	for (unit = 0; unit < texco_num; ++unit) {
		switch (texco[unit]) {
			case RAS_IRasterizer::RAS_TEXCO_ORCO:
			case RAS_IRasterizer::RAS_TEXCO_GLOB:
			{
				glClientActiveTexture(GL_TEXTURE0_ARB + unit);
				glEnableClientState(GL_TEXTURE_COORD_ARRAY);
				glTexCoordPointer(3, GL_FLOAT, m_stride, m_vertex_offset);
				break;
			}
			case RAS_IRasterizer::RAS_TEXCO_UV:
			{
				glClientActiveTexture(GL_TEXTURE0_ARB + unit);
				glEnableClientState(GL_TEXTURE_COORD_ARRAY);
				glTexCoordPointer(2, GL_FLOAT, m_stride, (void *)((intptr_t)m_uv_offset + (sizeof(GLfloat) * 2 * unit)));
				break;
			}
			case RAS_IRasterizer::RAS_TEXCO_NORM:
			{
				glClientActiveTexture(GL_TEXTURE0_ARB + unit);
				glEnableClientState(GL_TEXTURE_COORD_ARRAY);
				glTexCoordPointer(3, GL_FLOAT, m_stride, m_normal_offset);
				break;
			}
			case RAS_IRasterizer::RAS_TEXTANGENT:
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

	for (unit = 0; unit < attrib_num; ++unit) {
		switch (attrib[unit]) {
			case RAS_IRasterizer::RAS_TEXCO_ORCO:
			case RAS_IRasterizer::RAS_TEXCO_GLOB:
			{
				glVertexAttribPointerARB(unit, 3, GL_FLOAT, GL_FALSE, m_stride, m_vertex_offset);
				glEnableVertexAttribArrayARB(unit);
				break;
			}
			case RAS_IRasterizer::RAS_TEXCO_UV:
			{
				glVertexAttribPointerARB(unit, 2, GL_FLOAT, GL_FALSE, m_stride, (void *)((intptr_t)m_uv_offset + attrib_layer[unit] * sizeof(GLfloat) * 2));
				glEnableVertexAttribArrayARB(unit);
				break;
			}
			case RAS_IRasterizer::RAS_TEXCO_NORM:
			{
				glVertexAttribPointerARB(unit, 2, GL_FLOAT, GL_FALSE, m_stride, m_normal_offset);
				glEnableVertexAttribArrayARB(unit);
				break;
			}
			case RAS_IRasterizer::RAS_TEXTANGENT:
			{
				glVertexAttribPointerARB(unit, 4, GL_FLOAT, GL_FALSE, m_stride, m_tangent_offset);
				glEnableVertexAttribArrayARB(unit);
				break;
			}
			case RAS_IRasterizer::RAS_TEXCO_VCOL:
			{
				glVertexAttribPointerARB(unit, 4, GL_UNSIGNED_BYTE, GL_TRUE, m_stride, m_color_offset);
				glEnableVertexAttribArrayARB(unit);
				break;
			}
			default:
				break;
		}
	}
}

void VBO::Unbind(int texco_num, RAS_IRasterizer::TexCoGen *texco, int attrib_num, RAS_IRasterizer::TexCoGen *attrib, RAS_IRasterizer::DrawType drawingmode)
{
	if (m_useVao) {
		glBindVertexArray(0);
		return;
	}

	bool wireframe = (drawingmode == RAS_IRasterizer::RAS_WIREFRAME);

	glDisableClientState(GL_VERTEX_ARRAY);
	glDisableClientState(GL_NORMAL_ARRAY);
	if (!wireframe) {
		glDisableClientState(GL_COLOR_ARRAY);
	}
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);

	for (unsigned int unit = 0; unit < texco_num; ++unit) {
		if (texco[unit] != RAS_IRasterizer::RAS_TEXCO_DISABLE) {
			glClientActiveTextureARB(GL_TEXTURE0_ARB + unit);
			glDisableClientState(GL_TEXTURE_COORD_ARRAY);
		}
	}
	glClientActiveTextureARB(GL_TEXTURE0_ARB);

	for (unsigned int unit = 0; unit < attrib_num; ++unit) {
		if (attrib[unit] != RAS_IRasterizer::RAS_TEXCO_DISABLE) {
			glDisableVertexAttribArrayARB(unit);
		}
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

RAS_StorageVBO::RAS_StorageVBO(int *texco_num, RAS_IRasterizer::TexCoGen *texco, int *attrib_num, RAS_IRasterizer::TexCoGen *attrib, int *attrib_layer) :
	m_drawingmode(RAS_IRasterizer::RAS_TEXTURED),
	m_texco_num(texco_num),
	m_attrib_num(attrib_num),
	m_texco(texco),
	m_attrib(attrib),
	m_attrib_layer(attrib_layer)
{
}

RAS_StorageVBO::~RAS_StorageVBO()
{
}

bool RAS_StorageVBO::Init()
{
	return true;
}

void RAS_StorageVBO::Exit()
{
}

VBO *RAS_StorageVBO::GetVBO(RAS_DisplayArrayBucket *arrayBucket)
{
	VBO *vbo = (VBO *)arrayBucket->GetStorageInfo();
	if (!vbo) {
		vbo = new VBO(arrayBucket);
		arrayBucket->SetStorageInfo(vbo);
	}
	return vbo;
}

void RAS_StorageVBO::BindPrimitives(RAS_DisplayArrayBucket *arrayBucket)
{
	VBO *vbo = GetVBO(arrayBucket);
	vbo->Bind(*m_texco_num, m_texco, *m_attrib_num, m_attrib, m_attrib_layer, m_drawingmode);
}

void RAS_StorageVBO::UnbindPrimitives(RAS_DisplayArrayBucket *arrayBucket)
{
	VBO *vbo = GetVBO(arrayBucket);
	vbo->Unbind(*m_texco_num, m_texco, *m_attrib_num, m_attrib, m_drawingmode);
}

void RAS_StorageVBO::IndexPrimitives(RAS_MeshSlot *ms)
{
	RAS_DisplayArrayBucket *arrayBucket = ms->m_displayArrayBucket;
	VBO *vbo = GetVBO(arrayBucket);

	// Update the vbo if the mesh is modified or use a dynamic deformer.
	if (arrayBucket->IsMeshModified()) {
		vbo->UpdateData();
	}

	vbo->Draw();
}

void RAS_StorageVBO::IndexPrimitivesInstancing(RAS_DisplayArrayBucket *arrayBucket)
{
	VBO *vbo = GetVBO(arrayBucket);

	// Update the vbo if the mesh is modified or use a dynamic deformer.
	if (arrayBucket->IsMeshModified()) {
		vbo->UpdateData();
	}

	vbo->DrawInstancing(arrayBucket->GetNumActiveMeshSlots());
}
