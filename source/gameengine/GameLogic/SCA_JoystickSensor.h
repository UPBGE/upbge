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

/** \file SCA_JoystickSensor.h
 *  \ingroup gamelogic
 */

#pragma once

#include "DEV_JoystickDefines.h"
#include "SCA_ISensor.h"

class SCA_JoystickSensor : public SCA_ISensor {
  Py_Header

      /**
       * Axis 1-JOYAXIS_MAX, MUST be followed by m_axisf
       */
      int m_axis;
  /**
   * Axis flag to find direction, MUST be an int
   */
  int m_axisf;
  /**
   * The actual button
   */
  int m_button;
  /**
   * Flag for a pressed or released button
   */
  int m_buttonf;
  /**
   * The threshold value the axis acts upon
   */
  int m_precision;
  /**
   * Is an event triggered ?
   */
  bool m_istrig;
  /**
   * Last trigger state for this sensors joystick,
   * Otherwise it will trigger all the time
   * this is used to see if the trigger state changes.
   */
  bool m_istrig_prev;
  /**
   * The mode to determine axis,button or hat
   */
  short int m_joymode;
  /**
   * Select which joystick to use
   */
  short int m_joyindex;

  /**
   * Detect all events for the currently selected type
   */
  bool m_bAllEvents;

 public:
  enum KX_JOYSENSORMODE {
    KX_JOYSENSORMODE_NODEF = 0,
    KX_JOYSENSORMODE_BUTTON,
    KX_JOYSENSORMODE_AXIS,
    KX_JOYSENSORMODE_HAT,  // unused
    KX_JOYSENSORMODE_AXIS_SINGLE,
    KX_JOYSENSORMODE_SHOULDER_TRIGGER,
    KX_JOYSENSORMODE_MAX
  };

  enum KX_JOYSENS_BUTTON {
    KX_JOYSENS_BUTTON_NODEF = -1,
    KX_JOYSENS_BUTTON_A,
    KX_JOYSENS_BUTTON_B,
    KX_JOYSENS_BUTTON_X,
    KX_JOYSENS_BUTTON_Y,
    KX_JOYSENS_BUTTON_BACK,
    KX_JOYSENS_BUTTON_GUIDE,
    KX_JOYSENS_BUTTON_START,
    KX_JOYSENS_BUTTON_STICK_LEFT,
    KX_JOYSENS_BUTTON_STICK_RIGHT,
    KX_JOYSENS_BUTTON_SHOULDER_LEFT,
    KX_JOYSENS_BUTTON_SHOULDER_RIGHT,
    KX_JOYSENS_BUTTON_DPAD_UP,
    KX_JOYSENS_BUTTON_DPAD_DOWN,
    KX_JOYSENS_BUTTON_DPAD_LEFT,
    KX_JOYSENS_BUTTON_DPAD_RIGHT,
    KX_JOYSENS_BUTTON_MAX
  };

  enum KX_JOYSENS_AXIS_SINGLE {
    KX_JOYSENS_AXIS_SINGLE_NODEF = 0,
    KX_JOYSENS_AXIS_SINGLE_LEFT_STICK_HORIZONTAL,
    KX_JOYSENS_AXIS_SINGLE_LEFT_STICK_VERTICAL,
    KX_JOYSENS_AXIS_SINGLE_RIGHT_STICK_HORIZONTAL,
    KX_JOYSENS_AXIS_SINGLE_RIGHT_STICK_VERTICAL,
    KX_JOYSENS_AXIS_SINGLE_LEFT_SHOULDER_TRIGGER,
    KX_JOYSENS_AXIS_SINGLE_RIGHT_SHOULDER_TRIGGER,
    KX_JOYSENS_AXIS_SINGLE_MAX
  };

  enum KX_JOYSENS_AXIS {
    KX_JOYSENS_AXIS_NODEF = 0,
    KX_JOYSENS_AXIS_LEFT_STICK,
    KX_JOYSENS_AXIS_RIGHT_STICK,
    KX_JOYSENS_AXIS_SHOULDER_TRIGGER,
    KX_JOYSENS_AXIS_MAX
  };

  enum KX_JOYSENS_AXIS_STICK_DIRECTION {
    KX_JOYSENS_AXIS_STICK_DIRECTION_NODEF = -1,
    KX_JOYSENS_AXIS_STICK_DIRECTION_RIGHT,
    KX_JOYSENS_AXIS_STICK_DIRECTION_UP,
    KX_JOYSENS_AXIS_STICK_DIRECTION_LEFT,
    KX_JOYSENS_AXIS_STICK_DIRECTION_DOWN,
    KX_JOYSENS_AXIS_STICK_DIRECTION_MAX
  };

  SCA_JoystickSensor(class SCA_JoystickManager *eventmgr,
                     SCA_IObject *gameobj,
                     short int joyindex,
                     short int joymode,
                     int axis,
                     int axisf,
                     int prec,
                     int button,
                     bool allevents);
  virtual ~SCA_JoystickSensor();
  virtual EXP_Value *GetReplica();

  virtual bool Evaluate();
  virtual bool IsPositiveTrigger();
  virtual void Init();

  short int GetJoyIndex(void)
  {
    return m_joyindex;
  }

#ifdef WITH_PYTHON

  /* --------------------------------------------------------------------- */
  /* Python interface ---------------------------------------------------- */
  /* --------------------------------------------------------------------- */

  /* Joystick Index */
  EXP_PYMETHOD_DOC_NOARGS(SCA_JoystickSensor, GetButtonActiveList)
  EXP_PYMETHOD_DOC_VARARGS(SCA_JoystickSensor, GetButtonStatus)

  static PyObject *pyattr_get_axis_values(EXP_PyObjectPlus *self_v,
                                          const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_axis_single(EXP_PyObjectPlus *self_v,
                                          const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_check_hat(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_hat_values(EXP_PyObjectPlus *self_v,
                                         const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_hat_single(EXP_PyObjectPlus *self_v,
                                         const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_num_axis(EXP_PyObjectPlus *self_v,
                                       const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_num_buttons(EXP_PyObjectPlus *self_v,
                                          const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_num_hats(EXP_PyObjectPlus *self_v,
                                       const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_connected(EXP_PyObjectPlus *self_v,
                                        const EXP_PYATTRIBUTE_DEF *attrdef);

  /* attribute check */
  static int CheckAxis(EXP_PyObjectPlus *self, const PyAttributeDef *)
  {
    SCA_JoystickSensor *sensor = reinterpret_cast<SCA_JoystickSensor *>(self);
    if (sensor->m_axis < 1)
      sensor->m_axis = 1;
    else if (sensor->m_axis > JOYAXIS_MAX)
      sensor->m_axis = JOYAXIS_MAX;
    return 0;
  }

#endif /* WITH_PYTHON */
};
