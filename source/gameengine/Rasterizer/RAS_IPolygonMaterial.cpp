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

#include "DNA_material_types.h"

RAS_IPolyMaterial::RAS_IPolyMaterial(const std::string &name, GameSettings *game)
    : m_name(name), m_alphablend(0), m_zoffset(0.0f), m_rasMode(0), m_flag(0)
{
  if (game) {
    m_drawingmode = ConvertFaceMode(game);
  }

  for (unsigned short i = 0; i < RAS_Texture::MaxUnits; ++i) {
    m_textures[i] = nullptr;
  }
}

RAS_IPolyMaterial::~RAS_IPolyMaterial()
{
  for (unsigned short i = 0; i < RAS_Texture::MaxUnits; ++i) {
    if (m_textures[i]) {
      delete m_textures[i];
    }
  }
}

int RAS_IPolyMaterial::ConvertFaceMode(struct GameSettings *game) const
{
  int modefinal = 0;

  int orimode = game->face_orientation;
  int alpha_blend = game->alpha_blend;
  int flags = game->flag & (GEMAT_BACKCULL);

  modefinal = orimode | alpha_blend | flags;

  return modefinal;
}

bool RAS_IPolyMaterial::IsAlphaShadow() const
{
  return (m_rasMode & RAS_ALPHA_SHADOW);
}

bool RAS_IPolyMaterial::IsWire() const
{
  return (m_rasMode & RAS_WIRE);
}

bool RAS_IPolyMaterial::IsText() const
{
  return (m_rasMode & RAS_TEXT);
}

bool RAS_IPolyMaterial::IsCullFace() const
{
  return !(m_rasMode & (RAS_TWOSIDED | RAS_WIRE));
}

bool RAS_IPolyMaterial::IsAlpha() const
{
  return (m_rasMode & (RAS_ALPHA | RAS_ZSORT));
}

bool RAS_IPolyMaterial::IsAlphaDepth() const
{
  return (m_rasMode & RAS_DEPTH_ALPHA);
}

bool RAS_IPolyMaterial::IsZSort() const
{
  return (m_rasMode & RAS_ZSORT);
}

int RAS_IPolyMaterial::GetDrawingMode() const
{
  return m_drawingmode;
}

int RAS_IPolyMaterial::GetAlphaBlend() const
{
  return m_alphablend;
}

float RAS_IPolyMaterial::GetZOffset() const
{
  return m_zoffset;
}

std::string RAS_IPolyMaterial::GetName()
{
  return m_name;
}

unsigned int RAS_IPolyMaterial::GetFlag() const
{
  return m_flag;
}

bool RAS_IPolyMaterial::UsesLighting() const
{
  // Return false only if material is shadeless.
  return (m_flag & RAS_MULTILIGHT);
}

bool RAS_IPolyMaterial::CastsShadows() const
{
  return (m_flag & RAS_CASTSHADOW) != 0;
}

bool RAS_IPolyMaterial::OnlyShadow() const
{
  return (m_flag & RAS_ONLYSHADOW) != 0;
}

RAS_Texture *RAS_IPolyMaterial::GetTexture(unsigned int index)
{
  return m_textures[index];
}
