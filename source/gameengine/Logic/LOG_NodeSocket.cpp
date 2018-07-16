#include "LOG_NodeSocket.h"

LOG_NodeSocket::LOG_NodeSocket(const std::string& name, PyObject *value)
	:m_name(name),
	m_value(value)
{
	Py_XINCREF(m_value);
}

LOG_NodeSocket::~LOG_NodeSocket()
{
	Py_XDECREF(m_value);
}

LOG_NodeSocket::LOG_NodeSocket(const LOG_NodeSocket& other)
	:m_name(other.m_name),
	m_value(other.m_value)
{
	Py_XINCREF(m_value);
}

const std::string& LOG_NodeSocket::GetName() const
{
	return m_name;
}

void LOG_NodeSocket::SetValue(PyObject *value)
{
	Py_XDECREF(m_value);
	m_value = value;
	Py_XINCREF(m_value);
}

PyObject *LOG_NodeSocket::GetValue() const
{
	return m_value;
}
