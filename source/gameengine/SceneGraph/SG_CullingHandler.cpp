#include "SG_CullingHandler.h"

SG_CullingHandler::SG_CullingHandler(SG_CullingNodeList& nodes, const SG_Frustum& frustum)
	:m_activeNodes(nodes),
	m_frustum(frustum)
{
}

void SG_CullingHandler::Process(SG_CullingNode *node)
{
}
