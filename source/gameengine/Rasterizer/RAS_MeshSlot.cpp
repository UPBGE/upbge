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

/** \file gameengine/Rasterizer/RAS_MeshSlot.cpp
 *  \ingroup bgerast
 */

#include "RAS_MeshSlot.h"
#include "RAS_MeshUser.h"
#include "RAS_MaterialShader.h"
#include "RAS_TexVert.h"
#include "RAS_DisplayArrayStorage.h"
#include "RAS_MeshObject.h"
#include "GPU_material.h"
#include "GPU_shader.h"
#include "GPU_texture.h"
#include "DNA_scene_types.h"

#include "KX_Globals.h"
#include "KX_Scene.h"
#include "KX_Camera.h"

extern "C" {
#  include "../gpu/intern/gpu_codegen.h"
}

#ifdef _MSC_VER
#  pragma warning (disable:4786)
#endif

#ifdef WIN32
#  include <windows.h>
#endif // WIN32

static RAS_DummyNodeData dummyNodeData;

// mesh slot
RAS_MeshSlot::RAS_MeshSlot(RAS_MeshObject *mesh, RAS_MeshUser *meshUser, RAS_DisplayArrayBucket *arrayBucket)
	:m_displayArrayBucket(arrayBucket),
	m_mesh(mesh),
	m_pDerivedMesh(nullptr),
	m_meshUser(meshUser),
	m_batchPartIndex(-1)
{
	static const std::vector<RAS_RenderNodeDefine<RAS_MeshSlotUpwardNode> > nodeDefines = {
		{NODE_NORMAL, RAS_NODE_FUNC(RAS_MeshSlot::RunNodeNormal), nullptr},
// 		{NODE_DERIVED_MESH, RAS_NODE_FUNC(RAS_MeshSlot::RunNodeDerivedMesh), nullptr},
		{NODE_CUBE_MAP, RAS_NODE_FUNC(RAS_MeshSlot::RunNodeCubeMap), nullptr},
		{NODE_TEXT, RAS_NODE_FUNC(RAS_MeshSlot::RunNodeText), nullptr},
	};

	for (const RAS_RenderNodeDefine<RAS_MeshSlotUpwardNode>& define : nodeDefines) {
		m_node[define.m_index] = define.Init(this, &dummyNodeData);
	}
}

RAS_MeshSlot::~RAS_MeshSlot()
{
}

void RAS_MeshSlot::SetDisplayArrayBucket(RAS_DisplayArrayBucket *arrayBucket)
{
	m_displayArrayBucket = arrayBucket;
}

void RAS_MeshSlot::GenerateTree(RAS_DisplayArrayUpwardNode& root, RAS_UpwardTreeLeafs& leafs, const RAS_MeshSlotNodeTuple& tuple)
{
	RAS_ManagerNodeData *managerData = tuple.m_managerData;
	RAS_MaterialNodeData *materialData = tuple.m_materialData;

	NodeType type = NODE_NORMAL;
	if (materialData->m_text) {
		type = NODE_TEXT;
	}
	else if (managerData->m_cubeMap) {
		type = NODE_CUBE_MAP;
	}
	/*else if (m_pDerivedMesh) {
		type = NODE_DERIVED_MESH;
	}*/

	RAS_MeshSlotUpwardNode& node = m_node[type];
	node.SetParent(&root);
	leafs.push_back(&node);
}

void RAS_MeshSlot::PrepareRunNode(const RAS_MeshSlotNodeTuple& tuple)
{
	RAS_ManagerNodeData *managerData = tuple.m_managerData;
	RAS_MaterialNodeData *materialData = tuple.m_materialData;
	RAS_Rasterizer *rasty = managerData->m_rasty;

	rasty->SetClientObject(m_meshUser->GetClientObject());
	rasty->SetFrontFace(m_meshUser->GetFrontFace());
	/*
		if (displayArrayData->m_applyMatrix) {
			float mat[16];
			rasty->GetTransform(m_meshUser->GetMatrix(), materialData->m_drawingMode, mat);
			rasty->MultMatrix(mat);
		}
	*/
	materialData->m_shader->Update(rasty, m_meshUser); // TODO sent the matrix with billboard/ray transform
}

/*void RAS_MeshSlot::RunNodeDerivedMesh(const RAS_MeshSlotNodeTuple& tuple)
{
	RAS_ManagerNodeData *managerData = tuple.m_managerData;

	PrepareRunNode(tuple);

	managerData->m_rasty->IndexPrimitivesDerivedMesh(this);
}*/

void RAS_MeshSlot::RunNodeText(const RAS_MeshSlotNodeTuple& tuple)
{
	RAS_ManagerNodeData *managerData = tuple.m_managerData;

	PrepareRunNode(tuple);

	managerData->m_rasty->IndexPrimitivesText(this);
}

void RAS_MeshSlot::RunNodeCubeMap(const RAS_MeshSlotNodeTuple& tuple)
{
	PrepareRunNode(tuple);

	RAS_DisplayArrayNodeData *displayArrayData = tuple.m_displayArrayData;

	displayArrayData->m_arrayStorage->IndexPrimitivesInstancing(6);
}

void RAS_MeshSlot::RunNodeNormal(const RAS_MeshSlotNodeTuple& tuple)
{
	PrepareRunNode(tuple);

	/*RAS_ManagerNodeData *managerData = tuple.m_managerData;
	RAS_MaterialNodeData *materialData = tuple.m_materialData;
	RAS_DisplayArrayNodeData *displayArrayData = tuple.m_displayArrayData;

	RAS_DisplayArrayStorage *storage = displayArrayData->m_arrayStorage;
	if (materialData->m_zsort && storage) {
		m_mesh->SortPolygons(displayArrayData->m_array, managerData->m_trans * MT_Transform(m_meshUser->GetMatrix()),
							 storage->GetIndexMap());
		storage->FlushIndexMap();
	}

	storage->IndexPrimitives();*/
}
