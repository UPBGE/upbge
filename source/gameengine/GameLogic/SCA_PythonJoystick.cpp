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

/** \file gameengine/GameLogic/SCA_PythonJoystick.cpp
 *  \ingroup gamelogic
 */

#include "SCA_PythonJoystick.h"

#include "DEV_Joystick.h"
#include "DEV_JoystickResponse.h"

using namespace blender;

//#include "GHOST_C-api.h"

/* ------------------------------------------------------------------------- */
/* Native functions                                                          */
/* ------------------------------------------------------------------------- */

SCA_PythonJoystick::SCA_PythonJoystick(DEV_Joystick *joystick, int joyindex)
    : m_joystick(joystick),
      m_joyindex(joyindex),
      m_strengthLeft(0.0f),
      m_strengthRight(0.0f),
      m_duration(0),
      m_stickDeadzone(DEV_STICK_DEADZONE_DEFAULT),
      m_stickOuter(DEV_STICK_OUTER_DEFAULT),
      m_stickExponent(DEV_STICK_EXPONENT_DEFAULT),
      m_triggerDeadzone(DEV_TRIGGER_DEADZONE_DEFAULT)
{
}

SCA_PythonJoystick::~SCA_PythonJoystick()
{
}

std::string SCA_PythonJoystick::GetName()
{
  return m_joystick->GetName();
}

#ifdef WITH_PYTHON

/* ------------------------------------------------------------------------- */
/* Python functions                                                          */
/* ------------------------------------------------------------------------- */

/* Integration hooks ------------------------------------------------------- */
PyTypeObject SCA_PythonJoystick::Type = {PyVarObject_HEAD_INIT(nullptr, 0) "SCA_PythonJoystick",
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

PyMethodDef SCA_PythonJoystick::Methods[] = {
    EXP_PYMETHODTABLE_NOARGS(SCA_PythonJoystick, startVibration),
    EXP_PYMETHODTABLE_NOARGS(SCA_PythonJoystick, stopVibration),
    EXP_PYMETHODTABLE_KEYWORDS(SCA_PythonJoystick, enableSensors),
    EXP_PYMETHODTABLE(SCA_PythonJoystick, setLED),
    EXP_PYMETHODTABLE(SCA_PythonJoystick, startTriggerVibration),
    EXP_PYMETHODTABLE_NOARGS(SCA_PythonJoystick, stopTriggerVibration),
    EXP_PYMETHODTABLE_O(SCA_PythonJoystick, sendEffect),
    {nullptr, nullptr}  // Sentinel
};

PyAttributeDef SCA_PythonJoystick::Attributes[] = {
    EXP_PYATTRIBUTE_RO_FUNCTION("numButtons", SCA_PythonJoystick, pyattr_get_num_x),
    EXP_PYATTRIBUTE_RO_FUNCTION("numHats", SCA_PythonJoystick, pyattr_get_num_x),
    EXP_PYATTRIBUTE_RO_FUNCTION("numAxis", SCA_PythonJoystick, pyattr_get_num_x),
    EXP_PYATTRIBUTE_RO_FUNCTION("activeButtons", SCA_PythonJoystick, pyattr_get_active_buttons),
    EXP_PYATTRIBUTE_RO_FUNCTION("hatValues", SCA_PythonJoystick, pyattr_get_hat_values),
    EXP_PYATTRIBUTE_RO_FUNCTION("axisValues", SCA_PythonJoystick, pyattr_get_axis_values),
    EXP_PYATTRIBUTE_RO_FUNCTION("name", SCA_PythonJoystick, pyattr_get_name),
    EXP_PYATTRIBUTE_INT_RW("duration", 0, INT_MAX, true, SCA_PythonJoystick, m_duration),
    EXP_PYATTRIBUTE_FLOAT_RW("strengthLeft", 0.0, 1.0, SCA_PythonJoystick, m_strengthLeft),
    EXP_PYATTRIBUTE_FLOAT_RW("strengthRight", 0.0, 1.0, SCA_PythonJoystick, m_strengthRight),
    EXP_PYATTRIBUTE_RO_FUNCTION("isVibrating", SCA_PythonJoystick, pyattr_get_isVibrating),
    EXP_PYATTRIBUTE_RO_FUNCTION("hasVibration", SCA_PythonJoystick, pyattr_get_hasVibration),
    EXP_PYATTRIBUTE_FLOAT_RW_CHECK(
        "stickDeadzone", 0.0f, 1.0f, SCA_PythonJoystick, m_stickDeadzone, CheckResponse),
    EXP_PYATTRIBUTE_FLOAT_RW_CHECK(
        "stickOuter", 0.0f, 1.0f, SCA_PythonJoystick, m_stickOuter, CheckResponse),
    EXP_PYATTRIBUTE_FLOAT_RW_CHECK(
        "stickExponent", 0.0f, 10.0f, SCA_PythonJoystick, m_stickExponent, CheckResponse),
    EXP_PYATTRIBUTE_FLOAT_RW_CHECK(
        "triggerDeadzone", 0.0f, 1.0f, SCA_PythonJoystick, m_triggerDeadzone, CheckResponse),
    EXP_PYATTRIBUTE_RO_FUNCTION("leftStick", SCA_PythonJoystick, pyattr_get_left_stick),
    EXP_PYATTRIBUTE_RO_FUNCTION("rightStick", SCA_PythonJoystick, pyattr_get_right_stick),
    EXP_PYATTRIBUTE_RO_FUNCTION("triggers", SCA_PythonJoystick, pyattr_get_triggers),
    EXP_PYATTRIBUTE_RO_FUNCTION("type", SCA_PythonJoystick, pyattr_get_type),
    EXP_PYATTRIBUTE_RO_FUNCTION("buttonLabels", SCA_PythonJoystick, pyattr_get_button_labels),
    EXP_PYATTRIBUTE_RO_FUNCTION("guid", SCA_PythonJoystick, pyattr_get_guid),
    EXP_PYATTRIBUTE_RO_FUNCTION("connection", SCA_PythonJoystick, pyattr_get_connection),
    EXP_PYATTRIBUTE_RW_FUNCTION(
        "playerIndex", SCA_PythonJoystick, pyattr_get_player_index, pyattr_set_player_index),
    EXP_PYATTRIBUTE_RO_FUNCTION("hasGyro", SCA_PythonJoystick, pyattr_get_has_gyro),
    EXP_PYATTRIBUTE_RO_FUNCTION("hasAccel", SCA_PythonJoystick, pyattr_get_has_accel),
    EXP_PYATTRIBUTE_RO_FUNCTION("gyro", SCA_PythonJoystick, pyattr_get_gyro),
    EXP_PYATTRIBUTE_RO_FUNCTION("gyroDelta", SCA_PythonJoystick, pyattr_get_gyro_delta),
    EXP_PYATTRIBUTE_RO_FUNCTION("accel", SCA_PythonJoystick, pyattr_get_accel),
    EXP_PYATTRIBUTE_RO_FUNCTION("touchpads", SCA_PythonJoystick, pyattr_get_touchpads),
    EXP_PYATTRIBUTE_RO_FUNCTION("battery", SCA_PythonJoystick, pyattr_get_battery),
    EXP_PYATTRIBUTE_RO_FUNCTION("hasLED", SCA_PythonJoystick, pyattr_get_has_led),
    EXP_PYATTRIBUTE_RO_FUNCTION(
        "hasTriggerVibration", SCA_PythonJoystick, pyattr_get_has_trigger_vibration),
    EXP_PYATTRIBUTE_NULL  // Sentinel
};

// Use one function for numAxis, numButtons, and numHats
PyObject *SCA_PythonJoystick::pyattr_get_num_x(EXP_PyObjectPlus *self_v,
                                               const EXP_PYATTRIBUTE_DEF *attrdef)
{
  if (attrdef->m_name == "numButtons") {
    return PyLong_FromLong(JOYBUT_MAX);
  }
  else if (attrdef->m_name == "numAxis") {
    return PyLong_FromLong(JOYAXIS_MAX);
  }
  else if (attrdef->m_name == "numHats") {
    EXP_ShowDeprecationWarning("SCA_PythonJoystick.numHats", "SCA_PythonJoystick.numButtons");
    return PyLong_FromLong(0);
  }

  // If we got here, we have a problem...
  PyErr_SetString(PyExc_AttributeError, "invalid attribute");
  return nullptr;
}

PyObject *SCA_PythonJoystick::pyattr_get_active_buttons(EXP_PyObjectPlus *self_v,
                                                        const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_PythonJoystick *self = static_cast<SCA_PythonJoystick *>(self_v);

  const int button_number = JOYBUT_MAX;

  PyObject *list = PyList_New(0);
  PyObject *value;

  for (int i = 0; i < button_number; i++) {
    if (self->m_joystick->aButtonPressIsPositive(i)) {
      value = PyLong_FromLong(i);
      PyList_Append(list, value);
      Py_DECREF(value);
    }
  }

  /* XXX return list adapted to new names (A, B, X, Y, START, etc) */
  return list;
}

PyObject *SCA_PythonJoystick::pyattr_get_hat_values(EXP_PyObjectPlus *self_v,
                                                    const EXP_PYATTRIBUTE_DEF *attrdef)
{
  EXP_ShowDeprecationWarning("SCA_PythonJoystick.hatValues", "SCA_PythonJoystick.activeButtons");
  return PyList_New(0);
}

PyObject *SCA_PythonJoystick::pyattr_get_axis_values(EXP_PyObjectPlus *self_v,
                                                     const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_PythonJoystick *self = static_cast<SCA_PythonJoystick *>(self_v);

  int axis_index = JOYAXIS_MAX;
  PyObject *list = PyList_New(axis_index);
  int position;

  while (axis_index--) {
    position = self->m_joystick->GetAxisPosition(axis_index);

    // We get back a range from -32768 to 32767, so we use an if here to
    // get a perfect -1.0 to 1.0 mapping. Some oddball system might have an
    // actual min of -32767 for shorts, so we use SHRT_MIN/MAX to be safe.
    if (position < 0) {
      PyList_SET_ITEM(list, axis_index, PyFloat_FromDouble(position / ((double)-SHRT_MIN)));
    }
    else {
      PyList_SET_ITEM(list, axis_index, PyFloat_FromDouble(position / (double)SHRT_MAX));
    }
  }

  return list;
}

PyObject *SCA_PythonJoystick::pyattr_get_name(EXP_PyObjectPlus *self_v,
                                              const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_PythonJoystick *self = static_cast<SCA_PythonJoystick *>(self_v);

  return PyUnicode_FromStdString(self->m_joystick->GetName());
}

EXP_PYMETHODDEF_DOC_NOARGS(SCA_PythonJoystick,
                           startVibration,
                           "startVibration()\n"
                           "\tStarts the joystick vibration.\n")
{
  if (m_joystick && m_joystick->GetRumbleSupport()) {
    m_joystick->RumblePlay(m_strengthLeft, m_strengthRight, m_duration);
  }

  Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC_NOARGS(SCA_PythonJoystick,
                           stopVibration,
                           "StopVibration()\n"
                           "\tStops the joystick vibration.\n")
{
  if (m_joystick && m_joystick->GetRumbleSupport()) {
    m_joystick->RumbleStop();
  }

  Py_RETURN_NONE;
}

PyObject *SCA_PythonJoystick::pyattr_get_isVibrating(EXP_PyObjectPlus *self_v,
                                                     const struct EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_PythonJoystick *self = static_cast<SCA_PythonJoystick *>(self_v);

  if (!(self->m_joystick) || !(self->m_joystick->GetRumbleSupport())) {
    return Py_False;
  }

  return PyBool_FromLong(self->m_joystick->GetRumbleStatus());
}

PyObject *SCA_PythonJoystick::pyattr_get_hasVibration(EXP_PyObjectPlus *self_v,
                                                      const struct EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_PythonJoystick *self = static_cast<SCA_PythonJoystick *>(self_v);

  if (!(self->m_joystick)) {
    return Py_False;
  }

  return PyBool_FromLong(self->m_joystick->GetRumbleSupport());
}

/* Axis indices follow SDL_GamepadAxis: LEFTX, LEFTY, RIGHTX, RIGHTY, LEFT_TRIGGER, RIGHT_TRIGGER.
 * The response is computed on read: nothing is spent per logic tick, and scripts that never read
 * these attributes pay nothing. */
static PyObject *stick_to_python(
    DEV_Joystick *joystick, int axis_x, float inner, float outer, float exponent)
{
  float v[2];
  DEV_RadialDeadzone(joystick->GetAxisPosition(axis_x),
                     joystick->GetAxisPosition(axis_x + 1),
                     inner,
                     outer,
                     exponent,
                     v);
  /* A tuple rather than a mathutils.Vector: no wrapper allocation per read. */
  PyObject *tuple = PyTuple_New(2);
  PyTuple_SET_ITEM(tuple, 0, PyFloat_FromDouble(v[0]));
  PyTuple_SET_ITEM(tuple, 1, PyFloat_FromDouble(v[1]));
  return tuple;
}

PyObject *SCA_PythonJoystick::pyattr_get_left_stick(EXP_PyObjectPlus *self_v,
                                                    const EXP_PYATTRIBUTE_DEF * /*attrdef*/)
{
  SCA_PythonJoystick *self = static_cast<SCA_PythonJoystick *>(self_v);
  return stick_to_python(
      self->m_joystick, 0, self->m_stickDeadzone, self->m_stickOuter, self->m_stickExponent);
}

PyObject *SCA_PythonJoystick::pyattr_get_right_stick(EXP_PyObjectPlus *self_v,
                                                     const EXP_PYATTRIBUTE_DEF * /*attrdef*/)
{
  SCA_PythonJoystick *self = static_cast<SCA_PythonJoystick *>(self_v);
  return stick_to_python(
      self->m_joystick, 2, self->m_stickDeadzone, self->m_stickOuter, self->m_stickExponent);
}

PyObject *SCA_PythonJoystick::pyattr_get_triggers(EXP_PyObjectPlus *self_v,
                                                  const EXP_PYATTRIBUTE_DEF * /*attrdef*/)
{
  SCA_PythonJoystick *self = static_cast<SCA_PythonJoystick *>(self_v);
  PyObject *tuple = PyTuple_New(2);
  PyTuple_SET_ITEM(tuple,
                   0,
                   PyFloat_FromDouble(DEV_TriggerResponse(self->m_joystick->GetAxisPosition(4),
                                                          self->m_triggerDeadzone)));
  PyTuple_SET_ITEM(tuple,
                   1,
                   PyFloat_FromDouble(DEV_TriggerResponse(self->m_joystick->GetAxisPosition(5),
                                                          self->m_triggerDeadzone)));
  return tuple;
}

int SCA_PythonJoystick::CheckResponse(EXP_PyObjectPlus *self_v,
                                      const EXP_PYATTRIBUTE_DEF * /*attrdef*/)
{
  const SCA_PythonJoystick *self = static_cast<SCA_PythonJoystick *>(self_v);
  /* The setter has already clamped the value to the declared range, but NaN passes clamping.
   * Every test below is a positive comparison, so NaN fails it and the old value is restored. */
  if (self->m_stickDeadzone >= 0.0f && self->m_stickDeadzone < self->m_stickOuter &&
      self->m_stickOuter <= 1.0f && self->m_stickExponent > 0.0f &&
      self->m_triggerDeadzone >= 0.0f && self->m_triggerDeadzone < 1.0f)
  {
    return 0;
  }
  PyErr_SetString(PyExc_ValueError,
                  "joystick response requires 0 <= stickDeadzone < stickOuter <= 1, "
                  "stickExponent > 0 and 0 <= triggerDeadzone < 1");
  return 1;
}

PyObject *SCA_PythonJoystick::pyattr_get_type(EXP_PyObjectPlus *self_v,
                                              const EXP_PYATTRIBUTE_DEF * /*attrdef*/)
{
  SCA_PythonJoystick *self = static_cast<SCA_PythonJoystick *>(self_v);
  return PyUnicode_FromString(self->m_joystick->GetTypeName());
}

PyObject *SCA_PythonJoystick::pyattr_get_button_labels(EXP_PyObjectPlus *self_v,
                                                       const EXP_PYATTRIBUTE_DEF * /*attrdef*/)
{
  SCA_PythonJoystick *self = static_cast<SCA_PythonJoystick *>(self_v);
  /* Buttons 0 to 3 (south, east, west, north): the indices activeButtons reports. */
  PyObject *tuple = PyTuple_New(4);
  for (int i = 0; i < 4; i++) {
    PyTuple_SET_ITEM(tuple, i, PyUnicode_FromString(self->m_joystick->GetButtonLabel(i)));
  }
  return tuple;
}

PyObject *SCA_PythonJoystick::pyattr_get_guid(EXP_PyObjectPlus *self_v,
                                              const EXP_PYATTRIBUTE_DEF * /*attrdef*/)
{
  SCA_PythonJoystick *self = static_cast<SCA_PythonJoystick *>(self_v);
  return PyUnicode_FromStdString(self->m_joystick->GetGUID());
}

PyObject *SCA_PythonJoystick::pyattr_get_connection(EXP_PyObjectPlus *self_v,
                                                    const EXP_PYATTRIBUTE_DEF * /*attrdef*/)
{
  SCA_PythonJoystick *self = static_cast<SCA_PythonJoystick *>(self_v);
  return PyUnicode_FromString(self->m_joystick->GetConnectionName());
}

PyObject *SCA_PythonJoystick::pyattr_get_player_index(EXP_PyObjectPlus *self_v,
                                                      const EXP_PYATTRIBUTE_DEF * /*attrdef*/)
{
  SCA_PythonJoystick *self = static_cast<SCA_PythonJoystick *>(self_v);
  return PyLong_FromLong(self->m_joystick->GetPlayerIndex());
}

int SCA_PythonJoystick::pyattr_set_player_index(EXP_PyObjectPlus *self_v,
                                                const EXP_PYATTRIBUTE_DEF * /*attrdef*/,
                                                PyObject *value)
{
  SCA_PythonJoystick *self = static_cast<SCA_PythonJoystick *>(self_v);
  const long index = PyLong_AsLong(value);
  if (index == -1 && PyErr_Occurred()) {
    PyErr_SetString(PyExc_TypeError, "joystick.playerIndex = int: expected an integer");
    return PY_SET_ATTR_FAIL;
  }
  if (index < -1 || index > INT_MAX) {
    PyErr_SetString(PyExc_ValueError,
                    "joystick.playerIndex = int: expected -1 (no player) or a positive index");
    return PY_SET_ATTR_FAIL;
  }
  if (!self->m_joystick->SetPlayerIndex(int(index))) {
    PyErr_SetString(PyExc_RuntimeError, "joystick.playerIndex: the controller rejected the index");
    return PY_SET_ATTR_FAIL;
  }
  return PY_SET_ATTR_SUCCESS;
}

EXP_PYMETHODDEF_DOC(
    SCA_PythonJoystick,
    enableSensors,
    "enableSensors(gyro=True, accel=False)\n"
    "\tTurns the motion sensors on or off. Returns False if a requested sensor is\n"
    "\tmissing or could not be switched.\n")
{
  static const char *kwlist[] = {"gyro", "accel", nullptr};
  int gyro = 1, accel = 0;
  if (!PyArg_ParseTupleAndKeywords(
          args, kwds, "|pp:enableSensors", const_cast<char **>(kwlist), &gyro, &accel))
  {
    return nullptr;
  }
  return PyBool_FromLong(m_joystick->EnableSensors(gyro != 0, accel != 0));
}

/* Tuples rather than mathutils.Vector: no wrapper allocation per read. */
static PyObject *vec3_to_python(const float v[3])
{
  PyObject *tuple = PyTuple_New(3);
  for (int i = 0; i < 3; i++) {
    PyTuple_SET_ITEM(tuple, i, PyFloat_FromDouble(v[i]));
  }
  return tuple;
}

PyObject *SCA_PythonJoystick::pyattr_get_has_gyro(EXP_PyObjectPlus *self_v,
                                                  const EXP_PYATTRIBUTE_DEF * /*attrdef*/)
{
  SCA_PythonJoystick *self = static_cast<SCA_PythonJoystick *>(self_v);
  return PyBool_FromLong(self->m_joystick->HasGyro());
}

PyObject *SCA_PythonJoystick::pyattr_get_has_accel(EXP_PyObjectPlus *self_v,
                                                   const EXP_PYATTRIBUTE_DEF * /*attrdef*/)
{
  SCA_PythonJoystick *self = static_cast<SCA_PythonJoystick *>(self_v);
  return PyBool_FromLong(self->m_joystick->HasAccel());
}

PyObject *SCA_PythonJoystick::pyattr_get_gyro(EXP_PyObjectPlus *self_v,
                                              const EXP_PYATTRIBUTE_DEF * /*attrdef*/)
{
  SCA_PythonJoystick *self = static_cast<SCA_PythonJoystick *>(self_v);
  return vec3_to_python(self->m_joystick->GetGyro());
}

PyObject *SCA_PythonJoystick::pyattr_get_gyro_delta(EXP_PyObjectPlus *self_v,
                                                    const EXP_PYATTRIBUTE_DEF * /*attrdef*/)
{
  SCA_PythonJoystick *self = static_cast<SCA_PythonJoystick *>(self_v);
  return vec3_to_python(self->m_joystick->GetGyroDelta());
}

PyObject *SCA_PythonJoystick::pyattr_get_accel(EXP_PyObjectPlus *self_v,
                                               const EXP_PYATTRIBUTE_DEF * /*attrdef*/)
{
  SCA_PythonJoystick *self = static_cast<SCA_PythonJoystick *>(self_v);
  return vec3_to_python(self->m_joystick->GetAccel());
}

PyObject *SCA_PythonJoystick::pyattr_get_touchpads(EXP_PyObjectPlus *self_v,
                                                   const EXP_PYATTRIBUTE_DEF * /*attrdef*/)
{
  SCA_PythonJoystick *self = static_cast<SCA_PythonJoystick *>(self_v);
  /* One list per touchpad (the Steam Deck has two), each holding its fingers as
   * (down, x, y, pressure) tuples. */
  const int num_touchpads = self->m_joystick->GetNumTouchpads();
  PyObject *touchpads = PyList_New(num_touchpads);
  for (int t = 0; t < num_touchpads; t++) {
    float fingers[8][4];
    const int num_fingers = self->m_joystick->GetTouchpadFingers(t, fingers, 8);
    PyObject *list = PyList_New(num_fingers);
    for (int f = 0; f < num_fingers; f++) {
      PyObject *finger = PyTuple_New(4);
      PyTuple_SET_ITEM(finger, 0, PyBool_FromLong(fingers[f][0] != 0.0f));
      PyTuple_SET_ITEM(finger, 1, PyFloat_FromDouble(fingers[f][1]));
      PyTuple_SET_ITEM(finger, 2, PyFloat_FromDouble(fingers[f][2]));
      PyTuple_SET_ITEM(finger, 3, PyFloat_FromDouble(fingers[f][3]));
      PyList_SET_ITEM(list, f, finger);
    }
    PyList_SET_ITEM(touchpads, t, list);
  }
  return touchpads;
}

PyObject *SCA_PythonJoystick::pyattr_get_battery(EXP_PyObjectPlus *self_v,
                                                 const EXP_PYATTRIBUTE_DEF * /*attrdef*/)
{
  SCA_PythonJoystick *self = static_cast<SCA_PythonJoystick *>(self_v);
  int percent;
  const char *state = self->m_joystick->GetBatteryState(&percent);
  PyObject *tuple = PyTuple_New(2);
  PyTuple_SET_ITEM(tuple, 0, PyUnicode_FromString(state));
  PyTuple_SET_ITEM(tuple, 1, PyLong_FromLong(percent));
  return tuple;
}

PyObject *SCA_PythonJoystick::pyattr_get_has_led(EXP_PyObjectPlus *self_v,
                                                 const EXP_PYATTRIBUTE_DEF * /*attrdef*/)
{
  SCA_PythonJoystick *self = static_cast<SCA_PythonJoystick *>(self_v);
  return PyBool_FromLong(self->m_joystick->HasLED());
}

PyObject *SCA_PythonJoystick::pyattr_get_has_trigger_vibration(
    EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF * /*attrdef*/)
{
  SCA_PythonJoystick *self = static_cast<SCA_PythonJoystick *>(self_v);
  return PyBool_FromLong(self->m_joystick->HasTriggerRumble());
}

/* [0, 1] float to a byte; NaN goes to 0 because every comparison with it is false. */
static unsigned char unit_to_byte(float v)
{
  return (unsigned char)((v > 0.0f) ? ((v < 1.0f) ? v * 255.0f + 0.5f : 255.0f) : 0.0f);
}

EXP_PYMETHODDEF_DOC_VARARGS(
    SCA_PythonJoystick,
    setLED,
    "setLED(red, green, blue)\n"
    "\tSets the light bar color, each channel in [0, 1]. Returns False if\n"
    "\tthe controller has no LED.\n")
{
  float red, green, blue;
  if (!PyArg_ParseTuple(args, "fff:setLED", &red, &green, &blue)) {
    return nullptr;
  }
  return PyBool_FromLong(
      m_joystick->SetLED(unit_to_byte(red), unit_to_byte(green), unit_to_byte(blue)));
}

EXP_PYMETHODDEF_DOC_VARARGS(SCA_PythonJoystick,
                            startTriggerVibration,
                            "startTriggerVibration(left, right, duration)\n"
                            "\tVibrates the trigger motors, strengths in [0, 1], duration in ms.\n"
                            "\tReturns False if the controller has no trigger motors.\n")
{
  float left, right;
  int duration;
  if (!PyArg_ParseTuple(args, "ffi:startTriggerVibration", &left, &right, &duration)) {
    return nullptr;
  }
  if (duration < 0) {
    PyErr_SetString(PyExc_ValueError,
                    "startTriggerVibration(left, right, duration): duration must be >= 0");
    return nullptr;
  }
  return PyBool_FromLong(m_joystick->RumbleTriggers(left, right, (unsigned int)duration));
}

EXP_PYMETHODDEF_DOC_NOARGS(SCA_PythonJoystick,
                           stopTriggerVibration,
                           "stopTriggerVibration()\n"
                           "\tStops the trigger motors.\n")
{
  return PyBool_FromLong(m_joystick->RumbleTriggers(0.0f, 0.0f, 0));
}

EXP_PYMETHODDEF_DOC_O(SCA_PythonJoystick,
                      sendEffect,
                      "sendEffect(data)\n"
                      "\tSends a raw, controller-specific effect packet (bytes). Returns False\n"
                      "\tif the controller does not accept it.\n")
{
  Py_buffer buffer;
  if (PyObject_GetBuffer(value, &buffer, PyBUF_SIMPLE) != 0) {
    return nullptr;
  }
  bool ok = false;
  if (buffer.len > INT_MAX) {
    PyErr_SetString(PyExc_ValueError, "sendEffect(data): data is too large");
  }
  else {
    ok = m_joystick->SendEffect(buffer.buf, (int)buffer.len);
  }
  PyBuffer_Release(&buffer);
  if (PyErr_Occurred()) {
    return nullptr;
  }
  return PyBool_FromLong(ok);
}

#endif  // WITH_PYTHON
