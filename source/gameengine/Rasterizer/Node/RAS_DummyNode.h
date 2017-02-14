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

#ifndef __RAS_DUMMY_NODE_H__
#define __RAS_DUMMY_NODE_H__

class RAS_DummyNode
{
public:
	typedef RAS_DummyNode ParentType;

	void Print(unsigned short level, bool recursive)
	{
	}

	template <class Args>
	void Bind(const Args& args)
	{
	}

	template <class Args>
	void Unbind(const Args& args)
	{
	}

	template <class Args>
	void Execute(const Args& args)
	{
	}

	ParentType *GetParent()
	{
		return nullptr;
	}

	template <class NodeType>
	void AddChild(NodeType subNode)
	{
	}
};

#endif  // __RAS_DUMMY_NODE_H__
