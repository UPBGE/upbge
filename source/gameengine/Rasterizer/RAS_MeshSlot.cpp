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

#ifdef _MSC_VER
#  pragma warning (disable:4786)
#endif

#ifdef WIN32
#  include <windows.h>
#endif // WIN32

// mesh slot
RAS_MeshSlot::RAS_MeshSlot()
	:m_displayArray(nullptr),
	m_node(this, std::mem_fn(&RAS_MeshSlot::RunNode), nullptr),
	m_bucket(nullptr),
	m_displayArrayBucket(nullptr),
	m_mesh(nullptr),
	m_pDeformer(nullptr),
	m_pDerivedMesh(nullptr),
	m_meshUser(nullptr),
	m_batchPartIndex(-1)
{
}

RAS_MeshSlot::~RAS_MeshSlot()
{
	if (m_pDeformer) {
		// Remove the deformer user in the display array bucket.
		m_displayArrayBucket->RemoveDeformer(m_pDeformer);
	}

	if (m_displayArrayBucket) {
		m_displayArrayBucket->Release();
	}
}

RAS_MeshSlot::RAS_MeshSlot(const RAS_MeshSlot& slot)
{
	m_pDeformer = nullptr;
	m_pDerivedMesh = nullptr;
	m_meshUser = nullptr;
	m_batchPartIndex = -1;
	m_mesh = slot.m_mesh;
	m_meshMaterial = slot.m_meshMaterial;
	m_bucket = slot.m_bucket;
	m_displayArrayBucket = slot.m_displayArrayBucket;
	m_displayArray = slot.m_displayArray;
	m_node = RAS_MeshSlotUpwardNode(this, std::mem_fn(&RAS_MeshSlot::RunNode), nullptr);

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

	m_displayArrayBucket = new RAS_DisplayArrayBucket(bucket, m_displayArray, m_mesh, meshmat);
}

RAS_IDisplayArray *RAS_MeshSlot::GetDisplayArray()
{
	return m_displayArray;
}

void RAS_MeshSlot::SetDeformer(RAS_Deformer *deformer)
{
	if (deformer && m_pDeformer != deformer) {
		if (deformer->ShareVertexArray()) {
			// this deformer uses the base vertex array, first release the current ones
			m_displayArrayBucket->Release();
			m_displayArrayBucket = nullptr;
			// then hook to the base ones
			if (m_meshMaterial && m_meshMaterial->m_baseslot) {
				m_displayArrayBucket = m_meshMaterial->m_baseslot->m_displayArrayBucket->AddRef();
			}
		}
		else {
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
			}
			else {
				// the deformer is not using vertex array (Modifier), release them
				m_displayArrayBucket->Release();
				m_displayArrayBucket = m_bucket->FindDisplayArrayBucket(nullptr, m_mesh);
				if (m_displayArrayBucket) {
					m_displayArrayBucket->AddRef();
				}
				else {
					m_displayArrayBucket = new RAS_DisplayArrayBucket(m_bucket, nullptr, m_mesh, m_meshMaterial);
				}
			}
		}

		if (m_displayArrayBucket) {
			// Add the deformer user in the display array bucket.
			m_displayArrayBucket->AddDeformer(deformer);
			// Update m_displayArray to the display array bucket.
			m_displayArray = m_displayArrayBucket->GetDisplayArray();
		}
		else {
			m_displayArray = nullptr;
		}
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

void RAS_MeshSlot::GenerateTree(RAS_DisplayArrayUpwardNode *root, RAS_UpwardTreeLeafs *leafs)
{
	m_node.SetParent(root);
	leafs->push_back(&m_node);
}

void RAS_MeshSlot::RunNode(const RAS_RenderNodeArguments& args)
{
	RAS_Rasterizer *rasty = args.m_rasty;
	rasty->SetClientObject(m_meshUser->GetClientObject());
	rasty->SetFrontFace(m_meshUser->GetFrontFace());

	RAS_IPolyMaterial *material = m_bucket->GetPolyMaterial();

	if (!args.m_shaderOverride) {
		bool uselights = material->UsesLighting(rasty);
		rasty->ProcessLighting(uselights, args.m_trans);
		material->ActivateMeshSlot(this, rasty);
	}

	if (material->IsZSort() && rasty->GetDrawingMode() >= RAS_Rasterizer::RAS_SOLID) {
		m_mesh->SortPolygons(this, args.m_trans * MT_Transform(m_meshUser->GetMatrix()));
		m_displayArrayBucket->SetPolygonsModified(rasty);
	}

	rasty->PushMatrix();

	const bool istext = material->IsText();
	if ((!m_pDeformer || !m_pDeformer->SkipVertexTransform()) && !istext) {
		float mat[16];
		rasty->GetTransform(m_meshUser->GetMatrix(), material->GetDrawingMode(), mat);
		rasty->MultMatrix(mat);
	}

	if (istext) {
		rasty->IndexPrimitivesText(this);
	}
	else {
		rasty->IndexPrimitives(this);
	}

	rasty->PopMatrix();
}
