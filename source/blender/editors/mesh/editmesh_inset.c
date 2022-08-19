/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edmesh
 */

#include "MEM_guardedalloc.h"

#include "DNA_object_types.h"

#include "BLI_math.h"
#include "BLI_string.h"

#include "BLT_translation.h"

#include "BKE_context.h"
#include "BKE_editmesh.h"
#include "BKE_global.h"
#include "BKE_layer.h"
#include "BKE_unit.h"

#include "RNA_access.h"
#include "RNA_define.h"

#include "WM_api.h"
#include "WM_types.h"

#include "UI_interface.h"

#include "ED_mesh.h"
#include "ED_numinput.h"
#include "ED_screen.h"
#include "ED_space_api.h"
#include "ED_transform.h"
#include "ED_util.h"
#include "ED_view3d.h"

#include "mesh_intern.h" /* own include */

typedef struct {
  /** Must have a valid edit-mesh. */
  Object *ob;
  BMBackup mesh_backup;
} InsetObjectStore;

typedef struct {
  float old_thickness;
  float old_depth;
  bool modify_depth;
  float initial_length;
  float pixel_size; /* use when mouse input is interpreted as spatial distance */
  bool is_modal;
  bool shift;
  float shift_amount;
  float max_obj_scale;
  NumInput num_input;

  InsetObjectStore *ob_store;
  uint ob_store_len;

  /* modal only */
  int launch_event;
  float mcenter[2];
  void *draw_handle_pixel;
} InsetData;

static void edbm_inset_update_header(wmOperator *op, bContext *C)
{
  InsetData *opdata = op->customdata;

  const char *str = TIP_(
      "Confirm: Enter/LClick, Cancel: (Esc/RClick), Thickness: %s, "
      "Depth (Ctrl to tweak): %s (%s), Outset (O): (%s), Boundary (B): (%s), Individual (I): "
      "(%s)");

  char msg[UI_MAX_DRAW_STR];
  ScrArea *area = CTX_wm_area(C);
  Scene *sce = CTX_data_scene(C);

  if (area) {
    char flts_str[NUM_STR_REP_LEN * 2];
    if (hasNumInput(&opdata->num_input)) {
      outputNumInput(&opdata->num_input, flts_str, &sce->unit);
    }
    else {
      BKE_unit_value_as_string(flts_str,
                               NUM_STR_REP_LEN,
                               RNA_float_get(op->ptr, "thickness"),
                               4,
                               B_UNIT_LENGTH,
                               &sce->unit,
                               true);
      BKE_unit_value_as_string(flts_str + NUM_STR_REP_LEN,
                               NUM_STR_REP_LEN,
                               RNA_float_get(op->ptr, "depth"),
                               4,
                               B_UNIT_LENGTH,
                               &sce->unit,
                               true);
    }
    BLI_snprintf(msg,
                 sizeof(msg),
                 str,
                 flts_str,
                 flts_str + NUM_STR_REP_LEN,
                 WM_bool_as_string(opdata->modify_depth),
                 WM_bool_as_string(RNA_boolean_get(op->ptr, "use_outset")),
                 WM_bool_as_string(RNA_boolean_get(op->ptr, "use_boundary")),
                 WM_bool_as_string(RNA_boolean_get(op->ptr, "use_individual")));

    ED_area_status_text(area, msg);
  }
}

static bool edbm_inset_init(bContext *C, wmOperator *op, const bool is_modal)
{
  InsetData *opdata;
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);

  if (is_modal) {
    RNA_float_set(op->ptr, "thickness", 0.0f);
    RNA_float_set(op->ptr, "depth", 0.0f);
  }

  op->customdata = opdata = MEM_mallocN(sizeof(InsetData), "inset_operator_data");

  uint objects_used_len = 0;

  opdata->max_obj_scale = FLT_MIN;

  {
    uint ob_store_len = 0;
    Object **objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
        view_layer, CTX_wm_view3d(C), &ob_store_len);
    opdata->ob_store = MEM_malloc_arrayN(ob_store_len, sizeof(*opdata->ob_store), __func__);
    for (uint ob_index = 0; ob_index < ob_store_len; ob_index++) {
      Object *obedit = objects[ob_index];
      float scale = mat4_to_scale(obedit->obmat);
      opdata->max_obj_scale = max_ff(opdata->max_obj_scale, scale);
      BMEditMesh *em = BKE_editmesh_from_object(obedit);
      if (em->bm->totvertsel > 0) {
        opdata->ob_store[objects_used_len].ob = obedit;
        objects_used_len++;
      }
    }
    MEM_freeN(objects);
    opdata->ob_store_len = objects_used_len;
  }

  opdata->old_thickness = 0.0;
  opdata->old_depth = 0.0;
  opdata->modify_depth = false;
  opdata->shift = false;
  opdata->shift_amount = 0.0f;
  opdata->is_modal = is_modal;

  initNumInput(&opdata->num_input);
  opdata->num_input.idx_max = 1; /* Two elements. */
  opdata->num_input.unit_sys = scene->unit.system;
  opdata->num_input.unit_type[0] = B_UNIT_LENGTH;
  opdata->num_input.unit_type[1] = B_UNIT_LENGTH;

  if (is_modal) {
    ARegion *region = CTX_wm_region(C);

    for (uint ob_index = 0; ob_index < opdata->ob_store_len; ob_index++) {
      Object *obedit = opdata->ob_store[ob_index].ob;
      BMEditMesh *em = BKE_editmesh_from_object(obedit);
      opdata->ob_store[ob_index].mesh_backup = EDBM_redo_state_store(em);
    }

    opdata->draw_handle_pixel = ED_region_draw_cb_activate(
        region->type, ED_region_draw_mouse_line_cb, opdata->mcenter, REGION_DRAW_POST_PIXEL);
    G.moving = G_TRANSFORM_EDIT;
  }

  return true;
}

static void edbm_inset_exit(bContext *C, wmOperator *op)
{
  InsetData *opdata;
  ScrArea *area = CTX_wm_area(C);

  opdata = op->customdata;

  if (opdata->is_modal) {
    ARegion *region = CTX_wm_region(C);
    for (uint ob_index = 0; ob_index < opdata->ob_store_len; ob_index++) {
      EDBM_redo_state_free(&opdata->ob_store[ob_index].mesh_backup);
    }
    ED_region_draw_cb_exit(region->type, opdata->draw_handle_pixel);
    G.moving = 0;
  }

  if (area) {
    ED_area_status_text(area, NULL);
  }

  MEM_SAFE_FREE(opdata->ob_store);
  MEM_SAFE_FREE(op->customdata);
}

static void edbm_inset_cancel(bContext *C, wmOperator *op)
{
  InsetData *opdata;

  opdata = op->customdata;
  if (opdata->is_modal) {
    for (uint ob_index = 0; ob_index < opdata->ob_store_len; ob_index++) {
      Object *obedit = opdata->ob_store[ob_index].ob;
      BMEditMesh *em = BKE_editmesh_from_object(obedit);
      EDBM_redo_state_restore_and_free(&opdata->ob_store[ob_index].mesh_backup, em, true);
      EDBM_update(obedit->data,
                  &(const struct EDBMUpdate_Params){
                      .calc_looptri = false,
                      .calc_normals = false,
                      .is_destructive = true,
                  });
    }
  }

  edbm_inset_exit(C, op);

  /* need to force redisplay or we may still view the modified result */
  ED_region_tag_redraw(CTX_wm_region(C));
}

static bool edbm_inset_calc(wmOperator *op)
{
  InsetData *opdata;
  BMOperator bmop;
  bool changed = false;

  const bool use_boundary = RNA_boolean_get(op->ptr, "use_boundary");
  const bool use_even_offset = RNA_boolean_get(op->ptr, "use_even_offset");
  const bool use_relative_offset = RNA_boolean_get(op->ptr, "use_relative_offset");
  const bool use_edge_rail = RNA_boolean_get(op->ptr, "use_edge_rail");
  const float thickness = RNA_float_get(op->ptr, "thickness");
  const float depth = RNA_float_get(op->ptr, "depth");
  const bool use_outset = RNA_boolean_get(op->ptr, "use_outset");
  /* not passed onto the BMO */
  const bool use_select_inset = RNA_boolean_get(op->ptr, "use_select_inset");
  const bool use_individual = RNA_boolean_get(op->ptr, "use_individual");
  const bool use_interpolate = RNA_boolean_get(op->ptr, "use_interpolate");

  opdata = op->customdata;

  for (uint ob_index = 0; ob_index < opdata->ob_store_len; ob_index++) {
    Object *obedit = opdata->ob_store[ob_index].ob;
    BMEditMesh *em = BKE_editmesh_from_object(obedit);

    if (opdata->is_modal) {
      EDBM_redo_state_restore(&opdata->ob_store[ob_index].mesh_backup, em, false);
    }

    if (use_individual) {
      EDBM_op_init(em,
                   &bmop,
                   op,
                   "inset_individual faces=%hf use_even_offset=%b  use_relative_offset=%b "
                   "use_interpolate=%b thickness=%f depth=%f",
                   BM_ELEM_SELECT,
                   use_even_offset,
                   use_relative_offset,
                   use_interpolate,
                   thickness,
                   depth);
    }
    else {
      EDBM_op_init(
          em,
          &bmop,
          op,
          "inset_region faces=%hf use_boundary=%b use_even_offset=%b use_relative_offset=%b "
          "use_interpolate=%b thickness=%f depth=%f use_outset=%b use_edge_rail=%b",
          BM_ELEM_SELECT,
          use_boundary,
          use_even_offset,
          use_relative_offset,
          use_interpolate,
          thickness,
          depth,
          use_outset,
          use_edge_rail);

      if (use_outset) {
        BMO_slot_buffer_from_enabled_hflag(
            em->bm, &bmop, bmop.slots_in, "faces_exclude", BM_FACE, BM_ELEM_HIDDEN);
      }
    }
    BMO_op_exec(em->bm, &bmop);

    if (use_select_inset) {
      /* deselect original faces/verts */
      EDBM_flag_disable_all(em, BM_ELEM_SELECT);
      BMO_slot_buffer_hflag_enable(
          em->bm, bmop.slots_out, "faces.out", BM_FACE, BM_ELEM_SELECT, true);
    }
    else {
      EDBM_flag_disable_all(em, BM_ELEM_SELECT);
      BMO_slot_buffer_hflag_enable(em->bm, bmop.slots_in, "faces", BM_FACE, BM_ELEM_SELECT, true);
    }

    if (!EDBM_op_finish(em, &bmop, op, true)) {
      continue;
    }

    EDBM_update(obedit->data,
                &(const struct EDBMUpdate_Params){
                    .calc_looptri = true,
                    .calc_normals = false,
                    .is_destructive = true,
                });
    changed = true;
  }
  return changed;
}

static int edbm_inset_exec(bContext *C, wmOperator *op)
{
  if (!edbm_inset_init(C, op, false)) {
    return OPERATOR_CANCELLED;
  }

  if (!edbm_inset_calc(op)) {
    edbm_inset_exit(C, op);
    return OPERATOR_CANCELLED;
  }

  edbm_inset_exit(C, op);
  return OPERATOR_FINISHED;
}

static int edbm_inset_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  RegionView3D *rv3d = CTX_wm_region_view3d(C);
  InsetData *opdata;
  float mlen[2];
  float center_3d[3];

  if (!edbm_inset_init(C, op, true)) {
    return OPERATOR_CANCELLED;
  }

  opdata = op->customdata;

  opdata->launch_event = WM_userdef_event_type_from_keymap_type(event->type);

  /* initialize mouse values */
  if (!calculateTransformCenter(C, V3D_AROUND_CENTER_MEDIAN, center_3d, opdata->mcenter)) {
    /* in this case the tool will likely do nothing,
     * ideally this will never happen and should be checked for above */
    opdata->mcenter[0] = opdata->mcenter[1] = 0;
  }
  mlen[0] = opdata->mcenter[0] - event->mval[0];
  mlen[1] = opdata->mcenter[1] - event->mval[1];
  opdata->initial_length = len_v2(mlen);
  opdata->pixel_size = rv3d ? ED_view3d_pixel_size(rv3d, center_3d) : 1.0f;

  edbm_inset_calc(op);

  edbm_inset_update_header(op, C);

  WM_event_add_modal_handler(C, op);
  return OPERATOR_RUNNING_MODAL;
}

static int edbm_inset_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  InsetData *opdata = op->customdata;
  const bool has_numinput = hasNumInput(&opdata->num_input);

  /* Modal numinput active, try to handle numeric inputs first... */
  if (event->val == KM_PRESS && has_numinput && handleNumInput(C, &opdata->num_input, event)) {
    float amounts[2] = {RNA_float_get(op->ptr, "thickness"), RNA_float_get(op->ptr, "depth")};
    applyNumInput(&opdata->num_input, amounts);
    amounts[0] = max_ff(amounts[0], 0.0f);
    RNA_float_set(op->ptr, "thickness", amounts[0]);
    RNA_float_set(op->ptr, "depth", amounts[1]);

    if (edbm_inset_calc(op)) {
      edbm_inset_update_header(op, C);
      return OPERATOR_RUNNING_MODAL;
    }
    edbm_inset_cancel(C, op);
    return OPERATOR_CANCELLED;
  }
  if ((event->type == opdata->launch_event) && (event->val == KM_RELEASE) &&
      RNA_boolean_get(op->ptr, "release_confirm")) {
    edbm_inset_calc(op);
    edbm_inset_exit(C, op);
    return OPERATOR_FINISHED;
  }

  bool handled = false;
  switch (event->type) {
    case EVT_ESCKEY:
    case RIGHTMOUSE:
      edbm_inset_cancel(C, op);
      return OPERATOR_CANCELLED;

    case MOUSEMOVE:
      if (!has_numinput) {
        float mdiff[2];
        float amount;

        mdiff[0] = opdata->mcenter[0] - event->mval[0];
        mdiff[1] = opdata->mcenter[1] - event->mval[1];

        if (opdata->modify_depth) {
          amount = opdata->old_depth +
                   ((len_v2(mdiff) - opdata->initial_length) * opdata->pixel_size) /
                       opdata->max_obj_scale;
        }
        else {
          amount = opdata->old_thickness -
                   ((len_v2(mdiff) - opdata->initial_length) * opdata->pixel_size) /
                       opdata->max_obj_scale;
        }

        /* Fake shift-transform... */
        if (opdata->shift) {
          amount = (amount - opdata->shift_amount) * 0.1f + opdata->shift_amount;
        }

        if (opdata->modify_depth) {
          RNA_float_set(op->ptr, "depth", amount);
        }
        else {
          amount = max_ff(amount, 0.0f);
          RNA_float_set(op->ptr, "thickness", amount);
        }

        if (edbm_inset_calc(op)) {
          edbm_inset_update_header(op, C);
        }
        else {
          edbm_inset_cancel(C, op);
          return OPERATOR_CANCELLED;
        }
        handled = true;
      }
      break;

    case LEFTMOUSE:
    case EVT_PADENTER:
    case EVT_RETKEY:
      if ((event->val == KM_PRESS) ||
          ((event->val == KM_RELEASE) && RNA_boolean_get(op->ptr, "release_confirm"))) {
        edbm_inset_calc(op);
        edbm_inset_exit(C, op);
        return OPERATOR_FINISHED;
      }
      break;
    case EVT_LEFTSHIFTKEY:
    case EVT_RIGHTSHIFTKEY:
      if (event->val == KM_PRESS) {
        if (opdata->modify_depth) {
          opdata->shift_amount = RNA_float_get(op->ptr, "depth");
        }
        else {
          opdata->shift_amount = RNA_float_get(op->ptr, "thickness");
        }
        opdata->shift = true;
        handled = true;
      }
      else {
        opdata->shift_amount = 0.0f;
        opdata->shift = false;
        handled = true;
      }
      break;

    case EVT_LEFTCTRLKEY:
    case EVT_RIGHTCTRLKEY: {
      float mlen[2];

      mlen[0] = opdata->mcenter[0] - event->mval[0];
      mlen[1] = opdata->mcenter[1] - event->mval[1];

      if (event->val == KM_PRESS) {
        opdata->old_thickness = RNA_float_get(op->ptr, "thickness");
        if (opdata->shift) {
          opdata->shift_amount = opdata->old_thickness;
        }
        opdata->modify_depth = true;
      }
      else {
        opdata->old_depth = RNA_float_get(op->ptr, "depth");
        if (opdata->shift) {
          opdata->shift_amount = opdata->old_depth;
        }
        opdata->modify_depth = false;
      }
      opdata->initial_length = len_v2(mlen);

      edbm_inset_update_header(op, C);
      handled = true;
      break;
    }

    case EVT_OKEY:
      if (event->val == KM_PRESS) {
        const bool use_outset = RNA_boolean_get(op->ptr, "use_outset");
        RNA_boolean_set(op->ptr, "use_outset", !use_outset);
        if (edbm_inset_calc(op)) {
          edbm_inset_update_header(op, C);
        }
        else {
          edbm_inset_cancel(C, op);
          return OPERATOR_CANCELLED;
        }
        handled = true;
      }
      break;
    case EVT_BKEY:
      if (event->val == KM_PRESS) {
        const bool use_boundary = RNA_boolean_get(op->ptr, "use_boundary");
        RNA_boolean_set(op->ptr, "use_boundary", !use_boundary);
        if (edbm_inset_calc(op)) {
          edbm_inset_update_header(op, C);
        }
        else {
          edbm_inset_cancel(C, op);
          return OPERATOR_CANCELLED;
        }
        handled = true;
      }
      break;
    case EVT_IKEY:
      if (event->val == KM_PRESS) {
        const bool use_individual = RNA_boolean_get(op->ptr, "use_individual");
        RNA_boolean_set(op->ptr, "use_individual", !use_individual);
        if (edbm_inset_calc(op)) {
          edbm_inset_update_header(op, C);
        }
        else {
          edbm_inset_cancel(C, op);
          return OPERATOR_CANCELLED;
        }
        handled = true;
      }
      break;
  }

  /* Modal numinput inactive, try to handle numeric inputs last... */
  if (!handled && event->val == KM_PRESS && handleNumInput(C, &opdata->num_input, event)) {
    float amounts[2] = {RNA_float_get(op->ptr, "thickness"), RNA_float_get(op->ptr, "depth")};
    applyNumInput(&opdata->num_input, amounts);
    amounts[0] = max_ff(amounts[0], 0.0f);
    RNA_float_set(op->ptr, "thickness", amounts[0]);
    RNA_float_set(op->ptr, "depth", amounts[1]);

    if (edbm_inset_calc(op)) {
      edbm_inset_update_header(op, C);
      return OPERATOR_RUNNING_MODAL;
    }
    edbm_inset_cancel(C, op);
    return OPERATOR_CANCELLED;
  }

  return OPERATOR_RUNNING_MODAL;
}

void MESH_OT_inset(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* identifiers */
  ot->name = "Inset Faces";
  ot->idname = "MESH_OT_inset";
  ot->description = "Inset new faces into selected faces";

  /* api callbacks */
  ot->invoke = edbm_inset_invoke;
  ot->modal = edbm_inset_modal;
  ot->exec = edbm_inset_exec;
  ot->cancel = edbm_inset_cancel;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_GRAB_CURSOR_XY | OPTYPE_BLOCKING;

  /* properties */
  RNA_def_boolean(ot->srna, "use_boundary", true, "Boundary", "Inset face boundaries");
  RNA_def_boolean(ot->srna,
                  "use_even_offset",
                  true,
                  "Offset Even",
                  "Scale the offset to give more even thickness");
  RNA_def_boolean(ot->srna,
                  "use_relative_offset",
                  false,
                  "Offset Relative",
                  "Scale the offset by surrounding geometry");
  RNA_def_boolean(
      ot->srna, "use_edge_rail", false, "Edge Rail", "Inset the region along existing edges");

  prop = RNA_def_float_distance(
      ot->srna, "thickness", 0.0f, 0.0f, 1e12f, "Thickness", "", 0.0f, 10.0f);
  /* use 1 rather than 10 for max else dragging the button moves too far */
  RNA_def_property_ui_range(prop, 0.0, 1.0, 0.01, 4);

  prop = RNA_def_float_distance(
      ot->srna, "depth", 0.0f, -1e12f, 1e12f, "Depth", "", -10.0f, 10.0f);
  RNA_def_property_ui_range(prop, -10.0f, 10.0f, 0.01, 4);

  RNA_def_boolean(ot->srna, "use_outset", false, "Outset", "Outset rather than inset");
  RNA_def_boolean(
      ot->srna, "use_select_inset", false, "Select Outer", "Select the new inset faces");
  RNA_def_boolean(ot->srna, "use_individual", false, "Individual", "Individual face inset");
  RNA_def_boolean(
      ot->srna, "use_interpolate", true, "Interpolate", "Blend face data across the inset");

  prop = RNA_def_boolean(ot->srna, "release_confirm", 0, "Confirm on Release", "");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
}
