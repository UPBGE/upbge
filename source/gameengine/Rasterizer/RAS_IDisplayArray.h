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

/** \file RAS_IDisplayArray.h
 *  \ingroup bgerast
 */

#ifndef __RAS_IDISPLAY_ARRAY_H__
#define __RAS_IDISPLAY_ARRAY_H__

#include "RAS_DisplayArrayStorage.h"
#include "RAS_VertexData.h"
#include "RAS_Vertex.h"

#include "CM_Update.h"

#include <vector>
#include <algorithm>
#include <memory>

class RAS_IDisplayArray : public CM_UpdateServer<RAS_IDisplayArray>
{
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

protected:
	/// The display array primitive type.
	PrimitiveType m_type;
	/// The vertex format used.
	RAS_VertexFormat m_format;
	/// The vertex memory format used.
	RAS_VertexDataMemoryFormat m_memoryFormat;

	/// The vertex infos unused for rendering, e.g original or soft body index, flag.
	std::vector<RAS_VertexInfo> m_vertexInfos;
	/// Cached vertex data pointers. This list is constructed with the function UpdateCache.
	std::vector<RAS_IVertexData *> m_vertexDataPtrs;
	/// The indices used for rendering.
	std::vector<unsigned int> m_primitiveIndices;
	/// The indices of the original triangle independently of the primitive type.
	std::vector<unsigned int> m_triangleIndices;

	/// Maximum original vertex index.
	unsigned int m_maxOrigIndex;

	/** Polygon centers cache used to sort polygons depending on depth.
	 * This list is stored here because we store per array not per entire mesh.
	 */
	std::vector<mt::vec3, mt::simd_allocator<mt::vec3> > m_polygonCenters;

	/// The OpenGL data storage used for rendering.
	RAS_DisplayArrayStorage m_storage;

	RAS_IDisplayArray(const RAS_IDisplayArray& other);

public:
	RAS_IDisplayArray(PrimitiveType type, const RAS_VertexFormat& format,
			const RAS_VertexDataMemoryFormat& memoryFormat);
	virtual ~RAS_IDisplayArray();

	virtual RAS_IDisplayArray *GetReplica() = 0;

	/** Construct the display array corresponding of the vertex of the given format.
	 * \param type The type of primitives, one of the enumeration PrimitiveType.
	 * \param format The format of vertex to use.
	 */
	static RAS_IDisplayArray *ConstructArray(PrimitiveType type, const RAS_VertexFormat &format);

	/** Return a vertex pointer without using the cache. Used to get
	 * a vertex pointer during contruction.
	 */
	virtual RAS_Vertex GetVertexNoCache(const unsigned int index) = 0;

	inline RAS_Vertex GetVertex(const unsigned int index)
	{
		return RAS_Vertex(m_vertexDataPtrs[index], m_format);
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

	virtual unsigned int AddVertex(const RAS_Vertex& vert) = 0;

	virtual void DeleteVertexData(const RAS_Vertex& vert) = 0;

	inline void AddPrimitiveIndex(const unsigned int index)
	{
		m_primitiveIndices.push_back(index);
	}

	inline void AddTriangleIndex(const unsigned int origIndex)
	{
		m_triangleIndices.push_back(origIndex);
	}

	inline void AddVertexInfo(const RAS_VertexInfo& info)
	{
		m_maxOrigIndex = std::max(m_maxOrigIndex, info.GetOrigIndex());
		m_vertexInfos.push_back(info);
	}

	virtual void Clear() = 0;

	virtual const RAS_IVertexData *GetVertexPointer() const = 0;

	inline const unsigned int *GetPrimitiveIndexPointer() const
	{
		return (unsigned int *)m_primitiveIndices.data();
	}

	virtual unsigned int GetVertexCount() const = 0;

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

	virtual RAS_Vertex CreateVertex(
				const mt::vec3& xyz,
				const mt::vec2 * const uvs,
				const mt::vec4& tangent,
				const unsigned int *rgba,
				const mt::vec3& normal) = 0;

	virtual RAS_Vertex CreateVertex(
				const float xyz[3],
				const float (*uvs)[2],
				const float tangent[4],
				const unsigned int *rgba,
				const float normal[3]) = 0;

	/** Copy vertex data from an other display array. Different vertex type is allowed.
	 * \param other The other display array to copy from.
	 * \param flag The flag coresponding to datas to copy.
	 */
	void UpdateFrom(RAS_IDisplayArray *other, int flag);

	/// Copy vertex pointers to the cache list m_vertexPtrs.
	virtual void UpdateCache() = 0;

	/// Return the primitive type used for indices.
	PrimitiveType GetPrimitiveType() const;
	/// Return the primitive type used for indices in OpenGL value.
	int GetOpenGLPrimitiveType() const;

	/// Return the vertex format used.
	const RAS_VertexFormat& GetFormat() const;

	/// Return the vertex memory format used.
	const RAS_VertexDataMemoryFormat& GetMemoryFormat() const;

	/// Return the type of the display array.
	virtual Type GetType() const;

	RAS_DisplayArrayStorage *GetStorage();
	void ConstructStorage();
};

typedef std::vector<RAS_IDisplayArray *> RAS_IDisplayArrayList;

#endif  // __RAS_IDISPLAY_ARRAY_H__
