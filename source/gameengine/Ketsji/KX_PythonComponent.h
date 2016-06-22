/**
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

#ifndef __KX_PYCOMPONENT_H__
#define __KX_PYCOMPONENT_H__

#ifdef WITH_PYTHON

#include "EXP_Value.h"

class KX_GameObject;

class KX_PythonComponent : public CValue
{
	Py_Header
private:
	// member vars
	KX_GameObject *m_gameobj;
	STR_String m_name;

public:
	KX_PythonComponent(STR_String name);
	virtual ~KX_PythonComponent();

	KX_GameObject *GetGameObject();
	void SetGameObject(KX_GameObject *gameobj);

	// stuff for cvalue related things
	STR_String& GetName();

	static PyObject *py_component_new(PyTypeObject *type, PyObject *args, PyObject *kwds);
	static int py_component_init(PyObjectPlus_Proxy *self, PyObject *args, PyObject *kwds);

	// Attributes
	static PyObject *pyattr_get_object(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
};

#endif // WITH_PYTHON

#endif // __KX_PYCOMPONENT_H__
