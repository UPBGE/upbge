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

#include "RAS_TexVert.h"
#include <vector>

class RAS_IDisplayArray
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

protected:
	/// The display array primitive type.
	PrimitiveType m_type;
	/// Modification flag.
	unsigned short m_modifiedFlag;
	/// The vertex format used.
	RAS_TexVertFormat m_format;

	/// The vertex infos unused for rendering, e.g original or soft body index, flag.
	std::vector<RAS_TexVertInfo> m_vertexInfos;
	/// Cached vertex pointers. This list is constructed with the function UpdateCache.
	std::vector<RAS_ITexVert *> m_vertexPtrs;
	/// The indices used for rendering.
	std::vector<unsigned int> m_indices;

public:
	RAS_IDisplayArray(PrimitiveType type, const RAS_TexVertFormat& format);
	virtual ~RAS_IDisplayArray();

	virtual RAS_IDisplayArray *GetReplica() = 0;

	/** Construct the display array corresponding of the vertex of the given format.
	 * \param type The type of primitives, one of the enumeration PrimitiveType.
	 * \param format The format of vertex to use.
	 */
	static RAS_IDisplayArray *ConstructArray(PrimitiveType type, const RAS_TexVertFormat &format);

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
	virtual RAS_ITexVert *GetVertexNoCache(const unsigned int index) const = 0;

	inline RAS_ITexVert *GetVertex(const unsigned int index) const
	{
		return m_vertexPtrs[index];
	}

	inline unsigned int GetIndex(const unsigned int index) const
	{
		return m_indices[index];
	}

	inline void SetIndex(const unsigned int index, const unsigned int value)
	{
		m_indices[index] = value;
	}

	inline const RAS_TexVertInfo& GetVertexInfo(const unsigned int index) const
	{
		return m_vertexInfos[index];
	}

	inline RAS_TexVertInfo& GetVertexInfo(const unsigned int index)
	{
		return m_vertexInfos[index];
	}

	virtual void AddVertex(RAS_ITexVert *vert) = 0;

	inline void AddIndex(const unsigned int index)
	{
		m_indices.push_back(index);
	}

	inline void AddVertexInfo(const RAS_TexVertInfo& info)
	{
		m_vertexInfos.push_back(info);
	}

	virtual const RAS_ITexVert *GetVertexPointer() const = 0;

	inline const unsigned int *GetIndexPointer() const
	{
		return (unsigned int *)m_indices.data();
	}

	virtual unsigned int GetVertexCount() const = 0;

	inline unsigned int GetIndexCount() const
	{
		return m_indices.size();
	}

	virtual RAS_ITexVert *CreateVertex(
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
	const RAS_TexVertFormat& GetFormat() const;

	/// Return the type of the display array.
	virtual Type GetType() const;
};

typedef std::vector<RAS_IDisplayArray *> RAS_IDisplayArrayList;

#endif  // __RAS_IDISPLAY_ARRAY_H__
