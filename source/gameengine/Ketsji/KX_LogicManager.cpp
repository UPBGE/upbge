#include "KX_LogicManager.h"
#include "KX_PythonComponent.h"
#include "KX_GameObject.h"

#include "CM_List.h"

KX_LogicManager::KX_LogicManager()
{
}

KX_LogicManager::~KX_LogicManager()
{
}

void KX_LogicManager::RegisterObject(KX_GameObject *gameobj)
{
	// Always register only once an object.
	m_objects.push_back(gameobj);
}

void KX_LogicManager::UnregisterObject(KX_GameObject *gameobj)
{
	CM_ListRemoveIfFound(m_objects, gameobj);
}

void KX_LogicManager::Update()
{
	/* Update object components and nodes, we copy the object pointer in a second list to make
	 * sure that we iterate on a list which will not be modified, indeed components and nodes
	 * can add objects in theirs update.
	 */
	const std::vector<KX_GameObject *> objects = m_objects;
	for (KX_GameObject *gameobj : objects) {
		gameobj->UpdateLogic();
	}
}

void KX_LogicManager::Merge(KX_LogicManager& other)
{
	m_objects.insert(m_objects.end(), other.m_objects.begin(), other.m_objects.end());
	other.m_objects.clear();
}
