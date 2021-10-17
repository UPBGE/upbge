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

/** \file RAS_MaterialBucket.h
 *  \ingroup bgerast
 */

#pragma once

#include "MT_Transform.h"
#include "RAS_DisplayArrayBucket.h"

class RAS_IPolyMaterial;
class RAS_MaterialShader;

/* Contains a list of display arrays with the same material,
 * and a mesh slot for each mesh that uses display arrays in
 * this bucket */

class RAS_MaterialBucket {
 public:
  RAS_MaterialBucket(RAS_IPolyMaterial *mat);
  virtual ~RAS_MaterialBucket();

  // Material Properties
  RAS_IPolyMaterial *GetPolyMaterial() const;
  RAS_MaterialShader *GetShader() const;
  bool IsAlpha() const;
  bool IsZSort() const;
  bool IsWire() const;
  bool UseInstancing() const;

  /// Set the shader after its conversion or when changing to custom shader.
  void UpdateShader();

  void AddDisplayArrayBucket(RAS_DisplayArrayBucket *bucket);
  void RemoveDisplayArrayBucket(RAS_DisplayArrayBucket *bucket);

  void MoveDisplayArrayBucket(RAS_MeshMaterial *meshmat, RAS_MaterialBucket *bucket);

 private:
  RAS_IPolyMaterial *m_material;
  RAS_MaterialShader *m_shader;
  RAS_DisplayArrayBucketList m_displayArrayBucketList;
};
