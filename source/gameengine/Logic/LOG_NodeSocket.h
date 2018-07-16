#ifndef __LOG_NODE_SOCKET_H__
#define __LOG_NODE_SOCKET_H__

#include "EXP_Python.h"

#include <string>

class LOG_NodeSocket
{
private:
	std::string m_name;
	PyObject *m_value;

public:
	LOG_NodeSocket() = default;
	LOG_NodeSocket(const std::string& name, PyObject *value);
	~LOG_NodeSocket();

	LOG_NodeSocket(const LOG_NodeSocket& other);

	const std::string& GetName() const;

	PyObject *GetValue() const;
	void SetValue(PyObject *value);
};

#endif  // __LOG_NODE_SOCKET_H__
