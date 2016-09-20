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
#include "RAS_MaterialBucket.h"
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
	:m_displayArray(NULL),
	m_bucket(NULL),
	m_displayArrayBucket(NULL),
	m_mesh(NULL),
	m_pDeformer(NULL),
	m_pDerivedMesh(NULL),
	m_meshUser(NULL)
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
	m_pDeformer = NULL;
	m_pDerivedMesh = NULL;
	m_meshUser = NULL;
	m_mesh = slot.m_mesh;
	m_meshMaterial = slot.m_meshMaterial;
	m_bucket = slot.m_bucket;
	m_displayArrayBucket = slot.m_displayArrayBucket;
	m_displayArray = slot.m_displayArray;

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

	RAS_IDisplayArray::PrimitiveType type = (bucket->IsWire()) ? RAS_IDisplayArray::LINES : RAS_IDisplayArray::TRIANGLES;
	m_displayArray = RAS_IDisplayArray::ConstructArray(type, format);

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
			m_displayArrayBucket = NULL;
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
				m_displayArrayBucket = m_bucket->FindDisplayArrayBucket(NULL, m_mesh);
				if (m_displayArrayBucket) {
					m_displayArrayBucket->AddRef();
				}
				else {
					m_displayArrayBucket = new RAS_DisplayArrayBucket(m_bucket, NULL, m_mesh, m_meshMaterial);
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
			m_displayArray = NULL;
		}
	}
	m_pDeformer = deformer;
}

void RAS_MeshSlot::SetMeshUser(RAS_MeshUser *user)
{
	m_meshUser = user;
}
