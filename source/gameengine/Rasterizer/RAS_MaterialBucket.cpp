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

/** \file gameengine/Rasterizer/RAS_MaterialBucket.cpp
 *  \ingroup bgerast
 */

#include "RAS_MaterialBucket.h"

#include "CM_List.h"
#include "RAS_IPolygonMaterial.h"

#ifdef _MSC_VER
#  pragma warning(disable : 4786)
#endif

#ifdef WIN32
#  include <windows.h>
#endif  // WIN32

RAS_MaterialBucket::RAS_MaterialBucket(RAS_IPolyMaterial *mat) : m_material(mat), m_shader(nullptr)
{
}

RAS_MaterialBucket::~RAS_MaterialBucket()
{
}

RAS_IPolyMaterial *RAS_MaterialBucket::GetPolyMaterial() const
{
  return m_material;
}

RAS_MaterialShader *RAS_MaterialBucket::GetShader() const
{
  return m_shader;
}

bool RAS_MaterialBucket::IsAlpha() const
{
  return (m_material->IsAlpha());
}

bool RAS_MaterialBucket::IsZSort() const
{
  return (m_material->IsZSort());
}

bool RAS_MaterialBucket::IsWire() const
{
  return (m_material->IsWire());
}

bool RAS_MaterialBucket::UseInstancing() const
{
  return false;  //(m_material->UseInstancing());
}

void RAS_MaterialBucket::UpdateShader()
{
  m_shader = m_material->GetShader();
}

void RAS_MaterialBucket::AddDisplayArrayBucket(RAS_DisplayArrayBucket *bucket)
{
  m_displayArrayBucketList.push_back(bucket);
}

void RAS_MaterialBucket::RemoveDisplayArrayBucket(RAS_DisplayArrayBucket *bucket)
{
  if (m_displayArrayBucketList.size() == 0) {
    return;
  }
  CM_ListRemoveIfFound(m_displayArrayBucketList, bucket);
}

void RAS_MaterialBucket::MoveDisplayArrayBucket(RAS_MeshMaterial *meshmat,
                                                RAS_MaterialBucket *bucket)
{
  for (RAS_DisplayArrayBucketList::iterator dit = m_displayArrayBucketList.begin();
       dit != m_displayArrayBucketList.end();) {
    // In case of deformers, multiple display array bucket can use the same mesh and material.
    RAS_DisplayArrayBucket *displayArrayBucket = *dit;
    if (displayArrayBucket->GetMeshMaterial() != meshmat) {
      ++dit;
      continue;
    }

    displayArrayBucket->ChangeMaterialBucket(bucket);
    bucket->AddDisplayArrayBucket(displayArrayBucket);
    dit = m_displayArrayBucketList.erase(dit);
  }
}
