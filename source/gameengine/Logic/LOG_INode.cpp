#include "LOG_INode.h"
#include "LOG_INodeSocket.h"

#include "LOG_Object.h"

#include "CM_Message.h"

LOG_INode::LOG_INode()
	:m_status(NO_STATUS),
	m_object(nullptr)
{
	m_properties = PyDict_New();
}

LOG_INode::LOG_INode(const LOG_INode& other)
	:m_status(NO_STATUS),
	m_object(nullptr),
	m_inputs(other.m_inputs)
{
	m_properties = PyDict_Copy(other.m_properties);
}

LOG_INode::~LOG_INode()
{
	Py_DECREF(m_properties);
}

void LOG_INode::Relink(std::map<LOG_INode *, LOG_INode *>& nodeMap, std::map<LOG_INodeSocket *, LOG_INodeSocket *>& socketMap)
{
	RelinkSockets(nodeMap, socketMap, m_inputs);
}

LOG_Object *LOG_INode::GetObject() const
{
	return m_object;
}

void LOG_INode::SetObject(LOG_Object *obj)
{
	m_object = obj;
}

void LOG_INode::AddInput(LOG_INodeSocket *socket)
{
	m_inputs.Add(socket);
}

void LOG_INode::AddProperty(const std::string& name, PyObject *value)
{
	Py_INCREF(value);
	PyDict_SetItemString(m_properties, name.c_str(), value);
}

void LOG_INode::Start()
{
	PyObject *ret = PyObject_CallMethod(GetProxy(), "start", nullptr);

	if (PyErr_Occurred()) {
		PyErr_Print();
	}

	Py_XDECREF(ret);
}

PyTypeObject LOG_INode::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"LOG_INode",
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

PyMethodDef LOG_INode::Methods[] = {
	{nullptr, nullptr} // Sentinel
};

PyAttributeDef LOG_INode::Attributes[] = {
	EXP_PYATTRIBUTE_RO_FUNCTION("object", LOG_INode, pyattr_get_object),
	EXP_PYATTRIBUTE_RO_FUNCTION("inputs", LOG_INode, pyattr_get_inputs),
	EXP_PYATTRIBUTE_RO_FUNCTION("properties", LOG_INode, pyattr_get_properties),
	EXP_PYATTRIBUTE_NULL // Sentinel
};

PyObject *LOG_INode::pyattr_get_object(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	LOG_INode *self = static_cast<LOG_INode *>(self_v);
	LOG_Object *obj = self->GetObject();

	if (obj) {
		return obj->GetProxy();
	}
	else {
		Py_RETURN_NONE;
	}
}

PyObject *LOG_INode::pyattr_get_inputs(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	LOG_INode *self = static_cast<LOG_INode *>(self_v);
	return self->m_inputs.GetProxy();
}

PyObject *LOG_INode::pyattr_get_properties(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	LOG_INode *self = static_cast<LOG_INode *>(self_v);

	Py_INCREF(self->m_properties);
	return self->m_properties;
}

