/*
* ***** BEGIN GPL LICENSE BLOCK *****
*
* This program is free software; you can redistribute it and/or
* modify it under the terms of the GNU General Public License
* as published by the Free Software Foundation; either version 2
* of the License, or (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software Foundation,
* Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
*
* Contributor(s): Porteries Tristan.
*
* ***** END GPL LICENSE BLOCK *****
*/

#ifndef __RAS_DOWNWARD_NODE_H__
#define __RAS_DOWNWARD_NODE_H__

#include "RAS_BaseNode.h"
#include "RAS_DummyNode.h"

#include <vector>

#ifdef DEBUG
#  include <iostream>
#  include <boost/type_index.hpp>
#endif  // DEBUG

/** RAS_DownwardNode is a node which store its children nodes.
 *
 * A downward node is using for unsorted render when the bind and unbind call can be proceed
 * by iterating from the top of the tree to the bottom.
 * Each downward node in render process call its bind function then render all its children
 * nodes and finally call its unbind function.
 *
 * \param _ChildType The children node type.
 */
template <class NodeInfo, class _ChildType>
class RAS_DownwardNode : public RAS_BaseNode<NodeInfo>
{
public:
	using typename RAS_BaseNode<NodeInfo>::OwnerType;
	using typename RAS_BaseNode<NodeInfo>::DataType;
	using typename RAS_BaseNode<NodeInfo>::TupleType;
	using typename RAS_BaseNode<NodeInfo>::Leaf;
	using typename RAS_BaseNode<NodeInfo>::Function;
	typedef _ChildType ChildType;
	typedef std::vector<ChildType *> ChildTypeList;

private:
	/// Children nodes.
	ChildTypeList m_children;
	/// True when the node is linked to a parent node (used to avoid redundant insertion).
	bool m_linked;

public:
	RAS_DownwardNode(OwnerType *owner, DataType *data, Function bind, Function unbind)
		:RAS_BaseNode<NodeInfo>(owner, data, bind, unbind),
		m_linked(false)
	{
	}

	RAS_DownwardNode() = default;

	~RAS_DownwardNode()
	{
	}

	/** Returning true when a node is valid. A node is valid if it is always final or
	 * if it has at least one children.
	 */
	inline bool GetValid() const
	{
		if (!Leaf()) {
			return !m_children.empty();
		}
		return true;
	}

	/// Add a child node if it is valid.
	inline void AddChild(ChildType *child)
	{
		if (child->GetValid() && !child->GetLinked()) {
			m_children.push_back(child);
		}
	}

	inline bool GetLinked() const
	{
		return m_linked;
	}

	inline void SetLinked(bool linked)
	{
		m_linked = linked;
	}

	inline void Clear()
	{
		if (!Leaf()) {
			for (ChildType *child : m_children) {
				child->SetLinked(false);
			}
			m_children.clear();
		}
	}

	/** Recursive function calling the bind function, call itsefl in children nodes
	 * and calling unbind function.
	 * \param tuple The function tuple argument to use for binding and unbinding.
	 */
	template <class T = ChildType>
	inline typename std::enable_if<!std::is_same<T, RAS_DummyNode>::value, void>::type
	Execute(const TupleType& tuple)
	{
		this->Bind(tuple);

		typename ChildType::TupleType childTuple(tuple, this->m_data);
		for (ChildType *child : m_children) {
			child->Execute(childTuple);
		}

		this->Unbind(tuple);

		// In the same time we can remove the children nodes.
		Clear();
	}

	/// Function override to avoid try creating a RAS_DummyNodeTuple with arguments.
	template <class T = ChildType>
	inline typename std::enable_if<std::is_same<T, RAS_DummyNode>::value, void>::type
	Execute(const TupleType& tuple)
	{
		this->Bind(tuple);

		this->Unbind(tuple);

		// No need to clear as dummy node are never instanced.
	}

#ifdef DEBUG
	void Print(unsigned short level, bool recursive) const
	{
		for (unsigned short i = 0; i < level; ++i) {
			std::cout << "\t";
		}

		std::cout << boost::typeindex::type_id<OwnerType>().pretty_name() << "(" << this->m_owner << ") "<< std::endl;

		if (recursive) {
			for (ChildType *child : m_children) {
				child->Print(level + 1, recursive);
			}
		}
	}
#endif  // DEBUG
};

#endif  // __RAS_DOWNWARD_NODE_H__
