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

/** \file gameengine/Ketsji/KX_2DFilter.cpp
*  \ingroup ketsji
*/

#include "KX_2DFilter.h"
#include "EXP_PyObjectPlus.h"
#include "glew-mx.h"
#include "MT_Matrix4x4.h"
#include "MT_Matrix3x3.h"
#include "KX_PyMath.h"
#include "BLI_utildefines.h"

#define spit(x) std::cout << x << std::endl;

KX_2DFilter::KX_2DFilter(RAS_2DFilterData& data, RAS_2DFilterManager *manager)
	:RAS_2DFilter(data, manager),
	CValue()
{
	m_name = STR_String(0);
}

KX_2DFilter::~KX_2DFilter()
{
}

// stuff for cvalue related things
CValue *KX_2DFilter::Calc(VALUE_OPERATOR op, CValue *val)
{
	return NULL;
}

CValue *KX_2DFilter::CalcFinal(VALUE_DATA_TYPE dtype, VALUE_OPERATOR op, CValue *val)
{
	return NULL;
}

const STR_String &  KX_2DFilter::GetText()
{
	return GetName();
}

double KX_2DFilter::GetNumber()
{
	return -1.0;
}

STR_String& KX_2DFilter::GetName()
{
	return m_name;
}

void KX_2DFilter::SetName(const char *name)
{
}

CValue *KX_2DFilter::GetReplica()
{
	return NULL;
}

int KX_2DFilter::GetUniformLocation(const char *name)
{
	if (GLEW_ARB_fragment_shader && GLEW_ARB_vertex_shader && GLEW_ARB_shader_objects) {
		BLI_assert(GetProgramId() != 0);
		int location = glGetUniformLocationARB(GetProgramId(), name);

		if (location == -1) {
			spit("Invalid uniform value: " << name << ".");
		}

		return location;
	}
	return -1;
}

void KX_2DFilter::SetUniform(int uniform, const MT_Vector2 &vec)
{
	if (GLEW_ARB_fragment_shader && GLEW_ARB_vertex_shader && GLEW_ARB_shader_objects) {
		float value[2];
		vec.getValue(value);
		glUniform2fvARB(uniform, 1, value);
	}
}

void KX_2DFilter::SetUniform(int uniform, const MT_Vector3 &vec)
{
	if (GLEW_ARB_fragment_shader && GLEW_ARB_vertex_shader && GLEW_ARB_shader_objects) {
		float value[3];
		vec.getValue(value);
		glUniform3fvARB(uniform, 1, value);
	}
}

void KX_2DFilter::SetUniform(int uniform, const MT_Vector4 &vec)
{
	if (GLEW_ARB_fragment_shader && GLEW_ARB_vertex_shader && GLEW_ARB_shader_objects) {
		float value[4];
		vec.getValue(value);
		glUniform4fvARB(uniform, 1, value);
	}
}

void KX_2DFilter::SetUniform(int uniform, const unsigned int &val)
{
	if (GLEW_ARB_fragment_shader && GLEW_ARB_vertex_shader && GLEW_ARB_shader_objects) {
		glUniform1iARB(uniform, val);
	}
}

void KX_2DFilter::SetUniform(int uniform, const int val)
{
	if (GLEW_ARB_fragment_shader && GLEW_ARB_vertex_shader && GLEW_ARB_shader_objects) {
		glUniform1iARB(uniform, val);
	}
}

void KX_2DFilter::SetUniform(int uniform, const float &val)
{
	if (GLEW_ARB_fragment_shader && GLEW_ARB_vertex_shader && GLEW_ARB_shader_objects) {
		glUniform1fARB(uniform, val);
	}
}

void KX_2DFilter::SetUniform(int uniform, const MT_Matrix4x4 &vec, bool transpose)
{
	if (GLEW_ARB_fragment_shader && GLEW_ARB_vertex_shader && GLEW_ARB_shader_objects) {
		float value[16];
		// note: getValue gives back column major as needed by OpenGL
		vec.getValue(value);
		glUniformMatrix4fvARB(uniform, 1, transpose ? GL_TRUE : GL_FALSE, value);
	}
}

void KX_2DFilter::SetUniform(int uniform, const MT_Matrix3x3 &vec, bool transpose)
{
	if (GLEW_ARB_fragment_shader && GLEW_ARB_vertex_shader && GLEW_ARB_shader_objects) {
		float value[9];
		value[0] = (float)vec[0][0];
		value[1] = (float)vec[1][0];
		value[2] = (float)vec[2][0];
		value[3] = (float)vec[0][1];
		value[4] = (float)vec[1][1];
		value[5] = (float)vec[2][1];
		value[6] = (float)vec[0][2];
		value[7] = (float)vec[1][2];
		value[8] = (float)vec[2][2];
		glUniformMatrix3fvARB(uniform, 1, transpose ? GL_TRUE : GL_FALSE, value);
	}
}

void KX_2DFilter::SetUniform(int uniform, const float *val, int len)
{
	if (GLEW_ARB_fragment_shader && GLEW_ARB_vertex_shader && GLEW_ARB_shader_objects) {
		if (len == 2) {
			glUniform2fvARB(uniform, 1, (GLfloat *)val);
		}
		else if (len == 3) {
			glUniform3fvARB(uniform, 1, (GLfloat *)val);
		}
		else if (len == 4) {
			glUniform4fvARB(uniform, 1, (GLfloat *)val);
		}
		else {
			BLI_assert(0);
		}
	}
}

void KX_2DFilter::SetUniform(int uniform, const int *val, int len)
{
	if (GLEW_ARB_fragment_shader && GLEW_ARB_vertex_shader && GLEW_ARB_shader_objects) {
		if (len == 2) {
			glUniform2ivARB(uniform, 1, (GLint *)val);
		}
		else if (len == 3) {
			glUniform3ivARB(uniform, 1, (GLint *)val);
		}
		else if (len == 4) {
			glUniform4ivARB(uniform, 1, (GLint *)val);
		}
		else {
			BLI_assert(0);
		}
	}
}

void KX_2DFilter::SetSampler(int loc, int unit)
{
	if (GLEW_ARB_fragment_shader && GLEW_ARB_vertex_shader && GLEW_ARB_shader_objects) {
		glUniform1iARB(loc, unit);
	}
}

PyTypeObject KX_2DFilter::Type = {
	PyVarObject_HEAD_INIT(NULL, 0)
	"KX_2DFilter",
	sizeof(PyObjectPlus_Proxy),
	0,
	py_base_dealloc,
	0,
	0,
	0,
	0,
	py_base_repr,
	0, 0, 0, 0, 0, 0, 0, 0, 0,
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	0, 0, 0, 0, 0, 0, 0,
	Methods,
	0,
	0,
	&CValue::Type,
	0, 0, 0, 0, 0, 0,
	py_base_new
};

PyMethodDef KX_2DFilter::Methods[] = {
	KX_PYMETHODTABLE(KX_2DFilter, setUniform1f),
	KX_PYMETHODTABLE(KX_2DFilter, setUniform2f),
	KX_PYMETHODTABLE(KX_2DFilter, setUniform3f),
	KX_PYMETHODTABLE(KX_2DFilter, setUniform4f),
	KX_PYMETHODTABLE(KX_2DFilter, setUniform1i),
	KX_PYMETHODTABLE(KX_2DFilter, setUniform2i),
	KX_PYMETHODTABLE(KX_2DFilter, setUniform3i),
	KX_PYMETHODTABLE(KX_2DFilter, setUniform4i),
	KX_PYMETHODTABLE(KX_2DFilter, setUniformfv),
	KX_PYMETHODTABLE(KX_2DFilter, setUniformiv),
	KX_PYMETHODTABLE(KX_2DFilter, setSampler),
	KX_PYMETHODTABLE(KX_2DFilter, setUniformMatrix4),
	KX_PYMETHODTABLE(KX_2DFilter, setUniformMatrix3),
	{ NULL, NULL } //Sentinel
};

PyAttributeDef KX_2DFilter::Attributes[] = {
	{ NULL }    //Sentinel
};

KX_PYMETHODDEF_DOC(KX_2DFilter, setSampler, "setSampler(name, index)")
{
	if (GetError()) {
		Py_RETURN_NONE;
	}

	const char *uniform;
	int index = -1;

	if (PyArg_ParseTuple(args, "si:setSampler", &uniform, &index)) {
		int loc = GetUniformLocation(uniform);

		if (loc != -1) {
			if (index >= BL_Texture::MaxUnits || index < 0) {
				spit("Invalid texture sample index: " << index);
			}
			/* don't forget to use glUseProgramObjectARB to update uniforms */
			glUseProgramObjectARB(GetProgramId());
			SetUniform(loc, index);
		}
		Py_RETURN_NONE;
	}
	return NULL;
}

KX_PYMETHODDEF_DOC(KX_2DFilter, setUniform1f, "setUniform1f(name, fx)")
{
	if (GetError()) {
		Py_RETURN_NONE;
	}

	const char *uniform;
	float value = 0.0f;

	if (PyArg_ParseTuple(args, "sf:setUniform1f", &uniform, &value)) {
		int loc = GetUniformLocation(uniform);

		if (loc != -1) {
			/* don't forget to use glUseProgramObjectARB to update uniforms */
			glUseProgramObjectARB(GetProgramId());
			SetUniform(loc, (float)value);
		}
		Py_RETURN_NONE;
	}
	return NULL;
}

KX_PYMETHODDEF_DOC(KX_2DFilter, setUniform2f, "setUniform2f(name, fx, fy)")
{
	if (GetError()) {
		Py_RETURN_NONE;
	}

	const char *uniform;
	float array[2] = { 0.0f, 0.0f };

	if (PyArg_ParseTuple(args, "sff:setUniform2f", &uniform, &array[0], &array[1])) {
		int loc = GetUniformLocation(uniform);

		if (loc != -1) {
			/* don't forget to use glUseProgramObjectARB to update uniforms */
			glUseProgramObjectARB(GetProgramId());
			SetUniform(loc, array, 2);
		}
		Py_RETURN_NONE;
	}
	return NULL;
}

KX_PYMETHODDEF_DOC(KX_2DFilter, setUniform3f, "setUniform3f(name, fx,fy,fz) ")
{
	if (GetError()) {
		Py_RETURN_NONE;
	}

	const char *uniform;
	float array[3] = { 0.0f, 0.0f, 0.0f };

	if (PyArg_ParseTuple(args, "sfff:setUniform3f", &uniform, &array[0], &array[1], &array[2])) {
		int loc = GetUniformLocation(uniform);

		if (loc != -1) {
			/* don't forget to use glUseProgramObjectARB to update uniforms */
			glUseProgramObjectARB(GetProgramId());
			SetUniform(loc, array, 3);
		}
		Py_RETURN_NONE;
	}
	return NULL;
}

KX_PYMETHODDEF_DOC(KX_2DFilter, setUniform4f, "setUniform4f(name, fx,fy,fz, fw) ")
{
	if (GetError()) {
		Py_RETURN_NONE;
	}

	const char *uniform;
	float array[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

	if (PyArg_ParseTuple(args, "sffff:setUniform4f", &uniform, &array[0], &array[1], &array[2], &array[3])) {
		int loc = GetUniformLocation(uniform);

		if (loc != -1) {
			/* don't forget to use glUseProgramObjectARB to update uniforms */
			glUseProgramObjectARB(GetProgramId());
			SetUniform(loc, array, 4);
		}
		Py_RETURN_NONE;
	}
	return NULL;
}

KX_PYMETHODDEF_DOC(KX_2DFilter, setUniform1i, "setUniform1i(name, ix)")
{
	if (GetError()) {
		Py_RETURN_NONE;
	}

	const char *uniform;
	int value = 0;

	if (PyArg_ParseTuple(args, "si:setUniform1i", &uniform, &value)) {
		int loc = GetUniformLocation(uniform);

		if (loc != -1) {
			/* don't forget to use glUseProgramObjectARB to update uniforms */
			glUseProgramObjectARB(GetProgramId());
			SetUniform(loc, (int)value);
		}
		Py_RETURN_NONE;
	}
	return NULL;
}

KX_PYMETHODDEF_DOC(KX_2DFilter, setUniform2i, "setUniform2i(name, ix, iy)")
{
	if (GetError()) {
		Py_RETURN_NONE;
	}

	const char *uniform;
	int array[2] = { 0, 0 };

	if (PyArg_ParseTuple(args, "sii:setUniform2i", &uniform, &array[0], &array[1])) {
		int loc = GetUniformLocation(uniform);

		if (loc != -1) {
			/* don't forget to use glUseProgramObjectARB to update uniforms */
			glUseProgramObjectARB(GetProgramId());
			SetUniform(loc, array, 2);
		}
		Py_RETURN_NONE;
	}
	return NULL;
}

KX_PYMETHODDEF_DOC(KX_2DFilter, setUniform3i, "setUniform3i(name, ix,iy,iz) ")
{
	if (GetError()) {
		Py_RETURN_NONE;
	}

	const char *uniform;
	int array[3] = { 0, 0, 0 };

	if (PyArg_ParseTuple(args, "siii:setUniform3i", &uniform, &array[0], &array[1], &array[2])) {
		int loc = GetUniformLocation(uniform);

		if (loc != -1) {
			/* don't forget to use glUseProgramObjectARB to update uniforms */
			glUseProgramObjectARB(GetProgramId());
			SetUniform(loc, array, 3);
		}
		Py_RETURN_NONE;
	}
	return NULL;
}

KX_PYMETHODDEF_DOC(KX_2DFilter, setUniform4i, "setUniform4i(name, ix,iy,iz, iw) ")
{
	if (GetError()) {
		Py_RETURN_NONE;
	}

	const char *uniform;
	int array[4] = { 0, 0, 0, 0 };

	if (PyArg_ParseTuple(args, "siiii:setUniform4i", &uniform, &array[0], &array[1], &array[2], &array[3])) {
		int loc = GetUniformLocation(uniform);

		if (loc != -1) {
			/* don't forget to use glUseProgramObjectARB to update uniforms */
			glUseProgramObjectARB(GetProgramId());
			SetUniform(loc, array, 4);
		}
		Py_RETURN_NONE;
	}
	return NULL;
}

KX_PYMETHODDEF_DOC(KX_2DFilter, setUniformfv, "setUniformfv(float (list2 or list3 or list4))")
{
	if (GetError()) {
		Py_RETURN_NONE;
	}

	const char *uniform = "";
	PyObject *listPtr = NULL;
	float array_data[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

	if (PyArg_ParseTuple(args, "sO:setUniformfv", &uniform, &listPtr)) {
		int loc = GetUniformLocation(uniform);
		if (loc != -1) {
			if (PySequence_Check(listPtr)) {
				unsigned int list_size = PySequence_Size(listPtr);

				for (unsigned int i = 0; (i < list_size && i < 4); i++) {
					PyObject *item = PySequence_GetItem(listPtr, i);
					array_data[i] = (float)PyFloat_AsDouble(item);
					Py_DECREF(item);
				}

				switch (list_size) {
				case 2:
				{
					float array2[2] = { array_data[0], array_data[1] };
					/* don't forget to use glUseProgramObjectARB to update uniforms */
					glUseProgramObjectARB(GetProgramId());
					SetUniform(loc, array2, 2);

					Py_RETURN_NONE;
					break;
				}
				case 3:
				{
					float array3[3] = { array_data[0], array_data[1], array_data[2] };
					/* don't forget to use glUseProgramObjectARB to update uniforms */
					glUseProgramObjectARB(GetProgramId());
					SetUniform(loc, array3, 3);

					Py_RETURN_NONE;
					break;
				}
				case 4:
				{
					float array4[4] = { array_data[0], array_data[1], array_data[2], array_data[3] };
					/* don't forget to use glUseProgramObjectARB to update uniforms */
					glUseProgramObjectARB(GetProgramId());
					SetUniform(loc, array4, 4);

					Py_RETURN_NONE;
					break;
				}
				default:
				{
					PyErr_SetString(PyExc_TypeError,
						"shader.setUniform4i(name, ix,iy,iz, iw): KX_2DFilter. invalid list size");
					return NULL;
					break;
				}
				}
			}
		}
	}
	return NULL;
}

KX_PYMETHODDEF_DOC(KX_2DFilter, setUniformiv, "setUniformiv(uniform_name, (list2 or list3 or list4))")
{
	if (GetError()) {
		Py_RETURN_NONE;
	}

	const char *uniform = "";
	PyObject *listPtr = NULL;
	int array_data[4] = { 0, 0, 0, 0 };

	if (!PyArg_ParseTuple(args, "sO:setUniformiv", &uniform, &listPtr)) {
		return NULL;
	}

	int loc = GetUniformLocation(uniform);

	if (loc == -1) {
		PyErr_SetString(PyExc_TypeError,
			"shader.setUniformiv(...): KX_2DFilter, first string argument is not a valid uniform value");
		return NULL;
	}

	if (!PySequence_Check(listPtr)) {
		PyErr_SetString(PyExc_TypeError, "shader.setUniformiv(...): KX_2DFilter, second argument is not a sequence");
		return NULL;
	}

	unsigned int list_size = PySequence_Size(listPtr);

	for (unsigned int i = 0; (i < list_size && i < 4); i++) {
		PyObject *item = PySequence_GetItem(listPtr, i);
		array_data[i] = PyLong_AsLong(item);
		Py_DECREF(item);
	}

	if (PyErr_Occurred()) {
		PyErr_SetString(PyExc_TypeError,
			"shader.setUniformiv(...): KX_2DFilter, one or more values in the list is not an int");
		return NULL;
	}

	// Sanity checks done!
	switch (list_size) {
	case 2:
	{
		int array2[2] = { array_data[0], array_data[1] };
		/* don't forget to use glUseProgramObjectARB to update uniforms */
		glUseProgramObjectARB(GetProgramId());
		SetUniform(loc, array2, 2);

		Py_RETURN_NONE;
		break;
	}
	case 3:
	{
		int array3[3] = { array_data[0], array_data[1], array_data[2] };
		/* don't forget to use glUseProgramObjectARB to update uniforms */
		glUseProgramObjectARB(GetProgramId());
		SetUniform(loc, array3, 3);

		Py_RETURN_NONE;
		break;
	}
	case 4:
	{
		int array4[4] = { array_data[0], array_data[1], array_data[2], array_data[3] };
		/* don't forget to use glUseProgramObjectARB to update uniforms */
		glUseProgramObjectARB(GetProgramId());
		SetUniform(loc, array4, 4);

		Py_RETURN_NONE;
		break;
	}
	default:
	{
		PyErr_SetString(PyExc_TypeError,
			"shader.setUniformiv(...): KX_2DFilter, second argument, invalid list size, expected an int "
			"list between 2 and 4");
		return NULL;
		break;
	}
	}
	Py_RETURN_NONE;
}

KX_PYMETHODDEF_DOC(KX_2DFilter, setUniformMatrix4,
	"setUniformMatrix4(uniform_name, mat-4x4, transpose(row-major=true, col-major=false)")
{
	if (GetError()) {
		Py_RETURN_NONE;
	}

	float matr[16] = {
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f
	};

	const char *uniform;
	PyObject *matrix = NULL;
	int transp = 0; // python use column major by default, so no transpose....

	if (!PyArg_ParseTuple(args, "sO|i:setUniformMatrix4", &uniform, &matrix, &transp)) {
		return NULL;
	}

	int loc = GetUniformLocation(uniform);

	if (loc == -1) {
		PyErr_SetString(PyExc_TypeError,
			"shader.setUniformMatrix4(...): KX_2DFilter, first string argument is not a valid uniform value");
		return NULL;
	}

	MT_Matrix4x4 mat;

	if (!PyMatTo(matrix, mat)) {
		PyErr_SetString(PyExc_TypeError,
			"shader.setUniformMatrix4(...): KX_2DFilter, second argument cannot be converted into a 4x4 matrix");
		return NULL;
	}

	/* don't forget to use glUseProgramObjectARB to update uniforms */
	glUseProgramObjectARB(GetProgramId());
	SetUniform(loc, mat, (transp != 0));

	Py_RETURN_NONE;
}


KX_PYMETHODDEF_DOC(KX_2DFilter, setUniformMatrix3,
	"setUniformMatrix3(uniform_name, list[3x3], transpose(row-major=true, col-major=false)")
{
	if (GetError()) {
		Py_RETURN_NONE;
	}

	float matr[9] = {
		1.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 1.0f,
	};

	const char *uniform;
	PyObject *matrix = NULL;
	int transp = 0; // python use column major by default, so no transpose....

	if (!PyArg_ParseTuple(args, "sO|i:setUniformMatrix3", &uniform, &matrix, &transp)) {
		return NULL;
	}

	int loc = GetUniformLocation(uniform);

	if (loc == -1) {
		PyErr_SetString(PyExc_TypeError,
			"shader.setUniformMatrix3(...): KX_2DFilter, first string argument is not a valid uniform value");
		return NULL;
	}

	MT_Matrix3x3 mat;

	if (!PyMatTo(matrix, mat)) {
		PyErr_SetString(PyExc_TypeError,
			"shader.setUniformMatrix3(...): KX_2DFilter, second argument cannot be converted into a 3x3 matrix");
		return NULL;
	}
	/* don't forget to use glUseProgramObjectARB to update uniforms */
	glUseProgramObjectARB(GetProgramId());
	SetUniform(loc, mat, (transp != 0));

	Py_RETURN_NONE;
}
