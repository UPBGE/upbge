#ifndef __KX_CULLING_HANDLER_H__
#define __KX_CULLING_HANDLER_H__

#include "KX_CullingNode.h"
#include "SG_Frustum.h"

class KX_CullingHandler
{
private:
	/// List of all nodes to render after the culling pass.
	KX_CullingNodeList& m_activeNodes;
	/// The camera frustum data.
	const SG_Frustum& m_frustum;

public:
	KX_CullingHandler(KX_CullingNodeList& nodes, const SG_Frustum& frustum);
	~KX_CullingHandler() = default;

	/** Process the culling of a new node, if the culling succeeded the
	 * node is added in m_activeNodes.
	 */
	void Process(KX_CullingNode *node);
};

#endif  // __KX_CULLING_HANDLER_H__
