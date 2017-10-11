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
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/GameLogic/SCA_PythonMouse.cpp
 *  \ingroup gamelogic
 */


#include "SCA_PythonMouse.h"
#include "SCA_IInputDevice.h"
#include "RAS_ICanvas.h"

/* ------------------------------------------------------------------------- */
/* Native functions                                                          */
/* ------------------------------------------------------------------------- */

SCA_PythonMouse::SCA_PythonMouse(SCA_IInputDevice *mouse, RAS_ICanvas *canvas)
	:EXP_PyObjectPlus(),
	m_mouse(mouse),
	m_canvas(canvas)
{
}

SCA_PythonMouse::~SCA_PythonMouse()
{
}

#ifdef WITH_PYTHON

/* ------------------------------------------------------------------------- */
/* Python functions                                                          */
/* ------------------------------------------------------------------------- */

/* Integration hooks ------------------------------------------------------- */
PyTypeObject SCA_PythonMouse::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"SCA_PythonMouse",
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

PyMethodDef SCA_PythonMouse::Methods[] = {
	{nullptr, nullptr} //Sentinel
};

EXP_Attribute SCA_PythonMouse::Attributes[] = {
	EXP_ATTRIBUTE_RO_FUNCTION("events", pyattr_get_events),
	EXP_ATTRIBUTE_RO_FUNCTION("inputs", pyattr_get_inputs),
	EXP_ATTRIBUTE_RO_FUNCTION("active_events", pyattr_get_active_events),
	EXP_ATTRIBUTE_RO_FUNCTION("activeInputs", pyattr_get_active_inputs),
	EXP_ATTRIBUTE_RW_FUNCTION("position", pyattr_get_position, pyattr_set_position),
	EXP_ATTRIBUTE_RW_FUNCTION("visible", pyattr_get_visible, pyattr_set_visible),
	EXP_ATTRIBUTE_NULL	//Sentinel
};

PyObject *SCA_PythonMouse::pyattr_get_events()
{
	EXP_ShowDeprecationWarning("mouse.events", "mouse.inputs");

	PyObject *dict = PyDict_New();

	for (int i = SCA_IInputDevice::BEGINMOUSE; i <= SCA_IInputDevice::ENDMOUSE; i++)
	{
		SCA_InputEvent& input = m_mouse->GetInput((SCA_IInputDevice::SCA_EnumInputs)i);
		int event = 0;
		if (input.m_queue.empty()) {
			event = input.m_status[input.m_status.size() - 1];
		}
		else {
			event = input.m_queue[input.m_queue.size() - 1];
		}

		PyObject *key = PyLong_FromLong(i);
		PyObject *value = PyLong_FromLong(event);

		PyDict_SetItem(dict, key, value);

		Py_DECREF(key);
		Py_DECREF(value);
	}

	return dict;
}

PyObject *SCA_PythonMouse::pyattr_get_inputs()
{
	PyObject *dict = PyDict_New();

	for (int i = SCA_IInputDevice::BEGINMOUSE; i <= SCA_IInputDevice::ENDMOUSE; i++)
	{
		SCA_InputEvent& input = m_mouse->GetInput((SCA_IInputDevice::SCA_EnumInputs)i);

		PyObject *key = PyLong_FromLong(i);

		PyDict_SetItem(dict, key, input.GetProxy());

		Py_DECREF(key);
	}

	return dict;
}

PyObject *SCA_PythonMouse::pyattr_get_active_events()
{
	EXP_ShowDeprecationWarning("mouse.active_events", "mouse.activeInputs");

	PyObject *dict = PyDict_New();

	for (int i = SCA_IInputDevice::BEGINMOUSE; i <= SCA_IInputDevice::ENDMOUSE; i++)
	{
		SCA_InputEvent& input = m_mouse->GetInput((SCA_IInputDevice::SCA_EnumInputs)i);

		if (input.Find(SCA_InputEvent::ACTIVE)) {
			int event = 0;
			if (input.m_queue.empty()) {
				event = input.m_status[input.m_status.size() - 1];
			}
			else {
				event = input.m_queue[input.m_queue.size() - 1];
			}

			PyObject *key = PyLong_FromLong(i);
			PyObject *value = PyLong_FromLong(event);

			PyDict_SetItem(dict, key, value);

			Py_DECREF(key);
			Py_DECREF(value);
		}
	}
	Py_INCREF(dict);
	return dict;
}

PyObject *SCA_PythonMouse::pyattr_get_active_inputs()
{
	PyObject *dict = PyDict_New();

	for (int i = SCA_IInputDevice::BEGINMOUSE; i <= SCA_IInputDevice::ENDMOUSE; i++)
	{
		SCA_InputEvent& input = m_mouse->GetInput((SCA_IInputDevice::SCA_EnumInputs)i);

		if (input.Find(SCA_InputEvent::ACTIVE)) {
			PyObject *key = PyLong_FromLong(i);

			PyDict_SetItem(dict, key, input.GetProxy());

			Py_DECREF(key);
		}
	}
	Py_INCREF(dict);
	return dict;
}

mt::vec2 SCA_PythonMouse::pyattr_get_position()
{
	const SCA_InputEvent& xevent = m_mouse->GetInput(SCA_IInputDevice::MOUSEX);
	const SCA_InputEvent& yevent = m_mouse->GetInput(SCA_IInputDevice::MOUSEY);

	float x_coord, y_coord;

	x_coord = m_canvas->GetMouseNormalizedX(xevent.m_values[xevent.m_values.size() - 1]);
	y_coord = m_canvas->GetMouseNormalizedY(yevent.m_values[yevent.m_values.size() - 1]);

	return mt::vec2(x_coord, y_coord);
}

void SCA_PythonMouse::pyattr_set_position(const mt::vec2& value)
{
	const int x = (int)(value.x * m_canvas->GetMaxX());
	const int y = (int)(value.y * m_canvas->GetMaxY());

	m_canvas->SetMousePosition(x, y);
}

bool SCA_PythonMouse::pyattr_get_visible()
{
	return (m_canvas->GetMouseState() != RAS_ICanvas::MOUSE_INVISIBLE);
}

void SCA_PythonMouse::pyattr_set_visible(bool value)
{
	m_canvas->SetMouseState(value ? RAS_ICanvas::MOUSE_NORMAL : RAS_ICanvas::MOUSE_INVISIBLE);
}

#endif
