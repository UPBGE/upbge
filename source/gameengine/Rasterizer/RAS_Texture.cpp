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
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/Ketsji/RAS_Texture.cpp
 *  \ingroup ketsji
 */

#include "glew-mx.h"

#include "RAS_Texture.h"


RAS_Texture::RAS_Texture()
	:m_bindCode(-1),
	m_name(""),
	m_cubeMap(NULL),
	m_planar(NULL)
{
}

RAS_Texture::~RAS_Texture()
{
}

STR_String& RAS_Texture::GetName()
{
	return m_name;
}

void RAS_Texture::SetCubeMap(RAS_CubeMap *cubeMap)
{
	m_cubeMap = cubeMap;
}

RAS_CubeMap *RAS_Texture::GetCubeMap() const
{
	return m_cubeMap;
}

void RAS_Texture::SetPlanar(RAS_Planar *planar)
{
	m_planar = planar;
}

RAS_Planar *RAS_Texture::GetPlanar() const
{
	return m_planar;
}

int RAS_Texture::GetCubeMapTextureType()
{
	return GL_TEXTURE_CUBE_MAP;
}

int RAS_Texture::GetTexture2DType()
{
	return GL_TEXTURE_2D;
}

void RAS_Texture::DesactiveTextures()
{
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, 0);
}

int RAS_Texture::GetBindCode() const
{
	return m_bindCode;
}

void RAS_Texture::SetBindCode(int bindcode)
{
	m_bindCode = bindcode;
}
