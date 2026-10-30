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

/** \file gameengine/Ketsji/KX_BoneParentNodeRelationship.cpp
 *  \ingroup ketsji
 */

#include "KX_BoneParentNodeRelationship.h"

#include <cfloat>
#include <cmath>
#include <utility>

#include "BL_ArmatureObject.h"
#include "DNA_armature_types.h"
#include "MT_Matrix4x4.h"
#include "MT_Transform.h"

using namespace blender;

namespace {

bool transform_is_finite_and_nonsingular(const MT_Transform &transform)
{
  const MT_Vector3 &origin = transform.getOrigin();
  const MT_Matrix3x3 &basis = transform.getBasis();
  for (int row = 0; row < 3; ++row) {
    if (!std::isfinite(origin[row])) {
      return false;
    }
    for (int column = 0; column < 3; ++column) {
      if (!std::isfinite(basis[row][column])) {
        return false;
      }
    }
  }
  const MT_Scalar determinant = basis.determinant();
  return std::isfinite(determinant) && std::abs(determinant) > MT_Scalar(FLT_EPSILON);
}

}  // namespace

KX_BoneParentRelation::KX_BoneParentRelation(blender::Bone *bone)
    : m_boneName(bone != nullptr ? bone->name : "")
{
}

KX_BoneParentRelation::KX_BoneParentRelation(std::string bone_name)
    : m_boneName(std::move(bone_name))
{
}

KX_BoneParentRelation::~KX_BoneParentRelation()
{
}

const std::string &KX_BoneParentRelation::GetBoneName() const
{
  return m_boneName;
}

bool KX_BoneParentRelation::GetParentTransform(const SG_Node *parent,
                                               MT_Transform &r_transform) const
{
  if (parent == nullptr || m_boneName.empty()) {
    return false;
  }

  BL_ArmatureObject *armature = dynamic_cast<BL_ArmatureObject *>(
      static_cast<KX_GameObject *>(parent->GetSGClientObject()));
  if (armature == nullptr) {
    return false;
  }

  MT_Matrix4x4 bone_matrix;
  float bone_length = 0.0f;
  if (!armature->GetBoneMatrix(m_boneName, bone_matrix, bone_length) ||
      !std::isfinite(bone_length))
  {
    return false;
  }

  const MT_Transform tail_offset(MT_Vector3(0.0f, bone_length, 0.0f),
                                 MT_Matrix3x3::Identity());
  const MT_Matrix4x4 world_matrix = MT_Matrix4x4(parent->GetWorldTransform()) * bone_matrix *
                                    MT_Matrix4x4(tail_offset);
  r_transform = world_matrix.toTransform();
  return transform_is_finite_and_nonsingular(r_transform);
}

bool KX_BoneParentRelation::UpdateChildCoordinates(SG_Node *child,
                                                   const SG_Node *parent,
                                                   bool &parentUpdated)
{
  BLI_assert(child != nullptr);

  /* A pose may change even when the armature scene-graph transform does not. */
  parentUpdated = true;

  MT_Transform parent_transform;
  const bool valid_parent_transform = GetParentTransform(parent, parent_transform);
  bool coordinates_updated = false;
  if (valid_parent_transform) {
    const MT_Transform world_transform = parent_transform * child->GetLocalTransform();
    const MT_Matrix3x3 &basis = world_transform.getBasis();
    const MT_Vector3 world_scale(
        basis.getColumn(0).length(), basis.getColumn(1).length(), basis.getColumn(2).length());
    if (transform_is_finite_and_nonsingular(world_transform) &&
        std::isfinite(world_scale.x()) && std::isfinite(world_scale.y()) &&
        std::isfinite(world_scale.z()) && world_scale.x() > FLT_EPSILON &&
        world_scale.y() > FLT_EPSILON && world_scale.z() > FLT_EPSILON)
    {
      child->SetWorldScale(world_scale);
      child->SetWorldPosition(world_transform.getOrigin());
      child->SetWorldOrientation(basis.scaled(1.0f / world_scale.x(),
                                             1.0f / world_scale.y(),
                                             1.0f / world_scale.z()));
      coordinates_updated = true;
    }
  }
  else if (parent == nullptr) {
    child->SetWorldFromLocalTransform();
    coordinates_updated = true;
  }
  child->ClearModified();
  /* Bone pose changes are independent of the armature SG-node dirty flag. */
  child->ActivateRecheduleUpdateCallback();
  return coordinates_updated;
}

SG_ParentRelation *KX_BoneParentRelation::NewCopy()
{
  return new KX_BoneParentRelation(m_boneName);
}
