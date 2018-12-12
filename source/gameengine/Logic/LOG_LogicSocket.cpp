#include "LOG_LogicSocket.h"
#include "LOG_Node.h"

#include "EXP_Python.h"

LOG_LogicSocket::LOG_LogicSocket(const std::string& name, LOG_Node *node)
	:LOG_INodeSocket(name),
	m_node(node)
{
}

LOG_LogicSocket::LOG_LogicSocket(const LOG_LogicSocket& other)
	:LOG_INodeSocket(other),
	m_node(other.m_node)
{
}

LOG_LogicSocket::~LOG_LogicSocket()
{
}

EXP_Value *LOG_LogicSocket::GetReplica()
{
	EXP_Value *replica = new LOG_LogicSocket(*this);
	replica->ProcessReplica();
	return replica;
}

void LOG_LogicSocket::Relink(const std::map<LOG_INode *, LOG_INode *>& nodeMap)
{
	m_node = static_cast<LOG_Node *>(nodeMap.at(m_node));
}

PyTypeObject LOG_LogicSocket::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"LOG_LogicSocket",
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

PyMethodDef LOG_LogicSocket::Methods[] = {
	{nullptr, nullptr} // Sentinel
};

PyAttributeDef LOG_LogicSocket::Attributes[] = {
	EXP_PYATTRIBUTE_RO_FUNCTION("value", LOG_LogicSocket, pyattr_get_value),
	EXP_PYATTRIBUTE_NULL // Sentinel
};

PyObject *LOG_LogicSocket::pyattr_get_value(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	LOG_LogicSocket *self = static_cast<LOG_LogicSocket *>(self_v);

	if (self->m_node) {
		return self->m_node->GetProxy();
	}

	Py_RETURN_NONE;
}
