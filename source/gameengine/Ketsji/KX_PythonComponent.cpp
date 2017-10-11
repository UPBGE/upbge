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

#include "DNA_python_component_types.h"

#include "BKE_python_component.h"

KX_PythonComponent::KX_PythonComponent(const std::string& name)
	:m_pc(nullptr),
	m_gameobj(nullptr),
	m_name(name),
	m_init(false)
{
}

KX_PythonComponent::~KX_PythonComponent()
{
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

void KX_PythonComponent::SetBlenderPythonComponent(PythonComponent *pc)
{
	m_pc = pc;
}

void KX_PythonComponent::Start()
{
	PyObject *arg_dict = (PyObject *)BKE_python_component_argument_dict_new(m_pc);

	PyObject *ret = PyObject_CallMethod(GetProxy(), "start", "O", arg_dict);

	if (PyErr_Occurred()) {
		PyErr_Print();
	}

	Py_XDECREF(arg_dict);
	Py_XDECREF(ret);
}

void KX_PythonComponent::Update()
{
	if (!m_init) {
		Start();
		m_init = true;
	}

	PyObject *pycomp = GetProxy();
	if (!PyObject_CallMethod(pycomp, "update", "")) {
		PyErr_Print();
	}
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

EXP_Attribute KX_PythonComponent::Attributes[] = {
	EXP_ATTRIBUTE_RO("object", m_gameobj),
	EXP_ATTRIBUTE_NULL // Sentinel
};

#endif
