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
#include "RAS_IMaterial.h"
#include "RAS_IMaterialShader.h"
#include "RAS_Mesh.h"
#include "RAS_MeshUser.h"
#include "RAS_Deformer.h"
#include "RAS_BatchGroup.h"
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

RAS_DisplayArrayBucket::RAS_DisplayArrayBucket(RAS_MaterialBucket *bucket, RAS_DisplayArray *array,
                                               RAS_Mesh *mesh, RAS_MeshMaterial *meshmat, RAS_Deformer *deformer)
	:m_bucket(bucket),
	m_displayArray(array),
	m_mesh(mesh),
	m_meshMaterial(meshmat),
	m_deformer(deformer),
	m_arrayStorage(nullptr),
	m_materialUpdateClient(RAS_IMaterial::ATTRIBUTES_MODIFIED, RAS_IMaterial::ATTRIBUTES_MODIFIED),
	m_arrayUpdateClient(RAS_DisplayArray::ANY_MODIFIED, RAS_DisplayArray::STORAGE_INVALID),
	m_instancingNode(this, &m_nodeData, &RAS_DisplayArrayBucket::RunInstancingNode, nullptr),
	m_batchingNode(this, &m_nodeData, &RAS_DisplayArrayBucket::RunBatchingNode, nullptr)
{
	m_bucket->AddDisplayArrayBucket(this);

	// Display array can be null in case of text.
	if (m_displayArray) {
		m_downwardNode = RAS_DisplayArrayDownwardNode(this, &m_nodeData, &RAS_DisplayArrayBucket::RunDownwardNode, nullptr);
		m_upwardNode = RAS_DisplayArrayUpwardNode(this, &m_nodeData, &RAS_DisplayArrayBucket::BindUpwardNode,
		                                          &RAS_DisplayArrayBucket::UnbindUpwardNode);

		m_arrayStorage = &m_displayArray->GetStorage();
		m_displayArray->AddUpdateClient(&m_arrayUpdateClient);
	}
	else {
		// If there's no display array then we draw using text, in this case the display array bind/unbind should be avoid.
		m_downwardNode = RAS_DisplayArrayDownwardNode(this, &m_nodeData, &RAS_DisplayArrayBucket::RunDownwardNodeNoArray, nullptr);
		m_upwardNode = RAS_DisplayArrayUpwardNode(this, &m_nodeData, nullptr, nullptr);
	}

	// Initialize node arguments.
	m_nodeData.m_array = m_displayArray;
	m_nodeData.m_arrayStorage = m_arrayStorage;
	m_nodeData.m_attribStorage = nullptr;
	m_nodeData.m_applyMatrix = (!m_deformer || !m_deformer->SkipVertexTransform());

	RAS_IMaterial *material = bucket->GetMaterial();
	material->AddUpdateClient(&m_materialUpdateClient);
}

RAS_DisplayArrayBucket::~RAS_DisplayArrayBucket()
{
	m_bucket->RemoveDisplayArrayBucket(this);
}

RAS_MaterialBucket *RAS_DisplayArrayBucket::GetBucket() const
{
	return m_bucket;
}

RAS_DisplayArray *RAS_DisplayArrayBucket::GetDisplayArray() const
{
	return m_displayArray;
}

RAS_Mesh *RAS_DisplayArrayBucket::GetMesh() const
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
	return (m_displayArray && m_displayArray->GetType() == RAS_DisplayArray::BATCHING);
}

void RAS_DisplayArrayBucket::UpdateActiveMeshSlots(RAS_Rasterizer::DrawType drawingMode, RAS_IMaterialShader::GeomType geomMode)
{
	if (m_deformer) {
		m_deformer->Apply(m_displayArray);
	}

	if (m_displayArray) {
		const unsigned int modifiedFlag = m_arrayUpdateClient.GetInvalidAndClear();
		if (modifiedFlag != RAS_DisplayArray::NONE_MODIFIED) {
			if (modifiedFlag & RAS_DisplayArray::STORAGE_INVALID) {
				m_displayArray->ConstructStorage();
			}
			else if (modifiedFlag & RAS_DisplayArray::SIZE_MODIFIED) {
				m_arrayStorage->UpdateSize();
			}
			// Set the display array storage modified if the mesh is modified.
			else if (modifiedFlag & RAS_DisplayArray::MESH_MODIFIED) {
				m_arrayStorage->UpdateVertexData(modifiedFlag);
			}

			if (modifiedFlag & RAS_DisplayArray::POSITION_MODIFIED) {
				// Reset polygons center cache to ask update.
				m_displayArray->InvalidatePolygonCenters();
			}
		}

		/* Recreate the attribute (linking the shaders to a mesh) when the material changed
		 * or the mesh (display array) was resized.
		 */
		if (m_materialUpdateClient.GetInvalidAndClear() || modifiedFlag & RAS_DisplayArray::SIZE_MODIFIED) {
			RAS_IMaterial *mat = m_bucket->GetMaterial();
			const RAS_Mesh::LayersInfo& layersInfo = m_mesh->GetLayersInfo();

			// Construct the attribute array for all shader used by the material.
			for (unsigned short i = 0; i < RAS_Rasterizer::RAS_DRAW_MAX; ++i) {
				RAS_IMaterialShader *shader = mat->GetShader((RAS_Rasterizer::DrawType)i);
				const RAS_AttributeArray::AttribList attribList = shader->GetAttribs(layersInfo);
				m_attribArrays[i] = RAS_AttributeArray(attribList, m_displayArray);
			}
		}

		m_nodeData.m_attribStorage = m_attribArrays[drawingMode].GetStorage();
	}

	if (geomMode == RAS_IMaterialShader::GEOM_INSTANCING) {
		// Create the instancing buffer only if needed.
		if (!m_instancingBuffer[drawingMode]) {
			RAS_IMaterial *mat = m_bucket->GetMaterial();
			RAS_IMaterialShader *shader = mat->GetShader(drawingMode);
			m_instancingBuffer[drawingMode].reset(new RAS_InstancingBuffer(shader->GetInstancingAttribs()));
		}

		m_nodeData.m_instancingBuffer = m_instancingBuffer[drawingMode].get();
	}
}

void RAS_DisplayArrayBucket::GenerateTree(RAS_MaterialDownwardNode& downwardRoot, RAS_MaterialUpwardNode& upwardRoot,
		RAS_UpwardTreeLeafs& upwardLeafs, RAS_Rasterizer::DrawType drawingMode, bool sort,
		RAS_IMaterialShader::GeomType geomMode)
{
	if (m_activeMeshSlots.empty()) {
		return;
	}

	// Update deformer and render settings.
	UpdateActiveMeshSlots(drawingMode, geomMode);

	if (geomMode == RAS_IMaterialShader::GEOM_INSTANCING) {
		downwardRoot.AddChild(&m_instancingNode);
	}
	else if (UseBatching()) {
		downwardRoot.AddChild(&m_batchingNode);
	}
	else if (sort) {
		for (RAS_MeshSlot *slot : m_activeMeshSlots) {
			slot->GenerateTree(m_upwardNode, upwardLeafs);
		}

		m_upwardNode.SetParent(&upwardRoot);
	}
	else {
		downwardRoot.AddChild(&m_downwardNode);
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
		ms->RunNode(msTuple);
	}

	attribStorage->UnbindPrimitives();
}

void RAS_DisplayArrayBucket::RunDownwardNodeNoArray(const RAS_DisplayArrayNodeTuple& tuple)
{
	const RAS_MeshSlotNodeTuple msTuple(tuple, &m_nodeData);
	for (RAS_MeshSlot *ms : m_activeMeshSlots) {
		// Reuse the node function without spend time storing RAS_MeshSlot under nodes.
		ms->RunNode(msTuple);
	}
}

void RAS_DisplayArrayBucket::RunInstancingNode(const RAS_DisplayArrayNodeTuple& tuple)
{
	RAS_ManagerNodeData *managerData = tuple.m_managerData;
	RAS_ShaderNodeData *shaderData = tuple.m_shaderData;
	RAS_MaterialNodeData *materialData = tuple.m_materialData;
	RAS_Rasterizer *rasty = managerData->m_rasty;

	const unsigned int nummeshslots = m_activeMeshSlots.size();

	RAS_IMaterialShader *shader = shaderData->m_shader;
	RAS_InstancingBuffer *buffer = m_nodeData.m_instancingBuffer;

	// Bind the instancing buffer to work on it.
	buffer->Realloc(nummeshslots);

	/* If the material use the transparency we must sort all mesh slots depending on the distance.
	 * This code share the code used in RAS_BucketManager to do the sort.
	 */
	if (managerData->m_sort) {
		std::vector<RAS_BucketManager::SortedMeshSlot> sortedMeshSlots(nummeshslots);

		const mt::mat3x4& trans = managerData->m_trans;
		const mt::vec3 pnorm(trans[2], trans[5], trans[8]);
		std::transform(m_activeMeshSlots.begin(), m_activeMeshSlots.end(), sortedMeshSlots.begin(),
		               [&pnorm](RAS_MeshSlot *slot) {
			return RAS_BucketManager::SortedMeshSlot(slot, pnorm);
		});

		std::sort(sortedMeshSlots.begin(), sortedMeshSlots.end(), RAS_BucketManager::backtofront());
		RAS_MeshSlotList meshSlots(nummeshslots);
		for (unsigned int i = 0; i < nummeshslots; ++i) {
			meshSlots[i] = sortedMeshSlots[i].m_ms;
		}

		// Fill the buffer with the sorted mesh slots.
		buffer->Update(rasty, materialData->m_drawingMode, meshSlots);
	}
	else {
		// Fill the buffer with the original mesh slots.
		buffer->Update(rasty, materialData->m_drawingMode, m_activeMeshSlots);
	}

	RAS_AttributeArrayStorage *attribStorage = m_nodeData.m_attribStorage;
	// Make sure to bind the VAO before instancing attributes to not clear them.
	attribStorage->BindPrimitives();

	buffer->Bind();

	// Bind all vertex attributs for the used material and the given buffer offset.
	shader->ActivateInstancing(rasty, buffer);

	/* It's a major issue of the geometry instancing : we can't manage face wise.
	 * To be sure we don't use the old face wise we force it to true. */
	rasty->SetFrontFace(true);

	// Unbind the buffer to avoid conflict with the render after.
	buffer->Unbind();

	m_arrayStorage->IndexPrimitivesInstancing(nummeshslots);

	// Unbind attributes, both array attributes and instancing attributes.
	attribStorage->UnbindPrimitives();
}

void RAS_DisplayArrayBucket::RunBatchingNode(const RAS_DisplayArrayNodeTuple& tuple)
{
	RAS_ManagerNodeData *managerData = tuple.m_managerData;
	RAS_ShaderNodeData *shaderData = tuple.m_shaderData;

	RAS_IMaterialShader *shader = shaderData->m_shader;
	const unsigned int nummeshslots = m_activeMeshSlots.size();

	// We must use a int instead of unsigned size to match GLsizei type.
	std::vector<int> counts(nummeshslots);
	std::vector<intptr_t> indices(nummeshslots);

	RAS_BatchDisplayArray *batchArray = static_cast<RAS_BatchDisplayArray *>(m_displayArray);

	/* If the material use the transparency we must sort all mesh slots depending on the distance.
	 * This code share the code used in RAS_BucketManager to do the sort.
	 */
	if (managerData->m_sort) {
		std::vector<RAS_BucketManager::SortedMeshSlot> sortedMeshSlots(nummeshslots);

		const mt::mat3x4& trans = managerData->m_trans;
		const mt::vec3 pnorm(trans[2], trans[5], trans[8]);
		std::transform(m_activeMeshSlots.begin(), m_activeMeshSlots.end(), sortedMeshSlots.begin(),
		               [&pnorm](RAS_MeshSlot *slot) {
			return RAS_BucketManager::SortedMeshSlot(slot, pnorm);
		});

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

	/* It's a major issue of the batching : we can't manage face wise per object.
	 * To be sure we don't use the old face wise we force it to true. */
	rasty->SetFrontFace(true);

	// Retrieve batch group from first active mesh slot.
	RAS_BatchGroup *group = m_activeMeshSlots.front()->m_meshUser->GetBatchGroup();
	// Use batch group reference mesh user for layer and object color.
	shader->ActivateMeshUser(group->GetReferenceMeshUser(), rasty, managerData->m_trans);

	RAS_AttributeArrayStorage *attribStorage = m_nodeData.m_attribStorage;
	attribStorage->BindPrimitives();

	m_arrayStorage->IndexPrimitivesBatching(indices, counts);

	attribStorage->UnbindPrimitives();
}

void RAS_DisplayArrayBucket::ChangeMaterialBucket(RAS_MaterialBucket *bucket)
{
	m_bucket = bucket;

	// Change of material update looking.
	RAS_IMaterial *material = bucket->GetMaterial();
	material->MoveUpdateClient(&m_materialUpdateClient, RAS_IMaterial::ATTRIBUTES_MODIFIED);

	// Instancing buffers are linked to material attributes, invalid them.
	for (unsigned short i = 0; i < RAS_Rasterizer::RAS_DRAW_MAX; ++i) {
		m_instancingBuffer[i].reset(nullptr);
	}
}
