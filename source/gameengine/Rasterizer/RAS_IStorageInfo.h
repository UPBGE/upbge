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
 * Contributor(s): Tristan Porteries.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

#ifndef __RAS_ISTORAGE_INFO_H__
#define __RAS_ISTORAGE_INFO_H__

#include "RAS_Rasterizer.h"

/** This class is used to store special storage infos for an array
 * like VBO/IBO ID for VBO storage.
 */
class RAS_IStorageInfo
{
public:
	enum DataType {
		VERTEX_DATA,
		INDEX_DATA
	};

	RAS_IStorageInfo()
	{
	}
	virtual ~RAS_IStorageInfo()
	{
	}

	virtual void UpdateVertexData() = 0;
	virtual void UpdateSize() = 0;
	virtual unsigned int *GetIndexMap() = 0;
	virtual void FlushIndexMap() = 0;
	
};

#endif  // __RAS_ISTORAGE_INFO_H__
