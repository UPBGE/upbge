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

/** \file SCA_KeyboardSensor.h
 *  \ingroup gamelogic
 *  \brief Sensor for keyboard input
 */

#pragma once

#include <list>

#include "EXP_BoolValue.h"
#include "SCA_ISensor.h"

/**
 * The keyboard sensor listens to the keyboard, and passes on events
 * on selected keystrokes. It has an alternate mode in which it logs
 * keypresses to a property. Note that these modes are not mutually
 * exclusive.  */
class SCA_KeyboardSensor : public SCA_ISensor {
  Py_Header

      /**
       * the key this sensor is sensing for
       */
      int m_hotkey;
  short int m_qual, m_qual2;
  short int m_val;
  bool m_status[3];
  /**
   * If this toggle is true, all incoming key events generate a
   * response.
   */
  bool m_bAllKeys;

  /**
   * The name of the property to which logged text is appended. If
   * this property is not defined, no logging takes place.
   */
  std::string m_targetprop;
  /**
   * The property that indicates whether or not to log text when in
   * logging mode. If the property equals 0, no logging is done. For
   * all other values, logging is active. Logging can only become
   * active if there is a property to log to. Logging is independent
   * from hotkey settings. */
  std::string m_toggleprop;

  /**
   * Log the keystrokes from the current input buffer.
   */
  void LogKeystrokes();

 public:
  SCA_KeyboardSensor(class SCA_KeyboardManager *keybdmgr,
                     short int hotkey,
                     short int qual,
                     short int qual2,
                     bool bAllKeys,
                     const std::string &targetProp,
                     const std::string &toggleProp,
                     SCA_IObject *gameobj,
                     short int exitKey);
  virtual ~SCA_KeyboardSensor();
  virtual EXP_Value *GetReplica();
  virtual void Init();

  virtual bool Evaluate();
  virtual bool IsPositiveTrigger();

#ifdef WITH_PYTHON
  /* --------------------------------------------------------------------- */
  /* Python interface ---------------------------------------------------- */
  /* --------------------------------------------------------------------- */

  // KeyEvents:
  EXP_PYMETHOD_DOC_NOARGS(SCA_KeyboardSensor, getEventList);
  // KeyStatus:
  EXP_PYMETHOD_DOC_O(SCA_KeyboardSensor, getKeyStatus);

  static PyObject *pyattr_get_events(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_inputs(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
#endif
};
