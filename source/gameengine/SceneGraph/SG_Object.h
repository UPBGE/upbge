#ifndef __SG_OBJECT_H__
#define __SG_OBJECT_H__

#include "SG_BBox.h"

class SG_Node;

class SG_Object
{
protected:
	/// Object transformation and hierachie.
	SG_Node *m_node;
	/// The bounding box in node transform space.
	SG_BBox m_aabb;
	/// The culling state from the last culling pass.
	bool m_culled;

public:
	SG_Object();
	SG_Object(const SG_Object& other);
	virtual ~SG_Object();

	SG_Node *GetNode() const;
	void SetNode(SG_Node *node);

	SG_BBox& GetAabb();
	const SG_BBox& GetAabb() const;

	bool GetCulled() const;
	void SetCulled(bool culled);
};

#endif  // __SG_OBJECT_H__
