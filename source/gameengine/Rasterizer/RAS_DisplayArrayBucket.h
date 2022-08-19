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

/** \file RAS_DisplayArrayBucket.h
 *  \ingroup bgerast
 */

#pragma once

#include <vector>

#include "CM_RefCount.h"
#include "MT_Transform.h"
#include "RAS_Rasterizer.h"

class RAS_MaterialBucket;
class RAS_IDisplayArray;
class RAS_MeshObject;
class RAS_MeshMaterial;

class RAS_DisplayArrayBucket {
 private:
  enum NodeType {
    NODE_DOWNWARD_NORMAL = 0,
    // 		NODE_DOWNWARD_DERIVED_MESH,
    NODE_DOWNWARD_CUBE_MAP,
    NODE_DOWNWARD_INSTANCING,
    NODE_DOWNWARD_BATCHING,
    NODE_DOWNWARD_TYPE_MAX,

    NODE_UPWARD_NORMAL = 0,
    NODE_UPWARD_NO_ARRAY,
    NODE_UPWARD_TYPE_MAX
  };

  /// The parent bucket.
  RAS_MaterialBucket *m_bucket;
  /// The display array = list of vertexes and indexes.
  RAS_IDisplayArray *m_displayArray;
  /// The parent mesh object, it can be nullptr for text objects.
  RAS_MeshObject *m_mesh;
  /// The material mesh.
  RAS_MeshMaterial *m_meshMaterial;

 public:
  RAS_DisplayArrayBucket(RAS_MaterialBucket *bucket,
                         RAS_IDisplayArray *array,
                         RAS_MeshObject *mesh,
                         RAS_MeshMaterial *meshmat);
  ~RAS_DisplayArrayBucket();

  /// \section Accesor
  RAS_MaterialBucket *GetBucket() const;
  RAS_IDisplayArray *GetDisplayArray() const;
  RAS_MeshObject *GetMesh() const;
  RAS_MeshMaterial *GetMeshMaterial() const;

  /// \section Render Infos
  bool UseBatching() const;

  /// Replace the material bucket of this display array bucket by the one given.
  void ChangeMaterialBucket(RAS_MaterialBucket *bucket);
};

typedef std::vector<RAS_DisplayArrayBucket *> RAS_DisplayArrayBucketList;
