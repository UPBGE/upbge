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

RAS_IBatchDisplayArray::RAS_IBatchDisplayArray(PrimitiveType type, const RAS_VertexFormat &format)
	:RAS_IDisplayArray(type, format)
{
}

RAS_IBatchDisplayArray::~RAS_IBatchDisplayArray()
{
}

#define NEW_DISPLAY_ARRAY_UV(vertformat, uv, color, primtype) \
	if (vertformat.uvSize == uv && vertformat.colorSize == color) { \
		return new RAS_BatchDisplayArray<RAS_VertexData<uv, color> >(primtype, vertformat); \
	}

#define NEW_DISPLAY_ARRAY_COLOR(vertformat, color, primtype) \
	NEW_DISPLAY_ARRAY_UV(format, 1, color, type); \
	NEW_DISPLAY_ARRAY_UV(format, 2, color, type); \
	NEW_DISPLAY_ARRAY_UV(format, 3, color, type); \
	NEW_DISPLAY_ARRAY_UV(format, 4, color, type); \
	NEW_DISPLAY_ARRAY_UV(format, 5, color, type); \
	NEW_DISPLAY_ARRAY_UV(format, 6, color, type); \
	NEW_DISPLAY_ARRAY_UV(format, 7, color, type); \
	NEW_DISPLAY_ARRAY_UV(format, 8, color, type);

RAS_IBatchDisplayArray *RAS_IBatchDisplayArray::ConstructArray(RAS_IDisplayArray::PrimitiveType type, const RAS_VertexFormat &format)
{
	NEW_DISPLAY_ARRAY_COLOR(format, 1, type);
	NEW_DISPLAY_ARRAY_COLOR(format, 2, type);
	NEW_DISPLAY_ARRAY_COLOR(format, 3, type);
	NEW_DISPLAY_ARRAY_COLOR(format, 4, type);
	NEW_DISPLAY_ARRAY_COLOR(format, 5, type);
	NEW_DISPLAY_ARRAY_COLOR(format, 6, type);
	NEW_DISPLAY_ARRAY_COLOR(format, 7, type);
	NEW_DISPLAY_ARRAY_COLOR(format, 8, type);

	return nullptr;
}
#undef NEW_DISPLAY_ARRAY_UV
#undef NEW_DISPLAY_ARRAY_COLOR


RAS_IDisplayArray::Type RAS_IBatchDisplayArray::GetType() const
{
	return BATCHING;
}
