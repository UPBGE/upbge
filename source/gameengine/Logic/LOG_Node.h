#ifndef __LOG_NODE_H__
#define __LOG_NODE_H__

#include "LOG_BaseNode.h"

class LOG_Node : public LOG_BaseNode
{
	Py_Header

protected:
	std::vector<LOG_NodeSocket *> m_outputs;

	std::unique_ptr<EXP_BaseListWrapper> m_outputsWrapper;

public:
	LOG_Node();
	LOG_Node(const LOG_Node& other);
	virtual ~LOG_Node();

	virtual std::string GetName() const;
	virtual EXP_Value *GetReplica();

	bool ProcessReplica();

	void AddOutput(LOG_NodeSocket *socket);

	/// Update the outputs socket and return the next node to execute.
	LOG_Node *Update();

	static PyObject *py_node_new(PyTypeObject *type, PyObject *args, PyObject *kwds);

	unsigned int py_get_outputs_size();
	PyObject *py_get_outputs_item(unsigned int index);
	bool py_set_outputs_item(unsigned int index, PyObject *value);
	std::string py_get_outputs_name(unsigned int index);

	// Attributes
	static PyObject *pyattr_get_outputs(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
};

#endif  // __LOG_NODE_H__
