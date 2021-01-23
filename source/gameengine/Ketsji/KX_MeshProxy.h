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

/** \file KX_MeshProxy.h
 *  \ingroup ketsji
 */

#pragma once

#ifdef WITH_PYTHON

#  include "EXP_Value.h"

class RAS_MeshObject;
class SCA_LogicManager;

// utility conversion function
bool ConvertPythonToMesh(SCA_LogicManager *logicmgr,
                         PyObject *value,
                         RAS_MeshObject **object,
                         bool py_none_ok,
                         const char *error_prefix);

class KX_MeshProxy : public EXP_Value {
  Py_Header

      RAS_MeshObject *m_meshobj;

 public:
  KX_MeshProxy(RAS_MeshObject *mesh);
  virtual ~KX_MeshProxy();

  virtual RAS_MeshObject *GetMesh()
  {
    return m_meshobj;
  }

  // stuff for cvalue related things
  virtual std::string GetName();

  // stuff for python integration

  EXP_PYMETHOD(KX_MeshProxy, GetNumMaterials);  // Deprecated
  EXP_PYMETHOD(KX_MeshProxy, GetMaterialName);
  EXP_PYMETHOD(KX_MeshProxy, GetTextureName);
  EXP_PYMETHOD_NOARGS(KX_MeshProxy, GetNumPolygons);  // Deprecated

  // both take materialid (int)
  EXP_PYMETHOD(KX_MeshProxy, GetVertexArrayLength);
  EXP_PYMETHOD(KX_MeshProxy, GetVertex);
  EXP_PYMETHOD(KX_MeshProxy, GetPolygon);
  EXP_PYMETHOD(KX_MeshProxy, Transform);
  EXP_PYMETHOD(KX_MeshProxy, TransformUV);
  EXP_PYMETHOD(KX_MeshProxy, ReplaceMaterial);

  static PyObject *pyattr_get_materials(EXP_PyObjectPlus *self_v,
                                        const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_numMaterials(EXP_PyObjectPlus *self_v,
                                           const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_numPolygons(EXP_PyObjectPlus *self_v,
                                          const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_polygons(EXP_PyObjectPlus *self_v,
                                       const EXP_PYATTRIBUTE_DEF *attrdef);
};

#endif  // WITH_PYTHON
