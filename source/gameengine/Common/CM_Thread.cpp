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

/** \file gameengine/Common/CM_Thread.cpp
 *  \ingroup common
 */

#include "CM_Thread.h"

CM_ThreadLock::CM_ThreadLock()
{
}

CM_ThreadLock::~CM_ThreadLock()
{
}

CM_ThreadSpinLock::CM_ThreadSpinLock()
{
  BLI_spin_init(&m_spinlock);
}

CM_ThreadSpinLock::~CM_ThreadSpinLock()
{
  BLI_spin_end(&m_spinlock);
}

void CM_ThreadSpinLock::Lock()
{
  BLI_spin_lock(&m_spinlock);
}

void CM_ThreadSpinLock::Unlock()
{
  BLI_spin_unlock(&m_spinlock);
}

CM_ThreadMutex::CM_ThreadMutex()
{
  BLI_mutex_init(&m_mutex);
}

CM_ThreadMutex::~CM_ThreadMutex()
{
  BLI_mutex_end(&m_mutex);
}

void CM_ThreadMutex::Lock()
{
  BLI_mutex_lock(&m_mutex);
}

void CM_ThreadMutex::Unlock()
{
  BLI_mutex_unlock(&m_mutex);
}
