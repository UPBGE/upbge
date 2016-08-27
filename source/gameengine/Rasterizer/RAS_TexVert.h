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
	unsigned int uvSize;
	unsigned int colorSize;
};

template <unsigned int uvSize, unsigned int colorSize>
class RAS_TexVert : public RAS_ITexVert
{
friend class RAS_DisplayArray<RAS_TexVert<uvSize, colorSize> >;

private:
	float m_uvs[uvSize][2];
	unsigned int m_rgba[colorSize];

public:

	RAS_TexVert()
	{
	}

	RAS_TexVert(const MT_Vector3& xyz,
	            const MT_Vector2 uvs[uvSize],
	            const MT_Vector4& tangent,
				const unsigned int rgba[colorSize],
	            const MT_Vector3& normal)
		:RAS_ITexVert(xyz, tangent, normal)
	{
		for (int i = 0; i < uvSize; ++i) {
			uvs[i].getValue(m_uvs[i]);
		}

		for (unsigned short i = 0; i < colorSize; ++i) {
			m_rgba[i] = rgba[i];
		}
	}

	virtual ~RAS_TexVert()
	{
	}

	virtual const unsigned short getUvSize() const
	{
		return uvSize;
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

	virtual const unsigned short getColorSize() const
	{
		return colorSize;
	}

	virtual const unsigned char *getRGBA(const int index) const
	{
		return (unsigned char *)&m_rgba[index];
	}

	virtual void SetRGBA(const int index, const unsigned int rgba)
	{
		m_rgba[index] = rgba;
	}

	virtual void SetRGBA(const int index, const MT_Vector4& rgba)
	{
		unsigned char *colp = (unsigned char *)&m_rgba[index];
		colp[0] = (unsigned char)(rgba[0] * 255.0f);
		colp[1] = (unsigned char)(rgba[1] * 255.0f);
		colp[2] = (unsigned char)(rgba[2] * 255.0f);
		colp[3] = (unsigned char)(rgba[3] * 255.0f);
	}
};

#endif  // __RAS_TEXVERT_H__
