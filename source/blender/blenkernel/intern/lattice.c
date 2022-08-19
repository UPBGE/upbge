/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2001-2002 NaN Holding BV. All rights reserved. */

/** \file
 * \ingroup bke
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "MEM_guardedalloc.h"

#include "BLI_bitmap.h"
#include "BLI_listbase.h"
#include "BLI_math.h"
#include "BLI_utildefines.h"

#include "BLT_translation.h"

/* Allow using deprecated functionality for .blend file I/O. */
#define DNA_DEPRECATED_ALLOW

#include "DNA_curve_types.h"
#include "DNA_defaults.h"
#include "DNA_key_types.h"
#include "DNA_lattice_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BKE_anim_data.h"
#include "BKE_curve.h"
#include "BKE_deform.h"
#include "BKE_displist.h"
#include "BKE_idtype.h"
#include "BKE_lattice.h"
#include "BKE_lib_id.h"
#include "BKE_lib_query.h"
#include "BKE_main.h"
#include "BKE_modifier.h"
#include "BKE_object.h"

#include "DEG_depsgraph_query.h"

#include "BLO_read_write.h"

static void lattice_init_data(ID *id)
{
  Lattice *lattice = (Lattice *)id;

  BLI_assert(MEMCMP_STRUCT_AFTER_IS_ZERO(lattice, id));

  MEMCPY_STRUCT_AFTER(lattice, DNA_struct_default_get(Lattice), id);

  lattice->def = MEM_callocN(sizeof(BPoint), "lattvert"); /* temporary */
  BKE_lattice_resize(lattice, 2, 2, 2, NULL);             /* creates a uniform lattice */
}

static void lattice_copy_data(Main *bmain, ID *id_dst, const ID *id_src, const int flag)
{
  Lattice *lattice_dst = (Lattice *)id_dst;
  const Lattice *lattice_src = (const Lattice *)id_src;

  lattice_dst->def = MEM_dupallocN(lattice_src->def);

  if (lattice_src->key && (flag & LIB_ID_COPY_SHAPEKEY)) {
    BKE_id_copy_ex(bmain, &lattice_src->key->id, (ID **)&lattice_dst->key, flag);
    /* XXX This is not nice, we need to make BKE_id_copy_ex fully re-entrant... */
    lattice_dst->key->from = &lattice_dst->id;
  }

  BKE_defgroup_copy_list(&lattice_dst->vertex_group_names, &lattice_src->vertex_group_names);

  if (lattice_src->dvert) {
    int tot = lattice_src->pntsu * lattice_src->pntsv * lattice_src->pntsw;
    lattice_dst->dvert = MEM_mallocN(sizeof(MDeformVert) * tot, "Lattice MDeformVert");
    BKE_defvert_array_copy(lattice_dst->dvert, lattice_src->dvert, tot);
  }

  lattice_dst->editlatt = NULL;
  lattice_dst->batch_cache = NULL;
}

static void lattice_free_data(ID *id)
{
  Lattice *lattice = (Lattice *)id;

  BKE_lattice_batch_cache_free(lattice);

  BLI_freelistN(&lattice->vertex_group_names);

  MEM_SAFE_FREE(lattice->def);
  if (lattice->dvert) {
    BKE_defvert_array_free(lattice->dvert, lattice->pntsu * lattice->pntsv * lattice->pntsw);
    lattice->dvert = NULL;
  }
  if (lattice->editlatt) {
    Lattice *editlt = lattice->editlatt->latt;

    if (editlt->def) {
      MEM_freeN(editlt->def);
    }
    if (editlt->dvert) {
      BKE_defvert_array_free(editlt->dvert, lattice->pntsu * lattice->pntsv * lattice->pntsw);
    }

    MEM_freeN(editlt);
    MEM_freeN(lattice->editlatt);
    lattice->editlatt = NULL;
  }
}

static void lattice_foreach_id(ID *id, LibraryForeachIDData *data)
{
  Lattice *lattice = (Lattice *)id;
  BKE_LIB_FOREACHID_PROCESS_IDSUPER(data, lattice->key, IDWALK_CB_USER);
}

static void lattice_blend_write(BlendWriter *writer, ID *id, const void *id_address)
{
  Lattice *lt = (Lattice *)id;

  /* Clean up, important in undo case to reduce false detection of changed datablocks. */
  lt->editlatt = NULL;
  lt->batch_cache = NULL;

  /* write LibData */
  BLO_write_id_struct(writer, Lattice, id_address, &lt->id);
  BKE_id_blend_write(writer, &lt->id);

  /* write animdata */
  if (lt->adt) {
    BKE_animdata_blend_write(writer, lt->adt);
  }

  /* direct data */
  BLO_write_struct_array(writer, BPoint, lt->pntsu * lt->pntsv * lt->pntsw, lt->def);

  BKE_defbase_blend_write(writer, &lt->vertex_group_names);
  BKE_defvert_blend_write(writer, lt->pntsu * lt->pntsv * lt->pntsw, lt->dvert);
}

static void lattice_blend_read_data(BlendDataReader *reader, ID *id)
{
  Lattice *lt = (Lattice *)id;
  BLO_read_data_address(reader, &lt->def);

  BLO_read_data_address(reader, &lt->dvert);
  BKE_defvert_blend_read(reader, lt->pntsu * lt->pntsv * lt->pntsw, lt->dvert);
  BLO_read_list(reader, &lt->vertex_group_names);

  lt->editlatt = NULL;
  lt->batch_cache = NULL;

  BLO_read_data_address(reader, &lt->adt);
  BKE_animdata_blend_read_data(reader, lt->adt);
}

static void lattice_blend_read_lib(BlendLibReader *reader, ID *id)
{
  Lattice *lt = (Lattice *)id;
  BLO_read_id_address(reader, lt->id.lib, &lt->ipo);  // XXX deprecated - old animation system
  BLO_read_id_address(reader, lt->id.lib, &lt->key);
}

static void lattice_blend_read_expand(BlendExpander *expander, ID *id)
{
  Lattice *lt = (Lattice *)id;
  BLO_expand(expander, lt->ipo);  // XXX deprecated - old animation system
  BLO_expand(expander, lt->key);
}

IDTypeInfo IDType_ID_LT = {
    .id_code = ID_LT,
    .id_filter = FILTER_ID_LT,
    .main_listbase_index = INDEX_ID_LT,
    .struct_size = sizeof(Lattice),
    .name = "Lattice",
    .name_plural = "lattices",
    .translation_context = BLT_I18NCONTEXT_ID_LATTICE,
    .flags = IDTYPE_FLAGS_APPEND_IS_REUSABLE,
    .asset_type_info = NULL,

    .init_data = lattice_init_data,
    .copy_data = lattice_copy_data,
    .free_data = lattice_free_data,
    .make_local = NULL,
    .foreach_id = lattice_foreach_id,
    .foreach_cache = NULL,
    .foreach_path = NULL,
    .owner_get = NULL,

    .blend_write = lattice_blend_write,
    .blend_read_data = lattice_blend_read_data,
    .blend_read_lib = lattice_blend_read_lib,
    .blend_read_expand = lattice_blend_read_expand,

    .blend_read_undo_preserve = NULL,

    .lib_override_apply_post = NULL,
};

int BKE_lattice_index_from_uvw(Lattice *lt, const int u, const int v, const int w)
{
  const int totu = lt->pntsu;
  const int totv = lt->pntsv;

  return (w * (totu * totv) + (v * totu) + u);
}

void BKE_lattice_index_to_uvw(Lattice *lt, const int index, int *r_u, int *r_v, int *r_w)
{
  const int totu = lt->pntsu;
  const int totv = lt->pntsv;

  *r_u = (index % totu);
  *r_v = (index / totu) % totv;
  *r_w = (index / (totu * totv));
}

int BKE_lattice_index_flip(
    Lattice *lt, const int index, const bool flip_u, const bool flip_v, const bool flip_w)
{
  int u, v, w;

  BKE_lattice_index_to_uvw(lt, index, &u, &v, &w);

  if (flip_u) {
    u = (lt->pntsu - 1) - u;
  }

  if (flip_v) {
    v = (lt->pntsv - 1) - v;
  }

  if (flip_w) {
    w = (lt->pntsw - 1) - w;
  }

  return BKE_lattice_index_from_uvw(lt, u, v, w);
}

void BKE_lattice_bitmap_from_flag(
    Lattice *lt, BLI_bitmap *bitmap, const uint8_t flag, const bool clear, const bool respecthide)
{
  const unsigned int tot = lt->pntsu * lt->pntsv * lt->pntsw;
  BPoint *bp;

  bp = lt->def;
  for (int i = 0; i < tot; i++, bp++) {
    if ((bp->f1 & flag) && (!respecthide || !bp->hide)) {
      BLI_BITMAP_ENABLE(bitmap, i);
    }
    else {
      if (clear) {
        BLI_BITMAP_DISABLE(bitmap, i);
      }
    }
  }
}

void calc_lat_fudu(int flag, int res, float *r_fu, float *r_du)
{
  if (res == 1) {
    *r_fu = 0.0;
    *r_du = 0.0;
  }
  else if (flag & LT_GRID) {
    *r_fu = -0.5f * (res - 1);
    *r_du = 1.0f;
  }
  else {
    *r_fu = -1.0f;
    *r_du = 2.0f / (res - 1);
  }
}

void BKE_lattice_resize(Lattice *lt, int uNew, int vNew, int wNew, Object *ltOb)
{
  BPoint *bp;
  int i, u, v, w;
  float fu, fv, fw, uc, vc, wc, du = 0.0, dv = 0.0, dw = 0.0;
  float *co, (*vert_coords)[3] = NULL;

  /* vertex weight groups are just freed all for now */
  if (lt->dvert) {
    BKE_defvert_array_free(lt->dvert, lt->pntsu * lt->pntsv * lt->pntsw);
    lt->dvert = NULL;
  }

  while (uNew * vNew * wNew > 32000) {
    if (uNew >= vNew && uNew >= wNew) {
      uNew--;
    }
    else if (vNew >= uNew && vNew >= wNew) {
      vNew--;
    }
    else {
      wNew--;
    }
  }

  vert_coords = MEM_mallocN(sizeof(*vert_coords) * uNew * vNew * wNew, "tmp_vcos");

  calc_lat_fudu(lt->flag, uNew, &fu, &du);
  calc_lat_fudu(lt->flag, vNew, &fv, &dv);
  calc_lat_fudu(lt->flag, wNew, &fw, &dw);

  /* If old size is different than resolution changed in interface,
   * try to do clever reinit of points. Pretty simply idea, we just
   * deform new verts by old lattice, but scaling them to match old
   * size first.
   */
  if (ltOb) {
    const float default_size = 1.0;

    if (uNew != 1) {
      fu = -default_size / 2.0;
      du = default_size / (uNew - 1);
    }

    if (vNew != 1) {
      fv = -default_size / 2.0;
      dv = default_size / (vNew - 1);
    }

    if (wNew != 1) {
      fw = -default_size / 2.0;
      dw = default_size / (wNew - 1);
    }
  }

  co = vert_coords[0];
  for (w = 0, wc = fw; w < wNew; w++, wc += dw) {
    for (v = 0, vc = fv; v < vNew; v++, vc += dv) {
      for (u = 0, uc = fu; u < uNew; u++, co += 3, uc += du) {
        co[0] = uc;
        co[1] = vc;
        co[2] = wc;
      }
    }
  }

  if (ltOb) {
    float mat[4][4];
    int typeu = lt->typeu, typev = lt->typev, typew = lt->typew;

    /* works best if we force to linear type (endpoints match) */
    lt->typeu = lt->typev = lt->typew = KEY_LINEAR;

    if (ltOb->runtime.curve_cache) {
      /* prevent using deformed locations */
      BKE_displist_free(&ltOb->runtime.curve_cache->disp);
    }

    copy_m4_m4(mat, ltOb->obmat);
    unit_m4(ltOb->obmat);
    BKE_lattice_deform_coords(ltOb, NULL, vert_coords, uNew * vNew * wNew, 0, NULL, 1.0f);
    copy_m4_m4(ltOb->obmat, mat);

    lt->typeu = typeu;
    lt->typev = typev;
    lt->typew = typew;
  }

  lt->fu = fu;
  lt->fv = fv;
  lt->fw = fw;
  lt->du = du;
  lt->dv = dv;
  lt->dw = dw;

  lt->pntsu = uNew;
  lt->pntsv = vNew;
  lt->pntsw = wNew;

  lt->actbp = LT_ACTBP_NONE;
  MEM_freeN(lt->def);
  lt->def = MEM_callocN(lt->pntsu * lt->pntsv * lt->pntsw * sizeof(BPoint), "lattice bp");

  bp = lt->def;

  for (i = 0; i < lt->pntsu * lt->pntsv * lt->pntsw; i++, bp++) {
    copy_v3_v3(bp->vec, vert_coords[i]);
  }

  MEM_freeN(vert_coords);
}

Lattice *BKE_lattice_add(Main *bmain, const char *name)
{
  Lattice *lt;

  lt = BKE_id_new(bmain, ID_LT, name);

  return lt;
}

static BPoint *latt_bp(Lattice *lt, int u, int v, int w)
{
  return &lt->def[BKE_lattice_index_from_uvw(lt, u, v, w)];
}

void outside_lattice(Lattice *lt)
{
  BPoint *bp, *bp1, *bp2;
  int u, v, w;
  float fac1, du = 0.0, dv = 0.0, dw = 0.0;

  if (lt->flag & LT_OUTSIDE) {
    bp = lt->def;

    if (lt->pntsu > 1) {
      du = 1.0f / ((float)lt->pntsu - 1);
    }
    if (lt->pntsv > 1) {
      dv = 1.0f / ((float)lt->pntsv - 1);
    }
    if (lt->pntsw > 1) {
      dw = 1.0f / ((float)lt->pntsw - 1);
    }

    for (w = 0; w < lt->pntsw; w++) {

      for (v = 0; v < lt->pntsv; v++) {

        for (u = 0; u < lt->pntsu; u++, bp++) {
          if (u == 0 || v == 0 || w == 0 || u == lt->pntsu - 1 || v == lt->pntsv - 1 ||
              w == lt->pntsw - 1) {
            /* pass */
          }
          else {
            bp->hide = 1;
            bp->f1 &= ~SELECT;

            /* U extrema. */
            bp1 = latt_bp(lt, 0, v, w);
            bp2 = latt_bp(lt, lt->pntsu - 1, v, w);

            fac1 = du * u;
            bp->vec[0] = (1.0f - fac1) * bp1->vec[0] + fac1 * bp2->vec[0];
            bp->vec[1] = (1.0f - fac1) * bp1->vec[1] + fac1 * bp2->vec[1];
            bp->vec[2] = (1.0f - fac1) * bp1->vec[2] + fac1 * bp2->vec[2];

            /* V extrema. */
            bp1 = latt_bp(lt, u, 0, w);
            bp2 = latt_bp(lt, u, lt->pntsv - 1, w);

            fac1 = dv * v;
            bp->vec[0] += (1.0f - fac1) * bp1->vec[0] + fac1 * bp2->vec[0];
            bp->vec[1] += (1.0f - fac1) * bp1->vec[1] + fac1 * bp2->vec[1];
            bp->vec[2] += (1.0f - fac1) * bp1->vec[2] + fac1 * bp2->vec[2];

            /* W extrema. */
            bp1 = latt_bp(lt, u, v, 0);
            bp2 = latt_bp(lt, u, v, lt->pntsw - 1);

            fac1 = dw * w;
            bp->vec[0] += (1.0f - fac1) * bp1->vec[0] + fac1 * bp2->vec[0];
            bp->vec[1] += (1.0f - fac1) * bp1->vec[1] + fac1 * bp2->vec[1];
            bp->vec[2] += (1.0f - fac1) * bp1->vec[2] + fac1 * bp2->vec[2];

            mul_v3_fl(bp->vec, 1.0f / 3.0f);
          }
        }
      }
    }
  }
  else {
    bp = lt->def;

    for (w = 0; w < lt->pntsw; w++) {
      for (v = 0; v < lt->pntsv; v++) {
        for (u = 0; u < lt->pntsu; u++, bp++) {
          bp->hide = 0;
        }
      }
    }
  }
}

void BKE_lattice_vert_coords_get(const Lattice *lt, float (*vert_coords)[3])
{
  const int vert_len = lt->pntsu * lt->pntsv * lt->pntsw;
  for (int i = 0; i < vert_len; i++) {
    copy_v3_v3(vert_coords[i], lt->def[i].vec);
  }
}

float (*BKE_lattice_vert_coords_alloc(const Lattice *lt, int *r_vert_len))[3]
{
  const int vert_len = *r_vert_len = lt->pntsu * lt->pntsv * lt->pntsw;
  float(*vert_coords)[3] = MEM_mallocN(sizeof(*vert_coords) * vert_len, __func__);
  BKE_lattice_vert_coords_get(lt, vert_coords);
  return vert_coords;
}

void BKE_lattice_vert_coords_apply_with_mat4(struct Lattice *lt,
                                             const float (*vert_coords)[3],
                                             const float mat[4][4])
{
  int i, numVerts = lt->pntsu * lt->pntsv * lt->pntsw;
  for (i = 0; i < numVerts; i++) {
    mul_v3_m4v3(lt->def[i].vec, mat, vert_coords[i]);
  }
}

void BKE_lattice_vert_coords_apply(Lattice *lt, const float (*vert_coords)[3])
{
  const int vert_len = lt->pntsu * lt->pntsv * lt->pntsw;
  for (int i = 0; i < vert_len; i++) {
    copy_v3_v3(lt->def[i].vec, vert_coords[i]);
  }
}

void BKE_lattice_modifiers_calc(struct Depsgraph *depsgraph, Scene *scene, Object *ob)
{
  BKE_object_free_derived_caches(ob);
  if (ob->runtime.curve_cache == NULL) {
    ob->runtime.curve_cache = MEM_callocN(sizeof(CurveCache), "CurveCache for lattice");
  }

  Lattice *lt = ob->data;
  VirtualModifierData virtualModifierData;
  ModifierData *md = BKE_modifiers_get_virtual_modifierlist(ob, &virtualModifierData);
  float(*vert_coords)[3] = NULL;
  int numVerts;
  const bool is_editmode = (lt->editlatt != NULL);
  const ModifierEvalContext mectx = {depsgraph, ob, 0};

  for (; md; md = md->next) {
    const ModifierTypeInfo *mti = BKE_modifier_get_info(md->type);

    if (!(mti->flags & eModifierTypeFlag_AcceptsVertexCosOnly)) {
      continue;
    }
    if (!(md->mode & eModifierMode_Realtime)) {
      continue;
    }
    if (is_editmode && !(md->mode & eModifierMode_Editmode)) {
      continue;
    }
    if (mti->isDisabled && mti->isDisabled(scene, md, 0)) {
      continue;
    }
    if (mti->type != eModifierTypeType_OnlyDeform) {
      continue;
    }

    if (vert_coords == NULL) {
      /* Get either the edit-mode or regular lattice, whichever is in use now. */
      const Lattice *effective_lattice = BKE_object_get_lattice(ob);
      vert_coords = BKE_lattice_vert_coords_alloc(effective_lattice, &numVerts);
    }

    mti->deformVerts(md, &mectx, NULL, vert_coords, numVerts);
  }

  if (vert_coords == NULL) {
    return;
  }

  Lattice *lt_eval = BKE_object_get_evaluated_lattice(ob);
  if (lt_eval == NULL) {
    BKE_id_copy_ex(NULL, &lt->id, (ID **)&lt_eval, LIB_ID_COPY_LOCALIZE);
    BKE_object_eval_assign_data(ob, &lt_eval->id, true);
  }

  BKE_lattice_vert_coords_apply(lt_eval, vert_coords);
  MEM_freeN(vert_coords);
}

struct MDeformVert *BKE_lattice_deform_verts_get(const struct Object *oblatt)
{
  BLI_assert(oblatt->type == OB_LATTICE);
  Lattice *lt = BKE_object_get_lattice(oblatt);
  return lt->dvert;
}

struct BPoint *BKE_lattice_active_point_get(Lattice *lt)
{
  BLI_assert(GS(lt->id.name) == ID_LT);

  if (lt->editlatt) {
    lt = lt->editlatt->latt;
  }

  BLI_assert(lt->actbp < lt->pntsu * lt->pntsv * lt->pntsw);

  if ((lt->actbp != LT_ACTBP_NONE) && (lt->actbp < lt->pntsu * lt->pntsv * lt->pntsw)) {
    return &lt->def[lt->actbp];
  }

  return NULL;
}

void BKE_lattice_center_median(Lattice *lt, float cent[3])
{
  int i, numVerts;

  if (lt->editlatt) {
    lt = lt->editlatt->latt;
  }
  numVerts = lt->pntsu * lt->pntsv * lt->pntsw;

  zero_v3(cent);

  for (i = 0; i < numVerts; i++) {
    add_v3_v3(cent, lt->def[i].vec);
  }

  mul_v3_fl(cent, 1.0f / (float)numVerts);
}

static void boundbox_lattice(Object *ob)
{
  BoundBox *bb;
  Lattice *lt;
  float min[3], max[3];

  if (ob->runtime.bb == NULL) {
    ob->runtime.bb = MEM_callocN(sizeof(BoundBox), "Lattice boundbox");
  }

  bb = ob->runtime.bb;
  lt = ob->data;

  INIT_MINMAX(min, max);
  BKE_lattice_minmax_dl(ob, lt, min, max);
  BKE_boundbox_init_from_minmax(bb, min, max);

  bb->flag &= ~BOUNDBOX_DIRTY;
}

BoundBox *BKE_lattice_boundbox_get(Object *ob)
{
  boundbox_lattice(ob);

  return ob->runtime.bb;
}

void BKE_lattice_minmax_dl(Object *ob, Lattice *lt, float min[3], float max[3])
{
  DispList *dl = ob->runtime.curve_cache ?
                     BKE_displist_find(&ob->runtime.curve_cache->disp, DL_VERTS) :
                     NULL;

  if (!dl) {
    BKE_lattice_minmax(lt, min, max);
  }
  else {
    int i, numVerts;

    if (lt->editlatt) {
      lt = lt->editlatt->latt;
    }
    numVerts = lt->pntsu * lt->pntsv * lt->pntsw;

    for (i = 0; i < numVerts; i++) {
      minmax_v3v3_v3(min, max, &dl->verts[i * 3]);
    }
  }
}

void BKE_lattice_minmax(Lattice *lt, float min[3], float max[3])
{
  int i, numVerts;

  if (lt->editlatt) {
    lt = lt->editlatt->latt;
  }
  numVerts = lt->pntsu * lt->pntsv * lt->pntsw;

  for (i = 0; i < numVerts; i++) {
    minmax_v3v3_v3(min, max, lt->def[i].vec);
  }
}

void BKE_lattice_center_bounds(Lattice *lt, float cent[3])
{
  float min[3], max[3];

  INIT_MINMAX(min, max);

  BKE_lattice_minmax(lt, min, max);
  mid_v3_v3v3(cent, min, max);
}

void BKE_lattice_transform(Lattice *lt, const float mat[4][4], bool do_keys)
{
  BPoint *bp = lt->def;
  int i = lt->pntsu * lt->pntsv * lt->pntsw;

  while (i--) {
    mul_m4_v3(mat, bp->vec);
    bp++;
  }

  if (do_keys && lt->key) {
    KeyBlock *kb;

    for (kb = lt->key->block.first; kb; kb = kb->next) {
      float *fp = kb->data;
      for (i = kb->totelem; i--; fp += 3) {
        mul_m4_v3(mat, fp);
      }
    }
  }
}

void BKE_lattice_translate(Lattice *lt, const float offset[3], bool do_keys)
{
  int i, numVerts;

  numVerts = lt->pntsu * lt->pntsv * lt->pntsw;

  if (lt->def) {
    for (i = 0; i < numVerts; i++) {
      add_v3_v3(lt->def[i].vec, offset);
    }
  }

  if (lt->editlatt) {
    for (i = 0; i < numVerts; i++) {
      add_v3_v3(lt->editlatt->latt->def[i].vec, offset);
    }
  }

  if (do_keys && lt->key) {
    KeyBlock *kb;

    for (kb = lt->key->block.first; kb; kb = kb->next) {
      float *fp = kb->data;
      for (i = kb->totelem; i--; fp += 3) {
        add_v3_v3(fp, offset);
      }
    }
  }
}

bool BKE_lattice_is_any_selected(const Lattice *lt)
{
  /* Intentionally don't handle 'lt->editlatt' (caller must do this). */
  const BPoint *bp = lt->def;
  int a = lt->pntsu * lt->pntsv * lt->pntsw;
  while (a--) {
    if (bp->hide == 0) {
      if (bp->f1 & SELECT) {
        return true;
      }
    }
    bp++;
  }
  return false;
}

/* **** Depsgraph evaluation **** */

void BKE_lattice_eval_geometry(struct Depsgraph *UNUSED(depsgraph), Lattice *UNUSED(latt))
{
}

/* Draw Engine */
void (*BKE_lattice_batch_cache_dirty_tag_cb)(Lattice *lt, int mode) = NULL;
void (*BKE_lattice_batch_cache_free_cb)(Lattice *lt) = NULL;

void BKE_lattice_batch_cache_dirty_tag(Lattice *lt, int mode)
{
  if (lt->batch_cache) {
    BKE_lattice_batch_cache_dirty_tag_cb(lt, mode);
  }
}
void BKE_lattice_batch_cache_free(Lattice *lt)
{
  if (lt->batch_cache) {
    BKE_lattice_batch_cache_free_cb(lt);
  }
}
