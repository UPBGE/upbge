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

/** \file RAS_2DFilterManager.h
 *  \ingroup bgerast
 */

#ifndef __RAS_2DFILTERMANAGER_H__
#define __RAS_2DFILTERMANAGER_H__

#include "RAS_2DFilterData.h"
#include <map>

class RAS_ICanvas;
class RAS_Rasterizer;
class RAS_OffScreen;
class RAS_2DFilter;

typedef std::map<unsigned int, RAS_2DFilter *> RAS_PassTo2DFilter;

class RAS_2DFilterManager
{
public:
	enum FILTER_MODE {
		FILTER_ENABLED = -2,
		FILTER_DISABLED = -1,
		FILTER_NOFILTER = 0,
		FILTER_MOTIONBLUR,
		FILTER_BLUR,
		FILTER_SHARPEN,
		FILTER_DILATION,
		FILTER_EROSION,
		FILTER_LAPLACIAN,
		FILTER_SOBEL,
		FILTER_PREWITT,
		FILTER_GRAYSCALE,
		FILTER_SEPIA,
		FILTER_INVERT,
		FILTER_CUSTOMFILTER,
		FILTER_FXAA,
		FILTER_NUMBER_OF_FILTERS
	};

	RAS_2DFilterManager(bool useFxaa);
	virtual ~RAS_2DFilterManager();

	/** Applies the filters to the scene.
	 * \param rasty The rasterizer used for draw commands.
	 * \param canvas The canvas containing the screen viewport.
	 * \param inputofs The off screen used as input of the first filter.
	 * \param targetofs The off screen used as output of the last filter.
	 * \return The last used off screen, if none filters were rendered it's the
	 * same off screen than inputofs.
	 */
	RAS_OffScreen *RenderFilters(RAS_Rasterizer *rasty, RAS_ICanvas *canvas, RAS_OffScreen *inputofs, RAS_OffScreen *targetofs);

	/// Add a filter to the stack of filters managed by this object.
	RAS_2DFilter *AddFilter(RAS_2DFilterData& filterData);

	/// Removes the filters at a given pass index.
	void RemoveFilterPass(unsigned int passIndex);

	/// Get the existing filter for the given pass index.
	RAS_2DFilter *GetFilterPass(unsigned int passIndex);

private:
	RAS_PassTo2DFilter m_filters;

	/** Creates a filter matching the given filter data. Returns nullptr if no
	 * filter can be created with such information.
	 */
	RAS_2DFilter *CreateFilter(RAS_2DFilterData& filterData);
	/// Only return a new instanced filter.
	virtual RAS_2DFilter *NewFilter(RAS_2DFilterData& filterData) = 0;
};

#endif // __RAS_2DFILTERMANAGER_H__
