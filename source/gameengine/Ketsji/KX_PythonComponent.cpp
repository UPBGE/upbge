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
 * Contributor(s): Mitchell Stokes, Diego Lopes, Tristan Porteries.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

#ifdef WITH_PYTHON

#include "KX_PythonComponent.h"
#include "KX_GameObject.h"

#include "CM_Message.h"

KX_PythonComponent::KX_PythonComponent(const std::string& name)
	:m_startArgs(nullptr),
	m_gameobj(nullptr),
	m_name(name),
	m_init(false)
{
}

KX_PythonComponent::~KX_PythonComponent()
{
	Py_XDECREF(m_startArgs);
}

std::string KX_PythonComponent::GetName() const
{
	return m_name;
}

EXP_Value *KX_PythonComponent::GetReplica()
{
	KX_PythonComponent *replica = new KX_PythonComponent(*this);
	replica->ProcessReplica();

	// Subclass the python component.
	PyTypeObject *type = Py_TYPE(GetProxy());
	if (!py_base_new(type, PyTuple_Pack(1, replica->GetProxy()), nullptr)) {
		CM_Error("failed replicate component: \"" << m_name << "\"");
		delete replica;
		return nullptr;
	}

	return replica;
}

void KX_PythonComponent::ProcessReplica()
{
	EXP_Value::ProcessReplica();
	m_gameobj = nullptr;
	m_init = false;
}

KX_GameObject *KX_PythonComponent::GetGameObject() const
{
	return m_gameobj;
}

void KX_PythonComponent::SetGameObject(KX_GameObject *gameobj)
{
	m_gameobj = gameobj;
}

void KX_PythonComponent::SetStartArgs(PyObject *args)
{
	m_startArgs = args;
}

void KX_PythonComponent::Start()
{
	PyObject *ret = PyObject_CallMethod(GetProxy(), "start", "O", m_startArgs);
	if (PyErr_Occurred()) {
		PyErr_Print();
	}
	Py_XDECREF(ret);
}

void KX_PythonComponent::Update()
{
	if (!m_init) {
		Start();
		m_init = true;
	}

	PyObject *ret = PyObject_CallMethod(GetProxy(), "update", "");
	if (PyErr_Occurred()) {
		PyErr_Print();
	}
	Py_XDECREF(ret);
}

PyObject *KX_PythonComponent::py_component_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
	KX_PythonComponent *comp = new KX_PythonComponent(type->tp_name);

	PyObject *proxy = py_base_new(type, PyTuple_Pack(1, comp->GetProxy()), kwds);
	if (!proxy) {
		delete comp;
		return nullptr;
	}

	return proxy;
}

PyTypeObject KX_PythonComponent::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"KX_PythonComponent",
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
	py_component_new
};

PyMethodDef KX_PythonComponent::Methods[] = {
	{nullptr, nullptr} // Sentinel
};

PyAttributeDef KX_PythonComponent::Attributes[] = {
	EXP_PYATTRIBUTE_RO_FUNCTION("object", KX_PythonComponent, pyattr_get_object),
	EXP_PYATTRIBUTE_NULL // Sentinel
};

PyObject *KX_PythonComponent::pyattr_get_object(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_PythonComponent *self = static_cast<KX_PythonComponent *>(self_v);
	KX_GameObject *gameobj = self->GetGameObject();

	if (gameobj) {
		return gameobj->GetProxy();
	}
	else {
		Py_RETURN_NONE;
	}
}
#endif
