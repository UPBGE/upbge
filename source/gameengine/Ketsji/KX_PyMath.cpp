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
 * Initialize Python thingies.
 */

/** \file gameengine/Ketsji/KX_PyMath.cpp
 *  \ingroup ketsji
 */


#ifdef _MSC_VER
#  pragma warning (disable:4786)
#endif

#ifdef WITH_PYTHON

#include "EXP_ListValue.h"

#include "KX_PyMath.h"

bool PyOrientationTo(PyObject *pyval, mt::mat3 &rot, const char *error_prefix)
{
	int size = PySequence_Size(pyval);

	if (size == 4) {
		mt::quat qrot;
		if (PyQuatTo(pyval, qrot)) {
			rot = qrot.ToMatrix();
			return true;
		}
	}
	else if (size == 3) {
		/* 3x3 matrix or euler */
		mt::vec3 erot;
		if (PyVecTo(pyval, erot)) {
			rot = mt::mat3(erot);
			return true;
		}
		PyErr_Clear();

		if (PyMatTo(pyval, rot)) {
			return true;
		}
	}

	PyErr_Format(PyExc_TypeError, "%s, could not set the orientation from a 3x3 matrix, quaternion or euler sequence", error_prefix);
	return false;
}

bool PyQuatTo(PyObject *pyval, mt::quat &qrot)
{
	if (!PyVecTo(pyval, qrot)) {
		return false;
	}

	/* annoying!, Blender/Mathutils have the W axis first! */
	float w = qrot[0]; /* from python, this is actually the W */
	qrot[0] = qrot[1];
	qrot[1] = qrot[2];
	qrot[2] = qrot[3];
	qrot[3] = w;

	return true;
}

#ifdef USE_MATHUTILS
PyObject *PyObjectFrom(const mt::quat &qrot)
{
	float data[4];
	qrot.Pack(data);
	return Quaternion_CreatePyObject(data, nullptr);
}
#endif

PyObject *PyColorFromVector(const mt::vec3 &vec)
{
#ifdef USE_MATHUTILS
	return Color_CreatePyObject(vec.Data(), 3, nullptr);
#else
	return PyObjectFrom(vec);
#endif
}

#endif // WITH_PYTHON
