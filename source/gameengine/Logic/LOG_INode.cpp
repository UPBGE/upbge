#include "LOG_INode.h"
#include "LOG_INodeSocket.h"

#include "KX_GameObject.h"

#include "CM_Message.h"

LOG_INode::LOG_INode()
	:m_status(NO_STATUS),
	m_object(nullptr),
	m_inputsWrapper(this, EXP_BaseListWrapper::FLAG_NO_WEAK_REF),
	m_propertiesWrapper(this, EXP_BaseListWrapper::FLAG_NO_WEAK_REF)
{
}

LOG_INode::LOG_INode(const LOG_INode& other)
	:m_status(NO_STATUS),
	m_object(nullptr),
	m_inputs(other.m_inputs),
	m_properties(other.m_properties),
	m_inputsWrapper(this, EXP_BaseListWrapper::FLAG_NO_WEAK_REF),
	m_propertiesWrapper(this, EXP_BaseListWrapper::FLAG_NO_WEAK_REF)
{
}

LOG_INode::~LOG_INode()
{
}

void LOG_INode::ProcessReplica()
{
	EXP_Value::ProcessReplica();

	// Subclass the python node.
	PyObject *proxy = GetProxy();
	PyTypeObject *type = Py_TYPE(proxy);
	if (!py_base_new(type, PyTuple_Pack(1, proxy), nullptr)) {
		CM_Error("failed replicate node"); // TODO
		m_status = INIT_ERROR;
	}
}

KX_GameObject *LOG_INode::GetGameObject() const
{
	return m_object;
}

void LOG_INode::SetGameObject(KX_GameObject *gameobj)
{
	m_object = gameobj;
}

void LOG_INode::AddInput(LOG_INodeSocket *socket)
{
	m_inputs.push_back(socket);
}

void LOG_INode::AddProperty(LOG_INodeSocket *prop)
{
	m_properties.push_back(prop);
}

void LOG_INode::Start()
{
	PyObject *ret = PyObject_CallMethod(GetProxy(), "start", "");
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

unsigned int LOG_INode::py_get_inputs_size()
{
	return m_inputs.size();
}

PyObject *LOG_INode::py_get_inputs_item(unsigned int index)
{
	return m_inputs[index]->GetValue();
}

std::string LOG_INode::py_get_inputs_name(unsigned int index)
{
	return m_inputs[index]->GetName();
}

unsigned int LOG_INode::py_get_properties_size()
{
	return m_properties.size();
}

PyObject *LOG_INode::py_get_properties_item(unsigned int index)
{
	return m_properties[index]->GetValue();
}

std::string LOG_INode::py_get_properties_name(unsigned int index)
{
	return m_properties[index]->GetName();
}

PyObject *LOG_INode::pyattr_get_object(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	LOG_INode *self = static_cast<LOG_INode *>(self_v);
	KX_GameObject *gameobj = self->GetGameObject();

	if (gameobj) {
		return gameobj->GetProxy();
	}
	else {
		Py_RETURN_NONE;
	}
}

PyObject *LOG_INode::pyattr_get_inputs(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	LOG_INode *self = static_cast<LOG_INode *>(self_v);
	return self->m_inputsWrapper.GetProxy();
}

PyObject *LOG_INode::pyattr_get_properties(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	LOG_INode *self = static_cast<LOG_INode *>(self_v);
	return self->m_propertiesWrapper.GetProxy();
}

