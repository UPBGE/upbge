#ifndef __KX_CULLING_HANDLER_H__
#define __KX_CULLING_HANDLER_H__

#include "KX_CullingNode.h"
#include "SG_Frustum.h"

class KX_CullingHandler
{
private:
	KX_CullingNodeList& m_activeNodes;
	const SG_Frustum& m_frustum;

public:
	KX_CullingHandler(KX_CullingNodeList& nodes, const SG_Frustum& frustum);
	~KX_CullingHandler() = default;

	void Process(KX_CullingNode *node);
};

#endif  // __KX_CULLING_HANDLER_H__
