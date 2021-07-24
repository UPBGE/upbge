/**
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
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

#pragma once

#include "EXP_Value.h"

struct PythonProxy;

class KX_PythonProxy : public EXP_Value {

 private:
  bool m_init;

  PythonProxy *m_pp;

  PyObject *m_update;

  PyObject *m_dispose;

 public:
  KX_PythonProxy();

  virtual ~KX_PythonProxy();

  std::string GetName();

  PythonProxy *GetPrototype();

  void SetPrototype(PythonProxy *pp);

  virtual void Start();

  virtual void Update();

  virtual void Dispose();

  virtual KX_PythonProxy *NewInstance() = 0;

  virtual KX_PythonProxy *GetReplica();

  virtual void ProcessReplica();

  void Reset();
};
