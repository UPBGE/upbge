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
	RAS_DisplayArrayNodeData *displayArrayData = tuple.m_displayArrayData;
	RAS_Rasterizer *rasty = managerData->m_rasty;
	rasty->SetClientObject(m_meshUser->GetClientObject());
	rasty->SetFrontFace(m_meshUser->GetFrontFace());


	if (!managerData->m_shaderOverride) {
		materialData->m_material->ActivateMeshSlot(this, rasty);
	}

	if (materialData->m_zsort && managerData->m_drawingMode >= RAS_Rasterizer::RAS_SOLID && displayArrayData->m_storageInfo) {
		RAS_IStorageInfo *storage = displayArrayData->m_storageInfo;
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
		if (gpumat && ((rasty->GetDrawingMode() != RAS_Rasterizer::RAS_SHADOW) && (rasty->GetDrawingMode() != RAS_Rasterizer::RAS_WIREFRAME))) {
			GPUPass *pass = GPU_material_get_pass(gpumat);
			GPUShader *shader = GPU_pass_shader(pass);
			GPU_shader_bind(shader);

			rasty->ProcessLighting(materialData->m_useLighting, managerData->m_trans, shader);

			KX_Scene *scene = KX_GetActiveScene();
			KX_Camera *cam = scene->GetActiveCamera();

			// lit surface frag uniforms
			int projloc = GPU_shader_get_uniform(shader, "ProjectionMatrix");
			int viewinvloc = GPU_shader_get_uniform(shader, "ViewMatrixInverse");
			int viewloc = GPU_shader_get_uniform(shader, "ViewMatrix");
			// lit surface vert uniforms
			int modelviewprojloc = GPU_shader_get_uniform(shader, "ModelViewProjectionMatrix");
			int modelloc = GPU_shader_get_uniform(shader, "ModelMatrix");
			int modelviewloc = GPU_shader_get_uniform(shader, "ModelViewMatrix");
			int worldnormloc = GPU_shader_get_uniform(shader, "WorldNormalMatrix");
			int normloc = GPU_shader_get_uniform(shader, "NormalMatrix");

			MT_Matrix4x4 proj(cam->GetProjectionMatrix());
			MT_Matrix4x4 view(rasty->GetViewMatrix());
			MT_Matrix4x4 viewinv(rasty->GetViewInvMatrix());
			MT_Matrix4x4 model(m_meshUser->GetMatrix());
			MT_Matrix4x4 modelview(rasty->GetViewMatrix() * model);
			MT_Matrix4x4 modelviewproj(proj * modelview);
			MT_Matrix4x4 worldnorm(model.inverse());
			MT_Matrix4x4 norm(viewinv * worldnorm);

			float projf[16];
			float viewf[16];
			float viewinvf[16];
			float modelviewprojf[16];
			float modelf[16];
			float modelviewf[16];
			float worldnormf[9];
			float normf[9];

			proj.getValue(projf);
			view.getValue(viewf);
			viewinv.getValue(viewinvf);
			modelviewproj.getValue(modelviewprojf);
			model.getValue(modelf);
			modelview.getValue(modelviewf);

			int k = 0;
			for (int i = 0; i < 3; i++) {
				for (int j = 0; j < 3; j++) {
					worldnormf[k] = worldnorm[i][j];
					normf[k] = norm[i][j];
					k++;
				}
			}

			// MATRICES
			GPU_shader_uniform_vector(shader, projloc, 16, 1, (float *)projf);
			GPU_shader_uniform_vector(shader, viewloc, 16, 1, (float *)viewf);
			GPU_shader_uniform_vector(shader, viewinvloc, 16, 1, (float *)viewinvf);
			GPU_shader_uniform_vector(shader, modelviewprojloc, 16, 1, (float *)modelviewprojf);
			GPU_shader_uniform_vector(shader, modelloc, 16, 1, (float *)modelf);
			GPU_shader_uniform_vector(shader, modelviewloc, 16, 1, (float *)modelviewf);
			GPU_shader_uniform_vector(shader, worldnormloc, 9, 1, (float *)worldnormf);
			GPU_shader_uniform_vector(shader, normloc, 16, 9, (float *)normf);

			// UTIL_TEX
			int texloc = GPU_shader_get_uniform(shader, "utilTex");
			GPU_shader_uniform_texture(shader, texloc, scene->GetUtilTex());

			// PROBES
			int probcountloc = GPU_shader_get_uniform(shader, "probe_count");
			GPU_shader_uniform_int(shader, probcountloc, scene->GetProbeCount()); // There is always the background probe so 1 at least

			int lodmaxloc = GPU_shader_get_uniform(shader, "lodMax");
			GPU_shader_uniform_float(shader, lodmaxloc, scene->GetProbeLodMax());

			int probetexloc = GPU_shader_get_uniform(shader, "probeCubes");
			GPU_shader_uniform_texture(shader, probetexloc, scene->GetProbeTex());

			// MISCELLANEOUS
			int gridcountloc = GPU_shader_get_uniform(shader, "grid_count");
			GPU_shader_uniform_int(shader, gridcountloc, 0);

			int planarcountloc = GPU_shader_get_uniform(shader, "planar_count");
			GPU_shader_uniform_int(shader, planarcountloc, 0);

			int spectoggleloc = GPU_shader_get_uniform(shader, "specToggle");
			GPU_shader_uniform_int(shader, spectoggleloc, 1);
		}
		rasty->IndexPrimitives(displayArrayData->m_storageInfo);
	}
	rasty->PopMatrix();
}
