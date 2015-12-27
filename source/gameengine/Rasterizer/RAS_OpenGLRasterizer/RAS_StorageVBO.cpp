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
#include "RAS_MeshObject.h"
#include "RAS_Deformer.h"

#include "glew-mx.h"

VBO::VBO(RAS_DisplayArray *data, unsigned int indices)
{
	m_data = data;
	m_size = data->m_vertex.size();
	m_indices = indices;
	m_stride = sizeof(RAS_TexVert);

	m_mode = GL_TRIANGLES;

	// Generate Buffers
	glGenBuffersARB(1, &m_ibo);
	glGenBuffersARB(1, &m_vbo_id);

	// Fill the buffers with initial data
	UpdateIndices();
	UpdateData();

	// Establish offsets
	m_vertex_offset = (void *)(((RAS_TexVert *)0)->getXYZ());
	m_normal_offset = (void *)(((RAS_TexVert *)0)->getNormal());
	m_tangent_offset = (void *)(((RAS_TexVert *)0)->getTangent());
	m_color_offset = (void *)(((RAS_TexVert *)0)->getRGBA());
	m_uv_offset = (void *)(((RAS_TexVert *)0)->getUV(0));
}

VBO::~VBO()
{
	glDeleteBuffersARB(1, &m_ibo);
	glDeleteBuffersARB(1, &m_vbo_id);
}

void VBO::UpdateData()
{
	glBindBufferARB(GL_ARRAY_BUFFER_ARB, m_vbo_id);
	glBufferData(GL_ARRAY_BUFFER, m_stride * m_size, m_data->m_vertex.data(), GL_STATIC_DRAW);
}

void VBO::UpdateIndices()
{
	glBindBufferARB(GL_ELEMENT_ARRAY_BUFFER_ARB, m_ibo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, m_data->m_index.size() * sizeof(GLushort),
	             m_data->m_index.data(), GL_STATIC_DRAW);
}

void VBO::Bind(int texco_num, RAS_IRasterizer::TexCoGen *texco, int attrib_num, RAS_IRasterizer::TexCoGen *attrib, int *attrib_layer)
{
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
	glEnableClientState(GL_COLOR_ARRAY);
	glColorPointer(4, GL_UNSIGNED_BYTE, m_stride, m_color_offset);

	for (unit = 0; unit < texco_num; ++unit) {
		glClientActiveTexture(GL_TEXTURE0_ARB + unit);
		switch (texco[unit]) {
			case RAS_IRasterizer::RAS_TEXCO_ORCO:
			case RAS_IRasterizer::RAS_TEXCO_GLOB:
			{
				glEnableClientState(GL_TEXTURE_COORD_ARRAY);
				glTexCoordPointer(3, GL_FLOAT, m_stride, m_vertex_offset);
				break;
			}
			case RAS_IRasterizer::RAS_TEXCO_UV:
			{
				glEnableClientState(GL_TEXTURE_COORD_ARRAY);
				glTexCoordPointer(2, GL_FLOAT, m_stride, (void *)((intptr_t)m_uv_offset + (sizeof(GLfloat) * 2 * unit)));
				break;
			}
			case RAS_IRasterizer::RAS_TEXCO_NORM:
			{
				glEnableClientState(GL_TEXTURE_COORD_ARRAY);
				glTexCoordPointer(3, GL_FLOAT, m_stride, m_normal_offset);
				break;
			}
			case RAS_IRasterizer::RAS_TEXTANGENT:
			{
				glEnableClientState(GL_TEXTURE_COORD_ARRAY);
				glTexCoordPointer(4, GL_FLOAT, m_stride, m_tangent_offset);
				break;
			}
			default:
				break;
		}
	}
	glClientActiveTextureARB(GL_TEXTURE0_ARB);

	if (GLEW_ARB_vertex_program) {
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
				default:
					break;
			}
		}
	}
}

void VBO::Unbind(int attrib_num)
{
	glDisableClientState(GL_VERTEX_ARRAY);
	glDisableClientState(GL_NORMAL_ARRAY);
	glDisableClientState(GL_COLOR_ARRAY);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);

	if (GLEW_ARB_vertex_program) {
		for (int i = 0; i < attrib_num; ++i)
			glDisableVertexAttribArrayARB(i);
	}

	glBindBufferARB(GL_ARRAY_BUFFER_ARB, 0);
	glBindBufferARB(GL_ELEMENT_ARRAY_BUFFER_ARB, 0);
}

void VBO::Draw()
{
	glDrawElements(m_mode, m_indices, GL_UNSIGNED_SHORT, 0);
}

RAS_StorageVBO::RAS_StorageVBO(int *texco_num, RAS_IRasterizer::TexCoGen *texco, int *attrib_num, RAS_IRasterizer::TexCoGen *attrib, int *attrib_layer) :
	m_drawingmode(RAS_IRasterizer::KX_TEXTURED),
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
	m_vbo_lookup.clear();
}

VBO *RAS_StorageVBO::GetVBO(RAS_DisplayArray *array)
{
	VBO *vbo = m_vbo_lookup[array];
	if (!vbo) {
		m_vbo_lookup[array] = vbo = new VBO(array, array->m_index.size());
	}
	return vbo;
}

void RAS_StorageVBO::BindPrimitives(RAS_DisplayArray *array)
{
	if (!array) {
		return;
	}

	VBO *vbo = GetVBO(array);
	vbo->Bind(*m_texco_num, m_texco, *m_attrib_num, m_attrib, m_attrib_layer);
}

void RAS_StorageVBO::UnbindPrimitives(RAS_DisplayArray *array)
{
	if (!array) {
		return;
	}

	VBO *vbo = GetVBO(array);
	vbo->Unbind(*m_attrib_num);
}

void RAS_StorageVBO::IndexPrimitives(RAS_MeshSlot& ms)
{
	RAS_DisplayArray *array = ms.GetDisplayArray();
	VBO *vbo = GetVBO(array);

	// Update the vbo if the mesh is modified or use a dynamic deformer.
	if ((ms.m_mesh->GetModifiedFlag() & RAS_MeshObject::MESH_MODIFIED) ||
		(ms.m_pDeformer && ms.m_pDeformer->IsDynamic()))
	{
		vbo->UpdateData();
	}

	vbo->Draw();
}
