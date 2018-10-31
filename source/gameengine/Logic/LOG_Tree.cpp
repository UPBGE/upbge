#include "LOG_Tree.h"
#include "LOG_Node.h"

LOG_Tree::LOG_Tree()
	:m_rootNode(nullptr)
{
}

LOG_Tree::~LOG_Tree() = default;

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

void LOG_Tree::Start()
{
	for (std::unique_ptr<LOG_INode>& node : m_nodes) {
		node->Start();
	}
}

void LOG_Tree::Update()
{
	BLI_assert(m_rootNode);

	LOG_Node *nextNode = m_rootNode;
	while (nextNode) {
		nextNode = nextNode->Update();
	}
}
