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

#ifndef __RAS_STORAGE_VBO_H__
#define __RAS_STORAGE_VBO_H__

#include "glew-mx.h"

#include "RAS_IStorageInfo.h"

#include "RAS_OpenGLRasterizer.h"

class VBO : public RAS_IStorageInfo
{
public:
	VBO(RAS_DisplayArrayBucket *arrayBucket);
	virtual ~VBO();

	virtual void SetDataModified(RAS_IRasterizer::DrawType drawmode, DataType dataType);

	void Bind(RAS_OpenGLRasterizer::StorageAttribs *storageAttribs, RAS_IRasterizer::DrawType drawingmode);
	void Unbind(RAS_OpenGLRasterizer::StorageAttribs *storageAttribs, RAS_IRasterizer::DrawType drawingmode);
	void Draw();
	void DrawInstancing(unsigned int numinstance);
	void DrawBatching(const std::vector<void *>& indices, const std::vector<int>& counts);

	/// Updating the VBO using the display array vertices.
	void UpdateData();
	/// Updating the IBO using the display array indices.
	void UpdateIndices();

private:
	RAS_IDisplayArray *m_data;
	GLuint m_size;
	GLuint m_stride;
	GLuint m_indices;
	GLenum m_mode;
	GLuint m_ibo;
	GLuint m_vbo_id;
	/// The VAOs id allocated by OpenGL.
	GLuint m_vaos[RAS_IRasterizer::RAS_DRAW_MAX];
	/// Set to true when the VBO can use VAO (the GPU support VAO and there's no geometry instancing).
	bool m_useVao;
	/// Set to true when the VAO was already filled in a VBO::Bind() call.
	bool m_vaoInitialized[RAS_IRasterizer::RAS_DRAW_MAX];

	/// Set to true when the VBO/VAO is bound.
	bool m_bound;

	void *m_vertex_offset;
	void *m_normal_offset;
	void *m_color_offset;
	void *m_tangent_offset;
	void *m_uv_offset;

	/// Allocate the VBO and IBO using the display array size.
	void AllocData();
};

class RAS_StorageVBO
{
public:
	RAS_StorageVBO(RAS_OpenGLRasterizer::StorageAttribs *storageAttribs);
	~RAS_StorageVBO();

	void BindPrimitives(RAS_DisplayArrayBucket *arrayBucket);
	void UnbindPrimitives(RAS_DisplayArrayBucket *arrayBucket);
	void IndexPrimitives(RAS_MeshSlot *ms);
	void IndexPrimitivesInstancing(RAS_DisplayArrayBucket *arrayBucket);
	void IndexPrimitivesBatching(RAS_DisplayArrayBucket *arrayBucket, const std::vector<void *>& indices, const std::vector<int>& counts);

	void SetDrawingMode(RAS_IRasterizer::DrawType drawingmode)
	{
		m_drawingmode = drawingmode;
	};

protected:
	RAS_IRasterizer::DrawType m_drawingmode;

	RAS_OpenGLRasterizer::StorageAttribs *m_storageAttribs;

	VBO *GetVBO(RAS_DisplayArrayBucket *arrayBucket);

#ifdef WITH_CXX_GUARDEDALLOC
public:
	void *operator new(size_t num_bytes)
	{
		return MEM_mallocN(num_bytes, "GE:RAS_StorageVA");
	}
	void operator delete(void *mem)
	{
		MEM_freeN(mem);
	}
#endif
};

#endif  // __RAS_STORAGE_VBO_H__
