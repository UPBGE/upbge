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
#include "RAS_MeshUser.h"
#include "RAS_Polygon.h"
#include "RAS_IPolygonMaterial.h"
#include "RAS_IRasterizer.h"

#include "RAS_BucketManager.h"

#include <algorithm>
/* sorting */

void RAS_BucketManager::sortedmeshslot::set(RAS_MeshSlot *ms, RAS_MaterialBucket *bucket, const MT_Vector3& pnorm)
{
	// would be good to use the actual bounding box center instead
	float *matrix = ms->m_meshUser->GetMatrix();
	const MT_Vector3 pos(matrix[12], matrix[13], matrix[14]);

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

RAS_BucketManager::RAS_BucketManager()
{
	ClearNumActiveMeshSlotsCache();
}

RAS_BucketManager::~RAS_BucketManager()
{
	BucketList& buckets = m_buckets[ALL_BUCKET];
	for (BucketList::iterator it = buckets.begin(), end = buckets.end(); it != end; ++it) {
		delete *it;
	}
	buckets.clear();
}

unsigned int RAS_BucketManager::GetNumActiveMeshSlots(BucketType bucketType)
{
	int count = m_cachedNumActiveMeshSlots[bucketType];
	if (count != -1) {
		return count;
	}

	count = 0;
	BucketList& buckets = m_buckets[bucketType];
	for (BucketList::iterator it = buckets.begin(), end = buckets.end(); it != end; ++it) {
		count += (*it)->GetNumActiveMeshSlots();
	}

	// Update cache value.
	m_cachedNumActiveMeshSlots[bucketType] = count;

	return count;
}

void RAS_BucketManager::ClearNumActiveMeshSlotsCache()
{
	for (unsigned short i = 0; i < NUM_BUCKET_TYPE; ++i) {
		m_cachedNumActiveMeshSlots[i] = -1;
	}
}

void RAS_BucketManager::OrderBuckets(const MT_Transform& cameratrans, RAS_BucketManager::BucketType bucketType,
                                     std::vector<sortedmeshslot>& slots, bool alpha, RAS_IRasterizer *rasty)
{
	const unsigned int size = GetNumActiveMeshSlots(bucketType);
	// Discard if there's no mesh slots.
	if (size == 0) {
		return;
	}

	size_t i = 0;

	/* Camera's near plane equation: pnorm.dot(point) + pval,
	 * but we leave out pval since it's constant anyway */
	const MT_Vector3 pnorm(cameratrans.getBasis()[2]);

	slots.resize(size);

	BucketList& buckets = m_buckets[bucketType];

	for (BucketList::iterator bit = buckets.begin(); bit != buckets.end(); ++bit)
	{
		RAS_MaterialBucket *bucket = *bit;
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

void RAS_BucketManager::RenderSortedBuckets(const MT_Transform& cameratrans, RAS_IRasterizer *rasty, RAS_BucketManager::BucketType bucketType)
{
	std::vector<sortedmeshslot> slots;
	std::vector<sortedmeshslot>::iterator sit;

	OrderBuckets(cameratrans, bucketType, slots, true, rasty);
	// Discard if there's no mesh slots.
	if (slots.size() == 0) {
		return;
	}

	// The last display array and material bucket used to avoid double calls.
	RAS_DisplayArrayBucket *lastDisplayArrayBucket = NULL;
	RAS_MaterialBucket *lastMaterialBucket = NULL;

	bool matactivated = false;

	for (sit = slots.begin(); sit != slots.end(); ++sit) {
		RAS_MaterialBucket *bucket = sit->m_bucket;
		RAS_DisplayArrayBucket *displayArrayBucket = sit->m_ms->m_displayArrayBucket;

		/* Unbind display array here before unset material to use the proper
		 * number of attributs in RAS_IStorage::Unbind since this variable is
		 * global and mutated by all material during its activation.
		 */
		if (displayArrayBucket != lastDisplayArrayBucket && lastDisplayArrayBucket) {
			rasty->UnbindPrimitives(lastDisplayArrayBucket->GetStorageType(), lastDisplayArrayBucket);
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
			rasty->BindPrimitives(displayArrayBucket->GetStorageType(), displayArrayBucket);
			lastDisplayArrayBucket = displayArrayBucket;
		}

		bucket->RenderMeshSlot(cameratrans, rasty, sit->m_ms);
	}

	// Always unbind VBO or VA before unset the material to use the correct material attributs.
	rasty->UnbindPrimitives(lastDisplayArrayBucket->GetStorageType(), lastDisplayArrayBucket);

	if (matactivated) {
		lastMaterialBucket->DesactivateMaterial(rasty);
	}
}

void RAS_BucketManager::RenderBasicBuckets(const MT_Transform& cameratrans, RAS_IRasterizer *rasty, RAS_BucketManager::BucketType bucketType)
{
	BucketList& solidBuckets = m_buckets[bucketType];
	for (BucketList::iterator bit = solidBuckets.begin(); bit != solidBuckets.end(); ++bit) {
		RAS_MaterialBucket *bucket = *bit;
		bucket->RenderMeshSlots(cameratrans, rasty);
	}
}

void RAS_BucketManager::Renderbuckets(const MT_Transform& cameratrans, RAS_IRasterizer *rasty)
{
	ClearNumActiveMeshSlotsCache();

	switch (rasty->GetDrawingMode()) {
		case RAS_IRasterizer::RAS_SHADOW:
		{
			const bool isVarianceShadow = rasty->GetShadowMode() == RAS_IRasterizer::RAS_SHADOW_VARIANCE;

			rasty->SetDepthMask(RAS_IRasterizer::RAS_DEPTHMASK_ENABLED);

			/* Rendering solid regular materials with an override shader for
			 * variance shadow or an empty shader.
			 */

			if (GetNumActiveMeshSlots(SOLID_SHADOW_BUCKET) != 0) {
				rasty->SetOverrideShader(isVarianceShadow ?
				                         RAS_IRasterizer::RAS_OVERRIDE_SHADER_SHADOW_VARIANCE :
				                         RAS_IRasterizer::RAS_OVERRIDE_SHADER_BASIC);
			}
			RenderBasicBuckets(cameratrans, rasty, SOLID_SHADOW_BUCKET);

			/* Rendering solid instancing materials with a different override
			 * shader for variance and simple shadow.
			 */

			if (GetNumActiveMeshSlots(SOLID_SHADOW_INSTANCING_BUCKET) != 0) {
				rasty->SetOverrideShader(isVarianceShadow ?
				                         RAS_IRasterizer::RAS_OVERRIDE_SHADER_SHADOW_VARIANCE_INSTANCING :
				                         RAS_IRasterizer::RAS_OVERRIDE_SHADER_BASIC_INSTANCING);
			}
			RenderBasicBuckets(cameratrans, rasty, SOLID_SHADOW_INSTANCING_BUCKET);

			if (isVarianceShadow) {
				/* Rendering alpha shadow instancing materials with an override
				 * shader for variance shadow.
				 */

				if (GetNumActiveMeshSlots(ALPHA_SHADOW_INSTANCING_BUCKET) != 0) {
					rasty->SetOverrideShader(RAS_IRasterizer::RAS_OVERRIDE_SHADER_SHADOW_VARIANCE_INSTANCING);
				}
				RenderBasicBuckets(cameratrans, rasty, ALPHA_SHADOW_INSTANCING_BUCKET);

				/* Rendering alpha shadow regular materials with an override
				 * shader for variance shadow and ordering.
				 */

				if (GetNumActiveMeshSlots(ALPHA_SHADOW_BUCKET) != 0) {
					rasty->SetOverrideShader(RAS_IRasterizer::RAS_OVERRIDE_SHADER_SHADOW_VARIANCE);
				}
				RenderSortedBuckets(cameratrans, rasty, ALPHA_SHADOW_BUCKET);

				rasty->SetOverrideShader(RAS_IRasterizer::RAS_OVERRIDE_SHADER_NONE);
			}
			else {
				// Rendering alpha shadow materials (including instancing) with their shaders.

				rasty->SetOverrideShader(RAS_IRasterizer::RAS_OVERRIDE_SHADER_NONE);

				RenderBasicBuckets(cameratrans, rasty, ALPHA_SHADOW_INSTANCING_BUCKET);
				// Render alpha shadow regular materials with ordering.
				RenderSortedBuckets(cameratrans, rasty, ALPHA_SHADOW_BUCKET);
			}

			break;
		}
		case RAS_IRasterizer::RAS_WIREFRAME:
		{
			rasty->SetLines(true);
			rasty->SetDepthMask(RAS_IRasterizer::RAS_DEPTHMASK_ENABLED);

			// Rendering solid regular materials with an empty override shader.

			if (GetNumActiveMeshSlots(SOLID_BUCKET) != 0) {
				rasty->SetOverrideShader(RAS_IRasterizer::RAS_OVERRIDE_SHADER_BASIC);
			}
			RenderBasicBuckets(cameratrans, rasty, SOLID_BUCKET);

			/* Rendering solid, alpha and alpha depth instancing materials
			 * with an override shader.
			 */

			if ((GetNumActiveMeshSlots(SOLID_INSTANCING_BUCKET) +
				GetNumActiveMeshSlots(ALPHA_INSTANCING_BUCKET) +
				GetNumActiveMeshSlots(ALPHA_DEPTH_INSTANCING_BUCKET)) != 0) {
				rasty->SetOverrideShader(RAS_IRasterizer::RAS_OVERRIDE_SHADER_BASIC_INSTANCING);
			}
			RenderBasicBuckets(cameratrans, rasty, SOLID_INSTANCING_BUCKET);

			rasty->SetDepthMask(RAS_IRasterizer::RAS_DEPTHMASK_DISABLED);

			RenderBasicBuckets(cameratrans, rasty, ALPHA_INSTANCING_BUCKET);
			RenderBasicBuckets(cameratrans, rasty, ALPHA_DEPTH_INSTANCING_BUCKET);

			/* Rendering alpha and alpha depth regular materials with
			 * an empty shader and ordering.
			 */

			if ((GetNumActiveMeshSlots(ALPHA_BUCKET) + GetNumActiveMeshSlots(ALPHA_DEPTH_BUCKET)) != 0) {
				rasty->SetOverrideShader(RAS_IRasterizer::RAS_OVERRIDE_SHADER_BASIC);
			}
			RenderSortedBuckets(cameratrans, rasty, ALPHA_BUCKET);
			RenderSortedBuckets(cameratrans, rasty, ALPHA_DEPTH_BUCKET);

			rasty->SetOverrideShader(RAS_IRasterizer::RAS_OVERRIDE_SHADER_NONE);

			rasty->SetLines(false);
			rasty->SetDepthMask(RAS_IRasterizer::RAS_DEPTHMASK_ENABLED);
			break;
		}
		case RAS_IRasterizer::RAS_SOLID:
		case RAS_IRasterizer::RAS_TEXTURED:
		{
			/* Rendering solid and alpha (regular and instancing) materials
			 * with their shaders.
			 */

			rasty->SetDepthMask(RAS_IRasterizer::RAS_DEPTHMASK_ENABLED);

			RenderBasicBuckets(cameratrans, rasty, SOLID_BUCKET);
			RenderBasicBuckets(cameratrans, rasty, SOLID_INSTANCING_BUCKET);

			rasty->SetDepthMask(RAS_IRasterizer::RAS_DEPTHMASK_DISABLED);

			RenderBasicBuckets(cameratrans, rasty, ALPHA_INSTANCING_BUCKET);
			RenderSortedBuckets(cameratrans, rasty, ALPHA_BUCKET);

			// Render soft particles after all other materials.
			if ((GetNumActiveMeshSlots(ALPHA_DEPTH_BUCKET) + GetNumActiveMeshSlots(ALPHA_DEPTH_INSTANCING_BUCKET)) > 0) {
				rasty->UpdateGlobalDepthTexture();

				RenderBasicBuckets(cameratrans, rasty, ALPHA_DEPTH_INSTANCING_BUCKET);
				RenderSortedBuckets(cameratrans, rasty, ALPHA_DEPTH_BUCKET);
			}

			rasty->SetDepthMask(RAS_IRasterizer::RAS_DEPTHMASK_ENABLED);
			break;
		}
		default:
		{
			break;
		}
	}

	/* If we're drawing shadows and bucket wasn't rendered (outside of the lamp frustum or doesn't cast shadows)
	 * then the mesh is still modified, so we don't want to set MeshModified to false yet (it will mess up
	 * updating display lists). Just leave this step for the main render pass.
	 */
	if (rasty->GetDrawingMode() != RAS_IRasterizer::RAS_SHADOW) {
		/* All meshes should be up to date now */
		/* Don't do this while processing buckets because some meshes are split between buckets */
		BucketList& buckets = m_buckets[ALL_BUCKET];
		for (BucketList::iterator it = buckets.begin(), end = buckets.end(); it != end; ++it) {
			(*it)->SetMeshUnmodified();
		}
	}

	rasty->SetClientObject(NULL);
}

RAS_MaterialBucket *RAS_BucketManager::FindBucket(RAS_IPolyMaterial *material, bool &bucketCreated)
{
	bucketCreated = false;

	BucketList& buckets = m_buckets[ALL_BUCKET];
	for (BucketList::iterator it = buckets.begin(), end = buckets.end(); it != end; ++it) {
		RAS_MaterialBucket *bucket = *it;
		if (bucket->GetPolyMaterial() == material) {
			return bucket;
		}
	}

	RAS_MaterialBucket *bucket = new RAS_MaterialBucket(material);
	bucketCreated = true;

	const bool useinstancing = material->UseInstancing();
	if (!material->OnlyShadow()) {
		if (material->IsAlpha()) {
			if (material->IsAlphaDepth()) {
				m_buckets[useinstancing ? ALPHA_DEPTH_INSTANCING_BUCKET : ALPHA_DEPTH_BUCKET].push_back(bucket);
			}
			else {
				m_buckets[useinstancing ? ALPHA_INSTANCING_BUCKET : ALPHA_BUCKET].push_back(bucket);
			}
		}
		else {
			m_buckets[useinstancing ? SOLID_INSTANCING_BUCKET : SOLID_BUCKET].push_back(bucket);
		}
	}
	if (material->CastsShadows()) {
		if (material->IsAlphaShadow()) {
			m_buckets[useinstancing ? ALPHA_SHADOW_INSTANCING_BUCKET : ALPHA_SHADOW_BUCKET].push_back(bucket);
		}
		else {
			m_buckets[useinstancing ? SOLID_SHADOW_INSTANCING_BUCKET : SOLID_SHADOW_BUCKET].push_back(bucket);
		}
	}
	if (material->IsText()) {
		m_buckets[TEXT_BUCKET].push_back(bucket);
	}

	// Used to free the bucket.
	m_buckets[ALL_BUCKET].push_back(bucket);
	return bucket;
}

void RAS_BucketManager::ReleaseDisplayLists(RAS_IPolyMaterial *mat)
{
	BucketList& buckets = m_buckets[ALL_BUCKET];
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

void RAS_BucketManager::ReleaseMaterials(RAS_IPolyMaterial *mat)
{
	BucketList& buckets = m_buckets[ALL_BUCKET];
	for (BucketList::iterator it = buckets.begin(), end = buckets.end(); it != end; ++it) {
		RAS_MaterialBucket *bucket = *it;
		if (mat == NULL || (mat == bucket->GetPolyMaterial())) {
			bucket->GetPolyMaterial()->ReleaseMaterial();
		}
	}
}

/* frees the bucket, only used when freeing scenes */
void RAS_BucketManager::RemoveMaterial(RAS_IPolyMaterial *mat)
{
	for (unsigned short i = 0; i < NUM_BUCKET_TYPE; ++i) {
		BucketList& buckets = m_buckets[i];
		for (BucketList::iterator it = buckets.begin(); it != buckets.end();) {
			RAS_MaterialBucket *bucket = *it;
			if (mat == bucket->GetPolyMaterial()) {
				it = buckets.erase(it);
				if (i == ALL_BUCKET) {
					delete bucket;
				}
			}
			else {
				++it;
			}
		}
	}
}

void RAS_BucketManager::MergeBucketManager(RAS_BucketManager *other, SCA_IScene *scene)
{
	for (unsigned short i = 0; i < NUM_BUCKET_TYPE; ++i) {
		BucketList& buckets = m_buckets[i];
		BucketList& otherbuckets = other->m_buckets[i];
		buckets.insert(buckets.begin(), otherbuckets.begin(), otherbuckets.end());
		otherbuckets.clear();
	}
}
