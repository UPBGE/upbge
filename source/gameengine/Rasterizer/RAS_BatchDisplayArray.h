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

/** \file RAS_BatchDisplayArray.h
 *  \ingroup bgerast
 */

#ifndef __RAS_DISPLAY_ARRAY_BATCHING_H__
#define __RAS_DISPLAY_ARRAY_BATCHING_H__

#include "RAS_DisplayArray.h"
#include "RAS_IBatchDisplayArray.h"
#include "CM_Message.h"

#ifdef _MSC_VER
/* RAS_BatchDisplayArray uses a diamond inheritance from a virtual pure base class. Only one branch of the diamond
 * define these virtual pure functions and come in the final class with dominance. This behaviour is wanted
 * but MSVC warn about it, we just disable the warning.
 */
#  pragma warning (disable:4250)
#endif

template <class VertexData>
class RAS_BatchDisplayArray : public RAS_DisplayArray<VertexData>, public RAS_IBatchDisplayArray
{
protected:
	using RAS_DisplayArray<VertexData>::m_vertexes;
	using RAS_DisplayArray<VertexData>::m_primitiveIndices;

public:
	RAS_BatchDisplayArray(RAS_IDisplayArray::PrimitiveType type, const RAS_VertexFormat& format)
		:RAS_IDisplayArray(type, format, VertexData::GetMemoryFormat()),
		RAS_DisplayArray<VertexData>(type, format),
		RAS_IBatchDisplayArray(type, format, VertexData::GetMemoryFormat())
	{
	}

	virtual ~RAS_BatchDisplayArray()
	{
	}

	virtual RAS_IDisplayArray *GetReplica()
	{
		/* A batch display array must never be replicated.
		 * Display arrays are replicated when a deformer is used for a mesh slot
		 * but batch display array are not used in the case of deformer.
		 */
		BLI_assert(false);
		return nullptr;
	}

	/** Merge array in the batched array.
	 * \param iarray The array to merge, must be of the same vertex format than the
	 * batched array.
	 * \param mat The transformation to apply on each vertex in the merging.
	 */
	virtual unsigned int Merge(RAS_IDisplayArray *iarray, const mt::mat4& mat)
	{
		RAS_DisplayArray<VertexData> *array = dynamic_cast<RAS_DisplayArray<VertexData> *>(iarray);
		const unsigned int vertexcount = iarray->GetVertexCount();
		const unsigned int indexcount = iarray->GetPrimitiveIndexCount();

		const unsigned int startvertex = m_vertexes.size();
		const unsigned int startindex = m_primitiveIndices.size();

		// Add the part info.
		Part part;
		part.m_startVertex = startvertex;
		part.m_vertexCount = vertexcount;
		part.m_startIndex = startindex;
		part.m_indexCount = indexcount;
		part.m_indexOffset = (void *)(part.m_startIndex * sizeof(unsigned int));
		m_parts.push_back(part);

		// Pre-allocate vertex list and index list.
		m_vertexes.resize(startvertex + vertexcount);
		m_primitiveIndices.resize(startindex + indexcount);

#if 0
		CM_Debug("Add part : " << (m_parts.size() - 1) << ", start index: " << startindex << ", index count: " << indexcount << ", start vertex: " << startvertex << ", vertex count: " << vertexcount);
#endif  // DEBUG

		// Normal and tangent matrix.
		mt::mat4 nmat = mat;
		nmat(0, 3) = nmat(1, 3) = nmat(2, 3) = 0.0f;

		// Copy the vertex by not using a reference in the loop.
		for (unsigned int i = 0; i < vertexcount; ++i) {
			VertexData data = array->m_vertexes[i];
			RAS_Vertex vert(&data, m_format);
			// Transform the vertex position, normal and tangent.
			vert.Transform(mat, nmat);
			// Add the vertex in the list.
			m_vertexes[startvertex + i] = data;
		}

		// Copy the indices of the merged array with as gap the first vertex index.
		for (unsigned int i = 0; i < indexcount; ++i) {
			m_primitiveIndices[startindex + i] = (array->m_primitiveIndices[i] + startvertex);
		}

		// Update the cache to avoid accessing dangling vertex pointer from GetVertex().
		UpdateCache();
		// Request storage update.
		m_modifiedFlag |= SIZE_MODIFIED;

		return (m_parts.size() - 1);
	}

	virtual void Split(unsigned int partIndex)
	{
		const Part &part = m_parts[partIndex];

		const unsigned int startindex = part.m_startIndex;
		const unsigned int startvertex = part.m_startVertex;

		const unsigned int indexcount = part.m_indexCount;
		const unsigned int vertexcount = part.m_vertexCount;

		const unsigned int endvertex = startvertex + vertexcount;

#ifdef DEBUG
		CM_Debug("Move indices from " << startindex << " to " << m_primitiveIndices.size() - indexcount << ", shift of " << indexcount);
#endif  // DEBUG

		// Move the indices after the part to remove before of vertexcount places.
		for (unsigned int i = startindex, size = m_primitiveIndices.size() - indexcount; i < size; ++i) {
			m_primitiveIndices[i] = m_primitiveIndices[i + indexcount] - vertexcount;
		}

		// Erase the end of the index.
		m_primitiveIndices.erase(m_primitiveIndices.end() - indexcount, m_primitiveIndices.end());

#ifdef DEBUG
		CM_Debug("Remove vertexes : start vertex: " << startvertex << ", end vertex: " << endvertex);
#endif  // DEBUG

		// Erase vertex of the part to remove.
		m_vertexes.erase(m_vertexes.begin() + startvertex, m_vertexes.begin() + endvertex);

		// Reduce start vertex and start index of the part after the removed part.
		for (unsigned i = partIndex + 1, size = m_parts.size(); i < size; ++i) {
			Part& nextPart = m_parts[i];
			nextPart.m_startVertex -= vertexcount;
			nextPart.m_startIndex -= indexcount;
			nextPart.m_indexOffset = (void *)(nextPart.m_startIndex * sizeof(unsigned int));
		}

		// Remove the part info.
		m_parts.erase(m_parts.begin() + partIndex);

		// Update the cache to avoid accessing dangling vertex pointer from GetVertex().
		UpdateCache();
		// Request storage update.
		m_modifiedFlag |= SIZE_MODIFIED;
	}
};

#endif  // __RAS_DISPLAY_ARRAY_BATCHING_H__
