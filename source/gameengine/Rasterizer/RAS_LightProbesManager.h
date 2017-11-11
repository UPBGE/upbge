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

/** \file RAS_LightProbesManager.h
*  \ingroup bgerast
*/

#ifndef __RAS_LIGHTPROBESMANAGER_H__
#define __RAS_LIGHTPROBESMANAGER_H__

extern "C" {
#  include "eevee_private.h"
}

class RAS_Rasterizer;
class RAS_ICanvas;
class KX_Scene;
struct IDProperty;
struct DefaultTextureList;

class RAS_LightProbesManager
{

public:
	RAS_LightProbesManager(EEVEE_Data *vedata, RAS_ICanvas *canvas, IDProperty *props,
		RAS_Rasterizer *rasty, KX_Scene *scene);
	virtual ~RAS_LightProbesManager();

	void EEVEE_lightprobes_refresh_bge(EEVEE_SceneLayerData *sldata, EEVEE_Data *vedata, KX_Scene *scene);
	void UpdateProbes(KX_Scene *scene);

private:
	EEVEE_StorageList *m_stl;
	EEVEE_PassList *m_psl;
	EEVEE_TextureList *m_txl;
	EEVEE_FramebufferList *m_fbl;
	EEVEE_EffectsInfo *m_effects;
	DefaultTextureList *m_dtxl;
	EEVEE_Data *m_vedata;

	KX_Scene *m_scene; // used for DOF and motion blur

	RAS_Rasterizer *m_rasterizer; // used to create FrameBuffers
	IDProperty *m_props; // eevee engine properties

	unsigned int m_width;
	unsigned int m_height;
};

#endif // __RAS_LIGHTPROBESMANAGER_H__
