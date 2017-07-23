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



RAS_EeveeEffectsManager::RAS_EeveeEffectsManager(EEVEE_Data *vedata, RAS_ICanvas *canvas):
m_canvas(canvas)
{
	m_psl = vedata->psl;
	m_txl = vedata->txl;
	m_fbl = vedata->fbl;
	m_stl = vedata->stl;
	m_effects = m_stl->effects;
}

RAS_EeveeEffectsManager::~RAS_EeveeEffectsManager()
{
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
