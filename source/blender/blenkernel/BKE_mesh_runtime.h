/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2001-2002 NaN Holding BV. All rights reserved. */
#pragma once

/** \file
 * \ingroup bke
 *
 * This file contains access functions for the Mesh.runtime struct.
 */

//#include "BKE_customdata.h"  /* for eCustomDataMask */

#ifdef __cplusplus
extern "C" {
#endif

struct CustomData;
struct CustomData_MeshMasks;
struct Depsgraph;
struct KeyBlock;
struct MLoop;
struct MLoopTri;
struct MVertTri;
struct Mesh;
struct Object;
struct Scene;

/**
 * \brief Initialize the runtime of the given mesh.
 *
 * Function expects that the runtime is already cleared.
 */
void BKE_mesh_runtime_init_data(struct Mesh *mesh);
/**
 * \brief Free all data (and mutexes) inside the runtime of the given mesh.
 */
void BKE_mesh_runtime_free_data(struct Mesh *mesh);
/**
 * Clear all pointers which we don't want to be shared on copying the datablock.
 * However, keep all the flags which defines what the mesh is (for example, that
 * it's deformed only, or that its custom data layers are out of date.)
 */
void BKE_mesh_runtime_reset_on_copy(struct Mesh *mesh, int flag);
int BKE_mesh_runtime_looptri_len(const struct Mesh *mesh);
void BKE_mesh_runtime_looptri_recalc(struct Mesh *mesh);
/**
 * \note This function only fills a cache, and therefore the mesh argument can
 * be considered logically const. Concurrent access is protected by a mutex.
 * \note This is a ported copy of dm_getLoopTriArray(dm).
 */
const struct MLoopTri *BKE_mesh_runtime_looptri_ensure(const struct Mesh *mesh);
bool BKE_mesh_runtime_ensure_edit_data(struct Mesh *mesh);
bool BKE_mesh_runtime_clear_edit_data(struct Mesh *mesh);
bool BKE_mesh_runtime_reset_edit_data(struct Mesh *mesh);
void BKE_mesh_runtime_clear_geometry(struct Mesh *mesh);
/**
 * \brief This function clears runtime cache of the given mesh.
 *
 * Call this function to recalculate runtime data when used.
 */
void BKE_mesh_runtime_clear_cache(struct Mesh *mesh);

/* This is a copy of DM_verttri_from_looptri(). */
void BKE_mesh_runtime_verttri_from_looptri(struct MVertTri *r_verttri,
                                           const struct MLoop *mloop,
                                           const struct MLoopTri *looptri,
                                           int looptri_num);

/* NOTE: the functions below are defined in DerivedMesh.cc, and are intended to be moved
 * to a more suitable location when that file is removed.
 * They should also be renamed to use conventions from BKE, not old DerivedMesh.cc.
 * For now keep the names similar to avoid confusion. */

/* UPBGE */
struct DerivedMesh *mesh_get_derived_final(struct Depsgraph *depsgraph,
                                           struct Scene *scene,
                                           struct Object *ob,
                                           const struct CustomData_MeshMasks *dataMask);
/* End of  UPBGE */

struct Mesh *mesh_get_eval_final(struct Depsgraph *depsgraph,
                                 const struct Scene *scene,
                                 struct Object *ob,
                                 const struct CustomData_MeshMasks *dataMask);

struct Mesh *mesh_get_eval_deform(struct Depsgraph *depsgraph,
                                  const struct Scene *scene,
                                  struct Object *ob,
                                  const struct CustomData_MeshMasks *dataMask);

struct Mesh *mesh_create_eval_final(struct Depsgraph *depsgraph,
                                    const struct Scene *scene,
                                    struct Object *ob,
                                    const struct CustomData_MeshMasks *dataMask);

struct Mesh *mesh_create_eval_no_deform(struct Depsgraph *depsgraph,
                                        const struct Scene *scene,
                                        struct Object *ob,
                                        const struct CustomData_MeshMasks *dataMask);
struct Mesh *mesh_create_eval_no_deform_render(struct Depsgraph *depsgraph,
                                               const struct Scene *scene,
                                               struct Object *ob,
                                               const struct CustomData_MeshMasks *dataMask);

void BKE_mesh_runtime_eval_to_meshkey(struct Mesh *me_deformed,
                                      struct Mesh *me,
                                      struct KeyBlock *kb);

#ifndef NDEBUG
bool BKE_mesh_runtime_is_valid(struct Mesh *me_eval);
#endif /* NDEBUG */

#ifdef __cplusplus
}
#endif
