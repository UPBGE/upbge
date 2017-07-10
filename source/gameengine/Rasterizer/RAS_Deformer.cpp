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
#include "RAS_MeshObject.h"

RAS_Deformer::RAS_Deformer(RAS_MeshObject *mesh)
	:m_mesh(mesh),
	m_bDynamic(false),
	m_boundingBox(nullptr)
{
}

RAS_Deformer::~RAS_Deformer()
{
	for (RAS_DisplayArrayBucket *arrayBucket : m_displayArrayBucketList) {
		delete arrayBucket;
	}

	for (RAS_IDisplayArray *array : m_displayArrayList) {
		delete array;
	}
}

void RAS_Deformer::InitializeDisplayArrays()
{
	for (RAS_MeshMaterial *meshmat : m_mesh->GetMeshMaterialList()) {
		/* Duplicate the display array bucket and the display array if needed to store
		 * the mesh slot on a unique list (= display array bucket) and use an unique vertex
		 * array (=display array). */
		RAS_IDisplayArray *array = meshmat->GetDisplayArray()->GetReplica();

		RAS_DisplayArrayBucket *arrayBucket = new RAS_DisplayArrayBucket(meshmat->GetBucket(), array, m_mesh, meshmat, this);

		m_displayArrayList.push_back(array);
		m_displayArrayBucketList.push_back(arrayBucket);
	}
}

void RAS_Deformer::ProcessReplica()
{
	m_boundingBox = m_boundingBox->GetReplica();

	for (unsigned short i = 0, size = m_displayArrayList.size(); i < size; ++i) {
		RAS_IDisplayArray *array = m_displayArrayList[i] = m_displayArrayList[i]->GetReplica();
		RAS_MeshMaterial *meshmat = m_displayArrayBucketList[i]->GetMeshMaterial();
		m_displayArrayBucketList[i] = new RAS_DisplayArrayBucket(meshmat->GetBucket(), array, m_mesh, meshmat, this);
	}
}

RAS_MeshObject *RAS_Deformer::GetMesh() const
{
	return m_mesh;
}

RAS_IDisplayArray *RAS_Deformer::GetDisplayArray(unsigned short index) const
{
	return m_displayArrayList[index];
}

RAS_DisplayArrayBucket *RAS_Deformer::GetDisplayArrayBucket(unsigned short index) const
{
	return m_displayArrayBucketList[index];
}
