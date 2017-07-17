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
#include "RAS_DisplayArrayStorage.h"
#include "RAS_AttributeArrayStorage.h"
#include "RAS_MaterialBucket.h"
#include "RAS_MaterialShader.h"
#include "RAS_MeshObject.h"
#include "RAS_Deformer.h"
#include "RAS_Rasterizer.h"
#include "RAS_InstancingBuffer.h"
#include "RAS_BucketManager.h"

#include <algorithm>

#ifdef _MSC_VER
#  pragma warning (disable:4786)
#endif

#ifdef WIN32
#  include <windows.h>
#endif // WIN32

RAS_DisplayArrayBucket::RAS_DisplayArrayBucket(RAS_MaterialBucket *bucket, RAS_IDisplayArray *array,
											   RAS_MeshObject *mesh, RAS_MeshMaterial *meshmat, RAS_Deformer *deformer)
	:m_bucket(bucket),
	m_displayArray(array),
	m_mesh(mesh),
	m_meshMaterial(meshmat),
	m_deformer(deformer),
	m_attribArray(nullptr),
	m_instancingBuffer(nullptr)
{
	m_bucket->AddDisplayArrayBucket(this);

	static const std::vector<RAS_RenderNodeDefine<RAS_DisplayArrayDownwardNode> > downwardNodeDefines = {
		{NODE_DOWNWARD_NORMAL, RAS_NODE_FUNC(RAS_DisplayArrayBucket::RunDownwardNode), nullptr},
		{NODE_DOWNWARD_DERIVED_MESH, RAS_NODE_FUNC(RAS_DisplayArrayBucket::RunDownwardNodeDerivedMesh), nullptr},
		{NODE_DOWNWARD_INSTANCING, RAS_NODE_FUNC(RAS_DisplayArrayBucket::RunInstancingNode), nullptr},
		{NODE_DOWNWARD_BATCHING, RAS_NODE_FUNC(RAS_DisplayArrayBucket::RunBatchingNode), nullptr},
	};

	static const std::vector<RAS_RenderNodeDefine<RAS_DisplayArrayUpwardNode> > upwardNodeDefines = {
		{NODE_UPWARD_NORMAL, RAS_NODE_FUNC(RAS_DisplayArrayBucket::BindUpwardNode),
			RAS_NODE_FUNC(RAS_DisplayArrayBucket::UnbindUpwardNode)},
		{NODE_UPWARD_NO_ARRAY, nullptr, nullptr}
	};

	for (const RAS_RenderNodeDefine<RAS_DisplayArrayDownwardNode>& define : downwardNodeDefines) {
		m_downwardNode[define.m_index] = define.Init(this, &m_nodeData);
	}
	for (const RAS_RenderNodeDefine<RAS_DisplayArrayUpwardNode>& define : upwardNodeDefines) {
		m_upwardNode[define.m_index] = define.Init(this, &m_nodeData);
	}

	// Initialize node arguments.
	m_nodeData.m_array = m_displayArray;
	m_nodeData.m_applyMatrix = (!m_deformer || !m_deformer->SkipVertexTransform());
}

RAS_DisplayArrayBucket::~RAS_DisplayArrayBucket()
{
	m_bucket->RemoveDisplayArrayBucket(this);
}

RAS_MaterialBucket *RAS_DisplayArrayBucket::GetBucket() const
{
	return m_bucket;
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

void RAS_DisplayArrayBucket::RemoveActiveMeshSlots()
{
	m_activeMeshSlots.clear();
}

bool RAS_DisplayArrayBucket::UseBatching() const
{
	return (m_displayArray && m_displayArray->GetType() == RAS_IDisplayArray::BATCHING);
}

void RAS_DisplayArrayBucket::UpdateActiveMeshSlots(RAS_Rasterizer *rasty)
{
	bool arrayModified = false;

	if (m_deformer) {
		m_deformer->Apply(m_meshMaterial, m_displayArray);
	}

	if (m_displayArray) {
		if (m_displayArray->GetModifiedFlag() & RAS_IDisplayArray::MESH_MODIFIED) {
			arrayModified = true;
			m_displayArray->SetModifiedFlag(RAS_IDisplayArray::NONE_MODIFIED);
		}

		m_arrayStorage = m_displayArray->GetStorage();

		// Set the storage info modified if the mesh is modified.
		if (arrayModified) {
			m_arrayStorage->UpdateVertexData();
		}

		m_nodeData.m_arrayStorage = m_arrayStorage;
		m_nodeData.m_attribStorage = m_attribArray->GetStorage(rasty->GetDrawingMode());
	}
	else {
		m_nodeData.m_attribStorage = nullptr;
	}
}

void RAS_DisplayArrayBucket::ConstructAttribs()
{
	if (!m_displayArray) {
		return;
	}

	RAS_MaterialShader *shader = m_bucket->GetShader();
	const RAS_MeshObject::LayersInfo& layersInfo = m_mesh->GetLayersInfo();
	const RAS_AttributeArray::AttribList attribList = shader->GetAttribs(layersInfo);

	m_attribArray.reset(new RAS_AttributeArray(attribList, m_displayArray));
}

void RAS_DisplayArrayBucket::GenerateTree(RAS_MaterialDownwardNode& downwardRoot, RAS_MaterialUpwardNode& upwardRoot,
										  RAS_UpwardTreeLeafs& upwardLeafs, const RAS_DisplayArrayNodeTuple& tuple)
{
	if (m_activeMeshSlots.size() == 0) {
		return;
	}

	RAS_ManagerNodeData *managerData = tuple.m_managerData;

	// Update deformer and render settings.
	UpdateActiveMeshSlots(managerData->m_rasty);

	/*if (instancing) {
		downwardRoot.AddChild(&m_downwardNode[NODE_DOWNWARD_INSTANCING]);
	}*/

	if (UseBatching()) {
		downwardRoot.AddChild(&m_downwardNode[NODE_DOWNWARD_BATCHING]);
	}
	else if (managerData->m_sort) {
		RAS_DisplayArrayUpwardNode& upwardNode = m_upwardNode[(m_displayArray) ? NODE_UPWARD_NORMAL : NODE_UPWARD_NO_ARRAY];

		const RAS_MeshSlotNodeTuple msTuple(tuple, &m_nodeData);
		for (RAS_MeshSlot *slot : m_activeMeshSlots) {
			slot->GenerateTree(upwardNode, upwardLeafs, msTuple);
		}

		upwardNode.SetParent(&upwardRoot);
	}
	else {
		downwardRoot.AddChild(&m_downwardNode[(m_displayArray) ? NODE_DOWNWARD_NORMAL : NODE_DOWNWARD_DERIVED_MESH]);
	}
}

void RAS_DisplayArrayBucket::BindUpwardNode(const RAS_DisplayArrayNodeTuple& tuple)
{
	m_nodeData.m_attribStorage->BindPrimitives();
}

void RAS_DisplayArrayBucket::UnbindUpwardNode(const RAS_DisplayArrayNodeTuple& tuple)
{
	m_nodeData.m_attribStorage->UnbindPrimitives();
}

void RAS_DisplayArrayBucket::RunDownwardNode(const RAS_DisplayArrayNodeTuple& tuple)
{
	RAS_AttributeArrayStorage *attribStorage = m_nodeData.m_attribStorage;
	attribStorage->BindPrimitives();

	const RAS_MeshSlotNodeTuple msTuple(tuple, &m_nodeData);
	for (RAS_MeshSlot *ms : m_activeMeshSlots) {
		// Reuse the node function without spend time storing RAS_MeshSlot under nodes.
		ms->RunNodeNormal(msTuple);
	}

	attribStorage->UnbindPrimitives();
}

void RAS_DisplayArrayBucket::RunDownwardNodeDerivedMesh(const RAS_DisplayArrayNodeTuple& tuple)
{
	const RAS_MeshSlotNodeTuple msTuple(tuple, &m_nodeData);
	for (RAS_MeshSlot *ms : m_activeMeshSlots) {
		// Reuse the node function without spend time storing RAS_MeshSlot under nodes.
		ms->RunNodeDerivedMesh(msTuple);
	}
}

void RAS_DisplayArrayBucket::RunDownwardNodeText(const RAS_DisplayArrayNodeTuple& tuple)
{
	const RAS_MeshSlotNodeTuple msTuple(tuple, &m_nodeData);
	for (RAS_MeshSlot *ms : m_activeMeshSlots) {
		// Reuse the node function without spend time storing RAS_MeshSlot under nodes.
		ms->RunNodeText(msTuple);
	}
}

void RAS_DisplayArrayBucket::RunInstancingNode(const RAS_DisplayArrayNodeTuple& tuple)
{
#if 0
	RAS_ManagerNodeData *managerData = tuple.m_managerData;
	RAS_MaterialNodeData *materialData = tuple.m_materialData;
	RAS_Rasterizer *rasty = managerData->m_rasty;

	const unsigned int nummeshslots = m_activeMeshSlots.size(); 

	// Create the instancing buffer only if it needed.
	if (!m_instancingBuffer) {
		m_instancingBuffer.reset(new RAS_InstancingBuffer());
	}

	RAS_IPolyMaterial *material = materialData->m_material;

	// Bind the instancing buffer to work on it.
	m_instancingBuffer->Realloc(nummeshslots);

	/* If the material use the transparency we must sort all mesh slots depending on the distance.
	 * This code share the code used in RAS_BucketManager to do the sort.
	 */
	if (managerData->m_sort) {
		std::vector<RAS_BucketManager::SortedMeshSlot> sortedMeshSlots(nummeshslots);

		const MT_Vector3 pnorm(managerData->m_trans.getBasis()[2]);
		std::transform(m_activeMeshSlots.begin(), m_activeMeshSlots.end(), sortedMeshSlots.end(),
			[&pnorm](RAS_MeshSlot *slot) { return RAS_BucketManager::SortedMeshSlot(slot, pnorm); });

		std::sort(sortedMeshSlots.begin(), sortedMeshSlots.end(), RAS_BucketManager::backtofront());
		RAS_MeshSlotList meshSlots(nummeshslots);
		for (unsigned int i = 0; i < nummeshslots; ++i) {
			meshSlots[i] = sortedMeshSlots[i].m_ms;
		}

		// Fill the buffer with the sorted mesh slots.
		m_instancingBuffer->Update(rasty, materialData->m_drawingMode, meshSlots);
	}
	else {
		// Fill the buffer with the original mesh slots.
		m_instancingBuffer->Update(rasty, materialData->m_drawingMode, m_activeMeshSlots);
	}

	m_instancingBuffer->Bind();

	// Bind all vertex attributs for the used material and the given buffer offset.
	if (managerData->m_overrideShader) {
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

	RAS_AttributeArrayStorage *attribStorage = m_nodeData.m_attribStorage;
	attribStorage->BindPrimitives();

	m_arrayStorage->IndexPrimitivesInstancing(nummeshslots);
	// Unbind vertex attributs.
	if (managerData->m_overrideShader) {
		rasty->DesactivateOverrideShaderInstancing();
	}
	else {
		material->DesactivateInstancing();
	}

	attribStorage->UnbindPrimitives();
#endif
}

void RAS_DisplayArrayBucket::RunBatchingNode(const RAS_DisplayArrayNodeTuple& tuple)
{
#if 0
	RAS_ManagerNodeData *managerData = tuple.m_managerData;
	RAS_MaterialNodeData *materialData = tuple.m_materialData;

	unsigned int nummeshslots = m_activeMeshSlots.size();

	// We must use a int instead of unsigned size to match GLsizei type.
	std::vector<int> counts(nummeshslots);
	std::vector<void *> indices(nummeshslots);

	RAS_IBatchDisplayArray *batchArray = dynamic_cast<RAS_IBatchDisplayArray *>(m_displayArray);

	/* If the material use the transparency we must sort all mesh slots depending on the distance.
	 * This code share the code used in RAS_BucketManager to do the sort.
	 */
	if (managerData->m_sort) {
		std::vector<RAS_BucketManager::SortedMeshSlot> sortedMeshSlots(nummeshslots);

		const MT_Vector3 pnorm(managerData->m_trans.getBasis()[2]);
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

	RAS_Rasterizer *rasty = managerData->m_rasty;

	/* Because the batching use setting for all instances we use the original alpha blend.
	 * This requierd that the user use "alpha blend" mode if he will mutate object color alpha.
	 */
	rasty->SetAlphaBlend(materialData->m_material->GetAlphaBlend());

	/* It's a major issue of the batching : we can't manage face wise per object.
	 * To be sure we don't use the old face wise we force it to true. */
	rasty->SetFrontFace(true);

	RAS_AttributeArrayStorage *attribStorage = m_nodeData.m_attribStorage;
	attribStorage->BindPrimitives();

	m_arrayStorage->IndexPrimitivesBatching(indices, counts);

	attribStorage->UnbindPrimitives();
#endif
}

void RAS_DisplayArrayBucket::ChangeMaterialBucket(RAS_MaterialBucket *bucket)
{
	m_bucket = bucket;

	/// Reconstruct the attributes using the new material.
	ConstructAttribs();
}
