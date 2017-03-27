#ifndef __SG_CULLING_HANDLER_H__
#define __SG_CULLING_HANDLER_H__

#include "SG_CullingNode.h"
#include "SG_Frustum.h"

template <class NodeType>
class SG_CullingHandler
{
private:
	SG_CullingNodeList& m_activeNodes;
	const SG_Frustum& m_frustum;

public:
	SG_CullingHandler(SG_CullingNodeList& nodes, const SG_Frustum& frustum);
	~SG_CullingHandler() = default;

	void Process(NodeType *node) override;
};

#endif  // __SG_CULLING_HANDLER_H__
