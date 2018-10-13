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

/** \file gameengine/Rasterizer/RAS_Rasterizer.cpp
 *  \ingroup bgerastogl
 */

#include "RAS_Rasterizer.h"
#include "RAS_OpenGLRasterizer.h"
#include "RAS_IPolygonMaterial.h"
#include "RAS_DisplayArrayBucket.h"

#include "RAS_FrameBuffer.h"
#include "RAS_ICanvas.h"
#include "RAS_Rect.h"
#include "RAS_TextUser.h"
#include "RAS_Polygon.h"
#include "RAS_ILightObject.h"

#include "RAS_OpenGLLight.h"

#include "GPU_draw.h"
#include "GPU_extensions.h"
#include "GPU_material.h"
#include "GPU_shader.h"
#include "GPU_framebuffer.h"
#include "GPU_texture.h"
#include "GPU_matrix.h"

#include "BLI_math_vector.h"
#include "BLI_rect.h"

extern "C" {
#  include "BLF_api.h"
#  include "GPU_viewport.h"
#  include "GPU_uniformbuffer.h"
#  include "DRW_engine.h"
#  include "DRW_render.h"
#  include "eevee_private.h"
#  include "DNA_view3d_types.h"
}

#include "MEM_guardedalloc.h"

// XXX Clean these up <<<
#include "KX_RayCast.h"
#include "KX_GameObject.h"
// >>>

#include "CM_Message.h"

RAS_Rasterizer::FrameBuffers::FrameBuffers()
	:m_width(0),
	m_height(0),
	m_samples(0),
	m_hdr(RAS_HDR_NONE)
{
	for (int i = 0; i < RAS_FRAMEBUFFER_MAX; i++) {
		m_frameBuffers[i] = nullptr;
	}
}

RAS_Rasterizer::FrameBuffers::~FrameBuffers()
{
	/* Free FrameBuffer Textures Attachments */
	for (int i = 0; i < RAS_FRAMEBUFFER_MAX; i++) {
		if (m_frameBuffers[i]) {
			delete m_frameBuffers[i];
		}
	}
}

inline void RAS_Rasterizer::FrameBuffers::Update(RAS_ICanvas *canvas)
{
	const unsigned int width = canvas->GetWidth() + 1;
	const unsigned int height = canvas->GetHeight() + 1;

	if (width == m_width && height == m_height) {
		// No resize detected.
		return;
	}

	m_width = width;
	m_height = height;
	m_samples = canvas->GetSamples();
	m_hdr = canvas->GetHdrType();

	// Destruct all off screens.
	for (unsigned short i = 0; i < RAS_FRAMEBUFFER_MAX; ++i) {
		m_frameBuffers[i] = nullptr;
	}
}

inline RAS_FrameBuffer *RAS_Rasterizer::FrameBuffers::GetFrameBuffer(FrameBufferType fbtype)
{
	if (!m_frameBuffers[fbtype]) {
		// The offscreen need to be created now.

		// Check if the off screen type can support samples.
		const bool sampleofs = fbtype == RAS_FRAMEBUFFER_EYE_LEFT0 ||
							   fbtype == RAS_FRAMEBUFFER_EYE_RIGHT0;

		/* Some GPUs doesn't support high multisample value with GL_RGBA16F or GL_RGBA32F.
		 * To avoid crashing we check if the off screen was created and if not decremente
		 * the multisample value and try to create the off screen to find a supported value.
		 */
		for (int samples = m_samples; samples >= 0; --samples) {

			RAS_FrameBuffer *fb = new RAS_FrameBuffer(m_width, m_height, m_hdr, fbtype);
			
			if (!fb->GetFrameBuffer()) {
				delete fb;
				continue;
			}
			m_frameBuffers[fbtype] = fb;
			m_samples = samples;
			break;
		}
	}
	return m_frameBuffers[fbtype];
}

RAS_Rasterizer::FrameBufferType RAS_Rasterizer::NextFilterFrameBuffer(FrameBufferType type)
{
	switch (type) {
		case RAS_FRAMEBUFFER_FILTER0:
		{
			return RAS_FRAMEBUFFER_FILTER1;
		}
		case RAS_FRAMEBUFFER_FILTER1:
		// Passing a non-filter frame buffer is allowed.
		default:
		{
			return RAS_FRAMEBUFFER_FILTER0;
		}
	}
}

RAS_Rasterizer::FrameBufferType RAS_Rasterizer::NextRenderFrameBuffer(FrameBufferType type)
{
	switch (type) {
		case RAS_FRAMEBUFFER_EYE_LEFT0:
		{
			return RAS_FRAMEBUFFER_EYE_LEFT1;
		}
		case RAS_FRAMEBUFFER_EYE_LEFT1:
		{
			return RAS_FRAMEBUFFER_EYE_LEFT0;
		}
		case RAS_FRAMEBUFFER_EYE_RIGHT0:
		{
			return RAS_FRAMEBUFFER_EYE_RIGHT1;
		}
		case RAS_FRAMEBUFFER_EYE_RIGHT1:
		{
			return RAS_FRAMEBUFFER_EYE_RIGHT0;
		}
		// Passing a non-eye frame buffer is disallowed.
		default:
		{
			BLI_assert(false);
			return RAS_FRAMEBUFFER_EYE_LEFT0;
		}
	}
}

RAS_Rasterizer::RAS_Rasterizer()
	:m_fogenabled(false),
	m_time(0.0f),
	m_ambient(0.0f, 0.0f, 0.0f),
	m_campos(0.0f, 0.0f, 0.0f),
	m_camortho(false),
	m_camnegscale(false),
	m_stereomode(RAS_STEREO_NOSTEREO),
	m_curreye(RAS_STEREO_LEFTEYE),
	m_eyeseparation(0.0f),
	m_focallength(0.0f),
	m_setfocallength(false),
	m_noOfScanlines(32),
	m_clientobject(nullptr),
	m_auxilaryClientInfo(nullptr),
	m_drawingmode(RAS_TEXTURED),
	m_shadowMode(RAS_SHADOW_NONE),
	m_invertFrontFace(false),
	m_last_frontface(true)
{
	m_impl.reset(new RAS_OpenGLRasterizer(this));

	m_numgllights = m_impl->GetNumLights();
}

RAS_Rasterizer::~RAS_Rasterizer()
{
}

void RAS_Rasterizer::InitScreenShaders()
{
	/*static int zero = 0;
	static int one = 1;

	{
		DRWShadingGroup *shgrp = DRW_shgroup_create(GPU_shader_get_builtin_shader(GPU_SHADER_DRAW_FRAME_BUFFER), nullptr);
		DRW_shgroup_uniform_int(shgrp, "colortex", &zero, 1);

		m_screenShaders.normal = shgrp;
	}

	{
		DRWShadingGroup *shgrp = DRW_shgroup_create(GPU_shader_get_builtin_shader(GPU_SHADER_STEREO_ANAGLYPH), nullptr);
		DRW_shgroup_uniform_int(shgrp, "lefteyetex", &zero, 1);
		DRW_shgroup_uniform_int(shgrp, "righteyetex", &one, 1);

		m_screenShaders.anaglyph = shgrp;
	}

	{
		DRWShadingGroup *shgrp = DRW_shgroup_create(GPU_shader_get_builtin_shader(GPU_SHADER_STEREO_STIPPLE), nullptr);
		DRW_shgroup_uniform_int(shgrp, "lefteyetex", &zero, 1);
		DRW_shgroup_uniform_int(shgrp, "righteyetex", &one, 1);
		DRW_shgroup_uniform_int(shgrp, "stippleid", &one, 1);

		m_screenShaders.interlace = shgrp;
	}

	{
		DRWShadingGroup *shgrp = DRW_shgroup_create(GPU_shader_get_builtin_shader(GPU_SHADER_STEREO_STIPPLE), nullptr);
		DRW_shgroup_uniform_int(shgrp, "lefteyetex", &zero, 1);
		DRW_shgroup_uniform_int(shgrp, "righteyetex", &one, 1);
		DRW_shgroup_uniform_int(shgrp, "stippleid", &zero, 1);

		m_screenShaders.vinterlace = shgrp;
	}*/
}

void RAS_Rasterizer::ExitScreenShaders()
{
	/*DRW_shgroup_free(m_screenShaders.normal);
	DRW_shgroup_free(m_screenShaders.anaglyph);
	DRW_shgroup_free(m_screenShaders.interlace);
	DRW_shgroup_free(m_screenShaders.vinterlace);*/
}

void RAS_Rasterizer::Enable(RAS_Rasterizer::EnableBit bit)
{
	m_impl->Enable(bit);
}

void RAS_Rasterizer::Disable(RAS_Rasterizer::EnableBit bit)
{
	m_impl->Disable(bit);
}

void RAS_Rasterizer::SetDepthFunc(RAS_Rasterizer::DepthFunc func)
{
	m_impl->SetDepthFunc(func);
}

void RAS_Rasterizer::SetBlendFunc(BlendFunc src, BlendFunc dst)
{
	m_impl->SetBlendFunc(src, dst);
}

void RAS_Rasterizer::SetAmbientColor(const MT_Vector3& color)
{
	m_ambient = color;
}

void RAS_Rasterizer::Init()
{
	GPU_state_init();

	Disable(RAS_BLEND);
	Disable(RAS_ALPHA_TEST);

	SetFrontFace(true);

	SetColorMask(true, true, true, true);
}

void RAS_Rasterizer::Exit()
{
	SetClearDepth(1.0f);
	SetColorMask(true, true, true, true);

	SetClearColor(0.0f, 0.0f, 0.0f, 0.0f);

	Clear(RAS_COLOR_BUFFER_BIT | RAS_DEPTH_BUFFER_BIT);

	DRW_viewport_matrix_override_unset(DRW_MAT_VIEW);
	DRW_viewport_matrix_override_unset(DRW_MAT_VIEWINV);
	DRW_viewport_matrix_override_unset(DRW_MAT_WIN);
	DRW_viewport_matrix_override_unset(DRW_MAT_WININV);
	DRW_viewport_matrix_override_unset(DRW_MAT_PERS);
	DRW_viewport_matrix_override_unset(DRW_MAT_PERSINV);

	//DRW_game_render_loop_end();
}

void RAS_Rasterizer::BeginFrame(double time)
{
	m_time = time;

	GPU_matrix_reset();

	SetFrontFace(true);

	m_impl->BeginFrame();

	// Render Tools
	m_clientobject = nullptr;
	m_lastlightlayer = -1;
	m_lastauxinfo = nullptr;
	m_lastlighting = true; /* force disable in DisableLights() */
}

void RAS_Rasterizer::EndFrame()
{
	SetColorMask(true, true, true, true);

	Disable(RAS_MULTISAMPLE);
}

void RAS_Rasterizer::SetDrawingMode(RAS_Rasterizer::DrawType drawingmode)
{
	m_drawingmode = drawingmode;
}

RAS_Rasterizer::DrawType RAS_Rasterizer::GetDrawingMode()
{
	return m_drawingmode;
}

void RAS_Rasterizer::SetShadowMode(RAS_Rasterizer::ShadowType shadowmode)
{
	m_shadowMode = shadowmode;
}

RAS_Rasterizer::ShadowType RAS_Rasterizer::GetShadowMode()
{
	return m_shadowMode;
}

void RAS_Rasterizer::SetDepthMask(DepthMask depthmask)
{
	m_impl->SetDepthMask(depthmask);
}

unsigned int *RAS_Rasterizer::MakeScreenshot(int x, int y, int width, int height)
{
	return m_impl->MakeScreenshot(x, y, width, height);
}

void RAS_Rasterizer::Clear(int clearbit)
{
	m_impl->Clear(clearbit);
}

void RAS_Rasterizer::SetClearColor(float r, float g, float b, float a)
{
	m_impl->SetClearColor(r, g, b, a);
}

void RAS_Rasterizer::SetClearDepth(float d)
{
	m_impl->SetClearDepth(d);
}

void RAS_Rasterizer::SetColorMask(bool r, bool g, bool b, bool a)
{
	m_impl->SetColorMask(r, g, b, a);
}

void RAS_Rasterizer::DrawOverlayPlane()
{
	m_impl->DrawOverlayPlane();
}

RAS_DebugDraw& RAS_Rasterizer::GetDebugDraw(SCA_IScene *scene)
{
	return m_debugDraws[scene];
}

void RAS_Rasterizer::FlushDebugDraw(SCA_IScene *scene, RAS_ICanvas *canvas)
{
	m_debugDraws[scene].Flush(this, canvas);
}

void RAS_Rasterizer::UpdateFrameBuffers(RAS_ICanvas *canvas)
{
	m_frameBuffers.Update(canvas);
}

RAS_FrameBuffer *RAS_Rasterizer::GetFrameBuffer(FrameBufferType type)
{
	return m_frameBuffers.GetFrameBuffer(type);
}

void RAS_Rasterizer::DrawFrameBuffer(RAS_FrameBuffer *srcFrameBuffer, RAS_FrameBuffer *dstFrameBuffer)
{
	GPUTexture *src = GPU_framebuffer_color_texture(srcFrameBuffer->GetFrameBuffer());
	GPU_texture_bind(src, 0);

	GPUShader *shader = GPU_shader_get_builtin_shader(GPU_SHADER_DRAW_FRAME_BUFFER);
	GPU_shader_bind(shader);

	DrawOverlayPlane();

	GPU_shader_unbind();

	GPU_texture_unbind(src);
}

void RAS_Rasterizer::DrawFrameBuffer(RAS_ICanvas *canvas, RAS_FrameBuffer *frameBuffer)
{
	const RAS_Rect& viewport = canvas->GetViewportArea();
	SetViewport(viewport.GetLeft(), viewport.GetBottom(), viewport.GetWidth() + 1, viewport.GetHeight() + 1);
	SetScissor(viewport.GetLeft(), viewport.GetBottom(), viewport.GetWidth() + 1, viewport.GetHeight() + 1);

	GPU_framebuffer_restore();
	DrawFrameBuffer(frameBuffer, nullptr);
}

void RAS_Rasterizer::DrawStereoFrameBuffer(RAS_ICanvas *canvas, RAS_FrameBuffer *leftFb, RAS_FrameBuffer *rightFb)
{
//	//if (leftFb->GetSamples() > 0) {
//	//	// Then leftFb == RAS_FrameBuffer_EYE_LEFT0.
//	//	leftFb = leftFb->Blit(GetFrameBuffer(RAS_FrameBuffer_EYE_LEFT1), true, false);
//	//}
//
//	//if (rightFb->GetSamples() > 0) {
//	//	// Then rightFb == RAS_FrameBuffer_EYE_RIGHT0.
//	//	rightFb = rightFb->Blit(GetFrameBuffer(RAS_FrameBuffer_EYE_RIGHT1), true, false);
//	//}
//
//	const RAS_Rect& viewport = canvas->GetViewportArea();
//	SetViewport(viewport.GetLeft(), viewport.GetBottom(), viewport.GetWidth() + 1, viewport.GetHeight() + 1);
//	SetScissor(viewport.GetLeft(), viewport.GetBottom(), viewport.GetWidth() + 1, viewport.GetHeight() + 1);
//
//// 	Disable(RAS_CULL_FACE);
//// 	SetDepthFunc(RAS_ALWAYS);
//
//	GPU_framebuffer_restore();
//	GPU_texture_bind(GPU_framebuffer_color_texture(leftFb->GetFrameBuffer()), 0);
//	GPU_texture_bind(GPU_framebuffer_color_texture(rightFb->GetFrameBuffer()), 1);
//
//	switch (m_stereomode) {
//		case RAS_STEREO_INTERLACED:
//		{
//			DRW_bind_shader_shgroup(m_screenShaders.interlace/*, (DRWState)(DRW_STATE_WRITE_COLOR | DRW_STATE_DEPTH_ALWAYS)*/);
//			break;
//		}
//		case RAS_STEREO_VINTERLACE:
//		{
//			DRW_bind_shader_shgroup(m_screenShaders.interlace/*, (DRWState)(DRW_STATE_WRITE_COLOR | DRW_STATE_DEPTH_ALWAYS)*/);
//			break;
//		}
//		case RAS_STEREO_ANAGLYPH:
//		{
//			DRW_bind_shader_shgroup(m_screenShaders.anaglyph/*, (DRWState)(DRW_STATE_WRITE_COLOR | DRW_STATE_DEPTH_ALWAYS)*/);
//			break;
//		}
//		default:
//		{
//			BLI_assert(false);
//		}
//	}
//	
//	DrawOverlayPlane();
//
//	GPU_texture_unbind(GPU_framebuffer_color_texture(leftFb->GetFrameBuffer()));
//	GPU_texture_unbind(GPU_framebuffer_color_texture(rightFb->GetFrameBuffer()));
//
//// 	SetDepthFunc(RAS_LEQUAL);
//// 	Enable(RAS_CULL_FACE);
}

RAS_Rect RAS_Rasterizer::GetRenderArea(RAS_ICanvas *canvas, StereoEye eye)
{
	RAS_Rect area;
	// only above/below stereo method needs viewport adjustment
	switch (m_stereomode)
	{
		case RAS_STEREO_ABOVEBELOW:
		{
			switch (eye) {
				case RAS_STEREO_LEFTEYE:
				{
					// upper half of window
					area.SetLeft(0);
					area.SetBottom(canvas->GetHeight() -
								   int(canvas->GetHeight() - m_noOfScanlines) / 2);

					area.SetRight(int(canvas->GetWidth()));
					area.SetTop(int(canvas->GetHeight()));
					break;
				}
				case RAS_STEREO_RIGHTEYE:
				{
					// lower half of window
					area.SetLeft(0);
					area.SetBottom(0);
					area.SetRight(int(canvas->GetWidth()));
					area.SetTop(int(canvas->GetHeight() - m_noOfScanlines) / 2);
					break;
				}
			}
			break;
		}
		case RAS_STEREO_3DTVTOPBOTTOM:
		{
			switch (eye) {
				case RAS_STEREO_LEFTEYE:
				{
					// upper half of window
					area.SetLeft(0);
					area.SetBottom(canvas->GetHeight() -
								   canvas->GetHeight() / 2);

					area.SetRight(canvas->GetWidth());
					area.SetTop(canvas->GetHeight());
					break;
				}
				case RAS_STEREO_RIGHTEYE:
				{
					// lower half of window
					area.SetLeft(0);
					area.SetBottom(0);
					area.SetRight(canvas->GetWidth());
					area.SetTop(canvas->GetHeight() / 2);
					break;
				}
			}
			break;
		}
		case RAS_STEREO_SIDEBYSIDE:
		{
			switch (eye)
			{
				case RAS_STEREO_LEFTEYE:
				{
					// Left half of window
					area.SetLeft(0);
					area.SetBottom(0);
					area.SetRight(canvas->GetWidth() / 2);
					area.SetTop(canvas->GetHeight());
					break;
				}
				case RAS_STEREO_RIGHTEYE:
				{
					// Right half of window
					area.SetLeft(canvas->GetWidth() / 2);
					area.SetBottom(0);
					area.SetRight(canvas->GetWidth());
					area.SetTop(canvas->GetHeight());
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
			break;
		}
	}

	return area;
}

void RAS_Rasterizer::SetStereoMode(const StereoMode stereomode)
{
	m_stereomode = stereomode;
}

RAS_Rasterizer::StereoMode RAS_Rasterizer::GetStereoMode()
{
	return m_stereomode;
}

bool RAS_Rasterizer::Stereo()
{
	if (m_stereomode > RAS_STEREO_NOSTEREO) // > 0
		return true;
	else
		return false;
}

void RAS_Rasterizer::SetEye(const StereoEye eye)
{
	m_curreye = eye;
}

RAS_Rasterizer::StereoEye RAS_Rasterizer::GetEye()
{
	return m_curreye;
}

void RAS_Rasterizer::SetEyeSeparation(const float eyeseparation)
{
	m_eyeseparation = eyeseparation;
}

float RAS_Rasterizer::GetEyeSeparation()
{
	return m_eyeseparation;
}

void RAS_Rasterizer::SetFocalLength(const float focallength)
{
	m_focallength = focallength;
	m_setfocallength = true;
}

float RAS_Rasterizer::GetFocalLength()
{
	return m_focallength;
}

const MT_Matrix4x4& RAS_Rasterizer::GetViewMatrix() const
{
	return m_matrices.view;
}

const MT_Matrix4x4& RAS_Rasterizer::GetViewInvMatrix() const
{
	return m_matrices.viewinv;
}

const MT_Matrix4x4& RAS_Rasterizer::GetProjMatrix() const
{
	return m_matrices.proj;
}

const MT_Matrix4x4& RAS_Rasterizer::GetProjInvMatrix() const
{
	return m_matrices.projinv;
}

const MT_Matrix4x4& RAS_Rasterizer::GetPersMatrix() const
{
	return m_matrices.pers;
}

const MT_Matrix4x4& RAS_Rasterizer::GetPersInvMatrix() const
{
	return m_matrices.persinv;
}

void RAS_Rasterizer::IndexPrimitivesText(RAS_MeshSlot *ms)
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

MT_Matrix4x4 RAS_Rasterizer::GetFrustumMatrix(
	StereoEye eye,
    float left,
    float right,
    float bottom,
    float top,
    float frustnear,
    float frustfar,
    float focallength,
    bool perspective)
{
	// correction for stereo
	if (Stereo()) {
		// if Rasterizer.setFocalLength is not called we use the camera focallength
		if (!m_setfocallength) {
			// if focallength is null we use a value known to be reasonable
			m_focallength = (focallength == 0.0f) ? m_eyeseparation * 30.0f
							: focallength;
		}

		const float near_div_focallength = frustnear / m_focallength;
		const float offset = 0.5f * m_eyeseparation * near_div_focallength;
		switch (eye) {
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

	float mat[4][4];
	perspective_m4(mat, left, right, bottom, top, frustnear, frustfar);

	return MT_Matrix4x4(&mat[0][0]);
}

MT_Matrix4x4 RAS_Rasterizer::GetOrthoMatrix(
    float left,
    float right,
    float bottom,
    float top,
    float frustnear,
    float frustfar)
{
	float mat[4][4];
	orthographic_m4(mat, left, right, bottom, top, frustnear, frustfar);

	return MT_Matrix4x4(&mat[0][0]);
}

// next arguments probably contain redundant info, for later...
MT_Matrix4x4 RAS_Rasterizer::GetViewMatrix(StereoEye eye, const MT_Transform &camtrans, bool perspective)
{
	// correction for stereo
	if (Stereo() && perspective) {
		static const MT_Vector3 unitViewDir(0.0f, -1.0f, 0.0f);  // minus y direction, Blender convention
		static const MT_Vector3 unitViewupVec(0.0f, 0.0f, 1.0f);

		const MT_Matrix3x3& camOrientMat3x3 = camtrans.getBasis().transposed();
		// actual viewDir
		const MT_Vector3 viewDir = camOrientMat3x3 * unitViewDir;  // this is the moto convention, vector on right hand side
		// actual viewup vec
		const MT_Vector3 viewupVec = camOrientMat3x3 * unitViewupVec;

		// vector between eyes
		const MT_Vector3 eyeline = viewDir.cross(viewupVec);

		MT_Transform trans = camtrans;
		switch (eye) {
			case RAS_STEREO_LEFTEYE:
			{
				// translate to left by half the eye distance
				MT_Transform transform = MT_Transform::Identity();
				transform.translate(-(eyeline * m_eyeseparation / 2.0f));
				trans *= transform;
				break;
			}
			case RAS_STEREO_RIGHTEYE:
			{
				// translate to right by half the eye distance
				MT_Transform transform = MT_Transform::Identity();
				transform.translate(eyeline * m_eyeseparation / 2.0f);
				trans *= transform;
				break;
			}
		}

		return trans.toMatrix();
	}

	return camtrans.toMatrix();
}

void RAS_Rasterizer::SetMatrix(const MT_Matrix4x4& viewmat, const MT_Matrix4x4& projmat, const MT_Vector3& pos, const MT_Vector3& scale)
{
	m_matrices.view = viewmat;
	m_matrices.proj = projmat;
	m_matrices.viewinv = m_matrices.view.inverse();
	m_matrices.projinv = m_matrices.proj.inverse();
	m_matrices.pers = m_matrices.proj * m_matrices.view;
	m_matrices.persinv = m_matrices.pers.inverse();

	// Don't making variable negX/negY/negZ allow drastic time saving.
	if (scale[0] < 0.0f || scale[1] < 0.0f || scale[2] < 0.0f) {
		const bool negX = (scale[0] < 0.0f);
		const bool negY = (scale[1] < 0.0f);
		const bool negZ = (scale[2] < 0.0f);
		m_matrices.view.tscale((negX) ? -1.0f : 1.0f, (negY) ? -1.0f : 1.0f, (negZ) ? -1.0f : 1.0f, 1.0f);
		m_camnegscale = negX ^ negY ^ negZ;
	}
	else {
		m_camnegscale = false;
	}

	m_campos = pos;

	float mat[4][4];
	float matinv[4][4];

	m_matrices.view.getValue(&mat[0][0]);
	m_matrices.viewinv.getValue(&matinv[0][0]);

	DRW_viewport_matrix_override_set(mat, DRW_MAT_VIEW);
	DRW_viewport_matrix_override_set(matinv, DRW_MAT_VIEWINV);

	m_matrices.proj.getValue(&mat[0][0]);
	m_matrices.projinv.getValue(&matinv[0][0]);

	DRW_viewport_matrix_override_set(mat, DRW_MAT_WIN);
	DRW_viewport_matrix_override_set(matinv, DRW_MAT_WININV);

	m_matrices.pers.getValue(&mat[0][0]);
	m_matrices.persinv.getValue(&matinv[0][0]);

	DRW_viewport_matrix_override_set(mat, DRW_MAT_PERS);
	DRW_viewport_matrix_override_set(matinv, DRW_MAT_PERSINV);

	m_camortho = (mat[3][3] != 0.0f);
}

void RAS_Rasterizer::SetViewport(int x, int y, int width, int height)
{
	m_impl->SetViewport(x, y, width, height);
}

void RAS_Rasterizer::SetScissor(int x, int y, int width, int height)
{
	m_impl->SetScissor(x, y, width, height);
}

const MT_Vector3& RAS_Rasterizer::GetCameraPosition()
{
	return m_campos;
}

bool RAS_Rasterizer::GetCameraOrtho()
{
	return m_camortho;
}

void RAS_Rasterizer::SetCullFace(bool enable)
{
	if (enable) {
		Enable(RAS_CULL_FACE);
	}
	else {
		Disable(RAS_CULL_FACE);
	}
}

void RAS_Rasterizer::EnableClipPlane(int numplanes)
{
	m_impl->EnableClipPlane(numplanes);
}

void RAS_Rasterizer::DisableClipPlane(int numplanes)
{
	m_impl->DisableClipPlane(numplanes);
}

void RAS_Rasterizer::SetLines(bool enable)
{
	m_impl->SetLines(enable);
}

double RAS_Rasterizer::GetTime()
{
	return m_time;
}

void RAS_Rasterizer::SetPolygonOffset(float mult, float add)
{
	m_impl->SetPolygonOffset(mult, add);
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

void RAS_Rasterizer::SetAlphaBlend(int alphablend)
{
}

void RAS_Rasterizer::SetFrontFace(bool ccw)
{
	// Invert the front face if the camera has a negative scale or if we force to inverse the front face.
	ccw ^= (m_camnegscale || m_invertFrontFace);

	if (m_last_frontface == ccw) {
		return;
	}

	m_impl->SetFrontFace(ccw);

	m_last_frontface = ccw;
}

void RAS_Rasterizer::SetInvertFrontFace(bool invert)
{
	m_invertFrontFace = invert;
}

#include "BKE_global.h"
void RAS_Rasterizer::SetAnisotropicFiltering(short level)
{
	Main *bmain = G.main;
	GPU_set_anisotropic(bmain, (float)level);
}

short RAS_Rasterizer::GetAnisotropicFiltering()
{
	return (short)GPU_get_anisotropic();
}

void RAS_Rasterizer::SetMipmapping(MipmapOption val)
{
	Main *bmain = G.main;
	if (val == RAS_Rasterizer::RAS_MIPMAP_LINEAR) {
		GPU_set_linear_mipmap(1);
		GPU_set_mipmap(bmain, 1);
	}
	else if (val == RAS_Rasterizer::RAS_MIPMAP_NEAREST) {
		GPU_set_linear_mipmap(0);
		GPU_set_mipmap(bmain, 1);
	}
	else {
		GPU_set_linear_mipmap(0);
		GPU_set_mipmap(bmain, 0);
	}
}

RAS_Rasterizer::MipmapOption RAS_Rasterizer::GetMipmapping()
{
	if (GPU_get_mipmap()) {
		if (GPU_get_linear_mipmap()) {
			return RAS_Rasterizer::RAS_MIPMAP_LINEAR;
		}
		else {
			return RAS_Rasterizer::RAS_MIPMAP_NEAREST;
		}
	}
	else {
		return RAS_Rasterizer::RAS_MIPMAP_NONE;
	}
}

bool RAS_Rasterizer::RayHit(struct KX_ClientObjectInfo *client, KX_RayCast *result, RayCastTranform *raytransform)
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

bool RAS_Rasterizer::NeedRayCast(KX_ClientObjectInfo *UNUSED(info), void *UNUSED(data))
{
	return true;
}

void RAS_Rasterizer::GetTransform(float *origmat, int objectdrawmode, float mat[16])
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

		MT_Vector3 left;
		if (m_camortho) {
			left = m_matrices.view[2].to3d().safe_normalized();
		}
		else {
			const MT_Vector3 objpos(&origmat[12]);
			const MT_Vector3& campos = GetCameraPosition();
			left = (campos - objpos).safe_normalized();
		}

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
		// On success mat is written in the ray test.
		raytransform.mat = mat;
		raytransform.scale = gameobj->NodeGetWorldScaling();

		KX_RayCast::Callback<RAS_Rasterizer, RayCastTranform> callback(this, physics_controller, &raytransform);
		if (!KX_RayCast::RayTest(physics_environment, frompoint, topoint, callback)) {
			// couldn't find something to cast the shadow on...
			memcpy(mat, origmat, sizeof(float) * 16);
		}
	}
	else {
		// 'normal' object
		memcpy(mat, origmat, sizeof(float) * 16);
	}
}

void RAS_Rasterizer::DisableForText()
{
	SetAlphaBlend(GPU_BLEND_ALPHA);
	SetLines(false); /* needed for texture fonts otherwise they render as wireframe */

	Enable(RAS_CULL_FACE);

	//DisableLights();

	m_impl->DisableForText();
}

void RAS_Rasterizer::RenderText3D(
        int fontid, const std::string& text, int size, int dpi,
        const float color[4], const float mat[16], float aspect)
{
	/* TEMP: DISABLE TEXT DRAWING in 2.8 WAITING FOR REFACTOR */
	m_impl->RenderText3D(fontid, text, size, dpi, color, mat, aspect);
}

void RAS_Rasterizer::SetClientObject(void *obj)
{
	m_clientobject = obj;
}

void RAS_Rasterizer::SetAuxilaryClientInfo(void *inf)
{
	m_auxilaryClientInfo = inf;
}

void RAS_Rasterizer::PrintHardwareInfo()
{
	m_impl->PrintHardwareInfo();
}
