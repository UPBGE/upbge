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

#include "CM_Message.h"

#include "GPU_glew.h"

#include "BLI_math.h"


extern "C" {
#  include "DRW_render.h"
}

RAS_EeveeEffectsManager::RAS_EeveeEffectsManager(EEVEE_Data *vedata, RAS_ICanvas *canvas):
m_canvas(canvas)
{
	m_psl = vedata->psl;
	m_txl = vedata->txl;
	m_fbl = vedata->fbl;
	m_stl = vedata->stl;
	m_effects = m_stl->effects;

	InitBloom();
	InitBloomShaders();
}

RAS_EeveeEffectsManager::~RAS_EeveeEffectsManager()
{
}

void RAS_EeveeEffectsManager::InitBloomShaders()
{
	EEVEE_create_bloom_shgroups(m_effects, &m_bloomShGroup[BLOOM_FIRST], &m_bloomShGroup[BLOOM_DOWNSAMPLE],
		&m_bloomShGroup[BLOOM_UPSAMPLE], &m_bloomShGroup[BLOOM_BLIT], &m_bloomShGroup[BLOOM_RESOLVE]);
}

void RAS_EeveeEffectsManager::InitBloom()
{
	if ((m_effects->enabled_effects & EFFECT_BLOOM) != 0) {//BKE_collection_engine_property_value_get_bool(props, "bloom_enable")) {
		/* Bloom */
		int blitsize[2], texsize[2];

		/* Blit Buffer */
		m_effects->source_texel_size[0] = 1.0f / (m_canvas->GetWidth());
		m_effects->source_texel_size[1] = 1.0f / (m_canvas->GetHeight());

		blitsize[0] = (int)(m_canvas->GetWidth());
		blitsize[1] = (int)(m_canvas->GetHeight());

		m_effects->blit_texel_size[0] = 1.0f / (float)blitsize[0];
		m_effects->blit_texel_size[1] = 1.0f / (float)blitsize[1];

		m_bloomBlitOfs = new RAS_OffScreen(blitsize[0], blitsize[1], 0, GPU_RGB16F,
			GPU_OFFSCREEN_DEPTH_COMPARE, nullptr, RAS_Rasterizer::RAS_OFFSCREEN_BLOOMBLIT0);

		/* Parameters */
		float threshold = 0.8f;// BKE_collection_engine_property_value_get_float(props, "bloom_threshold");
		float knee = 0.5f;// BKE_collection_engine_property_value_get_float(props, "bloom_knee");
		float intensity = 0.8f;// BKE_collection_engine_property_value_get_float(props, "bloom_intensity");
		float radius = 6.5f;// BKE_collection_engine_property_value_get_float(props, "bloom_radius");

		/* determine the iteration count */
		const float minDim = (float)MIN2(blitsize[0], blitsize[1]);
		const float maxIter = (radius - 8.0f) + log(minDim) / log(2);
		const int maxIterInt = m_effects->bloom_iteration_ct = (int)maxIter;

		CLAMP(m_effects->bloom_iteration_ct, 1, MAX_BLOOM_STEP);

		m_effects->bloom_sample_scale = 0.5f + maxIter - (float)maxIterInt;
		m_effects->bloom_curve_threshold[0] = threshold - knee;
		m_effects->bloom_curve_threshold[1] = knee * 2.0f;
		m_effects->bloom_curve_threshold[2] = 0.25f / max_ff(1e-5f, knee);
		m_effects->bloom_curve_threshold[3] = threshold;
		m_effects->bloom_intensity = intensity;

		/* Downsample buffers */
		copy_v2_v2_int(texsize, blitsize);
		for (int i = 0; i < m_effects->bloom_iteration_ct; ++i) {
			texsize[0] /= 2; texsize[1] /= 2;
			texsize[0] = MAX2(texsize[0], 2);
			texsize[1] = MAX2(texsize[1], 2);

			m_effects->downsamp_texel_size[i][0] = 1.0f / (float)texsize[0];
			m_effects->downsamp_texel_size[i][1] = 1.0f / (float)texsize[1];

			m_bloomDownOfs[i] = new RAS_OffScreen(texsize[0], texsize[1], 0, GPU_RGB16F,
				GPU_OFFSCREEN_DEPTH_COMPARE, nullptr, RAS_Rasterizer::RAS_OFFSCREEN_BLOOMDOWN0);
			m_bloomDownOfs[i]->SetId(i);
		}


		/* Upsample buffers */
		copy_v2_v2_int(texsize, blitsize);
		for (int i = 0; i < m_effects->bloom_iteration_ct - 1; ++i) {
			texsize[0] /= 2; texsize[1] /= 2;
			texsize[0] = MAX2(texsize[0], 2);
			texsize[1] = MAX2(texsize[1], 2);

			m_bloomAccumOfs[i] = new RAS_OffScreen(texsize[0], texsize[1], 0, GPU_RGB16F,
				GPU_OFFSCREEN_DEPTH_COMPARE, nullptr, RAS_Rasterizer::RAS_OFFSCREEN_BLOOMACCUM0);
			m_bloomAccumOfs[i]->SetId(i);
		}
		m_bloomSteps = m_effects->bloom_iteration_ct;
	}
}

void RAS_EeveeEffectsManager::SwapOffscreens(RAS_Rasterizer *rasty)
{
	m_bloomBlitOfs = rasty->GetOffScreen(rasty->NextBloomOffScreen(m_bloomBlitOfs->GetType()), 0);
	for (int i = 0; i < m_bloomSteps; i++) {
		m_bloomDownOfs[i] = rasty->GetOffScreen(rasty->NextBloomOffScreen(m_bloomDownOfs[i]->GetType()), i);
		if (i < m_bloomSteps - 1) {
			m_bloomAccumOfs[i] = rasty->GetOffScreen(rasty->NextBloomOffScreen(m_bloomAccumOfs[i]->GetType()), i);
		}
	}
}

RAS_OffScreen *RAS_EeveeEffectsManager::RenderEeveeEffects(RAS_Rasterizer *rasty, RAS_ICanvas *canvas, RAS_OffScreen *inputofs, RAS_OffScreen *targetofs)
{
	rasty->Disable(RAS_Rasterizer::RAS_DEPTH_TEST);

	/* Bloom */
	if ((m_effects->enabled_effects & EFFECT_BLOOM) != 0) {

		SwapOffscreens(rasty);

		/* Extract bright pixels */
		copy_v2_v2(m_effects->unf_source_texel_size, m_effects->source_texel_size);
		m_effects->unf_source_buffer = inputofs->GetColorTexture();

		//DRW_framebuffer_bind(fbl->bloom_blit_fb);
		//DRW_draw_pass(psl->bloom_blit);
		m_bloomBlitOfs->Bind(); // j'ail l'impression qu'une cube map est toujours activée
		DRW_bind_shader_shgroup(m_bloomShGroup[BLOOM_BLIT]);
		rasty->DrawOverlayPlane();

		/* Downsample */
		copy_v2_v2(m_effects->unf_source_texel_size, m_effects->blit_texel_size);
		m_effects->unf_source_buffer = m_bloomBlitOfs->GetColorTexture(); // voila2 sec ici

		//DRW_framebuffer_bind(fbl->bloom_down_fb[0]);
		//DRW_draw_pass(psl->bloom_downsample_first);
		m_bloomDownOfs[0]->Bind();
		DRW_bind_shader_shgroup(m_bloomShGroup[BLOOM_FIRST]);
		rasty->DrawOverlayPlane();

		GPUTexture *last = m_bloomDownOfs[0]->GetColorTexture();

		for (int i = 1; i < m_effects->bloom_iteration_ct; ++i) {
			copy_v2_v2(m_effects->unf_source_texel_size, m_effects->downsamp_texel_size[i - 1]);
			m_effects->unf_source_buffer = last;

			//DRW_framebuffer_bind(fbl->bloom_down_fb[i]);
			//DRW_draw_pass(psl->bloom_downsample);

			m_bloomDownOfs[i]->Bind();
			DRW_bind_shader_shgroup(m_bloomShGroup[BLOOM_DOWNSAMPLE]);
			rasty->DrawOverlayPlane();

			/* Used in next loop */
			last = m_bloomDownOfs[i]->GetColorTexture();
		}

		/* Upsample and accumulate */
		for (int i = m_effects->bloom_iteration_ct - 2; i >= 0; --i) {
			copy_v2_v2(m_effects->unf_source_texel_size, m_effects->downsamp_texel_size[i]);
			m_effects->unf_source_buffer = m_bloomDownOfs[i]->GetColorTexture();
			m_effects->unf_base_buffer = last;

			//DRW_framebuffer_bind(fbl->bloom_accum_fb[i]);
			//DRW_draw_pass(psl->bloom_upsample);

			m_bloomAccumOfs[i]->Bind();
			DRW_bind_shader_shgroup(m_bloomShGroup[BLOOM_UPSAMPLE]);
			rasty->DrawOverlayPlane();

			last = m_bloomAccumOfs[i]->GetColorTexture();
		}

		/* Resolve */
		copy_v2_v2(m_effects->unf_source_texel_size, m_effects->downsamp_texel_size[0]);
		m_effects->unf_source_buffer = last;
		m_effects->unf_base_buffer = m_effects->source_buffer;

		m_effects->source_buffer = inputofs->GetColorTexture();

		DRW_bind_shader_shgroup(m_bloomShGroup[BLOOM_RESOLVE]);
		targetofs->Bind();
		rasty->DrawOverlayPlane();

		return targetofs;
	}

	rasty->Enable(RAS_Rasterizer::RAS_DEPTH_TEST);
	
	return inputofs;
}