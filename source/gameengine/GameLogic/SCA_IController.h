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

/** \file SCA_IController.h
 *  \ingroup gamelogic
 */

#pragma once

#include "SCA_ILogicBrick.h"

/**
 * Use of SG_DList element: none
 * Use of SG_QList element: build ordered list of activated controller on the owner object
 *                          Head: SCA_IObject::m_activeControllers
 */
class SCA_IController : public SCA_ILogicBrick {
  Py_Header

      protected : std::vector<SCA_ISensor *>
                      m_linkedsensors;
  std::vector<SCA_IActuator *> m_linkedactuators;
  unsigned int m_statemask;
  bool m_justActivated;
  bool m_bookmark;

 public:
  SCA_IController(SCA_IObject *gameobj);
  virtual ~SCA_IController();

  virtual void Trigger(SCA_LogicManager *logicmgr) = 0;

  void LinkToSensor(SCA_ISensor *sensor);
  void LinkToActuator(SCA_IActuator *);
  std::vector<SCA_ISensor *> &GetLinkedSensors();
  std::vector<SCA_IActuator *> &GetLinkedActuators();
  void UnlinkAllSensors();
  void UnlinkAllActuators();
  void UnlinkActuator(SCA_IActuator *actua);
  void UnlinkSensor(SCA_ISensor *sensor);
  void SetState(unsigned int state);
  void ApplyState(unsigned int state);
  void Deactivate();
  bool IsJustActivated();
  void ClrJustActivated();
  void SetBookmark(bool bookmark);
  void Activate(SG_DList &head);

#ifdef WITH_PYTHON
  static PyObject *pyattr_get_state(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_sensors(EXP_PyObjectPlus *self_v,
                                      const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_actuators(EXP_PyObjectPlus *self_v,
                                        const EXP_PYATTRIBUTE_DEF *attrdef);
#endif  // WITH_PYTHON
};
