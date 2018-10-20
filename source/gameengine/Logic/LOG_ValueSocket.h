#ifndef __LOG_VALUE_SOCKET_H__
#define __LOG_VALUE_SOCKET_H__

#include "LOG_INodeSocket.h"

class LOG_ValueSocket : public LOG_INodeSocket
{
private:
	PyObject *m_value;

public:
	LOG_ValueSocket() = default;
	LOG_ValueSocket(const std::string& name, PyObject *value);
	virtual ~LOG_ValueSocket();

	LOG_ValueSocket(const LOG_ValueSocket& other);

	virtual PyObject *GetValue() const;
};

#endif  // __LOG_VALUE_SOCKET_H__
