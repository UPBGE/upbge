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

/** \file RAS_MaterialBucket.h
 *  \ingroup bgerast
 */

#ifndef __RAS_MATERIAL_BUCKET_H__
#define __RAS_MATERIAL_BUCKET_H__

#include "RAS_DisplayArrayBucket.h"
#include "MT_Transform.h"

class RAS_IPolyMaterial;
class RAS_MaterialShader;
class RAS_Rasterizer;

/* Contains a list of display arrays with the same material,
 * and a mesh slot for each mesh that uses display arrays in
 * this bucket */

class RAS_MaterialBucket
{
public:
	RAS_MaterialBucket(RAS_IPolyMaterial *mat);
	virtual ~RAS_MaterialBucket();

	// Material Properties
	RAS_IPolyMaterial *GetPolyMaterial() const;
	RAS_MaterialShader *GetShader() const;
	bool IsAlpha() const;
	bool IsZSort() const;
	bool IsWire() const;
	bool UseInstancing() const;

	/// Set the shader after its conversion or when changing to custom shader.
	void UpdateShader();

	// Render nodes.
	void GenerateTree(RAS_ManagerDownwardNode& downwardRoot, RAS_ManagerUpwardNode& upwardRoot,
					  RAS_UpwardTreeLeafs& upwardLeafs, const RAS_MaterialNodeTuple& tuple);
	void BindNode(const RAS_MaterialNodeTuple& tuple);
	void UnbindNode(const RAS_MaterialNodeTuple& tuple);

	void RemoveActiveMeshSlots();

	void AddDisplayArrayBucket(RAS_DisplayArrayBucket *bucket);
	void RemoveDisplayArrayBucket(RAS_DisplayArrayBucket *bucket);

	void MoveDisplayArrayBucket(RAS_MeshMaterial *meshmat, RAS_MaterialBucket *bucket);

private:
	enum NodeType {
		NODE_DOWNWARD_NORMAL = 0,
		NODE_DOWNWARD_TYPE_MAX,

		NODE_UPWARD_NORMAL = 0,
		NODE_UPWARD_TYPE_MAX
	};

	RAS_IPolyMaterial *m_material;
	RAS_MaterialShader *m_shader;
	RAS_DisplayArrayBucketList m_displayArrayBucketList;

	RAS_MaterialNodeData m_nodeData;
	RAS_MaterialDownwardNode m_downwardNode[NODE_DOWNWARD_TYPE_MAX];
	RAS_MaterialUpwardNode m_upwardNode[NODE_UPWARD_TYPE_MAX];
};

#endif  // __RAS_MATERIAL_BUCKET_H__
