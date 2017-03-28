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
#include "RAS_Rasterizer.h"
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
	:m_material(mat),
	m_downwardNode(this, std::mem_fn(&RAS_MaterialBucket::BindNode), std::mem_fn(&RAS_MaterialBucket::UnbindNode)),
	m_upwardNode(this, std::mem_fn(&RAS_MaterialBucket::BindNode), std::mem_fn(&RAS_MaterialBucket::UnbindNode))
{
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

RAS_MeshSlot *RAS_MaterialBucket::NewMesh(RAS_MeshObject *mesh, RAS_MeshMaterial *meshmat, const RAS_TexVertFormat& format)
{
	RAS_MeshSlot *ms = new RAS_MeshSlot();
	ms->init(this, mesh, meshmat, format);

	m_meshSlots.push_back(ms);

	return ms;
}

void RAS_MaterialBucket::AddMesh(RAS_MeshSlot *ms)
{
	m_meshSlots.push_back(ms);
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

void RAS_MaterialBucket::RemoveActiveMeshSlots()
{
	for (RAS_DisplayArrayBucketList::iterator it = m_displayArrayBucketList.begin(), end = m_displayArrayBucketList.end();
		 it != end; ++it)
	{
		(*it)->RemoveActiveMeshSlots();
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

void RAS_MaterialBucket::ActivateMaterial(RAS_Rasterizer *rasty)
{
	m_material->Activate(rasty);
}

void RAS_MaterialBucket::DesactivateMaterial(RAS_Rasterizer *rasty)
{
	m_material->Desactivate(rasty);
}

void RAS_MaterialBucket::GenerateTree(RAS_ManagerDownwardNode *downwardRoot, RAS_ManagerUpwardNode *upwardRoot,
									  RAS_UpwardTreeLeafs *upwardLeafs, RAS_Rasterizer *rasty, bool sort)
{
	if (m_displayArrayBucketList.size() == 0) {
		return;
	}

	const bool instancing = UseInstancing();
	for (RAS_DisplayArrayBucket *displayArrayBucket : m_displayArrayBucketList) {
		displayArrayBucket->GenerateTree(&m_downwardNode, &m_upwardNode, upwardLeafs, rasty, sort, instancing);
	}

	downwardRoot->AddChild(&m_downwardNode);

	if (sort) {
		m_upwardNode.SetParent(upwardRoot);
	}
}

void RAS_MaterialBucket::BindNode(const RAS_RenderNodeArguments& args)
{
	args.m_rasty->SetCullFace(m_material->IsCullFace());
	if (!args.m_shaderOverride) {
		ActivateMaterial(args.m_rasty);
	}
}

void RAS_MaterialBucket::UnbindNode(const RAS_RenderNodeArguments& args)
{
	if (!args.m_shaderOverride) {
		DesactivateMaterial(args.m_rasty);
	}
}

void RAS_MaterialBucket::SetDisplayArrayUnmodified()
{
	for (RAS_DisplayArrayBucket *displayArrayBucket : m_displayArrayBucketList) {
		displayArrayBucket->SetDisplayArrayUnmodified();
	}
}

RAS_DisplayArrayBucket *RAS_MaterialBucket::FindDisplayArrayBucket(RAS_IDisplayArray *array, RAS_MeshObject *mesh)
{
	for (RAS_DisplayArrayBucket *displayArrayBucket : m_displayArrayBucketList) {
		if (displayArrayBucket->GetDisplayArray() == array && displayArrayBucket->GetMesh() == mesh) {
			return displayArrayBucket;
		}
	}
	return nullptr;
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

void RAS_MaterialBucket::MoveDisplayArrayBucket(RAS_MeshMaterial *meshmat, RAS_MaterialBucket *bucket)
{
	for (RAS_DisplayArrayBucketList::iterator dit = m_displayArrayBucketList.begin(); dit != m_displayArrayBucketList.end();) {
		// In case of deformers, multiple display array bucket can use the same mesh and material.
		RAS_DisplayArrayBucket *displayArrayBucket = *dit;
		if (displayArrayBucket->GetMeshMaterial() != meshmat) {
			++dit;
			continue;
		}

		for (RAS_MeshSlotList::iterator mit = m_meshSlots.begin(); mit != m_meshSlots.end();) {
			RAS_MeshSlot *ms = *mit;
			if (ms->m_displayArrayBucket == displayArrayBucket) {
				ms->m_bucket = bucket;
				bucket->AddMesh(ms);
				mit = m_meshSlots.erase(mit);
			}
			else {
				++mit;
			}
		}

		displayArrayBucket->ChangeMaterialBucket(bucket);
		bucket->AddDisplayArrayBucket(displayArrayBucket);
		dit = m_displayArrayBucketList.erase(dit);
	}
}
