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

#ifdef _MSC_VER
#  pragma warning (disable:4786)
#endif

#ifdef WIN32
#include <windows.h>
#endif // WIN32

#include "RAS_IPolygonMaterial.h"
#include "RAS_TexVert.h"
#include "RAS_IRasterizer.h"
#include "RAS_MeshObject.h"
#include "RAS_Deformer.h"	// __NLA

/* mesh slot */

RAS_MeshSlot::RAS_MeshSlot() : SG_QList()
{
	m_clientObj = NULL;
	m_pDeformer = NULL;
	m_OpenGLMatrix = NULL;
	m_mesh = NULL;
	m_bucket = NULL;
	m_bVisible = false;
	m_bCulled = true;
	m_bObjectColor = false;
	m_RGBAcolor = MT_Vector4(0.0, 0.0, 0.0, 0.0);
	m_DisplayList = NULL;
	m_bDisplayList = true;
	m_joinSlot = NULL;
	m_pDerivedMesh = NULL;
	m_displayArray = NULL;
}

RAS_MeshSlot::~RAS_MeshSlot()
{
#ifdef USE_SPLIT
	Split(true);

	while (m_joinedSlots.size())
		m_joinedSlots.front()->Split(true);
#endif

	if (m_displayArray) {
		m_displayArray->m_users--;
		if (m_displayArray->m_users == 0) {
			delete m_displayArray;
		}
	}

	if (m_DisplayList) {
		m_DisplayList->Release();
		m_DisplayList = NULL;
	}
}

RAS_MeshSlot::RAS_MeshSlot(const RAS_MeshSlot& slot) : SG_QList()
{
	m_clientObj = NULL;
	m_pDeformer = NULL;
	m_pDerivedMesh = NULL;
	m_OpenGLMatrix = NULL;
	m_mesh = slot.m_mesh;
	m_bucket = slot.m_bucket;
	m_bVisible = slot.m_bVisible;
	m_bCulled = slot.m_bCulled;
	m_bObjectColor = slot.m_bObjectColor;
	m_RGBAcolor = slot.m_RGBAcolor;
	m_DisplayList = NULL;
	m_bDisplayList = slot.m_bDisplayList;
	m_joinSlot = NULL;
	m_displayArray = slot.m_displayArray;
	m_joinedSlots = slot.m_joinedSlots;

	// don't copy display arrays for now because it breaks python 
	// access to vertices, but we'll need a solution if we want to
	// join display arrays for reducing draw calls.
	//*it = new RAS_DisplayArray(**it);
	//(*it)->m_users = 1;

	if (m_displayArray) {
		m_displayArray->m_users++;
	}
}

void RAS_MeshSlot::init(RAS_MaterialBucket *bucket)
{
	m_bucket = bucket;

	m_displayArray = new RAS_DisplayArray();
	m_displayArray->m_users = 1;
}

RAS_DisplayArray *RAS_MeshSlot::GetDisplayArray()
{
	return m_displayArray;
}

int RAS_MeshSlot::AddVertex(const RAS_TexVert& tv)
{
	RAS_DisplayArray *darray;
	int offset;
	
	darray = m_displayArray;
	darray->m_vertex.push_back(tv);
	offset = darray->m_vertex.size()-1;

	return offset;
}

void RAS_MeshSlot::AddPolygonVertex(int offset)
{
	RAS_DisplayArray *darray;

	darray = m_displayArray;
	darray->m_index.push_back(offset);
}

void RAS_MeshSlot::SetDeformer(RAS_Deformer* deformer)
{
	if (deformer && m_pDeformer != deformer) {
		if (deformer->ShareVertexArray()) {
			// this deformer uses the base vertex array, first release the current ones
			m_displayArray->m_users--;
			if (m_displayArray->m_users == 0) {
				delete m_displayArray;
			}
			m_displayArray = NULL;
			// then hook to the base ones
			RAS_MeshMaterial *mmat = m_mesh->GetMeshMaterial(m_bucket->GetPolyMaterial());
			if (mmat && mmat->m_baseslot) {
				m_displayArray = mmat->m_baseslot->GetDisplayArray();
				m_displayArray->m_users++;
			}
		}
		else {
			// no sharing
			// we create local copy of RAS_DisplayArray when we have a deformer:
			// this way we can avoid conflict between the vertex cache of duplicates
			if (deformer->UseVertexArray()) {
				// the deformer makes use of vertex array, make sure we have our local copy
				if (m_displayArray->m_users > 1) {
					// only need to copy if there are other users
					// note that this is the usual case as vertex arrays are held by the material base slot
					RAS_DisplayArray *newarray = new RAS_DisplayArray(*m_displayArray);
					newarray->m_users = 1;
					m_displayArray->m_users--;
					m_displayArray = newarray;
				}
			}
			else {
				// the deformer is not using vertex array (Modifier), release them
				m_displayArray->m_users--;
				if (m_displayArray->m_users == 0) {
					delete m_displayArray;
				}
				m_displayArray = NULL;
			}
		}
	}
	m_pDeformer = deformer;
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

	MT_Matrix4x4 transform = targetmat*mat;
	
	// m_mesh, clientobj
	m_joinSlot = target;
	m_joinInvTransform = transform;
	m_joinInvTransform.invert();
	target->m_joinedSlots.push_back(this);

	MT_Matrix4x4 ntransform = m_joinInvTransform.transposed();
	ntransform[0][3] = ntransform[1][3] = ntransform[2][3] = 0.0f;

	for (begin(mit); !end(mit); next(mit))
		for (i=mit.startvertex; i<mit.endvertex; i++)
			mit.vertex[i].Transform(transform, ntransform);
	
	/* We know we'll need a list at least this big, reserve in advance */
	target->m_displayArrays.reserve(target->m_displayArrays.size() + m_displayArrays.size());

	for (it=m_displayArrays.begin(); it!=m_displayArrays.end(); it++) {
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
	list<RAS_MeshSlot*>::iterator jit;
	RAS_MeshSlot *target = m_joinSlot;
	RAS_DisplayArrayList::iterator it, jt;
	iterator mit;
	size_t i, found0 = 0, found1 = 0;

	if (target && (force || !Equals(target))) {
		m_joinSlot = NULL;

		for (jit=target->m_joinedSlots.begin(); jit!=target->m_joinedSlots.end(); jit++) {
			if (*jit == this) {
				target->m_joinedSlots.erase(jit);
				found0 = 1;
				break;
			}
		}

		if (!found0)
			abort();

		for (it=m_displayArrays.begin(); it!=m_displayArrays.end(); it++) {
			found1 = 0;
			for (jt=target->m_displayArrays.begin(); jt!=target->m_displayArrays.end(); jt++) {
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
			for (i=mit.startvertex; i<mit.endvertex; i++)
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
	list<RAS_MeshSlot*>::iterator it;
	for (it=m_joinedSlots.begin(); it!=m_joinedSlots.end(); it++)
		if (!(*it)->m_bCulled)
			return false;
	return true;
}
#endif

/* material bucket sorting */

struct RAS_MaterialBucket::less
{
	bool operator()(const RAS_MaterialBucket* x, const RAS_MaterialBucket* y) const 
	{ 
		return *x->GetPolyMaterial() < *y->GetPolyMaterial(); 
	}
};

/* material bucket */

RAS_MaterialBucket::RAS_MaterialBucket(RAS_IPolyMaterial* mat)
{
	m_material = mat;
}

RAS_MaterialBucket::~RAS_MaterialBucket()
{
}

RAS_IPolyMaterial* RAS_MaterialBucket::GetPolyMaterial() const
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

RAS_MeshSlot* RAS_MaterialBucket::AddMesh()
{
	RAS_MeshSlot *ms;

	m_meshSlots.push_back(RAS_MeshSlot());
	
	ms = &m_meshSlots.back();
	ms->init(this);

	return ms;
}

RAS_MeshSlot* RAS_MaterialBucket::CopyMesh(RAS_MeshSlot *ms)
{
	m_meshSlots.push_back(RAS_MeshSlot(*ms));
	
	return &m_meshSlots.back();
}

void RAS_MaterialBucket::RemoveMesh(RAS_MeshSlot* ms)
{
	list<RAS_MeshSlot>::iterator it;

	for (it=m_meshSlots.begin(); it!=m_meshSlots.end(); it++) {
		if (&*it == ms) {
			m_meshSlots.erase(it);
			return;
		}
	}
}

list<RAS_MeshSlot>::iterator RAS_MaterialBucket::msBegin()
{
	return m_meshSlots.begin();
}

list<RAS_MeshSlot>::iterator RAS_MaterialBucket::msEnd()
{
	return m_meshSlots.end();
}

bool RAS_MaterialBucket::ActivateMaterial(const MT_Transform& cameratrans, RAS_IRasterizer* rasty)
{
	bool uselights;
	
	if (rasty->GetDrawingMode() == RAS_IRasterizer::KX_SHADOW && !m_material->CastsShadows())
		return false;

	if (rasty->GetDrawingMode() != RAS_IRasterizer::KX_SHADOW && m_material->OnlyShadow())
		return false;

	if (!rasty->SetMaterial(*m_material))
		return false;
	
	uselights= m_material->UsesLighting(rasty);
	rasty->ProcessLighting(uselights, cameratrans);
	
	return true;
}

void RAS_MaterialBucket::RenderMeshSlot(const MT_Transform& cameratrans, RAS_IRasterizer* rasty, RAS_MeshSlot &ms)
{
	m_material->ActivateMeshSlot(ms, rasty);

	if (ms.m_pDeformer)
	{
		if (ms.m_pDeformer->Apply(m_material))
			ms.m_mesh->SetMeshModified(true);
	//	KX_ReInstanceShapeFromMesh(ms.m_mesh); // Recompute the physics mesh. (Can't call KX_* from RAS_)
	}
	
	if (IsZSort() && rasty->GetDrawingMode() >= RAS_IRasterizer::KX_SOLID)
		ms.m_mesh->SortPolygons(ms, cameratrans*MT_Transform(ms.m_OpenGLMatrix));

	rasty->PushMatrix();
	if (!ms.m_pDeformer || !ms.m_pDeformer->SkipVertexTransform())
	{
		rasty->applyTransform(ms.m_OpenGLMatrix,m_material->GetDrawingMode());
	}

	if (rasty->QueryLists())
		if (ms.m_DisplayList)
			ms.m_DisplayList->SetModified(ms.m_mesh->MeshModified());

	// verify if we can use display list, not for deformed object, and
	// also don't create a new display list when drawing shadow buffers,
	// then it won't have texture coordinates for actual drawing. also
	// for zsort we can't make a display list, since the polygon order
	// changes all the time.
	if (ms.m_pDeformer && ms.m_pDeformer->IsDynamic())
		ms.m_bDisplayList = false;
	else if (!ms.m_DisplayList && rasty->GetDrawingMode() == RAS_IRasterizer::KX_SHADOW)
		ms.m_bDisplayList = false;
	else if (IsZSort())
		ms.m_bDisplayList = false;
	else if (m_material->UsesObjectColor() && ms.m_bObjectColor)
		ms.m_bDisplayList = false;
	else
		ms.m_bDisplayList = true;

	// for text drawing using faces
	if (m_material->GetDrawingMode() & RAS_IRasterizer::RAS_RENDER_3DPOLYGON_TEXT)
		rasty->IndexPrimitives_3DText(ms, m_material);
	// For multitexturing, blender glsl material and custom glsl shader.
	else {
		rasty->IndexPrimitives(ms);
	}

	rasty->PopMatrix();
}

void RAS_MaterialBucket::Optimize(MT_Scalar distance)
{
	/* TODO: still have to check before this works correct:
	 * - lightlayer, frontface, text, billboard
	 * - make it work with physics */
	
#if 0
	list<RAS_MeshSlot>::iterator it;
	list<RAS_MeshSlot>::iterator jt;

	// greed joining on all following buckets
	for (it=m_meshSlots.begin(); it!=m_meshSlots.end(); it++)
		for (jt=it, jt++; jt!=m_meshSlots.end(); jt++)
			jt->Join(&*it, distance);
#endif
}

