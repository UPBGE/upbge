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
 * Contributor(s): Tristan Porteries.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/Rasterizer/RAS_OpenGLRasterizer/RAS_OpenGLDebugDraw.cpp
 *  \ingroup bgerastogl
 */

#include "RAS_OpenGLDebugDraw.h"
#include "RAS_Rasterizer.h"
#include "RAS_ICanvas.h"
#include "RAS_DebugDraw.h"

#include "GPU_material.h"
#include "GPU_glew.h"
#include "GPU_shader.h"
#include "GPU_vertex_array.h"

extern "C" {
#  include "BLF_api.h"
}

template<class Item>
inline static void updateVbo(unsigned int vbo, const std::vector<Item>& data)
{
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(Item) * data.size(), data.data(), GL_STATIC_DRAW);
}

inline static void attribVector(unsigned short loc, unsigned short stride, intptr_t offset, unsigned short size, unsigned short divisor)
{
	glEnableVertexAttribArray(loc);
	glVertexAttribPointer(loc, size, GL_FLOAT, false, stride, (const void *)offset);
	glVertexAttribDivisorARB(loc, divisor);
}

inline static void attribMatrix(unsigned short loc, unsigned short stride, intptr_t offset, unsigned short size, unsigned short divisor)
{
	for (unsigned short i = 0; i < size; ++i) {
		glEnableVertexAttribArray(loc + i);
		glVertexAttribPointer(loc + i, size, GL_FLOAT, false, stride, (const void *)(offset + size * i * sizeof(float)));
		glVertexAttribDivisorARB(loc + i, divisor);
	}
}

RAS_OpenGLDebugDraw::RAS_OpenGLDebugDraw()
{
	static const GLubyte boxIndices[] = {
		0, 1, 1, 2, 2, 3, 3, 0, 0, 4, 4, 5, 5, 6, 6, 7, 7, 4, 1, 5, 2, 6, 3, 7, // Wire (24).
		0, 1, 2, 2, 3, 0, 1, 5, 6, 6, 2, 1, 7, 6, 5, 5, 4, 7, 4, 0, 3, 3, 7, 4, 4, 5, 1, 1, 0, 4, 3, 2, 6, 6, 7, 3 // Solid (36).
	};

	static const float unitBoxVertices[24] = {
		-1.0f, -1.0f, -1.0f,
		1.0f, -1.0f, -1.0f,
		1.0f, 1.0f, -1.0f,
		-1.0f, 1.0f, -1.0f,
		-1.0f, -1.0f, 1.0f,
		1.0f, -1.0f, 1.0f,
		1.0f, 1.0f, 1.0f,
		-1.0f, 1.0f, 1.0f
	};

	static const float unitBox2DVertices[8] = {
		0.0f, 0.0f,
		1.0f, 0.0f,
		1.0f, 1.0f,
		0.0f, 1.0f
	};

	glGenBuffers(MAX_IBO, m_ibos);
	glGenBuffers(MAX_VBO, m_vbos);

	// Initialize static IBOs and VBOs.
	glBindBuffer(GL_ARRAY_BUFFER, m_ibos[BOX_IBO]);
	glBufferData(GL_ARRAY_BUFFER, sizeof(boxIndices), (void *)boxIndices, GL_STATIC_DRAW);

	glBindBuffer(GL_ARRAY_BUFFER, m_vbos[BOX_UNIT_VBO]);
	glBufferData(GL_ARRAY_BUFFER, sizeof(unitBoxVertices), (void *)unitBoxVertices, GL_STATIC_DRAW);

	glBindBuffer(GL_ARRAY_BUFFER, m_vbos[BOX_2D_UNIT_VBO]);
	glBufferData(GL_ARRAY_BUFFER, sizeof(unitBox2DVertices), (void *)unitBox2DVertices, GL_STATIC_DRAW);

	glBindBuffer(GL_ARRAY_BUFFER, 0);

	m_colorShader = GPU_shader_get_builtin_shader(GPU_SHADER_FLAT_COLOR);
	m_frustumLineShader = GPU_shader_get_builtin_shader(GPU_SHADER_FRUSTUM_LINE);
	m_frustumSolidShader = GPU_shader_get_builtin_shader(GPU_SHADER_FRUSTUM_SOLID);
	m_box2dShader = GPU_shader_get_builtin_shader(GPU_SHADER_2D_BOX);

	GPU_create_vertex_arrays(MAX_VAO, m_vaos);
	GPU_bind_vertex_array(m_vaos[LINES_VAO]);
	{
		static const unsigned short stride = sizeof(RAS_DebugDraw::Line) / 2;
		const unsigned int pos = GPU_shader_get_attribute(m_colorShader, "pos");
		const unsigned int color = GPU_shader_get_attribute(m_colorShader, "color");

		glBindBuffer(GL_ARRAY_BUFFER, m_vbos[LINES_VBO]);
		attribVector(pos, stride, offsetof(RAS_DebugDraw::Line, m_from), 3, 0);
		attribVector(color, stride, offsetof(RAS_DebugDraw::Line, m_color), 4, 0);
	}

	static const unsigned short frustumStride = sizeof(RAS_DebugDraw::Frustum);

	GPU_bind_vertex_array(m_vaos[FRUSTUMS_LINE_VAO]);
	{
		const unsigned short pos = GPU_shader_get_attribute(m_frustumLineShader, "pos");
		const unsigned short mat = GPU_shader_get_attribute(m_frustumLineShader, "mat");
		const unsigned short color = GPU_shader_get_attribute(m_frustumLineShader, "color");

		glBindBuffer(GL_ARRAY_BUFFER, m_vbos[BOX_UNIT_VBO]);
		attribVector(pos, 0, 0, 3, 0);

		glBindBuffer(GL_ARRAY_BUFFER, m_vbos[FRUSTUMS_VBO]);
		attribVector(color, frustumStride, offsetof(RAS_DebugDraw::Frustum, m_wireColor), 4, 1);
		attribMatrix(mat, frustumStride, offsetof(RAS_DebugDraw::Frustum, m_persMat), 4, 1);

		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ibos[BOX_IBO]);
	}

	GPU_bind_vertex_array(m_vaos[FRUSTUMS_SOLID_VAO]);
	{
		const unsigned short pos = GPU_shader_get_attribute(m_frustumSolidShader, "pos");
		const unsigned short mat = GPU_shader_get_attribute(m_frustumSolidShader, "mat");
		const unsigned short insideColor = GPU_shader_get_attribute(m_frustumSolidShader, "insideColor");
		const unsigned short outsideColor = GPU_shader_get_attribute(m_frustumSolidShader, "outsideColor");

		glBindBuffer(GL_ARRAY_BUFFER, m_vbos[BOX_UNIT_VBO]);
		attribVector(pos, 0, 0, 3, 0);

		glBindBuffer(GL_ARRAY_BUFFER, m_vbos[FRUSTUMS_VBO]);
		attribVector(insideColor, frustumStride, offsetof(RAS_DebugDraw::Frustum, m_insideColor), 4, 1);
		attribVector(outsideColor, frustumStride, offsetof(RAS_DebugDraw::Frustum, m_outsideColor), 4, 1);
		attribMatrix(mat, frustumStride, offsetof(RAS_DebugDraw::Frustum, m_persMat), 4, 1);

		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ibos[BOX_IBO]);
	}

	GPU_bind_vertex_array(m_vaos[AABB_VAO]);
	{
		static const unsigned short stride = sizeof(RAS_DebugDraw::Aabb);
		const unsigned short pos = GPU_shader_get_attribute(m_frustumLineShader, "pos");
		const unsigned short mat = GPU_shader_get_attribute(m_frustumLineShader, "mat");
		const unsigned short color = GPU_shader_get_attribute(m_frustumLineShader, "color");

		glBindBuffer(GL_ARRAY_BUFFER, m_vbos[BOX_UNIT_VBO]);
		attribVector(pos, 0, 0, 3, 0);

		glBindBuffer(GL_ARRAY_BUFFER, m_vbos[AABB_VBO]);
		attribVector(color, stride, offsetof(RAS_DebugDraw::Aabb, m_color), 4, 1);
		attribMatrix(mat, stride, offsetof(RAS_DebugDraw::Aabb, m_mat), 4, 1);

		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ibos[BOX_IBO]);
	}

	GPU_bind_vertex_array(m_vaos[BOX_2D_VAO]);
	{
		static const unsigned short stride = sizeof(RAS_DebugDraw::Box2d);
		const unsigned short pos = GPU_shader_get_attribute(m_box2dShader, "pos");
		const unsigned short trans = GPU_shader_get_attribute(m_box2dShader, "trans");
		const unsigned short color = GPU_shader_get_attribute(m_box2dShader, "color");

		glBindBuffer(GL_ARRAY_BUFFER, m_vbos[BOX_2D_UNIT_VBO]);
		attribVector(pos, 0, 0, 2, 0);

		glBindBuffer(GL_ARRAY_BUFFER, m_vbos[BOX_2D_VBO]);
		attribVector(color, stride, offsetof(RAS_DebugDraw::Box2d, m_color), 4, 1);
		attribVector(trans, stride, offsetof(RAS_DebugDraw::Box2d, m_trans), 4, 1);
	}

	GPU_unbind_vertex_array();
	glBindBuffer(GL_ARRAY_BUFFER, 0);
}

RAS_OpenGLDebugDraw::~RAS_OpenGLDebugDraw()
{
	glDeleteBuffers(MAX_IBO, m_ibos);
	glDeleteBuffers(MAX_VBO, m_vbos);
	GPU_delete_vertex_arrays(MAX_VAO, m_vaos);
}

void RAS_OpenGLDebugDraw::Flush(RAS_Rasterizer *rasty, RAS_ICanvas *canvas, RAS_DebugDraw *debugDraw)
{
	rasty->SetFrontFace(true);
	rasty->SetAlphaBlend(GPU_BLEND_ALPHA);
	rasty->DisableLights();

	// draw lines
	const std::vector<RAS_DebugDraw::Line>& lines = debugDraw->m_lines;
	const unsigned int numlines = lines.size();
	if (numlines > 0) {
		updateVbo(m_vbos[LINES_VBO], lines);

		GPU_bind_vertex_array(m_vaos[LINES_VAO]);
		GPU_shader_bind(m_colorShader);
		glDrawArrays(GL_LINES, 0, numlines * 2);
	}

	const std::vector<RAS_DebugDraw::Frustum>& frustums = debugDraw->m_frustums;
	const unsigned int numfrustums = frustums.size();
	if (numfrustums > 0) {
		updateVbo(m_vbos[FRUSTUMS_VBO], frustums);

		GPU_bind_vertex_array(m_vaos[FRUSTUMS_LINE_VAO]);
		GPU_shader_bind(m_frustumLineShader);
		glDrawElementsInstancedARB(GL_LINES, 24, GL_UNSIGNED_BYTE, nullptr, numfrustums);

		GPU_bind_vertex_array(m_vaos[FRUSTUMS_SOLID_VAO]);
		GPU_shader_bind(m_frustumSolidShader);
		glDrawElementsInstancedARB(GL_TRIANGLES, 36, GL_UNSIGNED_BYTE, (const void *)(sizeof(GLubyte) * 24), numfrustums);
	}

	const std::vector<RAS_DebugDraw::Aabb>& aabbs = debugDraw->m_aabbs;
	const unsigned int numaabbs = aabbs.size();
	if (numaabbs > 0) {
		updateVbo(m_vbos[AABB_VBO], aabbs);

		GPU_bind_vertex_array(m_vaos[AABB_VAO]);
		GPU_shader_bind(m_frustumLineShader);
		glDrawElementsInstancedARB(GL_LINES, 24, GL_UNSIGNED_BYTE, nullptr, numaabbs);
	}

	const unsigned int width = canvas->GetWidth();
	const unsigned int height = canvas->GetHeight();

	rasty->Disable(RAS_Rasterizer::RAS_DEPTH_TEST);
	rasty->DisableForText();

	rasty->PushMatrix();
	rasty->LoadIdentity();

	rasty->SetMatrixMode(RAS_Rasterizer::RAS_PROJECTION);
	rasty->PushMatrix();
	rasty->LoadIdentity();

	glOrtho(0, width, height, 0, -100, 100);

	const std::vector<RAS_DebugDraw::Box2d>& boxes2d = debugDraw->m_boxes2d;
	const unsigned int numboxes = boxes2d.size();
	if (numboxes > 0) {
		updateVbo(m_vbos[BOX_2D_VBO], boxes2d);

		GPU_bind_vertex_array(m_vaos[BOX_2D_VAO]);
		GPU_shader_bind(m_box2dShader);
		glDrawArraysInstancedARB(GL_TRIANGLE_FAN, 0, 4, numboxes);
	}

	GPU_unbind_vertex_array();
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	GPU_shader_unbind();

	rasty->LoadIdentity();

	glOrtho(0, width, 0, height, -100, 100);

	BLF_size(blf_mono_font, 11, 72);

	BLF_enable(blf_mono_font, BLF_SHADOW);
	static float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
	BLF_shadow(blf_mono_font, 1, black);
	BLF_shadow_offset(blf_mono_font, 1, 1);

	for (const RAS_DebugDraw::Text2d& text2d : debugDraw->m_texts2d) {
		const std::string& text = text2d.m_text;
		const float xco = text2d.m_pos[0];
		const float yco = height - text2d.m_pos[1];

		glColor4fv(text2d.m_color);
		BLF_position(blf_mono_font, xco, yco, 0.0f);
		BLF_draw(blf_mono_font, text.c_str(), text.size());
	}
	BLF_disable(blf_mono_font, BLF_SHADOW);

	rasty->PopMatrix();
	rasty->SetMatrixMode(RAS_Rasterizer::RAS_MODELVIEW);

	rasty->PopMatrix();
}
