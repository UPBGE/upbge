/* SPDX-FileCopyrightText: 2009 Blender Authors, Joshua Leung.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edanimation
 */

#pragma once

struct ListBase;
struct wmOperatorType;

/* size of string buffers used for animation channel displayed names */
#define ANIM_CHAN_NAME_SIZE 256

/* KeyingSets/Keyframing Interface ------------- */

/** List of builtin KeyingSets (defined in `blender/animrig/keyingsets.cc`). */
extern ListBase builtin_keyingsets;

/* Operator Define Prototypes ------------------- */

/* -------------------------------------------------------------------- */
/** \name Main Keyframe Management operators
 *
 * These handle keyframes management from various spaces.
 * They only make use of Keying Sets.
 * \{ */

void ANIM_OT_keyframe_insert(wmOperatorType *ot);
void ANIM_OT_keyframe_delete(wmOperatorType *ot);
void ANIM_OT_keyframe_insert_by_name(wmOperatorType *ot);
void ANIM_OT_keyframe_delete_by_name(wmOperatorType *ot);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Main Keyframe Management operators
 *
 * These handle keyframes management from various spaces.
 * They will handle the menus required for each space.
 * \{ */

void ANIM_OT_keyframe_insert_menu(wmOperatorType *ot);

void ANIM_OT_keyframe_delete_v3d(wmOperatorType *ot);
void ANIM_OT_keyframe_delete_vse(wmOperatorType *ot);
void ANIM_OT_keyframe_clear_v3d(wmOperatorType *ot);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Keyframe management operators for UI buttons (RMB menu)
 * \{ */

void ANIM_OT_keyframe_insert_button(wmOperatorType *ot);
void ANIM_OT_keyframe_delete_button(wmOperatorType *ot);
void ANIM_OT_keyframe_clear_button(wmOperatorType *ot);

/** \} */

/* -------------------------------------------------------------------- */
/** \name KeyingSet management operators for UI buttons (RMB menu)
 * \{ */

void ANIM_OT_keyingset_button_add(wmOperatorType *ot);
void ANIM_OT_keyingset_button_remove(wmOperatorType *ot);

/** \} */

/* -------------------------------------------------------------------- */
/** \name KeyingSet management operators for RNA collections/UI buttons
 * \{ */

void ANIM_OT_keying_set_add(wmOperatorType *ot);
void ANIM_OT_keying_set_remove(wmOperatorType *ot);
void ANIM_OT_keying_set_path_add(wmOperatorType *ot);
void ANIM_OT_keying_set_path_remove(wmOperatorType *ot);

/** \} */

/* -------------------------------------------------------------------- */
/** \name KeyingSet general operators
 * \{ */

void ANIM_OT_keying_set_active_set(wmOperatorType *ot);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Driver management operators for UI buttons (RMB menu)
 * \{ */

void ANIM_OT_driver_button_add(wmOperatorType *ot);
void ANIM_OT_driver_button_remove(wmOperatorType *ot);
void ANIM_OT_driver_button_edit(wmOperatorType *ot);
void ANIM_OT_copy_driver_button(wmOperatorType *ot);
void ANIM_OT_paste_driver_button(wmOperatorType *ot);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Pose Asset operators
 * \{ */

namespace blender::ed::animrig {

void POSELIB_OT_create_pose_asset(wmOperatorType *ot);
void POSELIB_OT_asset_modify(wmOperatorType *ot);
void POSELIB_OT_asset_delete(wmOperatorType *ot);
void POSELIB_OT_screenshot_preview(wmOperatorType *ot);
}  // namespace blender::ed::animrig

/** \} */
