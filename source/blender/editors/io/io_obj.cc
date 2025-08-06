/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editor/io
 */

#ifdef WITH_IO_WAVEFRONT_OBJ

#  include "DNA_space_types.h"

#  include "BKE_context.hh"
#  include "BKE_file_handler.hh"
#  include "BKE_main.hh"
#  include "BKE_report.hh"

#  include "BLI_path_utils.hh"
#  include "BLI_string.h"
#  include "BLI_string_utf8.h"

#  include "BLT_translation.hh"

#  include "ED_fileselect.hh"
#  include "ED_outliner.hh"

#  include "RNA_access.hh"
#  include "RNA_define.hh"

#  include "UI_interface.hh"
#  include "UI_interface_layout.hh"
#  include "UI_resources.hh"

#  include "WM_api.hh"
#  include "WM_types.hh"

#  include "DEG_depsgraph.hh"

#  include "IO_orientation.hh"
#  include "IO_path_util_types.hh"
#  include "IO_wavefront_obj.hh"

#  include "io_obj.hh"
#  include "io_utils.hh"

static const EnumPropertyItem io_obj_export_evaluation_mode[] = {
    {DAG_EVAL_RENDER, "DAG_EVAL_RENDER", 0, "Render", "Export objects as they appear in render"},
    {DAG_EVAL_VIEWPORT,
     "DAG_EVAL_VIEWPORT",
     0,
     "Viewport",
     "Export objects as they appear in the viewport"},
    {0, nullptr, 0, nullptr, nullptr}};

static const EnumPropertyItem io_obj_path_mode[] = {
    {PATH_REFERENCE_AUTO, "AUTO", 0, "Auto", "Use relative paths with subdirectories only"},
    {PATH_REFERENCE_ABSOLUTE, "ABSOLUTE", 0, "Absolute", "Always write absolute paths"},
    {PATH_REFERENCE_RELATIVE, "RELATIVE", 0, "Relative", "Write relative paths where possible"},
    {PATH_REFERENCE_MATCH, "MATCH", 0, "Match", "Match absolute/relative setting with input path"},
    {PATH_REFERENCE_STRIP, "STRIP", 0, "Strip", "Write filename only"},
    {PATH_REFERENCE_COPY, "COPY", 0, "Copy", "Copy the file to the destination path"},
    {0, nullptr, 0, nullptr, nullptr}};

static const EnumPropertyItem io_obj_mtl_name_collision_mode[] = {
    {OBJ_MTL_NAME_COLLISION_MAKE_UNIQUE,
     "MAKE_UNIQUE",
     0,
     "Make Unique",
     "Create new materials with unique names for each OBJ file"},
    {OBJ_MTL_NAME_COLLISION_REFERENCE_EXISTING,
     "REFERENCE_EXISTING",
     0,
     "Reference Existing",
     "Use existing materials with same name instead of creating new ones"},
    {0, nullptr, 0, nullptr, nullptr}};

static wmOperatorStatus wm_obj_export_invoke(bContext *C,
                                             wmOperator *op,
                                             const wmEvent * /*event*/)
{
  ED_fileselect_ensure_default_filepath(C, op, ".obj");

  WM_event_add_fileselect(C, op);
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus wm_obj_export_exec(bContext *C, wmOperator *op)
{
  if (!RNA_struct_property_is_set_ex(op->ptr, "filepath", false)) {
    BKE_report(op->reports, RPT_ERROR, "No filepath given");
    return OPERATOR_CANCELLED;
  }
  OBJExportParams export_params;
  export_params.file_base_for_tests[0] = '\0';
  RNA_string_get(op->ptr, "filepath", export_params.filepath);
  export_params.blen_filepath = CTX_data_main(C)->filepath;
  export_params.export_animation = RNA_boolean_get(op->ptr, "export_animation");
  export_params.start_frame = RNA_int_get(op->ptr, "start_frame");
  export_params.end_frame = RNA_int_get(op->ptr, "end_frame");

  export_params.forward_axis = eIOAxis(RNA_enum_get(op->ptr, "forward_axis"));
  export_params.up_axis = eIOAxis(RNA_enum_get(op->ptr, "up_axis"));
  export_params.global_scale = RNA_float_get(op->ptr, "global_scale");
  export_params.apply_modifiers = RNA_boolean_get(op->ptr, "apply_modifiers");
  export_params.export_eval_mode = eEvaluationMode(RNA_enum_get(op->ptr, "export_eval_mode"));

  export_params.export_selected_objects = RNA_boolean_get(op->ptr, "export_selected_objects");
  export_params.export_uv = RNA_boolean_get(op->ptr, "export_uv");
  export_params.export_normals = RNA_boolean_get(op->ptr, "export_normals");
  export_params.export_colors = RNA_boolean_get(op->ptr, "export_colors");
  export_params.export_materials = RNA_boolean_get(op->ptr, "export_materials");
  export_params.path_mode = ePathReferenceMode(RNA_enum_get(op->ptr, "path_mode"));
  export_params.export_triangulated_mesh = RNA_boolean_get(op->ptr, "export_triangulated_mesh");
  export_params.export_curves_as_nurbs = RNA_boolean_get(op->ptr, "export_curves_as_nurbs");
  export_params.export_pbr_extensions = RNA_boolean_get(op->ptr, "export_pbr_extensions");

  export_params.export_object_groups = RNA_boolean_get(op->ptr, "export_object_groups");
  export_params.export_material_groups = RNA_boolean_get(op->ptr, "export_material_groups");
  export_params.export_vertex_groups = RNA_boolean_get(op->ptr, "export_vertex_groups");
  export_params.export_smooth_groups = RNA_boolean_get(op->ptr, "export_smooth_groups");
  export_params.smooth_groups_bitflags = RNA_boolean_get(op->ptr, "smooth_group_bitflags");

  export_params.reports = op->reports;

  RNA_string_get(op->ptr, "collection", export_params.collection);

  OBJ_export(C, &export_params);

  if (BKE_reports_contain(op->reports, RPT_ERROR)) {
    return OPERATOR_CANCELLED;
  }

  BKE_report(op->reports, RPT_INFO, "File exported successfully");
  return OPERATOR_FINISHED;
}

static void ui_obj_export_settings(const bContext *C, uiLayout *layout, PointerRNA *ptr)
{
  const bool export_animation = RNA_boolean_get(ptr, "export_animation");
  const bool export_smooth_groups = RNA_boolean_get(ptr, "export_smooth_groups");
  const bool export_materials = RNA_boolean_get(ptr, "export_materials");

  layout->use_property_split_set(true);
  layout->use_property_decorate_set(false);

  /* Object General options. */
  if (uiLayout *panel = layout->panel(C, "OBJ_export_general", false, IFACE_("General"))) {
    uiLayout *col = &panel->column(false);

    if (CTX_wm_space_file(C)) {
      uiLayout *sub = &col->column(false, IFACE_("Include"));
      sub->prop(ptr, "export_selected_objects", UI_ITEM_NONE, IFACE_("Selection Only"), ICON_NONE);
    }

    col->prop(ptr, "global_scale", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    col->prop(ptr, "forward_axis", UI_ITEM_NONE, IFACE_("Forward Axis"), ICON_NONE);
    col->prop(ptr, "up_axis", UI_ITEM_NONE, IFACE_("Up Axis"), ICON_NONE);
  }

  /* Geometry options. */
  if (uiLayout *panel = layout->panel(C, "OBJ_export_geometry", false, IFACE_("Geometry"))) {
    uiLayout *col = &panel->column(false);
    col->prop(ptr, "export_uv", UI_ITEM_NONE, IFACE_("UV Coordinates"), ICON_NONE);
    col->prop(ptr, "export_normals", UI_ITEM_NONE, IFACE_("Normals"), ICON_NONE);
    col->prop(ptr, "export_colors", UI_ITEM_NONE, IFACE_("Colors"), ICON_NONE);
    col->prop(ptr, "export_curves_as_nurbs", UI_ITEM_NONE, IFACE_("Curves as NURBS"), ICON_NONE);

    col->prop(
        ptr, "export_triangulated_mesh", UI_ITEM_NONE, IFACE_("Triangulated Mesh"), ICON_NONE);
    col->prop(ptr, "apply_modifiers", UI_ITEM_NONE, IFACE_("Apply Modifiers"), ICON_NONE);
    col->prop(ptr, "export_eval_mode", UI_ITEM_NONE, IFACE_("Properties"), ICON_NONE);
  }

  /* Grouping options. */
  if (uiLayout *panel = layout->panel(C, "OBJ_export_grouping", false, IFACE_("Grouping"))) {
    uiLayout *col = &panel->column(false);
    col->prop(ptr, "export_object_groups", UI_ITEM_NONE, IFACE_("Object Groups"), ICON_NONE);
    col->prop(ptr, "export_material_groups", UI_ITEM_NONE, IFACE_("Material Groups"), ICON_NONE);
    col->prop(ptr, "export_vertex_groups", UI_ITEM_NONE, IFACE_("Vertex Groups"), ICON_NONE);
    col->prop(ptr, "export_smooth_groups", UI_ITEM_NONE, IFACE_("Smooth Groups"), ICON_NONE);
    col = &col->column(false);
    col->enabled_set(export_smooth_groups);
    col->prop(
        ptr, "smooth_group_bitflags", UI_ITEM_NONE, IFACE_("Smooth Group Bitflags"), ICON_NONE);
  }

  /* Material options. */
  PanelLayout panel = layout->panel(C, "OBJ_export_materials", false);
  panel.header->use_property_split_set(false);
  panel.header->prop(ptr, "export_materials", UI_ITEM_NONE, "", ICON_NONE);
  panel.header->label(IFACE_("Materials"), ICON_NONE);
  if (panel.body) {
    uiLayout *col = &panel.body->column(false);
    col->enabled_set(export_materials);

    col->prop(ptr, "export_pbr_extensions", UI_ITEM_NONE, IFACE_("PBR Extensions"), ICON_NONE);
    col->prop(ptr, "path_mode", UI_ITEM_NONE, IFACE_("Path Mode"), ICON_NONE);
  }

  /* Animation options. */
  panel = layout->panel(C, "OBJ_export_animation", true);
  panel.header->use_property_split_set(false);
  panel.header->prop(ptr, "export_animation", UI_ITEM_NONE, "", ICON_NONE);
  panel.header->label(IFACE_("Animation"), ICON_NONE);
  if (panel.body) {
    uiLayout *col = &panel.body->column(false);
    col->enabled_set(export_animation);

    col->prop(ptr, "start_frame", UI_ITEM_NONE, IFACE_("Frame Start"), ICON_NONE);
    col->prop(ptr, "end_frame", UI_ITEM_NONE, IFACE_("End"), ICON_NONE);
  }
}

static void wm_obj_export_draw(bContext *C, wmOperator *op)
{
  ui_obj_export_settings(C, op->layout, op->ptr);
}

/**
 * Return true if any property in the UI is changed.
 */
static bool wm_obj_export_check(bContext *C, wmOperator *op)
{
  char filepath[FILE_MAX];
  Scene *scene = CTX_data_scene(C);
  bool changed = false;
  RNA_string_get(op->ptr, "filepath", filepath);

  if (!BLI_path_extension_check(filepath, ".obj")) {
    BLI_path_extension_ensure(filepath, FILE_MAX, ".obj");
    RNA_string_set(op->ptr, "filepath", filepath);
    changed = true;
  }

  {
    int start = RNA_int_get(op->ptr, "start_frame");
    int end = RNA_int_get(op->ptr, "end_frame");
    /* Set the defaults. */
    if (start == INT_MIN) {
      start = scene->r.sfra;
      changed = true;
    }
    if (end == INT_MAX) {
      end = scene->r.efra;
      changed = true;
    }
    /* Fix user errors. */
    if (end < start) {
      end = start;
      changed = true;
    }
    RNA_int_set(op->ptr, "start_frame", start);
    RNA_int_set(op->ptr, "end_frame", end);
  }
  return changed;
}

void WM_OT_obj_export(wmOperatorType *ot)
{
  PropertyRNA *prop;

  ot->name = "Export Wavefront OBJ";
  ot->description = "Save the scene to a Wavefront OBJ file";
  ot->idname = "WM_OT_obj_export";

  ot->invoke = wm_obj_export_invoke;
  ot->exec = wm_obj_export_exec;
  ot->poll = WM_operator_winactive;
  ot->ui = wm_obj_export_draw;
  ot->check = wm_obj_export_check;

  ot->flag = OPTYPE_PRESET;

  WM_operator_properties_filesel(ot,
                                 FILE_TYPE_FOLDER,
                                 FILE_BLENDER,
                                 FILE_SAVE,
                                 WM_FILESEL_FILEPATH | WM_FILESEL_SHOW_PROPS,
                                 FILE_DEFAULTDISPLAY,
                                 FILE_SORT_DEFAULT);

  /* Animation options. */
  RNA_def_boolean(ot->srna,
                  "export_animation",
                  false,
                  "Export Animation",
                  "Export multiple frames instead of the current frame only");
  RNA_def_int(ot->srna,
              "start_frame",
              INT_MIN, /* wm_obj_export_check uses this to set scene->r.sfra. */
              INT_MIN,
              INT_MAX,
              "Start Frame",
              "The first frame to be exported",
              INT_MIN,
              INT_MAX);
  RNA_def_int(ot->srna,
              "end_frame",
              INT_MAX, /* wm_obj_export_check uses this to set scene->r.efra. */
              INT_MIN,
              INT_MAX,
              "End Frame",
              "The last frame to be exported",
              INT_MIN,
              INT_MAX);
  /* Object transform options. */
  prop = RNA_def_enum(
      ot->srna, "forward_axis", io_transform_axis, IO_AXIS_NEGATIVE_Z, "Forward Axis", "");
  RNA_def_property_update_runtime(prop, io_ui_forward_axis_update);
  prop = RNA_def_enum(ot->srna, "up_axis", io_transform_axis, IO_AXIS_Y, "Up Axis", "");
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
  RNA_def_enum(ot->srna,
               "export_eval_mode",
               io_obj_export_evaluation_mode,
               DAG_EVAL_VIEWPORT,
               "Object Properties",
               "Determines properties like object visibility, modifiers etc., where they differ "
               "for Render and Viewport");
  RNA_def_boolean(ot->srna,
                  "export_selected_objects",
                  false,
                  "Export Selected Objects",
                  "Export only selected objects instead of all supported objects");
  RNA_def_boolean(ot->srna, "export_uv", true, "Export UVs", "");
  RNA_def_boolean(ot->srna,
                  "export_normals",
                  true,
                  "Export Normals",
                  "Export per-face normals if the face is flat-shaded, per-face-corner "
                  "normals if smooth-shaded");
  RNA_def_boolean(ot->srna, "export_colors", false, "Export Colors", "Export per-vertex colors");
  RNA_def_boolean(ot->srna,
                  "export_materials",
                  true,
                  "Export Materials",
                  "Export MTL library. There must be a Principled-BSDF node for image textures to "
                  "be exported to the MTL file");
  RNA_def_boolean(ot->srna,
                  "export_pbr_extensions",
                  false,
                  "Export Materials with PBR Extensions",
                  "Export MTL library using PBR extensions (roughness, metallic, sheen, "
                  "coat, anisotropy, transmission)");
  prop = RNA_def_enum(ot->srna,
                      "path_mode",
                      io_obj_path_mode,
                      PATH_REFERENCE_AUTO,
                      "Path Mode",
                      "Method used to reference paths");
  RNA_def_property_translation_context(prop, BLT_I18NCONTEXT_EDITOR_FILEBROWSER);
  RNA_def_boolean(ot->srna,
                  "export_triangulated_mesh",
                  false,
                  "Export Triangulated Mesh",
                  "All ngons with four or more vertices will be triangulated. Meshes in "
                  "the scene will not be affected. Behaves like Triangulate Modifier with "
                  "ngon-method: \"Beauty\", quad-method: \"Shortest Diagonal\", min vertices: 4");
  RNA_def_boolean(ot->srna,
                  "export_curves_as_nurbs",
                  false,
                  "Export Curves as NURBS",
                  "Export curves in parametric form instead of exporting as mesh");

  RNA_def_boolean(ot->srna,
                  "export_object_groups",
                  false,
                  "Export Object Groups",
                  "Append mesh name to object name, separated by a '_'");
  RNA_def_boolean(ot->srna,
                  "export_material_groups",
                  false,
                  "Export Material Groups",
                  "Generate an OBJ group for each part of a geometry using a different material");
  RNA_def_boolean(
      ot->srna,
      "export_vertex_groups",
      false,
      "Export Vertex Groups",
      "Export the name of the vertex group of a face. It is approximated "
      "by choosing the vertex group with the most members among the vertices of a face");
  RNA_def_boolean(ot->srna,
                  "export_smooth_groups",
                  false,
                  "Export Smooth Groups",
                  "Generate smooth groups identifiers for each group of smooth faces, as "
                  "unique integer values by default");
  RNA_def_boolean(
      ot->srna,
      "smooth_group_bitflags",
      false,
      "Bitflags Smooth Groups",
      "If exporting smoothgroups, generate 'bitflags' values for the groups, instead of "
      "unique integer values. The same bitflag value can be re-used for different groups of "
      "smooth faces, as long as they have no common sharp edges or vertices");

  /* Only show `.obj` or `.mtl` files by default. */
  prop = RNA_def_string(ot->srna, "filter_glob", "*.obj;*.mtl", 0, "Extension Filter", "");
  RNA_def_property_flag(prop, PROP_HIDDEN);

  prop = RNA_def_string(ot->srna, "collection", nullptr, MAX_ID_NAME - 2, "Collection", nullptr);
  RNA_def_property_flag(prop, PROP_HIDDEN);
}

static wmOperatorStatus wm_obj_import_exec(bContext *C, wmOperator *op)
{
  OBJImportParams import_params;
  import_params.global_scale = RNA_float_get(op->ptr, "global_scale");
  import_params.clamp_size = RNA_float_get(op->ptr, "clamp_size");
  import_params.forward_axis = eIOAxis(RNA_enum_get(op->ptr, "forward_axis"));
  import_params.up_axis = eIOAxis(RNA_enum_get(op->ptr, "up_axis"));
  import_params.use_split_objects = RNA_boolean_get(op->ptr, "use_split_objects");
  import_params.use_split_groups = RNA_boolean_get(op->ptr, "use_split_groups");
  import_params.import_vertex_groups = RNA_boolean_get(op->ptr, "import_vertex_groups");
  import_params.validate_meshes = RNA_boolean_get(op->ptr, "validate_meshes");
  import_params.close_spline_loops = RNA_boolean_get(op->ptr, "close_spline_loops");
  char separator[2] = {};
  RNA_string_get(op->ptr, "collection_separator", separator);
  import_params.collection_separator = separator[0];
  import_params.relative_paths = ((U.flag & USER_RELPATHS) != 0);
  import_params.clear_selection = true;
  import_params.mtl_name_collision_mode = eOBJMtlNameCollisionMode(
      RNA_enum_get(op->ptr, "mtl_name_collision_mode"));

  import_params.reports = op->reports;

  const auto paths = blender::ed::io::paths_from_operator_properties(op->ptr);

  if (paths.is_empty()) {
    BKE_report(op->reports, RPT_ERROR, "No filepath given");
    return OPERATOR_CANCELLED;
  }
  for (const auto &path : paths) {
    STRNCPY(import_params.filepath, path.c_str());
    OBJ_import(C, &import_params);
    /* Only first import clears selection. */
    import_params.clear_selection = false;
  };

  Scene *scene = CTX_data_scene(C);
  WM_event_add_notifier(C, NC_SCENE | ND_OB_SELECT, scene);
  WM_event_add_notifier(C, NC_SCENE | ND_OB_ACTIVE, scene);
  WM_event_add_notifier(C, NC_SCENE | ND_LAYER_CONTENT, scene);
  ED_outliner_select_sync_from_object_tag(C);

  return OPERATOR_FINISHED;
}

static void ui_obj_import_settings(const bContext *C, uiLayout *layout, PointerRNA *ptr)
{
  layout->use_property_split_set(true);
  layout->use_property_decorate_set(false);

  if (uiLayout *panel = layout->panel(C, "OBJ_import_general", false, IFACE_("General"))) {
    uiLayout *col = &panel->column(false);
    col->prop(ptr, "global_scale", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    col->prop(ptr, "clamp_size", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    col->prop(ptr, "forward_axis", UI_ITEM_NONE, IFACE_("Forward Axis"), ICON_NONE);
    col->prop(ptr, "up_axis", UI_ITEM_NONE, IFACE_("Up Axis"), ICON_NONE);
  }

  if (uiLayout *panel = layout->panel(C, "OBJ_import_options", false, IFACE_("Options"))) {
    uiLayout *col = &panel->column(false);
    col->prop(ptr, "use_split_objects", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    col->prop(ptr, "use_split_groups", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    col->prop(ptr, "import_vertex_groups", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    col->prop(ptr, "validate_meshes", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    col->prop(ptr, "close_spline_loops", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    col->prop(ptr, "collection_separator", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }

  if (uiLayout *panel = layout->panel(C, "OBJ_import_materials", false, IFACE_("Materials"))) {
    uiLayout *col = &panel->column(false);
    col->prop(ptr, "mtl_name_collision_mode", UI_ITEM_NONE, IFACE_("Name Collision"), ICON_NONE);
  }
}

static void wm_obj_import_draw(bContext *C, wmOperator *op)
{
  ui_obj_import_settings(C, op->layout, op->ptr);
}

void WM_OT_obj_import(wmOperatorType *ot)
{
  PropertyRNA *prop;

  ot->name = "Import Wavefront OBJ";
  ot->description = "Load a Wavefront OBJ scene";
  ot->idname = "WM_OT_obj_import";
  ot->flag = OPTYPE_UNDO | OPTYPE_PRESET;

  ot->invoke = blender::ed::io::filesel_drop_import_invoke;
  ot->exec = wm_obj_import_exec;
  ot->poll = WM_operator_winactive;
  ot->ui = wm_obj_import_draw;

  WM_operator_properties_filesel(ot,
                                 FILE_TYPE_FOLDER,
                                 FILE_BLENDER,
                                 FILE_OPENFILE,
                                 WM_FILESEL_FILEPATH | WM_FILESEL_SHOW_PROPS |
                                     WM_FILESEL_DIRECTORY | WM_FILESEL_FILES,
                                 FILE_DEFAULTDISPLAY,
                                 FILE_SORT_DEFAULT);

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
  RNA_def_float(
      ot->srna,
      "clamp_size",
      0.0f,
      0.0f,
      1000.0f,
      "Clamp Bounding Box",
      "Resize the objects to keep bounding box under this value. Value 0 disables clamping",
      0.0f,
      1000.0f);
  prop = RNA_def_enum(
      ot->srna, "forward_axis", io_transform_axis, IO_AXIS_NEGATIVE_Z, "Forward Axis", "");
  RNA_def_property_update_runtime(prop, io_ui_forward_axis_update);
  prop = RNA_def_enum(ot->srna, "up_axis", io_transform_axis, IO_AXIS_Y, "Up Axis", "");
  RNA_def_property_update_runtime(prop, io_ui_up_axis_update);
  RNA_def_boolean(ot->srna,
                  "use_split_objects",
                  true,
                  "Split By Object",
                  "Import each OBJ 'o' as a separate object");
  RNA_def_boolean(ot->srna,
                  "use_split_groups",
                  false,
                  "Split By Group",
                  "Import each OBJ 'g' as a separate object");
  RNA_def_boolean(ot->srna,
                  "import_vertex_groups",
                  false,
                  "Vertex Groups",
                  "Import OBJ groups as vertex groups");
  RNA_def_boolean(
      ot->srna,
      "validate_meshes",
      true,
      "Validate Meshes",
      "Ensure the data is valid "
      "(when disabled, data may be imported which causes crashes displaying or editing)");
  RNA_def_boolean(ot->srna,
                  "close_spline_loops",
                  true,
                  "Detect Cyclic Curves",
                  "Join curve endpoints if overlapping control points are detected "
                  "(if disabled, no curves will be cyclic)");

  RNA_def_string(ot->srna,
                 "collection_separator",
                 nullptr,
                 2,
                 "Path Separator",
                 "Character used to separate objects name into hierarchical structure");

  /* Material options */
  RNA_def_enum(ot->srna,
               "mtl_name_collision_mode",
               io_obj_mtl_name_collision_mode,
               OBJ_MTL_NAME_COLLISION_MAKE_UNIQUE,
               "Material Name Collision",
               "How to handle naming collisions when importing materials");

  /* Only show `.obj` or `.mtl` files by default. */
  prop = RNA_def_string(ot->srna, "filter_glob", "*.obj;*.mtl", 0, "Extension Filter", "");
  RNA_def_property_flag(prop, PROP_HIDDEN);
}

namespace blender::ed::io {
void obj_file_handler_add()
{
  auto fh = std::make_unique<blender::bke::FileHandlerType>();
  STRNCPY_UTF8(fh->idname, "IO_FH_obj");
  STRNCPY_UTF8(fh->import_operator, "WM_OT_obj_import");
  STRNCPY_UTF8(fh->export_operator, "WM_OT_obj_export");
  STRNCPY_UTF8(fh->label, "Wavefront OBJ");
  STRNCPY_UTF8(fh->file_extensions_str, ".obj");
  fh->poll_drop = poll_file_object_drop;
  bke::file_handler_add(std::move(fh));
}
}  // namespace blender::ed::io

#endif /* WITH_IO_WAVEFRONT_OBJ */
