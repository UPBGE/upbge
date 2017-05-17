#ifndef __RAS_UPWARD_NODE_VISITOR_H__
#define __RAS_UPWARD_NODE_VISITOR_H__

#include "RAS_BaseNode.h"
#include "RAS_DummyNode.h"

#include <memory>

/** This class is a dummy node iterator doing none iteration.
 * The node wrapped has the property to have also a dummy tuple.
 */
template <class NodeType>
class RAS_DummyUpwardNodeIterator
{
public:
	using DataType = typename NodeType::DataType;

private:
	NodeType *m_node;
	RAS_DummyNodeTuple m_tuple;

public:
	RAS_DummyUpwardNodeIterator(NodeType *node)
		:m_node(node)
	{
		m_node->Bind(m_tuple);
	}

	~RAS_DummyUpwardNodeIterator()
	{
		m_node->Unbind(m_tuple);
	}

	inline const RAS_DummyNodeTuple& GetTuple() const
	{
		return m_tuple;
	}

	inline DataType *GetData() const
	{
		return m_node->GetData();
	}

	inline bool NextNode(NodeType *node)
	{
		return false;
	}

	inline void Finish()
	{
		m_node->Unbind(m_tuple);
	}
};

/** RAS_UpwardNodeIterator is class using to proceed the sorted render using RAS_UpwardNode.
 *
 * A sorted render is proceed comparing the parent node of the current node with the parent
 * node of the previous node. If the both parent nodes are not the same, then the previous
 * parent node is calling its unbinding function and the current parent node is calling its
 * binding function.
 * The same operation is done recursively for parent node of parent node.
 *
 * \param NodeType The upward node type.
 */
template <class _NodeType>
class RAS_UpwardNodeIterator
{
public:
	using NodeType = _NodeType;
	using DataType = typename NodeType::DataType;
	using ParentNodeType = typename NodeType::ParentType;
	using TupleType = typename NodeType::TupleType;
	using ParentTupleType = typename ParentNodeType::TupleType;
	using ParentType = typename std::conditional<
		std::is_same<ParentTupleType, RAS_DummyNodeTuple>::value,
		RAS_DummyUpwardNodeIterator<ParentNodeType>,
		RAS_UpwardNodeIterator<ParentNodeType> >::type;

private:
	NodeType *m_node;
	ParentType m_parent;
	TupleType m_tuple;

public:
	RAS_UpwardNodeIterator(NodeType *node)
		:m_node(node),
		m_parent(node->GetParent()),
		m_tuple(m_parent.GetTuple(), m_parent.GetData())
	{
		m_node->Bind(m_tuple);
	}

	~RAS_UpwardNodeIterator()
	{
		m_node->Unbind(m_tuple);
	}

	inline const TupleType& GetTuple() const
	{
		return m_tuple;
	}

	inline DataType *GetData() const
	{
		return m_node->GetData();
	}

	inline bool NextNode(NodeType *node)
	{
		// If the parent node is unchanged, nothing is done.
		if (node == m_node) {
			return false;
		}

		// The node must be bound before to have a valid m_tuple.
		m_node->Unbind(m_tuple);

		/* Nodes request to be unbind or bind when their parent are kept bound.
		 * The nodes are then unbind before their parent and bind after their parent.
		 * This is proceeded by doing the recursive call between unbind and bind, unbind
		 * at recursion construction (upward), bind at recursion destruction (downward).
		 */
		if (m_parent.NextNode(node->GetParent())) {
			// We regenerate tuple only when the parent node changed.
			m_tuple = TupleType(m_parent.GetTuple(), m_parent.GetData());
		}

		m_node = node;
		m_node->Bind(m_tuple);

		return true;
	}
};

#endif  // __RAS_UPWARD_NODE_VISITOR_H__
