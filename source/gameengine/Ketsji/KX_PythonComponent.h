/**
 * $Id$
 *
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

#ifndef __KX_PYCOMPONENT
#define __KX_PYCOMPONENT
#ifndef DISABLE_PYTHON

#include "EXP_PyObjectPlus.h"

class KX_PythonComponent : public PyObjectPlus
{
	Py_Header
	private:
		// member vars
		class KX_GameObject *m_gameobj;
	STR_String m_name;

public:
	KX_PythonComponent(char *name);
	virtual ~KX_PythonComponent();

	STR_String& GetName();

	class KX_GameObject *GetGameobject();
	void SetGameobject(class KX_GameObject*);

	static PyObject *py_component_new(PyTypeObject *type, PyObject *args, PyObject *kwds);
	static int py_component_init(PyObjectPlus_Proxy *self, PyObject *args, PyObject *kwds);

	// Methods
	KX_PYMETHOD_DOC_O(KX_PythonComponent, start)

	// Attributes
	static PyObject*        pyattr_get_object(void* self_v, const KX_PYATTRIBUTE_DEF *attrdef);
};

#endif //ndef DISABLE_PYTHON
#endif //__KX_PYCOMPONENT
