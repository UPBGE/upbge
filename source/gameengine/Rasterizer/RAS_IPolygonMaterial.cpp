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

using namespace blender;

RAS_IPolyMaterial::RAS_IPolyMaterial(const std::string &name)
    : m_name(name)
{
  for (unsigned short i = 0; i < BL_Texture::MaxUnits; ++i) {
    m_textures[i] = nullptr;
  }
}

RAS_IPolyMaterial::~RAS_IPolyMaterial()
{
  for (unsigned short i = 0; i < BL_Texture::MaxUnits; ++i) {
    if (m_textures[i]) {
      delete m_textures[i];
    }
  }
}

std::string RAS_IPolyMaterial::GetName()
{
  return m_name;
}

BL_Texture *RAS_IPolyMaterial::GetTexture(unsigned int index)
{
  return m_textures[index];
}

BL_Texture *RAS_IPolyMaterial::GetTextureByNodeName(char *name)
{
  for (BL_Texture *tex : m_textures) {
    if (tex && STREQ(tex->GetName().c_str(), name)) {
      return tex;
    }
  }
  return nullptr;
}
