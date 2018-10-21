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

/** \file RAS_BatchDisplayArray.cpp
 *  \ingroup bgerast
 */

#include "RAS_BatchDisplayArray.h"
#ifdef DEBUG
#  include "CM_Message.h"
#endif

RAS_BatchDisplayArray::RAS_BatchDisplayArray(PrimitiveType type, const Format &format)
	:RAS_DisplayArray(type, format)
{
}

RAS_BatchDisplayArray::~RAS_BatchDisplayArray()
{
}

unsigned int RAS_BatchDisplayArray::Merge(RAS_DisplayArray *array, const mt::mat4& mat)
{
	const unsigned int vertexcount = array->GetVertexCount();
	const unsigned int indexcount = array->GetPrimitiveIndexCount();

	const unsigned int startvertex = GetVertexCount();
	const unsigned int startindex = m_primitiveIndices.size();

	// Add the part info.
	Part part;
	part.m_startVertex = startvertex;
	part.m_vertexCount = vertexcount;
	part.m_startIndex = startindex;
	part.m_indexCount = indexcount;
	part.m_indexOffset = part.m_startIndex * sizeof(unsigned int);
	m_parts.push_back(part);

	// Pre-allocate vertex list and index list.
	const unsigned int totalVertexCount = startvertex + vertexcount;
	m_vertexData.positions.resize(totalVertexCount);
	m_vertexData.normals.resize(totalVertexCount);
	m_vertexData.tangents.resize(totalVertexCount);
	// Uv and color are not resized here as they are just merged later.

	m_primitiveIndices.resize(startindex + indexcount);

#ifdef DEBUG
	CM_Debug("Add part : " << (m_parts.size() - 1) << ", start index: " << startindex << ", index count: " << indexcount << ", start vertex: " << startvertex << ", vertex count: " << vertexcount);
#endif  // DEBUG

	// Normal and tangent matrix.
	mt::mat4 nmat = mat;
	nmat(0, 3) = nmat(1, 3) = nmat(2, 3) = 0.0f;

	// Copy the vertices and transform.
	for (unsigned int i = 0; i < vertexcount; ++i) {
		m_vertexData.positions[startvertex + i] = mat * mt::vec3(array->GetPosition(i));
	}

	for (unsigned int i = 0; i < vertexcount; ++i) {
		m_vertexData.normals[startvertex + i] = nmat * mt::vec3(array->GetNormal(i));
	}

	for (unsigned int i = 0; i < vertexcount; ++i) {
		m_vertexData.tangents[startvertex + i] = nmat * mt::vec4(array->GetTangent(i));
	}

	for (unsigned short i = 0; i < m_format.uvSize; ++i) {
		m_vertexData.uvs[i].insert(m_vertexData.uvs[i].end(), array->m_vertexData.uvs[i].begin(), array->m_vertexData.uvs[i].end());
	}

	for (unsigned short i = 0; i < m_format.colorSize; ++i) {
		m_vertexData.colors[i].insert(m_vertexData.colors[i].end(), array->m_vertexData.colors[i].begin(), array->m_vertexData.colors[i].end());
	}

	// Copy the indices of the merged array with as gap the first vertex index.
	for (unsigned int i = 0; i < indexcount; ++i) {
		m_primitiveIndices[startindex + i] = (array->m_primitiveIndices[i] + startvertex);
	}

	// Request storage update.
	NotifyUpdate(SIZE_MODIFIED);

	return (m_parts.size() - 1);
}

void RAS_BatchDisplayArray::Split(unsigned int partIndex)
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

	// Erase vertices of the part to remove.
	m_vertexData.positions.erase(m_vertexData.positions.begin() + startvertex, m_vertexData.positions.begin() + endvertex);
	m_vertexData.normals.erase(m_vertexData.normals.begin() + startvertex, m_vertexData.normals.begin() + endvertex);
	m_vertexData.tangents.erase(m_vertexData.tangents.begin() + startvertex, m_vertexData.tangents.begin() + endvertex);

	for (unsigned short i = 0; i < m_format.uvSize; ++i) {
		m_vertexData.uvs[i].erase(m_vertexData.uvs[i].begin() + startvertex, m_vertexData.uvs[i].begin() + endvertex);
	}

	for (unsigned short i = 0; i < m_format.colorSize; ++i) {
		m_vertexData.colors[i].erase(m_vertexData.colors[i].begin() + startvertex, m_vertexData.colors[i].begin() + endvertex);
	}

	// Reduce start vertex and start index of the part after the removed part.
	for (unsigned i = partIndex + 1, size = m_parts.size(); i < size; ++i) {
		Part& nextPart = m_parts[i];
		nextPart.m_startVertex -= vertexcount;
		nextPart.m_startIndex -= indexcount;
		nextPart.m_indexOffset = (nextPart.m_startIndex * sizeof(unsigned int));
	}

	// Remove the part info.
	m_parts.erase(m_parts.begin() + partIndex);

	// Request storage update.
	NotifyUpdate(SIZE_MODIFIED);
}

RAS_DisplayArray::Type RAS_BatchDisplayArray::GetType() const
{
	return BATCHING;
}
