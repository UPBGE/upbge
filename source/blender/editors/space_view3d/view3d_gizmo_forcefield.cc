/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spview3d
 */

#include "BLI_listbase.h"

#include "BKE_context.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"

#include "DNA_object_force_types.h"
#include "DNA_object_types.h"

#include "ED_gizmo_library.hh"

#include "UI_resources.hh"

#include "MEM_guardedalloc.h"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "WM_types.hh"

#include "view3d_intern.hh" /* own include */

/* -------------------------------------------------------------------- */
/** \name Force Field Gizmos
 * \{ */

static bool WIDGETGROUP_forcefield_poll(const bContext *C, wmGizmoGroupType * /*gzgt*/)
{
  View3D *v3d = CTX_wm_view3d(C);

  if (v3d->gizmo_flag & (V3D_GIZMO_HIDE | V3D_GIZMO_HIDE_CONTEXT)) {
    return false;
  }
  if ((v3d->gizmo_show_empty & V3D_GIZMO_SHOW_EMPTY_FORCE_FIELD) == 0) {
    return false;
  }

  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  BKE_view_layer_synced_ensure(scene, view_layer);
  Base *base = BKE_view_layer_active_base_get(view_layer);
  if (base && BASE_SELECTABLE(v3d, base)) {
    const Object *ob = base->object;
    if (ob->pd && ob->pd->forcefield) {
      if (BKE_id_is_editable(CTX_data_main(C), &ob->id)) {
        return true;
      }
    }
  }
  return false;
}

static void WIDGETGROUP_forcefield_setup(const bContext * /*C*/, wmGizmoGroup *gzgroup)
{
  /* only wind effector for now */
  wmGizmoWrapper *wwrapper = MEM_mallocN<wmGizmoWrapper>(__func__);
  gzgroup->customdata = wwrapper;

  wwrapper->gizmo = WM_gizmo_new("GIZMO_GT_arrow_3d", gzgroup, nullptr);
  wmGizmo *gz = wwrapper->gizmo;
  RNA_enum_set(gz->ptr, "transform", ED_GIZMO_ARROW_XFORM_FLAG_CONSTRAINED);
  ED_gizmo_arrow3d_set_ui_range(gz, -200.0f, 200.0f);
  ED_gizmo_arrow3d_set_range_fac(gz, 6.0f);

  UI_GetThemeColor3fv(TH_GIZMO_PRIMARY, gz->color);
  UI_GetThemeColor3fv(TH_GIZMO_HI, gz->color_hi);

  /* All gizmos must perform undo. */
  LISTBASE_FOREACH (wmGizmo *, gz_iter, &gzgroup->gizmos) {
    WM_gizmo_set_flag(gz_iter, WM_GIZMO_NEEDS_UNDO, true);
  }
}

static void WIDGETGROUP_forcefield_refresh(const bContext *C, wmGizmoGroup *gzgroup)
{
  wmGizmoWrapper *wwrapper = static_cast<wmGizmoWrapper *>(gzgroup->customdata);
  wmGizmo *gz = wwrapper->gizmo;
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  BKE_view_layer_synced_ensure(scene, view_layer);
  Object *ob = BKE_view_layer_active_object_get(view_layer);
  PartDeflect *pd = ob->pd;

  if (pd->forcefield == PFIELD_WIND) {
    const float size = (ob->type == OB_EMPTY) ? ob->empty_drawsize : 1.0f;
    const float ofs[3] = {0.0f, -size, 0.0f};

    PointerRNA field_ptr = RNA_pointer_create_discrete(&ob->id, &RNA_FieldSettings, pd);
    WM_gizmo_set_matrix_location(gz, ob->object_to_world().location());
    WM_gizmo_set_matrix_rotation_from_z_axis(gz, ob->object_to_world().ptr()[2]);
    WM_gizmo_set_matrix_offset_location(gz, ofs);
    WM_gizmo_set_flag(gz, WM_GIZMO_HIDDEN, false);
    WM_gizmo_target_property_def_rna(gz, "offset", &field_ptr, "strength", -1);
  }
  else {
    WM_gizmo_set_flag(gz, WM_GIZMO_HIDDEN, true);
  }
}

void VIEW3D_GGT_force_field(wmGizmoGroupType *gzgt)
{
  gzgt->name = "Force Field Widgets";
  gzgt->idname = "VIEW3D_GGT_force_field";

  gzgt->flag |= (WM_GIZMOGROUPTYPE_PERSISTENT | WM_GIZMOGROUPTYPE_3D | WM_GIZMOGROUPTYPE_SCALE |
                 WM_GIZMOGROUPTYPE_DEPTH_3D);

  gzgt->poll = WIDGETGROUP_forcefield_poll;
  gzgt->setup = WIDGETGROUP_forcefield_setup;
  gzgt->setup_keymap = WM_gizmogroup_setup_keymap_generic_maybe_drag;
  gzgt->refresh = WIDGETGROUP_forcefield_refresh;
}

/** \} */
