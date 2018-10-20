#include "LOG_INodeSocket.h"

LOG_INodeSocket::LOG_INodeSocket(const std::string& name)
	:m_name(name)
{
}

const std::string& LOG_INodeSocket::GetName() const
{
	return m_name;
}
