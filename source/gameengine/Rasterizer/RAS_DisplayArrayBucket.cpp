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
#include "RAS_DisplayArray.h"
#include "RAS_MaterialBucket.h"
#include "RAS_IPolygonMaterial.h"
#include "RAS_MeshObject.h"
#include "RAS_Deformer.h"
#include "RAS_IRasterizer.h"
#include "RAS_IStorage.h"
#include "RAS_InstancingBuffer.h"
#include "RAS_BucketManager.h"

#include <algorithm>

#ifdef _MSC_VER
#  pragma warning (disable:4786)
#endif

#ifdef WIN32
#  include <windows.h>
#endif // WIN32

RAS_DisplayArrayBucket::RAS_DisplayArrayBucket(RAS_MaterialBucket *bucket, RAS_DisplayArray *array, RAS_MeshObject *mesh)
	:m_refcount(1),
	m_bucket(bucket),
	m_displayArray(array),
	m_mesh(mesh),
	m_useDisplayList(false),
	m_useVao(false),
	m_meshModified(false),
	m_storageInfo(NULL),
	m_instancingBuffer(NULL)
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

RAS_DisplayArrayBucket *RAS_DisplayArrayBucket::AddRef()
{
	++m_refcount;
	return this;
}

RAS_DisplayArrayBucket *RAS_DisplayArrayBucket::Release()
{
	--m_refcount;
	if (m_refcount == 0) {
		delete this;
		return NULL;
	}
	return this;
}

unsigned int RAS_DisplayArrayBucket::GetRefCount() const
{
	return m_refcount;
}

RAS_DisplayArrayBucket *RAS_DisplayArrayBucket::GetReplica()
{
	RAS_DisplayArrayBucket *replica = new RAS_DisplayArrayBucket(*this);
	replica->ProcessReplica();
	return replica;
}

void RAS_DisplayArrayBucket::ProcessReplica()
{
	m_refcount = 1;
	m_activeMeshSlots.clear();
	if (m_displayArray) {
		m_displayArray = new RAS_DisplayArray(*m_displayArray);
	}
	m_bucket->AddDisplayArrayBucket(this);
}

RAS_DisplayArray *RAS_DisplayArrayBucket::GetDisplayArray() const
{
	return m_displayArray;
}

RAS_MaterialBucket *RAS_DisplayArrayBucket::GetMaterialBucket() const
{
	return m_bucket;
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

bool RAS_DisplayArrayBucket::UseDisplayList() const
{
	return m_useDisplayList;
}

bool RAS_DisplayArrayBucket::UseVao() const
{
	return m_useVao;
}

bool RAS_DisplayArrayBucket::IsMeshModified() const
{
	return m_meshModified;
}

void RAS_DisplayArrayBucket::UpdateActiveMeshSlots(RAS_IRasterizer *rasty)
{
	// Reset values to default.
	m_useDisplayList = true;
	m_useVao = true;
	m_meshModified = false;

	RAS_IPolyMaterial *material = m_bucket->GetPolyMaterial();

	if (!rasty->UseDisplayLists()) {
		m_useDisplayList = false;
	}

	if (m_bucket->IsZSort() || m_bucket->UseInstancing() || !m_displayArray || material->UsesObjectColor()) {
		m_useDisplayList = false;
		m_useVao = false;
	}

	for (RAS_DeformerList::iterator it = m_deformerList.begin(), end = m_deformerList.end(); it != end; ++it) {
		RAS_Deformer *deformer = *it;

		deformer->Apply(material);

		// Test if one of deformers is dynamic.
		if (deformer->IsDynamic()) {
			m_useDisplayList = false;
			m_useVao = false;
			m_meshModified = true;
		}
	}

	if (m_mesh->GetModifiedFlag() & RAS_MeshObject::MESH_MODIFIED) {
		m_meshModified = true;
	}

	// Set the storage info modified if the mesh is modified.
	if (m_storageInfo) {
		m_storageInfo->SetMeshModified(rasty->GetDrawingMode(), m_meshModified);
	}
}

void RAS_DisplayArrayBucket::SetMeshUnmodified()
{
	m_mesh->SetModifiedFlag(0);
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
		m_storageInfo = NULL;
	}
}

void RAS_DisplayArrayBucket::RenderMeshSlots(const MT_Transform& cameratrans, RAS_IRasterizer *rasty)
{
	if (m_activeMeshSlots.size() == 0) {
		return;
	}

	// Update deformer and render settings.
	UpdateActiveMeshSlots(rasty);

	rasty->BindPrimitives(this);

	for (RAS_MeshSlotList::iterator it = m_activeMeshSlots.begin(), end = m_activeMeshSlots.end(); it != end; ++it) {
		RAS_MeshSlot *ms = *it;
		m_bucket->RenderMeshSlot(cameratrans, rasty, ms);
	}

	rasty->UnbindPrimitives(this);
}

void RAS_DisplayArrayBucket::RenderMeshSlotsInstancing(const MT_Transform& cameratrans, RAS_IRasterizer *rasty, bool alpha)
{
	unsigned int nummeshslots = m_activeMeshSlots.size(); 
	if (nummeshslots == 0) {
		return;
	}

	// Create the instancing buffer only if it needed.
	if (!m_instancingBuffer) {
		m_instancingBuffer = new RAS_InstancingBuffer();
	}

	RAS_IPolyMaterial *material = m_bucket->GetPolyMaterial();

	// Update deformer and render settings.
	UpdateActiveMeshSlots(rasty);

	// Bind the instancing buffer to work on it.
	m_instancingBuffer->Bind();

	/* If the material use the transparency we must sort all mesh slots depending on the distance.
	 * This code share the code used in RAS_BucketManager to do the sort.
	 */
	if (alpha) {
		std::vector<RAS_BucketManager::sortedmeshslot> sortedMeshSlots(nummeshslots);

		const MT_Vector3 pnorm(cameratrans.getBasis()[2]);
		unsigned int i = 0;
		for (RAS_MeshSlotList::iterator it = m_activeMeshSlots.begin(), end = m_activeMeshSlots.end(); it != end; ++it) {
			sortedMeshSlots[i++].set(*it, m_bucket, pnorm);
		}
		std::sort(sortedMeshSlots.begin(), sortedMeshSlots.end(), RAS_BucketManager::backtofront());
		std::vector<RAS_MeshSlot *> meshSlots(nummeshslots);
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

	// Bind all vertex attributs for the used material and the given buffer offset.
	if (rasty->GetOverrideShader() == RAS_IRasterizer::RAS_OVERRIDE_SHADER_NONE) {
		material->ActivateInstancing(
			rasty,
			m_instancingBuffer->GetMatrixOffset(),
			m_instancingBuffer->GetPositionOffset(),
			m_instancingBuffer->GetColorOffset(),
			m_instancingBuffer->GetStride());
	}
	else {
		rasty->ActivateOverrideShaderInstancing(
			m_instancingBuffer->GetMatrixOffset(),
			m_instancingBuffer->GetPositionOffset(),
			m_instancingBuffer->GetStride());
	}

	/* It's a major issue of the geometry instancing : we can't manage face wise.
	 * To be sure we don't use the old face wise we focre it to true. */
	rasty->SetFrontFace(true);

	// Unbind the buffer to avoid conflict with the render after.
	m_instancingBuffer->Unbind();

	rasty->BindPrimitives(this);

	rasty->IndexPrimitivesInstancing(this);
	// Unbind vertex attributs.
	if (rasty->GetOverrideShader() == RAS_IRasterizer::RAS_OVERRIDE_SHADER_NONE) {
		material->DesactivateInstancing();
	}
	else {
		rasty->DesactivateOverrideShaderInstancing();
	}

	rasty->UnbindPrimitives(this);
}

