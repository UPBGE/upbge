#include "LOG_ValueSocket.h"

#include "EXP_Python.h"

LOG_ValueSocket::LOG_ValueSocket(const std::string& name, PyObject *value)
	:LOG_INodeSocket(name),
	m_value(value)
{
	Py_XINCREF(m_value);
}

LOG_ValueSocket::~LOG_ValueSocket()
{
	Py_XDECREF(m_value);
}

LOG_ValueSocket::LOG_ValueSocket(const LOG_ValueSocket& other)
	:LOG_INodeSocket(other),
	m_value(other.m_value)
{
	Py_XINCREF(m_value);
}

void LOG_ValueSocket::SetValue(PyObject *value)
{
	Py_XDECREF(m_value);
	m_value = value;
	Py_XINCREF(m_value);
}

PyObject *LOG_ValueSocket::GetValue() const
{
	return m_value;
}
