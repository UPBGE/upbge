#ifndef __LOG_TREE_H__
#define __LOG_TREE_H__

#include <vector>
#include <memory>

class LOG_Object;
class LOG_INode;
class LOG_Node;

class LOG_Tree
{
private:
	std::vector<std::unique_ptr<LOG_INode> > m_nodes;
	LOG_Node *m_rootNode;

	bool m_init;

public:
	LOG_Tree();
	LOG_Tree(const LOG_Tree& other);
	~LOG_Tree();

	void AddNode(LOG_INode *node, bool root);
	void SetObject(LOG_Object *obj);

	void Update();
};

#endif  // __LOG_TREE_H__
