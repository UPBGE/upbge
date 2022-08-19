/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2001-2002 NaN Holding BV. All rights reserved. */

/** \file
 * \ingroup bke
 *
 * MetaBalls are created from a single Object (with a name without number in it).
 * All objects with the same name (but with a number in it) are added to this.
 */

#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "MEM_guardedalloc.h"

/* Allow using deprecated functionality for .blend file I/O. */
#define DNA_DEPRECATED_ALLOW

#include "DNA_defaults.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meta_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BLI_blenlib.h"
#include "BLI_math.h"
#include "BLI_string_utils.h"
#include "BLI_utildefines.h"

#include "BLT_translation.h"

#include "BKE_main.h"

#include "BKE_anim_data.h"
#include "BKE_curve.h"
#include "BKE_displist.h"
#include "BKE_geometry_set.hh"
#include "BKE_idtype.h"
#include "BKE_lattice.h"
#include "BKE_lib_id.h"
#include "BKE_lib_query.h"
#include "BKE_material.h"
#include "BKE_mball.h"
#include "BKE_mball_tessellate.h"
#include "BKE_mesh.h"
#include "BKE_object.h"
#include "BKE_scene.h"

#include "DEG_depsgraph.h"

#include "BLO_read_write.h"

static void metaball_init_data(ID *id)
{
  MetaBall *metaball = (MetaBall *)id;

  BLI_assert(MEMCMP_STRUCT_AFTER_IS_ZERO(metaball, id));

  MEMCPY_STRUCT_AFTER(metaball, DNA_struct_default_get(MetaBall), id);
}

static void metaball_copy_data(Main *UNUSED(bmain),
                               ID *id_dst,
                               const ID *id_src,
                               const int UNUSED(flag))
{
  MetaBall *metaball_dst = (MetaBall *)id_dst;
  const MetaBall *metaball_src = (const MetaBall *)id_src;

  BLI_duplicatelist(&metaball_dst->elems, &metaball_src->elems);

  metaball_dst->mat = static_cast<Material **>(MEM_dupallocN(metaball_src->mat));

  metaball_dst->editelems = nullptr;
  metaball_dst->lastelem = nullptr;
}

static void metaball_free_data(ID *id)
{
  MetaBall *metaball = (MetaBall *)id;

  MEM_SAFE_FREE(metaball->mat);

  BLI_freelistN(&metaball->elems);
  if (metaball->disp.first) {
    BKE_displist_free(&metaball->disp);
  }
}

static void metaball_foreach_id(ID *id, LibraryForeachIDData *data)
{
  MetaBall *metaball = (MetaBall *)id;
  for (int i = 0; i < metaball->totcol; i++) {
    BKE_LIB_FOREACHID_PROCESS_IDSUPER(data, metaball->mat[i], IDWALK_CB_USER);
  }
}

static void metaball_blend_write(BlendWriter *writer, ID *id, const void *id_address)
{
  MetaBall *mb = (MetaBall *)id;

  /* Clean up, important in undo case to reduce false detection of changed datablocks. */
  BLI_listbase_clear(&mb->disp);
  mb->editelems = nullptr;
  /* Must always be cleared (meta's don't have their own edit-data). */
  mb->needs_flush_to_id = 0;
  mb->lastelem = nullptr;

  /* write LibData */
  BLO_write_id_struct(writer, MetaBall, id_address, &mb->id);
  BKE_id_blend_write(writer, &mb->id);

  /* direct data */
  BLO_write_pointer_array(writer, mb->totcol, mb->mat);
  if (mb->adt) {
    BKE_animdata_blend_write(writer, mb->adt);
  }

  LISTBASE_FOREACH (MetaElem *, ml, &mb->elems) {
    BLO_write_struct(writer, MetaElem, ml);
  }
}

static void metaball_blend_read_data(BlendDataReader *reader, ID *id)
{
  MetaBall *mb = (MetaBall *)id;
  BLO_read_data_address(reader, &mb->adt);
  BKE_animdata_blend_read_data(reader, mb->adt);

  BLO_read_pointer_array(reader, (void **)&mb->mat);

  BLO_read_list(reader, &(mb->elems));

  BLI_listbase_clear(&mb->disp);
  mb->editelems = nullptr;
  /* Must always be cleared (meta's don't have their own edit-data). */
  mb->needs_flush_to_id = 0;
  // mb->edit_elems.first = mb->edit_elems.last = nullptr;
  mb->lastelem = nullptr;
}

static void metaball_blend_read_lib(BlendLibReader *reader, ID *id)
{
  MetaBall *mb = (MetaBall *)id;
  for (int a = 0; a < mb->totcol; a++) {
    BLO_read_id_address(reader, mb->id.lib, &mb->mat[a]);
  }

  BLO_read_id_address(reader, mb->id.lib, &mb->ipo);  // XXX deprecated - old animation system
}

static void metaball_blend_read_expand(BlendExpander *expander, ID *id)
{
  MetaBall *mb = (MetaBall *)id;
  for (int a = 0; a < mb->totcol; a++) {
    BLO_expand(expander, mb->mat[a]);
  }
}

IDTypeInfo IDType_ID_MB = {
    /* id_code */ ID_MB,
    /* id_filter */ FILTER_ID_MB,
    /* main_listbase_index */ INDEX_ID_MB,
    /* struct_size */ sizeof(MetaBall),
    /* name */ "Metaball",
    /* name_plural */ "metaballs",
    /* translation_context */ BLT_I18NCONTEXT_ID_METABALL,
    /* flags */ IDTYPE_FLAGS_APPEND_IS_REUSABLE,
    /* asset_type_info */ nullptr,

    /* init_data */ metaball_init_data,
    /* copy_data */ metaball_copy_data,
    /* free_data */ metaball_free_data,
    /* make_local */ nullptr,
    /* foreach_id */ metaball_foreach_id,
    /* foreach_cache */ nullptr,
    /* foreach_path */ nullptr,
    /* owner_get */ nullptr,

    /* blend_write */ metaball_blend_write,
    /* blend_read_data */ metaball_blend_read_data,
    /* blend_read_lib */ metaball_blend_read_lib,
    /* blend_read_expand */ metaball_blend_read_expand,

    /* blend_read_undo_preserve */ nullptr,

    /* lib_override_apply_post */ nullptr,
};

/* Functions */

MetaBall *BKE_mball_add(Main *bmain, const char *name)
{
  MetaBall *mb = static_cast<MetaBall *>(BKE_id_new(bmain, ID_MB, name));
  return mb;
}

MetaElem *BKE_mball_element_add(MetaBall *mb, const int type)
{
  MetaElem *ml = MEM_cnew<MetaElem>(__func__);

  unit_qt(ml->quat);

  ml->rad = 2.0;
  ml->s = 2.0;
  ml->flag = MB_SCALE_RAD;

  switch (type) {
    case MB_BALL:
      ml->type = MB_BALL;
      ml->expx = ml->expy = ml->expz = 1.0;

      break;
    case MB_TUBE:
      ml->type = MB_TUBE;
      ml->expx = ml->expy = ml->expz = 1.0;

      break;
    case MB_PLANE:
      ml->type = MB_PLANE;
      ml->expx = ml->expy = ml->expz = 1.0;

      break;
    case MB_ELIPSOID:
      ml->type = MB_ELIPSOID;
      ml->expx = 1.2f;
      ml->expy = 0.8f;
      ml->expz = 1.0;

      break;
    case MB_CUBE:
      ml->type = MB_CUBE;
      ml->expx = ml->expy = ml->expz = 1.0;

      break;
    default:
      break;
  }

  BLI_addtail(&mb->elems, ml);

  return ml;
}

BoundBox *BKE_mball_boundbox_get(Object *ob)
{
  BLI_assert(ob->type == OB_MBALL);
  if (ob->runtime.bb != NULL && (ob->runtime.bb->flag & BOUNDBOX_DIRTY) == 0) {
    return ob->runtime.bb;
  }
  if (ob->runtime.bb == NULL) {
    ob->runtime.bb = MEM_cnew<BoundBox>(__func__);
  }

  /* Expect that this function is only called for evaluated objects. */
  const Mesh *mesh_eval = BKE_object_get_evaluated_mesh(ob);
  float min[3];
  float max[3];
  if (mesh_eval) {
    INIT_MINMAX(min, max);
    if (!BKE_mesh_minmax(mesh_eval, min, max)) {
      copy_v3_fl(min, -1.0f);
      copy_v3_fl(max, 1.0f);
    }
  }
  else {
    copy_v3_fl(min, 0.0f);
    copy_v3_fl(max, 0.0f);
  }

  BKE_boundbox_init_from_minmax(ob->runtime.bb, min, max);
  ob->runtime.bb->flag &= ~BOUNDBOX_DIRTY;

  return ob->runtime.bb;
}

bool BKE_mball_is_basis(const Object *ob)
{
  /* Meta-Ball Basis Notes from Blender-2.5x
   * =======================================
   *
   * NOTE(@campbellbarton): This is a can of worms.
   *
   * This really needs a rewrite/refactor its totally broken in anything other than basic cases
   * Multiple Scenes + Set Scenes & mixing meta-ball basis _should_ work but fails to update the
   * depsgraph on rename and linking into scenes or removal of basis meta-ball.
   * So take care when changing this code.
   *
   * Main idiot thing here is that the system returns #BKE_mball_basis_find()
   * objects which fail a #BKE_mball_is_basis() test.
   *
   * Not only that but the depsgraph and their areas depend on this behavior,
   * so making small fixes here isn't worth it. */

  /* Just a quick test. */
  const int len = strlen(ob->id.name);
  return (!isdigit(ob->id.name[len - 1]));
}

bool BKE_mball_is_same_group(const Object *ob1, const Object *ob2)
{
  int basis1nr, basis2nr;
  char basis1name[MAX_ID_NAME], basis2name[MAX_ID_NAME];

  if (ob1->id.name[2] != ob2->id.name[2]) {
    /* Quick return in case first char of both ID's names is not the same... */
    return false;
  }

  BLI_split_name_num(basis1name, &basis1nr, ob1->id.name + 2, '.');
  BLI_split_name_num(basis2name, &basis2nr, ob2->id.name + 2, '.');

  return STREQ(basis1name, basis2name);
}

bool BKE_mball_is_basis_for(const Object *ob1, const Object *ob2)
{
  return BKE_mball_is_same_group(ob1, ob2) && BKE_mball_is_basis(ob1);
}

bool BKE_mball_is_any_selected(const MetaBall *mb)
{
  LISTBASE_FOREACH (const MetaElem *, ml, mb->editelems) {
    if (ml->flag & SELECT) {
      return true;
    }
  }
  return false;
}

bool BKE_mball_is_any_selected_multi(Base **bases, int bases_len)
{
  for (uint base_index = 0; base_index < bases_len; base_index++) {
    Object *obedit = bases[base_index]->object;
    MetaBall *mb = (MetaBall *)obedit->data;
    if (BKE_mball_is_any_selected(mb)) {
      return true;
    }
  }
  return false;
}

bool BKE_mball_is_any_unselected(const MetaBall *mb)
{
  LISTBASE_FOREACH (const MetaElem *, ml, mb->editelems) {
    if ((ml->flag & SELECT) == 0) {
      return true;
    }
  }
  return false;
}

static void mball_data_properties_copy(MetaBall *mb_dst, MetaBall *mb_src)
{
  mb_dst->wiresize = mb_src->wiresize;
  mb_dst->rendersize = mb_src->rendersize;
  mb_dst->thresh = mb_src->thresh;
  mb_dst->flag = mb_src->flag;
  DEG_id_tag_update(&mb_dst->id, 0);
}

void BKE_mball_properties_copy(Main *bmain, MetaBall *metaball_src)
{
  /**
   * WARNING: This code does not cover all potential corner-cases. E.g. if:
   * <pre>
   * |   Object   |   ObData   |
   * | ---------- | ---------- |
   * | Meta_A     | Meta_A     |
   * | Meta_A.001 | Meta_A.001 |
   * | Meta_B     | Meta_A     |
   * | Meta_B.001 | Meta_B.001 |
   * </pre>
   *
   * Calling this function with `metaball_src` being `Meta_A.001` will update `Meta_A`, but NOT
   * `Meta_B.001`. So in the 'Meta_B' family, the two metaballs will have unmatching settings now.
   *
   * Solving this case would drastically increase the complexity of this code though, so don't
   * think it would be worth it.
   */
  for (Object *ob_src = static_cast<Object *>(bmain->objects.first);
       ob_src != nullptr && !ID_IS_LINKED(ob_src);) {
    if (ob_src->data != metaball_src) {
      ob_src = static_cast<Object *>(ob_src->id.next);
      continue;
    }

    /* In this code we take advantage of two facts:
     *  - MetaBalls of the same family have the same basis name,
     *  - IDs are sorted by name in their Main listbase.
     * So, all MetaBall objects of the same family are contiguous in bmain list (potentially mixed
     * with non-meta-ball objects with same basis names).
     *
     * Using this, it is possible to process the whole set of meta-balls with a single loop on the
     * whole list of Objects, though additionally going backward on part of the list in some cases.
     */
    Object *ob_iter = nullptr;
    int obactive_nr, ob_nr;
    char obactive_name[MAX_ID_NAME], ob_name[MAX_ID_NAME];
    BLI_split_name_num(obactive_name, &obactive_nr, ob_src->id.name + 2, '.');

    for (ob_iter = static_cast<Object *>(ob_src->id.prev); ob_iter != nullptr;
         ob_iter = static_cast<Object *>(ob_iter->id.prev)) {
      if (ob_iter->id.name[2] != obactive_name[0]) {
        break;
      }
      if (ob_iter->type != OB_MBALL || ob_iter->data == metaball_src) {
        continue;
      }
      BLI_split_name_num(ob_name, &ob_nr, ob_iter->id.name + 2, '.');
      if (!STREQ(obactive_name, ob_name)) {
        break;
      }

      mball_data_properties_copy(static_cast<MetaBall *>(ob_iter->data), metaball_src);
    }

    for (ob_iter = static_cast<Object *>(ob_src->id.next); ob_iter != nullptr;
         ob_iter = static_cast<Object *>(ob_iter->id.next)) {
      if (ob_iter->id.name[2] != obactive_name[0] || ID_IS_LINKED(ob_iter)) {
        break;
      }
      if (ob_iter->type != OB_MBALL || ob_iter->data == metaball_src) {
        continue;
      }
      BLI_split_name_num(ob_name, &ob_nr, ob_iter->id.name + 2, '.');
      if (!STREQ(obactive_name, ob_name)) {
        break;
      }

      mball_data_properties_copy(static_cast<MetaBall *>(ob_iter->data), metaball_src);
    }

    ob_src = ob_iter;
  }
}

Object *BKE_mball_basis_find(Scene *scene, Object *object)
{
  Object *bob = object;
  int basisnr, obnr;
  char basisname[MAX_ID_NAME], obname[MAX_ID_NAME];

  BLI_split_name_num(basisname, &basisnr, object->id.name + 2, '.');

  LISTBASE_FOREACH (ViewLayer *, view_layer, &scene->view_layers) {
    LISTBASE_FOREACH (Base *, base, &view_layer->object_bases) {
      Object *ob = base->object;
      if ((ob->type == OB_MBALL) && !(base->flag & BASE_FROM_DUPLI)) {
        if (ob != bob) {
          BLI_split_name_num(obname, &obnr, ob->id.name + 2, '.');

          /* Object ob has to be in same "group" ... it means,
           * that it has to have same base of its name. */
          if (STREQ(obname, basisname)) {
            if (obnr < basisnr) {
              object = ob;
              basisnr = obnr;
            }
          }
        }
      }
    }
  }

  return object;
}

bool BKE_mball_minmax_ex(
    const MetaBall *mb, float min[3], float max[3], const float obmat[4][4], const short flag)
{
  const float scale = obmat ? mat4_to_scale(obmat) : 1.0f;
  bool changed = false;
  float centroid[3], vec[3];

  INIT_MINMAX(min, max);

  LISTBASE_FOREACH (const MetaElem *, ml, &mb->elems) {
    if ((ml->flag & flag) == flag) {
      const float scale_mb = (ml->rad * 0.5f) * scale;

      if (obmat) {
        mul_v3_m4v3(centroid, obmat, &ml->x);
      }
      else {
        copy_v3_v3(centroid, &ml->x);
      }

      /* TODO(@campbellbarton): non circle shapes cubes etc, probably nobody notices. */
      for (int i = -1; i != 3; i += 2) {
        copy_v3_v3(vec, centroid);
        add_v3_fl(vec, scale_mb * i);
        minmax_v3v3_v3(min, max, vec);
      }
      changed = true;
    }
  }

  return changed;
}

bool BKE_mball_minmax(const MetaBall *mb, float min[3], float max[3])
{
  INIT_MINMAX(min, max);

  LISTBASE_FOREACH (const MetaElem *, ml, &mb->elems) {
    minmax_v3v3_v3(min, max, &ml->x);
  }

  return (BLI_listbase_is_empty(&mb->elems) == false);
}

bool BKE_mball_center_median(const MetaBall *mb, float r_cent[3])
{
  int total = 0;

  zero_v3(r_cent);

  LISTBASE_FOREACH (const MetaElem *, ml, &mb->elems) {
    add_v3_v3(r_cent, &ml->x);
    total++;
  }

  if (total) {
    mul_v3_fl(r_cent, 1.0f / (float)total);
  }

  return (total != 0);
}

bool BKE_mball_center_bounds(const MetaBall *mb, float r_cent[3])
{
  float min[3], max[3];

  if (BKE_mball_minmax(mb, min, max)) {
    mid_v3_v3v3(r_cent, min, max);
    return true;
  }

  return false;
}

void BKE_mball_transform(MetaBall *mb, const float mat[4][4], const bool do_props)
{
  float quat[4];
  const float scale = mat4_to_scale(mat);
  const float scale_sqrt = sqrtf(scale);

  mat4_to_quat(quat, mat);

  LISTBASE_FOREACH (MetaElem *, ml, &mb->elems) {
    mul_m4_v3(mat, &ml->x);
    mul_qt_qtqt(ml->quat, quat, ml->quat);

    if (do_props) {
      ml->rad *= scale;
      /* hrmf, probably elems shouldn't be
       * treating scale differently - campbell */
      if (!MB_TYPE_SIZE_SQUARED(ml->type)) {
        mul_v3_fl(&ml->expx, scale);
      }
      else {
        mul_v3_fl(&ml->expx, scale_sqrt);
      }
    }
  }
}

void BKE_mball_translate(MetaBall *mb, const float offset[3])
{
  LISTBASE_FOREACH (MetaElem *, ml, &mb->elems) {
    add_v3_v3(&ml->x, offset);
  }
}

int BKE_mball_select_count(const MetaBall *mb)
{
  int sel = 0;
  LISTBASE_FOREACH (const MetaElem *, ml, mb->editelems) {
    if (ml->flag & SELECT) {
      sel++;
    }
  }
  return sel;
}

int BKE_mball_select_count_multi(Base **bases, int bases_len)
{
  int sel = 0;
  for (uint ob_index = 0; ob_index < bases_len; ob_index++) {
    const Object *obedit = bases[ob_index]->object;
    const MetaBall *mb = (MetaBall *)obedit->data;
    sel += BKE_mball_select_count(mb);
  }
  return sel;
}

bool BKE_mball_select_all(MetaBall *mb)
{
  bool changed = false;
  LISTBASE_FOREACH (MetaElem *, ml, mb->editelems) {
    if ((ml->flag & SELECT) == 0) {
      ml->flag |= SELECT;
      changed = true;
    }
  }
  return changed;
}

bool BKE_mball_select_all_multi_ex(Base **bases, int bases_len)
{
  bool changed_multi = false;
  for (uint ob_index = 0; ob_index < bases_len; ob_index++) {
    Object *obedit = bases[ob_index]->object;
    MetaBall *mb = static_cast<MetaBall *>(obedit->data);
    changed_multi |= BKE_mball_select_all(mb);
  }
  return changed_multi;
}

bool BKE_mball_deselect_all(MetaBall *mb)
{
  bool changed = false;
  LISTBASE_FOREACH (MetaElem *, ml, mb->editelems) {
    if ((ml->flag & SELECT) != 0) {
      ml->flag &= ~SELECT;
      changed = true;
    }
  }
  return changed;
}

bool BKE_mball_deselect_all_multi_ex(Base **bases, int bases_len)
{
  bool changed_multi = false;
  for (uint ob_index = 0; ob_index < bases_len; ob_index++) {
    Object *obedit = bases[ob_index]->object;
    MetaBall *mb = static_cast<MetaBall *>(obedit->data);
    changed_multi |= BKE_mball_deselect_all(mb);
    DEG_id_tag_update(&mb->id, ID_RECALC_SELECT);
  }
  return changed_multi;
}

bool BKE_mball_select_swap(MetaBall *mb)
{
  bool changed = false;
  LISTBASE_FOREACH (MetaElem *, ml, mb->editelems) {
    ml->flag ^= SELECT;
    changed = true;
  }
  return changed;
}

bool BKE_mball_select_swap_multi_ex(Base **bases, int bases_len)
{
  bool changed_multi = false;
  for (uint ob_index = 0; ob_index < bases_len; ob_index++) {
    Object *obedit = bases[ob_index]->object;
    MetaBall *mb = (MetaBall *)obedit->data;
    changed_multi |= BKE_mball_select_swap(mb);
  }
  return changed_multi;
}

/* **** Depsgraph evaluation **** */

void BKE_mball_data_update(Depsgraph *depsgraph, Scene *scene, Object *ob)
{
  BLI_assert(ob->type == OB_MBALL);

  BKE_object_free_derived_caches(ob);

  const Object *basis_object = BKE_mball_basis_find(scene, ob);
  if (ob != basis_object) {
    return;
  }

  Mesh *mesh = BKE_mball_polygonize(depsgraph, scene, ob);
  if (mesh == NULL) {
    return;
  }

  const MetaBall *mball = static_cast<MetaBall *>(ob->data);
  mesh->mat = static_cast<Material **>(MEM_dupallocN(mball->mat));
  mesh->totcol = mball->totcol;

  if (ob->parent && ob->parent->type == OB_LATTICE && ob->partype == PARSKEL) {
    int verts_num;
    float(*positions)[3] = BKE_mesh_vert_coords_alloc(mesh, &verts_num);
    BKE_lattice_deform_coords(ob->parent, ob, positions, verts_num, 0, NULL, 1.0f);
    BKE_mesh_vert_coords_apply(mesh, positions);
    MEM_freeN(positions);
  }

  ob->runtime.geometry_set_eval = new GeometrySet(GeometrySet::create_with_mesh(mesh));

  if (ob->runtime.bb == NULL) {
    ob->runtime.bb = MEM_cnew<BoundBox>(__func__);
  }
  blender::float3 min(std::numeric_limits<float>::max());
  blender::float3 max(-std::numeric_limits<float>::max());
  if (!ob->runtime.geometry_set_eval->compute_boundbox_without_instances(&min, &max)) {
    min = blender::float3(0);
    max = blender::float3(0);
  }
  BKE_boundbox_init_from_minmax(ob->runtime.bb, min, max);
};
