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

class RAS_IMaterial;
class RAS_Rasterizer;

class RAS_MaterialBucket
{
private:
	RAS_IMaterial *m_material;
	RAS_DisplayArrayBucketList m_displayArrayBucketList;

	RAS_MaterialDownwardNode m_downwardNode;
	RAS_MaterialUpwardNode m_upwardNode;
	RAS_MaterialNodeData m_nodeData;

public:
	RAS_MaterialBucket(RAS_IMaterial *mat);
	virtual ~RAS_MaterialBucket();

	// Material Properties
	RAS_IMaterial *GetMaterial() const;

	// Render nodes.
	void GenerateTree(RAS_ManagerDownwardNode& downwardRoot, RAS_ManagerUpwardNode& upwardRoot,
			RAS_UpwardTreeLeafs& upwardLeafs, RAS_Rasterizer *rasty, RAS_Rasterizer::DrawType drawingMode,
			bool sort);
	void BindNode(const RAS_MaterialNodeTuple& tuple);

	void RemoveActiveMeshSlots();

	void AddDisplayArrayBucket(RAS_DisplayArrayBucket *bucket);
	void RemoveDisplayArrayBucket(RAS_DisplayArrayBucket *bucket);

	void MoveDisplayArrayBucket(RAS_MeshMaterial *meshmat, RAS_MaterialBucket *bucket);
};

#endif  // __RAS_MATERIAL_BUCKET_H__
