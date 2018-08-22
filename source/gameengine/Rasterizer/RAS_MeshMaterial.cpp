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

#include "RAS_MeshMaterial.h"
#include "RAS_MaterialBucket.h"
#include "RAS_DisplayArray.h"
#include "RAS_DisplayArrayBucket.h"

RAS_MeshMaterial::RAS_MeshMaterial(RAS_Mesh *mesh, RAS_MaterialBucket *bucket, unsigned int index, const RAS_DisplayArray::Format& format)
	:m_bucket(bucket),
	m_index(index)
{
	const bool isWire = bucket->GetMaterial()->IsWire();
	RAS_DisplayArray::PrimitiveType type = isWire ? RAS_DisplayArray::LINES : RAS_DisplayArray::TRIANGLES;
	m_displayArray = new RAS_DisplayArray(type, format);

	m_displayArrayBucket = new RAS_DisplayArrayBucket(bucket, m_displayArray, mesh, this, nullptr);
}

RAS_MeshMaterial::RAS_MeshMaterial(const RAS_MeshMaterial& other, RAS_Mesh *mesh)
	:m_bucket(other.m_bucket),
	m_index(other.m_index)
{
	m_displayArray = new RAS_DisplayArray(*other.m_displayArray);
	m_displayArrayBucket = new RAS_DisplayArrayBucket(m_bucket, m_displayArray, mesh, this, nullptr);
}

RAS_MeshMaterial::~RAS_MeshMaterial()
{
	delete m_displayArrayBucket;
	delete m_displayArray;
}

unsigned int RAS_MeshMaterial::GetIndex() const
{
	return m_index;
}

RAS_MaterialBucket *RAS_MeshMaterial::GetBucket() const
{
	return m_bucket;
}

RAS_DisplayArray *RAS_MeshMaterial::GetDisplayArray() const
{
	return m_displayArray;
}

RAS_DisplayArrayBucket *RAS_MeshMaterial::GetDisplayArrayBucket() const
{
	return m_displayArrayBucket;
}

void RAS_MeshMaterial::ReplaceMaterial(RAS_MaterialBucket *bucket)
{
	// Avoid replacing the by the same material bucket.
	if (m_bucket != bucket) {
		m_bucket->MoveDisplayArrayBucket(this, bucket);
		m_bucket = bucket;
	}
}
