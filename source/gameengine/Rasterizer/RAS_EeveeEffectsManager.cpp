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
#include "RAS_FrameBuffer.h"
#include "RAS_EeveeEffectsManager.h"
#include "RAS_SceneLayerData.h"

#include "GPU_framebuffer.h"

#include "BLI_math.h"
#include "DNA_world_types.h"

#include "KX_Scene.h"  // For DOF,
#include "KX_Camera.h" // motion blur and AO

extern "C" {
#  include "DRW_render.h"
}

RAS_EeveeEffectsManager::RAS_EeveeEffectsManager(EEVEE_Data *vedata, RAS_ICanvas *canvas,
	IDProperty *props, RAS_Rasterizer *rasty, KX_Scene *scene):
m_props(props),
m_rasterizer(rasty),
m_scene(scene),
m_dofInitialized(false),
m_bloomTarget(nullptr),
m_blurTarget(nullptr),
m_dofTarget(nullptr)
{
	m_stl = vedata->stl;
	m_psl = vedata->psl;
	m_txl = vedata->txl;
	m_fbl = vedata->fbl;
	m_effects = m_stl->effects;
	m_dtxl = DRW_viewport_texture_list_get();
	m_vedata = vedata;

	m_width = canvas->GetWidth() + 1;
	m_height = canvas->GetHeight() + 1;

	// Depth of field
	m_dofTarget = new RAS_FrameBuffer(m_width / 2, m_height / 2, canvas->GetHdrType(), RAS_Rasterizer::RAS_FRAMEBUFFER_EYE_LEFT0);

	// Bloom
	m_bloomTarget = new RAS_FrameBuffer(m_width, m_height, canvas->GetHdrType(), RAS_Rasterizer::RAS_FRAMEBUFFER_EYE_LEFT0);

	// Camera Motion Blur
	m_shutter = BKE_collection_engine_property_value_get_float(m_props, "motion_blur_shutter");
	m_effects->motion_blur_samples = BKE_collection_engine_property_value_get_int(m_props, "motion_blur_samples");
	m_blurTarget = new RAS_FrameBuffer(m_width, m_height, canvas->GetHdrType(), RAS_Rasterizer::RAS_FRAMEBUFFER_EYE_LEFT0);

	// Ambient occlusion
	m_useAO = m_effects->use_ao;
}

RAS_EeveeEffectsManager::~RAS_EeveeEffectsManager()
{
	delete m_bloomTarget;
	delete m_blurTarget;
	delete m_dofTarget;
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
		m_effects->dof_params[2] = m_width / (1.0f * sensorScaled);
	}
}

RAS_FrameBuffer *RAS_EeveeEffectsManager::RenderBloom(RAS_FrameBuffer *inputfb)
{
	/* Bloom */
	if ((m_effects->enabled_effects & EFFECT_BLOOM) != 0) {
		struct GPUTexture *last;

		m_effects->source_buffer = GPU_framebuffer_color_texture(inputfb->GetFrameBuffer());

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

		m_rasterizer->SetViewport(0, 0, m_width, m_height);

		DRW_framebuffer_bind(m_bloomTarget->GetFrameBuffer());
		DRW_draw_pass(m_psl->bloom_resolve);

		return m_bloomTarget;
	}
	return inputfb;
}

RAS_FrameBuffer *RAS_EeveeEffectsManager::RenderMotionBlur(RAS_FrameBuffer *inputfb)
{
	/* Motion Blur */
	if (BKE_collection_engine_property_value_get_bool(m_props, "motion_blur_enable")) {

		KX_Camera *cam = m_scene->GetActiveCamera();

		m_effects->source_buffer = GPU_framebuffer_color_texture(inputfb->GetFrameBuffer());
		m_dtxl->depth = GPU_framebuffer_depth_texture(inputfb->GetFrameBuffer());
		float camToWorld[4][4];
		cam->GetCameraToWorld().getValue(&camToWorld[0][0]);
		camToWorld[3][0] *= m_shutter;
		camToWorld[3][1] *= m_shutter;
		camToWorld[3][2] *= m_shutter;
		copy_m4_m4(m_effects->current_ndc_to_world, camToWorld);

		m_rasterizer->SetViewport(0, 0, m_width, m_height);

		DRW_framebuffer_bind(m_blurTarget->GetFrameBuffer());
		DRW_draw_pass(m_psl->motion_blur);

		float worldToCam[4][4];
		cam->GetWorldToCamera().getValue(&worldToCam[0][0]);
		worldToCam[3][0] *= m_shutter;
		worldToCam[3][1] *= m_shutter;
		worldToCam[3][2] *= m_shutter;
		copy_m4_m4(m_effects->past_world_to_ndc, worldToCam);

		return m_blurTarget;
	}
	return inputfb;
}

RAS_FrameBuffer *RAS_EeveeEffectsManager::RenderDof(RAS_FrameBuffer *inputfb)
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

		m_effects->source_buffer = GPU_framebuffer_color_texture(inputfb->GetFrameBuffer());
		m_dtxl->depth = GPU_framebuffer_depth_texture(inputfb->GetFrameBuffer());

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
		DRW_framebuffer_bind(m_dofTarget->GetFrameBuffer());
		DRW_draw_pass(m_psl->dof_resolve);

		return m_dofTarget;
	}
	return inputfb;
}

void RAS_EeveeEffectsManager::CreateMinMaxDepth(RAS_FrameBuffer *inputfb)
{
	if (m_useAO || m_effects->enabled_effects & EFFECT_SSR) {
		/* Create stl->g_data->minmaxz from our depth texture.
		 * This texture is used as uniform if AO is enabled or some other effects...
		 * See: DRW_shgroup_uniform_buffer(shgrp, "minMaxDepthTex", &vedata->stl->g_data->minmaxz);
		 */
		EEVEE_create_minmax_buffer(m_scene->GetEeveeData(), GPU_framebuffer_depth_texture(inputfb->GetFrameBuffer()), -1);
	}
}

void RAS_EeveeEffectsManager::ComputeVolumetrics()
{
	if ((m_effects->enabled_effects & EFFECT_VOLUMETRIC) != 0) {
		/* Step 1: Participating Media Properties */
		DRW_framebuffer_bind(m_fbl->volumetric_fb);
		DRW_draw_pass(m_psl->volumetric_world_ps);
		DRW_draw_pass(m_psl->volumetric_objects_ps);

		/* Step 2: Scatter Light */
		DRW_framebuffer_bind(m_fbl->volumetric_scat_fb);
		DRW_draw_pass(m_psl->volumetric_scatter_ps);

		/* Step 3: Integration */
		DRW_framebuffer_bind(m_fbl->volumetric_integ_fb);
		DRW_draw_pass(m_psl->volumetric_integration_ps);

		/* Swap volume history buffers */
		SWAP(struct GPUFrameBuffer *, m_fbl->volumetric_scat_fb, m_fbl->volumetric_integ_fb);
		SWAP(GPUTexture *, m_txl->volume_scatter, m_txl->volume_scatter_history);
		SWAP(GPUTexture *, m_txl->volume_transmittance, m_txl->volume_transmittance_history);

		/* Restore */
		DRW_framebuffer_bind(m_fbl->main);
	}
}

RAS_FrameBuffer *RAS_EeveeEffectsManager::ResolveVolumetrics(RAS_FrameBuffer *inputfb)
{
	if ((m_effects->enabled_effects & EFFECT_VOLUMETRIC) != 0) {

		m_dtxl->depth = GPU_framebuffer_depth_texture(inputfb->GetFrameBuffer());
		EEVEE_effects_replace_e_data_depth(m_dtxl->depth);
		m_txl->color = GPU_framebuffer_color_texture(inputfb->GetFrameBuffer());
		EEVEE_effects_replace_e_data_color(m_txl->color);

		/* Step 4: Apply for opaque */
		//DRW_framebuffer_bind(inputfb->GetFrameBuffer());
		//m_rasterizer->SetViewport(0, 0, m_width, m_height);
		//DRW_framebuffer_bind(m_fbl->effect_fb);
		//DRW_draw_pass(m_psl->volumetric_resolve_ps);

		/* Swap the buffers and rebind depth to the current buffer */
		DRW_framebuffer_texture_detach(m_dtxl->depth);
		SWAP(struct GPUFrameBuffer *, m_fbl->main, m_fbl->effect_fb);
		SWAP(GPUTexture *, m_txl->color, m_txl->color_post);
		DRW_framebuffer_texture_attach(m_fbl->main, m_dtxl->depth, 0, 0);

		return inputfb;
	}
	return inputfb;
}

void RAS_EeveeEffectsManager::DoSSR(RAS_FrameBuffer *inputfb)
{
	if ((m_effects->enabled_effects & EFFECT_SSR) != 0) {

		m_txl->color_double_buffer = GPU_framebuffer_color_texture(inputfb->GetFrameBuffer()); // Color uniform for SSR shader
		m_dtxl->depth = GPU_framebuffer_depth_texture(inputfb->GetFrameBuffer());
		EEVEE_effects_replace_e_data_depth(m_dtxl->depth);                                     // Depth uniform for SSR shader
		KX_Camera *cam = m_scene->GetActiveCamera();
		MT_Matrix4x4 prevpers(cam->GetProjectionMatrix() * cam->GetModelviewMatrix());
		/* Notes:
		 * 1) In eevee the SSR is computed during several passes. I think stl->g_data->prev_persmat stores
		 * persmat for the next passes to compute SSR. In upbge I named it prevpers but it's the current frame
		 * product between cam proj and cam modelviewmat.
		 * 2) When you look at blender code, they say that persmat is viewmat * projmat. In bge it seems that
		 * we need the inverse -> proj * view.
		 */
		prevpers.getValue(&m_stl->g_data->prev_persmat[0][0]);                                 // Matrix uniform for SSR shader

		for (int i = 0; i < m_effects->ssr_ray_count; ++i) {
			DRW_framebuffer_texture_attach(m_fbl->screen_tracing_fb, m_stl->g_data->ssr_hit_output[i], i, 0);
		}
		DRW_framebuffer_bind(m_fbl->screen_tracing_fb);

		/* Raytrace. */
		DRW_draw_pass(m_psl->ssr_raytrace);

		for (int i = 0; i < m_effects->ssr_ray_count; ++i) {
			DRW_framebuffer_texture_detach(m_stl->g_data->ssr_hit_output[i]);
		}

		EEVEE_downsample_buffer(m_vedata, m_fbl->downsample_fb, m_txl->color_double_buffer, 9);

		/* Resolve at fullres */
		DRW_framebuffer_texture_detach(m_dtxl->depth);
		DRW_framebuffer_texture_detach(m_txl->ssr_normal_input);
		DRW_framebuffer_texture_detach(m_txl->ssr_specrough_input);
		DRW_framebuffer_bind(inputfb->GetFrameBuffer());
		DRW_draw_pass(m_psl->ssr_resolve);

		/* Restore */
		DRW_framebuffer_texture_attach(inputfb->GetFrameBuffer(), m_dtxl->depth, 0, 0);
		DRW_framebuffer_texture_attach(inputfb->GetFrameBuffer(), m_txl->ssr_normal_input, 1, 0);
		DRW_framebuffer_texture_attach(inputfb->GetFrameBuffer(), m_txl->ssr_specrough_input, 2, 0);
	}
}

void RAS_EeveeEffectsManager::DoGTAO(RAS_FrameBuffer *inputfb)
{
	if ((m_effects->enabled_effects & EFFECT_GTAO) != 0) {
		DefaultTextureList *dtxl = DRW_viewport_texture_list_get();
		EEVEE_effects_replace_e_data_depth(GPU_framebuffer_depth_texture(inputfb->GetFrameBuffer()));

		for (m_effects->ao_sample_nbr = 0.0; m_effects->ao_sample_nbr < m_effects->ao_samples; m_effects->ao_sample_nbr++) {

			DRW_framebuffer_texture_detach(m_txl->gtao_horizons);
			DRW_framebuffer_texture_layer_attach(m_fbl->gtao_fb, m_txl->gtao_horizons, 0, (int)m_effects->ao_sample_nbr, 0);
			DRW_framebuffer_bind(m_fbl->gtao_fb);

			DRW_draw_pass(m_psl->ao_horizon_search);
		}
	}
}


RAS_FrameBuffer *RAS_EeveeEffectsManager::RenderEeveeEffects(RAS_FrameBuffer *inputfb)
{
	m_rasterizer->Disable(RAS_Rasterizer::RAS_DEPTH_TEST);

	/* In the case the last draw material in material pass was transparent,
	 * GL_BLEND is enabled and this can cause trouble when we we render
	 * eevee effects (bloom accumulation) so I disable GL_BLEND here.
	 * Not sure this is the better place but...
	 * Maybe we can find common ogl states for PostRenderEevee and
	 * PostRenderScene (filters2D) and set all these states in a function
	 * called in KetsjiEngine.
	 */
	m_rasterizer->Disable(RAS_Rasterizer::RAS_BLEND);

	CreateMinMaxDepth(inputfb); // Used for AO and SSR and...?

	DoGTAO(inputfb);

	DoSSR(inputfb);

	//inputfb = ResolveVolumetrics(inputfb);

	inputfb = RenderMotionBlur(inputfb);

	inputfb = RenderDof(inputfb);

	inputfb = RenderBloom(inputfb);

	m_rasterizer->Enable(RAS_Rasterizer::RAS_DEPTH_TEST);
	
	return inputfb;
}