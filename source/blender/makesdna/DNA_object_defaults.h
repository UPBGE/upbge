/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup DNA
 */

#pragma once

#include "DNA_object_force_types.h"  // UPBGE
#include "DNA_vec_defaults.h"

/* clang-format off */

/* -------------------------------------------------------------------- */
/** \name Object Struct
 * \{ */

#define _DNA_DEFAULT_Object \
  { \
    /* Type is not very meaningful as a default, normally changed. */ \
    .type = OB_EMPTY, \
    .color = {1, 1, 1, 1}, \
 \
    .constinv = _DNA_DEFAULT_UNIT_M4, \
    .parentinv = _DNA_DEFAULT_UNIT_M4, \
 \
    .scale = {1, 1, 1}, \
    .dscale = {1, 1, 1}, \
    /* Objects should default to having Euler XYZ rotations, \
     * but rotations default to quaternions. */ \
    .rotmode = ROT_MODE_EUL, \
    /** See #unit_axis_angle. */ \
    .rotAxis = {0, 1, 0}, \
    .rotAngle = 0, \
    .drotAxis = {0, 1, 0}, \
    .drotAngle = 0, \
    .quat = _DNA_DEFAULT_UNIT_QT, \
    .dquat = _DNA_DEFAULT_UNIT_QT, \
    .flag = OB_FLAG_USE_SIMULATION_CACHE, \
    .protectflag = OB_LOCK_ROT4D, \
 \
    .dt = OB_TEXTURE, \
 \
    .empty_drawtype = OB_PLAINAXES, \
    .empty_drawsize = 1.0, \
    .empty_image_depth = OB_EMPTY_IMAGE_DEPTH_DEFAULT, \
    .ima_ofs = {-0.5, -0.5}, \
 \
    .instance_faces_scale = 1, \
    .col_group = 0x01,  \
    .col_mask = 0xffff, \
    .preview = NULL, \
    .duplicator_visibility_flag = OB_DUPLI_FLAG_VIEWPORT | OB_DUPLI_FLAG_RENDER, \
    .pc_ids = {NULL, NULL}, \
    .lineart = { .crease_threshold = DEG2RAD(140.0f) }, \
 \
    .mass = 1.0f, \
    .inertia = 1.0f, \
    .formfactor = 0.4f, \
    .damping = 0.04f, \
    .rdamping = 0.1f, \
    .anisotropicFriction = {1.0f, 1.0f, 1.0f}, \
    .gameflag = OB_PROP | OB_COLLISION, \
    .gameflag2 = 0, \
    .margin = 0.04f, \
    .friction = 0.5f, \
    .init_state = 1, \
    .state = 1, \
    .obstacleRad = 1.0f, \
    .step_height = 0.15f, \
    .jump_speed = 10.0f, \
    .fall_speed = 55.0f, \
    .max_jumps = 1, \
    .max_slope = M_PI_2, \
    .col_group = 0x01, \
    .col_mask = 0xffff, \
    .ccd_motion_threshold = 1.0f, \
    .ccd_swept_sphere_radius = 0.9f, \
    .lodfactor = 1.0f, \
  }

#define _DNA_DEFAULT_BulletSoftBody \
  { \
    .flag = OB_BSB_BENDING_CONSTRAINTS | OB_BSB_SHAPE_MATCHING | OB_BSB_AERO_VPOINT, \
    .linStiff = 0.5f, \
    .angStiff = 1.0f, \
    .volume = 1.0f, \
 \
    .viterations = 0, \
    .piterations = 2, \
    .diterations = 0, \
    .citerations = 4, \
 \
    .kSRHR_CL = 0.1f, \
    .kSKHR_CL = 1.f, \
    .kSSHR_CL = 0.5f, \
    .kSR_SPLT_CL = 0.5f, \
 \
    .kSK_SPLT_CL = 0.5f, \
    .kSS_SPLT_CL = 0.5f, \
    .kVCF = 1, \
    .kDP = 0, \
 \
    .kDG = 0, \
    .kLF = 0, \
    .kPR = 0, \
    .kVC = 0, \
 \
    .kDF = 0.2f, \
    .kMT = 0.05, \
    .kCHR = 1.0f, \
    .kKHR = 0.1f, \
 \
    .kSHR = 1.0f, \
    .kAHR = 0.7f, \
 \
    .collisionflags = OB_BSB_COL_CL_RS, \
    .numclusteriterations = 64, \
    .bending_dist = 2, \
    .welding = 0.f, \
  }

/** \} */

/* clang-format on */
