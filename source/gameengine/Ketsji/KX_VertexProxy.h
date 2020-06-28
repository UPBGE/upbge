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

/** \file KX_VertexProxy.h
 *  \ingroup ketsji
 */

#ifndef __KX_VERTEXPROXY_H__
#define __KX_VERTEXPROXY_H__

#ifdef WITH_PYTHON

#  include "EXP_Value.h"

class RAS_IVertex;
class RAS_IDisplayArray;

class KX_VertexProxy : public CValue {
  Py_Header

      protected : RAS_IVertex *m_vertex;
  RAS_IDisplayArray *m_array;

 public:
  KX_VertexProxy(RAS_IDisplayArray *array, RAS_IVertex *vertex);
  virtual ~KX_VertexProxy();

  RAS_IVertex *GetVertex();
  RAS_IDisplayArray *GetDisplayArray();

  // stuff for cvalue related things
  std::string GetName();

  static PyObject *pyattr_get_x(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_y(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_z(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_r(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_g(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_b(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_a(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_u(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_v(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_u2(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_v2(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_XYZ(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_UV(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_color(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_colors(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_normal(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_uvs(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
  static int pyattr_set_x(PyObjectPlus *self, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
  static int pyattr_set_y(PyObjectPlus *self, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
  static int pyattr_set_z(PyObjectPlus *self, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
  static int pyattr_set_u(PyObjectPlus *self, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
  static int pyattr_set_v(PyObjectPlus *self, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
  static int pyattr_set_u2(PyObjectPlus *self, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
  static int pyattr_set_v2(PyObjectPlus *self, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
  static int pyattr_set_r(PyObjectPlus *self, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
  static int pyattr_set_g(PyObjectPlus *self, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
  static int pyattr_set_b(PyObjectPlus *self, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
  static int pyattr_set_a(PyObjectPlus *self, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
  static int pyattr_set_XYZ(PyObjectPlus *self,
                            const KX_PYATTRIBUTE_DEF *attrdef,
                            PyObject *value);
  static int pyattr_set_UV(PyObjectPlus *self, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
  static int pyattr_set_color(PyObjectPlus *self,
                              const KX_PYATTRIBUTE_DEF *attrdef,
                              PyObject *value);
  static int pyattr_set_colors(PyObjectPlus *self,
                               const KX_PYATTRIBUTE_DEF *attrdef,
                               PyObject *value);
  static int pyattr_set_normal(PyObjectPlus *self,
                               const KX_PYATTRIBUTE_DEF *attrdef,
                               PyObject *value);
  static int pyattr_set_uvs(PyObjectPlus *self,
                            const KX_PYATTRIBUTE_DEF *attrdef,
                            PyObject *value);

  KX_PYMETHOD_NOARGS(KX_VertexProxy, GetXYZ);
  KX_PYMETHOD_O(KX_VertexProxy, SetXYZ);
  KX_PYMETHOD_NOARGS(KX_VertexProxy, GetUV1);
  KX_PYMETHOD_O(KX_VertexProxy, SetUV1);

  KX_PYMETHOD_NOARGS(KX_VertexProxy, GetUV2);
  KX_PYMETHOD_VARARGS(KX_VertexProxy, SetUV2);

  KX_PYMETHOD_NOARGS(KX_VertexProxy, GetRGBA);
  KX_PYMETHOD_O(KX_VertexProxy, SetRGBA);
  KX_PYMETHOD_NOARGS(KX_VertexProxy, GetNormal);
  KX_PYMETHOD_O(KX_VertexProxy, SetNormal);
};

#endif  // WITH_PYTHON

#endif  // __KX_VERTEXPROXY_H__
