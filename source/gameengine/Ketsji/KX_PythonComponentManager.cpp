#include "KX_PythonComponentManager.h"
#include "KX_PythonComponent.h"
#include "KX_GameObject.h"

#include "CM_List.h"

KX_PythonComponentManager::KX_PythonComponentManager()
{
}

KX_PythonComponentManager::~KX_PythonComponentManager()
{
}

void KX_PythonComponentManager::RegisterObject(KX_GameObject *gameobj)
{
	// Always register only once an object.
	m_objects.push_back(gameobj);
}

void KX_PythonComponentManager::UnregisterObject(KX_GameObject *gameobj)
{
	CM_ListRemoveIfFound(m_objects, gameobj);
}

void KX_PythonComponentManager::UpdateComponents()
{
	/* Update object components, we copy the object pointer in a second list to make
	 * sure that we iterate on a list which will not be modified, indeed components
	 * can add objects in theirs update.
	 */
	const std::vector<KX_GameObject *> objects = m_objects;
	for (KX_GameObject *gameobj : objects) {
		gameobj->UpdateComponents();
	}
}

void KX_PythonComponentManager::Merge(KX_PythonComponentManager& other)
{
	m_objects.insert(m_objects.end(), other.m_objects.begin(), other.m_objects.end());
	other.m_objects.clear();
}
