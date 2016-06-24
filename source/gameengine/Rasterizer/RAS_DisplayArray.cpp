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
 * The Original Code is: all of this file.
 *
 * Contributor(s): Porteries Tristan.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file RAS_DisplayArray.cpp
 *  \ingroup bgerast
 */

#include "RAS_DisplayArray.h"
#include "RAS_MeshObject.h"

#include "glew-mx.h"

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
	if (flag & (RAS_MeshObject::POSITION_MODIFIED | RAS_MeshObject::NORMAL_MODIFIED | RAS_MeshObject::TANGENT_MODIFIED | RAS_MeshObject::UVS_MODIFIED | RAS_MeshObject::COLORS_MODIFIED)) {
		for (unsigned int i = 0; i < other->m_vertex.size(); ++i) {
			switch (flag)
			{
			case RAS_MeshObject::POSITION_MODIFIED:
				m_vertex[i].SetXYZ(MT_Vector3(other->m_vertex[i].getXYZ()));
			case RAS_MeshObject::NORMAL_MODIFIED:
				m_vertex[i].SetNormal(MT_Vector3(other->m_vertex[i].getNormal()));
			case RAS_MeshObject::TANGENT_MODIFIED:
				m_vertex[i].SetTangent(MT_Vector4(other->m_vertex[i].getTangent()));
			case RAS_MeshObject::UVS_MODIFIED:
				for (unsigned int uv = 0; uv < 8; ++uv) {
					m_vertex[i].SetUV(uv, MT_Vector2(other->m_vertex[i].getUV(uv)));
				}
			case RAS_MeshObject::COLORS_MODIFIED:
				m_vertex[i].SetRGBA(*((unsigned int *)other->m_vertex[i].getRGBA()));
			}
		}
	}
}

