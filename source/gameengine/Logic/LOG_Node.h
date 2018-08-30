#ifndef __LOG_NODE_H__
#define __LOG_NODE_H__

#include "EXP_Value.h"
#include "EXP_ListWrapper.h"

class LOG_NodeSocket;
class KX_GameObject;

class LOG_Node : public EXP_Value
{
	Py_Header

private:
	KX_GameObject *m_object;

	std::vector<LOG_NodeSocket *> m_inputs;
	std::vector<LOG_NodeSocket *> m_outputs;
	std::vector<LOG_NodeSocket *> m_properties;

	EXP_BaseListWrapper *m_inputsWrapper;
	EXP_BaseListWrapper *m_outputsWrapper;
	EXP_BaseListWrapper *m_propertiesWrapper;

public:
	LOG_Node();
	virtual ~LOG_Node();

	virtual std::string GetName() const;
	virtual EXP_Value *GetReplica();

	void ProcessReplica();

	KX_GameObject *GetGameObject() const;
	void SetGameObject(KX_GameObject *gameobj);

	void AddInput(LOG_NodeSocket *socket);
	void AddOutput(LOG_NodeSocket *socket);

	void Start();
	/// Update the outputs socket and return the next node to execute.
	LOG_Node *Update();

	static PyObject *py_node_new(PyTypeObject *type, PyObject *args, PyObject *kwds);

	unsigned int py_get_inputs_size();
	PyObject *py_get_inputs_item(unsigned int index);
	std::string py_get_inputs_name(unsigned int index);

	unsigned int py_get_outputs_size();
	PyObject *py_get_outputs_item(unsigned int index);
	bool py_set_outputs_item(unsigned int index, PyObject *value);
	std::string py_get_outputs_name(unsigned int index);

	unsigned int py_get_properties_size();
	PyObject *py_get_properties_item(unsigned int index);
	bool py_set_properties_item(unsigned int index, PyObject *value);
	std::string py_get_properties_name(unsigned int index);

	// Attributes
	static PyObject *pyattr_get_object(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_inputs(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_outputs(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_properties(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
};

#endif  // __LOG_NODE_H__
