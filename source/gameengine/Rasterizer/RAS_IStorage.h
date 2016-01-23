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

#ifndef __KX_STORAGE
#define __KX_STORAGE

#include "RAS_IRasterizer.h"

#ifdef WITH_CXX_GUARDEDALLOC
  #include "MEM_guardedalloc.h"
#endif

class RAS_MeshSlot;
class RAS_DisplayArrayBucket;

/** This class is used to store special storage infos for an array
 * like VBO/IBO ID for VBO storage or DL for VA storage.
 * Currently it only exists for the virtual destructor.
 */
class RAS_IStorageInfo
{
public:
	RAS_IStorageInfo()
	{
	}
	virtual ~RAS_IStorageInfo()
	{
	}

	virtual void SetMeshModified(RAS_IRasterizer::DrawType drawType, bool modified)
	{
	}
};

class RAS_IStorage
{

public:
	virtual ~RAS_IStorage()
	{
	};

	virtual bool Init() = 0;
	virtual void Exit() = 0;

	virtual void BindPrimitives(RAS_DisplayArrayBucket *arrayBucket) = 0;
	virtual void UnbindPrimitives(RAS_DisplayArrayBucket *arrayBucket) = 0;

	virtual void IndexPrimitives(RAS_MeshSlot *ms) = 0;
	virtual void IndexPrimitivesInstancing(RAS_DisplayArrayBucket *arrayBucket)
	{
	}

	virtual void SetDrawingMode(RAS_IRasterizer::DrawType drawingmode) = 0;

#ifdef WITH_CXX_GUARDEDALLOC
	MEM_CXX_CLASS_ALLOC_FUNCS("GE:RAS_IStorage")
#endif
};

#endif //__KX_STORAGE
