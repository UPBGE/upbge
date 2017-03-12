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

#include "glew-mx.h"

#include "RAS_MeshUser.h"

#include "GPU_draw.h"
#include "GPU_extensions.h"
#include "GPU_material.h"
#include "GPU_shader.h"

extern "C" {
#  include "BLF_api.h"
#  include "BKE_DerivedMesh.h"
}

#include "MEM_guardedalloc.h"

#include "CM_Message.h"

// WARNING: Always respect the order from RAS_IRasterizer::EnableBit.
static const int openGLEnableBitEnums[] = {
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

// WARNING: Always respect the order from RAS_IRasterizer::MatrixMode.
static const int openGLMatrixModeEnums[] = {
	GL_PROJECTION, // RAS_PROJECTION
	GL_MODELVIEW, // RAS_MODELVIEW
	GL_TEXTURE // RAS_TEXTURE
};

// WARNING: Always respect the order from RAS_IRasterizer::BlendFunc.
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
	glTexCoordPointer(2, GL_FLOAT, sizeof(float) * 5, ((char *)nullptr) + sizeof(float) * 3);

	// Draw in traignel fan mode to reduce IBO size.
	glDrawElements(GL_TRIANGLE_FAN, 4, GL_UNSIGNED_BYTE, 0);

	// Disable vertex/uv pointer.
	glDisableClientState(GL_VERTEX_ARRAY);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);

	// Unbind screen plane VBO/IBO.
	glBindBufferARB(GL_ARRAY_BUFFER_ARB, 0);
	glBindBufferARB(GL_ELEMENT_ARRAY_BUFFER_ARB, 0);
}

RAS_OpenGLRasterizer::RAS_OpenGLRasterizer(RAS_IRasterizer *rasterizer)
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

void RAS_OpenGLRasterizer::Enable(RAS_IRasterizer::EnableBit bit)
{
	glEnable(openGLEnableBitEnums[bit]);
}

void RAS_OpenGLRasterizer::Disable(RAS_IRasterizer::EnableBit bit)
{
	glDisable(openGLEnableBitEnums[bit]);
}

void RAS_OpenGLRasterizer::EnableLight(unsigned short count)
{
	glDisable((GLenum)(GL_LIGHT0 + count));
}

void RAS_OpenGLRasterizer::DisableLight(unsigned short count)
{
	glDisable((GLenum)(GL_LIGHT0 + count));
}

void RAS_OpenGLRasterizer::SetDepthFunc(RAS_IRasterizer::DepthFunc func)
{
	glDepthFunc(openGLDepthFuncEnums[func]);
}

void RAS_OpenGLRasterizer::SetBlendFunc(RAS_IRasterizer::BlendFunc src, RAS_IRasterizer::BlendFunc dst)
{
	glBlendFunc(openGLBlendFuncEnums[src], openGLBlendFuncEnums[dst]);
}

void RAS_OpenGLRasterizer::Init()
{
	glShadeModel(GL_SMOOTH);
}

void RAS_OpenGLRasterizer::SetAmbient(const MT_Vector3& amb, float factor)
{
	float ambient[] = {amb.x() * factor, amb.y() * factor, amb.z() * factor, 1.0f};
	glLightModelfv(GL_LIGHT_MODEL_AMBIENT, ambient);
}

void RAS_OpenGLRasterizer::SetFog(short type, float start, float dist, float intensity, const MT_Vector3& color)
{
	float params[4] = {color[0], color[1], color[2], 1.0f};
	glFogi(GL_FOG_MODE, GL_LINEAR);
	glFogf(GL_FOG_DENSITY, intensity / 10.0f);
	glFogf(GL_FOG_START, start);
	glFogf(GL_FOG_END, start + dist);
	glFogfv(GL_FOG_COLOR, params);
}

void RAS_OpenGLRasterizer::Exit()
{
	if (GLEW_EXT_separate_specular_color || GLEW_VERSION_1_2)
		glLightModeli(GL_LIGHT_MODEL_COLOR_CONTROL, GL_SINGLE_COLOR);
}

void RAS_OpenGLRasterizer::BeginFrame()
{
	glShadeModel(GL_SMOOTH);
}

void RAS_OpenGLRasterizer::SetDepthMask(RAS_IRasterizer::DepthMask depthmask)
{
	glDepthMask(depthmask == RAS_IRasterizer::RAS_DEPTHMASK_DISABLED ? GL_FALSE : GL_TRUE);
}

unsigned int *RAS_OpenGLRasterizer::MakeScreenshot(int x, int y, int width, int height)
{
	unsigned int *pixeldata = nullptr;

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

	if (clearbit & RAS_IRasterizer::RAS_COLOR_BUFFER_BIT) {
		glclearbit |= GL_COLOR_BUFFER_BIT;
	}
	if (clearbit & RAS_IRasterizer::RAS_DEPTH_BUFFER_BIT) {
		glclearbit |= GL_DEPTH_BUFFER_BIT;
	}
	if (clearbit & RAS_IRasterizer::RAS_STENCIL_BUFFER_BIT) {
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

void RAS_OpenGLRasterizer::FlushDebugShapes(const RAS_IRasterizer::SceneDebugShape& debugShapes)
{
	// DrawDebugLines
	GLboolean light, tex, blend;

	light = glIsEnabled(GL_LIGHTING);
	tex = glIsEnabled(GL_TEXTURE_2D);
	blend = glIsEnabled(GL_BLEND);

	if (light) {
		Disable(RAS_IRasterizer::RAS_LIGHTING);
	}
	if (tex) {
		Disable(RAS_IRasterizer::RAS_TEXTURE_2D);
	}
	if (!blend) {
		Enable(RAS_IRasterizer::RAS_BLEND);
	}

	// draw lines
	glBegin(GL_LINES);
	for (const RAS_IRasterizer::DebugLine& line : debugShapes.m_lines) {
		glColor4fv(line.m_color.getValue());
		glVertex3fv(line.m_from.getValue());
		glVertex3fv(line.m_to.getValue());
	}
	glEnd();

	glEnableClientState(GL_VERTEX_ARRAY);
	// Draw aabbs
	for (const RAS_IRasterizer::DebugAabb& aabb : debugShapes.m_aabbs) {
		glColor4fv(aabb.m_color.getValue());

		const MT_Matrix3x3& rot = aabb.m_rot;
		const MT_Vector3& pos = aabb.m_pos;
		float mat[16] = {
			rot[0][0], rot[1][0], rot[2][0], 0.0,
			rot[0][1], rot[1][1], rot[2][1], 0.0,
			rot[0][2], rot[1][2], rot[2][2], 0.0,
			pos[0], pos[1], pos[2], 1.0
		};
		PushMatrix();
		MultMatrix(mat);

		const MT_Vector3& min = aabb.m_min;
		const MT_Vector3& max = aabb.m_max;

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

	// Draw boxes.
	static const GLubyte wireIndices[24] = {0, 1, 1, 2, 2, 3, 3, 0, 0, 4, 4, 5, 5, 6, 6, 7, 7, 4, 1, 5, 2, 6, 3, 7};
	for (const RAS_IRasterizer::DebugBox& box : debugShapes.m_boxes) {
		glVertexPointer(3, GL_FLOAT, sizeof(MT_Vector3), box.m_vertexes->getValue());
		glColor4fv(box.m_color.getValue());
		glDrawRangeElements(GL_LINES, 0, 7, 24, GL_UNSIGNED_BYTE, wireIndices);
	}

	static const GLubyte solidIndices[24] = {0, 1, 2, 3, 7, 6, 5, 4, 4, 5, 1, 0, 3, 2, 6, 7, 3, 7, 4, 0, 1, 5, 6, 2};
	for (const RAS_IRasterizer::DebugSolidBox& box : debugShapes.m_solidBoxes) {
		glVertexPointer(3, GL_FLOAT, sizeof(MT_Vector3), box.m_vertexes->getValue());
		glColor4fv(box.m_color.getValue());
		glDrawRangeElements(GL_LINES, 0, 7, 24, GL_UNSIGNED_BYTE, wireIndices);

		SetFrontFace(false);
		glColor4fv(box.m_insideColor.getValue());
		glDrawRangeElements(GL_QUADS, 0, 7, 24, GL_UNSIGNED_BYTE, solidIndices);

		SetFrontFace(true);
		glColor4fv(box.m_outsideColor.getValue());
		glDrawRangeElements(GL_QUADS, 0, 7, 24, GL_UNSIGNED_BYTE, solidIndices);
	}
	glDisableClientState(GL_VERTEX_ARRAY);

	// draw circles
	for (const RAS_IRasterizer::DebugCircle& circle : debugShapes.m_circles) {
		glBegin(GL_LINE_LOOP);
		glColor4fv(circle.m_color.getValue());

		static const MT_Vector3 worldUp(0.0f, 0.0f, 1.0f);
		const MT_Vector3& norm = circle.m_normal;
		MT_Matrix3x3 tr;
		if (norm.fuzzyZero() || norm == worldUp) {
			tr.setIdentity();
		}
		else {
			const MT_Vector3 xaxis = MT_cross(norm, worldUp);
			const MT_Vector3 yaxis = MT_cross(xaxis, norm);
			tr.setValue(xaxis.x(), xaxis.y(), xaxis.z(),
						yaxis.x(), yaxis.y(), yaxis.z(),
						norm.x(), norm.y(), norm.z());
		}
		const MT_Scalar rad = circle.m_radius;
		const int n = circle.m_sector;
		for (int j = 0; j < n; ++j) {
			const MT_Scalar theta = j * MT_2_PI / n;
			MT_Vector3 pos(cosf(theta) * rad, sinf(theta) * rad, 0.0f);
			pos = pos * tr;
			pos += circle.m_center;
			glVertex3fv(pos.getValue());
		}
		glEnd();
	}

	if (light) {
		Enable(RAS_IRasterizer::RAS_LIGHTING);
	}
	if (tex) {
		Enable(RAS_IRasterizer::RAS_TEXTURE_2D);
	}
	if (!blend) {
		Disable(RAS_IRasterizer::RAS_BLEND);
	}
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
		(mtexpoly == nullptr || mtexpoly->tpage == current_image)) {
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

void RAS_OpenGLRasterizer::DrawDerivedMesh(RAS_MeshSlot *ms, RAS_IRasterizer::DrawType drawingmode)
{
	// mesh data is in derived mesh
	current_bucket = ms->m_bucket;
	current_polymat = current_bucket->GetPolyMaterial();
	current_ms = ms;
	current_mesh = ms->m_mesh;
	current_wireframe = drawingmode <= RAS_IRasterizer::RAS_WIREFRAME;
	// MCol *mcol = (MCol*)ms->m_pDerivedMesh->getFaceDataArray(ms->m_pDerivedMesh, CD_MCOL); /* UNUSED */

	// handle two-side
	if (current_polymat->GetDrawingMode() & RAS_IRasterizer::RAS_BACKCULL)
		m_rasterizer->SetCullFace(true);
	else
		m_rasterizer->SetCullFace(false);

	if (current_bucket->IsWire()) {
		SetLines(true);
	}

	bool wireframe = (drawingmode == RAS_IRasterizer::RAS_WIREFRAME);
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
		ms->m_pDerivedMesh->drawFacesTex(ms->m_pDerivedMesh, CheckTexDM, nullptr, nullptr, DM_DRAW_USE_ACTIVE_UV);
	}

	if (current_bucket->IsWire()) {
		SetLines(false);
	}
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

void RAS_OpenGLRasterizer::SetPolygonOffset(float mult, float add)
{
	glPolygonOffset(mult, add);
}

void RAS_OpenGLRasterizer::EnableClipPlane(unsigned short index, const MT_Vector4& plane)
{
	double planev[4] = {plane.x(), plane.y(), plane.z(), plane.w()};
	glClipPlane(GL_CLIP_PLANE0 + index, planev);
	glEnable(GL_CLIP_PLANE0 + index);
}

void RAS_OpenGLRasterizer::DisableClipPlane(unsigned short index)
{
	glDisable(GL_CLIP_PLANE0 + index);
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

void RAS_OpenGLRasterizer::EnableLights()
{
	glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
	glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);
	glLightModeli(GL_LIGHT_MODEL_LOCAL_VIEWER, (m_rasterizer->GetCameraOrtho()) ? GL_FALSE : GL_TRUE);
}

void RAS_OpenGLRasterizer::DisableForText()
{
	for (int i = 0; i < RAS_Texture::MaxUnits; i++) {
		glActiveTextureARB(GL_TEXTURE0_ARB + i);

		if (GLEW_ARB_texture_cube_map) {
			Disable(RAS_IRasterizer::RAS_TEXTURE_CUBE_MAP);
		}
		Disable(RAS_IRasterizer::RAS_TEXTURE_2D);
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
	Disable(RAS_IRasterizer::RAS_DEPTH_TEST);

	SetMatrixMode(RAS_IRasterizer::RAS_PROJECTION);
	PushMatrix();
	LoadIdentity();

	glOrtho(0, width, 0, height, -100, 100);

	SetMatrixMode(RAS_IRasterizer::RAS_MODELVIEW);
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

	SetMatrixMode(RAS_IRasterizer::RAS_PROJECTION);
	PopMatrix();
	SetMatrixMode(RAS_IRasterizer::RAS_MODELVIEW);
	PopMatrix();

	Enable(RAS_IRasterizer::RAS_DEPTH_TEST);
}

void RAS_OpenGLRasterizer::RenderText3D(
        int fontid, const std::string& text, int size, int dpi,
        const float color[4], const float mat[16], float aspect)
{
	/* gl prepping */
	m_rasterizer->DisableForText();
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
	BLF_draw(fontid, text.c_str(), text.size());

	BLF_disable(fontid, BLF_MATRIX | BLF_ASPECT);

	m_rasterizer->SetAlphaBlend(GPU_BLEND_SOLID);
}

void RAS_OpenGLRasterizer::RenderText2D(
    RAS_IRasterizer::RAS_TEXT_RENDER_MODE mode,
    const std::string& text,
    int xco, int yco,
    int width, int height)
{
	/* This is a rather important line :( The gl-mode hasn't been left
	 * behind quite as neatly as we'd have wanted to. I don't know
	 * what cause it, though :/ .*/
	m_rasterizer->DisableForText();
	SetFrontFace(true);
	Disable(RAS_IRasterizer::RAS_DEPTH_TEST);

	SetMatrixMode(RAS_IRasterizer::RAS_PROJECTION);
	PushMatrix();
	LoadIdentity();

	glOrtho(0, width, 0, height, -100, 100);

	SetMatrixMode(RAS_IRasterizer::RAS_MODELVIEW);
	PushMatrix();
	LoadIdentity();

	if (mode == RAS_IRasterizer::RAS_TEXT_PADDED) {
		/* draw in black first */
		glColor3ub(0, 0, 0);
		BLF_size(blf_mono_font, 11, 72);
		BLF_position(blf_mono_font, (float)xco + 1, (float)(height - yco - 1), 0.0f);
		BLF_draw(blf_mono_font, text.c_str(), text.size());
	}

	/* the actual drawing */
	glColor3ub(255, 255, 255);
	BLF_size(blf_mono_font, 11, 72);
	BLF_position(blf_mono_font, (float)xco, (float)(height - yco), 0.0f);
	BLF_draw(blf_mono_font, text.c_str(), text.size());

	SetMatrixMode(RAS_IRasterizer::RAS_PROJECTION);
	PopMatrix();
	SetMatrixMode(RAS_IRasterizer::RAS_MODELVIEW);
	PopMatrix();

	Enable(RAS_IRasterizer::RAS_DEPTH_TEST);
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

void RAS_OpenGLRasterizer::MotionBlur(unsigned short state, float value)
{
	if (state) {
		if (state == 1) {
			// bugfix:load color buffer into accum buffer for the first time(state=1)
			glAccum(GL_LOAD, 1.0f);
			m_rasterizer->SetMotionBlur(2);
		}
		else if (value >= 0.0f && value <= 1.0f) {
			glAccum(GL_MULT, value);
			glAccum(GL_ACCUM, 1.0f - value);
			glAccum(GL_RETURN, 1.0f);
			glFlush();
		}
	}
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
