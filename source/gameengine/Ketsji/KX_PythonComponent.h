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

#ifdef WITH_PYTHON

#  include "KX_PythonProxy.h"

class KX_GameObject;

class KX_PythonComponent : public KX_PythonProxy {
  Py_Header

      private : KX_GameObject *m_gameobj;
  std::string m_name;

 public:
  KX_PythonComponent(const std::string &name);
  virtual ~KX_PythonComponent();

  // stuff for cvalue related things
  virtual std::string GetName();

  void ProcessReplica();

  KX_GameObject *GetGameObject() const;
  void SetGameObject(KX_GameObject *gameobj);

  virtual KX_PythonProxy *NewInstance();

  static PyObject *py_component_new(PyTypeObject *type, PyObject *args, PyObject *kwds);

  // Attributes
  static PyObject *pyattr_get_object(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
};

#endif  // WITH_PYTHON
