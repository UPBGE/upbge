#ifndef __SG_SCENE_H__
#define __SG_SCENE_H__

#include "SG_Node.h"

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

	/// Root nodes: nodes without parent.
	NodeList m_rootNodes;

public:
	/** Replicate node object.
	 * \param node The new node owning the object replica.
	 * \param origObject The original object to duplicate.
	 * \return The replicated object.
	 */
	virtual SG_Object *ReplicateNodeObject(SG_Node *node, SG_Object *origObject) = 0;

	/** Destruct node object.
	 * \param node The node owning the object to destruct.
	 * \param object The object to destruct.
	 */
	virtual void DestructNodeObject(SG_Node *node, SG_Object *object) = 0;

	void Schedule(SG_Node *node);
	void Reschedule(SG_Node *node);

	/// Add a root node.
	void AddRootNode(SG_Node *node);
	/// Remove a root node.
	void RemoveRootNode(SG_Node *node);

	/// Destruct all root nodes.
	void DestructRootNodes();

	/// Update all nodes.
	void UpdateParents();

	/// Merge data with an other scene.
	void Merge(SG_Scene *other);
};

#endif  // __SG_SCENE_H__
