/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2001-2002 NaN Holding BV. All rights reserved. */

/** \file
 * \ingroup edtransform
 */

#include "DNA_meta_types.h"

#include "MEM_guardedalloc.h"

#include "BLI_math.h"

#include "BKE_context.h"

#include "transform.h"
#include "transform_snap.h"

#include "transform_convert.h"

/* -------------------------------------------------------------------- */
/** \name Meta Elements Transform Creation
 * \{ */

static void createTransMBallVerts(bContext *UNUSED(C), TransInfo *t)
{
  FOREACH_TRANS_DATA_CONTAINER (t, tc) {
    MetaBall *mb = (MetaBall *)tc->obedit->data;
    MetaElem *ml;
    TransData *td;
    TransDataExtension *tx;
    float mtx[3][3], smtx[3][3];
    int count = 0, countsel = 0;
    const bool is_prop_edit = (t->flag & T_PROP_EDIT) != 0;
    const bool is_prop_connected = (t->flag & T_PROP_CONNECTED) != 0;

    /* count totals */
    for (ml = mb->editelems->first; ml; ml = ml->next) {
      if (ml->flag & SELECT) {
        countsel++;
      }
      if (is_prop_edit) {
        count++;
      }
    }

    /* Support other objects using PET to adjust these, unless connected is enabled. */
    if (((is_prop_edit && !is_prop_connected) ? count : countsel) == 0) {
      tc->data_len = 0;
      continue;
    }

    if (is_prop_edit) {
      tc->data_len = count;
    }
    else {
      tc->data_len = countsel;
    }

    td = tc->data = MEM_callocN(tc->data_len * sizeof(TransData), "TransObData(MBall EditMode)");
    tx = tc->data_ext = MEM_callocN(tc->data_len * sizeof(TransDataExtension),
                                    "MetaElement_TransExtension");

    copy_m3_m4(mtx, tc->obedit->obmat);
    pseudoinverse_m3_m3(smtx, mtx, PSEUDOINVERSE_EPSILON);

    for (ml = mb->editelems->first; ml; ml = ml->next) {
      if (is_prop_edit || (ml->flag & SELECT)) {
        td->loc = &ml->x;
        copy_v3_v3(td->iloc, td->loc);
        copy_v3_v3(td->center, td->loc);

        quat_to_mat3(td->axismtx, ml->quat);

        if (ml->flag & SELECT) {
          td->flag = TD_SELECTED | TD_USEQUAT | TD_SINGLESIZE;
        }
        else {
          td->flag = TD_USEQUAT;
        }

        copy_m3_m3(td->smtx, smtx);
        copy_m3_m3(td->mtx, mtx);

        td->ext = tx;

        /* Radius of MetaElem (mass of MetaElem influence) */
        if (ml->flag & MB_SCALE_RAD) {
          td->val = &ml->rad;
          td->ival = ml->rad;
        }
        else {
          td->val = &ml->s;
          td->ival = ml->s;
        }

        /* expx/expy/expz determine "shape" of some MetaElem types */
        tx->size = &ml->expx;
        tx->isize[0] = ml->expx;
        tx->isize[1] = ml->expy;
        tx->isize[2] = ml->expz;

        /* quat is used for rotation of MetaElem */
        tx->quat = ml->quat;
        copy_qt_qt(tx->iquat, ml->quat);

        tx->rot = NULL;

        td++;
        tx++;
      }
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Recalc Meta Ball
 * \{ */

static void recalcData_mball(TransInfo *t)
{
  if (t->state != TRANS_CANCEL) {
    applySnappingIndividual(t);
  }
  FOREACH_TRANS_DATA_CONTAINER (t, tc) {
    if (tc->data_len) {
      DEG_id_tag_update(tc->obedit->data, ID_RECALC_GEOMETRY);
    }
  }
}

/** \} */

TransConvertTypeInfo TransConvertType_MBall = {
    /* flags */ (T_EDIT | T_POINTS),
    /* createTransData */ createTransMBallVerts,
    /* recalcData */ recalcData_mball,
    /* special_aftertrans_update */ NULL,
};
