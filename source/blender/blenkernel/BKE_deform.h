/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2001-2002 NaN Holding BV. All rights reserved. */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** \file
 * \ingroup bke
 * \brief support for deformation groups and hooks.
 */

struct BlendDataReader;
struct BlendWriter;
struct ID;
struct ListBase;
struct MDeformVert;
struct MEdge;
struct MLoop;
struct MPoly;
struct Object;
struct bDeformGroup;

bool BKE_object_supports_vertex_groups(const struct Object *ob);
const struct ListBase *BKE_object_defgroup_list(const struct Object *ob);
struct ListBase *BKE_object_defgroup_list_mutable(struct Object *ob);

int BKE_object_defgroup_count(const struct Object *ob);
/**
 * \note For historical reasons, the index starts at 1 rather than 0.
 */
int BKE_object_defgroup_active_index_get(const struct Object *ob);
/**
 * \note For historical reasons, the index starts at 1 rather than 0.
 */
void BKE_object_defgroup_active_index_set(struct Object *ob, int new_index);

const struct ListBase *BKE_id_defgroup_list_get(const struct ID *id);
struct ListBase *BKE_id_defgroup_list_get_mutable(struct ID *id);
int BKE_id_defgroup_name_index(const struct ID *id, const char *name);
bool BKE_id_defgroup_name_find(const struct ID *id,
                               const char *name,
                               int *r_index,
                               struct bDeformGroup **r_group);

struct bDeformGroup *BKE_object_defgroup_new(struct Object *ob, const char *name);
void BKE_defgroup_copy_list(struct ListBase *outbase, const struct ListBase *inbase);
struct bDeformGroup *BKE_defgroup_duplicate(const struct bDeformGroup *ingroup);
struct bDeformGroup *BKE_object_defgroup_find_name(const struct Object *ob, const char *name);
/**
 * \note caller must free.
 */
int *BKE_object_defgroup_flip_map(const struct Object *ob, int *flip_map_len, bool use_default);
/**
 * \note caller must free.
 */
int *BKE_object_defgroup_flip_map_single(const struct Object *ob,
                                         int *flip_map_len,
                                         bool use_default,
                                         int defgroup);
int BKE_object_defgroup_flip_index(const struct Object *ob, int index, bool use_default);
int BKE_object_defgroup_name_index(const struct Object *ob, const char *name);
void BKE_object_defgroup_unique_name(struct bDeformGroup *dg, struct Object *ob);

struct MDeformWeight *BKE_defvert_find_index(const struct MDeformVert *dv, int defgroup);
/**
 * Ensures that `dv` has a deform weight entry for the specified defweight group.
 *
 * \note this function is mirrored in editmesh_tools.c, for use for edit-vertices.
 */
struct MDeformWeight *BKE_defvert_ensure_index(struct MDeformVert *dv, int defgroup);
/**
 * Adds the given vertex to the specified vertex group, with given weight.
 *
 * \warning this does NOT check for existing, assume caller already knows its not there.
 */
void BKE_defvert_add_index_notest(struct MDeformVert *dv, int defgroup, float weight);
/**
 * Removes the given vertex from the vertex group.
 *
 * \warning This function frees the given #MDeformWeight, do not use it afterward!
 */
void BKE_defvert_remove_group(struct MDeformVert *dvert, struct MDeformWeight *dw);
void BKE_defvert_clear(struct MDeformVert *dvert);
/**
 * \return The first group index shared by both deform verts
 * or -1 if none are found.
 */
int BKE_defvert_find_shared(const struct MDeformVert *dvert_a, const struct MDeformVert *dvert_b);
/**
 * \return true if has no weights.
 */
bool BKE_defvert_is_weight_zero(const struct MDeformVert *dvert, int defgroup_tot);

void BKE_defvert_array_free_elems(struct MDeformVert *dvert, int totvert);
void BKE_defvert_array_free(struct MDeformVert *dvert, int totvert);
void BKE_defvert_array_copy(struct MDeformVert *dst, const struct MDeformVert *src, int totvert);

float BKE_defvert_find_weight(const struct MDeformVert *dvert, int defgroup);
/**
 * Take care with this the rationale is:
 * - if the object has no vertex group. act like vertex group isn't set and return 1.0.
 * - if the vertex group exists but the 'defgroup' isn't found on this vertex, _still_ return 0.0.
 *
 * This is a bit confusing, just saves some checks from the caller.
 */
float BKE_defvert_array_find_weight_safe(const struct MDeformVert *dvert, int index, int defgroup);

/**
 * \return The total weight in all groups marked in the selection mask.
 */
float BKE_defvert_total_selected_weight(const struct MDeformVert *dv,
                                        int defbase_tot,
                                        const bool *defbase_sel);

/**
 * \return The representative weight of a multi-paint group, used for
 * viewport colors and actual painting.
 *
 * Result equal to sum of weights with auto normalize, and average otherwise.
 * Value is not clamped, since painting relies on multiplication being always
 * commutative with the collective weight function.
 */
float BKE_defvert_multipaint_collective_weight(const struct MDeformVert *dv,
                                               int defbase_tot,
                                               const bool *defbase_sel,
                                               int defbase_tot_sel,
                                               bool is_normalized);

/* This much unlocked weight is considered equivalent to none. */
#define VERTEX_WEIGHT_LOCK_EPSILON 1e-6f

/**
 * Computes the display weight for the lock relative weight paint mode.
 *
 * \return weight divided by 1-locked_weight with division by zero check
 */
float BKE_defvert_calc_lock_relative_weight(float weight,
                                            float locked_weight,
                                            float unlocked_weight);
/**
 * Computes the display weight for the lock relative weight paint mode, using weight data.
 *
 * \return weight divided by unlocked, or 1-locked_weight with division by zero check.
 */
float BKE_defvert_lock_relative_weight(float weight,
                                       const struct MDeformVert *dv,
                                       int defbase_tot,
                                       const bool *defbase_locked,
                                       const bool *defbase_unlocked);

void BKE_defvert_copy(struct MDeformVert *dvert_dst, const struct MDeformVert *dvert_src);
/**
 * Overwrite weights filtered by vgroup_subset.
 * - do nothing if neither are set.
 * - add destination weight if needed
 */
void BKE_defvert_copy_subset(struct MDeformVert *dvert_dst,
                             const struct MDeformVert *dvert_src,
                             const bool *vgroup_subset,
                             int vgroup_tot);
/**
 * Overwrite weights filtered by vgroup_subset and with mirroring specified by the flip map
 * - do nothing if neither are set.
 * - add destination weight if needed
 */
void BKE_defvert_mirror_subset(struct MDeformVert *dvert_dst,
                               const struct MDeformVert *dvert_src,
                               const bool *vgroup_subset,
                               int vgroup_tot,
                               const int *flip_map,
                               int flip_map_len);
/**
 * Copy an index from one #MDeformVert to another.
 * - do nothing if neither are set.
 * - add destination weight if needed.
 */
void BKE_defvert_copy_index(struct MDeformVert *dvert_dst,
                            int defgroup_dst,
                            const struct MDeformVert *dvert_src,
                            int defgroup_src);
/**
 * Only sync over matching weights, don't add or remove groups
 * warning, loop within loop.
 */
void BKE_defvert_sync(struct MDeformVert *dvert_dst,
                      const struct MDeformVert *dvert_src,
                      bool use_ensure);
/**
 * be sure all flip_map values are valid
 */
void BKE_defvert_sync_mapped(struct MDeformVert *dvert_dst,
                             const struct MDeformVert *dvert_src,
                             const int *flip_map,
                             int flip_map_len,
                             bool use_ensure);
/**
 * be sure all flip_map values are valid
 */
void BKE_defvert_remap(struct MDeformVert *dvert, const int *map, int map_len);
void BKE_defvert_flip(struct MDeformVert *dvert, const int *flip_map, int flip_map_len);
void BKE_defvert_flip_merged(struct MDeformVert *dvert, const int *flip_map, int flip_map_len);
void BKE_defvert_normalize(struct MDeformVert *dvert);
/**
 * Same as #BKE_defvert_normalize but takes a bool array.
 */
void BKE_defvert_normalize_subset(struct MDeformVert *dvert,
                                  const bool *vgroup_subset,
                                  int vgroup_tot);
/**
 * Same as BKE_defvert_normalize() if the locked vgroup is not a member of the subset
 */
void BKE_defvert_normalize_lock_single(struct MDeformVert *dvert,
                                       const bool *vgroup_subset,
                                       int vgroup_tot,
                                       uint def_nr_lock);
/**
 * Same as BKE_defvert_normalize() if no locked vgroup is a member of the subset
 */
void BKE_defvert_normalize_lock_map(struct MDeformVert *dvert,
                                    const bool *vgroup_subset,
                                    int vgroup_tot,
                                    const bool *lock_flags,
                                    int defbase_tot);

/* Utilities to 'extract' a given vgroup into a simple float array,
 * for verts, but also edges/polys/loops. */

void BKE_defvert_extract_vgroup_to_vertweights(const struct MDeformVert *dvert,
                                               int defgroup,
                                               int num_verts,
                                               bool invert_vgroup,
                                               float *r_weights);
/**
 * The following three make basic interpolation,
 * using temp vert_weights array to avoid looking up same weight several times.
 */
void BKE_defvert_extract_vgroup_to_edgeweights(const struct MDeformVert *dvert,
                                               int defgroup,
                                               int num_verts,
                                               struct MEdge *edges,
                                               int num_edges,
                                               bool invert_vgroup,
                                               float *r_weights);
void BKE_defvert_extract_vgroup_to_loopweights(const struct MDeformVert *dvert,
                                               int defgroup,
                                               int num_verts,
                                               struct MLoop *loops,
                                               int num_loops,
                                               bool invert_vgroup,
                                               float *r_weights);
void BKE_defvert_extract_vgroup_to_polyweights(const struct MDeformVert *dvert,
                                               int defgroup,
                                               int num_verts,
                                               struct MLoop *loops,
                                               int num_loops,
                                               struct MPoly *polys,
                                               int num_polys,
                                               bool invert_vgroup,
                                               float *r_weights);

void BKE_defvert_weight_to_rgb(float r_rgb[3], float weight);

void BKE_defvert_blend_write(struct BlendWriter *writer,
                             int count,
                             const struct MDeformVert *dvlist);
void BKE_defvert_blend_read(struct BlendDataReader *reader,
                            int count,
                            struct MDeformVert *mdverts);
void BKE_defbase_blend_write(struct BlendWriter *writer, const ListBase *defbase);

#ifdef __cplusplus
}
#endif
