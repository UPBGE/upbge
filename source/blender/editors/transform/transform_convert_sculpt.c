/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2001-2002 NaN Holding BV. All rights reserved. */

/** \file
 * \ingroup edtransform
 */

#include "MEM_guardedalloc.h"

#include "BLI_math.h"

#include "BKE_context.h"
#include "BKE_lib_id.h"
#include "BKE_paint.h"
#include "BKE_report.h"

#include "ED_sculpt.h"

#include "transform.h"
#include "transform_convert.h"

/* -------------------------------------------------------------------- */
/** \name Sculpt Transform Creation
 * \{ */

static void createTransSculpt(bContext *C, TransInfo *t)
{
  TransData *td;

  Scene *scene = t->scene;
  if (!BKE_id_is_editable(CTX_data_main(C), &scene->id)) {
    BKE_report(t->reports, RPT_ERROR, "Linked data can't text-space transform");
    return;
  }

  Object *ob = OBACT(t->view_layer);
  SculptSession *ss = ob->sculpt;

  {
    BLI_assert(t->data_container_len == 1);
    TransDataContainer *tc = t->data_container;
    tc->data_len = 1;
    tc->is_active = 1;
    td = tc->data = MEM_callocN(sizeof(TransData), "TransSculpt");
    td->ext = tc->data_ext = MEM_callocN(sizeof(TransDataExtension), "TransSculpt");
  }

  td->flag = TD_SELECTED;
  copy_v3_v3(td->center, ss->pivot_pos);
  mul_m4_v3(ob->obmat, td->center);
  td->ob = ob;

  td->loc = ss->pivot_pos;
  copy_v3_v3(td->iloc, ss->pivot_pos);

  if (is_zero_v4(ss->pivot_rot)) {
    ss->pivot_rot[3] = 1.0f;
  }

  float obmat_inv[3][3];
  copy_m3_m4(obmat_inv, ob->obmat);
  invert_m3(obmat_inv);

  td->ext->rot = NULL;
  td->ext->rotAxis = NULL;
  td->ext->rotAngle = NULL;
  td->ext->quat = ss->pivot_rot;
  copy_m4_m4(td->ext->obmat, ob->obmat);
  copy_m3_m3(td->ext->l_smtx, obmat_inv);
  copy_m3_m4(td->ext->r_mtx, ob->obmat);
  copy_m3_m3(td->ext->r_smtx, obmat_inv);

  copy_qt_qt(td->ext->iquat, ss->pivot_rot);
  td->ext->rotOrder = ROT_MODE_QUAT;

  ss->pivot_scale[0] = 1.0f;
  ss->pivot_scale[1] = 1.0f;
  ss->pivot_scale[2] = 1.0f;
  td->ext->size = ss->pivot_scale;
  copy_v3_v3(ss->init_pivot_scale, ss->pivot_scale);
  copy_v3_v3(td->ext->isize, ss->init_pivot_scale);

  copy_m3_m3(td->smtx, obmat_inv);
  copy_m3_m4(td->mtx, ob->obmat);
  copy_m3_m4(td->axismtx, ob->obmat);

  BLI_assert(!(t->options & CTX_PAINT_CURVE));
  ED_sculpt_init_transform(C, ob, t->undo_name);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Recalc Data object
 * \{ */

static void recalcData_sculpt(TransInfo *t)
{
  Object *ob = OBACT(t->view_layer);
  ED_sculpt_update_modal_transform(t->context, ob);
}

static void special_aftertrans_update__sculpt(bContext *C, TransInfo *t)
{
  Scene *scene = t->scene;
  if (!BKE_id_is_editable(CTX_data_main(C), &scene->id)) {
    /* `ED_sculpt_init_transform` was not called in this case. */
    return;
  }

  Object *ob = OBACT(t->view_layer);
  BLI_assert(!(t->options & CTX_PAINT_CURVE));
  ED_sculpt_end_transform(C, ob);
}

/** \} */

TransConvertTypeInfo TransConvertType_Sculpt = {
    /* flags */ 0,
    /* createTransData */ createTransSculpt,
    /* recalcData */ recalcData_sculpt,
    /* special_aftertrans_update */ special_aftertrans_update__sculpt,
};
