#ifndef __LOG_INODE_SOCKET_H__
#define __LOG_INODE_SOCKET_H__

#include <string>

#include "EXP_Value.h"
#include "LOG_INode.h" // For LOG_Node::NodeToNodeMap.

class LOG_INodeSocket : public EXP_Value
{
	Py_Header

private:
	std::string m_name;

public:
	LOG_INodeSocket() = default;
	LOG_INodeSocket(const std::string& name);
	LOG_INodeSocket(const LOG_INodeSocket& other) = default;
	virtual ~LOG_INodeSocket() = default;

	virtual std::string GetName() const;

	virtual void Relink(const std::map<LOG_INode *, LOG_INode *>& nodeMap);
};


#endif  // __LOG_NODE_SOCKET_H__
