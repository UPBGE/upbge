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

/** \file RAS_IPolygonMaterial.h
 *  \ingroup bgerast
 */

#pragma once

#include <map>
#include <string>

#include "DNA_material_types.h"
#include "DNA_scene_types.h"

#include "MT_Vector4.h"
#include "RAS_MeshObject.h"
#include "BL_Texture.h"

class RAS_MaterialShader;
class SCA_IScene;

/**
 * Polygon blender::Material on which the material buckets are sorted
 */
class RAS_IPolyMaterial {
 protected:
  std::string m_name;  // also needed for collisionsensor

  BL_Texture *m_textures[BL_Texture::MaxUnits];

 public:

  RAS_IPolyMaterial(const std::string &name);

  virtual ~RAS_IPolyMaterial();

  virtual std::string GetName();
  BL_Texture *GetTexture(unsigned int index);
  BL_Texture *GetTextureByNodeName(char *name);

  virtual blender::Material *GetBlenderMaterial() const = 0;
  virtual blender::Scene *GetBlenderScene() const = 0;
  virtual SCA_IScene *GetScene() const = 0;

  /*
   * PreCalculate texture gen
   */
  virtual void OnConstruction() = 0;
};
