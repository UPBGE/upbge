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

/** \file gameengine/Rasterizer/RAS_IPolygonMaterial.cpp
 *  \ingroup bgerast
 */


#include "RAS_IPolygonMaterial.h"
#include "RAS_IRasterizer.h"

#include "DNA_material_types.h"

RAS_IPolyMaterial::RAS_IPolyMaterial(
	const STR_String& matname,
	int alphablend,
	int rasmode,
	GameSettings *game)
	:m_materialname(matname),
	m_alphablend(alphablend),
	m_rasMode(rasmode),
	m_polymatid(m_newpolymatid++),
	m_flag(0)
{
	m_polymatid = m_newpolymatid++;
	m_drawingmode = ConvertFaceMode(game, m_rasMode & RAS_TEX);
}

int RAS_IPolyMaterial::ConvertFaceMode(struct GameSettings *game, bool image) const
{
	if (!game) {
		return (image ? GEMAT_TEX : 0);
	}

	int modefinal = 0;

	int orimode   = game->face_orientation;
	int alpha_blend = game->alpha_blend;
	int flags = game->flag & (GEMAT_TEXT | GEMAT_BACKCULL);

	modefinal = orimode | alpha_blend | flags;
	modefinal |= (image ? GEMAT_TEX : 0);

	return modefinal;
}

bool RAS_IPolyMaterial::IsAlphaShadow() const
{
	return m_alphablend != GEMAT_SOLID;
}

bool RAS_IPolyMaterial::IsWire() const
{
	return (m_rasMode & RAS_WIRE);
}

void RAS_IPolyMaterial::GetMaterialRGBAColor(unsigned char *rgba) const
{
	*rgba++ = 0xFF;
	*rgba++ = 0xFF;
	*rgba++ = 0xFF;
	*rgba++ = 0xFF;
}

bool RAS_IPolyMaterial::IsAlpha() const
{
	return (m_rasMode & (RAS_ALPHA | RAS_ZSORT));
}

bool RAS_IPolyMaterial::IsZSort() const
{
	return (m_rasMode & RAS_ZSORT);
}

unsigned int RAS_IPolyMaterial::hash() const
{
	return m_materialname.hash();
}

int RAS_IPolyMaterial::GetDrawingMode() const
{
	return m_drawingmode;
}

const STR_String& RAS_IPolyMaterial::GetMaterialName() const
{
	return m_materialname;
}

dword RAS_IPolyMaterial::GetMaterialNameHash() const
{
	return m_materialname.hash();
}

unsigned int RAS_IPolyMaterial::GetFlag() const
{
	return m_flag;
}

bool RAS_IPolyMaterial::UsesLighting(RAS_IRasterizer *rasty) const
{
	bool dolights = false;

	if (!(m_flag & RAS_BLENDERGLSL)) {
		dolights = (m_flag & RAS_MULTILIGHT) != 0;
	}
	else if (rasty->GetDrawingMode() < RAS_IRasterizer::RAS_SOLID) {
		/* pass */
	}
	else if (rasty->GetDrawingMode() == RAS_IRasterizer::RAS_SHADOW) {
		/* pass */
	}
	else {
		dolights = (m_rasMode & RAS_USE_LIGHT);
	}

	return dolights;
}

bool RAS_IPolyMaterial::CastsShadows() const
{
	return (m_flag & RAS_CASTSHADOW) != 0;
}

bool RAS_IPolyMaterial::OnlyShadow() const
{
	return (m_flag & RAS_ONLYSHADOW) != 0;
}

bool RAS_IPolyMaterial::UsesObjectColor() const
{
	return (!(m_flag & RAS_BLENDERGLSL)) && (m_flag & RAS_OBJECTCOLOR);
}

unsigned int RAS_IPolyMaterial::m_newpolymatid = 0;

