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

/** \file SCA_IActuator.h
 *  \ingroup gamelogic
 */

#pragma once

#include "SCA_IController.h"

/**
 * Use of SG_DList : None
 * Use of SG_QList : element of activated actuator list of their owner
 *                   Head: SCA_IObject::m_activeActuators
 */
class SCA_IActuator : public SCA_ILogicBrick {
  friend class SCA_LogicManager;

 protected:
  int m_type;
  /// Number of active links to controllers when 0, the actuator is automatically stopped.
  int m_links;
  bool m_posevent;
  bool m_negevent;

  std::vector<SCA_IController *> m_linkedcontrollers;

  void RemoveAllEvents();

 public:
  /**
   * This class also inherits the default copy constructors
   */
  enum KX_ACTUATOR_TYPE {
    KX_ACT_OBJECT,
    KX_ACT_IPO,
    KX_ACT_CAMERA,
    KX_ACT_COLLECTION,
    KX_ACT_SOUND,
    KX_ACT_PROPERTY,
    KX_ACT_ADD_OBJECT,
    KX_ACT_END_OBJECT,
    KX_ACT_DYNAMIC,
    KX_ACT_REPLACE_MESH,
    KX_ACT_TRACKTO,
    KX_ACT_CONSTRAINT,
    KX_ACT_SCENE,
    KX_ACT_RANDOM,
    KX_ACT_MESSAGE,
    KX_ACT_ACTION,
    KX_ACT_CD,
    KX_ACT_GAME,
    KX_ACT_VIBRATION,
    KX_ACT_VISIBILITY,
    KX_ACT_2DFILTER,
    KX_ACT_PARENT,
    KX_ACT_SHAPEACTION,
    KX_ACT_STATE,
    KX_ACT_ARMATURE,
    KX_ACT_STEERING,
    KX_ACT_MOUSE,
  };

  SCA_IActuator(SCA_IObject *gameobj, KX_ACTUATOR_TYPE type);
  virtual ~SCA_IActuator();

  /**
   * UnlinkObject(...)
   * Certain actuator use gameobject pointers (like TractTo actuator)
   * This function can be called when an object is removed to make
   * sure that the actuator will not use it anymore.
   */

  virtual bool UnlinkObject(SCA_IObject *clientobj);

  /**
   * Update(...)
   * Update the actuator based upon the events received since
   * the last call to Update, the current time and deltatime the
   * time elapsed in this frame ?
   * It is the responsibility of concrete Actuators to clear
   * their event's. This is usually done in the Update() method via
   * a call to RemoveAllEvents()
   */

  virtual bool Update(double curtime);
  virtual bool Update();

  /**
   * Add an event to an actuator.
   */
  void AddEvent(bool event);

  virtual void ProcessReplica();

  /**
   * Return true if all the current events
   * are negative. The definition of negative event is
   * not immediately clear. But usually refers to key-up events
   * or events where no action is required.
   */
  bool IsNegativeEvent() const;
  bool IsPositiveEvent() const;

  /**
   * Remove this actuator from the list of active actuators.
   * This function is only used to deactivate actuators outside the logic loop
   * e.g. when an object is deleted.
   */
  virtual void Deactivate();
  virtual void Activate(SG_DList &head);

  void LinkToController(SCA_IController *controller);
  void UnlinkController(SCA_IController *cont);
  void UnlinkAllControllers();

  void ClrLink();
  void IncLink();
  virtual void DecLink();
  bool IsNoLink() const;
  bool IsType(KX_ACTUATOR_TYPE type);
};
