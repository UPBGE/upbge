#ifndef __LOG_VALUE_SOCKET_H__
#define __LOG_VALUE_SOCKET_H__

#include "LOG_INodeSocket.h"

class LOG_ValueSocket : public LOG_INodeSocket
{
	Py_Header

private:
	PyObject *m_value;

public:
	LOG_ValueSocket() = default;
	LOG_ValueSocket(const std::string& name, PyObject *value);
	LOG_ValueSocket(const LOG_ValueSocket& other);
	virtual ~LOG_ValueSocket();

	virtual EXP_Value *GetReplica();

	// Attributes
	static PyObject *pyattr_get_value(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_value(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
};

#endif  // __LOG_VALUE_SOCKET_H__
