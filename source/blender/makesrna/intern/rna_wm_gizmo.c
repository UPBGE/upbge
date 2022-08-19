/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup RNA
 */

#include <stdlib.h>

#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_userdef_types.h"
#include "DNA_view3d_types.h"
#include "DNA_windowmanager_types.h"

#include "BLI_listbase.h"
#include "BLI_utildefines.h"

#include "BLT_translation.h"

#include "RNA_access.h"
#include "RNA_define.h"
#include "RNA_enum_types.h"

#include "rna_internal.h"

#include "WM_types.h"

#ifdef RNA_RUNTIME
/* enum definitions */
#endif /* RNA_RUNTIME */

#ifdef RNA_RUNTIME

#  include "BLI_string_utils.h"

#  include "WM_api.h"

#  include "DNA_workspace_types.h"

#  include "ED_screen.h"

#  include "UI_interface.h"

#  include "BKE_global.h"
#  include "BKE_idprop.h"
#  include "BKE_workspace.h"

#  include "MEM_guardedalloc.h"

#  include "GPU_state.h"

#  ifdef WITH_PYTHON
#    include "BPY_extern.h"
#  endif

/* -------------------------------------------------------------------- */
/** \name Gizmo API
 * \{ */

#  ifdef WITH_PYTHON
static void rna_gizmo_draw_cb(const struct bContext *C, struct wmGizmo *gz)
{
  extern FunctionRNA rna_Gizmo_draw_func;
  wmGizmoGroup *gzgroup = gz->parent_gzgroup;
  PointerRNA gz_ptr;
  ParameterList list;
  FunctionRNA *func;
  RNA_pointer_create(NULL, gz->type->rna_ext.srna, gz, &gz_ptr);
  /* Reference `RNA_struct_find_function(&gz_ptr, "draw")` directly. */
  func = &rna_Gizmo_draw_func;
  RNA_parameter_list_create(&list, &gz_ptr, func);
  RNA_parameter_set_lookup(&list, "context", &C);
  gzgroup->type->rna_ext.call((bContext *)C, &gz_ptr, func, &list);
  RNA_parameter_list_free(&list);
  /* This callback may have called bgl functions. */
  GPU_bgl_end();
}

static void rna_gizmo_draw_select_cb(const struct bContext *C, struct wmGizmo *gz, int select_id)
{
  extern FunctionRNA rna_Gizmo_draw_select_func;
  wmGizmoGroup *gzgroup = gz->parent_gzgroup;
  PointerRNA gz_ptr;
  ParameterList list;
  FunctionRNA *func;
  RNA_pointer_create(NULL, gz->type->rna_ext.srna, gz, &gz_ptr);
  /* Reference `RNA_struct_find_function(&gz_ptr, "draw_select")` directly. */
  func = &rna_Gizmo_draw_select_func;
  RNA_parameter_list_create(&list, &gz_ptr, func);
  RNA_parameter_set_lookup(&list, "context", &C);
  RNA_parameter_set_lookup(&list, "select_id", &select_id);
  gzgroup->type->rna_ext.call((bContext *)C, &gz_ptr, func, &list);
  RNA_parameter_list_free(&list);
  /* This callback may have called bgl functions. */
  GPU_bgl_end();
}

static int rna_gizmo_test_select_cb(struct bContext *C, struct wmGizmo *gz, const int location[2])
{
  extern FunctionRNA rna_Gizmo_test_select_func;
  wmGizmoGroup *gzgroup = gz->parent_gzgroup;
  PointerRNA gz_ptr;
  ParameterList list;
  FunctionRNA *func;
  RNA_pointer_create(NULL, gz->type->rna_ext.srna, gz, &gz_ptr);
  /* Reference `RNA_struct_find_function(&gz_ptr, "test_select")` directly. */
  func = &rna_Gizmo_test_select_func;
  RNA_parameter_list_create(&list, &gz_ptr, func);
  RNA_parameter_set_lookup(&list, "context", &C);
  RNA_parameter_set_lookup(&list, "location", location);
  gzgroup->type->rna_ext.call((bContext *)C, &gz_ptr, func, &list);

  void *ret;
  RNA_parameter_get_lookup(&list, "intersect_id", &ret);
  int intersect_id = *(int *)ret;

  RNA_parameter_list_free(&list);
  return intersect_id;
}

static int rna_gizmo_modal_cb(struct bContext *C,
                              struct wmGizmo *gz,
                              const struct wmEvent *event,
                              eWM_GizmoFlagTweak tweak_flag)
{
  extern FunctionRNA rna_Gizmo_modal_func;
  wmGizmoGroup *gzgroup = gz->parent_gzgroup;
  PointerRNA gz_ptr;
  ParameterList list;
  FunctionRNA *func;
  const int tweak_flag_int = tweak_flag;
  RNA_pointer_create(NULL, gz->type->rna_ext.srna, gz, &gz_ptr);
  /* Reference `RNA_struct_find_function(&gz_ptr, "modal")` directly. */
  func = &rna_Gizmo_modal_func;
  RNA_parameter_list_create(&list, &gz_ptr, func);
  RNA_parameter_set_lookup(&list, "context", &C);
  RNA_parameter_set_lookup(&list, "event", &event);
  RNA_parameter_set_lookup(&list, "tweak", &tweak_flag_int);
  gzgroup->type->rna_ext.call((bContext *)C, &gz_ptr, func, &list);

  void *ret;
  RNA_parameter_get_lookup(&list, "result", &ret);
  int ret_enum = *(int *)ret;

  RNA_parameter_list_free(&list);
  return ret_enum;
}

static void rna_gizmo_setup_cb(struct wmGizmo *gz)
{
  extern FunctionRNA rna_Gizmo_setup_func;
  wmGizmoGroup *gzgroup = gz->parent_gzgroup;
  PointerRNA gz_ptr;
  ParameterList list;
  FunctionRNA *func;
  RNA_pointer_create(NULL, gz->type->rna_ext.srna, gz, &gz_ptr);
  /* Reference `RNA_struct_find_function(&gz_ptr, "setup")` directly. */
  func = &rna_Gizmo_setup_func;
  RNA_parameter_list_create(&list, &gz_ptr, func);
  gzgroup->type->rna_ext.call((bContext *)NULL, &gz_ptr, func, &list);
  RNA_parameter_list_free(&list);
}

static int rna_gizmo_invoke_cb(struct bContext *C, struct wmGizmo *gz, const struct wmEvent *event)
{
  extern FunctionRNA rna_Gizmo_invoke_func;
  wmGizmoGroup *gzgroup = gz->parent_gzgroup;
  PointerRNA gz_ptr;
  ParameterList list;
  FunctionRNA *func;
  RNA_pointer_create(NULL, gz->type->rna_ext.srna, gz, &gz_ptr);
  /* Reference `RNA_struct_find_function(&gz_ptr, "invoke")` directly. */
  func = &rna_Gizmo_invoke_func;
  RNA_parameter_list_create(&list, &gz_ptr, func);
  RNA_parameter_set_lookup(&list, "context", &C);
  RNA_parameter_set_lookup(&list, "event", &event);
  gzgroup->type->rna_ext.call((bContext *)C, &gz_ptr, func, &list);

  void *ret;
  RNA_parameter_get_lookup(&list, "result", &ret);
  int ret_enum = *(int *)ret;

  RNA_parameter_list_free(&list);
  return ret_enum;
}

static void rna_gizmo_exit_cb(struct bContext *C, struct wmGizmo *gz, bool cancel)
{
  extern FunctionRNA rna_Gizmo_exit_func;
  wmGizmoGroup *gzgroup = gz->parent_gzgroup;
  PointerRNA gz_ptr;
  ParameterList list;
  FunctionRNA *func;
  RNA_pointer_create(NULL, gz->type->rna_ext.srna, gz, &gz_ptr);
  /* Reference `RNA_struct_find_function(&gz_ptr, "exit")` directly. */
  func = &rna_Gizmo_exit_func;
  RNA_parameter_list_create(&list, &gz_ptr, func);
  RNA_parameter_set_lookup(&list, "context", &C);
  {
    int cancel_i = cancel;
    RNA_parameter_set_lookup(&list, "cancel", &cancel_i);
  }
  gzgroup->type->rna_ext.call((bContext *)C, &gz_ptr, func, &list);
  RNA_parameter_list_free(&list);
}

static void rna_gizmo_select_refresh_cb(struct wmGizmo *gz)
{
  extern FunctionRNA rna_Gizmo_select_refresh_func;
  wmGizmoGroup *gzgroup = gz->parent_gzgroup;
  PointerRNA gz_ptr;
  ParameterList list;
  FunctionRNA *func;
  RNA_pointer_create(NULL, gz->type->rna_ext.srna, gz, &gz_ptr);
  /* Reference `RNA_struct_find_function(&gz_ptr, "select_refresh")` directly. */
  func = &rna_Gizmo_select_refresh_func;
  RNA_parameter_list_create(&list, &gz_ptr, func);
  gzgroup->type->rna_ext.call((bContext *)NULL, &gz_ptr, func, &list);
  RNA_parameter_list_free(&list);
}

#  endif /* WITH_PYTHON */

/* just to work around 'const char *' warning and to ensure this is a python op */
static void rna_Gizmo_bl_idname_set(PointerRNA *ptr, const char *value)
{
  wmGizmo *data = ptr->data;
  char *str = (char *)data->type->idname;
  if (!str[0]) {
    BLI_strncpy(str, value, MAX_NAME); /* utf8 already ensured */
  }
  else {
    BLI_assert_msg(0, "setting the bl_idname on a non-builtin operator");
  }
}

static void rna_Gizmo_update_redraw(Main *UNUSED(bmain), Scene *UNUSED(scene), PointerRNA *ptr)
{
  wmGizmo *gizmo = ptr->data;
  gizmo->do_draw = true;
}

static wmGizmo *rna_GizmoProperties_find_operator(PointerRNA *ptr)
{
#  if 0
  wmWindowManager *wm = (wmWindowManager *)ptr->owner_id;
#  endif

  /* We could try workaround this lookup, but not trivial. */
  for (bScreen *screen = G_MAIN->screens.first; screen; screen = screen->id.next) {
    IDProperty *properties = ptr->data;
    LISTBASE_FOREACH (ScrArea *, area, &screen->areabase) {
      LISTBASE_FOREACH (ARegion *, region, &area->regionbase) {
        if (region->gizmo_map) {
          wmGizmoMap *gzmap = region->gizmo_map;
          LISTBASE_FOREACH (wmGizmoGroup *, gzgroup, WM_gizmomap_group_list(gzmap)) {
            LISTBASE_FOREACH (wmGizmo *, gz, &gzgroup->gizmos) {
              if (gz->properties == properties) {
                return gz;
              }
            }
          }
        }
      }
    }
  }
  return NULL;
}

static StructRNA *rna_GizmoProperties_refine(PointerRNA *ptr)
{
  wmGizmo *gz = rna_GizmoProperties_find_operator(ptr);

  if (gz) {
    return gz->type->srna;
  }
  else {
    return ptr->type;
  }
}

static IDProperty **rna_GizmoProperties_idprops(PointerRNA *ptr)
{
  return (IDProperty **)&ptr->data;
}

static PointerRNA rna_Gizmo_properties_get(PointerRNA *ptr)
{
  wmGizmo *gz = ptr->data;
  return rna_pointer_inherit_refine(ptr, gz->type->srna, gz->properties);
}

/* wmGizmo.float */
#  define RNA_GIZMO_GENERIC_FLOAT_RW_DEF(func_id, member_id) \
    static float rna_Gizmo_##func_id##_get(PointerRNA *ptr) \
    { \
      wmGizmo *gz = ptr->data; \
      return gz->member_id; \
    } \
    static void rna_Gizmo_##func_id##_set(PointerRNA *ptr, float value) \
    { \
      wmGizmo *gz = ptr->data; \
      gz->member_id = value; \
    }
#  define RNA_GIZMO_GENERIC_FLOAT_ARRAY_INDEX_RW_DEF(func_id, member_id, index) \
    static float rna_Gizmo_##func_id##_get(PointerRNA *ptr) \
    { \
      wmGizmo *gz = ptr->data; \
      return gz->member_id[index]; \
    } \
    static void rna_Gizmo_##func_id##_set(PointerRNA *ptr, float value) \
    { \
      wmGizmo *gz = ptr->data; \
      gz->member_id[index] = value; \
    }
/* wmGizmo.float[len] */
#  define RNA_GIZMO_GENERIC_FLOAT_ARRAY_RW_DEF(func_id, member_id, len) \
    static void rna_Gizmo_##func_id##_get(PointerRNA *ptr, float value[len]) \
    { \
      wmGizmo *gz = ptr->data; \
      memcpy(value, gz->member_id, sizeof(float[len])); \
    } \
    static void rna_Gizmo_##func_id##_set(PointerRNA *ptr, const float value[len]) \
    { \
      wmGizmo *gz = ptr->data; \
      memcpy(gz->member_id, value, sizeof(float[len])); \
    }

/* wmGizmo.flag */
#  define RNA_GIZMO_GENERIC_FLAG_RW_DEF(func_id, member_id, flag_value) \
    static bool rna_Gizmo_##func_id##_get(PointerRNA *ptr) \
    { \
      wmGizmo *gz = ptr->data; \
      return (gz->member_id & flag_value) != 0; \
    } \
    static void rna_Gizmo_##func_id##_set(PointerRNA *ptr, bool value) \
    { \
      wmGizmo *gz = ptr->data; \
      SET_FLAG_FROM_TEST(gz->member_id, value, flag_value); \
    }

/* wmGizmo.flag (negative) */
#  define RNA_GIZMO_GENERIC_FLAG_NEG_RW_DEF(func_id, member_id, flag_value) \
    static bool rna_Gizmo_##func_id##_get(PointerRNA *ptr) \
    { \
      wmGizmo *gz = ptr->data; \
      return (gz->member_id & flag_value) == 0; \
    } \
    static void rna_Gizmo_##func_id##_set(PointerRNA *ptr, bool value) \
    { \
      wmGizmo *gz = ptr->data; \
      SET_FLAG_FROM_TEST(gz->member_id, !value, flag_value); \
    }

#  define RNA_GIZMO_FLAG_RO_DEF(func_id, member_id, flag_value) \
    static int rna_Gizmo_##func_id##_get(PointerRNA *ptr) \
    { \
      wmGizmo *gz = ptr->data; \
      return (gz->member_id & flag_value) != 0; \
    }

RNA_GIZMO_GENERIC_FLOAT_ARRAY_RW_DEF(color, color, 3);
RNA_GIZMO_GENERIC_FLOAT_ARRAY_RW_DEF(color_hi, color_hi, 3);

RNA_GIZMO_GENERIC_FLOAT_ARRAY_INDEX_RW_DEF(alpha, color, 3);
RNA_GIZMO_GENERIC_FLOAT_ARRAY_INDEX_RW_DEF(alpha_hi, color_hi, 3);

RNA_GIZMO_GENERIC_FLOAT_ARRAY_RW_DEF(matrix_space, matrix_space, 16);
RNA_GIZMO_GENERIC_FLOAT_ARRAY_RW_DEF(matrix_basis, matrix_basis, 16);
RNA_GIZMO_GENERIC_FLOAT_ARRAY_RW_DEF(matrix_offset, matrix_offset, 16);

static void rna_Gizmo_matrix_world_get(PointerRNA *ptr, float value[16])
{
  wmGizmo *gz = ptr->data;
  WM_gizmo_calc_matrix_final(gz, (float(*)[4])value);
}

RNA_GIZMO_GENERIC_FLOAT_RW_DEF(scale_basis, scale_basis);
RNA_GIZMO_GENERIC_FLOAT_RW_DEF(line_width, line_width);
RNA_GIZMO_GENERIC_FLOAT_RW_DEF(select_bias, select_bias);

RNA_GIZMO_GENERIC_FLAG_RW_DEF(flag_use_draw_hover, flag, WM_GIZMO_DRAW_HOVER);
RNA_GIZMO_GENERIC_FLAG_RW_DEF(flag_use_draw_modal, flag, WM_GIZMO_DRAW_MODAL);
RNA_GIZMO_GENERIC_FLAG_RW_DEF(flag_use_draw_value, flag, WM_GIZMO_DRAW_VALUE);
RNA_GIZMO_GENERIC_FLAG_RW_DEF(flag_use_draw_offset_scale, flag, WM_GIZMO_DRAW_OFFSET_SCALE);
RNA_GIZMO_GENERIC_FLAG_NEG_RW_DEF(flag_use_draw_scale, flag, WM_GIZMO_DRAW_NO_SCALE);
RNA_GIZMO_GENERIC_FLAG_RW_DEF(flag_hide, flag, WM_GIZMO_HIDDEN);
RNA_GIZMO_GENERIC_FLAG_RW_DEF(flag_hide_select, flag, WM_GIZMO_HIDDEN_SELECT);
RNA_GIZMO_GENERIC_FLAG_RW_DEF(flag_hide_keymap, flag, WM_GIZMO_HIDDEN_KEYMAP);
RNA_GIZMO_GENERIC_FLAG_RW_DEF(flag_use_grab_cursor, flag, WM_GIZMO_MOVE_CURSOR);
RNA_GIZMO_GENERIC_FLAG_RW_DEF(flag_use_select_background, flag, WM_GIZMO_SELECT_BACKGROUND);
RNA_GIZMO_GENERIC_FLAG_RW_DEF(flag_use_operator_tool_properties,
                              flag,
                              WM_GIZMO_OPERATOR_TOOL_INIT);
RNA_GIZMO_GENERIC_FLAG_RW_DEF(flag_use_event_handle_all, flag, WM_GIZMO_EVENT_HANDLE_ALL);
RNA_GIZMO_GENERIC_FLAG_NEG_RW_DEF(flag_use_tooltip, flag, WM_GIZMO_NO_TOOLTIP);

/* wmGizmo.state */
RNA_GIZMO_FLAG_RO_DEF(state_is_highlight, state, WM_GIZMO_STATE_HIGHLIGHT);
RNA_GIZMO_FLAG_RO_DEF(state_is_modal, state, WM_GIZMO_STATE_MODAL);
RNA_GIZMO_FLAG_RO_DEF(state_select, state, WM_GIZMO_STATE_SELECT);

static void rna_Gizmo_state_select_set(struct PointerRNA *ptr, bool value)
{
  wmGizmo *gz = ptr->data;
  wmGizmoGroup *gzgroup = gz->parent_gzgroup;
  WM_gizmo_select_set(gzgroup->parent_gzmap, gz, value);
}

static PointerRNA rna_Gizmo_group_get(PointerRNA *ptr)
{
  wmGizmo *gz = ptr->data;
  return rna_pointer_inherit_refine(ptr, &RNA_GizmoGroup, gz->parent_gzgroup);
}

#  ifdef WITH_PYTHON

static void rna_Gizmo_unregister(struct Main *bmain, StructRNA *type);
void BPY_RNA_gizmo_wrapper(wmGizmoType *gzgt, void *userdata);

static StructRNA *rna_Gizmo_register(Main *bmain,
                                     ReportList *reports,
                                     void *data,
                                     const char *identifier,
                                     StructValidateFunc validate,
                                     StructCallbackFunc call,
                                     StructFreeFunc free)
{
  struct {
    char idname[MAX_NAME];
  } temp_buffers;

  wmGizmoType dummygt = {NULL};
  wmGizmo dummymnp = {NULL};
  PointerRNA mnp_ptr;

  /* Two sets of functions. */
  int have_function[8];

  /* setup dummy gizmo & gizmo type to store static properties in */
  dummymnp.type = &dummygt;
  dummygt.idname = temp_buffers.idname;
  RNA_pointer_create(NULL, &RNA_Gizmo, &dummymnp, &mnp_ptr);

  /* Clear so we can detect if it's left unset. */
  temp_buffers.idname[0] = '\0';

  /* validate the python class */
  if (validate(&mnp_ptr, data, have_function) != 0) {
    return NULL;
  }

  if (strlen(identifier) >= sizeof(temp_buffers.idname)) {
    BKE_reportf(reports,
                RPT_ERROR,
                "Registering gizmo class: '%s' is too long, maximum length is %d",
                identifier,
                (int)sizeof(temp_buffers.idname));
    return NULL;
  }

  /* check if we have registered this gizmo type before, and remove it */
  {
    const wmGizmoType *gzt = WM_gizmotype_find(dummygt.idname, true);
    if (gzt && gzt->rna_ext.srna) {
      rna_Gizmo_unregister(bmain, gzt->rna_ext.srna);
    }
  }
  if (!RNA_struct_available_or_report(reports, dummygt.idname)) {
    return NULL;
  }

  { /* allocate the idname */
    /* For multiple strings see GizmoGroup. */
    dummygt.idname = BLI_strdup(temp_buffers.idname);
  }

  /* create a new gizmo type */
  dummygt.rna_ext.srna = RNA_def_struct_ptr(&BLENDER_RNA, dummygt.idname, &RNA_Gizmo);
  /* gizmo properties are registered separately */
  RNA_def_struct_flag(dummygt.rna_ext.srna, STRUCT_NO_IDPROPERTIES);
  dummygt.rna_ext.data = data;
  dummygt.rna_ext.call = call;
  dummygt.rna_ext.free = free;

  {
    int i = 0;
    dummygt.draw = (have_function[i++]) ? rna_gizmo_draw_cb : NULL;
    dummygt.draw_select = (have_function[i++]) ? rna_gizmo_draw_select_cb : NULL;
    dummygt.test_select = (have_function[i++]) ? rna_gizmo_test_select_cb : NULL;
    dummygt.modal = (have_function[i++]) ? rna_gizmo_modal_cb : NULL;
    //      dummygt.property_update = (have_function[i++]) ? rna_gizmo_property_update : NULL;
    //      dummygt.position_get = (have_function[i++]) ? rna_gizmo_position_get : NULL;
    dummygt.setup = (have_function[i++]) ? rna_gizmo_setup_cb : NULL;
    dummygt.invoke = (have_function[i++]) ? rna_gizmo_invoke_cb : NULL;
    dummygt.exit = (have_function[i++]) ? rna_gizmo_exit_cb : NULL;
    dummygt.select_refresh = (have_function[i++]) ? rna_gizmo_select_refresh_cb : NULL;

    BLI_assert(i == ARRAY_SIZE(have_function));
  }

  WM_gizmotype_append_ptr(BPY_RNA_gizmo_wrapper, (void *)&dummygt);

  /* update while blender is running */
  WM_main_add_notifier(NC_SCREEN | NA_EDITED, NULL);

  return dummygt.rna_ext.srna;
}

static void rna_Gizmo_unregister(struct Main *bmain, StructRNA *type)
{
  wmGizmoType *gzt = RNA_struct_blender_type_get(type);

  if (!gzt) {
    return;
  }

  WM_gizmotype_remove_ptr(NULL, bmain, gzt);

  /* Free extension after removing instances so `__del__` doesn't crash, see: T85567. */
  RNA_struct_free_extension(type, &gzt->rna_ext);
  RNA_struct_free(&BLENDER_RNA, type);

  /* Free gizmo group after the extension as it owns the identifier memory. */
  WM_gizmotype_free_ptr(gzt);

  WM_main_add_notifier(NC_SCREEN | NA_EDITED, NULL);
}

static void **rna_Gizmo_instance(PointerRNA *ptr)
{
  wmGizmo *gz = ptr->data;
  return &gz->py_instance;
}

#  endif /* WITH_PYTHON */

static StructRNA *rna_Gizmo_refine(PointerRNA *mnp_ptr)
{
  wmGizmo *gz = mnp_ptr->data;
  return (gz->type && gz->type->rna_ext.srna) ? gz->type->rna_ext.srna : &RNA_Gizmo;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Gizmo Group API
 * \{ */

static wmGizmoGroupType *rna_GizmoGroupProperties_find_gizmo_group_type(PointerRNA *ptr)
{
  IDProperty *properties = (IDProperty *)ptr->data;
  wmGizmoGroupType *gzgt = WM_gizmogrouptype_find(properties->name, false);
  return gzgt;
}

static StructRNA *rna_GizmoGroupProperties_refine(PointerRNA *ptr)
{
  wmGizmoGroupType *gzgt = rna_GizmoGroupProperties_find_gizmo_group_type(ptr);

  if (gzgt) {
    return gzgt->srna;
  }
  else {
    return ptr->type;
  }
}

static IDProperty **rna_GizmoGroupProperties_idprops(PointerRNA *ptr)
{
  return (IDProperty **)&ptr->data;
}

static wmGizmo *rna_GizmoGroup_gizmo_new(wmGizmoGroup *gzgroup,
                                         ReportList *reports,
                                         const char *idname)
{
  const wmGizmoType *gzt = WM_gizmotype_find(idname, true);
  if (gzt == NULL) {
    BKE_reportf(reports, RPT_ERROR, "GizmoType '%s' not known", idname);
    return NULL;
  }
  if ((gzgroup->type->flag & WM_GIZMOGROUPTYPE_3D) == 0) {
    /* Allow for neither callbacks to be set, while this doesn't seem like a valid use case,
     * there may be rare situations where a developer wants a gizmo to be draw-only. */
    if ((gzt->test_select == NULL) && (gzt->draw_select != NULL)) {
      BKE_reportf(reports,
                  RPT_ERROR,
                  "GizmoType '%s' is for a 3D gizmo-group. "
                  "The 'draw_select' callback is set where only 'test_select' will be used",
                  idname);
      return NULL;
    }
  }
  wmGizmo *gz = WM_gizmo_new_ptr(gzt, gzgroup, NULL);
  return gz;
}

static void rna_GizmoGroup_gizmo_remove(wmGizmoGroup *gzgroup, bContext *C, wmGizmo *gz)
{
  WM_gizmo_unlink(&gzgroup->gizmos, gzgroup->parent_gzmap, gz, C);
}

static void rna_GizmoGroup_gizmo_clear(wmGizmoGroup *gzgroup, bContext *C)
{
  while (gzgroup->gizmos.first) {
    WM_gizmo_unlink(&gzgroup->gizmos, gzgroup->parent_gzmap, gzgroup->gizmos.first, C);
  }
}

static void rna_GizmoGroup_name_get(PointerRNA *ptr, char *value)
{
  wmGizmoGroup *gzgroup = ptr->data;
  strcpy(value, gzgroup->type->name);
}

static int rna_GizmoGroup_name_length(PointerRNA *ptr)
{
  wmGizmoGroup *gzgroup = ptr->data;
  return strlen(gzgroup->type->name);
}

/* just to work around 'const char *' warning and to ensure this is a python op */
static void rna_GizmoGroup_bl_idname_set(PointerRNA *ptr, const char *value)
{
  wmGizmoGroup *data = ptr->data;
  char *str = (char *)data->type->idname;
  if (!str[0]) {
    BLI_strncpy(str, value, MAX_NAME); /* utf8 already ensured */
  }
  else {
    BLI_assert_msg(0, "setting the bl_idname on a non-builtin operator");
  }
}

static void rna_GizmoGroup_bl_label_set(PointerRNA *ptr, const char *value)
{
  wmGizmoGroup *data = ptr->data;
  char *str = (char *)data->type->name;
  if (!str[0]) {
    BLI_strncpy(str, value, MAX_NAME); /* utf8 already ensured */
  }
  else {
    BLI_assert_msg(0, "setting the bl_label on a non-builtin operator");
  }
}

static bool rna_GizmoGroup_has_reports_get(PointerRNA *ptr)
{
  wmGizmoGroup *gzgroup = ptr->data;
  return (gzgroup->reports && gzgroup->reports->list.first);
}

#  ifdef WITH_PYTHON

static bool rna_gizmogroup_poll_cb(const bContext *C, wmGizmoGroupType *gzgt)
{

  extern FunctionRNA rna_GizmoGroup_poll_func;

  PointerRNA ptr;
  ParameterList list;
  FunctionRNA *func;
  void *ret;
  bool visible;

  RNA_pointer_create(NULL, gzgt->rna_ext.srna, NULL, &ptr); /* dummy */
  func = &rna_GizmoGroup_poll_func; /* RNA_struct_find_function(&ptr, "poll"); */

  RNA_parameter_list_create(&list, &ptr, func);
  RNA_parameter_set_lookup(&list, "context", &C);
  gzgt->rna_ext.call((bContext *)C, &ptr, func, &list);

  RNA_parameter_get_lookup(&list, "visible", &ret);
  visible = *(bool *)ret;

  RNA_parameter_list_free(&list);

  return visible;
}

static void rna_gizmogroup_setup_cb(const bContext *C, wmGizmoGroup *gzgroup)
{
  extern FunctionRNA rna_GizmoGroup_setup_func;

  PointerRNA gzgroup_ptr;
  ParameterList list;
  FunctionRNA *func;

  RNA_pointer_create(NULL, gzgroup->type->rna_ext.srna, gzgroup, &gzgroup_ptr);
  func = &rna_GizmoGroup_setup_func; /* RNA_struct_find_function(&wgroupr, "setup"); */

  RNA_parameter_list_create(&list, &gzgroup_ptr, func);
  RNA_parameter_set_lookup(&list, "context", &C);
  gzgroup->type->rna_ext.call((bContext *)C, &gzgroup_ptr, func, &list);

  RNA_parameter_list_free(&list);
}

static wmKeyMap *rna_gizmogroup_setup_keymap_cb(const wmGizmoGroupType *gzgt, wmKeyConfig *config)
{
  extern FunctionRNA rna_GizmoGroup_setup_keymap_func;
  void *ret;

  PointerRNA ptr;
  ParameterList list;
  FunctionRNA *func;

  RNA_pointer_create(NULL, gzgt->rna_ext.srna, NULL, &ptr); /* dummy */
  func =
      &rna_GizmoGroup_setup_keymap_func; /* RNA_struct_find_function(&wgroupr, "setup_keymap"); */

  RNA_parameter_list_create(&list, &ptr, func);
  RNA_parameter_set_lookup(&list, "keyconfig", &config);
  gzgt->rna_ext.call(NULL, &ptr, func, &list);

  RNA_parameter_get_lookup(&list, "keymap", &ret);
  wmKeyMap *keymap = *(wmKeyMap **)ret;

  RNA_parameter_list_free(&list);

  return keymap;
}

static void rna_gizmogroup_refresh_cb(const bContext *C, wmGizmoGroup *gzgroup)
{
  extern FunctionRNA rna_GizmoGroup_refresh_func;

  PointerRNA gzgroup_ptr;
  ParameterList list;
  FunctionRNA *func;

  RNA_pointer_create(NULL, gzgroup->type->rna_ext.srna, gzgroup, &gzgroup_ptr);
  func = &rna_GizmoGroup_refresh_func; /* RNA_struct_find_function(&wgroupr, "refresh"); */

  RNA_parameter_list_create(&list, &gzgroup_ptr, func);
  RNA_parameter_set_lookup(&list, "context", &C);
  gzgroup->type->rna_ext.call((bContext *)C, &gzgroup_ptr, func, &list);

  RNA_parameter_list_free(&list);
}

static void rna_gizmogroup_draw_prepare_cb(const bContext *C, wmGizmoGroup *gzgroup)
{
  extern FunctionRNA rna_GizmoGroup_draw_prepare_func;

  PointerRNA gzgroup_ptr;
  ParameterList list;
  FunctionRNA *func;

  RNA_pointer_create(NULL, gzgroup->type->rna_ext.srna, gzgroup, &gzgroup_ptr);
  func =
      &rna_GizmoGroup_draw_prepare_func; /* RNA_struct_find_function(&wgroupr, "draw_prepare"); */

  RNA_parameter_list_create(&list, &gzgroup_ptr, func);
  RNA_parameter_set_lookup(&list, "context", &C);
  gzgroup->type->rna_ext.call((bContext *)C, &gzgroup_ptr, func, &list);

  RNA_parameter_list_free(&list);
}

static void rna_gizmogroup_invoke_prepare_cb(const bContext *C,
                                             wmGizmoGroup *gzgroup,
                                             wmGizmo *gz,
                                             const wmEvent *event)
{
  extern FunctionRNA rna_GizmoGroup_invoke_prepare_func;

  PointerRNA gzgroup_ptr;
  ParameterList list;
  FunctionRNA *func;

  RNA_pointer_create(NULL, gzgroup->type->rna_ext.srna, gzgroup, &gzgroup_ptr);
  /* Reference `RNA_struct_find_function(&wgroupr, "invoke_prepare")` directly. */
  func = &rna_GizmoGroup_invoke_prepare_func;

  RNA_parameter_list_create(&list, &gzgroup_ptr, func);
  RNA_parameter_set_lookup(&list, "context", &C);
  RNA_parameter_set_lookup(&list, "gizmo", &gz);
  RNA_parameter_set_lookup(&list, "event", &event);
  gzgroup->type->rna_ext.call((bContext *)C, &gzgroup_ptr, func, &list);

  RNA_parameter_list_free(&list);
}

void BPY_RNA_gizmogroup_wrapper(wmGizmoGroupType *gzgt, void *userdata);
static void rna_GizmoGroup_unregister(struct Main *bmain, StructRNA *type);

static StructRNA *rna_GizmoGroup_register(Main *bmain,
                                          ReportList *reports,
                                          void *data,
                                          const char *identifier,
                                          StructValidateFunc validate,
                                          StructCallbackFunc call,
                                          StructFreeFunc free)
{
  struct {
    char name[MAX_NAME];
    char idname[MAX_NAME];
  } temp_buffers;

  wmGizmoGroupType dummywgt = {NULL};
  wmGizmoGroup dummywg = {NULL};
  PointerRNA wgptr;

  /* Two sets of functions. */
  int have_function[6];

  /* setup dummy gizmogroup & gizmogroup type to store static properties in */
  dummywg.type = &dummywgt;
  dummywgt.name = temp_buffers.name;
  dummywgt.idname = temp_buffers.idname;

  RNA_pointer_create(NULL, &RNA_GizmoGroup, &dummywg, &wgptr);

  /* Clear so we can detect if it's left unset. */
  temp_buffers.idname[0] = temp_buffers.name[0] = '\0';

  /* validate the python class */
  if (validate(&wgptr, data, have_function) != 0) {
    return NULL;
  }

  if (strlen(identifier) >= sizeof(temp_buffers.idname)) {
    BKE_reportf(reports,
                RPT_ERROR,
                "Registering gizmogroup class: '%s' is too long, maximum length is %d",
                identifier,
                (int)sizeof(temp_buffers.idname));
    return NULL;
  }

  /* check if the area supports widgets */
  const struct wmGizmoMapType_Params wmap_params = {
      .spaceid = dummywgt.gzmap_params.spaceid,
      .regionid = dummywgt.gzmap_params.regionid,
  };

  wmGizmoMapType *gzmap_type = WM_gizmomaptype_ensure(&wmap_params);
  if (gzmap_type == NULL) {
    BKE_report(reports, RPT_ERROR, "Area type does not support gizmos");
    return NULL;
  }

  /* check if we have registered this gizmogroup type before, and remove it */
  {
    wmGizmoGroupType *gzgt = WM_gizmogrouptype_find(dummywgt.idname, true);
    if (gzgt && gzgt->rna_ext.srna) {
      rna_GizmoGroup_unregister(bmain, gzgt->rna_ext.srna);
    }
  }
  if (!RNA_struct_available_or_report(reports, dummywgt.idname)) {
    return NULL;
  }

  { /* allocate the idname */
    const char *strings[] = {
        temp_buffers.idname,
        temp_buffers.name,
    };
    char *strings_table[ARRAY_SIZE(strings)];
    BLI_string_join_array_by_sep_char_with_tableN(
        '\0', strings_table, strings, ARRAY_SIZE(strings));

    dummywgt.idname = strings_table[0]; /* allocated string stored here */
    dummywgt.name = strings_table[1];
    BLI_assert(ARRAY_SIZE(strings) == 2);
  }

  /* create a new gizmogroup type */
  dummywgt.rna_ext.srna = RNA_def_struct_ptr(&BLENDER_RNA, dummywgt.idname, &RNA_GizmoGroup);

  /* Gizmo group properties are registered separately. */
  RNA_def_struct_flag(dummywgt.rna_ext.srna, STRUCT_NO_IDPROPERTIES);

  dummywgt.rna_ext.data = data;
  dummywgt.rna_ext.call = call;
  dummywgt.rna_ext.free = free;

  /* We used to register widget group types like this, now we do it similar to
   * operator types. Thus we should be able to do the same as operator types now. */
  dummywgt.poll = (have_function[0]) ? rna_gizmogroup_poll_cb : NULL;
  dummywgt.setup_keymap = (have_function[1]) ? rna_gizmogroup_setup_keymap_cb : NULL;
  dummywgt.setup = (have_function[2]) ? rna_gizmogroup_setup_cb : NULL;
  dummywgt.refresh = (have_function[3]) ? rna_gizmogroup_refresh_cb : NULL;
  dummywgt.draw_prepare = (have_function[4]) ? rna_gizmogroup_draw_prepare_cb : NULL;
  dummywgt.invoke_prepare = (have_function[5]) ? rna_gizmogroup_invoke_prepare_cb : NULL;

  wmGizmoGroupType *gzgt = WM_gizmogrouptype_append_ptr(BPY_RNA_gizmogroup_wrapper,
                                                        (void *)&dummywgt);

  {
    const char *owner_id = RNA_struct_state_owner_get();
    if (owner_id) {
      BLI_strncpy(gzgt->owner_id, owner_id, sizeof(gzgt->owner_id));
    }
  }

  if (gzgt->flag & WM_GIZMOGROUPTYPE_PERSISTENT) {
    WM_gizmo_group_type_add_ptr_ex(gzgt, gzmap_type);

    /* update while blender is running */
    WM_main_add_notifier(NC_SCREEN | NA_EDITED, NULL);
  }

  return dummywgt.rna_ext.srna;
}

static void rna_GizmoGroup_unregister(struct Main *bmain, StructRNA *type)
{
  wmGizmoGroupType *gzgt = RNA_struct_blender_type_get(type);

  if (!gzgt) {
    return;
  }

  WM_gizmo_group_type_remove_ptr(bmain, gzgt);

  /* Free extension after removing instances so `__del__` doesn't crash, see: T85567. */
  RNA_struct_free_extension(type, &gzgt->rna_ext);
  RNA_struct_free(&BLENDER_RNA, type);

  /* Free gizmo group after the extension as it owns the identifier memory. */
  WM_gizmo_group_type_free_ptr(gzgt);

  WM_main_add_notifier(NC_SCREEN | NA_EDITED, NULL);
}

static void **rna_GizmoGroup_instance(PointerRNA *ptr)
{
  wmGizmoGroup *gzgroup = ptr->data;
  return &gzgroup->py_instance;
}

#  endif /* WITH_PYTHON */

static StructRNA *rna_GizmoGroup_refine(PointerRNA *gzgroup_ptr)
{
  wmGizmoGroup *gzgroup = gzgroup_ptr->data;
  return (gzgroup->type && gzgroup->type->rna_ext.srna) ? gzgroup->type->rna_ext.srna :
                                                          &RNA_GizmoGroup;
}

static void rna_GizmoGroup_gizmos_begin(CollectionPropertyIterator *iter, PointerRNA *gzgroup_ptr)
{
  wmGizmoGroup *gzgroup = gzgroup_ptr->data;
  rna_iterator_listbase_begin(iter, &gzgroup->gizmos, NULL);
}

/** \} */

#else /* RNA_RUNTIME */

/* GizmoGroup.gizmos */
static void rna_def_gizmos(BlenderRNA *brna, PropertyRNA *cprop)
{
  StructRNA *srna;

  FunctionRNA *func;
  PropertyRNA *parm;

  RNA_def_property_srna(cprop, "Gizmos");
  srna = RNA_def_struct(brna, "Gizmos", NULL);
  RNA_def_struct_sdna(srna, "wmGizmoGroup");
  RNA_def_struct_ui_text(srna, "Gizmos", "Collection of gizmos");

  func = RNA_def_function(srna, "new", "rna_GizmoGroup_gizmo_new");
  RNA_def_function_ui_description(func, "Add gizmo");
  RNA_def_function_flag(func, FUNC_USE_REPORTS);
  parm = RNA_def_string(func, "type", "Type", 0, "", "Gizmo identifier"); /* optional */
  RNA_def_parameter_flags(parm, 0, PARM_REQUIRED);
  parm = RNA_def_pointer(func, "gizmo", "Gizmo", "", "New gizmo");
  RNA_def_function_return(func, parm);

  func = RNA_def_function(srna, "remove", "rna_GizmoGroup_gizmo_remove");
  RNA_def_function_flag(func, FUNC_USE_CONTEXT);
  RNA_def_function_ui_description(func, "Delete gizmo");
  parm = RNA_def_pointer(func, "gizmo", "Gizmo", "", "New gizmo");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED);
  RNA_def_parameter_clear_flags(parm, PROP_THICK_WRAP, 0);

  func = RNA_def_function(srna, "clear", "rna_GizmoGroup_gizmo_clear");
  RNA_def_function_flag(func, FUNC_USE_CONTEXT);
  RNA_def_function_ui_description(func, "Delete all gizmos");
}

static void rna_def_gizmo(BlenderRNA *brna, PropertyRNA *cprop)
{
  StructRNA *srna;
  PropertyRNA *prop;

  FunctionRNA *func;
  PropertyRNA *parm;

  RNA_def_property_srna(cprop, "Gizmo");
  srna = RNA_def_struct(brna, "Gizmo", NULL);
  RNA_def_struct_sdna(srna, "wmGizmo");
  RNA_def_struct_ui_text(srna, "Gizmo", "Collection of gizmos");
  RNA_def_struct_refine_func(srna, "rna_Gizmo_refine");

#  ifdef WITH_PYTHON
  RNA_def_struct_register_funcs(
      srna, "rna_Gizmo_register", "rna_Gizmo_unregister", "rna_Gizmo_instance");
#  endif
  RNA_def_struct_translation_context(srna, BLT_I18NCONTEXT_OPERATOR_DEFAULT);

  prop = RNA_def_property(srna, "properties", PROP_POINTER, PROP_NONE);
  RNA_def_property_flag(prop, PROP_NEVER_NULL);
  RNA_def_property_struct_type(prop, "GizmoProperties");
  RNA_def_property_ui_text(prop, "Properties", "");
  RNA_def_property_pointer_funcs(prop, "rna_Gizmo_properties_get", NULL, NULL, NULL);

  /* -------------------------------------------------------------------- */
  /* Registerable Variables */

  RNA_define_verify_sdna(0); /* not in sdna */

  prop = RNA_def_property(srna, "bl_idname", PROP_STRING, PROP_NONE);
  RNA_def_property_string_sdna(prop, NULL, "type->idname");
  RNA_def_property_string_maxlength(prop, MAX_NAME);
  RNA_def_property_string_funcs(prop, NULL, NULL, "rna_Gizmo_bl_idname_set");
  // RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_flag(prop, PROP_REGISTER);

  RNA_define_verify_sdna(1); /* not in sdna */

  /* wmGizmo.draw */
  func = RNA_def_function(srna, "draw", NULL);
  RNA_def_function_ui_description(func, "");
  RNA_def_function_flag(func, FUNC_REGISTER);
  parm = RNA_def_pointer(func, "context", "Context", "", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED);

  /* wmGizmo.draw_select */
  func = RNA_def_function(srna, "draw_select", NULL);
  RNA_def_function_ui_description(func, "");
  RNA_def_function_flag(func, FUNC_REGISTER_OPTIONAL);
  parm = RNA_def_pointer(func, "context", "Context", "", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED);
  parm = RNA_def_int(func, "select_id", 0, 0, INT_MAX, "", "", 0, INT_MAX);

  /* wmGizmo.test_select */
  func = RNA_def_function(srna, "test_select", NULL);
  RNA_def_function_ui_description(func, "");
  RNA_def_function_flag(func, FUNC_REGISTER_OPTIONAL);
  parm = RNA_def_pointer(func, "context", "Context", "", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED);
  parm = RNA_def_int_array(func,
                           "location",
                           2,
                           NULL,
                           INT_MIN,
                           INT_MAX,
                           "Location",
                           "Region coordinates",
                           INT_MIN,
                           INT_MAX);
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED);
  parm = RNA_def_int(
      func, "intersect_id", -1, -1, INT_MAX, "", "Use -1 to skip this gizmo", -1, INT_MAX);
  RNA_def_function_return(func, parm);

  /* wmGizmo.handler */
  static EnumPropertyItem tweak_actions[] = {
      {WM_GIZMO_TWEAK_PRECISE, "PRECISE", 0, "Precise", ""},
      {WM_GIZMO_TWEAK_SNAP, "SNAP", 0, "Snap", ""},
      {0, NULL, 0, NULL, NULL},
  };
  func = RNA_def_function(srna, "modal", NULL);
  RNA_def_function_ui_description(func, "");
  RNA_def_function_flag(func, FUNC_REGISTER_OPTIONAL | FUNC_ALLOW_WRITE);
  parm = RNA_def_pointer(func, "context", "Context", "", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED);
  parm = RNA_def_pointer(func, "event", "Event", "", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED);
  /* TODO: should be a enum-flag. */
  parm = RNA_def_enum_flag(func, "tweak", tweak_actions, 0, "Tweak", "");
  RNA_def_parameter_flags(parm, 0, PARM_REQUIRED);
  parm = RNA_def_enum_flag(
      func, "result", rna_enum_operator_return_items, OPERATOR_FINISHED, "result", "");
  RNA_def_function_return(func, parm);
  /* wmGizmo.property_update */
  /* TODO */

  /* wmGizmo.setup */
  func = RNA_def_function(srna, "setup", NULL);
  RNA_def_function_ui_description(func, "");
  RNA_def_function_flag(func, FUNC_REGISTER_OPTIONAL | FUNC_ALLOW_WRITE);

  /* wmGizmo.invoke */
  func = RNA_def_function(srna, "invoke", NULL);
  RNA_def_function_ui_description(func, "");
  RNA_def_function_flag(func, FUNC_REGISTER_OPTIONAL | FUNC_ALLOW_WRITE);
  parm = RNA_def_pointer(func, "context", "Context", "", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED);
  parm = RNA_def_pointer(func, "event", "Event", "", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED);
  parm = RNA_def_enum_flag(
      func, "result", rna_enum_operator_return_items, OPERATOR_FINISHED, "result", "");
  RNA_def_function_return(func, parm);

  /* wmGizmo.exit */
  func = RNA_def_function(srna, "exit", NULL);
  RNA_def_function_ui_description(func, "");
  RNA_def_function_flag(func, FUNC_REGISTER_OPTIONAL | FUNC_ALLOW_WRITE);
  parm = RNA_def_pointer(func, "context", "Context", "", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED);
  parm = RNA_def_boolean(func, "cancel", 0, "Cancel, otherwise confirm", "");
  RNA_def_parameter_flags(parm, 0, PARM_REQUIRED);

  /* wmGizmo.cursor_get */
  /* TODO */

  /* wmGizmo.select_refresh */
  func = RNA_def_function(srna, "select_refresh", NULL);
  RNA_def_function_ui_description(func, "");
  RNA_def_function_flag(func, FUNC_REGISTER_OPTIONAL | FUNC_ALLOW_WRITE);

  /* -------------------------------------------------------------------- */
  /* Instance Variables */

  prop = RNA_def_property(srna, "group", PROP_POINTER, PROP_NONE);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_struct_type(prop, "GizmoGroup");
  RNA_def_property_pointer_funcs(prop, "rna_Gizmo_group_get", NULL, NULL, NULL);
  RNA_def_property_ui_text(prop, "", "Gizmo group this gizmo is a member of");

  /* Color & Alpha */
  prop = RNA_def_property(srna, "color", PROP_FLOAT, PROP_COLOR);
  RNA_def_property_array(prop, 3);
  RNA_def_property_float_funcs(prop, "rna_Gizmo_color_get", "rna_Gizmo_color_set", NULL);

  prop = RNA_def_property(srna, "alpha", PROP_FLOAT, PROP_NONE);
  RNA_def_property_ui_text(prop, "Alpha", "");
  RNA_def_property_float_funcs(prop, "rna_Gizmo_alpha_get", "rna_Gizmo_alpha_set", NULL);
  RNA_def_property_range(prop, 0.0f, 1.0f);
  RNA_def_property_update(prop, 0, "rna_Gizmo_update_redraw");

  /* Color & Alpha (highlight) */
  prop = RNA_def_property(srna, "color_highlight", PROP_FLOAT, PROP_COLOR);
  RNA_def_property_array(prop, 3);
  RNA_def_property_float_funcs(prop, "rna_Gizmo_color_hi_get", "rna_Gizmo_color_hi_set", NULL);

  prop = RNA_def_property(srna, "alpha_highlight", PROP_FLOAT, PROP_NONE);
  RNA_def_property_ui_text(prop, "Alpha", "");
  RNA_def_property_float_funcs(prop, "rna_Gizmo_alpha_hi_get", "rna_Gizmo_alpha_hi_set", NULL);
  RNA_def_property_range(prop, 0.0f, 1.0f);
  RNA_def_property_update(prop, 0, "rna_Gizmo_update_redraw");

  prop = RNA_def_property(srna, "matrix_space", PROP_FLOAT, PROP_MATRIX);
  RNA_def_property_multi_array(prop, 2, rna_matrix_dimsize_4x4);
  RNA_def_property_ui_text(prop, "Space Matrix", "");
  RNA_def_property_float_funcs(
      prop, "rna_Gizmo_matrix_space_get", "rna_Gizmo_matrix_space_set", NULL);
  RNA_def_property_update(prop, 0, "rna_Gizmo_update_redraw");

  prop = RNA_def_property(srna, "matrix_basis", PROP_FLOAT, PROP_MATRIX);
  RNA_def_property_multi_array(prop, 2, rna_matrix_dimsize_4x4);
  RNA_def_property_ui_text(prop, "Basis Matrix", "");
  RNA_def_property_float_funcs(
      prop, "rna_Gizmo_matrix_basis_get", "rna_Gizmo_matrix_basis_set", NULL);
  RNA_def_property_update(prop, 0, "rna_Gizmo_update_redraw");

  prop = RNA_def_property(srna, "matrix_offset", PROP_FLOAT, PROP_MATRIX);
  RNA_def_property_multi_array(prop, 2, rna_matrix_dimsize_4x4);
  RNA_def_property_ui_text(prop, "Offset Matrix", "");
  RNA_def_property_float_funcs(
      prop, "rna_Gizmo_matrix_offset_get", "rna_Gizmo_matrix_offset_set", NULL);
  RNA_def_property_update(prop, 0, "rna_Gizmo_update_redraw");

  prop = RNA_def_property(srna, "matrix_world", PROP_FLOAT, PROP_MATRIX);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_multi_array(prop, 2, rna_matrix_dimsize_4x4);
  RNA_def_property_ui_text(prop, "Final World Matrix", "");
  RNA_def_property_float_funcs(prop, "rna_Gizmo_matrix_world_get", NULL, NULL);

  prop = RNA_def_property(srna, "scale_basis", PROP_FLOAT, PROP_NONE);
  RNA_def_property_ui_text(prop, "Scale Basis", "");
  RNA_def_property_float_funcs(
      prop, "rna_Gizmo_scale_basis_get", "rna_Gizmo_scale_basis_set", NULL);
  RNA_def_property_range(prop, 0.0f, FLT_MAX);
  RNA_def_property_update(prop, 0, "rna_Gizmo_update_redraw");

  prop = RNA_def_property(srna, "line_width", PROP_FLOAT, PROP_PIXEL);
  RNA_def_property_ui_text(prop, "Line Width", "");
  RNA_def_property_float_funcs(prop, "rna_Gizmo_line_width_get", "rna_Gizmo_line_width_set", NULL);
  RNA_def_property_range(prop, 0.0f, FLT_MAX);
  RNA_def_property_update(prop, 0, "rna_Gizmo_update_redraw");

  prop = RNA_def_property(srna, "select_bias", PROP_FLOAT, PROP_NONE);
  RNA_def_property_ui_text(prop, "Select Bias", "Depth bias used for selection");
  RNA_def_property_float_funcs(
      prop, "rna_Gizmo_select_bias_get", "rna_Gizmo_select_bias_set", NULL);
  RNA_def_property_range(prop, -FLT_MAX, FLT_MAX);

  /* wmGizmo.flag */
  /* WM_GIZMO_HIDDEN */
  prop = RNA_def_property(srna, "hide", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(prop, "rna_Gizmo_flag_hide_get", "rna_Gizmo_flag_hide_set");
  RNA_def_property_ui_text(prop, "Hide", "");
  RNA_def_property_update(prop, 0, "rna_Gizmo_update_redraw");
  /* WM_GIZMO_HIDDEN_SELECT */
  prop = RNA_def_property(srna, "hide_select", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(
      prop, "rna_Gizmo_flag_hide_select_get", "rna_Gizmo_flag_hide_select_set");
  RNA_def_property_ui_text(prop, "Hide Select", "");
  RNA_def_property_update(prop, 0, "rna_Gizmo_update_redraw");
  /* WM_GIZMO_HIDDEN_KEYMAP */
  prop = RNA_def_property(srna, "hide_keymap", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(
      prop, "rna_Gizmo_flag_hide_keymap_get", "rna_Gizmo_flag_hide_keymap_set");
  RNA_def_property_ui_text(prop, "Hide Keymap", "Ignore the key-map for this gizmo");
  RNA_def_property_update(prop, 0, "rna_Gizmo_update_redraw");
  /* WM_GIZMO_MOVE_CURSOR */
  prop = RNA_def_property(srna, "use_grab_cursor", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(
      prop, "rna_Gizmo_flag_use_grab_cursor_get", "rna_Gizmo_flag_use_grab_cursor_set");
  RNA_def_property_ui_text(prop, "Grab Cursor", "");
  RNA_def_property_update(prop, 0, "rna_Gizmo_update_redraw");

  /* WM_GIZMO_DRAW_HOVER */
  prop = RNA_def_property(srna, "use_draw_hover", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(
      prop, "rna_Gizmo_flag_use_draw_hover_get", "rna_Gizmo_flag_use_draw_hover_set");
  RNA_def_property_ui_text(prop, "Show Hover", "");
  RNA_def_property_update(prop, 0, "rna_Gizmo_update_redraw");
  /* WM_GIZMO_DRAW_MODAL */
  prop = RNA_def_property(srna, "use_draw_modal", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(
      prop, "rna_Gizmo_flag_use_draw_modal_get", "rna_Gizmo_flag_use_draw_modal_set");
  RNA_def_property_ui_text(prop, "Show Active", "Show while dragging");
  RNA_def_property_update(prop, 0, "rna_Gizmo_update_redraw");
  /* WM_GIZMO_DRAW_VALUE */
  prop = RNA_def_property(srna, "use_draw_value", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(
      prop, "rna_Gizmo_flag_use_draw_value_get", "rna_Gizmo_flag_use_draw_value_set");
  RNA_def_property_ui_text(
      prop, "Show Value", "Show an indicator for the current value while dragging");
  RNA_def_property_update(prop, 0, "rna_Gizmo_update_redraw");
  /* WM_GIZMO_DRAW_OFFSET_SCALE */
  prop = RNA_def_property(srna, "use_draw_offset_scale", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(prop,
                                 "rna_Gizmo_flag_use_draw_offset_scale_get",
                                 "rna_Gizmo_flag_use_draw_offset_scale_set");
  RNA_def_property_ui_text(
      prop, "Scale Offset", "Scale the offset matrix (use to apply screen-space offset)");
  RNA_def_property_update(prop, 0, "rna_Gizmo_update_redraw");
  /* WM_GIZMO_DRAW_NO_SCALE (negated) */
  prop = RNA_def_property(srna, "use_draw_scale", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(
      prop, "rna_Gizmo_flag_use_draw_scale_get", "rna_Gizmo_flag_use_draw_scale_set");
  RNA_def_property_ui_text(prop, "Scale", "Use scale when calculating the matrix");
  RNA_def_property_update(prop, 0, "rna_Gizmo_update_redraw");
  /* WM_GIZMO_SELECT_BACKGROUND */
  prop = RNA_def_property(srna, "use_select_background", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(prop,
                                 "rna_Gizmo_flag_use_select_background_get",
                                 "rna_Gizmo_flag_use_select_background_set");
  RNA_def_property_ui_text(prop, "Select Background", "Don't write into the depth buffer");
  RNA_def_property_update(prop, 0, "rna_Gizmo_update_redraw");

  /* WM_GIZMO_OPERATOR_TOOL_INIT */
  prop = RNA_def_property(srna, "use_operator_tool_properties", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(prop,
                                 "rna_Gizmo_flag_use_operator_tool_properties_get",
                                 "rna_Gizmo_flag_use_operator_tool_properties_set");
  RNA_def_property_ui_text(
      prop,
      "Tool Property Init",
      "Merge active tool properties on activation (does not overwrite existing)");
  RNA_def_property_update(prop, 0, "rna_Gizmo_update_redraw");

  /* WM_GIZMO_EVENT_HANDLE_ALL */
  prop = RNA_def_property(srna, "use_event_handle_all", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(
      prop, "rna_Gizmo_flag_use_event_handle_all_get", "rna_Gizmo_flag_use_event_handle_all_set");
  RNA_def_property_ui_text(prop,
                           "Handle All Events",
                           "When highlighted, "
                           "do not pass events through to be handled by other keymaps");
  RNA_def_property_update(prop, 0, "rna_Gizmo_update_redraw");

  /* WM_GIZMO_NO_TOOLTIP (negated) */
  prop = RNA_def_property(srna, "use_tooltip", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(
      prop, "rna_Gizmo_flag_use_tooltip_get", "rna_Gizmo_flag_use_tooltip_set");
  RNA_def_property_ui_text(prop, "Use Tooltip", "Use tooltips when hovering over this gizmo");
  /* No update needed. */

  /* wmGizmo.state (readonly) */
  /* WM_GIZMO_STATE_HIGHLIGHT */
  prop = RNA_def_property(srna, "is_highlight", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(prop, "rna_Gizmo_state_is_highlight_get", NULL);
  RNA_def_property_ui_text(prop, "Highlight", "");
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  /* WM_GIZMO_STATE_MODAL */
  prop = RNA_def_property(srna, "is_modal", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(prop, "rna_Gizmo_state_is_modal_get", NULL);
  RNA_def_property_ui_text(prop, "Highlight", "");
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  /* WM_GIZMO_STATE_SELECT */
  /* (note that setting is involved, needs to handle array) */
  prop = RNA_def_property(srna, "select", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(prop, "rna_Gizmo_state_select_get", "rna_Gizmo_state_select_set");
  RNA_def_property_ui_text(prop, "Select", "");

  RNA_api_gizmo(srna);

  srna = RNA_def_struct(brna, "GizmoProperties", NULL);
  RNA_def_struct_ui_text(srna, "Gizmo Properties", "Input properties of an Gizmo");
  RNA_def_struct_refine_func(srna, "rna_GizmoProperties_refine");
  RNA_def_struct_idprops_func(srna, "rna_GizmoProperties_idprops");
  RNA_def_struct_flag(srna, STRUCT_NO_DATABLOCK_IDPROPERTIES);
}

static void rna_def_gizmogroup(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  FunctionRNA *func;
  PropertyRNA *parm;

  srna = RNA_def_struct(brna, "GizmoGroup", NULL);
  RNA_def_struct_ui_text(
      srna, "GizmoGroup", "Storage of an operator being executed, or registered after execution");
  RNA_def_struct_sdna(srna, "wmGizmoGroup");
  RNA_def_struct_refine_func(srna, "rna_GizmoGroup_refine");
#  ifdef WITH_PYTHON
  RNA_def_struct_register_funcs(
      srna, "rna_GizmoGroup_register", "rna_GizmoGroup_unregister", "rna_GizmoGroup_instance");
#  endif
  RNA_def_struct_translation_context(srna, BLT_I18NCONTEXT_OPERATOR_DEFAULT);

  /* -------------------------------------------------------------------- */
  /* Registration */

  RNA_define_verify_sdna(0); /* not in sdna */

  prop = RNA_def_property(srna, "bl_idname", PROP_STRING, PROP_NONE);
  RNA_def_property_string_sdna(prop, NULL, "type->idname");
  RNA_def_property_string_maxlength(prop, MAX_NAME);
  RNA_def_property_string_funcs(prop, NULL, NULL, "rna_GizmoGroup_bl_idname_set");
  RNA_def_property_flag(prop, PROP_REGISTER);
  RNA_def_struct_name_property(srna, prop);

  prop = RNA_def_property(srna, "bl_label", PROP_STRING, PROP_NONE);
  RNA_def_property_string_sdna(prop, NULL, "type->name");
  RNA_def_property_string_maxlength(prop, MAX_NAME); /* else it uses the pointer size! */
  RNA_def_property_string_funcs(prop, NULL, NULL, "rna_GizmoGroup_bl_label_set");
  // RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_flag(prop, PROP_REGISTER);

  prop = RNA_def_property(srna, "bl_space_type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, NULL, "type->gzmap_params.spaceid");
  RNA_def_property_enum_items(prop, rna_enum_space_type_items);
  RNA_def_property_flag(prop, PROP_REGISTER);
  RNA_def_property_ui_text(prop, "Space Type", "The space where the panel is going to be used in");

  prop = RNA_def_property(srna, "bl_region_type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, NULL, "type->gzmap_params.regionid");
  RNA_def_property_enum_items(prop, rna_enum_region_type_items);
  RNA_def_property_flag(prop, PROP_REGISTER);
  RNA_def_property_ui_text(
      prop, "Region Type", "The region where the panel is going to be used in");

  prop = RNA_def_property(srna, "bl_owner_id", PROP_STRING, PROP_NONE);
  RNA_def_property_string_sdna(prop, NULL, "type->owner_id");
  RNA_def_property_flag(prop, PROP_REGISTER_OPTIONAL);

  /* bl_options */
  static EnumPropertyItem gizmogroup_flag_items[] = {
      {WM_GIZMOGROUPTYPE_3D, "3D", 0, "3D", "Use in 3D viewport"},
      {WM_GIZMOGROUPTYPE_SCALE,
       "SCALE",
       0,
       "Scale",
       "Scale to respect zoom (otherwise zoom independent display size)"},
      {WM_GIZMOGROUPTYPE_DEPTH_3D,
       "DEPTH_3D",
       0,
       "Depth 3D",
       "Supports culled depth by other objects in the view"},
      {WM_GIZMOGROUPTYPE_SELECT, "SELECT", 0, "Select", "Supports selection"},
      {WM_GIZMOGROUPTYPE_PERSISTENT, "PERSISTENT", 0, "Persistent", ""},
      {WM_GIZMOGROUPTYPE_DRAW_MODAL_ALL,
       "SHOW_MODAL_ALL",
       0,
       "Show Modal All",
       "Show all while interacting, as well as this group when another is being interacted with"},
      {WM_GIZMOGROUPTYPE_DRAW_MODAL_EXCLUDE,
       "EXCLUDE_MODAL",
       0,
       "Exclude Modal",
       "Show all except this group while interacting"},
      {WM_GIZMOGROUPTYPE_TOOL_INIT,
       "TOOL_INIT",
       0,
       "Tool Init",
       "Postpone running until tool operator run (when used with a tool)"},
      {WM_GIZMOGROUPTYPE_TOOL_FALLBACK_KEYMAP,
       "TOOL_FALLBACK_KEYMAP",
       0,
       "Use fallback tools keymap",
       "Add fallback tools keymap to this gizmo type"},
      {WM_GIZMOGROUPTYPE_VR_REDRAWS,
       "VR_REDRAWS",
       0,
       "VR Redraws",
       "The gizmos are made for use with virtual reality sessions and require special redraw "
       "management"},
      {0, NULL, 0, NULL, NULL},
  };
  prop = RNA_def_property(srna, "bl_options", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, NULL, "type->flag");
  RNA_def_property_enum_items(prop, gizmogroup_flag_items);
  RNA_def_property_flag(prop, PROP_REGISTER_OPTIONAL | PROP_ENUM_FLAG);
  RNA_def_property_ui_text(prop, "Options", "Options for this operator type");

  RNA_define_verify_sdna(1); /* not in sdna */

  /* Functions */

  /* poll */
  func = RNA_def_function(srna, "poll", NULL);
  RNA_def_function_ui_description(func, "Test if the gizmo group can be called or not");
  RNA_def_function_flag(func, FUNC_NO_SELF | FUNC_REGISTER_OPTIONAL);
  RNA_def_function_return(func, RNA_def_boolean(func, "visible", 1, "", ""));
  parm = RNA_def_pointer(func, "context", "Context", "", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED);

  /* setup_keymap */
  func = RNA_def_function(srna, "setup_keymap", NULL);
  RNA_def_function_ui_description(
      func, "Initialize keymaps for this gizmo group, use fallback keymap when not present");
  RNA_def_function_flag(func, FUNC_NO_SELF | FUNC_REGISTER_OPTIONAL);
  parm = RNA_def_pointer(func, "keyconfig", "KeyConfig", "", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED);
  /* return */
  parm = RNA_def_pointer(func, "keymap", "KeyMap", "", "");
  RNA_def_property_flag(parm, PROP_NEVER_NULL);
  RNA_def_function_return(func, parm);

  /* setup */
  func = RNA_def_function(srna, "setup", NULL);
  RNA_def_function_ui_description(func, "Create gizmos function for the gizmo group");
  RNA_def_function_flag(func, FUNC_REGISTER);
  parm = RNA_def_pointer(func, "context", "Context", "", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED);

  /* refresh */
  func = RNA_def_function(srna, "refresh", NULL);
  RNA_def_function_ui_description(
      func, "Refresh data (called on common state changes such as selection)");
  RNA_def_function_flag(func, FUNC_REGISTER_OPTIONAL);
  parm = RNA_def_pointer(func, "context", "Context", "", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED);

  func = RNA_def_function(srna, "draw_prepare", NULL);
  RNA_def_function_ui_description(func, "Run before each redraw");
  RNA_def_function_flag(func, FUNC_REGISTER_OPTIONAL);
  parm = RNA_def_pointer(func, "context", "Context", "", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED);

  func = RNA_def_function(srna, "invoke_prepare", NULL);
  RNA_def_function_ui_description(func, "Run before invoke");
  RNA_def_function_flag(func, FUNC_REGISTER_OPTIONAL);
  parm = RNA_def_pointer(func, "context", "Context", "", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED);
  parm = RNA_def_pointer(func, "gizmo", "Gizmo", "", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED);

  /* -------------------------------------------------------------------- */
  /* Instance Variables */

  prop = RNA_def_property(srna, "name", PROP_STRING, PROP_NONE);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_string_funcs(
      prop, "rna_GizmoGroup_name_get", "rna_GizmoGroup_name_length", NULL);
  RNA_def_property_ui_text(prop, "Name", "");

  prop = RNA_def_property(srna, "has_reports", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE); /* this is 'virtual' property */
  RNA_def_property_boolean_funcs(prop, "rna_GizmoGroup_has_reports_get", NULL);
  RNA_def_property_ui_text(
      prop,
      "Has Reports",
      "GizmoGroup has a set of reports (warnings and errors) from last execution");

  RNA_define_verify_sdna(0); /* not in sdna */

  prop = RNA_def_property(srna, "gizmos", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_collection_sdna(prop, NULL, "gizmos", NULL);
  RNA_def_property_struct_type(prop, "Gizmo");
  RNA_def_property_collection_funcs(prop,
                                    "rna_GizmoGroup_gizmos_begin",
                                    "rna_iterator_listbase_next",
                                    "rna_iterator_listbase_end",
                                    "rna_iterator_listbase_get",
                                    NULL,
                                    NULL,
                                    NULL,
                                    NULL);

  RNA_def_property_ui_text(prop, "Gizmos", "List of gizmos in the Gizmo Map");
  rna_def_gizmo(brna, prop);
  rna_def_gizmos(brna, prop);

  RNA_define_verify_sdna(1); /* not in sdna */

  RNA_api_gizmogroup(srna);

  srna = RNA_def_struct(brna, "GizmoGroupProperties", NULL);
  RNA_def_struct_ui_text(srna, "Gizmo Group Properties", "Input properties of a Gizmo Group");
  RNA_def_struct_refine_func(srna, "rna_GizmoGroupProperties_refine");
  RNA_def_struct_idprops_func(srna, "rna_GizmoGroupProperties_idprops");
  RNA_def_struct_flag(srna, STRUCT_NO_DATABLOCK_IDPROPERTIES);
}

void RNA_def_wm_gizmo(BlenderRNA *brna)
{
  rna_def_gizmogroup(brna);
}

#endif /* RNA_RUNTIME */
