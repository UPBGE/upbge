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

#include "GPU_glew.h"

#include "RAS_IStorageInfo.h"
#include "RAS_Rasterizer.h"

class RAS_IDisplayArray;

class VBO : public RAS_IStorageInfo
{
public:
	VBO(RAS_IDisplayArray *array, bool instancing);
	virtual ~VBO();

	virtual void UpdateVertexData();
	virtual void UpdateSize();
	virtual unsigned int *GetIndexMap();
	virtual void FlushIndexMap();

	void Bind(RAS_Rasterizer::StorageAttribs *storageAttribs, RAS_Rasterizer::DrawType drawingmode);
	void Unbind(RAS_Rasterizer::StorageAttribs *storageAttribs, RAS_Rasterizer::DrawType drawingmode);
	void Draw();
	void DrawInstancing(unsigned int numinstance);
	void DrawBatching(const std::vector<void *>& indices, const std::vector<int>& counts);

private:
	RAS_IDisplayArray *m_data;
	GLuint m_size;
	GLuint m_stride;
	GLuint m_indices;
	GLenum m_mode;
	GLuint m_ibo;
	GLuint m_vbo_id;
	/// The VAOs id allocated by OpenGL.
	GLuint m_vaos[RAS_Rasterizer::RAS_DRAW_MAX];
	/// Set to true when the VBO can use VAO (the GPU support VAO and there's no geometry instancing).
	bool m_useVao;
	/// Set to true when the VAO was already filled in a VBO::Bind() call.
	bool m_vaoInitialized[RAS_Rasterizer::RAS_DRAW_MAX];

	void *m_vertex_offset;
	void *m_normal_offset;
	void *m_color_offset;
	void *m_tangent_offset;
	void *m_uv_offset;
};

class RAS_StorageVBO
{
public:
	RAS_StorageVBO(RAS_Rasterizer::StorageAttribs *storageAttribs);
	~RAS_StorageVBO();

	void BindPrimitives(RAS_Rasterizer::DrawType drawingMode, VBO *vbo);
	void UnbindPrimitives(RAS_Rasterizer::DrawType drawingMode, VBO *vbo);
	void IndexPrimitives(VBO *vbo);
	void IndexPrimitivesInstancing(VBO *vbo, unsigned int numslots);
	void IndexPrimitivesBatching(VBO *vbo, const std::vector<void *>& indices, const std::vector<int>& counts);

	RAS_IStorageInfo *GetStorageInfo(RAS_IDisplayArray *array, bool instancing);

protected:
	RAS_Rasterizer::StorageAttribs *m_storageAttribs;
};

#endif  // __RAS_STORAGE_VBO_H__
