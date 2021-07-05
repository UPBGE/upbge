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

/** \file gameengine/GameLogic/SCA_PythonKeyboard.cpp
 *  \ingroup gamelogic
 */

#include "SCA_PythonKeyboard.h"

#include "GHOST_C-api.h"

#include "SCA_IInputDevice.h"

/* ------------------------------------------------------------------------- */
/* Native functions                                                          */
/* ------------------------------------------------------------------------- */

SCA_PythonKeyboard::SCA_PythonKeyboard(SCA_IInputDevice *keyboard)
    : EXP_PyObjectPlus(), m_keyboard(keyboard)
{
}

SCA_PythonKeyboard::~SCA_PythonKeyboard()
{
}

#ifdef WITH_PYTHON

/* ------------------------------------------------------------------------- */
/* Python functions                                                          */
/* ------------------------------------------------------------------------- */

/* clipboard */
static PyObject *gPyGetClipboard(PyObject *args, PyObject *kwds)
{
  char *buf = (char *)GHOST_getClipboard(0);
  return PyUnicode_FromString(buf ? buf : "");
}

static PyObject *gPySetClipboard(PyObject *args, PyObject *value)
{
  char *buf;
  if (!PyArg_ParseTuple(value, "s:setClipboard", &buf))
    Py_RETURN_NONE;

  GHOST_putClipboard((char *)buf, 0);
  Py_RETURN_NONE;
}

/* Integration hooks ------------------------------------------------------- */
PyTypeObject SCA_PythonKeyboard::Type = {PyVarObject_HEAD_INIT(nullptr, 0) "SCA_PythonKeyboard",
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

PyMethodDef SCA_PythonKeyboard::Methods[] = {
    {"getClipboard", (PyCFunction)gPyGetClipboard, METH_VARARGS, "getCliboard doc"},
    {"setClipboard", (PyCFunction)gPySetClipboard, METH_VARARGS, "setCliboard doc"},
    {nullptr, nullptr}  // Sentinel
};

PyAttributeDef SCA_PythonKeyboard::Attributes[] = {
    EXP_PYATTRIBUTE_RO_FUNCTION("events", SCA_PythonKeyboard, pyattr_get_events),
    EXP_PYATTRIBUTE_RO_FUNCTION("inputs", SCA_PythonKeyboard, pyattr_get_inputs),
    EXP_PYATTRIBUTE_RO_FUNCTION("active_events", SCA_PythonKeyboard, pyattr_get_active_events),
    EXP_PYATTRIBUTE_RO_FUNCTION("activeInputs", SCA_PythonKeyboard, pyattr_get_active_inputs),
    EXP_PYATTRIBUTE_RO_FUNCTION("text", SCA_PythonKeyboard, pyattr_get_text),
    EXP_PYATTRIBUTE_NULL  // Sentinel
};

PyObject *SCA_PythonKeyboard::pyattr_get_events(EXP_PyObjectPlus *self_v,
                                                const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_PythonKeyboard *self = static_cast<SCA_PythonKeyboard *>(self_v);

  EXP_ShowDeprecationWarning("keyboard.events", "keyboard.inputs");

  PyObject *dict = PyDict_New();

  for (int i = SCA_IInputDevice::BEGINKEY; i <= SCA_IInputDevice::ENDKEY; i++) {
    SCA_InputEvent &input = self->m_keyboard->GetInput((SCA_IInputDevice::SCA_EnumInputs)i);
    int event = 0;
    if (input.m_queue.empty()) {
      event = input.m_status[input.m_status.size() - 1];
    }
    else {
      event = input.m_queue[input.m_queue.size() - 1];
    }

    PyObject *key = PyLong_FromLong(i);
    PyObject *value = PyLong_FromLong(event);

    PyDict_SetItem(dict, key, value);

    Py_DECREF(key);
    Py_DECREF(value);
  }
  return dict;
}

PyObject *SCA_PythonKeyboard::pyattr_get_inputs(EXP_PyObjectPlus *self_v,
                                                const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_PythonKeyboard *self = static_cast<SCA_PythonKeyboard *>(self_v);

  PyObject *dict = PyDict_New();

  for (int i = SCA_IInputDevice::BEGINKEY; i <= SCA_IInputDevice::ENDKEY; i++) {
    SCA_InputEvent &input = self->m_keyboard->GetInput((SCA_IInputDevice::SCA_EnumInputs)i);

    PyObject *key = PyLong_FromLong(i);

    PyDict_SetItem(dict, key, input.GetProxy());

    Py_DECREF(key);
  }
  return dict;
}

PyObject *SCA_PythonKeyboard::pyattr_get_active_inputs(EXP_PyObjectPlus *self_v,
                                                       const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_PythonKeyboard *self = static_cast<SCA_PythonKeyboard *>(self_v);

  PyObject *dict = PyDict_New();

  for (int i = SCA_IInputDevice::BEGINKEY; i <= SCA_IInputDevice::ENDKEY; i++) {
    SCA_InputEvent &input = self->m_keyboard->GetInput((SCA_IInputDevice::SCA_EnumInputs)i);

    if (input.Find(SCA_InputEvent::ACTIVE)) {
      PyObject *key = PyLong_FromLong(i);

      PyDict_SetItem(dict, key, input.GetProxy());

      Py_DECREF(key);
    }
  }
  return dict;
}

PyObject *SCA_PythonKeyboard::pyattr_get_active_events(EXP_PyObjectPlus *self_v,
                                                       const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_PythonKeyboard *self = static_cast<SCA_PythonKeyboard *>(self_v);

  EXP_ShowDeprecationWarning("keyboard.active_events", "keyboard.activeInputs");

  PyObject *dict = PyDict_New();

  for (int i = SCA_IInputDevice::BEGINKEY; i <= SCA_IInputDevice::ENDKEY; i++) {
    SCA_InputEvent &input = self->m_keyboard->GetInput((SCA_IInputDevice::SCA_EnumInputs)i);

    if (input.Find(SCA_InputEvent::ACTIVE)) {
      int event = 0;
      if (input.m_queue.empty()) {
        event = input.m_status[input.m_status.size() - 1];
      }
      else {
        event = input.m_queue[input.m_queue.size() - 1];
      }

      PyObject *key = PyLong_FromLong(i);
      PyObject *value = PyLong_FromLong(event);

      PyDict_SetItem(dict, key, value);

      Py_DECREF(key);
      Py_DECREF(value);
    }
  }
  return dict;
}

PyObject *SCA_PythonKeyboard::pyattr_get_text(EXP_PyObjectPlus *self_v,
                                              const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_PythonKeyboard *self = (SCA_PythonKeyboard *)self_v;

  return PyUnicode_FromWideChar(self->m_keyboard->GetText().c_str(),
                                self->m_keyboard->GetText().size());
}

#endif
