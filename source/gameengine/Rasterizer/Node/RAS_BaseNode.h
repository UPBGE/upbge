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

#ifndef __RAS_BASIC_NODE__
#define __RAS_BASIC_NODE__

/** RAS_BaseNode is a class wrapping a rendering class by simulating it with a
 * binding and unbinding function.
 * \param NodeInfo The node information.
 */
template <class NodeInfo>
class RAS_BaseNode
{
public:
	using OwnerType = typename NodeInfo::OwnerType;
	using TupleType = typename NodeInfo::TupleType;
	using DataType = typename NodeInfo::DataType;
	using Leaf = typename NodeInfo::Leaf;

	/** The type of function to call for binding and unbinding.
	 * It takes as arguments the tuple containing the data of the parent nodes.
	 */
	using Function = void (OwnerType::*)(const TupleType&);

protected:
	/// An instance of the wrapped class.
	OwnerType *m_owner;
	DataType *m_data;

	Function m_bind;
	Function m_unbind;

public:
	RAS_BaseNode(OwnerType *owner, DataType *data, Function bind, Function unbind)
		:m_owner(owner),
		m_data(data),
		m_bind(bind),
		m_unbind(unbind)
	{
	}

	RAS_BaseNode() = default;

	~RAS_BaseNode()
	{
	}

	inline OwnerType *GetOwner() const
	{
		return m_owner;
	}

	inline DataType *GetData() const
	{
		return m_data;
	}

	inline void Bind(const TupleType& tuple)
	{
		if (m_bind) {
			(m_owner->*m_bind)(tuple);
		}
	}

	inline void Unbind(const TupleType& tuple)
	{
		if (m_unbind) {
			(m_owner->*m_unbind)(tuple);
		}
	}
};

#endif  // __RAS_BASIC_NODE__
