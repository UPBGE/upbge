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
#pragma once

#include <vector>

#include "DetourStatNavMesh.h"
#include "EXP_PyObjectPlus.h"
#include "KX_GameObject.h"


class KX_NavMeshObject : public KX_GameObject {
  Py_Header

      protected : dtStatNavMesh *m_navMesh;

  bool BuildVertIndArrays(float *&vertices,
                          int &nverts,
                          unsigned short *&polys,
                          int &npolys,
                          unsigned short *&dmeshes,
                          float *&dvertices,
                          int &ndvertsuniq,
                          unsigned short *&dtris,
                          int &ndtris,
                          int &vertsPerPoly);

 public:
  KX_NavMeshObject();
  ~KX_NavMeshObject();

  virtual KX_PythonProxy *NewInstance();
  virtual void ProcessReplica();

  bool BuildNavMesh();
  dtStatNavMesh *GetNavMesh();
  int FindPath(const MT_Vector3 &from, const MT_Vector3 &to, float *path, int maxPathLen);
  float Raycast(const MT_Vector3 &from, const MT_Vector3 &to);

  enum NavMeshRenderMode { RM_WALLS, RM_POLYS, RM_TRIS, RM_MAX };
  void DrawNavMesh(NavMeshRenderMode mode);
  void DrawPath(const float *path, int pathLen, const MT_Vector4 &color);

  MT_Vector3 TransformToLocalCoords(const MT_Vector3 &wpos);
  MT_Vector3 TransformToWorldCoords(const MT_Vector3 &lpos);
#ifdef WITH_PYTHON
  /* --------------------------------------------------------------------- */
  /* Python interface ---------------------------------------------------- */
  /* --------------------------------------------------------------------- */

  static PyObject *game_object_new(PyTypeObject *type, PyObject *args, PyObject *kwds);

  EXP_PYMETHOD_DOC(KX_NavMeshObject, findPath);
  EXP_PYMETHOD_DOC(KX_NavMeshObject, raycast);
  EXP_PYMETHOD_DOC(KX_NavMeshObject, draw);
  EXP_PYMETHOD_DOC_NOARGS(KX_NavMeshObject, rebuild);
#endif /* WITH_PYTHON */
};
