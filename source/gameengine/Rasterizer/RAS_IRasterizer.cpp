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

/** \file gameengine/Rasterizer/RAS_IRasterizer/RAS_IRasterizer.cpp
 *  \ingroup bgerastogl
 */


#include "RAS_IRasterizer.h"
#include "RAS_OpenGLRasterizer.h"

#include "glew-mx.h"

#include "RAS_ICanvas.h"
#include "RAS_OffScreen.h"
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

#include "CM_Message.h"

RAS_IRasterizer::OffScreens::OffScreens()
	:m_width(0),
	m_height(0),
	m_samples(0),
	m_hdr(RAS_HDR_NONE)
{
}

RAS_IRasterizer::OffScreens::~OffScreens()
{
}

inline void RAS_IRasterizer::OffScreens::Update(RAS_ICanvas *canvas)
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
	for (unsigned short i = 0; i < RAS_IRasterizer::RAS_OFFSCREEN_MAX; ++i) {
		m_offScreens[i].reset(NULL);
	}
}

inline RAS_OffScreen *RAS_IRasterizer::OffScreens::GetOffScreen(OffScreenType type)
{
	if (!m_offScreens[type]) {
		// The offscreen need to be created now.

		// Check if the off screen type can support samples.
		const bool sampleofs = type == RAS_OFFSCREEN_RENDER ||
							   type == RAS_OFFSCREEN_EYE_LEFT0 ||
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

			// WARNING: Always respect the order from RAS_IRasterizer::HdrType.
			static const GPUHDRType hdrEnums[] = {
				GPU_HDR_NONE, // RAS_HDR_NONE
				GPU_HDR_HALF_FLOAT, // RAS_HDR_HALF_FLOAT
				GPU_HDR_FULL_FLOAT // RAS_HDR_FULL_FLOAT
			};

			RAS_OffScreen *ofs = new RAS_OffScreen(m_width, m_height, sampleofs ? samples : 0, hdrEnums[m_hdr], mode, NULL, type);
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

RAS_IRasterizer::OffScreenType RAS_IRasterizer::NextFilterOffScreen(RAS_IRasterizer::OffScreenType index)
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

RAS_IRasterizer::OffScreenType RAS_IRasterizer::NextEyeOffScreen(RAS_IRasterizer::OffScreenType index)
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

RAS_IRasterizer::OffScreenType RAS_IRasterizer::NextRenderOffScreen(RAS_IRasterizer::OffScreenType index)
{
	switch (index) {
		case RAS_OFFSCREEN_FINAL:
		{
			return RAS_OFFSCREEN_RENDER;
		}
		case RAS_OFFSCREEN_RENDER:
		{
			return RAS_OFFSCREEN_FINAL;
		}
		// Passing a non-render frame buffer is disallowed.
		default:
		{
			BLI_assert(false);
			return RAS_OFFSCREEN_RENDER;
		}
	}
}

RAS_IRasterizer::RAS_IRasterizer()
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

	m_impl.reset(new RAS_OpenGLRasterizer(this));
	m_storage.reset(new RAS_StorageVBO(&m_storageAttribs));

	m_numgllights = m_impl->GetNumLights();

	InitOverrideShadersInterface();
}

RAS_IRasterizer::~RAS_IRasterizer()
{
}

void RAS_IRasterizer::Enable(RAS_IRasterizer::EnableBit bit)
{
	m_impl->Enable(bit);
}

void RAS_IRasterizer::Disable(RAS_IRasterizer::EnableBit bit)
{
	m_impl->Disable(bit);
}

void RAS_IRasterizer::SetDepthFunc(RAS_IRasterizer::DepthFunc func)
{
	m_impl->SetDepthFunc(func);
}

void RAS_IRasterizer::SetBlendFunc(BlendFunc src, BlendFunc dst)
{
	m_impl->SetBlendFunc(src, dst);
}

void RAS_IRasterizer::SetAmbientColor(float color[3])
{
	m_ambient = MT_Vector3(color);
}

void RAS_IRasterizer::SetAmbient(float factor)
{
	m_impl->SetAmbient(m_ambient, factor);
}

void RAS_IRasterizer::SetFog(short type, float start, float dist, float intensity, float color[3])
{
	m_impl->SetFog(type, start, dist, intensity, color);
}

void RAS_IRasterizer::EnableFog(bool enable)
{
	m_fogenabled = enable;
}

void RAS_IRasterizer::DisplayFog()
{
	if ((m_drawingmode >= RAS_SOLID) && m_fogenabled) {
		Enable(RAS_FOG);
	}
	else {
		Disable(RAS_FOG);
	}
}

void RAS_IRasterizer::Init()
{
	GPU_state_init();

	Disable(RAS_BLEND);
	Disable(RAS_ALPHA_TEST);
	//m_last_alphablend = GPU_BLEND_SOLID;
	GPU_set_material_alpha_blend(GPU_BLEND_SOLID);

	SetFrontFace(true);

	SetColorMask(true, true, true, true);

	m_impl->Init();
}

void RAS_IRasterizer::Exit()
{
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
	m_impl->Exit();

	ResetGlobalDepthTexture();

	EndFrame();
}

void RAS_IRasterizer::BeginFrame(double time)
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

	m_impl->BeginFrame();

	Enable(RAS_MULTISAMPLE);

	Enable(RAS_SCISSOR_TEST);

	Enable(RAS_DEPTH_TEST);
	SetDepthFunc(RAS_LEQUAL);

	// Render Tools
	m_clientobject = NULL;
	m_lastlightlayer = -1;
	m_lastauxinfo = NULL;
	m_lastlighting = true; /* force disable in DisableLights() */

	DisableLights();
}

void RAS_IRasterizer::EndFrame()
{
	SetColorMask(true, true, true, true);

	Disable(RAS_MULTISAMPLE);

	Disable(RAS_FOG);
}

void RAS_IRasterizer::SetDrawingMode(RAS_IRasterizer::DrawType drawingmode)
{
	m_drawingmode = drawingmode;

	m_storage->SetDrawingMode(drawingmode);
}

RAS_IRasterizer::DrawType RAS_IRasterizer::GetDrawingMode()
{
	return m_drawingmode;
}

void RAS_IRasterizer::SetShadowMode(RAS_IRasterizer::ShadowType shadowmode)
{
	m_shadowMode = shadowmode;
}

RAS_IRasterizer::ShadowType RAS_IRasterizer::GetShadowMode()
{
	return m_shadowMode;
}

void RAS_IRasterizer::SetDepthMask(DepthMask depthmask)
{
	m_impl->SetDepthMask(depthmask);
}

unsigned int *RAS_IRasterizer::MakeScreenshot(int x, int y, int width, int height)
{
	return m_impl->MakeScreenshot(x, y, width, height);
}

void RAS_IRasterizer::Clear(int clearbit)
{
	m_impl->Clear(clearbit);
}

void RAS_IRasterizer::SetClearColor(float r, float g, float b, float a)
{
	m_impl->SetClearColor(r, g, b, a);
}

void RAS_IRasterizer::SetClearDepth(float d)
{
	m_impl->SetClearDepth(d);
}

void RAS_IRasterizer::SetColorMask(bool r, bool g, bool b, bool a)
{
	m_impl->SetColorMask(r, g, b, a);
}

void RAS_IRasterizer::DrawOverlayPlane()
{
	m_impl->DrawOverlayPlane();
}

void RAS_IRasterizer::FlushDebugShapes(SCA_IScene *scene)
{
	m_impl->FlushDebugShapes(scene);
}

void RAS_IRasterizer::DrawDebugLine(SCA_IScene *scene, const MT_Vector3 &from, const MT_Vector3 &to, const MT_Vector4 &color)
{
	m_impl->DrawDebugLine(scene, from, to, color);
}

void RAS_IRasterizer::DrawDebugCircle(SCA_IScene *scene, const MT_Vector3 &center, const MT_Scalar radius,
		const MT_Vector4 &color, const MT_Vector3 &normal, int nsector)
{
	m_impl->DrawDebugCircle(scene, center, radius, color, normal, nsector);
}

void RAS_IRasterizer::DrawDebugAabb(SCA_IScene *scene, const MT_Vector3& pos, const MT_Matrix3x3& rot,
		const MT_Vector3& min, const MT_Vector3& max, const MT_Vector4& color)
{
	m_impl->DrawDebugAabb(scene, pos, rot, min, max, color);
}

void RAS_IRasterizer::DrawDebugBox(SCA_IScene *scene, MT_Vector3 vertexes[8], const MT_Vector4& color)
{
	m_impl->DrawDebugBox(scene, vertexes, color);
}

void RAS_IRasterizer::DrawDebugSolidBox(SCA_IScene *scene, MT_Vector3 vertexes[8], const MT_Vector4& insideColor,
		const MT_Vector4& outsideColor, const MT_Vector4& lineColor)
{
	m_impl->DrawDebugSolidBox(scene, vertexes, insideColor, outsideColor, lineColor);
}

void RAS_IRasterizer::DrawDebugCameraFrustum(SCA_IScene *scene, const MT_Matrix4x4& projmat, const MT_Matrix4x4& viewmat)
{
	m_impl->DrawDebugCameraFrustum(scene, projmat, viewmat);
}

void RAS_IRasterizer::UpdateOffScreens(RAS_ICanvas *canvas)
{
	m_offScreens.Update(canvas);
}

RAS_OffScreen *RAS_IRasterizer::GetOffScreen(OffScreenType type)
{
	return m_offScreens.GetOffScreen(type);
}

void RAS_IRasterizer::DrawOffScreen(RAS_OffScreen *srcOffScreen, RAS_OffScreen *dstOffScreen)
{
	if (srcOffScreen->GetSamples() > 0) {
		srcOffScreen->Blit(dstOffScreen, true, true);
	}
	else {
		srcOffScreen->BindColorTexture(0);

		GPUShader *shader = GPU_shader_get_builtin_shader(GPU_SHADER_DRAW_FRAME_BUFFER);
		GPU_shader_bind(shader);

		OverrideShaderDrawFrameBufferInterface *interface = (OverrideShaderDrawFrameBufferInterface *)GPU_shader_get_interface(shader);
		GPU_shader_uniform_int(shader, interface->colorTexLoc, 0);

		DrawOverlayPlane();

		GPU_shader_unbind();

		srcOffScreen->UnbindColorTexture();
	}
}

void RAS_IRasterizer::DrawOffScreen(RAS_ICanvas *canvas, RAS_OffScreen *offScreen)
{
	if (offScreen->GetSamples() > 0) {
		offScreen = offScreen->Blit(GetOffScreen(RAS_OFFSCREEN_FINAL), true, false);
	}

	const int *viewport = canvas->GetViewPort();
	SetViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
	SetScissor(viewport[0], viewport[1], viewport[2], viewport[3]);

	Disable(RAS_CULL_FACE);
	SetDepthFunc(RAS_ALWAYS);

	RAS_OffScreen::RestoreScreen();
	DrawOffScreen(offScreen, NULL);

	SetDepthFunc(RAS_LEQUAL);
	Enable(RAS_CULL_FACE);
}

void RAS_IRasterizer::DrawStereoOffScreen(RAS_ICanvas *canvas, RAS_OffScreen *leftOffScreen, RAS_OffScreen *rightOffScreen)
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

	Disable(RAS_CULL_FACE);
	SetDepthFunc(RAS_ALWAYS);

	RAS_OffScreen::RestoreScreen();

	if (m_stereomode == RAS_STEREO_VINTERLACE || m_stereomode == RAS_STEREO_INTERLACED) {
		GPUShader *shader = GPU_shader_get_builtin_shader(GPU_SHADER_STEREO_STIPPLE);
		GPU_shader_bind(shader);

		OverrideShaderStereoStippleInterface *interface = (OverrideShaderStereoStippleInterface *)GPU_shader_get_interface(shader);

		leftOffScreen->BindColorTexture(0);
		rightOffScreen->BindColorTexture(1);

		GPU_shader_uniform_int(shader, interface->leftEyeTexLoc, 0);
		GPU_shader_uniform_int(shader, interface->rightEyeTexLoc, 1);
		GPU_shader_uniform_int(shader, interface->stippleIdLoc, (m_stereomode == RAS_STEREO_INTERLACED) ? 1 : 0);

		DrawOverlayPlane();

		GPU_shader_unbind();

		leftOffScreen->UnbindColorTexture();
		rightOffScreen->UnbindColorTexture();
	}
	else if (m_stereomode == RAS_STEREO_ANAGLYPH) {
		GPUShader *shader = GPU_shader_get_builtin_shader(GPU_SHADER_STEREO_ANAGLYPH);
		GPU_shader_bind(shader);

		OverrideShaderStereoAnaglyph *interface = (OverrideShaderStereoAnaglyph *)GPU_shader_get_interface(shader);

		leftOffScreen->BindColorTexture(0);
		rightOffScreen->BindColorTexture(1);

		GPU_shader_uniform_int(shader, interface->leftEyeTexLoc, 0);
		GPU_shader_uniform_int(shader, interface->rightEyeTexLoc, 1);

		DrawOverlayPlane();

		GPU_shader_unbind();

		leftOffScreen->UnbindColorTexture();
		rightOffScreen->UnbindColorTexture();
	}

	SetDepthFunc(RAS_LEQUAL);
	Enable(RAS_CULL_FACE);
}

void RAS_IRasterizer::SetRenderArea(RAS_ICanvas *canvas)
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

void RAS_IRasterizer::SetStereoMode(const StereoMode stereomode)
{
	m_stereomode = stereomode;
}

RAS_IRasterizer::StereoMode RAS_IRasterizer::GetStereoMode()
{
	return m_stereomode;
}

bool RAS_IRasterizer::Stereo()
{
	if (m_stereomode > RAS_STEREO_NOSTEREO) // > 0
		return true;
	else
		return false;
}

void RAS_IRasterizer::SetEye(const StereoEye eye)
{
	m_curreye = eye;
}

RAS_IRasterizer::StereoEye RAS_IRasterizer::GetEye()
{
	return m_curreye;
}

void RAS_IRasterizer::SetEyeSeparation(const float eyeseparation)
{
	m_eyeseparation = eyeseparation;
}

float RAS_IRasterizer::GetEyeSeparation()
{
	return m_eyeseparation;
}

void RAS_IRasterizer::SetFocalLength(const float focallength)
{
	m_focallength = focallength;
	m_setfocallength = true;
}

float RAS_IRasterizer::GetFocalLength()
{
	return m_focallength;
}

RAS_ISync *RAS_IRasterizer::CreateSync(int type)
{
	RAS_ISync *sync = new RAS_OpenGLSync();

	if (!sync->Create((RAS_ISync::RAS_SYNC_TYPE)type)) {
		delete sync;
		return NULL;
	}
	return sync;
}
void RAS_IRasterizer::SwapBuffers(RAS_ICanvas *canvas)
{
	canvas->SwapBuffers();
}

const MT_Matrix4x4& RAS_IRasterizer::GetViewMatrix() const
{
	return m_viewmatrix;
}

const MT_Matrix4x4& RAS_IRasterizer::GetViewInvMatrix() const
{
	return m_viewinvmatrix;
}

void RAS_IRasterizer::IndexPrimitivesText(RAS_MeshSlot *ms)
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

void RAS_IRasterizer::ClearTexCoords()
{
	m_storageAttribs.texcos.clear();
}

void RAS_IRasterizer::ClearAttribs()
{
	m_storageAttribs.attribs.clear();
}

void RAS_IRasterizer::ClearAttribLayers()
{
	m_storageAttribs.layers.clear();
}

void RAS_IRasterizer::SetTexCoords(const TexCoGenList& texcos)
{
	m_storageAttribs.texcos = texcos;
}

void RAS_IRasterizer::SetAttribs(const TexCoGenList& attribs)
{
	m_storageAttribs.attribs = attribs;
}

void RAS_IRasterizer::SetAttribLayers(const RAS_IRasterizer::AttribLayerList& layers)
{
	m_storageAttribs.layers = layers;
}

void RAS_IRasterizer::BindPrimitives(RAS_DisplayArrayBucket *arrayBucket)
{
	if (arrayBucket && arrayBucket->GetDisplayArray()) {
		// Set the proper uv layer for uv attributes.
		arrayBucket->SetAttribLayers(this);
		m_storage->BindPrimitives(arrayBucket);
	}
}

void RAS_IRasterizer::UnbindPrimitives(RAS_DisplayArrayBucket *arrayBucket)
{
	if (arrayBucket && arrayBucket->GetDisplayArray()) {
		m_storage->UnbindPrimitives(arrayBucket);
	}
}

void RAS_IRasterizer::IndexPrimitives(RAS_MeshSlot *ms)
{
	if (ms->m_pDerivedMesh) {
		m_impl->DrawDerivedMesh(ms, m_drawingmode);
	}
	else {
		m_storage->IndexPrimitives(ms);
	}
}

void RAS_IRasterizer::IndexPrimitivesInstancing(RAS_DisplayArrayBucket *arrayBucket)
{
	m_storage->IndexPrimitivesInstancing(arrayBucket);
}

void RAS_IRasterizer::IndexPrimitivesBatching(RAS_DisplayArrayBucket *arrayBucket, const std::vector<void *>& indices,
												   const std::vector<int>& counts)
{
	m_storage->IndexPrimitivesBatching(arrayBucket, indices, counts);
}

void RAS_IRasterizer::SetProjectionMatrix(MT_CmMatrix4x4 &mat)
{
	SetMatrixMode(RAS_PROJECTION);
	float *matrix = &mat(0, 0);
	LoadMatrix(matrix);

	m_camortho = (mat(3, 3) != 0.0f);
}

void RAS_IRasterizer::SetProjectionMatrix(const MT_Matrix4x4 & mat)
{
	SetMatrixMode(RAS_PROJECTION);
	float matrix[16];
	/* Get into argument. Looks a bit dodgy, but it's ok. */
	mat.getValue(matrix);
	LoadMatrix(matrix);

	m_camortho = (mat[3][3] != 0.0f);
}

MT_Matrix4x4 RAS_IRasterizer::GetFrustumMatrix(
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

	float mat[4][4];
	perspective_m4(mat, left, right, bottom, top, frustnear, frustfar);

	return MT_Matrix4x4(&mat[0][0]);
}

MT_Matrix4x4 RAS_IRasterizer::GetOrthoMatrix(
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
void RAS_IRasterizer::SetViewMatrix(const MT_Matrix4x4 &mat,
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
				MT_Transform transform = MT_Transform::Identity();
				transform.translate(-(eyeline * m_eyeseparation / 2.0f));
				m_viewmatrix *= MT_Matrix4x4(transform);
				break;
			}
			case RAS_STEREO_RIGHTEYE:
			{
				// translate to right by half the eye distance
				MT_Transform transform = MT_Transform::Identity();
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

void RAS_IRasterizer::SetViewport(int x, int y, int width, int height)
{
	m_impl->SetViewport(x, y, width, height);
}

void RAS_IRasterizer::GetViewport(int *rect)
{
	m_impl->GetViewport(rect);
}

void RAS_IRasterizer::SetScissor(int x, int y, int width, int height)
{
	m_impl->SetScissor(x, y, width, height);
}

const MT_Vector3& RAS_IRasterizer::GetCameraPosition()
{
	return m_campos;
}

bool RAS_IRasterizer::GetCameraOrtho()
{
	return m_camortho;
}

void RAS_IRasterizer::SetCullFace(bool enable)
{
	if (enable) {
		Enable(RAS_CULL_FACE);
	}
	else {
		Disable(RAS_CULL_FACE);
	}
}

void RAS_IRasterizer::SetLines(bool enable)
{
	m_impl->SetLines(enable);
}

void RAS_IRasterizer::SetSpecularity(float specX,
                                          float specY,
                                          float specZ,
                                          float specval)
{
	m_impl->SetSpecularity(specX, specY, specZ, specval);
}

void RAS_IRasterizer::SetShinyness(float shiny)
{
	m_impl->SetShinyness(shiny);
}

void RAS_IRasterizer::SetDiffuse(float difX, float difY, float difZ, float diffuse)
{
	m_impl->SetDiffuse(difX, difY, difZ, diffuse);
}

void RAS_IRasterizer::SetEmissive(float eX, float eY, float eZ, float e)
{
	m_impl->SetEmissive(eX, eY, eZ, e);
}

double RAS_IRasterizer::GetTime()
{
	return m_time;
}

void RAS_IRasterizer::SetPolygonOffset(float mult, float add)
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

void RAS_IRasterizer::EnableMotionBlur(float motionblurvalue)
{
	/* don't just set m_motionblur to 1, but check if it is 0 so
	 * we don't reset a motion blur that is already enabled */
	if (m_motionblur == 0) {
		m_motionblur = 1;
	}
	m_motionblurvalue = motionblurvalue;
}

void RAS_IRasterizer::DisableMotionBlur()
{
	m_motionblur = 0;
	m_motionblurvalue = -1.0f;
}

void RAS_IRasterizer::SetMotionBlur(unsigned short state)
{
	m_motionblur = state;
}

void RAS_IRasterizer::SetAlphaBlend(int alphablend)
{
	GPU_set_material_alpha_blend(alphablend);
}

void RAS_IRasterizer::SetFrontFace(bool ccw)
{
	if (m_camnegscale)
		ccw = !ccw;

	if (m_last_frontface == ccw) {
		return;
	}

	m_impl->SetFrontFace(ccw);

	m_last_frontface = ccw;
}

void RAS_IRasterizer::SetAnisotropicFiltering(short level)
{
	GPU_set_anisotropic((float)level);
}

short RAS_IRasterizer::GetAnisotropicFiltering()
{
	return (short)GPU_get_anisotropic();
}

void RAS_IRasterizer::SetMipmapping(MipmapOption val)
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

RAS_IRasterizer::MipmapOption RAS_IRasterizer::GetMipmapping()
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

void RAS_IRasterizer::InitOverrideShadersInterface()
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

GPUShader *RAS_IRasterizer::GetOverrideGPUShader(OverrideShaderType type)
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

void RAS_IRasterizer::SetOverrideShader(RAS_IRasterizer::OverrideShaderType type)
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

RAS_IRasterizer::OverrideShaderType RAS_IRasterizer::GetOverrideShader()
{
	return m_overrideShader;
}

void RAS_IRasterizer::ActivateOverrideShaderInstancing(void *matrixoffset, void *positionoffset, unsigned int stride)
{
	GPUShader *shader = GetOverrideGPUShader(m_overrideShader);
	if (shader) {
		GPU_shader_bind_instancing_attrib(shader, matrixoffset, positionoffset, stride);
	}
}

void RAS_IRasterizer::DesactivateOverrideShaderInstancing()
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

void RAS_IRasterizer::ProcessLighting(bool uselights, const MT_Transform& viewmat)
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
		EnableLights();
	}
	else {
		DisableLights();
	}
}

void RAS_IRasterizer::EnableLights()
{
	if (m_lastlighting == true) {
		return;
	}

	Enable(RAS_IRasterizer::RAS_LIGHTING);
	Enable(RAS_IRasterizer::RAS_COLOR_MATERIAL);

	m_impl->EnableLights();

	m_lastlighting = true;
}

void RAS_IRasterizer::DisableLights()
{
	if (m_lastlighting == false)
		return;

	Disable(RAS_IRasterizer::RAS_LIGHTING);
	Disable(RAS_IRasterizer::RAS_COLOR_MATERIAL);

	m_lastlighting = false;
}

RAS_ILightObject *RAS_IRasterizer::CreateLight()
{
	return new RAS_OpenGLLight(this);
}

void RAS_IRasterizer::AddLight(RAS_ILightObject *lightobject)
{
	RAS_OpenGLLight *gllight = dynamic_cast<RAS_OpenGLLight *>(lightobject);
	BLI_assert(gllight);
	m_lights.push_back(gllight);
}

void RAS_IRasterizer::RemoveLight(RAS_ILightObject *lightobject)
{
	RAS_OpenGLLight *gllight = dynamic_cast<RAS_OpenGLLight *>(lightobject);
	BLI_assert(gllight);

	std::vector<RAS_OpenGLLight *>::iterator lit =
	    std::find(m_lights.begin(), m_lights.end(), gllight);

	if (lit != m_lights.end()) {
		m_lights.erase(lit);
	}
}

bool RAS_IRasterizer::RayHit(struct KX_ClientObjectInfo *client, KX_RayCast *result, RayCastTranform *raytransform)
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

bool RAS_IRasterizer::NeedRayCast(KX_ClientObjectInfo *UNUSED(info), void *UNUSED(data))
{
	return true;
}

void RAS_IRasterizer::GetTransform(float *origmat, int objectdrawmode, float mat[16])
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
			left = m_viewmatrix[2].to3d().safe_normalized();
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
		raytransform.mat = mat;
		raytransform.scale = gameobj->NodeGetWorldScaling();

		KX_RayCast::Callback<RAS_IRasterizer, RayCastTranform> callback(this, physics_controller, &raytransform);
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

void RAS_IRasterizer::DisableForText()
{
	SetAlphaBlend(GPU_BLEND_ALPHA);
	SetLines(false); /* needed for texture fonts otherwise they render as wireframe */

	Enable(RAS_CULL_FACE);

	ProcessLighting(false, MT_Transform::Identity());

	m_impl->DisableForText();
}

void RAS_IRasterizer::RenderBox2D(int xco,
                                       int yco,
                                       int width,
                                       int height,
                                       float percentage)
{
	m_impl->RenderBox2D(xco, yco, width, height, percentage);
}

void RAS_IRasterizer::RenderText3D(
        int fontid, const std::string& text, int size, int dpi,
        const float color[4], const float mat[16], float aspect)
{
	m_impl->RenderText3D(fontid, text, size, dpi, color, mat, aspect);
}

void RAS_IRasterizer::RenderText2D(
    RAS_TEXT_RENDER_MODE mode,
    const std::string& text,
    int xco, int yco,
    int width, int height)
{
	m_impl->RenderText2D(mode, text, xco, yco, width, height);
}

void RAS_IRasterizer::PushMatrix()
{
	m_impl->PushMatrix();
}

void RAS_IRasterizer::PopMatrix()
{
	m_impl->PopMatrix();
}

void RAS_IRasterizer::SetMatrixMode(RAS_IRasterizer::MatrixMode mode)
{
	m_impl->SetMatrixMode(mode);
}

void RAS_IRasterizer::MultMatrix(const float mat[16])
{
	m_impl->MultMatrix(mat);
}

void RAS_IRasterizer::LoadMatrix(const float mat[16])
{
	m_impl->LoadMatrix(mat);
}

void RAS_IRasterizer::LoadIdentity()
{
	m_impl->LoadIdentity();
}

void RAS_IRasterizer::UpdateGlobalDepthTexture(RAS_OffScreen *offScreen)
{
	/* In case of multisamples the depth off screen must be blit to be used in shader.
	 * But the original off screen must be kept bound after the blit. */
	if (offScreen->GetSamples()) {
		RAS_OffScreen *dstOffScreen = GetOffScreen(RAS_IRasterizer::RAS_OFFSCREEN_BLIT_DEPTH);
		offScreen->Blit(dstOffScreen, false, true);
		// Restore original off screen.
		offScreen->Bind();
		offScreen = dstOffScreen;
	}

	GPU_texture_set_global_depth(offScreen->GetDepthTexture());
}

void RAS_IRasterizer::ResetGlobalDepthTexture()
{
	GPU_texture_set_global_depth(NULL);
}

void RAS_IRasterizer::MotionBlur()
{
	m_impl->MotionBlur(m_motionblur, m_motionblurvalue);
}

void RAS_IRasterizer::SetClientObject(void *obj)
{
	m_clientobject = obj;
}

void RAS_IRasterizer::SetAuxilaryClientInfo(void *inf)
{
	m_auxilaryClientInfo = inf;
}

void RAS_IRasterizer::PrintHardwareInfo()
{
	m_impl->PrintHardwareInfo();
}
