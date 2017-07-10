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

/** \file gameengine/Rasterizer/RAS_AttributeArray.cpp
 *  \ingroup bgerast
 */

#include "RAS_AttributeArray.h"
#include "RAS_AttributeArrayStorage.h"
#include "RAS_IDisplayArray.h"

RAS_AttributeArray::RAS_AttributeArray(const AttribList& attribs, RAS_IDisplayArray *array)
	:m_attribs(attribs),
	m_array(array)
{
}

RAS_AttributeArray::~RAS_AttributeArray()
{
}

RAS_AttributeArrayStorage *RAS_AttributeArray::GetStorage(RAS_Rasterizer::DrawType drawingMode)
{
	RAS_AttributeArrayStorage *storage = m_storages[drawingMode].get();
	if (!storage) {
		storage = new RAS_AttributeArrayStorage(m_array, m_array->GetStorage(), m_attribs);
		m_storages[drawingMode].reset(storage);
	}

	return storage;
}

void RAS_AttributeArray::DestructStorages()
{
	for (unsigned short i = 0; i < RAS_Rasterizer::RAS_DRAW_MAX; ++i) {
		m_storages[i].reset(nullptr);
	}
}
