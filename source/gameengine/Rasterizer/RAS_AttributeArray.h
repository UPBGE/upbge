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

/** \file RAS_AttributeArray.h
 *  \ingroup bgerast
 */

#ifndef __RAS_ATTRIBUTE_ARRAY_H__
#define __RAS_ATTRIBUTE_ARRAY_H__

#include "RAS_Rasterizer.h"

#include <vector>

class RAS_DisplayArray;
class RAS_AttributeArrayStorage;
class RAS_DisplayArrayStorage;

class RAS_AttributeArray
{
public:
	enum AttribType {
		RAS_ATTRIB_POS, // Vertex coordinates.
		RAS_ATTRIB_UV, // UV coordinates.
		RAS_ATTRIB_NORM, // Normal coordinates.
		RAS_ATTRIB_TANGENT, // Tangent coordinates.
		RAS_ATTRIB_COLOR, // Vertex color.
		RAS_ATTRIB_MAX
	};

	struct Attrib
	{
		unsigned short m_loc;
		AttribType m_type;
		bool m_texco;
		unsigned short m_layer;
	};

	/* Attribute list of the following format:
	 * hashed name: (attrib type, texco, layer(optional)).
	 */
	using AttribList = std::vector<Attrib>;
	using AttribTable = std::array<AttribList, RAS_Rasterizer::RAS_DRAW_MAX>;

private:
	std::unique_ptr<RAS_AttributeArrayStorage> m_storage;

public:
	RAS_AttributeArray();
	RAS_AttributeArray(const AttribList& attribs, RAS_DisplayArray *array);
	~RAS_AttributeArray();

	RAS_AttributeArray& operator=(RAS_AttributeArray&& other);

	RAS_AttributeArrayStorage *GetStorage() const;
};

#endif  // __RAS_ATTRIBUTE_ARRAY_H__
