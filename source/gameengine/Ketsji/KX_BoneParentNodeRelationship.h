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
 *
 */

/** \file KX_BoneParentNodeRelationship.h
 *  \ingroup ketsji
 */

#pragma once

#include "SG_ParentRelation.h"

struct Bone;

/**
 *  Bone parent relationship parents a child SG_Node frame to a
 *  bone in an armature object.
 */
class KX_BoneParentRelation : public SG_ParentRelation {

 private:
  Bone *m_bone;

 public:
  KX_BoneParentRelation(Bone *bone);
  virtual ~KX_BoneParentRelation();

  /**
   *  Updates the childs world coordinates relative to the parent's
   *  world coordinates.
   *
   *  Parent should be a BL_ArmatureObject.
   */
  bool UpdateChildCoordinates(SG_Node *child, const SG_Node *parent, bool &parentUpdated);

  /// Create a copy of this relationship
  virtual SG_ParentRelation *NewCopy();
};
