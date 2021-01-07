#include "KX_PythonComponentManager.h"
#include "KX_PythonComponent.h"
#include "KX_GameObject.h"

static bool compareObjectDepth(KX_GameObject *o1, KX_GameObject *o2)
{
  return o1->GetSGNode()->GetDepth() > o2->GetSGNode()->GetDepth();
}

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
  m_objects_changed = true;
}

void KX_PythonComponentManager::UnregisterObject(KX_GameObject *gameobj)
{
  std::vector<KX_GameObject *>::iterator it = std::find(m_objects.begin(), m_objects.end(), gameobj);
  if (it != m_objects.end()) {
    m_objects.erase(it);
  }
  m_objects_changed = true;
}

void KX_PythonComponentManager::UpdateComponents()
{
  if (m_objects_changed) {
    std::sort(m_objects.begin(), m_objects.end(), compareObjectDepth);

    m_objects_changed = false;
  }

  /* Update object components, we copy the object pointer in a second list to make
   * sure that we iterate on a list which will not be modified, indeed components
   * can add objects in theirs update.
   */
  const std::vector<KX_GameObject *> objects = m_objects;
  for (KX_GameObject *gameobj : objects) {
    gameobj->UpdateComponents();
  }
}
