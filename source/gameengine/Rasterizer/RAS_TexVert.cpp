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
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/Rasterizer/RAS_TexVert.cpp
 *  \ingroup bgerast
 */


#include "RAS_TexVert.h"
#include "MT_Matrix4x4.h"

RAS_ITexVert::RAS_ITexVert(const MT_Vector3& xyz,
						 const MT_Vector4& tangent,
						 const unsigned int rgba,
						 const MT_Vector3& normal,
						 const bool flat,
						 const unsigned int origindex)
{
	xyz.getValue(m_localxyz);
	SetRGBA(rgba);
	SetNormal(normal);
	SetTangent(tangent);
	m_flag = (flat) ? FLAT : 0;
	m_origindex = origindex;
	m_softBodyIndex = -1;
}

MT_Vector3 RAS_ITexVert::xyz() const
{
	return MT_Vector3(m_localxyz);
}

void RAS_ITexVert::SetRGBA(const MT_Vector4& rgba)
{
	unsigned char *colp = (unsigned char *)&m_rgba;
	colp[0] = (unsigned char)(rgba[0] * 255.0f);
	colp[1] = (unsigned char)(rgba[1] * 255.0f);
	colp[2] = (unsigned char)(rgba[2] * 255.0f);
	colp[3] = (unsigned char)(rgba[3] * 255.0f);
}


void RAS_ITexVert::SetXYZ(const MT_Vector3& xyz)
{
	xyz.getValue(m_localxyz);
}

void RAS_ITexVert::SetXYZ(const float xyz[3])
{
	copy_v3_v3(m_localxyz, xyz);
}

void RAS_ITexVert::SetRGBA(const unsigned int rgba)
{
	m_rgba = rgba;
}

void RAS_ITexVert::SetFlag(const short flag)
{
	m_flag = flag;
}

void RAS_ITexVert::SetNormal(const MT_Vector3& normal)
{
	normal.getValue(m_normal);
}

void RAS_ITexVert::SetTangent(const MT_Vector4& tangent)
{
	tangent.getValue(m_tangent);
}

// compare two vertices, and return true if both are almost identical (they can be shared)
bool RAS_ITexVert::closeTo(const RAS_ITexVert *other)
{
	const float eps = FLT_EPSILON;
	for (int i = 0; i < std::min(getUVSize(), other->getUVSize()); i++) {
		if (!compare_v2v2(getUV(i), other->getUV(i), eps)) {
			return false;
		}
	}

	return (/* m_flag == other->m_flag && */
		    /* at the moment the face only stores the smooth/flat setting so don't bother comparing it */
		   (m_rgba == other->m_rgba) &&
		   compare_v3v3(m_normal, other->m_normal, eps) &&
		   compare_v3v3(m_tangent, other->m_tangent, eps)
			/* don't bother comparing m_localxyz since we know there from the same vert */
			/* && compare_v3v3(m_localxyz, other->m_localxyz, eps))*/
		   );
}

short RAS_ITexVert::getFlag() const
{
	return m_flag;
}

void RAS_ITexVert::Transform(const MT_Matrix4x4& mat, const MT_Matrix4x4& nmat)
{
	SetXYZ((mat * MT_Vector4(m_localxyz[0], m_localxyz[1], m_localxyz[2], 1.0f)).to3d());
	SetNormal((nmat * MT_Vector4(m_normal[0], m_normal[1], m_normal[2], 1.0f)).to3d());
	SetTangent((nmat * MT_Vector4(m_tangent[0], m_tangent[1], m_tangent[2], 1.0f)));
}

void RAS_ITexVert::TransformUV(int index, const MT_Matrix4x4& mat)
{
	SetUV(index, (mat * MT_Vector4(getUV(index)[0], getUV(index)[1], 0.0f, 1.0f)).to2d());
}

RAS_ITexVertFactory::~RAS_ITexVertFactory()
{
}

RAS_ITexVertFactory *RAS_ITexVertFactory::CreateFactory(const RAS_TexVertFormat& format)
{
	switch (format.UVSize) {
		case 1:
			return new RAS_TexVertFactory<RAS_TexVert<1> >();
		case 2:
			return new RAS_TexVertFactory<RAS_TexVert<2> >();
		case 3:
			return new RAS_TexVertFactory<RAS_TexVert<3> >();
		case 4:
			return new RAS_TexVertFactory<RAS_TexVert<4> >();
		case 5:
			return new RAS_TexVertFactory<RAS_TexVert<5> >();
		case 6:
			return new RAS_TexVertFactory<RAS_TexVert<6> >();
		case 7:
			return new RAS_TexVertFactory<RAS_TexVert<7> >();
		case 8:
			return new RAS_TexVertFactory<RAS_TexVert<8> >();
	}

	return NULL;
}

