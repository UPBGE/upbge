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

/** \file gameengine/Ketsji/KX_NodeRelationships.cpp
 *  \ingroup ketsji
 */

#include "KX_NodeRelationships.h"

#include "MT_Matrix4x4.h"

KX_NormalParentRelation::KX_NormalParentRelation()
{
}

KX_NormalParentRelation::~KX_NormalParentRelation()
{
}

bool KX_NormalParentRelation::UpdateChildCoordinates(SG_Node *child,
                                                     const SG_Node *parent,
                                                     bool &parentUpdated)
{
  BLI_assert(child != nullptr);

  if (!parentUpdated && !child->IsModified()) {
    return false;
  }

  parentUpdated = true;

  // Simple case
  if (!parent) {
    child->SetWorldFromLocalTransform();
    child->ClearModified();
  }
  else {
    const MT_Transform trans(parent->GetWorldTransform() * child->GetLocalTransform());
    MT_Matrix4x4 tmat(trans.toMatrix());
    float sx =
        MT_Vector3(tmat.getElement(0, 0), tmat.getElement(1, 0), tmat.getElement(2, 0)).length();
    float sy =
        MT_Vector3(tmat.getElement(0, 1), tmat.getElement(1, 1), tmat.getElement(2, 1)).length();
    float sz =
        MT_Vector3(tmat.getElement(0, 2), tmat.getElement(1, 2), tmat.getElement(2, 2)).length();
    const MT_Vector3 scale(sx, sy, sz);
    const MT_Vector3 invscale(1.0f / sx, 1.0f / sy, 1.0f / sz);
    const MT_Vector3 pos = trans.getOrigin();
    const MT_Matrix3x3 rot = trans.getBasis().scaled(invscale.x(), invscale.y(), invscale.z());

    child->SetWorldScale(scale);
    child->SetWorldPosition(pos);
    child->SetWorldOrientation(rot);

    child->ClearModified();
  }
  return true;
}

SG_ParentRelation *KX_NormalParentRelation::NewCopy()
{
  return new KX_NormalParentRelation();
}

KX_VertexParentRelation::KX_VertexParentRelation()
{
}

KX_VertexParentRelation::~KX_VertexParentRelation()
{
}

bool KX_VertexParentRelation::UpdateChildCoordinates(SG_Node *child,
                                                     const SG_Node *parent,
                                                     bool &parentUpdated)
{

  BLI_assert(child != nullptr);

  if (!parentUpdated && !child->IsModified()) {
    return false;
  }

  child->SetWorldScale(child->GetLocalScale());

  if (parent) {
    child->SetWorldPosition(child->GetLocalPosition() + parent->GetWorldPosition());
  }
  else {
    child->SetWorldPosition(child->GetLocalPosition());
  }

  child->SetWorldOrientation(child->GetLocalOrientation());
  child->ClearModified();
  return true;
}

SG_ParentRelation *KX_VertexParentRelation::NewCopy()
{
  return new KX_VertexParentRelation();
};

bool KX_VertexParentRelation::IsVertexRelation()
{
  return true;
}

KX_SlowParentRelation::KX_SlowParentRelation(float relaxation)
    : m_relax(relaxation), m_initialized(false)
{
}

KX_SlowParentRelation::~KX_SlowParentRelation()
{
}

bool KX_SlowParentRelation::UpdateChildCoordinates(SG_Node *child,
                                                   const SG_Node *parent,
                                                   bool &parentUpdated)
{
  BLI_assert(child != nullptr);

  // The child will move even if the parent is not
  parentUpdated = true;

  const MT_Vector3 &child_scale = child->GetLocalScale();
  const MT_Vector3 &child_pos = child->GetLocalPosition();
  const MT_Matrix3x3 &child_rotation = child->GetLocalOrientation();

  // the childs world locations which we will update.
  MT_Vector3 child_w_scale;
  MT_Vector3 child_w_pos;
  MT_Matrix3x3 child_w_rotation;

  if (parent) {

    /* This is a slow parent relation
     * first compute the normal child world coordinates. */

    const MT_Vector3 &p_world_scale = parent->GetWorldScaling();
    const MT_Vector3 &p_world_pos = parent->GetWorldPosition();
    const MT_Matrix3x3 &p_world_rotation = parent->GetWorldOrientation();

    MT_Vector3 child_n_scale = p_world_scale * child_scale;
    MT_Matrix3x3 child_n_rotation = p_world_rotation * child_rotation;
    MT_Vector3 child_n_pos = p_world_pos + p_world_scale * (p_world_rotation * child_pos);

    if (m_initialized) {

      // Get the current world positions
      child_w_scale = child->GetWorldScaling();
      child_w_pos = child->GetWorldPosition();
      child_w_rotation = child->GetWorldOrientation();

      /* Now 'interpolate' the normal coordinates with the last
       * world coordinates to get the new world coordinates. */
      float weight = 1.0f / (m_relax + 1.0f);
      child_w_scale = (m_relax * child_w_scale + child_n_scale) * weight;
      child_w_pos = (m_relax * child_w_pos + child_n_pos) * weight;

      // For rotation we must go through quaternion
      MT_Quaternion child_w_quat = child_w_rotation.getRotation().slerp(
          child_n_rotation.getRotation(), weight);
      child_w_rotation.setRotation(child_w_quat);
    }
    else {
      child_w_scale = child_n_scale;
      child_w_pos = child_n_pos;
      child_w_rotation = child_n_rotation;
      m_initialized = true;
    }
  }
  else {

    child_w_scale = child_scale;
    child_w_pos = child_pos;
    child_w_rotation = child_rotation;
  }

  child->SetWorldScale(child_w_scale);
  child->SetWorldPosition(child_w_pos);
  child->SetWorldOrientation(child_w_rotation);
  child->ClearModified();
  // This node must always be updated, so reschedule it for next time
  child->ActivateRecheduleUpdateCallback();

  return true;
}

SG_ParentRelation *KX_SlowParentRelation::NewCopy()
{
  return new KX_SlowParentRelation(m_relax);
}

float KX_SlowParentRelation::GetTimeOffset()
{
  return m_relax;
}

void KX_SlowParentRelation::SetTimeOffset(float relaxation)
{
  m_relax = relaxation;
}

bool KX_SlowParentRelation::IsSlowRelation()
{
  return true;
}
