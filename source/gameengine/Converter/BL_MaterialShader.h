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

/** \file BL_MaterialShader.h
 *  \ingroup ketsji
 */

#ifndef __BL_MATERIALSHADER_H__
#define __BL_MATERIALSHADER_H__

#include "RAS_IMaterialShader.h"
#include "RAS_Texture.h" // for MaxUnits
#include "RAS_InstancingBuffer.h"

#include <string>

struct Material;
struct Scene;
struct GPUMaterial;
class KX_Scene;
class BL_Material;

/**
 * BL_MaterialShader
 * Blender GPU shader material
 */
class BL_MaterialShader : public RAS_IMaterialShader
{
private:
	/// The scene used for world and lamps.
	Scene *m_blenderScene;
	/// The blender material.
	Material *m_mat;
	/// The material alpah blending.
	int m_alphaBlend;
	/// GPU Material conatining the actual shader.
	GPUMaterial *m_gpuMat;
	/// The material using this material shader.
	BL_Material *m_material;

public:
	BL_MaterialShader(KX_Scene *scene, BL_Material *material, Material *ma, int alphaBlend);
	virtual ~BL_MaterialShader();

	bool Ok() const;

	void ReloadMaterial();

	virtual void Prepare(RAS_Rasterizer *rasty);
	virtual void Activate(RAS_Rasterizer *rasty);
	virtual void Deactivate(RAS_Rasterizer *rasty);
	virtual void ActivateInstancing(RAS_Rasterizer *rasty, RAS_InstancingBuffer *buffer);
	virtual void ActivateMeshUser(RAS_MeshUser *meshUser, RAS_Rasterizer *rasty, const mt::mat3x4& camtrans);

	virtual const RAS_AttributeArray::AttribList GetAttribs(const RAS_Mesh::LayersInfo& layersInfo) const;
	virtual RAS_InstancingBuffer::Attrib GetInstancingAttribs() const;

	void UpdateLights(RAS_Rasterizer *rasty);
	void Update(RAS_MeshUser *meshUser, RAS_Rasterizer *rasty);

	/// Return true if the shader uses a special vertex shader for geometry instancing.
	bool UseInstancing() const;
};

#endif // __BL_MATERIALSHADER_H__
