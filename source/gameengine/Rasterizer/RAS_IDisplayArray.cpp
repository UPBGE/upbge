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

#include "glew-mx.h"

RAS_IDisplayArray::RAS_IDisplayArray(PrimitiveType type)
	:m_type(type)
{
}

RAS_IDisplayArray::~RAS_IDisplayArray()
{
}

#define NEW_DISPLAY_ARRAY_UV(vertformat, uv, color, primtype) \
	if (vertformat.uvSize == uv && vertformat.colorSize == color) { \
		return new RAS_DisplayArray<RAS_TexVert<uv, color> >(primtype); \
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

RAS_IDisplayArray *RAS_IDisplayArray::ConstructArray(RAS_IDisplayArray::PrimitiveType type, const RAS_TexVertFormat &format)
{
	NEW_DISPLAY_ARRAY_COLOR(format, 1, type);
	NEW_DISPLAY_ARRAY_COLOR(format, 2, type);
	NEW_DISPLAY_ARRAY_COLOR(format, 3, type);
	NEW_DISPLAY_ARRAY_COLOR(format, 4, type);
	NEW_DISPLAY_ARRAY_COLOR(format, 5, type);
	NEW_DISPLAY_ARRAY_COLOR(format, 6, type);
	NEW_DISPLAY_ARRAY_COLOR(format, 7, type);
	NEW_DISPLAY_ARRAY_COLOR(format, 8, type);

	return NULL;
}
#undef NEW_DISPLAY_ARRAY_UV
#undef NEW_DISPLAY_ARRAY_COLOR

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
	if (flag & RAS_MeshObject::TANGENT_MODIFIED) {
		for (unsigned int i = 0, size = other->GetVertexCount(); i < size; ++i) {
			GetVertex(i)->SetTangent(MT_Vector4(other->GetVertex(i)->getTangent()));
		}
	}
	if (flag & RAS_MeshObject::UVS_MODIFIED) {
		for (unsigned int i = 0, size = other->GetVertexCount(); i < size; ++i) {
			for (unsigned int uv = 0, uvcount = min_ii(GetVertex(i)->getUvSize(), other->GetVertex(i)->getUvSize()); uv < uvcount; ++uv) {
				GetVertex(i)->SetUV(uv, MT_Vector2(other->GetVertex(i)->getUV(uv)));
			}
		}
	}
	if (flag & RAS_MeshObject::POSITION_MODIFIED) {
		for (unsigned int i = 0, size = other->GetVertexCount(); i < size; ++i) {
			GetVertex(i)->SetXYZ(MT_Vector3(other->GetVertex(i)->getXYZ()));
		}
	}
	if (flag & RAS_MeshObject::NORMAL_MODIFIED) {
		for (unsigned int i = 0, size = other->GetVertexCount(); i < size; ++i) {
			GetVertex(i)->SetNormal(MT_Vector3(other->GetVertex(i)->getNormal()));
		}
	}
	if (flag & RAS_MeshObject::COLORS_MODIFIED) {
		for (unsigned int i = 0, size = other->GetVertexCount(); i < size; ++i) {
			for (unsigned int color = 0, colorcount = min_ii(GetVertex(i)->getColorSize(), other->GetVertex(i)->getColorSize());
				 color < colorcount; ++color)
			{
				GetVertex(i)->SetRGBA(color, *((unsigned int *)other->GetVertex(i)->getRGBA(color)));
			}
		}
	}
}
