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

/** \file KX_PyMath.h
 *  \ingroup ketsji
 *  \brief Initialize Python thingies.
 */

#pragma once

#include "EXP_PyObjectPlus.h"
#include "EXP_Python.h"
#include "MT_Matrix3x3.h"
#include "MT_Matrix4x4.h"
#include "MT_Vector2.h"
#include "MT_Vector3.h"
#include "MT_Vector4.h"

#ifdef WITH_PYTHON
#  ifdef USE_MATHUTILS
#    include "../../blender/python/mathutils/mathutils.hh" /* so we can have mathutils callbacks */
using blender::BaseMathObject;
using blender::MatrixObject;
using blender::VectorObject;
using blender::QuaternionObject;
using blender::EulerObject;
#  endif

inline unsigned int Size(const MT_Matrix4x4 &)
{
  return 4;
}
inline unsigned int Size(const MT_Matrix3x3 &)
{
  return 3;
}
inline unsigned int Size(const MT_Vector2 &)
{
  return 2;
}
inline unsigned int Size(const MT_Vector3 &)
{
  return 3;
}
inline unsigned int Size(const MT_Vector4 &)
{
  return 4;
}

/**
 *  Converts the given python matrix (column-major) to an MT class (row-major).
 */
template<class T> bool PyMatTo(PyObject *pymat, T &mat)
{
  bool noerror = true;
  mat.setIdentity();

#  ifdef USE_MATHUTILS

  if (PyObject_TypeCheck(pymat, &blender::matrix_Type)) {
    blender::MatrixObject *pymatrix = (blender::MatrixObject *)pymat;

    if (BaseMath_ReadCallback(pymatrix) == -1)
      return false;

    if (pymatrix->col_num != Size(mat) || pymatrix->row_num != Size(mat))
      return false;

    for (unsigned int row = 0; row < Size(mat); row++) {
      for (unsigned int col = 0; col < Size(mat); col++) {
        mat[row][col] = *(pymatrix->matrix + col * pymatrix->row_num + row);
      }
    }
  }
  else

#  endif /* USE_MATHUTILS */

      if (PySequence_Check(pymat)) {
    /* Use PySequence_Fast for the outer sequence to avoid sq_item on buffer-view types
     * (Python 3.13 may assert-crash when calling sq_item on a MatrixObject with active
     * buffer view). */
    PyObject *seq = PySequence_Fast(pymat, "could not be converted to a matrix");
    if (!seq) {
      return false;
    }
    unsigned int rows = (unsigned int)PySequence_Fast_GET_SIZE(seq);
    if (rows != Size(mat)) {
      Py_DECREF(seq);
      return false;
    }

    PyObject **row_items = PySequence_Fast_ITEMS(seq);
    for (unsigned int row = 0; noerror && row < rows; row++) {
      PyObject *pyrow = row_items[row]; /* borrowed */
      if (!PyErr_Occurred() && PySequence_Check(pyrow)) {
        PyObject *rowseq = PySequence_Fast(pyrow, "row could not be converted");
        if (!rowseq) {
          noerror = false;
          break;
        }
        unsigned int cols = (unsigned int)PySequence_Fast_GET_SIZE(rowseq);
        if (cols != Size(mat)) {
          noerror = false;
          Py_DECREF(rowseq);
        }
        else {
          PyObject **col_items = PySequence_Fast_ITEMS(rowseq);
          for (unsigned int col = 0; col < cols; col++) {
            mat[row][col] = PyFloat_AsDouble(col_items[col]);
          }
          Py_DECREF(rowseq);
        }
      }
      else {
        noerror = false;
      }
    }
    Py_DECREF(seq);
  }
  else
    noerror = false;

  if (noerror == false)
    PyErr_SetString(PyExc_TypeError, "could not be converted to a matrix (sequence of sequences)");

  return noerror;
}

/**
 *  Converts a python sequence to a MT class.
 */
template<class T> bool PyVecTo(PyObject *pyval, T &vec)
{
#  ifdef USE_MATHUTILS
  /* no need for BaseMath_ReadCallback() here, reading the sequences will do this */

  if (PyObject_TypeCheck(pyval, &blender::vector_Type)) {
    blender::VectorObject *pyvec = (blender::VectorObject *)pyval;
    if (BaseMath_ReadCallback(pyvec) == -1) {
      return false; /* exception raised */
    }
    if (pyvec->vec_num != Size(vec)) {
      PyErr_Format(PyExc_AttributeError,
                   "error setting vector, %d args, should be %d",
                   pyvec->vec_num,
                   Size(vec));
      return false;
    }
    vec.setValue((float *)pyvec->vec);
    return true;
  }
  else if (PyObject_TypeCheck(pyval, &blender::quaternion_Type)) {
    blender::QuaternionObject *pyquat = (blender::QuaternionObject *)pyval;
    if (BaseMath_ReadCallback(pyquat) == -1) {
      return false; /* exception raised */
    }
    if (4 != Size(vec)) {
      PyErr_Format(
          PyExc_AttributeError, "error setting vector, %d args, should be %d", 4, Size(vec));
      return false;
    }
    /* xyzw -> wxyz reordering is done by PyQuatTo */
    vec.setValue((float *)pyquat->quat);
    return true;
  }
  else if (PyObject_TypeCheck(pyval, &blender::euler_Type)) {
    blender::EulerObject *pyeul = (blender::EulerObject *)pyval;
    if (BaseMath_ReadCallback(pyeul) == -1) {
      return false; /* exception raised */
    }
    if (3 != Size(vec)) {
      PyErr_Format(
          PyExc_AttributeError, "error setting vector, %d args, should be %d", 3, Size(vec));
      return false;
    }
    vec.setValue((float *)pyeul->eul);
    return true;
  }
  else if (PyObject_TypeCheck(pyval, &blender::matrix_Type)) {
    /* A matrix is a sequence of row-vectors, not a sequence of floats.
     * Reject it explicitly so it doesn't fall through to the generic
     * PySequence_Check branch below. In Python 3.13, PySequence_GetItem on a
     * MatrixObject with an active buffer view triggers an internal assertion crash. */
    PyErr_Format(PyExc_AttributeError,
                 "error setting vector, a matrix is not a valid sequence of numbers");
    return false;
  }
  else
#  endif
      if (PyTuple_Check(pyval)) {
    unsigned int numitems = PyTuple_GET_SIZE(pyval);
    if (numitems != Size(vec)) {
      PyErr_Format(PyExc_AttributeError,
                   "error setting vector, %d args, should be %d",
                   numitems,
                   Size(vec));
      return false;
    }

    for (unsigned int x = 0; x < numitems; x++)
      vec[x] = PyFloat_AsDouble(PyTuple_GET_ITEM(pyval, x)); /* borrow ref */

    if (PyErr_Occurred()) {
      PyErr_SetString(PyExc_AttributeError,
                      "one or more of the items in the sequence was not a float");
      return false;
    }

    return true;
  }
  else if (PyObject_TypeCheck(pyval, (PyTypeObject *)&EXP_PyObjectPlus::Type)) {
    /* note, include this check because PySequence_Check does too much introspection
     * on the PyObject (like getting its __class__, on a BGE type this means searching up
     * the parent list each time only to discover its not a sequence.
     * GameObjects are often used as an alternative to vectors so this is a common case
     * better to do a quick check for it, likely the error below will be ignored.
     *
     * This is not 'correct' since we have proxy type CListValues's which could
     * contain floats/ints but there no cases of CValueLists being this way
     */
    PyErr_Format(PyExc_AttributeError, "expected a sequence type");
    return false;
  }
  else if (PySequence_Check(pyval)) {
    /* Use PySequence_Fast to avoid calling sq_item on types like MatrixObject that may
     * have an active buffer view — in Python 3.13 this triggers an internal assertion. */
    PyObject *seq = PySequence_Fast(pyval, "error converting to vector");
    if (!seq) {
      return false;
    }
    unsigned int numitems = (unsigned int)PySequence_Fast_GET_SIZE(seq);
    if (numitems != Size(vec)) {
      PyErr_Format(PyExc_AttributeError,
                   "error setting vector, %d args, should be %d",
                   numitems,
                   Size(vec));
      Py_DECREF(seq);
      return false;
    }

    PyObject **items = PySequence_Fast_ITEMS(seq);
    for (unsigned int x = 0; x < numitems; x++) {
      vec[x] = PyFloat_AsDouble(items[x]);
    }
    Py_DECREF(seq);

    if (PyErr_Occurred()) {
      PyErr_SetString(PyExc_AttributeError,
                      "one or more of the items in the sequence was not a float");
      return false;
    }

    return true;
  }
  else {
    PyErr_Format(PyExc_AttributeError,
                 "not a sequence type, expected a sequence of numbers size %d",
                 Size(vec));
  }

  return false;
}

bool PyQuatTo(PyObject *pyval, MT_Quaternion &qrot);

bool PyOrientationTo(PyObject *pyval, MT_Matrix3x3 &mat, const char *error_prefix);

/**
 * Converts an MT_Matrix4x4 to a python object.
 */
PyObject *PyObjectFrom(const MT_Matrix4x4 &mat);

/**
 * Converts an MT_Matrix3x3 to a python object.
 */
PyObject *PyObjectFrom(const MT_Matrix3x3 &mat);

/**
 * Converts an MT_Vector2 to a python object.
 */
PyObject *PyObjectFrom(const MT_Vector2 &vec);

/**
 * Converts an MT_Vector3 to a python object
 */
PyObject *PyObjectFrom(const MT_Vector3 &vec);

#  ifdef USE_MATHUTILS
/**
 * Converts an MT_Quaternion to a python object.
 */
PyObject *PyObjectFrom(const MT_Quaternion &qrot);
#  endif

/**
 * Converts an MT_Vector4 to a python object.
 */
PyObject *PyObjectFrom(const MT_Vector4 &pos);

/**
 * Converts an MT_Vector3 to a python color object.
 */
PyObject *PyColorFromVector(const MT_Vector3 &vec);

#endif  // WITH_PYTHON
