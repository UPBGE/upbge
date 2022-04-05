/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2007 The Zdeno Ash Miklas. */

/** \file gameengine/VideoTexture/PyTypeList.cpp
 *  \ingroup bgevideotex
 */

#include "PyTypeList.h"

/// destructor
PyTypeList::~PyTypeList()
{
  // if list exists
  if (m_list.get() != nullptr)
    for (PyTypeListType::iterator it = m_list->begin(); it != m_list->end(); ++it)
      delete *it;
}

/// check, if type is in list
bool PyTypeList::in(PyTypeObject *type)
{
  // if list exists
  if (m_list.get() != nullptr)
    // iterate items in list
    for (PyTypeListType::iterator it = m_list->begin(); it != m_list->end(); ++it)
      // if item is found, return with success
      if ((*it)->getType() == type)
        return true;
  // otherwise return not found
  return false;
}

/// add type to list
void PyTypeList::add(PyTypeObject *type, const char *name)
{
  // if list doesn't exist, create it
  if (m_list.get() == nullptr)
    m_list.reset(new PyTypeListType());
  if (!in(type))
    // add new item to list
    m_list->push_back(new PyTypeListItem(type, name));
}

/// prepare types
bool PyTypeList::ready(void)
{
  // if list exists
  if (m_list.get() != nullptr)
    // iterate items in list
    for (PyTypeListType::iterator it = m_list->begin(); it != m_list->end(); ++it)
      // if preparation failed, report it
      if (PyType_Ready((*it)->getType()) < 0)
        return false;
  // success
  return true;
}

/// register types to module
void PyTypeList::reg(PyObject *module)
{
  // if list exists
  if (m_list.get() != nullptr)
    // iterate items in list
    for (PyTypeListType::iterator it = m_list->begin(); it != m_list->end(); ++it) {
      // increase ref count
      Py_INCREF((*it)->getType());
      // add type to module
      PyModule_AddObject(module, (*it)->getName(), (PyObject *)(*it)->getType());
    }
}
