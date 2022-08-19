/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2001-2002 NaN Holding BV. All rights reserved. */

/** \file
 * \ingroup edobj
 */

#include <stdlib.h>
#include <string.h>

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_math.h"
#include "BLI_string.h"
#include "BLI_utildefines.h"

#include "DNA_armature_types.h"
#include "DNA_curve_types.h"
#include "DNA_lattice_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BKE_action.h"
#include "BKE_context.h"
#include "BKE_deform.h"
#include "BKE_editmesh.h"
#include "BKE_layer.h"
#include "BKE_main.h"
#include "BKE_modifier.h"
#include "BKE_object.h"
#include "BKE_report.h"
#include "BKE_scene.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_build.h"
#include "DEG_depsgraph_query.h"

#include "RNA_access.h"
#include "RNA_define.h"
#include "RNA_enum_types.h"
#include "RNA_prototypes.h"

#include "ED_curve.h"
#include "ED_mesh.h"
#include "ED_screen.h"

#include "WM_api.h"
#include "WM_types.h"

#include "UI_resources.h"

#include "object_intern.h"

static int return_editmesh_indexar(BMEditMesh *em,
                                   int *r_indexar_num,
                                   int **r_indexar,
                                   float r_cent[3])
{
  BMVert *eve;
  BMIter iter;
  int *index, nr, indexar_num = 0;

  BM_ITER_MESH (eve, &iter, em->bm, BM_VERTS_OF_MESH) {
    if (BM_elem_flag_test(eve, BM_ELEM_SELECT)) {
      indexar_num++;
    }
  }
  if (indexar_num == 0) {
    return 0;
  }

  *r_indexar = index = MEM_mallocN(4 * indexar_num, "hook indexar");
  *r_indexar_num = indexar_num;
  nr = 0;
  zero_v3(r_cent);

  BM_ITER_MESH (eve, &iter, em->bm, BM_VERTS_OF_MESH) {
    if (BM_elem_flag_test(eve, BM_ELEM_SELECT)) {
      *index = nr;
      index++;
      add_v3_v3(r_cent, eve->co);
    }
    nr++;
  }

  mul_v3_fl(r_cent, 1.0f / (float)indexar_num);

  return indexar_num;
}

static bool return_editmesh_vgroup(Object *obedit, BMEditMesh *em, char *r_name, float r_cent[3])
{
  const int active_index = BKE_object_defgroup_active_index_get(obedit);
  const int cd_dvert_offset = active_index ?
                                  CustomData_get_offset(&em->bm->vdata, CD_MDEFORMVERT) :
                                  -1;

  if (cd_dvert_offset != -1) {
    const int defgrp_index = active_index - 1;
    int indexar_num = 0;

    MDeformVert *dvert;
    BMVert *eve;
    BMIter iter;

    /* find the vertices */
    BM_ITER_MESH (eve, &iter, em->bm, BM_VERTS_OF_MESH) {
      dvert = BM_ELEM_CD_GET_VOID_P(eve, cd_dvert_offset);

      if (BKE_defvert_find_weight(dvert, defgrp_index) > 0.0f) {
        add_v3_v3(r_cent, eve->co);
        indexar_num++;
      }
    }
    if (indexar_num) {
      const ListBase *defbase = BKE_object_defgroup_list(obedit);
      bDeformGroup *dg = BLI_findlink(defbase, defgrp_index);
      BLI_strncpy(r_name, dg->name, sizeof(dg->name));
      mul_v3_fl(r_cent, 1.0f / (float)indexar_num);
      return true;
    }
  }

  return false;
}

static void select_editbmesh_hook(Object *ob, HookModifierData *hmd)
{
  Mesh *me = ob->data;
  BMEditMesh *em = me->edit_mesh;
  BMVert *eve;
  BMIter iter;
  int index = 0, nr = 0;

  if (hmd->indexar == NULL) {
    return;
  }

  BM_ITER_MESH (eve, &iter, em->bm, BM_VERTS_OF_MESH) {
    if (nr == hmd->indexar[index]) {
      BM_vert_select_set(em->bm, eve, true);
      if (index < hmd->indexar_num - 1) {
        index++;
      }
    }

    nr++;
  }

  EDBM_select_flush(em);
}

static int return_editlattice_indexar(Lattice *editlatt,
                                      int **r_indexar,
                                      int *r_indexar_num,
                                      float r_cent[3])
{
  BPoint *bp;
  int *index, nr, indexar_num = 0, a;

  /* count */
  a = editlatt->pntsu * editlatt->pntsv * editlatt->pntsw;
  bp = editlatt->def;
  while (a--) {
    if (bp->f1 & SELECT) {
      if (bp->hide == 0) {
        indexar_num++;
      }
    }
    bp++;
  }

  if (indexar_num == 0) {
    return 0;
  }

  *r_indexar = index = MEM_mallocN(4 * indexar_num, "hook indexar");
  *r_indexar_num = indexar_num;
  nr = 0;
  zero_v3(r_cent);

  a = editlatt->pntsu * editlatt->pntsv * editlatt->pntsw;
  bp = editlatt->def;
  while (a--) {
    if (bp->f1 & SELECT) {
      if (bp->hide == 0) {
        *index = nr;
        index++;
        add_v3_v3(r_cent, bp->vec);
      }
    }
    bp++;
    nr++;
  }

  mul_v3_fl(r_cent, 1.0f / (float)indexar_num);

  return indexar_num;
}

static void select_editlattice_hook(Object *obedit, HookModifierData *hmd)
{
  Lattice *lt = obedit->data, *editlt;
  BPoint *bp;
  int index = 0, nr = 0, a;

  editlt = lt->editlatt->latt;
  /* count */
  a = editlt->pntsu * editlt->pntsv * editlt->pntsw;
  bp = editlt->def;
  while (a--) {
    if (hmd->indexar[index] == nr) {
      bp->f1 |= SELECT;
      if (index < hmd->indexar_num - 1) {
        index++;
      }
    }
    nr++;
    bp++;
  }
}

static int return_editcurve_indexar(Object *obedit,
                                    int **r_indexar,
                                    int *r_indexar_num,
                                    float r_cent[3])
{
  ListBase *editnurb = object_editcurve_get(obedit);
  BPoint *bp;
  BezTriple *bezt;
  int *index, a, nr, indexar_num = 0;

  LISTBASE_FOREACH (Nurb *, nu, editnurb) {
    if (nu->type == CU_BEZIER) {
      bezt = nu->bezt;
      a = nu->pntsu;
      while (a--) {
        if (bezt->f1 & SELECT) {
          indexar_num++;
        }
        if (bezt->f2 & SELECT) {
          indexar_num++;
        }
        if (bezt->f3 & SELECT) {
          indexar_num++;
        }
        bezt++;
      }
    }
    else {
      bp = nu->bp;
      a = nu->pntsu * nu->pntsv;
      while (a--) {
        if (bp->f1 & SELECT) {
          indexar_num++;
        }
        bp++;
      }
    }
  }
  if (indexar_num == 0) {
    return 0;
  }

  *r_indexar = index = MEM_mallocN(sizeof(*index) * indexar_num, "hook indexar");
  *r_indexar_num = indexar_num;
  nr = 0;
  zero_v3(r_cent);

  LISTBASE_FOREACH (Nurb *, nu, editnurb) {
    if (nu->type == CU_BEZIER) {
      bezt = nu->bezt;
      a = nu->pntsu;
      while (a--) {
        if (bezt->f1 & SELECT) {
          *index = nr;
          index++;
          add_v3_v3(r_cent, bezt->vec[0]);
        }
        nr++;
        if (bezt->f2 & SELECT) {
          *index = nr;
          index++;
          add_v3_v3(r_cent, bezt->vec[1]);
        }
        nr++;
        if (bezt->f3 & SELECT) {
          *index = nr;
          index++;
          add_v3_v3(r_cent, bezt->vec[2]);
        }
        nr++;
        bezt++;
      }
    }
    else {
      bp = nu->bp;
      a = nu->pntsu * nu->pntsv;
      while (a--) {
        if (bp->f1 & SELECT) {
          *index = nr;
          index++;
          add_v3_v3(r_cent, bp->vec);
        }
        nr++;
        bp++;
      }
    }
  }

  mul_v3_fl(r_cent, 1.0f / (float)indexar_num);

  return indexar_num;
}

static bool object_hook_index_array(Main *bmain,
                                    Scene *scene,
                                    Object *obedit,
                                    int **r_indexar,
                                    int *r_indexar_num,
                                    char *r_name,
                                    float r_cent[3])
{
  *r_indexar = NULL;
  *r_indexar_num = 0;
  r_name[0] = 0;

  switch (obedit->type) {
    case OB_MESH: {
      Mesh *me = obedit->data;

      BMEditMesh *em;

      EDBM_mesh_load(bmain, obedit);
      EDBM_mesh_make(obedit, scene->toolsettings->selectmode, true);

      DEG_id_tag_update(obedit->data, 0);

      em = me->edit_mesh;

      BKE_editmesh_looptri_and_normals_calc(em);

      /* check selected vertices first */
      if (return_editmesh_indexar(em, r_indexar_num, r_indexar, r_cent) == 0) {
        return return_editmesh_vgroup(obedit, em, r_name, r_cent);
      }
      return true;
    }
    case OB_CURVES_LEGACY:
    case OB_SURF:
      ED_curve_editnurb_load(bmain, obedit);
      ED_curve_editnurb_make(obedit);
      return return_editcurve_indexar(obedit, r_indexar, r_indexar_num, r_cent);
    case OB_LATTICE: {
      Lattice *lt = obedit->data;
      return return_editlattice_indexar(lt->editlatt->latt, r_indexar, r_indexar_num, r_cent);
    }
    default:
      return false;
  }
}

static void select_editcurve_hook(Object *obedit, HookModifierData *hmd)
{
  ListBase *editnurb = object_editcurve_get(obedit);
  BPoint *bp;
  BezTriple *bezt;
  int index = 0, a, nr = 0;

  LISTBASE_FOREACH (Nurb *, nu, editnurb) {
    if (nu->type == CU_BEZIER) {
      bezt = nu->bezt;
      a = nu->pntsu;
      while (a--) {
        if (nr == hmd->indexar[index]) {
          bezt->f1 |= SELECT;
          if (index < hmd->indexar_num - 1) {
            index++;
          }
        }
        nr++;
        if (nr == hmd->indexar[index]) {
          bezt->f2 |= SELECT;
          if (index < hmd->indexar_num - 1) {
            index++;
          }
        }
        nr++;
        if (nr == hmd->indexar[index]) {
          bezt->f3 |= SELECT;
          if (index < hmd->indexar_num - 1) {
            index++;
          }
        }
        nr++;

        bezt++;
      }
    }
    else {
      bp = nu->bp;
      a = nu->pntsu * nu->pntsv;
      while (a--) {
        if (nr == hmd->indexar[index]) {
          bp->f1 |= SELECT;
          if (index < hmd->indexar_num - 1) {
            index++;
          }
        }
        nr++;
        bp++;
      }
    }
  }
}

static void object_hook_from_context(
    bContext *C, PointerRNA *ptr, const int num, Object **r_ob, HookModifierData **r_hmd)
{
  Object *ob;
  HookModifierData *hmd;

  if (ptr->data) { /* if modifier context is available, use that */
    ob = (Object *)ptr->owner_id;
    hmd = ptr->data;
  }
  else { /* use the provided property */
    ob = CTX_data_edit_object(C);
    hmd = (HookModifierData *)BLI_findlink(&ob->modifiers, num);
  }

  if (ob && hmd && (hmd->modifier.type == eModifierType_Hook)) {
    *r_ob = ob;
    *r_hmd = hmd;
  }
  else {
    *r_ob = NULL;
    *r_hmd = NULL;
  }
}

static void object_hook_select(Object *ob, HookModifierData *hmd)
{
  if (hmd->indexar == NULL) {
    return;
  }

  if (ob->type == OB_MESH) {
    select_editbmesh_hook(ob, hmd);
  }
  else if (ob->type == OB_LATTICE) {
    select_editlattice_hook(ob, hmd);
  }
  else if (ob->type == OB_CURVES_LEGACY) {
    select_editcurve_hook(ob, hmd);
  }
  else if (ob->type == OB_SURF) {
    select_editcurve_hook(ob, hmd);
  }
}

/* special poll operators for hook operators */
/* TODO: check for properties window modifier context too as alternative? */
static bool hook_op_edit_poll(bContext *C)
{
  Object *obedit = CTX_data_edit_object(C);

  if (obedit) {
    if (ED_operator_editmesh(C)) {
      return true;
    }
    if (ED_operator_editsurfcurve(C)) {
      return true;
    }
    if (ED_operator_editlattice(C)) {
      return true;
    }
    // if (ED_operator_editmball(C)) return true;
  }

  return false;
}

static Object *add_hook_object_new(Main *bmain, ViewLayer *view_layer, View3D *v3d, Object *obedit)
{
  Base *basedit;
  Object *ob;

  ob = BKE_object_add(bmain, view_layer, OB_EMPTY, NULL);

  basedit = BKE_view_layer_base_find(view_layer, obedit);
  BLI_assert(view_layer->basact->object == ob);

  if (v3d && v3d->localvd) {
    view_layer->basact->local_view_bits |= v3d->local_view_uuid;
  }

  /* icky, BKE_object_add sets new base as active.
   * so set it back to the original edit object */
  view_layer->basact = basedit;

  return ob;
}

static int add_hook_object(const bContext *C,
                           Main *bmain,
                           Scene *scene,
                           ViewLayer *view_layer,
                           View3D *v3d,
                           Object *obedit,
                           Object *ob,
                           int mode,
                           ReportList *reports)
{
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  ModifierData *md = NULL;
  HookModifierData *hmd = NULL;
  float cent[3];
  float pose_mat[4][4];
  int indexar_num, ok, *indexar;
  char name[MAX_NAME];

  ok = object_hook_index_array(bmain, scene, obedit, &indexar, &indexar_num, name, cent);

  if (!ok) {
    BKE_report(reports, RPT_ERROR, "Requires selected vertices or active vertex group");
    return false;
  }

  if (mode == OBJECT_ADDHOOK_NEWOB && !ob) {

    ob = add_hook_object_new(bmain, view_layer, v3d, obedit);

    /* transform cent to global coords for loc */
    mul_v3_m4v3(ob->loc, obedit->obmat, cent);
  }

  md = obedit->modifiers.first;
  while (md && BKE_modifier_get_info(md->type)->type == eModifierTypeType_OnlyDeform) {
    md = md->next;
  }

  hmd = (HookModifierData *)BKE_modifier_new(eModifierType_Hook);
  BLI_insertlinkbefore(&obedit->modifiers, md, hmd);
  BLI_snprintf(hmd->modifier.name, sizeof(hmd->modifier.name), "Hook-%s", ob->id.name + 2);
  BKE_modifier_unique_name(&obedit->modifiers, (ModifierData *)hmd);

  hmd->object = ob;
  hmd->indexar = indexar;
  copy_v3_v3(hmd->cent, cent);
  hmd->indexar_num = indexar_num;
  BLI_strncpy(hmd->name, name, sizeof(hmd->name));

  unit_m4(pose_mat);

  invert_m4_m4(obedit->imat, obedit->obmat);
  if (mode == OBJECT_ADDHOOK_NEWOB) {
    /* pass */
  }
  else {
    /* may overwrite with pose-bone location, below */
    mul_v3_m4v3(cent, obedit->imat, ob->obmat[3]);
  }

  if (mode == OBJECT_ADDHOOK_SELOB_BONE) {
    bArmature *arm = ob->data;
    BLI_assert(ob->type == OB_ARMATURE);
    if (arm->act_bone) {
      bPoseChannel *pchan_act;

      BLI_strncpy(hmd->subtarget, arm->act_bone->name, sizeof(hmd->subtarget));

      pchan_act = BKE_pose_channel_active_if_layer_visible(ob);
      if (LIKELY(pchan_act)) {
        invert_m4_m4(pose_mat, pchan_act->pose_mat);
        mul_v3_m4v3(cent, ob->obmat, pchan_act->pose_mat[3]);
        mul_v3_m4v3(cent, obedit->imat, cent);
      }
    }
    else {
      BKE_report(reports, RPT_WARNING, "Armature has no active object bone");
    }
  }

  copy_v3_v3(hmd->cent, cent);

  /* matrix calculus */
  /* vert x (obmat x hook->imat) x hook->obmat x ob->imat */
  /*        (parentinv         )                          */
  Scene *scene_eval = DEG_get_evaluated_scene(depsgraph);
  Object *object_eval = DEG_get_evaluated_object(depsgraph, ob);
  BKE_object_transform_copy(object_eval, ob);
  BKE_object_where_is_calc(depsgraph, scene_eval, object_eval);

  invert_m4_m4(object_eval->imat, object_eval->obmat);
  /* apparently this call goes from right to left... */
  mul_m4_series(hmd->parentinv, pose_mat, object_eval->imat, obedit->obmat);

  DEG_relations_tag_update(bmain);

  return true;
}

static int object_add_hook_selob_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  Object *obedit = CTX_data_edit_object(C);
  Object *obsel = NULL;
  const bool use_bone = RNA_boolean_get(op->ptr, "use_bone");
  const int mode = use_bone ? OBJECT_ADDHOOK_SELOB_BONE : OBJECT_ADDHOOK_SELOB;

  CTX_DATA_BEGIN (C, Object *, ob, selected_objects) {
    if (ob != obedit) {
      obsel = ob;
      break;
    }
  }
  CTX_DATA_END;

  if (!obsel) {
    BKE_report(op->reports, RPT_ERROR, "Cannot add hook with no other selected objects");
    return OPERATOR_CANCELLED;
  }

  if (use_bone && obsel->type != OB_ARMATURE) {
    BKE_report(op->reports, RPT_ERROR, "Cannot add hook bone for a non armature object");
    return OPERATOR_CANCELLED;
  }

  if (add_hook_object(C, bmain, scene, view_layer, NULL, obedit, obsel, mode, op->reports)) {
    WM_event_add_notifier(C, NC_OBJECT | ND_MODIFIER, obedit);
    return OPERATOR_FINISHED;
  }
  return OPERATOR_CANCELLED;
}

void OBJECT_OT_hook_add_selob(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Hook to Selected Object";
  ot->description = "Hook selected vertices to the first selected object";
  ot->idname = "OBJECT_OT_hook_add_selob";

  /* api callbacks */
  ot->exec = object_add_hook_selob_exec;
  ot->poll = hook_op_edit_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_boolean(ot->srna,
                  "use_bone",
                  false,
                  "Active Bone",
                  "Assign the hook to the hook objects active bone");
}

static int object_add_hook_newob_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  View3D *v3d = CTX_wm_view3d(C);
  Object *obedit = CTX_data_edit_object(C);

  if (add_hook_object(
          C, bmain, scene, view_layer, v3d, obedit, NULL, OBJECT_ADDHOOK_NEWOB, op->reports)) {
    DEG_id_tag_update(&scene->id, ID_RECALC_SELECT);
    WM_event_add_notifier(C, NC_SCENE | ND_OB_SELECT, scene);
    WM_event_add_notifier(C, NC_OBJECT | ND_MODIFIER, obedit);
    return OPERATOR_FINISHED;
  }
  return OPERATOR_CANCELLED;
}

void OBJECT_OT_hook_add_newob(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Hook to New Object";
  ot->description = "Hook selected vertices to a newly created object";
  ot->idname = "OBJECT_OT_hook_add_newob";

  /* api callbacks */
  ot->exec = object_add_hook_newob_exec;
  ot->poll = hook_op_edit_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static int object_hook_remove_exec(bContext *C, wmOperator *op)
{
  int num = RNA_enum_get(op->ptr, "modifier");
  Object *ob = CTX_data_edit_object(C);
  HookModifierData *hmd = NULL;

  hmd = (HookModifierData *)BLI_findlink(&ob->modifiers, num);
  if (!hmd) {
    BKE_report(op->reports, RPT_ERROR, "Could not find hook modifier");
    return OPERATOR_CANCELLED;
  }

  /* remove functionality */

  BKE_modifier_remove_from_list(ob, (ModifierData *)hmd);
  BKE_modifier_free((ModifierData *)hmd);

  DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_OBJECT | ND_MODIFIER, ob);

  return OPERATOR_FINISHED;
}

static const EnumPropertyItem *hook_mod_itemf(bContext *C,
                                              PointerRNA *UNUSED(ptr),
                                              PropertyRNA *UNUSED(prop),
                                              bool *r_free)
{
  Object *ob = CTX_data_edit_object(C);
  EnumPropertyItem tmp = {0, "", 0, "", ""};
  EnumPropertyItem *item = NULL;
  ModifierData *md = NULL;
  int a, totitem = 0;

  if (!ob) {
    return DummyRNA_NULL_items;
  }

  for (a = 0, md = ob->modifiers.first; md; md = md->next, a++) {
    if (md->type == eModifierType_Hook) {
      tmp.value = a;
      tmp.icon = ICON_HOOK;
      tmp.identifier = md->name;
      tmp.name = md->name;
      RNA_enum_item_add(&item, &totitem, &tmp);
    }
  }

  RNA_enum_item_end(&item, &totitem);
  *r_free = true;

  return item;
}

void OBJECT_OT_hook_remove(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* identifiers */
  ot->name = "Remove Hook";
  ot->idname = "OBJECT_OT_hook_remove";
  ot->description = "Remove a hook from the active object";

  /* api callbacks */
  ot->exec = object_hook_remove_exec;
  ot->invoke = WM_menu_invoke;
  ot->poll = hook_op_edit_poll;

  /* flags */
  /* this operator removes modifier which isn't stored in local undo stack,
   * so redoing it from redo panel gives totally weird results. */
  ot->flag = /*OPTYPE_REGISTER|*/ OPTYPE_UNDO;

  /* properties */
  prop = RNA_def_enum(
      ot->srna, "modifier", DummyRNA_NULL_items, 0, "Modifier", "Modifier number to remove");
  RNA_def_enum_funcs(prop, hook_mod_itemf);
  RNA_def_property_flag(prop, PROP_ENUM_NO_TRANSLATE);
  ot->prop = prop;
}

static int object_hook_reset_exec(bContext *C, wmOperator *op)
{
  PointerRNA ptr = CTX_data_pointer_get_type(C, "modifier", &RNA_HookModifier);
  int num = RNA_enum_get(op->ptr, "modifier");
  Object *ob = NULL;
  HookModifierData *hmd = NULL;

  object_hook_from_context(C, &ptr, num, &ob, &hmd);
  if (hmd == NULL) {
    BKE_report(op->reports, RPT_ERROR, "Could not find hook modifier");
    return OPERATOR_CANCELLED;
  }

  BKE_object_modifier_hook_reset(ob, hmd);

  DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_OBJECT | ND_MODIFIER, ob);

  return OPERATOR_FINISHED;
}

void OBJECT_OT_hook_reset(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* identifiers */
  ot->name = "Reset Hook";
  ot->description = "Recalculate and clear offset transformation";
  ot->idname = "OBJECT_OT_hook_reset";

  /* callbacks */
  ot->exec = object_hook_reset_exec;
  ot->poll = hook_op_edit_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  prop = RNA_def_enum(
      ot->srna, "modifier", DummyRNA_NULL_items, 0, "Modifier", "Modifier number to assign to");
  RNA_def_enum_funcs(prop, hook_mod_itemf);
  RNA_def_property_flag(prop, PROP_ENUM_NO_TRANSLATE);
}

static int object_hook_recenter_exec(bContext *C, wmOperator *op)
{
  PointerRNA ptr = CTX_data_pointer_get_type(C, "modifier", &RNA_HookModifier);
  int num = RNA_enum_get(op->ptr, "modifier");
  Object *ob = NULL;
  HookModifierData *hmd = NULL;
  Scene *scene = CTX_data_scene(C);
  float bmat[3][3], imat[3][3];

  object_hook_from_context(C, &ptr, num, &ob, &hmd);
  if (hmd == NULL) {
    BKE_report(op->reports, RPT_ERROR, "Could not find hook modifier");
    return OPERATOR_CANCELLED;
  }

  /* recenter functionality */
  copy_m3_m4(bmat, ob->obmat);
  invert_m3_m3(imat, bmat);

  sub_v3_v3v3(hmd->cent, scene->cursor.location, ob->obmat[3]);
  mul_m3_v3(imat, hmd->cent);

  DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_OBJECT | ND_MODIFIER, ob);

  return OPERATOR_FINISHED;
}

void OBJECT_OT_hook_recenter(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* identifiers */
  ot->name = "Recenter Hook";
  ot->description = "Set hook center to cursor position";
  ot->idname = "OBJECT_OT_hook_recenter";

  /* callbacks */
  ot->exec = object_hook_recenter_exec;
  ot->poll = hook_op_edit_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  prop = RNA_def_enum(
      ot->srna, "modifier", DummyRNA_NULL_items, 0, "Modifier", "Modifier number to assign to");
  RNA_def_enum_funcs(prop, hook_mod_itemf);
  RNA_def_property_flag(prop, PROP_ENUM_NO_TRANSLATE);
}

static int object_hook_assign_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  PointerRNA ptr = CTX_data_pointer_get_type(C, "modifier", &RNA_HookModifier);
  int num = RNA_enum_get(op->ptr, "modifier");
  Object *ob = NULL;
  HookModifierData *hmd = NULL;
  float cent[3];
  char name[MAX_NAME];
  int *indexar, indexar_num;

  object_hook_from_context(C, &ptr, num, &ob, &hmd);
  if (hmd == NULL) {
    BKE_report(op->reports, RPT_ERROR, "Could not find hook modifier");
    return OPERATOR_CANCELLED;
  }

  /* assign functionality */

  if (!object_hook_index_array(bmain, scene, ob, &indexar, &indexar_num, name, cent)) {
    BKE_report(op->reports, RPT_WARNING, "Requires selected vertices or active vertex group");
    return OPERATOR_CANCELLED;
  }
  if (hmd->indexar) {
    MEM_freeN(hmd->indexar);
  }

  copy_v3_v3(hmd->cent, cent);
  hmd->indexar = indexar;
  hmd->indexar_num = indexar_num;

  DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_OBJECT | ND_MODIFIER, ob);

  return OPERATOR_FINISHED;
}

void OBJECT_OT_hook_assign(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* identifiers */
  ot->name = "Assign to Hook";
  ot->description = "Assign the selected vertices to a hook";
  ot->idname = "OBJECT_OT_hook_assign";

  /* callbacks */
  ot->exec = object_hook_assign_exec;
  ot->poll = hook_op_edit_poll;

  /* flags */
  /* this operator changes data stored in modifier which doesn't get pushed to undo stack,
   * so redoing it from redo panel gives totally weird results. */
  ot->flag = /*OPTYPE_REGISTER|*/ OPTYPE_UNDO;

  /* properties */
  prop = RNA_def_enum(
      ot->srna, "modifier", DummyRNA_NULL_items, 0, "Modifier", "Modifier number to assign to");
  RNA_def_enum_funcs(prop, hook_mod_itemf);
  RNA_def_property_flag(prop, PROP_ENUM_NO_TRANSLATE);
}

static int object_hook_select_exec(bContext *C, wmOperator *op)
{
  PointerRNA ptr = CTX_data_pointer_get_type(C, "modifier", &RNA_HookModifier);
  int num = RNA_enum_get(op->ptr, "modifier");
  Object *ob = NULL;
  HookModifierData *hmd = NULL;

  object_hook_from_context(C, &ptr, num, &ob, &hmd);
  if (hmd == NULL) {
    BKE_report(op->reports, RPT_ERROR, "Could not find hook modifier");
    return OPERATOR_CANCELLED;
  }

  /* select functionality */
  object_hook_select(ob, hmd);

  DEG_id_tag_update(ob->data, ID_RECALC_SELECT);
  WM_event_add_notifier(C, NC_GEOM | ND_SELECT, ob->data);

  return OPERATOR_FINISHED;
}

void OBJECT_OT_hook_select(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* identifiers */
  ot->name = "Select Hook";
  ot->description = "Select affected vertices on mesh";
  ot->idname = "OBJECT_OT_hook_select";

  /* callbacks */
  ot->exec = object_hook_select_exec;
  ot->poll = hook_op_edit_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  prop = RNA_def_enum(
      ot->srna, "modifier", DummyRNA_NULL_items, 0, "Modifier", "Modifier number to remove");
  RNA_def_enum_funcs(prop, hook_mod_itemf);
  RNA_def_property_flag(prop, PROP_ENUM_NO_TRANSLATE);
}
