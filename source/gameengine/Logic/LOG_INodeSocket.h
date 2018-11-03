#ifndef __LOG_INODE_SOCKET_H__
#define __LOG_INODE_SOCKET_H__

#include <string>

#include "EXP_Value.h"

class LOG_INodeSocket : public EXP_Value
{
	Py_Header

private:
	std::string m_name;

public:
	LOG_INodeSocket() = default;
	LOG_INodeSocket(const std::string& name);
	LOG_INodeSocket(const LOG_INodeSocket& other) = default;
	virtual ~LOG_INodeSocket() = default;

	virtual std::string GetName() const;
};

#endif  // __LOG_NODE_SOCKET_H__
