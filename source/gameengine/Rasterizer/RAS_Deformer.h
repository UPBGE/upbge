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
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file RAS_Deformer.h
 *  \ingroup bgerast
 */

#ifndef __RAS_DEFORMER_H__
#define __RAS_DEFORMER_H__

#ifdef _MSC_VER
#  pragma warning (disable:4786)  /* get rid of stupid stl-visual compiler debug warning */
#endif

#include "RAS_BoundingBox.h"
#include "RAS_DisplayArray.h" // For RAS_DisplayArrayList.
#include "RAS_DisplayArrayBucket.h" // For RAS_DisplayArrayBucketList.

#include <map>

class RAS_Mesh;
class SCA_IObject;
class RAS_MeshSlot;
class RAS_Rasterizer;

class RAS_Deformer
{
public:
	struct SkinVertData
	{
		float weights[4];
		unsigned char indices[4];
		unsigned char numbones;
	};

	struct SkinShaderData
	{
		const SkinVertData *m_vertData;
		const float *m_boneMatrices;
		unsigned char m_numBones;
	};

	RAS_Deformer(RAS_Mesh *mesh);
	virtual ~RAS_Deformer();

	virtual void Initialize();

	virtual void Apply(RAS_DisplayArray *array) = 0;
	virtual bool Update(void)=0;
	virtual void UpdateBuckets(void)=0;

	// true when deformer produces varying vertex (shape or armature)
	bool IsDynamic()
	{
		return m_bDynamic;
	}

	virtual bool SkipVertexTransform()
	{
		return false;
	}
	virtual bool UseShaderSkinning() const;

	RAS_BoundingBox *GetBoundingBox() const
	{
		return m_boundingBox;
	}

	RAS_Mesh *GetMesh() const;

	RAS_DisplayArray *GetDisplayArray(unsigned short index) const;
	RAS_DisplayArrayBucket *GetDisplayArrayBucket(unsigned short index) const;

	virtual SkinShaderData GetSkinningShaderData(RAS_DisplayArray *array) const;

protected:
	void InitializeDisplayArrays();

	/// Struct wrapping display arrays owned/used by the deformer.
	struct DisplayArraySlot
	{
		/// The unique display array owned by the deformer.
		RAS_DisplayArray *m_displayArray;
		/// The original display array used by the deformer to duplicate data.
		RAS_DisplayArray *m_origDisplayArray;
		/// The mesh material of the owning the original display array.
		RAS_MeshMaterial *m_meshMaterial;
		/// The unique display array bucket using the display array of this deformer.
		RAS_DisplayArrayBucket *m_displayArrayBucket;
		/// Update client of the orignal display array.
		CM_UpdateClient<RAS_DisplayArray> m_arrayUpdateClient;
	};

	std::vector<DisplayArraySlot> m_slots;

	RAS_Mesh *m_mesh;
	bool m_bDynamic;

	/// Deformer bounding box.
	RAS_BoundingBox *m_boundingBox;
};

#endif

