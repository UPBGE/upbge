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

	short getFlag() const;

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

	virtual unsigned int GetMemorySize() const = 0;

	virtual unsigned short getUVSize() const = 0;
	virtual const float *getUV(int unit) const = 0;

	const float *getXYZ() const
	{
		return m_localxyz;
	}

	const float *getNormal() const
	{
		return m_normal;
	}

	short int getSoftBodyIndex() const
	{
		return m_softBodyIndex;
	}

	void setSoftBodyIndex(short int sbIndex)
	{
		m_softBodyIndex = sbIndex;
	}

	const float *getTangent() const
	{
		return m_tangent;
	}

	const unsigned char *getRGBA() const
	{
		return (unsigned char *)&m_rgba;
	}

	unsigned int getOrigIndex() const
	{
		return m_origindex;
	}

	void SetXYZ(const MT_Vector3& xyz);
	void SetXYZ(const float xyz[3]);
	virtual void SetUV(int index, const MT_Vector2& uv) = 0;
	virtual void SetUV(int index, const float uv[2]) = 0;

	void SetRGBA(const unsigned int rgba);
	void SetNormal(const MT_Vector3& normal);
	void SetTangent(const MT_Vector4& tangent);
	void SetFlag(const short flag);

	void SetRGBA(const MT_Vector4& rgba);
	MT_Vector3 xyz() const;

	void Transform(const MT_Matrix4x4& mat,
				   const MT_Matrix4x4& nmat);
	void TransformUV(int index, const MT_Matrix4x4& mat);

	// compare two vertices, to test if they can be shared, used for
	// splitting up based on uv's, colors, etc
	bool closeTo(const RAS_ITexVert *other);
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

	virtual unsigned int GetMemorySize() const
	{
		return sizeof(RAS_TexVert<UVSize>);
	}

	virtual unsigned short getUVSize() const
	{
		return UVSize;
	}

	virtual const float *getUV(int unit) const
	{
		return m_uvs[unit];
	}

	virtual void SetUV(int index, const MT_Vector2& uv)
	{
		uv.getValue(m_uvs[index]);
	}

	virtual void SetUV(int index, const float uv[2])
	{
		copy_v2_v2(m_uvs[index], uv);
	}
};

class RAS_ITexVertFactory
{
public:
	virtual ~RAS_ITexVertFactory();
	static RAS_ITexVertFactory *CreateFactory(const RAS_TexVertFormat& format);
	virtual RAS_ITexVert *CreateVertex(
				const MT_Vector3& xyz,
				const MT_Vector2 uvs[RAS_ITexVert::MAX_UNIT],
				const MT_Vector4& tangent,
				const unsigned int rgba,
				const MT_Vector3& normal,
				const bool flat,
				const unsigned int origindex) = 0;
};

template <class Vertex>
class RAS_TexVertFactory : public RAS_ITexVertFactory
{
public:
	virtual ~RAS_TexVertFactory()
	{
	}

	virtual RAS_ITexVert *CreateVertex(
				const MT_Vector3& xyz,
				const MT_Vector2 uvs[RAS_ITexVert::MAX_UNIT],
				const MT_Vector4& tangent,
				const unsigned int rgba,
				const MT_Vector3& normal,
				const bool flat,
				const unsigned int origindex)
	{
		return new Vertex(xyz, uvs, tangent, rgba, normal, flat, origindex);
	}
};

#endif  /* __RAS_TEXVERT_H__ */
