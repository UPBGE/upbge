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

protected:
	PrimitiveType m_type;

	std::vector<RAS_TexVertInfo> m_vertexInfo;
	std::vector<RAS_ITexVert *> m_vertexPtr;
	std::vector<unsigned int> m_index;

public:
	RAS_IDisplayArray(PrimitiveType type);
	virtual ~RAS_IDisplayArray();

	virtual RAS_IDisplayArray *GetReplica() = 0;

	static RAS_IDisplayArray *ConstructArray(PrimitiveType type, const RAS_TexVertFormat &format);

	virtual unsigned int GetVertexMemorySize() const = 0;
	virtual void *GetVertexXYZOffset() const = 0;
	virtual void *GetVertexNormalOffset() const = 0;
	virtual void *GetVertexTangentOffset() const = 0;
	virtual void *GetVertexUVOffset() const = 0;
	virtual void *GetVertexColorOffset() const = 0;

	virtual RAS_ITexVert *GetVertexNoCache(const unsigned int index) const = 0;

	inline RAS_ITexVert *GetVertex(const unsigned int index) const
	{
		return m_vertexPtr[index];
	}

	inline unsigned int GetIndex(unsigned int index) const
	{
		return m_index[index];
	}

	inline const RAS_TexVertInfo& GetVertexInfo(const unsigned int index) const
	{
		return m_vertexInfo[index];
	}

	inline RAS_TexVertInfo& GetVertexInfo(const unsigned int index)
	{
		return m_vertexInfo[index];
	}

	virtual void AddVertex(RAS_ITexVert *vert) = 0;

	inline void AddIndex(const unsigned int index)
	{
		m_index.push_back(index);
	}

	inline void AddVertexInfo(const RAS_TexVertInfo& info)
	{
		m_vertexInfo.push_back(info);
	}

	virtual const RAS_ITexVert *GetVertexPointer() const = 0;

	inline const unsigned int *GetIndexPointer() const
	{
		return (unsigned int *)m_index.data();
	}

	virtual unsigned int GetVertexCount() const = 0;

	inline unsigned int GetIndexCount() const
	{
		return m_index.size();
	}

	virtual RAS_ITexVert *CreateVertex(
				const MT_Vector3& xyz,
				const MT_Vector2 * const uvs,
				const MT_Vector4& tangent,
				const unsigned int rgba,
				const MT_Vector3& normal) = 0;

	void UpdateFrom(RAS_IDisplayArray *other, int flag);

	virtual void UpdateCache() = 0;

	int GetOpenGLPrimitiveType() const;
};

#endif  // __RAS_IDISPLAY_ARRAY_H__
