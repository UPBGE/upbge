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

/** \file SCA_TimeEventManager.h
 *  \ingroup gamelogic
 */

#pragma once

#include <vector>

#include "EXP_Value.h"
#include "SCA_EventManager.h"

class SCA_TimeEventManager : public SCA_EventManager {
  std::vector<EXP_Value *> m_timevalues;  // values that need their time updated regularly

 public:
  SCA_TimeEventManager(class SCA_LogicManager *logicmgr);
  virtual ~SCA_TimeEventManager();

  virtual void NextFrame(double curtime, double fixedtime);
  virtual bool RegisterSensor(class SCA_ISensor *sensor);
  virtual bool RemoveSensor(class SCA_ISensor *sensor);
  void AddTimeProperty(EXP_Value *timeval);
  void RemoveTimeProperty(EXP_Value *timeval);

  /** Narrow checkpoint hooks. Reserve is the only potentially allocating operation. */
  bool PersistenceContainsTimeProperty(const EXP_Value *timeval) const;
  const std::vector<EXP_Value *> &PersistenceTimeProperties() const
  {
    return m_timevalues;
  }
  bool PersistenceReserveTimeProperties(size_t additional);
  bool PersistenceAttachPreparedTimeProperty(EXP_Value *timeval);
  bool PersistenceDetachTimeProperty(EXP_Value *timeval);

  std::vector<EXP_Value *> GetTimeValues();
};
