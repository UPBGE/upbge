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

/** \file gameengine/GameLogic/SCA_IScene.cpp
 *  \ingroup gamelogic
 */

#include "SCA_IScene.h"

#define DEBUG_MAX_DISPLAY 100

SCA_DebugProp::SCA_DebugProp(SCA_IObject *gameobj, const std::string &name)
    : m_obj(gameobj), m_name(name)
{
}

SCA_DebugProp::~SCA_DebugProp()
{
}

SCA_IScene::SCA_IScene()
{
}

void SCA_IScene::RemoveAllDebugProperties()
{
  m_debugList.clear();
}

SCA_IScene::~SCA_IScene()
{
  RemoveAllDebugProperties();
}

const std::vector<SCA_DebugProp> &SCA_IScene::GetDebugProperties() const
{
  return m_debugList;
}

bool SCA_IScene::PropertyInDebugList(SCA_IObject *gameobj, const std::string &name)
{
  for (const SCA_DebugProp &prop : m_debugList) {
    if (prop.m_obj == gameobj && prop.m_name == name) {
      return true;
    }
  }
  return false;
}

bool SCA_IScene::ObjectInDebugList(SCA_IObject *gameobj)
{
  for (const SCA_DebugProp &prop : m_debugList) {
    if (prop.m_obj == gameobj) {
      return true;
    }
  }
  return false;
}

void SCA_IScene::AddDebugProperty(SCA_IObject *gameobj, const std::string &name)
{
  if (m_debugList.size() < DEBUG_MAX_DISPLAY) {
    m_debugList.emplace_back(gameobj, name);
  }
}

void SCA_IScene::RemoveDebugProperty(SCA_IObject *gameobj, const std::string &name)
{
  for (std::vector<SCA_DebugProp>::iterator it = m_debugList.begin(); it != m_debugList.end();) {
    const SCA_DebugProp &prop = *it;

    if (prop.m_obj == gameobj && prop.m_name == name) {
      it = m_debugList.erase(it);
      break;
    }
    else {
      ++it;
    }
  }
}

void SCA_IScene::RemoveObjectDebugProperties(SCA_IObject *gameobj)
{
  for (std::vector<SCA_DebugProp>::iterator it = m_debugList.begin(); it != m_debugList.end();) {
    const SCA_DebugProp &prop = *it;

    if (prop.m_obj == gameobj) {
      it = m_debugList.erase(it);
      continue;
    }
    else {
      ++it;
    }
  }
}
