#include "LOG_FunctionSocket.h"
#include "LOG_FunctionNode.h"

LOG_FunctionSocket::LOG_FunctionSocket(const std::string& name, LOG_FunctionNode *node)
	:LOG_INodeSocket(name),
	m_node(node)
{
}

LOG_FunctionSocket::LOG_FunctionSocket(const LOG_FunctionSocket& other)
	:LOG_INodeSocket(other),
	m_node(other.m_node)
{
}

LOG_FunctionSocket::~LOG_FunctionSocket()
{
}

EXP_Value *LOG_FunctionSocket::GetReplica()
{
	EXP_Value *replica = new LOG_FunctionSocket(*this);
	replica->ProcessReplica();
	return replica;
}

void LOG_FunctionSocket::Relink(const std::map<LOG_INode *, LOG_INode *>& nodeMap)
{
	m_node = static_cast<LOG_FunctionNode *>(nodeMap.at(m_node));
}

PyTypeObject LOG_FunctionSocket::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"LOG_FunctionSocket",
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
	&LOG_INodeSocket::Type,
	0, 0, 0, 0, 0, 0,
	py_base_new
};

PyMethodDef LOG_FunctionSocket::Methods[] = {
	{nullptr, nullptr} // Sentinel
};

PyAttributeDef LOG_FunctionSocket::Attributes[] = {
	EXP_PYATTRIBUTE_RO_FUNCTION("value", LOG_FunctionSocket, pyattr_get_value),
	EXP_PYATTRIBUTE_NULL // Sentinel
};

PyObject *LOG_FunctionSocket::pyattr_get_value(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	LOG_FunctionSocket *self = static_cast<LOG_FunctionSocket *>(self_v);

	if (self->m_node) {
		return self->m_node->GetValue();
	}

	Py_RETURN_NONE;
}
