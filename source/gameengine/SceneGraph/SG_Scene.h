#ifndef __SG_SCENE_H__
#define __SG_SCENE_H__

#include "SG_Node.h"

#include <vector>

class SG_Node;

class SG_Scene
{
private:
	/**
	 * List of nodes that needs scenegraph update
	 * the Dlist is not object that must be updated
	 * the Qlist is for objects that needs to be rescheduled
	 * for updates after udpate is over (slow parent, bone parent).
	 */
	SG_QList m_head;

	NodeList m_rootNodes;

public:
	virtual SG_Object *ReplicateNodeObject(SG_Node *node, SG_Object *origObject) = 0;
	virtual void DestructNodeObject(SG_Node *node, SG_Object *object) = 0;

	void Schedule(SG_Node *node);
	void Reschedule(SG_Node *node);

	void AddRootNode(SG_Node *node);
	void RemoveRootNode(SG_Node *node);

	void DestructRootNodes();

	void UpdateParents();

	void Merge(SG_Scene *other);
};

#endif  // __SG_SCENE_H__
