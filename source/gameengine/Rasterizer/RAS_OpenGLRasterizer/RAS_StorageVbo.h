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

#include <vector>

struct RAS_DisplayArrayLayout;
class RAS_DisplayArray;

class RAS_StorageVbo
{
private:
	RAS_DisplayArray *m_array;
	unsigned int m_indices;
	GLenum m_mode;
	GLuint m_ibo;
	GLuint m_vbo;

	void CopyVertexData(const RAS_DisplayArrayLayout& layout, unsigned int modifiedFlag);

public:
	RAS_StorageVbo(RAS_DisplayArray *array);
	~RAS_StorageVbo();

	void BindVertexBuffer();
	void UnbindVertexBuffer();

	void BindIndexBuffer();
	void UnbindIndexBuffer();

	void UpdateVertexData(unsigned int modifiedFlag);
	void UpdateSize();
	unsigned int *GetIndexMap();
	void FlushIndexMap();

	void IndexPrimitives();
	void IndexPrimitivesInstancing(unsigned int numinstance);
	void IndexPrimitivesBatching(const std::vector<intptr_t>& indices, const std::vector<int>& counts);
};


#endif  // __RAS_STORAGE_VBO_H__
