#ifndef __LOG_INODE_SOCKET_H__
#define __LOG_INODE_SOCKET_H__

#include <string>

struct PyObject;

class LOG_INodeSocket
{
private:
	std::string m_name;

public:
	LOG_INodeSocket() = default;
	LOG_INodeSocket(const std::string& name);
	virtual ~LOG_INodeSocket() = default;

	LOG_INodeSocket(const LOG_INodeSocket& other) = default;

	const std::string& GetName() const;

	virtual PyObject *GetValue() const = 0;
};

#endif  // __LOG_NODE_SOCKET_H__
