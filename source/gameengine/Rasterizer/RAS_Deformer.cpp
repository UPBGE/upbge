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

#include "RAS_Deformer.h"

RAS_Deformer::~RAS_Deformer()
{
	for (RAS_DisplayArrayBucket *arrayBucket : m_displayArrayBucketList) {
		delete arrayBucket;
	}

	for (RAS_IDisplayArray *array : m_displayArrayList) {
		delete array;
	}
}

RAS_MeshObject *RAS_Deformer::GetMesh() const
{
	return m_mesh;
}

void RAS_Deformer::AddDisplayArray(RAS_IDisplayArray *array, RAS_DisplayArrayBucket *arrayBucket)
{
	m_displayArrayList.push_back(array);
	m_displayArrayBucketList.push_back(arrayBucket);
}

RAS_IDisplayArray *RAS_Deformer::GetDisplayArray(unsigned short index) const
{
	return m_displayArrayList[index];
}
