#ifndef __LOG_FUNCTION_SOCKET_H__
#define __LOG_FUNCTION_SOCKET_H__

#include "LOG_INodeSocket.h"

class LOG_FunctionNode;

class LOG_FunctionSocket : public LOG_INodeSocket
{
private:
	LOG_FunctionNode *m_node;

public:
	LOG_FunctionSocket() = default;
	LOG_FunctionSocket(const std::string& name, LOG_FunctionNode *node);
	virtual ~LOG_FunctionSocket();

	LOG_FunctionSocket(const LOG_FunctionSocket& other);

	virtual PyObject *GetValue() const;
};

#endif  // __LOG_FUNCTION_SOCKET_H__
