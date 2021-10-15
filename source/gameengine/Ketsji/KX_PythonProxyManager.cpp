/**
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
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

#include "KX_PythonProxyManager.h"

#include "CM_List.h"
#include "KX_GameObject.h"

static bool compareObjectDepth(KX_GameObject *o1, KX_GameObject *o2)
{
  return o1->GetSGNode()->GetDepth() > o2->GetSGNode()->GetDepth();
}

KX_PythonProxyManager::KX_PythonProxyManager()
{
}

KX_PythonProxyManager::~KX_PythonProxyManager()
{
}

void KX_PythonProxyManager::Register(KX_GameObject *gameobj)
{
  // Always register only once an object.
  m_objects.push_back(gameobj);
  m_objects_changed = true;
}

void KX_PythonProxyManager::Unregister(KX_GameObject *gameobj)
{
  CM_ListRemoveIfFound(m_objects, gameobj);
  m_objects_changed = true;
}

void KX_PythonProxyManager::Update()
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
    gameobj->Update();
  }
}
