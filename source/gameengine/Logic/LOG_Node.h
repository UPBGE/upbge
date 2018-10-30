#ifndef __LOG_NODE_H__
#define __LOG_NODE_H__

#include "LOG_INode.h"

class LOG_ValueSocket;

class LOG_Node : public LOG_INode
{
	Py_Header

protected:
	std::vector<LOG_ValueSocket *> m_outputs;

public:
	LOG_Node();
	LOG_Node(const LOG_Node& other);
	virtual ~LOG_Node();

	virtual NodeType GetType() const;
	virtual std::string GetName() const;
	virtual EXP_Value *GetReplica();

	void AddOutput(LOG_ValueSocket *socket);

	/// Update the outputs socket and return the next node to execute.
	LOG_Node *Update();

	static PyObject *py_node_new(PyTypeObject *type, PyObject *args, PyObject *kwds);

	unsigned int py_get_outputs_size();
	PyObject *py_get_outputs_item(unsigned int index);
	bool py_set_outputs_item(unsigned int index, PyObject *value);
	std::string py_get_outputs_name(unsigned int index);

	// Attributes
	static PyObject *pyattr_get_outputs(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);

protected:
	EXP_ListWrapper<LOG_Node, &LOG_Node::py_get_outputs_size, &LOG_Node::py_get_outputs_item,
		&LOG_Node::py_set_outputs_item, &LOG_Node::py_get_outputs_name> m_outputsWrapper;
};

#endif  // __LOG_NODE_H__
