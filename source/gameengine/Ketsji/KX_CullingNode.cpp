#include "KX_CullingNode.h"

KX_CullingNode::KX_CullingNode(KX_GameObject *object)
	:m_object(object)
{
}

KX_GameObject *KX_CullingNode::GetObject() const
{
	return m_object;
}

void KX_CullingNode::SetObject(KX_GameObject *object)
{
	m_object = object;
}
