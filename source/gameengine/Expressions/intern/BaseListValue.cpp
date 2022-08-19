/** \file gameengine/Expressions/BaseListValue.cpp
 *  \ingroup expressions
 */
// ListValue.cpp: implementation of the EXP_BaseListValue class.
//
//////////////////////////////////////////////////////////////////////
/*
 * Copyright (c) 1996-2000 Erwin Coumans <coockie@acm.org>
 *
 * Permission to use, copy, modify, distribute and sell this software
 * and its documentation for any purpose is hereby granted without fee,
 * provided that the above copyright notice appear in all copies and
 * that both that copyright notice and this permission notice appear
 * in supporting documentation.  Erwin Coumans makes no
 * representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 */

#include <regex>

#include "EXP_ListValue.h"

EXP_BaseListValue::EXP_BaseListValue() : m_bReleaseContents(true)
{
}

EXP_BaseListValue::~EXP_BaseListValue()
{
  if (m_bReleaseContents) {
    for (EXP_Value *item : m_pValueArray) {
      item->Release();
    }
  }
}

void EXP_BaseListValue::SetValue(int i, EXP_Value *val)
{
  m_pValueArray[i] = val;
}

EXP_Value *EXP_BaseListValue::GetValue(int i)
{
  return m_pValueArray[i];
}

EXP_Value *EXP_BaseListValue::FindValue(const std::string &name) const
{
  const VectorTypeConstIterator it = std::find_if(
      m_pValueArray.begin(), m_pValueArray.end(), [&name](EXP_Value *item) {
        return item->GetName() == name;
      });

  if (it != m_pValueArray.end()) {
    return *it;
  }
  return NULL;
}

bool EXP_BaseListValue::SearchValue(EXP_Value *val) const
{
  return (std::find(m_pValueArray.begin(), m_pValueArray.end(), val) != m_pValueArray.end());
}

void EXP_BaseListValue::Add(EXP_Value *value)
{
  m_pValueArray.push_back(value);
}

void EXP_BaseListValue::Insert(unsigned int i, EXP_Value *value)
{
  m_pValueArray.insert(m_pValueArray.begin() + i, value);
}

bool EXP_BaseListValue::RemoveValue(EXP_Value *val)
{
  bool result = false;
  for (VectorTypeIterator it = m_pValueArray.begin(); it != m_pValueArray.end();) {
    if (*it == val) {
      it = m_pValueArray.erase(it);
      result = true;
    }
    else {
      ++it;
    }
  }
  return result;
}

bool EXP_BaseListValue::CheckEqual(EXP_Value *first, EXP_Value *second)
{
  bool result = false;
  EXP_Value *eqval = first->Calc(VALUE_EQL_OPERATOR, second);
  if (eqval == NULL) {
    return false;
  }
  std::string text = eqval->GetText();
  if (text == EXP_BoolValue::sTrueString) {
    result = true;
  }
  eqval->Release();
  return result;
}

std::string EXP_BaseListValue::GetText()
{
  std::string strListRep = "[";
  std::string commastr = "";

  for (EXP_Value *item : m_pValueArray) {
    strListRep += commastr;
    strListRep += item->GetText();
    commastr = ", ";
  }
  strListRep += "]";

  return strListRep;
}

int EXP_BaseListValue::GetValueType()
{
  return VALUE_LIST_TYPE;
}

void EXP_BaseListValue::SetReleaseOnDestruct(bool bReleaseContents)
{
  m_bReleaseContents = bReleaseContents;
}

void EXP_BaseListValue::Remove(int i)
{
  m_pValueArray.erase(m_pValueArray.begin() + i);
}

void EXP_BaseListValue::Resize(int num)
{
  m_pValueArray.resize(num);
}

void EXP_BaseListValue::ReleaseAndRemoveAll()
{
  for (EXP_Value *item : m_pValueArray) {
    item->Release();
  }
  m_pValueArray.clear();
}

int EXP_BaseListValue::GetCount() const
{
  return m_pValueArray.size();
}

#ifdef WITH_PYTHON

/* --------------------------------------------------------------------- */
/* Python interface ---------------------------------------------------- */
/* --------------------------------------------------------------------- */

Py_ssize_t EXP_BaseListValue::bufferlen(PyObject *self)
{
  EXP_BaseListValue *list = static_cast<EXP_BaseListValue *>(EXP_PROXY_REF(self));
  if (list == nullptr) {
    return 0;
  }

  return (Py_ssize_t)list->GetCount();
}

PyObject *EXP_BaseListValue::buffer_item(PyObject *self, Py_ssize_t index)
{
  EXP_BaseListValue *list = static_cast<EXP_BaseListValue *>(EXP_PROXY_REF(self));

  if (list == nullptr) {
    PyErr_SetString(PyExc_SystemError, "val = list[i], " EXP_PROXY_ERROR_MSG);
    return nullptr;
  }

  int count = list->GetCount();

  if (index < 0) {
    index = count + index;
  }

  if (index < 0 || index >= count) {
    PyErr_SetString(PyExc_IndexError, "list[i]: Python ListIndex out of range in CValueList");
    return nullptr;
  }

  EXP_Value *cval = list->GetValue(index);

  PyObject *pyobj = cval->ConvertValueToPython();
  if (pyobj) {
    return pyobj;
  }
  else {
    return cval->GetProxy();
  }
}

// Just slice it into a python list...
PyObject *EXP_BaseListValue::buffer_slice(EXP_BaseListValue *list,
                                          Py_ssize_t start,
                                          Py_ssize_t stop)
{
  PyObject *newlist = PyList_New(stop - start);
  if (!newlist) {
    return nullptr;
  }

  for (Py_ssize_t i = start, j = 0; i < stop; i++, j++) {
    PyObject *pyobj = list->GetValue(i)->ConvertValueToPython();
    if (!pyobj) {
      pyobj = list->GetValue(i)->GetProxy();
    }
    PyList_SET_ITEM(newlist, j, pyobj);
  }
  return newlist;
}

PyObject *EXP_BaseListValue::mapping_subscript(PyObject *self, PyObject *key)
{
  EXP_BaseListValue *list = static_cast<EXP_BaseListValue *>(EXP_PROXY_REF(self));
  if (list == nullptr) {
    PyErr_SetString(PyExc_SystemError, "value = list[i], " EXP_PROXY_ERROR_MSG);
    return nullptr;
  }

  if (PyUnicode_Check(key)) {
    EXP_Value *item = list->FindValue(_PyUnicode_AsString(key));
    if (item) {
      PyObject *pyobj = item->ConvertValueToPython();
      if (pyobj) {
        return pyobj;
      }
      else {
        return item->GetProxy();
      }
    }
  }
  else if (PyIndex_Check(key)) {
    Py_ssize_t index = PyLong_AsSsize_t(key);
    return buffer_item(self, index);  // Wont add a ref.
  }
  else if (PySlice_Check(key)) {
    Py_ssize_t start, stop, step, slicelength;

    if (PySlice_GetIndicesEx(key, list->GetCount(), &start, &stop, &step, &slicelength) < 0) {
      return nullptr;
    }

    if (slicelength <= 0) {
      return PyList_New(0);
    }
    else if (step == 1) {
      return buffer_slice(list, start, stop);
    }
    else {
      PyErr_SetString(PyExc_TypeError, "list[slice]: slice steps not supported");
      return nullptr;
    }
  }

  PyErr_Format(PyExc_KeyError, "list[key]: '%R' key not in list", key);
  return nullptr;
}

// clist + list, return a list that python owns.
PyObject *EXP_BaseListValue::buffer_concat(PyObject *self, PyObject *other)
{
  EXP_BaseListValue *listval = static_cast<EXP_BaseListValue *>(EXP_PROXY_REF(self));

  if (listval == nullptr) {
    PyErr_SetString(PyExc_SystemError, "list+other, " EXP_PROXY_ERROR_MSG);
    return nullptr;
  }

  Py_ssize_t numitems_orig = listval->GetCount();

  /* For now, we support EXP_BaseListValue concatenated with items
   * and EXP_BaseListValue concatenated to Python Lists
   * and EXP_BaseListValue concatenated with another EXP_BaseListValue.
   */

  // Shallow copy, don't use listval->GetReplica(), it will screw up with KX_GameObjects.
  EXP_ListValue<EXP_Value> *listval_new = new EXP_ListValue<EXP_Value>();

  if (PyList_Check(other)) {
    Py_ssize_t numitems = PyList_GET_SIZE(other);

    // Copy the first part of the list.
    listval_new->Resize(numitems_orig + numitems);
    for (Py_ssize_t i = 0; i < numitems_orig; i++) {
      listval_new->SetValue(i, listval->GetValue(i)->AddRef());
    }

    for (Py_ssize_t i = 0; i < numitems; i++) {
      EXP_Value *listitemval = listval->ConvertPythonToValue(
          PyList_GET_ITEM(other, i), true, "cList + pyList: EXP_BaseListValue, ");

      if (listitemval) {
        listval_new->SetValue(i + numitems_orig, listitemval);
      }
      else {
        listval_new->Resize(numitems_orig +
                            i);  // Resize so we don't try release nullptr pointers.
        listval_new->Release();
        return nullptr;  // ConvertPythonToValue above sets the error.
      }
    }
  }
  else if (PyObject_TypeCheck(other, &EXP_BaseListValue::Type)) {
    // Add items from otherlist to this list.
    EXP_BaseListValue *otherval = static_cast<EXP_BaseListValue *>(EXP_PROXY_REF(other));
    if (otherval == nullptr) {
      listval_new->Release();
      PyErr_SetString(PyExc_SystemError, "list+other, " EXP_PROXY_ERROR_MSG);
      return nullptr;
    }

    Py_ssize_t numitems = otherval->GetCount();

    // Copy the first part of the list.
    listval_new->Resize(numitems_orig +
                        numitems);  // Resize so we don't try release nullptr pointers.
    for (Py_ssize_t i = 0; i < numitems_orig; i++) {
      listval_new->SetValue(i, listval->GetValue(i)->AddRef());
    }

    // Now copy the other part of the list.
    for (Py_ssize_t i = 0; i < numitems; i++) {
      listval_new->SetValue(i + numitems_orig, otherval->GetValue(i)->AddRef());
    }
  }
  return listval_new->NewProxy(true);  // Python owns this list.
}

int EXP_BaseListValue::buffer_contains(PyObject *self_v, PyObject *value)
{
  EXP_BaseListValue *self = static_cast<EXP_BaseListValue *>(EXP_PROXY_REF(self_v));

  if (self == nullptr) {
    PyErr_SetString(PyExc_SystemError, "val in list, " EXP_PROXY_ERROR_MSG);
    return -1;
  }

  if (PyUnicode_Check(value)) {
    if (self->FindValue(_PyUnicode_AsString(value))) {
      return 1;
    }
  }
  // Not dict like at all but this worked before __contains__ was used.
  else if (PyObject_TypeCheck(value, &EXP_Value::Type)) {
    EXP_Value *item = static_cast<EXP_Value *>(EXP_PROXY_REF(value));
    for (int i = 0; i < self->GetCount(); i++) {
      if (self->GetValue(i) == item) {
        return 1;
      }
    }
  }

  return 0;
}

PySequenceMethods EXP_BaseListValue::as_sequence = {
    bufferlen,                   //(inquiry)buffer_length, /*sq_length*/
    buffer_concat,               /*sq_concat*/
    nullptr,                     /*sq_repeat*/
    buffer_item,                 /*sq_item*/
                                 // TODO, slicing in py3
    nullptr,                     // buffer_slice, /*sq_slice*/
    nullptr,                     /*sq_ass_item*/
    nullptr,                     /*sq_ass_slice*/
    (objobjproc)buffer_contains, /* sq_contains */
    (binaryfunc) nullptr,        /* sq_inplace_concat */
    (ssizeargfunc) nullptr,      /* sq_inplace_repeat */
};

// Is this one used ?
PyMappingMethods EXP_BaseListValue::instance_as_mapping = {
    bufferlen,         /*mp_length*/
    mapping_subscript, /*mp_subscript*/
    nullptr            /*mp_ass_subscript*/
};

PyTypeObject EXP_BaseListValue::Type = {
    PyVarObject_HEAD_INIT(nullptr, 0) "EXP_ListValue", /*tp_name*/
    sizeof(EXP_PyObjectPlus_Proxy),                    /*tp_basicsize*/
    0,                                                 /*tp_itemsize*/
    /* methods */
    py_base_dealloc,      /*tp_dealloc*/
    0,                    /*tp_print*/
    0,                    /*tp_getattr*/
    0,                    /*tp_setattr*/
    0,                    /*tp_compare*/
    py_base_repr,         /*tp_repr*/
    0,                    /*tp_as_number*/
    &as_sequence,         /*tp_as_sequence*/
    &instance_as_mapping, /*tp_as_mapping*/
    0,                    /*tp_hash*/
    0,                    /*tp_call */
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

PyMethodDef EXP_BaseListValue::Methods[] = {
    // List style access.
    {"append", (PyCFunction)EXP_BaseListValue::sPyappend, METH_O},
    {"reverse", (PyCFunction)EXP_BaseListValue::sPyreverse, METH_NOARGS},
    {"index", (PyCFunction)EXP_BaseListValue::sPyindex, METH_O},
    {"count", (PyCFunction)EXP_BaseListValue::sPycount, METH_O},

    // Dict style access.
    {"get", (PyCFunction)EXP_BaseListValue::sPyget, METH_VARARGS},
    {"filter", (PyCFunction)EXP_BaseListValue::sPyfilter, METH_VARARGS},

    // Own cvalue funcs.
    {"from_id", (PyCFunction)EXP_BaseListValue::sPyfrom_id, METH_O},

    {nullptr, nullptr}  // Sentinel
};

PyAttributeDef EXP_BaseListValue::Attributes[] = {
    EXP_PYATTRIBUTE_NULL  // Sentinel
};

PyObject *EXP_BaseListValue::Pyappend(PyObject *value)
{
  EXP_Value *objval = ConvertPythonToValue(value, true, "list.append(i): CValueList, ");

  if (!objval) {
    // ConvertPythonToValue sets the error.
    return nullptr;
  }

  if (!EXP_PROXY_PYOWNS(m_proxy)) {
    PyErr_SetString(PyExc_TypeError, "list.append(i): internal values can't be modified");
    return nullptr;
  }

  Add(objval);

  Py_RETURN_NONE;
}

PyObject *EXP_BaseListValue::Pyreverse()
{
  if (!EXP_PROXY_PYOWNS(m_proxy)) {
    PyErr_SetString(PyExc_TypeError, "list.reverse(): internal values can't be modified");
    return nullptr;
  }

  std::reverse(m_pValueArray.begin(), m_pValueArray.end());
  Py_RETURN_NONE;
}

PyObject *EXP_BaseListValue::Pyindex(PyObject *value)
{
  PyObject *result = nullptr;

  EXP_Value *checkobj = ConvertPythonToValue(value, true, "val = cList[i]: CValueList, ");
  if (checkobj == nullptr) {
    // ConvertPythonToValue sets the error.
    return nullptr;
  }
  int numelem = GetCount();
  for (int i = 0; i < numelem; i++) {
    EXP_Value *elem = GetValue(i);
    if (checkobj == elem || CheckEqual(checkobj, elem)) {
      result = PyLong_FromLong(i);
      break;
    }
  }
  checkobj->Release();

  if (result == nullptr) {
    PyErr_SetString(PyExc_ValueError, "list.index(x): x not in EXP_BaseListValue");
  }
  return result;
}

PyObject *EXP_BaseListValue::Pycount(PyObject *value)
{
  int numfound = 0;

  EXP_Value *checkobj = ConvertPythonToValue(value, false, "");  // Error ignored.

  // in this case just return that there are no items in the list.
  if (checkobj == nullptr) {
    PyErr_Clear();
    return PyLong_FromLong(0);
  }

  int numelem = GetCount();
  for (int i = 0; i < numelem; i++) {
    EXP_Value *elem = GetValue(i);
    if (checkobj == elem || CheckEqual(checkobj, elem)) {
      numfound++;
    }
  }
  checkobj->Release();

  return PyLong_FromLong(numfound);
}

// Matches python dict.get(key, [default]).
PyObject *EXP_BaseListValue::Pyget(PyObject *args)
{
  char *key;
  PyObject *def = Py_None;

  if (!PyArg_ParseTuple(args, "s|O:get", &key, &def)) {
    return nullptr;
  }

  EXP_Value *item = FindValue(key);
  if (item) {
    PyObject *pyobj = item->ConvertValueToPython();
    if (pyobj) {
      return pyobj;
    }
    else {
      return item->GetProxy();
    }
  }

  Py_INCREF(def);
  return def;
}

PyObject *EXP_BaseListValue::Pyfilter(PyObject *args)
{
  const char *namestr = "";
  const char *propstr = "";

  if (!PyArg_ParseTuple(args, "s|s:filter", &namestr, &propstr)) {
    return nullptr;
  }

  if (strlen(namestr) == 0 && strlen(propstr) == 0) {
    PyErr_SetString(PyExc_ValueError, "list.filter(name, prop): empty expressions.");
    return nullptr;
  }

  std::regex namereg;
  std::regex propreg;
  try {
    namereg = std::regex(namestr);
    propreg = std::regex(propstr);
  }
  catch (const std::regex_error &error) {
    PyErr_Format(
        PyExc_ValueError, "list.filter(name, prop): invalid expression: %s.", error.what());
    return nullptr;
  }

  EXP_ListValue<EXP_Value> *result = new EXP_ListValue<EXP_Value>();
  result->SetReleaseOnDestruct(false);

  for (EXP_Value *item : m_pValueArray) {
    if (strlen(namestr) == 0 || std::regex_match(item->GetName(), namereg)) {
      if (strlen(propstr) == 0) {
        result->Add(item);
      }
      else {
        const std::vector<std::string> propnames = item->GetPropertyNames();
        for (const std::string &propname : propnames) {
          if (std::regex_match(propname, propreg)) {
            result->Add(item);
            break;
          }
        }
      }
    }
  }

  return result->NewProxy(true);
}

PyObject *EXP_BaseListValue::Pyfrom_id(PyObject *value)
{
  uintptr_t id = (uintptr_t)PyLong_AsVoidPtr(value);

  if (PyErr_Occurred()) {
    return nullptr;
  }

  int numelem = GetCount();
  for (int i = 0; i < numelem; i++) {
    if (reinterpret_cast<uintptr_t>(m_pValueArray[i]->m_proxy) == id) {
      return GetValue(i)->GetProxy();
    }
  }

  PyErr_SetString(PyExc_IndexError, "from_id(#): id not found in CValueList");
  return nullptr;
}

#endif  // WITH_PYTHON
