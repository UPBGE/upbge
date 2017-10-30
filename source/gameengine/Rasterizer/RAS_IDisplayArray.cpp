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

/** \file RAS_IDisplayArray.cpp
 *  \ingroup bgerast
 */

#include "RAS_DisplayArray.h"
#include "RAS_MeshObject.h"

#include "GPU_glew.h"

#include <algorithm>

struct PolygonSort
{
	/// Distance from polygon center to camera near plane.
	float m_z;
	/// Index of the first vertex in the polygon.
	unsigned int m_first;

	PolygonSort() = default;

	void Init(unsigned int first, const MT_Vector3& center, const MT_Vector3& pnorm)
	{
		m_first = first;
		m_z = pnorm.dot(center);
	}

	struct BackToFront
	{
		bool operator()(const PolygonSort &a, const PolygonSort &b) const
		{
			return a.m_z < b.m_z;
		}
	};
};

RAS_IDisplayArray::RAS_IDisplayArray(PrimitiveType type, const RAS_VertexFormat& format)
	:m_type(type),
	m_modifiedFlag(NONE_MODIFIED),
	m_format(format),
	m_maxOrigIndex(0)
{
}

RAS_IDisplayArray::~RAS_IDisplayArray()
{
}

#define NEW_DISPLAY_ARRAY_UV(vertformat, uv, color, primtype) \
	if (vertformat.uvSize == uv && vertformat.colorSize == color) { \
		return new RAS_DisplayArray<RAS_Vertex<uv, color> >(primtype, vertformat); \
	}

#define NEW_DISPLAY_ARRAY_COLOR(vertformat, color, primtype) \
	NEW_DISPLAY_ARRAY_UV(format, 1, color, type); \
	NEW_DISPLAY_ARRAY_UV(format, 2, color, type); \
	NEW_DISPLAY_ARRAY_UV(format, 3, color, type); \
	NEW_DISPLAY_ARRAY_UV(format, 4, color, type); \
	NEW_DISPLAY_ARRAY_UV(format, 5, color, type); \
	NEW_DISPLAY_ARRAY_UV(format, 6, color, type); \
	NEW_DISPLAY_ARRAY_UV(format, 7, color, type); \
	NEW_DISPLAY_ARRAY_UV(format, 8, color, type);

RAS_IDisplayArray *RAS_IDisplayArray::ConstructArray(RAS_IDisplayArray::PrimitiveType type, const RAS_VertexFormat &format)
{
	NEW_DISPLAY_ARRAY_COLOR(format, 1, type);
	NEW_DISPLAY_ARRAY_COLOR(format, 2, type);
	NEW_DISPLAY_ARRAY_COLOR(format, 3, type);
	NEW_DISPLAY_ARRAY_COLOR(format, 4, type);
	NEW_DISPLAY_ARRAY_COLOR(format, 5, type);
	NEW_DISPLAY_ARRAY_COLOR(format, 6, type);
	NEW_DISPLAY_ARRAY_COLOR(format, 7, type);
	NEW_DISPLAY_ARRAY_COLOR(format, 8, type);

	return nullptr;
}
#undef NEW_DISPLAY_ARRAY_UV
#undef NEW_DISPLAY_ARRAY_COLOR

void RAS_IDisplayArray::SortPolygons(const MT_Transform& transform, unsigned int *indexmap)
{
	const unsigned int totpoly = GetPrimitiveIndexCount() / 3;

	if (totpoly <= 1 || m_type == LINES) {
		return;
	}

	// Extract camera Z plane.
	const MT_Vector3 pnorm(transform.getBasis()[2]);

	if (m_polygonCenters.size() != totpoly) {
		m_polygonCenters.resize(totpoly, MT_Vector3(0.0f, 0.0f, 0.0f));
		for (unsigned int i = 0; i < totpoly; ++i) {
			// Compute polygon center.
			MT_Vector3& center = m_polygonCenters[i];
			for (unsigned short j = 0; j < 3; ++j) {
				/* Note that we don't divide by 3 as it is not needed
				 * to compare polygons. */
				center += GetVertex(m_primitiveIndices[i * 3 + j])->xyz();
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

void RAS_IDisplayArray::InvalidatePolygonCenters()
{
	m_polygonCenters.clear();
}

RAS_IDisplayArray::PrimitiveType RAS_IDisplayArray::GetPrimitiveType() const
{
	return m_type;
}

int RAS_IDisplayArray::GetOpenGLPrimitiveType() const
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

void RAS_IDisplayArray::UpdateFrom(RAS_IDisplayArray *other, int flag)
{
	if (flag & TANGENT_MODIFIED) {
		for (unsigned int i = 0, size = other->GetVertexCount(); i < size; ++i) {
			GetVertex(i)->SetTangent(MT_Vector4(other->GetVertex(i)->getTangent()));
		}
	}
	if (flag & UVS_MODIFIED) {
		const unsigned short uvSize = min_ii(GetVertexUvSize(), other->GetVertexUvSize());
		for (unsigned int i = 0, size = other->GetVertexCount(); i < size; ++i) {
			for (unsigned int uv = 0; uv < uvSize; ++uv) {
				GetVertex(i)->SetUV(uv, MT_Vector2(other->GetVertex(i)->getUV(uv)));
			}
		}
	}
	if (flag & POSITION_MODIFIED) {
		for (unsigned int i = 0, size = other->GetVertexCount(); i < size; ++i) {
			GetVertex(i)->SetXYZ(MT_Vector3(other->GetVertex(i)->getXYZ()));
		}
	}
	if (flag & NORMAL_MODIFIED) {
		for (unsigned int i = 0, size = other->GetVertexCount(); i < size; ++i) {
			GetVertex(i)->SetNormal(MT_Vector3(other->GetVertex(i)->getNormal()));
		}
	}
	if (flag & COLORS_MODIFIED) {
		const unsigned short colorSize = min_ii(GetVertexColorSize(), other->GetVertexColorSize());
		for (unsigned int i = 0, size = other->GetVertexCount(); i < size; ++i) {
			for (unsigned int color = 0; color < colorSize; ++color) {
				GetVertex(i)->SetRGBA(color, other->GetVertex(i)->getRawRGBA(color));
			}
		}
	}
}

unsigned short RAS_IDisplayArray::GetModifiedFlag() const
{
	return m_modifiedFlag;
}

void RAS_IDisplayArray::AppendModifiedFlag(unsigned short flag)
{
	SetModifiedFlag(m_modifiedFlag | flag);
}

void RAS_IDisplayArray::SetModifiedFlag(unsigned short flag)
{
	m_modifiedFlag = flag;
}

const RAS_VertexFormat& RAS_IDisplayArray::GetFormat() const
{
	return m_format;
}

RAS_IDisplayArray::Type RAS_IDisplayArray::GetType() const
{
	return NORMAL;
}
