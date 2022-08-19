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

/** \file SCA_EventManager.h
 *  \ingroup gamelogic
 */

#pragma once

#include <algorithm>
#include <set>
#include <vector>

class SCA_ISensor;

class SCA_EventManager {
 protected:
  class SCA_LogicManager
      *m_logicmgr; /* all event manager subclasses use this (other then TimeEventManager) */

  std::vector<SCA_ISensor *> m_sensors;

 public:
  enum EVENT_MANAGER_TYPE {
    KEYBOARD_EVENTMGR = 0,
    MOUSE_EVENTMGR,
    ALWAYS_EVENTMGR,
    TOUCH_EVENTMGR,
    PROPERTY_EVENTMGR,
    TIME_EVENTMGR,
    RANDOM_EVENTMGR,
    RAY_EVENTMGR,
    NETWORK_EVENTMGR,
    JOY_EVENTMGR,
    ACTUATOR_EVENTMGR,
    BASIC_EVENTMGR
  };

  SCA_EventManager(SCA_LogicManager *logicmgr, EVENT_MANAGER_TYPE mgrtype);
  virtual ~SCA_EventManager();

  virtual bool RemoveSensor(class SCA_ISensor *sensor);
  virtual void NextFrame(double curtime, double fixedtime);
  virtual void NextFrame();
  virtual void UpdateFrame();
  virtual void EndFrame();
  virtual bool RegisterSensor(class SCA_ISensor *sensor);
  int GetType();
  // SG_DList &GetSensors() { return m_sensors; }

  void Replace_LogicManager(SCA_LogicManager *logicmgr)
  {
    m_logicmgr = logicmgr;
  }

 protected:
  EVENT_MANAGER_TYPE m_mgrtype;
};
