#ifndef __LOG_FUNCTION_SOCKET_H__
#define __LOG_FUNCTION_SOCKET_H__

#include "LOG_INodeSocket.h"

class LOG_FunctionNode;

class LOG_FunctionSocket : public LOG_INodeSocket
{
	Py_Header

private:
	LOG_FunctionNode *m_node;

public:
	LOG_FunctionSocket() = default;
	LOG_FunctionSocket(const std::string& name, LOG_FunctionNode *node);
	LOG_FunctionSocket(const LOG_FunctionSocket& other);
	virtual ~LOG_FunctionSocket();

	// Attributes
	static PyObject *pyattr_get_value(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
};

#endif  // __LOG_FUNCTION_SOCKET_H__
