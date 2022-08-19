/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include <string.h>

#include "MEM_guardedalloc.h"

#include "DNA_brush_types.h"
#include "DNA_space_types.h"

#include "BLI_array_utils.h"

#include "BKE_context.h"
#include "BKE_paint.h"
#include "BKE_undo_system.h"

#include "ED_paint.h"
#include "ED_undo.h"

#include "WM_api.h"

#include "paint_intern.h"

/* -------------------------------------------------------------------- */
/** \name Undo Conversion
 * \{ */

typedef struct UndoCurve {
  PaintCurvePoint *points; /* points of curve */
  int tot_points;
  int add_index;
} UndoCurve;

static void undocurve_from_paintcurve(UndoCurve *uc, const PaintCurve *pc)
{
  BLI_assert(BLI_array_is_zeroed(uc, 1));
  uc->points = MEM_dupallocN(pc->points);
  uc->tot_points = pc->tot_points;
  uc->add_index = pc->add_index;
}

static void undocurve_to_paintcurve(const UndoCurve *uc, PaintCurve *pc)
{
  MEM_SAFE_FREE(pc->points);
  pc->points = MEM_dupallocN(uc->points);
  pc->tot_points = uc->tot_points;
  pc->add_index = uc->add_index;
}

static void undocurve_free_data(UndoCurve *uc)
{
  MEM_SAFE_FREE(uc->points);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Implements ED Undo System
 * \{ */

typedef struct PaintCurveUndoStep {
  UndoStep step;

  UndoRefID_PaintCurve pc_ref;

  UndoCurve data;
} PaintCurveUndoStep;

static bool paintcurve_undosys_poll(bContext *C)
{
  if (C == NULL || !paint_curve_poll(C)) {
    return false;
  }
  Paint *p = BKE_paint_get_active_from_context(C);
  return (p->brush && p->brush->paint_curve);
}

static void paintcurve_undosys_step_encode_init(struct bContext *C, UndoStep *us_p)
{
  /* XXX, use to set the undo type only. */
  UNUSED_VARS(C, us_p);
}

static bool paintcurve_undosys_step_encode(struct bContext *C,
                                           struct Main *UNUSED(bmain),
                                           UndoStep *us_p)
{
  /* FIXME Double check this, it should not be needed here at all? undo system is supposed to
   * ensure that. */
  if (!paint_curve_poll(C)) {
    return false;
  }

  Paint *p = BKE_paint_get_active_from_context(C);
  PaintCurve *pc = p ? (p->brush ? p->brush->paint_curve : NULL) : NULL;
  if (pc == NULL) {
    return false;
  }

  PaintCurveUndoStep *us = (PaintCurveUndoStep *)us_p;
  BLI_assert(us->step.data_size == 0);

  us->pc_ref.ptr = pc;
  undocurve_from_paintcurve(&us->data, pc);

  return true;
}

static void paintcurve_undosys_step_decode(struct bContext *UNUSED(C),
                                           struct Main *UNUSED(bmain),
                                           UndoStep *us_p,
                                           const eUndoStepDir UNUSED(dir),
                                           bool UNUSED(is_final))
{
  PaintCurveUndoStep *us = (PaintCurveUndoStep *)us_p;
  undocurve_to_paintcurve(&us->data, us->pc_ref.ptr);
}

static void paintcurve_undosys_step_free(UndoStep *us_p)
{
  PaintCurveUndoStep *us = (PaintCurveUndoStep *)us_p;
  undocurve_free_data(&us->data);
}

static void paintcurve_undosys_foreach_ID_ref(UndoStep *us_p,
                                              UndoTypeForEachIDRefFn foreach_ID_ref_fn,
                                              void *user_data)
{
  PaintCurveUndoStep *us = (PaintCurveUndoStep *)us_p;
  foreach_ID_ref_fn(user_data, ((UndoRefID *)&us->pc_ref));
}

void ED_paintcurve_undosys_type(UndoType *ut)
{
  ut->name = "Paint Curve";
  ut->poll = paintcurve_undosys_poll;
  ut->step_encode_init = paintcurve_undosys_step_encode_init;
  ut->step_encode = paintcurve_undosys_step_encode;
  ut->step_decode = paintcurve_undosys_step_decode;
  ut->step_free = paintcurve_undosys_step_free;

  ut->step_foreach_ID_ref = paintcurve_undosys_foreach_ID_ref;

  ut->flags = 0;

  ut->step_size = sizeof(PaintCurveUndoStep);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Utilities
 * \{ */

void ED_paintcurve_undo_push_begin(const char *name)
{
  UndoStack *ustack = ED_undo_stack_get();
  bContext *C = NULL; /* special case, we never read from this. */
  BKE_undosys_step_push_init_with_type(ustack, C, name, BKE_UNDOSYS_TYPE_PAINTCURVE);
}

void ED_paintcurve_undo_push_end(bContext *C)
{
  UndoStack *ustack = ED_undo_stack_get();
  BKE_undosys_step_push(ustack, C, NULL);
  BKE_undosys_stack_limit_steps_and_memory_defaults(ustack);
  WM_file_tag_modified();
}

/** \} */
