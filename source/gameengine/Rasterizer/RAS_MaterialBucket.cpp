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

/** \file gameengine/Rasterizer/RAS_MaterialBucket.cpp
 *  \ingroup bgerast
 */

#include "RAS_MaterialBucket.h"
#include "RAS_IPolygonMaterial.h"
#include "RAS_IRasterizer.h"
#include "RAS_MeshObject.h"
#include "RAS_MeshUser.h"
#include "RAS_Deformer.h"

#include <algorithm>

#ifdef _MSC_VER
#  pragma warning (disable:4786)
#endif

#ifdef WIN32
#  include <windows.h>
#endif // WIN32

RAS_MaterialBucket::RAS_MaterialBucket(RAS_IPolyMaterial *mat)
{
	m_material = mat;
}

RAS_MaterialBucket::~RAS_MaterialBucket()
{
	for (RAS_MeshSlotList::iterator it = m_meshSlots.begin(), end = m_meshSlots.end(); it != end; ++it) {
		delete (*it);
	}
}

RAS_IPolyMaterial *RAS_MaterialBucket::GetPolyMaterial() const
{
	return m_material;
}

bool RAS_MaterialBucket::IsAlpha() const
{
	return (m_material->IsAlpha());
}

bool RAS_MaterialBucket::IsZSort() const
{
	return (m_material->IsZSort());
}

bool RAS_MaterialBucket::IsWire() const
{
	return (m_material->IsWire());
}

bool RAS_MaterialBucket::UseInstancing() const
{
	return (m_material->UseInstancing());
}

RAS_MeshSlot *RAS_MaterialBucket::AddMesh(RAS_MeshObject *mesh, RAS_MeshMaterial *meshmat, const RAS_TexVertFormat& format)
{
	RAS_MeshSlot *ms = new RAS_MeshSlot();
	ms->init(this, mesh, meshmat, format);

	m_meshSlots.push_back(ms);

	return ms;
}

RAS_MeshSlot *RAS_MaterialBucket::CopyMesh(RAS_MeshSlot *ms)
{
	RAS_MeshSlot *newMeshSlot = new RAS_MeshSlot(*ms);
	m_meshSlots.push_back(newMeshSlot);

	return newMeshSlot;
}

void RAS_MaterialBucket::RemoveMesh(RAS_MeshSlot *ms)
{
	RAS_MeshSlotList::iterator it = std::find(m_meshSlots.begin(), m_meshSlots.end(), ms);
	if (it != m_meshSlots.end()) {
		m_meshSlots.erase(it);
		delete ms;
	}
}

void RAS_MaterialBucket::RemoveMeshObject(RAS_MeshObject *mesh)
{
	for (RAS_MeshSlotList::iterator it = m_meshSlots.begin(); it != m_meshSlots.end();) {
		RAS_MeshSlot *ms = *it;
		if (ms->m_mesh == mesh) {
			delete ms;
			it = m_meshSlots.erase(it);
		}
		else {
			++it;
		}
	}
}

unsigned int RAS_MaterialBucket::GetNumActiveMeshSlots()
{
	unsigned int count = 0;
	for (RAS_DisplayArrayBucketList::iterator it = m_displayArrayBucketList.begin(), end = m_displayArrayBucketList.end();
		it != end; ++it)
	{
		RAS_DisplayArrayBucket *displayArrayBucket = *it;
		count += displayArrayBucket->GetNumActiveMeshSlots();
	}

	return count;
}

RAS_MeshSlotList::iterator RAS_MaterialBucket::msBegin()
{
	return m_meshSlots.begin();
}

RAS_MeshSlotList::iterator RAS_MaterialBucket::msEnd()
{
	return m_meshSlots.end();
}

bool RAS_MaterialBucket::ActivateMaterial(RAS_IRasterizer *rasty)
{
	if (rasty->GetOverrideShader() == RAS_IRasterizer::RAS_OVERRIDE_SHADER_NONE) {
		m_material->Activate(rasty);
	}

	return true;
}

void RAS_MaterialBucket::DesactivateMaterial(RAS_IRasterizer *rasty)
{
	if (rasty->GetOverrideShader() == RAS_IRasterizer::RAS_OVERRIDE_SHADER_NONE) {
		m_material->Desactivate(rasty);
	}
}

void RAS_MaterialBucket::RenderMeshSlot(const MT_Transform& cameratrans, RAS_IRasterizer *rasty, RAS_MeshSlot *ms)
{
	RAS_MeshUser *meshUser = ms->m_meshUser;
	rasty->SetClientObject(meshUser->GetClientObject());
	rasty->SetFrontFace(meshUser->GetFrontFace());

	// Inverse condition of in ActivateMaterial.
	if (!(rasty->GetDrawingMode() == RAS_IRasterizer::RAS_SHADOW && !m_material->CastsShadows()) ||
		(rasty->GetDrawingMode() != RAS_IRasterizer::RAS_SHADOW && m_material->OnlyShadow()))
	{
		bool uselights = m_material->UsesLighting(rasty);
		rasty->ProcessLighting(uselights, cameratrans);
	}

	if (rasty->GetOverrideShader() == RAS_IRasterizer::RAS_OVERRIDE_SHADER_NONE) {
		m_material->ActivateMeshSlot(ms, rasty);
	}

	if (IsZSort() && rasty->GetDrawingMode() >= RAS_IRasterizer::RAS_SOLID)
		ms->m_mesh->SortPolygons(ms, cameratrans * MT_Transform(meshUser->GetMatrix()));

	rasty->PushMatrix();
	if ((!ms->m_pDeformer || !ms->m_pDeformer->SkipVertexTransform()) && !m_material->IsText()) {
		float mat[16];
		rasty->GetTransform(meshUser->GetMatrix(), m_material->GetDrawingMode(), mat);
		rasty->MultMatrix(mat);
	}

	if (m_material->IsText()) {
		rasty->IndexPrimitivesText(ms);
	}
	else {
		rasty->IndexPrimitives(ms->m_displayArrayBucket->GetStorageType(), ms);
	}

	rasty->PopMatrix();
}

void RAS_MaterialBucket::RenderMeshSlots(const MT_Transform& cameratrans, RAS_IRasterizer *rasty)
{
	if (GetNumActiveMeshSlots() == 0) {
		return;
	}

	bool matactivated = ActivateMaterial(rasty);

	for (RAS_DisplayArrayBucketList::iterator it = m_displayArrayBucketList.begin(), end = m_displayArrayBucketList.end();
		it != end; ++it)
	{
		RAS_DisplayArrayBucket *displayArrayBucket = *it;
		if (!matactivated) {
			displayArrayBucket->RemoveActiveMeshSlots();
			continue;
		}

		// Choose the rendering mode : geometry instancing render / regular render.
		if (UseInstancing()) {
			displayArrayBucket->RenderMeshSlotsInstancing(cameratrans, rasty, IsAlpha());
		}
		else {
			displayArrayBucket->RenderMeshSlots(cameratrans, rasty);
		}

		displayArrayBucket->RemoveActiveMeshSlots();
	}

	if (matactivated) {
		DesactivateMaterial(rasty);
	}
}

void RAS_MaterialBucket::SetMeshUnmodified()
{
	for (RAS_DisplayArrayBucketList::iterator it = m_displayArrayBucketList.begin(), end = m_displayArrayBucketList.end();
		it != end; ++it)
	{
		RAS_DisplayArrayBucket *displayArrayBucket = *it;
		displayArrayBucket->SetMeshUnmodified();
	}
}

RAS_DisplayArrayBucket *RAS_MaterialBucket::FindDisplayArrayBucket(RAS_IDisplayArray *array, RAS_MeshObject *mesh)
{
	for (RAS_DisplayArrayBucketList::iterator it = m_displayArrayBucketList.begin(), end = m_displayArrayBucketList.end();
		it != end; ++it)
	{
		RAS_DisplayArrayBucket *displayArrayBucket = *it;
		if (displayArrayBucket->GetDisplayArray() == array && displayArrayBucket->GetMesh() == mesh) {
			return displayArrayBucket;
		}
	}
	return NULL;
}

void RAS_MaterialBucket::AddDisplayArrayBucket(RAS_DisplayArrayBucket *bucket)
{
	m_displayArrayBucketList.push_back(bucket);
}

void RAS_MaterialBucket::RemoveDisplayArrayBucket(RAS_DisplayArrayBucket *bucket)
{
	RAS_DisplayArrayBucketList::iterator it = std::find(m_displayArrayBucketList.begin(), m_displayArrayBucketList.end(), bucket);
	if (it != m_displayArrayBucketList.end()) {
		m_displayArrayBucketList.erase(it);
	}
}

RAS_DisplayArrayBucketList& RAS_MaterialBucket::GetDisplayArrayBucketList()
{
	return m_displayArrayBucketList;
}
