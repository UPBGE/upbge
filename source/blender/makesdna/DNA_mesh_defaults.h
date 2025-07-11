/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup DNA
 */

#pragma once

/* clang-format off */

/* -------------------------------------------------------------------- */
/** \name Mesh Struct
 * \{ */

#define _DNA_DEFAULT_Mesh \
  { \
    .texspace_size = {1.0f, 1.0f, 1.0f}, \
    .texspace_flag = ME_TEXSPACE_FLAG_AUTO, \
    .remesh_voxel_size = 0.1f, \
    .remesh_voxel_adaptivity = 0.0f, \
    .face_sets_color_seed = 0, \
    .face_sets_color_default = 1, \
    .flag = ME_REMESH_REPROJECT_VOLUME | ME_REMESH_REPROJECT_ATTRIBUTES, \
    .editflag = ME_EDIT_MIRROR_VERTEX_GROUPS, \
    .radial_symmetry = {1, 1, 1} \
  }

/** \} */

/* clang-format on */
