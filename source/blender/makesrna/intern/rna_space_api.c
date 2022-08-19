/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup RNA
 */

#include "DNA_object_types.h"

#include "RNA_access.h"
#include "RNA_define.h"

#include "rna_internal.h"

#ifdef RNA_RUNTIME

#  include "BKE_global.h"

#  include "ED_fileselect.h"
#  include "ED_screen.h"
#  include "ED_text.h"

int rna_object_type_visibility_icon_get_common(int object_type_exclude_viewport,
                                               const int *object_type_exclude_select)
{
  const int view_value = (object_type_exclude_viewport != 0);

  if (object_type_exclude_select) {
    /* Ignore selection values when view is off,
     * intent is to show if visible objects aren't selectable. */
    const int select_value = (*object_type_exclude_select & ~object_type_exclude_viewport) != 0;
    return ICON_VIS_SEL_11 + (view_value << 1) + select_value;
  }

  return view_value ? ICON_HIDE_ON : ICON_HIDE_OFF;
}

static void rna_RegionView3D_update(ID *id, RegionView3D *rv3d, bContext *C)
{
  bScreen *screen = (bScreen *)id;

  ScrArea *area;
  ARegion *region;

  area_region_from_regiondata(screen, rv3d, &area, &region);

  if (area && region && area->spacetype == SPACE_VIEW3D) {
    Main *bmain = CTX_data_main(C);
    View3D *v3d = area->spacedata.first;
    wmWindowManager *wm = CTX_wm_manager(C);
    wmWindow *win;

    for (win = wm->windows.first; win; win = win->next) {
      if (WM_window_get_active_screen(win) == screen) {
        Scene *scene = WM_window_get_active_scene(win);
        ViewLayer *view_layer = WM_window_get_active_view_layer(win);
        Depsgraph *depsgraph = BKE_scene_ensure_depsgraph(bmain, scene, view_layer);

        ED_view3d_update_viewmat(depsgraph, scene, v3d, region, NULL, NULL, NULL, false);
        break;
      }
    }
  }
}

static void rna_SpaceTextEditor_region_location_from_cursor(
    ID *id, SpaceText *st, int line, int column, int r_pixel_pos[2])
{
  bScreen *screen = (bScreen *)id;
  ScrArea *area = BKE_screen_find_area_from_space(screen, (SpaceLink *)st);
  if (area) {
    ARegion *region = BKE_area_find_region_type(area, RGN_TYPE_WINDOW);
    const int cursor_co[2] = {line, column};
    ED_text_region_location_from_cursor(st, region, cursor_co, r_pixel_pos);
  }
}

#else

void RNA_api_region_view3d(StructRNA *srna)
{
  FunctionRNA *func;

  func = RNA_def_function(srna, "update", "rna_RegionView3D_update");
  RNA_def_function_flag(func, FUNC_USE_SELF_ID | FUNC_USE_CONTEXT);
  RNA_def_function_ui_description(func, "Recalculate the view matrices");
}

void RNA_api_space_node(StructRNA *srna)
{
  FunctionRNA *func;
  PropertyRNA *parm;

  func = RNA_def_function(
      srna, "cursor_location_from_region", "rna_SpaceNodeEditor_cursor_location_from_region");
  RNA_def_function_ui_description(func, "Set the cursor location using region coordinates");
  RNA_def_function_flag(func, FUNC_USE_CONTEXT);
  parm = RNA_def_int(func, "x", 0, INT_MIN, INT_MAX, "x", "Region x coordinate", -10000, 10000);
  RNA_def_parameter_flags(parm, 0, PARM_REQUIRED);
  parm = RNA_def_int(func, "y", 0, INT_MIN, INT_MAX, "y", "Region y coordinate", -10000, 10000);
  RNA_def_parameter_flags(parm, 0, PARM_REQUIRED);
}

void RNA_api_space_text(StructRNA *srna)
{
  FunctionRNA *func;
  PropertyRNA *parm;

  func = RNA_def_function(
      srna, "region_location_from_cursor", "rna_SpaceTextEditor_region_location_from_cursor");
  RNA_def_function_ui_description(
      func, "Retrieve the region position from the given line and character position");
  RNA_def_function_flag(func, FUNC_USE_SELF_ID);
  parm = RNA_def_int(func, "line", 0, INT_MIN, INT_MAX, "Line", "Line index", 0, INT_MAX);
  RNA_def_parameter_flags(parm, 0, PARM_REQUIRED);
  parm = RNA_def_int(func, "column", 0, INT_MIN, INT_MAX, "Column", "Column index", 0, INT_MAX);
  RNA_def_parameter_flags(parm, 0, PARM_REQUIRED);
  parm = RNA_def_int_array(
      func, "result", 2, NULL, -1, INT_MAX, "", "Region coordinates", -1, INT_MAX);
  RNA_def_function_output(func, parm);
}

void rna_def_object_type_visibility_flags_common(StructRNA *srna, int noteflag)
{
  PropertyRNA *prop;

  struct {
    const char *name;
    int type_mask;
    const char *identifier[2];
  } info[] = {
      {"Mesh", (1 << OB_MESH), {"show_object_viewport_mesh", "show_object_select_mesh"}},
      {"Curve",
       (1 << OB_CURVES_LEGACY),
       {"show_object_viewport_curve", "show_object_select_curve"}},
      {"Surface", (1 << OB_SURF), {"show_object_viewport_surf", "show_object_select_surf"}},
      {"Meta", (1 << OB_MBALL), {"show_object_viewport_meta", "show_object_select_meta"}},
      {"Font", (1 << OB_FONT), {"show_object_viewport_font", "show_object_select_font"}},
      {"Hair Curves",
       (1 << OB_CURVES),
       {"show_object_viewport_curves", "show_object_select_curves"}},
      {"Point Cloud",
       (1 << OB_POINTCLOUD),
       {"show_object_viewport_pointcloud", "show_object_select_pointcloud"}},
      {"Volume", (1 << OB_VOLUME), {"show_object_viewport_volume", "show_object_select_volume"}},
      {"Armature",
       (1 << OB_ARMATURE),
       {"show_object_viewport_armature", "show_object_select_armature"}},
      {"Lattice",
       (1 << OB_LATTICE),
       {"show_object_viewport_lattice", "show_object_select_lattice"}},
      {"Empty", (1 << OB_EMPTY), {"show_object_viewport_empty", "show_object_select_empty"}},
      {"Grease Pencil",
       (1 << OB_GPENCIL),
       {"show_object_viewport_grease_pencil", "show_object_select_grease_pencil"}},
      {"Camera", (1 << OB_CAMERA), {"show_object_viewport_camera", "show_object_select_camera"}},
      {"Light", (1 << OB_LAMP), {"show_object_viewport_light", "show_object_select_light"}},
      {"Speaker",
       (1 << OB_SPEAKER),
       {"show_object_viewport_speaker", "show_object_select_speaker"}},
      {"Light Probe",
       (1 << OB_LIGHTPROBE),
       {"show_object_viewport_light_probe", "show_object_select_light_probe"}},
  };

  const char *view_mask_member[2] = {
      "object_type_exclude_viewport",
      "object_type_exclude_select",
  };
  for (int mask_index = 0; mask_index < 2; mask_index++) {
    for (int type_index = 0; type_index < ARRAY_SIZE(info); type_index++) {
      prop = RNA_def_property(
          srna, info[type_index].identifier[mask_index], PROP_BOOLEAN, PROP_NONE);
      RNA_def_property_boolean_negative_sdna(
          prop, NULL, view_mask_member[mask_index], info[type_index].type_mask);
      RNA_def_property_ui_text(prop, info[type_index].name, "");
      RNA_def_property_update(prop, noteflag, NULL);
    }
  }
}

void RNA_api_space_filebrowser(StructRNA *srna)
{
  FunctionRNA *func;
  PropertyRNA *parm;

  func = RNA_def_function(srna, "activate_asset_by_id", "ED_fileselect_activate_by_id");
  RNA_def_function_ui_description(
      func, "Activate and select the asset entry that represents the given ID");

  parm = RNA_def_property(func, "id_to_activate", PROP_POINTER, PROP_NONE);
  RNA_def_property_struct_type(parm, "ID");
  RNA_def_parameter_flags(parm, 0, PARM_REQUIRED);

  parm = RNA_def_boolean(
      func,
      "deferred",
      0,
      "",
      "Whether to activate the ID immediately (false) or after the file browser refreshes (true)");

  /* Select file by relative path. */
  func = RNA_def_function(
      srna, "activate_file_by_relative_path", "ED_fileselect_activate_by_relpath");
  RNA_def_function_ui_description(func,
                                  "Set active file and add to selection based on relative path to "
                                  "current File Browser directory");
  RNA_def_property(func, "relative_path", PROP_STRING, PROP_FILEPATH);

  /* Deselect all files. */
  func = RNA_def_function(srna, "deselect_all", "ED_fileselect_deselect_all");
  RNA_def_function_ui_description(func, "Deselect all files");
}

#endif
