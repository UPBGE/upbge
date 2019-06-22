#include "LOG_Node.h"
#include "LOG_ValueSocket.h"

#include "CM_Message.h"

LOG_Node::LOG_Node()
	:m_updateMeth(nullptr)
{
}

LOG_Node::LOG_Node(const LOG_Node& other)
	:LOG_INode(other),
	m_outputs(other.m_outputs),
	m_updateMeth(nullptr)
{
}

LOG_Node::~LOG_Node()
{
	Py_XDECREF(m_updateMeth);
}

LOG_INode::NodeType LOG_Node::GetNodeType() const
{
	return TYPE_NODE;
}

std::string LOG_Node::GetName() const
{
	return "LOG_Node";
}

EXP_Value *LOG_Node::GetReplica()
{
	LOG_Node *replica = new LOG_Node(*this);
	replica->ProcessReplica();

	// Subclass the python node.
	PyTypeObject *type = Py_TYPE(GetProxy());
	if (!py_base_new(type, PyTuple_Pack(1, replica->GetProxy()), nullptr)) {
		delete replica;
		return nullptr;
	}

	return replica;
}

void LOG_Node::Relink(std::map<LOG_INode *, LOG_INode *>& nodeMap, std::map<LOG_INodeSocket *, LOG_INodeSocket *>& socketMap)
{
	LOG_INode::Relink(nodeMap, socketMap);

	RelinkSockets(nodeMap, socketMap, m_outputs);
}

void LOG_Node::AddOutput(LOG_ValueSocket *socket)
{
	m_outputs.Add(socket);
}

void LOG_Node::Start()
{
	LOG_INode::Start();

	m_updateMeth = PyObject_GetAttrString(GetProxy(), "update");
}

LOG_Node *LOG_Node::Update()
{
	LOG_Node *nextNode = nullptr;

	PyObject *ret = PyObject_CallObject(m_updateMeth, nullptr);

	if (PyErr_Occurred()) {
		PyErr_Print();
	}
	else if (PyObject_TypeCheck(ret, &LOG_Node::Type)) {
		nextNode = static_cast<LOG_Node *>EXP_PROXY_REF(ret);

		if (!nextNode) {
			PyErr_SetString(PyExc_SystemError, EXP_PROXY_ERROR_MSG);
		}
	}
	else if (ret != Py_None) {
		CM_Error("failed get next logic node"); // TODO
	}

	Py_XDECREF(ret);

	return nextNode;
}

PyObject *LOG_Node::py_node_new(PyTypeObject *type, PyObject *_args, PyObject *kwds)
{
	LOG_Node *node = new LOG_Node();

	PyObject *args = PyTuple_Pack(1, node->GetProxy());
	PyObject *proxy = py_base_new(type, args, kwds);
	Py_DECREF(args);

	if (!proxy) {
		delete node;
		return nullptr;
	}

	return proxy;
}

PyTypeObject LOG_Node::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"LOG_Node",
	sizeof(EXP_PyObjectPlus_Proxy),
	0,
	py_base_dealloc,
	0,
	0,
	0,
	0,
	py_base_repr,
	0, 0, 0, 0, 0, 0, 0, 0, 0,
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	0, 0, 0, 0, 0, 0, 0,
	Methods,
	0,
	0,
	&LOG_INode::Type,
	0, 0, 0, 0, 0, 0,
	py_node_new
};

PyMethodDef LOG_Node::Methods[] = {
	{nullptr, nullptr} // Sentinel
};

PyAttributeDef LOG_Node::Attributes[] = {
	EXP_PYATTRIBUTE_RO_FUNCTION("outputs", LOG_Node, pyattr_get_outputs),
	EXP_PYATTRIBUTE_NULL // Sentinel
};

PyObject *LOG_Node::pyattr_get_outputs(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	LOG_Node *self = static_cast<LOG_Node *>(self_v);
	return self->m_outputs.GetProxy();
}
