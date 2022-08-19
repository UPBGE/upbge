/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2001-2002 NaN Holding BV. All rights reserved. */

/** \file
 * \ingroup bke
 */

#include "MEM_guardedalloc.h"

/* Allow using deprecated functionality for .blend file I/O. */
#define DNA_DEPRECATED_ALLOW

#include "DNA_defaults.h"
#include "DNA_key_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_object_types.h"

#include "BLI_bitmap.h"
#include "BLI_edgehash.h"
#include "BLI_endian_switch.h"
#include "BLI_ghash.h"
#include "BLI_hash.h"
#include "BLI_index_range.hh"
#include "BLI_linklist.h"
#include "BLI_listbase.h"
#include "BLI_math.h"
#include "BLI_math_vector.hh"
#include "BLI_memarena.h"
#include "BLI_span.hh"
#include "BLI_string.h"
#include "BLI_task.hh"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "BLT_translation.h"

#include "BKE_anim_data.h"
#include "BKE_attribute.hh"
#include "BKE_bpath.h"
#include "BKE_deform.h"
#include "BKE_editmesh.h"
#include "BKE_global.h"
#include "BKE_idtype.h"
#include "BKE_key.h"
#include "BKE_lib_id.h"
#include "BKE_lib_query.h"
#include "BKE_main.h"
#include "BKE_material.h"
#include "BKE_mesh.h"
#include "BKE_mesh_legacy_convert.h" // UPBGE
#include "BKE_mesh_runtime.h"
#include "BKE_mesh_wrapper.h"
#include "BKE_modifier.h"
#include "BKE_multires.h"
#include "BKE_object.h"

#include "PIL_time.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_query.h"

#include "BLO_read_write.h"

using blender::float3;
using blender::MutableSpan;
using blender::VArray;
using blender::Vector;

static void mesh_clear_geometry(Mesh *mesh);
static void mesh_tessface_clear_intern(Mesh *mesh, int free_customdata);

static void mesh_init_data(ID *id)
{
  Mesh *mesh = (Mesh *)id;

  BLI_assert(MEMCMP_STRUCT_AFTER_IS_ZERO(mesh, id));

  MEMCPY_STRUCT_AFTER(mesh, DNA_struct_default_get(Mesh), id);

  CustomData_reset(&mesh->vdata);
  CustomData_reset(&mesh->edata);
  CustomData_reset(&mesh->fdata);
  CustomData_reset(&mesh->pdata);
  CustomData_reset(&mesh->ldata);

  BKE_mesh_runtime_init_data(mesh);

  /* A newly created mesh does not have normals, so tag them dirty. This will be cleared
   * by #BKE_mesh_vertex_normals_clear_dirty or #BKE_mesh_poly_normals_ensure. */
  BKE_mesh_normals_tag_dirty(mesh);

  mesh->face_sets_color_seed = BLI_hash_int(PIL_check_seconds_timer_i() & UINT_MAX);
}

static void mesh_copy_data(Main *bmain, ID *id_dst, const ID *id_src, const int flag)
{
  Mesh *mesh_dst = (Mesh *)id_dst;
  const Mesh *mesh_src = (const Mesh *)id_src;

  BKE_mesh_runtime_reset_on_copy(mesh_dst, flag);
  if ((mesh_src->id.tag & LIB_TAG_NO_MAIN) == 0) {
    /* This is a direct copy of a main mesh, so for now it has the same topology. */
    mesh_dst->runtime.deformed_only = true;
  }
  /* This option is set for run-time meshes that have been copied from the current objects mode.
   * Currently this is used for edit-mesh although it could be used for sculpt or other
   * kinds of data specific to an objects mode.
   *
   * The flag signals that the mesh hasn't been modified from the data that generated it,
   * allowing us to use the object-mode data for drawing.
   *
   * While this could be the callers responsibility, keep here since it's
   * highly unlikely we want to create a duplicate and not use it for drawing. */
  mesh_dst->runtime.is_original_bmesh = false;

  /* Only do tessface if we have no polys. */
  const bool do_tessface = ((mesh_src->totface != 0) && (mesh_src->totpoly == 0));

  CustomData_MeshMasks mask = CD_MASK_MESH;

  if (mesh_src->id.tag & LIB_TAG_NO_MAIN) {
    /* For copies in depsgraph, keep data like #CD_ORIGINDEX and #CD_ORCO. */
    CustomData_MeshMasks_update(&mask, &CD_MASK_DERIVEDMESH);
  }

  mesh_dst->mat = (Material **)MEM_dupallocN(mesh_src->mat);

  BKE_defgroup_copy_list(&mesh_dst->vertex_group_names, &mesh_src->vertex_group_names);

  const eCDAllocType alloc_type = (flag & LIB_ID_COPY_CD_REFERENCE) ? CD_REFERENCE : CD_DUPLICATE;
  CustomData_copy(&mesh_src->vdata, &mesh_dst->vdata, mask.vmask, alloc_type, mesh_dst->totvert);
  CustomData_copy(&mesh_src->edata, &mesh_dst->edata, mask.emask, alloc_type, mesh_dst->totedge);
  CustomData_copy(&mesh_src->ldata, &mesh_dst->ldata, mask.lmask, alloc_type, mesh_dst->totloop);
  CustomData_copy(&mesh_src->pdata, &mesh_dst->pdata, mask.pmask, alloc_type, mesh_dst->totpoly);
  if (do_tessface) {
    CustomData_copy(&mesh_src->fdata, &mesh_dst->fdata, mask.fmask, alloc_type, mesh_dst->totface);
  }
  else {
    mesh_tessface_clear_intern(mesh_dst, false);
  }

  BKE_mesh_update_customdata_pointers(mesh_dst, do_tessface);

  mesh_dst->cd_flag = mesh_src->cd_flag;

  mesh_dst->edit_mesh = nullptr;

  mesh_dst->mselect = (MSelect *)MEM_dupallocN(mesh_dst->mselect);

  /* Set normal layers dirty. They should be dirty by default on new meshes anyway, but being
   * explicit about it is safer. Alternatively normal layers could be copied if they aren't dirty,
   * avoiding recomputation in some cases. However, a copied mesh is often changed anyway, so that
   * idea is not clearly better. With proper reference counting, all custom data layers could be
   * copied as the cost would be much lower. */
  BKE_mesh_normals_tag_dirty(mesh_dst);

  /* TODO: Do we want to add flag to prevent this? */
  if (mesh_src->key && (flag & LIB_ID_COPY_SHAPEKEY)) {
    BKE_id_copy_ex(bmain, &mesh_src->key->id, (ID **)&mesh_dst->key, flag);
    /* XXX This is not nice, we need to make BKE_id_copy_ex fully re-entrant... */
    mesh_dst->key->from = &mesh_dst->id;
  }

  BKE_mesh_assert_normals_dirty_or_calculated(mesh_dst);
}

void BKE_mesh_free_editmesh(struct Mesh *mesh)
{
  if (mesh->edit_mesh == nullptr) {
    return;
  }

  if (mesh->edit_mesh->is_shallow_copy == false) {
    BKE_editmesh_free_data(mesh->edit_mesh);
  }
  MEM_freeN(mesh->edit_mesh);
  mesh->edit_mesh = nullptr;
}

static void mesh_free_data(ID *id)
{
  Mesh *mesh = (Mesh *)id;

  BLI_freelistN(&mesh->vertex_group_names);

  BKE_mesh_free_editmesh(mesh);

  BKE_mesh_runtime_free_data(mesh);
  mesh_clear_geometry(mesh);
  MEM_SAFE_FREE(mesh->mat);
}

static void mesh_foreach_id(ID *id, LibraryForeachIDData *data)
{
  Mesh *mesh = (Mesh *)id;
  BKE_LIB_FOREACHID_PROCESS_IDSUPER(data, mesh->texcomesh, IDWALK_CB_NEVER_SELF);
  BKE_LIB_FOREACHID_PROCESS_IDSUPER(data, mesh->key, IDWALK_CB_USER);
  for (int i = 0; i < mesh->totcol; i++) {
    BKE_LIB_FOREACHID_PROCESS_IDSUPER(data, mesh->mat[i], IDWALK_CB_USER);
  }
}

static void mesh_foreach_path(ID *id, BPathForeachPathData *bpath_data)
{
  Mesh *me = (Mesh *)id;
  if (me->ldata.external) {
    BKE_bpath_foreach_path_fixed_process(bpath_data, me->ldata.external->filepath);
  }
}

static void mesh_blend_write(BlendWriter *writer, ID *id, const void *id_address)
{
  Mesh *mesh = (Mesh *)id;
  const bool is_undo = BLO_write_is_undo(writer);

  Vector<CustomDataLayer, 16> vert_layers;
  Vector<CustomDataLayer, 16> edge_layers;
  Vector<CustomDataLayer, 16> loop_layers;
  Vector<CustomDataLayer, 16> poly_layers;

  /* cache only - don't write */
  mesh->mface = nullptr;
  mesh->totface = 0;
  memset(&mesh->fdata, 0, sizeof(mesh->fdata));
  mesh->runtime = blender::dna::shallow_zero_initialize();

  /* Do not store actual geometry data in case this is a library override ID. */
  if (ID_IS_OVERRIDE_LIBRARY(mesh) && !is_undo) {
    mesh->mvert = nullptr;
    mesh->totvert = 0;
    memset(&mesh->vdata, 0, sizeof(mesh->vdata));

    mesh->medge = nullptr;
    mesh->totedge = 0;
    memset(&mesh->edata, 0, sizeof(mesh->edata));

    mesh->mloop = nullptr;
    mesh->totloop = 0;
    memset(&mesh->ldata, 0, sizeof(mesh->ldata));

    mesh->mpoly = nullptr;
    mesh->totpoly = 0;
    memset(&mesh->pdata, 0, sizeof(mesh->pdata));
  }
  else {
    if (!BLO_write_is_undo(writer)) {
      BKE_mesh_legacy_convert_hide_layers_to_flags(mesh);
    }

    CustomData_blend_write_prepare(mesh->vdata, vert_layers, {".hide_vert"});
    CustomData_blend_write_prepare(mesh->edata, edge_layers, {".hide_edge"});
    CustomData_blend_write_prepare(mesh->ldata, loop_layers);
    CustomData_blend_write_prepare(mesh->pdata, poly_layers, {".hide_poly"});
  }

  BLO_write_id_struct(writer, Mesh, id_address, &mesh->id);
  BKE_id_blend_write(writer, &mesh->id);

  /* direct data */
  if (mesh->adt) {
    BKE_animdata_blend_write(writer, mesh->adt);
  }

  BKE_defbase_blend_write(writer, &mesh->vertex_group_names);

  BLO_write_pointer_array(writer, mesh->totcol, mesh->mat);
  BLO_write_raw(writer, sizeof(MSelect) * mesh->totselect, mesh->mselect);

  CustomData_blend_write(
      writer, &mesh->vdata, vert_layers, mesh->totvert, CD_MASK_MESH.vmask, &mesh->id);
  CustomData_blend_write(
      writer, &mesh->edata, edge_layers, mesh->totedge, CD_MASK_MESH.emask, &mesh->id);
  /* fdata is really a dummy - written so slots align */
  CustomData_blend_write(writer, &mesh->fdata, {}, mesh->totface, CD_MASK_MESH.fmask, &mesh->id);
  CustomData_blend_write(
      writer, &mesh->ldata, loop_layers, mesh->totloop, CD_MASK_MESH.lmask, &mesh->id);
  CustomData_blend_write(
      writer, &mesh->pdata, poly_layers, mesh->totpoly, CD_MASK_MESH.pmask, &mesh->id);
}

static void mesh_blend_read_data(BlendDataReader *reader, ID *id)
{
  Mesh *mesh = (Mesh *)id;
  BLO_read_pointer_array(reader, (void **)&mesh->mat);

  BLO_read_data_address(reader, &mesh->mvert);
  BLO_read_data_address(reader, &mesh->medge);
  BLO_read_data_address(reader, &mesh->mface);
  BLO_read_data_address(reader, &mesh->mloop);
  BLO_read_data_address(reader, &mesh->mpoly);
  BLO_read_data_address(reader, &mesh->tface);
  BLO_read_data_address(reader, &mesh->mtface);
  BLO_read_data_address(reader, &mesh->mcol);
  BLO_read_data_address(reader, &mesh->dvert);
  BLO_read_data_address(reader, &mesh->mloopcol);
  BLO_read_data_address(reader, &mesh->mloopuv);
  BLO_read_data_address(reader, &mesh->mselect);

  /* animdata */
  BLO_read_data_address(reader, &mesh->adt);
  BKE_animdata_blend_read_data(reader, mesh->adt);

  /* Normally BKE_defvert_blend_read should be called in CustomData_blend_read,
   * but for backwards compatibility in do_versions to work we do it here. */
  BKE_defvert_blend_read(reader, mesh->totvert, mesh->dvert);
  BLO_read_list(reader, &mesh->vertex_group_names);

  CustomData_blend_read(reader, &mesh->vdata, mesh->totvert);
  CustomData_blend_read(reader, &mesh->edata, mesh->totedge);
  CustomData_blend_read(reader, &mesh->fdata, mesh->totface);
  CustomData_blend_read(reader, &mesh->ldata, mesh->totloop);
  CustomData_blend_read(reader, &mesh->pdata, mesh->totpoly);

  mesh->texflag &= ~ME_AUTOSPACE_EVALUATED;
  mesh->edit_mesh = nullptr;

  mesh->runtime = blender::dna::shallow_zero_initialize();
  BKE_mesh_runtime_init_data(mesh);

  /* happens with old files */
  if (mesh->mselect == nullptr) {
    mesh->totselect = 0;
  }

  if (BLO_read_requires_endian_switch(reader) && mesh->tface) {
    TFace *tf = mesh->tface;
    for (int i = 0; i < mesh->totface; i++, tf++) {
      BLI_endian_switch_uint32_array(tf->col, 4);
    }
  }

  if (!BLO_read_data_is_undo(reader)) {
    BKE_mesh_legacy_convert_flags_to_hide_layers(mesh);
  }

  /* We don't expect to load normals from files, since they are derived data. */
  BKE_mesh_normals_tag_dirty(mesh);
  BKE_mesh_assert_normals_dirty_or_calculated(mesh);
}

static void mesh_blend_read_lib(BlendLibReader *reader, ID *id)
{
  Mesh *me = (Mesh *)id;
  /* this check added for python created meshes */
  if (me->mat) {
    for (int i = 0; i < me->totcol; i++) {
      BLO_read_id_address(reader, me->id.lib, &me->mat[i]);
    }
  }
  else {
    me->totcol = 0;
  }

  BLO_read_id_address(reader, me->id.lib, &me->ipo);  // XXX: deprecated: old anim sys
  BLO_read_id_address(reader, me->id.lib, &me->key);
  BLO_read_id_address(reader, me->id.lib, &me->texcomesh);
}

static void mesh_read_expand(BlendExpander *expander, ID *id)
{
  Mesh *me = (Mesh *)id;
  for (int a = 0; a < me->totcol; a++) {
    BLO_expand(expander, me->mat[a]);
  }

  BLO_expand(expander, me->key);
  BLO_expand(expander, me->texcomesh);
}

IDTypeInfo IDType_ID_ME = {
    /* id_code */ ID_ME,
    /* id_filter */ FILTER_ID_ME,
    /* main_listbase_index */ INDEX_ID_ME,
    /* struct_size */ sizeof(Mesh),
    /* name */ "Mesh",
    /* name_plural */ "meshes",
    /* translation_context */ BLT_I18NCONTEXT_ID_MESH,
    /* flags */ IDTYPE_FLAGS_APPEND_IS_REUSABLE,
    /* asset_type_info */ nullptr,

    /* init_data */ mesh_init_data,
    /* copy_data */ mesh_copy_data,
    /* free_data */ mesh_free_data,
    /* make_local */ nullptr,
    /* foreach_id */ mesh_foreach_id,
    /* foreach_cache */ nullptr,
    /* foreach_path */ mesh_foreach_path,
    /* owner_get */ nullptr,

    /* blend_write */ mesh_blend_write,
    /* blend_read_data */ mesh_blend_read_data,
    /* blend_read_lib */ mesh_blend_read_lib,
    /* blend_read_expand */ mesh_read_expand,

    /* blend_read_undo_preserve */ nullptr,

    /* lib_override_apply_post */ nullptr,
};

enum {
  MESHCMP_DVERT_WEIGHTMISMATCH = 1,
  MESHCMP_DVERT_GROUPMISMATCH,
  MESHCMP_DVERT_TOTGROUPMISMATCH,
  MESHCMP_LOOPCOLMISMATCH,
  MESHCMP_LOOPUVMISMATCH,
  MESHCMP_LOOPMISMATCH,
  MESHCMP_POLYVERTMISMATCH,
  MESHCMP_POLYMISMATCH,
  MESHCMP_EDGEUNKNOWN,
  MESHCMP_VERTCOMISMATCH,
  MESHCMP_CDLAYERS_MISMATCH,
  MESHCMP_ATTRIBUTE_VALUE_MISMATCH,
};

static const char *cmpcode_to_str(int code)
{
  switch (code) {
    case MESHCMP_DVERT_WEIGHTMISMATCH:
      return "Vertex Weight Mismatch";
    case MESHCMP_DVERT_GROUPMISMATCH:
      return "Vertex Group Mismatch";
    case MESHCMP_DVERT_TOTGROUPMISMATCH:
      return "Vertex Doesn't Belong To Same Number Of Groups";
    case MESHCMP_LOOPCOLMISMATCH:
      return "Color Attribute Mismatch";
    case MESHCMP_LOOPUVMISMATCH:
      return "UV Mismatch";
    case MESHCMP_LOOPMISMATCH:
      return "Loop Mismatch";
    case MESHCMP_POLYVERTMISMATCH:
      return "Loop Vert Mismatch In Poly Test";
    case MESHCMP_POLYMISMATCH:
      return "Loop Vert Mismatch";
    case MESHCMP_EDGEUNKNOWN:
      return "Edge Mismatch";
    case MESHCMP_VERTCOMISMATCH:
      return "Vertex Coordinate Mismatch";
    case MESHCMP_CDLAYERS_MISMATCH:
      return "CustomData Layer Count Mismatch";
    case MESHCMP_ATTRIBUTE_VALUE_MISMATCH:
      return "Attribute Value Mismatch";
    default:
      return "Mesh Comparison Code Unknown";
  }
}

/** Thresh is threshold for comparing vertices, UV's, vertex colors, weights, etc. */
static int customdata_compare(
    CustomData *c1, CustomData *c2, const int total_length, Mesh *m1, Mesh *m2, const float thresh)
{
  const float thresh_sq = thresh * thresh;
  CustomDataLayer *l1, *l2;
  int layer_count1 = 0, layer_count2 = 0, j;
  const uint64_t cd_mask_non_generic = CD_MASK_MVERT | CD_MASK_MEDGE | CD_MASK_MPOLY |
                                       CD_MASK_MLOOPUV | CD_MASK_PROP_BYTE_COLOR |
                                       CD_MASK_MDEFORMVERT;
  const uint64_t cd_mask_all_attr = CD_MASK_PROP_ALL | cd_mask_non_generic;

  for (int i = 0; i < c1->totlayer; i++) {
    l1 = &c1->layers[i];
    if ((CD_TYPE_AS_MASK(l1->type) & cd_mask_all_attr) && l1->anonymous_id == nullptr) {
      layer_count1++;
    }
  }

  for (int i = 0; i < c2->totlayer; i++) {
    l2 = &c2->layers[i];
    if ((CD_TYPE_AS_MASK(l2->type) & cd_mask_all_attr) && l2->anonymous_id == nullptr) {
      layer_count2++;
    }
  }

  if (layer_count1 != layer_count2) {
    return MESHCMP_CDLAYERS_MISMATCH;
  }

  l1 = c1->layers;
  l2 = c2->layers;

  for (int i1 = 0; i1 < c1->totlayer; i1++) {
    l1 = c1->layers + i1;
    for (int i2 = 0; i2 < c2->totlayer; i2++) {
      l2 = c2->layers + i2;
      if (l1->type != l2->type || !STREQ(l1->name, l2->name) || l1->anonymous_id != nullptr ||
          l2->anonymous_id != nullptr) {
        continue;
      }
      /* At this point `l1` and `l2` have the same name and type, so they should be compared. */

      switch (l1->type) {

        case CD_MVERT: {
          MVert *v1 = (MVert *)l1->data;
          MVert *v2 = (MVert *)l2->data;
          int vtot = m1->totvert;

          for (j = 0; j < vtot; j++, v1++, v2++) {
            for (int k = 0; k < 3; k++) {
              if (compare_threshold_relative(v1->co[k], v2->co[k], thresh)) {
                return MESHCMP_VERTCOMISMATCH;
              }
            }
          }
          break;
        }

        /* We're order-agnostic for edges here. */
        case CD_MEDGE: {
          MEdge *e1 = (MEdge *)l1->data;
          MEdge *e2 = (MEdge *)l2->data;
          int etot = m1->totedge;
          EdgeHash *eh = BLI_edgehash_new_ex(__func__, etot);

          for (j = 0; j < etot; j++, e1++) {
            BLI_edgehash_insert(eh, e1->v1, e1->v2, e1);
          }

          for (j = 0; j < etot; j++, e2++) {
            if (!BLI_edgehash_lookup(eh, e2->v1, e2->v2)) {
              return MESHCMP_EDGEUNKNOWN;
            }
          }
          BLI_edgehash_free(eh, nullptr);
          break;
        }
        case CD_MPOLY: {
          MPoly *p1 = (MPoly *)l1->data;
          MPoly *p2 = (MPoly *)l2->data;
          int ptot = m1->totpoly;

          for (j = 0; j < ptot; j++, p1++, p2++) {
            MLoop *lp1, *lp2;
            int k;

            if (p1->totloop != p2->totloop) {
              return MESHCMP_POLYMISMATCH;
            }

            lp1 = m1->mloop + p1->loopstart;
            lp2 = m2->mloop + p2->loopstart;

            for (k = 0; k < p1->totloop; k++, lp1++, lp2++) {
              if (lp1->v != lp2->v) {
                return MESHCMP_POLYVERTMISMATCH;
              }
            }
          }
          break;
        }
        case CD_MLOOP: {
          MLoop *lp1 = (MLoop *)l1->data;
          MLoop *lp2 = (MLoop *)l2->data;
          int ltot = m1->totloop;

          for (j = 0; j < ltot; j++, lp1++, lp2++) {
            if (lp1->v != lp2->v) {
              return MESHCMP_LOOPMISMATCH;
            }
          }
          break;
        }
        case CD_MLOOPUV: {
          MLoopUV *lp1 = (MLoopUV *)l1->data;
          MLoopUV *lp2 = (MLoopUV *)l2->data;
          int ltot = m1->totloop;

          for (j = 0; j < ltot; j++, lp1++, lp2++) {
            if (len_squared_v2v2(lp1->uv, lp2->uv) > thresh_sq) {
              return MESHCMP_LOOPUVMISMATCH;
            }
          }
          break;
        }
        case CD_PROP_BYTE_COLOR: {
          MLoopCol *lp1 = (MLoopCol *)l1->data;
          MLoopCol *lp2 = (MLoopCol *)l2->data;
          int ltot = m1->totloop;

          for (j = 0; j < ltot; j++, lp1++, lp2++) {
            if (lp1->r != lp2->r || lp1->g != lp2->g || lp1->b != lp2->b || lp1->a != lp2->a) {
              return MESHCMP_LOOPCOLMISMATCH;
            }
          }
          break;
        }
        case CD_MDEFORMVERT: {
          MDeformVert *dv1 = (MDeformVert *)l1->data;
          MDeformVert *dv2 = (MDeformVert *)l2->data;
          int dvtot = m1->totvert;

          for (j = 0; j < dvtot; j++, dv1++, dv2++) {
            int k;
            MDeformWeight *dw1 = dv1->dw, *dw2 = dv2->dw;

            if (dv1->totweight != dv2->totweight) {
              return MESHCMP_DVERT_TOTGROUPMISMATCH;
            }

            for (k = 0; k < dv1->totweight; k++, dw1++, dw2++) {
              if (dw1->def_nr != dw2->def_nr) {
                return MESHCMP_DVERT_GROUPMISMATCH;
              }
              if (fabsf(dw1->weight - dw2->weight) > thresh) {
                return MESHCMP_DVERT_WEIGHTMISMATCH;
              }
            }
          }
          break;
        }
        case CD_PROP_FLOAT: {
          const float *l1_data = (float *)l1->data;
          const float *l2_data = (float *)l2->data;

          for (int i = 0; i < total_length; i++) {
            if (compare_threshold_relative(l1_data[i], l2_data[i], thresh)) {
              return MESHCMP_ATTRIBUTE_VALUE_MISMATCH;
            }
          }
          break;
        }
        case CD_PROP_FLOAT2: {
          const float(*l1_data)[2] = (float(*)[2])l1->data;
          const float(*l2_data)[2] = (float(*)[2])l2->data;

          for (int i = 0; i < total_length; i++) {
            if (compare_threshold_relative(l1_data[i][0], l2_data[i][0], thresh)) {
              return MESHCMP_ATTRIBUTE_VALUE_MISMATCH;
            }
            if (compare_threshold_relative(l1_data[i][1], l2_data[i][1], thresh)) {
              return MESHCMP_ATTRIBUTE_VALUE_MISMATCH;
            }
          }
          break;
        }
        case CD_PROP_FLOAT3: {
          const float(*l1_data)[3] = (float(*)[3])l1->data;
          const float(*l2_data)[3] = (float(*)[3])l2->data;

          for (int i = 0; i < total_length; i++) {
            if (compare_threshold_relative(l1_data[i][0], l2_data[i][0], thresh)) {
              return MESHCMP_ATTRIBUTE_VALUE_MISMATCH;
            }
            if (compare_threshold_relative(l1_data[i][1], l2_data[i][1], thresh)) {
              return MESHCMP_ATTRIBUTE_VALUE_MISMATCH;
            }
            if (compare_threshold_relative(l1_data[i][2], l2_data[i][2], thresh)) {
              return MESHCMP_ATTRIBUTE_VALUE_MISMATCH;
            }
          }
          break;
        }
        case CD_PROP_INT32: {
          const int *l1_data = (int *)l1->data;
          const int *l2_data = (int *)l2->data;

          for (int i = 0; i < total_length; i++) {
            if (l1_data[i] != l2_data[i]) {
              return MESHCMP_ATTRIBUTE_VALUE_MISMATCH;
            }
          }
          break;
        }
        case CD_PROP_INT8: {
          const int8_t *l1_data = (int8_t *)l1->data;
          const int8_t *l2_data = (int8_t *)l2->data;

          for (int i = 0; i < total_length; i++) {
            if (l1_data[i] != l2_data[i]) {
              return MESHCMP_ATTRIBUTE_VALUE_MISMATCH;
            }
          }
          break;
        }
        case CD_PROP_BOOL: {
          const bool *l1_data = (bool *)l1->data;
          const bool *l2_data = (bool *)l2->data;

          for (int i = 0; i < total_length; i++) {
            if (l1_data[i] != l2_data[i]) {
              return MESHCMP_ATTRIBUTE_VALUE_MISMATCH;
            }
          }
          break;
        }
        case CD_PROP_COLOR: {
          const MPropCol *l1_data = (MPropCol *)l1->data;
          const MPropCol *l2_data = (MPropCol *)l2->data;

          for (int i = 0; i < total_length; i++) {
            for (j = 0; j < 4; j++) {
              if (compare_threshold_relative(l1_data[i].color[j], l2_data[i].color[j], thresh)) {
                return MESHCMP_ATTRIBUTE_VALUE_MISMATCH;
              }
            }
          }
          break;
        }
        default: {
          break;
        }
      }
    }
  }

  return 0;
}

const char *BKE_mesh_cmp(Mesh *me1, Mesh *me2, float thresh)
{
  int c;

  if (!me1 || !me2) {
    return "Requires two input meshes";
  }

  if (me1->totvert != me2->totvert) {
    return "Number of verts don't match";
  }

  if (me1->totedge != me2->totedge) {
    return "Number of edges don't match";
  }

  if (me1->totpoly != me2->totpoly) {
    return "Number of faces don't match";
  }

  if (me1->totloop != me2->totloop) {
    return "Number of loops don't match";
  }

  if ((c = customdata_compare(&me1->vdata, &me2->vdata, me1->totvert, me1, me2, thresh))) {
    return cmpcode_to_str(c);
  }

  if ((c = customdata_compare(&me1->edata, &me2->edata, me1->totedge, me1, me2, thresh))) {
    return cmpcode_to_str(c);
  }

  if ((c = customdata_compare(&me1->ldata, &me2->ldata, me1->totloop, me1, me2, thresh))) {
    return cmpcode_to_str(c);
  }

  if ((c = customdata_compare(&me1->pdata, &me2->pdata, me1->totpoly, me1, me2, thresh))) {
    return cmpcode_to_str(c);
  }

  return nullptr;
}

static void mesh_ensure_tessellation_customdata(Mesh *me)
{
  if (UNLIKELY((me->totface != 0) && (me->totpoly == 0))) {
    /* Pass, otherwise this function  clears 'mface' before
     * versioning 'mface -> mpoly' code kicks in T30583.
     *
     * Callers could also check but safer to do here - campbell */
  }
  else {
    const int tottex_original = CustomData_number_of_layers(&me->ldata, CD_MLOOPUV);
    const int totcol_original = CustomData_number_of_layers(&me->ldata, CD_PROP_BYTE_COLOR);

    const int tottex_tessface = CustomData_number_of_layers(&me->fdata, CD_MTFACE);
    const int totcol_tessface = CustomData_number_of_layers(&me->fdata, CD_MCOL);

    if (tottex_tessface != tottex_original || totcol_tessface != totcol_original) {
      BKE_mesh_tessface_clear(me);

      BKE_mesh_add_mface_layers(&me->fdata, &me->ldata, me->totface);

      /* TODO: add some `--debug-mesh` option. */
      if (G.debug & G_DEBUG) {
        /* NOTE(@campbellbarton): this warning may be un-called for if we are initializing the mesh
         * for the first time from #BMesh, rather than giving a warning about this we could be
         * smarter and check if there was any data to begin with, for now just print the warning
         * with some info to help troubleshoot what's going on. */
        printf(
            "%s: warning! Tessellation uvs or vcol data got out of sync, "
            "had to reset!\n    CD_MTFACE: %d != CD_MLOOPUV: %d || CD_MCOL: %d != "
            "CD_PROP_BYTE_COLOR: "
            "%d\n",
            __func__,
            tottex_tessface,
            tottex_original,
            totcol_tessface,
            totcol_original);
      }
    }
  }
}

void BKE_mesh_ensure_skin_customdata(Mesh *me)
{
  BMesh *bm = me->edit_mesh ? me->edit_mesh->bm : nullptr;
  MVertSkin *vs;

  if (bm) {
    if (!CustomData_has_layer(&bm->vdata, CD_MVERT_SKIN)) {
      BMVert *v;
      BMIter iter;

      BM_data_layer_add(bm, &bm->vdata, CD_MVERT_SKIN);

      /* Mark an arbitrary vertex as root */
      BM_ITER_MESH (v, &iter, bm, BM_VERTS_OF_MESH) {
        vs = (MVertSkin *)CustomData_bmesh_get(&bm->vdata, v->head.data, CD_MVERT_SKIN);
        vs->flag |= MVERT_SKIN_ROOT;
        break;
      }
    }
  }
  else {
    if (!CustomData_has_layer(&me->vdata, CD_MVERT_SKIN)) {
      vs = (MVertSkin *)CustomData_add_layer(
          &me->vdata, CD_MVERT_SKIN, CD_DEFAULT, nullptr, me->totvert);

      /* Mark an arbitrary vertex as root */
      if (vs) {
        vs->flag |= MVERT_SKIN_ROOT;
      }
    }
  }
}

bool BKE_mesh_ensure_facemap_customdata(struct Mesh *me)
{
  BMesh *bm = me->edit_mesh ? me->edit_mesh->bm : nullptr;
  bool changed = false;
  if (bm) {
    if (!CustomData_has_layer(&bm->pdata, CD_FACEMAP)) {
      BM_data_layer_add(bm, &bm->pdata, CD_FACEMAP);
      changed = true;
    }
  }
  else {
    if (!CustomData_has_layer(&me->pdata, CD_FACEMAP)) {
      CustomData_add_layer(&me->pdata, CD_FACEMAP, CD_DEFAULT, nullptr, me->totpoly);
      changed = true;
    }
  }
  return changed;
}

bool BKE_mesh_clear_facemap_customdata(struct Mesh *me)
{
  BMesh *bm = me->edit_mesh ? me->edit_mesh->bm : nullptr;
  bool changed = false;
  if (bm) {
    if (CustomData_has_layer(&bm->pdata, CD_FACEMAP)) {
      BM_data_layer_free(bm, &bm->pdata, CD_FACEMAP);
      changed = true;
    }
  }
  else {
    if (CustomData_has_layer(&me->pdata, CD_FACEMAP)) {
      CustomData_free_layers(&me->pdata, CD_FACEMAP, me->totpoly);
      changed = true;
    }
  }
  return changed;
}

/**
 * This ensures grouped custom-data (e.g. #CD_MLOOPUV and #CD_MTFACE, or
 * #CD_PROP_BYTE_COLOR and #CD_MCOL) have the same relative active/render/clone/mask indices.
 *
 * NOTE(@campbellbarton): that for undo mesh data we want to skip 'ensure_tess_cd' call since
 * we don't want to store memory for #MFace data when its only used for older
 * versions of the mesh.
 */
static void mesh_update_linked_customdata(Mesh *me, const bool do_ensure_tess_cd)
{
  if (do_ensure_tess_cd) {
    mesh_ensure_tessellation_customdata(me);
  }

  CustomData_bmesh_update_active_layers(&me->fdata, &me->ldata);
}

void BKE_mesh_update_customdata_pointers(Mesh *me, const bool do_ensure_tess_cd)
{
  mesh_update_linked_customdata(me, do_ensure_tess_cd);

  me->mvert = (MVert *)CustomData_get_layer(&me->vdata, CD_MVERT);
  me->dvert = (MDeformVert *)CustomData_get_layer(&me->vdata, CD_MDEFORMVERT);

  me->medge = (MEdge *)CustomData_get_layer(&me->edata, CD_MEDGE);

  me->mface = (MFace *)CustomData_get_layer(&me->fdata, CD_MFACE);
  me->mcol = (MCol *)CustomData_get_layer(&me->fdata, CD_MCOL);
  me->mtface = (MTFace *)CustomData_get_layer(&me->fdata, CD_MTFACE);

  me->mpoly = (MPoly *)CustomData_get_layer(&me->pdata, CD_MPOLY);
  me->mloop = (MLoop *)CustomData_get_layer(&me->ldata, CD_MLOOP);

  me->mloopcol = (MLoopCol *)CustomData_get_layer(&me->ldata, CD_PROP_BYTE_COLOR);
  me->mloopuv = (MLoopUV *)CustomData_get_layer(&me->ldata, CD_MLOOPUV);
}

bool BKE_mesh_has_custom_loop_normals(Mesh *me)
{
  if (me->edit_mesh) {
    return CustomData_has_layer(&me->edit_mesh->bm->ldata, CD_CUSTOMLOOPNORMAL);
  }

  return CustomData_has_layer(&me->ldata, CD_CUSTOMLOOPNORMAL);
}

void BKE_mesh_free_data_for_undo(Mesh *me)
{
  mesh_free_data(&me->id);
}

/**
 * \note on data that this function intentionally doesn't free:
 *
 * - Materials and shape keys are not freed here (#Mesh.mat & #Mesh.key).
 *   As freeing shape keys requires tagging the depsgraph for updated relations,
 *   which is expensive.
 *   Material slots should be kept in sync with the object.
 *
 * - Edit-Mesh (#Mesh.edit_mesh)
 *   Since edit-mesh is tied to the objects mode,
 *   which crashes when called in edit-mode, see: T90972.
 */
static void mesh_clear_geometry(Mesh *mesh)
{
  CustomData_free(&mesh->vdata, mesh->totvert);
  CustomData_free(&mesh->edata, mesh->totedge);
  CustomData_free(&mesh->fdata, mesh->totface);
  CustomData_free(&mesh->ldata, mesh->totloop);
  CustomData_free(&mesh->pdata, mesh->totpoly);

  MEM_SAFE_FREE(mesh->mselect);

  mesh->totvert = 0;
  mesh->totedge = 0;
  mesh->totface = 0;
  mesh->totloop = 0;
  mesh->totpoly = 0;
  mesh->act_face = -1;
  mesh->totselect = 0;

  BKE_mesh_update_customdata_pointers(mesh, false);
}

void BKE_mesh_clear_geometry(Mesh *mesh)
{
  BKE_animdata_free(&mesh->id, false);
  BKE_mesh_runtime_clear_cache(mesh);
  mesh_clear_geometry(mesh);
}

static void mesh_tessface_clear_intern(Mesh *mesh, int free_customdata)
{
  if (free_customdata) {
    CustomData_free(&mesh->fdata, mesh->totface);
  }
  else {
    CustomData_reset(&mesh->fdata);
  }

  mesh->mface = nullptr;
  mesh->mtface = nullptr;
  mesh->mcol = nullptr;
  mesh->totface = 0;
}

Mesh *BKE_mesh_add(Main *bmain, const char *name)
{
  Mesh *me = (Mesh *)BKE_id_new(bmain, ID_ME, name);

  return me;
}

/* Custom data layer functions; those assume that totXXX are set correctly. */
static void mesh_ensure_cdlayers_primary(Mesh *mesh, bool do_tessface)
{
  if (!CustomData_get_layer(&mesh->vdata, CD_MVERT)) {
    CustomData_add_layer(&mesh->vdata, CD_MVERT, CD_CALLOC, nullptr, mesh->totvert);
  }
  if (!CustomData_get_layer(&mesh->edata, CD_MEDGE)) {
    CustomData_add_layer(&mesh->edata, CD_MEDGE, CD_CALLOC, nullptr, mesh->totedge);
  }
  if (!CustomData_get_layer(&mesh->ldata, CD_MLOOP)) {
    CustomData_add_layer(&mesh->ldata, CD_MLOOP, CD_CALLOC, nullptr, mesh->totloop);
  }
  if (!CustomData_get_layer(&mesh->pdata, CD_MPOLY)) {
    CustomData_add_layer(&mesh->pdata, CD_MPOLY, CD_CALLOC, nullptr, mesh->totpoly);
  }

  if (do_tessface && !CustomData_get_layer(&mesh->fdata, CD_MFACE)) {
    CustomData_add_layer(&mesh->fdata, CD_MFACE, CD_CALLOC, nullptr, mesh->totface);
  }
}

Mesh *BKE_mesh_new_nomain(
    int verts_len, int edges_len, int tessface_len, int loops_len, int polys_len)
{
  Mesh *mesh = (Mesh *)BKE_libblock_alloc(
      nullptr, ID_ME, BKE_idtype_idcode_to_name(ID_ME), LIB_ID_CREATE_LOCALIZE);
  BKE_libblock_init_empty(&mesh->id);

  /* Don't use #CustomData_reset because we don't want to touch custom-data. */
  copy_vn_i(mesh->vdata.typemap, CD_NUMTYPES, -1);
  copy_vn_i(mesh->edata.typemap, CD_NUMTYPES, -1);
  copy_vn_i(mesh->fdata.typemap, CD_NUMTYPES, -1);
  copy_vn_i(mesh->ldata.typemap, CD_NUMTYPES, -1);
  copy_vn_i(mesh->pdata.typemap, CD_NUMTYPES, -1);

  mesh->totvert = verts_len;
  mesh->totedge = edges_len;
  mesh->totface = tessface_len;
  mesh->totloop = loops_len;
  mesh->totpoly = polys_len;

  mesh_ensure_cdlayers_primary(mesh, true);
  BKE_mesh_update_customdata_pointers(mesh, false);

  return mesh;
}

void BKE_mesh_copy_parameters(Mesh *me_dst, const Mesh *me_src)
{
  /* Copy general settings. */
  me_dst->editflag = me_src->editflag;
  me_dst->flag = me_src->flag;
  me_dst->smoothresh = me_src->smoothresh;
  me_dst->remesh_voxel_size = me_src->remesh_voxel_size;
  me_dst->remesh_voxel_adaptivity = me_src->remesh_voxel_adaptivity;
  me_dst->remesh_mode = me_src->remesh_mode;
  me_dst->symmetry = me_src->symmetry;

  me_dst->face_sets_color_seed = me_src->face_sets_color_seed;
  me_dst->face_sets_color_default = me_src->face_sets_color_default;

  /* Copy texture space. */
  me_dst->texflag = me_src->texflag;
  copy_v3_v3(me_dst->loc, me_src->loc);
  copy_v3_v3(me_dst->size, me_src->size);

  me_dst->vertex_group_active_index = me_src->vertex_group_active_index;
}

void BKE_mesh_copy_parameters_for_eval(Mesh *me_dst, const Mesh *me_src)
{
  /* User counts aren't handled, don't copy into a mesh from #G_MAIN. */
  BLI_assert(me_dst->id.tag & (LIB_TAG_NO_MAIN | LIB_TAG_COPIED_ON_WRITE));

  BKE_mesh_copy_parameters(me_dst, me_src);

  BKE_mesh_assert_normals_dirty_or_calculated(me_dst);

  /* Copy vertex group names. */
  BLI_assert(BLI_listbase_is_empty(&me_dst->vertex_group_names));
  BKE_defgroup_copy_list(&me_dst->vertex_group_names, &me_src->vertex_group_names);

  /* Copy materials. */
  if (me_dst->mat != nullptr) {
    MEM_freeN(me_dst->mat);
  }
  me_dst->mat = (Material **)MEM_dupallocN(me_src->mat);
  me_dst->totcol = me_src->totcol;
}

Mesh *BKE_mesh_new_nomain_from_template_ex(const Mesh *me_src,
                                           int verts_len,
                                           int edges_len,
                                           int tessface_len,
                                           int loops_len,
                                           int polys_len,
                                           CustomData_MeshMasks mask)
{
  /* Only do tessface if we are creating tessfaces or copying from mesh with only tessfaces. */
  const bool do_tessface = (tessface_len || ((me_src->totface != 0) && (me_src->totpoly == 0)));

  Mesh *me_dst = (Mesh *)BKE_id_new_nomain(ID_ME, nullptr);

  me_dst->mselect = (MSelect *)MEM_dupallocN(me_src->mselect);

  me_dst->totvert = verts_len;
  me_dst->totedge = edges_len;
  me_dst->totface = tessface_len;
  me_dst->totloop = loops_len;
  me_dst->totpoly = polys_len;

  me_dst->cd_flag = me_src->cd_flag;
  BKE_mesh_copy_parameters_for_eval(me_dst, me_src);

  CustomData_copy(&me_src->vdata, &me_dst->vdata, mask.vmask, CD_CALLOC, verts_len);
  CustomData_copy(&me_src->edata, &me_dst->edata, mask.emask, CD_CALLOC, edges_len);
  CustomData_copy(&me_src->ldata, &me_dst->ldata, mask.lmask, CD_CALLOC, loops_len);
  CustomData_copy(&me_src->pdata, &me_dst->pdata, mask.pmask, CD_CALLOC, polys_len);
  if (do_tessface) {
    CustomData_copy(&me_src->fdata, &me_dst->fdata, mask.fmask, CD_CALLOC, tessface_len);
  }
  else {
    mesh_tessface_clear_intern(me_dst, false);
  }

  /* The destination mesh should at least have valid primary CD layers,
   * even in cases where the source mesh does not. */
  mesh_ensure_cdlayers_primary(me_dst, do_tessface);
  BKE_mesh_update_customdata_pointers(me_dst, false);

  /* Expect that normals aren't copied at all, since the destination mesh is new. */
  BLI_assert(BKE_mesh_vertex_normals_are_dirty(me_dst));

  return me_dst;
}

Mesh *BKE_mesh_new_nomain_from_template(const Mesh *me_src,
                                        int verts_len,
                                        int edges_len,
                                        int tessface_len,
                                        int loops_len,
                                        int polys_len)
{
  return BKE_mesh_new_nomain_from_template_ex(
      me_src, verts_len, edges_len, tessface_len, loops_len, polys_len, CD_MASK_EVERYTHING);
}

void BKE_mesh_eval_delete(struct Mesh *mesh_eval)
{
  /* Evaluated mesh may point to edit mesh, but never owns it. */
  mesh_eval->edit_mesh = nullptr;
  mesh_free_data(&mesh_eval->id);
  BKE_libblock_free_data(&mesh_eval->id, false);
  MEM_freeN(mesh_eval);
}

Mesh *BKE_mesh_copy_for_eval(const Mesh *source, bool reference)
{
  int flags = LIB_ID_COPY_LOCALIZE;

  if (reference) {
    flags |= LIB_ID_COPY_CD_REFERENCE;
  }

  Mesh *result = (Mesh *)BKE_id_copy_ex(nullptr, &source->id, nullptr, flags);
  return result;
}

BMesh *BKE_mesh_to_bmesh_ex(const Mesh *me,
                            const struct BMeshCreateParams *create_params,
                            const struct BMeshFromMeshParams *convert_params)
{
  const BMAllocTemplate allocsize = BMALLOC_TEMPLATE_FROM_ME(me);

  BMesh *bm = BM_mesh_create(&allocsize, create_params);
  BM_mesh_bm_from_me(bm, me, convert_params);

  return bm;
}

BMesh *BKE_mesh_to_bmesh(Mesh *me,
                         Object *ob,
                         const bool add_key_index,
                         const struct BMeshCreateParams *params)
{
  BMeshFromMeshParams bmesh_from_mesh_params{};
  bmesh_from_mesh_params.calc_face_normal = false;
  bmesh_from_mesh_params.calc_vert_normal = false;
  bmesh_from_mesh_params.add_key_index = add_key_index;
  bmesh_from_mesh_params.use_shapekey = true;
  bmesh_from_mesh_params.active_shapekey = ob->shapenr;
  return BKE_mesh_to_bmesh_ex(me, params, &bmesh_from_mesh_params);
}

Mesh *BKE_mesh_from_bmesh_nomain(BMesh *bm,
                                 const struct BMeshToMeshParams *params,
                                 const Mesh *me_settings)
{
  BLI_assert(params->calc_object_remap == false);
  Mesh *mesh = (Mesh *)BKE_id_new_nomain(ID_ME, nullptr);
  BM_mesh_bm_to_me(nullptr, bm, mesh, params);
  BKE_mesh_copy_parameters_for_eval(mesh, me_settings);
  return mesh;
}

Mesh *BKE_mesh_from_bmesh_for_eval_nomain(BMesh *bm,
                                          const CustomData_MeshMasks *cd_mask_extra,
                                          const Mesh *me_settings)
{
  Mesh *mesh = (Mesh *)BKE_id_new_nomain(ID_ME, nullptr);
  BM_mesh_bm_to_me_for_eval(bm, mesh, cd_mask_extra);
  BKE_mesh_copy_parameters_for_eval(mesh, me_settings);
  return mesh;
}

static void ensure_orig_index_layer(CustomData &data, const int size)
{
  if (CustomData_has_layer(&data, CD_ORIGINDEX)) {
    return;
  }
  int *indices = (int *)CustomData_add_layer(&data, CD_ORIGINDEX, CD_DEFAULT, nullptr, size);
  range_vn_i(indices, size, 0);
}

void BKE_mesh_ensure_default_orig_index_customdata(Mesh *mesh)
{
  BLI_assert(mesh->runtime.wrapper_type == ME_WRAPPER_TYPE_MDATA);
  BKE_mesh_ensure_default_orig_index_customdata_no_check(mesh);
}

void BKE_mesh_ensure_default_orig_index_customdata_no_check(Mesh *mesh)
{
  ensure_orig_index_layer(mesh->vdata, mesh->totvert);
  ensure_orig_index_layer(mesh->edata, mesh->totedge);
  ensure_orig_index_layer(mesh->pdata, mesh->totpoly);
}

BoundBox *BKE_mesh_boundbox_get(Object *ob)
{
  /* This is Object-level data access,
   * DO NOT touch to Mesh's bb, would be totally thread-unsafe. */
  if (ob->runtime.bb == nullptr || ob->runtime.bb->flag & BOUNDBOX_DIRTY) {
    Mesh *me = (Mesh *)ob->data;
    float min[3], max[3];

    INIT_MINMAX(min, max);
    if (!BKE_mesh_wrapper_minmax(me, min, max)) {
      min[0] = min[1] = min[2] = -1.0f;
      max[0] = max[1] = max[2] = 1.0f;
    }

    if (ob->runtime.bb == nullptr) {
      ob->runtime.bb = (BoundBox *)MEM_mallocN(sizeof(*ob->runtime.bb), __func__);
    }
    BKE_boundbox_init_from_minmax(ob->runtime.bb, min, max);
    ob->runtime.bb->flag &= ~BOUNDBOX_DIRTY;
  }

  return ob->runtime.bb;
}

void BKE_mesh_texspace_calc(Mesh *me)
{
  if (me->texflag & ME_AUTOSPACE) {
    float min[3], max[3];

    INIT_MINMAX(min, max);
    if (!BKE_mesh_wrapper_minmax(me, min, max)) {
      min[0] = min[1] = min[2] = -1.0f;
      max[0] = max[1] = max[2] = 1.0f;
    }

    float loc[3], size[3];
    mid_v3_v3v3(loc, min, max);

    size[0] = (max[0] - min[0]) / 2.0f;
    size[1] = (max[1] - min[1]) / 2.0f;
    size[2] = (max[2] - min[2]) / 2.0f;

    for (int a = 0; a < 3; a++) {
      if (size[a] == 0.0f) {
        size[a] = 1.0f;
      }
      else if (size[a] > 0.0f && size[a] < 0.00001f) {
        size[a] = 0.00001f;
      }
      else if (size[a] < 0.0f && size[a] > -0.00001f) {
        size[a] = -0.00001f;
      }
    }

    copy_v3_v3(me->loc, loc);
    copy_v3_v3(me->size, size);

    me->texflag |= ME_AUTOSPACE_EVALUATED;
  }
}

void BKE_mesh_texspace_ensure(Mesh *me)
{
  if ((me->texflag & ME_AUTOSPACE) && !(me->texflag & ME_AUTOSPACE_EVALUATED)) {
    BKE_mesh_texspace_calc(me);
  }
}

void BKE_mesh_texspace_get(Mesh *me, float r_loc[3], float r_size[3])
{
  BKE_mesh_texspace_ensure(me);

  if (r_loc) {
    copy_v3_v3(r_loc, me->loc);
  }
  if (r_size) {
    copy_v3_v3(r_size, me->size);
  }
}

void BKE_mesh_texspace_get_reference(Mesh *me, char **r_texflag, float **r_loc, float **r_size)
{
  BKE_mesh_texspace_ensure(me);

  if (r_texflag != nullptr) {
    *r_texflag = &me->texflag;
  }
  if (r_loc != nullptr) {
    *r_loc = me->loc;
  }
  if (r_size != nullptr) {
    *r_size = me->size;
  }
}

void BKE_mesh_texspace_copy_from_object(Mesh *me, Object *ob)
{
  float *texloc, *texsize;
  char *texflag;

  if (BKE_object_obdata_texspace_get(ob, &texflag, &texloc, &texsize)) {
    me->texflag = *texflag;
    copy_v3_v3(me->loc, texloc);
    copy_v3_v3(me->size, texsize);
  }
}

float (*BKE_mesh_orco_verts_get(Object *ob))[3]
{
  Mesh *me = (Mesh *)ob->data;
  Mesh *tme = me->texcomesh ? me->texcomesh : me;

  /* Get appropriate vertex coordinates */
  float(*vcos)[3] = (float(*)[3])MEM_calloc_arrayN(me->totvert, sizeof(*vcos), "orco mesh");
  MVert *mvert = tme->mvert;
  int totvert = min_ii(tme->totvert, me->totvert);

  for (int a = 0; a < totvert; a++, mvert++) {
    copy_v3_v3(vcos[a], mvert->co);
  }

  return vcos;
}

void BKE_mesh_orco_verts_transform(Mesh *me, float (*orco)[3], int totvert, int invert)
{
  float loc[3], size[3];

  BKE_mesh_texspace_get(me->texcomesh ? me->texcomesh : me, loc, size);

  if (invert) {
    for (int a = 0; a < totvert; a++) {
      float *co = orco[a];
      madd_v3_v3v3v3(co, loc, co, size);
    }
  }
  else {
    for (int a = 0; a < totvert; a++) {
      float *co = orco[a];
      co[0] = (co[0] - loc[0]) / size[0];
      co[1] = (co[1] - loc[1]) / size[1];
      co[2] = (co[2] - loc[2]) / size[2];
    }
  }
}

void BKE_mesh_orco_ensure(Object *ob, Mesh *mesh)
{
  if (CustomData_has_layer(&mesh->vdata, CD_ORCO)) {
    return;
  }

  /* Orcos are stored in normalized 0..1 range by convention. */
  float(*orcodata)[3] = BKE_mesh_orco_verts_get(ob);
  BKE_mesh_orco_verts_transform(mesh, orcodata, mesh->totvert, false);
  CustomData_add_layer(&mesh->vdata, CD_ORCO, CD_ASSIGN, orcodata, mesh->totvert);
}

Mesh *BKE_mesh_from_object(Object *ob)
{
  if (ob == nullptr) {
    return nullptr;
  }
  if (ob->type == OB_MESH) {
    return (Mesh *)ob->data;
  }

  return nullptr;
}

void BKE_mesh_assign_object(Main *bmain, Object *ob, Mesh *me)
{
  Mesh *old = nullptr;

  if (ob == nullptr) {
    return;
  }

  multires_force_sculpt_rebuild(ob);

  if (ob->type == OB_MESH) {
    old = (Mesh *)ob->data;
    if (old) {
      id_us_min(&old->id);
    }
    ob->data = me;
    id_us_plus((ID *)me);
  }

  BKE_object_materials_test(bmain, ob, (ID *)me);

  BKE_modifiers_test_object(ob);
}

void BKE_mesh_material_index_remove(Mesh *me, short index)
{
  MPoly *mp;
  MFace *mf;
  int i;

  for (mp = me->mpoly, i = 0; i < me->totpoly; i++, mp++) {
    if (mp->mat_nr && mp->mat_nr >= index) {
      mp->mat_nr--;
    }
  }

  for (mf = me->mface, i = 0; i < me->totface; i++, mf++) {
    if (mf->mat_nr && mf->mat_nr >= index) {
      mf->mat_nr--;
    }
  }
}

bool BKE_mesh_material_index_used(Mesh *me, short index)
{
  MPoly *mp;
  MFace *mf;
  int i;

  for (mp = me->mpoly, i = 0; i < me->totpoly; i++, mp++) {
    if (mp->mat_nr == index) {
      return true;
    }
  }

  for (mf = me->mface, i = 0; i < me->totface; i++, mf++) {
    if (mf->mat_nr == index) {
      return true;
    }
  }

  return false;
}

void BKE_mesh_material_index_clear(Mesh *me)
{
  MPoly *mp;
  MFace *mf;
  int i;

  for (mp = me->mpoly, i = 0; i < me->totpoly; i++, mp++) {
    mp->mat_nr = 0;
  }

  for (mf = me->mface, i = 0; i < me->totface; i++, mf++) {
    mf->mat_nr = 0;
  }
}

void BKE_mesh_material_remap(Mesh *me, const uint *remap, uint remap_len)
{
  const short remap_len_short = (short)remap_len;

#define MAT_NR_REMAP(n) \
  if (n < remap_len_short) { \
    BLI_assert(n >= 0 && remap[n] < remap_len_short); \
    n = remap[n]; \
  } \
  ((void)0)

  if (me->edit_mesh) {
    BMEditMesh *em = me->edit_mesh;
    BMIter iter;
    BMFace *efa;

    BM_ITER_MESH (efa, &iter, em->bm, BM_FACES_OF_MESH) {
      MAT_NR_REMAP(efa->mat_nr);
    }
  }
  else {
    int i;
    for (i = 0; i < me->totpoly; i++) {
      MAT_NR_REMAP(me->mpoly[i].mat_nr);
    }
  }

#undef MAT_NR_REMAP
}

void BKE_mesh_smooth_flag_set(Mesh *me, const bool use_smooth)
{
  if (use_smooth) {
    for (int i = 0; i < me->totpoly; i++) {
      me->mpoly[i].flag |= ME_SMOOTH;
    }
  }
  else {
    for (int i = 0; i < me->totpoly; i++) {
      me->mpoly[i].flag &= ~ME_SMOOTH;
    }
  }
}

void BKE_mesh_auto_smooth_flag_set(Mesh *me,
                                   const bool use_auto_smooth,
                                   const float auto_smooth_angle)
{
  if (use_auto_smooth) {
    me->flag |= ME_AUTOSMOOTH;
    me->smoothresh = auto_smooth_angle;
  }
  else {
    me->flag &= ~ME_AUTOSMOOTH;
  }
}

int poly_find_loop_from_vert(const MPoly *poly, const MLoop *loopstart, uint vert)
{
  for (int j = 0; j < poly->totloop; j++, loopstart++) {
    if (loopstart->v == vert) {
      return j;
    }
  }

  return -1;
}

int poly_get_adj_loops_from_vert(const MPoly *poly, const MLoop *mloop, uint vert, uint r_adj[2])
{
  int corner = poly_find_loop_from_vert(poly, &mloop[poly->loopstart], vert);

  if (corner != -1) {
    /* vertex was found */
    r_adj[0] = ME_POLY_LOOP_PREV(mloop, poly, corner)->v;
    r_adj[1] = ME_POLY_LOOP_NEXT(mloop, poly, corner)->v;
  }

  return corner;
}

int BKE_mesh_edge_other_vert(const MEdge *e, int v)
{
  if (e->v1 == v) {
    return e->v2;
  }
  if (e->v2 == v) {
    return e->v1;
  }

  return -1;
}

void BKE_mesh_looptri_get_real_edges(const Mesh *mesh, const MLoopTri *looptri, int r_edges[3])
{
  for (int i = 2, i_next = 0; i_next < 3; i = i_next++) {
    const MLoop *l1 = &mesh->mloop[looptri->tri[i]], *l2 = &mesh->mloop[looptri->tri[i_next]];
    const MEdge *e = &mesh->medge[l1->e];

    bool is_real = (l1->v == e->v1 && l2->v == e->v2) || (l1->v == e->v2 && l2->v == e->v1);

    r_edges[i] = is_real ? l1->e : -1;
  }
}

bool BKE_mesh_minmax(const Mesh *me, float r_min[3], float r_max[3])
{
  using namespace blender;
  if (me->totvert == 0) {
    return false;
  }

  struct Result {
    float3 min;
    float3 max;
  };

  const Result minmax = threading::parallel_reduce(
      IndexRange(me->totvert),
      1024,
      Result{float3(FLT_MAX), float3(-FLT_MAX)},
      [&](IndexRange range, const Result &init) {
        Result result = init;
        for (const int i : range) {
          math::min_max(float3(me->mvert[i].co), result.min, result.max);
        }
        return result;
      },
      [](const Result &a, const Result &b) {
        return Result{math::min(a.min, b.min), math::max(a.max, b.max)};
      });

  copy_v3_v3(r_min, math::min(minmax.min, float3(r_min)));
  copy_v3_v3(r_max, math::max(minmax.max, float3(r_max)));

  return true;
}

void BKE_mesh_transform(Mesh *me, const float mat[4][4], bool do_keys)
{
  int i;
  MVert *mvert = (MVert *)CustomData_duplicate_referenced_layer(&me->vdata, CD_MVERT, me->totvert);
  float(*lnors)[3] = (float(*)[3])CustomData_duplicate_referenced_layer(
      &me->ldata, CD_NORMAL, me->totloop);

  /* If the referenced layer has been re-allocated need to update pointers stored in the mesh. */
  BKE_mesh_update_customdata_pointers(me, false);

  for (i = 0; i < me->totvert; i++, mvert++) {
    mul_m4_v3(mat, mvert->co);
  }

  if (do_keys && me->key) {
    LISTBASE_FOREACH (KeyBlock *, kb, &me->key->block) {
      float *fp = (float *)kb->data;
      for (i = kb->totelem; i--; fp += 3) {
        mul_m4_v3(mat, fp);
      }
    }
  }

  /* don't update normals, caller can do this explicitly.
   * We do update loop normals though, those may not be auto-generated
   * (see e.g. STL import script)! */
  if (lnors) {
    float m3[3][3];

    copy_m3_m4(m3, mat);
    normalize_m3(m3);
    for (i = 0; i < me->totloop; i++, lnors++) {
      mul_m3_v3(m3, *lnors);
    }
  }
  BKE_mesh_tag_coords_changed(me);
}

void BKE_mesh_translate(Mesh *me, const float offset[3], const bool do_keys)
{
  CustomData_duplicate_referenced_layer(&me->vdata, CD_MVERT, me->totvert);
  /* If the referenced layer has been re-allocated need to update pointers stored in the mesh. */
  BKE_mesh_update_customdata_pointers(me, false);

  int i = me->totvert;
  for (MVert *mvert = me->mvert; i--; mvert++) {
    add_v3_v3(mvert->co, offset);
  }

  if (do_keys && me->key) {
    LISTBASE_FOREACH (KeyBlock *, kb, &me->key->block) {
      float *fp = (float *)kb->data;
      for (i = kb->totelem; i--; fp += 3) {
        add_v3_v3(fp, offset);
      }
    }
  }
  BKE_mesh_tag_coords_changed_uniformly(me);
}

void BKE_mesh_ensure_navmesh(Mesh *me)
{
  if (!CustomData_has_layer(&me->pdata, CD_RECAST)) {
    int i;
    int polys_len = me->totpoly;
    int *recastData;
    recastData = (int *)MEM_malloc_arrayN(polys_len, sizeof(int), __func__);
    for (i = 0; i < polys_len; i++) {
      recastData[i] = i + 1;
    }
    CustomData_add_layer_named(
        &me->pdata, CD_RECAST, CD_ASSIGN, recastData, polys_len, "recastData");
  }
}

void BKE_mesh_tessface_clear(Mesh *mesh)
{
  mesh_tessface_clear_intern(mesh, true);
}

void BKE_mesh_do_versions_cd_flag_init(Mesh *mesh)
{
  if (UNLIKELY(mesh->cd_flag)) {
    return;
  }

  MVert *mv;
  MEdge *med;
  int i;

  for (mv = mesh->mvert, i = 0; i < mesh->totvert; mv++, i++) {
    if (mv->bweight != 0) {
      mesh->cd_flag |= ME_CDFLAG_VERT_BWEIGHT;
      break;
    }
  }

  for (med = mesh->medge, i = 0; i < mesh->totedge; med++, i++) {
    if (med->bweight != 0) {
      mesh->cd_flag |= ME_CDFLAG_EDGE_BWEIGHT;
      if (mesh->cd_flag & ME_CDFLAG_EDGE_CREASE) {
        break;
      }
    }
    if (med->crease != 0) {
      mesh->cd_flag |= ME_CDFLAG_EDGE_CREASE;
      if (mesh->cd_flag & ME_CDFLAG_EDGE_BWEIGHT) {
        break;
      }
    }
  }
}

/* -------------------------------------------------------------------- */
/* MSelect functions (currently used in weight paint mode) */

void BKE_mesh_mselect_clear(Mesh *me)
{
  MEM_SAFE_FREE(me->mselect);
  me->totselect = 0;
}

void BKE_mesh_mselect_validate(Mesh *me)
{
  MSelect *mselect_src, *mselect_dst;
  int i_src, i_dst;

  if (me->totselect == 0) {
    return;
  }

  mselect_src = me->mselect;
  mselect_dst = (MSelect *)MEM_malloc_arrayN(
      (me->totselect), sizeof(MSelect), "Mesh selection history");

  for (i_src = 0, i_dst = 0; i_src < me->totselect; i_src++) {
    int index = mselect_src[i_src].index;
    switch (mselect_src[i_src].type) {
      case ME_VSEL: {
        if (me->mvert[index].flag & SELECT) {
          mselect_dst[i_dst] = mselect_src[i_src];
          i_dst++;
        }
        break;
      }
      case ME_ESEL: {
        if (me->medge[index].flag & SELECT) {
          mselect_dst[i_dst] = mselect_src[i_src];
          i_dst++;
        }
        break;
      }
      case ME_FSEL: {
        if (me->mpoly[index].flag & SELECT) {
          mselect_dst[i_dst] = mselect_src[i_src];
          i_dst++;
        }
        break;
      }
      default: {
        BLI_assert_unreachable();
        break;
      }
    }
  }

  MEM_freeN(mselect_src);

  if (i_dst == 0) {
    MEM_freeN(mselect_dst);
    mselect_dst = nullptr;
  }
  else if (i_dst != me->totselect) {
    mselect_dst = (MSelect *)MEM_reallocN(mselect_dst, sizeof(MSelect) * i_dst);
  }

  me->totselect = i_dst;
  me->mselect = mselect_dst;
}

int BKE_mesh_mselect_find(Mesh *me, int index, int type)
{
  BLI_assert(ELEM(type, ME_VSEL, ME_ESEL, ME_FSEL));

  for (int i = 0; i < me->totselect; i++) {
    if ((me->mselect[i].index == index) && (me->mselect[i].type == type)) {
      return i;
    }
  }

  return -1;
}

int BKE_mesh_mselect_active_get(Mesh *me, int type)
{
  BLI_assert(ELEM(type, ME_VSEL, ME_ESEL, ME_FSEL));

  if (me->totselect) {
    if (me->mselect[me->totselect - 1].type == type) {
      return me->mselect[me->totselect - 1].index;
    }
  }
  return -1;
}

void BKE_mesh_mselect_active_set(Mesh *me, int index, int type)
{
  const int msel_index = BKE_mesh_mselect_find(me, index, type);

  if (msel_index == -1) {
    /* add to the end */
    me->mselect = (MSelect *)MEM_reallocN(me->mselect, sizeof(MSelect) * (me->totselect + 1));
    me->mselect[me->totselect].index = index;
    me->mselect[me->totselect].type = type;
    me->totselect++;
  }
  else if (msel_index != me->totselect - 1) {
    /* move to the end */
    SWAP(MSelect, me->mselect[msel_index], me->mselect[me->totselect - 1]);
  }

  BLI_assert((me->mselect[me->totselect - 1].index == index) &&
             (me->mselect[me->totselect - 1].type == type));
}

void BKE_mesh_count_selected_items(const Mesh *mesh, int r_count[3])
{
  r_count[0] = r_count[1] = r_count[2] = 0;
  if (mesh->edit_mesh) {
    BMesh *bm = mesh->edit_mesh->bm;
    r_count[0] = bm->totvertsel;
    r_count[1] = bm->totedgesel;
    r_count[2] = bm->totfacesel;
  }
  /* We could support faces in paint modes. */
}

void BKE_mesh_vert_coords_get(const Mesh *mesh, float (*vert_coords)[3])
{
  const MVert *mv = mesh->mvert;
  for (int i = 0; i < mesh->totvert; i++, mv++) {
    copy_v3_v3(vert_coords[i], mv->co);
  }
}

float (*BKE_mesh_vert_coords_alloc(const Mesh *mesh, int *r_vert_len))[3]
{
  float(*vert_coords)[3] = (float(*)[3])MEM_mallocN(sizeof(float[3]) * mesh->totvert, __func__);
  BKE_mesh_vert_coords_get(mesh, vert_coords);
  if (r_vert_len) {
    *r_vert_len = mesh->totvert;
  }
  return vert_coords;
}

void BKE_mesh_vert_coords_apply(Mesh *mesh, const float (*vert_coords)[3])
{
  /* This will just return the pointer if it wasn't a referenced layer. */
  MVert *mv = (MVert *)CustomData_duplicate_referenced_layer(
      &mesh->vdata, CD_MVERT, mesh->totvert);
  mesh->mvert = mv;
  for (int i = 0; i < mesh->totvert; i++, mv++) {
    copy_v3_v3(mv->co, vert_coords[i]);
  }
  BKE_mesh_tag_coords_changed(mesh);
}

void BKE_mesh_vert_coords_apply_with_mat4(Mesh *mesh,
                                          const float (*vert_coords)[3],
                                          const float mat[4][4])
{
  /* This will just return the pointer if it wasn't a referenced layer. */
  MVert *mv = (MVert *)CustomData_duplicate_referenced_layer(
      &mesh->vdata, CD_MVERT, mesh->totvert);
  mesh->mvert = mv;
  for (int i = 0; i < mesh->totvert; i++, mv++) {
    mul_v3_m4v3(mv->co, mat, vert_coords[i]);
  }
  BKE_mesh_tag_coords_changed(mesh);
}

static float (*ensure_corner_normal_layer(Mesh &mesh))[3]
{
  float(*r_loopnors)[3];
  if (CustomData_has_layer(&mesh.ldata, CD_NORMAL)) {
    r_loopnors = (float(*)[3])CustomData_get_layer(&mesh.ldata, CD_NORMAL);
    memset(r_loopnors, 0, sizeof(float[3]) * mesh.totloop);
  }
  else {
    r_loopnors = (float(*)[3])CustomData_add_layer(
        &mesh.ldata, CD_NORMAL, CD_CALLOC, nullptr, mesh.totloop);
    CustomData_set_layer_flag(&mesh.ldata, CD_NORMAL, CD_FLAG_TEMPORARY);
  }
  return r_loopnors;
}

void BKE_mesh_calc_normals_split_ex(Mesh *mesh,
                                    MLoopNorSpaceArray *r_lnors_spacearr,
                                    float (*r_corner_normals)[3])
{
  short(*clnors)[2] = nullptr;

  /* Note that we enforce computing clnors when the clnor space array is requested by caller here.
   * However, we obviously only use the auto-smooth angle threshold
   * only in case auto-smooth is enabled. */
  const bool use_split_normals = (r_lnors_spacearr != nullptr) ||
                                 ((mesh->flag & ME_AUTOSMOOTH) != 0);
  const float split_angle = (mesh->flag & ME_AUTOSMOOTH) != 0 ? mesh->smoothresh : (float)M_PI;

  /* may be nullptr */
  clnors = (short(*)[2])CustomData_get_layer(&mesh->ldata, CD_CUSTOMLOOPNORMAL);

  BKE_mesh_normals_loop_split(mesh->mvert,
                              BKE_mesh_vertex_normals_ensure(mesh),
                              mesh->totvert,
                              mesh->medge,
                              mesh->totedge,
                              mesh->mloop,
                              r_corner_normals,
                              mesh->totloop,
                              mesh->mpoly,
                              BKE_mesh_poly_normals_ensure(mesh),
                              mesh->totpoly,
                              use_split_normals,
                              split_angle,
                              r_lnors_spacearr,
                              clnors,
                              nullptr);

  BKE_mesh_assert_normals_dirty_or_calculated(mesh);
}

void BKE_mesh_calc_normals_split(Mesh *mesh)
{
  BKE_mesh_calc_normals_split_ex(mesh, nullptr, ensure_corner_normal_layer(*mesh));
}

/* Split faces helper functions. */

struct SplitFaceNewVert {
  struct SplitFaceNewVert *next;
  int new_index;
  int orig_index;
  float *vnor;
};

struct SplitFaceNewEdge {
  struct SplitFaceNewEdge *next;
  int new_index;
  int orig_index;
  int v1;
  int v2;
};

/* Detect needed new vertices, and update accordingly loops' vertex indices.
 * WARNING! Leaves mesh in invalid state. */
static int split_faces_prepare_new_verts(Mesh *mesh,
                                         MLoopNorSpaceArray *lnors_spacearr,
                                         SplitFaceNewVert **new_verts,
                                         MemArena *memarena)
{
  /* This is now mandatory, trying to do the job in simple way without that data is doomed to fail,
   * even when only dealing with smooth/flat faces one can find cases that no simple algorithm
   * can handle properly. */
  BLI_assert(lnors_spacearr != nullptr);

  const int loops_len = mesh->totloop;
  int verts_len = mesh->totvert;
  MLoop *mloop = mesh->mloop;
  BKE_mesh_vertex_normals_ensure(mesh);
  float(*vert_normals)[3] = BKE_mesh_vertex_normals_for_write(mesh);

  BLI_bitmap *verts_used = BLI_BITMAP_NEW(verts_len, __func__);
  BLI_bitmap *done_loops = BLI_BITMAP_NEW(loops_len, __func__);

  MLoop *ml = mloop;
  MLoopNorSpace **lnor_space = lnors_spacearr->lspacearr;

  BLI_assert(lnors_spacearr->data_type == MLNOR_SPACEARR_LOOP_INDEX);

  for (int loop_idx = 0; loop_idx < loops_len; loop_idx++, ml++, lnor_space++) {
    if (!BLI_BITMAP_TEST(done_loops, loop_idx)) {
      const int vert_idx = ml->v;
      const bool vert_used = BLI_BITMAP_TEST_BOOL(verts_used, vert_idx);
      /* If vert is already used by another smooth fan, we need a new vert for this one. */
      const int new_vert_idx = vert_used ? verts_len++ : vert_idx;

      BLI_assert(*lnor_space);

      if ((*lnor_space)->flags & MLNOR_SPACE_IS_SINGLE) {
        /* Single loop in this fan... */
        BLI_assert(POINTER_AS_INT((*lnor_space)->loops) == loop_idx);
        BLI_BITMAP_ENABLE(done_loops, loop_idx);
        if (vert_used) {
          ml->v = new_vert_idx;
        }
      }
      else {
        for (LinkNode *lnode = (*lnor_space)->loops; lnode; lnode = lnode->next) {
          const int ml_fan_idx = POINTER_AS_INT(lnode->link);
          BLI_BITMAP_ENABLE(done_loops, ml_fan_idx);
          if (vert_used) {
            mloop[ml_fan_idx].v = new_vert_idx;
          }
        }
      }

      if (!vert_used) {
        BLI_BITMAP_ENABLE(verts_used, vert_idx);
        /* We need to update that vertex's normal here, we won't go over it again. */
        /* This is important! *DO NOT* set vnor to final computed lnor,
         * vnor should always be defined to 'automatic normal' value computed from its polys,
         * not some custom normal.
         * Fortunately, that's the loop normal space's 'lnor' reference vector. ;) */
        copy_v3_v3(vert_normals[vert_idx], (*lnor_space)->vec_lnor);
      }
      else {
        /* Add new vert to list. */
        SplitFaceNewVert *new_vert = (SplitFaceNewVert *)BLI_memarena_alloc(memarena,
                                                                            sizeof(*new_vert));
        new_vert->orig_index = vert_idx;
        new_vert->new_index = new_vert_idx;
        new_vert->vnor = (*lnor_space)->vec_lnor; /* See note above. */
        new_vert->next = *new_verts;
        *new_verts = new_vert;
      }
    }
  }

  MEM_freeN(done_loops);
  MEM_freeN(verts_used);

  return verts_len - mesh->totvert;
}

/* Detect needed new edges, and update accordingly loops' edge indices.
 * WARNING! Leaves mesh in invalid state. */
static int split_faces_prepare_new_edges(const Mesh *mesh,
                                         SplitFaceNewEdge **new_edges,
                                         MemArena *memarena)
{
  const int num_polys = mesh->totpoly;
  int num_edges = mesh->totedge;
  MEdge *medge = mesh->medge;
  MLoop *mloop = mesh->mloop;
  const MPoly *mpoly = mesh->mpoly;

  BLI_bitmap *edges_used = BLI_BITMAP_NEW(num_edges, __func__);
  EdgeHash *edges_hash = BLI_edgehash_new_ex(__func__, num_edges);

  const MPoly *mp = mpoly;
  for (int poly_idx = 0; poly_idx < num_polys; poly_idx++, mp++) {
    MLoop *ml_prev = &mloop[mp->loopstart + mp->totloop - 1];
    MLoop *ml = &mloop[mp->loopstart];
    for (int loop_idx = 0; loop_idx < mp->totloop; loop_idx++, ml++) {
      void **eval;
      if (!BLI_edgehash_ensure_p(edges_hash, ml_prev->v, ml->v, &eval)) {
        const int edge_idx = ml_prev->e;

        /* That edge has not been encountered yet, define it. */
        if (BLI_BITMAP_TEST(edges_used, edge_idx)) {
          /* Original edge has already been used, we need to define a new one. */
          const int new_edge_idx = num_edges++;
          *eval = POINTER_FROM_INT(new_edge_idx);
          ml_prev->e = new_edge_idx;

          SplitFaceNewEdge *new_edge = (SplitFaceNewEdge *)BLI_memarena_alloc(memarena,
                                                                              sizeof(*new_edge));
          new_edge->orig_index = edge_idx;
          new_edge->new_index = new_edge_idx;
          new_edge->v1 = ml_prev->v;
          new_edge->v2 = ml->v;
          new_edge->next = *new_edges;
          *new_edges = new_edge;
        }
        else {
          /* We can re-use original edge. */
          medge[edge_idx].v1 = ml_prev->v;
          medge[edge_idx].v2 = ml->v;
          *eval = POINTER_FROM_INT(edge_idx);
          BLI_BITMAP_ENABLE(edges_used, edge_idx);
        }
      }
      else {
        /* Edge already known, just update loop's edge index. */
        ml_prev->e = POINTER_AS_INT(*eval);
      }

      ml_prev = ml;
    }
  }

  MEM_freeN(edges_used);
  BLI_edgehash_free(edges_hash, nullptr);

  return num_edges - mesh->totedge;
}

/* Perform actual split of vertices. */
static void split_faces_split_new_verts(Mesh *mesh,
                                        SplitFaceNewVert *new_verts,
                                        const int num_new_verts)
{
  const int verts_len = mesh->totvert - num_new_verts;
  MVert *mvert = mesh->mvert;
  float(*vert_normals)[3] = BKE_mesh_vertex_normals_for_write(mesh);

  /* Normals were already calculated at the beginning of this operation, we rely on that to update
   * them partially here. */
  BLI_assert(!BKE_mesh_vertex_normals_are_dirty(mesh));

  /* Remember new_verts is a single linklist, so its items are in reversed order... */
  MVert *new_mv = &mvert[mesh->totvert - 1];
  for (int i = mesh->totvert - 1; i >= verts_len; i--, new_mv--, new_verts = new_verts->next) {
    BLI_assert(new_verts->new_index == i);
    BLI_assert(new_verts->new_index != new_verts->orig_index);
    CustomData_copy_data(&mesh->vdata, &mesh->vdata, new_verts->orig_index, i, 1);
    if (new_verts->vnor) {
      copy_v3_v3(vert_normals[i], new_verts->vnor);
    }
  }
}

/* Perform actual split of edges. */
static void split_faces_split_new_edges(Mesh *mesh,
                                        SplitFaceNewEdge *new_edges,
                                        const int num_new_edges)
{
  const int num_edges = mesh->totedge - num_new_edges;
  MEdge *medge = mesh->medge;

  /* Remember new_edges is a single linklist, so its items are in reversed order... */
  MEdge *new_med = &medge[mesh->totedge - 1];
  for (int i = mesh->totedge - 1; i >= num_edges; i--, new_med--, new_edges = new_edges->next) {
    BLI_assert(new_edges->new_index == i);
    BLI_assert(new_edges->new_index != new_edges->orig_index);
    CustomData_copy_data(&mesh->edata, &mesh->edata, new_edges->orig_index, i, 1);
    new_med->v1 = new_edges->v1;
    new_med->v2 = new_edges->v2;
  }
}

void BKE_mesh_split_faces(Mesh *mesh, bool free_loop_normals)
{
  const int num_polys = mesh->totpoly;

  if (num_polys == 0) {
    return;
  }
  BKE_mesh_tessface_clear(mesh);

  MLoopNorSpaceArray lnors_spacearr = {nullptr};
  /* Compute loop normals and loop normal spaces (a.k.a. smooth fans of faces around vertices). */
  BKE_mesh_calc_normals_split_ex(mesh, &lnors_spacearr, ensure_corner_normal_layer(*mesh));
  /* Stealing memarena from loop normals space array. */
  MemArena *memarena = lnors_spacearr.mem;

  SplitFaceNewVert *new_verts = nullptr;
  SplitFaceNewEdge *new_edges = nullptr;

  /* Ensure we own the layers, we need to do this before split_faces_prepare_new_verts as it will
   * directly assign new indices to existing edges and loops. */
  CustomData_duplicate_referenced_layers(&mesh->vdata, mesh->totvert);
  CustomData_duplicate_referenced_layers(&mesh->edata, mesh->totedge);
  CustomData_duplicate_referenced_layers(&mesh->ldata, mesh->totloop);
  /* Update pointers in case we duplicated referenced layers. */
  BKE_mesh_update_customdata_pointers(mesh, false);

  /* Detect loop normal spaces (a.k.a. smooth fans) that will need a new vert. */
  const int num_new_verts = split_faces_prepare_new_verts(
      mesh, &lnors_spacearr, &new_verts, memarena);

  if (num_new_verts > 0) {
    /* Reminder: beyond this point, there is no way out, mesh is in invalid state
     * (due to early-reassignment of loops' vertex and edge indices to new,
     * to-be-created split ones). */

    const int num_new_edges = split_faces_prepare_new_edges(mesh, &new_edges, memarena);
    /* We can have to split a vertex without having to add a single new edge... */
    const bool do_edges = (num_new_edges > 0);

    /* Reallocate all vert and edge related data. */
    mesh->totvert += num_new_verts;
    CustomData_realloc(&mesh->vdata, mesh->totvert);
    if (do_edges) {
      mesh->totedge += num_new_edges;
      CustomData_realloc(&mesh->edata, mesh->totedge);
    }
    /* Update pointers to a newly allocated memory. */
    BKE_mesh_update_customdata_pointers(mesh, false);

    /* Update normals manually to avoid recalculation after this operation. */
    mesh->runtime.vert_normals = (float(*)[3])MEM_reallocN(mesh->runtime.vert_normals,
                                                           sizeof(float[3]) * mesh->totvert);

    /* Perform actual split of vertices and edges. */
    split_faces_split_new_verts(mesh, new_verts, num_new_verts);
    if (do_edges) {
      split_faces_split_new_edges(mesh, new_edges, num_new_edges);
    }
  }

  /* NOTE: after this point mesh is expected to be valid again. */

  /* CD_NORMAL is expected to be temporary only. */
  if (free_loop_normals) {
    CustomData_free_layers(&mesh->ldata, CD_NORMAL, mesh->totloop);
  }

  /* Also frees new_verts/edges temp data, since we used its memarena to allocate them. */
  BKE_lnor_spacearr_free(&lnors_spacearr);

  BKE_mesh_assert_normals_dirty_or_calculated(mesh);
#ifdef VALIDATE_MESH
  BKE_mesh_validate(mesh, true, true);
#endif
}

/* **** Depsgraph evaluation **** */

void BKE_mesh_eval_geometry(Depsgraph *depsgraph, Mesh *mesh)
{
  DEG_debug_print_eval(depsgraph, __func__, mesh->id.name, mesh);
  BKE_mesh_texspace_calc(mesh);
  /* We are here because something did change in the mesh. This means we can not trust the existing
   * evaluated mesh, and we don't know what parts of the mesh did change. So we simply delete the
   * evaluated mesh and let objects to re-create it with updated settings. */
  if (mesh->runtime.mesh_eval != nullptr) {
    mesh->runtime.mesh_eval->edit_mesh = nullptr;
    BKE_id_free(nullptr, mesh->runtime.mesh_eval);
    mesh->runtime.mesh_eval = nullptr;
  }
  if (DEG_is_active(depsgraph)) {
    Mesh *mesh_orig = (Mesh *)DEG_get_original_id(&mesh->id);
    if (mesh->texflag & ME_AUTOSPACE_EVALUATED) {
      mesh_orig->texflag |= ME_AUTOSPACE_EVALUATED;
      copy_v3_v3(mesh_orig->loc, mesh->loc);
      copy_v3_v3(mesh_orig->size, mesh->size);
    }
  }
}

