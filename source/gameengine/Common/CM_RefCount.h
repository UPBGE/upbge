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
 * Contributor(s): Tristan Porteries.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file CM_RefCount.h
 *  \ingroup common
 */

#pragma once

#include "BLI_utildefines.h"

/** \brief Reference counter base class. This class manages the destruction of an object
 * based on a reference counter, when the counter is to zero the object is destructed.
 */
template<class T> class CM_RefCount {
 private:
  int m_refCount;

 public:
  CM_RefCount() : m_refCount(1)
  {
  }

  virtual ~CM_RefCount()
  {
  }

  CM_RefCount(const CM_RefCount &/*other*/)
  {
    m_refCount = 1;
  }

  /// Increase the reference count of the object.
  T *AddRef()
  {
    BLI_assert(m_refCount > 0);
    ++m_refCount;

    return static_cast<T *>(this);
  }

  /// Decrease the reference count of the object and destruct at zero.
  T *Release()
  {
    BLI_assert(m_refCount > 0);
    --m_refCount;
    if (m_refCount == 0) {
      delete this;
      return nullptr;
    }

    return static_cast<T *>(this);
  }

  int GetRefCount() const
  {
    return m_refCount;
  }
};

/** Increase the reference count of a object. Used in case of multiple levels
 * inheritance in the goal to return the value back.
 */
template<class T> T *CM_AddRef(T *val)
{
  return static_cast<T *>(val->AddRef());
}

/** Decrease the reference count of a object and destruct at zero. Used in case
 * of multiple levels inheritance in the goal to return the value back.
 */
template<class T> T *CM_Release(T *val)
{
  return static_cast<T *>(val->Release());
}
