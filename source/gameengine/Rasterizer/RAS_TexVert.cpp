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

