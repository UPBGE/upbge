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

/** \file RAS_MeshMaterial.h
 *  \ingroup bgerast
 */

#ifndef __RAS_MESH_MATERIAL_H__
#define __RAS_MESH_MATERIAL_H__

#include "RAS_DisplayArray.h"

#include <vector>

class RAS_Mesh;
class RAS_DisplayArrayBucket;
class RAS_MaterialBucket;

/** \brief Node between material and mesh.
 * Own the display array and the display array bucket used to draw the part of the mesh
 * with the bucket material.
 */
class RAS_MeshMaterial
{
private:
	RAS_MaterialBucket *m_bucket;
	/// The blender material index position in the mesh.
	unsigned int m_index;

	RAS_DisplayArray *m_displayArray;
	RAS_DisplayArrayBucket *m_displayArrayBucket;

public:
	RAS_MeshMaterial(RAS_Mesh *mesh, RAS_MaterialBucket *bucket, unsigned int index, const RAS_DisplayArray::Format& format);
	/** Copy mesh material for a given mesh object.
	 * \param mesh Owner of copied mesh material.
	 */
	RAS_MeshMaterial(const RAS_MeshMaterial& other, RAS_Mesh *mesh);
	RAS_MeshMaterial(RAS_Mesh *mesh, RAS_MaterialBucket *bucket, unsigned int index, RAS_DisplayArray *array);
	~RAS_MeshMaterial();


	unsigned int GetIndex() const;
	RAS_MaterialBucket *GetBucket() const;
	RAS_DisplayArray *GetDisplayArray() const;
	RAS_DisplayArrayBucket *GetDisplayArrayBucket() const;

	void ReplaceMaterial(RAS_MaterialBucket *bucket);
};

using RAS_MeshMaterialList = std::vector<RAS_MeshMaterial *>;

#endif  // __RAS_MESH_MATERIAL_H__
