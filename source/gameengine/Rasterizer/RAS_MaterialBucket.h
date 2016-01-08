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

#include "RAS_DisplayArrayBucket.h" // needed for RAS_DisplayArrayBucketList
#include "MT_Transform.h"

class RAS_IPolyMaterial;
class RAS_IRasterizer;

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
	bool IsAlpha() const;
	bool IsZSort() const;

	// Rendering
	bool ActivateMaterial(RAS_IRasterizer *rasty);
	void DesactivateMaterial(RAS_IRasterizer *rasty);
	void RenderMeshSlot(const MT_Transform& cameratrans, RAS_IRasterizer *rasty, RAS_MeshSlot *ms);

	// Mesh Slot Access
	RAS_MeshSlotList::iterator msBegin();
	RAS_MeshSlotList::iterator msEnd();

	RAS_MeshSlot *AddMesh(RAS_MeshObject *mesh);
	RAS_MeshSlot *CopyMesh(RAS_MeshSlot *ms);
	void RemoveMesh(RAS_MeshSlot *ms);
	void Optimize(MT_Scalar distance);

	/** Find a display array bucket for the given display array, if not retrurn a new one.
	 * mesh param is used only to create a new display array not to find it.
	 */
	RAS_DisplayArrayBucket *FindDisplayArrayBucket(RAS_DisplayArray *array, RAS_MeshObject *mesh);
	void AddDisplayArrayBucket(RAS_DisplayArrayBucket *bucket);
	void RemoveDisplayArrayBucket(RAS_DisplayArrayBucket *bucket);

	RAS_DisplayArrayBucketList& GetDisplayArrayBucketList();

private:
	RAS_MeshSlotList m_meshSlots; // all the mesh slots
	RAS_IPolyMaterial *m_material;
	RAS_DisplayArrayBucketList m_displayArrayBucketList;

#ifdef WITH_CXX_GUARDEDALLOC
	MEM_CXX_CLASS_ALLOC_FUNCS("GE:RAS_MaterialBucket")
#endif
};

#endif  // __RAS_MATERIAL_BUCKET_H__
