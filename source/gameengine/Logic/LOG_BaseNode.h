#ifndef __LOG_BASE_NODE_H__
#define __LOG_BASE_NODE_H__

#include "EXP_ListWrapper.h"

class LOG_INodeSocket;
class KX_GameObject;

class LOG_BaseNode : public EXP_Value
{
	Py_Header;

protected:
	enum Status {
		NO_STATUS,
		INIT_ERROR,
		INIT_SUCESS
	} m_status;

	KX_GameObject *m_object;

	std::vector<LOG_INodeSocket *> m_inputs;
	std::vector<LOG_INodeSocket *> m_properties;

public:
	LOG_BaseNode();
	LOG_BaseNode(const LOG_BaseNode& other);
	virtual ~LOG_BaseNode();

	virtual void ProcessReplica();

	KX_GameObject *GetGameObject() const;
	void SetGameObject(KX_GameObject *gameobj);

	void AddInput(LOG_INodeSocket *socket);

	void Start();

	unsigned int py_get_inputs_size();
	PyObject *py_get_inputs_item(unsigned int index);
	std::string py_get_inputs_name(unsigned int index);

	unsigned int py_get_properties_size();
	PyObject *py_get_properties_item(unsigned int index);
	std::string py_get_properties_name(unsigned int index);

	// Attributes
	static PyObject *pyattr_get_object(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_inputs(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_properties(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);

protected:
	EXP_ListWrapper<LOG_BaseNode, &LOG_BaseNode::py_get_inputs_size,
		&LOG_BaseNode::py_get_inputs_item, nullptr, &LOG_BaseNode::py_get_inputs_name> m_inputsWrapper;
	EXP_ListWrapper<LOG_BaseNode, &LOG_BaseNode::py_get_properties_size,
		&LOG_BaseNode::py_get_properties_item, nullptr, &LOG_BaseNode::py_get_properties_name> m_propertiesWrapper;
};

#endif  // __LOG_BASE_NODE_H__
