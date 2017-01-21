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

#include <vector>

#ifdef DEBUG
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
template <class _ChildType, class InfoType, RAS_NodeFlag Flag, class Args>
class RAS_DownwardNode : public RAS_BaseNode<InfoType, Flag, Args>
{
public:
	using typename RAS_BaseNode<InfoType, Flag, Args>::Function;
	typedef _ChildType ChildType;
	typedef std::vector<ChildType *> ChildTypeList;

private:
	ChildTypeList m_children;

public:
	RAS_DownwardNode(InfoType *info, Function bind, Function unbind)
		:RAS_BaseNode<InfoType, Flag, Args>(info, bind, unbind)
	{
	}

	RAS_DownwardNode()
	{
	}

	~RAS_DownwardNode()
	{
	}

	/** Returning true when a node is valid. A node is valid if it is always final or
	 * if it has at least one children.
	 */
	inline bool GetValid() const
	{
		if (Flag == RAS_NodeFlag::NEVER_FINAL) {
			return m_children.size() > 0;
		}
		return true;
	}

	/// Add a child node if it is valid.
	inline void AddChild(ChildType *child)
	{
		if (child->GetValid()) {
			m_children.push_back(child);
		}
	}

	inline void Clear()
	{
		if (Flag == RAS_NodeFlag::NEVER_FINAL) {
			m_children.clear();
		}
	}

	/** Recursive function calling the bind function, call itsefl in children nodes
	 * and calling unbind function.
	 * \param Args The function arguments to use for binding and unbinding.
	 */
	void Execute(const Args& args)
	{
		this->Bind(args);

		for (ChildType *child : m_children) {
			child->Execute(args);
		}

		this->Unbind(args);

		// In the same time we can remove the children nodes.
		Clear();
	}

#ifdef DEBUG
	void Print(unsigned short level, bool recursive)
	{
		for (unsigned short i = 0; i < level; ++i) {
			std::cout << "\t";
		}

		std::cout << boost::typeindex::type_id<InfoType>().pretty_name() << "(" << this->m_info << ") "<< std::endl;

		if (recursive) {
			for (ChildType *child : m_children) {
				child->Print(level + 1, recursive);
			}
		}
	}
#endif  // DEBUG
};

#endif  // __RAS_DOWNWARD_NODE_H__
