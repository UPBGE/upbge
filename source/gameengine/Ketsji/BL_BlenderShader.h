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

struct Material;
struct Scene;
struct GPUMaterial;
class KX_Scene;
class BL_Material;
class RAS_IRasterizer;
class RAS_MeshSlot;

#define BL_MAX_ATTRIB 16

/**
 * BL_BlenderShader
 * Blender GPU shader material
 */
class BL_BlenderShader
{
private:
	Scene *m_blenderScene;
	Material *m_mat;
	BL_Material *m_blMaterial;
	int m_lightLayer;
	int m_alphaBlend;
	GPUMaterial *m_GPUMat;

	/** The cooresponding uv layer index to the given attribut index.
	 * If the attribut doesn't use UV info it will return -1. */
	int m_uvLayers[BL_MAX_ATTRIB];

	void ParseAttribs();

public:
	BL_BlenderShader(KX_Scene *scene, Material *ma, BL_Material *blmat, int lightlayer);
	virtual ~BL_BlenderShader();

	bool Ok() const
	{
		return (m_GPUMat != NULL);
	}
	void SetProg(bool enable, double time = 0.0, RAS_IRasterizer *rasty = NULL);

	int GetAttribNum();
	void SetAttribs(RAS_IRasterizer *ras);
	void Update(RAS_MeshSlot *ms, RAS_IRasterizer * rasty);

	/**
	 * retrun true if the shader uses a special vertex program for geometry instancing
	 */
	bool UseInstancing() const;
	void ActivateInstancing(void *matrixoffset, void *positionoffset, void *coloroffset, unsigned int stride);
	void DesactivateInstancing();

	void ReloadMaterial();
	int GetAlphaBlend();

#ifdef WITH_CXX_GUARDEDALLOC
	MEM_CXX_CLASS_ALLOC_FUNCS("GE:BL_BlenderShader")
#endif
};

#endif /* __BL_BLENDERSHADER_H__ */
