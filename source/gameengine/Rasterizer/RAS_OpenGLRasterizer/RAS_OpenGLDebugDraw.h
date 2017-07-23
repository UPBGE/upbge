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

/** \file RAS_OpenGLDebugDraw.h
 *  \ingroup bgerastogl
 */

#ifndef __RAS_OPEN_GL_DEBUG_DRAW_H__
#define __RAS_OPEN_GL_DEBUG_DRAW_H__

class RAS_Rasterizer;
class RAS_ICanvas;
class RAS_DebugDraw;
struct GPUShader;

class RAS_OpenGLDebugDraw
{
private:
	enum IboType {
		BOX_IBO = 0,
		MAX_IBO
	};

	enum VboType {
		BOX_UNIT_VBO = 0,
		BOX_2D_UNIT_VBO,
		LINES_VBO,
		BOX_VBO,
		BOX_2D_VBO,
		AABB_VBO,
		FRUSTUMS_VBO,
		MAX_VBO
	};

	enum VaoType {
		LINES_VAO = 0,
		FRUSTUMS_LINE_VAO,
		FRUSTUMS_SOLID_VAO,
		AABB_VAO,
		BOX_2D_VAO,
		MAX_VAO
	};

	unsigned int m_ibos[MAX_IBO];
	unsigned int m_vbos[MAX_VBO];
	unsigned int m_vaos[MAX_VAO];

	GPUShader *m_colorShader;
	GPUShader *m_frustumLineShader;
	GPUShader *m_frustumSolidShader;
	GPUShader *m_box2dShader;

public:
	RAS_OpenGLDebugDraw();
	~RAS_OpenGLDebugDraw();

	void Flush(RAS_Rasterizer *rasty, RAS_ICanvas *canvas, RAS_DebugDraw *debugDraw);
};

#endif  // __RAS_OPEN_GL_DEBUG_DRAW_H__
