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
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file SCA_PythonInputDevice.h
 *  \ingroup gamelogic
 */

#pragma once

#include "EXP_Value.h"

/**
 * Python wrapper of a #DEV_RawJoystick, stored in `bge.logic.inputDevices`.
 *
 * Exposes the raw axes, buttons and hats of any SDL joystick-class device (flight sticks,
 * throttles, wheels, pedals, gamepads...). Instances are created and deleted by
 * #updatePythonInputDevices when devices are connected and disconnected.
 */
class SCA_PythonInputDevice : public EXP_Value {
  Py_Header private : class DEV_RawJoystick *m_device;

 public:
  SCA_PythonInputDevice(class DEV_RawJoystick *device);
  virtual ~SCA_PythonInputDevice();

  virtual std::string GetName();

#ifdef WITH_PYTHON
  static PyObject *pyattr_get_name(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_guid(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_vendor_id(EXP_PyObjectPlus *self_v,
                                        const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_product_id(EXP_PyObjectPlus *self_v,
                                         const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_device_type(EXP_PyObjectPlus *self_v,
                                          const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_is_gamepad(EXP_PyObjectPlus *self_v,
                                         const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_index(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_num_axes(EXP_PyObjectPlus *self_v,
                                       const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_num_buttons(EXP_PyObjectPlus *self_v,
                                          const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_num_hats(EXP_PyObjectPlus *self_v,
                                       const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_axis_values(EXP_PyObjectPlus *self_v,
                                          const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_button_values(EXP_PyObjectPlus *self_v,
                                            const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_active_buttons(EXP_PyObjectPlus *self_v,
                                             const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_hat_values(EXP_PyObjectPlus *self_v,
                                         const EXP_PYATTRIBUTE_DEF *attrdef);

  EXP_PYMETHOD_DOC_O(SCA_PythonInputDevice, getAxis);
  EXP_PYMETHOD_DOC_O(SCA_PythonInputDevice, getButton);
  EXP_PYMETHOD_DOC_O(SCA_PythonInputDevice, getHat);
#endif
};
