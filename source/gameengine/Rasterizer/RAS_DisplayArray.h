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

#ifndef __RAS_DISPLAY_ARRAY_H__
#define __RAS_DISPLAY_ARRAY_H__

#include "RAS_IDisplayArray.h"

template <class Vertex>
class RAS_BatchDisplayArray;

/// An array with data used for OpenGL drawing
template <class Vertex>
class RAS_DisplayArray : public virtual RAS_IDisplayArray
{
friend class RAS_BatchDisplayArray<Vertex>;

protected:
	std::vector<Vertex> m_vertexes;

public:
	RAS_DisplayArray(PrimitiveType type, const RAS_TexVertFormat& format)
		:RAS_IDisplayArray(type, format)
	{
	}

	virtual ~RAS_DisplayArray()
	{
	}

	virtual RAS_IDisplayArray *GetReplica()
	{
		RAS_DisplayArray<Vertex> *replica = new RAS_DisplayArray<Vertex>(*this);
		replica->UpdateCache();

		return replica;
	}

	virtual unsigned int GetVertexMemorySize() const
	{
		return sizeof(Vertex);
	}

	virtual void *GetVertexXYZOffset() const
	{
		return (void *)offsetof(Vertex, m_localxyz);
	}

	virtual void *GetVertexNormalOffset() const
	{
		return (void *)offsetof(Vertex, m_normal);
	}

	virtual void *GetVertexTangentOffset() const
	{
		return (void *)offsetof(Vertex, m_tangent);
	}

	virtual void *GetVertexUVOffset() const
	{
		return (void *)offsetof(Vertex, m_uvs);
	}

	virtual void *GetVertexColorOffset() const
	{
		return (void *)offsetof(Vertex, m_rgba);
	}

	virtual unsigned short GetVertexUvSize() const
	{
		return Vertex::UvSize;
	}

	virtual unsigned short GetVertexColorSize() const
	{
		return Vertex::ColorSize;
	}

	virtual RAS_ITexVert *GetVertexNoCache(const unsigned int index) const
	{
		return (RAS_ITexVert *)&m_vertexes[index];
	}

	virtual const RAS_ITexVert *GetVertexPointer() const
	{
		return (RAS_ITexVert *)m_vertexes.data();
	}

	virtual void AddVertex(RAS_ITexVert *vert)
	{
		m_vertexes.push_back(*((Vertex *)vert));
	}

	virtual unsigned int GetVertexCount() const
	{
		return m_vertexes.size();
	}

	virtual RAS_ITexVert *CreateVertex(
				const MT_Vector3& xyz,
				const MT_Vector2 * const uvs,
				const MT_Vector4& tangent,
				const unsigned int *rgba,
				const MT_Vector3& normal)
	{
		return new Vertex(xyz, uvs, tangent, rgba, normal);
	}

	virtual void UpdateCache()
	{
		const unsigned int size = GetVertexCount();
		m_vertexPtrs.resize(size);
		for (unsigned int i = 0; i < size; ++i) {
			m_vertexPtrs[i] = (RAS_ITexVert *)&m_vertexes[i];
		}
	}
};

#endif  // __RAS_DISPLAY_ARRAY_H__
