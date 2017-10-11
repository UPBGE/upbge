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
}

SCA_PythonJoystick::~SCA_PythonJoystick()
{
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

EXP_Attribute SCA_PythonJoystick::Attributes[] = {
	EXP_ATTRIBUTE_RO_FUNCTION("numButtons", pyattr_get_num_x),
	EXP_ATTRIBUTE_RO_FUNCTION("numHats", pyattr_get_num_x),
	EXP_ATTRIBUTE_RO_FUNCTION("numAxis", pyattr_get_num_x),
	EXP_ATTRIBUTE_RO_FUNCTION("activeButtons", pyattr_get_active_buttons),
	EXP_ATTRIBUTE_RO_FUNCTION("hatValues", pyattr_get_hat_values),
	EXP_ATTRIBUTE_RO_FUNCTION("axisValues", pyattr_get_axis_values),
	EXP_ATTRIBUTE_RO_FUNCTION("name", pyattr_get_name),
	EXP_ATTRIBUTE_NULL	//Sentinel
};

// Use one function for numAxis, numButtons, and numHats
int SCA_PythonJoystick::pyattr_get_num_x(const EXP_Attribute *attrdef)
{
	if (attrdef->m_name == "numButtons") {
		return JOYBUT_MAX;
	}
	else if (attrdef->m_name == "numAxis") {
		return JOYAXIS_MAX;
	}
	else if (attrdef->m_name == "numHats") {
		EXP_ShowDeprecationWarning("SCA_PythonJoystick.numHats", "SCA_PythonJoystick.numButtons");
		return 0;
	}
	return 0;
}

std::vector<int> SCA_PythonJoystick::pyattr_get_active_buttons()
{
	std::vector<int> values;
	for (int i = 0; i < JOYBUT_MAX; i++) {
		if (m_joystick->aButtonPressIsPositive(i)) {
			values.push_back(i);
		}
	}

	return values;
}

std::vector<int> SCA_PythonJoystick::pyattr_get_hat_values()
{
	EXP_ShowDeprecationWarning("SCA_PythonJoystick.hatValues", "SCA_PythonJoystick.activeButtons");
	return {};
}

std::vector<int> SCA_PythonJoystick::pyattr_get_axis_values()
{
	std::vector<int> values(JOYAXIS_MAX);
	for (unsigned short i = 0; i < JOYAXIS_MAX; ++i) {
		const int position = m_joystick->GetAxisPosition(i);

		// We get back a range from -32768 to 32767, so we use an if here to
		// get a perfect -1.0 to 1.0 mapping. Some oddball system might have an
		// actual min of -32767 for shorts, so we use SHRT_MIN/MAX to be safe.
		values[i] = (position < 0) ? (position / ((double)-SHRT_MIN)) : (position / (double)SHRT_MAX);
	}

	return values;
}

std::string SCA_PythonJoystick::pyattr_get_name()
{
	return m_joystick->GetName();
}
#endif
