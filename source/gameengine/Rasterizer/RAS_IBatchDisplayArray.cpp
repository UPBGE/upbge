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

/** \file RAS_IBatchDisplayArray.cpp
 *  \ingroup bgerast
 */

#include "RAS_BatchDisplayArray.h"

#include "CM_Template.h"

RAS_IBatchDisplayArray::RAS_IBatchDisplayArray(PrimitiveType type, const RAS_VertexFormat &format,
		const RAS_VertexDataMemoryFormat& memoryFormat)
	:RAS_IDisplayArray(type, format, memoryFormat)
{
}

RAS_IBatchDisplayArray::~RAS_IBatchDisplayArray()
{
}

RAS_IBatchDisplayArray *RAS_IBatchDisplayArray::ConstructArray(RAS_IDisplayArray::PrimitiveType type, const RAS_VertexFormat &format)
{
	return CM_InstantiateTemplateSwitch<RAS_IBatchDisplayArray, RAS_BatchDisplayArray, RAS_VertexFormatTuple>(format, type, format);
}

RAS_IDisplayArray::Type RAS_IBatchDisplayArray::GetType() const
{
	return BATCHING;
}
