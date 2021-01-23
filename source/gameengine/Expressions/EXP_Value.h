/*
 * Value.h: interface for the EXP_Value class.
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

/** \file EXP_Value.h
 *  \ingroup expressions
 */

#pragma once

#ifdef _MSC_VER
#  pragma warning(disable : 4786)
#endif

#include <map>     // Array functionality for the property list.
#include <string>  // std::string class.
#include <vector>

#include "CM_RefCount.h"

#ifndef GEN_NO_TRACE
#  undef trace
#  define trace(exp) ((void)nullptr)
#endif

enum VALUE_OPERATOR {
  VALUE_MOD_OPERATOR,  // %
  VALUE_ADD_OPERATOR,  // +
  VALUE_SUB_OPERATOR,  // -
  VALUE_MUL_OPERATOR,  // *
  VALUE_DIV_OPERATOR,  // /
  VALUE_NEG_OPERATOR,  // -
  VALUE_POS_OPERATOR,  // +
  VALUE_AND_OPERATOR,  // &&
  VALUE_OR_OPERATOR,   // ||
  VALUE_EQL_OPERATOR,  // ==
  VALUE_NEQ_OPERATOR,  // !=
  VALUE_GRE_OPERATOR,  // >
  VALUE_LES_OPERATOR,  // <
  VALUE_GEQ_OPERATOR,  // >=
  VALUE_LEQ_OPERATOR,  // <=
  VALUE_NOT_OPERATOR,  // !
  VALUE_NO_OPERATOR    // no operation at all
};

enum VALUE_DATA_TYPE {
  VALUE_NO_TYPE,  // Abstract baseclass.
  VALUE_INT_TYPE,
  VALUE_FLOAT_TYPE,
  VALUE_STRING_TYPE,
  VALUE_BOOL_TYPE,
  VALUE_ERROR_TYPE,
  VALUE_EMPTY_TYPE,
  VALUE_LIST_TYPE,
  VALUE_VOID_TYPE,
  VALUE_VECTOR_TYPE,
  VALUE_MAX_TYPE  // Only here to provide number of types.
};

#include "EXP_PyObjectPlus.h"
#ifdef WITH_PYTHON
#  include "object.h"
#endif

/**
 * Baseclass EXP_Value
 *
 * Together with EXP_Expression, EXP_Value and it's derived classes can be used to
 * parse expressions into a parsetree with error detecting/correcting capabilities
 * also expandable by a CFactory pluginsystem
 *
 * Base class for all editor functionality, flexible object type that allows
 * calculations and uses reference counting for memory management.
 *
 * Features:
 * - Calculations (Calc() / CalcFinal())
 * - Property system (SetProperty() / GetProperty() / FindIdentifier())
 * - Replication (GetReplica())
 * - Flags (IsError())
 *
 * - Some small editor-specific things added
 * - A helperclass CompressorArchive handles the serialization
 *
 */
class EXP_Value : public EXP_PyObjectPlus, public CM_RefCount<EXP_Value> {
  Py_Header public : EXP_Value();
  virtual ~EXP_Value();

#ifdef WITH_PYTHON
  virtual PyObject *py_repr(void)
  {
    return PyUnicode_FromStdString(GetText());
  }

  virtual PyObject *ConvertValueToPython()
  {
    return nullptr;
  }

  virtual EXP_Value *ConvertPythonToValue(PyObject *pyobj,
                                          const bool do_type_exception,
                                          const char *error_prefix);

  static PyObject *pyattr_get_name(EXP_PyObjectPlus *self, const EXP_PYATTRIBUTE_DEF *attrdef);

  virtual PyObject *ConvertKeysToPython(void);
#endif  // WITH_PYTHON

  /// Expression Calculation
  virtual EXP_Value *Calc(VALUE_OPERATOR op, EXP_Value *val);
  virtual EXP_Value *CalcFinal(VALUE_DATA_TYPE dtype, VALUE_OPERATOR op, EXP_Value *val);

  /// Property Management
  /// Set property <ioProperty>, overwrites and releases a previous property with the same name if
  /// needed.
  virtual void SetProperty(const std::string &name, EXP_Value *ioProperty);
  virtual EXP_Value *GetProperty(const std::string &inName);
  /// Get text description of property with name <inName>, returns an empty string if there is no
  /// property named <inName>.
  const std::string GetPropertyText(const std::string &inName);
  float GetPropertyNumber(const std::string &inName, float defnumber);
  /// Remove the property named <inName>, returns true if the property was succesfully removed,
  /// false if property was not found or could not be removed.
  virtual bool RemoveProperty(const std::string &inName);
  virtual std::vector<std::string> GetPropertyNames();
  /// Clear all properties.
  virtual void ClearProperties();

  /// Get property number <inIndex>.
  virtual EXP_Value *GetProperty(int inIndex);
  /// Get the amount of properties assiocated with this value.
  virtual int GetPropertyCount();

  virtual EXP_Value *FindIdentifier(const std::string &identifiername);

  virtual std::string GetText();
  virtual double GetNumber();
  /// Get Prop value type.
  virtual int GetValueType();

  /// Retrieve the name of the value.
  virtual std::string GetName() = 0;
  /// Set the name of the value.
  virtual void SetName(const std::string &name);
  /** Sets the value to this cvalue.
   * \attention this particular function should never be called. Why not abstract?
   */
  virtual void SetValue(EXP_Value *newval);
  virtual EXP_Value *GetReplica();
  virtual void ProcessReplica();

  std::string op2str(VALUE_OPERATOR op);

  virtual bool IsError() const;

 protected:
  virtual void DestructFromPython();

 private:
  /// Properties for user/game etc.
  std::map<std::string, EXP_Value *> m_properties;
};

/** EXP_PropValue is a EXP_Value derived class, that implements the identification (String name)
 * SetName() / GetName(),
 * normal classes should derive from EXP_PropValue, real lightweight classes straight from
 * EXP_Value
 */
class EXP_PropValue : public EXP_Value {
 public:
  EXP_PropValue()
  {
  }

  virtual ~EXP_PropValue()
  {
  }

  virtual void SetName(const std::string &name)
  {
    m_strNewName = name;
  }

  virtual std::string GetName()
  {
    return m_strNewName;
  }

 protected:
  std::string m_strNewName;
};
