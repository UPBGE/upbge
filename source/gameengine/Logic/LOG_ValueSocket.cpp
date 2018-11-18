#include "LOG_ValueSocket.h"

#include "EXP_Python.h"

LOG_ValueSocket::LOG_ValueSocket(const std::string& name, PyObject *value)
	:LOG_INodeSocket(name),
	m_value(value)
{
	Py_XINCREF(m_value);
}

LOG_ValueSocket::LOG_ValueSocket(const LOG_ValueSocket& other)
	:LOG_INodeSocket(other),
	m_value(other.m_value)
{
	Py_XINCREF(m_value);
}

LOG_ValueSocket::~LOG_ValueSocket()
{
	Py_XDECREF(m_value);
}

EXP_Value *LOG_ValueSocket::GetReplica()
{
	EXP_Value *replica = new LOG_ValueSocket(*this);
	replica->ProcessReplica(); // TODO
	return replica;
}

PyTypeObject LOG_ValueSocket::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"LOG_ValueSocket",
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

PyMethodDef LOG_ValueSocket::Methods[] = {
	{nullptr, nullptr} // Sentinel
};

PyAttributeDef LOG_ValueSocket::Attributes[] = {
	EXP_PYATTRIBUTE_RW_FUNCTION("value", LOG_ValueSocket, pyattr_get_value, pyattr_set_value),
	EXP_PYATTRIBUTE_NULL // Sentinel
};

PyObject *LOG_ValueSocket::pyattr_get_value(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	LOG_ValueSocket *self = static_cast<LOG_ValueSocket *>(self_v);

	Py_INCREF(self->m_value);
	return self->m_value;
}

int LOG_ValueSocket::pyattr_set_value(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	LOG_ValueSocket *self = static_cast<LOG_ValueSocket *>(self_v);

	Py_XDECREF(self->m_value);
	self->m_value = value;
	Py_XINCREF(self->m_value);

	return PY_SET_ATTR_SUCCESS;
}
