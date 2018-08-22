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
#include "RAS_OpenGLDebugDraw.h"
#include "RAS_IMaterial.h"
#include "RAS_DisplayArrayBucket.h"
#include "RAS_InstancingBuffer.h"

#include "RAS_ICanvas.h"
#include "RAS_OffScreen.h"
#include "RAS_Rect.h"
#include "RAS_TextUser.h"
#include "RAS_ILightObject.h"

#include "RAS_OpenGLLight.h"
#include "RAS_OpenGLSync.h"

#include "GPU_draw.h"
#include "GPU_extensions.h"
#include "GPU_material.h"
#include "GPU_shader.h"
#include "GPU_framebuffer.h"
#include "GPU_texture.h"

#include "BLI_math_vector.h"

extern "C" {
#  include "BKE_global.h"
#  include "BLF_api.h"
}

#include "MEM_guardedalloc.h"

// XXX Clean these up <<<
#include "KX_RayCast.h"
#include "KX_GameObject.h"
// >>>

#include "CM_Message.h"
#include "CM_List.h"

RAS_Rasterizer::OffScreens::OffScreens()
	:m_width(0),
	m_height(0),
	m_samples(0),
	m_hdr(RAS_HDR_NONE)
{
}

RAS_Rasterizer::OffScreens::~OffScreens()
{
}

inline void RAS_Rasterizer::OffScreens::Update(RAS_ICanvas *canvas)
{
	const unsigned int width = canvas->GetWidth();
	const unsigned int height = canvas->GetHeight();

	if (width == m_width && height == m_height) {
		// No resize detected.
		return;
	}

	m_width = width;
	m_height = height;
	m_samples = canvas->GetSamples();
	m_hdr = canvas->GetHdrType();

	// Destruct all off screens.
	for (unsigned short i = 0; i < RAS_Rasterizer::RAS_OFFSCREEN_MAX; ++i) {
		m_offScreens[i].reset(nullptr);
	}
}

inline RAS_OffScreen *RAS_Rasterizer::OffScreens::GetOffScreen(OffScreenType type)
{
	if (!m_offScreens[type]) {
		// The offscreen need to be created now.

		// Check if the off screen type can support samples.
		const bool sampleofs = type == RAS_OFFSCREEN_EYE_LEFT0 ||
		                       type == RAS_OFFSCREEN_EYE_RIGHT0;

		/* Some GPUs doesn't support high multisample value with GL_RGBA16F or GL_RGBA32F.
		 * To avoid crashing we check if the off screen was created and if not decremente
		 * the multisample value and try to create the off screen to find a supported value.
		 */
		for (int samples = m_samples; samples >= 0; --samples) {
			// Get off screen mode : render buffer support for multisampled off screen.
			GPUOffScreenMode mode = GPU_OFFSCREEN_MODE_NONE;
			if (sampleofs && (samples > 0)) {
				mode = (GPUOffScreenMode)(GPU_OFFSCREEN_RENDERBUFFER_COLOR | GPU_OFFSCREEN_RENDERBUFFER_DEPTH);
			}

			// WARNING: Always respect the order from RAS_Rasterizer::HdrType.
			static const GPUHDRType hdrEnums[] = {
				GPU_HDR_NONE, // RAS_HDR_NONE
				GPU_HDR_HALF_FLOAT, // RAS_HDR_HALF_FLOAT
				GPU_HDR_FULL_FLOAT // RAS_HDR_FULL_FLOAT
			};

			RAS_OffScreen *ofs = new RAS_OffScreen(m_width, m_height, sampleofs ? samples : 0, hdrEnums[m_hdr], mode, nullptr, type);
			if (!ofs->GetValid()) {
				delete ofs;
				continue;
			}

			m_offScreens[type].reset(ofs);
			m_samples = samples;
			break;
		}

		/* Creating an off screen restore the default frame buffer object.
		 * We have to rebind the last off screen. */
		RAS_OffScreen *lastOffScreen = RAS_OffScreen::GetLastOffScreen();
		if (lastOffScreen) {
			lastOffScreen->Bind();
		}
	}

	return m_offScreens[type].get();
}

RAS_Rasterizer::OffScreenType RAS_Rasterizer::NextFilterOffScreen(RAS_Rasterizer::OffScreenType index)
{
	switch (index) {
		case RAS_OFFSCREEN_FILTER0:
		{
			return RAS_OFFSCREEN_FILTER1;
		}
		case RAS_OFFSCREEN_FILTER1:
		// Passing a non-filter frame buffer is allowed.
		default:
		{
			return RAS_OFFSCREEN_FILTER0;
		}
	}
}

RAS_Rasterizer::OffScreenType RAS_Rasterizer::NextRenderOffScreen(RAS_Rasterizer::OffScreenType index)
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
		// Passing a non-eye frame buffer is disallowed.
		default:
		{
			BLI_assert(false);
			return RAS_OFFSCREEN_EYE_LEFT0;
		}
	}
}

// Each shader used to draw the offscreen to screen by color management.
GPUBuiltinShader offScreenToScreenShaderTable[RAS_Rasterizer::RAS_SHADER_TO_SCREEN_MAX][RAS_Rasterizer::RAS_COLOR_MANAGEMENT_MAX] = {
	// Linear, sRGB
	{GPU_SHADER_DRAW_FRAME_BUFFER, GPU_SHADER_DRAW_FRAME_BUFFER_SRGB}, // Normal
	{GPU_SHADER_STEREO_STIPPLE, GPU_SHADER_STEREO_STIPPLE_SRGB}, // Stereo stipple
	{GPU_SHADER_STEREO_ANAGLYPH, GPU_SHADER_STEREO_ANAGLYPH_SRGB} // Stereo anaglyph
};

RAS_Rasterizer::RAS_Rasterizer()
	:m_time(0.0f),
	m_ambient(mt::zero3),
	m_viewmatrix(mt::mat4::Identity()),
	m_viewinvmatrix(mt::mat4::Identity()),
	m_campos(mt::zero3),
	m_camortho(false),
	m_camnegscale(false),
	m_stereomode(RAS_STEREO_NOSTEREO),
	m_curreye(RAS_STEREO_LEFTEYE),
	m_eyeseparation(0.0f),
	m_focallength(0.0f),
	m_setfocallength(false),
	m_noOfScanlines(32),
	m_colorManagement(RAS_COLOR_MANAGEMENT_LINEAR),
	m_motionblur(0),
	m_motionblurvalue(-1.0f),
	m_clientobject(nullptr),
	m_auxilaryClientInfo(nullptr),
	m_drawingmode(RAS_TEXTURED),
	m_invertFrontFace(false)
{
	m_impl.reset(new RAS_OpenGLRasterizer(this));
	m_debugDrawImpl.reset(new RAS_OpenGLDebugDraw());

	m_numgllights = m_impl->GetNumLights();

	m_state.frontFace = -1;
	m_state.cullFace = -1;
	m_state.polyOffset[0] = -1.0f;
	m_state.polyOffset[1] = -1.0f;
}

RAS_Rasterizer::~RAS_Rasterizer()
{
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

void RAS_Rasterizer::SetAmbientColor(const mt::vec3& color)
{
	m_ambient = color;
}

void RAS_Rasterizer::SetAmbient(float factor)
{
	m_impl->SetAmbient(m_ambient, factor);
}

void RAS_Rasterizer::SetFog(short type, float start, float dist, float intensity, const mt::vec3& color)
{
	m_impl->SetFog(type, start, dist, intensity, color);
}

void RAS_Rasterizer::Init()
{
	GPU_state_init();

	Disable(RAS_BLEND);
	Disable(RAS_ALPHA_TEST);
	//m_last_alphablend = GPU_BLEND_SOLID;
	GPU_set_material_alpha_blend(GPU_BLEND_SOLID);

	SetFrontFace(true);

	SetColorMask(true, true, true, true);

	m_impl->Init();

	InitOverrideShadersInterface();
}

void RAS_Rasterizer::Exit()
{
	SetCullFace(true);
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
	m_impl->Exit();

	ResetGlobalDepthTexture();

	EndFrame();
}

void RAS_Rasterizer::BeginFrame(double time)
{
	m_time = time;

	m_state.polyOffset[0] = -1.0f;
	m_state.polyOffset[1] = -1.0f;

	SetCullFace(true);
	Enable(RAS_DEPTH_TEST);

	Disable(RAS_BLEND);
	Disable(RAS_ALPHA_TEST);
	//m_last_alphablend = GPU_BLEND_SOLID;
	GPU_set_material_alpha_blend(GPU_BLEND_SOLID);

	SetFrontFace(true);

	m_impl->BeginFrame();

	Enable(RAS_MULTISAMPLE);

	Enable(RAS_SCISSOR_TEST);

	SetDepthFunc(RAS_LEQUAL);

	// Render Tools
	m_clientobject = nullptr;
	m_lastlightlayer = -1;
	m_lastauxinfo = nullptr;
	m_lastlighting = true; /* force disable in DisableLights() */

	DisableLights();
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

void RAS_Rasterizer::UpdateOffScreens(RAS_ICanvas *canvas)
{
	m_offScreens.Update(canvas);
}

RAS_OffScreen *RAS_Rasterizer::GetOffScreen(OffScreenType type)
{
	return m_offScreens.GetOffScreen(type);
}

void RAS_Rasterizer::DrawOffScreen(RAS_OffScreen *srcOffScreen, RAS_OffScreen *dstOffScreen)
{
	if (srcOffScreen->GetSamples() > 0) {
		srcOffScreen->Blit(dstOffScreen, true, true);
	}
	else {
		srcOffScreen->BindColorTexture(0);

		GPUShader *shader = GPU_shader_get_builtin_shader(GPU_SHADER_DRAW_FRAME_BUFFER);
		GPU_shader_bind(shader);

		DrawOverlayPlane();

		GPU_shader_unbind();

		srcOffScreen->UnbindColorTexture();
	}
}

void RAS_Rasterizer::DrawOffScreenToScreen(RAS_ICanvas *canvas, RAS_OffScreen *offScreen)
{
	if (offScreen->GetSamples() > 0) {
		offScreen = offScreen->Blit(GetOffScreen(RAS_OFFSCREEN_EYE_LEFT1), true, false);
	}

	const int *viewport = canvas->GetViewPort();
	SetViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
	SetScissor(viewport[0], viewport[1], viewport[2], viewport[3]);

	SetFrontFace(true);
	SetDepthFunc(RAS_ALWAYS);

	RAS_OffScreen::RestoreScreen();

	offScreen->BindColorTexture(0);

	GPUShader *shader =
		GPU_shader_get_builtin_shader(offScreenToScreenShaderTable[RAS_SHADER_TO_SCREEN_NORMAL][m_colorManagement]);
	GPU_shader_bind(shader);

	DrawOverlayPlane();

	GPU_shader_unbind();

	offScreen->UnbindColorTexture();

	SetDepthFunc(RAS_LEQUAL);
}

void RAS_Rasterizer::DrawStereoOffScreenToScreen(RAS_ICanvas *canvas, RAS_OffScreen *leftOffScreen,
		RAS_OffScreen *rightOffScreen, StereoMode stereoMode)
{
	if (leftOffScreen->GetSamples() > 0) {
		// Then leftOffScreen == RAS_OFFSCREEN_EYE_LEFT0.
		leftOffScreen = leftOffScreen->Blit(GetOffScreen(RAS_OFFSCREEN_EYE_LEFT1), true, false);
	}

	if (rightOffScreen->GetSamples() > 0) {
		// Then rightOffScreen == RAS_OFFSCREEN_EYE_RIGHT0.
		rightOffScreen = rightOffScreen->Blit(GetOffScreen(RAS_OFFSCREEN_EYE_RIGHT1), true, false);
	}

	const int *viewport = canvas->GetViewPort();
	SetViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
	SetScissor(viewport[0], viewport[1], viewport[2], viewport[3]);

	SetFrontFace(true);
	SetDepthFunc(RAS_ALWAYS);

	RAS_OffScreen::RestoreScreen();

	if (stereoMode == RAS_STEREO_VINTERLACE || stereoMode == RAS_STEREO_INTERLACED) {
		GPUShader *shader = 
			GPU_shader_get_builtin_shader(offScreenToScreenShaderTable[RAS_SHADER_TO_SCREEN_STEREO_STIPPLE][m_colorManagement]);
		GPU_shader_bind(shader);

		OverrideShaderStereoStippleInterface *interface = (OverrideShaderStereoStippleInterface *)GPU_shader_get_interface(shader);
		GPU_shader_uniform_int(shader, interface->stippleIdLoc, (stereoMode == RAS_STEREO_INTERLACED) ? 1 : 0);
	}
	else if (stereoMode == RAS_STEREO_ANAGLYPH) {
		GPUShader *shader = 
			GPU_shader_get_builtin_shader(offScreenToScreenShaderTable[RAS_SHADER_TO_SCREEN_STEREO_ANAGLYPH][m_colorManagement]);
		GPU_shader_bind(shader);
	}

	leftOffScreen->BindColorTexture(0);
	rightOffScreen->BindColorTexture(1);

	DrawOverlayPlane();

	leftOffScreen->UnbindColorTexture();
	rightOffScreen->UnbindColorTexture();

	GPU_shader_unbind();

	SetDepthFunc(RAS_LEQUAL);
}

RAS_Rect RAS_Rasterizer::GetRenderArea(RAS_ICanvas *canvas, StereoMode stereoMode, StereoEye eye)
{
	RAS_Rect area;
	// only above/below stereo method needs viewport adjustment
	switch (stereoMode) {
		case RAS_STEREO_ABOVEBELOW:
		{
			switch (eye) {
				case RAS_STEREO_LEFTEYE:
				{
					// upper half of window
					area.SetLeft(0);
					area.SetBottom(canvas->GetHeight() - (canvas->GetHeight() - m_noOfScanlines - 1) / 2);

					area.SetRight(canvas->GetMaxX());
					area.SetTop(canvas->GetMaxY());
					break;
				}
				case RAS_STEREO_RIGHTEYE:
				{
					// lower half of window
					area.SetLeft(0);
					area.SetBottom(0);
					area.SetRight(canvas->GetMaxX());
					area.SetTop((canvas->GetMaxY() - m_noOfScanlines) / 2);
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
					area.SetBottom(canvas->GetHeight() - canvas->GetHeight() / 2);

					area.SetRight(canvas->GetWidth() - 1);
					area.SetTop(canvas->GetHeight() - 1);
					break;
				}
				case RAS_STEREO_RIGHTEYE:
				{
					// lower half of window
					area.SetLeft(0);
					area.SetBottom(0);
					area.SetRight(canvas->GetWidth() - 1);
					area.SetTop((canvas->GetHeight() - 1) / 2);
					break;
				}
			}
			break;
		}
		case RAS_STEREO_SIDEBYSIDE:
		{
			switch (eye) {
				case RAS_STEREO_LEFTEYE:
				{
					// Left half of window
					area.SetLeft(0);
					area.SetBottom(0);
					area.SetRight((canvas->GetWidth() - 1) / 2);
					area.SetTop(canvas->GetHeight() - 1);
					break;
				}
				case RAS_STEREO_RIGHTEYE:
				{
					// Right half of window
					area.SetLeft(canvas->GetWidth() / 2);
					area.SetBottom(0);
					area.SetRight(canvas->GetWidth() - 1);
					area.SetTop(canvas->GetHeight() - 1);
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
			area.SetRight(canvas->GetWidth() - 1);
			area.SetTop(canvas->GetHeight() - 1);
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

RAS_ISync *RAS_Rasterizer::CreateSync(int type)
{
	RAS_ISync *sync = new RAS_OpenGLSync();

	if (!sync->Create((RAS_ISync::RAS_SYNC_TYPE)type)) {
		delete sync;
		return nullptr;
	}
	return sync;
}

const mt::mat4& RAS_Rasterizer::GetViewMatrix() const
{
	return m_viewmatrix;
}

const mt::mat4& RAS_Rasterizer::GetViewInvMatrix() const
{
	return m_viewinvmatrix;
}

void RAS_Rasterizer::IndexPrimitivesText(RAS_MeshSlot *ms)
{
	RAS_TextUser *textUser = (RAS_TextUser *)ms->m_meshUser;

	float mat[16];
	textUser->GetMatrix().Pack(mat);

	const mt::vec3& spacing = textUser->GetSpacing();
	const mt::vec3& offset = textUser->GetOffset();

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
		             textUser->GetColor().Data(), mat, textUser->GetAspect());
	}
}

void RAS_Rasterizer::SetProjectionMatrix(const mt::mat4 & mat)
{
	SetMatrixMode(RAS_PROJECTION);
	LoadMatrix((float *)mat.Data());

	m_camortho = (mat(3, 3) != 0.0f);
}

mt::mat4 RAS_Rasterizer::GetFrustumMatrix(StereoMode stereoMode, StereoEye eye, float focallength,
                                          float left, float right, float bottom, float top, float frustnear, float frustfar)
{
	// correction for stereo
	if (stereoMode > RAS_STEREO_NOSTEREO) {
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
		if (stereoMode == RAS_STEREO_3DTVTOPBOTTOM) {
			// restore the vertical frustum because the 3DTV will
			// expand the top and bottom part to the full size of the screen
			bottom *= 2.0f;
			top *= 2.0f;
		}
	}

	return GetFrustumMatrix(left, right, bottom, top, frustnear, frustfar);
}

mt::mat4 RAS_Rasterizer::GetFrustumMatrix(float left, float right, float bottom, float top, float frustnear, float frustfar)
{
	return mt::mat4::Perspective(left, right, bottom, top, frustnear, frustfar);
}

mt::mat4 RAS_Rasterizer::GetOrthoMatrix(float left,
                                        float right,
                                        float bottom,
                                        float top,
                                        float frustnear,
                                        float frustfar)
{
	return mt::mat4::Ortho(left, right, bottom, top, frustnear, frustfar);
}

// next arguments probably contain redundant info, for later...
mt::mat4 RAS_Rasterizer::GetViewMatrix(StereoMode stereoMode, StereoEye eye, const mt::mat3x4 &camtrans, bool perspective)
{
	// correction for stereo
	if ((stereoMode != RAS_STEREO_NOSTEREO) && perspective) {
		static const mt::vec3 unitViewDir = -mt::axisY3;  // minus y direction, Blender convention
		static const mt::vec3 unitViewupVec = mt::axisZ3;

		const mt::mat3& camOrientMat3x3 = camtrans.RotationMatrix().Transpose();
		// actual viewDir
		const mt::vec3 viewDir = camOrientMat3x3 * unitViewDir;  // this is the moto convention, vector on right hand side
		// actual viewup vec
		const mt::vec3 viewupVec = camOrientMat3x3 * unitViewupVec;

		// vector between eyes
		const mt::vec3 eyeline = mt::cross(viewDir, viewupVec);

		mt::mat3x4 trans = camtrans;
		switch (eye) {
			case RAS_STEREO_LEFTEYE:
			{
				// translate to left by half the eye distance
				const mt::mat3x4 transform(mt::mat3::Identity(), -eyeline *m_eyeseparation / 2.0f);
				trans *= transform;
				break;
			}
			case RAS_STEREO_RIGHTEYE:
			{
				// translate to right by half the eye distance
				const mt::mat3x4 transform(mt::mat3::Identity(), eyeline *m_eyeseparation / 2.0f);
				trans *= transform;
				break;
			}
		}

		return mt::mat4::FromAffineTransform(trans);
	}

	return mt::mat4::FromAffineTransform(camtrans);
}

void RAS_Rasterizer::SetViewMatrix(const mt::mat4& viewmat, bool negscale)
{
	m_viewmatrix = viewmat;
	m_viewinvmatrix = m_viewmatrix.Inverse();
	m_campos = m_viewinvmatrix.TranslationVector3D();
	m_camnegscale = negscale;

	SetMatrixMode(RAS_MODELVIEW);
	LoadMatrix((float *)m_viewmatrix.Data());
}

void RAS_Rasterizer::SetViewMatrix(const mt::mat4& viewmat)
{
	SetViewMatrix(viewmat, false);
}

void RAS_Rasterizer::SetViewMatrix(const mt::mat4 &viewmat, const mt::vec3& scale)
{
	mt::mat4 mat = viewmat;
	for (unsigned short i = 0; i < 3; ++i) {
		// Negate row scaling if the scale is negative.
		if (scale[i] < 0.0f) {
			for (unsigned short j = 0; j < 4; ++j) {
				mat(i, j) *= -1.0f;
			}
		}
	}

	const bool negscale = (scale.x * scale.y * scale.z) < 0.0f;
	SetViewMatrix(mat, negscale);
}

void RAS_Rasterizer::SetViewport(int x, int y, int width, int height)
{
	m_impl->SetViewport(x, y, width, height);
}

void RAS_Rasterizer::GetViewport(int *rect)
{
	m_impl->GetViewport(rect);
}

void RAS_Rasterizer::SetScissor(int x, int y, int width, int height)
{
	m_impl->SetScissor(x, y, width, height);
}

const mt::vec3& RAS_Rasterizer::GetCameraPosition()
{
	return m_campos;
}

bool RAS_Rasterizer::GetCameraOrtho()
{
	return m_camortho;
}

void RAS_Rasterizer::SetCullFace(bool enable)
{
	if (enable == m_state.cullFace) {
		return;
	}
	m_state.cullFace = enable;

	if (enable) {
		Enable(RAS_CULL_FACE);
	}
	else {
		Disable(RAS_CULL_FACE);
	}
}

void RAS_Rasterizer::EnableClipPlane(unsigned short index, const mt::vec4& plane)
{
	m_impl->EnableClipPlane(index, plane);
}

void RAS_Rasterizer::DisableClipPlane(unsigned short index)
{
	m_impl->DisableClipPlane(index);
}

void RAS_Rasterizer::SetLines(bool enable)
{
	m_impl->SetLines(enable);
}

void RAS_Rasterizer::SetSpecularity(float specX,
                                    float specY,
                                    float specZ,
                                    float specval)
{
	m_impl->SetSpecularity(specX, specY, specZ, specval);
}

void RAS_Rasterizer::SetShinyness(float shiny)
{
	m_impl->SetShinyness(shiny);
}

void RAS_Rasterizer::SetDiffuse(float difX, float difY, float difZ, float diffuse)
{
	m_impl->SetDiffuse(difX, difY, difZ, diffuse);
}

void RAS_Rasterizer::SetEmissive(float eX, float eY, float eZ, float e)
{
	m_impl->SetEmissive(eX, eY, eZ, e);
}

double RAS_Rasterizer::GetTime()
{
	return m_time;
}

void RAS_Rasterizer::SetPolygonOffset(DrawType drawingMode, float mult, float add)
{
	if (m_state.polyOffset[0] == mult && m_state.polyOffset[1] == add) {
		return;
	}

	m_impl->SetPolygonOffset(mult, add);

	EnableBit mode = RAS_POLYGON_OFFSET_FILL;
	if (drawingMode < RAS_TEXTURED) {
		mode = RAS_POLYGON_OFFSET_LINE;
	}
	if (mult != 0.0f || add != 0.0f) {
		Enable(mode);
	}
	else {
		Disable(mode);
	}

	m_state.polyOffset[0] = mult;
	m_state.polyOffset[1] = add;
}

void RAS_Rasterizer::EnableMotionBlur(float motionblurvalue)
{
	/* don't just set m_motionblur to 1, but check if it is 0 so
	 * we don't reset a motion blur that is already enabled */
	if (m_motionblur == 0) {
		m_motionblur = 1;
	}
	m_motionblurvalue = motionblurvalue;
}

void RAS_Rasterizer::DisableMotionBlur()
{
	m_motionblur = 0;
	m_motionblurvalue = -1.0f;
}

void RAS_Rasterizer::SetMotionBlur(unsigned short state)
{
	m_motionblur = state;
}

void RAS_Rasterizer::SetAlphaBlend(int alphablend)
{
	GPU_set_material_alpha_blend(alphablend);
}

void RAS_Rasterizer::SetFrontFace(bool ccw)
{
	// Invert the front face if the camera has a negative scale or if we force to inverse the front face.
	ccw ^= (m_camnegscale || m_invertFrontFace);

	if (m_state.frontFace == ccw) {
		return;
	}

	m_impl->SetFrontFace(ccw);

	m_state.frontFace = ccw;
}

void RAS_Rasterizer::SetInvertFrontFace(bool invert)
{
	m_invertFrontFace = invert;
}

void RAS_Rasterizer::SetColorManagment(ColorManagement colorManagement)
{
	m_colorManagement = colorManagement;
}

void RAS_Rasterizer::SetAnisotropicFiltering(short level)
{
	GPU_set_anisotropic(G.main, (float)level);
}

short RAS_Rasterizer::GetAnisotropicFiltering()
{
	return (short)GPU_get_anisotropic();
}

void RAS_Rasterizer::SetMipmapping(MipmapOption val)
{
	switch (val) {
		case RAS_Rasterizer::RAS_MIPMAP_LINEAR:
		{
			GPU_set_linear_mipmap(1);
			GPU_set_mipmap(G.main, 1);
			break;
		}
		case RAS_Rasterizer::RAS_MIPMAP_NEAREST:
		{
			GPU_set_linear_mipmap(0);
			GPU_set_mipmap(G.main, 1);
			break;
		}
		default:
		{
			GPU_set_linear_mipmap(0);
			GPU_set_mipmap(G.main, 0);
		}
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

void RAS_Rasterizer::InitOverrideShadersInterface()
{
	// Find uniform location for FBO shaders.

	// Draw frame buffer shader.
	for (unsigned short i = 0; i < RAS_COLOR_MANAGEMENT_MAX; ++i) {
		{
			GPUShader *shader = 
				GPU_shader_get_builtin_shader(offScreenToScreenShaderTable[RAS_SHADER_TO_SCREEN_NORMAL][i]);
			if (!GPU_shader_get_interface(shader)) {
				OverrideShaderDrawFrameBufferInterface *interface = (OverrideShaderDrawFrameBufferInterface *)MEM_mallocN(sizeof(OverrideShaderDrawFrameBufferInterface), "OverrideShaderDrawFrameBufferInterface");

				interface->colorTexLoc = GPU_shader_get_uniform(shader, "colortex");

				GPU_shader_bind(shader);
				GPU_shader_uniform_int(shader, interface->colorTexLoc, 0);
				GPU_shader_unbind();

				GPU_shader_set_interface(shader, interface);
			}
		}

		// Stipple stereo shader.
		{
			GPUShader *shader = 
				GPU_shader_get_builtin_shader(offScreenToScreenShaderTable[RAS_SHADER_TO_SCREEN_STEREO_STIPPLE][i]);
			if (!GPU_shader_get_interface(shader)) {
				OverrideShaderStereoStippleInterface *interface = (OverrideShaderStereoStippleInterface *)MEM_mallocN(sizeof(OverrideShaderStereoStippleInterface), "OverrideShaderStereoStippleInterface");

				interface->leftEyeTexLoc = GPU_shader_get_uniform(shader, "lefteyetex");
				interface->rightEyeTexLoc = GPU_shader_get_uniform(shader, "righteyetex");
				interface->stippleIdLoc = GPU_shader_get_uniform(shader, "stippleid");

				GPU_shader_bind(shader);
				GPU_shader_uniform_int(shader, interface->leftEyeTexLoc, 0);
				GPU_shader_uniform_int(shader, interface->rightEyeTexLoc, 1);
				GPU_shader_unbind();

				GPU_shader_set_interface(shader, interface);
			}
		}

		// Anaglyph stereo shader.
		{
			GPUShader *shader = 
				GPU_shader_get_builtin_shader(offScreenToScreenShaderTable[RAS_SHADER_TO_SCREEN_STEREO_ANAGLYPH][i]);
			if (!GPU_shader_get_interface(shader)) {
				OverrideShaderStereoAnaglyph *interface = (OverrideShaderStereoAnaglyph *)MEM_mallocN(sizeof(OverrideShaderStereoAnaglyph), "OverrideShaderStereoAnaglyph");

				interface->leftEyeTexLoc = GPU_shader_get_uniform(shader, "lefteyetex");
				interface->rightEyeTexLoc = GPU_shader_get_uniform(shader, "righteyetex");

				GPU_shader_bind(shader);
				GPU_shader_uniform_int(shader, interface->leftEyeTexLoc, 0);
				GPU_shader_uniform_int(shader, interface->rightEyeTexLoc, 1);
				GPU_shader_unbind();

				GPU_shader_set_interface(shader, interface);
			}
		}
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

void RAS_Rasterizer::ProcessLighting(bool uselights, const mt::mat3x4& viewmat)
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
			m_impl->DisableLight(count);
		}

		viewmat.PackFromAffineTransform(glviewmat);

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
		EnableLights();
	}
	else {
		DisableLights();
	}
}

void RAS_Rasterizer::EnableLights()
{
	if (m_lastlighting == true) {
		return;
	}

	Enable(RAS_Rasterizer::RAS_LIGHTING);
	Enable(RAS_Rasterizer::RAS_COLOR_MATERIAL);

	m_impl->EnableLights();

	m_lastlighting = true;
}

void RAS_Rasterizer::DisableLights()
{
	if (m_lastlighting == false) {
		return;
	}

	Disable(RAS_Rasterizer::RAS_LIGHTING);
	Disable(RAS_Rasterizer::RAS_COLOR_MATERIAL);

	m_lastlighting = false;
}

RAS_ILightObject *RAS_Rasterizer::CreateLight()
{
	return new RAS_OpenGLLight(this);
}

void RAS_Rasterizer::AddLight(RAS_ILightObject *lightobject)
{
	RAS_OpenGLLight *gllight = static_cast<RAS_OpenGLLight *>(lightobject);
	BLI_assert(gllight);
	m_lights.push_back(gllight);
}

void RAS_Rasterizer::RemoveLight(RAS_ILightObject *lightobject)
{
	RAS_OpenGLLight *gllight = static_cast<RAS_OpenGLLight *>(lightobject);
	BLI_assert(gllight);

	CM_ListRemoveIfFound(m_lights, gllight);
}

bool RAS_Rasterizer::RayHit(struct KX_ClientObjectInfo *client, KX_RayCast *result, RayCastTranform *raytransform)
{
	if (result->m_hitMesh) {
		const RAS_Mesh::PolygonInfo poly = result->m_hitMesh->GetPolygon(result->m_hitPolygon);
		if (!(poly.flags & RAS_Mesh::PolygonInfo::VISIBLE)) {
			return false;
		}

		const mt::mat4& origmat = raytransform->origmat;
		float *mat = raytransform->mat;
		const mt::vec3& scale = raytransform->scale;
		const mt::vec3& point = result->m_hitPoint;
		mt::vec3 resultnormal(result->m_hitNormal);
		mt::vec3 left = origmat.GetColumn(0).xyz();
		mt::vec3 dir = -(mt::cross(left, resultnormal)).SafeNormalized(mt::axisX3);
		left = (mt::cross(dir, resultnormal)).SafeNormalized(mt::axisX3);
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

void RAS_Rasterizer::GetTransform(const mt::mat4& origmat, int objectdrawmode, float mat[16])
{
	if (objectdrawmode == RAS_IMaterial::RAS_NORMAL) {
		// 'normal' object
		origmat.Pack(mat);
	}
	else if (ELEM(objectdrawmode, RAS_IMaterial::RAS_HALO, RAS_IMaterial::RAS_BILLBOARD)) {
		// rotate the billboard/halo
		//page 360/361 3D Game Engine Design, David Eberly for a discussion
		// on screen aligned and axis aligned billboards
		// assumed is that the preprocessor transformed all billboard polygons
		// so that their normal points into the positive x direction (1.0f, 0.0f, 0.0f)
		// when new parenting for objects is done, this rotation
		// will be moved into the object

		mt::vec3 left;
		if (m_camortho) {
			left = m_viewmatrix.GetColumn(2).xyz().SafeNormalized(mt::axisX3);
		}
		else {
			const mt::vec3 objpos(&origmat[12]);
			const mt::vec3& campos = GetCameraPosition();
			left = (campos - objpos).SafeNormalized(mt::axisX3);
		}

		mt::vec3 up = mt::vec3(&origmat[8]).SafeNormalized(mt::axisX3);

		// get scaling of halo object
		const mt::vec3& scale = mt::vec3(len_v3(&origmat[0]), len_v3(&origmat[4]), len_v3(&origmat[8]));

		if (objectdrawmode & RAS_IMaterial::RAS_HALO) {
			up = (up - mt::dot(up, left) * left).SafeNormalized(mt::axisX3);
		}
		else {
			left = (left - mt::dot(up, left) * up).SafeNormalized(mt::axisX3);
		}

		mt::vec3 dir = (mt::cross(up, left)).Normalized();

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
	else {
		// shadow must be cast to the ground, physics system needed here!
		const mt::vec3 frompoint(&origmat[12]);
		KX_GameObject *gameobj = KX_GameObject::GetClientObject((KX_ClientObjectInfo *)m_clientobject);
		mt::vec3 direction = -mt::axisZ3;

		direction.Normalize();
		direction *= 100000.0f;

		const mt::vec3 topoint = frompoint + direction;

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
			origmat.Pack(mat);
		}
	}
}

void RAS_Rasterizer::FlushDebug(RAS_ICanvas *canvas, RAS_DebugDraw *debugDraw)
{
	m_debugDrawImpl->Flush(this, canvas, debugDraw);
}

void RAS_Rasterizer::DisableForText()
{
	SetAlphaBlend(GPU_BLEND_ALPHA);
	SetLines(false); /* needed for texture fonts otherwise they render as wireframe */

	SetCullFace(true);

	DisableLights();

	m_impl->DisableForText();
}

void RAS_Rasterizer::RenderText3D(int fontid, const std::string& text, int size, int dpi,
                                  const float color[4], const float mat[16], float aspect)
{
	m_impl->RenderText3D(fontid, text, size, dpi, color, mat, aspect);
}

void RAS_Rasterizer::PushMatrix()
{
	m_impl->PushMatrix();
}

void RAS_Rasterizer::PopMatrix()
{
	m_impl->PopMatrix();
}

void RAS_Rasterizer::SetMatrixMode(RAS_Rasterizer::MatrixMode mode)
{
	m_impl->SetMatrixMode(mode);
}

void RAS_Rasterizer::MultMatrix(const float mat[16])
{
	m_impl->MultMatrix(mat);
}

void RAS_Rasterizer::LoadMatrix(const float mat[16])
{
	m_impl->LoadMatrix(mat);
}

void RAS_Rasterizer::LoadIdentity()
{
	m_impl->LoadIdentity();
}

void RAS_Rasterizer::UpdateGlobalDepthTexture(RAS_OffScreen *offScreen)
{
	/* In case of multisamples the depth off screen must be blit to be used in shader.
	 * But the original off screen must be kept bound after the blit. */
	if (offScreen->GetSamples()) {
		RAS_OffScreen *dstOffScreen = GetOffScreen(RAS_Rasterizer::RAS_OFFSCREEN_BLIT_DEPTH);
		offScreen->Blit(dstOffScreen, false, true);
		// Restore original off screen.
		offScreen->Bind();
		offScreen = dstOffScreen;
	}

	GPU_texture_set_global_depth(offScreen->GetDepthTexture());
}

void RAS_Rasterizer::ResetGlobalDepthTexture()
{
	GPU_texture_set_global_depth(nullptr);
}

void RAS_Rasterizer::MotionBlur()
{
	m_impl->MotionBlur(m_motionblur, m_motionblurvalue);
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
