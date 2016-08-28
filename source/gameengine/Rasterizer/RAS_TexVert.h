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

/** \file RAS_TexVert.h
 *  \ingroup bgerast
 */

#ifndef __RAS_TEXVERT_H__
#define __RAS_TEXVERT_H__

#include "RAS_ITexVert.h"

template <class Vertex>
class RAS_DisplayArray;

// Struct used to pass the vertex format to functions.
struct RAS_TexVertFormat
{
	unsigned int UVSize;
};

template <unsigned int UVSize>
class RAS_TexVert : public RAS_ITexVert
{
friend class RAS_DisplayArray<RAS_TexVert<UVSize> >;

private:
	float m_uvs[UVSize][2];

public:

	RAS_TexVert()
	{
	}

	RAS_TexVert(const MT_Vector3& xyz,
	            const MT_Vector2 uvs[UVSize],
	            const MT_Vector4& tangent,
	            const unsigned int rgba,
	            const MT_Vector3& normal)
		:RAS_ITexVert(xyz, tangent, rgba, normal)
	{
		for (int i = 0; i < UVSize; ++i) {
			uvs[i].getValue(m_uvs[i]);
		}
	}

	virtual ~RAS_TexVert()
	{
	}

	virtual const unsigned short getUVSize() const
	{
		return UVSize;
	}

	virtual const float *getUV(const int unit) const
	{
		return m_uvs[unit];
	}

	virtual void SetUV(const int index, const MT_Vector2& uv)
	{
		uv.getValue(m_uvs[index]);
	}

	virtual void SetUV(const int index, const float uv[2])
	{
		copy_v2_v2(m_uvs[index], uv);
	}
};

#endif  // __RAS_TEXVERT_H__
