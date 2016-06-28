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

RAS_IDisplayArray *RAS_IDisplayArray::ConstructArray(RAS_IDisplayArray::PrimitiveType type, const RAS_TexVertFormat &format)
{
	switch (format.UVSize) {
		case 1:
			return new RAS_DisplayArray<RAS_TexVert<1> >(type);
		case 2:
			return new RAS_DisplayArray<RAS_TexVert<2> >(type);
		case 3:
			return new RAS_DisplayArray<RAS_TexVert<3> >(type);
		case 4:
			return new RAS_DisplayArray<RAS_TexVert<4> >(type);
		case 5:
			return new RAS_DisplayArray<RAS_TexVert<5> >(type);
		case 6:
			return new RAS_DisplayArray<RAS_TexVert<6> >(type);
		case 7:
			return new RAS_DisplayArray<RAS_TexVert<7> >(type);
		case 8:
			return new RAS_DisplayArray<RAS_TexVert<8> >(type);
	};

	return NULL;
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
	if (flag & RAS_MeshObject::TANGENT_MODIFIED) {
		for (unsigned int i = 0, size = other->GetVertexCount(); i < size; ++i) {
			GetVertex(i)->SetTangent(MT_Vector4(other->GetVertex(i)->getTangent()));
		}
	}
	if (flag & RAS_MeshObject::UVS_MODIFIED) {
		for (unsigned int i = 0, size = other->GetVertexCount(); i < size; ++i) {
			for (unsigned int uv = 0, uvcount = min_ii(GetVertex(i)->getUVSize(), other->GetVertex(i)->getUVSize()); uv < uvcount; ++uv) {
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
			GetVertex(i)->SetRGBA(*((unsigned int *)other->GetVertex(i)->getRGBA()));
		}
	}
}
