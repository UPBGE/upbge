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

/** \file CM_Thread.h
 *  \ingroup common
 */

#pragma once

#include "BLI_threads.h"

class CM_ThreadLock {
 public:
  CM_ThreadLock();
  virtual ~CM_ThreadLock();

  virtual void Lock() = 0;
  virtual void Unlock() = 0;
};

class CM_ThreadSpinLock : public CM_ThreadLock {
 public:
  CM_ThreadSpinLock();
  virtual ~CM_ThreadSpinLock();

  virtual void Lock();
  virtual void Unlock();

 private:
  SpinLock m_spinlock;
};

class CM_ThreadMutex : public CM_ThreadLock {
 public:
  CM_ThreadMutex();
  virtual ~CM_ThreadMutex();

  virtual void Lock();
  virtual void Unlock();

 private:
  ThreadMutex m_mutex;
};
