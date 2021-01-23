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
 * Contributor(s): Ulysse Martin, Tristan Porteries.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file KX_LodLevel.h
 *  \ingroup ketsji
 */

#pragma once

#include "EXP_PyObjectPlus.h"
#include "RAS_MeshObject.h"

class KX_LodLevel : public EXP_PyObjectPlus {
  Py_Header private : float m_distance;
  float m_hysteresis;
  short m_level;
  unsigned short m_flags;
  RAS_MeshObject *m_meshobj;
  struct Object *m_object;

 public:
  KX_LodLevel(float distance,
              float hysteresis,
              unsigned short level,
              RAS_MeshObject *meshobj,
              struct Object *object,
              unsigned short flag);
  virtual ~KX_LodLevel();

  float GetDistance() const;
  float GetHysteresis() const;
  unsigned short GetLevel() const;
  unsigned short GetFlag() const;
  RAS_MeshObject *GetMesh() const;
  struct Object *GetObject();

  enum {
    /// Use custom hysteresis for this level.
    USE_HYSTERESIS = (1 << 0),
    /// Use a different mesh than original.
    USE_MESH = (1 << 1),
    /// Use a different material than original mesh.
    USE_MATERIAL = (1 << 2)
  };

#ifdef WITH_PYTHON

  virtual PyObject *py_repr()
  {
    return PyUnicode_FromStdString(m_meshobj->GetName());
  }

  static PyObject *pyattr_get_mesh(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_use_hysteresis(EXP_PyObjectPlus *self_v,
                                             const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_use_mesh(EXP_PyObjectPlus *self_v,
                                       const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_use_material(EXP_PyObjectPlus *self_v,
                                           const EXP_PYATTRIBUTE_DEF *attrdef);

#endif  // WITH_PYTHON
};
