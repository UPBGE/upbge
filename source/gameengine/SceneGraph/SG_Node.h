/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file SG_Node.h
 *  \ingroup bgesg
 */

#ifndef __SG_NODE_H__
#define __SG_NODE_H__

#include "SG_QList.h"
#include "SG_ParentRelation.h"

#include "mathfu.h"

#include "CM_Thread.h"

#include <vector>
#include <memory>

class SG_Familly;
class SG_Node;
class SG_Scene;
class SG_Object;

typedef void (*SG_UpdateTransformCallback)(SG_Node *sgnode, SG_Object *object, SG_Scene *scene);

/**
 * SG_Callbacks hold 2 call backs to the outside world.
 * The first is meant to be called when objects are replicated.
 * And allows the outside world to synchronize external objects
 * with replicated nodes and their children.
 * The second is called when a node is destroyed and again
 * is their for synchronization purposes
 * These callbacks may both be nullptr.
 * The efficacy of this approach has not been proved some
 * alternatives might be to perform all replication and destruction
 * externally.
 * To define a class interface rather than a simple function
 * call back so that replication information can be transmitted from
 * parent->child.
 */
struct SG_Callbacks {
	SG_Callbacks()
		:m_updatefunc(nullptr)
	{
	}

	SG_Callbacks(SG_UpdateTransformCallback updatefunc)
		:m_updatefunc(updatefunc)
	{
	}

	SG_UpdateTransformCallback m_updatefunc;
};

typedef std::vector<SG_Node *> NodeList;

/**
 * Scenegraph node.
 */
class SG_Node : public SG_QList, public mt::SimdClassAllocator
{
public:

	enum DirtyFlag {
		DIRTY_NONE = 0,
		DIRTY_ALL = 0xFF,
		DIRTY_RENDER = (1 << 0),
		DIRTY_CULLING = (1 << 1)
	};

	SG_Node(SG_Object *object, SG_Scene *scene, SG_Callbacks& callbacks, SG_ParentRelation *relation);
	SG_Node(const SG_Node& other);
	~SG_Node();

	/**
	 * Set the parent of this node.
	 */
	void SetParent(SG_Node *parent);

	/**
	 * Return true if the node is the ancestor of child
	 */
	bool IsAncessor(const SG_Node *child) const;

	/**
	 * Get the current list of children. Do not use this interface for
	 * adding or removing children please use the methods of this class for
	 * that.
	 * \return a reference to the list of children of this node.
	 */
	const NodeList& GetChildren() const;

	/**
	 * Clear the list of children associated with this node
	 */
	void ClearSGChildren();

	/**
	 * return the parent of this node if it exists.
	 */
	SG_Node *GetParent() const;

	/**
	 * Return the top node in this node's Scene graph hierarchy
	 */
	SG_Node *GetRootSGParent();

	/**
	 * Disconnect this node from it's parent
	 */
	void DisconnectFromParent();

	/**
	 * Return vertex parent status.
	 */
	bool IsVertexParent();

	/**
	 * Return slow parent status.
	 */
	bool IsSlowParent();

	/**
	 * Update the spatial data of this node. Iterate through
	 * the children of this node and update their world data.
	 */
	void UpdateWorldData(bool parentUpdated = false);
	void UpdateWorldDataThread(bool parentUpdated = false);

	/**
	 * Update the simulation time of this node. Iterate through
	 * the children nodes and update their simulated time.
	 */
	void SetSimulatedTime(double time, bool recurse);
	void SetSimulatedTimeThread(double time, bool recurse);

	/**
	 * Schedule this node for update by placing it in head queue
	 */
	bool Schedule(SG_QList& head);

	/**
	 * Used during Scenegraph update
	 */
	static SG_Node *GetNextScheduled(SG_QList& head);

	/**
	 * Make this node ready for schedule on next update. This is needed for nodes
	 * that must always be updated (slow parent, bone parent)
	 */
	bool Reschedule(SG_QList& head);

	/**
	 * Used during Scenegraph update
	 */
	static SG_Node *GetNextRescheduled(SG_QList& head);

	/**
	 * Node replication functions.
	 */
	SG_Node *GetReplica();

	/// Needed for replication

	SG_Callbacks& GetCallBackFunctions();

	SG_Object *GetObject() const;
	void SetObject(SG_Object *object);

	SG_Scene *GetScene() const;
	void SetScene(SG_Scene *scene);

	/// Change scene of this node and all its recursive children.
	void ReplaceScene(SG_Scene *scene);

	void ClearModified();
	void SetModified();
	void ClearDirty(DirtyFlag flag);

	/**
	 * Define the relationship this node has with it's parent
	 * node. You should pass an unshared instance of an SG_ParentRelation
	 * allocated on the heap to this method. Ownership of this
	 * instance is assumed by this class.
	 * You may call this function several times in the lifetime
	 * of a node to change the relationship dynamically.
	 * You must call this method before the first call to UpdateSpatialData().
	 * An assertion will be fired at run-time in debug if this is not
	 * the case.
	 * The relation is activated only if no controllers of this object
	 * updated the coordinates of the child.
	 */
	void SetParentRelation(SG_ParentRelation *relation);
	SG_ParentRelation *GetParentRelation();

	/**
	 * Apply a translation relative to the current position.
	 * if local then the translation is assumed to be in the
	 * local coordinates of this object. If not then the translation
	 * is assumed to be in global coordinates. In this case
	 * you must provide a pointer to the parent of this object if it
	 * exists otherwise if there is no parent set it to nullptr
	 */
	void RelativeTranslate(const mt::vec3& trans, const SG_Node *parent, bool local);
	void SetLocalPosition(const mt::vec3& trans);
	void SetWorldPosition(const mt::vec3& trans);
	void RelativeRotate(const mt::mat3& rot, bool local);
	void SetLocalOrientation(const mt::mat3& rot);
	void SetWorldOrientation(const mt::mat3& rot);
	void RelativeScale(const mt::vec3& scale);
	void SetLocalScale(const mt::vec3& scale);
	void SetWorldScale(const mt::vec3& scale);

	const mt::vec3& GetLocalPosition() const;
	const mt::mat3& GetLocalOrientation() const;
	const mt::vec3& GetLocalScale() const;
	const mt::vec3& GetWorldPosition() const;
	const mt::mat3& GetWorldOrientation() const;
	const mt::vec3& GetWorldScaling() const;

	void SetWorldFromLocalTransform();
	mt::mat3x4 GetWorldTransform() const;
	mt::mat3x4 GetLocalTransform() const;

	bool IsNegativeScaling() const;

	bool ComputeWorldTransforms(const SG_Node *parent, bool& parentUpdated);

	const std::shared_ptr<SG_Familly>& GetFamilly() const;
	void SetFamilly(const std::shared_ptr<SG_Familly>& familly);

	bool IsModified();
	bool IsDirty(DirtyFlag flag);

protected:
	friend class SG_Controller;
	friend class KX_BoneParentRelation;
	friend class KX_VertexParentRelation;
	friend class KX_SlowParentRelation;
	friend class KX_NormalParentRelation;

	/**
	 * Add a child to this object.
	 * This just stores a pointer to the child and does not
	 * make a deep copy.
	 */
	void AddChild(SG_Node *child);

	/**
	 * Remove a child node from this object. This just removes the child
	 * pointer from the list of children - it does not destroy the child.
	 * This does not inform the child that this node is no longer it's parent.
	 * If the node was not a child of this object no action is performed.
	 */
	void RemoveChild(SG_Node *child);

	void ActivateUpdateTransformCallback();

	void Reschedule();

	/**
	 * Update the world coordinates of this spatial node.
	 */
	void UpdateSpatialData(const SG_Node *parent, bool& parentUpdated);

private:
	void UpdateWorldDataThreadSchedule(bool parentUpdated = false);

	/// The owned object of this node.
	SG_Object *m_object;
	/// The scene of this node.
	SG_Scene *m_scene;

	SG_Callbacks m_callbacks;

	/// The list of children of this node.
	NodeList m_children;
	/// The parent of this node may be nullptr.
	SG_Node *m_parent;

	mt::vec3 m_localPosition;
	mt::mat3 m_localRotation;
	mt::vec3 m_localScaling;

	mt::vec3 m_worldPosition;
	mt::mat3 m_worldRotation;
	mt::vec3 m_worldScaling;

	std::unique_ptr<SG_ParentRelation> m_parent_relation;

	std::shared_ptr<SG_Familly> m_familly;
	CM_ThreadMutex m_mutex;

	bool m_modified;
	unsigned short m_dirty;
};

#endif  // __SG_NODE_H__
