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
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file KX_Light.h
 *  \ingroup ketsji
 */

#pragma once

#include "KX_GameObject.h"

struct Light;
struct Object;

class KX_LightObject : public KX_GameObject {
  Py_Header

      protected :

      Object *m_obLight;
  Light *m_light;

 public:
  KX_LightObject();
  virtual ~KX_LightObject();

  virtual KX_PythonProxy *NewInstance();
  virtual void ProcessReplica();

  virtual int GetGameObjectType() const
  {
    return OBJ_LIGHT;
  }

  Light *GetLight();

  virtual void SetBlenderObject(Object *obj);

#ifdef WITH_PYTHON
  static PyObject *game_object_new(PyTypeObject *type, PyObject *args, PyObject *kwds);
#endif
};
