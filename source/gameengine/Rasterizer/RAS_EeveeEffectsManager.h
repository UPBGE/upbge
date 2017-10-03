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
struct GPUFrameBuffer;
struct GPUTexture;
class KX_Scene;
struct DRWShadingGroup;
struct IDProperty;

class RAS_EeveeEffectsManager
{

public:
	RAS_EeveeEffectsManager(EEVEE_Data *vedata, RAS_ICanvas *canvas, IDProperty *props,
		RAS_Rasterizer *rasty, KX_Scene *scene);
	virtual ~RAS_EeveeEffectsManager();

	/* FrameBuffers can't be initialized before the canvas size is available.
	 * With the current launching process, canvas size is available after
	 * the effects manager is created. So we can't initialize effects framebuffers
	 * in effects manager constructor.
	 * I'll initialize it in RenderEeveeEffects for now.
	 */
	void InitFrameBuffers();
	bool m_frameBuffersInitialized;

	GPUFrameBuffer *RenderEeveeEffects(GPUFrameBuffer *inputfb);

	void InitDof();

	GPUFrameBuffer *RenderBloom(GPUFrameBuffer *inputfb);
	GPUFrameBuffer *RenderMotionBlur(GPUFrameBuffer *inputfb);
	GPUFrameBuffer *RenderDof(GPUFrameBuffer *inputfb);
	GPUFrameBuffer *RenderVolumetrics(GPUFrameBuffer *inputfb);
	void UpdateAO(GPUFrameBuffer *inputfb);

private:
	EEVEE_StorageList *m_stl;
	EEVEE_PassList *m_psl;
	EEVEE_TextureList *m_txl;
	EEVEE_FramebufferList *m_fbl;
	EEVEE_EffectsInfo *m_effects;

	KX_Scene *m_scene; // used for DOF and motion blur

	RAS_ICanvas *m_canvas; // used to get viewport size
	RAS_Rasterizer *m_rasterizer; // used to create FrameBuffers
	IDProperty *m_props; // eevee engine properties

	GPUFrameBuffer *m_bloomTarget;
	GPUFrameBuffer *m_blurTarget;
	GPUFrameBuffer *m_dofTarget;

	float m_shutter; // camera motion blur

	bool m_dofInitialized; // see comment in RenderDof()

	bool m_useAO;

	bool m_useVolumetricNodes; // avoid rendering volumetrics when no background nodes
};

#endif // __RAS_EEVEEEFFECTSMANAGER_H__
