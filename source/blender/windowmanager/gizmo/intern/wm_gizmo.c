/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2014 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup wm
 */

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_math.h"

#include "BKE_context.h"

#include "GPU_batch.h"

#include "RNA_access.h"
#include "RNA_define.h"
#include "RNA_prototypes.h"

#include "BKE_global.h"
#include "BKE_idprop.h"
#include "BKE_main.h"

#include "WM_api.h"
#include "WM_toolsystem.h"
#include "WM_types.h"

#include "ED_screen.h"
#include "ED_view3d.h"

#include "UI_interface.h"

#ifdef WITH_PYTHON
#  include "BPY_extern.h"
#endif

/* only for own init/exit calls (wm_gizmotype_init/wm_gizmotype_free) */
#include "wm.h"

/* own includes */
#include "wm_gizmo_intern.h"
#include "wm_gizmo_wmapi.h"

static void wm_gizmo_register(wmGizmoGroup *gzgroup, wmGizmo *gz);

/**
 * \note Follow #wm_operator_create convention.
 */
static wmGizmo *wm_gizmo_create(const wmGizmoType *gzt, PointerRNA *properties)
{
  BLI_assert(gzt != NULL);
  BLI_assert(gzt->struct_size >= sizeof(wmGizmo));

  wmGizmo *gz = MEM_callocN(
      gzt->struct_size + (sizeof(wmGizmoProperty) * gzt->target_property_defs_len), __func__);
  gz->type = gzt;

  /* initialize properties, either copy or create */
  gz->ptr = MEM_callocN(sizeof(PointerRNA), "wmGizmoPtrRNA");
  if (properties && properties->data) {
    gz->properties = IDP_CopyProperty(properties->data);
  }
  else {
    IDPropertyTemplate val = {0};
    gz->properties = IDP_New(IDP_GROUP, &val, "wmGizmoProperties");
  }
  RNA_pointer_create(G_MAIN->wm.first, gzt->srna, gz->properties, gz->ptr);

  WM_gizmo_properties_sanitize(gz->ptr, 0);

  unit_m4(gz->matrix_space);
  unit_m4(gz->matrix_basis);
  unit_m4(gz->matrix_offset);

  gz->drag_part = -1;

  return gz;
}

wmGizmo *WM_gizmo_new_ptr(const wmGizmoType *gzt, wmGizmoGroup *gzgroup, PointerRNA *properties)
{
  wmGizmo *gz = wm_gizmo_create(gzt, properties);

  wm_gizmo_register(gzgroup, gz);

  if (gz->type->setup != NULL) {
    gz->type->setup(gz);
  }

  return gz;
}

wmGizmo *WM_gizmo_new(const char *idname, wmGizmoGroup *gzgroup, PointerRNA *properties)
{
  const wmGizmoType *gzt = WM_gizmotype_find(idname, false);
  return WM_gizmo_new_ptr(gzt, gzgroup, properties);
}

/**
 * Initialize default values and allocate needed memory for members.
 */
static void gizmo_init(wmGizmo *gz)
{
  const float color_default[4] = {1.0f, 1.0f, 1.0f, 1.0f};

  gz->scale_basis = 1.0f;
  gz->line_width = 1.0f;

  /* defaults */
  copy_v4_v4(gz->color, color_default);
  copy_v4_v4(gz->color_hi, color_default);
}

/**
 * Register \a gizmo.
 *
 * \note Not to be confused with type registration from RNA.
 */
static void wm_gizmo_register(wmGizmoGroup *gzgroup, wmGizmo *gz)
{
  gizmo_init(gz);
  wm_gizmogroup_gizmo_register(gzgroup, gz);
}

void WM_gizmo_free(wmGizmo *gz)
{
  if (gz->type->free != NULL) {
    gz->type->free(gz);
  }

#ifdef WITH_PYTHON
  if (gz->py_instance) {
    /* do this first in case there are any __del__ functions or
     * similar that use properties */
    BPY_DECREF_RNA_INVALIDATE(gz->py_instance);
  }
#endif

  if (gz->op_data) {
    for (int i = 0; i < gz->op_data_len; i++) {
      WM_operator_properties_free(&gz->op_data[i].ptr);
    }
    MEM_freeN(gz->op_data);
  }

  if (gz->ptr != NULL) {
    WM_gizmo_properties_free(gz->ptr);
    MEM_freeN(gz->ptr);
  }

  if (gz->type->target_property_defs_len != 0) {
    wmGizmoProperty *gz_prop_array = WM_gizmo_target_property_array(gz);
    for (int i = 0; i < gz->type->target_property_defs_len; i++) {
      wmGizmoProperty *gz_prop = &gz_prop_array[i];
      if (gz_prop->custom_func.free_fn) {
        gz_prop->custom_func.free_fn(gz, gz_prop);
      }
    }
  }

  MEM_freeN(gz);
}

void WM_gizmo_unlink(ListBase *gizmolist, wmGizmoMap *gzmap, wmGizmo *gz, bContext *C)
{
  if (gz->state & WM_GIZMO_STATE_HIGHLIGHT) {
    wm_gizmomap_highlight_set(gzmap, C, NULL, 0);
  }
  if (gz->state & WM_GIZMO_STATE_MODAL) {
    wm_gizmomap_modal_set(gzmap, C, gz, NULL, false);
  }
  /* Unlink instead of setting so we don't run callbacks. */
  if (gz->state & WM_GIZMO_STATE_SELECT) {
    WM_gizmo_select_unlink(gzmap, gz);
  }

  if (gizmolist) {
    BLI_remlink(gizmolist, gz);
  }

  BLI_assert(gzmap->gzmap_context.highlight != gz);
  BLI_assert(gzmap->gzmap_context.modal != gz);

  WM_gizmo_free(gz);
}

/* -------------------------------------------------------------------- */
/** \name Gizmo Creation API
 *
 * API for defining data on gizmo creation.
 *
 * \{ */

struct wmGizmoOpElem *WM_gizmo_operator_get(wmGizmo *gz, int part_index)
{
  if (gz->op_data && ((part_index >= 0) && (part_index < gz->op_data_len))) {
    return &gz->op_data[part_index];
  }
  return NULL;
}

PointerRNA *WM_gizmo_operator_set(wmGizmo *gz,
                                  int part_index,
                                  wmOperatorType *ot,
                                  IDProperty *properties)
{
  BLI_assert(part_index < 255);
  /* We could pre-allocate these but using multiple is such a rare thing. */
  if (part_index >= gz->op_data_len) {
    gz->op_data_len = part_index + 1;
    gz->op_data = MEM_recallocN(gz->op_data, sizeof(*gz->op_data) * gz->op_data_len);
  }
  wmGizmoOpElem *gzop = &gz->op_data[part_index];
  gzop->type = ot;

  if (gzop->ptr.data) {
    WM_operator_properties_free(&gzop->ptr);
  }
  WM_operator_properties_create_ptr(&gzop->ptr, ot);

  if (properties) {
    gzop->ptr.data = properties;
  }

  return &gzop->ptr;
}

int WM_gizmo_operator_invoke(bContext *C, wmGizmo *gz, wmGizmoOpElem *gzop, const wmEvent *event)
{
  if (gz->flag & WM_GIZMO_OPERATOR_TOOL_INIT) {
    /* Merge toolsettings into the gizmo properties. */
    PointerRNA tref_ptr;
    bToolRef *tref = WM_toolsystem_ref_from_context(C);
    if (tref && WM_toolsystem_ref_properties_get_from_operator(tref, gzop->type, &tref_ptr)) {
      if (gzop->ptr.data == NULL) {
        IDPropertyTemplate val = {0};
        gzop->ptr.data = IDP_New(IDP_GROUP, &val, "wmOperatorProperties");
      }
      IDP_MergeGroup(gzop->ptr.data, tref_ptr.data, false);
    }
  }
  return WM_operator_name_call_ptr(C, gzop->type, WM_OP_INVOKE_DEFAULT, &gzop->ptr, event);
}

static void wm_gizmo_set_matrix_rotation_from_z_axis__internal(float matrix[4][4],
                                                               const float z_axis[3])
{
  /* old code, seems we can use simpler method */
#if 0
  const float z_global[3] = {0.0f, 0.0f, 1.0f};
  float rot[3][3];

  rotation_between_vecs_to_mat3(rot, z_global, z_axis);
  copy_v3_v3(matrix[0], rot[0]);
  copy_v3_v3(matrix[1], rot[1]);
  copy_v3_v3(matrix[2], rot[2]);
#else
  normalize_v3_v3(matrix[2], z_axis);
  ortho_basis_v3v3_v3(matrix[0], matrix[1], matrix[2]);
#endif
}

static void wm_gizmo_set_matrix_rotation_from_yz_axis__internal(float matrix[4][4],
                                                                const float y_axis[3],
                                                                const float z_axis[3])
{
  normalize_v3_v3(matrix[1], y_axis);
  normalize_v3_v3(matrix[2], z_axis);
  cross_v3_v3v3(matrix[0], matrix[1], matrix[2]);
  normalize_v3(matrix[0]);
}

void WM_gizmo_set_matrix_rotation_from_z_axis(wmGizmo *gz, const float z_axis[3])
{
  wm_gizmo_set_matrix_rotation_from_z_axis__internal(gz->matrix_basis, z_axis);
}
void WM_gizmo_set_matrix_rotation_from_yz_axis(wmGizmo *gz,
                                               const float y_axis[3],
                                               const float z_axis[3])
{
  wm_gizmo_set_matrix_rotation_from_yz_axis__internal(gz->matrix_basis, y_axis, z_axis);
}
void WM_gizmo_set_matrix_location(wmGizmo *gz, const float origin[3])
{
  copy_v3_v3(gz->matrix_basis[3], origin);
}

void WM_gizmo_set_matrix_offset_rotation_from_z_axis(wmGizmo *gz, const float z_axis[3])
{
  wm_gizmo_set_matrix_rotation_from_z_axis__internal(gz->matrix_offset, z_axis);
}
void WM_gizmo_set_matrix_offset_rotation_from_yz_axis(wmGizmo *gz,
                                                      const float y_axis[3],
                                                      const float z_axis[3])
{
  wm_gizmo_set_matrix_rotation_from_yz_axis__internal(gz->matrix_offset, y_axis, z_axis);
}
void WM_gizmo_set_matrix_offset_location(wmGizmo *gz, const float offset[3])
{
  copy_v3_v3(gz->matrix_offset[3], offset);
}

void WM_gizmo_set_flag(wmGizmo *gz, const int flag, const bool enable)
{
  if (enable) {
    gz->flag |= flag;
  }
  else {
    gz->flag &= ~flag;
  }
}

void WM_gizmo_set_scale(wmGizmo *gz, const float scale)
{
  gz->scale_basis = scale;
}

void WM_gizmo_set_line_width(wmGizmo *gz, const float line_width)
{
  gz->line_width = line_width;
}

void WM_gizmo_get_color(const wmGizmo *gz, float color[4])
{
  copy_v4_v4(color, gz->color);
}
void WM_gizmo_set_color(wmGizmo *gz, const float color[4])
{
  copy_v4_v4(gz->color, color);
}

void WM_gizmo_get_color_highlight(const wmGizmo *gz, float color_hi[4])
{
  copy_v4_v4(color_hi, gz->color_hi);
}
void WM_gizmo_set_color_highlight(wmGizmo *gz, const float color_hi[4])
{
  copy_v4_v4(gz->color_hi, color_hi);
}

/** \} */ /* Gizmo Creation API. */

/* -------------------------------------------------------------------- */
/** \name Gizmo Callback Assignment
 * \{ */

void WM_gizmo_set_fn_custom_modal(struct wmGizmo *gz, wmGizmoFnModal fn)
{
  gz->custom_modal = fn;
}

/** \} */

/* -------------------------------------------------------------------- */

bool wm_gizmo_select_set_ex(
    wmGizmoMap *gzmap, wmGizmo *gz, bool select, bool use_array, bool use_callback)
{
  bool changed = false;

  if (select) {
    if ((gz->state & WM_GIZMO_STATE_SELECT) == 0) {
      if (use_array) {
        wm_gizmomap_select_array_push_back(gzmap, gz);
      }
      gz->state |= WM_GIZMO_STATE_SELECT;
      changed = true;
    }
  }
  else {
    if (gz->state & WM_GIZMO_STATE_SELECT) {
      if (use_array) {
        wm_gizmomap_select_array_remove(gzmap, gz);
      }
      gz->state &= ~WM_GIZMO_STATE_SELECT;
      changed = true;
    }
  }

  /* In the case of unlinking we only want to remove from the array
   * and not write to the external state */
  if (use_callback && changed) {
    if (gz->type->select_refresh) {
      gz->type->select_refresh(gz);
    }
  }

  return changed;
}

bool WM_gizmo_select_unlink(wmGizmoMap *gzmap, wmGizmo *gz)
{
  return wm_gizmo_select_set_ex(gzmap, gz, false, true, false);
}

bool WM_gizmo_select_set(wmGizmoMap *gzmap, wmGizmo *gz, bool select)
{
  return wm_gizmo_select_set_ex(gzmap, gz, select, true, true);
}

bool WM_gizmo_highlight_set(wmGizmoMap *gzmap, wmGizmo *gz)
{
  return wm_gizmomap_highlight_set(gzmap, NULL, gz, gz ? gz->highlight_part : 0);
}

bool wm_gizmo_select_and_highlight(bContext *C, wmGizmoMap *gzmap, wmGizmo *gz)
{
  if (WM_gizmo_select_set(gzmap, gz, true)) {
    wm_gizmomap_highlight_set(gzmap, C, gz, gz->highlight_part);
    return true;
  }
  return false;
}

void WM_gizmo_modal_set_from_setup(struct wmGizmoMap *gzmap,
                                   struct bContext *C,
                                   struct wmGizmo *gz,
                                   int part_index,
                                   const wmEvent *event)
{
  gz->highlight_part = part_index;
  WM_gizmo_highlight_set(gzmap, gz);
  if (false) {
    wm_gizmomap_modal_set(gzmap, C, gz, event, true);
  }
  else {
    /* WEAK: but it works. */
    WM_operator_name_call(C, "GIZMOGROUP_OT_gizmo_tweak", WM_OP_INVOKE_DEFAULT, NULL, event);
  }
}

void wm_gizmo_calculate_scale(wmGizmo *gz, const bContext *C)
{
  const RegionView3D *rv3d = CTX_wm_region_view3d(C);
  float scale = UI_DPI_FAC;

  if ((gz->parent_gzgroup->type->flag & WM_GIZMOGROUPTYPE_SCALE) == 0) {
    scale *= U.gizmo_size;
    if (rv3d) {
      /* 'ED_view3d_pixel_size' includes 'U.pixelsize', remove it. */
      float matrix_world[4][4];
      if (gz->type->matrix_basis_get) {
        float matrix_basis[4][4];
        gz->type->matrix_basis_get(gz, matrix_basis);
        mul_m4_m4m4(matrix_world, gz->matrix_space, matrix_basis);
      }
      else {
        mul_m4_m4m4(matrix_world, gz->matrix_space, gz->matrix_basis);
      }

      /* Exclude matrix_offset from scale. */
      scale *= ED_view3d_pixel_size_no_ui_scale(rv3d, matrix_world[3]);
    }
  }

  gz->scale_final = gz->scale_basis * scale;
}

static void gizmo_update_prop_data(wmGizmo *gz)
{
  /* gizmo property might have been changed, so update gizmo */
  if (gz->type->property_update) {
    wmGizmoProperty *gz_prop_array = WM_gizmo_target_property_array(gz);
    for (int i = 0; i < gz->type->target_property_defs_len; i++) {
      wmGizmoProperty *gz_prop = &gz_prop_array[i];
      if (WM_gizmo_target_property_is_valid(gz_prop)) {
        gz->type->property_update(gz, gz_prop);
      }
    }
  }
}

void wm_gizmo_update(wmGizmo *gz, const bContext *C, const bool refresh_map)
{
  if (refresh_map) {
    gizmo_update_prop_data(gz);
  }
  wm_gizmo_calculate_scale(gz, C);
}

int wm_gizmo_is_visible(wmGizmo *gz)
{
  if (gz->flag & WM_GIZMO_HIDDEN) {
    return 0;
  }
  if ((gz->state & WM_GIZMO_STATE_MODAL) &&
      !(gz->flag & (WM_GIZMO_DRAW_MODAL | WM_GIZMO_DRAW_VALUE))) {
    /* don't draw while modal (dragging) */
    return 0;
  }
  if ((gz->flag & WM_GIZMO_DRAW_HOVER) && !(gz->state & WM_GIZMO_STATE_HIGHLIGHT) &&
      !(gz->state & WM_GIZMO_STATE_SELECT)) /* still draw selected gizmos */
  {
    /* update but don't draw */
    return WM_GIZMO_IS_VISIBLE_UPDATE;
  }

  return WM_GIZMO_IS_VISIBLE_UPDATE | WM_GIZMO_IS_VISIBLE_DRAW;
}

void WM_gizmo_calc_matrix_final_params(const wmGizmo *gz,
                                       const struct WM_GizmoMatrixParams *params,
                                       float r_mat[4][4])
{
  const float(*const matrix_space)[4] = params->matrix_space ? params->matrix_space :
                                                               gz->matrix_space;
  const float(*const matrix_basis)[4] = params->matrix_basis ? params->matrix_basis :
                                                               gz->matrix_basis;
  const float(*const matrix_offset)[4] = params->matrix_offset ? params->matrix_offset :
                                                                 gz->matrix_offset;
  const float *scale_final = params->scale_final ? params->scale_final : &gz->scale_final;

  float final_matrix[4][4];
  if (params->matrix_basis == NULL && gz->type->matrix_basis_get) {
    gz->type->matrix_basis_get(gz, final_matrix);
  }
  else {
    copy_m4_m4(final_matrix, matrix_basis);
  }

  if (gz->flag & WM_GIZMO_DRAW_NO_SCALE) {
    mul_m4_m4m4(final_matrix, final_matrix, matrix_offset);
  }
  else {
    if (gz->flag & WM_GIZMO_DRAW_OFFSET_SCALE) {
      mul_mat3_m4_fl(final_matrix, *scale_final);
      mul_m4_m4m4(final_matrix, final_matrix, matrix_offset);
    }
    else {
      mul_m4_m4m4(final_matrix, final_matrix, matrix_offset);
      mul_mat3_m4_fl(final_matrix, *scale_final);
    }
  }

  mul_m4_m4m4(r_mat, matrix_space, final_matrix);
}

void WM_gizmo_calc_matrix_final_no_offset(const wmGizmo *gz, float r_mat[4][4])
{
  float mat_identity[4][4];
  unit_m4(mat_identity);

  WM_gizmo_calc_matrix_final_params(gz,
                                    &((struct WM_GizmoMatrixParams){
                                        .matrix_space = NULL,
                                        .matrix_basis = NULL,
                                        .matrix_offset = mat_identity,
                                        .scale_final = NULL,
                                    }),
                                    r_mat);
}

void WM_gizmo_calc_matrix_final(const wmGizmo *gz, float r_mat[4][4])
{
  WM_gizmo_calc_matrix_final_params(gz,
                                    &((struct WM_GizmoMatrixParams){
                                        .matrix_space = NULL,
                                        .matrix_basis = NULL,
                                        .matrix_offset = NULL,
                                        .scale_final = NULL,
                                    }),
                                    r_mat);
}

/* -------------------------------------------------------------------- */
/** \name Gizmo Property Access
 *
 * Matches `WM_operator_properties` conventions.
 *
 * \{ */

void WM_gizmo_properties_create_ptr(PointerRNA *ptr, wmGizmoType *gzt)
{
  RNA_pointer_create(NULL, gzt->srna, NULL, ptr);
}

void WM_gizmo_properties_create(PointerRNA *ptr, const char *gtstring)
{
  const wmGizmoType *gzt = WM_gizmotype_find(gtstring, false);

  if (gzt) {
    WM_gizmo_properties_create_ptr(ptr, (wmGizmoType *)gzt);
  }
  else {
    RNA_pointer_create(NULL, &RNA_GizmoProperties, NULL, ptr);
  }
}

void WM_gizmo_properties_alloc(PointerRNA **ptr, IDProperty **properties, const char *gtstring)
{
  if (*properties == NULL) {
    IDPropertyTemplate val = {0};
    *properties = IDP_New(IDP_GROUP, &val, "wmOpItemProp");
  }

  if (*ptr == NULL) {
    *ptr = MEM_callocN(sizeof(PointerRNA), "wmOpItemPtr");
    WM_gizmo_properties_create(*ptr, gtstring);
  }

  (*ptr)->data = *properties;
}

void WM_gizmo_properties_sanitize(PointerRNA *ptr, const bool no_context)
{
  RNA_STRUCT_BEGIN (ptr, prop) {
    switch (RNA_property_type(prop)) {
      case PROP_ENUM:
        if (no_context) {
          RNA_def_property_flag(prop, PROP_ENUM_NO_CONTEXT);
        }
        else {
          RNA_def_property_clear_flag(prop, PROP_ENUM_NO_CONTEXT);
        }
        break;
      case PROP_POINTER: {
        StructRNA *ptype = RNA_property_pointer_type(ptr, prop);

        /* recurse into gizmo properties */
        if (RNA_struct_is_a(ptype, &RNA_GizmoProperties)) {
          PointerRNA opptr = RNA_property_pointer_get(ptr, prop);
          WM_gizmo_properties_sanitize(&opptr, no_context);
        }
        break;
      }
      default:
        break;
    }
  }
  RNA_STRUCT_END;
}

bool WM_gizmo_properties_default(PointerRNA *ptr, const bool do_update)
{
  bool changed = false;
  RNA_STRUCT_BEGIN (ptr, prop) {
    switch (RNA_property_type(prop)) {
      case PROP_POINTER: {
        StructRNA *ptype = RNA_property_pointer_type(ptr, prop);
        if (ptype != &RNA_Struct) {
          PointerRNA opptr = RNA_property_pointer_get(ptr, prop);
          changed |= WM_gizmo_properties_default(&opptr, do_update);
        }
        break;
      }
      default:
        if ((do_update == false) || (RNA_property_is_set(ptr, prop) == false)) {
          if (RNA_property_reset(ptr, prop, -1)) {
            changed = true;
          }
        }
        break;
    }
  }
  RNA_STRUCT_END;

  return changed;
}

void WM_gizmo_properties_reset(wmGizmo *gz)
{
  if (gz->ptr->data) {
    PropertyRNA *iterprop;
    iterprop = RNA_struct_iterator_property(gz->type->srna);

    RNA_PROP_BEGIN (gz->ptr, itemptr, iterprop) {
      PropertyRNA *prop = itemptr.data;

      if ((RNA_property_flag(prop) & PROP_SKIP_SAVE) == 0) {
        const char *identifier = RNA_property_identifier(prop);
        RNA_struct_idprops_unset(gz->ptr, identifier);
      }
    }
    RNA_PROP_END;
  }
}

void WM_gizmo_properties_clear(PointerRNA *ptr)
{
  IDProperty *properties = ptr->data;

  if (properties) {
    IDP_ClearProperty(properties);
  }
}

void WM_gizmo_properties_free(PointerRNA *ptr)
{
  IDProperty *properties = ptr->data;

  if (properties) {
    IDP_FreeProperty(properties);
    ptr->data = NULL; /* just in case */
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name General Utilities
 * \{ */

bool WM_gizmo_context_check_drawstep(const struct bContext *C, eWM_GizmoFlagMapDrawStep step)
{
  switch (step) {
    case WM_GIZMOMAP_DRAWSTEP_2D: {
      break;
    }
    case WM_GIZMOMAP_DRAWSTEP_3D: {
      wmWindowManager *wm = CTX_wm_manager(C);
      if (ED_screen_animation_playing(wm)) {
        return false;
      }
      break;
    }
  }
  return true;
}

/** \} */
