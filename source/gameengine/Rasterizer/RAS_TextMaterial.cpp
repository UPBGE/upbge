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

/** \file gameengine/Rasterizer/RAS_TextMaterial.cpp
 *  \ingroup ketsji
 */

#include "RAS_TextMaterial.h"
#include "RAS_TextShader.h"

RAS_TextMaterial::RAS_TextMaterial()
	:RAS_IMaterial("__TextMaterial__")
{
	m_rasMode |= (RAS_ALPHA | RAS_TEXT);
}

RAS_TextMaterial::~RAS_TextMaterial()
{
}

void RAS_TextMaterial::Prepare()
{
}

RAS_IMaterialShader *RAS_TextMaterial::GetShader(RAS_Rasterizer::DrawType UNUSED(drawingMode)) const
{
	return RAS_TextShader::GetSingleton();
}

const std::string RAS_TextMaterial::GetTextureName() const
{
	return "";
}

SCA_IScene *RAS_TextMaterial::GetScene() const
{
	return nullptr;
}

bool RAS_TextMaterial::UseInstancing() const
{
	return false;
}

void RAS_TextMaterial::ReloadMaterial()
{
}

void RAS_TextMaterial::UpdateIPO(const mt::vec4 &rgba, const mt::vec3 &specrgb, float hard, float spec, float ref,
                                float emit, float ambient, float alpha, float specalpha)
{
}

RAS_TextMaterial *RAS_TextMaterial::GetSingleton()
{
	static RAS_TextMaterial singleton;
	return &singleton;
}

