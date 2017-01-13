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
#include "SG_Controller.h"

#include <algorithm>

SG_Stage gSG_Stage = SG_STAGE_UNKNOWN;

SG_Node::SG_Node(void *clientobj, void *clientinfo, SG_Callbacks& callbacks)
	:SG_QList(),
	m_SGclientObject(clientobj),
	m_SGclientInfo(clientinfo),
	m_callbacks(callbacks),
	m_SGparent(NULL),
	m_localPosition(0.0f, 0.0f, 0.0f),
	m_localRotation(1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f),
	m_localScaling(1.0f, 1.0f, 1.0f),
	m_worldPosition(0.0f, 0.0f, 0.0f),
	m_worldRotation(1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f),
	m_worldScaling(1.0f, 1.0f, 1.0f),
	m_parent_relation(NULL),
	m_bbox(MT_Vector3(-1.0f, -1.0f, -1.0f), MT_Vector3(1.0f, 1.0f, 1.0f)),
	m_modified(true),
	m_ogldirty(false)
{
}

SG_Node::SG_Node(const SG_Node & other)
	:SG_QList(),
	m_SGclientObject(other.m_SGclientObject),
	m_SGclientInfo(other.m_SGclientInfo),
	m_callbacks(other.m_callbacks),
	m_children(other.m_children),
	m_SGparent(other.m_SGparent),
	m_localPosition(other.m_localPosition),
	m_localRotation(other.m_localRotation),
	m_localScaling(other.m_localScaling),
	m_worldPosition(other.m_worldPosition),
	m_worldRotation(other.m_worldRotation),
	m_worldScaling(other.m_worldScaling),
	m_parent_relation(other.m_parent_relation->NewCopy()),
	m_bbox(other.m_bbox),
	m_modified(true),
	m_ogldirty(false)
{
}

SG_Node::~SG_Node()
{
	SGControllerList::iterator contit;

	for (contit = m_SGcontrollers.begin(); contit != m_SGcontrollers.end(); ++contit) {
		delete (*contit);
	}

	delete m_parent_relation;
}

SG_Node *SG_Node::GetSGReplica()
{
	SG_Node *replica = new SG_Node(*this);
	if (replica == NULL) {
		return NULL;
	}

	ProcessSGReplica(&replica);

	return replica;
}

void SG_Node::ProcessSGReplica(SG_Node **replica)
{
	// Apply the replication call back function.
	if (!ActivateReplicationCallback(*replica)) {
		delete (*replica);
		*replica = NULL;
		return;
	}

	// clear the replica node of it's parent.
	(*replica)->m_SGparent = NULL;

	if (m_children.begin() != m_children.end()) {
		// if this node has children, the replica has too, so clear and clone children
		(*replica)->ClearSGChildren();

		NodeList::iterator childit;
		for (childit = m_children.begin(); childit != m_children.end(); ++childit) {
			SG_Node *childnode = (*childit)->GetSGReplica();
			if (childnode) {
				(*replica)->AddChild(childnode);
			}
		}
	}
	// Nodes without children and without client object are
	// not worth to keep, they will just take up CPU
	// This can happen in partial replication of hierarchy
	// during group duplication.
	if ((*replica)->m_children.empty() &&
	    (*replica)->GetSGClientObject() == NULL)
	{
		delete (*replica);
		*replica = NULL;
	}
}

void SG_Node::Destruct()
{
	// Not entirely sure what Destruct() expects to happen.
	// I think it probably means just to call the DestructionCallback
	// in the right order on all the children - rather than free any memory

	// We'll delete m_parent_relation now anyway.

	delete(m_parent_relation);
	m_parent_relation = NULL;

	if (m_children.begin() != m_children.end()) {
		NodeList::iterator childit;
		for (childit = m_children.begin(); childit != m_children.end(); ++childit) {
			// call the SG_Node destruct method on each of our children }-)
			(*childit)->Destruct();
		}
	}

	ActivateDestructionCallback();
}

const SG_Node *SG_Node::GetRootSGParent() const
{
	return (m_SGparent ? (const SG_Node *)m_SGparent->GetRootSGParent() : (const SG_Node *)this);
}

bool SG_Node::IsAncessor(const SG_Node *child) const
{
	return (!child->m_SGparent) ? false :
	       (child->m_SGparent == this) ? true : IsAncessor(child->m_SGparent);
}

void SG_Node::DisconnectFromParent()
{
	if (m_SGparent) {
		m_SGparent->RemoveChild(this);
		m_SGparent = NULL;
	}
}

void SG_Node::AddChild(SG_Node *child)
{
	m_children.push_back(child);
	child->SetSGParent(this); // this way ?
}

void SG_Node::RemoveChild(SG_Node *child)
{
	NodeList::iterator childfound = find(m_children.begin(), m_children.end(), child);

	if (childfound != m_children.end()) {
		m_children.erase(childfound);
	}
}

void SG_Node::UpdateWorldData(double time, bool parentUpdated)
{
	if (UpdateSpatialData(GetSGParent(), time, parentUpdated)) {
		// to update the
		ActivateUpdateTransformCallback();
	}

	// The node is updated, remove it from the update list
	Delink();

	// update children's worlddata
	for (NodeList::iterator it = m_children.begin(); it != m_children.end(); ++it) {
		(*it)->UpdateWorldData(time, parentUpdated);
	}
}

void SG_Node::SetSimulatedTime(double time, bool recurse)
{
	// update the controllers of this node.
	SetControllerTime(time);

	// update children's simulate time.
	if (recurse) {
		for (NodeList::iterator it = m_children.begin(); it != m_children.end(); ++it) {
			(*it)->SetSimulatedTime(time, recurse);
		}
	}
}

void SG_Node::AddSGController(SG_Controller *cont)
{
	m_SGcontrollers.push_back(cont);
}

void SG_Node::RemoveSGController(SG_Controller *cont)
{
	SGControllerList::iterator contit;
	
	m_SGcontrollers.erase(std::remove(m_SGcontrollers.begin(), m_SGcontrollers.end(), cont));
}

void SG_Node::RemoveAllControllers()
{
	m_SGcontrollers.clear();
}

void SG_Node::SetControllerTime(double time)
{
	SGControllerList::iterator contit;

	for (contit = m_SGcontrollers.begin(); contit != m_SGcontrollers.end(); ++contit)
	{
		(*contit)->SetSimulatedTime(time);
	}
}


void SG_Node::SetParentRelation(SG_ParentRelation *relation)
{
	delete m_parent_relation;
	m_parent_relation = relation;
	SetModified();
}

/**
 * Update Spatial Data.
 * Calculates WorldTransform., (either doing its self or using the linked SGControllers)
 */
bool SG_Node::UpdateSpatialData(const SG_Node *parent, double time, bool& parentUpdated)
{
	bool bComputesWorldTransform = false;

	// update spatial controllers

	SGControllerList::iterator cit = GetSGControllerList().begin();
	SGControllerList::const_iterator c_end = GetSGControllerList().end();

	for (; cit != c_end; ++cit) {
		if ((*cit)->Update(time)) {
			bComputesWorldTransform = true;
		}
	}

	// If none of the objects updated our values then we ask the
	// parent_relation object owned by this class to update
	// our world coordinates.

	if (!bComputesWorldTransform) {
		bComputesWorldTransform = ComputeWorldTransforms(parent, parentUpdated);
	}

	return bComputesWorldTransform;
}

/**
 * Position and translation methods
 */
void SG_Node::RelativeTranslate(const MT_Vector3& trans, const SG_Node *parent, bool local)
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

/**
 * Scaling methods.
 */

/**
 * Orientation and rotation methods.
 */
void SG_Node::RelativeRotate(const MT_Matrix3x3& rot, bool local)
{
	m_localRotation = m_localRotation * (
		local ?
		rot
		:
		(GetWorldOrientation().inverse() * rot * GetWorldOrientation()));
	SetModified();
}

MT_Transform SG_Node::GetWorldTransform() const
{
	return MT_Transform(m_worldPosition,
	                    m_worldRotation.scaled(
							m_worldScaling[0], m_worldScaling[1], m_worldScaling[2]));
}

bool SG_Node::inside(const MT_Vector3 &point) const
{
	MT_Scalar radius = m_worldScaling[m_worldScaling.closestAxis()] * m_bbox.GetRadius();
	return (m_worldPosition.distance2(point) <= radius * radius) ?
	       m_bbox.transform(GetWorldTransform()).inside(point) :
	       false;
}

void SG_Node::getBBox(MT_Vector3 *box) const
{
	m_bbox.get(box, GetWorldTransform());
}

void SG_Node::getAABBox(MT_Vector3 *box) const
{
	m_bbox.getaa(box, GetWorldTransform());
}

