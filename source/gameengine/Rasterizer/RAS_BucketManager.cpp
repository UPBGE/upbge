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
#include "RAS_Rasterizer.h"

#include "RAS_BucketManager.h"

#include <algorithm>
/* sorting */

RAS_BucketManager::SortedMeshSlot::SortedMeshSlot(RAS_MeshSlot *ms, const MT_Vector3& pnorm)
	:m_ms(ms)
{
	// would be good to use the actual bounding box center instead
	float *matrix = m_ms->m_meshUser->GetMatrix();
	const MT_Vector3 pos(matrix[12], matrix[13], matrix[14]);

	m_z = MT_dot(pnorm, pos);
}

RAS_BucketManager::SortedMeshSlot::SortedMeshSlot(RAS_MeshSlotUpwardNode *node, const MT_Vector3& pnorm)
	:m_node(node)
{
	RAS_MeshSlot *ms = m_node->GetInfo();
	// would be good to use the actual bounding box center instead
	float *matrix = ms->m_meshUser->GetMatrix();
	const MT_Vector3 pos(matrix[12], matrix[13], matrix[14]);

	m_z = MT_dot(pnorm, pos);
}

bool RAS_BucketManager::backtofront::operator()(const SortedMeshSlot &a, const SortedMeshSlot &b)
{
	return (a.m_z < b.m_z) || (a.m_z == b.m_z && a.m_ms < b.m_ms);
}

bool RAS_BucketManager::fronttoback::operator()(const SortedMeshSlot &a, const SortedMeshSlot &b)
{
	return (a.m_z > b.m_z) || (a.m_z == b.m_z && a.m_ms > b.m_ms);
}

RAS_BucketManager::RAS_BucketManager()
	:m_downwardNode(this, nullptr, nullptr),
	m_upwardNode(this, nullptr, nullptr)
{
}

RAS_BucketManager::~RAS_BucketManager()
{
	BucketList& buckets = m_buckets[ALL_BUCKET];
	for (BucketList::iterator it = buckets.begin(), end = buckets.end(); it != end; ++it) {
		delete *it;
	}
	buckets.clear();
}

void RAS_BucketManager::RenderSortedBuckets(const MT_Transform& cameratrans, RAS_Rasterizer *rasty, RAS_BucketManager::BucketType bucketType)
{
	BucketList& solidBuckets = m_buckets[bucketType];
	RAS_UpwardTreeLeafs leafs;
	for (RAS_MaterialBucket *bucket : solidBuckets) {
		bucket->GenerateTree(&m_downwardNode, &m_upwardNode, &leafs, rasty, true);
	}

	RAS_RenderNodeArguments args(cameratrans, rasty, true, rasty->GetOverrideShader() != RAS_Rasterizer::RAS_OVERRIDE_SHADER_NONE);
	if (m_downwardNode.GetValid()) {
		m_downwardNode.Execute(args);
	}
	if (leafs.size() > 0) {
		/* Camera's near plane equation: pnorm.dot(point) + pval,
		 * but we leave out pval since it's constant anyway */
		const MT_Vector3 pnorm(cameratrans.getBasis()[2]);
		std::vector<SortedMeshSlot> sortedSlots(leafs.size());
		// Generate all SortedMeshSlot corresponding to all the leafs nodes.
		std::transform(leafs.begin(), leafs.end(), sortedSlots.begin(),
				[&pnorm](RAS_MeshSlotUpwardNode *node) { return SortedMeshSlot(node, pnorm); });

		std::sort(sortedSlots.begin(), sortedSlots.end(), backtofront());

		RAS_MeshSlotUpwardNodeIterator visitor;
		for (const SortedMeshSlot& sortedSlot : sortedSlots) {
			visitor.NextNode(sortedSlot.m_node, args);
		}
		visitor.Unbind(args);
	}
}

void RAS_BucketManager::RenderBasicBuckets(const MT_Transform& cameratrans, RAS_Rasterizer *rasty, RAS_BucketManager::BucketType bucketType)
{
	BucketList& solidBuckets = m_buckets[bucketType];
	for (RAS_MaterialBucket *bucket : solidBuckets) {
		bucket->GenerateTree(&m_downwardNode, nullptr, nullptr, rasty, false);
	}

	if (m_downwardNode.GetValid()) {
		RAS_RenderNodeArguments args(cameratrans, rasty, false, rasty->GetOverrideShader() != RAS_Rasterizer::RAS_OVERRIDE_SHADER_NONE);
		m_downwardNode.Execute(args);
	}
}

void RAS_BucketManager::Renderbuckets(const MT_Transform& cameratrans, RAS_Rasterizer *rasty, RAS_OffScreen *offScreen)
{
	switch (rasty->GetDrawingMode()) {
		case RAS_Rasterizer::RAS_SHADOW:
		{
			const bool isVarianceShadow = rasty->GetShadowMode() == RAS_Rasterizer::RAS_SHADOW_VARIANCE;

			rasty->SetDepthMask(RAS_Rasterizer::RAS_DEPTHMASK_ENABLED);

			/* Rendering solid regular materials with an override shader for
			 * variance shadow or an empty shader.
			 */

			if (m_buckets[SOLID_SHADOW_BUCKET].size() > 0) {
				rasty->SetOverrideShader(isVarianceShadow ?
				                         RAS_Rasterizer::RAS_OVERRIDE_SHADER_SHADOW_VARIANCE :
				                         RAS_Rasterizer::RAS_OVERRIDE_SHADER_BLACK);
			}
			RenderBasicBuckets(cameratrans, rasty, SOLID_SHADOW_BUCKET);

			/* Rendering solid instancing materials with a different override
			 * shader for variance and simple shadow.
			 */

			if (m_buckets[SOLID_SHADOW_INSTANCING_BUCKET].size() > 0) {
				rasty->SetOverrideShader(isVarianceShadow ?
				                         RAS_Rasterizer::RAS_OVERRIDE_SHADER_SHADOW_VARIANCE_INSTANCING :
				                         RAS_Rasterizer::RAS_OVERRIDE_SHADER_BLACK_INSTANCING);
			}
			RenderBasicBuckets(cameratrans, rasty, SOLID_SHADOW_INSTANCING_BUCKET);

			if (isVarianceShadow) {
				/* Rendering alpha shadow instancing materials with an override
				 * shader for variance shadow.
				 */

				if (m_buckets[ALPHA_SHADOW_INSTANCING_BUCKET].size() > 0) {
					rasty->SetOverrideShader(RAS_Rasterizer::RAS_OVERRIDE_SHADER_SHADOW_VARIANCE_INSTANCING);
				}
				RenderBasicBuckets(cameratrans, rasty, ALPHA_SHADOW_INSTANCING_BUCKET);

				/* Rendering alpha shadow regular materials with an override
				 * shader for variance shadow and ordering.
				 */

				if (m_buckets[ALPHA_SHADOW_BUCKET].size() > 0) {
					rasty->SetOverrideShader(RAS_Rasterizer::RAS_OVERRIDE_SHADER_SHADOW_VARIANCE);
				}
				RenderBasicBuckets(cameratrans, rasty, ALPHA_SHADOW_BUCKET);

				rasty->SetOverrideShader(RAS_Rasterizer::RAS_OVERRIDE_SHADER_NONE);
			}
			else {
				// Rendering alpha shadow materials (including instancing) with their shaders.

				rasty->SetOverrideShader(RAS_Rasterizer::RAS_OVERRIDE_SHADER_NONE);

				RenderBasicBuckets(cameratrans, rasty, ALPHA_SHADOW_INSTANCING_BUCKET);
				// Render alpha shadow regular materials with ordering.
				RenderBasicBuckets(cameratrans, rasty, ALPHA_SHADOW_BUCKET);
			}

			break;
		}
		case RAS_Rasterizer::RAS_WIREFRAME:
		{
			rasty->SetLines(true);
			rasty->SetDepthMask(RAS_Rasterizer::RAS_DEPTHMASK_ENABLED);

			// Rendering solid regular materials with an empty override shader.

			if (m_buckets[SOLID_BUCKET].size() > 0) {
				rasty->SetOverrideShader(RAS_Rasterizer::RAS_OVERRIDE_SHADER_BLACK);
			}
			RenderBasicBuckets(cameratrans, rasty, SOLID_BUCKET);

			/* Rendering solid, alpha and alpha depth instancing materials
			 * with an override shader.
			 */

			if ((m_buckets[SOLID_INSTANCING_BUCKET].size() +
				m_buckets[ALPHA_INSTANCING_BUCKET].size() +
				m_buckets[ALPHA_DEPTH_INSTANCING_BUCKET].size()) != 0) {
				rasty->SetOverrideShader(RAS_Rasterizer::RAS_OVERRIDE_SHADER_BLACK_INSTANCING);
			}
			RenderBasicBuckets(cameratrans, rasty, SOLID_INSTANCING_BUCKET);

			rasty->SetDepthMask(RAS_Rasterizer::RAS_DEPTHMASK_DISABLED);

			rasty->ResetGlobalDepthTexture();
			RenderBasicBuckets(cameratrans, rasty, ALPHA_INSTANCING_BUCKET);
			RenderBasicBuckets(cameratrans, rasty, ALPHA_DEPTH_INSTANCING_BUCKET);

			/* Rendering alpha and alpha depth regular materials with
			 * an empty shader and ordering.
			 */

			if ((m_buckets[ALPHA_BUCKET].size() + m_buckets[ALPHA_DEPTH_BUCKET].size()) != 0) {
				rasty->SetOverrideShader(RAS_Rasterizer::RAS_OVERRIDE_SHADER_BLACK);
			}
			RenderSortedBuckets(cameratrans, rasty, ALPHA_BUCKET);
			RenderSortedBuckets(cameratrans, rasty, ALPHA_DEPTH_BUCKET);

			rasty->SetOverrideShader(RAS_Rasterizer::RAS_OVERRIDE_SHADER_NONE);

			rasty->SetLines(false);
			rasty->SetDepthMask(RAS_Rasterizer::RAS_DEPTHMASK_ENABLED);
			break;
		}
		case RAS_Rasterizer::RAS_SOLID:
		case RAS_Rasterizer::RAS_TEXTURED:
		{
			/* Rendering solid and alpha (regular and instancing) materials
			 * with their shaders.
			 */

			rasty->SetDepthMask(RAS_Rasterizer::RAS_DEPTHMASK_ENABLED);

			RenderBasicBuckets(cameratrans, rasty, SOLID_BUCKET);
			RenderBasicBuckets(cameratrans, rasty, SOLID_INSTANCING_BUCKET);

			rasty->SetDepthMask(RAS_Rasterizer::RAS_DEPTHMASK_DISABLED);

			RenderBasicBuckets(cameratrans, rasty, ALPHA_INSTANCING_BUCKET);
			RenderSortedBuckets(cameratrans, rasty, ALPHA_BUCKET);

			// Render soft particles after all other materials.
			if ((m_buckets[ALPHA_DEPTH_BUCKET].size() + m_buckets[ALPHA_DEPTH_INSTANCING_BUCKET].size()) > 0) {
				rasty->UpdateGlobalDepthTexture(offScreen);

				RenderBasicBuckets(cameratrans, rasty, ALPHA_DEPTH_INSTANCING_BUCKET);
				RenderSortedBuckets(cameratrans, rasty, ALPHA_DEPTH_BUCKET);
			}

			rasty->SetDepthMask(RAS_Rasterizer::RAS_DEPTHMASK_ENABLED);
			break;
		}
		case RAS_Rasterizer::RAS_RENDERER:
		{
			/* Rendering solid and alpha (regular and instancing) materials
			 * with their shaders.
			 */

			rasty->SetDepthMask(RAS_Rasterizer::RAS_DEPTHMASK_ENABLED);

			RenderBasicBuckets(cameratrans, rasty, SOLID_BUCKET);
			RenderBasicBuckets(cameratrans, rasty, SOLID_INSTANCING_BUCKET);

			rasty->SetDepthMask(RAS_Rasterizer::RAS_DEPTHMASK_DISABLED);

			// Don't use depth transparency because the renderer could not offer a depth texture.
			rasty->ResetGlobalDepthTexture();
			RenderBasicBuckets(cameratrans, rasty, ALPHA_DEPTH_INSTANCING_BUCKET);
			RenderSortedBuckets(cameratrans, rasty, ALPHA_DEPTH_BUCKET);

			RenderBasicBuckets(cameratrans, rasty, ALPHA_INSTANCING_BUCKET);
			RenderSortedBuckets(cameratrans, rasty, ALPHA_BUCKET);

			rasty->SetDepthMask(RAS_Rasterizer::RAS_DEPTHMASK_ENABLED);
			break;
		}
		default:
		{
			break;
		}
	}

	BucketList& buckets = m_buckets[ALL_BUCKET];
	for (BucketList::iterator it = buckets.begin(), end = buckets.end(); it != end; ++it) {
		(*it)->RemoveActiveMeshSlots();
	}

	/* If we're drawing shadows and bucket wasn't rendered (outside of the lamp frustum or doesn't cast shadows)
	 * then the mesh is still modified, so we don't want to set MeshModified to false yet (it will mess up
	 * updating display lists). Just leave this step for the main render pass.
	 */
	if (rasty->GetDrawingMode() != RAS_Rasterizer::RAS_SHADOW) {
		/* All meshes should be up to date now */
		/* Don't do this while processing buckets because some meshes are split between buckets */
		for (BucketList::iterator it = buckets.begin(), end = buckets.end(); it != end; ++it) {
			(*it)->SetDisplayArrayUnmodified();
		}
	}

	rasty->SetClientObject(nullptr);
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
		if (mat == nullptr || (mat == bucket->GetPolyMaterial())) {
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
