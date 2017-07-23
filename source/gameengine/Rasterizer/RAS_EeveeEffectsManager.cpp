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

		DRWFboTexture tex_blit = { &m_txl->bloom_blit, DRW_TEX_RGB_11_11_10, DRW_TEX_FILTER };
		//DRW_framebuffer_init(&m_fbl->bloom_blit_fb, &draw_engine_eevee_type,
			//(int)blitsize[0], (int)blitsize[1],
			//&tex_blit, 1);

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

			DRWFboTexture tex_bloom = { &m_txl->bloom_downsample[i], DRW_TEX_RGB_11_11_10, DRW_TEX_FILTER };
			//DRW_framebuffer_init(&m_fbl->bloom_down_fb[i], &draw_engine_eevee_type,
				//(int)texsize[0], (int)texsize[1],
				//&tex_bloom, 1);
		}
	}
}

//RAS_2DFilter *RAS_2DFilterManager::AddFilter(RAS_2DFilterData& filterData)
//{
//	RAS_2DFilter *filter = CreateFilter(filterData);
//
//	m_filters[filterData.filterPassIndex] = filter;
//	// By default enable the filter.
//	filter->SetEnabled(true);
//
//	return filter;
//}
//
//void RAS_2DFilterManager::RemoveFilterPass(unsigned int passIndex)
//{
//	m_filters.erase(passIndex);
//}
//
//RAS_2DFilter *RAS_2DFilterManager::GetFilterPass(unsigned int passIndex)
//{
//	RAS_PassTo2DFilter::iterator it = m_filters.find(passIndex);
//	return (it != m_filters.end()) ? it->second : nullptr;
//}

RAS_OffScreen *RAS_EeveeEffectsManager::RenderEeveeEffects(RAS_Rasterizer *rasty, RAS_ICanvas *canvas, RAS_OffScreen *inputofs, RAS_OffScreen *targetofs)
{
	//if (m_filters.size() == 0 || !m_toneMapAdded) {
	//	// No filters, discard.
	//	if (!m_toneMapAdded) { // TODO define builtin filters.
	//		RAS_2DFilterData toneMapData;
	//		toneMapData.filterMode = RAS_2DFilterManager::FILTER_TONEMAP;
	//		toneMapData.filterPassIndex = m_filters.size() + 1;
	//		toneMapData.mipmap = false;
	//		AddFilter(toneMapData);
	//		m_toneMapAdded = true;
	//	}
	//	return inputofs;
	//}

#if 1
	return inputofs;
#else

	rasty->Disable(RAS_Rasterizer::RAS_CULL_FACE);
	rasty->Disable(RAS_Rasterizer::RAS_DEPTH_TEST);
	rasty->SetDepthMask(RAS_Rasterizer::RAS_DEPTHMASK_DISABLED);
	rasty->Disable(RAS_Rasterizer::RAS_BLEND);
	rasty->Disable(RAS_Rasterizer::RAS_ALPHA_TEST);

	rasty->SetLines(false);

	RAS_OffScreen *previousofs = inputofs;

	/* Set source off screen to RAS_OFFSCREEN_FILTER0 in case of multisample and blit,
	* else keep the original source off screen. */
	if (inputofs->GetSamples()) {
		previousofs = rasty->GetOffScreen(RAS_Rasterizer::RAS_OFFSCREEN_FILTER0);
		// No need to bind previousofs because a blit is proceeded.
		rasty->DrawOffScreen(inputofs, previousofs);
	}

	// The filter color input off screen, changed for each filters.
	RAS_OffScreen *colorofs;
	// The filter depth input off scree, unchanged for each filters.
	RAS_OffScreen *depthofs = previousofs;

	// Used to know if a filter is the last of the container.
	//RAS_PassTo2DFilter::const_iterator pend = std::prev(m_filters.end());

	//for (RAS_PassTo2DFilter::iterator begin = m_filters.begin(), it = begin, end = m_filters.end(); it != end; ++it) {
	//	RAS_2DFilter *filter = it->second;

	//	/* Assign the previous off screen to the input off screen. At the first render it's the
	//	* input off screen sent to RenderFilters. */
	//	colorofs = previousofs;

	//	RAS_OffScreen *ftargetofs;
	//	// Computing the filter targeted off screen.
	//	if (it == pend) {
	//		// Render to the targeted off screen for the last filter.
	//		ftargetofs = targetofs;
	//	}
	//	else {
	//		// Else render to the next off screen compared to the input off screen.
	//		ftargetofs = rasty->GetOffScreen(RAS_Rasterizer::NextFilterOffScreen(colorofs->GetType()));
	//	}

	//	/* Get the output off screen of the filter, could be the same as the input off screen
	//	* if no modifications were made or the targeted off screen.
	//	* This output off screen is used for the next filter as input off screen */
	//	previousofs = filter->Start(rasty, canvas, depthofs, colorofs, ftargetofs);
	//	filter->End();
	//}

	// The last filter doesn't use its own off screen and didn't render to the targeted off screen ?
	if (previousofs != targetofs) {
		// Render manually to the targeted off screen as the last filter didn't do it for us.
		targetofs->Bind();
		rasty->DrawOffScreen(previousofs, targetofs);
	}

	rasty->Enable(RAS_Rasterizer::RAS_DEPTH_TEST);
	rasty->SetDepthMask(RAS_Rasterizer::RAS_DEPTHMASK_ENABLED);
	rasty->Enable(RAS_Rasterizer::RAS_CULL_FACE);

	return targetofs;
#endif
}
