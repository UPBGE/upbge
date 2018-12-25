/* ***** BEGIN GPL LICENSE BLOCK *****
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

/** \file gameengine/SceneGraph/SG_Node.cpp
 *  \ingroup bgesg
 */


#include "SG_Node.h"
#include "SG_Scene.h"
#include "SG_Familly.h"
#include "SG_Controller.h"

#include "CM_List.h"

#include "BLI_utildefines.h"

static CM_ThreadMutex scheduleMutex;
static CM_ThreadMutex transformMutex;

SG_Node::SG_Node(SG_Object *object, SG_Scene *scene, SG_Callbacks& callbacks, SG_ParentRelation *relation)
	:SG_QList(),
	m_object(object),
	m_scene(scene),
	m_callbacks(callbacks),
	m_parent(nullptr),
	m_localPosition(mt::zero3),
	m_localRotation(mt::mat3::Identity()),
	m_localScaling(mt::one3),
	m_worldPosition(mt::zero3),
	m_worldRotation(mt::mat3::Identity()),
	m_worldScaling(mt::one3),
	m_parent_relation(relation),
	m_familly(new SG_Familly()),
	m_modified(true),
	m_dirty(DIRTY_NONE)
{
	BLI_assert(m_scene);
	BLI_assert((m_object != nullptr) == (m_callbacks.m_updatefunc != nullptr));

	m_scene->AddRootNode(this);
}

SG_Node::SG_Node(const SG_Node& other)
	:SG_QList(),
	m_object(nullptr),
	m_scene(other.m_scene),
	m_callbacks(other.m_callbacks),
	m_parent(nullptr),
	m_localPosition(other.m_localPosition),
	m_localRotation(other.m_localRotation),
	m_localScaling(other.m_localScaling),
	m_worldPosition(other.m_worldPosition),
	m_worldRotation(other.m_worldRotation),
	m_worldScaling(other.m_worldScaling),
	m_parent_relation(other.m_parent_relation ? other.m_parent_relation->NewCopy() : nullptr),
	m_familly(new SG_Familly()),
	m_dirty(DIRTY_NONE)
{
}

SG_Node::~SG_Node()
{
	// Delete all children.
	for (SG_Node *childnode : m_children) {
		delete childnode;
	}

	// Destruct node object.
	if (m_object) {
		m_scene->DestructNodeObject(this, m_object);
	}

	// Remove the node from scene roots.
	if (!m_parent) {
		m_scene->RemoveRootNode(this);
	}
}

SG_Node *SG_Node::GetReplica()
{
	// Avoid duplicate leaf without object.
	if (m_children.empty() && !m_object) {
		return nullptr;
	}

	// Duplicate the node.
	SG_Node *replica = new SG_Node(*this);

	// Try to duplicate the object.
	if (m_object) {
		SG_Object *object = m_scene->ReplicateNodeObject(replica, m_object);
		// The scene can refuse an object duplication.
		if (!object) {
			delete replica;
			return nullptr;
		}
		replica->SetObject(object);
	}

	// Duplicate all children and link them to the new parent.
	for (SG_Node *child : m_children) {
		// Duplicate a child.
		SG_Node *replicanode = child->GetReplica();
		if (replicanode) {
			// Link it if succeded duplication.
			replicanode->SetParent(replica);
		}
	}

	// Add the duplicated node to root if the original node is a scene root.
	if (!m_parent) {
		m_scene->AddRootNode(replica);
	}

	return replica;
}

SG_Node *SG_Node::GetRootSGParent()
{
	return (m_parent ? m_parent->GetRootSGParent() : this);
}

bool SG_Node::IsAncessor(const SG_Node *child) const
{
	return (!child->m_parent) ? false :
	       (child->m_parent == this) ? true : IsAncessor(child->m_parent);
}

const NodeList& SG_Node::GetChildren() const
{
	return m_children;
}

void SG_Node::ClearSGChildren()
{
	m_children.clear();
}

SG_Node *SG_Node::GetParent() const
{
	return m_parent;
}

void SG_Node::SetParent(SG_Node *parent)
{
	BLI_assert(parent);

	// Detect new parenting.
	if (!m_parent) {
		// Remove ourself from scene roots.
		m_scene->RemoveRootNode(this);
	}
	else {
		// Remove ourself from our previous parent.
		m_parent->RemoveChild(this);
	}

	// Relink from child and parent.
	m_parent = parent;
	m_parent->AddChild(this);

	// Use the familly of our parent.
	SetFamilly(m_parent->GetFamilly());
}

void SG_Node::DisconnectFromParent()
{
	if (m_parent) {
		// Disconnect from our parent.
		m_parent->RemoveChild(this);
		m_parent = nullptr;

		// This node is now a scene root.
		m_scene->AddRootNode(this);

		// Use an unique family.
		SetFamilly(std::make_shared<SG_Familly>());
	}
}

bool SG_Node::IsVertexParent()
{
	if (m_parent_relation) {
		return m_parent_relation->IsVertexRelation();
	}
	return false;
}

bool SG_Node::IsSlowParent()
{
	if (m_parent_relation) {
		return m_parent_relation->IsSlowRelation();
	}
	return false;
}

void SG_Node::AddChild(SG_Node *child)
{
	m_children.push_back(child);
}

void SG_Node::RemoveChild(SG_Node *child)
{
	CM_ListRemoveIfFound(m_children, child);
}

void SG_Node::UpdateWorldData(bool parentUpdated)
{
	UpdateSpatialData(m_parent, parentUpdated);

	ActivateUpdateTransformCallback();

	// The node is updated, remove it from the update list
	Delink();

	// update children's worlddata
	for (SG_Node *childnode : m_children) {
		childnode->UpdateWorldData(parentUpdated);
	}
}

void SG_Node::UpdateWorldDataThread(bool parentUpdated)
{
	CM_ThreadSpinLock& famillyMutex = m_familly->GetMutex();
	famillyMutex.Lock();

	UpdateWorldDataThreadSchedule(parentUpdated);

	famillyMutex.Unlock();
}

void SG_Node::UpdateWorldDataThreadSchedule(bool parentUpdated)
{
	UpdateSpatialData(m_parent, parentUpdated);

	ActivateUpdateTransformCallback();

	scheduleMutex.Lock();
	// The node is updated, remove it from the update list
	Delink();
	scheduleMutex.Unlock();

	// update children's worlddata
	for (SG_Node *childnode : m_children) {
		childnode->UpdateWorldDataThreadSchedule(parentUpdated);
	}
}

bool SG_Node::Schedule(SG_QList& head)
{
	scheduleMutex.Lock();
	// Put top parent in front of list to make sure they are updated before their
	// children => the children will be udpated and removed from the list before
	// we get to them, should they be in the list too.
	const bool result = (m_parent) ? head.AddBack(this) : head.AddFront(this);
	scheduleMutex.Unlock();

	return result;
}

SG_Node *SG_Node::GetNextScheduled(SG_QList& head)
{
	scheduleMutex.Lock();
	SG_Node *result = static_cast<SG_Node *>(head.Remove());
	scheduleMutex.Unlock();

	return result;
}

bool SG_Node::Reschedule(SG_QList& head)
{
	scheduleMutex.Lock();
	const bool result = head.QAddBack(this);
	scheduleMutex.Unlock();

	return result;
}

SG_Node *SG_Node::GetNextRescheduled(SG_QList& head)
{
	scheduleMutex.Lock();
	SG_Node *result = static_cast<SG_Node *>(head.QRemove());
	scheduleMutex.Unlock();

	return result;
}

SG_Callbacks& SG_Node::GetCallBackFunctions()
{
	return m_callbacks;
}

SG_Object *SG_Node::GetObject() const
{
	return m_object;
}

void SG_Node::SetObject(SG_Object *object)
{
	m_object = object;
}

SG_Scene *SG_Node::GetScene() const
{
	return m_scene;
}

void SG_Node::SetScene(SG_Scene *scene)
{
	m_scene = scene;
}

void SG_Node::ReplaceScene(SG_Scene* scene)
{
	m_scene = scene;
	for (SG_Node *child : m_children) {
		child->ReplaceScene(scene);
	}
}

void SG_Node::ClearModified()
{
	m_modified = false;
	m_dirty = DIRTY_ALL;
}

void SG_Node::SetModified()
{
	m_modified = true;

	// HACK, this check assumes that the scheduled nodes are put on a DList (see SG_Node.h)
	// The early check on Empty() allows up to avoid calling the schedule function
	// when the node is already scheduled for update.
	scheduleMutex.Lock();
	const bool empty = Empty();
	scheduleMutex.Unlock();

	if (empty) {
		m_scene->Schedule(this);
	}
}

void SG_Node::ClearDirty(DirtyFlag flag)
{
	m_dirty &= ~flag;
}

void SG_Node::SetParentRelation(SG_ParentRelation *relation)
{
	m_parent_relation.reset(relation);
	SetModified();
}

SG_ParentRelation *SG_Node::GetParentRelation()
{
	return m_parent_relation.get();
}

/**
 * Update Spatial Data.
 * Calculates WorldTransform., (either doing its self or using the linked SGControllers)
 */
void SG_Node::UpdateSpatialData(const SG_Node *parent, bool& parentUpdated)
{
	// Ask the parent_relation object owned by this class to update our world coordinates.
	ComputeWorldTransforms(parent, parentUpdated);
}

/**
 * Position and translation methods
 */
void SG_Node::RelativeTranslate(const mt::vec3& trans, const SG_Node *parent, bool local)
{
	if (local) {
		m_localPosition += m_localRotation * trans;
	}
	else {
		if (parent) {
			m_localPosition += trans * parent->GetWorldOrientation();
		}
		else {
			m_localPosition += trans;
		}
	}
	SetModified();
}

void SG_Node::SetLocalPosition(const mt::vec3& trans)
{
	m_localPosition = trans;
	SetModified();
}

void SG_Node::SetWorldPosition(const mt::vec3& trans)
{
	m_worldPosition = trans;
}

/**
 * Scaling methods.
 */

/**
 * Orientation and rotation methods.
 */
void SG_Node::RelativeRotate(const mt::mat3& rot, bool local)
{
	m_localRotation = m_localRotation * (
		local ?
		rot
		:
		(GetWorldOrientation().Inverse() * rot * GetWorldOrientation()));
	SetModified();
}

void SG_Node::SetLocalOrientation(const mt::mat3& rot)
{
	m_localRotation = rot;
	SetModified();
}

void SG_Node::SetWorldOrientation(const mt::mat3& rot)
{
	m_worldRotation = rot;
}

void SG_Node::RelativeScale(const mt::vec3& scale)
{
	m_localScaling = m_localScaling * scale;
	SetModified();
}

void SG_Node::SetLocalScale(const mt::vec3& scale)
{
	m_localScaling = scale;
	SetModified();
}

void SG_Node::SetWorldScale(const mt::vec3& scale)
{
	m_worldScaling = scale;
}

const mt::vec3& SG_Node::GetLocalPosition() const
{
	return m_localPosition;
}

const mt::mat3& SG_Node::GetLocalOrientation() const
{
	return m_localRotation;
}

const mt::vec3& SG_Node::GetLocalScale() const
{
	return m_localScaling;
}

const mt::vec3& SG_Node::GetWorldPosition() const
{
	return m_worldPosition;
}

const mt::mat3& SG_Node::GetWorldOrientation() const
{
	return m_worldRotation;
}

const mt::vec3& SG_Node::GetWorldScaling() const
{
	return m_worldScaling;
}

void SG_Node::SetWorldFromLocalTransform()
{
	m_worldPosition = m_localPosition;
	m_worldScaling = m_localScaling;
	m_worldRotation = m_localRotation;
}

mt::mat3x4 SG_Node::GetWorldTransform() const
{
	return mt::mat3x4(m_worldRotation, m_worldPosition, m_worldScaling);
}

mt::mat3x4 SG_Node::GetLocalTransform() const
{
	return mt::mat3x4(m_localRotation, m_localPosition, m_localScaling);
}

bool SG_Node::IsNegativeScaling() const
{
	return (m_worldScaling.x * m_worldScaling.y * m_worldScaling.z) < 0.0f;
}

bool SG_Node::ComputeWorldTransforms(const SG_Node *parent, bool& parentUpdated)
{
	return m_parent_relation->UpdateChildCoordinates(this, parent, parentUpdated);
}

const std::shared_ptr<SG_Familly>& SG_Node::GetFamilly() const
{
	BLI_assert(m_familly != nullptr);

	return m_familly;
}

void SG_Node::SetFamilly(const std::shared_ptr<SG_Familly>& familly)
{
	BLI_assert(familly != nullptr);

	m_familly = familly;
	for (SG_Node *child : m_children) {
		child->SetFamilly(m_familly);
	}
}

bool SG_Node::IsModified()
{
	return m_modified;
}

bool SG_Node::IsDirty(DirtyFlag flag)
{
	return (m_dirty & flag);
}

void SG_Node::ActivateUpdateTransformCallback()
{
	if (m_callbacks.m_updatefunc) {
		// Call client provided update func.
		transformMutex.Lock();
		m_callbacks.m_updatefunc(this, m_object, m_scene);
		transformMutex.Unlock();
	}
}

void SG_Node::Reschedule()
{
	m_scene->Reschedule(this);
}
