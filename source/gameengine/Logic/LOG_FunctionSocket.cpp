#include "LOG_FunctionSocket.h"
#include "LOG_FunctionNode.h"

LOG_FunctionSocket::LOG_FunctionSocket(const std::string& name, LOG_FunctionNode *node)
	:LOG_INodeSocket(name),
	m_node(node)
{
}

LOG_FunctionSocket::~LOG_FunctionSocket()
{
}

LOG_FunctionSocket::LOG_FunctionSocket(const LOG_FunctionSocket& other)
	:LOG_INodeSocket(other),
	m_node(other.m_node)
{
}

PyObject *LOG_FunctionSocket::GetValue() const
{
	return m_node->GetValue();
}
