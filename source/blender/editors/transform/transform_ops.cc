/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edtransform
 */

#include "MEM_guardedalloc.h"

#include "DNA_curve_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BLI_math_vector.h"
#include "BLI_utildefines.h"

#include "BLT_translation.hh"

#include "BKE_context.hh"
#include "BKE_global.hh"
#include "BKE_report.hh"
#include "BKE_scene.hh"
#include "BKE_screen.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"

#include "WM_api.hh"
#include "WM_message.hh"
#include "WM_toolsystem.hh"
#include "WM_types.hh"

#include "UI_interface.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "ED_screen.hh"
/** For #USE_LOOPSLIDE_HACK only. */
#include "ED_mesh.hh"

#include "transform.hh"
#include "transform_convert.hh"

namespace blender::ed::transform {

struct TransformModeItem {
  const char *idname;
  int mode;
  void (*opfunc)(wmOperatorType *);
};

static const float VecZero[3] = {0, 0, 0};
static const float VecOne[3] = {1, 1, 1};

static const char OP_TRANSLATION[] = "TRANSFORM_OT_translate";
static const char OP_ROTATION[] = "TRANSFORM_OT_rotate";
static const char OP_TOSPHERE[] = "TRANSFORM_OT_tosphere";
static const char OP_RESIZE[] = "TRANSFORM_OT_resize";
static const char OP_SKIN_RESIZE[] = "TRANSFORM_OT_skin_resize";
static const char OP_SHEAR[] = "TRANSFORM_OT_shear";
static const char OP_BEND[] = "TRANSFORM_OT_bend";
static const char OP_SHRINK_FATTEN[] = "TRANSFORM_OT_shrink_fatten";
static const char OP_PUSH_PULL[] = "TRANSFORM_OT_push_pull";
static const char OP_TILT[] = "TRANSFORM_OT_tilt";
static const char OP_TRACKBALL[] = "TRANSFORM_OT_trackball";
static const char OP_MIRROR[] = "TRANSFORM_OT_mirror";
static const char OP_BONE_SIZE[] = "TRANSFORM_OT_bbone_resize";
static const char OP_EDGE_SLIDE[] = "TRANSFORM_OT_edge_slide";
static const char OP_VERT_SLIDE[] = "TRANSFORM_OT_vert_slide";
static const char OP_EDGE_CREASE[] = "TRANSFORM_OT_edge_crease";
static const char OP_VERT_CREASE[] = "TRANSFORM_OT_vert_crease";
static const char OP_EDGE_BWEIGHT[] = "TRANSFORM_OT_edge_bevelweight";
static const char OP_SEQ_SLIDE[] = "TRANSFORM_OT_seq_slide";
static const char OP_NORMAL_ROTATION[] = "TRANSFORM_OT_rotate_normal";

static void TRANSFORM_OT_translate(wmOperatorType *ot);
static void TRANSFORM_OT_rotate(wmOperatorType *ot);
static void TRANSFORM_OT_tosphere(wmOperatorType *ot);
static void TRANSFORM_OT_resize(wmOperatorType *ot);
static void TRANSFORM_OT_skin_resize(wmOperatorType *ot);
static void TRANSFORM_OT_shear(wmOperatorType *ot);
static void TRANSFORM_OT_bend(wmOperatorType *ot);
static void TRANSFORM_OT_shrink_fatten(wmOperatorType *ot);
static void TRANSFORM_OT_push_pull(wmOperatorType *ot);
static void TRANSFORM_OT_tilt(wmOperatorType *ot);
static void TRANSFORM_OT_trackball(wmOperatorType *ot);
static void TRANSFORM_OT_mirror(wmOperatorType *ot);
static void TRANSFORM_OT_bbone_resize(wmOperatorType *ot);
static void TRANSFORM_OT_edge_slide(wmOperatorType *ot);
static void TRANSFORM_OT_vert_slide(wmOperatorType *ot);
static void TRANSFORM_OT_edge_crease(wmOperatorType *ot);
static void TRANSFORM_OT_vert_crease(wmOperatorType *ot);
static void TRANSFORM_OT_edge_bevelweight(wmOperatorType *ot);
static void TRANSFORM_OT_seq_slide(wmOperatorType *ot);
static void TRANSFORM_OT_rotate_normal(wmOperatorType *ot);

static TransformModeItem transform_modes[] = {
    {OP_TRANSLATION, TFM_TRANSLATION, TRANSFORM_OT_translate},
    {OP_ROTATION, TFM_ROTATION, TRANSFORM_OT_rotate},
    {OP_TOSPHERE, TFM_TOSPHERE, TRANSFORM_OT_tosphere},
    {OP_RESIZE, TFM_RESIZE, TRANSFORM_OT_resize},
    {OP_SKIN_RESIZE, TFM_SKIN_RESIZE, TRANSFORM_OT_skin_resize},
    {OP_SHEAR, TFM_SHEAR, TRANSFORM_OT_shear},
    {OP_BEND, TFM_BEND, TRANSFORM_OT_bend},
    {OP_SHRINK_FATTEN, TFM_SHRINKFATTEN, TRANSFORM_OT_shrink_fatten},
    {OP_PUSH_PULL, TFM_PUSHPULL, TRANSFORM_OT_push_pull},
    {OP_TILT, TFM_TILT, TRANSFORM_OT_tilt},
    {OP_TRACKBALL, TFM_TRACKBALL, TRANSFORM_OT_trackball},
    {OP_MIRROR, TFM_MIRROR, TRANSFORM_OT_mirror},
    {OP_BONE_SIZE, TFM_BONESIZE, TRANSFORM_OT_bbone_resize},
    {OP_EDGE_SLIDE, TFM_EDGE_SLIDE, TRANSFORM_OT_edge_slide},
    {OP_VERT_SLIDE, TFM_VERT_SLIDE, TRANSFORM_OT_vert_slide},
    {OP_EDGE_CREASE, TFM_EDGE_CREASE, TRANSFORM_OT_edge_crease},
    {OP_VERT_CREASE, TFM_VERT_CREASE, TRANSFORM_OT_vert_crease},
    {OP_EDGE_BWEIGHT, TFM_BWEIGHT, TRANSFORM_OT_edge_bevelweight},
    {OP_SEQ_SLIDE, TFM_SEQ_SLIDE, TRANSFORM_OT_seq_slide},
    {OP_NORMAL_ROTATION, TFM_NORMAL_ROTATION, TRANSFORM_OT_rotate_normal},
    {nullptr, 0},
};

}  // namespace blender::ed::transform

const EnumPropertyItem rna_enum_transform_mode_type_items[] = {
    {blender::ed::transform::TFM_INIT, "INIT", 0, "Init", ""},
    {blender::ed::transform::TFM_DUMMY, "DUMMY", 0, "Dummy", ""},
    {blender::ed::transform::TFM_TRANSLATION, "TRANSLATION", 0, "Translation", ""},
    {blender::ed::transform::TFM_ROTATION, "ROTATION", 0, "Rotation", ""},
    {blender::ed::transform::TFM_RESIZE, "RESIZE", 0, "Resize", ""},
    {blender::ed::transform::TFM_SKIN_RESIZE, "SKIN_RESIZE", 0, "Skin Resize", ""},
    {blender::ed::transform::TFM_TOSPHERE, "TOSPHERE", 0, "To Sphere", ""},
    {blender::ed::transform::TFM_SHEAR, "SHEAR", 0, "Shear", ""},
    {blender::ed::transform::TFM_BEND, "BEND", 0, "Bend", ""},
    {blender::ed::transform::TFM_SHRINKFATTEN, "SHRINKFATTEN", 0, "Shrink/Fatten", ""},
    {blender::ed::transform::TFM_TILT, "TILT", 0, "Tilt", ""},
    {blender::ed::transform::TFM_TRACKBALL, "TRACKBALL", 0, "Trackball", ""},
    {blender::ed::transform::TFM_PUSHPULL, "PUSHPULL", 0, "Push/Pull", ""},
    {blender::ed::transform::TFM_EDGE_CREASE, "CREASE", 0, "Crease", ""},
    {blender::ed::transform::TFM_VERT_CREASE, "VERTEX_CREASE", 0, "Vertex Crease", ""},
    {blender::ed::transform::TFM_MIRROR, "MIRROR", 0, "Mirror", ""},
    {blender::ed::transform::TFM_BONESIZE, "BONE_SIZE", 0, "Bone Size", ""},
    {blender::ed::transform::TFM_BONE_ENVELOPE, "BONE_ENVELOPE", 0, "Bone Envelope", ""},
    {blender::ed::transform::TFM_BONE_ENVELOPE_DIST,
     "BONE_ENVELOPE_DIST",
     0,
     "Bone Envelope Distance",
     ""},
    {blender::ed::transform::TFM_CURVE_SHRINKFATTEN,
     "CURVE_SHRINKFATTEN",
     0,
     "Curve Shrink/Fatten",
     ""},
    {blender::ed::transform::TFM_MASK_SHRINKFATTEN,
     "MASK_SHRINKFATTEN",
     0,
     "Mask Shrink/Fatten",
     ""},
    {blender::ed::transform::TFM_BONE_ROLL, "BONE_ROLL", 0, "Bone Roll", ""},
    {blender::ed::transform::TFM_TIME_TRANSLATE, "TIME_TRANSLATE", 0, "Time Translate", ""},
    {blender::ed::transform::TFM_TIME_SLIDE, "TIME_SLIDE", 0, "Time Slide", ""},
    {blender::ed::transform::TFM_TIME_SCALE, "TIME_SCALE", 0, "Time Scale", ""},
    {blender::ed::transform::TFM_TIME_EXTEND, "TIME_EXTEND", 0, "Time Extend", ""},
    {blender::ed::transform::TFM_BAKE_TIME, "BAKE_TIME", 0, "Bake Time", ""},
    {blender::ed::transform::TFM_BWEIGHT, "BWEIGHT", 0, "Bevel Weight", ""},
    {blender::ed::transform::TFM_ALIGN, "ALIGN", 0, "Align", ""},
    {blender::ed::transform::TFM_EDGE_SLIDE, "EDGESLIDE", 0, "Edge Slide", ""},
    {blender::ed::transform::TFM_SEQ_SLIDE, "SEQSLIDE", 0, "Sequence Slide", ""},
    {blender::ed::transform::TFM_GPENCIL_OPACITY,
     "GPENCIL_OPACITY",
     0,
     "Grease Pencil Opacity",
     ""},
    {0, nullptr, 0, nullptr, nullptr},
};

namespace blender::ed::transform {

static wmOperatorStatus select_orientation_exec(bContext *C, wmOperator *op)
{
  Scene *scene = CTX_data_scene(C);

  int orientation = RNA_enum_get(op->ptr, "orientation");

  BKE_scene_orientation_slot_set_index(&scene->orientation_slots[SCE_ORIENT_DEFAULT], orientation);

  WM_event_add_notifier(C, NC_SCENE | ND_TOOLSETTINGS, nullptr);
  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_VIEW3D, nullptr);

  wmMsgBus *mbus = CTX_wm_message_bus(C);
  WM_msg_publish_rna_prop(mbus, &scene->id, scene, TransformOrientationSlot, type);

  return OPERATOR_FINISHED;
}

static wmOperatorStatus select_orientation_invoke(bContext *C,
                                                  wmOperator * /*op*/,
                                                  const wmEvent * /*event*/)
{
  uiPopupMenu *pup;
  uiLayout *layout;

  pup = UI_popup_menu_begin(C, IFACE_("Orientation"), ICON_NONE);
  layout = UI_popup_menu_layout(pup);
  layout->op_enum("TRANSFORM_OT_select_orientation", "orientation");
  UI_popup_menu_end(C, pup);

  return OPERATOR_INTERFACE;
}

static void TRANSFORM_OT_select_orientation(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* Identifiers. */
  ot->name = "Select Orientation";
  ot->description = "Select transformation orientation";
  ot->idname = "TRANSFORM_OT_select_orientation";
  ot->flag = OPTYPE_UNDO;

  /* API callbacks. */
  ot->invoke = select_orientation_invoke;
  ot->exec = select_orientation_exec;
  ot->poll = ED_operator_view3d_active;

  prop = RNA_def_property(ot->srna, "orientation", PROP_ENUM, PROP_NONE);
  RNA_def_property_ui_text(prop, "Orientation", "Transformation orientation");
  RNA_def_enum_funcs(prop, rna_TransformOrientation_itemf);
}

static wmOperatorStatus delete_orientation_exec(bContext *C, wmOperator * /*op*/)
{
  Scene *scene = CTX_data_scene(C);
  BIF_removeTransformOrientationIndex(C,
                                      scene->orientation_slots[SCE_ORIENT_DEFAULT].index_custom);

  WM_event_add_notifier(C, NC_SCENE | NA_EDITED, scene);

  wmMsgBus *mbus = CTX_wm_message_bus(C);
  WM_msg_publish_rna_prop(mbus, &scene->id, scene, Scene, transform_orientation_slots);

  return OPERATOR_FINISHED;
}

static wmOperatorStatus delete_orientation_invoke(bContext *C,
                                                  wmOperator *op,
                                                  const wmEvent * /*event*/)
{
  return delete_orientation_exec(C, op);
}

static bool delete_orientation_poll(bContext *C)
{
  if (ED_operator_areaactive(C) == 0) {
    return false;
  }

  Scene *scene = CTX_data_scene(C);
  return ((scene->orientation_slots[SCE_ORIENT_DEFAULT].type >= V3D_ORIENT_CUSTOM) &&
          (scene->orientation_slots[SCE_ORIENT_DEFAULT].index_custom != -1));
}

static void TRANSFORM_OT_delete_orientation(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Delete Orientation";
  ot->description = "Delete transformation orientation";
  ot->idname = "TRANSFORM_OT_delete_orientation";
  ot->flag = OPTYPE_UNDO;

  /* API callbacks. */
  ot->invoke = delete_orientation_invoke;
  ot->exec = delete_orientation_exec;
  ot->poll = delete_orientation_poll;
}

static wmOperatorStatus create_orientation_exec(bContext *C, wmOperator *op)
{
  char name[MAX_NAME];
  const bool use = RNA_boolean_get(op->ptr, "use");
  const bool overwrite = RNA_boolean_get(op->ptr, "overwrite");
  const bool use_view = RNA_boolean_get(op->ptr, "use_view");
  View3D *v3d = CTX_wm_view3d(C);
  Scene *scene = CTX_data_scene(C);

  RNA_string_get(op->ptr, "name", name);

  if (use && !v3d) {
    BKE_report(op->reports,
               RPT_ERROR,
               "Create Orientation's 'use' parameter only valid in a 3DView context");
    return OPERATOR_CANCELLED;
  }

  if (!BIF_createTransformOrientation(C, op->reports, name, use_view, use, overwrite)) {
    BKE_report(op->reports, RPT_ERROR, "Unable to create orientation");
    return OPERATOR_CANCELLED;
  }

  if (use) {
    wmMsgBus *mbus = CTX_wm_message_bus(C);
    WM_msg_publish_rna_prop(mbus, &scene->id, scene, Scene, transform_orientation_slots);
    WM_event_add_notifier(C, NC_SCENE | NA_EDITED, scene);
  }

  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_VIEW3D, nullptr);

  return OPERATOR_FINISHED;
}

static void TRANSFORM_OT_create_orientation(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Create Orientation";
  ot->description = "Create transformation orientation from selection";
  ot->idname = "TRANSFORM_OT_create_orientation";
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* API callbacks. */
  ot->exec = create_orientation_exec;
  ot->poll = ED_operator_areaactive;

  RNA_def_string(
      ot->srna, "name", nullptr, MAX_NAME, "Name", "Name of the new custom orientation");
  RNA_def_boolean(
      ot->srna,
      "use_view",
      false,
      "Use View",
      "Use the current view instead of the active object to create the new orientation");

  WM_operatortype_props_advanced_begin(ot);

  RNA_def_boolean(
      ot->srna, "use", false, "Use After Creation", "Select orientation after its creation");
  RNA_def_boolean(ot->srna,
                  "overwrite",
                  false,
                  "Overwrite Previous",
                  "Overwrite previously created orientation with same name");
}

#ifdef USE_LOOPSLIDE_HACK
/**
 * Special hack for #MESH_OT_loopcut_slide so we get back to the selection mode
 * Do this for all meshes in multi-object edit-mode so their select-mode is in sync
 * for following operators
 */
static void transformops_loopsel_hack(bContext *C, wmOperator *op)
{
  if (op->type->idname == OP_EDGE_SLIDE) {
    if (op->opm && op->opm->opm && op->opm->opm->prev) {
      wmOperator *op_prev = op->opm->opm->prev;
      Scene *scene = CTX_data_scene(C);
      bool mesh_select_mode[3];
      PropertyRNA *prop = RNA_struct_find_property(op_prev->ptr, "mesh_select_mode_init");

      if (prop && RNA_property_is_set(op_prev->ptr, prop)) {
        ToolSettings *ts = scene->toolsettings;
        short selectmode_orig;

        RNA_property_boolean_get_array(op_prev->ptr, prop, mesh_select_mode);
        selectmode_orig = ((mesh_select_mode[0] ? SCE_SELECT_VERTEX : 0) |
                           (mesh_select_mode[1] ? SCE_SELECT_EDGE : 0) |
                           (mesh_select_mode[2] ? SCE_SELECT_FACE : 0));

        /* Still switch if we were originally in face select mode. */
        if ((ts->selectmode != selectmode_orig) && (selectmode_orig != SCE_SELECT_FACE)) {
          ts->selectmode = selectmode_orig;
          EDBM_selectmode_set_multi(C, selectmode_orig);
        }
      }
    }
  }
}
#else
/* Prevent removal by cleanup. */
#  error "loopslide hack removed!"
#endif /* USE_LOOPSLIDE_HACK */

static void transformops_exit(bContext *C, wmOperator *op)
{
#ifdef USE_LOOPSLIDE_HACK
  transformops_loopsel_hack(C, op);
#endif

  TransInfo *t = static_cast<TransInfo *>(op->customdata);
  saveTransform(C, t, op);
  MEM_freeN(t);
  op->customdata = nullptr;
  G.moving = 0;
}

static int transformops_mode(wmOperator *op)
{
  for (TransformModeItem *tmode = transform_modes; tmode->idname; tmode++) {
    if (op->type->idname == tmode->idname) {
      return tmode->mode;
    }
  }

  return RNA_enum_get(op->ptr, "mode");
}

static int transformops_data(bContext *C, wmOperator *op, const wmEvent *event)
{
  int retval = 1;
  if (op->customdata == nullptr) {
    TransInfo *t = MEM_callocN<TransInfo>("TransInfo data2");

    t->undo_name = op->type->name;

    int mode = transformops_mode(op);
    retval = initTransform(C, t, op, event, mode);

    /* Store data. */
    if (retval) {
      G.moving = special_transform_moving(t);
      op->customdata = t;
    }
    else {
      MEM_freeN(t);
    }
  }

  return retval; /* Return 0 on error. */
}

static wmOperatorStatus transform_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  wmOperatorStatus exit_code = OPERATOR_PASS_THROUGH;

  TransInfo *t = static_cast<TransInfo *>(op->customdata);
  const eTfmMode mode_prev = t->mode;

#if defined(WITH_INPUT_NDOF) && 0
  /* Stable 2D mouse coords map to different 3D coords while the 3D mouse is active
   * in other words, 2D deltas are no longer good enough!
   * disable until individual 'transformers' behave better. */

  if (event->type == NDOF_MOTION) {
    return OPERATOR_PASS_THROUGH;
  }
#endif

  /* XXX insert keys are called here, and require context. */
  t->context = C;

  exit_code = transformEvent(t, op, event);
  t->context = nullptr;

  /* Allow navigation while transforming. */
  if (t->vod && (exit_code & OPERATOR_PASS_THROUGH)) {
    RegionView3D *rv3d = static_cast<RegionView3D *>(t->region->regiondata);
    const bool is_navigating = (rv3d->rflag & RV3D_NAVIGATING) != 0;
    if (ED_view3d_navigation_do(C, t->vod, event, t->center_global)) {
      if (!is_navigating) {
        /* Navigation has started. */

        if (t->modifiers & MOD_PRECISION) {
          /* WORKAROUND: Remove precision modification, it may have be unintentionally enabled. */
          t->modifiers &= ~MOD_PRECISION;
          t->mouse.precision = false;
          transform_input_virtual_mval_reset(t);
        }
      }

      if (rv3d->rflag & RV3D_NAVIGATING) {
        /* Navigation is running. */

        /* Do not update transform while navigating. This can be distracting. */
        return OPERATOR_RUNNING_MODAL;
      }

      {
        /* Navigation has ended. */

        /* Call before #applyMouseInput. */
        transformViewUpdate(t);

        /* Mouse input is outdated. */
        t->mval = float2(event->mval);
        applyMouseInput(t, &t->mouse, t->mval, t->values);
        t->redraw |= TREDRAW_HARD;
      }
    }
  }

  transformApply(C, t);

  exit_code |= transformEnd(C, t);

  if ((exit_code & OPERATOR_RUNNING_MODAL) == 0) {
    transformops_exit(C, op);
    exit_code &= ~OPERATOR_PASS_THROUGH; /* Preventively remove pass-through. */
  }
  else {
    if (mode_prev != t->mode) {
      /* WARNING: this is not normal to switch operator types
       * normally it would not be supported but transform happens
       * to share callbacks between different operators. */
      wmOperatorType *ot_new = nullptr;
      TransformModeItem *item = transform_modes;
      while (item->idname) {
        if (item->mode == t->mode) {
          ot_new = WM_operatortype_find(item->idname, false);
          break;
        }
        item++;
      }

      BLI_assert(ot_new != nullptr);
      if (ot_new) {
        WM_operator_type_set(op, ot_new);
      }
      /* End suspicious code. */
    }
  }

  return exit_code;
}

static void transform_cancel(bContext *C, wmOperator *op)
{
  TransInfo *t = static_cast<TransInfo *>(op->customdata);

  t->state = TRANS_CANCEL;
  transformEnd(C, t);
  transformops_exit(C, op);
}

static wmOperatorStatus transform_exec(bContext *C, wmOperator *op)
{
  TransInfo *t;

  if (!transformops_data(C, op, nullptr)) {
    G.moving = 0;
    return OPERATOR_CANCELLED;
  }

  t = static_cast<TransInfo *>(op->customdata);

  t->options |= CTX_AUTOCONFIRM;

  transformApply(C, t);

  transformEnd(C, t);

  transformops_exit(C, op);

  WM_event_add_notifier(C, NC_OBJECT | ND_TRANSFORM, nullptr);

  return OPERATOR_FINISHED;
}

static wmOperatorStatus transform_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  if (!transformops_data(C, op, event)) {
    G.moving = 0;
    return OPERATOR_CANCELLED;
  }

  /* When modal, allow 'value' to set initial offset. */
  if ((event == nullptr) && RNA_struct_property_is_set(op->ptr, "value")) {
    return transform_exec(C, op);
  }

  /* Add temp handler. */
  WM_event_add_modal_handler(C, op);

  /* Use when modal input has some transformation to begin with. */
  TransInfo *t = static_cast<TransInfo *>(op->customdata);
  if ((t->flag & T_NO_CURSOR_WRAP) == 0) {
    op->flag |= OP_IS_MODAL_GRAB_CURSOR; /* XXX maybe we want this with the gizmo only? */
  }
  if (UNLIKELY(!is_zero_v4(t->values_modal_offset))) {
    transformApply(C, t);
  }

  return OPERATOR_RUNNING_MODAL;
}

static bool transform_poll_property(const bContext *C, wmOperator *op, const PropertyRNA *prop)
{
  const char *prop_id = RNA_property_identifier(prop);

  /* Orientation/Constraints. */
  if (STRPREFIX(prop_id, "constraint")) {
    /* Hide orientation axis if no constraints are set, since it won't be used. */
    PropertyRNA *prop_con = RNA_struct_find_property(op->ptr, "orient_type");
    if (!ELEM(prop_con, nullptr, prop)) {

      /* Special case: show constraint axis if we don't have values,
       * needed for mirror operator. */
      if (STREQ(prop_id, "constraint_axis") &&
          (RNA_struct_find_property(op->ptr, "value") == nullptr))
      {
        return true;
      }

      return false;
    }
    return true;
  }

  /* Orientation Axis. */
  if (STREQ(prop_id, "orient_axis")) {
    eTfmMode mode = (eTfmMode)transformops_mode(op);
    return mode != TFM_ALIGN;
  }

  /* Proportional Editing. */
  if (STRPREFIX(prop_id, "proportional") || STRPREFIX(prop_id, "use_proportional")) {
    ScrArea *area = CTX_wm_area(C);
    if (area->spacetype == SPACE_NLA) {
      /* Hide properties that are not supported in some spaces. */
      return false;
    }

    PropertyRNA *prop_pet = RNA_struct_find_property(op->ptr, "use_proportional_edit");
    if ((prop_pet != prop) && (RNA_property_boolean_get(op->ptr, prop_pet) == false)) {
      /* If "use_proportional_edit" is false, hide:
       * - "proportional_edit_falloff",
       * - "proportional_size",
       * - "use_proportional_connected",
       * - "use_proportional_projected". */
      return false;
    }
    return true;
  }

  /* Snapping. */
  if (STREQ(prop_id, "use_snap_project")) {
    return RNA_boolean_get(op->ptr, "snap");
  }

  if (STREQ(prop_id, "use_even_offset")) {
    /* Even offset isn't meaningful for individual faces. */
    if (op->opm && STREQ(op->opm->idname, "MESH_OT_extrude_faces_move")) {
      return false;
    }
    return true;
  }

  /* #P_CORRECT_UV. */
  if (STREQ(prop_id, "correct_uv")) {
    ScrArea *area = CTX_wm_area(C);
    return area->spacetype == SPACE_VIEW3D;
  }

  return true;
}

void properties_register(wmOperatorType *ot, int flags)
{
  PropertyRNA *prop;

  if (flags & P_ORIENT_AXIS) {
    prop = RNA_def_property(ot->srna, "orient_axis", PROP_ENUM, PROP_NONE);
    RNA_def_property_ui_text(prop, "Axis", "");
    RNA_def_property_enum_default(prop, 2);
    RNA_def_property_enum_items(prop, rna_enum_axis_xyz_items);
    RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  }
  if (flags & P_ORIENT_AXIS_ORTHO) {
    prop = RNA_def_property(ot->srna, "orient_axis_ortho", PROP_ENUM, PROP_NONE);
    RNA_def_property_ui_text(prop, "Axis Ortho", "");
    RNA_def_property_enum_default(prop, 0);
    RNA_def_property_enum_items(prop, rna_enum_axis_xyz_items);
    RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  }

  if (flags & P_ORIENT_MATRIX) {
    prop = RNA_def_property(ot->srna, "orient_type", PROP_ENUM, PROP_NONE);
    RNA_def_property_ui_text(prop, "Orientation", "Transformation orientation");
    RNA_def_enum_funcs(prop, rna_TransformOrientation_itemf);

    /* Set by 'orient_type' or gizmo which acts on non-standard orientation. */
    prop = RNA_def_float_matrix(
        ot->srna, "orient_matrix", 3, 3, nullptr, 0.0f, 0.0f, "Matrix", "", 0.0f, 0.0f);
    RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);

    /* Only use 'orient_matrix' when 'orient_matrix_type == orient_type',
     * this allows us to reuse the orientation set by a gizmo for eg, without disabling the ability
     * to switch over to other orientations. */
    prop = RNA_def_property(ot->srna, "orient_matrix_type", PROP_ENUM, PROP_NONE);
    RNA_def_property_ui_text(prop, "Matrix Orientation", "");
    RNA_def_enum_funcs(prop, rna_TransformOrientation_itemf);
    RNA_def_property_flag(prop, PROP_HIDDEN);
  }

  if (flags & P_CONSTRAINT) {
    RNA_def_boolean_vector(ot->srna, "constraint_axis", 3, nullptr, "Constraint Axis", "");
  }

  if (flags & P_MIRROR) {
    prop = RNA_def_boolean(ot->srna, "mirror", false, "Mirror Editing", "");
    if ((flags & P_MIRROR_DUMMY) == P_MIRROR_DUMMY) {
      /* Only used so macros can disable this option. */
      RNA_def_property_flag(prop, PROP_HIDDEN);
    }
  }

  if (flags & P_PROPORTIONAL) {
    RNA_def_boolean(ot->srna, "use_proportional_edit", false, "Proportional Editing", "");
    prop = RNA_def_enum(ot->srna,
                        "proportional_edit_falloff",
                        rna_enum_proportional_falloff_items,
                        0,
                        "Proportional Falloff",
                        "Falloff type for proportional editing mode");
    /* Abusing id_curve :/ */
    RNA_def_property_translation_context(prop, BLT_I18NCONTEXT_ID_CURVE_LEGACY);
    RNA_def_float(ot->srna,
                  "proportional_size",
                  1,
                  T_PROP_SIZE_MIN,
                  T_PROP_SIZE_MAX,
                  "Proportional Size",
                  "",
                  0.001f,
                  100.0f);

    RNA_def_boolean(ot->srna, "use_proportional_connected", false, "Connected", "");
    RNA_def_boolean(ot->srna, "use_proportional_projected", false, "Projected (2D)", "");
  }

  if (flags & P_SNAP) {
    prop = RNA_def_boolean(ot->srna, "snap", false, "Use Snapping Options", "");
    RNA_def_property_flag(prop, PROP_HIDDEN);

    if ((flags & P_GEO_SNAP) == P_GEO_SNAP) {
      prop = RNA_def_enum_flag(ot->srna,
                               "snap_elements",
                               rna_enum_snap_element_items,
                               SCE_SNAP_TO_INCREMENT,
                               "Snap to Elements",
                               "");
      RNA_def_property_flag(prop, PROP_HIDDEN);

      RNA_def_boolean(ot->srna, "use_snap_project", false, "Project Individual Elements", "");

      /* TODO(@gfxcoder): Rename `snap_target` to `snap_base` to avoid previous ambiguity of
       * "target" (now, "base" or "source" is geometry to be moved and "target" is geometry to
       * which moved geometry is snapped). */
      prop = RNA_def_enum(ot->srna,
                          "snap_target",
                          rna_enum_snap_source_items,
                          0,
                          "Snap Base",
                          "Point on source that will snap to target");
      RNA_def_property_flag(prop, PROP_HIDDEN);

      /* Target selection. */
      prop = RNA_def_boolean(ot->srna, "use_snap_self", true, "Target: Include Active", "");
      RNA_def_property_flag(prop, PROP_HIDDEN);
      prop = RNA_def_boolean(ot->srna, "use_snap_edit", true, "Target: Include Edit", "");
      RNA_def_property_flag(prop, PROP_HIDDEN);
      prop = RNA_def_boolean(ot->srna, "use_snap_nonedit", true, "Target: Include Non-Edited", "");
      RNA_def_property_flag(prop, PROP_HIDDEN);
      prop = RNA_def_boolean(
          ot->srna, "use_snap_selectable", false, "Target: Exclude Non-Selectable", "");
      RNA_def_property_flag(prop, PROP_HIDDEN);

      prop = RNA_def_float_vector(
          ot->srna, "snap_point", 3, nullptr, -FLT_MAX, FLT_MAX, "Point", "", -FLT_MAX, FLT_MAX);
      RNA_def_property_flag(prop, PROP_HIDDEN);

      if ((flags & P_ALIGN_SNAP) == P_ALIGN_SNAP) {
        prop = RNA_def_boolean(ot->srna, "snap_align", false, "Align with Point Normal", "");
        RNA_def_property_flag(prop, PROP_HIDDEN);
        prop = RNA_def_float_vector(ot->srna,
                                    "snap_normal",
                                    3,
                                    nullptr,
                                    -FLT_MAX,
                                    FLT_MAX,
                                    "Normal",
                                    "",
                                    -FLT_MAX,
                                    FLT_MAX);
        RNA_def_property_flag(prop, PROP_HIDDEN);
      }
    }
  }

  if (flags & P_GPENCIL_EDIT) {
    prop = RNA_def_boolean(ot->srna,
                           "gpencil_strokes",
                           false,
                           "Edit Grease Pencil",
                           "Edit selected Grease Pencil strokes");
    RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
  }

  if (flags & P_CURSOR_EDIT) {
    prop = RNA_def_boolean(ot->srna, "cursor_transform", false, "Transform Cursor", "");
    RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
  }

  if ((flags & P_OPTIONS) && !(flags & P_NO_TEXSPACE)) {
    prop = RNA_def_boolean(
        ot->srna, "texture_space", false, "Edit Texture Space", "Edit object data texture space");
    RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
    prop = RNA_def_boolean(
        ot->srna, "remove_on_cancel", false, "Remove on Cancel", "Remove elements on cancel");
    RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
    prop = RNA_def_boolean(ot->srna,
                           "use_duplicated_keyframes",
                           false,
                           "Duplicated Keyframes",
                           "Transform duplicated keyframes");
    RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
  }

  if (flags & P_CORRECT_UV) {
    RNA_def_boolean(
        ot->srna, "correct_uv", true, "Correct UVs", "Correct UV coordinates when transforming");
  }

  if (flags & P_CENTER) {
    /* For gizmos that define their own center. */
    prop = RNA_def_property(ot->srna, "center_override", PROP_FLOAT, PROP_XYZ);
    RNA_def_property_array(prop, 3);
    RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
    RNA_def_property_ui_text(prop, "Center Override", "Force using this center value (when set)");
  }

  if (flags & P_VIEW2D_EDGE_PAN) {
    prop = RNA_def_boolean(
        ot->srna, "view2d_edge_pan", false, "Edge Pan", "Enable edge panning in 2D view");
    RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
  }

  if ((flags & P_NO_DEFAULTS) == 0) {
    prop = RNA_def_boolean(ot->srna,
                           "release_confirm",
                           false,
                           "Confirm on Release",
                           "Always confirm operation when releasing button");
    RNA_def_property_flag(prop, PROP_HIDDEN);

    prop = RNA_def_boolean(
        ot->srna, "use_accurate", false, "Accurate", "Use accurate transformation");
    RNA_def_property_flag(prop, PROP_HIDDEN);
  }

  if (flags & P_POST_TRANSFORM) {
    prop = RNA_def_boolean(ot->srna,
                           "use_automerge_and_split",
                           false,
                           "Auto Merge & Split",
                           "Forces the use of Auto Merge and Split");
    RNA_def_property_flag(prop, PROP_HIDDEN);
  }

  if (flags & P_TRANSLATE_ORIGIN) {
    prop = RNA_def_boolean(ot->srna,
                           "translate_origin",
                           false,
                           "Translate Origin",
                           "Translate origin instead of selection");
    RNA_def_property_flag(prop, PROP_HIDDEN);
  }
}

static void TRANSFORM_OT_translate(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Move";
  ot->description = "Move selected items";
  ot->idname = OP_TRANSLATION;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING;

  /* API callbacks. */
  ot->invoke = transform_invoke;
  ot->exec = transform_exec;
  ot->modal = transform_modal;
  ot->cancel = transform_cancel;
  ot->poll = ED_operator_screenactive;
  ot->poll_property = transform_poll_property;

  RNA_def_float_translation(
      ot->srna, "value", 3, nullptr, -FLT_MAX, FLT_MAX, "Move", "", -FLT_MAX, FLT_MAX);

  WM_operatortype_props_advanced_begin(ot);

  properties_register(ot,
                      P_ORIENT_MATRIX | P_CONSTRAINT | P_PROPORTIONAL | P_MIRROR | P_ALIGN_SNAP |
                          P_OPTIONS | P_GPENCIL_EDIT | P_CURSOR_EDIT | P_VIEW2D_EDGE_PAN |
                          P_POST_TRANSFORM | P_TRANSLATE_ORIGIN);
}

static void TRANSFORM_OT_resize(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Resize";
  ot->description = "Scale (resize) selected items";
  ot->idname = OP_RESIZE;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING;

  /* API callbacks. */
  ot->invoke = transform_invoke;
  ot->exec = transform_exec;
  ot->modal = transform_modal;
  ot->cancel = transform_cancel;
  ot->poll = ED_operator_screenactive;
  ot->poll_property = transform_poll_property;

  RNA_def_float_vector(
      ot->srna, "value", 3, VecOne, -FLT_MAX, FLT_MAX, "Scale", "", -FLT_MAX, FLT_MAX);

  PropertyRNA *prop;
  prop = RNA_def_float_vector(ot->srna,
                              "mouse_dir_constraint",
                              3,
                              VecZero,
                              -FLT_MAX,
                              FLT_MAX,
                              "Mouse Directional Constraint",
                              "",
                              -FLT_MAX,
                              FLT_MAX);
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);

  WM_operatortype_props_advanced_begin(ot);

  properties_register(ot,
                      P_ORIENT_MATRIX | P_CONSTRAINT | P_PROPORTIONAL | P_MIRROR | P_GEO_SNAP |
                          P_OPTIONS | P_GPENCIL_EDIT | P_CENTER);
}

static void TRANSFORM_OT_skin_resize(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Skin Resize";
  ot->description = "Scale selected vertices' skin radii";
  ot->idname = OP_SKIN_RESIZE;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING;

  /* API callbacks. */
  ot->invoke = transform_invoke;
  ot->exec = transform_exec;
  ot->modal = transform_modal;
  ot->cancel = transform_cancel;
  ot->poll = ED_operator_editmesh;
  ot->poll_property = transform_poll_property;

  RNA_def_float_vector(
      ot->srna, "value", 3, VecOne, -FLT_MAX, FLT_MAX, "Scale", "", -FLT_MAX, FLT_MAX);

  WM_operatortype_props_advanced_begin(ot);

  properties_register(ot,
                      P_ORIENT_MATRIX | P_CONSTRAINT | P_PROPORTIONAL | P_MIRROR | P_GEO_SNAP |
                          P_OPTIONS | P_NO_TEXSPACE);
}

static void TRANSFORM_OT_trackball(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Trackball";
  ot->description = "Trackball style rotation of selected items";
  ot->idname = OP_TRACKBALL;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING;

  /* API callbacks. */
  ot->invoke = transform_invoke;
  ot->exec = transform_exec;
  ot->modal = transform_modal;
  ot->cancel = transform_cancel;
  ot->poll = ED_operator_screenactive;
  ot->poll_property = transform_poll_property;

  /* Maybe we could use float_vector_xyz here too? */
  RNA_def_float_rotation(
      ot->srna, "value", 2, nullptr, -FLT_MAX, FLT_MAX, "Angle", "", -FLT_MAX, FLT_MAX);

  WM_operatortype_props_advanced_begin(ot);

  properties_register(ot, P_PROPORTIONAL | P_MIRROR | P_SNAP | P_GPENCIL_EDIT | P_CENTER);
}

static void TRANSFORM_OT_rotate(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Rotate";
  ot->description = "Rotate selected items";
  ot->idname = OP_ROTATION;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING;

  /* API callbacks. */
  ot->invoke = transform_invoke;
  ot->exec = transform_exec;
  ot->modal = transform_modal;
  ot->cancel = transform_cancel;
  ot->poll = ED_operator_screenactive;
  ot->poll_property = transform_poll_property;

  RNA_def_float_rotation(
      ot->srna, "value", 0, nullptr, -FLT_MAX, FLT_MAX, "Angle", "", -M_PI * 2, M_PI * 2);

  WM_operatortype_props_advanced_begin(ot);

  properties_register(ot,
                      P_ORIENT_AXIS | P_ORIENT_MATRIX | P_CONSTRAINT | P_PROPORTIONAL | P_MIRROR |
                          P_GEO_SNAP | P_GPENCIL_EDIT | P_CENTER);
}

static bool tilt_poll(bContext *C)
{
  Object *obedit = CTX_data_edit_object(C);
  if (!obedit) {
    return false;
  }
  if (obedit->type == OB_CURVES_LEGACY) {
    Curve *cu = (Curve *)obedit->data;
    return (cu->flag & CU_3D) && (nullptr != cu->editnurb);
  }
  if (obedit->type == OB_CURVES) {
    return true;
  }
  return true;
}

static void TRANSFORM_OT_tilt(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Tilt";
  /* Optional -
   * "Tilt selected vertices"
   * "Specify an extra axis rotation for selected vertices of 3D curve". */
  ot->description = "Tilt selected control vertices of 3D curve";
  ot->idname = OP_TILT;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING;

  /* API callbacks. */
  ot->invoke = transform_invoke;
  ot->exec = transform_exec;
  ot->modal = transform_modal;
  ot->cancel = transform_cancel;
  ot->poll = tilt_poll;
  ot->poll_property = transform_poll_property;

  RNA_def_float_rotation(
      ot->srna, "value", 0, nullptr, -FLT_MAX, FLT_MAX, "Angle", "", -M_PI * 2, M_PI * 2);

  WM_operatortype_props_advanced_begin(ot);

  properties_register(ot, P_PROPORTIONAL | P_MIRROR | P_SNAP);
}

static void TRANSFORM_OT_bend(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Bend";
  ot->description = "Bend selected items between the 3D cursor and the mouse";
  ot->idname = OP_BEND;
  /* Depend on cursor location because the cursor location is used to define the region to bend. */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING | OPTYPE_DEPENDS_ON_CURSOR;

  /* API callbacks. */
  ot->invoke = transform_invoke;
  // ot->exec = transform_exec; /* Unsupported. */
  ot->modal = transform_modal;
  ot->cancel = transform_cancel;
  ot->poll = ED_operator_region_view3d_active;
  ot->poll_property = transform_poll_property;

  RNA_def_float_rotation(
      ot->srna, "value", 1, nullptr, -FLT_MAX, FLT_MAX, "Angle", "", -M_PI * 2, M_PI * 2);

  WM_operatortype_props_advanced_begin(ot);

  properties_register(ot, P_PROPORTIONAL | P_MIRROR | P_SNAP | P_GPENCIL_EDIT | P_CENTER);
}

static bool transform_shear_poll(bContext *C)
{
  if (!ED_operator_screenactive(C)) {
    return false;
  }

  ScrArea *area = CTX_wm_area(C);
  return area && !ELEM(area->spacetype, SPACE_ACTION);
}

static void TRANSFORM_OT_shear(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Shear";
  ot->description = "Shear selected items along the given axis";
  ot->idname = OP_SHEAR;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING;

  /* API callbacks. */
  ot->invoke = transform_invoke;
  ot->exec = transform_exec;
  ot->modal = transform_modal;
  ot->cancel = transform_cancel;
  ot->poll = transform_shear_poll;
  ot->poll_property = transform_poll_property;

  RNA_def_float(ot->srna, "value", 0, -FLT_MAX, FLT_MAX, "Offset", "", -FLT_MAX, FLT_MAX);

  WM_operatortype_props_advanced_begin(ot);

  properties_register(ot,
                      P_ORIENT_AXIS | P_ORIENT_AXIS_ORTHO | P_ORIENT_MATRIX | P_PROPORTIONAL |
                          P_MIRROR | P_SNAP | P_GPENCIL_EDIT);
}

static void TRANSFORM_OT_push_pull(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Push/Pull";
  ot->description = "Push/Pull selected items";
  ot->idname = OP_PUSH_PULL;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING;

  /* API callbacks. */
  ot->invoke = transform_invoke;
  ot->exec = transform_exec;
  ot->modal = transform_modal;
  ot->cancel = transform_cancel;
  ot->poll = ED_operator_screenactive;
  ot->poll_property = transform_poll_property;

  RNA_def_float(ot->srna, "value", 0, -FLT_MAX, FLT_MAX, "Distance", "", -FLT_MAX, FLT_MAX);

  WM_operatortype_props_advanced_begin(ot);

  properties_register(ot, P_PROPORTIONAL | P_MIRROR | P_SNAP | P_CENTER);
}

static void TRANSFORM_OT_shrink_fatten(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Shrink/Fatten";
  ot->description = "Shrink/fatten selected vertices along normals";
  ot->idname = OP_SHRINK_FATTEN;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING;

  /* API callbacks. */
  ot->invoke = transform_invoke;
  ot->exec = transform_exec;
  ot->modal = transform_modal;
  ot->cancel = transform_cancel;
  ot->poll = ED_operator_editmesh;
  ot->poll_property = transform_poll_property;

  RNA_def_float_distance(ot->srna, "value", 0, -FLT_MAX, FLT_MAX, "Offset", "", -FLT_MAX, FLT_MAX);

  RNA_def_boolean(ot->srna,
                  "use_even_offset",
                  false,
                  "Offset Even",
                  "Scale the offset to give more even thickness");

  WM_operatortype_props_advanced_begin(ot);

  properties_register(ot, P_PROPORTIONAL | P_MIRROR | P_SNAP);
}

static void TRANSFORM_OT_tosphere(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "To Sphere";
  ot->description = "Move selected items outward in a spherical shape around geometric center";
  ot->idname = OP_TOSPHERE;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING;

  /* API callbacks. */
  ot->invoke = transform_invoke;
  ot->exec = transform_exec;
  ot->modal = transform_modal;
  ot->cancel = transform_cancel;
  ot->poll = ED_operator_screenactive;
  ot->poll_property = transform_poll_property;

  RNA_def_float_factor(ot->srna, "value", 0, 0, 1, "Factor", "", 0, 1);

  WM_operatortype_props_advanced_begin(ot);

  properties_register(ot, P_PROPORTIONAL | P_MIRROR | P_SNAP | P_GPENCIL_EDIT | P_CENTER);
}

static void TRANSFORM_OT_mirror(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Mirror";
  ot->description = "Mirror selected items around one or more axes";
  ot->idname = OP_MIRROR;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING;

  /* API callbacks. */
  ot->invoke = transform_invoke;
  ot->exec = transform_exec;
  ot->modal = transform_modal;
  ot->cancel = transform_cancel;
  ot->poll = ED_operator_screenactive;
  ot->poll_property = transform_poll_property;

  properties_register(ot, P_ORIENT_MATRIX | P_CONSTRAINT | P_GPENCIL_EDIT | P_CENTER);
}

static void TRANSFORM_OT_bbone_resize(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Scale B-Bone";
  ot->description = "Scale selected bendy bones display size";
  ot->idname = OP_BONE_SIZE;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING;

  /* API callbacks. */
  ot->invoke = transform_invoke;
  ot->exec = transform_exec;
  ot->modal = transform_modal;
  ot->cancel = transform_cancel;
  ot->poll = ED_operator_object_active;
  ot->poll_property = transform_poll_property;

  RNA_def_float_translation(
      ot->srna, "value", 3, VecOne, -FLT_MAX, FLT_MAX, "Display Size", "", -FLT_MAX, FLT_MAX);

  WM_operatortype_props_advanced_begin(ot);

  properties_register(ot, P_ORIENT_MATRIX | P_CONSTRAINT | P_MIRROR);
}

static void TRANSFORM_OT_edge_slide(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* Identifiers. */
  ot->name = "Edge Slide";
  ot->description = "Slide an edge loop along a mesh";
  ot->idname = OP_EDGE_SLIDE;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING;

  /* API callbacks. */
  ot->invoke = transform_invoke;
  ot->exec = transform_exec;
  ot->modal = transform_modal;
  ot->cancel = transform_cancel;
  ot->poll = ED_operator_editmesh;
  ot->poll_property = transform_poll_property;

  RNA_def_float_factor(ot->srna, "value", 0, -10.0f, 10.0f, "Factor", "", -1.0f, 1.0f);

  prop = RNA_def_boolean(ot->srna, "single_side", false, "Single Side", "");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
  RNA_def_boolean(ot->srna,
                  "use_even",
                  false,
                  "Even",
                  "Make the edge loop match the shape of the adjacent edge loop");

  WM_operatortype_props_advanced_begin(ot);

  RNA_def_boolean(ot->srna,
                  "flipped",
                  false,
                  "Flipped",
                  "When Even mode is active, flips between the two adjacent edge loops");
  RNA_def_boolean(ot->srna, "use_clamp", true, "Clamp", "Clamp within the edge extents");

  properties_register(ot, P_MIRROR | P_GEO_SNAP | P_CORRECT_UV);
}

static void TRANSFORM_OT_vert_slide(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Vertex Slide";
  ot->description = "Slide a vertex along a mesh";
  ot->idname = OP_VERT_SLIDE;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING | OPTYPE_DEPENDS_ON_CURSOR;

  /* API callbacks. */
  ot->invoke = transform_invoke;
  ot->exec = transform_exec;
  ot->modal = transform_modal;
  ot->cancel = transform_cancel;
  ot->poll = ED_operator_editmesh;
  ot->poll_property = transform_poll_property;

  RNA_def_float_factor(ot->srna, "value", 0, -10.0f, 10.0f, "Factor", "", -1.0f, 1.0f);
  RNA_def_boolean(ot->srna,
                  "use_even",
                  false,
                  "Even",
                  "Make the edge loop match the shape of the adjacent edge loop");

  WM_operatortype_props_advanced_begin(ot);

  RNA_def_boolean(ot->srna,
                  "flipped",
                  false,
                  "Flipped",
                  "When Even mode is active, flips between the two adjacent edge loops");
  RNA_def_boolean(ot->srna, "use_clamp", true, "Clamp", "Clamp within the edge extents");

  properties_register(ot, P_MIRROR | P_GEO_SNAP | P_CORRECT_UV);
}

static void TRANSFORM_OT_edge_crease(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Edge Crease";
  ot->description = "Change the crease of edges";
  ot->idname = OP_EDGE_CREASE;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING;

  /* API callbacks. */
  ot->invoke = transform_invoke;
  ot->exec = transform_exec;
  ot->modal = transform_modal;
  ot->cancel = transform_cancel;
  ot->poll = ED_operator_editmesh;
  ot->poll_property = transform_poll_property;

  RNA_def_float_factor(ot->srna, "value", 0, -1.0f, 1.0f, "Factor", "", -1.0f, 1.0f);

  WM_operatortype_props_advanced_begin(ot);

  properties_register(ot, P_SNAP);
}

static void TRANSFORM_OT_vert_crease(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Vertex Crease";
  ot->description = "Change the crease of vertices";
  ot->idname = OP_VERT_CREASE;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING;

  /* API callbacks. */
  ot->invoke = transform_invoke;
  ot->exec = transform_exec;
  ot->modal = transform_modal;
  ot->cancel = transform_cancel;
  ot->poll = ED_operator_editmesh;
  ot->poll_property = transform_poll_property;

  RNA_def_float_factor(ot->srna, "value", 0, -1.0f, 1.0f, "Factor", "", -1.0f, 1.0f);

  WM_operatortype_props_advanced_begin(ot);

  properties_register(ot, P_SNAP);
}

static void TRANSFORM_OT_edge_bevelweight(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Edge Bevel Weight";
  ot->description = "Change the bevel weight of edges";
  ot->idname = OP_EDGE_BWEIGHT;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING;

  /* API callbacks. */
  ot->invoke = transform_invoke;
  ot->exec = transform_exec;
  ot->modal = transform_modal;
  ot->cancel = transform_cancel;
  ot->poll = ED_operator_editmesh;

  RNA_def_float_factor(ot->srna, "value", 0, -1.0f, 1.0f, "Factor", "", -1.0f, 1.0f);

  WM_operatortype_props_advanced_begin(ot);

  properties_register(ot, P_SNAP);
}

static void TRANSFORM_OT_seq_slide(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Sequence Slide";
  ot->description = "Slide a sequence strip in time";
  ot->idname = OP_SEQ_SLIDE;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING;

  /* API callbacks. */
  ot->invoke = transform_invoke;
  ot->exec = transform_exec;
  ot->modal = transform_modal;
  ot->cancel = transform_cancel;
  ot->poll = ED_operator_sequencer_active;

  /* Properties. */
  PropertyRNA *prop;

  prop = RNA_def_float_vector(
      ot->srna, "value", 2, nullptr, -FLT_MAX, FLT_MAX, "Offset", "", -FLT_MAX, FLT_MAX);
  RNA_def_property_ui_range(prop, -FLT_MAX, FLT_MAX, 1, 0);

  prop = RNA_def_boolean(ot->srna,
                         "use_restore_handle_selection",
                         false,
                         "Restore Handle Selection",
                         "Restore handle selection after tweaking");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);

  WM_operatortype_props_advanced_begin(ot);

  properties_register(ot, P_OPTIONS | P_SNAP | P_VIEW2D_EDGE_PAN);
}

static void TRANSFORM_OT_rotate_normal(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Rotate Normals";
  ot->description = "Rotate custom normal of selected items";
  ot->idname = OP_NORMAL_ROTATION;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING;

  /* API callbacks. */
  ot->invoke = transform_invoke;
  ot->exec = transform_exec;
  ot->modal = transform_modal;
  ot->cancel = transform_cancel;
  ot->poll = ED_operator_editmesh;
  ot->poll_property = transform_poll_property;

  RNA_def_float_rotation(
      ot->srna, "value", 0, nullptr, -FLT_MAX, FLT_MAX, "Angle", "", -M_PI * 2, M_PI * 2);

  properties_register(ot, P_ORIENT_AXIS | P_ORIENT_MATRIX | P_CONSTRAINT | P_MIRROR);
}

static void TRANSFORM_OT_transform(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* Identifiers. */
  ot->name = "Transform";
  ot->description = "Transform selected items by mode type";
  ot->idname = "TRANSFORM_OT_transform";
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING;

  /* API callbacks. */
  ot->invoke = transform_invoke;
  ot->exec = transform_exec;
  ot->modal = transform_modal;
  ot->cancel = transform_cancel;
  ot->poll = ED_operator_screenactive;
  ot->poll_property = transform_poll_property;

  prop = RNA_def_enum(
      ot->srna, "mode", rna_enum_transform_mode_type_items, TFM_TRANSLATION, "Mode", "");
  RNA_def_property_flag(prop, PROP_HIDDEN);

  RNA_def_float_vector(
      ot->srna, "value", 4, nullptr, -FLT_MAX, FLT_MAX, "Values", "", -FLT_MAX, FLT_MAX);

  WM_operatortype_props_advanced_begin(ot);

  properties_register(ot,
                      P_ORIENT_AXIS | P_ORIENT_MATRIX | P_CONSTRAINT | P_PROPORTIONAL | P_MIRROR |
                          P_ALIGN_SNAP | P_GPENCIL_EDIT | P_CENTER | P_POST_TRANSFORM | P_OPTIONS);
}

static wmOperatorStatus transform_from_gizmo_invoke(bContext *C,
                                                    wmOperator * /*op*/,
                                                    const wmEvent *event)
{
  bToolRef *tref = WM_toolsystem_ref_from_context(C);
  if (tref) {
    ARegion *region = CTX_wm_region(C);
    wmGizmoMap *gzmap = region->runtime->gizmo_map;
    wmGizmoGroup *gzgroup = gzmap ? WM_gizmomap_group_find(gzmap, "VIEW3D_GGT_xform_gizmo") :
                                    nullptr;
    if (gzgroup != nullptr) {
      PointerRNA gzg_ptr;
      WM_toolsystem_ref_properties_ensure_from_gizmo_group(tref, gzgroup->type, &gzg_ptr);
      const int drag_action = RNA_enum_get(&gzg_ptr, "drag_action");
      const char *op_id = nullptr;
      switch (drag_action) {
        case V3D_GIZMO_SHOW_OBJECT_TRANSLATE:
          op_id = "TRANSFORM_OT_translate";
          break;
        case V3D_GIZMO_SHOW_OBJECT_ROTATE:
          op_id = "TRANSFORM_OT_rotate";
          break;
        case V3D_GIZMO_SHOW_OBJECT_SCALE:
          op_id = "TRANSFORM_OT_resize";
          break;
        default:
          break;
      }
      if (op_id) {
        wmOperatorType *ot = WM_operatortype_find(op_id, true);
        PointerRNA op_ptr;
        WM_operator_properties_create_ptr(&op_ptr, ot);
        RNA_boolean_set(&op_ptr, "release_confirm", true);
        WM_operator_name_call_ptr(C, ot, wm::OpCallContext::InvokeDefault, &op_ptr, event);
        WM_operator_properties_free(&op_ptr);
        return OPERATOR_FINISHED;
      }
    }
  }
  return OPERATOR_PASS_THROUGH;
}

/* Use with 'TRANSFORM_GGT_gizmo'. */
static void TRANSFORM_OT_from_gizmo(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Transform from Gizmo";
  ot->description = "Transform selected items by mode type";
  ot->idname = "TRANSFORM_OT_from_gizmo";
  ot->flag = 0;

  /* API callbacks. */
  ot->poll = ED_operator_regionactive;
  ot->invoke = transform_from_gizmo_invoke;
}

void transform_operatortypes()
{
  TransformModeItem *tmode;

  for (tmode = transform_modes; tmode->idname; tmode++) {
    WM_operatortype_append(tmode->opfunc);
  }

  WM_operatortype_append(TRANSFORM_OT_transform);

  WM_operatortype_append(TRANSFORM_OT_select_orientation);
  WM_operatortype_append(TRANSFORM_OT_create_orientation);
  WM_operatortype_append(TRANSFORM_OT_delete_orientation);

  WM_operatortype_append(TRANSFORM_OT_from_gizmo);
}

void keymap_transform(wmKeyConfig *keyconf)
{
  wmKeyMap *modalmap = transform_modal_keymap(keyconf);

  TransformModeItem *tmode;

  for (tmode = transform_modes; tmode->idname; tmode++) {
    WM_modalkeymap_assign(modalmap, tmode->idname);
  }
  WM_modalkeymap_assign(modalmap, "TRANSFORM_OT_transform");
}

}  // namespace blender::ed::transform
