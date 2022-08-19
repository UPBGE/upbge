/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2012 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup edmask
 */

#include <stdlib.h>

#include "BLI_listbase.h"
#include "BLI_math.h"
#include "BLI_utildefines.h"

#include "BKE_context.h"
#include "BKE_mask.h"

#include "DNA_mask_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "DEG_depsgraph.h"

#include "RNA_access.h"
#include "RNA_define.h"

#include "WM_api.h"
#include "WM_types.h"

#include "ED_mask.h" /* own include */

#include "mask_intern.h" /* own include */

static int mask_shape_key_insert_exec(bContext *C, wmOperator *UNUSED(op))
{
  Scene *scene = CTX_data_scene(C);
  const int frame = scene->r.cfra;
  Mask *mask = CTX_data_edit_mask(C);
  bool changed = false;

  LISTBASE_FOREACH (MaskLayer *, mask_layer, &mask->masklayers) {
    MaskLayerShape *mask_layer_shape;

    if (!ED_mask_layer_select_check(mask_layer)) {
      continue;
    }

    mask_layer_shape = BKE_mask_layer_shape_verify_frame(mask_layer, frame);
    BKE_mask_layer_shape_from_mask(mask_layer, mask_layer_shape);
    changed = true;
  }

  if (changed) {
    WM_event_add_notifier(C, NC_MASK | ND_DATA, mask);
    DEG_id_tag_update(&mask->id, 0);

    return OPERATOR_FINISHED;
  }
  return OPERATOR_CANCELLED;
}

void MASK_OT_shape_key_insert(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Insert Shape Key";
  ot->description = "Insert mask shape keyframe for active mask layer at the current frame";
  ot->idname = "MASK_OT_shape_key_insert";

  /* api callbacks */
  ot->exec = mask_shape_key_insert_exec;
  ot->poll = ED_maskedit_mask_visible_splines_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static int mask_shape_key_clear_exec(bContext *C, wmOperator *UNUSED(op))
{
  Scene *scene = CTX_data_scene(C);
  const int frame = scene->r.cfra;
  Mask *mask = CTX_data_edit_mask(C);
  bool changed = false;

  LISTBASE_FOREACH (MaskLayer *, mask_layer, &mask->masklayers) {
    MaskLayerShape *mask_layer_shape;

    if (!ED_mask_layer_select_check(mask_layer)) {
      continue;
    }

    mask_layer_shape = BKE_mask_layer_shape_find_frame(mask_layer, frame);

    if (mask_layer_shape) {
      BKE_mask_layer_shape_unlink(mask_layer, mask_layer_shape);
      changed = true;
    }
  }

  if (changed) {
    WM_event_add_notifier(C, NC_MASK | ND_DATA, mask);
    DEG_id_tag_update(&mask->id, ID_RECALC_GEOMETRY);

    return OPERATOR_FINISHED;
  }
  return OPERATOR_CANCELLED;
}

void MASK_OT_shape_key_clear(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Clear Shape Key";
  ot->description = "Remove mask shape keyframe for active mask layer at the current frame";
  ot->idname = "MASK_OT_shape_key_clear";

  /* api callbacks */
  ot->exec = mask_shape_key_clear_exec;
  ot->poll = ED_maskedit_mask_visible_splines_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static int mask_shape_key_feather_reset_exec(bContext *C, wmOperator *UNUSED(op))
{
  Scene *scene = CTX_data_scene(C);
  const int frame = scene->r.cfra;
  Mask *mask = CTX_data_edit_mask(C);
  bool changed = false;

  LISTBASE_FOREACH (MaskLayer *, mask_layer, &mask->masklayers) {

    if (mask_layer->visibility_flag & (MASK_HIDE_VIEW | MASK_HIDE_SELECT)) {
      continue;
    }

    if (mask_layer->splines_shapes.first) {
      MaskLayerShape *mask_layer_shape_reset;
      MaskLayerShape *mask_layer_shape;

      /* get the shapekey of the current state */
      mask_layer_shape_reset = BKE_mask_layer_shape_alloc(mask_layer, frame);
      /* initialize from mask - as if inseting a keyframe */
      BKE_mask_layer_shape_from_mask(mask_layer, mask_layer_shape_reset);

      for (mask_layer_shape = mask_layer->splines_shapes.first; mask_layer_shape;
           mask_layer_shape = mask_layer_shape->next) {

        if (mask_layer_shape_reset->tot_vert == mask_layer_shape->tot_vert) {
          int i_abs = 0;
          MaskLayerShapeElem *shape_ele_src;
          MaskLayerShapeElem *shape_ele_dst;

          shape_ele_src = (MaskLayerShapeElem *)mask_layer_shape_reset->data;
          shape_ele_dst = (MaskLayerShapeElem *)mask_layer_shape->data;

          LISTBASE_FOREACH (MaskSpline *, spline, &mask_layer->splines) {
            for (int i = 0; i < spline->tot_point; i++) {
              MaskSplinePoint *point = &spline->points[i];

              if (MASKPOINT_ISSEL_ANY(point)) {
                /* TODO: nicer access here. */
                shape_ele_dst->value[6] = shape_ele_src->value[6];
              }

              shape_ele_src++;
              shape_ele_dst++;

              i_abs++;
            }
          }
        }
        else {
          // printf("%s: skipping\n", __func__);
        }

        changed = true;
      }

      BKE_mask_layer_shape_free(mask_layer_shape_reset);
    }
  }

  if (changed) {
    WM_event_add_notifier(C, NC_MASK | ND_DATA, mask);
    DEG_id_tag_update(&mask->id, 0);

    return OPERATOR_FINISHED;
  }
  return OPERATOR_CANCELLED;
}

void MASK_OT_shape_key_feather_reset(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Feather Reset Animation";
  ot->description = "Reset feather weights on all selected points animation values";
  ot->idname = "MASK_OT_shape_key_feather_reset";

  /* api callbacks */
  ot->exec = mask_shape_key_feather_reset_exec;
  ot->poll = ED_maskedit_mask_visible_splines_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/*
 * - loop over selected shape-keys.
 * - find first-selected/last-selected pairs.
 * - move these into a temp list.
 * - re-key all the original shapes.
 * - copy unselected values back from the original.
 * - free the original.
 */
static int mask_shape_key_rekey_exec(bContext *C, wmOperator *op)
{
  Scene *scene = CTX_data_scene(C);
  const int frame = scene->r.cfra;
  Mask *mask = CTX_data_edit_mask(C);
  bool changed = false;

  const bool do_feather = RNA_boolean_get(op->ptr, "feather");
  const bool do_location = RNA_boolean_get(op->ptr, "location");

  LISTBASE_FOREACH (MaskLayer *, mask_layer, &mask->masklayers) {
    if (mask_layer->visibility_flag & (MASK_HIDE_VIEW | MASK_HIDE_SELECT)) {
      continue;
    }

    /* we need at least one point selected here to bother re-interpolating */
    if (!ED_mask_layer_select_check(mask_layer)) {
      continue;
    }

    if (mask_layer->splines_shapes.first) {
      MaskLayerShape *mask_layer_shape, *mask_layer_shape_next;
      MaskLayerShape *mask_layer_shape_lastsel = NULL;

      for (mask_layer_shape = mask_layer->splines_shapes.first; mask_layer_shape;
           mask_layer_shape = mask_layer_shape_next) {
        MaskLayerShape *mask_layer_shape_a = NULL;
        MaskLayerShape *mask_layer_shape_b = NULL;

        mask_layer_shape_next = mask_layer_shape->next;

        /* find contiguous selections */
        if (mask_layer_shape->flag & MASK_SHAPE_SELECT) {
          if (mask_layer_shape_lastsel == NULL) {
            mask_layer_shape_lastsel = mask_layer_shape;
          }
          if ((mask_layer_shape->next == NULL) ||
              (((MaskLayerShape *)mask_layer_shape->next)->flag & MASK_SHAPE_SELECT) == 0) {
            mask_layer_shape_a = mask_layer_shape_lastsel;
            mask_layer_shape_b = mask_layer_shape;
            mask_layer_shape_lastsel = NULL;

            /* this will be freed below, step over selection */
            mask_layer_shape_next = mask_layer_shape->next;
          }
        }

        /* we have a from<>to? - re-interpolate! */
        if (mask_layer_shape_a && mask_layer_shape_b) {
          ListBase shapes_tmp = {NULL, NULL};
          MaskLayerShape *mask_layer_shape_tmp;
          MaskLayerShape *mask_layer_shape_tmp_next;
          MaskLayerShape *mask_layer_shape_tmp_last = mask_layer_shape_b->next;
          MaskLayerShape *mask_layer_shape_tmp_rekey;

          /* move keys */
          for (mask_layer_shape_tmp = mask_layer_shape_a;
               mask_layer_shape_tmp && (mask_layer_shape_tmp != mask_layer_shape_tmp_last);
               mask_layer_shape_tmp = mask_layer_shape_tmp_next) {
            mask_layer_shape_tmp_next = mask_layer_shape_tmp->next;
            BLI_remlink(&mask_layer->splines_shapes, mask_layer_shape_tmp);
            BLI_addtail(&shapes_tmp, mask_layer_shape_tmp);
          }

          /* re-key, NOTE: can't modify the keys here since it messes up. */
          for (mask_layer_shape_tmp = shapes_tmp.first; mask_layer_shape_tmp;
               mask_layer_shape_tmp = mask_layer_shape_tmp->next) {
            BKE_mask_layer_evaluate(mask_layer, mask_layer_shape_tmp->frame, true);
            mask_layer_shape_tmp_rekey = BKE_mask_layer_shape_verify_frame(
                mask_layer, mask_layer_shape_tmp->frame);
            BKE_mask_layer_shape_from_mask(mask_layer, mask_layer_shape_tmp_rekey);
            mask_layer_shape_tmp_rekey->flag = mask_layer_shape_tmp->flag & MASK_SHAPE_SELECT;
          }

          /* restore unselected points and free copies */
          for (mask_layer_shape_tmp = shapes_tmp.first; mask_layer_shape_tmp;
               mask_layer_shape_tmp = mask_layer_shape_tmp_next) {
            /* restore */
            int i_abs = 0;
            MaskLayerShapeElem *shape_ele_src;
            MaskLayerShapeElem *shape_ele_dst;

            mask_layer_shape_tmp_next = mask_layer_shape_tmp->next;

            /* we know this exists, added above */
            mask_layer_shape_tmp_rekey = BKE_mask_layer_shape_find_frame(
                mask_layer, mask_layer_shape_tmp->frame);

            shape_ele_src = (MaskLayerShapeElem *)mask_layer_shape_tmp->data;
            shape_ele_dst = (MaskLayerShapeElem *)mask_layer_shape_tmp_rekey->data;

            LISTBASE_FOREACH (MaskSpline *, spline, &mask_layer->splines) {
              for (int i = 0; i < spline->tot_point; i++) {
                MaskSplinePoint *point = &spline->points[i];

                /* not especially efficient but makes this easier to follow */
                SWAP(MaskLayerShapeElem, *shape_ele_src, *shape_ele_dst);

                if (MASKPOINT_ISSEL_ANY(point)) {
                  if (do_location) {
                    memcpy(shape_ele_dst->value, shape_ele_src->value, sizeof(float[6]));
                  }
                  if (do_feather) {
                    shape_ele_dst->value[6] = shape_ele_src->value[6];
                  }
                }

                shape_ele_src++;
                shape_ele_dst++;

                i_abs++;
              }
            }

            BKE_mask_layer_shape_free(mask_layer_shape_tmp);
          }

          changed = true;
        }
      }

      /* re-evaluate */
      BKE_mask_layer_evaluate(mask_layer, frame, true);
    }
  }

  if (changed) {
    WM_event_add_notifier(C, NC_MASK | ND_DATA, mask);
    DEG_id_tag_update(&mask->id, 0);

    return OPERATOR_FINISHED;
  }
  return OPERATOR_CANCELLED;
}

void MASK_OT_shape_key_rekey(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Re-Key Points of Selected Shapes";
  ot->description =
      "Recalculate animation data on selected points for frames selected in the dopesheet";
  ot->idname = "MASK_OT_shape_key_rekey";

  /* api callbacks */
  ot->exec = mask_shape_key_rekey_exec;
  ot->poll = ED_maskedit_mask_visible_splines_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  RNA_def_boolean(ot->srna, "location", true, "Location", "");
  RNA_def_boolean(ot->srna, "feather", true, "Feather", "");
}

/* *** Shape Key Utils *** */

void ED_mask_layer_shape_auto_key(MaskLayer *mask_layer, const int frame)
{
  MaskLayerShape *mask_layer_shape;

  mask_layer_shape = BKE_mask_layer_shape_verify_frame(mask_layer, frame);
  BKE_mask_layer_shape_from_mask(mask_layer, mask_layer_shape);
}

bool ED_mask_layer_shape_auto_key_all(Mask *mask, const int frame)
{
  bool changed = false;

  LISTBASE_FOREACH (MaskLayer *, mask_layer, &mask->masklayers) {
    ED_mask_layer_shape_auto_key(mask_layer, frame);
    changed = true;
  }

  return changed;
}

bool ED_mask_layer_shape_auto_key_select(Mask *mask, const int frame)
{
  bool changed = false;

  LISTBASE_FOREACH (MaskLayer *, mask_layer, &mask->masklayers) {

    if (!ED_mask_layer_select_check(mask_layer)) {
      continue;
    }

    ED_mask_layer_shape_auto_key(mask_layer, frame);
    changed = true;
  }

  return changed;
}
