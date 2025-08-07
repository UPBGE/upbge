/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup animrig
 *
 * \brief Functions to deal with Armatures.
 */

#pragma once

#include "ANIM_bone_collections.hh"

#include "DNA_armature_types.h"

namespace blender::animrig {

/**
 * Returns true if the given Bone is visible. This includes bone collection visibility.
 */
inline bool bone_is_visible(const bArmature *armature, const Bone *bone)
{
  const bool bone_itself_visible = (bone->flag & BONE_HIDDEN_P) == 0;
  return bone_itself_visible && ANIM_bone_in_visible_collection(armature, bone);
}

inline bool bone_is_visible_pchan(const bArmature *armature, const bPoseChannel *pchan)
{
  const bool bone_itself_visible = (pchan->drawflag & PCHAN_DRAW_HIDDEN) == 0;
  return bone_itself_visible && ANIM_bone_in_visible_collection(armature, pchan->bone);
}

inline bool bone_is_visible_editbone(const bArmature *armature, const EditBone *ebone)
{
  const bool bone_itself_visible = (ebone->flag & BONE_HIDDEN_A) == 0;
  return bone_itself_visible && ANIM_bonecoll_is_visible_editbone(armature, ebone);
}

/**
 * Returns true if the bone is selected. This includes a visibility check
 * because invisible bones cannot be selected, no matter their flag.
 */
inline bool bone_is_selected(const bArmature *armature, const Bone *bone)
{
  return (bone->flag & BONE_SELECTED) && bone_is_visible(armature, bone);
}

inline bool bone_is_selected(const bArmature *armature, const bPoseChannel *pchan)
{
  return (pchan->bone->flag & BONE_SELECTED) && bone_is_visible_pchan(armature, pchan);
}

inline bool bone_is_selected(const bArmature *armature, const EditBone *ebone)
{
  return (ebone->flag & BONE_SELECTED) && bone_is_visible_editbone(armature, ebone);
}

/**
 * Iterates all descendents of the given pose bone including the bone itself. Iterates breadth
 * first.
 */
void pose_bone_descendent_iterator(bPose &pose,
                                   bPoseChannel &pose_bone,
                                   FunctionRef<void(bPoseChannel &child_bone)> callback);

}  // namespace blender::animrig
