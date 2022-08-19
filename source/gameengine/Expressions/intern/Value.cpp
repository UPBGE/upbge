/** \file gameengine/Expressions/Value.cpp
 *  \ingroup expressions
 */
// Value.cpp: implementation of the EXP_Value class.
// developed at Eindhoven University of Technology, 1997
// by the OOPS team
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

#include "EXP_Value.h"

#include "EXP_BoolValue.h"
#include "EXP_ErrorValue.h"
#include "EXP_FloatValue.h"
#include "EXP_IntValue.h"
#include "EXP_StringValue.h"

#ifdef WITH_PYTHON

PyTypeObject EXP_Value::Type = {PyVarObject_HEAD_INIT(nullptr, 0) "EXP_Value",
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
                                &EXP_PyObjectPlus::Type,
                                0,
                                0,
                                0,
                                0,
                                0,
                                0,
                                py_base_new};

PyMethodDef EXP_Value::Methods[] = {
    {nullptr, nullptr}  // Sentinel
};
#endif  // WITH_PYTHON

EXP_Value::EXP_Value()
{
}

EXP_Value::~EXP_Value()
{
  ClearProperties();
}

std::string EXP_Value::op2str(VALUE_OPERATOR op)
{
  std::string opmsg;
  switch (op) {
    case VALUE_MOD_OPERATOR: {
      opmsg = " % ";
      break;
    }
    case VALUE_ADD_OPERATOR: {
      opmsg = " + ";
      break;
    }
    case VALUE_SUB_OPERATOR: {
      opmsg = " - ";
      break;
    }
    case VALUE_MUL_OPERATOR: {
      opmsg = " * ";
      break;
    }
    case VALUE_DIV_OPERATOR: {
      opmsg = " / ";
      break;
    }
    case VALUE_NEG_OPERATOR: {
      opmsg = " -";
      break;
    }
    case VALUE_POS_OPERATOR: {
      opmsg = " +";
      break;
    }
    case VALUE_AND_OPERATOR: {
      opmsg = " & ";
      break;
    }
    case VALUE_OR_OPERATOR: {
      opmsg = " | ";
      break;
    }
    case VALUE_EQL_OPERATOR: {
      opmsg = " = ";
      break;
    }
    case VALUE_NEQ_OPERATOR: {
      opmsg = " != ";
      break;
    }
    case VALUE_NOT_OPERATOR: {
      opmsg = " !";
      break;
    }
    default: {
      opmsg = "Error in Errorhandling routine.";
      break;
    }
  }
  return opmsg;
}

//---------------------------------------------------------------------------------------------------------------------
//	Property Management
//---------------------------------------------------------------------------------------------------------------------

/// Set property <ioProperty>, overwrites and releases a previous property with the same name if
/// needed.
void EXP_Value::SetProperty(const std::string &name, EXP_Value *ioProperty)
{
  // Check if somebody is setting an empty property.
  if (ioProperty == nullptr) {
    trace("Warning:trying to set empty property!");
    return;
  }

  // Try to replace property (if so -> exit as soon as we replaced it).
  EXP_Value *oldval = m_properties[name];
  if (oldval) {
    oldval->Release();
  }

  // Add property at end of array.
  m_properties[name] = ioProperty->AddRef();
}

/// Get pointer to a property with name <inName>, returns nullptr if there is no property named
/// <inName>.
EXP_Value *EXP_Value::GetProperty(const std::string &inName)
{
  std::map<std::string, EXP_Value *>::iterator it = m_properties.find(inName);
  if (it != m_properties.end()) {
    return it->second;
  }
  return nullptr;
}

/// Get text description of property with name <inName>, returns an empty string if there is no
/// property named <inName>.
const std::string EXP_Value::GetPropertyText(const std::string &inName)
{
  EXP_Value *property = GetProperty(inName);
  if (property) {
    return property->GetText();
  }
  else {
    return "";
  }
}

float EXP_Value::GetPropertyNumber(const std::string &inName, float defnumber)
{
  EXP_Value *property = GetProperty(inName);
  if (property) {
    return property->GetNumber();
  }
  else {
    return defnumber;
  }
}

/// Remove the property named <inName>, returns true if the property was succesfully removed, false
/// if property was not found or could not be removed.
bool EXP_Value::RemoveProperty(const std::string &inName)
{
  std::map<std::string, EXP_Value *>::iterator it = m_properties.find(inName);
  if (it != m_properties.end()) {
    (*it).second->Release();
    m_properties.erase(it);
    return true;
  }

  return false;
}

/// Get Property Names.
std::vector<std::string> EXP_Value::GetPropertyNames()
{
  const unsigned short size = m_properties.size();
  std::vector<std::string> result(size);

  unsigned short i = 0;
  for (const auto &pair : m_properties) {
    result[i++] = pair.first;
  }
  return result;
}

/// Clear all properties.
void EXP_Value::ClearProperties()
{
  // Remove all properties.
  for (const auto &pair : m_properties) {
    pair.second->Release();
  }

  // Delete property array.
  m_properties.clear();
}

/// Get property number <inIndex>.
EXP_Value *EXP_Value::GetProperty(int inIndex)
{
  int count = 0;

  for (const auto &pair : m_properties) {
    if (count++ == inIndex) {
      return pair.second;
    }
  }
  return nullptr;
}

/// Get the amount of properties assiocated with this value.
int EXP_Value::GetPropertyCount()
{
  return m_properties.size();
}

void EXP_Value::DestructFromPython()
{
#ifdef WITH_PYTHON
  // Avoid decrefing freed proxy in destructor.
  m_proxy = nullptr;
  Release();
#endif  // WITH_PYTHON
}

void EXP_Value::ProcessReplica()
{
  EXP_PyObjectPlus::ProcessReplica();

  // Copy all props.
  for (auto &pair : m_properties) {
    pair.second = pair.second->GetReplica();
  }
}

int EXP_Value::GetValueType()
{
  return VALUE_NO_TYPE;
}

EXP_Value *EXP_Value::FindIdentifier(const std::string &identifiername)
{
  EXP_Value *result = nullptr;

  int pos = 0;
  // if a dot exists, explode the name into pieces to get the subcontext
  if ((pos = identifiername.find('.')) != std::string::npos) {
    const std::string rightstring = identifiername.substr(pos + 1);
    const std::string leftstring = identifiername.substr(0, pos);
    EXP_Value *tempresult = GetProperty(leftstring);
    if (tempresult) {
      result = tempresult->FindIdentifier(rightstring);
    }
  }
  else {
    result = GetProperty(identifiername);
    if (result) {
      return result->AddRef();
    }
  }
  if (!result) {
    result = new EXP_ErrorValue(identifiername + " not found");
  }
  return result;
}

#ifdef WITH_PYTHON

PyAttributeDef EXP_Value::Attributes[] = {
    EXP_PYATTRIBUTE_RO_FUNCTION("name", EXP_Value, pyattr_get_name),
    EXP_PYATTRIBUTE_NULL  // Sentinel
};

PyObject *EXP_Value::pyattr_get_name(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
  EXP_Value *self = static_cast<EXP_Value *>(self_v);
  return PyUnicode_FromStdString(self->GetName());
}

/**
 * There are 2 reasons this could return nullptr
 * - unsupported type.
 * - error converting (overflow).
 *
 * \param do_type_exception Use to skip raising an exception for unknown types.
 */
EXP_Value *EXP_Value::ConvertPythonToValue(PyObject *pyobj,
                                           const bool do_type_exception,
                                           const char *error_prefix)
{

  EXP_Value *vallie;
  // Note: Boolean check should go before Int check [#34677].
  if (PyBool_Check(pyobj)) {
    vallie = new EXP_BoolValue((bool)PyLong_AsLongLong(pyobj));
  }
  else if (PyFloat_Check(pyobj)) {
    const double tval = PyFloat_AsDouble(pyobj);
    if (tval > (double)FLT_MAX || tval < (double)-FLT_MAX) {
      PyErr_Format(PyExc_OverflowError,
                   "%soverflow converting from float, out of internal range",
                   error_prefix);
      vallie = nullptr;
    }
    else {
      vallie = new EXP_FloatValue((float)tval);
    }
  }
  else if (PyLong_Check(pyobj)) {
    vallie = new EXP_IntValue((cInt)PyLong_AsLongLong(pyobj));
  }
  else if (PyUnicode_Check(pyobj)) {
    vallie = new EXP_StringValue(_PyUnicode_AsString(pyobj), "");
  }
  // Note, don't let these get assigned to GameObject props, must check elsewhere.
  else if (PyObject_TypeCheck(pyobj, &EXP_Value::Type)) {
    vallie = (static_cast<EXP_Value *>(EXP_PROXY_REF(pyobj)))->AddRef();
  }
  else {
    if (do_type_exception) {
      // Return an error value from the caller.
      PyErr_Format(
          PyExc_TypeError, "%scould convert python value to a game engine property", error_prefix);
    }
    vallie = nullptr;
  }
  return vallie;
}

PyObject *EXP_Value::ConvertKeysToPython(void)
{
  PyObject *pylist = PyList_New(m_properties.size());

  Py_ssize_t i = 0;
  for (const auto &pair : m_properties) {
    PyList_SET_ITEM(pylist, i++, PyUnicode_FromStdString(pair.first));
  }

  return pylist;
}

#endif  // WITH_PYTHON

EXP_Value *EXP_Value::Calc(VALUE_OPERATOR op, EXP_Value *val)
{
  return nullptr;
}

EXP_Value *EXP_Value::CalcFinal(VALUE_DATA_TYPE dtype, VALUE_OPERATOR op, EXP_Value *val)
{
  return nullptr;
}

void EXP_Value::SetValue(EXP_Value *newval)
{
  // No one should get here.
  BLI_assert(false);
}

std::string EXP_Value::GetText()
{
  return GetName();
}

double EXP_Value::GetNumber()
{
  return -1.0;
}

void EXP_Value::SetName(const std::string &name)
{
}

EXP_Value *EXP_Value::GetReplica()
{
  return nullptr;
}

bool EXP_Value::IsError() const
{
  return false;
}
