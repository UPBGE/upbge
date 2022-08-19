/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2001-2002 NaN Holding BV. All rights reserved. */

/** \file
 * \ingroup bke
 */

#include <ctype.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "MEM_guardedalloc.h"

#include "DNA_gpencil_types.h"
#include "DNA_lattice_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BLI_listbase.h"
#include "BLI_math.h"
#include "BLI_string.h"
#include "BLI_string_utils.h"
#include "BLI_utildefines.h"

#include "BLT_translation.h"

#include "BKE_customdata.h"
#include "BKE_data_transfer.h"
#include "BKE_deform.h" /* own include */
#include "BKE_mesh.h"
#include "BKE_mesh_mapping.h"
#include "BKE_object.h"
#include "BKE_object_deform.h"

#include "BLO_read_write.h"

#include "data_transfer_intern.h"

bDeformGroup *BKE_object_defgroup_new(Object *ob, const char *name)
{
  bDeformGroup *defgroup;

  BLI_assert(OB_TYPE_SUPPORT_VGROUP(ob->type));

  defgroup = MEM_callocN(sizeof(bDeformGroup), __func__);

  BLI_strncpy(defgroup->name, name, sizeof(defgroup->name));

  ListBase *defbase = BKE_object_defgroup_list_mutable(ob);

  BLI_addtail(defbase, defgroup);
  BKE_object_defgroup_unique_name(defgroup, ob);

  BKE_object_batch_cache_dirty_tag(ob);

  return defgroup;
}

void BKE_defgroup_copy_list(ListBase *outbase, const ListBase *inbase)
{
  bDeformGroup *defgroup, *defgroupn;

  BLI_listbase_clear(outbase);

  for (defgroup = inbase->first; defgroup; defgroup = defgroup->next) {
    defgroupn = BKE_defgroup_duplicate(defgroup);
    BLI_addtail(outbase, defgroupn);
  }
}

bDeformGroup *BKE_defgroup_duplicate(const bDeformGroup *ingroup)
{
  bDeformGroup *outgroup;

  if (!ingroup) {
    BLI_assert(0);
    return NULL;
  }

  outgroup = MEM_callocN(sizeof(bDeformGroup), "copy deformGroup");

  /* For now, just copy everything over. */
  memcpy(outgroup, ingroup, sizeof(bDeformGroup));

  outgroup->next = outgroup->prev = NULL;

  return outgroup;
}

void BKE_defvert_copy_subset(MDeformVert *dvert_dst,
                             const MDeformVert *dvert_src,
                             const bool *vgroup_subset,
                             const int vgroup_tot)
{
  int defgroup;
  for (defgroup = 0; defgroup < vgroup_tot; defgroup++) {
    if (vgroup_subset[defgroup]) {
      BKE_defvert_copy_index(dvert_dst, defgroup, dvert_src, defgroup);
    }
  }
}

void BKE_defvert_mirror_subset(MDeformVert *dvert_dst,
                               const MDeformVert *dvert_src,
                               const bool *vgroup_subset,
                               const int vgroup_tot,
                               const int *flip_map,
                               const int flip_map_len)
{
  int defgroup;
  for (defgroup = 0; defgroup < vgroup_tot && defgroup < flip_map_len; defgroup++) {
    if (vgroup_subset[defgroup] && (dvert_dst != dvert_src || flip_map[defgroup] != defgroup)) {
      BKE_defvert_copy_index(dvert_dst, flip_map[defgroup], dvert_src, defgroup);
    }
  }
}

void BKE_defvert_copy(MDeformVert *dvert_dst, const MDeformVert *dvert_src)
{
  if (dvert_dst->totweight == dvert_src->totweight) {
    if (dvert_src->totweight) {
      memcpy(dvert_dst->dw, dvert_src->dw, dvert_src->totweight * sizeof(MDeformWeight));
    }
  }
  else {
    if (dvert_dst->dw) {
      MEM_freeN(dvert_dst->dw);
    }

    if (dvert_src->totweight) {
      dvert_dst->dw = MEM_dupallocN(dvert_src->dw);
    }
    else {
      dvert_dst->dw = NULL;
    }

    dvert_dst->totweight = dvert_src->totweight;
  }
}

void BKE_defvert_copy_index(MDeformVert *dvert_dst,
                            const int defgroup_dst,
                            const MDeformVert *dvert_src,
                            const int defgroup_src)
{
  MDeformWeight *dw_dst;

  const MDeformWeight *dw_src = BKE_defvert_find_index(dvert_src, defgroup_src);

  if (dw_src) {
    /* Source is valid, ensure destination is created. */
    dw_dst = BKE_defvert_ensure_index(dvert_dst, defgroup_dst);
    dw_dst->weight = dw_src->weight;
  }
  else {
    /* Source was NULL, assign zero (could also remove). */
    dw_dst = BKE_defvert_find_index(dvert_dst, defgroup_dst);

    if (dw_dst) {
      dw_dst->weight = 0.0f;
    }
  }
}

void BKE_defvert_sync(MDeformVert *dvert_dst, const MDeformVert *dvert_src, const bool use_ensure)
{
  if (dvert_src->totweight && dvert_dst->totweight) {
    MDeformWeight *dw_src = dvert_src->dw;
    for (int i = 0; i < dvert_src->totweight; i++, dw_src++) {
      MDeformWeight *dw_dst;
      if (use_ensure) {
        dw_dst = BKE_defvert_ensure_index(dvert_dst, dw_src->def_nr);
      }
      else {
        dw_dst = BKE_defvert_find_index(dvert_dst, dw_src->def_nr);
      }

      if (dw_dst) {
        dw_dst->weight = dw_src->weight;
      }
    }
  }
}

void BKE_defvert_sync_mapped(MDeformVert *dvert_dst,
                             const MDeformVert *dvert_src,
                             const int *flip_map,
                             const int flip_map_len,
                             const bool use_ensure)
{
  if (dvert_src->totweight && dvert_dst->totweight) {
    MDeformWeight *dw_src = dvert_src->dw;
    for (int i = 0; i < dvert_src->totweight; i++, dw_src++) {
      if (dw_src->def_nr < flip_map_len) {
        MDeformWeight *dw_dst;
        if (use_ensure) {
          dw_dst = BKE_defvert_ensure_index(dvert_dst, flip_map[dw_src->def_nr]);
        }
        else {
          dw_dst = BKE_defvert_find_index(dvert_dst, flip_map[dw_src->def_nr]);
        }

        if (dw_dst) {
          dw_dst->weight = dw_src->weight;
        }
      }
    }
  }
}

void BKE_defvert_remap(MDeformVert *dvert, const int *map, const int map_len)
{
  MDeformWeight *dw = dvert->dw;
  for (int i = dvert->totweight; i != 0; i--, dw++) {
    if (dw->def_nr < map_len) {
      BLI_assert(map[dw->def_nr] >= 0);

      dw->def_nr = map[dw->def_nr];
    }
  }
}

void BKE_defvert_normalize_subset(MDeformVert *dvert,
                                  const bool *vgroup_subset,
                                  const int vgroup_tot)
{
  if (dvert->totweight == 0) {
    /* nothing */
  }
  else if (dvert->totweight == 1) {
    MDeformWeight *dw = dvert->dw;
    if ((dw->def_nr < vgroup_tot) && vgroup_subset[dw->def_nr]) {
      dw->weight = 1.0f;
    }
  }
  else {
    MDeformWeight *dw = dvert->dw;
    float tot_weight = 0.0f;
    for (int i = dvert->totweight; i != 0; i--, dw++) {
      if ((dw->def_nr < vgroup_tot) && vgroup_subset[dw->def_nr]) {
        tot_weight += dw->weight;
      }
    }

    if (tot_weight > 0.0f) {
      float scalar = 1.0f / tot_weight;
      dw = dvert->dw;
      for (int i = dvert->totweight; i != 0; i--, dw++) {
        if ((dw->def_nr < vgroup_tot) && vgroup_subset[dw->def_nr]) {
          dw->weight *= scalar;

          /* in case of division errors with very low weights */
          CLAMP(dw->weight, 0.0f, 1.0f);
        }
      }
    }
  }
}

void BKE_defvert_normalize(MDeformVert *dvert)
{
  if (dvert->totweight == 0) {
    /* nothing */
  }
  else if (dvert->totweight == 1) {
    dvert->dw[0].weight = 1.0f;
  }
  else {
    MDeformWeight *dw;
    unsigned int i;
    float tot_weight = 0.0f;

    for (i = dvert->totweight, dw = dvert->dw; i != 0; i--, dw++) {
      tot_weight += dw->weight;
    }

    if (tot_weight > 0.0f) {
      float scalar = 1.0f / tot_weight;
      for (i = dvert->totweight, dw = dvert->dw; i != 0; i--, dw++) {
        dw->weight *= scalar;

        /* in case of division errors with very low weights */
        CLAMP(dw->weight, 0.0f, 1.0f);
      }
    }
  }
}

void BKE_defvert_normalize_lock_single(MDeformVert *dvert,
                                       const bool *vgroup_subset,
                                       const int vgroup_tot,
                                       const uint def_nr_lock)
{
  if (dvert->totweight == 0) {
    /* nothing */
  }
  else if (dvert->totweight == 1) {
    MDeformWeight *dw = dvert->dw;
    if ((dw->def_nr < vgroup_tot) && vgroup_subset[dw->def_nr]) {
      if (def_nr_lock != dw->def_nr) {
        dw->weight = 1.0f;
      }
    }
  }
  else {
    MDeformWeight *dw_lock = NULL;
    MDeformWeight *dw;
    unsigned int i;
    float tot_weight = 0.0f;
    float lock_iweight = 1.0f;

    for (i = dvert->totweight, dw = dvert->dw; i != 0; i--, dw++) {
      if ((dw->def_nr < vgroup_tot) && vgroup_subset[dw->def_nr]) {
        if (dw->def_nr != def_nr_lock) {
          tot_weight += dw->weight;
        }
        else {
          dw_lock = dw;
          lock_iweight = (1.0f - dw_lock->weight);
          CLAMP(lock_iweight, 0.0f, 1.0f);
        }
      }
    }

    if (tot_weight > 0.0f) {
      /* paranoid, should be 1.0 but in case of float error clamp anyway */

      float scalar = (1.0f / tot_weight) * lock_iweight;
      for (i = dvert->totweight, dw = dvert->dw; i != 0; i--, dw++) {
        if ((dw->def_nr < vgroup_tot) && vgroup_subset[dw->def_nr]) {
          if (dw != dw_lock) {
            dw->weight *= scalar;

            /* in case of division errors with very low weights */
            CLAMP(dw->weight, 0.0f, 1.0f);
          }
        }
      }
    }
  }
}

void BKE_defvert_normalize_lock_map(MDeformVert *dvert,
                                    const bool *vgroup_subset,
                                    const int vgroup_tot,
                                    const bool *lock_flags,
                                    const int defbase_tot)
{
  if (dvert->totweight == 0) {
    /* nothing */
  }
  else if (dvert->totweight == 1) {
    MDeformWeight *dw = dvert->dw;
    if ((dw->def_nr < vgroup_tot) && vgroup_subset[dw->def_nr]) {
      if ((dw->def_nr < defbase_tot) && (lock_flags[dw->def_nr] == false)) {
        dw->weight = 1.0f;
      }
    }
  }
  else {
    MDeformWeight *dw;
    unsigned int i;
    float tot_weight = 0.0f;
    float lock_iweight = 0.0f;

    for (i = dvert->totweight, dw = dvert->dw; i != 0; i--, dw++) {
      if ((dw->def_nr < vgroup_tot) && vgroup_subset[dw->def_nr]) {
        if ((dw->def_nr < defbase_tot) && (lock_flags[dw->def_nr] == false)) {
          tot_weight += dw->weight;
        }
        else {
          /* invert after */
          lock_iweight += dw->weight;
        }
      }
    }

    lock_iweight = max_ff(0.0f, 1.0f - lock_iweight);

    if (tot_weight > 0.0f) {
      /* paranoid, should be 1.0 but in case of float error clamp anyway */

      float scalar = (1.0f / tot_weight) * lock_iweight;
      for (i = dvert->totweight, dw = dvert->dw; i != 0; i--, dw++) {
        if ((dw->def_nr < vgroup_tot) && vgroup_subset[dw->def_nr]) {
          if ((dw->def_nr < defbase_tot) && (lock_flags[dw->def_nr] == false)) {
            dw->weight *= scalar;

            /* in case of division errors with very low weights */
            CLAMP(dw->weight, 0.0f, 1.0f);
          }
        }
      }
    }
  }
}

void BKE_defvert_flip(MDeformVert *dvert, const int *flip_map, const int flip_map_len)
{
  MDeformWeight *dw;
  int i;

  for (dw = dvert->dw, i = 0; i < dvert->totweight; dw++, i++) {
    if (dw->def_nr < flip_map_len) {
      if (flip_map[dw->def_nr] >= 0) {
        dw->def_nr = flip_map[dw->def_nr];
      }
    }
  }
}

void BKE_defvert_flip_merged(MDeformVert *dvert, const int *flip_map, const int flip_map_len)
{
  MDeformWeight *dw, *dw_cpy;
  float weight;
  int i, totweight = dvert->totweight;

  /* copy weights */
  for (dw = dvert->dw, i = 0; i < totweight; dw++, i++) {
    if (dw->def_nr < flip_map_len) {
      if (flip_map[dw->def_nr] >= 0) {
        /* error checkers complain of this but we'll never get NULL return */
        dw_cpy = BKE_defvert_ensure_index(dvert, flip_map[dw->def_nr]);
        dw = &dvert->dw[i]; /* in case array got realloced */

        /* distribute weights: if only one of the vertex groups was
         * assigned this will halve the weights, otherwise it gets
         * evened out. this keeps it proportional to other groups */
        weight = 0.5f * (dw_cpy->weight + dw->weight);
        dw_cpy->weight = weight;
        dw->weight = weight;
      }
    }
  }
}

bool BKE_object_supports_vertex_groups(const Object *ob)
{
  const ID *id = (const ID *)ob->data;
  if (id == NULL) {
    return false;
  }

  return ELEM(GS(id->name), ID_ME, ID_LT, ID_GD);
}

const ListBase *BKE_id_defgroup_list_get(const ID *id)
{
  switch (GS(id->name)) {
    case ID_ME: {
      const Mesh *me = (const Mesh *)id;
      return &me->vertex_group_names;
    }
    case ID_LT: {
      const Lattice *lt = (const Lattice *)id;
      return &lt->vertex_group_names;
    }
    case ID_GD: {
      const bGPdata *gpd = (const bGPdata *)id;
      return &gpd->vertex_group_names;
    }
    default: {
      BLI_assert_unreachable();
    }
  }
  return NULL;
}

static const int *object_defgroup_active_index_get_p(const Object *ob)
{
  BLI_assert(BKE_object_supports_vertex_groups(ob));
  switch (ob->type) {
    case OB_MESH: {
      const Mesh *mesh = (const Mesh *)ob->data;
      return &mesh->vertex_group_active_index;
    }
    case OB_LATTICE: {
      const Lattice *lattice = (const Lattice *)ob->data;
      return &lattice->vertex_group_active_index;
    }
    case OB_GPENCIL: {
      const bGPdata *gpd = (const bGPdata *)ob->data;
      return &gpd->vertex_group_active_index;
    }
  }
  return NULL;
}

ListBase *BKE_id_defgroup_list_get_mutable(ID *id)
{
  /* Cast away const just for the accessor. */
  return (ListBase *)BKE_id_defgroup_list_get(id);
}

bDeformGroup *BKE_object_defgroup_find_name(const Object *ob, const char *name)
{
  if (name == NULL || name[0] == '\0') {
    return NULL;
  }
  const ListBase *defbase = BKE_object_defgroup_list(ob);
  return BLI_findstring(defbase, name, offsetof(bDeformGroup, name));
}

int BKE_id_defgroup_name_index(const ID *id, const char *name)
{
  int index;
  if (!BKE_id_defgroup_name_find(id, name, &index, NULL)) {
    return -1;
  }
  return index;
}

bool BKE_id_defgroup_name_find(const struct ID *id,
                               const char *name,
                               int *r_index,
                               struct bDeformGroup **r_group)
{
  if (name == NULL || name[0] == '\0') {
    return false;
  }
  const ListBase *defbase = BKE_id_defgroup_list_get(id);
  int index;
  LISTBASE_FOREACH_INDEX (bDeformGroup *, group, defbase, index) {
    if (STREQ(name, group->name)) {
      if (r_index != NULL) {
        *r_index = index;
      }
      if (r_group != NULL) {
        *r_group = group;
      }
      return true;
    }
  }
  return false;
}

const ListBase *BKE_object_defgroup_list(const Object *ob)
{
  BLI_assert(BKE_object_supports_vertex_groups(ob));
  return BKE_id_defgroup_list_get((const ID *)ob->data);
}

int BKE_object_defgroup_name_index(const Object *ob, const char *name)
{
  return BKE_id_defgroup_name_index((ID *)ob->data, name);
}

ListBase *BKE_object_defgroup_list_mutable(Object *ob)
{
  BLI_assert(BKE_object_supports_vertex_groups(ob));
  return BKE_id_defgroup_list_get_mutable((ID *)ob->data);
}

int BKE_object_defgroup_count(const Object *ob)
{
  return BLI_listbase_count(BKE_object_defgroup_list(ob));
}

int BKE_object_defgroup_active_index_get(const Object *ob)
{
  return *object_defgroup_active_index_get_p(ob);
}

void BKE_object_defgroup_active_index_set(Object *ob, const int new_index)
{
  /* Cast away const just for the accessor. */
  int *index = (int *)object_defgroup_active_index_get_p(ob);
  *index = new_index;
}

int *BKE_object_defgroup_flip_map(const Object *ob, int *flip_map_len, const bool use_default)
{
  const ListBase *defbase = BKE_object_defgroup_list(ob);
  int defbase_tot = *flip_map_len = BLI_listbase_count(defbase);

  if (defbase_tot == 0) {
    return NULL;
  }

  bDeformGroup *dg;
  char name_flip[sizeof(dg->name)];
  int i, flip_num, *map = MEM_mallocN(defbase_tot * sizeof(int), __func__);

  for (i = 0; i < defbase_tot; i++) {
    map[i] = -1;
  }

  for (dg = defbase->first, i = 0; dg; dg = dg->next, i++) {
    if (map[i] == -1) { /* may be calculated previously */

      /* in case no valid value is found, use this */
      if (use_default) {
        map[i] = i;
      }

      BLI_string_flip_side_name(name_flip, dg->name, false, sizeof(name_flip));

      if (!STREQ(name_flip, dg->name)) {
        flip_num = BKE_object_defgroup_name_index(ob, name_flip);
        if (flip_num >= 0) {
          map[i] = flip_num;
          map[flip_num] = i; /* save an extra lookup */
        }
      }
    }
  }
  return map;
}

int *BKE_object_defgroup_flip_map_single(const Object *ob,
                                         int *flip_map_len,
                                         const bool use_default,
                                         int defgroup)
{
  const ListBase *defbase = BKE_object_defgroup_list(ob);
  int defbase_tot = *flip_map_len = BLI_listbase_count(defbase);

  if (defbase_tot == 0) {
    return NULL;
  }

  bDeformGroup *dg;
  char name_flip[sizeof(dg->name)];
  int i, flip_num, *map = MEM_mallocN(defbase_tot * sizeof(int), __func__);

  for (i = 0; i < defbase_tot; i++) {
    map[i] = use_default ? i : -1;
  }

  dg = BLI_findlink(defbase, defgroup);

  BLI_string_flip_side_name(name_flip, dg->name, false, sizeof(name_flip));
  if (!STREQ(name_flip, dg->name)) {
    flip_num = BKE_object_defgroup_name_index(ob, name_flip);

    if (flip_num != -1) {
      map[defgroup] = flip_num;
      map[flip_num] = defgroup;
    }
  }

  return map;
}

int BKE_object_defgroup_flip_index(const Object *ob, int index, const bool use_default)
{
  const ListBase *defbase = BKE_object_defgroup_list(ob);
  bDeformGroup *dg = BLI_findlink(defbase, index);
  int flip_index = -1;

  if (dg) {
    char name_flip[sizeof(dg->name)];
    BLI_string_flip_side_name(name_flip, dg->name, false, sizeof(name_flip));

    if (!STREQ(name_flip, dg->name)) {
      flip_index = BKE_object_defgroup_name_index(ob, name_flip);
    }
  }

  return (flip_index == -1 && use_default) ? index : flip_index;
}

static bool defgroup_find_name_dupe(const char *name, bDeformGroup *dg, Object *ob)
{
  const ListBase *defbase = BKE_object_defgroup_list(ob);
  bDeformGroup *curdef;

  for (curdef = defbase->first; curdef; curdef = curdef->next) {
    if (dg != curdef) {
      if (STREQ(curdef->name, name)) {
        return true;
      }
    }
  }

  return false;
}

static bool defgroup_unique_check(void *arg, const char *name)
{
  struct {
    Object *ob;
    void *dg;
  } *data = arg;
  return defgroup_find_name_dupe(name, data->dg, data->ob);
}

void BKE_object_defgroup_unique_name(bDeformGroup *dg, Object *ob)
{
  struct {
    Object *ob;
    void *dg;
  } data;
  data.ob = ob;
  data.dg = dg;

  BLI_uniquename_cb(defgroup_unique_check, &data, DATA_("Group"), '.', dg->name, sizeof(dg->name));
}

float BKE_defvert_find_weight(const struct MDeformVert *dvert, const int defgroup)
{
  MDeformWeight *dw = BKE_defvert_find_index(dvert, defgroup);
  return dw ? dw->weight : 0.0f;
}

float BKE_defvert_array_find_weight_safe(const struct MDeformVert *dvert,
                                         const int index,
                                         const int defgroup)
{
  /* Invalid defgroup index means the vgroup selected is invalid,
   * does not exist, in that case it is OK to return 1.0
   * (i.e. maximum weight, as if no vgroup was selected).
   * But in case of valid defgroup and NULL dvert data pointer, it means that vgroup **is** valid,
   * and just totally empty, so we shall return '0.0' value then! */
  if (defgroup == -1) {
    return 1.0f;
  }
  if (dvert == NULL) {
    return 0.0f;
  }

  return BKE_defvert_find_weight(dvert + index, defgroup);
}

MDeformWeight *BKE_defvert_find_index(const MDeformVert *dvert, const int defgroup)
{
  if (dvert && defgroup >= 0) {
    MDeformWeight *dw = dvert->dw;
    unsigned int i;

    for (i = dvert->totweight; i != 0; i--, dw++) {
      if (dw->def_nr == defgroup) {
        return dw;
      }
    }
  }
  else {
    BLI_assert(0);
  }

  return NULL;
}

MDeformWeight *BKE_defvert_ensure_index(MDeformVert *dvert, const int defgroup)
{
  MDeformWeight *dw_new;

  /* do this check always, this function is used to check for it */
  if (!dvert || defgroup < 0) {
    BLI_assert(0);
    return NULL;
  }

  dw_new = BKE_defvert_find_index(dvert, defgroup);
  if (dw_new) {
    return dw_new;
  }

  dw_new = MEM_mallocN(sizeof(MDeformWeight) * (dvert->totweight + 1), "deformWeight");
  if (dvert->dw) {
    memcpy(dw_new, dvert->dw, sizeof(MDeformWeight) * dvert->totweight);
    MEM_freeN(dvert->dw);
  }
  dvert->dw = dw_new;
  dw_new += dvert->totweight;
  dw_new->weight = 0.0f;
  dw_new->def_nr = defgroup;
  /* Group index */

  dvert->totweight++;

  return dw_new;
}

void BKE_defvert_add_index_notest(MDeformVert *dvert, int defgroup, const float weight)
{
  /* TODO: merge with #BKE_defvert_ensure_index! */

  MDeformWeight *dw_new;

  /* do this check always, this function is used to check for it */
  if (!dvert || defgroup < 0) {
    BLI_assert(0);
    return;
  }

  dw_new = MEM_callocN(sizeof(MDeformWeight) * (dvert->totweight + 1),
                       "defvert_add_to group, new deformWeight");
  if (dvert->dw) {
    memcpy(dw_new, dvert->dw, sizeof(MDeformWeight) * dvert->totweight);
    MEM_freeN(dvert->dw);
  }
  dvert->dw = dw_new;
  dw_new += dvert->totweight;
  dw_new->weight = weight;
  dw_new->def_nr = defgroup;
  dvert->totweight++;
}

void BKE_defvert_remove_group(MDeformVert *dvert, MDeformWeight *dw)
{
  if (dvert && dw) {
    int i = dw - dvert->dw;

    /* Security check! */
    if (i < 0 || i >= dvert->totweight) {
      return;
    }

    dvert->totweight--;
    /* If there are still other deform weights attached to this vert then remove
     * this deform weight, and reshuffle the others.
     */
    if (dvert->totweight) {
      BLI_assert(dvert->dw != NULL);

      if (i != dvert->totweight) {
        dvert->dw[i] = dvert->dw[dvert->totweight];
      }

      dvert->dw = MEM_reallocN(dvert->dw, sizeof(MDeformWeight) * dvert->totweight);
    }
    else {
      /* If there are no other deform weights left then just remove this one. */
      MEM_freeN(dvert->dw);
      dvert->dw = NULL;
    }
  }
}

void BKE_defvert_clear(MDeformVert *dvert)
{
  MEM_SAFE_FREE(dvert->dw);

  dvert->totweight = 0;
}

int BKE_defvert_find_shared(const MDeformVert *dvert_a, const MDeformVert *dvert_b)
{
  if (dvert_a->totweight && dvert_b->totweight) {
    MDeformWeight *dw = dvert_a->dw;
    unsigned int i;

    for (i = dvert_a->totweight; i != 0; i--, dw++) {
      if (dw->weight > 0.0f && BKE_defvert_find_weight(dvert_b, dw->def_nr) > 0.0f) {
        return dw->def_nr;
      }
    }
  }

  return -1;
}

bool BKE_defvert_is_weight_zero(const struct MDeformVert *dvert, const int defgroup_tot)
{
  MDeformWeight *dw = dvert->dw;
  for (int i = dvert->totweight; i != 0; i--, dw++) {
    if (dw->weight != 0.0f) {
      /* check the group is in-range, happens on rare situations */
      if (LIKELY(dw->def_nr < defgroup_tot)) {
        return false;
      }
    }
  }
  return true;
}

float BKE_defvert_total_selected_weight(const struct MDeformVert *dv,
                                        int defbase_tot,
                                        const bool *defbase_sel)
{
  float total = 0.0f;
  const MDeformWeight *dw = dv->dw;

  if (defbase_sel == NULL) {
    return total;
  }

  for (int i = dv->totweight; i != 0; i--, dw++) {
    if (dw->def_nr < defbase_tot) {
      if (defbase_sel[dw->def_nr]) {
        total += dw->weight;
      }
    }
  }

  return total;
}

float BKE_defvert_multipaint_collective_weight(const struct MDeformVert *dv,
                                               int defbase_tot,
                                               const bool *defbase_sel,
                                               int defbase_tot_sel,
                                               bool is_normalized)
{
  float total = BKE_defvert_total_selected_weight(dv, defbase_tot, defbase_sel);

  /* in multipaint, get the average if auto normalize is inactive
   * get the sum if it is active */
  if (!is_normalized) {
    total /= defbase_tot_sel;
  }

  return total;
}

float BKE_defvert_calc_lock_relative_weight(float weight,
                                            float locked_weight,
                                            float unlocked_weight)
{
  /* First try normalizing unlocked weights. */
  if (unlocked_weight > 0.0f) {
    return weight / unlocked_weight;
  }

  /* If no unlocked weight exists, take locked into account. */
  if (locked_weight <= 0.0f) {
    return weight;
  }

  /* handle division by zero */
  if (locked_weight >= 1.0f - VERTEX_WEIGHT_LOCK_EPSILON) {
    if (weight != 0.0f) {
      return 1.0f;
    }

    /* resolve 0/0 to 0 */
    return 0.0f;
  }

  /* non-degenerate division */
  return weight / (1.0f - locked_weight);
}

float BKE_defvert_lock_relative_weight(float weight,
                                       const struct MDeformVert *dv,
                                       int defbase_tot,
                                       const bool *defbase_locked,
                                       const bool *defbase_unlocked)
{
  float unlocked = BKE_defvert_total_selected_weight(dv, defbase_tot, defbase_unlocked);

  if (unlocked > 0.0f) {
    return weight / unlocked;
  }

  float locked = BKE_defvert_total_selected_weight(dv, defbase_tot, defbase_locked);

  return BKE_defvert_calc_lock_relative_weight(weight, locked, unlocked);
}

/* -------------------------------------------------------------------- */
/** \name Defvert Array functions
 * \{ */

void BKE_defvert_array_copy(MDeformVert *dst, const MDeformVert *src, int totvert)
{
  /* Assumes dst is already set up */

  if (!src || !dst) {
    return;
  }

  memcpy(dst, src, totvert * sizeof(MDeformVert));

  for (int i = 0; i < totvert; i++) {
    if (src[i].dw) {
      dst[i].dw = MEM_mallocN(sizeof(MDeformWeight) * src[i].totweight, "copy_deformWeight");
      memcpy(dst[i].dw, src[i].dw, sizeof(MDeformWeight) * src[i].totweight);
    }
  }
}

void BKE_defvert_array_free_elems(MDeformVert *dvert, int totvert)
{
  /* Instead of freeing the verts directly,
   * call this function to delete any special
   * vert data */

  if (!dvert) {
    return;
  }

  /* Free any special data from the verts */
  for (int i = 0; i < totvert; i++) {
    if (dvert[i].dw) {
      MEM_freeN(dvert[i].dw);
    }
  }
}

void BKE_defvert_array_free(MDeformVert *dvert, int totvert)
{
  /* Instead of freeing the verts directly,
   * call this function to delete any special
   * vert data */
  if (!dvert) {
    return;
  }

  /* Free any special data from the verts */
  BKE_defvert_array_free_elems(dvert, totvert);

  MEM_freeN(dvert);
}

void BKE_defvert_extract_vgroup_to_vertweights(const MDeformVert *dvert,
                                               const int defgroup,
                                               const int num_verts,
                                               const bool invert_vgroup,
                                               float *r_weights)
{
  if (dvert && defgroup != -1) {
    int i = num_verts;

    while (i--) {
      const float w = BKE_defvert_find_weight(&dvert[i], defgroup);
      r_weights[i] = invert_vgroup ? (1.0f - w) : w;
    }
  }
  else {
    copy_vn_fl(r_weights, num_verts, invert_vgroup ? 1.0f : 0.0f);
  }
}

void BKE_defvert_extract_vgroup_to_edgeweights(const MDeformVert *dvert,
                                               const int defgroup,
                                               const int num_verts,
                                               MEdge *edges,
                                               const int num_edges,
                                               const bool invert_vgroup,
                                               float *r_weights)
{
  if (dvert && defgroup != -1) {
    int i = num_edges;
    float *tmp_weights = MEM_mallocN(sizeof(*tmp_weights) * (size_t)num_verts, __func__);

    BKE_defvert_extract_vgroup_to_vertweights(
        dvert, defgroup, num_verts, invert_vgroup, tmp_weights);

    while (i--) {
      MEdge *me = &edges[i];

      r_weights[i] = (tmp_weights[me->v1] + tmp_weights[me->v2]) * 0.5f;
    }

    MEM_freeN(tmp_weights);
  }
  else {
    copy_vn_fl(r_weights, num_edges, 0.0f);
  }
}

void BKE_defvert_extract_vgroup_to_loopweights(const MDeformVert *dvert,
                                               const int defgroup,
                                               const int num_verts,
                                               MLoop *loops,
                                               const int num_loops,
                                               const bool invert_vgroup,
                                               float *r_weights)
{
  if (dvert && defgroup != -1) {
    int i = num_loops;
    float *tmp_weights = MEM_mallocN(sizeof(*tmp_weights) * (size_t)num_verts, __func__);

    BKE_defvert_extract_vgroup_to_vertweights(
        dvert, defgroup, num_verts, invert_vgroup, tmp_weights);

    while (i--) {
      MLoop *ml = &loops[i];

      r_weights[i] = tmp_weights[ml->v];
    }

    MEM_freeN(tmp_weights);
  }
  else {
    copy_vn_fl(r_weights, num_loops, 0.0f);
  }
}

void BKE_defvert_extract_vgroup_to_polyweights(const MDeformVert *dvert,
                                               const int defgroup,
                                               const int num_verts,
                                               MLoop *loops,
                                               const int UNUSED(num_loops),
                                               MPoly *polys,
                                               const int num_polys,
                                               const bool invert_vgroup,
                                               float *r_weights)
{
  if (dvert && defgroup != -1) {
    int i = num_polys;
    float *tmp_weights = MEM_mallocN(sizeof(*tmp_weights) * (size_t)num_verts, __func__);

    BKE_defvert_extract_vgroup_to_vertweights(
        dvert, defgroup, num_verts, invert_vgroup, tmp_weights);

    while (i--) {
      MPoly *mp = &polys[i];
      MLoop *ml = &loops[mp->loopstart];
      int j = mp->totloop;
      float w = 0.0f;

      for (; j--; ml++) {
        w += tmp_weights[ml->v];
      }
      r_weights[i] = w / (float)mp->totloop;
    }

    MEM_freeN(tmp_weights);
  }
  else {
    copy_vn_fl(r_weights, num_polys, 0.0f);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Data Transfer
 * \{ */

static void vgroups_datatransfer_interp(const CustomDataTransferLayerMap *laymap,
                                        void *dest,
                                        const void **sources,
                                        const float *weights,
                                        const int count,
                                        const float mix_factor)
{
  MDeformVert **data_src = (MDeformVert **)sources;
  MDeformVert *data_dst = (MDeformVert *)dest;
  const int idx_src = laymap->data_src_n;
  const int idx_dst = laymap->data_dst_n;

  const int mix_mode = laymap->mix_mode;

  int i, j;

  MDeformWeight *dw_src;
  MDeformWeight *dw_dst = BKE_defvert_find_index(data_dst, idx_dst);
  float weight_src = 0.0f, weight_dst = 0.0f;

  bool has_dw_sources = false;
  if (sources) {
    for (i = count; i--;) {
      for (j = data_src[i]->totweight; j--;) {
        if ((dw_src = &data_src[i]->dw[j])->def_nr == idx_src) {
          weight_src += dw_src->weight * weights[i];
          has_dw_sources = true;
          break;
        }
      }
    }
  }

  if (dw_dst) {
    weight_dst = dw_dst->weight;
  }
  else if (mix_mode == CDT_MIX_REPLACE_ABOVE_THRESHOLD) {
    return; /* Do not affect destination. */
  }

  weight_src = data_transfer_interp_float_do(mix_mode, weight_dst, weight_src, mix_factor);

  CLAMP(weight_src, 0.0f, 1.0f);

  /* Do not create a destination MDeformWeight data if we had no sources at all. */
  if (!has_dw_sources) {
    BLI_assert(weight_src == 0.0f);
    if (dw_dst) {
      dw_dst->weight = weight_src;
    }
  }
  else if (!dw_dst) {
    BKE_defvert_add_index_notest(data_dst, idx_dst, weight_src);
  }
  else {
    dw_dst->weight = weight_src;
  }
}

static bool data_transfer_layersmapping_vgroups_multisrc_to_dst(ListBase *r_map,
                                                                const int mix_mode,
                                                                const float mix_factor,
                                                                const float *mix_weights,
                                                                const int num_elem_dst,
                                                                const bool use_create,
                                                                const bool use_delete,
                                                                Object *ob_src,
                                                                Object *ob_dst,
                                                                const MDeformVert *data_src,
                                                                MDeformVert *data_dst,
                                                                CustomData *UNUSED(cd_src),
                                                                CustomData *cd_dst,
                                                                const bool UNUSED(use_dupref_dst),
                                                                const int tolayers,
                                                                const bool *use_layers_src,
                                                                const int num_layers_src)
{
  int idx_src;
  int idx_dst;
  const ListBase *src_list = BKE_object_defgroup_list(ob_src);
  ListBase *dst_defbase = BKE_object_defgroup_list_mutable(ob_dst);

  int tot_dst = BLI_listbase_count(dst_defbase);

  const size_t elem_size = sizeof(*((MDeformVert *)NULL));

  switch (tolayers) {
    case DT_LAYERS_INDEX_DST:
      idx_dst = tot_dst;

      /* Find last source actually used! */
      idx_src = num_layers_src;
      while (idx_src-- && !use_layers_src[idx_src]) {
        /* pass */
      }
      idx_src++;

      if (idx_dst < idx_src) {
        if (use_create) {
          /* Create as much vgroups as necessary! */
          for (; idx_dst < idx_src; idx_dst++) {
            BKE_object_defgroup_add(ob_dst);
          }
        }
        else {
          /* Otherwise, just try to map what we can with existing dst vgroups. */
          idx_src = idx_dst;
        }
      }
      else if (use_delete && idx_dst > idx_src) {
        while (idx_dst-- > idx_src) {
          BKE_object_defgroup_remove(ob_dst, dst_defbase->last);
        }
      }
      if (r_map) {
        /* At this stage, we **need** a valid CD_MDEFORMVERT layer on dest!
         * Again, use_create is not relevant in this case */
        if (!data_dst) {
          data_dst = CustomData_add_layer(cd_dst, CD_MDEFORMVERT, CD_CALLOC, NULL, num_elem_dst);
        }

        while (idx_src--) {
          if (!use_layers_src[idx_src]) {
            continue;
          }
          data_transfer_layersmapping_add_item(r_map,
                                               CD_FAKE_MDEFORMVERT,
                                               mix_mode,
                                               mix_factor,
                                               mix_weights,
                                               data_src,
                                               data_dst,
                                               idx_src,
                                               idx_src,
                                               elem_size,
                                               0,
                                               0,
                                               0,
                                               vgroups_datatransfer_interp,
                                               NULL);
        }
      }
      break;
    case DT_LAYERS_NAME_DST: {
      bDeformGroup *dg_src, *dg_dst;

      if (use_delete) {
        /* Remove all unused dst vgroups first, simpler in this case. */
        for (dg_dst = dst_defbase->first; dg_dst;) {
          bDeformGroup *dg_dst_next = dg_dst->next;

          if (BKE_object_defgroup_name_index(ob_src, dg_dst->name) == -1) {
            BKE_object_defgroup_remove(ob_dst, dg_dst);
          }
          dg_dst = dg_dst_next;
        }
      }

      for (idx_src = 0, dg_src = src_list->first; idx_src < num_layers_src;
           idx_src++, dg_src = dg_src->next) {
        if (!use_layers_src[idx_src]) {
          continue;
        }

        if ((idx_dst = BKE_object_defgroup_name_index(ob_dst, dg_src->name)) == -1) {
          if (use_create) {
            BKE_object_defgroup_add_name(ob_dst, dg_src->name);
            idx_dst = BKE_object_defgroup_active_index_get(ob_dst) - 1;
          }
          else {
            /* If we are not allowed to create missing dst vgroups, just skip matching src one. */
            continue;
          }
        }
        if (r_map) {
          /* At this stage, we **need** a valid CD_MDEFORMVERT layer on dest!
           * use_create is not relevant in this case */
          if (!data_dst) {
            data_dst = CustomData_add_layer(cd_dst, CD_MDEFORMVERT, CD_CALLOC, NULL, num_elem_dst);
          }

          data_transfer_layersmapping_add_item(r_map,
                                               CD_FAKE_MDEFORMVERT,
                                               mix_mode,
                                               mix_factor,
                                               mix_weights,
                                               data_src,
                                               data_dst,
                                               idx_src,
                                               idx_dst,
                                               elem_size,
                                               0,
                                               0,
                                               0,
                                               vgroups_datatransfer_interp,
                                               NULL);
        }
      }
      break;
    }
    default:
      return false;
  }

  return true;
}

bool data_transfer_layersmapping_vgroups(ListBase *r_map,
                                         const int mix_mode,
                                         const float mix_factor,
                                         const float *mix_weights,
                                         const int num_elem_dst,
                                         const bool use_create,
                                         const bool use_delete,
                                         Object *ob_src,
                                         Object *ob_dst,
                                         CustomData *cd_src,
                                         CustomData *cd_dst,
                                         const bool use_dupref_dst,
                                         const int fromlayers,
                                         const int tolayers)
{
  int idx_src, idx_dst;

  const size_t elem_size = sizeof(*((MDeformVert *)NULL));

  /* NOTE:
   * VGroups are a bit hairy, since their layout is defined on object level (ob->defbase),
   * while their actual data is a (mesh) CD layer.
   * This implies we may have to handle data layout itself while having NULL data itself,
   * and even have to support NULL data_src in transfer data code
   * (we always create a data_dst, though).
   *
   * NOTE: Above comment is outdated, but this function was written when that was true.
   */

  const ListBase *src_defbase = BKE_object_defgroup_list(ob_src);
  if (BLI_listbase_is_empty(src_defbase)) {
    if (use_delete) {
      BKE_object_defgroup_remove_all(ob_dst);
    }
    return true;
  }

  const MDeformVert *data_src = CustomData_get_layer(cd_src, CD_MDEFORMVERT);

  MDeformVert *data_dst = CustomData_get_layer(cd_dst, CD_MDEFORMVERT);
  if (data_dst && use_dupref_dst && r_map) {
    /* If dest is a derivedmesh, we do not want to overwrite cdlayers of org mesh! */
    data_dst = CustomData_duplicate_referenced_layer(cd_dst, CD_MDEFORMVERT, num_elem_dst);
  }

  if (fromlayers == DT_LAYERS_ACTIVE_SRC || fromlayers >= 0) {
    /* NOTE: use_delete has not much meaning in this case, ignored. */

    if (fromlayers >= 0) {
      idx_src = fromlayers;
      if (idx_src >= BLI_listbase_count(src_defbase)) {
        /* This can happen when vgroups are removed from source object...
         * Remapping would be really tricky here, we'd need to go over all objects in
         * Main every time we delete a vgroup... for now, simpler and safer to abort. */
        return false;
      }
    }
    else if ((idx_src = BKE_object_defgroup_active_index_get(ob_src) - 1) == -1) {
      return false;
    }

    if (tolayers >= 0) {
      /* NOTE: in this case we assume layer exists! */
      idx_dst = tolayers;
      const ListBase *dst_defbase = BKE_object_defgroup_list(ob_dst);
      BLI_assert(idx_dst < BLI_listbase_count(dst_defbase));
      UNUSED_VARS_NDEBUG(dst_defbase);
    }
    else if (tolayers == DT_LAYERS_ACTIVE_DST) {
      if ((idx_dst = BKE_object_defgroup_active_index_get(ob_dst) - 1) == -1) {
        bDeformGroup *dg_src;
        if (!use_create) {
          return true;
        }
        dg_src = BLI_findlink(src_defbase, idx_src);
        BKE_object_defgroup_add_name(ob_dst, dg_src->name);
        idx_dst = BKE_object_defgroup_active_index_get(ob_dst) - 1;
      }
    }
    else if (tolayers == DT_LAYERS_INDEX_DST) {
      int num = BLI_listbase_count(src_defbase);
      idx_dst = idx_src;
      if (num <= idx_dst) {
        if (!use_create) {
          return true;
        }
        /* Create as much vgroups as necessary! */
        for (; num <= idx_dst; num++) {
          BKE_object_defgroup_add(ob_dst);
        }
      }
    }
    else if (tolayers == DT_LAYERS_NAME_DST) {
      bDeformGroup *dg_src = BLI_findlink(src_defbase, idx_src);
      if ((idx_dst = BKE_object_defgroup_name_index(ob_dst, dg_src->name)) == -1) {
        if (!use_create) {
          return true;
        }
        BKE_object_defgroup_add_name(ob_dst, dg_src->name);
        idx_dst = BKE_object_defgroup_active_index_get(ob_dst) - 1;
      }
    }
    else {
      return false;
    }

    if (r_map) {
      /* At this stage, we **need** a valid CD_MDEFORMVERT layer on dest!
       * use_create is not relevant in this case */
      if (!data_dst) {
        data_dst = CustomData_add_layer(cd_dst, CD_MDEFORMVERT, CD_CALLOC, NULL, num_elem_dst);
      }

      data_transfer_layersmapping_add_item(r_map,
                                           CD_FAKE_MDEFORMVERT,
                                           mix_mode,
                                           mix_factor,
                                           mix_weights,
                                           data_src,
                                           data_dst,
                                           idx_src,
                                           idx_dst,
                                           elem_size,
                                           0,
                                           0,
                                           0,
                                           vgroups_datatransfer_interp,
                                           NULL);
    }
  }
  else {
    int num_src, num_sel_unused;
    bool *use_layers_src = NULL;
    bool ret = false;

    switch (fromlayers) {
      case DT_LAYERS_ALL_SRC:
        use_layers_src = BKE_object_defgroup_subset_from_select_type(
            ob_src, WT_VGROUP_ALL, &num_src, &num_sel_unused);
        break;
      case DT_LAYERS_VGROUP_SRC_BONE_SELECT:
        use_layers_src = BKE_object_defgroup_subset_from_select_type(
            ob_src, WT_VGROUP_BONE_SELECT, &num_src, &num_sel_unused);
        break;
      case DT_LAYERS_VGROUP_SRC_BONE_DEFORM:
        use_layers_src = BKE_object_defgroup_subset_from_select_type(
            ob_src, WT_VGROUP_BONE_DEFORM, &num_src, &num_sel_unused);
        break;
    }

    if (use_layers_src) {
      ret = data_transfer_layersmapping_vgroups_multisrc_to_dst(r_map,
                                                                mix_mode,
                                                                mix_factor,
                                                                mix_weights,
                                                                num_elem_dst,
                                                                use_create,
                                                                use_delete,
                                                                ob_src,
                                                                ob_dst,
                                                                data_src,
                                                                data_dst,
                                                                cd_src,
                                                                cd_dst,
                                                                use_dupref_dst,
                                                                tolayers,
                                                                use_layers_src,
                                                                num_src);
    }

    MEM_SAFE_FREE(use_layers_src);
    return ret;
  }

  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Various utils & helpers.
 * \{ */

void BKE_defvert_weight_to_rgb(float r_rgb[3], const float weight)
{
  const float blend = ((weight / 2.0f) + 0.5f);

  if (weight <= 0.25f) { /* blue->cyan */
    r_rgb[0] = 0.0f;
    r_rgb[1] = blend * weight * 4.0f;
    r_rgb[2] = blend;
  }
  else if (weight <= 0.50f) { /* cyan->green */
    r_rgb[0] = 0.0f;
    r_rgb[1] = blend;
    r_rgb[2] = blend * (1.0f - ((weight - 0.25f) * 4.0f));
  }
  else if (weight <= 0.75f) { /* green->yellow */
    r_rgb[0] = blend * ((weight - 0.50f) * 4.0f);
    r_rgb[1] = blend;
    r_rgb[2] = 0.0f;
  }
  else if (weight <= 1.0f) { /* yellow->red */
    r_rgb[0] = blend;
    r_rgb[1] = blend * (1.0f - ((weight - 0.75f) * 4.0f));
    r_rgb[2] = 0.0f;
  }
  else {
    /* exceptional value, unclamped or nan,
     * avoid uninitialized memory use */
    r_rgb[0] = 1.0f;
    r_rgb[1] = 0.0f;
    r_rgb[2] = 1.0f;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name .blend file I/O
 * \{ */

void BKE_defbase_blend_write(BlendWriter *writer, const ListBase *defbase)
{
  LISTBASE_FOREACH (bDeformGroup *, defgroup, defbase) {
    BLO_write_struct(writer, bDeformGroup, defgroup);
  }
}

void BKE_defvert_blend_write(BlendWriter *writer, int count, const MDeformVert *dvlist)
{
  if (dvlist == NULL) {
    return;
  }

  /* Write the dvert list */
  BLO_write_struct_array(writer, MDeformVert, count, dvlist);

  /* Write deformation data for each dvert */
  for (int i = 0; i < count; i++) {
    if (dvlist[i].dw) {
      BLO_write_struct_array(writer, MDeformWeight, dvlist[i].totweight, dvlist[i].dw);
    }
  }
}

void BKE_defvert_blend_read(BlendDataReader *reader, int count, MDeformVert *mdverts)
{
  if (mdverts == NULL) {
    return;
  }

  for (int i = count; i > 0; i--, mdverts++) {
    /* Convert to vertex group allocation system. */
    MDeformWeight *dw;
    if (mdverts->dw && (dw = BLO_read_get_new_data_address(reader, mdverts->dw))) {
      const size_t dw_len = sizeof(MDeformWeight) * mdverts->totweight;
      void *dw_tmp = MEM_mallocN(dw_len, __func__);
      memcpy(dw_tmp, dw, dw_len);
      mdverts->dw = dw_tmp;
      MEM_freeN(dw);
    }
    else {
      mdverts->dw = NULL;
      mdverts->totweight = 0;
    }
  }
}

/** \} */
