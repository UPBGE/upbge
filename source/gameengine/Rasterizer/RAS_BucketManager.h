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

/** \file RAS_BucketManager.h
 *  \ingroup bgerast
 */

#pragma once

#include <vector>

#include "MT_Transform.h"
#include "RAS_MaterialBucket.h"


class RAS_BucketManager {
 public:
  typedef std::vector<RAS_MaterialBucket *> BucketList;

 private:
  enum BucketType {
    ALL_BUCKET = 0,
    NUM_BUCKET_TYPE,
  };

  /// Override shaders.
  enum OverrideShaderType {
    OVERRIDE_SHADER_NONE = 0,
    OVERRIDE_SHADER_BLACK,
    OVERRIDE_SHADER_SHADOW,
    OVERRIDE_SHADER_MAX
  };

  BucketList m_buckets[NUM_BUCKET_TYPE];

 public:
  /** Initialize bucket manager
   */
  RAS_BucketManager();
  virtual ~RAS_BucketManager();

  RAS_MaterialBucket *FindBucket(RAS_IPolyMaterial *material, bool &bucketCreated);

  void UpdateShaders(RAS_IPolyMaterial *material = nullptr);
  void ReleaseMaterials(RAS_IPolyMaterial *material = nullptr);

  // freeing scenes only
  void RemoveMaterial(RAS_IPolyMaterial *mat);

  // for merging
  void MergeBucketManager(RAS_BucketManager *other);
  BucketList &GetBuckets()
  {
    return m_buckets[ALL_BUCKET];
  }
};
