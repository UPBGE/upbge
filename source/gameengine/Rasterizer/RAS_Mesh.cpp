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

/** \file gameengine/Rasterizer/RAS_Mesh.cpp
 *  \ingroup bgerast
 */

#include "DNA_mesh_types.h"

#include "RAS_Mesh.h"
#include "RAS_MeshUser.h"
#include "RAS_BoundingBoxManager.h"
#include "RAS_IMaterial.h"
#include "RAS_BucketManager.h"
#include "RAS_DisplayArray.h"
#include "RAS_Deformer.h"

#include "SCA_IScene.h"

#include "CM_Message.h"

RAS_Mesh::RAS_Mesh(Mesh *mesh, const LayersInfo& layersInfo)
	:m_name(mesh->id.name + 2),
	m_layersInfo(layersInfo),
	m_boundingBox(nullptr),
	m_mesh(mesh)
{
}

RAS_Mesh::RAS_Mesh(const RAS_Mesh& other)
	:m_name(other.m_name),
	m_layersInfo(other.m_layersInfo),
	m_boundingBox(nullptr),
	m_mesh(other.m_mesh)
{
	for (RAS_MeshMaterial *meshmat : other.m_materials) {
		RAS_MeshMaterial *newmeshmat = new RAS_MeshMaterial(*meshmat, this);
		m_materials.push_back(newmeshmat);
	}
}

RAS_Mesh::~RAS_Mesh()
{
	for (RAS_MeshMaterial *meshmat : m_materials) {
		delete meshmat;
	}
	m_materials.clear();
}

const RAS_MeshMaterialList& RAS_Mesh::GetMeshMaterialList() const
{
	return m_materials;
}

unsigned short RAS_Mesh::GetNumMaterials() const
{
	return m_materials.size();
}

std::string RAS_Mesh::GetMaterialName(unsigned int matid) const
{
	RAS_MeshMaterial *mmat = GetMeshMaterial(matid);

	if (mmat) {
		return mmat->GetBucket()->GetMaterial()->GetName();
	}

	return "";
}

RAS_MeshMaterial *RAS_Mesh::GetMeshMaterial(unsigned int matid) const
{
	if (m_materials.size() > matid) {
		return m_materials[matid];
	}

	return nullptr;
}

RAS_MeshMaterial *RAS_Mesh::GetMeshMaterialBlenderIndex(unsigned int index) const
{
	for (RAS_MeshMaterial *meshmat : m_materials) {
		if (meshmat->GetIndex() == index) {
			return meshmat;
		}
	}

	return nullptr;
}

RAS_MeshMaterial *RAS_Mesh::FindMaterialName(const std::string& name) const
{
	for (RAS_MeshMaterial *meshmat : m_materials) {
		// Check without the MA prefix.
		if (name == std::string(meshmat->GetBucket()->GetMaterial()->GetName(), 2)) {
			return meshmat;
		}
	}

	return nullptr;
}

unsigned int RAS_Mesh::GetNumPolygons() const
{
	return m_numPolygons;
}

RAS_Mesh::PolygonInfo RAS_Mesh::GetPolygon(unsigned int index) const
{
	// Convert triangle index to triangle vertex index.
	index *= 3;

	for (const PolygonRangeInfo& range : m_polygonRanges) {
		if (index >= range.startIndex && index < range.endIndex) {

			// Convert to relative index.
			index -= range.startIndex;

			RAS_DisplayArray *array = range.array;
			PolygonInfo polyInfo{
				array,
				{array->GetTriangleIndex(index),
				 array->GetTriangleIndex(index + 1),
				 array->GetTriangleIndex(index + 2)},
				range.flags, range.matId
			};

			return polyInfo;
		}
	}

	BLI_assert(false);

	return PolygonInfo();
}

const std::string& RAS_Mesh::GetName() const
{
	return m_name;
}

std::string RAS_Mesh::GetTextureName(unsigned int matid) const
{
	RAS_MeshMaterial *mmat = GetMeshMaterial(matid);

	if (mmat) {
		return mmat->GetBucket()->GetMaterial()->GetTextureName();
	}

	return "";
}

RAS_MeshMaterial *RAS_Mesh::AddMaterial(RAS_MaterialBucket *bucket, unsigned int index, const RAS_DisplayArray::Format& format)
{
	RAS_MeshMaterial *meshmat = GetMeshMaterialBlenderIndex(index);

	// none found, create a new one
	if (!meshmat) {
		meshmat = new RAS_MeshMaterial(this, bucket, index, format);
		m_materials.push_back(meshmat);
	}

	return meshmat;
}

RAS_DisplayArray *RAS_Mesh::GetDisplayArray(unsigned int matid) const
{
	RAS_MeshMaterial *mmat = GetMeshMaterial(matid);

	if (!mmat) {
		return nullptr;
	}

	RAS_DisplayArray *array = mmat->GetDisplayArray();

	return array;
}

RAS_BoundingBox *RAS_Mesh::GetBoundingBox() const
{
	return m_boundingBox;
}

RAS_MeshUser *RAS_Mesh::AddMeshUser(void *clientobj, RAS_Deformer *deformer)
{
	RAS_BoundingBox *boundingBox = (deformer) ? deformer->GetBoundingBox() : m_boundingBox;
	RAS_MeshUser *meshUser = new RAS_MeshUser(clientobj, boundingBox, deformer);

	for (unsigned short i = 0, nummat = m_materials.size(); i < nummat; ++i) {
		RAS_DisplayArrayBucket *arrayBucket = (deformer) ?
		                                      deformer->GetDisplayArrayBucket(i) : m_materials[i]->GetDisplayArrayBucket();
		meshUser->NewMeshSlot(arrayBucket);
	}
	return meshUser;
}

void RAS_Mesh::EndConversion(RAS_BoundingBoxManager *boundingBoxManager)
{
	RAS_DisplayArrayList arrayList;

	// Construct a list of all the display arrays used by this mesh.
	for (RAS_MeshMaterial *meshmat : m_materials) {
		RAS_DisplayArray *array = meshmat->GetDisplayArray();

		const std::string materialname = meshmat->GetBucket()->GetMaterial()->GetName();
		if (array->GetVertexCount() == 0) {
			CM_Warning("mesh \"" << m_name << "\" has no vertices for material \"" << materialname
			                     << "\". It introduces performance decrease for empty render.");
		}
		else {
			// Generate bounding box only for non-empty display arrays.
			arrayList.push_back(array);
		}

		if (array->GetPrimitiveIndexCount() == 0) {
			CM_Warning("mesh \"" << m_name << "\" has no primitives for material \"" << materialname
			                     << "\". It introduces performance decrease for empty render.");
		}
	}

	if (arrayList.empty()) {
		// Use a dummy bounding box if there's no valid display arrays.
		m_boundingBox = boundingBoxManager->CreateBoundingBox();
	}
	else {
		// Construct the bounding box of this mesh without deformers.
		m_boundingBox = boundingBoxManager->CreateMeshBoundingBox(arrayList);
	}
	m_boundingBox->Update(true);

	// Construct polygon range info.
	unsigned int startIndex = 0;
	for (unsigned short i = 0, size = m_materials.size(); i < size; ++i) {
		RAS_MeshMaterial *meshmat = m_materials[i];
		RAS_DisplayArray *array = meshmat->GetDisplayArray();
		const unsigned indexCount = array->GetTriangleIndexCount();
		if (indexCount == 0) {
			continue;
		}

		// Compute absolute array end index.
		const unsigned int endIndex = startIndex + indexCount - 1;

		RAS_IMaterial *mat = meshmat->GetBucket()->GetMaterial();
		PolygonInfo::Flags flags =
			((mat->IsVisible()) ? PolygonInfo::VISIBLE : PolygonInfo::NONE |
			 (mat->IsCollider()) ? PolygonInfo::COLLIDER : PolygonInfo::NONE |
			 (mat->IsTwoSided()) ? PolygonInfo::TWOSIDE : PolygonInfo::NONE);

		m_polygonRanges.push_back({array, startIndex, endIndex, flags, i});

		// Update absolute start array index for the next array.
		startIndex += indexCount;
	}

	m_numPolygons = startIndex;
}

const RAS_Mesh::LayersInfo& RAS_Mesh::GetLayersInfo() const
{
	return m_layersInfo;
}
