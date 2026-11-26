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

/** \file gameengine/GameLogic/SCA_PythonInputDevice.cpp
 *  \ingroup gamelogic
 */

#include "SCA_PythonInputDevice.h"

#include "DEV_RawJoystick.h"

/* ------------------------------------------------------------------------- */
/* Native functions                                                          */
/* ------------------------------------------------------------------------- */

SCA_PythonInputDevice::SCA_PythonInputDevice(DEV_RawJoystick *device) : m_device(device)
{
}

SCA_PythonInputDevice::~SCA_PythonInputDevice()
{
}

std::string SCA_PythonInputDevice::GetName()
{
  return m_device->GetName();
}

#ifdef WITH_PYTHON

/* ------------------------------------------------------------------------- */
/* Python functions                                                          */
/* ------------------------------------------------------------------------- */

/* Integration hooks ------------------------------------------------------- */
PyTypeObject SCA_PythonInputDevice::Type = {
    PyVarObject_HEAD_INIT(nullptr, 0) "SCA_PythonInputDevice",
    sizeof(EXP_PyObjectPlus_Proxy),
    0,
    py_base_dealloc,
    0,
    0,
    0,
    0,
    py_base_repr,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    Methods,
    0,
    0,
    &EXP_PyObjectPlus::Type,
    0,
    0,
    0,
    0,
    0,
    0,
    py_base_new};

PyMethodDef SCA_PythonInputDevice::Methods[] = {
    EXP_PYMETHODTABLE_O(SCA_PythonInputDevice, getAxis),
    EXP_PYMETHODTABLE_O(SCA_PythonInputDevice, getButton),
    EXP_PYMETHODTABLE_O(SCA_PythonInputDevice, getHat),
    {nullptr, nullptr}  // Sentinel
};

PyAttributeDef SCA_PythonInputDevice::Attributes[] = {
    EXP_PYATTRIBUTE_RO_FUNCTION("name", SCA_PythonInputDevice, pyattr_get_name),
    EXP_PYATTRIBUTE_RO_FUNCTION("guid", SCA_PythonInputDevice, pyattr_get_guid),
    EXP_PYATTRIBUTE_RO_FUNCTION("vendorId", SCA_PythonInputDevice, pyattr_get_vendor_id),
    EXP_PYATTRIBUTE_RO_FUNCTION("productId", SCA_PythonInputDevice, pyattr_get_product_id),
    EXP_PYATTRIBUTE_RO_FUNCTION("deviceType", SCA_PythonInputDevice, pyattr_get_device_type),
    EXP_PYATTRIBUTE_RO_FUNCTION("isGamepad", SCA_PythonInputDevice, pyattr_get_is_gamepad),
    EXP_PYATTRIBUTE_RO_FUNCTION("index", SCA_PythonInputDevice, pyattr_get_index),
    EXP_PYATTRIBUTE_RO_FUNCTION("numAxes", SCA_PythonInputDevice, pyattr_get_num_axes),
    EXP_PYATTRIBUTE_RO_FUNCTION("numButtons", SCA_PythonInputDevice, pyattr_get_num_buttons),
    EXP_PYATTRIBUTE_RO_FUNCTION("numHats", SCA_PythonInputDevice, pyattr_get_num_hats),
    EXP_PYATTRIBUTE_RO_FUNCTION("axisValues", SCA_PythonInputDevice, pyattr_get_axis_values),
    EXP_PYATTRIBUTE_RO_FUNCTION("buttonValues", SCA_PythonInputDevice, pyattr_get_button_values),
    EXP_PYATTRIBUTE_RO_FUNCTION("activeButtons",
                                SCA_PythonInputDevice,
                                pyattr_get_active_buttons),
    EXP_PYATTRIBUTE_RO_FUNCTION("hatValues", SCA_PythonInputDevice, pyattr_get_hat_values),
    EXP_PYATTRIBUTE_NULL  // Sentinel
};

/**
 * Convert \a value to an input index in the range [0, \a count).
 * \return False with a Python exception set if \a value is not a valid index.
 */
static bool input_index_from_py(PyObject *value, int count, const char *error_prefix, int *r_index)
{
  const long index = PyLong_AsLong(value);
  if (index == -1 && PyErr_Occurred()) {
    PyErr_Format(PyExc_TypeError, "%s: expected an int", error_prefix);
    return false;
  }

  if (index < 0 || index >= count) {
    PyErr_Format(
        PyExc_IndexError, "%s: index %ld out of range [0, %d)", error_prefix, index, count);
    return false;
  }

  *r_index = int(index);
  return true;
}

PyObject *SCA_PythonInputDevice::pyattr_get_name(EXP_PyObjectPlus *self_v,
                                                 const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_PythonInputDevice *self = static_cast<SCA_PythonInputDevice *>(self_v);
  return PyUnicode_FromStdString(self->m_device->GetName());
}

PyObject *SCA_PythonInputDevice::pyattr_get_guid(EXP_PyObjectPlus *self_v,
                                                 const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_PythonInputDevice *self = static_cast<SCA_PythonInputDevice *>(self_v);
  return PyUnicode_FromStdString(self->m_device->GetGUID());
}

PyObject *SCA_PythonInputDevice::pyattr_get_vendor_id(EXP_PyObjectPlus *self_v,
                                                      const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_PythonInputDevice *self = static_cast<SCA_PythonInputDevice *>(self_v);
  return PyLong_FromLong(self->m_device->GetVendorId());
}

PyObject *SCA_PythonInputDevice::pyattr_get_product_id(EXP_PyObjectPlus *self_v,
                                                       const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_PythonInputDevice *self = static_cast<SCA_PythonInputDevice *>(self_v);
  return PyLong_FromLong(self->m_device->GetProductId());
}

PyObject *SCA_PythonInputDevice::pyattr_get_device_type(EXP_PyObjectPlus *self_v,
                                                        const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_PythonInputDevice *self = static_cast<SCA_PythonInputDevice *>(self_v);

  const char *type = "unknown";
  switch (self->m_device->GetType()) {
    case DEV_RawJoystick::Type::Unknown:
      break;
    case DEV_RawJoystick::Type::Gamepad:
      type = "gamepad";
      break;
    case DEV_RawJoystick::Type::Wheel:
      type = "wheel";
      break;
    case DEV_RawJoystick::Type::ArcadeStick:
      type = "arcade_stick";
      break;
    case DEV_RawJoystick::Type::FlightStick:
      type = "flight_stick";
      break;
    case DEV_RawJoystick::Type::DancePad:
      type = "dance_pad";
      break;
    case DEV_RawJoystick::Type::Guitar:
      type = "guitar";
      break;
    case DEV_RawJoystick::Type::DrumKit:
      type = "drum_kit";
      break;
    case DEV_RawJoystick::Type::ArcadePad:
      type = "arcade_pad";
      break;
    case DEV_RawJoystick::Type::Throttle:
      type = "throttle";
      break;
  }

  return PyUnicode_FromString(type);
}

PyObject *SCA_PythonInputDevice::pyattr_get_is_gamepad(EXP_PyObjectPlus *self_v,
                                                       const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_PythonInputDevice *self = static_cast<SCA_PythonInputDevice *>(self_v);
  return PyBool_FromLong(self->m_device->IsGamepad());
}

PyObject *SCA_PythonInputDevice::pyattr_get_index(EXP_PyObjectPlus *self_v,
                                                  const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_PythonInputDevice *self = static_cast<SCA_PythonInputDevice *>(self_v);
  return PyLong_FromLong(self->m_device->GetSlot());
}

PyObject *SCA_PythonInputDevice::pyattr_get_num_axes(EXP_PyObjectPlus *self_v,
                                                     const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_PythonInputDevice *self = static_cast<SCA_PythonInputDevice *>(self_v);
  return PyLong_FromLong(self->m_device->GetNumAxes());
}

PyObject *SCA_PythonInputDevice::pyattr_get_num_buttons(EXP_PyObjectPlus *self_v,
                                                        const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_PythonInputDevice *self = static_cast<SCA_PythonInputDevice *>(self_v);
  return PyLong_FromLong(self->m_device->GetNumButtons());
}

PyObject *SCA_PythonInputDevice::pyattr_get_num_hats(EXP_PyObjectPlus *self_v,
                                                     const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_PythonInputDevice *self = static_cast<SCA_PythonInputDevice *>(self_v);
  return PyLong_FromLong(self->m_device->GetNumHats());
}

PyObject *SCA_PythonInputDevice::pyattr_get_axis_values(EXP_PyObjectPlus *self_v,
                                                        const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_PythonInputDevice *self = static_cast<SCA_PythonInputDevice *>(self_v);

  const int num_axes = self->m_device->GetNumAxes();
  PyObject *list = PyList_New(num_axes);
  for (int i = 0; i < num_axes; i++) {
    PyList_SET_ITEM(list, i, PyFloat_FromDouble(self->m_device->GetAxis(i)));
  }

  return list;
}

PyObject *SCA_PythonInputDevice::pyattr_get_button_values(EXP_PyObjectPlus *self_v,
                                                          const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_PythonInputDevice *self = static_cast<SCA_PythonInputDevice *>(self_v);

  const int num_buttons = self->m_device->GetNumButtons();
  PyObject *list = PyList_New(num_buttons);
  for (int i = 0; i < num_buttons; i++) {
    PyList_SET_ITEM(list, i, PyBool_FromLong(self->m_device->GetButton(i)));
  }

  return list;
}

PyObject *SCA_PythonInputDevice::pyattr_get_active_buttons(EXP_PyObjectPlus *self_v,
                                                           const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_PythonInputDevice *self = static_cast<SCA_PythonInputDevice *>(self_v);

  const int num_buttons = self->m_device->GetNumButtons();
  PyObject *list = PyList_New(0);
  for (int i = 0; i < num_buttons; i++) {
    if (self->m_device->GetButton(i)) {
      PyObject *value = PyLong_FromLong(i);
      PyList_Append(list, value);
      Py_DECREF(value);
    }
  }

  return list;
}

PyObject *SCA_PythonInputDevice::pyattr_get_hat_values(EXP_PyObjectPlus *self_v,
                                                       const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_PythonInputDevice *self = static_cast<SCA_PythonInputDevice *>(self_v);

  const int num_hats = self->m_device->GetNumHats();
  PyObject *list = PyList_New(num_hats);
  for (int i = 0; i < num_hats; i++) {
    PyList_SET_ITEM(list, i, PyLong_FromLong(self->m_device->GetHat(i)));
  }

  return list;
}

EXP_PYMETHODDEF_DOC_O(SCA_PythonInputDevice,
                      getAxis,
                      "getAxis(index)\n"
                      "\tReturn the position of an axis between -1.0 and 1.0.\n")
{
  int index;
  if (!input_index_from_py(value, m_device->GetNumAxes(), "getAxis(index)", &index)) {
    return nullptr;
  }

  return PyFloat_FromDouble(m_device->GetAxis(index));
}

EXP_PYMETHODDEF_DOC_O(SCA_PythonInputDevice,
                      getButton,
                      "getButton(index)\n"
                      "\tReturn True if the button is pressed.\n")
{
  int index;
  if (!input_index_from_py(value, m_device->GetNumButtons(), "getButton(index)", &index)) {
    return nullptr;
  }

  return PyBool_FromLong(m_device->GetButton(index));
}

EXP_PYMETHODDEF_DOC_O(SCA_PythonInputDevice,
                      getHat,
                      "getHat(index)\n"
                      "\tReturn the position of a hat as a bitmask of 1 (up), 2 (right), "
                      "4 (down) and 8 (left), 0 when centered.\n")
{
  int index;
  if (!input_index_from_py(value, m_device->GetNumHats(), "getHat(index)", &index)) {
    return nullptr;
  }

  return PyLong_FromLong(m_device->GetHat(index));
}

#endif  // WITH_PYTHON
