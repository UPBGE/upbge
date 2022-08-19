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
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/GameLogic/SCA_PythonMouse.cpp
 *  \ingroup gamelogic
 */

#include "SCA_PythonMouse.h"

#include "RAS_ICanvas.h"
#include "SCA_IInputDevice.h"

/* ------------------------------------------------------------------------- */
/* Native functions                                                          */
/* ------------------------------------------------------------------------- */

SCA_PythonMouse::SCA_PythonMouse(SCA_IInputDevice *mouse, RAS_ICanvas *canvas)
    : EXP_PyObjectPlus(), m_mouse(mouse), m_canvas(canvas)
{
#ifdef WITH_PYTHON
  m_event_dict = PyDict_New();
#endif
}

SCA_PythonMouse::~SCA_PythonMouse()
{
#ifdef WITH_PYTHON
  PyDict_Clear(m_event_dict);
  Py_DECREF(m_event_dict);
#endif
}

#ifdef WITH_PYTHON

/* ------------------------------------------------------------------------- */
/* Python functions                                                          */
/* ------------------------------------------------------------------------- */

/* Integration hooks ------------------------------------------------------- */
PyTypeObject SCA_PythonMouse::Type = {PyVarObject_HEAD_INIT(nullptr, 0) "SCA_PythonMouse",
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

PyMethodDef SCA_PythonMouse::Methods[] = {
    {nullptr, nullptr}  // Sentinel
};

PyAttributeDef SCA_PythonMouse::Attributes[] = {
    EXP_PYATTRIBUTE_RO_FUNCTION("events", SCA_PythonMouse, pyattr_get_events),
    EXP_PYATTRIBUTE_RO_FUNCTION("inputs", SCA_PythonMouse, pyattr_get_inputs),
    EXP_PYATTRIBUTE_RO_FUNCTION("active_events", SCA_PythonMouse, pyattr_get_active_events),
    EXP_PYATTRIBUTE_RO_FUNCTION("activeInputs", SCA_PythonMouse, pyattr_get_active_inputs),
    EXP_PYATTRIBUTE_RW_FUNCTION(
        "position", SCA_PythonMouse, pyattr_get_position, pyattr_set_position),
    EXP_PYATTRIBUTE_RW_FUNCTION(
        "visible", SCA_PythonMouse, pyattr_get_visible, pyattr_set_visible),
    EXP_PYATTRIBUTE_NULL  // Sentinel
};

PyObject *SCA_PythonMouse::pyattr_get_events(EXP_PyObjectPlus *self_v,
                                             const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_PythonMouse *self = static_cast<SCA_PythonMouse *>(self_v);

  EXP_ShowDeprecationWarning("mouse.events", "mouse.inputs");

  for (int i = SCA_IInputDevice::BEGINMOUSE; i <= SCA_IInputDevice::ENDMOUSE; i++) {
    SCA_InputEvent &input = self->m_mouse->GetInput((SCA_IInputDevice::SCA_EnumInputs)i);
    int event = 0;
    if (input.m_queue.size() > 0) {
      event = input.m_queue[input.m_queue.size() - 1];
    }
    else {
      event = input.m_status[input.m_status.size() - 1];
    }

    PyObject *key = PyLong_FromLong(i);
    PyObject *value = PyLong_FromLong(event);

    PyDict_SetItem(self->m_event_dict, key, value);

    Py_DECREF(key);
    Py_DECREF(value);
  }
  Py_INCREF(self->m_event_dict);
  return self->m_event_dict;
}

PyObject *SCA_PythonMouse::pyattr_get_inputs(EXP_PyObjectPlus *self_v,
                                             const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_PythonMouse *self = static_cast<SCA_PythonMouse *>(self_v);

  for (int i = SCA_IInputDevice::BEGINMOUSE; i <= SCA_IInputDevice::ENDMOUSE; i++) {
    SCA_InputEvent &input = self->m_mouse->GetInput((SCA_IInputDevice::SCA_EnumInputs)i);

    PyObject *key = PyLong_FromLong(i);

    PyDict_SetItem(self->m_event_dict, key, input.GetProxy());

    Py_DECREF(key);
  }
  Py_INCREF(self->m_event_dict);
  return self->m_event_dict;
}

PyObject *SCA_PythonMouse::pyattr_get_active_events(EXP_PyObjectPlus *self_v,
                                                    const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_PythonMouse *self = static_cast<SCA_PythonMouse *>(self_v);

  EXP_ShowDeprecationWarning("mouse.active_events", "mouse.activeInputs");

  PyDict_Clear(self->m_event_dict);

  for (int i = SCA_IInputDevice::BEGINMOUSE; i <= SCA_IInputDevice::ENDMOUSE; i++) {
    SCA_InputEvent &input = self->m_mouse->GetInput((SCA_IInputDevice::SCA_EnumInputs)i);

    if (input.Find(SCA_InputEvent::ACTIVE)) {
      int event = 0;
      if (input.m_queue.size() > 0) {
        event = input.m_queue[input.m_queue.size() - 1];
      }
      else {
        event = input.m_status[input.m_status.size() - 1];
      }

      PyObject *key = PyLong_FromLong(i);
      PyObject *value = PyLong_FromLong(event);

      PyDict_SetItem(self->m_event_dict, key, value);

      Py_DECREF(key);
      Py_DECREF(value);
    }
  }
  Py_INCREF(self->m_event_dict);
  return self->m_event_dict;
}

PyObject *SCA_PythonMouse::pyattr_get_active_inputs(EXP_PyObjectPlus *self_v,
                                                    const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_PythonMouse *self = static_cast<SCA_PythonMouse *>(self_v);

  PyDict_Clear(self->m_event_dict);

  for (int i = SCA_IInputDevice::BEGINMOUSE; i <= SCA_IInputDevice::ENDMOUSE; i++) {
    SCA_InputEvent &input = self->m_mouse->GetInput((SCA_IInputDevice::SCA_EnumInputs)i);

    if (input.Find(SCA_InputEvent::ACTIVE)) {
      PyObject *key = PyLong_FromLong(i);

      PyDict_SetItem(self->m_event_dict, key, input.GetProxy());

      Py_DECREF(key);
    }
  }
  Py_INCREF(self->m_event_dict);
  return self->m_event_dict;
}

PyObject *SCA_PythonMouse::pyattr_get_position(EXP_PyObjectPlus *self_v,
                                               const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_PythonMouse *self = static_cast<SCA_PythonMouse *>(self_v);
  const SCA_InputEvent &xevent = self->m_mouse->GetInput(SCA_IInputDevice::MOUSEX);
  const SCA_InputEvent &yevent = self->m_mouse->GetInput(SCA_IInputDevice::MOUSEY);

  float x_coord, y_coord;

  x_coord = self->m_canvas->GetMouseNormalizedX(xevent.m_values[xevent.m_values.size() - 1]);
  y_coord = self->m_canvas->GetMouseNormalizedY(yevent.m_values[yevent.m_values.size() - 1]);

  PyObject *ret = PyTuple_New(2);

  PyTuple_SET_ITEM(ret, 0, PyFloat_FromDouble(x_coord));
  PyTuple_SET_ITEM(ret, 1, PyFloat_FromDouble(y_coord));

  return ret;
}

int SCA_PythonMouse::pyattr_set_position(EXP_PyObjectPlus *self_v,
                                         const EXP_PYATTRIBUTE_DEF *attrdef,
                                         PyObject *value)
{
  SCA_PythonMouse *self = static_cast<SCA_PythonMouse *>(self_v);
  int x, y;
  float pyx, pyy;
  if (!PyArg_ParseTuple(value, "ff:position", &pyx, &pyy))
    return PY_SET_ATTR_FAIL;

  x = (int)(pyx * self->m_canvas->GetWidth());
  y = (int)(pyy * self->m_canvas->GetHeight());

  self->m_canvas->SetMousePosition(x, y);

  return PY_SET_ATTR_SUCCESS;
}

PyObject *SCA_PythonMouse::pyattr_get_visible(EXP_PyObjectPlus *self_v,
                                              const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_PythonMouse *self = static_cast<SCA_PythonMouse *>(self_v);

  int visible;

  if (self->m_canvas->GetMouseState() == RAS_ICanvas::MOUSE_INVISIBLE)
    visible = 0;
  else
    visible = 1;

  return PyBool_FromLong(visible);
}

int SCA_PythonMouse::pyattr_set_visible(EXP_PyObjectPlus *self_v,
                                        const EXP_PYATTRIBUTE_DEF *attrdef,
                                        PyObject *value)
{
  SCA_PythonMouse *self = static_cast<SCA_PythonMouse *>(self_v);

  int visible = PyObject_IsTrue(value);

  if (visible == -1) {
    PyErr_SetString(PyExc_AttributeError,
                    "SCA_PythonMouse.visible = bool: SCA_PythonMouse, expected True or False");
    return PY_SET_ATTR_FAIL;
  }

  if (visible)
    self->m_canvas->SetMouseState(RAS_ICanvas::MOUSE_NORMAL);
  else
    self->m_canvas->SetMouseState(RAS_ICanvas::MOUSE_INVISIBLE);

  return PY_SET_ATTR_SUCCESS;
}

#endif
