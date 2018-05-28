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

/** \file RAS_BoundingBoxManager.cpp
 *  \ingroup bgerast
 */

#include "RAS_BoundingBoxManager.h"

#include <algorithm>

RAS_BoundingBoxManager::RAS_BoundingBoxManager()
{
}

RAS_BoundingBoxManager::~RAS_BoundingBoxManager()
{
	for (RAS_BoundingBox *boundingBox : m_boundingBoxList) {
		delete boundingBox;
	}

	BLI_assert(m_activeBoundingBoxList.empty());
}

RAS_BoundingBox *RAS_BoundingBoxManager::CreateBoundingBox()
{
	RAS_BoundingBox *boundingBox = new RAS_BoundingBox(this);
	return boundingBox;
}

RAS_BoundingBox *RAS_BoundingBoxManager::CreateMeshBoundingBox(const RAS_DisplayArrayList& arrayList)
{
	RAS_BoundingBox *boundingBox = new RAS_MeshBoundingBox(this, arrayList);

	return boundingBox;
}

void RAS_BoundingBoxManager::Update(bool force)
{
	for (RAS_BoundingBox *boundingBox : m_activeBoundingBoxList) {
		boundingBox->Update(force);
	}
}

void RAS_BoundingBoxManager::ClearModified()
{
	for (RAS_BoundingBox *boundingBox : m_activeBoundingBoxList) {
		boundingBox->ClearModified();
	}
}

void RAS_BoundingBoxManager::Merge(RAS_BoundingBoxManager *other)
{
	for (RAS_BoundingBox *boundingBox : other->m_boundingBoxList) {
		boundingBox->SetManager(this);
		m_boundingBoxList.push_back(boundingBox);
	}
	other->m_boundingBoxList.clear();

	m_activeBoundingBoxList.insert(m_activeBoundingBoxList.begin(), other->m_activeBoundingBoxList.begin(), other->m_activeBoundingBoxList.end());
	other->m_activeBoundingBoxList.clear();
}
