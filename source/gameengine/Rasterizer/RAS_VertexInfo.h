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
 * Contributor(s): Tristan Porteries.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file RAS_VertexInfo.h
 *  \ingroup bgerast
 */

#ifndef __RAS_VERTEX_INFO_H__
#define __RAS_VERTEX_INFO_H__

#include <cstdint>

class RAS_VertexInfo
{
public:
	enum {
		FLAT = 1,
	};

private:
	unsigned int m_origindex;
	short m_softBodyIndex;
	uint8_t m_flag;

public:
	RAS_VertexInfo(unsigned int origindex, bool flat);
	~RAS_VertexInfo();

	inline const unsigned int GetOrigIndex() const
	{
		return m_origindex;
	}

	inline short int GetSoftBodyIndex() const
	{
		return m_softBodyIndex;
	}

	inline void SetSoftBodyIndex(short int sbIndex)
	{
		m_softBodyIndex = sbIndex;
	}

	inline const uint8_t GetFlag() const
	{
		return m_flag;
	}

	inline void SetFlag(const uint8_t flag)
	{
		m_flag = flag;
	}
};


#endif  // __RAS_VERTEX_INFO_H__
