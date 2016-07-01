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

/** \file RAS_TexVert.h
 *  \ingroup bgerast
 */

#ifndef __RAS_TEXVERT_H__
#define __RAS_TEXVERT_H__

#include "MT_Vector3.h"
#include "MT_Vector2.h"
#include "MT_Matrix4x4.h"
#include "MT_Transform.h"

#include "BLI_math.h"

#ifdef WITH_CXX_GUARDEDALLOC
#include "MEM_guardedalloc.h"
#endif

template <class Vertex>
class RAS_DisplayArray;

class RAS_ITexVert
{
protected:
	float m_localxyz[3]; // 3 * 4 = 12
	unsigned int m_rgba; // 4
	float m_tangent[4]; // 4*4 = 16
	float m_normal[3]; // 3*4 = 12
	short m_flag; // 2
	short m_softBodyIndex; //2
	unsigned int m_origindex; // 4

public:
	enum
	{
		FLAT = 1,
		MAX_UNIT = 8
	};

	RAS_ITexVert()
	{
	}
	RAS_ITexVert(const MT_Vector3& xyz,
	            const MT_Vector4& tangent,
	            const unsigned int rgba,
	            const MT_Vector3& normal,
	            const bool flat,
	            const unsigned int origindex);

	virtual ~RAS_ITexVert()
	{
	}

	virtual const unsigned short getUVSize() const = 0;
	virtual const float *getUV(const int unit) const = 0;

	virtual void SetUV(const int index, const MT_Vector2& uv) = 0;
	virtual void SetUV(const int index, const float uv[2]) = 0;

	inline const float *getXYZ() const
	{
		return m_localxyz;
	}

	inline const float *getNormal() const
	{
		return m_normal;
	}

	inline short int getSoftBodyIndex() const
	{
		return m_softBodyIndex;
	}

	inline void setSoftBodyIndex(short int sbIndex)
	{
		m_softBodyIndex = sbIndex;
	}

	inline const short getFlag() const
	{
		return m_flag;
	}

	inline const float *getTangent() const
	{
		return m_tangent;
	}

	inline const unsigned char *getRGBA() const
	{
		return (unsigned char *)&m_rgba;
	}

	inline const unsigned int getOrigIndex() const
	{
		return m_origindex;
	}

	inline MT_Vector3 xyz() const
	{
		return MT_Vector3(m_localxyz);
	}

	inline void SetRGBA(const MT_Vector4& rgba)
	{
		unsigned char *colp = (unsigned char *)&m_rgba;
		colp[0] = (unsigned char)(rgba[0] * 255.0f);
		colp[1] = (unsigned char)(rgba[1] * 255.0f);
		colp[2] = (unsigned char)(rgba[2] * 255.0f);
		colp[3] = (unsigned char)(rgba[3] * 255.0f);
	}


	inline void SetXYZ(const MT_Vector3& xyz)
	{
		xyz.getValue(m_localxyz);
	}

	inline void SetXYZ(const float xyz[3])
	{
		copy_v3_v3(m_localxyz, xyz);
	}

	inline void SetRGBA(const unsigned int rgba)
	{
		m_rgba = rgba;
	}

	inline void SetFlag(const short flag)
	{
		m_flag = flag;
	}

	inline void SetNormal(const MT_Vector3& normal)
	{
		normal.getValue(m_normal);
	}

	inline void SetTangent(const MT_Vector4& tangent)
	{
		tangent.getValue(m_tangent);
	}

	// compare two vertices, to test if they can be shared, used for
	// splitting up based on uv's, colors, etc
	inline const bool closeTo(const RAS_ITexVert *other)
	{
		static const float eps = FLT_EPSILON;
		for (int i = 0, size = min_ii(getUVSize(), other->getUVSize()); i < size; ++i) {
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

	inline void Transform(const MT_Matrix4x4& mat, const MT_Matrix4x4& nmat)
	{
		SetXYZ((mat * MT_Vector4(m_localxyz[0], m_localxyz[1], m_localxyz[2], 1.0f)).to3d());
		SetNormal((nmat * MT_Vector4(m_normal[0], m_normal[1], m_normal[2], 1.0f)).to3d());
		SetTangent((nmat * MT_Vector4(m_tangent[0], m_tangent[1], m_tangent[2], 1.0f)));
	}

	inline void TransformUV(const int index, const MT_Matrix4x4& mat)
	{
		SetUV(index, (mat * MT_Vector4(getUV(index)[0], getUV(index)[1], 0.0f, 1.0f)).to2d());
	}
};

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
	            const MT_Vector3& normal,
	            const bool flat,
	            const unsigned int origindex)
		:RAS_ITexVert(xyz, tangent, rgba, normal, flat, origindex)
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

#endif  /* __RAS_TEXVERT_H__ */
