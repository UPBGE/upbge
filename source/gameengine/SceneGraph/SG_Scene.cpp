#include "SG_Scene.h"

#include "CM_List.h"

#include "BLI_utildefines.h"

void SG_Scene::Schedule(SG_Node *node)
{
	node->Schedule(m_head);
}

void SG_Scene::Reschedule(SG_Node *node)
{
	node->Reschedule(m_head);
}

void SG_Scene::AddRootNode(SG_Node *node)
{
	m_rootNodes.push_back(node);
}

void SG_Scene::RemoveRootNode(SG_Node *node)
{
	CM_ListRemoveIfFound(m_rootNodes, node);
}

void SG_Scene::DestructRootNodes()
{
	for (SG_Node *node : m_rootNodes) {
		// Destruct linked object.
		node->Destruct();
	}
}

void SG_Scene::UpdateParents()
{
	// We use the SG dynamic list
	SG_Node *node;

	while ((node = SG_Node::GetNextScheduled(m_head))) {
		node->UpdateWorldData();
	}

	// The list must be empty here
	BLI_assert(m_head.Empty());
	// Some nodes may be ready for reschedule, move them to schedule list for next time.
	while ((node = SG_Node::GetNextRescheduled(m_head))) {
		node->Schedule(m_head);
	}
}

void SG_Scene::Merge(SG_Scene *other)
{
	m_rootNodes.insert(m_rootNodes.begin(), other->m_rootNodes.begin(), other->m_rootNodes.end());
}
