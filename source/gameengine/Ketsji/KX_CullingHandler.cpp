#include "KX_CullingHandler.h"
#include "KX_GameObject.h"

#include "SG_Node.h"

KX_CullingHandler::KX_CullingHandler(KX_CullingNodeList& nodes, const SG_Frustum& frustum)
	:m_activeNodes(nodes),
	m_frustum(frustum)
{
}

void KX_CullingHandler::Process(KX_CullingNode *node)
{
	SG_Node *sgnode = node->GetObject()->GetSGNode();
	const MT_Transform trans = sgnode->GetWorldTransform();
	const MT_Vector3 &scale = sgnode->GetWorldScaling();
	const SG_BBox& aabb = node->GetAabb();

	bool culled = true;
	const SG_Frustum::TestType sphereTest = m_frustum.SphereInsideFrustum(trans(aabb.GetCenter()), fabs(scale[scale.closestAxis()]) * aabb.GetRadius());

	// First test if the sphere is in the frustum as it is faster to test than box.
	if (sphereTest == SG_Frustum::INSIDE) {
		culled = false;
	}
	// If the sphere intersects we made a box test because the box could be not homogeneous.
	else if (sphereTest == SG_Frustum::INTERSECT) {
		const MT_Matrix4x4 mat = MT_Matrix4x4(trans);
		culled = (m_frustum.AabbInsideFrustum(aabb.GetMin(), aabb.GetMax(), mat) == SG_Frustum::OUTSIDE);
	}

	node->SetCulled(culled);
	if (!culled) {
		m_activeNodes.push_back(node);
	}
}
