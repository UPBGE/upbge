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
 * The Original Code is Copyright (C) Blender Foundation
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file BL_BlenderShader.h
 *  \ingroup ketsji
 */

#ifndef __BL_BLENDERSHADER_H__
#define __BL_BLENDERSHADER_H__

#include "RAS_AttributeArray.h"
#include "RAS_MeshObject.h"
#include "RAS_Texture.h" // for MaxUnits
#include "RAS_MaterialShader.h"
#include <string>

struct Material;
struct Scene;
struct GPUMaterial;
struct DRWShadingGroup;
class KX_Scene;

/**
 * BL_BlenderShader
 * Blender GPU shader material
 */
class BL_BlenderShader : public RAS_MaterialShader
{
private:
	Scene *m_blenderScene;
	Material *m_mat;
	DRWShadingGroup *m_shGroup;
	DRWShadingGroup *m_depthShGroup;
	DRWShadingGroup *m_depthClipShGroup;
	GPUMaterial *m_gpuMat;
	GPUMaterial *m_depthGpuMat;
	GPUMaterial *m_depthClipGpuMat;


public:
	BL_BlenderShader(KX_Scene *scene, Material *ma, int lightlayer);
	virtual ~BL_BlenderShader();

	void ReloadMaterial(KX_Scene *scene);

	GPUMaterial *GetGpuMaterial(RAS_Rasterizer::DrawType drawtype);
	DRWShadingGroup *GetDRWShadingGroup(RAS_Rasterizer::DrawType drawtype);

	void PrintDebugInfos(RAS_Rasterizer::DrawType drawtype);

	virtual bool IsValid(RAS_Rasterizer::DrawType drawtype) const;
	virtual void Activate(RAS_Rasterizer *rasty);
	virtual void Desactivate();
	virtual void Update(RAS_Rasterizer *rasty, RAS_MeshUser *meshUser);
	virtual const RAS_AttributeArray::AttribList GetAttribs(const RAS_MeshObject::LayersInfo& layersInfo) const;

};

#endif // __BL_BLENDERSHADER_H__
