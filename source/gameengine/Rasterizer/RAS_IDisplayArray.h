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

#include "RAS_Vertex.h"
#include <vector>
#include <algorithm>

class RAS_IDisplayArray
{
public:
	enum PrimitiveType {
		TRIANGLES = 0,
		LINES,
		POINTS
	};

	enum Type {
		NORMAL,
		BATCHING
	};

protected:
	/// The display array primitive type.
	PrimitiveType m_type;
	/// Modification flag.
	unsigned short m_modifiedFlag;
	/// The vertex format used.
	RAS_VertexFormat m_format;

	/// The vertex infos unused for rendering, e.g original or soft body index, flag.
	std::vector<RAS_VertexInfo> m_vertexInfos;
	/// Cached vertex pointers. This list is constructed with the function UpdateCache.
	std::vector<RAS_IVertex *> m_vertexPtrs;
	/// The indices used for rendering.
	std::vector<unsigned int> m_primitiveIndices;
	/// The indices of the original triangle independently of the primitive type.
	std::vector<unsigned int> m_triangleIndices;

	/// Maximum original vertex index.
	unsigned int m_maxOrigIndex;

	/** Polygon centers cache used to sort polygons depending on depth.
	 * This list is stored here because we store per array not per entire mesh.
	 */
	std::vector<MT_Vector3> m_polygonCenters;

public:
	RAS_IDisplayArray(PrimitiveType type, const RAS_VertexFormat& format);
	virtual ~RAS_IDisplayArray();

	virtual RAS_IDisplayArray *GetReplica() = 0;

	/** Construct the display array corresponding of the vertex of the given format.
	 * \param type The type of primitives, one of the enumeration PrimitiveType.
	 * \param format The format of vertex to use.
	 */
	static RAS_IDisplayArray *ConstructArray(PrimitiveType type, const RAS_VertexFormat &format);

	virtual unsigned int GetVertexMemorySize() const = 0;
	virtual void *GetVertexXYZOffset() const = 0;
	virtual void *GetVertexNormalOffset() const = 0;
	virtual void *GetVertexTangentOffset() const = 0;
	virtual void *GetVertexUVOffset() const = 0;
	virtual void *GetVertexColorOffset() const = 0;
	virtual unsigned short GetVertexUvSize() const = 0;
	virtual unsigned short GetVertexColorSize() const = 0;

	/** Return a vertex pointer without using the cache. Used to get
	 * a vertex pointer during contruction.
	 */
	virtual RAS_IVertex *GetVertexNoCache(const unsigned int index) const = 0;

	inline RAS_IVertex *GetVertex(const unsigned int index) const
	{
		return m_vertexPtrs[index];
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

	virtual unsigned int AddVertex(RAS_IVertex *vert) = 0;

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
		m_maxOrigIndex = std::max(m_maxOrigIndex, info.getOrigIndex());
		m_vertexInfos.push_back(info);
	}

	virtual void Clear() = 0;

	virtual const RAS_IVertex *GetVertexPointer() const = 0;

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

	void SortPolygons(const MT_Transform &transform, unsigned int *indexmap);
	void InvalidatePolygonCenters();

	virtual RAS_IVertex *CreateVertex(
				const MT_Vector3& xyz,
				const MT_Vector2 * const uvs,
				const MT_Vector4& tangent,
				const unsigned int *rgba,
				const MT_Vector3& normal) = 0;

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

	/// Modification categories.
	enum {
		NONE_MODIFIED = 0,
		POSITION_MODIFIED = 1 << 0, // Vertex position modified.
		NORMAL_MODIFIED = 1 << 1, // Vertex normal modified.
		UVS_MODIFIED = 1 << 2, // Vertex UVs modified.
		COLORS_MODIFIED = 1 << 3, // Vertex colors modified.
		TANGENT_MODIFIED = 1 << 4, // Vertex tangent modified.
		SIZE_MODIFIED = 1 << 5, // Vertex and index array changed of size.
		AABB_MODIFIED = POSITION_MODIFIED,
		MESH_MODIFIED = POSITION_MODIFIED | NORMAL_MODIFIED | UVS_MODIFIED |
						COLORS_MODIFIED | TANGENT_MODIFIED
	};

	/// Return display array modified flag.
	unsigned short GetModifiedFlag() const;
	/** Mix display array modified flag with a new flag.
	 * \param flag The flag to mix.
	 */
	void AppendModifiedFlag(unsigned short flag);
	/// Set the display array modified flag.
	void SetModifiedFlag(unsigned short flag);

	/// Return the vertex format used.
	const RAS_VertexFormat& GetFormat() const;

	/// Return the type of the display array.
	virtual Type GetType() const;
};

typedef std::vector<RAS_IDisplayArray *> RAS_IDisplayArrayList;

#endif  // __RAS_IDISPLAY_ARRAY_H__
