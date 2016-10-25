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


#include <math.h>
#include <stdlib.h>

#include "RAS_OpenGLRasterizer.h"

#include "glew-mx.h"

#include "RAS_ICanvas.h"
#include "RAS_Rect.h"
#include "RAS_ITexVert.h"
#include "RAS_MeshObject.h"
#include "RAS_MeshUser.h"
#include "RAS_TextUser.h"
#include "RAS_Polygon.h"
#include "RAS_DisplayArray.h"
#include "RAS_ILightObject.h"
#include "MT_CmMatrix4x4.h"

#include "RAS_OpenGLLight.h"
#include "RAS_OpenGLSync.h"

#include "RAS_StorageVA.h"
#include "RAS_StorageVBO.h"

#include "GPU_draw.h"
#include "GPU_extensions.h"
#include "GPU_material.h"
#include "GPU_shader.h"
#include "GPU_framebuffer.h"
#include "GPU_texture.h"

#include "BLI_math_vector.h"
#include "BLI_math_matrix.h"

extern "C" {
	#include "BLF_api.h"
	#include "BKE_DerivedMesh.h"
}

#include "MEM_guardedalloc.h"

// XXX Clean these up <<<
#include "EXP_Value.h"
#include "KX_Scene.h"
#include "KX_RayCast.h"
#include "KX_GameObject.h"
// >>>

// WARNING: Always respect the order from RAS_IRasterizer::EnableBit.
static int openGLEnableBitEnums[] = {
	GL_DEPTH_TEST, // RAS_DEPTH_TEST
	GL_ALPHA_TEST, // RAS_ALPHA_TEST
	GL_SCISSOR_TEST, // RAS_SCISSOR_TEST
	GL_TEXTURE_2D, // RAS_TEXTURE_2D
	GL_TEXTURE_CUBE_MAP_ARB, // RAS_TEXTURE_CUBE_MAP
	GL_BLEND, // RAS_BLEND
	GL_COLOR_MATERIAL, // RAS_COLOR_MATERIAL
	GL_CULL_FACE, // RAS_CULL_FACE
	GL_FOG, // RAS_FOG
	GL_LIGHTING, // RAS_LIGHTING
	GL_MULTISAMPLE_ARB, // RAS_MULTISAMPLE
	GL_POLYGON_STIPPLE, // RAS_POLYGON_STIPPLE
	GL_POLYGON_OFFSET_FILL, // RAS_POLYGON_OFFSET_FILL
	GL_POLYGON_OFFSET_LINE, // RAS_POLYGON_OFFSET_LINE
	GL_TEXTURE_GEN_S, // RAS_TEXTURE_GEN_S
	GL_TEXTURE_GEN_T, // RAS_TEXTURE_GEN_T
	GL_TEXTURE_GEN_R, // RAS_TEXTURE_GEN_R
	GL_TEXTURE_GEN_Q // RAS_TEXTURE_GEN_Q
};

// WARNING: Always respect the order from RAS_IRasterizer::DepthFunc.
static int openGLDepthFuncEnums[] = {
	GL_NEVER, // RAS_NEVER
	GL_LEQUAL, // RAS_LEQUAL
	GL_LESS, // RAS_LESS
	GL_ALWAYS, // RAS_ALWAYS
	GL_GEQUAL, // RAS_GEQUAL
	GL_GREATER, // RAS_GREATER
	GL_NOTEQUAL, // RAS_NOTEQUAL
	GL_EQUAL // RAS_EQUAL
};

// WARNING: Always respect the order from RAS_IRasterizer::MatrixMode.
static int openGLMatrixModeEnums[] = {
	GL_PROJECTION, // RAS_PROJECTION
	GL_MODELVIEW, // RAS_MODELVIEW
	GL_TEXTURE // RAS_TEXTURE
};

// WARNING: Always respect the order from RAS_IRasterizer::BlendFunc.
static int openGLBlendFuncEnums[] = {
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
	// Generate the VBO and IBO for screen overlay plane.
	glGenBuffersARB(1, &m_vbo);
	glGenBuffersARB(1, &m_ibo);

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
	glBindBufferARB(GL_ELEMENT_ARRAY_BUFFER_ARB, m_ibo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

	// Send vertexes in the screen plane VBO.
	glBindBufferARB(GL_ARRAY_BUFFER_ARB, m_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

	// Unbind modified VBOs
	glBindBufferARB(GL_ARRAY_BUFFER_ARB, 0);
	glBindBufferARB(GL_ELEMENT_ARRAY_BUFFER_ARB, 0);
}

RAS_OpenGLRasterizer::ScreenPlane::~ScreenPlane()
{
	// Delete screen overlay plane VBO/IBO
	glDeleteBuffersARB(1, &m_vbo);
	glDeleteBuffersARB(1, &m_ibo);
}

inline void RAS_OpenGLRasterizer::ScreenPlane::Render()
{
	// Bind screen plane VBO/IBO
	glBindBufferARB(GL_ARRAY_BUFFER_ARB, m_vbo);
	glBindBufferARB(GL_ELEMENT_ARRAY_BUFFER_ARB, m_ibo);

	// Enable vertex/uv pointer.
	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);

	// Bind vertex/uv pointer with VBO offset. (position = 0, uv = 3*float, stride = 5*float).
	glVertexPointer(3, GL_FLOAT, sizeof(float) * 5, 0);
	glTexCoordPointer(2, GL_FLOAT, sizeof(float) * 5, ((char *)NULL) + sizeof(float) * 3);

	// Draw in traignel fan mode to reduce IBO size.
	glDrawElements(GL_TRIANGLE_FAN, 4, GL_UNSIGNED_BYTE, 0);

	// Disable vertex/uv pointer.
	glDisableClientState(GL_VERTEX_ARRAY);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);

	// Unbind screen plane VBO/IBO.
	glBindBufferARB(GL_ARRAY_BUFFER_ARB, 0);
	glBindBufferARB(GL_ELEMENT_ARRAY_BUFFER_ARB, 0);
}

RAS_OpenGLRasterizer::OffScreens::OffScreens()
	:m_currentIndex(-1),
	m_width(0),
	m_height(0),
	m_samples(-1),
	m_hdr(RAS_HDR_NONE)
{
	for (unsigned short i = 0; i < RAS_IRasterizer::RAS_OFFSCREEN_MAX; ++i) {
		m_offScreens[i] = NULL;
	}
}

RAS_OpenGLRasterizer::OffScreens::~OffScreens()
{
	for (unsigned short i = 0; i < RAS_IRasterizer::RAS_OFFSCREEN_MAX; ++i) {
		if (m_offScreens[i]) {
			GPU_offscreen_free(m_offScreens[i]);
		}
	}
}

GPUOffScreen *RAS_OpenGLRasterizer::OffScreens::GetOffScreen(unsigned short index)
{
	const short lastIndex = m_currentIndex;

	if (!m_offScreens[index]) {
		// The offscreen need to be created now.

		// Check if the off screen index can support samples.
		const bool sampleofs = index == RAS_OFFSCREEN_RENDER ||
							   index == RAS_OFFSCREEN_EYE_LEFT0 ||
							   index == RAS_OFFSCREEN_EYE_RIGHT0;

		/* Some GPUs doesn't support high multisample value with GL_RGBA16F or GL_RGBA32F.
		 * To avoid crashing we check if the off screen was created and if not decremente
		 * the multisample value and try to create the off screen to find a supported value.
		 */
		for (int samples = m_samples; samples >= 0; --samples) {
			// Get off screen mode : render buffer support for multisampled off screen.
			int mode = GPU_OFFSCREEN_MODE_NONE;
			if (sampleofs && (samples > 0)) {
				mode = GPU_OFFSCREEN_RENDERBUFFER_COLOR | GPU_OFFSCREEN_RENDERBUFFER_DEPTH;
			}

			char errout[256];
			GPUOffScreen *ofs = GPU_offscreen_create(m_width, m_height, sampleofs ? samples : 0, (GPUHDRType)m_hdr, mode, errout);
			if (ofs) {
				m_offScreens[index] = ofs;
				m_samples = samples;
				break;
			}
		}

		/* Creating an off screen restore the default frame buffer object.
		 * We have to rebind the last off screen. */
		if (lastIndex != -1) {
			Bind(lastIndex);
		}
	}

	BLI_assert(lastIndex == m_currentIndex);

	return m_offScreens[index];
}

inline void RAS_OpenGLRasterizer::OffScreens::Update(RAS_ICanvas *canvas)
{
	const unsigned int width = canvas->GetWidth() + 1;
	const unsigned int height = canvas->GetHeight() + 1;

	if (width == m_width && height == m_height) {
		// No resize detected.
		return;
	}

	m_width = width;
	m_height = height;

	// The samples value was not yet set.
	if (m_samples == -1) {
		m_samples = canvas->GetSamples();
	}

	switch (canvas->GetHdrType()) {
		case RAS_HDR_NONE:
		{
			m_hdr = GPU_HDR_NONE;
			break;
		}
		case RAS_HDR_HALF_FLOAT:
		{
			m_hdr = GPU_HDR_HALF_FLOAT;
			break;
		}
		case RAS_HDR_FULL_FLOAT:
		{
			m_hdr = GPU_HDR_FULL_FLOAT;
			break;
		}
	}

	// Destruct all off screens.
	for (unsigned short i = 0; i < RAS_IRasterizer::RAS_OFFSCREEN_MAX; ++i) {
		if (m_offScreens[i]) {
			GPU_offscreen_free(m_offScreens[i]);
			m_offScreens[i] = NULL;
		}
	}
}

inline void RAS_OpenGLRasterizer::OffScreens::Bind(unsigned short index)
{
	GPU_offscreen_bind_simple(GetOffScreen(index));

	m_currentIndex = index;
}

inline void RAS_OpenGLRasterizer::OffScreens::RestoreScreen()
{
	GPU_framebuffer_restore();

	m_currentIndex = -1;
}

inline void RAS_OpenGLRasterizer::OffScreens::Blit(unsigned short srcindex, unsigned short dstindex, bool color, bool depth)
{
	GPU_offscreen_blit(GetOffScreen(srcindex), GetOffScreen(dstindex), color, depth);
}

inline void RAS_OpenGLRasterizer::OffScreens::BindTexture(unsigned short index, unsigned short slot, OffScreen type)
{
	GPUTexture *tex = NULL;
	GPUOffScreen *ofs = GetOffScreen(index);
	if (type == RAS_IRasterizer::RAS_OFFSCREEN_COLOR) {
		tex = GPU_offscreen_texture(ofs);
	}
	else if (type == RAS_IRasterizer::RAS_OFFSCREEN_DEPTH) {
		tex = GPU_offscreen_depth_texture(ofs);
	}
	GPU_texture_bind(tex, slot);
}

inline void RAS_OpenGLRasterizer::OffScreens::UnbindTexture(unsigned short index, OffScreen type)
{
	GPUTexture *tex = NULL;
	GPUOffScreen *ofs = GetOffScreen(index);
	if (type == RAS_IRasterizer::RAS_OFFSCREEN_COLOR) {
		tex = GPU_offscreen_texture(ofs);
	}
	else if (type == RAS_IRasterizer::RAS_OFFSCREEN_DEPTH) {
		tex = GPU_offscreen_depth_texture(ofs);
	}
	GPU_texture_unbind(tex);
}

inline short RAS_OpenGLRasterizer::OffScreens::GetCurrentIndex() const
{
	return m_currentIndex;
}

inline int RAS_OpenGLRasterizer::OffScreens::GetSamples(unsigned short index)
{
	return GPU_offscreen_samples(GetOffScreen(index));
}

inline GPUTexture *RAS_OpenGLRasterizer::OffScreens::GetDepthTexture(unsigned short index)
{
	return GPU_offscreen_depth_texture(GetOffScreen(index));
}

unsigned short RAS_IRasterizer::NextFilterOffScreen(unsigned short index)
{
	switch (index) {
		case RAS_OFFSCREEN_FILTER0:
		{
			return RAS_OFFSCREEN_FILTER1;
		}
		case RAS_OFFSCREEN_FILTER1:
		{
			return RAS_OFFSCREEN_FILTER0;
		}
	}

	// Passing a non-filter frame buffer is allowed.
	return RAS_OFFSCREEN_FILTER0;
}

unsigned short RAS_IRasterizer::NextEyeOffScreen(unsigned short index)
{
	switch (index) {
		case RAS_OFFSCREEN_EYE_LEFT0:
		{
			return RAS_OFFSCREEN_EYE_LEFT1;
		}
		case RAS_OFFSCREEN_EYE_LEFT1:
		{
			return RAS_OFFSCREEN_EYE_LEFT0;
		}
		case RAS_OFFSCREEN_EYE_RIGHT0:
		{
			return RAS_OFFSCREEN_EYE_RIGHT1;
		}
		case RAS_OFFSCREEN_EYE_RIGHT1:
		{
			return RAS_OFFSCREEN_EYE_RIGHT0;
		}
	}

	// Passing a non-eye frame buffer is disallowed.
	BLI_assert(false);

	return RAS_OFFSCREEN_EYE_LEFT0;
}

RAS_OpenGLRasterizer::RAS_OpenGLRasterizer()
	: m_fogenabled(false),
	m_time(0.0f),
	m_campos(0.0f, 0.0f, 0.0f),
	m_camortho(false),
	m_camnegscale(false),
	m_stereomode(RAS_STEREO_NOSTEREO),
	m_curreye(RAS_STEREO_LEFTEYE),
	m_eyeseparation(0.0f),
	m_focallength(0.0f),
	m_setfocallength(false),
	m_noOfScanlines(32),
	m_motionblur(0),
	m_motionblurvalue(-1.0f),
	m_clientobject(NULL),
	m_auxilaryClientInfo(NULL),
	m_drawingmode(RAS_TEXTURED),
	m_shadowMode(RAS_SHADOW_NONE),
	//m_last_alphablend(GPU_BLEND_SOLID),
	m_last_frontface(true),
	m_overrideShader(RAS_OVERRIDE_SHADER_NONE)
{
	m_viewmatrix.setIdentity();
	m_viewinvmatrix.setIdentity();

	m_storages[RAS_STORAGE_VA] = new RAS_StorageVA(&m_storageAttribs);
	m_storages[RAS_STORAGE_VBO] = new RAS_StorageVBO(&m_storageAttribs);

	glGetIntegerv(GL_MAX_LIGHTS, (GLint *)&m_numgllights);
	if (m_numgllights < 8)
		m_numgllights = 8;

	InitOverrideShadersInterface();
}

RAS_OpenGLRasterizer::~RAS_OpenGLRasterizer()
{
	for (unsigned short i = 0; i < RAS_STORAGE_MAX; ++i) {
		delete m_storages[i];
	}
}

void RAS_OpenGLRasterizer::Enable(RAS_IRasterizer::EnableBit bit)
{
	glEnable(openGLEnableBitEnums[bit]);
}

void RAS_OpenGLRasterizer::Disable(RAS_IRasterizer::EnableBit bit)
{
	glDisable(openGLEnableBitEnums[bit]);
}

void RAS_OpenGLRasterizer::SetDepthFunc(RAS_IRasterizer::DepthFunc func)
{
	glDepthFunc(openGLDepthFuncEnums[func]);
}

void RAS_OpenGLRasterizer::SetBlendFunc(BlendFunc src, BlendFunc dst)
{
	glBlendFunc(openGLBlendFuncEnums[src], openGLBlendFuncEnums[dst]);
}

void RAS_OpenGLRasterizer::Init()
{
	GPU_state_init();

	m_ambr = 0.0f;
	m_ambg = 0.0f;
	m_ambb = 0.0f;

	Disable(RAS_BLEND);
	Disable(RAS_ALPHA_TEST);
	//m_last_alphablend = GPU_BLEND_SOLID;
	GPU_set_material_alpha_blend(GPU_BLEND_SOLID);

	SetFrontFace(true);

	SetColorMask(true, true, true, true);

	glShadeModel(GL_SMOOTH);

	for (unsigned short i = 0; i < RAS_STORAGE_MAX; ++i) {
		m_storages[i]->Init();
	}
}

void RAS_OpenGLRasterizer::SetAmbientColor(float color[3])
{
	m_ambr = color[0];
	m_ambg = color[1];
	m_ambb = color[2];
}

void RAS_OpenGLRasterizer::SetAmbient(float factor)
{
	float ambient[] = {m_ambr * factor, m_ambg * factor, m_ambb * factor, 1.0f};
	glLightModelfv(GL_LIGHT_MODEL_AMBIENT, ambient);
}

void RAS_OpenGLRasterizer::SetFog(short type, float start, float dist, float intensity, float color[3])
{
	float params[4] = {color[0], color[1], color[2], 1.0f};
	glFogi(GL_FOG_MODE, GL_LINEAR);
	glFogf(GL_FOG_DENSITY, intensity / 10.0f);
	glFogf(GL_FOG_START, start);
	glFogf(GL_FOG_END, start + dist);
	glFogfv(GL_FOG_COLOR, params);
}

void RAS_OpenGLRasterizer::EnableFog(bool enable)
{
	m_fogenabled = enable;
}

void RAS_OpenGLRasterizer::DisplayFog()
{
	if ((m_drawingmode >= RAS_SOLID) && m_fogenabled) {
		Enable(RAS_FOG);
	}
	else {
		Disable(RAS_FOG);
	}
}

void RAS_OpenGLRasterizer::Exit()
{
	for (unsigned short i = 0; i < RAS_STORAGE_MAX; ++i) {
		m_storages[i]->Exit();
	}

	Enable(RAS_CULL_FACE);
	Enable(RAS_DEPTH_TEST);

	SetClearDepth(1.0f);
	SetColorMask(true, true, true, true);

	SetClearColor(0.0f, 0.0f, 0.0f, 0.0f);

	Clear(RAS_COLOR_BUFFER_BIT | RAS_DEPTH_BUFFER_BIT);
	SetDepthMask(RAS_DEPTHMASK_ENABLED);
	SetDepthFunc(RAS_LEQUAL);
	SetBlendFunc(RAS_ONE, RAS_ZERO);

	Disable(RAS_POLYGON_STIPPLE);

	Disable(RAS_LIGHTING);
	if (GLEW_EXT_separate_specular_color || GLEW_VERSION_1_2)
		glLightModeli(GL_LIGHT_MODEL_COLOR_CONTROL, GL_SINGLE_COLOR);

	GPU_texture_set_global_depth(NULL);

	EndFrame();
}

void RAS_OpenGLRasterizer::DrawOverlayPlane()
{
	m_screenPlane.Render();
}

void RAS_OpenGLRasterizer::BeginFrame(double time)
{
	m_time = time;

	// Blender camera routine destroys the settings
	if (m_drawingmode < RAS_SOLID) {
		Disable(RAS_CULL_FACE);
		Disable(RAS_DEPTH_TEST);
	}
	else {
		Enable(RAS_CULL_FACE);
		Enable(RAS_DEPTH_TEST);
	}

	Disable(RAS_BLEND);
	Disable(RAS_ALPHA_TEST);
	//m_last_alphablend = GPU_BLEND_SOLID;
	GPU_set_material_alpha_blend(GPU_BLEND_SOLID);

	SetFrontFace(true);

	glShadeModel(GL_SMOOTH);

	Enable(RAS_MULTISAMPLE);

	Enable(RAS_SCISSOR_TEST);

	Enable(RAS_DEPTH_TEST);
	SetDepthFunc(RAS_LEQUAL);

	// Render Tools
	m_clientobject = NULL;
	m_lastlightlayer = -1;
	m_lastauxinfo = NULL;
	m_lastlighting = true; /* force disable in DisableOpenGLLights() */

	DisableOpenGLLights();
}

void RAS_OpenGLRasterizer::SetDrawingMode(RAS_IRasterizer::DrawType drawingmode)
{
	m_drawingmode = drawingmode;

	for (unsigned short i = 0; i < RAS_STORAGE_MAX; ++i) {
		m_storages[i]->SetDrawingMode(drawingmode);
	}
}

RAS_IRasterizer::DrawType RAS_OpenGLRasterizer::GetDrawingMode()
{
	return m_drawingmode;
}

void RAS_OpenGLRasterizer::SetShadowMode(RAS_IRasterizer::ShadowType shadowmode)
{
	m_shadowMode = shadowmode;
}

RAS_IRasterizer::ShadowType RAS_OpenGLRasterizer::GetShadowMode()
{
	return m_shadowMode;
}

void RAS_OpenGLRasterizer::SetDepthMask(DepthMask depthmask)
{
	glDepthMask(depthmask == RAS_DEPTHMASK_DISABLED ? GL_FALSE : GL_TRUE);
}

unsigned int *RAS_OpenGLRasterizer::MakeScreenshot(int x, int y, int width, int height)
{
	unsigned int *pixeldata = NULL;

	if (width && height) {
		pixeldata = (unsigned int*) malloc(sizeof(unsigned int) * width * height);
		glReadBuffer(GL_FRONT);
		glReadPixels(x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixeldata);
		glFinish();
		glReadBuffer(GL_BACK);
	}

	return pixeldata;
}

void RAS_OpenGLRasterizer::Clear(int clearbit)
{
	GLbitfield glclearbit = 0;

	if ((clearbit & RAS_COLOR_BUFFER_BIT) == RAS_COLOR_BUFFER_BIT) {
		glclearbit |= GL_COLOR_BUFFER_BIT;
	}
	if ((clearbit & RAS_DEPTH_BUFFER_BIT) == RAS_DEPTH_BUFFER_BIT) {
		glclearbit |= GL_DEPTH_BUFFER_BIT;
	}
	if ((clearbit & RAS_STENCIL_BUFFER_BIT) == RAS_STENCIL_BUFFER_BIT) {
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

void RAS_OpenGLRasterizer::FlushDebugShapes(SCA_IScene *scene)
{
	std::vector<OglDebugShape> &debugShapes = m_debugShapes[scene];
	if (debugShapes.empty())
		return;

	// DrawDebugLines
	GLboolean light, tex, blend;

	light = glIsEnabled(GL_LIGHTING);
	tex = glIsEnabled(GL_TEXTURE_2D);
	blend = glIsEnabled(GL_BLEND);

	if (light) {
		Disable(RAS_LIGHTING);
	}
	if (tex) {
		Disable(RAS_TEXTURE_2D);
	}
	if (!blend) {
		Enable(RAS_BLEND);
	}

	// draw lines
	glBegin(GL_LINES);
	for (unsigned int i = 0; i < debugShapes.size(); i++) {
		if (debugShapes[i].m_type != OglDebugShape::LINE)
			continue;
		glColor4f(debugShapes[i].m_color[0], debugShapes[i].m_color[1], debugShapes[i].m_color[2], debugShapes[i].m_color[3]);
		const MT_Scalar *fromPtr = &debugShapes[i].m_pos.x();
		const MT_Scalar *toPtr = &debugShapes[i].m_param.x();
		glVertex3fv(fromPtr);
		glVertex3fv(toPtr);
	}
	glEnd();

	glEnableClientState(GL_VERTEX_ARRAY);
	// Draw boxes
	for (unsigned int i = 0; i < debugShapes.size(); i++) {
		if (debugShapes[i].m_type != OglDebugShape::BOX) {
			continue;
		}
		glColor4f(debugShapes[i].m_color[0], debugShapes[i].m_color[1], debugShapes[i].m_color[2], debugShapes[i].m_color[3]);

		const MT_Matrix3x3& rot = debugShapes[i].m_rot;
		const MT_Vector3& pos = debugShapes[i].m_pos;
		float mat[16] = {
			rot[0][0], rot[1][0], rot[2][0], 0.0,
			rot[0][1], rot[1][1], rot[2][1], 0.0,
			rot[0][2], rot[1][2], rot[2][2], 0.0,
			pos[0], pos[1], pos[2], 1.0
		};
		PushMatrix();
		MultMatrix(mat);

		const MT_Vector3& min = debugShapes[i].m_param;
		const MT_Vector3& max = debugShapes[i].m_param2;

		float vertexes[24] = {
			(float)min[0], (float)min[1], (float)min[2],
			(float)max[0], (float)min[1], (float)min[2],
			(float)max[0], (float)max[1], (float)min[2],
			(float)min[0], (float)max[1], (float)min[2],
			(float)min[0], (float)min[1], (float)max[2],
			(float)max[0], (float)min[1], (float)max[2],
			(float)max[0], (float)max[1], (float)max[2],
			(float)min[0], (float)max[1], (float)max[2]
		};

		static unsigned short indexes[24] = {
			0, 1, 1, 2,
			2, 3, 3, 0,
			4, 5, 5, 6,
			6, 7, 7, 4,
			0, 4, 1, 5,
			2, 6, 3, 7
		};

		glVertexPointer(3, GL_FLOAT, 0, vertexes);
		glDrawElements(GL_LINES, 24, GL_UNSIGNED_SHORT, indexes);

		PopMatrix();
	}
	glDisableClientState(GL_VERTEX_ARRAY);

	// draw circles
	for (unsigned int i = 0; i < debugShapes.size(); i++) {
		if (debugShapes[i].m_type != OglDebugShape::CIRCLE)
			continue;
		glBegin(GL_LINE_LOOP);
		glColor4f(debugShapes[i].m_color[0], debugShapes[i].m_color[1], debugShapes[i].m_color[2], debugShapes[i].m_color[3]);

		static const MT_Vector3 worldUp(0.0f, 0.0f, 1.0f);
		MT_Vector3 norm = debugShapes[i].m_param;
		MT_Matrix3x3 tr;
		if (norm.fuzzyZero() || norm == worldUp) {
			tr.setIdentity();
		}
		else {
			MT_Vector3 xaxis, yaxis;
			xaxis = MT_cross(norm, worldUp);
			yaxis = MT_cross(xaxis, norm);
			tr.setValue(xaxis.x(), xaxis.y(), xaxis.z(),
			            yaxis.x(), yaxis.y(), yaxis.z(),
			            norm.x(), norm.y(), norm.z());
		}
		MT_Scalar rad = debugShapes[i].m_param2.x();
		int n = (int)debugShapes[i].m_param2.y();
		for (int j = 0; j < n; j++) {
			MT_Scalar theta = j * MT_2_PI / n;
			MT_Vector3 pos(cosf(theta) * rad, sinf(theta) * rad, 0.0f);
			pos = pos * tr;
			pos += debugShapes[i].m_pos;
			const MT_Scalar *posPtr = &pos.x();
			glVertex3fv(posPtr);
		}
		glEnd();
	}

	if (light) {
		Enable(RAS_LIGHTING);
	}
	if (tex) {
		Enable(RAS_TEXTURE_2D);
	}
	if (!blend) {
		Disable(RAS_BLEND);
	}

	debugShapes.clear();
}

void RAS_OpenGLRasterizer::DrawDebugLine(SCA_IScene *scene, const MT_Vector3 &from, const MT_Vector3 &to, const MT_Vector4 &color)
{
	OglDebugShape line;
	line.m_type = OglDebugShape::LINE;
	line.m_pos = from;
	line.m_param = to;
	line.m_color = color;
	m_debugShapes[scene].push_back(line);
}

void RAS_OpenGLRasterizer::DrawDebugCircle(SCA_IScene *scene, const MT_Vector3 &center, const MT_Scalar radius,
		const MT_Vector4 &color, const MT_Vector3 &normal, int nsector)
{
	OglDebugShape line;
	line.m_type = OglDebugShape::CIRCLE;
	line.m_pos = center;
	line.m_param = normal;
	line.m_color = color;
	line.m_param2.x() = radius;
	line.m_param2.y() = (float)nsector;
	m_debugShapes[scene].push_back(line);
}

void RAS_OpenGLRasterizer::DrawDebugBox(SCA_IScene *scene, const MT_Vector3& pos, const MT_Matrix3x3& rot,
		const MT_Vector3& min, const MT_Vector3& max, const MT_Vector4& color)
{
	OglDebugShape box;
	box.m_type = OglDebugShape::BOX;
	box.m_pos = pos;
	box.m_rot = rot;
	box.m_param = min;
	box.m_param2 = max;
	box.m_color = color;
	m_debugShapes[scene].push_back(box);
}

void RAS_OpenGLRasterizer::EndFrame()
{
	SetColorMask(true, true, true, true);

	Disable(RAS_MULTISAMPLE);

	Disable(RAS_FOG);
}

void RAS_OpenGLRasterizer::UpdateOffScreens(RAS_ICanvas *canvas)
{
	m_offScreens.Update(canvas);
}

void RAS_OpenGLRasterizer::BindOffScreen(unsigned short index)
{
	m_offScreens.Bind(index);
}

void RAS_OpenGLRasterizer::DrawOffScreen(unsigned short srcindex, unsigned short dstindex)
{
	if (m_offScreens.GetSamples(srcindex) == 0) {
		m_offScreens.BindTexture(srcindex, 0, RAS_IRasterizer::RAS_OFFSCREEN_COLOR);

		GPUShader *shader = GPU_shader_get_builtin_shader(GPU_SHADER_DRAW_FRAME_BUFFER);
		GPU_shader_bind(shader);

		OverrideShaderDrawFrameBufferInterface *interface = (OverrideShaderDrawFrameBufferInterface *)GPU_shader_get_interface(shader);
		GPU_shader_uniform_int(shader, interface->colorTexLoc, 0);

		DrawOverlayPlane();

		GPU_shader_unbind();

		m_offScreens.UnbindTexture(srcindex, RAS_IRasterizer::RAS_OFFSCREEN_COLOR);
	}
	else {
		m_offScreens.Blit(srcindex, dstindex, true, true);
	}
}

void RAS_OpenGLRasterizer::DrawOffScreen(RAS_ICanvas *canvas, unsigned short index)
{
	if (m_offScreens.GetSamples(index) > 0) {
		m_offScreens.Blit(index, RAS_OFFSCREEN_FINAL, true, false);
		index = RAS_OFFSCREEN_FINAL;
	}

	const int *viewport = canvas->GetViewPort();
	SetViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
	SetScissor(viewport[0], viewport[1], viewport[2], viewport[3]);

	Disable(RAS_CULL_FACE);
	SetDepthFunc(RAS_ALWAYS);

	m_offScreens.RestoreScreen();
	DrawOffScreen(index, 0);

	SetDepthFunc(RAS_LEQUAL);
	Enable(RAS_CULL_FACE);
}

void RAS_OpenGLRasterizer::DrawStereoOffScreen(RAS_ICanvas *canvas, unsigned short lefteyeindex, unsigned short righteyeindex)
{
	if (m_offScreens.GetSamples(lefteyeindex) > 0) {
		// Then lefteyeindex == RAS_OFFSCREEN_EYE_LEFT0.
		m_offScreens.Blit(RAS_OFFSCREEN_EYE_LEFT0, RAS_OFFSCREEN_EYE_LEFT1, true, false);
		lefteyeindex = RAS_OFFSCREEN_EYE_LEFT1;
	}

	if (m_offScreens.GetSamples(righteyeindex) > 0) {
		// Then righteyeindex == RAS_OFFSCREEN_EYE_RIGHT0.
		m_offScreens.Blit(RAS_OFFSCREEN_EYE_RIGHT0, RAS_OFFSCREEN_EYE_RIGHT1, true, false);
		righteyeindex = RAS_OFFSCREEN_EYE_RIGHT1;
	}

	const int *viewport = canvas->GetViewPort();
	SetViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
	SetScissor(viewport[0], viewport[1], viewport[2], viewport[3]);

	Disable(RAS_CULL_FACE);
	SetDepthFunc(RAS_ALWAYS);

	m_offScreens.RestoreScreen();

	if (m_stereomode == RAS_STEREO_VINTERLACE || m_stereomode == RAS_STEREO_INTERLACED) {
		GPUShader *shader = GPU_shader_get_builtin_shader(GPU_SHADER_STEREO_STIPPLE);
		GPU_shader_bind(shader);

		OverrideShaderStereoStippleInterface *interface = (OverrideShaderStereoStippleInterface *)GPU_shader_get_interface(shader);

		m_offScreens.BindTexture(lefteyeindex, 0, RAS_IRasterizer::RAS_OFFSCREEN_COLOR);
		m_offScreens.BindTexture(righteyeindex, 1, RAS_IRasterizer::RAS_OFFSCREEN_COLOR);

		GPU_shader_uniform_int(shader, interface->leftEyeTexLoc, 0);
		GPU_shader_uniform_int(shader, interface->rightEyeTexLoc, 1);
		GPU_shader_uniform_int(shader, interface->stippleIdLoc, (m_stereomode == RAS_STEREO_INTERLACED) ? 1 : 0);

		DrawOverlayPlane();

		GPU_shader_unbind();

		m_offScreens.UnbindTexture(lefteyeindex, RAS_IRasterizer::RAS_OFFSCREEN_COLOR);
		m_offScreens.UnbindTexture(righteyeindex, RAS_IRasterizer::RAS_OFFSCREEN_COLOR);
	}
	else if (m_stereomode == RAS_STEREO_ANAGLYPH) {
		GPUShader *shader = GPU_shader_get_builtin_shader(GPU_SHADER_STEREO_ANAGLYPH);
		GPU_shader_bind(shader);

		OverrideShaderStereoAnaglyph *interface = (OverrideShaderStereoAnaglyph *)GPU_shader_get_interface(shader);

		m_offScreens.BindTexture(lefteyeindex, 0, RAS_IRasterizer::RAS_OFFSCREEN_COLOR);
		m_offScreens.BindTexture(righteyeindex, 1, RAS_IRasterizer::RAS_OFFSCREEN_COLOR);

		GPU_shader_uniform_int(shader, interface->leftEyeTexLoc, 0);
		GPU_shader_uniform_int(shader, interface->rightEyeTexLoc, 1);

		DrawOverlayPlane();

		GPU_shader_unbind();

		m_offScreens.UnbindTexture(lefteyeindex, RAS_IRasterizer::RAS_OFFSCREEN_COLOR);
		m_offScreens.UnbindTexture(righteyeindex, RAS_IRasterizer::RAS_OFFSCREEN_COLOR);
	}

	SetDepthFunc(RAS_LEQUAL);
	Enable(RAS_CULL_FACE);
}

void RAS_OpenGLRasterizer::BindOffScreenTexture(unsigned short index, unsigned short slot, OffScreen type)
{
	m_offScreens.BindTexture(index, slot, type);
}

void RAS_OpenGLRasterizer::UnbindOffScreenTexture(unsigned short index, OffScreen type)
{
	m_offScreens.UnbindTexture(index, type);
}

short RAS_OpenGLRasterizer::GetCurrentOffScreenIndex() const
{
	return m_offScreens.GetCurrentIndex();
}

int RAS_OpenGLRasterizer::GetOffScreenSamples(unsigned short index)
{
	return m_offScreens.GetSamples(index);
}

void RAS_OpenGLRasterizer::SetRenderArea(RAS_ICanvas *canvas)
{
	if (canvas == NULL) {
		return;
	}

	RAS_Rect area;
	// only above/below stereo method needs viewport adjustment
	switch (m_stereomode)
	{
		case RAS_STEREO_ABOVEBELOW:
		{
			switch (m_curreye) {
				case RAS_STEREO_LEFTEYE:
				{
					// upper half of window
					area.SetLeft(0);
					area.SetBottom(canvas->GetHeight() -
								   int(canvas->GetHeight() - m_noOfScanlines) / 2);

					area.SetRight(int(canvas->GetWidth()));
					area.SetTop(int(canvas->GetHeight()));
					canvas->SetDisplayArea(&area);
					break;
				}
				case RAS_STEREO_RIGHTEYE:
				{
					// lower half of window
					area.SetLeft(0);
					area.SetBottom(0);
					area.SetRight(int(canvas->GetWidth()));
					area.SetTop(int(canvas->GetHeight() - m_noOfScanlines) / 2);
					canvas->SetDisplayArea(&area);
					break;
				}
			}
			break;
		}
		case RAS_STEREO_3DTVTOPBOTTOM:
		{
			switch (m_curreye) {
				case RAS_STEREO_LEFTEYE:
				{
					// upper half of window
					area.SetLeft(0);
					area.SetBottom(canvas->GetHeight() -
								   canvas->GetHeight() / 2);

					area.SetRight(canvas->GetWidth());
					area.SetTop(canvas->GetHeight());
					canvas->SetDisplayArea(&area);
					break;
				}
				case RAS_STEREO_RIGHTEYE:
				{
					// lower half of window
					area.SetLeft(0);
					area.SetBottom(0);
					area.SetRight(canvas->GetWidth());
					area.SetTop(canvas->GetHeight() / 2);
					canvas->SetDisplayArea(&area);
					break;
				}
			}
			break;
		}
		case RAS_STEREO_SIDEBYSIDE:
		{
			switch (m_curreye)
			{
				case RAS_STEREO_LEFTEYE:
				{
					// Left half of window
					area.SetLeft(0);
					area.SetBottom(0);
					area.SetRight(canvas->GetWidth() / 2);
					area.SetTop(canvas->GetHeight());
					canvas->SetDisplayArea(&area);
					break;
				}
				case RAS_STEREO_RIGHTEYE:
				{
					// Right half of window
					area.SetLeft(canvas->GetWidth() / 2);
					area.SetBottom(0);
					area.SetRight(canvas->GetWidth());
					area.SetTop(canvas->GetHeight());
					canvas->SetDisplayArea(&area);
					break;
				}
			}
			break;
		}
		default:
		{
			// every available pixel
			area.SetLeft(0);
			area.SetBottom(0);
			area.SetRight(int(canvas->GetWidth()));
			area.SetTop(int(canvas->GetHeight()));
			canvas->SetDisplayArea(&area);
			break;
		}
	}
}

void RAS_OpenGLRasterizer::SetStereoMode(const StereoMode stereomode)
{
	m_stereomode = stereomode;
}

RAS_IRasterizer::StereoMode RAS_OpenGLRasterizer::GetStereoMode()
{
	return m_stereomode;
}

bool RAS_OpenGLRasterizer::Stereo()
{
	if (m_stereomode > RAS_STEREO_NOSTEREO) // > 0
		return true;
	else
		return false;
}

void RAS_OpenGLRasterizer::SetEye(const StereoEye eye)
{
	m_curreye = eye;
}

RAS_IRasterizer::StereoEye RAS_OpenGLRasterizer::GetEye()
{
	return m_curreye;
}

void RAS_OpenGLRasterizer::SetEyeSeparation(const float eyeseparation)
{
	m_eyeseparation = eyeseparation;
}

float RAS_OpenGLRasterizer::GetEyeSeparation()
{
	return m_eyeseparation;
}

void RAS_OpenGLRasterizer::SetFocalLength(const float focallength)
{
	m_focallength = focallength;
	m_setfocallength = true;
}

float RAS_OpenGLRasterizer::GetFocalLength()
{
	return m_focallength;
}

RAS_ISync *RAS_OpenGLRasterizer::CreateSync(int type)
{
	RAS_ISync *sync;

	sync = new RAS_OpenGLSync();

	if (!sync->Create((RAS_ISync::RAS_SYNC_TYPE)type)) {
		delete sync;
		return NULL;
	}
	return sync;
}
void RAS_OpenGLRasterizer::SwapBuffers(RAS_ICanvas *canvas)
{
	canvas->SwapBuffers();
}

const MT_Matrix4x4& RAS_OpenGLRasterizer::GetViewMatrix() const
{
	return m_viewmatrix;
}

const MT_Matrix4x4& RAS_OpenGLRasterizer::GetViewInvMatrix() const
{
	return m_viewinvmatrix;
}

void RAS_OpenGLRasterizer::IndexPrimitivesText(RAS_MeshSlot *ms)
{
	RAS_TextUser *textUser = (RAS_TextUser *)ms->m_meshUser;

	float mat[16];
	memcpy(mat, textUser->GetMatrix(), sizeof(float) * 16);

	const MT_Vector3& spacing = textUser->GetSpacing();
	const MT_Vector3& offset = textUser->GetOffset();

	mat[12] += offset[0];
	mat[13] += offset[1];
	mat[14] += offset[2];

	for (unsigned short int i = 0, size = textUser->GetTexts().size(); i < size; ++i) {
		if (i != 0) {
			mat[12] -= spacing[0];
			mat[13] -= spacing[1];
			mat[14] -= spacing[2];
		}
		RenderText3D(textUser->GetFontId(), textUser->GetTexts()[i], textUser->GetSize(), textUser->GetDpi(),
					 textUser->GetColor().getValue(), mat, textUser->GetAspect());
	}
}

void RAS_OpenGLRasterizer::ClearTexCoords()
{
	m_storageAttribs.texcos.clear();
}

void RAS_OpenGLRasterizer::ClearAttribs()
{
	m_storageAttribs.attribs.clear();
}

void RAS_OpenGLRasterizer::ClearAttribLayers()
{
	m_storageAttribs.layers.clear();
}

void RAS_OpenGLRasterizer::SetTexCoords(const TexCoGenList& texcos)
{
	m_storageAttribs.texcos = texcos;
}

void RAS_OpenGLRasterizer::SetAttribs(const TexCoGenList& attribs)
{
	m_storageAttribs.attribs = attribs;
}

void RAS_OpenGLRasterizer::SetAttribLayers(const RAS_IRasterizer::AttribLayerList& layers)
{
	m_storageAttribs.layers = layers;
}

void RAS_OpenGLRasterizer::BindPrimitives(StorageType storage, RAS_DisplayArrayBucket *arrayBucket)
{
	if (arrayBucket && arrayBucket->GetDisplayArray() && storage != RAS_STORAGE_NONE) {
		// Set the proper uv layer for uv attributes.
		arrayBucket->SetAttribLayers(this);
		m_storages[storage]->BindPrimitives(arrayBucket);
	}
}

void RAS_OpenGLRasterizer::UnbindPrimitives(StorageType storage, RAS_DisplayArrayBucket *arrayBucket)
{
	if (arrayBucket && arrayBucket->GetDisplayArray() && storage != RAS_STORAGE_NONE) {
		m_storages[storage]->UnbindPrimitives(arrayBucket);
	}
}

void RAS_OpenGLRasterizer::IndexPrimitives(StorageType storage, RAS_MeshSlot *ms)
{
	if (ms->m_pDerivedMesh) {
		DrawDerivedMesh(ms);
	}
	else if (storage != RAS_STORAGE_NONE) {
		m_storages[storage]->IndexPrimitives(ms);
	}
}

void RAS_OpenGLRasterizer::IndexPrimitivesInstancing(StorageType storage, RAS_DisplayArrayBucket *arrayBucket)
{
	m_storages[storage]->IndexPrimitivesInstancing(arrayBucket);
}


// Code for hooking into Blender's mesh drawing for derived meshes.
// If/when we use more of Blender's drawing code, we may be able to
// clean this up
static bool current_wireframe;
static RAS_MaterialBucket *current_bucket;
static RAS_IPolyMaterial *current_polymat;
static RAS_MeshSlot *current_ms;
static RAS_MeshObject *current_mesh;
static int current_blmat_nr;
static GPUVertexAttribs current_gpu_attribs;
static Image *current_image;
static int CheckMaterialDM(int matnr, void *attribs)
{
	// only draw the current material
	if (matnr != current_blmat_nr)
		return 0;
	GPUVertexAttribs *gattribs = (GPUVertexAttribs *)attribs;
	if (gattribs)
		memcpy(gattribs, &current_gpu_attribs, sizeof(GPUVertexAttribs));
	return 1;
}

static DMDrawOption CheckTexDM(MTexPoly *mtexpoly, const bool has_mcol, int matnr)
{

	// index is the original face index, retrieve the polygon
	if (matnr == current_blmat_nr &&
		(mtexpoly == NULL || mtexpoly->tpage == current_image)) {
		// must handle color.
		if (current_wireframe)
			return DM_DRAW_OPTION_NO_MCOL;
		if (current_polymat->UsesObjectColor()) {
			const MT_Vector4& rgba = current_ms->m_meshUser->GetColor();
			glColor4d(rgba[0], rgba[1], rgba[2], rgba[3]);
			// don't use mcol
			return DM_DRAW_OPTION_NO_MCOL;
		}
		if (!has_mcol) {
			// we have to set the color from the material
			unsigned char rgba[4];
			current_polymat->GetRGBAColor(rgba);
			glColor4ubv((const GLubyte *)rgba);
			return DM_DRAW_OPTION_NORMAL;
		}
		return DM_DRAW_OPTION_NORMAL;
	}
	return DM_DRAW_OPTION_SKIP;
}

void RAS_OpenGLRasterizer::DrawDerivedMesh(RAS_MeshSlot *ms)
{
	// mesh data is in derived mesh
	current_bucket = ms->m_bucket;
	current_polymat = current_bucket->GetPolyMaterial();
	current_ms = ms;
	current_mesh = ms->m_mesh;
	current_wireframe = m_drawingmode <= RAS_IRasterizer::RAS_WIREFRAME;
	// MCol *mcol = (MCol*)ms->m_pDerivedMesh->getFaceDataArray(ms->m_pDerivedMesh, CD_MCOL); /* UNUSED */

	// handle two-side
	if (current_polymat->GetDrawingMode() & RAS_IRasterizer::RAS_BACKCULL)
		this->SetCullFace(true);
	else
		this->SetCullFace(false);

	if (current_bucket->IsWire()) {
		SetLines(true);
	}

	bool wireframe = (m_drawingmode == RAS_WIREFRAME);
	if (current_polymat->GetFlag() & RAS_BLENDERGLSL) {
		// GetMaterialIndex return the original mface material index,
		// increment by 1 to match what derived mesh is doing
		current_blmat_nr = current_ms->m_meshMaterial->m_index + 1;
		// For GLSL we need to retrieve the GPU material attribute
		Material* blmat = current_polymat->GetBlenderMaterial();
		Scene* blscene = current_polymat->GetBlenderScene();
		if (!current_wireframe && blscene && blmat)
			GPU_material_vertex_attributes(GPU_material_from_blender(blscene, blmat, false, current_polymat->UseInstancing()), &current_gpu_attribs);
		else
			memset(&current_gpu_attribs, 0, sizeof(current_gpu_attribs));
		// DM draw can mess up blending mode, restore at the end
		int current_blend_mode = GPU_get_material_alpha_blend();

		if (wireframe) {
			glColor4f(0.0f, 0.0f, 0.0f, 1.0f);
		}
		ms->m_pDerivedMesh->drawFacesGLSL(ms->m_pDerivedMesh, CheckMaterialDM);
		GPU_set_material_alpha_blend(current_blend_mode);
	} else {
		//ms->m_pDerivedMesh->drawMappedFacesTex(ms->m_pDerivedMesh, CheckTexfaceDM, mcol);
		current_blmat_nr = current_ms->m_meshMaterial->m_index;
		current_image = current_polymat->GetBlenderImage();

		if (wireframe) {
			glColor4f(0.0f, 0.0f, 0.0f, 1.0f);
		}
		ms->m_pDerivedMesh->drawFacesTex(ms->m_pDerivedMesh, CheckTexDM, NULL, NULL, DM_DRAW_USE_ACTIVE_UV);
	}

	if (current_bucket->IsWire()) {
		SetLines(false);
	}
}

void RAS_OpenGLRasterizer::SetProjectionMatrix(MT_CmMatrix4x4 &mat)
{
	SetMatrixMode(RAS_PROJECTION);
	float *matrix = &mat(0, 0);
	LoadMatrix(matrix);

	m_camortho = (mat(3, 3) != 0.0f);
}

void RAS_OpenGLRasterizer::SetProjectionMatrix(const MT_Matrix4x4 & mat)
{
	SetMatrixMode(RAS_PROJECTION);
	float matrix[16];
	/* Get into argument. Looks a bit dodgy, but it's ok. */
	mat.getValue(matrix);
	LoadMatrix(matrix);

	m_camortho = (mat[3][3] != 0.0f);
}

MT_Matrix4x4 RAS_OpenGLRasterizer::GetFrustumMatrix(
    float left,
    float right,
    float bottom,
    float top,
    float frustnear,
    float frustfar,
    float focallength,
    bool perspective)
{
	MT_Matrix4x4 result;
	float mat[16];

	// correction for stereo
	if (Stereo()) {
		float near_div_focallength;
		float offset;

		// if Rasterizer.setFocalLength is not called we use the camera focallength
		if (!m_setfocallength) {
			// if focallength is null we use a value known to be reasonable
			m_focallength = (focallength == 0.0f) ? m_eyeseparation * 30.0f
							: focallength;
		}

		near_div_focallength = frustnear / m_focallength;
		offset = 0.5f * m_eyeseparation * near_div_focallength;
		switch (m_curreye) {
			case RAS_STEREO_LEFTEYE:
			{
				left += offset;
				right += offset;
				break;
			}
			case RAS_STEREO_RIGHTEYE:
			{
				left -= offset;
				right -= offset;
				break;
			}
		}
		// leave bottom and top untouched
		if (m_stereomode == RAS_STEREO_3DTVTOPBOTTOM) {
			// restore the vertical frustum because the 3DTV will
			// expand the top and bottom part to the full size of the screen
			bottom *= 2.0f;
			top *= 2.0f;
		}
	}

	SetMatrixMode(RAS_PROJECTION);
	LoadIdentity();
	glFrustum(left, right, bottom, top, frustnear, frustfar);

	glGetFloatv(GL_PROJECTION_MATRIX, mat);
	result.setValue(mat);

	return result;
}

MT_Matrix4x4 RAS_OpenGLRasterizer::GetOrthoMatrix(
    float left,
    float right,
    float bottom,
    float top,
    float frustnear,
    float frustfar)
{
	MT_Matrix4x4 result;
	float mat[16];

	// stereo is meaningless for orthographic, disable it
	SetMatrixMode(RAS_PROJECTION);
	LoadIdentity();
	glOrtho(left, right, bottom, top, frustnear, frustfar);

	glGetFloatv(GL_PROJECTION_MATRIX, mat);
	result.setValue(mat);

	return result;
}

// next arguments probably contain redundant info, for later...
void RAS_OpenGLRasterizer::SetViewMatrix(const MT_Matrix4x4 &mat,
                                         const MT_Matrix3x3 & camOrientMat3x3,
                                         const MT_Vector3 & pos,
					 const MT_Vector3 & scale,
                                         bool perspective)
{
	m_viewmatrix = mat;

	// correction for stereo
	if (Stereo() && perspective) {
		MT_Vector3 unitViewDir(0.0f, -1.0f, 0.0f);  // minus y direction, Blender convention
		MT_Vector3 unitViewupVec(0.0f, 0.0f, 1.0f);
		MT_Vector3 viewDir, viewupVec;
		MT_Vector3 eyeline;

		// actual viewDir
		viewDir = camOrientMat3x3 * unitViewDir;  // this is the moto convention, vector on right hand side
		// actual viewup vec
		viewupVec = camOrientMat3x3 * unitViewupVec;

		// vector between eyes
		eyeline = viewDir.cross(viewupVec);

		switch (m_curreye) {
			case RAS_STEREO_LEFTEYE:
			{
				// translate to left by half the eye distance
				MT_Transform transform;
				transform.setIdentity();
				transform.translate(-(eyeline * m_eyeseparation / 2.0f));
				m_viewmatrix *= MT_Matrix4x4(transform);
				break;
			}
			case RAS_STEREO_RIGHTEYE:
			{
				// translate to right by half the eye distance
				MT_Transform transform;
				transform.setIdentity();
				transform.translate(eyeline * m_eyeseparation / 2.0f);
				m_viewmatrix *= MT_Matrix4x4(transform);
				break;
			}
		}
	}

	// Don't making variable negX/negY/negZ allow drastic time saving.
	if (scale[0] < 0.0f || scale[1] < 0.0f || scale[2] < 0.0f) {
		const bool negX = (scale[0] < 0.0f);
		const bool negY = (scale[1] < 0.0f);
		const bool negZ = (scale[2] < 0.0f);
		m_viewmatrix.tscale((negX) ? -1.0f : 1.0f, (negY) ? -1.0f : 1.0f, (negZ) ? -1.0f : 1.0f, 1.0f);
		m_camnegscale = negX ^ negY ^ negZ;
	}
	else {
		m_camnegscale = false;
	}
	m_viewinvmatrix = m_viewmatrix;
	m_viewinvmatrix.invert();

	// note: getValue gives back column major as needed by OpenGL
	MT_Scalar glviewmat[16];
	m_viewmatrix.getValue(glviewmat);

	SetMatrixMode(RAS_MODELVIEW);
	LoadMatrix(glviewmat);
	m_campos = pos;
}

void RAS_OpenGLRasterizer::SetViewport(int x, int y, int width, int height)
{
	glViewport(x, y, width, height);
}

void RAS_OpenGLRasterizer::GetViewport(int *rect)
{
	glGetIntegerv(GL_VIEWPORT, rect);
}

void RAS_OpenGLRasterizer::SetScissor(int x, int y, int width, int height)
{
	glScissor(x, y, width, height);
}

const MT_Vector3& RAS_OpenGLRasterizer::GetCameraPosition()
{
	return m_campos;
}

bool RAS_OpenGLRasterizer::GetCameraOrtho()
{
	return m_camortho;
}

void RAS_OpenGLRasterizer::SetCullFace(bool enable)
{
	if (enable) {
		Enable(RAS_CULL_FACE);
	}
	else {
		Disable(RAS_CULL_FACE);
	}
}

void RAS_OpenGLRasterizer::SetLines(bool enable)
{
	if (enable) {
		glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
	}
	else {
		glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
	}
}

void RAS_OpenGLRasterizer::SetSpecularity(float specX,
                                          float specY,
                                          float specZ,
                                          float specval)
{
	GLfloat mat_specular[] = {specX, specY, specZ, specval};
	glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, mat_specular);
}

void RAS_OpenGLRasterizer::SetShinyness(float shiny)
{
	GLfloat mat_shininess[] = { shiny };
	glMaterialfv(GL_FRONT_AND_BACK, GL_SHININESS, mat_shininess);
}

void RAS_OpenGLRasterizer::SetDiffuse(float difX, float difY, float difZ, float diffuse)
{
	GLfloat mat_diffuse[] = {difX, difY, difZ, diffuse};
	glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, mat_diffuse);
}

void RAS_OpenGLRasterizer::SetEmissive(float eX, float eY, float eZ, float e)
{
	GLfloat mat_emit[] = {eX, eY, eZ, e};
	glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, mat_emit);
}

double RAS_OpenGLRasterizer::GetTime()
{
	return m_time;
}

void RAS_OpenGLRasterizer::SetPolygonOffset(float mult, float add)
{
	glPolygonOffset(mult, add);
	EnableBit mode = RAS_POLYGON_OFFSET_FILL;
	if (m_drawingmode < RAS_TEXTURED) {
		mode = RAS_POLYGON_OFFSET_LINE;
	}
	if (mult != 0.0f || add != 0.0f) {
		Enable(mode);
	}
	else {
		Disable(mode);
	}
}

void RAS_OpenGLRasterizer::EnableMotionBlur(float motionblurvalue)
{
	/* don't just set m_motionblur to 1, but check if it is 0 so
	 * we don't reset a motion blur that is already enabled */
	if (m_motionblur == 0) {
		m_motionblur = 1;
	}
	m_motionblurvalue = motionblurvalue;
}

void RAS_OpenGLRasterizer::DisableMotionBlur()
{
	m_motionblur = 0;
	m_motionblurvalue = -1.0f;
}

void RAS_OpenGLRasterizer::SetAlphaBlend(int alphablend)
{
	GPU_set_material_alpha_blend(alphablend);
}

void RAS_OpenGLRasterizer::SetFrontFace(bool ccw)
{
	if (m_camnegscale)
		ccw = !ccw;

	if (m_last_frontface == ccw) {
		return;
	}

	if (ccw) {
		glFrontFace(GL_CCW);
	}
	else {
		glFrontFace(GL_CW);
	}

	m_last_frontface = ccw;
}

void RAS_OpenGLRasterizer::SetAnisotropicFiltering(short level)
{
	GPU_set_anisotropic((float)level);
}

short RAS_OpenGLRasterizer::GetAnisotropicFiltering()
{
	return (short)GPU_get_anisotropic();
}

void RAS_OpenGLRasterizer::SetMipmapping(MipmapOption val)
{
	if (val == RAS_IRasterizer::RAS_MIPMAP_LINEAR) {
		GPU_set_linear_mipmap(1);
		GPU_set_mipmap(1);
	}
	else if (val == RAS_IRasterizer::RAS_MIPMAP_NEAREST) {
		GPU_set_linear_mipmap(0);
		GPU_set_mipmap(1);
	}
	else {
		GPU_set_linear_mipmap(0);
		GPU_set_mipmap(0);
	}
}

RAS_IRasterizer::MipmapOption RAS_OpenGLRasterizer::GetMipmapping()
{
	if (GPU_get_mipmap()) {
		if (GPU_get_linear_mipmap()) {
			return RAS_IRasterizer::RAS_MIPMAP_LINEAR;
		}
		else {
			return RAS_IRasterizer::RAS_MIPMAP_NEAREST;
		}
	}
	else {
		return RAS_IRasterizer::RAS_MIPMAP_NONE;
	}
}

void RAS_OpenGLRasterizer::InitOverrideShadersInterface()
{
	// Find uniform location for FBO shaders.

	// Draw frame buffer shader.
	{
		GPUShader *shader = GPU_shader_get_builtin_shader(GPU_SHADER_DRAW_FRAME_BUFFER);
		if (!GPU_shader_get_interface(shader)) {
			OverrideShaderDrawFrameBufferInterface *interface = (OverrideShaderDrawFrameBufferInterface *)MEM_mallocN(sizeof(OverrideShaderDrawFrameBufferInterface), "OverrideShaderDrawFrameBufferInterface");

			interface->colorTexLoc = GPU_shader_get_uniform(shader, "colortex");

			GPU_shader_set_interface(shader, interface);
		}
	}

	// Stipple stereo shader.
	{
		GPUShader *shader = GPU_shader_get_builtin_shader(GPU_SHADER_STEREO_STIPPLE);
		if (!GPU_shader_get_interface(shader)) {
			OverrideShaderStereoStippleInterface *interface = (OverrideShaderStereoStippleInterface *)MEM_mallocN(sizeof(OverrideShaderStereoStippleInterface), "OverrideShaderStereoStippleInterface");

			interface->leftEyeTexLoc = GPU_shader_get_uniform(shader, "lefteyetex");
			interface->rightEyeTexLoc = GPU_shader_get_uniform(shader, "righteyetex");
			interface->stippleIdLoc = GPU_shader_get_uniform(shader, "stippleid");

			GPU_shader_set_interface(shader, interface);
		}
	}

	// Anaglyph stereo shader.
	{
		GPUShader *shader = GPU_shader_get_builtin_shader(GPU_SHADER_STEREO_ANAGLYPH);
		if (!GPU_shader_get_interface(shader)) {
			OverrideShaderStereoAnaglyph *interface = (OverrideShaderStereoAnaglyph *)MEM_mallocN(sizeof(OverrideShaderStereoAnaglyph), "OverrideShaderStereoAnaglyph");

			interface->leftEyeTexLoc = GPU_shader_get_uniform(shader, "lefteyetex");
			interface->rightEyeTexLoc = GPU_shader_get_uniform(shader, "righteyetex");

			GPU_shader_set_interface(shader, interface);
		}
	}
}

GPUShader *RAS_OpenGLRasterizer::GetOverrideGPUShader(OverrideShaderType type)
{
	GPUShader *shader = NULL;
	switch (type) {
		case RAS_OVERRIDE_SHADER_NONE:
		case RAS_OVERRIDE_SHADER_BASIC:
		{
			break;
		}
		case RAS_OVERRIDE_SHADER_BASIC_INSTANCING:
		{
			shader = GPU_shader_get_builtin_shader(GPU_SHADER_INSTANCING);
			break;
		}
		case RAS_OVERRIDE_SHADER_SHADOW_VARIANCE:
		{
			shader = GPU_shader_get_builtin_shader(GPU_SHADER_VSM_STORE);
			break;
		}
		case RAS_OVERRIDE_SHADER_SHADOW_VARIANCE_INSTANCING:
		{
			shader = GPU_shader_get_builtin_shader(GPU_SHADER_VSM_STORE_INSTANCING);
			break;
		}
	}

	return shader;
}

void RAS_OpenGLRasterizer::SetOverrideShader(RAS_OpenGLRasterizer::OverrideShaderType type)
{
	if (type == m_overrideShader) {
		return;
	}

	GPUShader *shader = GetOverrideGPUShader(type);
	if (shader) {
		GPU_shader_bind(shader);
	}
	else {
		GPU_shader_unbind();
	}
	m_overrideShader = type;
}

RAS_IRasterizer::OverrideShaderType RAS_OpenGLRasterizer::GetOverrideShader()
{
	return m_overrideShader;
}

void RAS_OpenGLRasterizer::ActivateOverrideShaderInstancing(void *matrixoffset, void *positionoffset, unsigned int stride)
{
	GPUShader *shader = GetOverrideGPUShader(m_overrideShader);
	if (shader) {
		GPU_shader_bind_instancing_attrib(shader, matrixoffset, positionoffset, stride);
	}
}

void RAS_OpenGLRasterizer::DesactivateOverrideShaderInstancing()
{
	GPUShader *shader = GetOverrideGPUShader(m_overrideShader);
	if (shader) {
		GPU_shader_unbind_instancing_attrib(shader);
	}
}

/**
 * Render Tools
 */

/* ProcessLighting performs lighting on objects. the layer is a bitfield that
 * contains layer information. There are 20 'official' layers in blender. A
 * light is applied on an object only when they are in the same layer. OpenGL
 * has a maximum of 8 lights (simultaneous), so 20 * 8 lights are possible in
 * a scene. */

void RAS_OpenGLRasterizer::ProcessLighting(bool uselights, const MT_Transform& viewmat)
{
	bool enable = false;
	int layer = -1;

	/* find the layer */
	if (uselights) {
		if (m_clientobject) {
			layer = KX_GameObject::GetClientObject((KX_ClientObjectInfo *)m_clientobject)->GetLayer();
		}
	}

	/* avoid state switching */
	if (m_lastlightlayer == layer && m_lastauxinfo == m_auxilaryClientInfo) {
		return;
	}

	m_lastlightlayer = layer;
	m_lastauxinfo = m_auxilaryClientInfo;

	/* enable/disable lights as needed */
	if (layer >= 0) {
		//enable = ApplyLights(layer, viewmat);
		// taken from blender source, incompatibility between Blender Object / GameObject
		KX_Scene *kxscene = (KX_Scene *)m_auxilaryClientInfo;
		float glviewmat[16];
		unsigned int count;
		std::vector<RAS_OpenGLLight *>::iterator lit = m_lights.begin();

		for (count = 0; count < m_numgllights; count++) {
			glDisable((GLenum)(GL_LIGHT0 + count));
		}

		viewmat.getValue(glviewmat);

		PushMatrix();
		LoadMatrix(glviewmat);
		for (lit = m_lights.begin(), count = 0; !(lit == m_lights.end()) && count < m_numgllights; ++lit) {
			RAS_OpenGLLight *light = (*lit);

			if (light->ApplyFixedFunctionLighting(kxscene, layer, count)) {
				count++;
			}
		}
		PopMatrix();

		enable = count > 0;
	}

	if (enable) {
		EnableOpenGLLights();
	}
	else {
		DisableOpenGLLights();
	}
}

void RAS_OpenGLRasterizer::EnableOpenGLLights()
{
	if (m_lastlighting == true) {
		return;
	}

	Enable(RAS_LIGHTING);
	Enable(RAS_COLOR_MATERIAL);

	glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
	glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);
	glLightModeli(GL_LIGHT_MODEL_LOCAL_VIEWER, (GetCameraOrtho()) ? GL_FALSE : GL_TRUE);

	if (GLEW_EXT_separate_specular_color || GLEW_VERSION_1_2)  {
		glLightModeli(GL_LIGHT_MODEL_COLOR_CONTROL, GL_SEPARATE_SPECULAR_COLOR);
	}

	m_lastlighting = true;
}

void RAS_OpenGLRasterizer::DisableOpenGLLights()
{
	if (m_lastlighting == false)
		return;

	Disable(RAS_LIGHTING);
	Disable(RAS_COLOR_MATERIAL);

	m_lastlighting = false;
}

RAS_ILightObject *RAS_OpenGLRasterizer::CreateLight()
{
	return new RAS_OpenGLLight(this);
}

void RAS_OpenGLRasterizer::AddLight(RAS_ILightObject *lightobject)
{
	RAS_OpenGLLight *gllight = dynamic_cast<RAS_OpenGLLight *>(lightobject);
	assert(gllight);
	m_lights.push_back(gllight);
}

void RAS_OpenGLRasterizer::RemoveLight(RAS_ILightObject *lightobject)
{
	RAS_OpenGLLight *gllight = dynamic_cast<RAS_OpenGLLight *>(lightobject);
	assert(gllight);

	std::vector<RAS_OpenGLLight *>::iterator lit =
	    std::find(m_lights.begin(), m_lights.end(), gllight);

	if (lit != m_lights.end()) {
		m_lights.erase(lit);
	}
}

bool RAS_OpenGLRasterizer::RayHit(struct KX_ClientObjectInfo *client, KX_RayCast *result, RayCastTranform *raytransform)
{
	if (result->m_hitMesh) {
		RAS_Polygon *poly = result->m_hitMesh->GetPolygon(result->m_hitPolygon);
		if (!poly->IsVisible()) {
			return false;
		}

		float *origmat = raytransform->origmat;
		float *mat = raytransform->mat;
		const MT_Vector3& scale = raytransform->scale;
		const MT_Vector3& point = result->m_hitPoint;
		MT_Vector3 resultnormal(result->m_hitNormal);
		MT_Vector3 left(&origmat[0]);
		MT_Vector3 dir = -(left.cross(resultnormal)).safe_normalized();
		left = (dir.cross(resultnormal)).safe_normalized();
		// for the up vector, we take the 'resultnormal' returned by the physics

		// we found the "ground", but the cast matrix doesn't take
		// scaling in consideration, so we must apply the object scale
		left *= scale[0];
		dir *= scale[1];
		resultnormal *= scale[2];

		float tmpmat[16] = {
			left[0], left[1], left[2], 0.0f,
			dir[0], dir[1], dir[2], 0.0f,
			resultnormal[0], resultnormal[1], resultnormal[2], 0.0f,
			point[0], point[1], point[2], 1.0f,
		};
		memcpy(mat, tmpmat, sizeof(float) * 16);

		return true;
	}
	else {
		return false;
	}
}

void RAS_OpenGLRasterizer::GetTransform(float *origmat, int objectdrawmode, float mat[16])
{
	if (objectdrawmode & RAS_IPolyMaterial::BILLBOARD_SCREENALIGNED ||
	    objectdrawmode & RAS_IPolyMaterial::BILLBOARD_AXISALIGNED)
	{
		// rotate the billboard/halo
		//page 360/361 3D Game Engine Design, David Eberly for a discussion
		// on screen aligned and axis aligned billboards
		// assumed is that the preprocessor transformed all billboard polygons
		// so that their normal points into the positive x direction (1.0f, 0.0f, 0.0f)
		// when new parenting for objects is done, this rotation
		// will be moved into the object

		const MT_Vector3 objpos(&origmat[12]);
		const MT_Vector3& campos = GetCameraPosition();
		MT_Vector3 left = (campos - objpos).safe_normalized();
		MT_Vector3 up = MT_Vector3(&origmat[8]).safe_normalized();

		// get scaling of halo object
		const MT_Vector3& scale = MT_Vector3(len_v3(&origmat[0]), len_v3(&origmat[4]), len_v3(&origmat[8]));

		if (objectdrawmode & RAS_IPolyMaterial::BILLBOARD_SCREENALIGNED) {
			up = (up - up.dot(left) * left).safe_normalized();
		}
		else {
			left = (left - up.dot(left) * up).safe_normalized();
		}

		MT_Vector3 dir = (up.cross(left)).normalized();

		// we have calculated the row vectors, now we keep
		// local scaling into account:

		left *= scale[0];
		dir *= scale[1];
		up *= scale[2];

		const float tmpmat[16] = {
			left[0], left[1], left[2], 0.0f,
			dir[0], dir[1], dir[2], 0.0f,
			up[0], up[1], up[2], 0.0f,
			origmat[12], origmat[13], origmat[14], 1.0f,
		};
		memcpy(mat, tmpmat, sizeof(float) * 16);
	}
	else if (objectdrawmode & RAS_IPolyMaterial::SHADOW) {
		// shadow must be cast to the ground, physics system needed here!
		const MT_Vector3 frompoint(&origmat[12]);
		KX_GameObject *gameobj = KX_GameObject::GetClientObject((KX_ClientObjectInfo *)m_clientobject);
		MT_Vector3 direction = MT_Vector3(0.0f, 0.0f, -1.0f);

		direction.normalize();
		direction *= 100000.0f;

		const MT_Vector3 topoint = frompoint + direction;

		KX_Scene *kxscene = (KX_Scene *)m_auxilaryClientInfo;
		PHY_IPhysicsEnvironment *physics_environment = kxscene->GetPhysicsEnvironment();
		PHY_IPhysicsController *physics_controller = gameobj->GetPhysicsController();

		KX_GameObject *parent = gameobj->GetParent();
		if (!physics_controller && parent) {
			physics_controller = parent->GetPhysicsController();
		}

		RayCastTranform raytransform;
		raytransform.origmat = origmat;
		raytransform.mat = mat;
		raytransform.scale = gameobj->NodeGetWorldScaling();

		KX_RayCast::Callback<RAS_OpenGLRasterizer, RayCastTranform> callback(this, physics_controller, &raytransform);
		if (!KX_RayCast::RayTest(physics_environment, frompoint, topoint, callback)) {
			// couldn't find something to cast the shadow on...
			memcpy(mat, origmat, sizeof(float) * 16);
		}
		else {
			memcpy(mat, raytransform.mat, sizeof(float) * 16);
		}
	}
	else {
		// 'normal' object
		memcpy(mat, origmat, sizeof(float) * 16);
	}
}

void RAS_OpenGLRasterizer::DisableForText()
{
	SetAlphaBlend(GPU_BLEND_ALPHA);
	SetLines(false); /* needed for texture fonts otherwise they render as wireframe */

	Enable(RAS_CULL_FACE);

	ProcessLighting(false, MT_Transform::Identity());

	for (int i = 0; i < RAS_Texture::MaxUnits; i++) {
		glActiveTextureARB(GL_TEXTURE0_ARB + i);

		if (GLEW_ARB_texture_cube_map) {
			Disable(RAS_TEXTURE_CUBE_MAP);
		}
		Disable(RAS_TEXTURE_2D);
	}

	glActiveTextureARB(GL_TEXTURE0_ARB);
}

void RAS_OpenGLRasterizer::RenderBox2D(int xco,
                                       int yco,
                                       int width,
                                       int height,
                                       float percentage)
{
	/* This is a rather important line :( The gl-mode hasn't been left
	 * behind quite as neatly as we'd have wanted to. I don't know
	 * what cause it, though :/ .*/
	Disable(RAS_DEPTH_TEST);

	SetMatrixMode(RAS_PROJECTION);
	PushMatrix();
	LoadIdentity();

	glOrtho(0, width, 0, height, -100, 100);

	SetMatrixMode(RAS_MODELVIEW);
	PushMatrix();
	LoadIdentity();

	yco = height - yco;
	int barsize = 50;

	/* draw in black first */
	glColor3ub(0, 0, 0);
	glBegin(GL_QUADS);
	glVertex2f(xco + 1 + 1 + barsize * percentage, yco - 1 + 10);
	glVertex2f(xco + 1, yco - 1 + 10);
	glVertex2f(xco + 1, yco - 1);
	glVertex2f(xco + 1 + 1 + barsize * percentage, yco - 1);
	glEnd();

	glColor3ub(255, 255, 255);
	glBegin(GL_QUADS);
	glVertex2f(xco + 1 + barsize * percentage, yco + 10);
	glVertex2f(xco, yco + 10);
	glVertex2f(xco, yco);
	glVertex2f(xco + 1 + barsize * percentage, yco);
	glEnd();

	SetMatrixMode(RAS_PROJECTION);
	PopMatrix();
	SetMatrixMode(RAS_MODELVIEW);
	PopMatrix();

	Enable(RAS_DEPTH_TEST);
}

void RAS_OpenGLRasterizer::RenderText3D(
        int fontid, const char *text, int size, int dpi,
        const float color[4], const float mat[16], float aspect)
{
	/* gl prepping */
	DisableForText();
	SetFrontFace(true);

	/* the actual drawing */
	glColor4fv(color);

	/* multiply the text matrix by the object matrix */
	BLF_enable(fontid, BLF_MATRIX | BLF_ASPECT);
	BLF_matrix(fontid, mat);

	/* aspect is the inverse scale that allows you to increase
	 * your resolution without sizing the final text size
	 * the bigger the size, the smaller the aspect */
	BLF_aspect(fontid, aspect, aspect, aspect);

	BLF_size(fontid, size, dpi);
	BLF_position(fontid, 0, 0, 0);
	BLF_draw(fontid, text, 65535);

	BLF_disable(fontid, BLF_MATRIX | BLF_ASPECT);

	SetAlphaBlend(GPU_BLEND_SOLID);
}

void RAS_OpenGLRasterizer::RenderText2D(
    RAS_TEXT_RENDER_MODE mode,
    const char *text,
    int xco, int yco,
    int width, int height)
{
	/* This is a rather important line :( The gl-mode hasn't been left
	 * behind quite as neatly as we'd have wanted to. I don't know
	 * what cause it, though :/ .*/
	DisableForText();
	SetFrontFace(true);
	Disable(RAS_DEPTH_TEST);

	SetMatrixMode(RAS_PROJECTION);
	PushMatrix();
	LoadIdentity();

	glOrtho(0, width, 0, height, -100, 100);

	SetMatrixMode(RAS_MODELVIEW);
	PushMatrix();
	LoadIdentity();

	if (mode == RAS_TEXT_PADDED) {
		/* draw in black first */
		glColor3ub(0, 0, 0);
		BLF_size(blf_mono_font, 11, 72);
		BLF_position(blf_mono_font, (float)xco + 1, (float)(height - yco - 1), 0.0f);
		BLF_draw(blf_mono_font, text, 65535); /* XXX, use real len */
	}

	/* the actual drawing */
	glColor3ub(255, 255, 255);
	BLF_size(blf_mono_font, 11, 72);
	BLF_position(blf_mono_font, (float)xco, (float)(height - yco), 0.0f);
	BLF_draw(blf_mono_font, text, 65535); /* XXX, use real len */

	SetMatrixMode(RAS_PROJECTION);
	PopMatrix();
	SetMatrixMode(RAS_MODELVIEW);
	PopMatrix();

	Enable(RAS_DEPTH_TEST);
}

void RAS_OpenGLRasterizer::PushMatrix()
{
	glPushMatrix();
}

void RAS_OpenGLRasterizer::PopMatrix()
{
	glPopMatrix();
}

void RAS_OpenGLRasterizer::SetMatrixMode(RAS_IRasterizer::MatrixMode mode)
{
	glMatrixMode(openGLMatrixModeEnums[mode]);
}

void RAS_OpenGLRasterizer::MultMatrix(const float mat[16])
{
	glMultMatrixf(mat);
}

void RAS_OpenGLRasterizer::LoadMatrix(const float mat[16])
{
	glLoadMatrixf(mat);
}

void RAS_OpenGLRasterizer::LoadIdentity()
{
	glLoadIdentity();
}

void RAS_OpenGLRasterizer::UpdateGlobalDepthTexture()
{
	unsigned short index = m_offScreens.GetCurrentIndex();
	if (m_offScreens.GetSamples(index)) {
		m_offScreens.Blit(index, RAS_IRasterizer::RAS_OFFSCREEN_BLIT_DEPTH, false, true);
		// Restore original off screen.
		m_offScreens.Bind(index);
		index = RAS_IRasterizer::RAS_OFFSCREEN_BLIT_DEPTH;
	}

	GPU_texture_set_global_depth(m_offScreens.GetDepthTexture(index));
}

void RAS_OpenGLRasterizer::MotionBlur()
{
	int state = GetMotionBlurState();
	float motionblurvalue;
	if (state) {
		motionblurvalue = GetMotionBlurValue();
		if (state == 1) {
			// bugfix:load color buffer into accum buffer for the first time(state=1)
			glAccum(GL_LOAD, 1.0f);
			SetMotionBlurState(2);
		}
		else if (motionblurvalue >= 0.0f && motionblurvalue <= 1.0f) {
			glAccum(GL_MULT, motionblurvalue);
			glAccum(GL_ACCUM, 1.0f - motionblurvalue);
			glAccum(GL_RETURN, 1.0f);
			glFlush();
		}
	}
}

void RAS_OpenGLRasterizer::SetClientObject(void *obj)
{
	m_clientobject = obj;
}

void RAS_OpenGLRasterizer::SetAuxilaryClientInfo(void *inf)
{
	m_auxilaryClientInfo = inf;
}

void RAS_OpenGLRasterizer::PrintHardwareInfo()
{
	CM_Message("GL_VENDOR: " << glGetString(GL_VENDOR));
	CM_Message("GL_RENDERER: " << glGetString(GL_RENDERER));
	CM_Message("GL_VERSION:  " << glGetString(GL_VERSION));
	bool support=0;
	CM_Message("Supported Extensions...");
	CM_Message(" GL_ARB_shader_objects supported?       "<< (GLEW_ARB_shader_objects?"yes.":"no."));

	support= GLEW_ARB_vertex_shader;
	CM_Message(" GL_ARB_vertex_shader supported?        "<< (support?"yes.":"no."));
	if (support) {
		CM_Message(" ----------Details----------");
		int max=0;
		glGetIntegerv(GL_MAX_VERTEX_UNIFORM_COMPONENTS_ARB, (GLint*)&max);
		CM_Message("  Max uniform components." << max);

		glGetIntegerv(GL_MAX_VARYING_FLOATS_ARB, (GLint*)&max);
		CM_Message("  Max varying floats." << max);

		glGetIntegerv(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS_ARB, (GLint*)&max);
		CM_Message("  Max vertex texture units." << max);

		glGetIntegerv(GL_MAX_VERTEX_ATTRIBS_ARB, (GLint*)&max);
		CM_Message("  Max vertex attribs." << max);

		glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS_ARB, (GLint*)&max);
		CM_Message("  Max combined texture units." << max);
		CM_Message("");
	}

	support=GLEW_ARB_fragment_shader;
	CM_Message(" GL_ARB_fragment_shader supported?      "<< (support?"yes.":"no."));
	if (support) {
		CM_Message(" ----------Details----------");
		int max=0;
		glGetIntegerv(GL_MAX_FRAGMENT_UNIFORM_COMPONENTS_ARB, (GLint*)&max);
		CM_Message("  Max uniform components." << max);
		CM_Message("");
	}

	support = GLEW_ARB_texture_cube_map;
	CM_Message(" GL_ARB_texture_cube_map supported?     "<< (support?"yes.":"no."));
	if (support) {
		CM_Message(" ----------Details----------");
		int size=0;
		glGetIntegerv(GL_MAX_CUBE_MAP_TEXTURE_SIZE_ARB, (GLint*)&size);
		CM_Message("  Max cubemap size." << size);
		CM_Message("");
	}

	support = GLEW_ARB_multitexture;
	CM_Message(" GL_ARB_multitexture supported?         "<< (support?"yes.":"no."));
	if (support) {
		CM_Message(" ----------Details----------");
		int units=0;
		glGetIntegerv(GL_MAX_TEXTURE_UNITS_ARB, (GLint*)&units);
		CM_Message("  Max texture units available.  " << units);
		CM_Message("");
	}

	CM_Message(" GL_ARB_texture_env_combine supported?  "<< (GLEW_ARB_texture_env_combine?"yes.":"no."));

	CM_Message(" GL_ARB_texture_non_power_of_two supported?  " << (GPU_full_non_power_of_two_support()?"yes.":"no."));

	CM_Message(" GL_ARB_draw_instanced supported?  "<< (GLEW_ARB_draw_instanced?"yes.":"no."));
}

