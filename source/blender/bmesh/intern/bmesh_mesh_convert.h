/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2004 Blender Foundation. All rights reserved. */

#pragma once

/** \file
 * \ingroup bmesh
 */

#include "bmesh.h"

struct CustomData_MeshMasks;
struct Main;
struct Mesh;

void BM_mesh_cd_flag_ensure(BMesh *bm, struct Mesh *mesh, char cd_flag);
void BM_mesh_cd_flag_apply(BMesh *bm, char cd_flag);
char BM_mesh_cd_flag_from_bmesh(BMesh *bm);

struct BMeshFromMeshParams {
  bool calc_face_normal;
  bool calc_vert_normal;
  /* add a vertex CD_SHAPE_KEYINDEX layer */
  bool add_key_index;
  /* set vertex coordinates from the shapekey */
  bool use_shapekey;
  /* define the active shape key (index + 1) */
  int active_shapekey;
  struct CustomData_MeshMasks cd_mask_extra;
};
/**
 * \brief Mesh -> BMesh
 * \param bm: The mesh to write into, while this is typically a newly created BMesh,
 * merging into existing data is supported.
 * Note the custom-data layout isn't used.
 * If more comprehensive merging is needed we should move this into a separate function
 * since this should be kept fast for edit-mode switching and storing undo steps.
 *
 * \warning This function doesn't calculate face normals.
 */
void BM_mesh_bm_from_me(BMesh *bm, const struct Mesh *me, const struct BMeshFromMeshParams *params)
    ATTR_NONNULL(1, 3);

struct BMeshToMeshParams {
  /** Update object hook indices & vertex parents. */
  bool calc_object_remap;
  /**
   * This re-assigns shape-key indices. Only do if the BMesh will have continued use
   * to update the mesh & shape key in the future.
   * In the case the BMesh is freed immediately, this can be left false.
   *
   * This is needed when flushing changes from edit-mode into object mode,
   * so a second flush or edit-mode exit doesn't run with indices
   * that have become invalid from updating the shape-key, see T71865.
   */
  bool update_shapekey_indices;
  /**
   * Instead of copying the basis shape-key into the #MVert array,
   * copy the #BMVert.co directly to #MVert.co (used for reading undo data).
   */
  bool active_shapekey_to_mvert;
  struct CustomData_MeshMasks cd_mask_extra;
};

/**
 * \param bmain: May be NULL in case \a calc_object_remap parameter option is not set.
 */
void BM_mesh_bm_to_me(struct Main *bmain,
                      BMesh *bm,
                      struct Mesh *me,
                      const struct BMeshToMeshParams *params) ATTR_NONNULL(2, 3, 4);

/**
 * A version of #BM_mesh_bm_to_me intended for getting the mesh
 * to pass to the modifier stack for evaluation,
 * instead of mode switching (where we make sure all data is kept
 * and do expensive lookups to maintain shape keys).
 *
 * Key differences:
 *
 * - Don't support merging with existing mesh.
 * - Ignore shape-keys.
 * - Ignore vertex-parents.
 * - Ignore selection history.
 * - Uses simpler method to calculate #ME_EDGEDRAW
 * - Uses #CD_MASK_DERIVEDMESH instead of #CD_MASK_MESH.
 *
 * \note Was `cddm_from_bmesh_ex` in 2.7x, removed `MFace` support.
 */
void BM_mesh_bm_to_me_for_eval(BMesh *bm,
                               struct Mesh *me,
                               const struct CustomData_MeshMasks *cd_mask_extra)
    ATTR_NONNULL(1, 2);
