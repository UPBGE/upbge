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
#include "RAS_MaterialShader.h"
#include "RAS_OverrideShader.h"
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
	m_shader(nullptr)
{
	static const std::vector<RAS_RenderNodeDefine<RAS_MaterialDownwardNode> > downwardNodeDefines = {
		{NODE_DOWNWARD_NORMAL, RAS_NODE_FUNC(RAS_MaterialBucket::BindNode), RAS_NODE_FUNC(RAS_MaterialBucket::UnbindNode)},
		{NODE_DOWNWARD_OVERRIDE, nullptr, nullptr},
	};

	static const std::vector<RAS_RenderNodeDefine<RAS_MaterialUpwardNode> > upwardNodeDefines = {
		{NODE_UPWARD_NORMAL, RAS_NODE_FUNC(RAS_MaterialBucket::BindNode), RAS_NODE_FUNC(RAS_MaterialBucket::UnbindNode)}
	};

	for (const RAS_RenderNodeDefine<RAS_MaterialDownwardNode>& define : downwardNodeDefines) {
		m_downwardNode[define.m_index] = define.Init(this, &m_nodeData);
	}
	for (const RAS_RenderNodeDefine<RAS_MaterialUpwardNode>& define : upwardNodeDefines) {
		m_upwardNode[define.m_index] = define.Init(this, &m_nodeData);
	}

	m_nodeData.m_material = m_material;
	m_nodeData.m_drawingMode = m_material->GetDrawingMode();
	m_nodeData.m_cullFace = m_material->IsCullFace();
	m_nodeData.m_zsort = m_material->IsZSort();
	m_nodeData.m_text = m_material->IsText();
	m_nodeData.m_zoffset = m_material->GetZOffset();
}

RAS_MaterialBucket::~RAS_MaterialBucket()
{
}

RAS_IPolyMaterial *RAS_MaterialBucket::GetPolyMaterial() const
{
	return m_material;
}

RAS_MaterialShader *RAS_MaterialBucket::GetShader() const
{
	return m_shader;
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
	return false; //(m_material->UseInstancing());
}

void RAS_MaterialBucket::UpdateShader()
{
	m_shader = m_material->GetShader();
}

void RAS_MaterialBucket::RemoveActiveMeshSlots()
{
	for (RAS_DisplayArrayBucketList::iterator it = m_displayArrayBucketList.begin(), end = m_displayArrayBucketList.end();
		 it != end; ++it)
	{
		(*it)->RemoveActiveMeshSlots();
	}
}

void RAS_MaterialBucket::GenerateTree(RAS_ManagerDownwardNode& downwardRoot, RAS_ManagerUpwardNode& upwardRoot,
									  RAS_UpwardTreeLeafs& upwardLeafs, const RAS_MaterialNodeTuple& tuple)
{
	if (m_displayArrayBucketList.size() == 0) {
		return;
	}

	RAS_ManagerNodeData *managerData = tuple.m_managerData;
	RAS_OverrideShader *overrideShader = managerData->m_overrideShader;

	m_nodeData.m_shader = overrideShader ? overrideShader : m_shader;
	RAS_MaterialDownwardNode& downwardNode = m_downwardNode[overrideShader ? NODE_DOWNWARD_OVERRIDE : NODE_DOWNWARD_NORMAL];

	const RAS_DisplayArrayNodeTuple arrayTuple(tuple, &m_nodeData);
	for (RAS_DisplayArrayBucket *displayArrayBucket : m_displayArrayBucketList) {
		displayArrayBucket->GenerateTree(downwardNode, m_upwardNode[NODE_UPWARD_NORMAL], upwardLeafs, arrayTuple);
	}

	downwardRoot.AddChild(&downwardNode);

	if (managerData->m_sort) {
		m_upwardNode[NODE_UPWARD_NORMAL].SetParent(&upwardRoot);
	}
}

void RAS_MaterialBucket::BindNode(const RAS_MaterialNodeTuple& tuple)
{
	RAS_ManagerNodeData *managerData = tuple.m_managerData;
	RAS_Rasterizer *rasty = managerData->m_rasty;

	
	rasty->SetCullFace(m_nodeData.m_cullFace);
	rasty->SetPolygonOffset(-m_nodeData.m_zoffset, 0.0f);

	m_shader->Activate(rasty);
}

void RAS_MaterialBucket::UnbindNode(const RAS_MaterialNodeTuple& tuple)
{
	m_shader->Desactivate();
}

void RAS_MaterialBucket::AddDisplayArrayBucket(RAS_DisplayArrayBucket *bucket)
{
	m_displayArrayBucketList.push_back(bucket);
}

void RAS_MaterialBucket::RemoveDisplayArrayBucket(RAS_DisplayArrayBucket *bucket)
{
	if (m_displayArrayBucketList.size() == 0) {
		return;
	}
	RAS_DisplayArrayBucketList::iterator it = std::find(m_displayArrayBucketList.begin(), m_displayArrayBucketList.end(), bucket);
	if (it != m_displayArrayBucketList.end()) {
		m_displayArrayBucketList.erase(it);
	}
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

		displayArrayBucket->ChangeMaterialBucket(bucket);
		bucket->AddDisplayArrayBucket(displayArrayBucket);
		dit = m_displayArrayBucketList.erase(dit);
	}
}
