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

/** \file RAS_EeveeEffectsManager.h
*  \ingroup bgerast
*/

#ifndef __RAS_EEVEEEFFECTSMANAGER_H__
#define __RAS_EEVEEEFFECTSMANAGER_H__

extern "C" {
#  include "eevee_private.h"
}

class RAS_ICanvas;
class RAS_Rasterizer;
class RAS_OffScreen;
struct DRWShadingGroup;

class RAS_EeveeEffectsManager
{
private:
	enum BloomShader {
		BLOOM_FIRST,
		BLOOM_DOWNSAMPLE,
		BLOOM_UPSAMPLE,
		BLOOM_BLIT,
		BLOOM_RESOLVE,
		BLOOM_MAX
	};

	DRWShadingGroup *m_bloomShGroup[BLOOM_MAX];

public:
	RAS_EeveeEffectsManager(EEVEE_Data *vedata, RAS_ICanvas *canvas);
	virtual ~RAS_EeveeEffectsManager();

	/** Applies the filters to the scene.
	* \param rasty The rasterizer used for draw commands.
	* \param canvas The canvas containing the screen viewport.
	* \param inputofs The off screen used as input of the first filter.
	* \param targetofs The off screen used as output of the last filter.
	* \return The last used off screen, if none filters were rendered it's the
	* same off screen than inputofs.
	*/
	RAS_OffScreen *RenderEeveeEffects(RAS_Rasterizer *rasty, RAS_ICanvas *canvas, RAS_OffScreen *inputofs);

	/*/// Add a filter to the stack of filters managed by this object.
	RAS_2DFilter *AddFilter(RAS_2DFilterData& filterData);

	/// Removes the filters at a given pass index.
	void RemoveFilterPass(unsigned int passIndex);

	/// Get the existing filter for the given pass index.
	RAS_2DFilter *GetFilterPass(unsigned int passIndex);*/

	void InitBloomShaders();
	void InitBloom();

private:
	EEVEE_EffectsInfo *m_effectsInfo;
	EEVEE_PassList *m_psl;
	EEVEE_TextureList *m_txl;
	EEVEE_FramebufferList *m_fbl;
	EEVEE_StorageList *m_stl;
	EEVEE_EffectsInfo *m_effects;

	RAS_ICanvas *m_canvas;

	/** Creates a filter matching the given filter data. Returns nullptr if no
	* filter can be created with such information.
	*/
	/*RAS_2DFilter *CreateFilter(RAS_2DFilterData& filterData);
	/// Only return a new instanced filter.
	virtual RAS_2DFilter *NewFilter(RAS_2DFilterData& filterData) = 0;*/
};

#endif // __RAS_EEVEEEFFECTSMANAGER_H__
