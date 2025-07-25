/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_math_vector_types.hh"
#include "BLI_offset_indices.hh"
#include "BLI_span.hh"
#include "BLI_string_ref.hh"
#include "BLI_virtual_array_fwd.hh"

/** \file
 * \ingroup bke
 * \brief support for deformation groups and hooks.
 */

struct BlendDataReader;
struct BlendWriter;
struct ID;
struct ListBase;
struct MDeformVert;
struct MDeformWeight;
struct Object;
struct bDeformGroup;

bool BKE_id_supports_vertex_groups(const ID *id);
bool BKE_object_supports_vertex_groups(const Object *ob);
const ListBase *BKE_object_defgroup_list(const Object *ob);
ListBase *BKE_object_defgroup_list_mutable(Object *ob);

int BKE_object_defgroup_count(const Object *ob);
/**
 * \note For historical reasons, the index starts at 1 rather than 0.
 */
int BKE_object_defgroup_active_index_get(const Object *ob);
/**
 * \note For historical reasons, the index starts at 1 rather than 0.
 */
void BKE_object_defgroup_active_index_set(Object *ob, int new_index);

/**
 * Return the ID's vertex group names.
 * Supports Mesh (ME), Lattice (LT), and GreasePencil (GD) IDs.
 * \return ListBase of bDeformGroup pointers.
 */
const ListBase *BKE_id_defgroup_list_get(const ID *id);
ListBase *BKE_id_defgroup_list_get_mutable(ID *id);
int BKE_defgroup_name_index(const ListBase *defbase, blender::StringRef name);
int BKE_id_defgroup_name_index(const ID *id, blender::StringRef name);
bool BKE_defgroup_listbase_name_find(const ListBase *defbase,
                                     blender::StringRef name,
                                     int *r_index,
                                     bDeformGroup **r_group);
bool BKE_id_defgroup_name_find(const ID *id,
                               blender::StringRef name,
                               int *r_index,
                               bDeformGroup **r_group);

bDeformGroup *BKE_object_defgroup_new(Object *ob, blender::StringRef name);
void BKE_defgroup_copy_list(ListBase *outbase, const ListBase *inbase);
bDeformGroup *BKE_defgroup_duplicate(const bDeformGroup *ingroup);
bDeformGroup *BKE_object_defgroup_find_name(const Object *ob, blender::StringRef name);
/**
 * Returns flip map for the vertex-groups of `ob`.
 *
 * \param use_default: How to handle cases where no symmetrical group is found.
 * - false: sets these indices to -1, indicating the group should be ignored.
 * - true: sets the index to its location in the array (making the group point to itself).
 *   Enable this for symmetrical actions which apply weight operations on symmetrical vertices
 *   where the symmetrical group will be used (if found), otherwise the same group is used.
 *
 * \return An index array `r_flip_map_num` length,
 * (aligned with the list result from `BKE_id_defgroup_list_get(ob)`).
 * referencing the index of the symmetrical vertex-group of a fall-back value (see `use_default`).
 * The caller is responsible for freeing the array.
 */
int *BKE_object_defgroup_flip_map(const Object *ob, bool use_default, int *r_flip_map_num);

/**
 * A version of #BKE_object_defgroup_flip_map that ignores locked groups.
 */
int *BKE_object_defgroup_flip_map_unlocked(const Object *ob,
                                           bool use_default,
                                           int *r_flip_map_num);
/**
 * A version of #BKE_object_defgroup_flip_map that only takes a single group into account.
 */
int *BKE_object_defgroup_flip_map_single(const Object *ob,
                                         bool use_default,
                                         int defgroup,
                                         int *r_flip_map_num);
int BKE_object_defgroup_flip_index(const Object *ob, int index, bool use_default);
int BKE_object_defgroup_name_index(const Object *ob, blender::StringRef name);
void BKE_object_defgroup_unique_name(bDeformGroup *dg, Object *ob);
void BKE_object_defgroup_set_name(bDeformGroup *dg, Object *ob, const char *new_name);

MDeformWeight *BKE_defvert_find_index(const MDeformVert *dv, int defgroup);
/**
 * Ensures that `dv` has a deform weight entry for the specified group (`defgroup`).
 *
 * \note this function is mirrored in editmesh_tools.cc, for use for edit-vertices.
 */
MDeformWeight *BKE_defvert_ensure_index(MDeformVert *dv, int defgroup);
/**
 * Adds the given vertex to the specified vertex group, with given weight.
 *
 * \warning this does NOT check for existing, assume caller already knows its not there.
 */
void BKE_defvert_add_index_notest(MDeformVert *dv, int defgroup, float weight);
/**
 * Removes the given vertex from the vertex group.
 *
 * \warning This function frees the given #MDeformWeight, do not use it afterward!
 */
void BKE_defvert_remove_group(MDeformVert *dvert, MDeformWeight *dw);
void BKE_defvert_clear(MDeformVert *dvert);
/**
 * \return The first group index shared by both deform verts
 * or -1 if none are found.
 */
int BKE_defvert_find_shared(const MDeformVert *dvert_a, const MDeformVert *dvert_b);
/**
 * \return true if has no weights.
 */
bool BKE_defvert_is_weight_zero(const MDeformVert *dvert, int defgroup_tot);

void BKE_defvert_array_free_elems(MDeformVert *dvert, int totvert);
void BKE_defvert_array_free(MDeformVert *dvert, int totvert);
void BKE_defvert_array_copy(MDeformVert *dst, const MDeformVert *src, int totvert);

float BKE_defvert_find_weight(const MDeformVert *dvert, int defgroup);
/**
 * Take care with this the rationale is:
 * - if the object has no vertex group. act like vertex group isn't set and return 1.0.
 * - if the vertex group exists but the 'defgroup' isn't found on this vertex, _still_ return 0.0.
 *
 * This is a bit confusing, just saves some checks from the caller.
 */
float BKE_defvert_array_find_weight_safe(const MDeformVert *dvert,
                                         int index,
                                         int defgroup,
                                         bool invert);

/**
 * \return The total weight in all groups marked in the selection mask.
 */
float BKE_defvert_total_selected_weight(const MDeformVert *dv,
                                        int defbase_num,
                                        const bool *defbase_sel);

/**
 * \return The representative weight of a multi-paint group, used for
 * viewport colors and actual painting.
 *
 * Result equal to sum of weights with auto normalize, and average otherwise.
 * Value is not clamped, since painting relies on multiplication being always
 * commutative with the collective weight function.
 */
float BKE_defvert_multipaint_collective_weight(const MDeformVert *dv,
                                               int defbase_num,
                                               const bool *defbase_sel,
                                               int defbase_sel_num,
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
                                       const MDeformVert *dv,
                                       int defbase_num,
                                       const bool *defbase_locked,
                                       const bool *defbase_unlocked);

void BKE_defvert_copy(MDeformVert *dvert_dst, const MDeformVert *dvert_src);
/**
 * Overwrite weights filtered by vgroup_subset.
 * - do nothing if neither are set.
 * - add destination weight if needed
 */
void BKE_defvert_copy_subset(MDeformVert *dvert_dst,
                             const MDeformVert *dvert_src,
                             const bool *vgroup_subset,
                             int vgroup_num);
/**
 * Overwrite weights filtered by vgroup_subset and with mirroring specified by the flip map
 * - do nothing if neither are set.
 * - add destination weight if needed
 */
void BKE_defvert_mirror_subset(MDeformVert *dvert_dst,
                               const MDeformVert *dvert_src,
                               const bool *vgroup_subset,
                               int vgroup_num,
                               const int *flip_map,
                               int flip_map_num);
/**
 * Copy an index from one #MDeformVert to another.
 * - do nothing if neither are set.
 * - add destination weight if needed.
 */
void BKE_defvert_copy_index(MDeformVert *dvert_dst,
                            int defgroup_dst,
                            const MDeformVert *dvert_src,
                            int defgroup_src);
/**
 * Only sync over matching weights, don't add or remove groups
 * warning, loop within loop.
 */
void BKE_defvert_sync(MDeformVert *dvert_dst, const MDeformVert *dvert_src, bool use_ensure);
/**
 * be sure all flip_map values are valid
 */
void BKE_defvert_sync_mapped(MDeformVert *dvert_dst,
                             const MDeformVert *dvert_src,
                             const int *flip_map,
                             int flip_map_num,
                             bool use_ensure);
/**
 * be sure all flip_map values are valid
 */
void BKE_defvert_remap(MDeformVert *dvert, const int *map, int map_len);
void BKE_defvert_flip(MDeformVert *dvert, const int *flip_map, int flip_map_num);
void BKE_defvert_flip_merged(MDeformVert *dvert, const int *flip_map, int flip_map_num);

/**
 * Normalize all the vertex group weights on a vertex.
 *
 * Note: this ignores whether groups are locked or not, and will therefore
 * happily modify even locked groups.
 *
 * See #BKE_defvert_normalize_ex() for parameter documentation.
 */
void BKE_defvert_normalize(MDeformVert &dvert);

/**
 * Normalize a subset of vertex group weights among themselves.
 *
 * Note: this ignores whether groups are locked or not, and will therefore
 * happily modify even locked groups.
 *
 * See #BKE_defvert_normalize_ex() for parameter documentation.
 */
void BKE_defvert_normalize_subset(MDeformVert &dvert, blender::Span<bool> subset_flags);

/**
 * Normalize a subset of vertex group weights among themselves, but leaving
 * locked groups unmodified.
 *
 * See #BKE_defvert_normalize_ex() for parameter documentation.
 */
void BKE_defvert_normalize_lock_map(MDeformVert &dvert,
                                    blender::Span<bool> subset_flags,
                                    blender::Span<bool> lock_flags);

/**
 * Normalize the vertex groups of a vertex, with all the bells and whistles.
 *
 * \param dvert: the vertex weights to be normalized.
 *
 * \param subset_flags: span of bools indicating which vertex groups are
 * included vs ignored in this function. True means included, false means
 * ignored. Note that this is different than locking: locked groups are not
 * *modified*, but their weights are still accounted for in the normalization
 * process, whereas ignored groups aren't accounted for at all. May be empty,
 * indicating all vertex groups are included. If not empty, its length must
 * match the number of vertex groups in the source data (e.g. the mesh).
 *
 * \param lock_flags: span of bools with `true` indicating the vertex groups
 * that are completely locked from modification, even if that prevents
 * normalization. May be empty, indicating no locked groups. If not empty, its
 * length must match the number of vertex groups in the source data (e.g. the
 * mesh).
 *
 * \param soft_lock_flags: span of bools with `true` indicating a set of vertex
 * groups that are "soft locked". The intended use case for this is to "protect"
 * weights that have just been set by a tool or operator during post-process
 * normalization. When possible, only non-soft-locked weights will be modified
 * to achieve normalization, but if necessary soft-locked will also be modified.
 * NOTE: in theory this could be used for purposes other than "just set" groups,
 * but corner cases are handled with that use case in mind. May be empty,
 * indicating no "soft locked" groups. If not empty, its length must match the
 * number of vertex groups in the source data (e.g. the mesh).
 */
void BKE_defvert_normalize_ex(MDeformVert &dvert,
                              blender::Span<bool> vgroup_subset,
                              blender::Span<bool> lock_flags,
                              blender::Span<bool> soft_lock_flags);

/* Utilities to 'extract' a given vgroup into a simple float array,
 * for verts, but also edges/faces/loops. */

void BKE_defvert_extract_vgroup_to_vertweights(
    const MDeformVert *dvert, int defgroup, int verts_num, bool invert_vgroup, float *r_weights);

/**
 * The following three make basic interpolation,
 * using temp vert_weights array to avoid looking up same weight several times.
 */
void BKE_defvert_extract_vgroup_to_edgeweights(const MDeformVert *dvert,
                                               int defgroup,
                                               int verts_num,
                                               blender::Span<blender::int2> edges,
                                               bool invert_vgroup,
                                               float *r_weights);
void BKE_defvert_extract_vgroup_to_loopweights(const MDeformVert *dvert,
                                               int defgroup,
                                               int verts_num,
                                               blender::Span<int> corner_verts,
                                               bool invert_vgroup,
                                               float *r_weights);

void BKE_defvert_extract_vgroup_to_faceweights(const MDeformVert *dvert,
                                               int defgroup,
                                               int verts_num,
                                               const blender::Span<int> corner_verts,
                                               blender::OffsetIndices<int> faces,
                                               bool invert_vgroup,
                                               float *r_weights);

void BKE_defvert_weight_to_rgb(float r_rgb[3], float weight);

void BKE_defvert_blend_write(BlendWriter *writer, int count, const MDeformVert *dvlist);
void BKE_defvert_blend_read(BlendDataReader *reader, int count, MDeformVert *mdverts);
void BKE_defbase_blend_write(BlendWriter *writer, const ListBase *defbase);

namespace blender::bke {

VArray<float> varray_for_deform_verts(Span<MDeformVert> dverts, int defgroup_index);
VMutableArray<float> varray_for_mutable_deform_verts(MutableSpan<MDeformVert> dverts,
                                                     int defgroup_index);
void remove_defgroup_index(MutableSpan<MDeformVert> dverts, int defgroup_index);

void gather_deform_verts(Span<MDeformVert> src, Span<int> indices, MutableSpan<MDeformVert> dst);
void gather_deform_verts(Span<MDeformVert> src,
                         const IndexMask &indices,
                         MutableSpan<MDeformVert> dst);

}  // namespace blender::bke
