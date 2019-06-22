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
 * Contributor(s): Mitchell Stokes.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/GameLogic/SCA_PythonJoystick.cpp
 *  \ingroup gamelogic
 */


#include "SCA_PythonJoystick.h"
#include "DEV_Joystick.h"
#include "SCA_IInputDevice.h"

//#include "GHOST_C-api.h"

/* ------------------------------------------------------------------------- */
/* Native functions                                                          */
/* ------------------------------------------------------------------------- */

SCA_PythonJoystick::SCA_PythonJoystick(DEV_Joystick *joystick, int joyindex)
	:m_joystick(joystick),
	m_joyindex(joyindex)
{
#ifdef WITH_PYTHON
	m_event_dict = PyDict_New();
#endif
}

SCA_PythonJoystick::~SCA_PythonJoystick()
{
#ifdef WITH_PYTHON
	PyDict_Clear(m_event_dict);
	Py_DECREF(m_event_dict);
#endif
}

std::string SCA_PythonJoystick::GetName() const
{
	return m_joystick->GetName();
}

#ifdef WITH_PYTHON

/* ------------------------------------------------------------------------- */
/* Python functions                                                          */
/* ------------------------------------------------------------------------- */

/* Integration hooks ------------------------------------------------------- */
PyTypeObject SCA_PythonJoystick::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"SCA_PythonJoystick",
	sizeof(EXP_PyObjectPlus_Proxy),
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
	&EXP_PyObjectPlus::Type,
	0, 0, 0, 0, 0, 0,
	py_base_new
};

PyMethodDef SCA_PythonJoystick::Methods[] = {
	{nullptr, nullptr} //Sentinel
};

PyAttributeDef SCA_PythonJoystick::Attributes[] = {
	EXP_PYATTRIBUTE_RO_FUNCTION("numButtons", SCA_PythonJoystick, pyattr_get_num_x),
	EXP_PYATTRIBUTE_RO_FUNCTION("numHats", SCA_PythonJoystick, pyattr_get_num_x),
	EXP_PYATTRIBUTE_RO_FUNCTION("numAxis", SCA_PythonJoystick, pyattr_get_num_x),
	EXP_PYATTRIBUTE_RO_FUNCTION("activeButtons", SCA_PythonJoystick, pyattr_get_active_buttons),
	EXP_PYATTRIBUTE_RO_FUNCTION("hatValues", SCA_PythonJoystick, pyattr_get_hat_values),
	EXP_PYATTRIBUTE_RO_FUNCTION("axisValues", SCA_PythonJoystick, pyattr_get_axis_values),
	EXP_PYATTRIBUTE_RO_FUNCTION("name", SCA_PythonJoystick, pyattr_get_name),
	EXP_PYATTRIBUTE_NULL    //Sentinel
};

// Use one function for numAxis, numButtons, and numHats
PyObject *SCA_PythonJoystick::pyattr_get_num_x(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	if (attrdef->m_name == "numButtons") {
		return PyLong_FromLong(JOYBUT_MAX);
	}
	else if (attrdef->m_name == "numAxis") {
		return PyLong_FromLong(JOYAXIS_MAX);
	}
	else if (attrdef->m_name == "numHats") {
		EXP_ShowDeprecationWarning("SCA_PythonJoystick.numHats", "SCA_PythonJoystick.numButtons");
		return PyLong_FromLong(0);
	}

	// If we got here, we have a problem...
	PyErr_SetString(PyExc_AttributeError, "invalid attribute");
	return nullptr;
}

PyObject *SCA_PythonJoystick::pyattr_get_active_buttons(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	SCA_PythonJoystick *self = static_cast<SCA_PythonJoystick *>(self_v);

	const int button_number = JOYBUT_MAX;

	PyObject *list = PyList_New(0);
	PyObject *value;

	for (int i = 0; i < button_number; i++) {
		if (self->m_joystick->aButtonPressIsPositive(i)) {
			value = PyLong_FromLong(i);
			PyList_Append(list, value);
			Py_DECREF(value);
		}
	}

	/* XXX return list adapted to new names (A, B, X, Y, START, etc) */
	return list;
}

PyObject *SCA_PythonJoystick::pyattr_get_hat_values(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	EXP_ShowDeprecationWarning("SCA_PythonJoystick.hatValues", "SCA_PythonJoystick.activeButtons");
	return PyList_New(0);
}

PyObject *SCA_PythonJoystick::pyattr_get_axis_values(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	SCA_PythonJoystick *self = static_cast<SCA_PythonJoystick *>(self_v);

	int axis_index = JOYAXIS_MAX;
	PyObject *list = PyList_New(axis_index);
	int position;

	while (axis_index--) {
		position = self->m_joystick->GetAxisPosition(axis_index);

		// We get back a range from -32768 to 32767, so we use an if here to
		// get a perfect -1.0 to 1.0 mapping. Some oddball system might have an
		// actual min of -32767 for shorts, so we use SHRT_MIN/MAX to be safe.
		if (position < 0) {
			PyList_SET_ITEM(list, axis_index, PyFloat_FromDouble(position / ((double)-SHRT_MIN)));
		}
		else {
			PyList_SET_ITEM(list, axis_index, PyFloat_FromDouble(position / (double)SHRT_MAX));
		}
	}

	return list;
}

PyObject *SCA_PythonJoystick::pyattr_get_name(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	SCA_PythonJoystick *self = static_cast<SCA_PythonJoystick *>(self_v);

	return PyUnicode_FromStdString(self->m_joystick->GetName());
}
#endif
