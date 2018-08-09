#include "RAS_SortedMeshSlot.h"
#include "RAS_MeshUser.h"
#include "RAS_DisplayArrayBucket.h"
#include "RAS_BoundingBox.h"

#include "tbb/parallel_for.h"
#include "tbb/parallel_sort.h"

class InitLeafsTask
{
public:
	const RAS_UpwardTreeLeafs& m_leafs;
	mt::vec3 m_pnorm;
	RAS_SortedMeshSlotList& m_result;

	InitLeafsTask(const RAS_UpwardTreeLeafs& leafs, const mt::vec3& pnorm, RAS_SortedMeshSlotList& result)
		:m_leafs(leafs),
		m_pnorm(pnorm),
		m_result(result)
	{
	}

	void operator()(const tbb::blocked_range<size_t>& r) const
	{
		for (unsigned int i = r.begin(), end = r.end(); i < end; ++i) {
			m_result[i] = RAS_SortedMeshSlot(m_leafs[i], m_pnorm);
		}
	}
};

class InitSlotsTask
{
public:
	const RAS_MeshSlotList& m_slots;
	mt::vec3 m_pnorm;
	RAS_SortedMeshSlotList& m_result;

	InitSlotsTask(const RAS_MeshSlotList& slots, const mt::vec3& pnorm, RAS_SortedMeshSlotList& result)
		:m_slots(slots),
		m_pnorm(pnorm),
		m_result(result)
	{
	}

	void operator()(const tbb::blocked_range<size_t>& r) const
	{
		for (unsigned int i = r.begin(), end = r.end(); i < end; ++i) {
			m_result[i] = RAS_SortedMeshSlot(m_slots[i], m_pnorm);
		}
	}
};


RAS_SortedMeshSlot::RAS_SortedMeshSlot(RAS_MeshSlot *ms, const mt::vec3& pnorm)
	:RAS_SortedMeshSlot(ms->m_meshUser, ms, pnorm)
{
	m_ms = ms;
}

RAS_SortedMeshSlot::RAS_SortedMeshSlot(RAS_MeshSlotUpwardNode *node, const mt::vec3& pnorm)
	:RAS_SortedMeshSlot(node->GetOwner()->m_meshUser, node->GetOwner(), pnorm)
{
	m_node = node;
}

RAS_SortedMeshSlot::RAS_SortedMeshSlot(RAS_MeshUser *meshUser, RAS_MeshSlot *ms, const mt::vec3& pnorm)
{
	mt::vec3 center;
	float radius;

	RAS_DisplayArray *array = ms->m_displayArrayBucket->GetDisplayArray();
	if (array) {
		center = array->GetAabbCenter();
		radius = array->GetAabbRadius();
	}
	else {
		RAS_BoundingBox *boundingBox = meshUser->GetBoundingBox();
		mt::vec3 aabbMin;
		mt::vec3 aabbMax;
		boundingBox->GetAabb(aabbMin, aabbMax);

		center = (aabbMin + aabbMax) * 0.5f;
		radius = 0.0f;
	}

	const mt::mat4& matrix = meshUser->GetMatrix();

	const mt::vec3 pos = matrix * center;
	const float shift = (matrix.ScaleVector3D() * (pnorm * radius)).Length();

	/* Camera's near plane equation: pnorm.dot(point) + pval,
	 * but we leave out pval since it's constant anyway */
	m_z = mt::dot(pnorm, pos) + shift;
}

struct BackToFront
{
	inline bool operator()(RAS_SortedMeshSlot &a, RAS_SortedMeshSlot &b) const
	{
		return (a.m_z < b.m_z);
	}
};

RAS_SortedMeshSlotList RAS_SortedMeshSlot::Sort(const RAS_UpwardTreeLeafs& leafs, const mt::mat3x4& trans)
{
	const mt::vec3 pnorm(trans[2], trans[5], trans[8]);
	const unsigned int count = leafs.size();
	RAS_SortedMeshSlotList result(count);

	InitLeafsTask task(leafs, pnorm, result);
	tbb::parallel_for(tbb::blocked_range<size_t>(0, count), task, tbb::static_partitioner());

	return Sort(result);
}

RAS_SortedMeshSlotList RAS_SortedMeshSlot::Sort(const RAS_MeshSlotList& slots, const mt::mat3x4& trans)
{
	const mt::vec3 pnorm(trans[2], trans[5], trans[8]);
	const unsigned int count = slots.size();
	RAS_SortedMeshSlotList result(count);

	InitSlotsTask task(slots, pnorm, result);
	tbb::parallel_for(tbb::blocked_range<size_t>(0, count), task);

	return Sort(result);
}

RAS_SortedMeshSlotList RAS_SortedMeshSlot::Sort(RAS_SortedMeshSlotList& slots)
{
	tbb::parallel_sort(slots, BackToFront());
	return slots;
}
