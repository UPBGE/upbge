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
/** \file SCA_IObject.h
 *  \ingroup gamelogic
 *  \brief An abstract object that has some logic, python scripting and
 *   reference counting Note: transformation stuff has been moved to
 *   SceneGraph
 */

#pragma once

#include <vector>

#include "KX_PythonProxy.h"
#include "SG_QList.h"

class SCA_IObject;
class SCA_ISensor;
class SCA_IController;
class SCA_IActuator;

typedef std::vector<SCA_ISensor *> SCA_SensorList;
typedef std::vector<SCA_IController *> SCA_ControllerList;
typedef std::vector<SCA_IActuator *> SCA_ActuatorList;
typedef std::vector<SCA_IObject *> SCA_ObjectList;

class SCA_IObject : public KX_PythonProxy {

  Py_Header

      protected :

      SCA_SensorList m_sensors;
  SCA_ControllerList m_controllers;
  SCA_ActuatorList m_actuators;
  /// Actuators that use a pointer to this object.
  SCA_ActuatorList m_registeredActuators;
  /// Objects that hold reference to this object.
  SCA_ObjectList m_registeredObjects;

  /** SG_Dlist: element of objects with active actuators
   *            Head: SCA_LogicManager::m_activeActuators
   *  SG_QList: Head of active actuators list on this object
   *            Elements: SCA_IActuator
   */
  SG_QList m_activeActuators;

  /** SG_Dlist: element of list os lists with active controllers
   *            Head: SCA_LogicManager::m_activeControllers
   *  SG_QList: Head of active controller list on this object
   *            Elements: SCA_IController
   */
  SG_QList m_activeControllers;

  /** SG_Dlist: element of list of lists of active controllers
   *            Head: SCA_LogicManager::m_activeControllers
   *  SG_QList: Head of active bookmarked controller list globally
   *            Elements: SCA_IController with bookmark option
   */
  static SG_QList m_activeBookmarkedControllers;

  /// Ignore updates?
  bool m_logicSuspended;

  /// Init state of object (used when object is created).
  unsigned int m_initState;

  /// Current state = bit mask of state that are active.
  unsigned int m_state;

  /// State used to suspend/restore logic
  unsigned int m_backupState;

  /// Pointer inside state actuator list for sorting.
  SG_QList *m_firstState;

 public:
  SCA_IObject();
  virtual ~SCA_IObject();

  SCA_ControllerList &GetControllers();
  SCA_SensorList &GetSensors();
  SCA_ActuatorList &GetActuators();
  SG_QList &GetActiveActuators();
  SG_QList &GetActiveControllers();
  static SG_QList &GetActiveBookmarkedControllers();

  void AddSensor(SCA_ISensor *act);
  void ReserveSensor(int num);
  void AddController(SCA_IController *act);
  void ReserveController(int num);
  void AddActuator(SCA_IActuator *act);
  void ReserveActuator(int num);
  void RegisterActuator(SCA_IActuator *act);
  void UnregisterActuator(SCA_IActuator *act);

  void RegisterObject(SCA_IObject *objs);
  void UnregisterObject(SCA_IObject *objs);
  /**
   * UnlinkObject(...)
   * this object is informed that one of the object to which it holds a reference is deleted
   * returns true if there was indeed a reference.
   */
  virtual bool UnlinkObject(SCA_IObject *clientobj);

  SCA_ISensor *FindSensor(const std::string &sensorname);
  SCA_IActuator *FindActuator(const std::string &actuatorname);
  SCA_IController *FindController(const std::string &controllername);

  virtual void ReParentLogic();

  /// Suspend all progress.
  void SuspendLogic(void);

  /// Resume progress.
  void ResumeLogic(void);

  /// Set init state.
  void SetInitState(unsigned int initState);

  /// Initialize the state when object is created.
  void ResetState();

  /// Set the object state.
  void SetState(unsigned int state);

  /// Get the object state.
  unsigned int GetState();

  SG_QList **GetFirstState();
  void SetFirstState(SG_QList *firstState);

  virtual int GetGameObjectType() const;

  typedef enum ObjectTypes {
    OBJ_ARMATURE = 0,
    OBJ_CAMERA = 1,
    OBJ_LIGHT = 2,
    OBJ_TEXT = 3
  } ObjectTypes;
};
