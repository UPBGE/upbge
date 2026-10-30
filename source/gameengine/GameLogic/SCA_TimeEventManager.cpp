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

/** \file gameengine/GameLogic/SCA_TimeEventManager.cpp
 *  \ingroup gamelogic
 */

#ifdef _MSC_VER
/* This warning tells us about truncation of __long__ stl-generated names.
 * It can occasionally cause DevStudio to have internal compiler warnings. */
#  pragma warning(disable : 4786)
#endif

#include "SCA_TimeEventManager.h"

#include <algorithm>

#include "CM_List.h"
#include "EXP_FloatValue.h"

using namespace blender;

SCA_TimeEventManager::SCA_TimeEventManager(SCA_LogicManager *logicmgr)
    : SCA_EventManager(nullptr, TIME_EVENTMGR)
{
}

SCA_TimeEventManager::~SCA_TimeEventManager()
{
  for (EXP_Value *prop : m_timevalues) {
    prop->Release();
  }
}

bool SCA_TimeEventManager::RegisterSensor(SCA_ISensor *sensor)
{
  // not yet
  return false;
}

bool SCA_TimeEventManager::RemoveSensor(SCA_ISensor *sensor)
{
  // empty
  return false;
}

void SCA_TimeEventManager::NextFrame(double curtime, double fixedtime)
{
  if (m_timevalues.empty() && fixedtime <= 0.0) {
    return;
  }

  EXP_FloatValue *floatval = new EXP_FloatValue(curtime);

  // update sensors, but ... need deltatime !
  for (EXP_Value *prop : m_timevalues) {
    float newtime = prop->GetNumber() + fixedtime;
    floatval->SetFloat(newtime);
    prop->SetValue(floatval);
  }

  floatval->Release();
}

void SCA_TimeEventManager::AddTimeProperty(EXP_Value *timeval)
{
  timeval->AddRef();
  try {
    m_timevalues.push_back(timeval);
  }
  catch (...) {
    timeval->Release();
    throw;
  }
}

void SCA_TimeEventManager::RemoveTimeProperty(EXP_Value *timeval)
{
  if (CM_ListRemoveIfFound(m_timevalues, timeval)) {
    timeval->Release();
  }
}

bool SCA_TimeEventManager::PersistenceContainsTimeProperty(const EXP_Value *timeval) const
{
  return std::find(m_timevalues.begin(), m_timevalues.end(), timeval) != m_timevalues.end();
}

bool SCA_TimeEventManager::PersistenceReserveTimeProperties(const size_t additional)
{
  if (additional > m_timevalues.max_size() - m_timevalues.size()) {
    return false;
  }
  try {
    m_timevalues.reserve(m_timevalues.size() + additional);
    return true;
  }
  catch (...) {
    return false;
  }
}

bool SCA_TimeEventManager::PersistenceAttachPreparedTimeProperty(EXP_Value *timeval)
{
  if (timeval == nullptr || PersistenceContainsTimeProperty(timeval) ||
      m_timevalues.size() == m_timevalues.capacity())
  {
    return false;
  }
  timeval->AddRef();
  m_timevalues.push_back(timeval);
  return true;
}

bool SCA_TimeEventManager::PersistenceDetachTimeProperty(EXP_Value *timeval)
{
  const auto found = std::find(m_timevalues.begin(), m_timevalues.end(), timeval);
  if (found == m_timevalues.end()) {
    return false;
  }
  m_timevalues.erase(found);
  timeval->Release();
  return true;
}

std::vector<EXP_Value *> SCA_TimeEventManager::GetTimeValues()
{
  return m_timevalues;
}
