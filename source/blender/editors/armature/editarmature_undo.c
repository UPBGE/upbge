/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2001-2002 NaN Holding BV. All rights reserved. */

/** \file
 * \ingroup edarmature
 */

#include "MEM_guardedalloc.h"

#include "CLG_log.h"

#include "DNA_armature_types.h"
#include "DNA_layer_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BLI_array_utils.h"
#include "BLI_listbase.h"

#include "BKE_armature.h"
#include "BKE_context.h"
#include "BKE_layer.h"
#include "BKE_main.h"
#include "BKE_object.h"
#include "BKE_undo_system.h"

#include "DEG_depsgraph.h"

#include "ED_armature.h"
#include "ED_object.h"
#include "ED_undo.h"
#include "ED_util.h"

#include "WM_api.h"
#include "WM_types.h"

/** We only need this locally. */
static CLG_LogRef LOG = {"ed.undo.armature"};

/* -------------------------------------------------------------------- */
/** \name Undo Conversion
 * \{ */

typedef struct UndoArmature {
  EditBone *act_edbone;
  ListBase lb;
  size_t undo_size;
} UndoArmature;

static void undoarm_to_editarm(UndoArmature *uarm, bArmature *arm)
{
  EditBone *ebone;

  ED_armature_ebone_listbase_free(arm->edbo, true);
  ED_armature_ebone_listbase_copy(arm->edbo, &uarm->lb, true);

  /* active bone */
  if (uarm->act_edbone) {
    ebone = uarm->act_edbone;
    arm->act_edbone = ebone->temp.ebone;
  }
  else {
    arm->act_edbone = NULL;
  }

  ED_armature_ebone_listbase_temp_clear(arm->edbo);
}

static void *undoarm_from_editarm(UndoArmature *uarm, bArmature *arm)
{
  BLI_assert(BLI_array_is_zeroed(uarm, 1));

  /* TODO: include size of ID-properties. */
  uarm->undo_size = 0;

  ED_armature_ebone_listbase_copy(&uarm->lb, arm->edbo, false);

  /* active bone */
  if (arm->act_edbone) {
    EditBone *ebone = arm->act_edbone;
    uarm->act_edbone = ebone->temp.ebone;
  }

  ED_armature_ebone_listbase_temp_clear(&uarm->lb);

  LISTBASE_FOREACH (EditBone *, ebone, &uarm->lb) {
    uarm->undo_size += sizeof(EditBone);
  }

  return uarm;
}

static void undoarm_free_data(UndoArmature *uarm)
{
  ED_armature_ebone_listbase_free(&uarm->lb, false);
}

static Object *editarm_object_from_context(bContext *C)
{
  ViewLayer *view_layer = CTX_data_view_layer(C);
  Object *obedit = OBEDIT_FROM_VIEW_LAYER(view_layer);
  if (obedit && obedit->type == OB_ARMATURE) {
    bArmature *arm = obedit->data;
    if (arm->edbo != NULL) {
      return obedit;
    }
  }
  return NULL;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Implements ED Undo System
 *
 * \note This is similar for all edit-mode types.
 * \{ */

typedef struct ArmatureUndoStep_Elem {
  struct ArmatureUndoStep_Elem *next, *prev;
  UndoRefID_Object obedit_ref;
  UndoArmature data;
} ArmatureUndoStep_Elem;

typedef struct ArmatureUndoStep {
  UndoStep step;
  ArmatureUndoStep_Elem *elems;
  uint elems_len;
} ArmatureUndoStep;

static bool armature_undosys_poll(bContext *C)
{
  return editarm_object_from_context(C) != NULL;
}

static bool armature_undosys_step_encode(struct bContext *C, struct Main *bmain, UndoStep *us_p)
{
  ArmatureUndoStep *us = (ArmatureUndoStep *)us_p;

  /* Important not to use the 3D view when getting objects because all objects
   * outside of this list will be moved out of edit-mode when reading back undo steps. */
  ViewLayer *view_layer = CTX_data_view_layer(C);
  uint objects_len = 0;
  Object **objects = ED_undo_editmode_objects_from_view_layer(view_layer, &objects_len);

  us->elems = MEM_callocN(sizeof(*us->elems) * objects_len, __func__);
  us->elems_len = objects_len;

  for (uint i = 0; i < objects_len; i++) {
    Object *ob = objects[i];
    ArmatureUndoStep_Elem *elem = &us->elems[i];

    elem->obedit_ref.ptr = ob;
    bArmature *arm = elem->obedit_ref.ptr->data;
    undoarm_from_editarm(&elem->data, arm);
    arm->needs_flush_to_id = 1;
    us->step.data_size += elem->data.undo_size;
  }
  MEM_freeN(objects);

  bmain->is_memfile_undo_flush_needed = true;

  return true;
}

static void armature_undosys_step_decode(struct bContext *C,
                                         struct Main *bmain,
                                         UndoStep *us_p,
                                         const eUndoStepDir UNUSED(dir),
                                         bool UNUSED(is_final))
{
  ArmatureUndoStep *us = (ArmatureUndoStep *)us_p;

  ED_undo_object_editmode_restore_helper(
      C, &us->elems[0].obedit_ref.ptr, us->elems_len, sizeof(*us->elems));

  BLI_assert(BKE_object_is_in_editmode(us->elems[0].obedit_ref.ptr));

  for (uint i = 0; i < us->elems_len; i++) {
    ArmatureUndoStep_Elem *elem = &us->elems[i];
    Object *obedit = elem->obedit_ref.ptr;
    bArmature *arm = obedit->data;
    if (arm->edbo == NULL) {
      /* Should never fail, may not crash but can give odd behavior. */
      CLOG_ERROR(&LOG,
                 "name='%s', failed to enter edit-mode for object '%s', undo state invalid",
                 us_p->name,
                 obedit->id.name);
      continue;
    }
    undoarm_to_editarm(&elem->data, arm);
    arm->needs_flush_to_id = 1;
    DEG_id_tag_update(&arm->id, ID_RECALC_GEOMETRY);
  }

  /* The first element is always active */
  ED_undo_object_set_active_or_warn(
      CTX_data_scene(C), CTX_data_view_layer(C), us->elems[0].obedit_ref.ptr, us_p->name, &LOG);

  /* Check after setting active. */
  BLI_assert(armature_undosys_poll(C));

  bmain->is_memfile_undo_flush_needed = true;

  WM_event_add_notifier(C, NC_GEOM | ND_DATA, NULL);
}

static void armature_undosys_step_free(UndoStep *us_p)
{
  ArmatureUndoStep *us = (ArmatureUndoStep *)us_p;

  for (uint i = 0; i < us->elems_len; i++) {
    ArmatureUndoStep_Elem *elem = &us->elems[i];
    undoarm_free_data(&elem->data);
  }
  MEM_freeN(us->elems);
}

static void armature_undosys_foreach_ID_ref(UndoStep *us_p,
                                            UndoTypeForEachIDRefFn foreach_ID_ref_fn,
                                            void *user_data)
{
  ArmatureUndoStep *us = (ArmatureUndoStep *)us_p;

  for (uint i = 0; i < us->elems_len; i++) {
    ArmatureUndoStep_Elem *elem = &us->elems[i];
    foreach_ID_ref_fn(user_data, ((UndoRefID *)&elem->obedit_ref));
  }
}

void ED_armature_undosys_type(UndoType *ut)
{
  ut->name = "Edit Armature";
  ut->poll = armature_undosys_poll;
  ut->step_encode = armature_undosys_step_encode;
  ut->step_decode = armature_undosys_step_decode;
  ut->step_free = armature_undosys_step_free;

  ut->step_foreach_ID_ref = armature_undosys_foreach_ID_ref;

  ut->flags = UNDOTYPE_FLAG_NEED_CONTEXT_FOR_ENCODE;

  ut->step_size = sizeof(ArmatureUndoStep);
}

/** \} */
