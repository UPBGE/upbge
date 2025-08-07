/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_path_utils.hh"
#include "BLI_string.h"

#include "BLT_translation.hh"

#include "BKE_file_handler.hh"

#include "CLG_log.h"

#include "DNA_space_types.h"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_prototypes.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "UI_interface.hh"
#include "UI_interface_layout.hh"

#include "io_drop_import_file.hh"
#include "io_utils.hh"

static CLG_LogRef LOG = {"io.drop_import_file"};

/**
 * Return a vector of file handlers that support any file path in `paths` and the call to
 * `poll_drop` returns #true. Unlike `bke::file_handlers_poll_file_drop`, it ensures that file
 * handlers have a valid import operator.
 */
static blender::Vector<blender::bke::FileHandlerType *> drop_import_file_poll_file_handlers(
    const bContext *C, const blender::Span<std::string> paths, const bool quiet = true)
{
  using namespace blender;
  auto file_handlers = bke::file_handlers_poll_file_drop(C, paths);
  file_handlers.remove_if([quiet](const bke::FileHandlerType *file_handler) {
    return WM_operatortype_find(file_handler->import_operator, quiet) == nullptr;
  });
  return file_handlers;
}

/**
 * Sets in the RNA pointer all file paths supported by the file handler.
 */
static void file_handler_import_operator_write_ptr(
    const blender::bke::FileHandlerType *file_handler,
    PointerRNA &props,
    const blender::Span<std::string> paths)
{

  const auto supported_paths = file_handler->filter_supported_paths(paths);

  PropertyRNA *filepath_prop = RNA_struct_find_property_check(props, "filepath", PROP_STRING);
  if (filepath_prop) {
    RNA_property_string_set(&props, filepath_prop, paths[supported_paths[0]].c_str());
  }

  PropertyRNA *directory_prop = RNA_struct_find_property_check(props, "directory", PROP_STRING);
  char dir[FILE_MAX];
  BLI_path_split_dir_part(paths[0].c_str(), dir, sizeof(dir));
  if (directory_prop) {
    RNA_property_string_set(&props, directory_prop, dir);
  }

  PropertyRNA *files_prop = RNA_struct_find_collection_property_check(
      props, "files", &RNA_OperatorFileListElement);
  if (files_prop) {
    RNA_property_collection_clear(&props, files_prop);
    for (const auto &index : supported_paths) {
      char file[FILE_MAX];
      STRNCPY(file, paths[index].c_str());
      BLI_path_rel(file, dir);

      PointerRNA item_ptr{};
      RNA_property_collection_add(&props, files_prop, &item_ptr);
      BLI_assert_msg(BLI_path_is_rel(file), "Expected path to be relative (start with '//')");
      RNA_string_set(&item_ptr, "name", file + 2);
    }
  }
  const bool has_any_filepath_prop = filepath_prop || directory_prop || files_prop;
  if (!has_any_filepath_prop) {
    CLOG_WARN(&LOG,
              "The '%s' file handler import operator ('%s') is missing the required operator "
              "properties.",
              file_handler->idname,
              file_handler->import_operator);
  }
  if (directory_prop && !files_prop) {
    CLOG_WARN(
        &LOG,
        "The '%s' file handler import operator ('%s') is missing the 'files' operator property.",
        file_handler->idname,
        file_handler->import_operator);
  }
  if (!directory_prop && files_prop) {
    CLOG_WARN(&LOG,
              "The '%s' file handler import operator ('%s') is missing the 'directory' operator "
              "property.",
              file_handler->idname,
              file_handler->import_operator);
  }
}

static wmOperatorStatus wm_drop_import_file_exec(bContext *C, wmOperator *op)
{
  const auto paths = blender::ed::io::paths_from_operator_properties(op->ptr);
  if (paths.is_empty()) {
    return OPERATOR_CANCELLED;
  }

  auto file_handlers = drop_import_file_poll_file_handlers(C, paths, false);
  if (file_handlers.is_empty()) {
    return OPERATOR_CANCELLED;
  }

  wmOperatorType *ot = WM_operatortype_find(file_handlers[0]->import_operator, false);
  PointerRNA file_props;
  WM_operator_properties_create_ptr(&file_props, ot);
  file_handler_import_operator_write_ptr(file_handlers[0], file_props, paths);

  WM_operator_name_call_ptr(
      C, ot, blender::wm::OpCallContext::InvokeDefault, &file_props, nullptr);
  WM_operator_properties_free(&file_props);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus wm_drop_import_file_invoke(bContext *C,
                                                   wmOperator *op,
                                                   const wmEvent * /*event*/)
{
  const auto paths = blender::ed::io::paths_from_operator_properties(op->ptr);
  if (paths.is_empty()) {
    return OPERATOR_CANCELLED;
  }

  auto file_handlers = drop_import_file_poll_file_handlers(C, paths, false);
  if (file_handlers.size() == 1) {
    return wm_drop_import_file_exec(C, op);
  }

  /**
   * Create a menu with all file handler import operators that can support any files in paths and
   * let user decide which to use.
   */
  uiPopupMenu *pup = UI_popup_menu_begin(C, "", ICON_NONE);
  uiLayout *layout = UI_popup_menu_layout(pup);
  layout->operator_context_set(blender::wm::OpCallContext::InvokeDefault);

  for (auto *file_handler : file_handlers) {
    wmOperatorType *ot = WM_operatortype_find(file_handler->import_operator, false);
    PointerRNA file_props = layout->op(ot,
                                       CTX_TIP_(ot->translation_context, ot->name),
                                       ICON_NONE,
                                       blender::wm::OpCallContext::InvokeDefault,
                                       UI_ITEM_NONE);
    file_handler_import_operator_write_ptr(file_handler, file_props, paths);
  }

  UI_popup_menu_end(C, pup);
  return OPERATOR_INTERFACE;
}

void WM_OT_drop_import_file(wmOperatorType *ot)
{
  ot->name = "Drop to Import File";
  ot->description = "Operator that allows file handlers to receive file drops";
  ot->idname = "WM_OT_drop_import_file";
  ot->flag = OPTYPE_INTERNAL;
  ot->exec = wm_drop_import_file_exec;
  ot->invoke = wm_drop_import_file_invoke;

  PropertyRNA *prop;

  prop = RNA_def_string_dir_path(
      ot->srna, "directory", nullptr, FILE_MAX, "Directory", "Directory of the file");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);

  prop = RNA_def_collection_runtime(ot->srna, "files", &RNA_OperatorFileListElement, "Files", "");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
}

static void drop_import_file_copy(bContext * /*C*/, wmDrag *drag, wmDropBox *drop)
{
  blender::ed::io::paths_to_operator_properties(drop->ptr, WM_drag_get_paths(drag));
}

static bool drop_import_file_poll(bContext *C, wmDrag *drag, const wmEvent * /*event*/)
{
  if (drag->type != WM_DRAG_PATH) {
    return false;
  }
  const auto paths = WM_drag_get_paths(drag);
  return !drop_import_file_poll_file_handlers(C, paths, true).is_empty();
}

static std::string drop_import_file_tooltip(bContext *C,
                                            wmDrag *drag,
                                            const int /*xy*/[2],
                                            wmDropBox * /*drop*/)
{
  const auto paths = WM_drag_get_paths(drag);
  const auto file_handlers = drop_import_file_poll_file_handlers(C, paths, true);
  if (file_handlers.size() == 1) {
    wmOperatorType *ot = WM_operatortype_find(file_handlers[0]->import_operator, false);
    return TIP_(ot->name);
  }

  return TIP_("Multiple file handlers can be used, drop to pick which to use");
}

void ED_dropbox_drop_import_file()
{
  ListBase *lb = WM_dropboxmap_find("Window", SPACE_EMPTY, RGN_TYPE_WINDOW);
  WM_dropbox_add(lb,
                 "WM_OT_drop_import_file",
                 drop_import_file_poll,
                 drop_import_file_copy,
                 nullptr,
                 drop_import_file_tooltip);
}
