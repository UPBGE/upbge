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

#ifndef __KX_PYMATH_H__
#define __KX_PYMATH_H__

#include "mathfu.h"

#include "EXP_Python.h"
#include "EXP_PyObjectPlus.h"

#ifdef WITH_PYTHON
#ifdef USE_MATHUTILS
extern "C" {
#include "../../blender/python/mathutils/mathutils.h" /* so we can have mathutils callbacks */
}
#endif

inline unsigned int Size(const mt::mat4&)          { return 4; }
inline unsigned int Size(const mt::mat3&)          { return 3; }
inline unsigned int Size(const mt::vec2&)                { return 2; }
inline unsigned int Size(const mt::vec3&)                { return 3; }
inline unsigned int Size(const mt::vec4&)                { return 4; }
inline unsigned int Size(const mt::quat&)                { return 4; }
inline unsigned int Size(const mt::vec2_packed&) { return 2; }
inline unsigned int Size(const mt::vec3_packed&) { return 3; }
inline unsigned int Size(const mt::vec4_packed&) { return 4; }

/**
 *  Converts the given python matrix (column-major) to an MT class (row-major).
 */
template<class T>
bool PyMatTo(PyObject *pymat, T& mat)
{
	bool noerror = true;

#ifdef USE_MATHUTILS

	if (MatrixObject_Check(pymat))
	{
		MatrixObject *pymatrix = (MatrixObject *)pymat;

		if (BaseMath_ReadCallback(pymatrix) == -1)
			return false;

		if (pymatrix->num_col != Size(mat) || pymatrix->num_row != Size(mat))
			return false;

		for (unsigned int row = 0; row < Size(mat); row++)
		{
			for (unsigned int col = 0; col < Size(mat); col++)
			{
				mat(row, col) = *(pymatrix->matrix + col * pymatrix->num_row + row);
			}
		}
	}
	else

#endif /* USE_MATHUTILS */


	if (PySequence_Check(pymat))
	{
		unsigned int rows = PySequence_Size(pymat);
		if (rows != Size(mat))
			return false;
			
		for (unsigned int row = 0; noerror && row < rows; row++)
		{
			PyObject *pyrow = PySequence_GetItem(pymat, row); /* new ref */
			if (!PyErr_Occurred() && PySequence_Check(pyrow))
			{
				unsigned int cols = PySequence_Size(pyrow);
				if (cols != Size(mat)) {
					noerror = false;
				}
				else {
					for (unsigned int col = 0; col < cols; col++) {
						PyObject *item = PySequence_GetItem(pyrow, col); /* new ref */
						mat(row, col) = PyFloat_AsDouble(item);
						Py_DECREF(item);
					}
				}
			}
			else {
				noerror = false;
			}
			Py_DECREF(pyrow);
		}
	} else 
		noerror = false;
	
	if (noerror==false)
		PyErr_SetString(PyExc_TypeError, "could not be converted to a matrix (sequence of sequences)");
	
	return noerror;
}

/**
 *  Converts a python sequence to a MT class.
 */
template<class T>
bool PyVecTo(PyObject *pyval, T& vec)
{
#ifdef USE_MATHUTILS
	/* no need for BaseMath_ReadCallback() here, reading the sequences will do this */
	
	if (VectorObject_Check(pyval)) {
		VectorObject *pyvec= (VectorObject *)pyval;
		if (BaseMath_ReadCallback(pyvec) == -1) {
			return false; /* exception raised */
		}
		if (pyvec->size != Size(vec)) {
			PyErr_Format(PyExc_AttributeError, "error setting vector, %d args, should be %d", pyvec->size, Size(vec));
			return false;
		}
		vec = T(pyvec->vec);
		return true;
	}
	else if (QuaternionObject_Check(pyval)) {
		QuaternionObject *pyquat= (QuaternionObject *)pyval;
		if (BaseMath_ReadCallback(pyquat) == -1) {
			return false; /* exception raised */
		}
		if (4 != Size(vec)) {
			PyErr_Format(PyExc_AttributeError, "error setting vector, %d args, should be %d", 4, Size(vec));
			return false;
		}
		/* xyzw -> wxyz reordering is done by PyQuatTo */
		vec = T(pyquat->quat);
		return true;
	}
	else if (EulerObject_Check(pyval)) {
		EulerObject *pyeul= (EulerObject *)pyval;
		if (BaseMath_ReadCallback(pyeul) == -1) {
			return false; /* exception raised */
		}
		if (3 != Size(vec)) {
			PyErr_Format(PyExc_AttributeError, "error setting vector, %d args, should be %d", 3, Size(vec));
			return false;
		}
		vec = T(pyeul->eul);
		return true;
	}
	else
#endif
	if (PyTuple_Check(pyval)) {
		unsigned int numitems = PyTuple_GET_SIZE(pyval);
		if (numitems != Size(vec)) {
			PyErr_Format(PyExc_AttributeError, "error setting vector, %d args, should be %d", numitems, Size(vec));
			return false;
		}
		
		for (unsigned int x = 0; x < numitems; x++)
			vec[x] = PyFloat_AsDouble(PyTuple_GET_ITEM(pyval, x)); /* borrow ref */
		
		if (PyErr_Occurred()) {
			PyErr_SetString(PyExc_AttributeError, "one or more of the items in the sequence was not a float");
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
		 * This is not 'correct' since we have proxy type EXP_ListValues's which could
		 * contain floats/ints but there no cases of EXP_ValueLists being this way
		 */
		PyErr_Format(PyExc_AttributeError, "expected a sequence type");
		return false;
	}
	else if (PySequence_Check(pyval)) {
		unsigned int numitems = PySequence_Size(pyval);
		if (numitems != Size(vec)) {
			PyErr_Format(PyExc_AttributeError, "error setting vector, %d args, should be %d", numitems, Size(vec));
			return false;
		}
		
		for (unsigned int x = 0; x < numitems; x++) {
			PyObject *item = PySequence_GetItem(pyval, x); /* new ref */
			vec[x] = PyFloat_AsDouble(item);
			Py_DECREF(item);
		}
		
		if (PyErr_Occurred()) {
			PyErr_SetString(PyExc_AttributeError, "one or more of the items in the sequence was not a float");
			return false;
		}
		
		return true;
	}
	else {
		PyErr_Format(PyExc_AttributeError, "not a sequence type, expected a sequence of numbers size %d", Size(vec));
	}
	
	return false;
}


bool PyQuatTo(PyObject *pyval, mt::quat &qrot);

bool PyOrientationTo(PyObject *pyval, mt::mat3 &mat, const char *error_prefix);

/// Converts an mt::matX to a python object.
template <int Rows, int Cols>
PyObject *PyObjectFrom(const mt::Matrix<float, Rows, Cols> &mat)
{
#ifdef USE_MATHUTILS
	float fmat[Rows * Cols];
	mat.Pack(fmat);
	return Matrix_CreatePyObject(fmat, Rows, Cols, nullptr);
#else
	PyObject *list = PyList_New(Rows);

	for (unsigned short i = 0; i < Rows; ++i) {
		PyObject *row = PyList_New(Cols);
		for (unsigned short j = 0; j < Cols; ++j) {
			PyList_SET_ITEM(row, j, PyFloat_FromDouble(mat[i][j]));
		}
		PyList_SET_ITEM(list, i, row);
	}

	return list;
#endif
}

#ifdef USE_MATHUTILS
/// Converts an mt::quat to a python object.
PyObject *PyObjectFrom(const mt::quat &qrot);
#endif

/// Converts an mt::vecX to a python object.
template <int Size>
PyObject *PyObjectFrom(const mt::Vector<float, Size> &vec)
{
#ifdef USE_MATHUTILS
	return Vector_CreatePyObject(vec.Data(), Size, nullptr);
#else
	PyObject *list = PyList_New(Size);
	for (unsigned short i = 0; i < Size; ++i) {
		PyList_SET_ITEM(list, i, PyFloat_FromDouble(vec[i]));
	}
	return list;
#endif
}

/// Converts an mt::vecX_packed to a python object.
template <int Size>
PyObject *PyObjectFrom(const mt::VectorPacked<float, Size> &vec)
{
#ifdef USE_MATHUTILS
	return Vector_CreatePyObject(vec.data, Size, nullptr);
#else
	PyObject *list = PyList_New(Size);
	for (unsigned short i = 0; i < Size; ++i) {
		PyList_SET_ITEM(list, i, PyFloat_FromDouble(vec.data[i]));
	}
	return list;
#endif
}

/// Converts an mt::vec3 to a python color object.
PyObject *PyColorFromVector(const mt::vec3 &vec);

/// Convert a float array to a python object.
template <int Size>
PyObject *PyObjectFrom(const float (&vec)[Size])
{
#ifdef USE_MATHUTILS
	return Vector_CreatePyObject(vec, Size, nullptr);
#else
	PyObject *list = PyList_New(Size);
	for (unsigned short i = 0; i < Size; ++i) {
		PyList_SET_ITEM(list, i, PyFloat_FromDouble(vec[i]));
	}
	return list;
#endif
}

#endif  // WITH_PYTHON

#endif  // __KX_PYMATH_H__
