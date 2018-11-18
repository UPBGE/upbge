#include "LOG_INodeSocket.h"

LOG_INodeSocket::LOG_INodeSocket(const std::string& name)
	:m_name(name)
{
}

std::string LOG_INodeSocket::GetName() const
{
	return m_name;
}

void LOG_INodeSocket::Relink(const std::map<LOG_INode *, LOG_INode *>& nodeMap)
{
}

PyTypeObject LOG_INodeSocket::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"LOG_INodeSocket",
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
	&EXP_Value::Type,
	0, 0, 0, 0, 0, 0,
	py_base_new
};

PyMethodDef LOG_INodeSocket::Methods[] = {
	{nullptr, nullptr} // Sentinel
};

PyAttributeDef LOG_INodeSocket::Attributes[] = {
	EXP_PYATTRIBUTE_NULL // Sentinel
};
