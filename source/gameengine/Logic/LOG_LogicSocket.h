#ifndef __LOG_LOGIC_SOCKET_H__
#define __LOG_LOGIC_SOCKET_H__

#include "LOG_INodeSocket.h"

class LOG_Node;

class LOG_LogicSocket : public LOG_INodeSocket
{
	Py_Header

private:
	LOG_Node *m_node;

public:
	LOG_LogicSocket() = default;
	LOG_LogicSocket(const std::string& name, LOG_Node *node);
	LOG_LogicSocket(const LOG_LogicSocket& other);
	virtual ~LOG_LogicSocket();

	virtual EXP_Value *GetReplica();

	virtual void Relink(const std::map<LOG_INode *, LOG_INode *>& nodeMap);
	// Attributes
	static PyObject *pyattr_get_value(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
};

#endif  // __LOG_LOGIC_SOCKET_H__
