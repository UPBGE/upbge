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
 * Contributor(s): Porteries Tristan.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file ListWrapper.cpp
 *  \ingroup expressions
 */

#ifdef WITH_PYTHON

#  include "EXP_ListWrapper.h"

EXP_ListWrapper::EXP_ListWrapper(void *client,
                                 PyObject *base,
                                 bool (*checkValid)(void *),
                                 int (*getSize)(void *),
                                 PyObject *(*getItem)(void *, int),
                                 const std::string (*getItemName)(void *, int),
                                 bool (*setItem)(void *, int, PyObject *),
                                 int flag)
    : m_client(client),
      m_base(base),
      m_checkValid(checkValid),
      m_getSize(getSize),
      m_getItem(getItem),
      m_getItemName(getItemName),
      m_setItem(setItem),
      m_flag(flag)
{
  /* Incref to always have a existing pointer.
   * If there's no base python proxy it mean that we must manage the
   * invalidation of this list manualy when the instance which create
   * it is freed.
   */
  if (m_base) {
    Py_INCREF(m_base);
  }
}

EXP_ListWrapper::~EXP_ListWrapper()
{
  if (m_base) {
    Py_DECREF(m_base);
  }
}

bool EXP_ListWrapper::CheckValid()
{
  if (m_base && !EXP_PROXY_REF(m_base)) {
    return false;
  }
  return m_checkValid ? (*m_checkValid)(m_client) : true;
}

int EXP_ListWrapper::GetSize()
{
  return (*m_getSize)(m_client);
}

PyObject *EXP_ListWrapper::GetItem(int index)
{
  return (*m_getItem)(m_client, index);
}

const std::string EXP_ListWrapper::GetItemName(int index)
{
  return (*m_getItemName)(m_client, index);
}

bool EXP_ListWrapper::SetItem(int index, PyObject *item)
{
  return (*m_setItem)(m_client, index, item);
}

bool EXP_ListWrapper::AllowSetItem()
{
  return m_setItem != nullptr;
}

bool EXP_ListWrapper::AllowGetItemByName()
{
  return m_getItemName != nullptr;
}

bool EXP_ListWrapper::AllowFindValue()
{
  return (m_flag & FLAG_FIND_VALUE);
}

// ================================================================

std::string EXP_ListWrapper::GetName()
{
  return "ListWrapper";
}

std::string EXP_ListWrapper::GetText()
{
  std::string strListRep = "[";
  std::string commastr = "";

  for (unsigned int i = 0, size = GetSize(); i < size; ++i) {
    strListRep += commastr;
    strListRep += _PyUnicode_AsString(PyObject_Repr(GetItem(i)));
    commastr = ", ";
  }
  strListRep += "]";

  return strListRep;
}

int EXP_ListWrapper::GetValueType()
{
  return -1;
}

Py_ssize_t EXP_ListWrapper::py_len(PyObject *self)
{
  EXP_ListWrapper *list = (EXP_ListWrapper *)EXP_PROXY_REF(self);
  // Invalid list.
  if (!list->CheckValid()) {
    PyErr_SetString(PyExc_SystemError, "len(EXP_ListWrapper), " EXP_PROXY_ERROR_MSG);
    return 0;
  }

  return (Py_ssize_t)list->GetSize();
}

PyObject *EXP_ListWrapper::py_get_item(PyObject *self, Py_ssize_t index)
{
  EXP_ListWrapper *list = (EXP_ListWrapper *)EXP_PROXY_REF(self);
  // Invalid list.
  if (!list->CheckValid()) {
    PyErr_SetString(PyExc_SystemError, "val = EXP_ListWrapper[i], " EXP_PROXY_ERROR_MSG);
    return nullptr;
  }

  int size = list->GetSize();

  if (index < 0) {
    index = size + index;
  }
  if (index < 0 || index >= size) {
    PyErr_SetString(PyExc_IndexError,
                    "EXP_ListWrapper[i]: List index out of range in EXP_ListWrapper");
    return nullptr;
  }

  PyObject *pyobj = list->GetItem(index);

  return pyobj;
}

int EXP_ListWrapper::py_set_item(PyObject *self, Py_ssize_t index, PyObject *value)
{
  EXP_ListWrapper *list = (EXP_ListWrapper *)EXP_PROXY_REF(self);
  // Invalid list.
  if (!list->CheckValid()) {
    PyErr_SetString(PyExc_SystemError, "EXP_ListWrapper[i] = val, " EXP_PROXY_ERROR_MSG);
    return -1;
  }

  if (!list->AllowSetItem()) {
    PyErr_SetString(PyExc_TypeError, "EXP_ListWrapper's item type doesn't support assignment");
    return -1;
  }

  if (!value) {
    PyErr_SetString(PyExc_TypeError, "EXP_ListWrapper doesn't support item deletion");
    return -1;
  }

  int size = list->GetSize();

  if (index < 0) {
    index = size + index;
  }
  if (index < 0 || index >= size) {
    PyErr_SetString(PyExc_IndexError,
                    "EXP_ListWrapper[i]: List index out of range in EXP_ListWrapper");
    return -1;
  }

  if (!list->SetItem(index, value)) {
    return -1;
  }
  return 0;
}

PyObject *EXP_ListWrapper::py_mapping_subscript(PyObject *self, PyObject *key)
{
  EXP_ListWrapper *list = (EXP_ListWrapper *)EXP_PROXY_REF(self);
  // Invalid list.
  if (!list->CheckValid()) {
    PyErr_SetString(PyExc_SystemError, "val = EXP_ListWrapper[key], " EXP_PROXY_ERROR_MSG);
    return nullptr;
  }

  if (PyIndex_Check(key)) {
    Py_ssize_t index = PyLong_AsSsize_t(key);
    return py_get_item(self, index);
  }
  else if (PyUnicode_Check(key)) {
    if (!list->AllowGetItemByName()) {
      PyErr_SetString(PyExc_SystemError,
                      "EXP_ListWrapper's item type doesn't support access by key");
      return nullptr;
    }

    const char *name = _PyUnicode_AsString(key);
    int size = list->GetSize();

    for (unsigned int i = 0; i < size; ++i) {
      if (list->GetItemName(i) == name) {
        return list->GetItem(i);
      }
    }

    PyErr_Format(PyExc_KeyError, "requested item \"%s\" does not exist", name);
    return nullptr;
  }

  PyErr_Format(PyExc_KeyError, "EXP_ListWrapper[key]: '%R' key not in list", key);
  return nullptr;
}

int EXP_ListWrapper::py_mapping_ass_subscript(PyObject *self, PyObject *key, PyObject *value)
{
  EXP_ListWrapper *list = (EXP_ListWrapper *)EXP_PROXY_REF(self);
  // Invalid list.
  if (!list->CheckValid()) {
    PyErr_SetString(PyExc_SystemError, "val = EXP_ListWrapper[key], " EXP_PROXY_ERROR_MSG);
    return -1;
  }

  if (!list->AllowSetItem()) {
    PyErr_SetString(PyExc_TypeError, "EXP_ListWrapper's item type doesn't support assignment");
    return -1;
  }

  if (PyIndex_Check(key)) {
    Py_ssize_t index = PyLong_AsSsize_t(key);
    return py_set_item(self, index, value);
  }
  else if (PyUnicode_Check(key)) {
    if (!list->AllowGetItemByName()) {
      PyErr_SetString(PyExc_SystemError,
                      "EXP_ListWrapper's item type doesn't support access by key");
      return -1;
    }

    const char *name = _PyUnicode_AsString(key);
    int size = list->GetSize();

    for (unsigned int i = 0; i < size; ++i) {
      if (list->GetItemName(i) == name) {
        if (!list->SetItem(i, value)) {
          return -1;
        }
        return 0;
      }
    }

    PyErr_Format(PyExc_KeyError, "requested item \"%s\" does not exist", name);
    return -1;
  }

  PyErr_Format(PyExc_KeyError, "EXP_ListWrapper[key]: '%R' key not in list", key);
  return -1;
}

int EXP_ListWrapper::py_contains(PyObject *self, PyObject *key)
{
  EXP_ListWrapper *list = (EXP_ListWrapper *)EXP_PROXY_REF(self);
  // Invalid list.
  if (!list->CheckValid()) {
    PyErr_SetString(PyExc_SystemError, "val = EXP_ListWrapper[i], " EXP_PROXY_ERROR_MSG);
    return -1;
  }

  if (PyUnicode_Check(key)) {
    if (!list->AllowGetItemByName()) {
      PyErr_SetString(PyExc_SystemError,
                      "EXP_ListWrapper's item type doesn't support access by key");
      return -1;
    }

    const char *name = _PyUnicode_AsString(key);

    for (unsigned int i = 0, size = list->GetSize(); i < size; ++i) {
      if (list->GetItemName(i) == name) {
        return 1;
      }
    }
  }

  if (list->AllowFindValue()) {
    for (unsigned int i = 0, size = list->GetSize(); i < size; ++i) {
      if (PyObject_RichCompareBool(list->GetItem(i), key, Py_EQ) == 1) {
        return 1;
      }
    }
  }

  return 0;
}

PySequenceMethods EXP_ListWrapper::py_as_sequence = {
    py_len,                   // sq_length
    nullptr,                  // sq_concat
    nullptr,                  // sq_repeat
    py_get_item,              // sq_item
    nullptr,                  // sq_slice
    py_set_item,              // sq_ass_item
    nullptr,                  // sq_ass_slice
    (objobjproc)py_contains,  // sq_contains
    (binaryfunc) nullptr,     // sq_inplace_concat
    (ssizeargfunc) nullptr,   // sq_inplace_repeat
};

PyMappingMethods EXP_ListWrapper::py_as_mapping = {
    py_len,                   // mp_length
    py_mapping_subscript,     // mp_subscript
    py_mapping_ass_subscript  // mp_ass_subscript
};

PyTypeObject EXP_ListWrapper::Type = {
    PyVarObject_HEAD_INIT(nullptr, 0) "EXP_ListWrapper",  // tp_name
    sizeof(EXP_PyObjectPlus_Proxy),                       // tp_basicsize
    0,                                                    // tp_itemsize
    py_base_dealloc,                                      // tp_dealloc
    0,                                                    // tp_print
    0,                                                    // tp_getattr
    0,                                                    // tp_setattr
    0,                                                    // tp_compare
    py_base_repr,                                         // tp_repr
    0,                                                    // tp_as_number
    &py_as_sequence,                                      // tp_as_sequence
    &py_as_mapping,                                       // tp_as_mapping
    0,                                                    // tp_hash
    0,                                                    // tp_call
    0,
    nullptr,
    nullptr,
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
    &EXP_Value::Type,
    0,
    0,
    0,
    0,
    0,
    0,
    py_base_new};

PyMethodDef EXP_ListWrapper::Methods[] = {
    {"get", (PyCFunction)EXP_ListWrapper::sPyGet, METH_VARARGS}, {nullptr, nullptr}  // Sentinel
};

PyAttributeDef EXP_ListWrapper::Attributes[] = {
    EXP_PYATTRIBUTE_NULL  // Sentinel
};

// Matches python dict.get(key, [default]).
PyObject *EXP_ListWrapper::PyGet(PyObject *args)
{
  char *name;
  PyObject *def = Py_None;

  // Invalid list.
  if (!CheckValid()) {
    PyErr_SetString(PyExc_SystemError, "val = EXP_ListWrapper[i], " EXP_PROXY_ERROR_MSG);
    return nullptr;
  }

  if (!AllowGetItemByName()) {
    PyErr_SetString(PyExc_SystemError,
                    "EXP_ListWrapper's item type doesn't support access by key");
    return nullptr;
  }

  if (!PyArg_ParseTuple(args, "s|O:get", &name, &def)) {
    return nullptr;
  }

  for (unsigned int i = 0; i < GetSize(); ++i) {
    if (GetItemName(i) == name) {
      return GetItem(i);
    }
  }

  Py_INCREF(def);
  return def;
}

#endif  // WITH_PYTHON
