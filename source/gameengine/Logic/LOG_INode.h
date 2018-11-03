#ifndef __LOG_INODE_H__
#define __LOG_INODE_H__

#include "EXP_ListValue.h"

class LOG_INodeSocket;
class LOG_Object;

class LOG_INode : public EXP_Value
{
	Py_Header;

protected:
	enum Status
	{
		NO_STATUS,
		INIT_ERROR,
		INIT_SUCESS
	} m_status;

	LOG_Object *m_object;

	EXP_ListValue<LOG_INodeSocket> m_inputs;

	PyObject *m_properties;

public:
	enum NodeType
	{
		TYPE_NODE,
		TYPE_FUNCTION
	};

	LOG_INode();
	LOG_INode(const LOG_INode& other);
	virtual ~LOG_INode();

	virtual NodeType GetNodeType() const = 0;

	virtual void ProcessReplica();

	LOG_Object *GetObject() const;
	void SetObject(LOG_Object *obj);

	void AddInput(LOG_INodeSocket *socket);
	void AddProperty(const std::string& name, PyObject *value);

	virtual void Start();

	// Attributes
	static PyObject *pyattr_get_object(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_inputs(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_properties(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
};

#endif  // __LOG_INODE_H__
