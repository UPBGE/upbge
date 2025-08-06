/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editor/io
 */

#ifdef WITH_IO_PLY

#  include "BKE_context.hh"
#  include "BKE_file_handler.hh"
#  include "BKE_main.hh"
#  include "BKE_report.hh"

#  include "BLI_string.h"
#  include "BLI_string_utf8.h"

#  include "WM_api.hh"
#  include "WM_types.hh"

#  include "DNA_space_types.h"

#  include "ED_fileselect.hh"
#  include "ED_outliner.hh"

#  include "RNA_access.hh"
#  include "RNA_define.hh"

#  include "BLT_translation.hh"

#  include "UI_interface.hh"
#  include "UI_interface_layout.hh"
#  include "UI_resources.hh"

#  include "IO_orientation.hh"

#  include "IO_ply.hh"
#  include "io_ply_ops.hh"
#  include "io_utils.hh"

static const EnumPropertyItem ply_vertex_colors_mode[] = {
    {int(ePLYVertexColorMode::None), "NONE", 0, "None", "Do not import/export color attributes"},
    {int(ePLYVertexColorMode::sRGB),
     "SRGB",
     0,
     "sRGB",
     "Vertex colors in the file are in sRGB color space"},
    {int(ePLYVertexColorMode::Linear),
     "LINEAR",
     0,
     "Linear",
     "Vertex colors in the file are in linear color space"},
    {0, nullptr, 0, nullptr, nullptr}};

static wmOperatorStatus wm_ply_export_invoke(bContext *C,
                                             wmOperator *op,
                                             const wmEvent * /*event*/)
{
  ED_fileselect_ensure_default_filepath(C, op, ".ply");

  WM_event_add_fileselect(C, op);
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus wm_ply_export_exec(bContext *C, wmOperator *op)
{
  if (!RNA_struct_property_is_set_ex(op->ptr, "filepath", false)) {
    BKE_report(op->reports, RPT_ERROR, "No filepath given");
    return OPERATOR_CANCELLED;
  }
  PLYExportParams export_params;
  export_params.file_base_for_tests[0] = '\0';
  RNA_string_get(op->ptr, "filepath", export_params.filepath);
  export_params.blen_filepath = CTX_data_main(C)->filepath;

  export_params.forward_axis = eIOAxis(RNA_enum_get(op->ptr, "forward_axis"));
  export_params.up_axis = eIOAxis(RNA_enum_get(op->ptr, "up_axis"));
  export_params.global_scale = RNA_float_get(op->ptr, "global_scale");
  export_params.apply_modifiers = RNA_boolean_get(op->ptr, "apply_modifiers");

  export_params.export_selected_objects = RNA_boolean_get(op->ptr, "export_selected_objects");
  export_params.export_uv = RNA_boolean_get(op->ptr, "export_uv");
  export_params.export_normals = RNA_boolean_get(op->ptr, "export_normals");
  export_params.vertex_colors = ePLYVertexColorMode(RNA_enum_get(op->ptr, "export_colors"));
  export_params.export_attributes = RNA_boolean_get(op->ptr, "export_attributes");
  export_params.export_triangulated_mesh = RNA_boolean_get(op->ptr, "export_triangulated_mesh");
  export_params.ascii_format = RNA_boolean_get(op->ptr, "ascii_format");

  RNA_string_get(op->ptr, "collection", export_params.collection);

  export_params.reports = op->reports;

  PLY_export(C, export_params);

  if (BKE_reports_contain(op->reports, RPT_ERROR)) {
    return OPERATOR_CANCELLED;
  }

  BKE_report(op->reports, RPT_INFO, "File exported successfully");
  return OPERATOR_FINISHED;
}

static void wm_ply_export_draw(bContext *C, wmOperator *op)
{
  uiLayout *layout = op->layout;
  PointerRNA *ptr = op->ptr;

  layout->use_property_split_set(true);
  layout->use_property_decorate_set(false);

  if (uiLayout *panel = layout->panel(C, "PLY_export_general", false, IFACE_("General"))) {
    uiLayout *col = &panel->column(false);

    uiLayout *sub = &col->column(false, IFACE_("Format"));
    sub->prop(ptr, "ascii_format", UI_ITEM_NONE, IFACE_("ASCII"), ICON_NONE);

    /* The Selection only options only make sense when using regular export. */
    if (CTX_wm_space_file(C)) {
      sub = &col->column(false, IFACE_("Include"));
      sub->prop(ptr, "export_selected_objects", UI_ITEM_NONE, IFACE_("Selection Only"), ICON_NONE);
    }

    col->prop(ptr, "global_scale", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    col->prop(ptr, "forward_axis", UI_ITEM_NONE, IFACE_("Forward Axis"), ICON_NONE);
    col->prop(ptr, "up_axis", UI_ITEM_NONE, IFACE_("Up Axis"), ICON_NONE);
  }

  if (uiLayout *panel = layout->panel(C, "PLY_export_geometry", false, IFACE_("Geometry"))) {
    uiLayout *col = &panel->column(false);

    col->prop(ptr, "export_uv", UI_ITEM_NONE, IFACE_("UV Coordinates"), ICON_NONE);
    col->prop(ptr, "export_normals", UI_ITEM_NONE, IFACE_("Vertex Normals"), ICON_NONE);
    col->prop(ptr, "export_attributes", UI_ITEM_NONE, IFACE_("Vertex Attributes"), ICON_NONE);
    col->prop(ptr, "export_colors", UI_ITEM_NONE, IFACE_("Vertex Colors"), ICON_NONE);

    col->prop(
        ptr, "export_triangulated_mesh", UI_ITEM_NONE, IFACE_("Triangulated Mesh"), ICON_NONE);
    col->prop(ptr, "apply_modifiers", UI_ITEM_NONE, IFACE_("Apply Modifiers"), ICON_NONE);
  }
}

/**
 * Return true if any property in the UI is changed.
 */
static bool wm_ply_export_check(bContext * /*C*/, wmOperator *op)
{
  char filepath[FILE_MAX];
  bool changed = false;
  RNA_string_get(op->ptr, "filepath", filepath);

  if (!BLI_path_extension_check(filepath, ".ply")) {
    BLI_path_extension_ensure(filepath, FILE_MAX, ".ply");
    RNA_string_set(op->ptr, "filepath", filepath);
    changed = true;
  }
  return changed;
}

void WM_OT_ply_export(wmOperatorType *ot)
{
  PropertyRNA *prop;

  ot->name = "Export PLY";
  ot->description = "Save the scene to a PLY file";
  ot->idname = "WM_OT_ply_export";

  ot->invoke = wm_ply_export_invoke;
  ot->exec = wm_ply_export_exec;
  ot->poll = WM_operator_winactive;
  ot->ui = wm_ply_export_draw;
  ot->check = wm_ply_export_check;

  ot->flag = OPTYPE_PRESET;

  WM_operator_properties_filesel(ot,
                                 FILE_TYPE_FOLDER,
                                 FILE_BLENDER,
                                 FILE_SAVE,
                                 WM_FILESEL_FILEPATH | WM_FILESEL_SHOW_PROPS,
                                 FILE_DEFAULTDISPLAY,
                                 FILE_SORT_DEFAULT);

  /* Object transform options. */
  prop = RNA_def_enum(ot->srna, "forward_axis", io_transform_axis, IO_AXIS_Y, "Forward Axis", "");
  RNA_def_property_update_runtime(prop, io_ui_forward_axis_update);
  prop = RNA_def_enum(ot->srna, "up_axis", io_transform_axis, IO_AXIS_Z, "Up Axis", "");
  RNA_def_property_update_runtime(prop, io_ui_up_axis_update);
  RNA_def_float(
      ot->srna,
      "global_scale",
      1.0f,
      0.0001f,
      10000.0f,
      "Scale",
      "Value by which to enlarge or shrink the objects with respect to the world's origin",
      0.0001f,
      10000.0f);
  /* File Writer options. */
  RNA_def_boolean(
      ot->srna, "apply_modifiers", true, "Apply Modifiers", "Apply modifiers to exported meshes");
  RNA_def_boolean(ot->srna,
                  "export_selected_objects",
                  false,
                  "Export Selected Objects",
                  "Export only selected objects instead of all supported objects");
  prop = RNA_def_string(ot->srna,
                        "collection",
                        nullptr,
                        MAX_ID_NAME - 2,
                        "Source Collection",
                        "Export only objects from this collection (and its children)");
  RNA_def_property_flag(prop, PROP_HIDDEN);

  RNA_def_boolean(ot->srna, "export_uv", true, "Export UVs", "");
  RNA_def_boolean(
      ot->srna,
      "export_normals",
      false,
      "Export Vertex Normals",
      "Export specific vertex normals if available, export calculated normals otherwise");
  RNA_def_enum(ot->srna,
               "export_colors",
               ply_vertex_colors_mode,
               int(ePLYVertexColorMode::sRGB),
               "Export Vertex Colors",
               "Export vertex color attributes");
  RNA_def_boolean(ot->srna,
                  "export_attributes",
                  true,
                  "Export Vertex Attributes",
                  "Export custom vertex attributes");
  RNA_def_boolean(ot->srna,
                  "export_triangulated_mesh",
                  false,
                  "Export Triangulated Mesh",
                  "All ngons with four or more vertices will be triangulated. Meshes in "
                  "the scene will not be affected. Behaves like Triangulate Modifier with "
                  "ngon-method: \"Beauty\", quad-method: \"Shortest Diagonal\", min vertices: 4");
  RNA_def_boolean(ot->srna,
                  "ascii_format",
                  false,
                  "ASCII Format",
                  "Export file in ASCII format, export as binary otherwise");

  /* Only show `.ply` files by default. */
  prop = RNA_def_string(ot->srna, "filter_glob", "*.ply", 0, "Extension Filter", "");
  RNA_def_property_flag(prop, PROP_HIDDEN);
}

static wmOperatorStatus wm_ply_import_exec(bContext *C, wmOperator *op)
{
  PLYImportParams params;
  params.forward_axis = eIOAxis(RNA_enum_get(op->ptr, "forward_axis"));
  params.up_axis = eIOAxis(RNA_enum_get(op->ptr, "up_axis"));
  params.use_scene_unit = RNA_boolean_get(op->ptr, "use_scene_unit");
  params.global_scale = RNA_float_get(op->ptr, "global_scale");
  params.merge_verts = RNA_boolean_get(op->ptr, "merge_verts");
  params.import_attributes = RNA_boolean_get(op->ptr, "import_attributes");
  params.vertex_colors = ePLYVertexColorMode(RNA_enum_get(op->ptr, "import_colors"));

  params.reports = op->reports;

  const auto paths = blender::ed::io::paths_from_operator_properties(op->ptr);

  if (paths.is_empty()) {
    BKE_report(op->reports, RPT_ERROR, "No filepath given");
    return OPERATOR_CANCELLED;
  }
  for (const auto &path : paths) {
    STRNCPY(params.filepath, path.c_str());
    PLY_import(C, params);
  };

  Scene *scene = CTX_data_scene(C);
  WM_event_add_notifier(C, NC_SCENE | ND_OB_SELECT, scene);
  WM_event_add_notifier(C, NC_SCENE | ND_OB_ACTIVE, scene);
  WM_event_add_notifier(C, NC_SCENE | ND_LAYER_CONTENT, scene);
  ED_outliner_select_sync_from_object_tag(C);

  return OPERATOR_FINISHED;
}

static void ui_ply_import_settings(const bContext *C, uiLayout *layout, PointerRNA *ptr)
{
  layout->use_property_split_set(true);
  layout->use_property_decorate_set(false);

  if (uiLayout *panel = layout->panel(C, "PLY_import_general", false, IFACE_("General"))) {
    uiLayout *col = &panel->column(false);
    col->prop(ptr, "global_scale", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    col->prop(ptr, "use_scene_unit", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    col->prop(ptr, "forward_axis", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    col->prop(ptr, "up_axis", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }

  if (uiLayout *panel = layout->panel(C, "PLY_import_options", false, IFACE_("Options"))) {
    uiLayout *col = &panel->column(false);
    col->prop(ptr, "merge_verts", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    col->prop(ptr, "import_colors", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
}

static void wm_ply_import_draw(bContext *C, wmOperator *op)
{
  ui_ply_import_settings(C, op->layout, op->ptr);
}

void WM_OT_ply_import(wmOperatorType *ot)
{
  PropertyRNA *prop;

  ot->name = "Import PLY";
  ot->description = "Import an PLY file as an object";
  ot->idname = "WM_OT_ply_import";

  ot->invoke = blender::ed::io::filesel_drop_import_invoke;
  ot->exec = wm_ply_import_exec;
  ot->ui = wm_ply_import_draw;
  ot->poll = WM_operator_winactive;
  ot->flag = OPTYPE_UNDO | OPTYPE_PRESET;

  WM_operator_properties_filesel(ot,
                                 FILE_TYPE_FOLDER,
                                 FILE_BLENDER,
                                 FILE_OPENFILE,
                                 WM_FILESEL_FILEPATH | WM_FILESEL_FILES | WM_FILESEL_DIRECTORY |
                                     WM_FILESEL_SHOW_PROPS,
                                 FILE_DEFAULTDISPLAY,
                                 FILE_SORT_DEFAULT);

  RNA_def_float(ot->srna, "global_scale", 1.0f, 1e-6f, 1e6f, "Scale", "", 0.001f, 1000.0f);
  RNA_def_boolean(ot->srna,
                  "use_scene_unit",
                  false,
                  "Scene Unit",
                  "Apply current scene's unit (as defined by unit scale) to imported data");
  prop = RNA_def_enum(ot->srna, "forward_axis", io_transform_axis, IO_AXIS_Y, "Forward Axis", "");
  RNA_def_property_update_runtime(prop, io_ui_forward_axis_update);
  prop = RNA_def_enum(ot->srna, "up_axis", io_transform_axis, IO_AXIS_Z, "Up Axis", "");
  RNA_def_property_update_runtime(prop, io_ui_up_axis_update);
  RNA_def_boolean(ot->srna, "merge_verts", false, "Merge Vertices", "Merges vertices by distance");
  RNA_def_enum(ot->srna,
               "import_colors",
               ply_vertex_colors_mode,
               int(ePLYVertexColorMode::sRGB),
               "Vertex Colors",
               "Import vertex color attributes");
  RNA_def_boolean(
      ot->srna, "import_attributes", true, "Vertex Attributes", "Import custom vertex attributes");

  /* Only show `.ply` files by default. */
  prop = RNA_def_string(ot->srna, "filter_glob", "*.ply", 0, "Extension Filter", "");
  RNA_def_property_flag(prop, PROP_HIDDEN);
}

namespace blender::ed::io {
void ply_file_handler_add()
{
  auto fh = std::make_unique<blender::bke::FileHandlerType>();
  STRNCPY_UTF8(fh->idname, "IO_FH_ply");
  STRNCPY_UTF8(fh->import_operator, "WM_OT_ply_import");
  STRNCPY_UTF8(fh->export_operator, "WM_OT_ply_export");
  STRNCPY_UTF8(fh->label, "Stanford PLY");
  STRNCPY_UTF8(fh->file_extensions_str, ".ply");
  fh->poll_drop = poll_file_object_drop;
  bke::file_handler_add(std::move(fh));
}
}  // namespace blender::ed::io

#endif /* WITH_IO_PLY */
