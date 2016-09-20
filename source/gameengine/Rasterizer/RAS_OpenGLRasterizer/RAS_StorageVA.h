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

#ifndef __KX_VERTEXARRAYSTORAGE
#define __KX_VERTEXARRAYSTORAGE

#include "RAS_IStorage.h"
#include "RAS_OpenGLRasterizer.h"

class RAS_DisplayList : public RAS_IStorageInfo
{
public:
	enum LIST_TYPE {
		BIND_LIST = 0,
		UNBIND_LIST = 1,
		DRAW_LIST = 2,
		NUM_LIST = 3
	};

private:
	int m_list[RAS_IRasterizer::RAS_DRAW_MAX][NUM_LIST];

	void RemoveAllList(RAS_IRasterizer::DrawType drawmode);

public:
	RAS_DisplayList();
	virtual ~RAS_DisplayList();

	virtual void SetMeshModified(RAS_IRasterizer::DrawType drawmode, bool modified);

	/** Return true if the list already exists and was called.
	 * False mean : we need call all opengl functions and finish
	 * with an End call.
	 */
	bool Draw(RAS_IRasterizer::DrawType drawmode, LIST_TYPE type);
	/// Finish the display list, must be called after Draw when it return false.
	void End(RAS_IRasterizer::DrawType drawmode, LIST_TYPE type);
};

class RAS_StorageVA : public RAS_IStorage
{
public:
	RAS_StorageVA(RAS_OpenGLRasterizer::StorageAttribs *storageAttribs);
	virtual ~RAS_StorageVA();

	virtual bool Init();
	virtual void Exit();

	virtual void BindPrimitives(RAS_DisplayArrayBucket *arrayBucket);
	virtual void UnbindPrimitives(RAS_DisplayArrayBucket *arrayBucket);
	virtual void IndexPrimitives(RAS_MeshSlot *ms);
	virtual void IndexPrimitivesInstancing(RAS_DisplayArrayBucket *arrayBucket);

	virtual void SetDrawingMode(RAS_IRasterizer::DrawType drawingmode)
	{
		m_drawingmode = drawingmode;
	};

protected:
	RAS_IRasterizer::DrawType m_drawingmode;

	RAS_OpenGLRasterizer::StorageAttribs *m_storageAttribs;

	RAS_DisplayList *GetDisplayList(RAS_DisplayArrayBucket *arrayBucket);

	virtual void EnableTextures(bool enable);
	virtual void TexCoordPtr(const RAS_ITexVert *tv, const unsigned int stride);


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

#endif //__KX_VERTEXARRAYSTORAGE
