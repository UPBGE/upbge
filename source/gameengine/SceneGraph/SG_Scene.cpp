#include "SG_Scene.h"

#include "BLI_utildefines.h"

void SG_Scene::Schedule(SG_Node *node)
{
	node->Schedule(m_head);
}

void SG_Scene::Reschedule(SG_Node *node)
{
	node->Reschedule(m_head);
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
