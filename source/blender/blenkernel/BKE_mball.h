/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2001-2002 NaN Holding BV. All rights reserved. */
#pragma once

/** \file
 * \ingroup bke
 */

#ifdef __cplusplus
extern "C" {
#endif

struct Base;
struct BoundBox;
struct Depsgraph;
struct Main;
struct MetaBall;
struct MetaElem;
struct Object;
struct Scene;

struct MetaBall *BKE_mball_add(struct Main *bmain, const char *name);

bool BKE_mball_is_any_selected(const struct MetaBall *mb);
bool BKE_mball_is_any_selected_multi(struct Base **bases, int bases_len);
bool BKE_mball_is_any_unselected(const struct MetaBall *mb);

/**
 * Return `true` if `ob1` and `ob2` are part of the same metaBall group.
 *
 * \note Currently checks whether their two base names (without numerical suffix) is the same.
 */
bool BKE_mball_is_same_group(const struct Object *ob1, const struct Object *ob2);
/**
 * Return `true` if `ob1` and `ob2` are part of the same metaBall group, and `ob1` is its
 * basis.
 */
bool BKE_mball_is_basis_for(const struct Object *ob1, const struct Object *ob2);
/**
 * Test, if \a ob is a basis meta-ball.
 *
 * It test last character of Object ID name.
 * If last character is digit it return 0, else it return 1.
 */
bool BKE_mball_is_basis(const struct Object *ob);
/**
 * This function finds the basis meta-ball.
 *
 * Basis meta-ball doesn't include any number at the end of
 * its name. All meta-balls with same base of name can be
 * blended. meta-balls with different basic name can't be blended.
 *
 * \warning #BKE_mball_is_basis() can fail on returned object, see function docs for details.
 */
struct Object *BKE_mball_basis_find(struct Scene *scene, struct Object *ob);

/**
 * Return or compute bounding-box for given meta-ball object.
 */
struct BoundBox *BKE_mball_boundbox_get(struct Object *ob);

/**
 * Copy some properties from a meta-ball obdata to all other meta-ball obdata belonging to the same
 * family (i.e. object sharing the same name basis).
 *
 * When some properties (wire-size, threshold, update flags) of meta-ball are changed, then this
 * properties are copied to all meta-balls in same "group" (meta-balls with same base name:
 * `MBall`, `MBall.001`, `MBall.002`, etc). The most important is to copy properties to the base
 * meta-ball, because this meta-ball influences polygonization of meta-balls.
 */
void BKE_mball_properties_copy(struct Main *bmain, struct MetaBall *active_metaball);

bool BKE_mball_minmax_ex(
    const struct MetaBall *mb, float min[3], float max[3], const float obmat[4][4], short flag);

/* Basic vertex data functions. */

bool BKE_mball_minmax(const struct MetaBall *mb, float min[3], float max[3]);
bool BKE_mball_center_median(const struct MetaBall *mb, float r_cent[3]);
bool BKE_mball_center_bounds(const struct MetaBall *mb, float r_cent[3]);
void BKE_mball_transform(struct MetaBall *mb, const float mat[4][4], bool do_props);
void BKE_mball_translate(struct MetaBall *mb, const float offset[3]);

/**
 * Most simple meta-element adding function.
 *
 * \note don't do context manipulation here (rna uses).
 */
struct MetaElem *BKE_mball_element_add(struct MetaBall *mb, int type);

/* *** select funcs *** */

int BKE_mball_select_count(const struct MetaBall *mb);
int BKE_mball_select_count_multi(struct Base **bases, int bases_len);
bool BKE_mball_select_all(struct MetaBall *mb);
bool BKE_mball_select_all_multi_ex(struct Base **bases, int bases_len);
bool BKE_mball_deselect_all(struct MetaBall *mb);
bool BKE_mball_deselect_all_multi_ex(struct Base **bases, int bases_len);
bool BKE_mball_select_swap(struct MetaBall *mb);
bool BKE_mball_select_swap_multi_ex(struct Base **bases, int bases_len);

/* **** Depsgraph evaluation **** */

void BKE_mball_data_update(struct Depsgraph *depsgraph, struct Scene *scene, struct Object *ob);

#ifdef __cplusplus
}
#endif
