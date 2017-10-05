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
class RAS_FrameBuffer;
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

	RAS_FrameBuffer *RenderEeveeEffects(RAS_FrameBuffer *inputfb);

	void InitDof();

	RAS_FrameBuffer *RenderBloom(RAS_FrameBuffer *inputfb);
	RAS_FrameBuffer *RenderMotionBlur(RAS_FrameBuffer *inputfb);
	RAS_FrameBuffer *RenderDof(RAS_FrameBuffer *inputfb);
	RAS_FrameBuffer *RenderVolumetrics(RAS_FrameBuffer *inputfb);
	void UpdateAO(RAS_FrameBuffer *inputfb);

private:
	EEVEE_StorageList *m_stl;
	EEVEE_PassList *m_psl;
	EEVEE_TextureList *m_txl;
	EEVEE_FramebufferList *m_fbl;
	EEVEE_EffectsInfo *m_effects;

	KX_Scene *m_scene; // used for DOF and motion blur

	RAS_Rasterizer *m_rasterizer; // used to create FrameBuffers
	IDProperty *m_props; // eevee engine properties

	RAS_FrameBuffer *m_bloomTarget;
	RAS_FrameBuffer *m_blurTarget;
	RAS_FrameBuffer *m_dofTarget;

	unsigned int m_width; // Canvas width
	unsigned int m_height; // Canvas height

	float m_shutter; // camera motion blur

	bool m_dofInitialized; // see comment in RenderDof()

	bool m_useAO;

	bool m_useVolumetricNodes; // avoid rendering volumetrics when no background nodes
};

#endif // __RAS_EEVEEEFFECTSMANAGER_H__
