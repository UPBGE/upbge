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
 * Contributor(s): Mitchell Stokes.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file SCA_PythonJoystick.h
 *  \ingroup gamelogic
 */

#pragma once

#include "EXP_Value.h"

class SCA_PythonJoystick : public EXP_Value {
  Py_Header private : class DEV_Joystick *m_joystick;
  int m_joyindex;
  float m_strengthLeft;
  float m_strengthRight;
  int m_duration;
  /** Response applied by leftStick, rightStick and triggers (see DEV_JoystickResponse.h). */
  float m_stickDeadzone;
  float m_stickOuter;
  float m_stickExponent;
  float m_triggerDeadzone;

 public:
  SCA_PythonJoystick(class DEV_Joystick *joystick, int joyindex);
  virtual ~SCA_PythonJoystick();

  virtual std::string GetName();

#ifdef WITH_PYTHON
  static PyObject *pyattr_get_num_x(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_active_buttons(EXP_PyObjectPlus *self_v,
                                             const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_hat_values(EXP_PyObjectPlus *self_v,
                                         const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_axis_values(EXP_PyObjectPlus *self_v,
                                          const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_name(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);

  EXP_PYMETHOD_DOC_NOARGS(SCA_PythonJoystick, startVibration);
  EXP_PYMETHOD_DOC_NOARGS(SCA_PythonJoystick, stopVibration);
  EXP_PYMETHOD_DOC(SCA_PythonJoystick, enableSensors);
  EXP_PYMETHOD_DOC_VARARGS(SCA_PythonJoystick, setLED);
  EXP_PYMETHOD_DOC_VARARGS(SCA_PythonJoystick, startTriggerVibration);
  EXP_PYMETHOD_DOC_NOARGS(SCA_PythonJoystick, stopTriggerVibration);
  EXP_PYMETHOD_DOC_O(SCA_PythonJoystick, sendEffect);

  static PyObject *pyattr_get_isVibrating(EXP_PyObjectPlus *self_v,
                                          const struct EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_hasVibration(EXP_PyObjectPlus *self_v,
                                           const struct EXP_PYATTRIBUTE_DEF *attrdef);

  static PyObject *pyattr_get_left_stick(EXP_PyObjectPlus *self_v,
                                         const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_right_stick(EXP_PyObjectPlus *self_v,
                                          const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_triggers(EXP_PyObjectPlus *self_v,
                                       const EXP_PYATTRIBUTE_DEF *attrdef);
  static int CheckResponse(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);

  static PyObject *pyattr_get_is_gamepad(EXP_PyObjectPlus *self_v,
                                         const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_type(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_button_labels(EXP_PyObjectPlus *self_v,
                                            const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_guid(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_vendor_id(EXP_PyObjectPlus *self_v,
                                        const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_product_id(EXP_PyObjectPlus *self_v,
                                         const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_connection(EXP_PyObjectPlus *self_v,
                                         const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_player_index(EXP_PyObjectPlus *self_v,
                                           const EXP_PYATTRIBUTE_DEF *attrdef);
  static int pyattr_set_player_index(EXP_PyObjectPlus *self_v,
                                     const EXP_PYATTRIBUTE_DEF *attrdef,
                                     PyObject *value);
  static PyObject *pyattr_get_has_gyro(EXP_PyObjectPlus *self_v,
                                       const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_has_accel(EXP_PyObjectPlus *self_v,
                                        const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_gyro(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_gyro_delta(EXP_PyObjectPlus *self_v,
                                         const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_accel(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_touchpads(EXP_PyObjectPlus *self_v,
                                        const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_battery(EXP_PyObjectPlus *self_v,
                                      const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_has_led(EXP_PyObjectPlus *self_v,
                                      const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_has_trigger_vibration(EXP_PyObjectPlus *self_v,
                                                    const EXP_PYATTRIBUTE_DEF *attrdef);
#endif
};
