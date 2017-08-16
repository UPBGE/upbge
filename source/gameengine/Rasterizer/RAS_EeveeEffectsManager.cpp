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
* Contributor(s): Pierluigi Grassi, Porteries Tristan.
*
* ***** END GPL LICENSE BLOCK *****
*/

/** \file gameengine/Rasterizer/RAS_EeveeEffectsManager.cpp
*  \ingroup bgerast
*/

#include "RAS_ICanvas.h"
#include "RAS_Rasterizer.h"
#include "RAS_OffScreen.h"
#include "RAS_EeveeEffectsManager.h"
#include "RAS_SceneLayerData.h"

#include "BLI_math.h"
#include "DNA_world_types.h"

#include "KX_Scene.h"  // For DOF,
#include "KX_Camera.h" // motion blur and AO

extern "C" {
#  include "DRW_render.h"
}

RAS_EeveeEffectsManager::RAS_EeveeEffectsManager(EEVEE_Data *vedata, RAS_ICanvas *canvas, IDProperty *props, KX_Scene *scene):
m_canvas(canvas),
m_props(props),
m_scene(scene),
m_dofInitialized(false)
{
	m_stl = vedata->stl;
	m_psl = vedata->psl;
	m_txl = vedata->txl;
	m_fbl = vedata->fbl;
	m_effects = m_stl->effects;

	static const GPUTextureFormat dataTypeEnums[] = {
		GPU_RGBA8, // RAS_HDR_NONE
		GPU_RGBA16F, // RAS_HDR_HALF_FLOAT
		GPU_RGBA32F // RAS_HDR_FULL_FLOAT
	};

	m_savedDepth = m_scene->GetDefaultTextureList()->depth;

	// Bloom
	m_bloomTarget.reset(new RAS_OffScreen(m_canvas->GetWidth() + 1, m_canvas->GetHeight() + 1, 0, dataTypeEnums[m_canvas->GetHdrType()],
		GPU_OFFSCREEN_DEPTH_COMPARE, nullptr, RAS_Rasterizer::RAS_OFFSCREEN_EYE_LEFT0));
	InitBloom();

	// Camera Motion Blur
	m_shutter = BKE_collection_engine_property_value_get_float(m_props, "motion_blur_shutter");
	m_blurTarget.reset(new RAS_OffScreen(m_canvas->GetWidth() + 1, m_canvas->GetHeight() + 1, 0, dataTypeEnums[m_canvas->GetHdrType()],
		GPU_OFFSCREEN_DEPTH_COMPARE, nullptr, RAS_Rasterizer::RAS_OFFSCREEN_EYE_LEFT0));

	// Depth of field
	m_dofTarget.reset(new RAS_OffScreen(int(m_canvas->GetWidth() / 2), int(m_canvas->GetHeight() / 2), 0, dataTypeEnums[m_canvas->GetHdrType()],
		GPU_OFFSCREEN_DEPTH_COMPARE, nullptr, RAS_Rasterizer::RAS_OFFSCREEN_EYE_LEFT0));

	// Ambient occlusion
	m_useAO = m_effects->use_ao;

	// Volumetrics
	World *world = m_scene->GetBlenderScene()->world;
	m_useVolumetricNodes = (world && world->use_nodes && world->nodetree);
}

RAS_EeveeEffectsManager::~RAS_EeveeEffectsManager()
{
	// Restore dtxl->depth at ge exit
	m_scene->GetDefaultTextureList()->depth = m_savedDepth;
}

void RAS_EeveeEffectsManager::UpdateViewVecs()
{
	/* Update viewvecs */
	const bool is_persp = DRW_viewport_is_persp_get();
	float invproj[4][4], winmat[4][4];
	/* view vectors for the corners of the view frustum.
	* Can be used to recreate the world space position easily */
	float viewvecs[3][4] = {
		{ -1.0f, -1.0f, -1.0f, 1.0f },
		{ 1.0f, -1.0f, -1.0f, 1.0f },
		{ -1.0f, 1.0f, -1.0f, 1.0f }
	};

	KX_Camera *cam = m_scene->GetActiveCamera();
	/* invert the view matrix */
	cam->GetProjectionMatrix().getValue(&winmat[0][0]);
	invert_m4_m4(invproj, winmat);

	/* convert the view vectors to view space */
	for (int i = 0; i < 3; i++) {
		mul_m4_v4(invproj, viewvecs[i]);
		/* normalized trick see:
		* http://www.derschmale.com/2014/01/26/reconstructing-positions-from-the-depth-buffer */
		mul_v3_fl(viewvecs[i], 1.0f / viewvecs[i][3]);
		if (is_persp)
			mul_v3_fl(viewvecs[i], 1.0f / viewvecs[i][2]);
		viewvecs[i][3] = 1.0;
	}

	copy_v4_v4(m_stl->g_data->viewvecs[0], viewvecs[0]);
	copy_v4_v4(m_stl->g_data->viewvecs[1], viewvecs[1]);

	/* we need to store the differences */
	m_stl->g_data->viewvecs[1][0] -= viewvecs[0][0];
	m_stl->g_data->viewvecs[1][1] = viewvecs[2][1] - viewvecs[0][1];

	/* calculate a depth offset as well */
	if (!is_persp) {
		float vec_far[] = { -1.0f, -1.0f, 1.0f, 1.0f };
		mul_m4_v4(invproj, vec_far);
		mul_v3_fl(vec_far, 1.0f / vec_far[3]);
		m_stl->g_data->viewvecs[1][2] = vec_far[2] - viewvecs[0][2];
	}
}

void RAS_EeveeEffectsManager::InitBloom()
{
	/* Bloom */
	if ((m_effects->enabled_effects & EFFECT_BLOOM) != 0) {
		/* We have to update eevee effects to the game engine viewport size */
		int blitsize[2], texsize[2];

		/* Blit Buffer */
		m_effects->source_texel_size[0] = 1.0f / (m_canvas->GetWidth());
		m_effects->source_texel_size[1] = 1.0f / (m_canvas->GetHeight());

		blitsize[0] = (int)(m_canvas->GetWidth());
		blitsize[1] = (int)(m_canvas->GetHeight());

		m_effects->blit_texel_size[0] = 1.0f / (float)blitsize[0];
		m_effects->blit_texel_size[1] = 1.0f / (float)blitsize[1];

		/* Downsample buffers */
		copy_v2_v2_int(texsize, blitsize);
		for (int i = 0; i < m_effects->bloom_iteration_ct; ++i) {
			texsize[0] /= 2; texsize[1] /= 2;
			texsize[0] = MAX2(texsize[0], 2);
			texsize[1] = MAX2(texsize[1], 2);

			m_effects->downsamp_texel_size[i][0] = 1.0f / (float)texsize[0];
			m_effects->downsamp_texel_size[i][1] = 1.0f / (float)texsize[1];
		}
	}
}

void RAS_EeveeEffectsManager::InitDof()
{
	if (m_effects->enabled_effects & EFFECT_DOF) {
		/* Depth Of Field */
		KX_Camera *cam = m_scene->GetActiveCamera();
		float sensorSize = cam->GetCameraData()->m_sensor_x;
		/* Only update params that needs to be updated */
		float scaleCamera = 0.001f;
		float sensorScaled = scaleCamera * sensorSize;
		m_effects->dof_params[2] = m_canvas->GetWidth() / (1.0f * sensorScaled);
	}
}

RAS_OffScreen *RAS_EeveeEffectsManager::RenderBloom(RAS_Rasterizer *rasty, RAS_OffScreen *inputofs)
{
	/* Bloom */
	if ((m_effects->enabled_effects & EFFECT_BLOOM) != 0) {
		struct GPUTexture *last;

		m_effects->source_buffer = inputofs->GetColorTexture();

		/* Extract bright pixels */
		copy_v2_v2(m_effects->unf_source_texel_size, m_effects->source_texel_size);
		m_effects->unf_source_buffer = m_effects->source_buffer;

		DRW_framebuffer_bind(m_fbl->bloom_blit_fb);
		DRW_draw_pass(m_psl->bloom_blit);

		/* Downsample */
		copy_v2_v2(m_effects->unf_source_texel_size, m_effects->blit_texel_size);
		m_effects->unf_source_buffer = m_txl->bloom_blit;

		DRW_framebuffer_bind(m_fbl->bloom_down_fb[0]);
		DRW_draw_pass(m_psl->bloom_downsample_first);

		last = m_txl->bloom_downsample[0];

		for (int i = 1; i < m_effects->bloom_iteration_ct; ++i) {
			copy_v2_v2(m_effects->unf_source_texel_size, m_effects->downsamp_texel_size[i - 1]);
			m_effects->unf_source_buffer = last;

			DRW_framebuffer_bind(m_fbl->bloom_down_fb[i]);
			DRW_draw_pass(m_psl->bloom_downsample);

			/* Used in next loop */
			last = m_txl->bloom_downsample[i];
		}

		/* Upsample and accumulate */
		for (int i = m_effects->bloom_iteration_ct - 2; i >= 0; --i) {
			copy_v2_v2(m_effects->unf_source_texel_size, m_effects->downsamp_texel_size[i]);
			m_effects->unf_source_buffer = m_txl->bloom_downsample[i];
			m_effects->unf_base_buffer = last;

			DRW_framebuffer_bind(m_fbl->bloom_accum_fb[i]);
			DRW_draw_pass(m_psl->bloom_upsample);

			last = m_txl->bloom_upsample[i];
		}

		/* Resolve */
		copy_v2_v2(m_effects->unf_source_texel_size, m_effects->downsamp_texel_size[0]);
		m_effects->unf_source_buffer = last;
		m_effects->unf_base_buffer = m_effects->source_buffer;

		rasty->SetViewport(0, 0, m_canvas->GetWidth() + 1, m_canvas->GetHeight() + 1);

		m_bloomTarget->Bind();
		DRW_draw_pass(m_psl->bloom_resolve);

		return m_bloomTarget.get();
	}
	return inputofs;
}

RAS_OffScreen *RAS_EeveeEffectsManager::RenderMotionBlur(RAS_Rasterizer *rasty, RAS_OffScreen *inputofs)
{
	/* Motion Blur */
	if ((m_effects->enabled_effects & EFFECT_MOTION_BLUR) != 0) {

		KX_Camera *cam = m_scene->GetActiveCamera();

		m_effects->source_buffer = inputofs->GetColorTexture();
		m_scene->GetDefaultTextureList()->depth = inputofs->GetDepthTexture();
		float camToWorld[4][4];
		cam->GetCameraToWorld().getValue(&camToWorld[0][0]);
		camToWorld[3][0] *= m_shutter;
		camToWorld[3][1] *= m_shutter;
		camToWorld[3][2] *= m_shutter;
		copy_m4_m4(m_effects->current_ndc_to_world, camToWorld);

		m_blurTarget->Bind();
		DRW_draw_pass(m_psl->motion_blur);

		float worldToCam[4][4];
		cam->GetWorldToCamera().getValue(&worldToCam[0][0]);
		worldToCam[3][0] *= m_shutter;
		worldToCam[3][1] *= m_shutter;
		worldToCam[3][2] *= m_shutter;
		copy_m4_m4(m_effects->past_world_to_ndc, worldToCam);

		return m_blurTarget.get();
	}
	return inputofs;
}

RAS_OffScreen *RAS_EeveeEffectsManager::RenderDof(RAS_Rasterizer *rasty, RAS_OffScreen *inputofs)
{
	/* Depth Of Field */
	if ((m_effects->enabled_effects & EFFECT_DOF) != 0) {

		if (!m_dofInitialized) {
			/* We need to initialize at runtime (not in constructor)
			 * to access m_scene->GetActiveCamera()
			 */
			InitDof();
			m_dofInitialized = true;
		}

		float clear_col[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

		m_effects->source_buffer = inputofs->GetColorTexture();
		m_scene->GetDefaultTextureList()->depth = inputofs->GetDepthTexture();

		/* Downsample */
		DRW_framebuffer_bind(m_fbl->dof_down_fb);
		DRW_draw_pass(m_psl->dof_down);

		/* Scatter Far */
		m_effects->unf_source_buffer = m_txl->dof_down_far;
		copy_v2_fl2(m_effects->dof_layer_select, 0.0f, 1.0f);
		DRW_framebuffer_bind(m_fbl->dof_scatter_far_fb);
		DRW_framebuffer_clear(true, false, false, clear_col, 0.0f);
		DRW_draw_pass(m_psl->dof_scatter);

		/* Scatter Near */
		if ((m_effects->enabled_effects & EFFECT_BLOOM) != 0) {
			/* Reuse bloom half res buffer */
			m_effects->unf_source_buffer = m_txl->bloom_downsample[0];
		}
		else {
			m_effects->unf_source_buffer = m_txl->dof_down_near;
		}
		copy_v2_fl2(m_effects->dof_layer_select, 1.0f, 0.0f);
		DRW_framebuffer_bind(m_fbl->dof_scatter_near_fb);
		DRW_framebuffer_clear(true, false, false, clear_col, 0.0f);
		DRW_draw_pass(m_psl->dof_scatter);

		/* Resolve */
		m_dofTarget->Bind();
		DRW_draw_pass(m_psl->dof_resolve);

		return m_dofTarget.get();
	}
	return inputofs;
}

void RAS_EeveeEffectsManager::UpdateAO(RAS_OffScreen *inputofs)
{
	if (m_useAO) {
		/* Create stl->g_data->minmaxz from our depth texture.
		 * This texture is used as uniform if AO is enabled.
		 * See: DRW_shgroup_uniform_buffer(shgrp, "minMaxDepthTex", &vedata->stl->g_data->minmaxz);
		 */
		EEVEE_create_minmax_buffer(m_scene->GetEeveeData(), inputofs->GetDepthTexture());

		UpdateViewVecs();
	}
}

RAS_OffScreen *RAS_EeveeEffectsManager::RenderVolumetrics(RAS_Rasterizer *rasty, RAS_OffScreen *inputofs)
{
	if ((m_effects->enabled_effects & EFFECT_VOLUMETRIC) != 0 && m_useVolumetricNodes) {

		UpdateViewVecs();

		EEVEE_effects_replace_dtxl_depth(inputofs->GetDepthTexture());
		//e_data.depth_src = dtxl->depth;

		/* Compute volumetric integration at halfres. */
		DRW_framebuffer_texture_attach(m_fbl->volumetric_fb, m_stl->g_data->volumetric, 0, 0);
		EEVEE_SceneLayerData *sldata = (EEVEE_SceneLayerData *)(&m_scene->GetSceneLayerData()->GetData());
		if (sldata->volumetrics->use_colored_transmit) {
			DRW_framebuffer_texture_attach(m_fbl->volumetric_fb, m_stl->g_data->volumetric_transmit, 1, 0);
		}
		DRW_framebuffer_bind(m_fbl->volumetric_fb);
		DRW_draw_pass(m_psl->volumetric_integrate_ps);

		/* Resolve at fullres */
		DRW_framebuffer_texture_detach(m_scene->GetDefaultTextureList()->depth);
		DRW_framebuffer_bind(m_fbl->main);
		if (sldata->volumetrics->use_colored_transmit) {
			DRW_draw_pass(m_psl->volumetric_resolve_transmit_ps);
		}
		inputofs->Bind();
		DRW_draw_pass(m_psl->volumetric_resolve_ps);

		///* Restore */
		//DRW_framebuffer_texture_attach(fbl->main, dtxl->depth, 0, 0);
		//DRW_framebuffer_texture_detach(stl->g_data->volumetric);
		//if (sldata->volumetrics->use_colored_transmit) {
		//	DRW_framebuffer_texture_detach(stl->g_data->volumetric_transmit);
		//}

		///* Rebind main buffer after attach/detach operations */
		//DRW_framebuffer_bind(fbl->main);
		return inputofs;
	}
	return inputofs;
}


RAS_OffScreen *RAS_EeveeEffectsManager::RenderEeveeEffects(RAS_Rasterizer *rasty, RAS_OffScreen *inputofs)
{
	rasty->Disable(RAS_Rasterizer::RAS_DEPTH_TEST);

	UpdateAO(inputofs);

	inputofs = RenderVolumetrics(rasty, inputofs);

	inputofs = RenderMotionBlur(rasty, inputofs);

	inputofs = RenderDof(rasty, inputofs);

	inputofs = RenderBloom(rasty, inputofs);

	rasty->Enable(RAS_Rasterizer::RAS_DEPTH_TEST);
	
	return inputofs;
}