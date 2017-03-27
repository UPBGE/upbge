#ifndef __KX_CULLING_NODE_H__
#define __KX_CULLING_NODE_H__

#include "SG_CullingNode.h"

class KX_GameObject;

class KX_CullingNode : public SG_CullingNode
{
private:
	KX_GameObject *m_object;

public:
	KX_CullingNode(KX_GameObject *object);
	~KX_CullingNode() = default;

	KX_GameObject *GetObject() const;
	void SetObject(KX_GameObject *object);
};

using KX_CullingNodeList = std::vector<KX_CullingNode *>;

#endif  // __KX_CULLING_NODE_H__
