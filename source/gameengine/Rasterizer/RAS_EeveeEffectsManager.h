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
class KX_Scene;
struct DRWShadingGroup;
struct IDProperty;

class RAS_EeveeEffectsManager
{

public:
	RAS_EeveeEffectsManager(EEVEE_Data *vedata, RAS_ICanvas *canvas, IDProperty *props, KX_Scene *scene);
	virtual ~RAS_EeveeEffectsManager();

	RAS_OffScreen *RenderEeveeEffects(RAS_Rasterizer *rasty, RAS_OffScreen *inputofs);

	void InitBloom();
	void InitDof();

	RAS_OffScreen *RenderBloom(RAS_Rasterizer *rasty, RAS_OffScreen *inputofs);
	RAS_OffScreen *RenderMotionBlur(RAS_Rasterizer *rasty, RAS_OffScreen *inputofs);
	RAS_OffScreen *RenderDof(RAS_Rasterizer *rasty, RAS_OffScreen *inputofs);
	RAS_OffScreen *RenderVolumetrics(RAS_Rasterizer *rasty, RAS_OffScreen *inputofs);
	void UpdateAO(RAS_OffScreen *inputofs);

private:
	EEVEE_StorageList *m_stl;
	EEVEE_PassList *m_psl;
	EEVEE_TextureList *m_txl;
	EEVEE_FramebufferList *m_fbl;
	EEVEE_EffectsInfo *m_effects;

	KX_Scene *m_scene; // used for DOF and motion blur

	GPUTexture *m_savedDepth; // used to restore dtxl->depth at ge exit

	RAS_ICanvas *m_canvas; // used to get viewport size
	IDProperty *m_props; // eevee engine properties

	/* eevee effects offscreen targets
	 * std::unique_ptr is used to avoid
	 * to take care about offscreens deletion
	 */
	std::unique_ptr<RAS_OffScreen> m_bloomTarget;
	std::unique_ptr<RAS_OffScreen> m_blurTarget;
	std::unique_ptr<RAS_OffScreen> m_dofTarget;

	float m_shutter; // camera motion blur

	bool m_dofInitialized; // see comment in RenderDof()

	bool m_useAO;
};

#endif // __RAS_EEVEEEFFECTSMANAGER_H__
