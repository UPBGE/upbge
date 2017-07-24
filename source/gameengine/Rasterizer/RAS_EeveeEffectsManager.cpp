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
	if (1) {//BKE_collection_engine_property_value_get_bool(props, "bloom_enable")) {
		/* Bloom */
		int blitsize[2], texsize[2];

		/* Blit Buffer */
		m_effects->source_texel_size[0] = 1.0f / (m_canvas->GetWidth() + 1);
		m_effects->source_texel_size[1] = 1.0f / (m_canvas->GetHeight() + 1);

		blitsize[0] = (int)(m_canvas->GetWidth() + 1);
		blitsize[1] = (int)(m_canvas->GetHeight() + 1);

		m_effects->blit_texel_size[0] = 1.0f / (float)blitsize[0];
		m_effects->blit_texel_size[1] = 1.0f / (float)blitsize[1];

		m_bloomBlitOfs.reset(new RAS_OffScreen(blitsize[0], blitsize[1], 0, GPU_R11F_G11F_B10F,
				GPU_OFFSCREEN_DEPTH_COMPARE, nullptr, RAS_Rasterizer::RAS_OFFSCREEN_CUSTOM));

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

			m_bloomDownOfs[i].reset(new RAS_OffScreen(texsize[0], texsize[1], 0, GPU_R11F_G11F_B10F,
					GPU_OFFSCREEN_DEPTH_COMPARE, nullptr, RAS_Rasterizer::RAS_OFFSCREEN_CUSTOM));
		}
	}
}

RAS_OffScreen *RAS_EeveeEffectsManager::RenderEeveeEffects(RAS_Rasterizer *rasty, RAS_ICanvas *canvas, RAS_OffScreen *inputofs,
														   RAS_OffScreen *targetofs)
{
#if 0

	/* Bloom */
	if ((effects->enabled_effects & EFFECT_BLOOM) != 0) {
		struct GPUTexture *last;

		/* Extract bright pixels */
		copy_v2_v2(effects->unf_source_texel_size, effects->source_texel_size);
		effects->unf_source_buffer = effects->source_buffer;

		DRW_framebuffer_bind(fbl->bloom_blit_fb);
		DRW_draw_pass(psl->bloom_blit);

		/* Downsample */
		copy_v2_v2(effects->unf_source_texel_size, effects->blit_texel_size);
		effects->unf_source_buffer = txl->bloom_blit;

		DRW_framebuffer_bind(fbl->bloom_down_fb[0]);
		DRW_draw_pass(psl->bloom_downsample_first);

		last = txl->bloom_downsample[0];

		for (int i = 1; i < effects->bloom_iteration_ct; ++i) {
			copy_v2_v2(effects->unf_source_texel_size, effects->downsamp_texel_size[i-1]);
			effects->unf_source_buffer = last;

			DRW_framebuffer_bind(fbl->bloom_down_fb[i]);
			DRW_draw_pass(psl->bloom_downsample);

			/* Used in next loop */
			last = txl->bloom_downsample[i];
		}

		/* Upsample and accumulate */
		for (int i = effects->bloom_iteration_ct - 2; i >= 0; --i) {
			copy_v2_v2(effects->unf_source_texel_size, effects->downsamp_texel_size[i]);
			effects->unf_source_buffer = txl->bloom_downsample[i];
			effects->unf_base_buffer = last;

			DRW_framebuffer_bind(fbl->bloom_accum_fb[i]);
			DRW_draw_pass(psl->bloom_upsample);

			last = txl->bloom_upsample[i];
		}

		/* Resolve */
		copy_v2_v2(effects->unf_source_texel_size, effects->downsamp_texel_size[0]);
		effects->unf_source_buffer = last;
		effects->unf_base_buffer = effects->source_buffer;

		DRW_framebuffer_bind(effects->target_buffer);
		DRW_draw_pass(psl->bloom_resolve);
		SWAP_BUFFERS();
	}

#endif

	return inputofs;
}
