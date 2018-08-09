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
#include "RAS_Mesh.h"
#include "RAS_MeshUser.h"
#include "RAS_IPolygonMaterial.h"
#include "RAS_Rasterizer.h"
#include "RAS_SortedMeshSlot.h"

#include "RAS_BucketManager.h"

#include <algorithm>
/* sorting */

RAS_BucketManager::RAS_BucketManager(RAS_IPolyMaterial *textMaterial)
	:m_downwardNode(this, &m_nodeData, nullptr, nullptr),
	m_upwardNode(this, &m_nodeData, nullptr, nullptr)
{
	bool created;
	m_text.m_bucket = FindBucket(textMaterial, created);
	m_text.m_arrayBucket = new RAS_DisplayArrayBucket(m_text.m_bucket, nullptr, nullptr, nullptr, nullptr);
}

RAS_BucketManager::~RAS_BucketManager()
{
	delete m_text.m_arrayBucket;

	for (RAS_MaterialBucket *bucket : m_buckets[ALL_BUCKET]) {
		delete bucket;
	}
}

void RAS_BucketManager::PrepareBuckets(RAS_Rasterizer *rasty, RAS_BucketManager::BucketType bucketType)
{
	if (m_nodeData.m_shaderOverride) {
		return;
	}

	for (RAS_MaterialBucket *bucket : m_buckets[bucketType]) {
		RAS_IPolyMaterial *mat = bucket->GetMaterial();
		mat->Prepare(rasty);
	}
}

void RAS_BucketManager::RenderSortedBuckets(RAS_Rasterizer *rasty, RAS_BucketManager::BucketType bucketType)
{
	PrepareBuckets(rasty, bucketType);

	BucketList& solidBuckets = m_buckets[bucketType];
	RAS_UpwardTreeLeafs leafs;
	for (RAS_MaterialBucket *bucket : solidBuckets) {
		bucket->GenerateTree(m_downwardNode, m_upwardNode, leafs, m_nodeData.m_drawingMode, true);
	}

	m_nodeData.m_sort = true;

	if (m_downwardNode.GetValid()) {
		m_downwardNode.Execute(RAS_DummyNodeTuple());
	}
	if (!leafs.empty()) {
		const RAS_SortedMeshSlotList sortedSlots = RAS_SortedMeshSlot::Sort(leafs, m_nodeData.m_trans);

		std::vector<RAS_SortedMeshSlot>::const_iterator it = sortedSlots.begin();
		RAS_MeshSlotUpwardNodeIterator iterator((it++)->m_node);
		for (std::vector<RAS_SortedMeshSlot>::const_iterator end = sortedSlots.end(); it != end; ++it) {
			iterator.NextNode(it->m_node);
		}
	}
}

void RAS_BucketManager::RenderBasicBuckets(RAS_Rasterizer *rasty, RAS_BucketManager::BucketType bucketType)
{
	PrepareBuckets(rasty, bucketType);

	RAS_UpwardTreeLeafs leafs;
	for (RAS_MaterialBucket *bucket : m_buckets[bucketType]) {
		bucket->GenerateTree(m_downwardNode, m_upwardNode, leafs, m_nodeData.m_drawingMode, false);
	}

	if (m_downwardNode.GetValid()) {
		m_nodeData.m_sort = false;
		m_downwardNode.Execute(RAS_DummyNodeTuple());
	}
}

void RAS_BucketManager::Renderbuckets(RAS_Rasterizer::DrawType drawingMode, const mt::mat3x4& cameratrans, RAS_Rasterizer *rasty,
                                      RAS_OffScreen *offScreen)
{
	m_nodeData.m_rasty = rasty;
	m_nodeData.m_trans = cameratrans;
	m_nodeData.m_drawingMode = drawingMode;

	switch (drawingMode) {
		case RAS_Rasterizer::RAS_SHADOW:
		{
			const bool isVarianceShadow = rasty->GetShadowMode() == RAS_Rasterizer::RAS_SHADOW_VARIANCE;

			rasty->SetDepthMask(RAS_Rasterizer::RAS_DEPTHMASK_ENABLED);

			/* Rendering solid regular materials with an override shader for
			 * variance shadow or an empty shader.
			 */

			m_nodeData.m_shaderOverride = true;
			if (!m_buckets[SOLID_SHADOW_BUCKET].empty()) {
				rasty->SetOverrideShader(isVarianceShadow ?
				                         RAS_Rasterizer::RAS_OVERRIDE_SHADER_SHADOW_VARIANCE :
				                         RAS_Rasterizer::RAS_OVERRIDE_SHADER_BLACK);
			}
			RenderBasicBuckets(rasty, SOLID_SHADOW_BUCKET);

			/* Rendering solid instancing materials with a different override
			 * shader for variance and simple shadow.
			 */

			if (!m_buckets[SOLID_SHADOW_INSTANCING_BUCKET].empty()) {
				rasty->SetOverrideShader(isVarianceShadow ?
				                         RAS_Rasterizer::RAS_OVERRIDE_SHADER_SHADOW_VARIANCE_INSTANCING :
				                         RAS_Rasterizer::RAS_OVERRIDE_SHADER_BLACK_INSTANCING);
			}
			RenderBasicBuckets(rasty, SOLID_SHADOW_INSTANCING_BUCKET);

			if (isVarianceShadow) {
				/* Rendering alpha shadow instancing materials with an override
				 * shader for variance shadow.
				 */

				if (!m_buckets[ALPHA_SHADOW_INSTANCING_BUCKET].empty()) {
					rasty->SetOverrideShader(RAS_Rasterizer::RAS_OVERRIDE_SHADER_SHADOW_VARIANCE_INSTANCING);
				}
				RenderBasicBuckets(rasty, ALPHA_SHADOW_INSTANCING_BUCKET);

				/* Rendering alpha shadow regular materials with an override
				 * shader for variance shadow and ordering.
				 */

				if (!m_buckets[ALPHA_SHADOW_BUCKET].empty()) {
					rasty->SetOverrideShader(RAS_Rasterizer::RAS_OVERRIDE_SHADER_SHADOW_VARIANCE);
				}
				RenderBasicBuckets(rasty, ALPHA_SHADOW_BUCKET);

				rasty->SetOverrideShader(RAS_Rasterizer::RAS_OVERRIDE_SHADER_NONE);
			}
			else {
				// Rendering alpha shadow materials (including instancing) with their shaders.

				rasty->SetOverrideShader(RAS_Rasterizer::RAS_OVERRIDE_SHADER_NONE);
				m_nodeData.m_shaderOverride = false;

				RenderBasicBuckets(rasty, ALPHA_SHADOW_INSTANCING_BUCKET);
				// Render alpha shadow regular materials with ordering.
				RenderBasicBuckets(rasty, ALPHA_SHADOW_BUCKET);
			}

			break;
		}
		case RAS_Rasterizer::RAS_WIREFRAME:
		{
			m_nodeData.m_shaderOverride = true;
			rasty->SetLines(true);
			rasty->SetDepthMask(RAS_Rasterizer::RAS_DEPTHMASK_ENABLED);

			// Rendering solid regular materials with an empty override shader.

			if (!m_buckets[SOLID_BUCKET].empty()) {
				rasty->SetOverrideShader(RAS_Rasterizer::RAS_OVERRIDE_SHADER_BLACK);
			}
			RenderBasicBuckets(rasty, SOLID_BUCKET);

			/* Rendering solid, alpha and alpha depth instancing materials
			 * with an override shader.
			 */

			if ((m_buckets[SOLID_INSTANCING_BUCKET].size() + m_buckets[ALPHA_INSTANCING_BUCKET].size()) > 0) {
				rasty->SetOverrideShader(RAS_Rasterizer::RAS_OVERRIDE_SHADER_BLACK_INSTANCING);
			}
			RenderBasicBuckets(rasty, SOLID_INSTANCING_BUCKET);
			RenderSortedBuckets(rasty, ALPHA_INSTANCING_BUCKET);

			/* Rendering alpha and alpha depth regular materials with
			 * an empty shader and ordering.
			 */

			if (!m_buckets[ALPHA_BUCKET].empty()) {
				rasty->SetOverrideShader(RAS_Rasterizer::RAS_OVERRIDE_SHADER_BLACK);
			}
			RenderSortedBuckets(rasty, ALPHA_BUCKET);

			rasty->SetOverrideShader(RAS_Rasterizer::RAS_OVERRIDE_SHADER_NONE);

			rasty->SetLines(false);
			break;
		}
		case RAS_Rasterizer::RAS_TEXTURED:
		{
			/* Rendering solid and alpha (regular and instancing) materials
			 * with their shaders.
			 */

			m_nodeData.m_shaderOverride = false;
			rasty->SetDepthMask(RAS_Rasterizer::RAS_DEPTHMASK_ENABLED);

			RenderBasicBuckets(rasty, SOLID_BUCKET);
			RenderBasicBuckets(rasty, SOLID_INSTANCING_BUCKET);

			rasty->SetDepthMask(RAS_Rasterizer::RAS_DEPTHMASK_DISABLED);

			// Update depth transparency depth texture after rendering all solid materials.
			if ((m_buckets[ALPHA_DEPTH_BUCKET].size() + m_buckets[ALPHA_DEPTH_INSTANCING_BUCKET].size()) > 0) {
				rasty->UpdateGlobalDepthTexture(offScreen);
			}
			RenderBasicBuckets(rasty, ALPHA_INSTANCING_BUCKET);
			RenderSortedBuckets(rasty, ALPHA_BUCKET);


			rasty->SetDepthMask(RAS_Rasterizer::RAS_DEPTHMASK_ENABLED);
			break;
		}
		case RAS_Rasterizer::RAS_RENDERER:
		{
			/* Rendering solid and alpha (regular and instancing) materials
			 * with their shaders.
			 */

			m_nodeData.m_shaderOverride = false;
			rasty->SetDepthMask(RAS_Rasterizer::RAS_DEPTHMASK_ENABLED);

			RenderBasicBuckets(rasty, SOLID_BUCKET);
			RenderBasicBuckets(rasty, SOLID_INSTANCING_BUCKET);

			rasty->SetDepthMask(RAS_Rasterizer::RAS_DEPTHMASK_DISABLED);

			// Don't use depth transparency because the renderer could not offer a depth texture.
			rasty->ResetGlobalDepthTexture();

			RenderBasicBuckets(rasty, ALPHA_INSTANCING_BUCKET);
			RenderSortedBuckets(rasty, ALPHA_BUCKET);

			rasty->SetDepthMask(RAS_Rasterizer::RAS_DEPTHMASK_ENABLED);
			break;
		}
		default:
		{
			break;
		}
	}

	for (RAS_MaterialBucket *bucket : m_buckets[ALL_BUCKET]) {
		bucket->RemoveActiveMeshSlots();
	}

	rasty->SetClientObject(nullptr);
}

RAS_MaterialBucket *RAS_BucketManager::FindBucket(RAS_IPolyMaterial *material, bool &bucketCreated)
{
	bucketCreated = false;

	for (RAS_MaterialBucket *bucket : m_buckets[ALL_BUCKET]) {
		if (bucket->GetMaterial() == material) {
			return bucket;
		}
	}

	RAS_MaterialBucket *bucket = new RAS_MaterialBucket(material);
	bucketCreated = true;

	const bool useinstancing = material->UseInstancing();
	if (!material->OnlyShadow()) {
		if (material->IsAlpha()) {
			m_buckets[useinstancing ? ALPHA_INSTANCING_BUCKET : ALPHA_BUCKET].push_back(bucket);
			if (material->IsAlphaDepth()) {
				m_buckets[useinstancing ? ALPHA_DEPTH_INSTANCING_BUCKET : ALPHA_DEPTH_BUCKET].push_back(bucket);
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

	// Used to free the bucket.
	m_buckets[ALL_BUCKET].push_back(bucket);
	return bucket;
}

RAS_DisplayArrayBucket *RAS_BucketManager::GetTextDisplayArrayBucket() const
{
	return m_text.m_arrayBucket;
}

void RAS_BucketManager::ReloadMaterials(RAS_IPolyMaterial *mat)
{
	for (RAS_MaterialBucket *bucket : m_buckets[ALL_BUCKET]) {
		if (mat == nullptr || (mat == bucket->GetMaterial())) {
			bucket->GetMaterial()->ReloadMaterial();
		}
	}
}

/* frees the bucket, only used when freeing scenes */
void RAS_BucketManager::RemoveMaterial(RAS_IPolyMaterial *mat)
{
	for (unsigned short i = 0; i < NUM_BUCKET_TYPE; ++i) {
		BucketList& buckets = m_buckets[i];
		for (BucketList::iterator it = buckets.begin(); it != buckets.end(); ) {
			RAS_MaterialBucket *bucket = *it;
			if (mat == bucket->GetMaterial()) {
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

void RAS_BucketManager::Merge(RAS_BucketManager *other, SCA_IScene *scene)
{
	for (unsigned short i = 0; i < NUM_BUCKET_TYPE; ++i) {
		BucketList& buckets = m_buckets[i];
		BucketList& otherbuckets = other->m_buckets[i];

		// Skip the text bucket.
		std::remove_copy(otherbuckets.begin(), otherbuckets.end(), std::back_inserter(buckets), other->m_text.m_bucket);
		otherbuckets.clear();
	}
}
