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

/** \file gameengine/Rasterizer/RAS_DisplayArrayBucket.cpp
 *  \ingroup bgerast
 */

#include "RAS_DisplayArrayBucket.h"
#include "RAS_BatchDisplayArray.h"
#include "RAS_MaterialBucket.h"
#include "RAS_IPolygonMaterial.h"
#include "RAS_MeshObject.h"
#include "RAS_Deformer.h"
#include "RAS_Rasterizer.h"
#include "RAS_IStorageInfo.h"
#include "RAS_InstancingBuffer.h"
#include "RAS_BucketManager.h"

#include <algorithm>

#ifdef _MSC_VER
#  pragma warning (disable:4786)
#endif

#ifdef WIN32
#  include <windows.h>
#endif // WIN32

RAS_DisplayArrayBucket::RAS_DisplayArrayBucket(RAS_MaterialBucket *bucket, RAS_IDisplayArray *array, RAS_MeshObject *mesh, RAS_MeshMaterial *meshmat)
	:m_bucket(bucket),
	m_displayArray(array),
	m_mesh(mesh),
	m_meshMaterial(meshmat),
	m_useVao(true),
	m_storageInfo(nullptr),
	m_instancingBuffer(nullptr),
	m_downwardNode(this, std::mem_fn(&RAS_DisplayArrayBucket::RunDownwardNode), nullptr),
	m_upwardNode(this, std::mem_fn(&RAS_DisplayArrayBucket::BindUpwardNode), std::mem_fn(&RAS_DisplayArrayBucket::UnbindUpwardNode)),
	m_instancingNode(this, std::mem_fn(&RAS_DisplayArrayBucket::RunInstancingNode), nullptr),
	m_batchingNode(this, std::mem_fn(&RAS_DisplayArrayBucket::RunBatchingNode), nullptr)
{
	m_bucket->AddDisplayArrayBucket(this);
}

RAS_DisplayArrayBucket::~RAS_DisplayArrayBucket()
{
	m_bucket->RemoveDisplayArrayBucket(this);
	DestructStorageInfo();

	if (m_instancingBuffer) {
		delete m_instancingBuffer;
	}

	if (m_displayArray) {
		delete m_displayArray;
	}
}

RAS_DisplayArrayBucket *RAS_DisplayArrayBucket::GetReplica()
{
	RAS_DisplayArrayBucket *replica = new RAS_DisplayArrayBucket(*this);
	replica->ProcessReplica();
	return replica;
}

void RAS_DisplayArrayBucket::ProcessReplica()
{
	m_activeMeshSlots.clear();
	if (m_displayArray) {
		m_displayArray = m_displayArray->GetReplica();
	}

	m_downwardNode = RAS_DisplayArrayDownwardNode(this, std::mem_fn(&RAS_DisplayArrayBucket::RunDownwardNode), nullptr);
	m_upwardNode = RAS_DisplayArrayUpwardNode(this, std::mem_fn(&RAS_DisplayArrayBucket::BindUpwardNode), std::mem_fn(&RAS_DisplayArrayBucket::UnbindUpwardNode));
	m_instancingNode = RAS_DisplayArrayDownwardNode(this, std::mem_fn(&RAS_DisplayArrayBucket::RunInstancingNode), nullptr);
	m_batchingNode = RAS_DisplayArrayDownwardNode(this, std::mem_fn(&RAS_DisplayArrayBucket::RunBatchingNode), nullptr);

	m_bucket->AddDisplayArrayBucket(this);
}

RAS_IDisplayArray *RAS_DisplayArrayBucket::GetDisplayArray() const
{
	return m_displayArray;
}

RAS_MeshObject *RAS_DisplayArrayBucket::GetMesh() const
{
	return m_mesh;
}

RAS_MeshMaterial *RAS_DisplayArrayBucket::GetMeshMaterial() const
{
	return m_meshMaterial;
}

void RAS_DisplayArrayBucket::ActivateMesh(RAS_MeshSlot *slot)
{
	m_activeMeshSlots.push_back(slot);
}

RAS_MeshSlotList& RAS_DisplayArrayBucket::GetActiveMeshSlots()
{
	return m_activeMeshSlots;
}

void RAS_DisplayArrayBucket::RemoveActiveMeshSlots()
{
	m_activeMeshSlots.clear();
}

unsigned int RAS_DisplayArrayBucket::GetNumActiveMeshSlots() const
{
	return m_activeMeshSlots.size();
}

void RAS_DisplayArrayBucket::AddDeformer(RAS_Deformer *deformer)
{
	m_deformerList.push_back(deformer);
}

void RAS_DisplayArrayBucket::RemoveDeformer(RAS_Deformer *deformer)
{
	RAS_DeformerList::iterator it = std::find(m_deformerList.begin(), m_deformerList.end(), deformer);
	if (it != m_deformerList.end()) {
		m_deformerList.erase(it);
	}
}

bool RAS_DisplayArrayBucket::UseVao() const
{
	return m_useVao;
}

bool RAS_DisplayArrayBucket::UseBatching() const
{
	return (m_displayArray && m_displayArray->GetType() == RAS_IDisplayArray::BATCHING);
}

void RAS_DisplayArrayBucket::UpdateActiveMeshSlots(RAS_Rasterizer *rasty)
{
	// Reset values to default.
	m_useVao = true;
	bool arrayModified = false;

	RAS_IPolyMaterial *material = m_bucket->GetPolyMaterial();

	if (material->IsZSort() || m_bucket->UseInstancing() || !m_displayArray || material->UsesObjectColor()) {
		m_useVao = false;
	}

	for (RAS_Deformer *deformer : m_deformerList) {
		deformer->Apply(material, m_meshMaterial);

		// Test if one of deformers is dynamic.
		if (deformer->IsDynamic()) {
			arrayModified = true;
		}
	}

	if (m_displayArray && m_displayArray->GetModifiedFlag() & RAS_IDisplayArray::MESH_MODIFIED) {
		arrayModified = true;
	}

	// Set the storage info modified if the mesh is modified.
	if (arrayModified && m_storageInfo) {
		m_storageInfo->SetDataModified(rasty->GetDrawingMode(), RAS_IStorageInfo::VERTEX_DATA);
	}
}

void RAS_DisplayArrayBucket::SetDisplayArrayUnmodified()
{
	if (m_displayArray) {
		m_displayArray->SetModifiedFlag(RAS_IDisplayArray::NONE_MODIFIED);
	}
}

void RAS_DisplayArrayBucket::SetPolygonsModified(RAS_Rasterizer *rasty)
{
	if (m_storageInfo) {
		m_storageInfo->SetDataModified(rasty->GetDrawingMode(), RAS_IStorageInfo::INDEX_DATA);
	}
}

RAS_IStorageInfo *RAS_DisplayArrayBucket::GetStorageInfo() const
{
	return m_storageInfo;
}

void RAS_DisplayArrayBucket::SetStorageInfo(RAS_IStorageInfo *info)
{
	m_storageInfo = info;
}

void RAS_DisplayArrayBucket::DestructStorageInfo()
{
	if (m_storageInfo) {
		delete m_storageInfo;
		m_storageInfo = nullptr;
	}
}

void RAS_DisplayArrayBucket::GenerateAttribLayers()
{
	if (!m_mesh) {
		return;
	}

	RAS_IPolyMaterial *polymat = m_bucket->GetPolyMaterial();
	const RAS_MeshObject::LayersInfo& layersInfo = m_mesh->GetLayersInfo();
	m_attribLayers = polymat->GetAttribLayers(layersInfo);
}

void RAS_DisplayArrayBucket::SetAttribLayers(RAS_Rasterizer *rasty) const
{
	rasty->SetAttribLayers(m_attribLayers);
}

void RAS_DisplayArrayBucket::GenerateTree(RAS_MaterialDownwardNode *downwardRoot, RAS_MaterialUpwardNode *upwardRoot,
										  RAS_UpwardTreeLeafs *upwardLeafs, RAS_Rasterizer *rasty, bool sort, bool instancing)
{
	if (m_activeMeshSlots.size() == 0) {
		return;
	}

	// Update deformer and render settings.
	UpdateActiveMeshSlots(rasty);

	if (instancing) {
		downwardRoot->AddChild(&m_instancingNode);
	}
	else if (UseBatching()) {
		downwardRoot->AddChild(&m_batchingNode);
	}
	else if (sort) {
		for (RAS_MeshSlot *slot : m_activeMeshSlots) {
			slot->GenerateTree(&m_upwardNode, upwardLeafs);
		}

		m_upwardNode.SetParent(upwardRoot);
	}
	else {
		downwardRoot->AddChild(&m_downwardNode);
	}
}

void RAS_DisplayArrayBucket::BindUpwardNode(const RAS_RenderNodeArguments& args)
{
	args.m_rasty->BindPrimitives(this);
}

void RAS_DisplayArrayBucket::UnbindUpwardNode(const RAS_RenderNodeArguments& args)
{
	args.m_rasty->UnbindPrimitives(this);
}

void RAS_DisplayArrayBucket::RunDownwardNode(const RAS_RenderNodeArguments& args)
{
	RAS_Rasterizer *rasty = args.m_rasty;
	rasty->BindPrimitives(this);

	for (RAS_MeshSlot *ms : m_activeMeshSlots) {
		// Reuse the node function without spend time storing RAS_MeshSlot under nodes.
		ms->RunNode(args);
	}

	rasty->UnbindPrimitives(this);
}

void RAS_DisplayArrayBucket::RunInstancingNode(const RAS_RenderNodeArguments& args)
{
	const unsigned int nummeshslots = m_activeMeshSlots.size(); 

	// Create the instancing buffer only if it needed.
	if (!m_instancingBuffer) {
		m_instancingBuffer = new RAS_InstancingBuffer();
	}

	RAS_Rasterizer *rasty = args.m_rasty;
	RAS_IPolyMaterial *material = m_bucket->GetPolyMaterial();

	// Bind the instancing buffer to work on it.
	m_instancingBuffer->Realloc(nummeshslots);

	/* If the material use the transparency we must sort all mesh slots depending on the distance.
	 * This code share the code used in RAS_BucketManager to do the sort.
	 */
	if (args.m_sort) {
		std::vector<RAS_BucketManager::SortedMeshSlot> sortedMeshSlots(nummeshslots);

		const MT_Vector3 pnorm(args.m_trans.getBasis()[2]);
		std::transform(m_activeMeshSlots.begin(), m_activeMeshSlots.end(), sortedMeshSlots.end(),
			[&pnorm](RAS_MeshSlot *slot) { return RAS_BucketManager::SortedMeshSlot(slot, pnorm); });

		std::sort(sortedMeshSlots.begin(), sortedMeshSlots.end(), RAS_BucketManager::backtofront());
		RAS_MeshSlotList meshSlots(nummeshslots);
		for (unsigned int i = 0; i < nummeshslots; ++i) {
			meshSlots[i] = sortedMeshSlots[i].m_ms;
		}

		// Fill the buffer with the sorted mesh slots.
		m_instancingBuffer->Update(rasty, material->GetDrawingMode(), meshSlots);
	}
	else {
		// Fill the buffer with the original mesh slots.
		m_instancingBuffer->Update(rasty, material->GetDrawingMode(), m_activeMeshSlots);
	}

	m_instancingBuffer->Bind();

	// Bind all vertex attributs for the used material and the given buffer offset.
	if (args.m_shaderOverride) {
		rasty->ActivateOverrideShaderInstancing(
			m_instancingBuffer->GetMatrixOffset(),
			m_instancingBuffer->GetPositionOffset(),
			m_instancingBuffer->GetStride());
	}
	else {
		material->ActivateInstancing(
			rasty,
			m_instancingBuffer->GetMatrixOffset(),
			m_instancingBuffer->GetPositionOffset(),
			m_instancingBuffer->GetColorOffset(),
			m_instancingBuffer->GetStride());
	}

	/* Because the geometry instancing use setting for all instances we use the original alpha blend.
	 * This requierd that the user use "alpha blend" mode if he will mutate object color alpha.
	 */
	rasty->SetAlphaBlend(material->GetAlphaBlend());

	/* It's a major issue of the geometry instancing : we can't manage face wise.
	 * To be sure we don't use the old face wise we force it to true. */
	rasty->SetFrontFace(true);

	// Unbind the buffer to avoid conflict with the render after.
	m_instancingBuffer->Unbind();

	rasty->BindPrimitives(this);

	rasty->IndexPrimitivesInstancing(this);
	// Unbind vertex attributs.
	if (args.m_shaderOverride) {
		rasty->DesactivateOverrideShaderInstancing();
	}
	else {
		material->DesactivateInstancing();
	}

	rasty->UnbindPrimitives(this);
}

void RAS_DisplayArrayBucket::RunBatchingNode(const RAS_RenderNodeArguments& args)
{
	unsigned int nummeshslots = m_activeMeshSlots.size();

	// We must use a int instead of unsigned size to match GLsizei type.
	std::vector<int> counts(nummeshslots);
	std::vector<void *> indices(nummeshslots);

	RAS_IBatchDisplayArray *batchArray = dynamic_cast<RAS_IBatchDisplayArray *>(m_displayArray);

	/* If the material use the transparency we must sort all mesh slots depending on the distance.
	 * This code share the code used in RAS_BucketManager to do the sort.
	 */
	if (args.m_sort) {
		std::vector<RAS_BucketManager::SortedMeshSlot> sortedMeshSlots(nummeshslots);

		const MT_Vector3 pnorm(args.m_trans.getBasis()[2]);
		std::transform(m_activeMeshSlots.begin(), m_activeMeshSlots.end(), sortedMeshSlots.begin(),
					   [&pnorm](RAS_MeshSlot *slot) { return RAS_BucketManager::SortedMeshSlot(slot, pnorm); });

		std::sort(sortedMeshSlots.begin(), sortedMeshSlots.end(), RAS_BucketManager::backtofront());
		for (unsigned int i = 0; i < nummeshslots; ++i) {
			const short index = sortedMeshSlots[i].m_ms->m_batchPartIndex;
			indices[i] = batchArray->GetPartIndexOffset(index);
			counts[i] = batchArray->GetPartIndexCount(index);
		}
	}
	else {
		for (unsigned int i = 0; i < nummeshslots; ++i) {
			const short index = m_activeMeshSlots[i]->m_batchPartIndex;
			indices[i] = batchArray->GetPartIndexOffset(index);
			counts[i] = batchArray->GetPartIndexCount(index);
		}
	}

	RAS_Rasterizer *rasty = args.m_rasty;
	RAS_IPolyMaterial *material = m_bucket->GetPolyMaterial();

	/* Because the batching use setting for all instances we use the original alpha blend.
	 * This requierd that the user use "alpha blend" mode if he will mutate object color alpha.
	 */
	rasty->SetAlphaBlend(material->GetAlphaBlend());

	/* It's a major issue of the batching : we can't manage face wise per object.
	 * To be sure we don't use the old face wise we force it to true. */
	rasty->SetFrontFace(true);

	rasty->BindPrimitives(this);

	rasty->IndexPrimitivesBatching(this, indices, counts);

	rasty->UnbindPrimitives(this);
}

void RAS_DisplayArrayBucket::ChangeMaterialBucket(RAS_MaterialBucket *bucket)
{
	m_bucket = bucket;

	/// Regenerate the attribute's layers using the new material.
	GenerateAttribLayers();

	/// Free the storage info because the attribute's layer may changed.
	DestructStorageInfo();
}
