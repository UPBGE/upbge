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
#include "RAS_DisplayArray.h"

RAS_AttributeArray::RAS_AttributeArray(RAS_DisplayArray *array)
	:m_array(array)
{
}

RAS_AttributeArray::RAS_AttributeArray(const AttribList& attribs, RAS_DisplayArray *array)
	:m_attribs(attribs),
	m_array(array)
{
}

RAS_AttributeArray::~RAS_AttributeArray()
{
}

RAS_AttributeArray& RAS_AttributeArray::operator=(RAS_AttributeArray&& other)
{
	m_array = other.m_array;
	m_attribs = std::move(other.m_attribs);

	for (std::unique_ptr<RAS_AttributeArrayStorage>& storage : m_storages) {
		storage.reset(nullptr);
	}

	return *this;
}

RAS_AttributeArrayStorage *RAS_AttributeArray::GetStorage(RAS_Rasterizer::DrawType drawingMode)
{
	std::unique_ptr<RAS_AttributeArrayStorage>& storage = m_storages[drawingMode];
	if (!storage) {
		storage.reset(new RAS_AttributeArrayStorage(m_array->GetLayout(), &m_array->GetStorage(), m_attribs));
	}

	return storage.get();
}

void RAS_AttributeArray::Clear()
{
	for (std::unique_ptr<RAS_AttributeArrayStorage> &storage : m_storages) {
		storage.reset(nullptr);
	}
}
