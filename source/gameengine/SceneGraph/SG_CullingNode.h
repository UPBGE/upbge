#ifndef __SG_CULLING_OBJECT_H__
#define __SG_CULLING_OBJECT_H__

#include "SG_BBox.h"

#include <vector>

/// \brief Node used for the culling of a scene graph node, then a scene object.
class SG_CullingNode
{
private:
	/// The bounding box of the node in the scene graph node transform space.
	SG_BBox m_aabb;
	/// The culling state from the last culling pass.
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
