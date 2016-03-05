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

#ifdef WITH_PYTHON

#include "KX_PythonComponent.h"
#include "KX_GameObject.h"
#include "BLI_string.h"

/* ------------------------------------------------------------------------- */
/* Native functions                                                          */
/* ------------------------------------------------------------------------- */

KX_PythonComponent::KX_PythonComponent(char *name)
	: PyObjectPlus(),
	  m_gameobj(NULL),
	  m_name(name)
{
}

KX_PythonComponent::~KX_PythonComponent()
{
}

STR_String& KX_PythonComponent::GetName()
{
	return m_name;
}

KX_GameObject* KX_PythonComponent::GetGameObject()
{
	return m_gameobj;
}

void KX_PythonComponent::SetGameObject(KX_GameObject *gameobj)
{
	m_gameobj = gameobj;
}

/* ------------------------------------------------------------------------- */
/* Python functions                                                          */
/* ------------------------------------------------------------------------- */

PyObject *KX_PythonComponent::py_component_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
	PyObjectPlus_Proxy *self;
	char name[64];

	self = (PyObjectPlus_Proxy*)type->tp_alloc(type, 0);

	if (self) {
		BLI_strncpy(name, Py_TYPE(self)->tp_name, sizeof(name));

		self->ptr = NULL;
		self->ref = new KX_PythonComponent(name);
		self->py_owns = true;
		self->py_ref = true;
	}

	return (PyObject*)self;
}

int KX_PythonComponent::py_component_init(PyObjectPlus_Proxy *self, PyObject *args, PyObject *kwds)
{
	KX_GameObject *gameobj;
	KX_PythonComponent *kxpycomp;
	PyObject *pyobj;

	if (!PyArg_ParseTuple(args, "O", &pyobj)) {
		return -1;
	}

	if (!PyObject_IsInstance(pyobj, (PyObject*)&KX_GameObject::Type)) {
		PyErr_SetString(PyExc_TypeError, "expected a KX_GameObject for first argument");
		return -1;
	}

	gameobj = static_cast<KX_GameObject*>(BGE_PROXY_REF(pyobj));
	kxpycomp = static_cast<KX_PythonComponent*>(BGE_PROXY_REF(self));

	kxpycomp->SetGameObject(gameobj);

	return 0;
}

/* Integration hooks ------------------------------------------------------- */
PyTypeObject KX_PythonComponent::Type = {
	PyVarObject_HEAD_INIT(NULL, 0)
	"KX_PythonComponent",
	sizeof(PyObjectPlus_Proxy),
	0,
	py_base_dealloc,
	0,
	0,
	0,
	0,
	py_base_repr,
	0,0,0,0,0,0,0,0,0,
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	0,0,0,0,0,0,0,
	Methods,
	0,
	0,
	&PyObjectPlus::Type,
	0,0,0,0,
	(initproc)py_component_init,
	0,
	py_component_new
};

PyMethodDef KX_PythonComponent::Methods[] = {
	KX_PYMETHODTABLE_O(KX_PythonComponent, start),
	{NULL,NULL} //Sentinel
};

PyAttributeDef KX_PythonComponent::Attributes[] = {
	KX_PYATTRIBUTE_RO_FUNCTION("object", KX_PythonComponent, pyattr_get_object),
	{ NULL }        //Sentinel
};

PyObject* KX_PythonComponent::pyattr_get_object(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_PythonComponent* self= static_cast<KX_PythonComponent*>(self_v);
	KX_GameObject *gameobj = self->GetGameObject();

	if (gameobj) {
		return gameobj->GetProxy();
	}
	else {
		Py_RETURN_NONE;
	}
}

KX_PYMETHODDEF_DOC_O(KX_PythonComponent, start,
					 "start(args)\n"
					 "initializes the component")
{
	printf("base start\n");
	// We leave this empty, derived classes should define their own if they need it
	Py_RETURN_NONE;
}
#endif
