#include "KX_PythonComponentManager.h"
#include "KX_PythonComponent.h"
#include "KX_GameObject.h"

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
  std::vector<KX_GameObject *>::iterator it = std::find(m_objects.begin(), m_objects.end(), gameobj);
  if (it != m_objects.end()) {
    m_objects.erase(it);
  }
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
