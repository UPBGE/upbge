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

/** \file RAS_Vertex.h
 *  \ingroup bgerast
 */

#ifndef __RAS_TEXVERT_H__
#define __RAS_TEXVERT_H__

#include "RAS_VertexData.h"

#include "MT_Matrix4x4.h"

#include "BLI_math_vector.h"

/// Struct used to pass the vertex format to functions.
struct RAS_VertexFormat
{
	unsigned short uvSize;
	unsigned short colorSize;
};

/// Operators used to compare the contents (uv size, color size, ...) of two vertex formats.
bool operator== (const RAS_VertexFormat& format1, const RAS_VertexFormat& format2);
bool operator!= (const RAS_VertexFormat& format1, const RAS_VertexFormat& format2);

class RAS_VertexInfo
{
public:
	enum {
		FLAT = 1,
	};

private:
	unsigned int m_origindex;
	short m_softBodyIndex;
	short m_flag;

public:
	RAS_VertexInfo(unsigned int origindex, bool flat);
	~RAS_VertexInfo();

	inline const unsigned int GetOrigIndex() const
	{
		return m_origindex;
	}

	inline short int GetSoftBodyIndex() const
	{
		return m_softBodyIndex;
	}

	inline void SetSoftBodyIndex(short int sbIndex)
	{
		m_softBodyIndex = sbIndex;
	}

	inline const short GetFlag() const
	{
		return m_flag;
	}

	inline void SetFlag(const short flag)
	{
		m_flag = flag;
	}
};

class RAS_Vertex
{
public:
	enum {
		MAX_UNIT = 8
	};

private:
	RAS_IVertexData *m_data;
	RAS_VertexFormat m_format;

	inline float *GetUvInternal(const unsigned short index) const
	{
		return (float *)(intptr_t(m_data) + (sizeof(RAS_VertexDataBasic) + sizeof(float[2]) * index));
	}

	inline unsigned int *GetColorInternal(const unsigned short index) const
	{
		return (unsigned int *)(intptr_t(m_data) + (sizeof(RAS_VertexDataBasic) + sizeof(float[2]) * m_format.uvSize + sizeof(unsigned int) * index));
	}

public:
	RAS_Vertex(RAS_IVertexData *data, const RAS_VertexFormat& format)
		:m_data(data),
		m_format(format)
	{
	}

	~RAS_Vertex()
	{
	}

	inline RAS_IVertexData *GetData() const
	{
		return m_data;
	}

	inline const RAS_VertexFormat& GetFormat() const
	{
		return m_format;
	}

	inline const float *GetXYZ() const
	{
		return m_data->position;
	}

	inline const float *GetNormal() const
	{
		return m_data->normal;
	}

	inline const float *GetTangent() const
	{
		return m_data->tangent;
	}

	inline MT_Vector3 xyz() const
	{
		return MT_Vector3(m_data->position);
	}

	inline void SetXYZ(const MT_Vector3& xyz)
	{
		xyz.getValue(m_data->position);
	}

	inline void SetXYZ(const float xyz[3])
	{
		copy_v3_v3(m_data->position, xyz);
	}

	inline void SetNormal(const MT_Vector3& normal)
	{
		normal.getValue(m_data->normal);
	}

	inline void SetNormal(const float normal[3])
	{
		copy_v3_v3(m_data->normal, normal);
	}

	inline void SetTangent(const MT_Vector4& tangent)
	{
		tangent.getValue(m_data->tangent);
	}

	inline const float *GetUv(const int index) const
	{
		return GetUvInternal(index);
	}

	inline void SetUV(const int index, const MT_Vector2& uv)
	{
		uv.getValue(GetUvInternal(index));
	}

	inline void SetUV(const int index, const float uv[2])
	{
		copy_v2_v2(GetUvInternal(index), uv);
	}

	inline const unsigned char *GetColor(const int index) const
	{
		return (unsigned char *)GetColorInternal(index);
	}

	inline const unsigned int GetRawColor(const int index) const
	{
		return *GetColorInternal(index);
	}

	inline void SetRGBA(const int index, const unsigned int rgba)
	{
		*GetColorInternal(index) = rgba;
	}

	inline void SetRGBA(const int index, const MT_Vector4& rgba)
	{
		unsigned char *colp = (unsigned char *)GetColorInternal(index);
		colp[0] = (unsigned char)(rgba[0] * 255.0f);
		colp[1] = (unsigned char)(rgba[1] * 255.0f);
		colp[2] = (unsigned char)(rgba[2] * 255.0f);
		colp[3] = (unsigned char)(rgba[3] * 255.0f);
	}

	// compare two vertices, to test if they can be shared, used for
	// splitting up based on uv's, colors, etc
	inline const bool CloseTo(const RAS_Vertex& other)
	{
		BLI_assert(m_format == other.GetFormat());
		static const float eps = FLT_EPSILON;
		for (int i = 0, size = m_format.uvSize; i < size; ++i) {
			if (!compare_v2v2(GetUv(i), other.GetUv(i), eps)) {
				return false;
			}
		}

		for (int i = 0, size = m_format.colorSize; i < size; ++i) {
			if (GetRawColor(i) != other.GetRawColor(i)) {
				return false;
			}
		}

		return (/* m_flag == other->m_flag && */
				/* at the moment the face only stores the smooth/flat setting so don't bother comparing it */
				compare_v3v3(m_data->normal, other.m_data->normal, eps) &&
				compare_v3v3(m_data->tangent, other.m_data->tangent, eps)
				/* don't bother comparing m_data->position since we know there from the same vert */
				/* && compare_v3v3(m_data->position, other->m_data->position, eps))*/
				);
	}

	inline void Transform(const MT_Matrix4x4& mat, const MT_Matrix4x4& nmat)
	{
		SetXYZ((mat * MT_Vector4(m_data->position[0], m_data->position[1], m_data->position[2], 1.0f)).to3d());
		SetNormal((nmat * MT_Vector4(m_data->normal[0], m_data->normal[1], m_data->normal[2], 1.0f)).to3d());
		SetTangent((nmat * MT_Vector4(m_data->tangent[0], m_data->tangent[1], m_data->tangent[2], 1.0f)));
	}

	inline void TransformUv(const int index, const MT_Matrix4x4& mat)
	{
		SetUV(index, (mat * MT_Vector4(GetUv(index)[0], GetUv(index)[1], 0.0f, 1.0f)).to2d());
	}
};

#endif  // __RAS_TEXVERT_H__
