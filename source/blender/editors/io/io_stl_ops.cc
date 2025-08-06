/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editor/io
 */

#ifdef WITH_IO_STL

#  include "BKE_context.hh"
#  include "BKE_file_handler.hh"
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

#  include "IO_stl.hh"
#  include "io_stl_ops.hh"
#  include "io_utils.hh"

static wmOperatorStatus wm_stl_export_invoke(bContext *C,
                                             wmOperator *op,
                                             const wmEvent * /*event*/)
{
  ED_fileselect_ensure_default_filepath(C, op, ".stl");

  WM_event_add_fileselect(C, op);
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus wm_stl_export_exec(bContext *C, wmOperator *op)
{
  if (!RNA_struct_property_is_set_ex(op->ptr, "filepath", false)) {
    BKE_report(op->reports, RPT_ERROR, "No filename given");
    return OPERATOR_CANCELLED;
  }
  STLExportParams export_params;
  RNA_string_get(op->ptr, "filepath", export_params.filepath);
  export_params.forward_axis = eIOAxis(RNA_enum_get(op->ptr, "forward_axis"));
  export_params.up_axis = eIOAxis(RNA_enum_get(op->ptr, "up_axis"));
  export_params.global_scale = RNA_float_get(op->ptr, "global_scale");
  export_params.apply_modifiers = RNA_boolean_get(op->ptr, "apply_modifiers");
  export_params.export_selected_objects = RNA_boolean_get(op->ptr, "export_selected_objects");
  export_params.use_scene_unit = RNA_boolean_get(op->ptr, "use_scene_unit");
  export_params.ascii_format = RNA_boolean_get(op->ptr, "ascii_format");
  export_params.use_batch = RNA_boolean_get(op->ptr, "use_batch");

  RNA_string_get(op->ptr, "collection", export_params.collection);

  export_params.reports = op->reports;

  STL_export(C, &export_params);

  if (BKE_reports_contain(op->reports, RPT_ERROR)) {
    return OPERATOR_CANCELLED;
  }

  BKE_report(op->reports, RPT_INFO, "File exported successfully");
  return OPERATOR_FINISHED;
}

static void wm_stl_export_draw(bContext *C, wmOperator *op)
{
  uiLayout *layout = op->layout;
  PointerRNA *ptr = op->ptr;

  layout->use_property_split_set(true);
  layout->use_property_decorate_set(false);

  if (uiLayout *panel = layout->panel(C, "STL_export_general", false, IFACE_("General"))) {
    uiLayout *col = &panel->column(false);

    uiLayout *sub = &col->column(false, IFACE_("Format"));
    sub->prop(ptr, "ascii_format", UI_ITEM_NONE, IFACE_("ASCII"), ICON_NONE);

    /* The Batch mode and Selection only options only make sense when using regular export. */
    if (CTX_wm_space_file(C)) {
      col->prop(ptr, "use_batch", UI_ITEM_NONE, IFACE_("Batch"), ICON_NONE);

      sub = &col->column(false, IFACE_("Include"));
      sub->prop(ptr, "export_selected_objects", UI_ITEM_NONE, IFACE_("Selection Only"), ICON_NONE);
    }

    sub->prop(ptr, "global_scale", UI_ITEM_NONE, IFACE_("Scale"), ICON_NONE);
    sub->prop(ptr, "use_scene_unit", UI_ITEM_NONE, IFACE_("Scene Unit"), ICON_NONE);
    sub->prop(ptr, "forward_axis", UI_ITEM_NONE, IFACE_("Forward"), ICON_NONE);
    sub->prop(ptr, "up_axis", UI_ITEM_NONE, IFACE_("Up"), ICON_NONE);
  }

  if (uiLayout *panel = layout->panel(C, "STL_export_geometry", false, IFACE_("Geometry"))) {
    uiLayout *col = &panel->column(false);
    col->prop(ptr, "apply_modifiers", UI_ITEM_NONE, IFACE_("Apply Modifiers"), ICON_NONE);
  }
}

/**
 * Return true if any property in the UI is changed.
 */
static bool wm_stl_export_check(bContext * /*C*/, wmOperator *op)
{
  char filepath[FILE_MAX];
  bool changed = false;
  bool use_batch = RNA_boolean_get(op->ptr, "use_batch");
  RNA_string_get(op->ptr, "filepath", filepath);

  /* Enforce an extension on the filepath unless Batch mode is used. Batch mode
   * will perform substitutions, including the extension, during its processing. */
  if (!use_batch && !BLI_path_extension_check(filepath, ".stl")) {
    BLI_path_extension_ensure(filepath, FILE_MAX, ".stl");
    RNA_string_set(op->ptr, "filepath", filepath);
    changed = true;
  }
  return changed;
}

void WM_OT_stl_export(wmOperatorType *ot)
{
  PropertyRNA *prop;

  ot->name = "Export STL";
  ot->description = "Save the scene to an STL file";
  ot->idname = "WM_OT_stl_export";

  ot->invoke = wm_stl_export_invoke;
  ot->exec = wm_stl_export_exec;
  ot->poll = WM_operator_winactive;
  ot->ui = wm_stl_export_draw;
  ot->check = wm_stl_export_check;

  ot->flag = OPTYPE_PRESET;

  WM_operator_properties_filesel(ot,
                                 FILE_TYPE_FOLDER,
                                 FILE_BLENDER,
                                 FILE_SAVE,
                                 WM_FILESEL_FILEPATH | WM_FILESEL_SHOW_PROPS,
                                 FILE_DEFAULTDISPLAY,
                                 FILE_SORT_DEFAULT);

  RNA_def_boolean(ot->srna,
                  "ascii_format",
                  false,
                  "ASCII Format",
                  "Export file in ASCII format, export as binary otherwise");
  RNA_def_boolean(
      ot->srna, "use_batch", false, "Batch Export", "Export each object to a separate file");
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

  RNA_def_float(ot->srna, "global_scale", 1.0f, 1e-6f, 1e6f, "Scale", "", 0.001f, 1000.0f);
  RNA_def_boolean(ot->srna,
                  "use_scene_unit",
                  false,
                  "Scene Unit",
                  "Apply current scene's unit (as defined by unit scale) to exported data");

  prop = RNA_def_enum(ot->srna, "forward_axis", io_transform_axis, IO_AXIS_Y, "Forward Axis", "");
  RNA_def_property_update_runtime(prop, io_ui_forward_axis_update);

  prop = RNA_def_enum(ot->srna, "up_axis", io_transform_axis, IO_AXIS_Z, "Up Axis", "");
  RNA_def_property_update_runtime(prop, io_ui_up_axis_update);

  RNA_def_boolean(
      ot->srna, "apply_modifiers", true, "Apply Modifiers", "Apply modifiers to exported meshes");

  /* Only show `.stl` files by default. */
  prop = RNA_def_string(ot->srna, "filter_glob", "*.stl", 0, "Extension Filter", "");
  RNA_def_property_flag(prop, PROP_HIDDEN);
}

static wmOperatorStatus wm_stl_import_exec(bContext *C, wmOperator *op)
{
  STLImportParams params;
  params.forward_axis = eIOAxis(RNA_enum_get(op->ptr, "forward_axis"));
  params.up_axis = eIOAxis(RNA_enum_get(op->ptr, "up_axis"));
  params.use_facet_normal = RNA_boolean_get(op->ptr, "use_facet_normal");
  params.use_scene_unit = RNA_boolean_get(op->ptr, "use_scene_unit");
  params.global_scale = RNA_float_get(op->ptr, "global_scale");
  params.use_mesh_validate = RNA_boolean_get(op->ptr, "use_mesh_validate");

  params.reports = op->reports;

  const auto paths = blender::ed::io::paths_from_operator_properties(op->ptr);

  if (paths.is_empty()) {
    BKE_report(op->reports, RPT_ERROR, "No filepath given");
    return OPERATOR_CANCELLED;
  }
  for (const auto &path : paths) {
    STRNCPY(params.filepath, path.c_str());
    STL_import(C, &params);
  }

  Scene *scene = CTX_data_scene(C);
  WM_event_add_notifier(C, NC_SCENE | ND_OB_SELECT, scene);
  WM_event_add_notifier(C, NC_SCENE | ND_OB_ACTIVE, scene);
  WM_event_add_notifier(C, NC_SCENE | ND_LAYER_CONTENT, scene);
  ED_outliner_select_sync_from_object_tag(C);

  return OPERATOR_FINISHED;
}

static bool wm_stl_import_check(bContext * /*C*/, wmOperator *op)
{
  const int num_axes = 3;
  /* Both forward and up axes cannot be the same (or same except opposite sign). */
  if (RNA_enum_get(op->ptr, "forward_axis") % num_axes ==
      (RNA_enum_get(op->ptr, "up_axis") % num_axes))
  {
    RNA_enum_set(op->ptr, "up_axis", RNA_enum_get(op->ptr, "up_axis") % num_axes + 1);
    return true;
  }
  return false;
}

static void ui_stl_import_settings(const bContext *C, uiLayout *layout, PointerRNA *ptr)
{
  layout->use_property_split_set(true);
  layout->use_property_decorate_set(false);

  if (uiLayout *panel = layout->panel(C, "STL_import_general", false, IFACE_("General"))) {
    uiLayout *col = &panel->column(false);
    col->prop(ptr, "global_scale", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    col->prop(ptr, "use_scene_unit", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    col->prop(ptr, "forward_axis", UI_ITEM_NONE, IFACE_("Forward Axis"), ICON_NONE);
    col->prop(ptr, "up_axis", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }

  if (uiLayout *panel = layout->panel(C, "STL_import_options", false, IFACE_("Options"))) {
    uiLayout *col = &panel->column(false);
    col->prop(ptr, "use_facet_normal", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    col->prop(ptr, "use_mesh_validate", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
}

static void wm_stl_import_draw(bContext *C, wmOperator *op)
{
  ui_stl_import_settings(C, op->layout, op->ptr);
}

void WM_OT_stl_import(wmOperatorType *ot)
{
  PropertyRNA *prop;

  ot->name = "Import STL";
  ot->description = "Import an STL file as an object";
  ot->idname = "WM_OT_stl_import";

  ot->invoke = blender::ed::io::filesel_drop_import_invoke;
  ot->exec = wm_stl_import_exec;
  ot->poll = WM_operator_winactive;
  ot->check = wm_stl_import_check;
  ot->ui = wm_stl_import_draw;
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
  RNA_def_boolean(ot->srna,
                  "use_facet_normal",
                  false,
                  "Facet Normals",
                  "Use (import) facet normals (note that this will still give flat shading)");
  RNA_def_enum(ot->srna, "forward_axis", io_transform_axis, IO_AXIS_Y, "Forward Axis", "");
  RNA_def_enum(ot->srna, "up_axis", io_transform_axis, IO_AXIS_Z, "Up Axis", "");

  RNA_def_boolean(
      ot->srna,
      "use_mesh_validate",
      true,
      "Validate Mesh",
      "Ensure the data is valid "
      "(when disabled, data may be imported which causes crashes displaying or editing)");

  /* Only show `.stl` files by default. */
  prop = RNA_def_string(ot->srna, "filter_glob", "*.stl", 0, "Extension Filter", "");
  RNA_def_property_flag(prop, PROP_HIDDEN);
}

namespace blender::ed::io {
void stl_file_handler_add()
{
  auto fh = std::make_unique<blender::bke::FileHandlerType>();
  STRNCPY_UTF8(fh->idname, "IO_FH_stl");
  STRNCPY_UTF8(fh->import_operator, "WM_OT_stl_import");
  STRNCPY_UTF8(fh->export_operator, "WM_OT_stl_export");
  STRNCPY_UTF8(fh->label, "STL");
  STRNCPY_UTF8(fh->file_extensions_str, ".stl");
  fh->poll_drop = poll_file_object_drop;
  bke::file_handler_add(std::move(fh));
}
}  // namespace blender::ed::io

#endif /* WITH_IO_STL */
