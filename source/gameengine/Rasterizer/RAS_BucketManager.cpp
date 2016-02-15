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

/** \file gameengine/Rasterizer/RAS_BucketManager.cpp
 *  \ingroup bgerast
 */

#ifdef _MSC_VER
   /* don't show these anoying STL warnings */
#  pragma warning (disable:4786)
#endif

#include "RAS_MaterialBucket.h"
#include "RAS_MeshObject.h"
#include "RAS_Polygon.h"
#include "RAS_IPolygonMaterial.h"
#include "RAS_IRasterizer.h"

#include "RAS_BucketManager.h"

#include <algorithm>
/* sorting */

void RAS_BucketManager::sortedmeshslot::set(RAS_MeshSlot *ms, RAS_MaterialBucket *bucket, const MT_Vector3& pnorm)
{
	// would be good to use the actual bounding box center instead
	MT_Point3 pos(ms->m_OpenGLMatrix[12], ms->m_OpenGLMatrix[13], ms->m_OpenGLMatrix[14]);

	m_z = MT_dot(pnorm, pos);
	m_ms = ms;
	m_bucket = bucket;
}

bool RAS_BucketManager::backtofront::operator()(const sortedmeshslot &a, const sortedmeshslot &b)
{
	return (a.m_z < b.m_z) || (a.m_z == b.m_z && a.m_ms < b.m_ms);
}

bool RAS_BucketManager::fronttoback::operator()(const sortedmeshslot &a, const sortedmeshslot &b)
{
	return (a.m_z > b.m_z) || (a.m_z == b.m_z && a.m_ms > b.m_ms);
}

/* bucket manager */

RAS_BucketManager::RAS_BucketManager()
{

}

RAS_BucketManager::~RAS_BucketManager()
{
	BucketList::iterator it;

	for (unsigned short i = 0; i < NUM_BUCKET_TYPE; ++i) {
		BucketList& buckets = m_buckets[i];
		for (BucketList::iterator it = buckets.begin(), end = buckets.end(); it != end; ++it) {
			delete *it;
		}
		buckets.clear();
	}
}

void RAS_BucketManager::OrderBuckets(const MT_Transform& cameratrans, BucketList& buckets, std::vector<sortedmeshslot>& slots,
									 bool alpha, RAS_IRasterizer *rasty)
{
	BucketList::iterator bit;
	RAS_MeshSlotList::iterator mit;
	size_t size = 0, i = 0;

	/* Camera's near plane equation: pnorm.dot(point) + pval,
	 * but we leave out pval since it's constant anyway */
	const MT_Vector3 pnorm(cameratrans.getBasis()[2]);

	for (bit = buckets.begin(); bit != buckets.end(); ++bit) {
		RAS_DisplayArrayBucketList& displayArrayBucketList = (*bit)->GetDisplayArrayBucketList();
		for (RAS_DisplayArrayBucketList::iterator dbit = displayArrayBucketList.begin(), dbend = displayArrayBucketList.end();
			 dbit != dbend; ++dbit)
		{
			size += (*dbit)->GetNumActiveMeshSlots();
		}
	}

	slots.resize(size);

	for (bit = buckets.begin(); bit != buckets.end(); ++bit)
	{
		RAS_MaterialBucket* bucket = *bit;
		RAS_DisplayArrayBucketList& displayArrayBucketList = (*bit)->GetDisplayArrayBucketList();
		for (RAS_DisplayArrayBucketList::iterator dbit = displayArrayBucketList.begin(), dbend = displayArrayBucketList.end();
			 dbit != dbend; ++dbit)
		{
			RAS_DisplayArrayBucket *displayArrayBucket = *dbit;
			RAS_MeshSlotList& activeMeshSlots = displayArrayBucket->GetActiveMeshSlots();

			// Update deformer and render settings.
			displayArrayBucket->UpdateActiveMeshSlots(rasty);

			for (RAS_MeshSlotList::iterator it = activeMeshSlots.begin(), end = activeMeshSlots.end(); it != end; ++it) {
				slots[i++].set(*it, bucket, pnorm);
			}
			displayArrayBucket->RemoveActiveMeshSlots();
		}
	}
		
	if (alpha)
		sort(slots.begin(), slots.end(), backtofront());
	else
		sort(slots.begin(), slots.end(), fronttoback());
}

void RAS_BucketManager::RenderSortedBuckets(const MT_Transform& cameratrans, RAS_IRasterizer* rasty, RAS_BucketManager::BucketType bucketType)
{
	std::vector<sortedmeshslot> slots;
	std::vector<sortedmeshslot>::iterator sit;

	OrderBuckets(cameratrans, m_buckets[bucketType], slots, true, rasty);

	// The last display array and material bucket used to avoid double calls.
	RAS_DisplayArrayBucket *lastDisplayArrayBucket = NULL;
	RAS_MaterialBucket *lastMaterialBucket = NULL;

	bool matactivated = false;

	for (sit=slots.begin(); sit!=slots.end(); ++sit) {
		RAS_MaterialBucket *bucket = sit->m_bucket;
		RAS_DisplayArrayBucket *displayArrayBucket = sit->m_ms->m_displayArrayBucket;

		/* Unbind display array here before unset material to use the proper
		 * number of attributs in RAS_IStorage::Unbind since this variable is
		 * global and mutated by all material during its activation.
		 */
		if (displayArrayBucket != lastDisplayArrayBucket) {
			rasty->UnbindPrimitives(lastDisplayArrayBucket);
		}
		if (bucket != lastMaterialBucket) {
			if (matactivated) {
				lastMaterialBucket->DesactivateMaterial(rasty);
			}
			matactivated = bucket->ActivateMaterial(rasty);
			lastMaterialBucket = bucket;
		}

		/* Bind the new display array here after material activation to use
		 * proper attributs numbers, same as display array unbind before.
		 */
		if (displayArrayBucket != lastDisplayArrayBucket) {
			rasty->BindPrimitives(displayArrayBucket);
			lastDisplayArrayBucket = displayArrayBucket;
		}

		bucket->RenderMeshSlot(cameratrans, rasty, sit->m_ms);
	}

	if (matactivated && lastMaterialBucket) {
		lastMaterialBucket->DesactivateMaterial(rasty);
	}
	rasty->UnbindPrimitives(lastDisplayArrayBucket);
}

void RAS_BucketManager::RenderBasicBuckets(const MT_Transform& cameratrans, RAS_IRasterizer* rasty, RAS_BucketManager::BucketType bucketType)
{
	BucketList& solidBuckets = m_buckets[bucketType];
	for (BucketList::iterator bit = solidBuckets.begin(); bit != solidBuckets.end(); ++bit) {
		RAS_MaterialBucket* bucket = *bit;
		bucket->RenderMeshSlots(cameratrans, rasty);
	}
}

void RAS_BucketManager::Renderbuckets(const MT_Transform& cameratrans, RAS_IRasterizer* rasty)
{
	bool isShadow = rasty->GetDrawingMode() == RAS_IRasterizer::RAS_SHADOW;

	rasty->SetDepthMask(RAS_IRasterizer::RAS_DEPTHMASK_ENABLED);

	RenderBasicBuckets(cameratrans, rasty, SOLID_BUCKET);
	RenderBasicBuckets(cameratrans, rasty, SOLID_INSTANCING_BUCKET);

	// Having depth masks disabled/enabled gives different artifacts in
	// case no sorting is done or is done inexact. For compatibility, we
	// disable it.
	if (isShadow) {
		rasty->SetDepthMask(RAS_IRasterizer::RAS_DEPTHMASK_DISABLED);
	}

	RenderBasicBuckets(cameratrans, rasty, ALPHA_INSTANCING_BUCKET);
	RenderSortedBuckets(cameratrans, rasty, ALPHA_BUCKET);

	rasty->SetDepthMask(RAS_IRasterizer::RAS_DEPTHMASK_ENABLED);

	/* If we're drawing shadows and bucket wasn't rendered (outside of the lamp frustum or doesn't cast shadows)
	 * then the mesh is still modified, so we don't want to set MeshModified to false yet (it will mess up
	 * updating display lists). Just leave this step for the main render pass.
	 */
	if (!isShadow) {
		/* All meshes should be up to date now */
		/* Don't do this while processing buckets because some meshes are split between buckets */
		for (unsigned short i = 0; i < NUM_BUCKET_TYPE; ++i) {
			BucketList& buckets = m_buckets[i];
			for (BucketList::iterator it = buckets.begin(), end = buckets.end(); it != end; ++it) {
				(*it)->SetMeshUnmodified();
			}
		}
	}
	

	rasty->SetClientObject(NULL);
}

RAS_MaterialBucket *RAS_BucketManager::FindBucket(RAS_IPolyMaterial *material, bool &bucketCreated)
{
	bucketCreated = false;

	for (unsigned short i = 0; i < NUM_BUCKET_TYPE; ++i) {
		BucketList& buckets = m_buckets[i];
		for (BucketList::iterator it = buckets.begin(), end = buckets.end(); it != end; ++it) {
			RAS_MaterialBucket *bucket = *it;
			if (bucket->GetPolyMaterial() == material) {
				return bucket;
			}
		}
	}
	
	RAS_MaterialBucket *bucket = new RAS_MaterialBucket(material);
	bucketCreated = true;

	const bool useinstancing = bucket->UseInstancing();
	std::cout << "useinstancing : " << useinstancing << std::endl;
	if (bucket->IsAlpha())
		m_buckets[useinstancing ? ALPHA_INSTANCING_BUCKET : ALPHA_BUCKET].push_back(bucket);
	else
		m_buckets[useinstancing ? SOLID_INSTANCING_BUCKET : SOLID_BUCKET].push_back(bucket);
	
	return bucket;
}

void RAS_BucketManager::OptimizeBuckets(MT_Scalar distance)
{
	distance = 10.0f;

	for (unsigned short i = 0; i < NUM_BUCKET_TYPE; ++i) {
		BucketList& buckets = m_buckets[i];
		for (BucketList::iterator it = buckets.begin(), end = buckets.end(); it != end; ++it) {
			(*it)->Optimize(distance);
		}
	}
}

void RAS_BucketManager::ReleaseDisplayLists(RAS_IPolyMaterial *mat)
{
	BucketList::iterator bit;
	RAS_MeshSlotList::iterator mit;

	for (unsigned short i = 0; i < NUM_BUCKET_TYPE; ++i) {
		BucketList& buckets = m_buckets[i];
		for (BucketList::iterator it = buckets.begin(), end = buckets.end(); it != end; ++it) {
			RAS_MaterialBucket *bucket = *it;
			if (bucket->GetPolyMaterial() != mat && mat) {
				continue;
			}
			RAS_DisplayArrayBucketList& displayArrayBucketList = bucket->GetDisplayArrayBucketList();
			for (RAS_DisplayArrayBucketList::iterator dit = displayArrayBucketList.begin(), dend = displayArrayBucketList.end();
				dit != dend; ++dit)
			{
				(*dit)->DestructStorageInfo();
			}
		}
	}
}

void RAS_BucketManager::ReleaseMaterials(RAS_IPolyMaterial * mat)
{
	for (unsigned short i = 0; i < NUM_BUCKET_TYPE; ++i) {
		BucketList& buckets = m_buckets[i];
		for (BucketList::iterator it = buckets.begin(), end = buckets.end(); it != end; ++it) {
			RAS_MaterialBucket *bucket = *it;
			if (mat == NULL || (mat == bucket->GetPolyMaterial())) {
				bucket->GetPolyMaterial()->ReleaseMaterial();
			}
		}
	}
}

/* frees the bucket, only used when freeing scenes */
void RAS_BucketManager::RemoveMaterial(RAS_IPolyMaterial * mat)
{
	for (unsigned short i = 0; i < NUM_BUCKET_TYPE; ++i) {
		BucketList& buckets = m_buckets[i];
		for (BucketList::iterator it = buckets.begin(), end = buckets.end(); it != end;) {
			RAS_MaterialBucket *bucket = *it;
			if (mat == bucket->GetPolyMaterial()) {
				buckets.erase(it++);
				delete bucket;
			}
			else {
				++it;
			}
		}
	}
}

//#include <stdio.h>

void RAS_BucketManager::MergeBucketManager(RAS_BucketManager *other, SCA_IScene *scene)
{
	/* concatenate lists */
	// printf("BEFORE %d %d\n", GetSolidBuckets().size(), GetAlphaBuckets().size());

	GetSolidBuckets().insert( GetSolidBuckets().end(), other->GetSolidBuckets().begin(), other->GetSolidBuckets().end() );
	other->GetSolidBuckets().clear();

	GetAlphaBuckets().insert( GetAlphaBuckets().end(), other->GetAlphaBuckets().begin(), other->GetAlphaBuckets().end() );
	other->GetAlphaBuckets().clear();
	//printf("AFTER %d %d\n", GetSolidBuckets().size(), GetAlphaBuckets().size());
}

