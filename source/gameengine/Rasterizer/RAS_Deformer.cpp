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
#include "RAS_Mesh.h"

RAS_Deformer::RAS_Deformer(RAS_Mesh *mesh)
	:m_mesh(mesh),
	m_bDynamic(false),
	m_boundingBox(nullptr)
{
}

RAS_Deformer::~RAS_Deformer()
{
	for (const DisplayArraySlot& slot : m_slots) {
		delete slot.m_displayArray;
		delete slot.m_displayArrayBucket;
	}
}

void RAS_Deformer::Initialize()
{
	InitializeDisplayArrays();
}

bool RAS_Deformer::UseShaderSkinning() const
{
	return false;
}

RAS_Mesh *RAS_Deformer::GetMesh() const
{
	return m_mesh;
}

RAS_DisplayArray *RAS_Deformer::GetDisplayArray(unsigned short index) const
{
	return m_slots[index].m_displayArray;
}

RAS_DisplayArrayBucket *RAS_Deformer::GetDisplayArrayBucket(unsigned short index) const
{
	return m_slots[index].m_displayArrayBucket;
}

RAS_Deformer::SkinShaderData RAS_Deformer::GetSkinningShaderData(RAS_DisplayArray *array) const
{
	return {nullptr, nullptr, 0};
}

void RAS_Deformer::InitializeDisplayArrays()
{
	for (RAS_MeshMaterial *meshmat : m_mesh->GetMeshMaterialList()) {
		RAS_DisplayArray *origArray = meshmat->GetDisplayArray();
		/* Duplicate the display array bucket and the display array if needed to store
		 * the mesh slot on a unique list (= display array bucket) and use an unique vertex
		 * array (=display array). */
		RAS_DisplayArray *array = new RAS_DisplayArray(*origArray);

		RAS_DisplayArrayBucket *arrayBucket = new RAS_DisplayArrayBucket(meshmat->GetBucket(), array, m_mesh, meshmat, this);

		m_slots.push_back(
			{array, origArray, meshmat, arrayBucket,
			 {RAS_DisplayArray::TANGENT_MODIFIED | RAS_DisplayArray::UVS_MODIFIED | RAS_DisplayArray::COLORS_MODIFIED,
			  RAS_DisplayArray::NONE_MODIFIED}});
	}

	for (DisplayArraySlot& slot : m_slots) {
		slot.m_origDisplayArray->AddUpdateClient(&slot.m_arrayUpdateClient);
	}
}
