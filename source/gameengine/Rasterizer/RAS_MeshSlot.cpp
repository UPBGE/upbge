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
#include "RAS_MaterialBucket.h"
#include "RAS_IPolygonMaterial.h"
#include "RAS_TexVert.h"
#include "RAS_MeshObject.h"
#include "RAS_Deformer.h"
#include "RAS_DisplayArray.h"
#include "RAS_IStorageInfo.h"
#include "GPU_material.h"
#include "GPU_shader.h"

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
RAS_MeshSlot::RAS_MeshSlot()
	:m_displayArray(nullptr),
	m_node(this, &dummyNodeData, std::mem_fn(&RAS_MeshSlot::RunNode), nullptr),
	m_bucket(nullptr),
	m_displayArrayBucket(nullptr),
	m_mesh(nullptr),
	m_meshMaterial(nullptr),
	m_pDeformer(nullptr),
	m_pDerivedMesh(nullptr),
	m_meshUser(nullptr),
	m_batchPartIndex(-1),
	m_gpuMat(nullptr)
{
}

RAS_MeshSlot::~RAS_MeshSlot()
{
	if (m_displayArrayBucket) {
		m_displayArrayBucket->Release();
	}
}

RAS_MeshSlot::RAS_MeshSlot(const RAS_MeshSlot& slot)
	:m_displayArray(slot.m_displayArray),
	m_node(this, &dummyNodeData, std::mem_fn(&RAS_MeshSlot::RunNode), nullptr),
	m_bucket(slot.m_bucket),
	m_displayArrayBucket(slot.m_displayArrayBucket),
	m_mesh(slot.m_mesh),
	m_meshMaterial(slot.m_meshMaterial),
	m_pDeformer(nullptr),
	m_pDerivedMesh(nullptr),
	m_meshUser(nullptr),
	m_batchPartIndex(-1),
	m_gpuMat(nullptr)
{
	if (m_displayArrayBucket) {
		m_displayArrayBucket->AddRef();
	}
}

void RAS_MeshSlot::init(RAS_MaterialBucket *bucket, RAS_MeshObject *mesh,
						RAS_MeshMaterial *meshmat, const RAS_TexVertFormat& format)
{
	m_bucket = bucket;
	m_mesh = mesh;
	m_meshMaterial = meshmat;

	// Test if the mesh slot is not owned by a font object, no mesh.
	if (mesh && meshmat) {
		RAS_IDisplayArray::PrimitiveType type = (bucket->IsWire()) ? RAS_IDisplayArray::LINES : RAS_IDisplayArray::TRIANGLES;
		m_displayArray = RAS_IDisplayArray::ConstructArray(type, format);
	}

	m_displayArrayBucket = new RAS_DisplayArrayBucket(bucket, m_displayArray, m_mesh, meshmat, m_pDeformer);
}

RAS_IDisplayArray *RAS_MeshSlot::GetDisplayArray()
{
	return m_displayArray;
}

void RAS_MeshSlot::SetDeformer(RAS_Deformer *deformer)
{
	if (deformer && m_pDeformer != deformer) {
		// no sharing
		// we create local copy of RAS_DisplayArray when we have a deformer:
		// this way we can avoid conflict between the vertex cache of duplicates
		if (deformer->UseVertexArray()) {
			// the deformer makes use of vertex array, make sure we have our local copy
			if (m_displayArrayBucket->GetRefCount() > 1) {
				// only need to copy if there are other users
				// note that this is the usual case as vertex arrays are held by the material base slot
				m_displayArrayBucket->Release();
				m_displayArrayBucket = m_displayArrayBucket->GetReplica();
			}
			m_displayArrayBucket->SetDeformer(deformer);
		}
		else {
			// the deformer is not using vertex array (Modifier), release them
			m_displayArrayBucket->Release();
			m_displayArrayBucket = new RAS_DisplayArrayBucket(m_bucket, nullptr, m_mesh, m_meshMaterial, deformer);
		}

		// Update m_displayArray to the display array bucket.
		m_displayArray = m_displayArrayBucket->GetDisplayArray();
	}
	m_pDeformer = deformer;
}

void RAS_MeshSlot::SetMeshUser(RAS_MeshUser *user)
{
	m_meshUser = user;
}

void RAS_MeshSlot::SetDisplayArrayBucket(RAS_DisplayArrayBucket *arrayBucket)
{
	if (m_displayArrayBucket) {
		m_displayArrayBucket->Release();
	}

	m_displayArrayBucket = arrayBucket;
	m_displayArray = m_displayArrayBucket->GetDisplayArray();
}

void RAS_MeshSlot::SetGpuMat(GPUMaterial *mat)
{
	m_gpuMat = mat;
}

GPUMaterial *RAS_MeshSlot::GetGpuMat()
{
	return m_gpuMat;
}

void RAS_MeshSlot::GenerateTree(RAS_DisplayArrayUpwardNode& root, RAS_UpwardTreeLeafs& leafs)
{
	m_node.SetParent(&root);
	leafs.push_back(&m_node);
}

void RAS_MeshSlot::RunNode(const RAS_MeshSlotNodeTuple& tuple)
{
	RAS_ManagerNodeData *managerData = tuple.m_managerData;
	RAS_MaterialNodeData *materialData = tuple.m_materialData;
	RAS_Rasterizer *rasty = managerData->m_rasty;
	rasty->SetClientObject(m_meshUser->GetClientObject());
	rasty->SetFrontFace(m_meshUser->GetFrontFace());


	if (!managerData->m_shaderOverride) {
		rasty->ProcessLighting(materialData->m_useLighting, managerData->m_trans);
		materialData->m_material->ActivateMeshSlot(this, rasty);
	}

	if (materialData->m_zsort && managerData->m_drawingMode >= RAS_Rasterizer::RAS_SOLID) {
		RAS_IStorageInfo *storage = tuple.m_displayArrayData->m_storageInfo;
		m_mesh->SortPolygons(this, managerData->m_trans * MT_Transform(m_meshUser->GetMatrix()), storage->GetIndexMap());
		storage->FlushIndexMap();
	}

	rasty->PushMatrix();

	const bool istext = materialData->m_text;
	if ((!m_pDeformer || !m_pDeformer->SkipVertexTransform()) && !istext) {
		float mat[16];
		rasty->GetTransform(m_meshUser->GetMatrix(), materialData->m_drawingMode, mat);
		rasty->MultMatrix(mat);
	}

	if (istext) {
		rasty->IndexPrimitivesText(this);
	}
	else if (m_pDerivedMesh) {
		rasty->IndexPrimitivesDerivedMesh(this);
	}
	else {
		/* For blender shaders (not custom), we need
		* to bind shader to update uniforms.
		* In shadow pass for now, we don't update
		* the ShaderInterface uniforms because
		* GPUMaterial is not accessible (RenderShadowBuffer
		* is called before main rendering and the GPUMaterial
		* is bound only when we do the main rendering).
		* Refs: - For drawing mode, rasterizer drawing mode is
		* set to RAS_SHADOWS in KX_KetsjiEngine::RenderShadowBuffers(KX_Scene *scene)
		* - To see the order of rendering operations, this is in
		* KX_KetsjiEngine::Render() (RenderShadowBuffers is called
		* at the begining of Render()
		* (m_drawingmode != 0) is used to avoid crash when we press
		* P while we are in wireframe viewport shading mode.
		*/
		GPUMaterial *gpumat = GetGpuMat();
		if (gpumat && !(rasty->GetDrawingMode() & RAS_Rasterizer::RAS_SHADOW) && (rasty->GetDrawingMode() != 0)) {
			GPUPass *pass = GPU_material_get_pass(gpumat);
			GPUShader *shader = GPU_pass_shader(pass);
			GPU_shader_bind(shader);
		}
		rasty->IndexPrimitives(tuple.m_displayArrayData->m_storageInfo);
	}

	rasty->PopMatrix();
}
