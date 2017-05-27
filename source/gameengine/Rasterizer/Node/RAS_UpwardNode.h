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

#ifndef __RAS_UPWARD_NODE_H__
#define __RAS_UPWARD_NODE_H__

#include "RAS_BaseNode.h"

#ifdef DEBUG
#  include <iostream>
#  include <boost/type_index.hpp>
#endif  // DEBUG

/** RAS_UpwardNode is a node storing its parent node.
 *
 * A upward node is using for sorted render were two non-consecutive nodes could share
 * the same parent node. In this case the render can be proceed from top to bottom, but
 * from the bottom (leafs) to the bottom. This process is external in RAS_UpwardNodeIterator.
 *
 * \param _ParentType The parent node type.
 */
template <class NodeInfo, class _ParentType>
class RAS_UpwardNode : public RAS_BaseNode<NodeInfo>
{
public:
	using typename RAS_BaseNode<NodeInfo>::OwnerType;
	using typename RAS_BaseNode<NodeInfo>::DataType;
	using typename RAS_BaseNode<NodeInfo>::Function;
	typedef _ParentType ParentType;

private:
	ParentType *m_parent;

public:
	RAS_UpwardNode(OwnerType *owner, DataType *data, Function bind, Function unbind)
		:RAS_BaseNode<NodeInfo>(owner, data, bind, unbind),
		m_parent(nullptr)
	{
	}

	RAS_UpwardNode() = default;

	~RAS_UpwardNode()
	{
	}

	inline ParentType *GetParent() const
	{
		return m_parent;
	}

	inline void SetParent(ParentType *parent)
	{
		m_parent = parent;
	}

#ifdef DEBUG
	void Print(unsigned short level, bool recursive)
	{
		for (unsigned short i = 0; i < level; ++i) {
			std::cout << "\t";
		}

		std::cout << boost::typeindex::type_id<OwnerType>().pretty_name() << "(" << this->m_owner << ") "<< std::endl;

		if (recursive) {
			m_parent->Print(level + 1, recursive);
		}
	}
#endif  // DEBUG
};

#endif  // __RAS_UPWARD_NODE_H__
