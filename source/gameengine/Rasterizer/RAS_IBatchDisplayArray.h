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

/** \file RAS_IBatchDisplayArray.h
 *  \ingroup bgerast
 */

#ifndef __RAS_IDISPLAY_ARRAY_BATCHING_H__
#define __RAS_IDISPLAY_ARRAY_BATCHING_H__

#include "RAS_IDisplayArray.h"

class RAS_IBatchDisplayArray : public virtual RAS_IDisplayArray
{
protected:
	/// This struct is dedicated to store all the info of a part.
	struct Part
	{
		/// Relative pointer to the start index, used for VBO.
		void *m_indexOffset;

		unsigned int m_startVertex;
		unsigned int m_vertexCount;

		unsigned int m_startIndex;
		unsigned int m_indexCount;
	};

	/// The part's info.
	std::vector<Part> m_parts;

public:
	RAS_IBatchDisplayArray(PrimitiveType type, const RAS_TexVertFormat &format);
	virtual ~RAS_IBatchDisplayArray();

	/** Construct the display array batching corresponding of the vertex of the given format.
	 * \param type The type of primitives, one of the enumeration PrimitiveType.
	 * \param format The format of vertex to use.
	 */
	static RAS_IBatchDisplayArray *ConstructArray(PrimitiveType type, const RAS_TexVertFormat &format);

	inline void *GetPartIndexOffset(const unsigned short index)
	{
		return m_parts[index].m_indexOffset;
	}

	inline unsigned int GetPartIndexCount(const unsigned short index)
	{
		return m_parts[index].m_indexCount;
	}

	/** Merge a display array with a transform matrix.
	 * \param iarray The display array to merge.
	 * \param mat The matrix applied on all the vertices.
	 * \return The index of the part just added.
	 */
	virtual unsigned int Merge(RAS_IDisplayArray *iarray, const MT_Matrix4x4& mat) = 0;

	/** Split a part.
	 * \param partIndex The index of the part to remove.
	 */
	virtual void Split(unsigned int partIndex) = 0;

	virtual Type GetType() const;
};

#endif  // __RAS_IDISPLAY_ARRAY_BATCHING_H__
