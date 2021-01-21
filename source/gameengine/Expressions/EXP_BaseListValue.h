/*
 * ListValue.h: interface for the EXP_BaseListValue class.
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

/** \file EXP_BaseListValue.h
 *  \ingroup expressions
 */

#pragma once

#include "EXP_Value.h"

class EXP_BaseListValue : public EXP_PropValue {
  Py_Header

      public : typedef std::vector<EXP_Value *>
                   VectorType;
  typedef VectorType::iterator VectorTypeIterator;
  typedef VectorType::const_iterator VectorTypeConstIterator;

 protected:
  VectorType m_pValueArray;
  bool m_bReleaseContents;

  void SetValue(int i, EXP_Value *val);
  EXP_Value *GetValue(int i);
  EXP_Value *FindValue(const std::string &name) const;
  bool SearchValue(EXP_Value *val) const;
  void Add(EXP_Value *value);
  void Insert(unsigned int i, EXP_Value *value);
  bool RemoveValue(EXP_Value *val);
  bool CheckEqual(EXP_Value *first, EXP_Value *second);

 public:
  EXP_BaseListValue();
  virtual ~EXP_BaseListValue();

  virtual int GetValueType();
  virtual EXP_Value *GetReplica() = 0;
  virtual std::string GetText();

  void SetReleaseOnDestruct(bool bReleaseContents);

  void Remove(int i);
  void Resize(int num);
  void ReleaseAndRemoveAll();
  int GetCount() const;

#ifdef WITH_PYTHON

  EXP_PYMETHOD_O(EXP_BaseListValue, append);
  EXP_PYMETHOD_NOARGS(EXP_BaseListValue, reverse);
  EXP_PYMETHOD_O(EXP_BaseListValue, index);
  EXP_PYMETHOD_O(EXP_BaseListValue, count);
  EXP_PYMETHOD_VARARGS(EXP_BaseListValue, get);
  EXP_PYMETHOD_VARARGS(EXP_BaseListValue, filter);
  EXP_PYMETHOD_O(EXP_BaseListValue, from_id);

  static Py_ssize_t bufferlen(PyObject *self);
  static PyObject *buffer_item(PyObject *self, Py_ssize_t index);
  static PyObject *buffer_slice(EXP_BaseListValue *list, Py_ssize_t start, Py_ssize_t stop);
  static PyObject *mapping_subscript(PyObject *self, PyObject *key);
  static PyObject *buffer_concat(PyObject *self, PyObject *other);
  static int buffer_contains(PyObject *self_v, PyObject *value);

  static PySequenceMethods as_sequence;
  static PyMappingMethods instance_as_mapping;
#endif
};
