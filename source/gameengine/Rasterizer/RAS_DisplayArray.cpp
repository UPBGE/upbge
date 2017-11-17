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

/** \file RAS_DisplayArray.cpp
 *  \ingroup bgerast
 */

#include "RAS_DisplayArray.h"
#include "RAS_DisplayArrayStorage.h"
#include "RAS_Mesh.h"

#include "GPU_glew.h"

#include <algorithm>

struct PolygonSort {
	/// Distance from polygon center to camera near plane.
	float m_z;
	/// Index of the first vertex in the polygon.
	unsigned int m_first;

	PolygonSort() = default;

	void Init(unsigned int first, const mt::vec3& center, const mt::vec3& pnorm)
	{
		m_first = first;
		m_z = mt::dot(pnorm, center);
	}

	struct BackToFront {
		bool operator()(const PolygonSort &a, const PolygonSort &b) const
		{
			return a.m_z < b.m_z;
		}
	};
};

RAS_DisplayArray::RAS_DisplayArray(PrimitiveType type, const RAS_DisplayArray::Format& format)
	:m_type(type),
	m_format(format),
	m_maxOrigIndex(0)
{
}

RAS_DisplayArray::RAS_DisplayArray(const RAS_DisplayArray& other)
	:m_type(other.m_type),
	m_format(other.m_format),
	m_vertexData(other.m_vertexData),
	m_vertexInfos(other.m_vertexInfos),
	m_primitiveIndices(other.m_primitiveIndices),
	m_triangleIndices(other.m_triangleIndices),
	m_maxOrigIndex(other.m_maxOrigIndex),
	m_polygonCenters(other.m_polygonCenters)
{
}

RAS_DisplayArray::~RAS_DisplayArray()
{
}

RAS_IDisplayArray::RAS_IDisplayArray(PrimitiveType type, const RAS_VertexFormat& format,
		const RAS_VertexDataMemoryFormat& memoryFormat, const IndexList& primitiveIndices, const IndexList& triangleIndices)
	:m_type(type),
	m_format(format),
	m_memoryFormat(memoryFormat),
	m_primitiveIndices(primitiveIndices),
	m_triangleIndices(triangleIndices),
	m_maxOrigIndex(0)
{
}

unsigned int RAS_DisplayArray::AddVertex(const mt::vec3_packed& pos, const mt::vec3_packed& nor, const mt::vec4_packed& tan,
			mt::vec2_packed uvs[RAS_Texture::MaxUnits], unsigned int colors[RAS_Texture::MaxUnits], unsigned int origIndex, uint8_t flag)
{
	m_vertexData.positions.push_back(pos);
	m_vertexData.normals.push_back(nor);
	m_vertexData.tangents.push_back(tan);

	for (unsigned short i = 0; i < m_format.uvSize; ++i) {
		m_vertexData.uvs[i].push_back(uvs[i]);
	}

	for (unsigned short i = 0; i < m_format.colorSize; ++i) {
		m_vertexData.colors[i].push_back({colors[i]});
	}

	m_maxOrigIndex = std::max(m_maxOrigIndex, origIndex);
	m_vertexInfos.emplace_back(origIndex, flag);

	return m_vertexInfos.size() - 1;
}

void RAS_DisplayArray::Clear()
{
	m_vertexData.positions.clear();
	m_vertexData.normals.clear();
	m_vertexData.tangents.clear();

	for (unsigned short i = 0; i < m_format.uvSize; ++i) {
		m_vertexData.uvs[i].clear();
	}

	for (unsigned short i = 0; i < m_format.colorSize; ++i) {
		m_vertexData.colors[i].clear();
	}

	m_vertexInfos.clear();
	m_primitiveIndices.clear();
	m_triangleIndices.clear();
	m_maxOrigIndex = 0;
}

RAS_IDisplayArray *RAS_IDisplayArray::Construct(RAS_IDisplayArray::PrimitiveType type, const RAS_VertexFormat &format,
		const IVertexDataList& vertices, const IndexList& primitiveIndices, const IndexList& triangleIndices)
{
	return CM_InstantiateTemplateSwitch<RAS_IDisplayArray, RAS_DisplayArray, RAS_VertexFormatTuple>(format,
			type, format, vertices, primitiveIndices, triangleIndices);
}

void RAS_DisplayArray::SortPolygons(const mt::mat3x4& transform, unsigned int *indexmap)
{
	const unsigned int totpoly = GetPrimitiveIndexCount() / 3;

	if (totpoly <= 1 || m_type == LINES) {
		return;
	}

	// Extract camera Z plane.
	const mt::vec3 pnorm(transform[2], transform[5], transform[8]);

	if (m_polygonCenters.size() != totpoly) {
		m_polygonCenters.resize(totpoly, mt::zero3);
		for (unsigned int i = 0; i < totpoly; ++i) {
			// Compute polygon center.
			mt::vec3& center = m_polygonCenters[i];
			for (unsigned short j = 0; j < 3; ++j) {
				/* Note that we don't divide by 3 as it is not needed
				 * to compare polygons. */
				center += mt::vec3(m_vertexData.positions[m_primitiveIndices[i * 3 + j]]);
			}
		}
	}

	std::vector<PolygonSort> sortedPoly(totpoly);
	// Get indices and polygon distance into temporary array.
	for (unsigned int i = 0; i < totpoly; ++i) {
		sortedPoly[i].Init(i * 3, pnorm, m_polygonCenters[i]);
	}

	std::sort(sortedPoly.begin(), sortedPoly.end(), PolygonSort::BackToFront());

	// Get indices from temporary array.
	for (unsigned int i = 0; i < totpoly; ++i) {
		const unsigned int first = sortedPoly[i].m_first;
		for (unsigned short j = 0; j < 3; ++j) {
			indexmap[i * 3 + j] = m_primitiveIndices[first + j];
		}
	}
}

void RAS_DisplayArray::InvalidatePolygonCenters()
{
	m_polygonCenters.clear();
}

RAS_DisplayArray::PrimitiveType RAS_DisplayArray::GetPrimitiveType() const
{
	return m_type;
}

int RAS_DisplayArray::GetOpenGLPrimitiveType() const
{
	switch (m_type) {
		case LINES:
		{
			return GL_LINES;
		}
		case TRIANGLES:
		{
			return GL_TRIANGLES;
		}
	}
	return 0;
}

void RAS_DisplayArray::UpdateFrom(RAS_DisplayArray *other, int flag)
{
	BLI_assert(m_format == other->GetFormat());

	if (flag & POSITION_MODIFIED) {
		m_vertexData.positions = other->m_vertexData.positions;
	}
	if (flag & NORMAL_MODIFIED) {
		m_vertexData.normals = other->m_vertexData.normals;
	}
	if (flag & TANGENT_MODIFIED) {
		m_vertexData.tangents = other->m_vertexData.tangents;
	}
	if (flag & UVS_MODIFIED) {
		for (unsigned short i = 0; i < m_format.uvSize; ++i) {
			m_vertexData.uvs[i] = other->m_vertexData.uvs[i];
		}
	}
	if (flag & COLORS_MODIFIED) {
		for (unsigned short i = 0; i < m_format.colorSize; ++i) {
			m_vertexData.colors[i] = other->m_vertexData.colors[i];
		}
	}

	NotifyUpdate(flag);
}

const RAS_DisplayArray::Format& RAS_DisplayArray::GetFormat() const
{
	return m_format;
}

RAS_DisplayArrayLayout RAS_DisplayArray::GetLayout() const
{
	RAS_DisplayArrayLayout layout;
	intptr_t offset = 0;

	const unsigned int size = GetVertexCount();

	layout.position = offset;
	offset += sizeof(mt::vec3_packed) * size;

	layout.normal = offset;
	offset += sizeof(mt::vec3_packed) * size;
	
	layout.tangent = offset;
	offset += sizeof(mt::vec4_packed) * size;

	for (unsigned short i = 0; i < m_format.uvSize; ++i) {
		layout.uvs[i] = offset;
		offset += sizeof(mt::vec2_packed) * size;
	}

	for (unsigned short i = 0; i < m_format.colorSize; ++i) {
		layout.colors[i] = offset;
		offset += sizeof(unsigned int) * size;
	}

	layout.size = offset;

	return layout;
}

RAS_DisplayArray::Type RAS_DisplayArray::GetType() const
{
	return NORMAL;
}

RAS_DisplayArrayStorage& RAS_DisplayArray::GetStorage()
{
	return m_storage;
}

void RAS_DisplayArray::ConstructStorage()
{
	m_storage.Construct(this);
	m_storage.UpdateSize();
}
