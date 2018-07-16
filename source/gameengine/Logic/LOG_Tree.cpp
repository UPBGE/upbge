#include "LOG_Tree.h"
#include "LOG_Node.h"

LOG_Tree::LOG_Tree()
	:m_rootNode(nullptr)
{
}

LOG_Tree::~LOG_Tree() = default;

void LOG_Tree::AddNode(LOG_Node *node, bool root)
{
	m_nodes.emplace_back(node);
	if (root) {
		m_rootNode = node;
	}
}

void LOG_Tree::SetGameObject(KX_GameObject *gameobj)
{
	for (std::unique_ptr<LOG_Node>& node : m_nodes) {
		node->SetGameObject(gameobj);
	}
}

void LOG_Tree::Start()
{
	for (std::unique_ptr<LOG_Node>& node : m_nodes) {
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
