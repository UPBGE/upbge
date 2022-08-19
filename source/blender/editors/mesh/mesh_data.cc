/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2009 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup edmesh
 */

#include "MEM_guardedalloc.h"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_view3d_types.h"

#include "BLI_array.hh"
#include "BLI_math.h"
#include "BLI_utildefines.h"

#include "BKE_attribute.h"
#include "BKE_context.h"
#include "BKE_customdata.h"
#include "BKE_editmesh.h"
#include "BKE_mesh.h"
#include "BKE_mesh_runtime.h"
#include "BKE_report.h"

#include "DEG_depsgraph.h"

#include "RNA_access.h"
#include "RNA_define.h"
#include "RNA_prototypes.h"

#include "WM_api.h"
#include "WM_types.h"

#include "ED_mesh.h"
#include "ED_object.h"
#include "ED_paint.h"
#include "ED_screen.h"
#include "ED_uvedit.h"
#include "ED_view3d.h"

#include "mesh_intern.h" /* own include */

using blender::Array;

static CustomData *mesh_customdata_get_type(Mesh *me, const char htype, int *r_tot)
{
  CustomData *data;
  BMesh *bm = (me->edit_mesh) ? me->edit_mesh->bm : nullptr;
  int tot;

  switch (htype) {
    case BM_VERT:
      if (bm) {
        data = &bm->vdata;
        tot = bm->totvert;
      }
      else {
        data = &me->vdata;
        tot = me->totvert;
      }
      break;
    case BM_EDGE:
      if (bm) {
        data = &bm->edata;
        tot = bm->totedge;
      }
      else {
        data = &me->edata;
        tot = me->totedge;
      }
      break;
    case BM_LOOP:
      if (bm) {
        data = &bm->ldata;
        tot = bm->totloop;
      }
      else {
        data = &me->ldata;
        tot = me->totloop;
      }
      break;
    case BM_FACE:
      if (bm) {
        data = &bm->pdata;
        tot = bm->totface;
      }
      else {
        data = &me->pdata;
        tot = me->totpoly;
      }
      break;
    default:
      BLI_assert(0);
      tot = 0;
      data = nullptr;
      break;
  }

  *r_tot = tot;
  return data;
}

#define GET_CD_DATA(me, data) ((me)->edit_mesh ? &(me)->edit_mesh->bm->data : &(me)->data)
static void delete_customdata_layer(Mesh *me, CustomDataLayer *layer)
{
  const int type = layer->type;
  CustomData *data;
  int layer_index, tot, n;

  char htype = BM_FACE;
  if (ELEM(type, CD_PROP_BYTE_COLOR, CD_MLOOPUV)) {
    htype = BM_LOOP;
  }
  else if (ELEM(type, CD_PROP_COLOR)) {
    htype = BM_VERT;
  }

  data = mesh_customdata_get_type(me, htype, &tot);
  layer_index = CustomData_get_layer_index(data, type);
  n = (layer - &data->layers[layer_index]);
  BLI_assert(n >= 0 && (n + layer_index) < data->totlayer);

  if (me->edit_mesh) {
    BM_data_layer_free_n(me->edit_mesh->bm, data, type, n);
  }
  else {
    CustomData_free_layer(data, type, tot, layer_index + n);
    BKE_mesh_update_customdata_pointers(me, true);
  }
}

static void mesh_uv_reset_array(float **fuv, const int len)
{
  if (len == 3) {
    fuv[0][0] = 0.0;
    fuv[0][1] = 0.0;

    fuv[1][0] = 1.0;
    fuv[1][1] = 0.0;

    fuv[2][0] = 1.0;
    fuv[2][1] = 1.0;
  }
  else if (len == 4) {
    fuv[0][0] = 0.0;
    fuv[0][1] = 0.0;

    fuv[1][0] = 1.0;
    fuv[1][1] = 0.0;

    fuv[2][0] = 1.0;
    fuv[2][1] = 1.0;

    fuv[3][0] = 0.0;
    fuv[3][1] = 1.0;
    /* Make sure we ignore 2-sided faces. */
  }
  else if (len > 2) {
    float fac = 0.0f, dfac = 1.0f / (float)len;

    dfac *= (float)M_PI * 2.0f;

    for (int i = 0; i < len; i++) {
      fuv[i][0] = 0.5f * sinf(fac) + 0.5f;
      fuv[i][1] = 0.5f * cosf(fac) + 0.5f;

      fac += dfac;
    }
  }
}

static void mesh_uv_reset_bmface(BMFace *f, const int cd_loop_uv_offset)
{
  Array<float *, BM_DEFAULT_NGON_STACK_SIZE> fuv(f->len);
  BMIter liter;
  BMLoop *l;
  int i;

  BM_ITER_ELEM_INDEX (l, &liter, f, BM_LOOPS_OF_FACE, i) {
    fuv[i] = ((MLoopUV *)BM_ELEM_CD_GET_VOID_P(l, cd_loop_uv_offset))->uv;
  }

  mesh_uv_reset_array(fuv.data(), f->len);
}

static void mesh_uv_reset_mface(MPoly *mp, MLoopUV *mloopuv)
{
  Array<float *, BM_DEFAULT_NGON_STACK_SIZE> fuv(mp->totloop);

  for (int i = 0; i < mp->totloop; i++) {
    fuv[i] = mloopuv[mp->loopstart + i].uv;
  }

  mesh_uv_reset_array(fuv.data(), mp->totloop);
}

void ED_mesh_uv_loop_reset_ex(Mesh *me, const int layernum)
{
  BMEditMesh *em = me->edit_mesh;

  if (em) {
    /* Collect BMesh UVs */
    const int cd_loop_uv_offset = CustomData_get_n_offset(&em->bm->ldata, CD_MLOOPUV, layernum);

    BMFace *efa;
    BMIter iter;

    BLI_assert(cd_loop_uv_offset != -1);

    BM_ITER_MESH (efa, &iter, em->bm, BM_FACES_OF_MESH) {
      if (!BM_elem_flag_test(efa, BM_ELEM_SELECT)) {
        continue;
      }

      mesh_uv_reset_bmface(efa, cd_loop_uv_offset);
    }
  }
  else {
    /* Collect Mesh UVs */
    BLI_assert(CustomData_has_layer(&me->ldata, CD_MLOOPUV));
    MLoopUV *mloopuv = (MLoopUV *)CustomData_get_layer_n(&me->ldata, CD_MLOOPUV, layernum);

    for (int i = 0; i < me->totpoly; i++) {
      mesh_uv_reset_mface(&me->mpoly[i], mloopuv);
    }
  }

  DEG_id_tag_update(&me->id, 0);
}

void ED_mesh_uv_loop_reset(bContext *C, Mesh *me)
{
  /* could be ldata or pdata */
  CustomData *ldata = GET_CD_DATA(me, ldata);
  const int layernum = CustomData_get_active_layer(ldata, CD_MLOOPUV);
  ED_mesh_uv_loop_reset_ex(me, layernum);

  WM_event_add_notifier(C, NC_GEOM | ND_DATA, me);
}

int ED_mesh_uv_add(
    Mesh *me, const char *name, const bool active_set, const bool do_init, ReportList *reports)
{
  /* NOTE: keep in sync with #ED_mesh_color_add. */

  BMEditMesh *em;
  int layernum_dst;

  bool is_init = false;

  if (me->edit_mesh) {
    em = me->edit_mesh;

    layernum_dst = CustomData_number_of_layers(&em->bm->ldata, CD_MLOOPUV);
    if (layernum_dst >= MAX_MTFACE) {
      BKE_reportf(reports, RPT_WARNING, "Cannot add more than %i UV maps", MAX_MTFACE);
      return -1;
    }

    /* CD_MLOOPUV */
    BM_data_layer_add_named(em->bm, &em->bm->ldata, CD_MLOOPUV, name);
    /* copy data from active UV */
    if (layernum_dst && do_init) {
      const int layernum_src = CustomData_get_active_layer(&em->bm->ldata, CD_MLOOPUV);
      BM_data_layer_copy(em->bm, &em->bm->ldata, CD_MLOOPUV, layernum_src, layernum_dst);

      is_init = true;
    }
    if (active_set || layernum_dst == 0) {
      CustomData_set_layer_active(&em->bm->ldata, CD_MLOOPUV, layernum_dst);
    }
  }
  else {
    layernum_dst = CustomData_number_of_layers(&me->ldata, CD_MLOOPUV);
    if (layernum_dst >= MAX_MTFACE) {
      BKE_reportf(reports, RPT_WARNING, "Cannot add more than %i UV maps", MAX_MTFACE);
      return -1;
    }

    if (me->mloopuv && do_init) {
      CustomData_add_layer_named(
          &me->ldata, CD_MLOOPUV, CD_DUPLICATE, me->mloopuv, me->totloop, name);
      is_init = true;
    }
    else {
      CustomData_add_layer_named(&me->ldata, CD_MLOOPUV, CD_DEFAULT, nullptr, me->totloop, name);
    }

    if (active_set || layernum_dst == 0) {
      CustomData_set_layer_active(&me->ldata, CD_MLOOPUV, layernum_dst);
    }

    BKE_mesh_update_customdata_pointers(me, true);
  }

  /* don't overwrite our copied coords */
  if (!is_init && do_init) {
    ED_mesh_uv_loop_reset_ex(me, layernum_dst);
  }

  DEG_id_tag_update(&me->id, 0);
  WM_main_add_notifier(NC_GEOM | ND_DATA, me);

  return layernum_dst;
}

void ED_mesh_uv_ensure(Mesh *me, const char *name)
{
  BMEditMesh *em;
  int layernum_dst;

  if (me->edit_mesh) {
    em = me->edit_mesh;

    layernum_dst = CustomData_number_of_layers(&em->bm->ldata, CD_MLOOPUV);
    if (layernum_dst == 0) {
      ED_mesh_uv_add(me, name, true, true, nullptr);
    }
  }
  else {
    layernum_dst = CustomData_number_of_layers(&me->ldata, CD_MLOOPUV);
    if (layernum_dst == 0) {
      ED_mesh_uv_add(me, name, true, true, nullptr);
    }
  }
}

bool ED_mesh_uv_remove_index(Mesh *me, const int n)
{
  CustomData *ldata = GET_CD_DATA(me, ldata);
  CustomDataLayer *cdlu;
  int index;

  index = CustomData_get_layer_index_n(ldata, CD_MLOOPUV, n);
  cdlu = (index == -1) ? nullptr : &ldata->layers[index];

  if (!cdlu) {
    return false;
  }

  delete_customdata_layer(me, cdlu);

  DEG_id_tag_update(&me->id, 0);
  WM_main_add_notifier(NC_GEOM | ND_DATA, me);

  return true;
}
bool ED_mesh_uv_remove_active(Mesh *me)
{
  CustomData *ldata = GET_CD_DATA(me, ldata);
  const int n = CustomData_get_active_layer(ldata, CD_MLOOPUV);

  if (n != -1) {
    return ED_mesh_uv_remove_index(me, n);
  }
  return false;
}
bool ED_mesh_uv_remove_named(Mesh *me, const char *name)
{
  CustomData *ldata = GET_CD_DATA(me, ldata);
  const int n = CustomData_get_named_layer(ldata, CD_MLOOPUV, name);
  if (n != -1) {
    return ED_mesh_uv_remove_index(me, n);
  }
  return false;
}

int ED_mesh_color_add(
    Mesh *me, const char *name, const bool active_set, const bool do_init, ReportList *reports)
{
  /* NOTE: keep in sync with #ED_mesh_uv_add. */

  BMEditMesh *em;
  int layernum;

  if (me->edit_mesh) {
    em = me->edit_mesh;

    layernum = CustomData_number_of_layers(&em->bm->ldata, CD_PROP_BYTE_COLOR);
    if (layernum >= MAX_MCOL) {
      BKE_reportf(reports, RPT_WARNING, "Cannot add more than %i vertex color layers", MAX_MCOL);
      return -1;
    }

    /* CD_PROP_BYTE_COLOR */
    BM_data_layer_add_named(em->bm, &em->bm->ldata, CD_PROP_BYTE_COLOR, name);
    /* copy data from active vertex color layer */
    if (layernum && do_init) {
      const int layernum_dst = CustomData_get_active_layer(&em->bm->ldata, CD_PROP_BYTE_COLOR);
      BM_data_layer_copy(em->bm, &em->bm->ldata, CD_PROP_BYTE_COLOR, layernum_dst, layernum);
    }
    if (active_set || layernum == 0) {
      CustomData_set_layer_active(&em->bm->ldata, CD_PROP_BYTE_COLOR, layernum);
    }
  }
  else {
    layernum = CustomData_number_of_layers(&me->ldata, CD_PROP_BYTE_COLOR);
    if (layernum >= MAX_MCOL) {
      BKE_reportf(reports, RPT_WARNING, "Cannot add more than %i vertex color layers", MAX_MCOL);
      return -1;
    }

    if (me->mloopcol && do_init) {
      CustomData_add_layer_named(
          &me->ldata, CD_PROP_BYTE_COLOR, CD_DUPLICATE, me->mloopcol, me->totloop, name);
    }
    else {
      CustomData_add_layer_named(
          &me->ldata, CD_PROP_BYTE_COLOR, CD_DEFAULT, nullptr, me->totloop, name);
    }

    if (active_set || layernum == 0) {
      CustomData_set_layer_active(&me->ldata, CD_PROP_BYTE_COLOR, layernum);
    }

    BKE_mesh_update_customdata_pointers(me, true);
  }

  DEG_id_tag_update(&me->id, 0);
  WM_main_add_notifier(NC_GEOM | ND_DATA, me);

  return layernum;
}

bool ED_mesh_color_ensure(Mesh *me, const char *name)
{
  BLI_assert(me->edit_mesh == nullptr);
  CustomDataLayer *layer = BKE_id_attributes_active_color_get(&me->id);

  if (!layer) {
    CustomData_add_layer_named(
        &me->ldata, CD_PROP_BYTE_COLOR, CD_DEFAULT, nullptr, me->totloop, name);
    layer = me->ldata.layers + CustomData_get_layer_index(&me->ldata, CD_PROP_BYTE_COLOR);

    BKE_id_attributes_active_color_set(&me->id, layer);
    BKE_mesh_update_customdata_pointers(me, true);
  }

  DEG_id_tag_update(&me->id, 0);

  return (layer != nullptr);
}

/*********************** General poll ************************/

static bool layers_poll(bContext *C)
{
  Object *ob = ED_object_context(C);
  ID *data = (ob) ? static_cast<ID *>(ob->data) : nullptr;
  return (ob && !ID_IS_LINKED(ob) && !ID_IS_OVERRIDE_LIBRARY(ob) && ob->type == OB_MESH && data &&
          !ID_IS_LINKED(data) && !ID_IS_OVERRIDE_LIBRARY(data));
}

/*********************** Sculpt Vertex colors operators ************************/

int ED_mesh_sculpt_color_add(Mesh *me, const char *name, const bool do_init, ReportList *reports)
{
  /* NOTE: keep in sync with #ED_mesh_uv_add. */

  BMEditMesh *em;
  int layernum;

  if (me->edit_mesh) {
    em = me->edit_mesh;

    layernum = CustomData_number_of_layers(&em->bm->vdata, CD_PROP_COLOR);
    if (layernum >= MAX_MCOL) {
      BKE_reportf(
          reports, RPT_WARNING, "Cannot add more than %i sculpt vertex color layers", MAX_MCOL);
      return -1;
    }

    /* CD_PROP_COLOR */
    BM_data_layer_add_named(em->bm, &em->bm->vdata, CD_PROP_COLOR, name);
    /* copy data from active vertex color layer */
    if (layernum && do_init) {
      const int layernum_dst = CustomData_get_active_layer(&em->bm->vdata, CD_PROP_COLOR);
      BM_data_layer_copy(em->bm, &em->bm->vdata, CD_PROP_COLOR, layernum_dst, layernum);
    }
    if (layernum == 0) {
      CustomData_set_layer_active(&em->bm->vdata, CD_PROP_COLOR, layernum);
    }
  }
  else {
    layernum = CustomData_number_of_layers(&me->vdata, CD_PROP_COLOR);
    if (layernum >= MAX_MCOL) {
      BKE_reportf(
          reports, RPT_WARNING, "Cannot add more than %i sculpt vertex color layers", MAX_MCOL);
      return -1;
    }

    if (CustomData_has_layer(&me->vdata, CD_PROP_COLOR) && do_init) {
      const MPropCol *color_data = (const MPropCol *)CustomData_get_layer(&me->vdata,
                                                                          CD_PROP_COLOR);
      CustomData_add_layer_named(
          &me->vdata, CD_PROP_COLOR, CD_DUPLICATE, (MPropCol *)color_data, me->totvert, name);
    }
    else {
      CustomData_add_layer_named(
          &me->vdata, CD_PROP_COLOR, CD_DEFAULT, nullptr, me->totvert, name);
    }

    if (layernum == 0) {
      CustomData_set_layer_active(&me->vdata, CD_PROP_COLOR, layernum);
    }

    BKE_mesh_update_customdata_pointers(me, true);
  }

  DEG_id_tag_update(&me->id, 0);
  WM_main_add_notifier(NC_GEOM | ND_DATA, me);

  return layernum;
}

/*********************** UV texture operators ************************/

static bool uv_texture_remove_poll(bContext *C)
{
  if (!layers_poll(C)) {
    return false;
  }

  Object *ob = ED_object_context(C);
  Mesh *me = static_cast<Mesh *>(ob->data);
  CustomData *ldata = GET_CD_DATA(me, ldata);
  const int active = CustomData_get_active_layer(ldata, CD_MLOOPUV);
  if (active != -1) {
    return true;
  }

  return false;
}

static int mesh_uv_texture_add_exec(bContext *C, wmOperator *op)
{
  Object *ob = ED_object_context(C);
  Mesh *me = static_cast<Mesh *>(ob->data);

  if (ED_mesh_uv_add(me, nullptr, true, true, op->reports) == -1) {
    return OPERATOR_CANCELLED;
  }

  if (ob->mode & OB_MODE_TEXTURE_PAINT) {
    Scene *scene = CTX_data_scene(C);
    ED_paint_proj_mesh_data_check(scene, ob, nullptr, nullptr, nullptr, nullptr);
    WM_event_add_notifier(C, NC_SCENE | ND_TOOLSETTINGS, nullptr);
  }

  return OPERATOR_FINISHED;
}

void MESH_OT_uv_texture_add(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Add UV Map";
  ot->description = "Add UV map";
  ot->idname = "MESH_OT_uv_texture_add";

  /* api callbacks */
  ot->poll = layers_poll;
  ot->exec = mesh_uv_texture_add_exec;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static int mesh_uv_texture_remove_exec(bContext *C, wmOperator *UNUSED(op))
{
  Object *ob = ED_object_context(C);
  Mesh *me = static_cast<Mesh *>(ob->data);

  if (!ED_mesh_uv_remove_active(me)) {
    return OPERATOR_CANCELLED;
  }

  if (ob->mode & OB_MODE_TEXTURE_PAINT) {
    Scene *scene = CTX_data_scene(C);
    ED_paint_proj_mesh_data_check(scene, ob, nullptr, nullptr, nullptr, nullptr);
    WM_event_add_notifier(C, NC_SCENE | ND_TOOLSETTINGS, nullptr);
  }

  return OPERATOR_FINISHED;
}

void MESH_OT_uv_texture_remove(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Remove UV Map";
  ot->description = "Remove UV map";
  ot->idname = "MESH_OT_uv_texture_remove";

  /* api callbacks */
  ot->poll = uv_texture_remove_poll;
  ot->exec = mesh_uv_texture_remove_exec;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/* *** CustomData clear functions, we need an operator for each *** */

static int mesh_customdata_clear_exec__internal(bContext *C, char htype, int type)
{
  Mesh *me = ED_mesh_context(C);

  int tot;
  CustomData *data = mesh_customdata_get_type(me, htype, &tot);

  BLI_assert(CustomData_layertype_is_singleton(type) == true);

  if (CustomData_has_layer(data, type)) {
    if (me->edit_mesh) {
      BM_data_layer_free(me->edit_mesh->bm, data, type);
    }
    else {
      CustomData_free_layers(data, type, tot);
    }

    DEG_id_tag_update(&me->id, 0);
    WM_event_add_notifier(C, NC_GEOM | ND_DATA, me);

    return OPERATOR_FINISHED;
  }
  return OPERATOR_CANCELLED;
}

/* Clear Mask */
static bool mesh_customdata_mask_clear_poll(bContext *C)
{
  Object *ob = ED_object_context(C);
  if (ob && ob->type == OB_MESH) {
    Mesh *me = static_cast<Mesh *>(ob->data);

    /* special case - can't run this if we're in sculpt mode */
    if (ob->mode & OB_MODE_SCULPT) {
      return false;
    }

    if (!ID_IS_LINKED(me) && !ID_IS_OVERRIDE_LIBRARY(me)) {
      CustomData *data = GET_CD_DATA(me, vdata);
      if (CustomData_has_layer(data, CD_PAINT_MASK)) {
        return true;
      }
      data = GET_CD_DATA(me, ldata);
      if (CustomData_has_layer(data, CD_GRID_PAINT_MASK)) {
        return true;
      }
    }
  }
  return false;
}
static int mesh_customdata_mask_clear_exec(bContext *C, wmOperator *UNUSED(op))
{
  int ret_a = mesh_customdata_clear_exec__internal(C, BM_VERT, CD_PAINT_MASK);
  int ret_b = mesh_customdata_clear_exec__internal(C, BM_LOOP, CD_GRID_PAINT_MASK);

  if (ret_a == OPERATOR_FINISHED || ret_b == OPERATOR_FINISHED) {
    return OPERATOR_FINISHED;
  }
  return OPERATOR_CANCELLED;
}

void MESH_OT_customdata_mask_clear(wmOperatorType *ot)
{
  /* NOTE: no create_mask yet */

  /* identifiers */
  ot->name = "Clear Sculpt Mask Data";
  ot->idname = "MESH_OT_customdata_mask_clear";
  ot->description = "Clear vertex sculpt masking data from the mesh";

  /* api callbacks */
  ot->exec = mesh_customdata_mask_clear_exec;
  ot->poll = mesh_customdata_mask_clear_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/**
 * Clear Skin
 * \return -1 invalid state, 0 no skin, 1 has skin.
 */
static int mesh_customdata_skin_state(bContext *C)
{
  Object *ob = ED_object_context(C);

  if (ob && ob->type == OB_MESH) {
    Mesh *me = static_cast<Mesh *>(ob->data);
    if (!ID_IS_LINKED(me) && !ID_IS_OVERRIDE_LIBRARY(me)) {
      CustomData *data = GET_CD_DATA(me, vdata);
      return CustomData_has_layer(data, CD_MVERT_SKIN);
    }
  }
  return -1;
}

static bool mesh_customdata_skin_add_poll(bContext *C)
{
  return (mesh_customdata_skin_state(C) == 0);
}

static int mesh_customdata_skin_add_exec(bContext *C, wmOperator *UNUSED(op))
{
  Object *ob = ED_object_context(C);
  Mesh *me = static_cast<Mesh *>(ob->data);

  BKE_mesh_ensure_skin_customdata(me);

  DEG_id_tag_update(&me->id, 0);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, me);

  return OPERATOR_FINISHED;
}

void MESH_OT_customdata_skin_add(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Add Skin Data";
  ot->idname = "MESH_OT_customdata_skin_add";
  ot->description = "Add a vertex skin layer";

  /* api callbacks */
  ot->exec = mesh_customdata_skin_add_exec;
  ot->poll = mesh_customdata_skin_add_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static bool mesh_customdata_skin_clear_poll(bContext *C)
{
  return (mesh_customdata_skin_state(C) == 1);
}

static int mesh_customdata_skin_clear_exec(bContext *C, wmOperator *UNUSED(op))
{
  return mesh_customdata_clear_exec__internal(C, BM_VERT, CD_MVERT_SKIN);
}

void MESH_OT_customdata_skin_clear(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Clear Skin Data";
  ot->idname = "MESH_OT_customdata_skin_clear";
  ot->description = "Clear vertex skin layer";

  /* api callbacks */
  ot->exec = mesh_customdata_skin_clear_exec;
  ot->poll = mesh_customdata_skin_clear_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/* Clear custom loop normals */
static int mesh_customdata_custom_splitnormals_add_exec(bContext *C, wmOperator *UNUSED(op))
{
  Mesh *me = ED_mesh_context(C);

  if (!BKE_mesh_has_custom_loop_normals(me)) {
    CustomData *data = GET_CD_DATA(me, ldata);

    if (me->edit_mesh) {
      /* Tag edges as sharp according to smooth threshold if needed,
       * to preserve autosmooth shading. */
      if (me->flag & ME_AUTOSMOOTH) {
        BM_edges_sharp_from_angle_set(me->edit_mesh->bm, me->smoothresh);
      }

      BM_data_layer_add(me->edit_mesh->bm, data, CD_CUSTOMLOOPNORMAL);
    }
    else {
      /* Tag edges as sharp according to smooth threshold if needed,
       * to preserve autosmooth shading. */
      if (me->flag & ME_AUTOSMOOTH) {
        BKE_edges_sharp_from_angle_set(me->mvert,
                                       me->totvert,
                                       me->medge,
                                       me->totedge,
                                       me->mloop,
                                       me->totloop,
                                       me->mpoly,
                                       BKE_mesh_poly_normals_ensure(me),
                                       me->totpoly,
                                       me->smoothresh);
      }

      CustomData_add_layer(data, CD_CUSTOMLOOPNORMAL, CD_DEFAULT, nullptr, me->totloop);
    }

    DEG_id_tag_update(&me->id, 0);
    WM_event_add_notifier(C, NC_GEOM | ND_DATA, me);

    return OPERATOR_FINISHED;
  }
  return OPERATOR_CANCELLED;
}

void MESH_OT_customdata_custom_splitnormals_add(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Add Custom Split Normals Data";
  ot->idname = "MESH_OT_customdata_custom_splitnormals_add";
  ot->description = "Add a custom split normals layer, if none exists yet";

  /* api callbacks */
  ot->exec = mesh_customdata_custom_splitnormals_add_exec;
  ot->poll = ED_operator_editable_mesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static int mesh_customdata_custom_splitnormals_clear_exec(bContext *C, wmOperator *UNUSED(op))
{
  Mesh *me = ED_mesh_context(C);

  if (BKE_mesh_has_custom_loop_normals(me)) {
    BMEditMesh *em = me->edit_mesh;
    if (em != nullptr && em->bm->lnor_spacearr != nullptr) {
      BKE_lnor_spacearr_clear(em->bm->lnor_spacearr);
    }
    return mesh_customdata_clear_exec__internal(C, BM_LOOP, CD_CUSTOMLOOPNORMAL);
  }
  return OPERATOR_CANCELLED;
}

void MESH_OT_customdata_custom_splitnormals_clear(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Clear Custom Split Normals Data";
  ot->idname = "MESH_OT_customdata_custom_splitnormals_clear";
  ot->description = "Remove the custom split normals layer, if it exists";

  /* api callbacks */
  ot->exec = mesh_customdata_custom_splitnormals_clear_exec;
  ot->poll = ED_operator_editable_mesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/************************** Add Geometry Layers *************************/

void ED_mesh_update(Mesh *mesh, bContext *C, bool calc_edges, bool calc_edges_loose)
{
  if (calc_edges || ((mesh->totpoly || mesh->totface) && mesh->totedge == 0)) {
    BKE_mesh_calc_edges(mesh, calc_edges, true);
  }

  if (calc_edges_loose && mesh->totedge) {
    BKE_mesh_calc_edges_loose(mesh);
  }

  /* Default state is not to have tessface's so make sure this is the case. */
  BKE_mesh_tessface_clear(mesh);

  /* Tag lazily calculated data as dirty. */
  BKE_mesh_normals_tag_dirty(mesh);

  DEG_id_tag_update(&mesh->id, 0);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, mesh);
}

static void mesh_add_verts(Mesh *mesh, int len)
{
  if (len == 0) {
    return;
  }

  int totvert = mesh->totvert + len;
  CustomData vdata;
  CustomData_copy(&mesh->vdata, &vdata, CD_MASK_MESH.vmask, CD_DEFAULT, totvert);
  CustomData_copy_data(&mesh->vdata, &vdata, 0, 0, mesh->totvert);

  if (!CustomData_has_layer(&vdata, CD_MVERT)) {
    CustomData_add_layer(&vdata, CD_MVERT, CD_CALLOC, nullptr, totvert);
  }

  CustomData_free(&mesh->vdata, mesh->totvert);
  mesh->vdata = vdata;
  BKE_mesh_update_customdata_pointers(mesh, false);

  BKE_mesh_runtime_clear_cache(mesh);

  /* scan the input list and insert the new vertices */

  /* set default flags */
  MVert *mvert = &mesh->mvert[mesh->totvert];
  for (int i = 0; i < len; i++, mvert++) {
    mvert->flag |= SELECT;
  }

  /* set final vertex list size */
  mesh->totvert = totvert;
}

static void mesh_add_edges(Mesh *mesh, int len)
{
  CustomData edata;
  MEdge *medge;
  int i, totedge;

  if (len == 0) {
    return;
  }

  totedge = mesh->totedge + len;

  /* Update custom-data. */
  CustomData_copy(&mesh->edata, &edata, CD_MASK_MESH.emask, CD_DEFAULT, totedge);
  CustomData_copy_data(&mesh->edata, &edata, 0, 0, mesh->totedge);

  if (!CustomData_has_layer(&edata, CD_MEDGE)) {
    CustomData_add_layer(&edata, CD_MEDGE, CD_CALLOC, nullptr, totedge);
  }

  CustomData_free(&mesh->edata, mesh->totedge);
  mesh->edata = edata;
  BKE_mesh_update_customdata_pointers(mesh, false); /* new edges don't change tessellation */

  BKE_mesh_runtime_clear_cache(mesh);

  /* set default flags */
  medge = &mesh->medge[mesh->totedge];
  for (i = 0; i < len; i++, medge++) {
    medge->flag = ME_EDGEDRAW | ME_EDGERENDER | SELECT;
  }

  mesh->totedge = totedge;
}

static void mesh_add_loops(Mesh *mesh, int len)
{
  CustomData ldata;
  int totloop;

  if (len == 0) {
    return;
  }

  totloop = mesh->totloop + len; /* new face count */

  /* update customdata */
  CustomData_copy(&mesh->ldata, &ldata, CD_MASK_MESH.lmask, CD_DEFAULT, totloop);
  CustomData_copy_data(&mesh->ldata, &ldata, 0, 0, mesh->totloop);

  if (!CustomData_has_layer(&ldata, CD_MLOOP)) {
    CustomData_add_layer(&ldata, CD_MLOOP, CD_CALLOC, nullptr, totloop);
  }

  BKE_mesh_runtime_clear_cache(mesh);

  CustomData_free(&mesh->ldata, mesh->totloop);
  mesh->ldata = ldata;
  BKE_mesh_update_customdata_pointers(mesh, true);

  mesh->totloop = totloop;
}

static void mesh_add_polys(Mesh *mesh, int len)
{
  CustomData pdata;
  MPoly *mpoly;
  int i, totpoly;

  if (len == 0) {
    return;
  }

  totpoly = mesh->totpoly + len; /* new face count */

  /* update customdata */
  CustomData_copy(&mesh->pdata, &pdata, CD_MASK_MESH.pmask, CD_DEFAULT, totpoly);
  CustomData_copy_data(&mesh->pdata, &pdata, 0, 0, mesh->totpoly);

  if (!CustomData_has_layer(&pdata, CD_MPOLY)) {
    CustomData_add_layer(&pdata, CD_MPOLY, CD_CALLOC, nullptr, totpoly);
  }

  CustomData_free(&mesh->pdata, mesh->totpoly);
  mesh->pdata = pdata;
  BKE_mesh_update_customdata_pointers(mesh, true);

  BKE_mesh_runtime_clear_cache(mesh);

  /* set default flags */
  mpoly = &mesh->mpoly[mesh->totpoly];
  for (i = 0; i < len; i++, mpoly++) {
    mpoly->flag = ME_FACE_SEL;
  }

  mesh->totpoly = totpoly;
}

/* -------------------------------------------------------------------- */
/** \name Add Geometry
 * \{ */

void ED_mesh_verts_add(Mesh *mesh, ReportList *reports, int count)
{
  if (mesh->edit_mesh) {
    BKE_report(reports, RPT_ERROR, "Cannot add vertices in edit mode");
    return;
  }
  mesh_add_verts(mesh, count);
}

void ED_mesh_edges_add(Mesh *mesh, ReportList *reports, int count)
{
  if (mesh->edit_mesh) {
    BKE_report(reports, RPT_ERROR, "Cannot add edges in edit mode");
    return;
  }
  mesh_add_edges(mesh, count);
}

void ED_mesh_loops_add(Mesh *mesh, ReportList *reports, int count)
{
  if (mesh->edit_mesh) {
    BKE_report(reports, RPT_ERROR, "Cannot add loops in edit mode");
    return;
  }
  mesh_add_loops(mesh, count);
}

void ED_mesh_polys_add(Mesh *mesh, ReportList *reports, int count)
{
  if (mesh->edit_mesh) {
    BKE_report(reports, RPT_ERROR, "Cannot add polygons in edit mode");
    return;
  }
  mesh_add_polys(mesh, count);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Remove Geometry
 * \{ */

static void mesh_remove_verts(Mesh *mesh, int len)
{
  if (len == 0) {
    return;
  }
  const int totvert = mesh->totvert - len;
  CustomData_free_elem(&mesh->vdata, totvert, len);
  mesh->totvert = totvert;
}

static void mesh_remove_edges(Mesh *mesh, int len)
{
  if (len == 0) {
    return;
  }
  const int totedge = mesh->totedge - len;
  CustomData_free_elem(&mesh->edata, totedge, len);
  mesh->totedge = totedge;
}

static void mesh_remove_loops(Mesh *mesh, int len)
{
  if (len == 0) {
    return;
  }
  const int totloop = mesh->totloop - len;
  CustomData_free_elem(&mesh->ldata, totloop, len);
  mesh->totloop = totloop;
}

static void mesh_remove_polys(Mesh *mesh, int len)
{
  if (len == 0) {
    return;
  }
  const int totpoly = mesh->totpoly - len;
  CustomData_free_elem(&mesh->pdata, totpoly, len);
  mesh->totpoly = totpoly;
}

void ED_mesh_verts_remove(Mesh *mesh, ReportList *reports, int count)
{
  if (mesh->edit_mesh) {
    BKE_report(reports, RPT_ERROR, "Cannot remove vertices in edit mode");
    return;
  }
  if (count > mesh->totvert) {
    BKE_report(reports, RPT_ERROR, "Cannot remove more vertices than the mesh contains");
    return;
  }

  mesh_remove_verts(mesh, count);
}

void ED_mesh_edges_remove(Mesh *mesh, ReportList *reports, int count)
{
  if (mesh->edit_mesh) {
    BKE_report(reports, RPT_ERROR, "Cannot remove edges in edit mode");
    return;
  }
  if (count > mesh->totedge) {
    BKE_report(reports, RPT_ERROR, "Cannot remove more edges than the mesh contains");
    return;
  }

  mesh_remove_edges(mesh, count);
}

void ED_mesh_loops_remove(Mesh *mesh, ReportList *reports, int count)
{
  if (mesh->edit_mesh) {
    BKE_report(reports, RPT_ERROR, "Cannot remove loops in edit mode");
    return;
  }
  if (count > mesh->totloop) {
    BKE_report(reports, RPT_ERROR, "Cannot remove more loops than the mesh contains");
    return;
  }

  mesh_remove_loops(mesh, count);
}

void ED_mesh_polys_remove(Mesh *mesh, ReportList *reports, int count)
{
  if (mesh->edit_mesh) {
    BKE_report(reports, RPT_ERROR, "Cannot remove polys in edit mode");
    return;
  }
  if (count > mesh->totpoly) {
    BKE_report(reports, RPT_ERROR, "Cannot remove more polys than the mesh contains");
    return;
  }

  mesh_remove_polys(mesh, count);
}

void ED_mesh_geometry_clear(Mesh *mesh)
{
  mesh_remove_verts(mesh, mesh->totvert);
  mesh_remove_edges(mesh, mesh->totedge);
  mesh_remove_loops(mesh, mesh->totloop);
  mesh_remove_polys(mesh, mesh->totpoly);
}

/** \} */

void ED_mesh_report_mirror_ex(wmOperator *op, int totmirr, int totfail, char selectmode)
{
  const char *elem_type;

  if (selectmode & SCE_SELECT_VERTEX) {
    elem_type = "vertices";
  }
  else if (selectmode & SCE_SELECT_EDGE) {
    elem_type = "edges";
  }
  else {
    elem_type = "faces";
  }

  if (totfail) {
    BKE_reportf(
        op->reports, RPT_WARNING, "%d %s mirrored, %d failed", totmirr, elem_type, totfail);
  }
  else {
    BKE_reportf(op->reports, RPT_INFO, "%d %s mirrored", totmirr, elem_type);
  }
}

void ED_mesh_report_mirror(wmOperator *op, int totmirr, int totfail)
{
  ED_mesh_report_mirror_ex(op, totmirr, totfail, SCE_SELECT_VERTEX);
}

Mesh *ED_mesh_context(bContext *C)
{
  Mesh *mesh = static_cast<Mesh *>(CTX_data_pointer_get_type(C, "mesh", &RNA_Mesh).data);
  if (mesh != nullptr) {
    return mesh;
  }

  Object *ob = ED_object_active_context(C);
  if (ob == nullptr) {
    return nullptr;
  }

  ID *data = (ID *)ob->data;
  if (data == nullptr || GS(data->name) != ID_ME) {
    return nullptr;
  }

  return (Mesh *)data;
}
