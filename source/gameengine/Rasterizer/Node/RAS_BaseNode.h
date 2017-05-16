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

#include <functional>

/** RAS_BaseNode is a class wrapping a rendering class by simulating it with a
 * binding and unbinding function.
 * \param InfoType The class to wrap functions from.
 * \param Leaf True if the node is a leaf of the tree.
 * \param Args The arguments type to pass to the binding and unbinding functions.
 */
template <class InfoType, bool Leaf, class Args>
class RAS_BaseNode
{
public:
	/** The type of function to call for binding and unbinding.
	 * It takes as arguments the class the node is wrapping and the structure
	 * containing the arguments.
	 */
	typedef std::function<void(InfoType *, const Args&)> Function;

protected:
	/// An instance of the wrapped class.
	InfoType *m_info;

	Function m_bind;
	Function m_unbind;

public:
	RAS_BaseNode(InfoType *info, Function bind, Function unbind)
		:m_info(info),
		m_bind(bind),
		m_unbind(unbind)
	{
	}

	RAS_BaseNode()
	{
	}

	~RAS_BaseNode()
	{
	}

	inline InfoType *GetInfo() const
	{
		return m_info;
	}

	inline void Bind(const Args& args)
	{
		if (m_bind) {
			m_bind(m_info, args);
		}
	}

	inline void Unbind(const Args& args)
	{
		if (m_unbind) {
			m_unbind(m_info, args);
		}
	}
};

#endif  // __RAS_BASIC_NODE__
