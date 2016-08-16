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
	RAS_IDisplayArray();
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
				const MT_Vector2 * const uvs,
				const MT_Vector4& tangent,
				const unsigned int rgba,
				const MT_Vector3& normal,
				const bool flat,
				const unsigned int origindex) = 0;

	void UpdateFrom(RAS_IDisplayArray *other, int flag);
	int GetOpenGLPrimitiveType() const;
};

#endif  // __RAS_IDISPLAY_ARRAY_H__
