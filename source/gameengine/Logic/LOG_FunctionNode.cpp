#include "LOG_FunctionNode.h"

#include "CM_Message.h"

LOG_FunctionNode::LOG_FunctionNode()
	:m_getMeth(nullptr)
{
}

LOG_FunctionNode::LOG_FunctionNode(const LOG_FunctionNode& other)
	:m_getMeth(nullptr)
{
}

LOG_FunctionNode::~LOG_FunctionNode()
{
	Py_XDECREF(m_getMeth);
}

LOG_INode::NodeType LOG_FunctionNode::GetNodeType() const
{
	return TYPE_FUNCTION;
}

std::string LOG_FunctionNode::GetName() const
{
	return "LOG_FunctionNode";
}

EXP_Value *LOG_FunctionNode::GetReplica()
{
	LOG_FunctionNode *replica = new LOG_FunctionNode(*this);
	replica->ProcessReplica();

	return replica;
}

void LOG_FunctionNode::Start()
{
	LOG_INode::Start();

	m_getMeth = PyObject_GetAttrString(GetProxy(), "get");
}

PyObject *LOG_FunctionNode::GetValue()
{
	PyObject *ret = PyObject_CallObject(m_getMeth, nullptr);

	if (PyErr_Occurred()) {
		PyErr_Print();

		Py_INCREF(Py_None);
		return Py_None;
	}

	return ret;
}

PyObject *LOG_FunctionNode::py_node_new(PyTypeObject *type, PyObject *_args, PyObject *kwds)
{
	LOG_FunctionNode *node = new LOG_FunctionNode();

	PyObject *args = PyTuple_Pack(1, node->GetProxy());
	PyObject *proxy = py_base_new(type, args, kwds);
	Py_DECREF(args);

	if (!proxy) {
		delete node;
		return nullptr;
	}

	return proxy;
}

PyTypeObject LOG_FunctionNode::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"LOG_FunctionNode",
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

PyMethodDef LOG_FunctionNode::Methods[] = {
	{nullptr, nullptr} // Sentinel
};

PyAttributeDef LOG_FunctionNode::Attributes[] = {
	EXP_PYATTRIBUTE_NULL // Sentinel
};
