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

/** \file gameengine/Rasterizer/RAS_OpenGLRasterizer/RAS_OpenGLRasterizer.cpp
 *  \ingroup bgerastogl
 */

#include "RAS_OpenGLRasterizer.h"
#include "RAS_IPolygonMaterial.h"

#include "GPU_glew.h"

#include "RAS_MeshUser.h"

#include "GPU_draw.h"
#include "GPU_extensions.h"
#include "GPU_material.h"
#include "GPU_shader.h"
#include "GPU_texture.h"
#include "GPU_matrix.h"

extern "C" {
#  include "BLF_api.h"
#  include "BLI_math.h"
#  include "BKE_DerivedMesh.h"
#  include "DNA_material_types.h"
#  include "GPU_immediate.h"
}

#include "MEM_guardedalloc.h"

#include "CM_Message.h"

#include <cstring> // For memcpy.

// WARNING: Always respect the order from RAS_Rasterizer::EnableBit.
static const int openGLEnableBitEnums[] = {
	GL_DEPTH_TEST, // RAS_DEPTH_TEST
	GL_ALPHA_TEST, // RAS_ALPHA_TEST
	GL_SCISSOR_TEST, // RAS_SCISSOR_TEST
	GL_TEXTURE_2D, // RAS_TEXTURE_2D
	GL_TEXTURE_CUBE_MAP, // RAS_TEXTURE_CUBE_MAP
	GL_BLEND, // RAS_BLEND
	GL_COLOR_MATERIAL, // RAS_COLOR_MATERIAL
	GL_CULL_FACE, // RAS_CULL_FACE
	GL_FOG, // RAS_FOG
	GL_LIGHTING, // RAS_LIGHTING
	GL_MULTISAMPLE, // RAS_MULTISAMPLE
	GL_POLYGON_STIPPLE, // RAS_POLYGON_STIPPLE
	GL_POLYGON_OFFSET_FILL, // RAS_POLYGON_OFFSET_FILL
	GL_POLYGON_OFFSET_LINE, // RAS_POLYGON_OFFSET_LINE
	GL_TEXTURE_GEN_S, // RAS_TEXTURE_GEN_S
	GL_TEXTURE_GEN_T, // RAS_TEXTURE_GEN_T
	GL_TEXTURE_GEN_R, // RAS_TEXTURE_GEN_R
	GL_TEXTURE_GEN_Q // RAS_TEXTURE_GEN_Q
};

// WARNING: Always respect the order from RAS_Rasterizer::DepthFunc.
static const int openGLDepthFuncEnums[] = {
	GL_NEVER, // RAS_NEVER
	GL_LEQUAL, // RAS_LEQUAL
	GL_LESS, // RAS_LESS
	GL_ALWAYS, // RAS_ALWAYS
	GL_GEQUAL, // RAS_GEQUAL
	GL_GREATER, // RAS_GREATER
	GL_NOTEQUAL, // RAS_NOTEQUAL
	GL_EQUAL // RAS_EQUAL
};

// WARNING: Always respect the order from RAS_Rasterizer::MatrixMode.
static const int openGLMatrixModeEnums[] = {
	GL_PROJECTION, // RAS_PROJECTION
	GL_MODELVIEW, // RAS_MODELVIEW
	GL_TEXTURE // RAS_TEXTURE
};

// WARNING: Always respect the order from RAS_Rasterizer::BlendFunc.
static const int openGLBlendFuncEnums[] = {
	GL_ZERO, // RAS_ZERO,
	GL_ONE, // RAS_ONE,
	GL_SRC_COLOR, // RAS_SRC_COLOR,
	GL_ONE_MINUS_SRC_COLOR, // RAS_ONE_MINUS_SRC_COLOR,
	GL_DST_COLOR, // RAS_DST_COLOR,
	GL_ONE_MINUS_DST_COLOR, // RAS_ONE_MINUS_DST_COLOR,
	GL_SRC_ALPHA, // RAS_SRC_ALPHA,
	GL_ONE_MINUS_SRC_ALPHA, // RAS_ONE_MINUS_SRC_ALPHA,
	GL_DST_ALPHA, // RAS_DST_ALPHA,
	GL_ONE_MINUS_DST_ALPHA, // RAS_ONE_MINUS_DST_ALPHA,
	GL_SRC_ALPHA_SATURATE // RAS_SRC_ALPHA_SATURATE
};

RAS_OpenGLRasterizer::ScreenPlane::ScreenPlane()
{
	glGenVertexArrays(1, &m_vao);
	// Generate the VBO and IBO for screen overlay plane.
	glGenBuffers(1, &m_vbo);
	glGenBuffers(1, &m_ibo);

	glBindVertexArray(m_vao);

	// Vertexes for screen plane, it contains the vertex position (3 floats) and the vertex uv after (2 floats, total size = 5 floats).
	static const float vertices[] = {
		//   3f position   |   2f UV
		-1.0f, -1.0f, 1.0f, 0.0f, 0.0f,
		-1.0f,  1.0f, 1.0f, 0.0f, 1.0f,
		 1.0f,  1.0f, 1.0f, 1.0f, 1.0f,
		 1.0f, -1.0f, 1.0f, 1.0f, 0.0f
	};
	// Indices for screen plane.
	static const GLubyte indices[] = {3, 2, 1, 0};

	// Send indices in the sreen plane IBO.
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ibo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

	// Send vertexes in the screen plane VBO.
	glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

	// VAO -> vertices
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 5, 0);

	// VAO -> texcoords
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 5, ((char *)nullptr) + sizeof(float) * 3);

	// Unbind VBO
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	// VAO -> Unbind
	glBindVertexArray(0);
}

RAS_OpenGLRasterizer::ScreenPlane::~ScreenPlane()
{
	glDeleteVertexArrays(1, &m_vao);
	glDeleteBuffers(1, &m_vbo);
	glDeleteBuffers(1, &m_ibo);
}

inline void RAS_OpenGLRasterizer::ScreenPlane::Render()
{
	// Bind screen plane VAO
	glBindVertexArray(m_vao);

	// Draw in triangle fan mode to reduce IBO size.
	glDrawElements(GL_TRIANGLE_FAN, 4, GL_UNSIGNED_BYTE, 0);

	// Disable VAO.
	glBindVertexArray(0);
}

RAS_OpenGLRasterizer::RAS_OpenGLRasterizer(RAS_Rasterizer *rasterizer)
	:m_rasterizer(rasterizer)
{
}

RAS_OpenGLRasterizer::~RAS_OpenGLRasterizer()
{
}

unsigned short RAS_OpenGLRasterizer::GetNumLights() const
{
	int numlights = 0;
	glGetIntegerv(GL_MAX_LIGHTS, (GLint *)&numlights);

	if (numlights > 8) {
		return 8;
	}
	return numlights;
}

void RAS_OpenGLRasterizer::Enable(RAS_Rasterizer::EnableBit bit)
{
	glEnable(openGLEnableBitEnums[bit]);
}

void RAS_OpenGLRasterizer::Disable(RAS_Rasterizer::EnableBit bit)
{
	glDisable(openGLEnableBitEnums[bit]);
}

void RAS_OpenGLRasterizer::SetDepthFunc(RAS_Rasterizer::DepthFunc func)
{
	glDepthFunc(openGLDepthFuncEnums[func]);
}

void RAS_OpenGLRasterizer::SetBlendFunc(RAS_Rasterizer::BlendFunc src, RAS_Rasterizer::BlendFunc dst)
{
	glBlendFunc(openGLBlendFuncEnums[src], openGLBlendFuncEnums[dst]);
}

void RAS_OpenGLRasterizer::BeginFrame()
{
}

void RAS_OpenGLRasterizer::SetDepthMask(RAS_Rasterizer::DepthMask depthmask)
{
	glDepthMask(depthmask == RAS_Rasterizer::RAS_DEPTHMASK_DISABLED ? GL_FALSE : GL_TRUE);
}

unsigned int *RAS_OpenGLRasterizer::MakeScreenshot(int x, int y, int width, int height)
{
	unsigned int *pixeldata = nullptr;

	if (width && height) {
		pixeldata = (unsigned int *)malloc(sizeof(unsigned int) * width * height);
		glReadPixels(x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixeldata);
	}

	return pixeldata;
}

void RAS_OpenGLRasterizer::Clear(int clearbit)
{
	GLbitfield glclearbit = 0;

	if (clearbit & RAS_Rasterizer::RAS_COLOR_BUFFER_BIT) {
		glclearbit |= GL_COLOR_BUFFER_BIT;
	}
	if (clearbit & RAS_Rasterizer::RAS_DEPTH_BUFFER_BIT) {
		glclearbit |= GL_DEPTH_BUFFER_BIT;
	}
	if (clearbit & RAS_Rasterizer::RAS_STENCIL_BUFFER_BIT) {
		glclearbit |= GL_STENCIL_BUFFER_BIT;
	}

	glClear(glclearbit);
}

void RAS_OpenGLRasterizer::SetClearColor(float r, float g, float b, float a)
{
	glClearColor(r, g, b, a);
}

void RAS_OpenGLRasterizer::SetClearDepth(float d)
{
	glClearDepth(d);
}

void RAS_OpenGLRasterizer::SetColorMask(bool r, bool g, bool b, bool a)
{
	glColorMask(r ? GL_TRUE : GL_FALSE, g ? GL_TRUE : GL_FALSE, b ? GL_TRUE : GL_FALSE, a ? GL_TRUE : GL_FALSE);
}

void RAS_OpenGLRasterizer::DrawOverlayPlane()
{
	m_screenPlane.Render();
}

// Code for hooking into Blender's mesh drawing for derived meshes.
// If/when we use more of Blender's drawing code, we may be able to
// clean this up
static int current_blmat_nr;
static GPUVertexAttribs current_gpu_attribs;
static int CheckMaterialDM(int matnr, void *attribs)
{
	// only draw the current material
	if (matnr != current_blmat_nr) {
		return 0;
	}
	GPUVertexAttribs *gattribs = (GPUVertexAttribs *)attribs;
	if (gattribs) {
		memcpy(gattribs, &current_gpu_attribs, sizeof(GPUVertexAttribs));
	}
	return 1;
}

void RAS_OpenGLRasterizer::SetViewport(int x, int y, int width, int height)
{
	glViewport(x, y, width, height);
}

void RAS_OpenGLRasterizer::SetScissor(int x, int y, int width, int height)
{
	glScissor(x, y, width, height);
}

void RAS_OpenGLRasterizer::SetLines(bool enable)
{
	if (enable) {
		glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
		glLineWidth(1.0f);
	}
	else {
		glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
	}
}

void RAS_OpenGLRasterizer::SetPolygonOffset(float mult, float add)
{
	glPolygonOffset(mult, add);
}

void RAS_OpenGLRasterizer::EnableClipPlane(int numplanes)
{
	for (int i = 0; i < numplanes; ++i) {
		glEnable(GL_CLIP_DISTANCE0 + i);
	}
}

void RAS_OpenGLRasterizer::DisableClipPlane(int numplanes)
{
	for (int i = 0; i < numplanes; ++i) {
		glDisable(GL_CLIP_DISTANCE0 + i);
	}
}

void RAS_OpenGLRasterizer::SetFrontFace(bool ccw)
{
	if (ccw) {
		glFrontFace(GL_CCW);
	}
	else {
		glFrontFace(GL_CW);
	}
}

void RAS_OpenGLRasterizer::DisableForText()
{
	for (int i = 0; i < RAS_Texture::MaxUnits; i++) {
		glActiveTexture(GL_TEXTURE0 + i);

		if (GLEW_ARB_texture_cube_map) {
			Disable(RAS_Rasterizer::RAS_TEXTURE_CUBE_MAP);
		}
		Disable(RAS_Rasterizer::RAS_TEXTURE_2D);
	}

	glActiveTexture(GL_TEXTURE0);
}

void RAS_OpenGLRasterizer::RenderText3D(
        int fontid, const std::string& text, int size, int dpi,
        const float color[4], const float mat[16], float aspect)
{
	/* gl prepping */
	m_rasterizer->DisableForText();
	SetFrontFace(true);

	/* the actual drawing */
	BLF_color4fv(fontid, color);

	/* multiply the text matrix by the object matrix */
	BLF_enable(fontid, BLF_MATRIX | BLF_ASPECT);
	BLF_matrix(fontid, mat);

	/* aspect is the inverse scale that allows you to increase
	 * your resolution without sizing the final text size
	 * the bigger the size, the smaller the aspect */
	BLF_aspect(fontid, aspect, aspect, aspect);

	BLF_size(fontid, size, dpi);
	BLF_position(fontid, 0, 0, 0);
	BLF_draw(fontid, text.c_str(), text.size());

	BLF_disable(fontid, BLF_MATRIX | BLF_ASPECT);

	m_rasterizer->SetAlphaBlend(GPU_BLEND_SOLID);
}

void RAS_OpenGLRasterizer::PrintHardwareInfo()
{
	CM_Message("GL_VENDOR: " << glGetString(GL_VENDOR));
	CM_Message("GL_RENDERER: " << glGetString(GL_RENDERER));
	CM_Message("GL_VERSION:  " << glGetString(GL_VERSION));
	bool support=0;
	CM_Message("Supported Extensions...");
	CM_Message(" GL_ARB_shader_objects supported?       "<< (GLEW_ARB_shader_objects?"yes.":"no."));
	CM_Message(" GL_ARB_geometry_shader4 supported?     "<< (GLEW_ARB_geometry_shader4 ? "yes." : "no."));

	support= GLEW_ARB_vertex_shader;
	CM_Message(" GL_ARB_vertex_shader supported?        "<< (support?"yes.":"no."));
	if (support) {
		CM_Message(" ----------Details----------");
		int max=0;
		glGetIntegerv(GL_MAX_VERTEX_UNIFORM_COMPONENTS, (GLint*)&max);
		CM_Message("  Max uniform components." << max);

		glGetIntegerv(GL_MAX_VARYING_FLOATS, (GLint*)&max);
		CM_Message("  Max varying floats." << max);

		glGetIntegerv(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS, (GLint*)&max);
		CM_Message("  Max vertex texture units." << max);

		glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, (GLint*)&max);
		CM_Message("  Max vertex attribs." << max);

		glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, (GLint*)&max);
		CM_Message("  Max combined texture units." << max);
		CM_Message("");
	}

	support=GLEW_ARB_fragment_shader;
	CM_Message(" GL_ARB_fragment_shader supported?      "<< (support?"yes.":"no."));
	if (support) {
		CM_Message(" ----------Details----------");
		int max=0;
		glGetIntegerv(GL_MAX_FRAGMENT_UNIFORM_COMPONENTS, (GLint*)&max);
		CM_Message("  Max uniform components." << max);
		CM_Message("");
	}

	support = GLEW_ARB_texture_cube_map;
	CM_Message(" GL_ARB_texture_cube_map supported?     "<< (support?"yes.":"no."));
	if (support) {
		CM_Message(" ----------Details----------");
		int size=0;
		glGetIntegerv(GL_MAX_CUBE_MAP_TEXTURE_SIZE, (GLint*)&size);
		CM_Message("  Max cubemap size." << size);
		CM_Message("");
	}

	support = GLEW_ARB_multitexture;
	CM_Message(" GL_ARB_multitexture supported?         "<< (support?"yes.":"no."));
	if (support) {
		CM_Message(" ----------Details----------");
		int units=0;
		glGetIntegerv(GL_MAX_TEXTURE_UNITS, (GLint*)&units);
		CM_Message("  Max texture units available.  " << units);
		CM_Message("");
	}

	CM_Message(" GL_ARB_texture_env_combine supported?  "<< (GLEW_ARB_texture_env_combine?"yes.":"no."));

	CM_Message(" GL_ARB_draw_instanced supported?  "<< (GLEW_ARB_draw_instanced?"yes.":"no."));
}
