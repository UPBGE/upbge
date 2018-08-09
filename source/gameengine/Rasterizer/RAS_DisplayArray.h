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

/** \file RAS_DisplayArray.h
 *  \ingroup bgerast
 */

#ifndef __RAS_IDISPLAY_ARRAY_H__
#define __RAS_IDISPLAY_ARRAY_H__

#include "RAS_DisplayArrayStorage.h"
#include "RAS_DisplayArrayLayout.h"
#include "RAS_VertexInfo.h"

#include "CM_Update.h"

#include "BLI_math_vector.h"

#include "mathfu.h"

#include <vector>
#include <algorithm>
#include <memory>

class RAS_BatchDisplayArray;
class RAS_StorageVbo;

class RAS_DisplayArray : public CM_UpdateServer<RAS_DisplayArray>, public mt::SimdClassAllocator
{
	friend RAS_BatchDisplayArray;
	friend RAS_StorageVbo;

public:
	enum PrimitiveType {
		TRIANGLES,
		LINES,
	};

	enum Type {
		NORMAL,
		BATCHING
	};

	/// Modification categories.
	enum {
		NONE_MODIFIED = 0,
		POSITION_MODIFIED = 1 << 0, // Vertex position modified.
		NORMAL_MODIFIED = 1 << 1, // Vertex normal modified.
		UVS_MODIFIED = 1 << 2, // Vertex UVs modified.
		COLORS_MODIFIED = 1 << 3, // Vertex colors modified.
		TANGENT_MODIFIED = 1 << 4, // Vertex tangent modified.
		SIZE_MODIFIED = 1 << 5, // Vertex and index array changed of size.
		STORAGE_INVALID = 1 << 6, // Storage not yet created.
		AABB_MODIFIED = POSITION_MODIFIED,
		MESH_MODIFIED = POSITION_MODIFIED | NORMAL_MODIFIED | UVS_MODIFIED |
						COLORS_MODIFIED | TANGENT_MODIFIED,
		ANY_MODIFIED = MESH_MODIFIED | SIZE_MODIFIED | STORAGE_INVALID
	};

	/// Struct used to pass the vertex format to functions.
	struct Format
	{
		uint8_t uvSize;
		uint8_t colorSize;

		Format() = default;

		/// Operators used to compare the contents (uv size, color size, ...) of two vertex formats.
		inline bool operator== (const Format& other) const
		{
			return (uvSize == other.uvSize && colorSize == other.colorSize);
		}

		inline bool operator!= (const Format& other) const
		{
			return !(*this == other);
		}
	};

protected:
	/// The display array primitive type.
	PrimitiveType m_type;
	/// The vertex format used.
	Format m_format;

	struct VertexData
	{
		std::vector<mt::vec3_packed> positions;
		std::vector<mt::vec3_packed> normals;
		std::vector<mt::vec4_packed> tangents;
		std::vector<mt::vec2_packed> uvs[RAS_Texture::MaxUnits];

		union Color
		{
			unsigned int m_flat;
			unsigned char m_array[4];
		};

		std::vector<Color> colors[RAS_Texture::MaxUnits];
	} m_vertexData;

	/// The vertex infos unused for rendering, e.g original or soft body index, flag.
	std::vector<RAS_VertexInfo> m_vertexInfos;
	/// The indices used for rendering.
	std::vector<unsigned int> m_primitiveIndices;
	/// The indices of the original triangle independently of the primitive type.
	std::vector<unsigned int> m_triangleIndices;

	/// Maximum original vertex index.
	unsigned int m_maxOrigIndex;

	/** Polygon centers cache used to sort polygons depending on depth.
	 * This list is stored here because we sort per array not per entire mesh.
	 */
	std::vector<mt::vec3, mt::simd_allocator<mt::vec3> > m_polygonCenters;

	/// AABB used for culling or for sorting center.
	mt::vec3 m_aabbMin;
	mt::vec3 m_aabbMax;
	mt::vec3 m_aabbCenter;
	float m_aabbRadius;

	/// The OpenGL data storage used for rendering.
	RAS_DisplayArrayStorage m_storage;

public:
	RAS_DisplayArray(PrimitiveType type, const Format& format);
	RAS_DisplayArray(const RAS_DisplayArray& other);
	~RAS_DisplayArray();

	inline mt::vec3_packed& GetPosition(const unsigned int index)
	{
		return m_vertexData.positions[index];
	}

	inline mt::vec3_packed& GetNormal(const unsigned int index)
	{
		return m_vertexData.normals[index];
	}

	inline mt::vec4_packed& GetTangent(const unsigned int index)
	{
		return m_vertexData.tangents[index];
	}

	inline mt::vec2_packed& GetUv(const unsigned int index, const unsigned short layer)
	{
		return m_vertexData.uvs[layer][index];
	}

	inline unsigned char(&GetColor(const unsigned int index, const unsigned short layer))[4]
	{
		return m_vertexData.colors[layer][index].m_array;
	}

	inline unsigned int& GetRawColor(const unsigned int index, const unsigned short layer)
	{
		return m_vertexData.colors[layer][index].m_flat;
	}

	inline void SetPosition(const unsigned int index, const mt::vec3_packed& value)
	{
		m_vertexData.positions[index] = value;
	}

	inline void SetPosition(const unsigned int index, const mt::vec3& value)
	{
		m_vertexData.positions[index] = value;
	}

	inline void SetNormal(const unsigned int index, const mt::vec3_packed& value)
	{
		m_vertexData.normals[index] = value;
	}

	inline void SetNormal(const unsigned int index, const mt::vec3& value)
	{
		m_vertexData.normals[index] = value;
	}
	
	inline void SetTangent(const unsigned int index, const mt::vec4_packed& value)
	{
		m_vertexData.tangents[index] = value;
	}

	inline void SetTangent(const unsigned int index, const mt::vec4& value)
	{
		m_vertexData.tangents[index] = value;
	}

	inline void SetUv(const unsigned int index, const unsigned short layer, const mt::vec2_packed& value)
	{
		m_vertexData.uvs[layer][index] = value;
	}

	inline void SetUv(const unsigned int index, const unsigned short layer, const mt::vec2& value)
	{
		m_vertexData.uvs[layer][index] = value;
	}

	inline void SetColor(const unsigned int index, const unsigned short layer, const unsigned char value[4])
	{
		VertexData::Color color;
		copy_v4_v4_uchar(color.m_array, value);
		m_vertexData.colors[layer][index] = color;
	}

	inline void SetColor(const unsigned int index, const unsigned short layer, const unsigned int value)
	{
		m_vertexData.colors[layer][index].m_flat = value;
	}

	inline void SetColor(const unsigned int index, const unsigned short layer, const mt::vec4& col)
	{
		VertexData::Color& color = m_vertexData.colors[layer][index];
		for (unsigned short i = 0; i < 4; ++i) {
			color.m_array[i] = (unsigned char)(col[i] * 255.0f);
		}
	}

	inline unsigned int GetPrimitiveIndex(const unsigned int index) const
	{
		return m_primitiveIndices[index];
	}

	inline unsigned int GetTriangleIndex(const unsigned int index) const
	{
		return m_triangleIndices[index];
	}

	inline const RAS_VertexInfo& GetVertexInfo(const unsigned int index) const
	{
		return m_vertexInfos[index];
	}

	inline RAS_VertexInfo& GetVertexInfo(const unsigned int index)
	{
		return m_vertexInfos[index];
	}

	unsigned int AddVertex(const mt::vec3_packed& pos, const mt::vec3_packed& nor, const mt::vec4_packed& tan,
			mt::vec2_packed uvs[RAS_Texture::MaxUnits], unsigned int colors[RAS_Texture::MaxUnits], unsigned int origIndex, uint8_t flag);

	inline void AddPrimitiveIndex(const unsigned int index)
	{
		m_primitiveIndices.push_back(index);
	}

	inline void AddTriangleIndex(const unsigned int origIndex)
	{
		m_triangleIndices.push_back(origIndex);
	}

	void Clear();

	inline unsigned int GetVertexCount() const
	{
		return m_vertexInfos.size();
	}

	inline unsigned int GetPrimitiveIndexCount() const
	{
		return m_primitiveIndices.size();
	}

	inline unsigned int GetTriangleIndexCount() const
	{
		return m_triangleIndices.size();
	}

	inline unsigned int GetMaxOrigIndex() const
	{
		return m_maxOrigIndex;
	}

	void SortPolygons(const mt::mat3x4& transform, unsigned int *indexmap);
	void InvalidatePolygonCenters();

	/** Copy vertex data from an other display array. Different vertex type is allowed.
	 * \param other The other display array to copy from.
	 * \param flag The flag coresponding to datas to copy.
	 */
	void UpdateFrom(RAS_DisplayArray *other, int flag);

	void GetAabb(mt::vec3& aabbMin, mt::vec3& aabbMax) const;
	const mt::vec3& GetAabbCenter() const;
	float GetAabbRadius() const;
	void UpdateAabb();

	/// Return the primitive type used for indices.
	PrimitiveType GetPrimitiveType() const;
	/// Return the primitive type used for indices in OpenGL value.
	int GetOpenGLPrimitiveType() const;

	/// Return the vertex format used.
	const Format& GetFormat() const;

	/// Return the vertex memory format used.
	RAS_DisplayArrayLayout GetLayout() const;

	/// Return the type of the display array.
	virtual Type GetType() const;

	RAS_DisplayArrayStorage& GetStorage();
	void ConstructStorage();
};

using RAS_DisplayArrayList = std::vector<RAS_DisplayArray *>;

#endif  // __RAS_IDISPLAY_ARRAY_H__
