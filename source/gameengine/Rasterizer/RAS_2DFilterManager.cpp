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

/** \file gameengine/Rasterizer/RAS_2DFilterManager.cpp
 *  \ingroup bgerast
 */

#include "RAS_ICanvas.h"
#include "RAS_IRasterizer.h"
#include "RAS_OffScreen.h"
#include "RAS_2DFilterManager.h"
#include "RAS_2DFilter.h"

#include "CM_Message.h"

#include "glew-mx.h"

extern "C" {
	extern char datatoc_RAS_Blur2DFilter_glsl[];
	extern char datatoc_RAS_Sharpen2DFilter_glsl[];
	extern char datatoc_RAS_Dilation2DFilter_glsl[];
	extern char datatoc_RAS_Erosion2DFilter_glsl[];
	extern char datatoc_RAS_Laplacian2DFilter_glsl[];
	extern char datatoc_RAS_Sobel2DFilter_glsl[];
	extern char datatoc_RAS_Prewitt2DFilter_glsl[];
	extern char datatoc_RAS_GrayScale2DFilter_glsl[];
	extern char datatoc_RAS_Sepia2DFilter_glsl[];
	extern char datatoc_RAS_Invert2DFilter_glsl[];
}

RAS_2DFilterManager::RAS_2DFilterManager()
{
}

RAS_2DFilterManager::~RAS_2DFilterManager()
{
	for (RAS_PassTo2DFilter::iterator it = m_filters.begin(), end = m_filters.end(); it != end; ++it) {
		RAS_2DFilter *filter = it->second;
		delete filter;
	}
}

RAS_2DFilter *RAS_2DFilterManager::AddFilter(RAS_2DFilterData& filterData)
{
	RAS_2DFilter *filter = CreateFilter(filterData);

	m_filters[filterData.filterPassIndex] = filter;
	// By default enable the filter.
	filter->SetEnabled(true);

	return filter;
}

void RAS_2DFilterManager::RemoveFilterPass(unsigned int passIndex)
{
	m_filters.erase(passIndex);
}

RAS_2DFilter *RAS_2DFilterManager::GetFilterPass(unsigned int passIndex)
{
	RAS_PassTo2DFilter::iterator it = m_filters.find(passIndex);
	return (it != m_filters.end()) ? it->second : nullptr;
}

RAS_OffScreen *RAS_2DFilterManager::RenderFilters(RAS_IRasterizer *rasty, RAS_ICanvas *canvas, RAS_OffScreen *inputofs, RAS_OffScreen *targetofs)
{
	/* The output off screen is set to the inputs off screen in case none filters will be rendered.
	 * Its value is changed later when rendering the filters. */
	RAS_OffScreen *outputofs = inputofs;

	if (m_filters.size() == 0) {
		// No filters, discard.
		return outputofs;
	}

	RAS_OffScreen *colorofs = inputofs;
	RAS_OffScreen *depthofs = colorofs;

	rasty->Disable(RAS_IRasterizer::RAS_CULL_FACE);
	rasty->SetDepthFunc(RAS_IRasterizer::RAS_ALWAYS);
	rasty->Disable(RAS_IRasterizer::RAS_BLEND);
	rasty->Disable(RAS_IRasterizer::RAS_ALPHA_TEST);

	rasty->SetLines(false);

	// Used to know if a filter is the last of the container.
	RAS_PassTo2DFilter::const_iterator pend = std::prev(m_filters.end());

	for (RAS_PassTo2DFilter::iterator begin = m_filters.begin(), it = begin, end = m_filters.end(); it != end; ++it) {
		RAS_2DFilter *filter = it->second;

		// Computing the depth and color input off screens.
		if (it == begin) {
			/* Set source off screen to RAS_OFFSCREEN_FILTER0 in case of multisample and blit,
			 * else keep the original source off screen. */
			if (inputofs->GetSamples()) {
				colorofs = depthofs = rasty->GetOffScreen(RAS_IRasterizer::RAS_OFFSCREEN_FILTER0);
				rasty->DrawOffScreen(inputofs, colorofs);
			}
		}
		else {
			colorofs = rasty->GetOffScreen(RAS_IRasterizer::NextFilterOffScreen(colorofs->GetType()));
		}

		// Computing the output off screen.
		if (it == pend) {
			// Render to the targeted FBO for the last filter.
			outputofs = targetofs;
		}
		else {
			outputofs = rasty->GetOffScreen(RAS_IRasterizer::NextFilterOffScreen(colorofs->GetType()));
		}

		filter->Start(rasty, canvas, depthofs, colorofs, outputofs);
		filter->End();
	}

	rasty->SetDepthFunc(RAS_IRasterizer::RAS_LEQUAL);
	rasty->Enable(RAS_IRasterizer::RAS_CULL_FACE);

	return outputofs;
}

RAS_2DFilter *RAS_2DFilterManager::CreateFilter(RAS_2DFilterData& filterData)
{
	RAS_2DFilter *result = nullptr;
	std::string shaderSource;
	switch(filterData.filterMode) {
		case RAS_2DFilterManager::FILTER_MOTIONBLUR:
			break;
		case RAS_2DFilterManager::FILTER_BLUR:
			shaderSource = datatoc_RAS_Blur2DFilter_glsl;
			break;
		case RAS_2DFilterManager::FILTER_SHARPEN:
			shaderSource = datatoc_RAS_Sharpen2DFilter_glsl;
			break;
		case RAS_2DFilterManager::FILTER_DILATION:
			shaderSource = datatoc_RAS_Dilation2DFilter_glsl;
			break;
		case RAS_2DFilterManager::FILTER_EROSION:
			shaderSource = datatoc_RAS_Erosion2DFilter_glsl;
			break;
		case RAS_2DFilterManager::FILTER_LAPLACIAN:
			shaderSource = datatoc_RAS_Laplacian2DFilter_glsl;
			break;
		case RAS_2DFilterManager::FILTER_SOBEL:
			shaderSource = datatoc_RAS_Sobel2DFilter_glsl;
			break;
		case RAS_2DFilterManager::FILTER_PREWITT:
			shaderSource = datatoc_RAS_Prewitt2DFilter_glsl;
			break;
		case RAS_2DFilterManager::FILTER_GRAYSCALE:
			shaderSource = datatoc_RAS_GrayScale2DFilter_glsl;
			break;
		case RAS_2DFilterManager::FILTER_SEPIA:
			shaderSource = datatoc_RAS_Sepia2DFilter_glsl;
			break;
		case RAS_2DFilterManager::FILTER_INVERT:
			shaderSource = datatoc_RAS_Invert2DFilter_glsl;
			break;
	}
	if (shaderSource.empty()) {
		if(filterData.filterMode == RAS_2DFilterManager::FILTER_CUSTOMFILTER) {
			result = NewFilter(filterData);
		}
		else {
			CM_Error("cannot create filter for mode: " << filterData.filterMode << ".");
		}
	}
	else {
		filterData.shaderText = shaderSource;
		result = NewFilter(filterData);
	}
	return result;
}
