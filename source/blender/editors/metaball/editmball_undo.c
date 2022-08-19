/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edmeta
 */

#include <math.h>
#include <string.h>

#include "MEM_guardedalloc.h"

#include "CLG_log.h"

#include "BLI_array_utils.h"
#include "BLI_listbase.h"
#include "BLI_utildefines.h"

#include "DNA_defs.h"
#include "DNA_layer_types.h"
#include "DNA_meta_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BKE_context.h"
#include "BKE_layer.h"
#include "BKE_main.h"
#include "BKE_object.h"
#include "BKE_undo_system.h"

#include "DEG_depsgraph.h"

#include "ED_mball.h"
#include "ED_object.h"
#include "ED_undo.h"
#include "ED_util.h"

#include "WM_api.h"
#include "WM_types.h"

/** We only need this locally. */
static CLG_LogRef LOG = {"ed.undo.mball"};

/* -------------------------------------------------------------------- */
/** \name Undo Conversion
 * \{ */

typedef struct UndoMBall {
  ListBase editelems;
  int lastelem_index;
  size_t undo_size;
} UndoMBall;

/* free all MetaElems from ListBase */
static void freeMetaElemlist(ListBase *lb)
{
  MetaElem *ml;

  if (lb == NULL) {
    return;
  }

  while ((ml = BLI_pophead(lb))) {
    MEM_freeN(ml);
  }
}

static void undomball_to_editmball(UndoMBall *umb, MetaBall *mb)
{
  freeMetaElemlist(mb->editelems);
  mb->lastelem = NULL;

  /* copy 'undo' MetaElems to 'edit' MetaElems */
  int index = 0;
  for (MetaElem *ml_undo = umb->editelems.first; ml_undo; ml_undo = ml_undo->next, index += 1) {
    MetaElem *ml_edit = MEM_dupallocN(ml_undo);
    BLI_addtail(mb->editelems, ml_edit);
    if (index == umb->lastelem_index) {
      mb->lastelem = ml_edit;
    }
  }
}

static void *editmball_from_undomball(UndoMBall *umb, MetaBall *mb)
{
  BLI_assert(BLI_array_is_zeroed(umb, 1));

  /* allocate memory for undo ListBase */
  umb->lastelem_index = -1;

  /* copy contents of current ListBase to the undo ListBase */
  int index = 0;
  for (MetaElem *ml_edit = mb->editelems->first; ml_edit; ml_edit = ml_edit->next, index += 1) {
    MetaElem *ml_undo = MEM_dupallocN(ml_edit);
    BLI_addtail(&umb->editelems, ml_undo);
    if (ml_edit == mb->lastelem) {
      umb->lastelem_index = index;
    }
    umb->undo_size += sizeof(MetaElem);
  }

  return umb;
}

/* free undo ListBase of MetaElems */
static void undomball_free_data(UndoMBall *umb)
{
  freeMetaElemlist(&umb->editelems);
}

static Object *editmball_object_from_context(bContext *C)
{
  ViewLayer *view_layer = CTX_data_view_layer(C);
  Object *obedit = OBEDIT_FROM_VIEW_LAYER(view_layer);
  if (obedit && obedit->type == OB_MBALL) {
    MetaBall *mb = obedit->data;
    if (mb->editelems != NULL) {
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

typedef struct MBallUndoStep_Elem {
  UndoRefID_Object obedit_ref;
  UndoMBall data;
} MBallUndoStep_Elem;

typedef struct MBallUndoStep {
  UndoStep step;
  MBallUndoStep_Elem *elems;
  uint elems_len;
} MBallUndoStep;

static bool mball_undosys_poll(bContext *C)
{
  return editmball_object_from_context(C) != NULL;
}

static bool mball_undosys_step_encode(struct bContext *C, struct Main *bmain, UndoStep *us_p)
{
  MBallUndoStep *us = (MBallUndoStep *)us_p;

  /* Important not to use the 3D view when getting objects because all objects
   * outside of this list will be moved out of edit-mode when reading back undo steps. */
  ViewLayer *view_layer = CTX_data_view_layer(C);
  uint objects_len = 0;
  Object **objects = ED_undo_editmode_objects_from_view_layer(view_layer, &objects_len);

  us->elems = MEM_callocN(sizeof(*us->elems) * objects_len, __func__);
  us->elems_len = objects_len;

  for (uint i = 0; i < objects_len; i++) {
    Object *ob = objects[i];
    MBallUndoStep_Elem *elem = &us->elems[i];

    elem->obedit_ref.ptr = ob;
    MetaBall *mb = ob->data;
    editmball_from_undomball(&elem->data, mb);
    mb->needs_flush_to_id = 1;
    us->step.data_size += elem->data.undo_size;
  }
  MEM_freeN(objects);

  bmain->is_memfile_undo_flush_needed = true;

  return true;
}

static void mball_undosys_step_decode(struct bContext *C,
                                      struct Main *bmain,
                                      UndoStep *us_p,
                                      const eUndoStepDir UNUSED(dir),
                                      bool UNUSED(is_final))
{
  MBallUndoStep *us = (MBallUndoStep *)us_p;

  ED_undo_object_editmode_restore_helper(
      C, &us->elems[0].obedit_ref.ptr, us->elems_len, sizeof(*us->elems));

  BLI_assert(BKE_object_is_in_editmode(us->elems[0].obedit_ref.ptr));

  for (uint i = 0; i < us->elems_len; i++) {
    MBallUndoStep_Elem *elem = &us->elems[i];
    Object *obedit = elem->obedit_ref.ptr;
    MetaBall *mb = obedit->data;
    if (mb->editelems == NULL) {
      /* Should never fail, may not crash but can give odd behavior. */
      CLOG_ERROR(&LOG,
                 "name='%s', failed to enter edit-mode for object '%s', undo state invalid",
                 us_p->name,
                 obedit->id.name);
      continue;
    }
    undomball_to_editmball(&elem->data, mb);
    mb->needs_flush_to_id = 1;
    DEG_id_tag_update(&mb->id, ID_RECALC_GEOMETRY);
  }

  /* The first element is always active */
  ED_undo_object_set_active_or_warn(
      CTX_data_scene(C), CTX_data_view_layer(C), us->elems[0].obedit_ref.ptr, us_p->name, &LOG);

  /* Check after setting active. */
  BLI_assert(mball_undosys_poll(C));

  bmain->is_memfile_undo_flush_needed = true;

  WM_event_add_notifier(C, NC_GEOM | ND_DATA, NULL);
}

static void mball_undosys_step_free(UndoStep *us_p)
{
  MBallUndoStep *us = (MBallUndoStep *)us_p;

  for (uint i = 0; i < us->elems_len; i++) {
    MBallUndoStep_Elem *elem = &us->elems[i];
    undomball_free_data(&elem->data);
  }
  MEM_freeN(us->elems);
}

static void mball_undosys_foreach_ID_ref(UndoStep *us_p,
                                         UndoTypeForEachIDRefFn foreach_ID_ref_fn,
                                         void *user_data)
{
  MBallUndoStep *us = (MBallUndoStep *)us_p;

  for (uint i = 0; i < us->elems_len; i++) {
    MBallUndoStep_Elem *elem = &us->elems[i];
    foreach_ID_ref_fn(user_data, ((UndoRefID *)&elem->obedit_ref));
  }
}

void ED_mball_undosys_type(UndoType *ut)
{
  ut->name = "Edit MBall";
  ut->poll = mball_undosys_poll;
  ut->step_encode = mball_undosys_step_encode;
  ut->step_decode = mball_undosys_step_decode;
  ut->step_free = mball_undosys_step_free;

  ut->step_foreach_ID_ref = mball_undosys_foreach_ID_ref;

  ut->flags = UNDOTYPE_FLAG_NEED_CONTEXT_FOR_ENCODE;

  ut->step_size = sizeof(MBallUndoStep);
}

/** \} */
