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

/** \file RAS_DisplayArrayBucket.h
 *  \ingroup bgerast
 */

#ifndef __RAS_DISPLAY_MATERIAL_BUCKET_H__
#define __RAS_DISPLAY_MATERIAL_BUCKET_H__

#include "CM_RefCount.h"

#include "RAS_MeshSlot.h"
#include "RAS_Rasterizer.h" // needed for RAS_Rasterizer::StorageType and RAS_Rasterizer::AttribLayerList

#include "MT_Transform.h"

#include <vector>

class RAS_MaterialBucket;
class RAS_IDisplayArray;
class RAS_MeshObject;
class RAS_MeshMaterial;
class RAS_Deformer;
class RAS_IStorageInfo;
class RAS_InstancingBuffer;

typedef std::vector<RAS_Deformer *> RAS_DeformerList;

class RAS_DisplayArrayBucket : public CM_RefCount<RAS_DisplayArrayBucket>
{
private:
	/// The parent bucket.
	RAS_MaterialBucket *m_bucket;
	/// The display array = list of vertexes and indexes.
	RAS_IDisplayArray *m_displayArray;
	/// The parent mesh object, it can be nullptr for text objects.
	RAS_MeshObject *m_mesh;
	/// The material mesh.
	RAS_MeshMaterial *m_meshMaterial;
	/// The list fo all visible mesh slots to render this frame.
	RAS_MeshSlotList m_activeMeshSlots;
	/// The list of all deformer usign this display array.
	RAS_DeformerList m_deformerList;

	/// As m_useDisplayList but without rasterizer value.
	bool m_useVao;

	/// True if a deformer is dynamic or the mesh i modified this frame.
	bool m_meshModified;

	/** Info created by the storage and freed by this class.
	 * So it's an unique instance by display array bucket.
	 * It contains all infos about special redering e.g
	 * VBO and IBO ID for VBO storage, DL for VA storage.
	 */
	RAS_IStorageInfo *m_storageInfo;

	/// The vertex buffer object containing all the datas used for the instancing rendering.
	RAS_InstancingBuffer *m_instancingBuffer;

	/// The attribute's layers used by the couple mesh material.
	RAS_Rasterizer::AttribLayerList m_attribLayers;

	RAS_DisplayArrayDownwardNode m_downwardNode;
	RAS_DisplayArrayUpwardNode m_upwardNode;
	RAS_DisplayArrayDownwardNode m_instancingNode;
	RAS_DisplayArrayDownwardNode m_batchingNode;

public:
	RAS_DisplayArrayBucket(RAS_MaterialBucket *bucket, RAS_IDisplayArray *array, RAS_MeshObject *mesh, RAS_MeshMaterial *meshmat);
	~RAS_DisplayArrayBucket();

	/// \section Replication
	RAS_DisplayArrayBucket *GetReplica();
	void ProcessReplica();

	/// \section Accesor
	RAS_IDisplayArray *GetDisplayArray() const;
	RAS_MeshObject *GetMesh() const;
	RAS_MeshMaterial *GetMeshMaterial() const;

	/// \section Active Mesh Slots Management.
	void ActivateMesh(RAS_MeshSlot *slot);
	RAS_MeshSlotList& GetActiveMeshSlots();
	unsigned int GetNumActiveMeshSlots() const;
	/// Remove all mesh slots from the list.
	void RemoveActiveMeshSlots();

	/// \section Deformer
	/// Add a deformer user.
	void AddDeformer(RAS_Deformer *deformer);
	/// Remove the given deformer.
	void RemoveDeformer(RAS_Deformer *deformer);

	/// \section Render Infos
	bool UseVao() const;
	bool UseBatching() const;

	/// Update render infos.
	void UpdateActiveMeshSlots(RAS_Rasterizer *rasty);
	/// Set the mesh object as unmodified flag.
	void SetDisplayArrayUnmodified();
	/// Notice the storage info that the indices list (polygons) changed.
	void SetPolygonsModified(RAS_Rasterizer *rasty);

	RAS_IStorageInfo *GetStorageInfo() const;
	void SetStorageInfo(RAS_IStorageInfo *info);
	void DestructStorageInfo();

	/** Generate the attribute's layers for the used mesh and material couple.
	 * WARNING: Always call when shader in the material are valid.
	 */
	void GenerateAttribLayers();

	void SetAttribLayers(RAS_Rasterizer *rasty) const;

	void GenerateTree(RAS_MaterialDownwardNode *downwardRoot, RAS_MaterialUpwardNode *upwardRoot,
					  RAS_UpwardTreeLeafs *upwardLeafs, RAS_Rasterizer *rasty, bool sort, bool instancing);
	void BindUpwardNode(const RAS_RenderNodeArguments& args);
	void UnbindUpwardNode(const RAS_RenderNodeArguments& args);
	void RunDownwardNode(const RAS_RenderNodeArguments& args);
	void RunInstancingNode(const RAS_RenderNodeArguments& args);
	void RunBatchingNode(const RAS_RenderNodeArguments& args);

	/// Replace the material bucket of this display array bucket by the one given.
	void ChangeMaterialBucket(RAS_MaterialBucket *bucket);
};

typedef std::vector<RAS_DisplayArrayBucket *> RAS_DisplayArrayBucketList;

#endif  // __RAS_DISPLAY_MATERIAL_BUCKET_H__
