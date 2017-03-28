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
 * The Original Code is: all of this file.
 *
 * Contributor(s): Porteries Tristan.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file RAS_InstancingBuffer.h
 *  \ingroup bgerast
 */

#ifndef __RAS_INSTANCING_BUFFER_H__
#define __RAS_INSTANCING_BUFFER_H__

#include "RAS_MeshSlot.h"

class RAS_Rasterizer;

struct GPUBuffer;

class RAS_InstancingBuffer
{
	/// The OpenGL VBO.
	GPUBuffer *m_vbo;
	/// The matrix offset in the VBO.
	void *m_matrixOffset;
	/// The position offset in the VBO.
	void *m_positionOffset;
	/// The color offset in the VBO.
	void *m_colorOffset;
	/// The instance structure stride in the VBO.
	unsigned int m_stride;

	/// Structure used to store object info for geometry instancing objects render.
	struct InstancingObject
	{
		float matrix[9];
		float position[3];
		unsigned char color[4];
	};

public:
	RAS_InstancingBuffer();
	virtual ~RAS_InstancingBuffer();

	/// Realloc the VBO.
	void Realloc(unsigned int size);
	/// Bind the VBO before work on it.
	void Bind();
	/// Unbind the VBO after work on it.
	void Unbind();

	/** Allocate the VBO and fill it with a InstancingObject per mesh slots.
	 * \param rasty Rasterizer used to compute the mesh slot matrix, useful for billboard material.
	 * \param drawingmode The material drawing mode used to detect a billboard/halo/shadow material.
	 * \param meshSlots The list of all non-culled and visible mesh slots (= game object).
	 */
	void Update(RAS_Rasterizer *rasty, int drawingmode, RAS_MeshSlotList &meshSlots);

	inline void *GetMatrixOffset() const
	{
		return m_matrixOffset;
	}
	inline void *GetPositionOffset() const
	{
		return m_positionOffset;
	}
	inline void *GetColorOffset() const
	{
		return m_colorOffset;
	}
	inline unsigned int GetStride() const
	{
		return m_stride;
	}
};

#endif // __RAS_INSTANCING_BUFFER_H__

