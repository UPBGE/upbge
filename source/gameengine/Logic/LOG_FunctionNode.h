#ifndef __LOG_FUNCTION_NODE_H__
#define __LOG_FUNCTION_NODE_H__

#include "LOG_INode.h"

class LOG_FunctionNode : public LOG_INode
{
	Py_Header

private:
	PyObject *m_getMeth;

public:
	LOG_FunctionNode();
	LOG_FunctionNode(const LOG_FunctionNode& other);
	virtual ~LOG_FunctionNode();

	virtual NodeType GetNodeType() const;
	virtual std::string GetName() const;
	virtual EXP_Value *GetReplica();

	virtual void Start();

	PyObject *GetValue();

	static PyObject *py_node_new(PyTypeObject *type, PyObject *args, PyObject *kwds);
};

#endif  // __LOG_FUNCTION_NODE_H__
