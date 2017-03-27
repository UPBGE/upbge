#ifndef __SG_CULLING_OBJECT_H__
#define __SG_CULLING_OBJECT_H__

#include "SG_BBox.h"

#include <vector>

class SG_CullingNode
{
private:
	SG_BBox m_aabb;
	bool m_culled;

public:
	SG_CullingNode();
	~SG_CullingNode() = default;

	SG_BBox& GetAabb();
	const SG_BBox& GetAabb() const;

	bool GetCulled() const;
	void SetCulled(bool culled);
};

using SG_CullingNodeList = std::vector<SG_CullingNode *>;

#endif  // __SG_CULLING_OBJECT_H__
