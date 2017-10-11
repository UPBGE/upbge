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

/** \file SCA_PythonMouse.h
 *  \ingroup gamelogic
 */

#ifndef __SCA_PYTHONMOUSE_H__
#define __SCA_PYTHONMOUSE_H__

#include "EXP_PyObjectPlus.h"

class SCA_PythonMouse : public EXP_PyObjectPlus
{
	Py_Header(SCA_PythonMouse)
private:
	class SCA_IInputDevice *m_mouse;
	class RAS_ICanvas *m_canvas;

public:
	SCA_PythonMouse(class SCA_IInputDevice* mouse, class RAS_ICanvas* canvas);
	virtual ~SCA_PythonMouse();

	void Show(bool visible);

#ifdef WITH_PYTHON
	EXP_PYMETHOD_DOC(SCA_PythonMouse, show);

	PyObject *pyattr_get_events();
	PyObject *pyattr_get_inputs();
	PyObject *pyattr_get_active_events();
	PyObject *pyattr_get_active_inputs();
	mt::vec2 pyattr_get_position();
	void pyattr_set_position(const mt::vec2& value);
	bool pyattr_get_visible();
	void pyattr_set_visible(bool value);
#endif
};

#endif  /* __SCA_PYTHONMOUSE_H__ */
