#include "LOG_Node.h"

#include "KX_GameObject.h"

#include "CM_Message.h"

LOG_Node::LOG_Node()
	:m_object(nullptr),
	m_inputsWrapper(new EXP_ListWrapper<LOG_Node, &LOG_Node::py_get_inputs_size, &LOG_Node::py_get_inputs_item,
			nullptr, &LOG_Node::py_get_inputs_name>(this, EXP_BaseListWrapper::FLAG_NO_WEAK_REF)),
	m_outputsWrapper(new EXP_ListWrapper<LOG_Node, &LOG_Node::py_get_outputs_size, &LOG_Node::py_get_outputs_item,
			&LOG_Node::py_set_outputs_item, &LOG_Node::py_get_outputs_name>(this, EXP_BaseListWrapper::FLAG_NO_WEAK_REF)),
	m_propertiesWrapper(new EXP_ListWrapper<LOG_Node, &LOG_Node::py_get_properties_size, &LOG_Node::py_get_properties_item,
			&LOG_Node::py_set_properties_item, &LOG_Node::py_get_properties_name>(this, EXP_BaseListWrapper::FLAG_NO_WEAK_REF))
{
}

LOG_Node::~LOG_Node()
{
}

std::string LOG_Node::GetName() const
{
	return "LOG_Node";
}

EXP_Value *LOG_Node::GetReplica()
{
	LOG_Node *replica = new LOG_Node(*this);
	replica->ProcessReplica();

	// Subclass the python node.
	PyTypeObject *type = Py_TYPE(GetProxy());
	if (!py_base_new(type, PyTuple_Pack(1, replica->GetProxy()), nullptr)) {
		CM_Error("failed replicate node");
		delete replica;
		return nullptr;
	}

	return replica;
}

void LOG_Node::ProcessReplica()
{
	EXP_Value::ProcessReplica();
	m_object = nullptr;
}

KX_GameObject *LOG_Node::GetGameObject() const
{
	return m_object;
}

void LOG_Node::SetGameObject(KX_GameObject *gameobj)
{
	m_object = gameobj;
}

void LOG_Node::AddInput(const LOG_NodeSocket& socket)
{
	m_inputs.push_back(socket);
}

void LOG_Node::AddOutput(const LOG_NodeSocket& socket)
{
	m_outputs.push_back(socket);
}

void LOG_Node::Start()
{
	PyObject *ret = PyObject_CallMethod(GetProxy(), "start", "");
	if (PyErr_Occurred()) {
		PyErr_Print();
	}
	Py_XDECREF(ret);
}

LOG_Node *LOG_Node::Update()
{
	LOG_Node *nextNode = nullptr;

	PyObject *ret = PyObject_CallMethod(GetProxy(), "update", "");
	if (PyErr_Occurred()) {
		PyErr_Print();
	}
	else if (PyObject_TypeCheck(ret, &LOG_Node::Type)) {
		nextNode = static_cast<LOG_Node *>EXP_PROXY_REF(ret);

		/* sets the error */
		if (nextNode == nullptr) {
			PyErr_SetString(PyExc_SystemError, EXP_PROXY_ERROR_MSG);
		}
	}
	else if (ret != Py_None) {
		CM_Error("failed get next logic node");
	}

	Py_XDECREF(ret);

	return nextNode;
}

PyObject *LOG_Node::py_node_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
	LOG_Node *node = new LOG_Node();

	PyObject *proxy = py_base_new(type, PyTuple_Pack(1, node->GetProxy()), kwds);
	if (!proxy) {
		delete node;
		return nullptr;
	}

	return proxy;
}

PyTypeObject LOG_Node::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"LOG_Node",
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
	py_node_new
};

PyMethodDef LOG_Node::Methods[] = {
	{nullptr, nullptr} // Sentinel
};

PyAttributeDef LOG_Node::Attributes[] = {
	EXP_PYATTRIBUTE_RO_FUNCTION("object", LOG_Node, pyattr_get_object),
	EXP_PYATTRIBUTE_RO_FUNCTION("inputs", LOG_Node, pyattr_get_inputs),
	EXP_PYATTRIBUTE_RO_FUNCTION("outputs", LOG_Node, pyattr_get_outputs),
	EXP_PYATTRIBUTE_RO_FUNCTION("properties", LOG_Node, pyattr_get_properties),
	EXP_PYATTRIBUTE_NULL // Sentinel
};

unsigned int LOG_Node::py_get_inputs_size()
{
	return m_inputs.size();
}

PyObject *LOG_Node::py_get_inputs_item(unsigned int index)
{
	return m_inputs[index].GetValue();
}

std::string LOG_Node::py_get_inputs_name(unsigned int index)
{
	return m_inputs[index].GetName();
}

unsigned int LOG_Node::py_get_outputs_size()
{
	return m_outputs.size();
}

PyObject *LOG_Node::py_get_outputs_item(unsigned int index)
{
	return m_outputs[index].GetValue();
}

bool LOG_Node::py_set_outputs_item(unsigned int index, PyObject *value)
{
	m_outputs[index].SetValue(value);
	return true;
}

std::string LOG_Node::py_get_outputs_name(unsigned int index)
{
	return m_outputs[index].GetName();
}

unsigned int LOG_Node::py_get_properties_size()
{
	return m_properties.size();
}

PyObject *LOG_Node::py_get_properties_item(unsigned int index)
{
	return m_properties[index].GetValue();
}

bool LOG_Node::py_set_properties_item(unsigned int index, PyObject *value)
{
	m_properties[index].SetValue(value);
	return true;
}

std::string LOG_Node::py_get_properties_name(unsigned int index)
{
	return m_properties[index].GetName();
}

PyObject *LOG_Node::pyattr_get_object(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	LOG_Node *self = static_cast<LOG_Node *>(self_v);
	KX_GameObject *gameobj = self->GetGameObject();

	if (gameobj) {
		return gameobj->GetProxy();
	}
	else {
		Py_RETURN_NONE;
	}
}

PyObject *LOG_Node::pyattr_get_inputs(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	LOG_Node *self = static_cast<LOG_Node *>(self_v);
	return self->m_inputsWrapper->GetProxy();
}

PyObject *LOG_Node::pyattr_get_outputs(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	LOG_Node *self = static_cast<LOG_Node *>(self_v);
	return self->m_outputsWrapper->GetProxy();
}

PyObject *LOG_Node::pyattr_get_properties(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	LOG_Node *self = static_cast<LOG_Node *>(self_v);
	return self->m_propertiesWrapper->GetProxy();
}


