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

/** \file RAS_DisplayArray.h
 *  \ingroup bgerast
 */

#ifndef __RAS_DISPLAY_ARRAY_H__
#define __RAS_DISPLAY_ARRAY_H__

#include "RAS_IDisplayArray.h"

/// An array with data used for OpenGL drawing
template <class Vertex>
class RAS_DisplayArray : public RAS_IDisplayArray
{
private:
	std::vector<Vertex> m_vertex;

public:
	friend Vertex;

	RAS_DisplayArray(PrimitiveType type)
		:RAS_IDisplayArray(type)
	{
	}

	virtual ~RAS_DisplayArray()
	{
	}

	virtual RAS_IDisplayArray *GetReplica()
	{
		return new RAS_DisplayArray<Vertex>(*this);
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

	virtual RAS_ITexVert *GetVertexNoCache(unsigned int index) const
	{
		return (RAS_ITexVert *)&m_vertex[index];
	}

	virtual const RAS_ITexVert *GetVertexPointer() const
	{
		return (RAS_ITexVert *)m_vertex.data();
	}

	virtual void AddVertex(RAS_ITexVert *vert)
	{
		m_vertex.push_back(*((Vertex *)vert));
	}

	virtual unsigned int GetVertexCount() const
	{
		return m_vertex.size();
	}

	virtual RAS_ITexVert *CreateVertex(
				const MT_Vector3& xyz,
				const MT_Vector2 * const uvs,
				const MT_Vector4& tangent,
				const unsigned int rgba,
				const MT_Vector3& normal)
	{
		return new Vertex(xyz, uvs, tangent, rgba, normal);
	}

	virtual void UpdateCache()
	{
		const unsigned int size = GetVertexCount();
		m_vertexPtr.resize(size);
		for (unsigned int i = 0; i < size; ++i) {
			m_vertexPtr[i] = (RAS_ITexVert *)&m_vertex[i];
		}
	}
};

#endif  // __RAS_DISPLAY_ARRAY_H__
