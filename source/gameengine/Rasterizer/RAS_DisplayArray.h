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

#include "RAS_TexVert.h"

#include <vector>

class RAS_IDisplayArray
{
public:
	enum PrimitiveType {
		TRIANGLES,
		LINES,
	};

protected:
	PrimitiveType m_type;

public:
	virtual ~RAS_IDisplayArray();

	virtual RAS_IDisplayArray *GetReplica() = 0;

	static RAS_IDisplayArray *ConstructArray(PrimitiveType type, const RAS_TexVertFormat &format);

	virtual unsigned int GetVertexMemorySize() const = 0;
	virtual void *GetVertexXYZOffset() const = 0;
	virtual void *GetVertexNormalOffset() const = 0;
	virtual void *GetVertexTangentOffset() const = 0;
	virtual void *GetVertexUVOffset() const = 0;
	virtual void *GetVertexColorOffset() const = 0;

	virtual RAS_ITexVert *GetVertex(unsigned int index) const = 0;
	virtual unsigned int GetIndex(unsigned int index) const = 0;

	virtual const RAS_ITexVert *GetVertexPointer() const = 0;
	virtual const unsigned int *GetIndexPointer() const = 0;

	virtual void AddVertex(RAS_ITexVert *vert) = 0;
	virtual void AddIndex(unsigned int index) = 0;

	virtual unsigned int GetVertexCount() const = 0;
	virtual unsigned int GetIndexCount() const = 0;

	virtual RAS_ITexVert *CreateVertex(
				const MT_Vector3& xyz,
				const MT_Vector2 uvs[RAS_ITexVert::MAX_UNIT],
				const MT_Vector4& tangent,
				const unsigned int rgba,
				const MT_Vector3& normal,
				const bool flat,
				const unsigned int origindex) = 0;

	void UpdateFrom(RAS_IDisplayArray *other, int flag);
	int GetOpenGLPrimitiveType() const;
};

/// An array with data used for OpenGL drawing
template <class Vertex>
class RAS_DisplayArray : public RAS_IDisplayArray
{
private:
	std::vector<Vertex> m_vertex;
	std::vector<unsigned int> m_index;

public:
	friend Vertex;

	RAS_DisplayArray(PrimitiveType type)
	{
		m_type = type;
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

	virtual RAS_ITexVert *GetVertex(unsigned int index) const
	{
		return (RAS_ITexVert *)&m_vertex[index];
	}

	virtual unsigned int GetIndex(unsigned int index) const
	{
		return m_index[index];
	}

	virtual const RAS_ITexVert *GetVertexPointer() const
	{
		return (RAS_ITexVert *)m_vertex.data();
	}

	virtual const unsigned int *GetIndexPointer() const
	{
		return (unsigned int *)m_index.data();
	}

	virtual void AddVertex(RAS_ITexVert *vert)
	{
		m_vertex.push_back(*((Vertex *)vert));
	}

	virtual void AddIndex(unsigned int index)
	{
		m_index.push_back(index);
	}

	virtual unsigned int GetVertexCount() const
	{
		return m_vertex.size();
	}

	virtual unsigned int GetIndexCount() const
	{
		return m_index.size();
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

#endif  // __RAS_DISPLAY_ARRAY_H__
