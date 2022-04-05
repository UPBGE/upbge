/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2007 The Zdeno Ash Miklas. */

/** \file PyTypeList.h
 *  \ingroup bgevideotex
 */

#pragma once

#include <memory>
#include <vector>

#include "Common.h"
#include "EXP_PyObjectPlus.h"

// forward declaration
class PyTypeListItem;

// type for list of types
typedef std::vector<PyTypeListItem *> PyTypeListType;

/// class to store list of python types
class PyTypeList {
 public:
  /// destructor
  ~PyTypeList();

  /// check, if type is in list
  bool in(PyTypeObject *type);

  /// add type to list
  void add(PyTypeObject *type, const char *name);

  /// prepare types
  bool ready(void);

  /// register types to module
  void reg(PyObject *module);

 protected:
  /// pointer to list of types
  //#if (__cplusplus > 199711L) /* || (defined(_MSC_VER) && _MSC_VER >= 1800) */
  std::unique_ptr<PyTypeListType> m_list;
  //#else
  //  std::auto_ptr<PyTypeListType> m_list;
  //#endif
};

/// class for item of python type list
class PyTypeListItem {
 public:
  /// constructor adds type into list
  PyTypeListItem(PyTypeObject *type, const char *name) : m_type(type), m_name(name)
  {
  }

  /// does type match
  PyTypeObject *getType(void)
  {
    return m_type;
  }

  /// get name of type
  const char *getName(void)
  {
    return m_name;
  }

 protected:
  /// pointer to type object
  PyTypeObject *m_type;
  /// name of type
  const char *m_name;
};
