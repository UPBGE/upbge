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
#include "RAS_TextMaterial.h"
#include "RAS_Rasterizer.h"

#include "RAS_BucketManager.h"

#include <algorithm>
/* sorting */

RAS_BucketManager::SortedMeshSlot::SortedMeshSlot(RAS_MeshSlot *ms, const mt::vec3& pnorm)
	:m_ms(ms)
{
	// would be good to use the actual bounding box center instead
	const mt::vec3 pos = m_ms->m_meshUser->GetMatrix().TranslationVector3D();

	m_z = mt::dot(pnorm, pos);
}

RAS_BucketManager::SortedMeshSlot::SortedMeshSlot(RAS_MeshSlotUpwardNode *node, const mt::vec3& pnorm)
	:m_node(node)
{
	RAS_MeshSlot *ms = m_node->GetOwner();
	// would be good to use the actual bounding box center instead
	const mt::vec3 pos = ms->m_meshUser->GetMatrix().TranslationVector3D();

	m_z = mt::dot(pnorm, pos);
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
	:m_downwardNode(this, &m_nodeData, nullptr, nullptr),
	m_upwardNode(this, &m_nodeData, nullptr, nullptr)
{
	m_text.m_bucket = FindBucket(RAS_TextMaterial::GetSingleton());
	m_text.m_arrayBucket = new RAS_DisplayArrayBucket(m_text.m_bucket, nullptr, nullptr, nullptr, nullptr);
}

RAS_BucketManager::~RAS_BucketManager()
{
	delete m_text.m_arrayBucket;

	for (RAS_MaterialBucket *bucket : m_buckets[ALL_BUCKET]) {
		delete bucket;
	}
}

void RAS_BucketManager::RenderSortedBuckets(RAS_Rasterizer *rasty, RAS_BucketManager::BucketType bucketType)
{
	BucketList& solidBuckets = m_buckets[bucketType];
	RAS_UpwardTreeLeafs leafs;
	for (RAS_MaterialBucket *bucket : solidBuckets) {
		bucket->GenerateTree(m_downwardNode, m_upwardNode, leafs, rasty, m_nodeData.m_drawingMode, true);
	}

	m_nodeData.m_sort = true;

	if (m_downwardNode.GetValid()) {
		m_downwardNode.Execute(RAS_DummyNodeTuple());
	}
	if (!leafs.empty()) {
		/* Camera's near plane equation: pnorm.dot(point) + pval,
		 * but we leave out pval since it's constant anyway */
		const mt::mat3x4& trans = m_nodeData.m_trans;
		const mt::vec3 pnorm(trans[2], trans[5], trans[8]);
		std::vector<SortedMeshSlot> sortedSlots(leafs.size());
		// Generate all SortedMeshSlot corresponding to all the leafs nodes.
		std::transform(leafs.begin(), leafs.end(), sortedSlots.begin(),
		               [&pnorm](RAS_MeshSlotUpwardNode *node) {
			return SortedMeshSlot(node, pnorm);
		});

		std::sort(sortedSlots.begin(), sortedSlots.end(), backtofront());

		std::vector<SortedMeshSlot>::const_iterator it = sortedSlots.begin();
		RAS_MeshSlotUpwardNodeIterator iterator((it++)->m_node);
		for (std::vector<SortedMeshSlot>::const_iterator end = sortedSlots.end(); it != end; ++it) {
			iterator.NextNode(it->m_node);
		}
	}
}

void RAS_BucketManager::RenderBasicBuckets(RAS_Rasterizer *rasty, RAS_BucketManager::BucketType bucketType)
{
	RAS_UpwardTreeLeafs leafs;
	for (RAS_MaterialBucket *bucket : m_buckets[bucketType]) {
		bucket->GenerateTree(m_downwardNode, m_upwardNode, leafs, rasty, m_nodeData.m_drawingMode, false);
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

	rasty->SetDepthMask(RAS_Rasterizer::RAS_DEPTHMASK_ENABLED);

	switch (drawingMode) {
		case RAS_Rasterizer::RAS_SHADOW:
		case RAS_Rasterizer::RAS_SHADOW_VARIANCE:
		{
			RenderBasicBuckets(rasty, SHADOW_BUCKET);
			break;
		}
		case RAS_Rasterizer::RAS_WIREFRAME:
		{
			rasty->SetLines(true);

			RenderBasicBuckets(rasty, SOLID_ALPHA_BUCKET);

			rasty->SetLines(false);
			break;
		}
		case RAS_Rasterizer::RAS_TEXTURED:
		{
			RenderBasicBuckets(rasty, SOLID_BUCKET);

			rasty->SetDepthMask(RAS_Rasterizer::RAS_DEPTHMASK_DISABLED);

			if (!offScreen) {
				// Texture renderers could not provide an offscreen, in this case reset the depth texture.
				rasty->ResetGlobalDepthTexture();
			}
			else if (!m_buckets[ALPHA_DEPTH_BUCKET].empty()) {
				// Update depth transparency depth texture after rendering all solid materials.
				rasty->UpdateGlobalDepthTexture(offScreen);
			}

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

RAS_MaterialBucket *RAS_BucketManager::FindBucket(RAS_IMaterial *material)
{
	for (RAS_MaterialBucket *bucket : m_buckets[ALL_BUCKET]) {
		if (bucket->GetMaterial() == material) {
			return bucket;
		}
	}

	RAS_MaterialBucket *bucket = new RAS_MaterialBucket(material);

	if (!material->OnlyShadow()) {
		if (material->IsAlpha()) {
			m_buckets[ALPHA_BUCKET].push_back(bucket);
			if (material->IsAlphaDepth()) {
				m_buckets[ALPHA_DEPTH_BUCKET].push_back(bucket);
			}
		}
		else {
			m_buckets[SOLID_BUCKET].push_back(bucket);
		}
		m_buckets[SOLID_ALPHA_BUCKET].push_back(bucket);
	}
	if (material->CastsShadows()) {
		m_buckets[SHADOW_BUCKET].push_back(bucket);
	}

	// Used to free the bucket.
	m_buckets[ALL_BUCKET].push_back(bucket);
	return bucket;
}

RAS_DisplayArrayBucket *RAS_BucketManager::GetTextDisplayArrayBucket() const
{
	return m_text.m_arrayBucket;
}

void RAS_BucketManager::ReloadMaterials(RAS_IMaterial *mat)
{
	for (RAS_MaterialBucket *bucket : m_buckets[ALL_BUCKET]) {
		if (mat == nullptr || (mat == bucket->GetMaterial())) {
			bucket->GetMaterial()->ReloadMaterial();
		}
	}
}

void RAS_BucketManager::RemoveMaterial(RAS_IMaterial *mat)
{
	BucketList& allBuckets = m_buckets[ALL_BUCKET];
	for (BucketList::iterator it = allBuckets.begin(); it != allBuckets.end();) {
		RAS_MaterialBucket *bucket = *it;
		// Find the bucket in ALL_BUCKET list.
		if (mat == bucket->GetMaterial()) {
			// Iterate over all bucket list excepted ALL_BUCKET and remove the bucket.
			for (unsigned short i = 0; i < ALL_BUCKET; ++i) {
				CM_ListRemoveIfFound(m_buckets[i], bucket);
			}
			// Remove the bucket from ALL_BUCKET and destruct it.
			it = allBuckets.erase(it);
			delete bucket;
		}
		else {
			++it;
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
