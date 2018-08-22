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

/** \file gameengine/Rasterizer/RAS_IMaterial.cpp
 *  \ingroup bgerast
 */


#include "RAS_IMaterial.h"

RAS_IMaterial::RAS_IMaterial(const std::string& name)
	:m_name(name),
	m_drawingMode(0),
	m_zoffset(0.0f),
	m_rasMode(0),
	m_flag(0)
{
	for (unsigned short i = 0; i < RAS_Texture::MaxUnits; ++i) {
		m_textures[i] = nullptr;
	}
}

RAS_IMaterial::~RAS_IMaterial()
{
	for (unsigned short i = 0; i < RAS_Texture::MaxUnits; ++i) {
		if (m_textures[i]) {
			delete m_textures[i];
		}
	}
}

bool RAS_IMaterial::IsAlphaShadow() const
{
	return (m_rasMode & RAS_ALPHA_SHADOW);
}

bool RAS_IMaterial::IsWire() const
{
	return (m_rasMode & RAS_WIRE);
}

bool RAS_IMaterial::IsText() const
{
	return (m_rasMode & RAS_TEXT);
}

bool RAS_IMaterial::IsCullFace() const
{
	return !(m_rasMode & (RAS_TWOSIDED | RAS_WIRE));
}

bool RAS_IMaterial::IsTwoSided() const
{
	return (m_rasMode & RAS_TWOSIDED);
}

bool RAS_IMaterial::IsVisible() const
{
	return (m_rasMode & RAS_VISIBLE);
}

bool RAS_IMaterial::IsCollider() const
{
	return (m_rasMode & RAS_COLLIDER);
}

bool RAS_IMaterial::IsAlpha() const
{
	return (m_rasMode & (RAS_ALPHA | RAS_ZSORT));
}

bool RAS_IMaterial::IsAlphaDepth() const
{
	return (m_rasMode & RAS_DEPTH_ALPHA);
}

bool RAS_IMaterial::IsZSort() const
{
	return (m_rasMode & RAS_ZSORT);
}

int RAS_IMaterial::GetDrawingMode() const
{
	return m_drawingMode;
}

float RAS_IMaterial::GetZOffset() const
{
	return m_zoffset;
}

std::string RAS_IMaterial::GetName()
{
	return m_name;
}

unsigned int RAS_IMaterial::GetFlag() const
{
	return m_flag;
}

bool RAS_IMaterial::CastsShadows() const
{
	return (m_flag & RAS_CASTSHADOW) != 0;
}

bool RAS_IMaterial::OnlyShadow() const
{
	return (m_flag & RAS_ONLYSHADOW) != 0;
}

RAS_Texture *RAS_IMaterial::GetTexture(unsigned int index)
{
	return m_textures[index];
}

void RAS_IMaterial::UpdateTextures()
{
	/** We make sure that all gpu textures are the same in material textures here
	 * than in gpu material.
	 */
	for (unsigned short i = 0; i < RAS_Texture::MaxUnits; i++) {
		RAS_Texture *tex = m_textures[i];
		if (tex && tex->Ok()) {
			tex->CheckValidTexture();
		}
	}
}

void RAS_IMaterial::ActivateTextures()
{
	for (unsigned short i = 0; i < RAS_Texture::MaxUnits; i++) {
		if (m_textures[i] && m_textures[i]->Ok()) {
			m_textures[i]->ActivateTexture(i);
		}
	}
}

void RAS_IMaterial::DeactivateTextures()
{
	for (unsigned short i = 0; i < RAS_Texture::MaxUnits; i++) {
		if (m_textures[i] && m_textures[i]->Ok()) {
			m_textures[i]->DisableTexture();
		}
	}
}
