#include "KX_CullingHandler.h"
#include "KX_GameObject.h"

#include "SG_Node.h"
#include "BVH.h"

#include "tbb/tbb.h"

class CullTask
{
public:
	std::vector<KX_GameObject *> m_activeObjects;
	EXP_ListValue<KX_GameObject> *m_objects;
	KX_CullingHandler& m_handler;
	int m_layer;

	CullTask(EXP_ListValue<KX_GameObject> *objects, KX_CullingHandler& handler, int layer)
		:m_objects(objects),
		m_handler(handler),
		m_layer(layer)
	{
	}

	CullTask(const CullTask& other, tbb::split)
		:m_objects(other.m_objects),
		m_handler(other.m_handler),
		m_layer(other.m_layer)
	{
	}

	void operator()(const tbb::blocked_range<size_t>& r)
	{
		for (unsigned int i = r.begin(), end = r.end(); i < end; ++i) {
			KX_GameObject *obj = m_objects->GetValue(i);
			if (obj->Renderable(m_layer)) {
				// Update the object bounding volume box.
				obj->UpdateBounds(false);

				SG_CullingNode& node = obj->GetCullingNode();
				const bool culled = m_handler.Test(obj->NodeGetWorldTransform(), obj->NodeGetWorldScaling(), node.GetAabb());

				node.SetCulled(culled);
				if (!culled) {
					m_activeObjects.push_back(obj);
				}
			}
		}
	}

	void join(const CullTask& other)
	{
		m_activeObjects.insert(m_activeObjects.end(), other.m_activeObjects.begin(), other.m_activeObjects.end());
	}
};

KX_CullingHandler::KX_CullingHandler(EXP_ListValue<KX_GameObject> *objects, const SG_Frustum& frustum, int layer)
	:m_objects(objects),
	m_frustum(frustum),
	m_layer(layer)
{
	std::vector<bvh::Object *> nodes;
	for (KX_GameObject *obj : objects) {
		if (obj->Renderable(m_layer)) {
			nodes.push_back(&obj->GetCullingNode());
		}
	}

	m_tree = bvh::BVH(&nodes);
}

bool KX_CullingHandler::Test(const mt::mat3x4& trans, const mt::vec3& scale, const SG_BBox& aabb) const
{
	bool culled = true;
	const float maxscale = std::max(std::max(fabs(scale.x), fabs(scale.y)), fabs(scale.z));
	const SG_Frustum::TestType sphereTest = m_frustum.SphereInsideFrustum(trans * aabb.GetCenter(), maxscale * aabb.GetRadius());

	// First test if the sphere is in the frustum as it is faster to test than box.
	if (sphereTest == SG_Frustum::INSIDE) {
		culled = false;
	}
	// If the sphere intersects we made a box test because the box could be not homogeneous.
	else if (sphereTest == SG_Frustum::INTERSECT) {
		const mt::mat4 mat = mt::mat4::FromAffineTransform(trans);
		culled = (m_frustum.AabbInsideFrustum(aabb.GetMin(), aabb.GetMax(), mat) == SG_Frustum::OUTSIDE);
	}

	return culled;
}

bool KX_CullingHandler::Test(bvh::BVHFlatNode& node)
{
	const bvh::BBox& box = node.bbox;
	const mt::vec3 diag = box.max - box.min;
	SG_Frustum::TestType test = m_frustum.SphereInsideFrustum((box.min + diag * 0.5f), diag.Length());

	// First test if the sphere is in the frustum as it is faster to test than box.
	if (test == SG_Frustum::INSIDE) {
		return false;
	}
	else if (test == SG_Frustum::OUTSIDE) {
		return true;
	}

	test = m_frustum.AabbInsideFrustum(box.min, box.max);
	if (test == SG_Frustum::INSIDE) {
		return false;
	}
	else if (test == SG_Frustum::OUTSIDE) {
		return true;
	}

	return true;
}

std::vector<KX_GameObject *> KX_CullingHandler::Process()
{
	/*CullTask task(m_objects, *this, m_layer);
	tbb::parallel_reduce(tbb::blocked_range<size_t>(0, m_objects->GetCount()), task);*/

	Test(*m_tree.flatTree);

	return {};
}
