#ifndef __LOG_NODE_H__
#define __LOG_NODE_H__

#include "LOG_INode.h"

class LOG_ValueSocket;

class LOG_Node : public LOG_INode
{
	Py_Header

protected:
	/// Output sockets.
	EXP_ListValue<LOG_ValueSocket> m_outputs;
	/// Python object for the update() function.
	PyObject *m_updateMeth;

public:
	LOG_Node();
	LOG_Node(const LOG_Node& other);
	virtual ~LOG_Node();

	virtual NodeType GetNodeType() const;
	virtual std::string GetName() const;
	virtual EXP_Value *GetReplica();

	void AddOutput(LOG_ValueSocket *socket);

	virtual void Start();

	/// Update the outputs socket and return the next node to execute.
	LOG_Node *Update();

	static PyObject *py_node_new(PyTypeObject *type, PyObject *_args, PyObject *kwds);

	// Attributes
	static PyObject *pyattr_get_outputs(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
};

#endif  // __LOG_NODE_H__
