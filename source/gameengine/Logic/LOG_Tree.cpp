#include "LOG_Tree.h"
#include "LOG_Node.h"

LOG_Tree::LOG_Tree()
	:m_rootNode(nullptr),
	m_init(false)
{
}

LOG_Tree::LOG_Tree(const LOG_Tree& other)
	:m_rootNode(nullptr),
	m_init(false)
{
	for (const std::unique_ptr<LOG_INode>& node : other.m_nodes) {
		LOG_INode *replica = static_cast<LOG_INode *>(node->GetReplica());
		if (node.get() == other.m_rootNode) {
			m_rootNode = static_cast<LOG_Node *>(replica);
		}

		m_nodes.emplace_back(replica);
	}
}

LOG_Tree::~LOG_Tree()
{
}

void LOG_Tree::AddNode(LOG_INode *node, bool root)
{
	m_nodes.emplace_back(node);
	if (root) {
		m_rootNode = static_cast<LOG_Node *>(node);
	}
}

void LOG_Tree::SetObject(LOG_Object *obj)
{
	for (std::unique_ptr<LOG_INode>& node : m_nodes) {
		node->SetObject(obj);
	}
}

void LOG_Tree::Update()
{
	BLI_assert(m_rootNode);

	if (!m_init) {
		for (std::unique_ptr<LOG_INode>& node : m_nodes) {
			node->Start();
		}

		m_init = true;
	}

	LOG_Node *nextNode = m_rootNode;
	while (nextNode) {
		nextNode = nextNode->Update();
	}
}
