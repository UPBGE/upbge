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
	m_clientObj(NULL),
	m_pDeformer(NULL),
	m_pDerivedMesh(NULL),
	m_OpenGLMatrix(NULL),
	m_bVisible(false),
	m_bCulled(true),
	m_bObjectColor(false),
	m_RGBAcolor(MT_Vector4(0.0f, 0.0f, 0.0f, 0.0f)),
	m_DisplayList(NULL),
	m_bDisplayList(true),
	m_joinSlot(NULL)
{
}

RAS_MeshSlot::~RAS_MeshSlot()
{
#ifdef USE_SPLIT
	Split(true);

	while (m_joinedSlots.size())
		m_joinedSlots.front()->Split(true);
#endif

	if (m_displayArrayBucket) {
		m_displayArrayBucket->Release();
	}

	if (m_DisplayList) {
		m_DisplayList->Release();
		m_DisplayList = NULL;
	}
}

RAS_MeshSlot::RAS_MeshSlot(const RAS_MeshSlot& slot)
{
	m_clientObj = NULL;
	m_pDeformer = NULL;
	m_pDerivedMesh = NULL;
	m_OpenGLMatrix = NULL;
	m_mesh = slot.m_mesh;
	m_bucket = slot.m_bucket;
	m_displayArrayBucket = slot.m_displayArrayBucket;
	m_bVisible = slot.m_bVisible;
	m_bCulled = slot.m_bCulled;
	m_bObjectColor = slot.m_bObjectColor;
	m_RGBAcolor = slot.m_RGBAcolor;
	m_DisplayList = NULL;
	m_bDisplayList = slot.m_bDisplayList;
	m_joinSlot = NULL;
	m_displayArray = slot.m_displayArray;
	m_joinedSlots = slot.m_joinedSlots;

	if (m_displayArrayBucket) {
		m_displayArrayBucket->AddRef();
	}
}

void RAS_MeshSlot::init(RAS_MaterialBucket *bucket)
{
	m_bucket = bucket;
	m_displayArrayBucket = new RAS_DisplayArrayBucket(bucket, (new RAS_DisplayArray()));
	m_displayArray = m_displayArrayBucket->GetDisplayArray();
}

RAS_DisplayArray *RAS_MeshSlot::GetDisplayArray()
{
	return m_displayArray;
}

int RAS_MeshSlot::AddVertex(const RAS_TexVert& tv)
{
	m_displayArray->m_vertex.push_back(tv);
	return (m_displayArray->m_vertex.size() - 1);
}

void RAS_MeshSlot::AddPolygonVertex(int offset)
{
	m_displayArray->m_index.push_back(offset);
}

void RAS_MeshSlot::SetDeformer(RAS_Deformer *deformer)
{
	if (deformer && m_pDeformer != deformer) {
		if (deformer->ShareVertexArray()) {
			// this deformer uses the base vertex array, first release the current ones
			m_displayArrayBucket->Release();
			m_displayArrayBucket = NULL;
			// then hook to the base ones
			RAS_MeshMaterial *mmat = m_mesh->GetMeshMaterial(m_bucket->GetPolyMaterial());
			if (mmat && mmat->m_baseslot) {
				m_displayArrayBucket = mmat->m_baseslot->m_displayArrayBucket->AddRef();
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
				m_displayArrayBucket = m_bucket->FindDisplayArrayBucket(NULL)->AddRef();
			}
		}
	}
	m_pDeformer = deformer;

	// Update m_displayArray to the display array bucket.
	m_displayArray = m_displayArrayBucket ? m_displayArrayBucket->GetDisplayArray() : NULL;
}

bool RAS_MeshSlot::Equals(RAS_MeshSlot *target)
{
	if (!m_OpenGLMatrix || !target->m_OpenGLMatrix)
		return false;
	if (m_pDeformer || target->m_pDeformer)
		return false;
	if (m_bVisible != target->m_bVisible)
		return false;
	if (m_bObjectColor != target->m_bObjectColor)
		return false;
	if (m_bObjectColor && !(m_RGBAcolor == target->m_RGBAcolor))
		return false;

	return true;
}

bool RAS_MeshSlot::Join(RAS_MeshSlot *target, MT_Scalar distance)
{
#if 0
	RAS_DisplayArrayList::iterator it;
	iterator mit;
	size_t i;

	// verify if we can join
	if (m_joinSlot || (m_joinedSlots.empty() == false) || target->m_joinSlot)
		return false;

	if (!Equals(target))
		return false;

	MT_Vector3 co(&m_OpenGLMatrix[12]);
	MT_Vector3 targetco(&target->m_OpenGLMatrix[12]);

	if ((co - targetco).length() > distance)
		return false;

	MT_Matrix4x4 mat(m_OpenGLMatrix);
	MT_Matrix4x4 targetmat(target->m_OpenGLMatrix);
	targetmat.invert();

	MT_Matrix4x4 transform = targetmat * mat;

	// m_mesh, clientobj
	m_joinSlot = target;
	m_joinInvTransform = transform;
	m_joinInvTransform.invert();
	target->m_joinedSlots.push_back(this);

	MT_Matrix4x4 ntransform = m_joinInvTransform.transposed();
	ntransform[0][3] = ntransform[1][3] = ntransform[2][3] = 0.0f;

	for (begin(mit); !end(mit); next(mit))
		for (i = mit.startvertex; i < mit.endvertex; i++)
			mit.vertex[i].Transform(transform, ntransform);

	/* We know we'll need a list at least this big, reserve in advance */
	target->m_displayArrays.reserve(target->m_displayArrays.size() + m_displayArrays.size());

	for (it = m_displayArrays.begin(); it != m_displayArrays.end(); it++) {
		target->m_displayArrays.push_back(*it);
		target->m_endarray++;
		target->m_endvertex = target->m_displayArrays.back()->m_vertex.size();
		target->m_endindex = target->m_displayArrays.back()->m_index.size();
	}

	if (m_DisplayList) {
		m_DisplayList->Release();
		m_DisplayList = NULL;
	}
	if (target->m_DisplayList) {
		target->m_DisplayList->Release();
		target->m_DisplayList = NULL;
	}
#endif
	return true;
#if 0
	return false;
#endif
}

bool RAS_MeshSlot::Split(bool force)
{
#if 0
	list<RAS_MeshSlot *>::iterator jit;
	RAS_MeshSlot *target = m_joinSlot;
	RAS_DisplayArrayList::iterator it, jt;
	iterator mit;
	size_t i, found0 = 0, found1 = 0;

	if (target && (force || !Equals(target))) {
		m_joinSlot = NULL;

		for (jit = target->m_joinedSlots.begin(); jit != target->m_joinedSlots.end(); jit++) {
			if (*jit == this) {
				target->m_joinedSlots.erase(jit);
				found0 = 1;
				break;
			}
		}

		if (!found0)
			abort();

		for (it = m_displayArrays.begin(); it != m_displayArrays.end(); it++) {
			found1 = 0;
			for (jt = target->m_displayArrays.begin(); jt != target->m_displayArrays.end(); jt++) {
				if (*jt == *it) {
					target->m_displayArrays.erase(jt);
					target->m_endarray--;
					found1 = 1;
					break;
				}
			}

			if (!found1)
				abort();
		}

		if (target->m_displayArrays.empty() == false) {
			target->m_endvertex = target->m_displayArrays.back()->m_vertex.size();
			target->m_endindex = target->m_displayArrays.back()->m_index.size();
		}
		else {
			target->m_endvertex = 0;
			target->m_endindex = 0;
		}

		MT_Matrix4x4 ntransform = m_joinInvTransform.inverse().transposed();
		ntransform[0][3] = ntransform[1][3] = ntransform[2][3] = 0.0f;

		for (begin(mit); !end(mit); next(mit))
			for (i = mit.startvertex; i < mit.endvertex; i++)
				mit.vertex[i].Transform(m_joinInvTransform, ntransform);

		if (target->m_DisplayList) {
			target->m_DisplayList->Release();
			target->m_DisplayList = NULL;
		}

		return true;
	}
#endif
	return false;
}


#ifdef USE_SPLIT
bool RAS_MeshSlot::IsCulled()
{
	if (m_joinSlot)
		return true;
	if (!m_bCulled)
		return false;
	list<RAS_MeshSlot *>::iterator it;
	for (it = m_joinedSlots.begin(); it != m_joinedSlots.end(); it++)
		if (!(*it)->m_bCulled)
			return false;
	return true;
}
#endif
