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
	m_probe(nullptr)
{
}

RAS_Texture::~RAS_Texture()
{
}

std::string& RAS_Texture::GetName()
{
	return m_name;
}

void RAS_Texture::SetProbe(RAS_TextureProbe *probe)
{
	m_probe = probe;
}

RAS_TextureProbe *RAS_Texture::GetProbe() const
{
	return m_probe;
}

int RAS_Texture::GetCubeMapTextureType()
{
	return GL_TEXTURE_CUBE_MAP;
}

int RAS_Texture::GetTexture2DType()
{
	return GL_TEXTURE_2D;
}

std::initializer_list<int> RAS_Texture::GetCubeMapTargets()
{
	static std::initializer_list<int> targets = {
		GL_TEXTURE_CUBE_MAP_POSITIVE_Z_ARB,
		GL_TEXTURE_CUBE_MAP_NEGATIVE_Z_ARB,
		GL_TEXTURE_CUBE_MAP_POSITIVE_X_ARB,
		GL_TEXTURE_CUBE_MAP_NEGATIVE_X_ARB,
		GL_TEXTURE_CUBE_MAP_POSITIVE_Y_ARB,
		GL_TEXTURE_CUBE_MAP_NEGATIVE_Y_ARB
	};

	return targets;
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
