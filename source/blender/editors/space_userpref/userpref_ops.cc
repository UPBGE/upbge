/* SPDX-FileCopyrightText: 2009 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spuserpref
 */

#include <cstring>
#include <fmt/format.h>

#include "DNA_screen_types.h"
#include "DNA_space_types.h"

#include "BLI_listbase.h"
#ifdef WIN32
#  include "BLI_winstuff.h"
#endif
#include "BLI_fileops.h"
#include "BLI_path_utils.hh"
#include "BLI_string.h"
#include "BLI_string_utf8.h"

#include "BKE_callbacks.hh"
#include "BKE_context.hh"
#include "BKE_global.hh"
#include "BKE_main.hh"
#include "BKE_preferences.h"

#include "BKE_report.hh"

#include "BLT_translation.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_prototypes.hh"
#include "RNA_types.hh"

#include "UI_interface.hh"
#include "UI_interface_layout.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_asset.hh"
#include "ED_userpref.hh"

#include "MEM_guardedalloc.h"

/* -------------------------------------------------------------------- */
/** \name Reset Default Theme Operator
 * \{ */

static wmOperatorStatus preferences_reset_default_theme_exec(bContext *C, wmOperator * /*op*/)
{
  Main *bmain = CTX_data_main(C);
  UI_theme_init_default();
  UI_style_init_default();
  WM_reinit_gizmomap_all(bmain);
  WM_event_add_notifier(C, NC_WINDOW, nullptr);
  U.runtime.is_dirty = true;
  return OPERATOR_FINISHED;
}

static void PREFERENCES_OT_reset_default_theme(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Reset to Default Theme";
  ot->idname = "PREFERENCES_OT_reset_default_theme";
  ot->description = "Reset to the default theme colors";

  /* callbacks */
  ot->exec = preferences_reset_default_theme_exec;

  /* flags */
  ot->flag = OPTYPE_REGISTER;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Add Auto-Execution Path Operator
 * \{ */

static wmOperatorStatus preferences_autoexec_add_exec(bContext * /*C*/, wmOperator * /*op*/)
{
  bPathCompare *path_cmp = MEM_callocN<bPathCompare>("bPathCompare");
  BLI_addtail(&U.autoexec_paths, path_cmp);
  U.runtime.is_dirty = true;
  return OPERATOR_FINISHED;
}

static void PREFERENCES_OT_autoexec_path_add(wmOperatorType *ot)
{
  ot->name = "Add Auto-Execution Path";
  ot->idname = "PREFERENCES_OT_autoexec_path_add";
  ot->description = "Add path to exclude from auto-execution";

  ot->exec = preferences_autoexec_add_exec;

  ot->flag = OPTYPE_INTERNAL;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Remove Auto-Execution Path Operator
 * \{ */

static wmOperatorStatus preferences_autoexec_remove_exec(bContext * /*C*/, wmOperator *op)
{
  const int index = RNA_int_get(op->ptr, "index");
  bPathCompare *path_cmp = static_cast<bPathCompare *>(BLI_findlink(&U.autoexec_paths, index));
  if (path_cmp) {
    BLI_freelinkN(&U.autoexec_paths, path_cmp);
    U.runtime.is_dirty = true;
  }
  return OPERATOR_FINISHED;
}

static void PREFERENCES_OT_autoexec_path_remove(wmOperatorType *ot)
{
  ot->name = "Remove Auto-Execution Path";
  ot->idname = "PREFERENCES_OT_autoexec_path_remove";
  ot->description = "Remove path to exclude from auto-execution";

  ot->exec = preferences_autoexec_remove_exec;

  ot->flag = OPTYPE_INTERNAL;

  RNA_def_int(ot->srna, "index", 0, 0, INT_MAX, "Index", "", 0, 1000);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Add Asset Library Operator
 * \{ */

static wmOperatorStatus preferences_asset_library_add_exec(bContext *C, wmOperator *op)
{
  char *path = RNA_string_get_alloc(op->ptr, "directory", nullptr, 0, nullptr);
  char dirname[FILE_MAXFILE];

  BLI_path_slash_rstrip(path);
  BLI_path_split_file_part(path, dirname, sizeof(dirname));

  /* nullptr is a valid directory path here. A library without path will be created then. */
  const bUserAssetLibrary *new_library = BKE_preferences_asset_library_add(&U, dirname, path);
  /* Activate new library in the UI for further setup. */
  U.active_asset_library = BLI_findindex(&U.asset_libraries, new_library);
  U.runtime.is_dirty = true;

  /* There's no dedicated notifier for the Preferences. */
  WM_main_add_notifier(NC_WINDOW, nullptr);
  blender::ed::asset::list::clear_all_library(C);

  MEM_freeN(path);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus preferences_asset_library_add_invoke(bContext *C,
                                                             wmOperator *op,
                                                             const wmEvent * /*event*/)
{
  if (!RNA_struct_property_is_set(op->ptr, "directory")) {
    WM_event_add_fileselect(C, op);
    return OPERATOR_RUNNING_MODAL;
  }

  return preferences_asset_library_add_exec(C, op);
}

static void PREFERENCES_OT_asset_library_add(wmOperatorType *ot)
{
  ot->name = "Add Asset Library";
  ot->idname = "PREFERENCES_OT_asset_library_add";
  ot->description = "Add a directory to be used by the Asset Browser as source of assets";

  ot->exec = preferences_asset_library_add_exec;
  ot->invoke = preferences_asset_library_add_invoke;

  ot->flag = OPTYPE_INTERNAL;

  WM_operator_properties_filesel(ot,
                                 FILE_TYPE_FOLDER,
                                 FILE_SPECIAL,
                                 FILE_OPENFILE,
                                 WM_FILESEL_DIRECTORY,
                                 FILE_DEFAULTDISPLAY,
                                 FILE_SORT_DEFAULT);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Remove Asset Library Operator
 * \{ */

static bool preferences_asset_library_remove_poll(bContext *C)
{
  if (BLI_listbase_is_empty(&U.asset_libraries)) {
    CTX_wm_operator_poll_msg_set(C, "There is no asset library to remove");
    return false;
  }
  return true;
}

static wmOperatorStatus preferences_asset_library_remove_exec(bContext *C, wmOperator *op)
{
  const int index = RNA_int_get(op->ptr, "index");
  bUserAssetLibrary *library = static_cast<bUserAssetLibrary *>(
      BLI_findlink(&U.asset_libraries, index));
  if (!library) {
    return OPERATOR_CANCELLED;
  }

  BKE_preferences_asset_library_remove(&U, library);
  const int count_remaining = BLI_listbase_count(&U.asset_libraries);
  /* Update active library index to be in range. */
  CLAMP(U.active_asset_library, 0, count_remaining - 1);
  U.runtime.is_dirty = true;

  blender::ed::asset::list::clear_all_library(C);
  /* Trigger refresh for the Asset Browser. */
  WM_main_add_notifier(NC_SPACE | ND_SPACE_ASSET_PARAMS, nullptr);

  return OPERATOR_FINISHED;
}

static void PREFERENCES_OT_asset_library_remove(wmOperatorType *ot)
{
  ot->name = "Remove Asset Library";
  ot->idname = "PREFERENCES_OT_asset_library_remove";
  ot->description =
      "Remove a path to a .blend file, so the Asset Browser will not attempt to show it anymore";

  ot->exec = preferences_asset_library_remove_exec;
  ot->poll = preferences_asset_library_remove_poll;

  ot->flag = OPTYPE_INTERNAL;

  RNA_def_int(ot->srna, "index", 0, 0, INT_MAX, "Index", "", 0, 1000);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Add Extension Repository Operator
 * \{ */

enum class bUserExtensionRepoAddType {
  Remote = 0,
  Local = 1,
};

static const char *preferences_extension_repo_default_name_from_type(
    const bUserExtensionRepoAddType repo_type)
{
  switch (repo_type) {
    case bUserExtensionRepoAddType::Remote: {
      return "Remote Repository";
    }
    case bUserExtensionRepoAddType::Local: {
      return "User Repository";
    }
  }
  BLI_assert_unreachable();
  return "";
}

static wmOperatorStatus preferences_extension_repo_add_exec(bContext *C, wmOperator *op)
{
  const bUserExtensionRepoAddType repo_type = bUserExtensionRepoAddType(
      RNA_enum_get(op->ptr, "type"));

  Main *bmain = CTX_data_main(C);
  BKE_callback_exec_null(bmain, BKE_CB_EVT_EXTENSION_REPOS_UPDATE_PRE);

  char name[sizeof(bUserExtensionRepo::name)] = "";
  char remote_url[sizeof(bUserExtensionRepo::remote_url)] = "";
  char *access_token = nullptr;
  char custom_directory[sizeof(bUserExtensionRepo::custom_dirpath)] = "";

  const bool use_custom_directory = RNA_boolean_get(op->ptr, "use_custom_directory");
  const bool use_access_token = RNA_boolean_get(op->ptr, "use_access_token");
  const bool use_sync_on_startup = RNA_boolean_get(op->ptr, "use_sync_on_startup");
  if (use_custom_directory) {
    RNA_string_get(op->ptr, "custom_directory", custom_directory);
    BLI_path_slash_rstrip(custom_directory);
  }

  if (repo_type == bUserExtensionRepoAddType::Remote) {
    RNA_string_get(op->ptr, "remote_url", remote_url);

    if (use_access_token) {
      if (RNA_string_length(op->ptr, "access_token")) {
        access_token = RNA_string_get_alloc(op->ptr, "access_token", nullptr, 0, nullptr);
      }
    }
  }

  /* Setup the name using the following logic:
   * - It has been set so leave as-is.
   * - Initialize it based on the URL (default for remote repositories).
   * - Use a default name as a fallback.
   */
  {
    PropertyRNA *prop = RNA_struct_find_property(op->ptr, "name");
    if (RNA_property_is_set(op->ptr, prop)) {
      RNA_property_string_get(op->ptr, prop, name);
    }

    /* Unset or empty, auto-name based on remote URL or local directory. */
    if (name[0] == '\0') {
      switch (repo_type) {
        case bUserExtensionRepoAddType::Remote: {
          BKE_preferences_extension_remote_to_name(remote_url, name);
          break;
        }
        case bUserExtensionRepoAddType::Local: {
          if (use_custom_directory) {
            const char *custom_directory_basename = BLI_path_basename(custom_directory);
            STRNCPY_UTF8(name, custom_directory_basename);
            BLI_path_slash_rstrip(name);
          }
          break;
        }
      }
    }
    if (name[0] == '\0') {
      STRNCPY_UTF8(name, preferences_extension_repo_default_name_from_type(repo_type));
    }
  }

  const char *module = custom_directory[0] ? BLI_path_basename(custom_directory) : name;
  /* Not essential but results in more readable module names.
   * Otherwise URL's have their '.' removed, making for quite unreadable module names. */
  char module_buf[FILE_MAX];
  {
    STRNCPY_UTF8(module_buf, module);
    int i;
    for (i = 0; module_buf[i]; i++) {
      if (ELEM(module_buf[i], '.', '-', '/', '\\')) {
        module_buf[i] = '_';
      }
    }
    /* Strip any trailing underscores. */
    while ((i > 0) && (module_buf[--i] == '_')) {
      module_buf[i] = '\0';
    }
    module = module_buf;
  }

  bUserExtensionRepo *new_repo = BKE_preferences_extension_repo_add(
      &U, name, module, custom_directory);

  if (use_sync_on_startup) {
    new_repo->flag |= USER_EXTENSION_REPO_FLAG_SYNC_ON_STARTUP;
  }
  if (use_custom_directory) {
    new_repo->flag |= USER_EXTENSION_REPO_FLAG_USE_CUSTOM_DIRECTORY;
  }

  if (repo_type == bUserExtensionRepoAddType::Remote) {
    STRNCPY_UTF8(new_repo->remote_url, remote_url);
    new_repo->flag |= USER_EXTENSION_REPO_FLAG_USE_REMOTE_URL;

    if (use_access_token) {
      new_repo->flag |= USER_EXTENSION_REPO_FLAG_USE_ACCESS_TOKEN;
    }
    if (access_token) {
      new_repo->access_token = access_token;
    }
  }

  /* Activate new repository in the UI for further setup. */
  U.active_extension_repo = BLI_findindex(&U.extension_repos, new_repo);
  U.runtime.is_dirty = true;

  {
    PointerRNA new_repo_ptr = RNA_pointer_create_discrete(
        nullptr, &RNA_UserExtensionRepo, new_repo);
    PointerRNA *pointers[] = {&new_repo_ptr};

    BKE_callback_exec_null(bmain, BKE_CB_EVT_EXTENSION_REPOS_UPDATE_POST);
    BKE_callback_exec(bmain, pointers, ARRAY_SIZE(pointers), BKE_CB_EVT_EXTENSION_REPOS_SYNC);
  }

  /* There's no dedicated notifier for the Preferences. */
  WM_event_add_notifier(C, NC_WINDOW, nullptr);

  /* Mainly useful when adding a repository from a popup since it's not as obvious
   * the repository was added compared to the repository popover. */
  BKE_reportf(op->reports,
              RPT_INFO,
              "Added %s \"%s\"",
              preferences_extension_repo_default_name_from_type(repo_type),
              new_repo->name);

  return OPERATOR_FINISHED;
}

static wmOperatorStatus preferences_extension_repo_add_invoke(bContext *C,
                                                              wmOperator *op,
                                                              const wmEvent *event)
{
  const bUserExtensionRepoAddType repo_type = bUserExtensionRepoAddType(
      RNA_enum_get(op->ptr, "type"));
  PropertyRNA *prop_name = RNA_struct_find_property(op->ptr, "name");
  if (!RNA_property_is_set(op->ptr, prop_name)) {
    const char *name_default = preferences_extension_repo_default_name_from_type(repo_type);
    /* Leave unset, let this be set by the URL. */
    if (repo_type == bUserExtensionRepoAddType::Remote) {
      name_default = nullptr;
    }
    RNA_property_string_set(op->ptr, prop_name, name_default);
  }

  return WM_operator_props_popup_confirm_ex(
      C, op, event, IFACE_("Add New Extension Repository"), IFACE_("Create"));
}

static void preferences_extension_repo_add_ui(bContext * /*C*/, wmOperator *op)
{

  uiLayout *layout = op->layout;
  layout->use_property_split_set(true);
  layout->use_property_decorate_set(false);

  PointerRNA *ptr = op->ptr;
  const bUserExtensionRepoAddType repo_type = bUserExtensionRepoAddType(RNA_enum_get(ptr, "type"));

  switch (repo_type) {
    case bUserExtensionRepoAddType::Remote: {
      layout->prop(op->ptr, "remote_url", UI_ITEM_R_IMMEDIATE, std::nullopt, ICON_NONE);
      layout->prop(op->ptr, "use_sync_on_startup", UI_ITEM_NONE, std::nullopt, ICON_NONE);

      layout->separator(0.2f, LayoutSeparatorType::Line);

      const bool use_access_token = RNA_boolean_get(ptr, "use_access_token");
      const int token_icon = (use_access_token && RNA_string_length(op->ptr, "access_token")) ?
                                 ICON_LOCKED :
                                 ICON_UNLOCKED;

      uiLayout *row = &layout->row(true, IFACE_("Authentication"));
      row->prop(op->ptr, "use_access_token", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      uiLayout *col = &layout->row(false);
      col->active_set(use_access_token);
      /* Use "immediate" flag to refresh the icon. */
      col->prop(op->ptr, "access_token", UI_ITEM_R_IMMEDIATE, std::nullopt, token_icon);

      layout->separator(0.2f, LayoutSeparatorType::Line);

      break;
    }
    case bUserExtensionRepoAddType::Local: {
      layout->prop(op->ptr, "name", UI_ITEM_R_IMMEDIATE, std::nullopt, ICON_NONE);
      break;
    }
  }

  layout->prop(op->ptr, "use_custom_directory", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  uiLayout *col = &layout->row(false);
  col->active_set(RNA_boolean_get(ptr, "use_custom_directory"));
  col->prop(op->ptr, "custom_directory", UI_ITEM_NONE, std::nullopt, ICON_NONE);
}

static void PREFERENCES_OT_extension_repo_add(wmOperatorType *ot)
{
  ot->name = "Add Extension Repository";
  ot->idname = "PREFERENCES_OT_extension_repo_add";
  ot->description = "Add a new repository used to store extensions";

  ot->invoke = preferences_extension_repo_add_invoke;
  ot->exec = preferences_extension_repo_add_exec;
  ot->ui = preferences_extension_repo_add_ui;

  ot->flag = OPTYPE_INTERNAL | OPTYPE_REGISTER;

  static const EnumPropertyItem repo_type_items[] = {
      {int(bUserExtensionRepoAddType::Remote),
       "REMOTE",
       ICON_INTERNET,
       "Add Remote Repository",
       "Add a repository referencing a remote repository "
       "with support for listing and updating extensions"},
      {int(bUserExtensionRepoAddType::Local),
       "LOCAL",
       ICON_DISK_DRIVE,
       "Add Local Repository",
       "Add a repository managed manually without referencing an external repository"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  /* After creating a new repository some settings can't be easily changed
   * (especially the custom directory). To avoid showing a partially initialized repository,
   * set these values upon creation instead of having the user create the repository and change
   * them afterwards.
   *
   * An alternative solution could be implemented by creating an "uninitialized" repository,
   * setting up all it's properties then running an "initialize" operator however this seems
   * unnecessarily confusing as in most cases a user can do this in one step by naming and
   * setting the repositories URL (optionally the custom-directory). */

  /* Copy the RNA values are copied into the operator to avoid repetition. */
  StructRNA *type_ref = &RNA_UserExtensionRepo;

  { /* Name. */
    const char *prop_id = "name";
    const PropertyRNA *prop_ref = RNA_struct_type_find_property(type_ref, prop_id);
    PropertyRNA *prop = RNA_def_string(ot->srna,
                                       prop_id,
                                       nullptr,
                                       sizeof(bUserExtensionRepo::name),
                                       RNA_property_ui_name_raw(prop_ref),
                                       RNA_property_ui_description_raw(prop_ref));
    RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  }

  { /* Remote Path. */
    const char *prop_id = "remote_url";
    const PropertyRNA *prop_ref = RNA_struct_type_find_property(type_ref, prop_id);
    PropertyRNA *prop = RNA_def_string(ot->srna,
                                       prop_id,
                                       nullptr,
                                       sizeof(bUserExtensionRepo::remote_url),
                                       RNA_property_ui_name_raw(prop_ref),
                                       RNA_property_ui_description_raw(prop_ref));
    RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  }

  { /* Use Access Token. */
    const char *prop_id = "use_access_token";
    const PropertyRNA *prop_ref = RNA_struct_type_find_property(type_ref, prop_id);
    PropertyRNA *prop = RNA_def_boolean(ot->srna,
                                        prop_id,
                                        false,
                                        RNA_property_ui_name_raw(prop_ref),
                                        RNA_property_ui_description_raw(prop_ref));
    RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  }

  { /* Access Token (dynamic length). */
    const char *prop_id = "access_token";
    const PropertyRNA *prop_ref = RNA_struct_type_find_property(type_ref, prop_id);
    PropertyRNA *prop = RNA_def_string(ot->srna,
                                       prop_id,
                                       nullptr,
                                       0,
                                       RNA_property_ui_name_raw(prop_ref),
                                       RNA_property_ui_description_raw(prop_ref));
    RNA_def_property_flag(prop, PROP_SKIP_SAVE);
    RNA_def_property_subtype(prop, PROP_PASSWORD);
  }

  { /* Check for Updated on Startup. */
    const char *prop_id = "use_sync_on_startup";
    const PropertyRNA *prop_ref = RNA_struct_type_find_property(type_ref, prop_id);
    PropertyRNA *prop = RNA_def_boolean(ot->srna,
                                        prop_id,
                                        false,
                                        RNA_property_ui_name_raw(prop_ref),
                                        RNA_property_ui_description_raw(prop_ref));
    RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  }

  { /* Use Custom Directory. */
    const char *prop_id = "use_custom_directory";
    const PropertyRNA *prop_ref = RNA_struct_type_find_property(type_ref, prop_id);
    PropertyRNA *prop = RNA_def_boolean(ot->srna,
                                        prop_id,
                                        false,
                                        RNA_property_ui_name_raw(prop_ref),
                                        RNA_property_ui_description_raw(prop_ref));
    RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  }

  { /* Custom Directory. */
    const char *prop_id = "custom_directory";
    const PropertyRNA *prop_ref = RNA_struct_type_find_property(type_ref, prop_id);
    PropertyRNA *prop = RNA_def_string_dir_path(ot->srna,
                                                prop_id,
                                                nullptr,
                                                sizeof(bUserExtensionRepo::custom_dirpath),
                                                RNA_property_ui_name_raw(prop_ref),
                                                RNA_property_ui_description_raw(prop_ref));
    RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  }

  ot->prop = RNA_def_enum(
      ot->srna, "type", repo_type_items, 0, "Type", "The kind of repository to add");
  RNA_def_property_flag(ot->prop, PROP_SKIP_SAVE | PROP_HIDDEN);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Remove Extension Repository Operator
 * \{ */

static bool preferences_extension_repo_remove_poll(bContext *C)
{
  if (BLI_listbase_is_empty(&U.extension_repos)) {
    CTX_wm_operator_poll_msg_set(C, "There is no extension repository to remove");
    return false;
  }
  return true;
}

static wmOperatorStatus preferences_extension_repo_remove_invoke(bContext *C,
                                                                 wmOperator *op,
                                                                 const wmEvent * /*event*/)
{
  const int index = RNA_int_get(op->ptr, "index");
  bool remove_files = RNA_boolean_get(op->ptr, "remove_files");
  const bUserExtensionRepo *repo = static_cast<bUserExtensionRepo *>(
      BLI_findlink(&U.extension_repos, index));

  if (!repo) {
    return OPERATOR_CANCELLED;
  }

  if (remove_files) {
    if ((repo->flag & USER_EXTENSION_REPO_FLAG_USE_REMOTE_URL) == 0) {
      if (repo->source == USER_EXTENSION_REPO_SOURCE_SYSTEM) {
        remove_files = false;
      }
    }
  }

  std::string message;
  if (remove_files) {
    char dirpath[FILE_MAX];
    char user_dirpath[FILE_MAX];
    BKE_preferences_extension_repo_dirpath_get(repo, dirpath, sizeof(dirpath));
    BKE_preferences_extension_repo_user_dirpath_get(repo, user_dirpath, sizeof(user_dirpath));

    if (dirpath[0] || user_dirpath[0]) {
      message = IFACE_("Remove all files in:");
      const char *paths[] = {dirpath, user_dirpath};
      for (int i = 0; i < ARRAY_SIZE(paths); i++) {
        if (paths[i][0] == '\0') {
          continue;
        }
        message.append(fmt::format("\n\"{}\"", paths[i]));
      }
    }
    else {
      message = IFACE_("Remove, local files not found.");
      remove_files = false;
    }
  }
  else {
    message = IFACE_("Remove, keeping local files.");
  }

  const char *confirm_text = remove_files ? IFACE_("Remove Repository & Files") :
                                            IFACE_("Remove Repository");

  return WM_operator_confirm_ex(
      C, op, nullptr, message.c_str(), confirm_text, ALERT_ICON_WARNING, true);
}

static wmOperatorStatus preferences_extension_repo_remove_exec(bContext *C, wmOperator *op)
{
  const int index = RNA_int_get(op->ptr, "index");
  bool remove_files = RNA_boolean_get(op->ptr, "remove_files");
  bUserExtensionRepo *repo = static_cast<bUserExtensionRepo *>(
      BLI_findlink(&U.extension_repos, index));
  if (!repo) {
    return OPERATOR_CANCELLED;
  }

  Main *bmain = CTX_data_main(C);
  BKE_callback_exec_null(bmain, BKE_CB_EVT_EXTENSION_REPOS_UPDATE_PRE);

  if (remove_files) {
    if ((repo->flag & USER_EXTENSION_REPO_FLAG_USE_REMOTE_URL) == 0) {
      if (repo->source == USER_EXTENSION_REPO_SOURCE_SYSTEM) {
        /* The UI doesn't show this option, if it's accessed disallow it. */
        BKE_report(op->reports, RPT_WARNING, "Unable to remove files for \"System\" repositories");
        remove_files = false;
      }
    }
  }

  if (remove_files) {
    if (!BKE_preferences_extension_repo_module_is_valid(repo)) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  /* Account for it not being null terminated. */
                  "Unable to remove files, the module name \"%.*s\" is invalid and "
                  "could remove non-repository files",
                  int(sizeof(repo->module)),
                  repo->module);
      remove_files = false;
    }
  }

  if (remove_files) {
    char dirpath[FILE_MAX];
    BKE_preferences_extension_repo_dirpath_get(repo, dirpath, sizeof(dirpath));
    if (dirpath[0] && BLI_is_dir(dirpath)) {

      /* Removing custom directories has the potential to remove user data
       * if users accidentally point this to their home directory or similar.
       * Even though the UI shows a warning, we better prevent any accidents
       * caused by recursive removal, see #119481.
       * Only check custom directories because the non-custom directory is always
       * a specific location under Blender's local extensions directory. */
      const bool recursive = (repo->flag & USER_EXTENSION_REPO_FLAG_USE_CUSTOM_DIRECTORY) == 0;

      /* Perform package manager specific clear operations,
       * needed when `recursive` is false so the empty directory can be removed.
       * If it's not empty there will be a warning that the directory couldn't be removed.
       * The user will have to do this manually which is good since unknown files
       * could be user data. */
      BKE_callback_exec_string(bmain, BKE_CB_EVT_EXTENSION_REPOS_FILES_CLEAR, dirpath);

      if (BLI_delete(dirpath, true, recursive) != 0) {
        BKE_reportf(op->reports,
                    RPT_WARNING,
                    "Unable to remove directory: %s",
                    errno ? strerror(errno) : "unknown");
      }
    }

    BKE_preferences_extension_repo_user_dirpath_get(repo, dirpath, sizeof(dirpath));
    if (dirpath[0] && BLI_is_dir(dirpath)) {
      if (BLI_delete(dirpath, true, true) != 0) {
        BKE_reportf(op->reports,
                    RPT_WARNING,
                    "Unable to remove directory: %s",
                    errno ? strerror(errno) : "unknown");
      }
    }
  }

  BKE_preferences_extension_repo_remove(&U, repo);
  const int count_remaining = BLI_listbase_count(&U.extension_repos);
  /* Update active repo index to be in range. */
  CLAMP(U.active_extension_repo, 0, count_remaining - 1);
  U.runtime.is_dirty = true;

  BKE_callback_exec_null(bmain, BKE_CB_EVT_EXTENSION_REPOS_UPDATE_POST);

  /* There's no dedicated notifier for the Preferences. */
  WM_event_add_notifier(C, NC_WINDOW, nullptr);

  return OPERATOR_FINISHED;
}

static void PREFERENCES_OT_extension_repo_remove(wmOperatorType *ot)
{
  ot->name = "Remove Extension Repository";
  ot->idname = "PREFERENCES_OT_extension_repo_remove";
  ot->description = "Remove an extension repository";

  ot->invoke = preferences_extension_repo_remove_invoke;
  ot->exec = preferences_extension_repo_remove_exec;
  ot->poll = preferences_extension_repo_remove_poll;

  ot->flag = OPTYPE_INTERNAL;

  PropertyRNA *prop;
  prop = RNA_def_int(ot->srna, "index", 0, 0, INT_MAX, "Index", "", 0, 1000);
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  prop = RNA_def_boolean(ot->srna,
                         "remove_files",
                         false,
                         "Remove Files",
                         "Remove extension files when removing the repository");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Drop Extension Operator
 * \{ */

static wmOperatorStatus preferences_extension_url_drop_invoke(bContext *C,
                                                              wmOperator *op,
                                                              const wmEvent *event)
{
  std::string url = RNA_string_get(op->ptr, "url");
  const bool url_is_file = STRPREFIX(url.c_str(), "file://");
  const bool url_is_online = STRPREFIX(url.c_str(), "http://") ||
                             STRPREFIX(url.c_str(), "https://");
  const bool url_is_remote = url_is_file | url_is_online;

  /* NOTE: searching for hard-coded add-on name isn't great.
   * Needed since #WM_dropbox_add expects the operator to exist on startup. */
  const char *idname_external = url_is_remote ? "extensions.package_install" :
                                                "extensions.package_install_files";
  bool use_url = true;

  if (url_is_online && (G.f & G_FLAG_INTERNET_ALLOW) == 0) {
    idname_external = "extensions.userpref_allow_online_popup";
    use_url = false;
  }

  wmOperatorType *ot = WM_operatortype_find(idname_external, true);
  wmOperatorStatus retval;
  if (ot) {
    PointerRNA props_ptr;
    WM_operator_properties_create_ptr(&props_ptr, ot);
    if (use_url) {
      RNA_string_set(&props_ptr, "url", url.c_str());
    }
    WM_operator_name_call_ptr(C, ot, blender::wm::OpCallContext::InvokeDefault, &props_ptr, event);
    WM_operator_properties_free(&props_ptr);
    retval = OPERATOR_FINISHED;
  }
  else {
    BKE_reportf(op->reports, RPT_ERROR, "Extension operator not found \"%s\"", idname_external);
    retval = OPERATOR_CANCELLED;
  }
  return retval;
}

static void PREFERENCES_OT_extension_url_drop(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Drop Extension URL";
  ot->description = "Handle dropping an extension URL";
  ot->idname = "PREFERENCES_OT_extension_url_drop";

  /* API callbacks. */
  ot->invoke = preferences_extension_url_drop_invoke;

  RNA_def_string(ot->srna, "url", nullptr, 0, "URL", "Location of the extension to install");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Associate File Type Operator (Windows only)
 * \{ */

static bool associate_blend_poll(bContext *C)
{
#ifdef WIN32
  if (BLI_windows_is_store_install()) {
    CTX_wm_operator_poll_msg_set(C, "Not available for Microsoft Store installations");
    return false;
  }
  return true;
#elif defined(__APPLE__)
  CTX_wm_operator_poll_msg_set(C, "Windows & Linux only operator");
  return false;
#else
  UNUSED_VARS(C);
  return true;
#endif
}

#if !defined(__APPLE__)
static bool associate_blend(bool do_register, bool all_users, char **r_error_msg)
{
  const bool result = WM_platform_associate_set(do_register, all_users, r_error_msg);
#  ifdef WIN32
  if ((result == false) &&
      /* For some reason the message box isn't shown in this case. */
      (all_users == false))
  {
    const char *msg = do_register ? "Unable to register file association" :
                                    "Unable to unregister file association";
    MessageBox(0, msg, "Blender", MB_OK | MB_ICONERROR);
  }
#  endif /* !WIN32 */
  return result;
}
#endif

static wmOperatorStatus associate_blend_exec(bContext * /*C*/, wmOperator *op)
{
#ifdef __APPLE__
  UNUSED_VARS(op);
  BLI_assert_unreachable();
  return OPERATOR_CANCELLED;
#else

#  ifdef WIN32
  if (BLI_windows_is_store_install()) {
    BKE_report(
        op->reports, RPT_ERROR, "Registration not possible from Microsoft Store installations");
    return OPERATOR_CANCELLED;
  }
#  endif

  const bool all_users = (U.uiflag & USER_REGISTER_ALL_USERS);
  char *error_msg = nullptr;

  WM_cursor_wait(true);
  const bool success = associate_blend(true, all_users, &error_msg);
  WM_cursor_wait(false);

  if (!success) {
    BKE_report(
        op->reports, RPT_ERROR, error_msg ? error_msg : "Unable to register file association");
    if (error_msg) {
      MEM_freeN(error_msg);
    }
    return OPERATOR_CANCELLED;
  }
  BLI_assert(error_msg == nullptr);
  BKE_report(op->reports, RPT_INFO, "File association registered");
  return OPERATOR_FINISHED;
#endif /* !__APPLE__ */
}

static void PREFERENCES_OT_associate_blend(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Register File Association";
  ot->description = "Use this installation for .blend files and to display thumbnails";
  ot->idname = "PREFERENCES_OT_associate_blend";

  /* API callbacks. */
  ot->exec = associate_blend_exec;
  ot->poll = associate_blend_poll;
}

static wmOperatorStatus unassociate_blend_exec(bContext * /*C*/, wmOperator *op)
{
#ifdef __APPLE__
  UNUSED_VARS(op);
  BLI_assert_unreachable();
  return OPERATOR_CANCELLED;
#else
#  ifdef WIN32
  if (BLI_windows_is_store_install()) {
    BKE_report(
        op->reports, RPT_ERROR, "Unregistration not possible from Microsoft Store installations");
    return OPERATOR_CANCELLED;
  }
#  endif

  const bool all_users = (U.uiflag & USER_REGISTER_ALL_USERS);
  char *error_msg = nullptr;

  WM_cursor_wait(true);
  bool success = associate_blend(false, all_users, &error_msg);
  WM_cursor_wait(false);

  if (!success) {
    BKE_report(
        op->reports, RPT_ERROR, error_msg ? error_msg : "Unable to unregister file association");
    if (error_msg) {
      MEM_freeN(error_msg);
    }
    return OPERATOR_CANCELLED;
  }
  BLI_assert(error_msg == nullptr);
  BKE_report(op->reports, RPT_INFO, "File association unregistered");
  return OPERATOR_FINISHED;
#endif /* !__APPLE__ */
}

static void PREFERENCES_OT_unassociate_blend(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Remove File Association";
  ot->description = "Remove this installation's associations with .blend files";
  ot->idname = "PREFERENCES_OT_unassociate_blend";

  /* API callbacks. */
  ot->exec = unassociate_blend_exec;
  ot->poll = associate_blend_poll;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Drag & Drop URL
 * \{ */

static bool drop_extension_url_poll(bContext * /*C*/, wmDrag *drag, const wmEvent * /*event*/)
{
  if (drag->type != WM_DRAG_STRING) {
    return false;
  }

  /* NOTE(@ideasman42): it should be possible to drag a URL into the text editor or Python console.
   * In the future we may support dragging images into Blender by URL, so treating any single-line
   * URL as an extension could back-fire. Avoid problems in the future by limiting the text which
   * is accepted as an extension to ZIP's or URL's that reference known repositories. */

  const std::string &str = WM_drag_get_string(drag);

  /* Only URL formatted text. */
  const char *cstr = str.c_str();
  if (BKE_preferences_extension_repo_remote_scheme_end(cstr) == 0) {
    return false;
  }

  /* Only single line strings. */
  if (str.find('\n') != std::string::npos) {
    return false;
  }

  bool has_known_extension = false;
  {
    /* Strip parameters from the URL (if they exist) before the file extension is checked.
     * This allows for `https://example.org/api/v1/file.zip?repository=/api/v1/`.
     * This allows draggable links to specify their repository, see: #120665. */
    std::string str_strip;
    const char *cstr_maybe_copy = cstr;
    size_t param_char = str.find('?');
    if (param_char != std::string::npos) {
      str_strip = str.substr(0, param_char);
      cstr_maybe_copy = str_strip.c_str();
    }

    const char *cstr_ext = BLI_path_extension(cstr_maybe_copy);
    if (cstr_ext && STRCASEEQ(cstr_ext, ".zip")) {
      has_known_extension = true;
    }
  }

  /* Check the URL has a `.zip` suffix OR has a known repository as a prefix.
   * This is needed to support redirects which don't contain an extension. */
  if (!has_known_extension &&
      !BKE_preferences_extension_repo_find_by_remote_url_prefix(&U, cstr, true))
  {
    return false;
  }

  return true;
}

static void drop_extension_url_copy(bContext * /*C*/, wmDrag *drag, wmDropBox *drop)
{
  /* Copy drag URL to properties. */
  const std::string &str = WM_drag_get_string(drag);
  RNA_string_set(drop->ptr, "url", str.c_str());
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Drag & Drop Paths
 * \{ */

static bool drop_extension_path_poll(bContext * /*C*/, wmDrag *drag, const wmEvent * /*event*/)
{
  if (drag->type != WM_DRAG_PATH) {
    return false;
  }

  const char *cstr = WM_drag_get_single_path(drag);
  const char *cstr_ext = BLI_path_extension(cstr);
  if (!(cstr_ext && STRCASEEQ(cstr_ext, ".zip"))) {
    return false;
  }

  return true;
}

static void drop_extension_path_copy(bContext * /*C*/, wmDrag *drag, wmDropBox *drop)
{
  /* Copy drag URL to properties. */
  const char *cstr = WM_drag_get_single_path(drag);
  RNA_string_set(drop->ptr, "url", cstr);
}

/** \} */

static void ED_dropbox_drop_extension()
{
  ListBase *lb = WM_dropboxmap_find("Window", SPACE_EMPTY, RGN_TYPE_WINDOW);
  WM_dropbox_add(lb,
                 "PREFERENCES_OT_extension_url_drop",
                 drop_extension_url_poll,
                 drop_extension_url_copy,
                 nullptr,
                 nullptr);
  WM_dropbox_add(lb,
                 "PREFERENCES_OT_extension_url_drop",
                 drop_extension_path_poll,
                 drop_extension_path_copy,
                 nullptr,
                 nullptr);
}

void ED_operatortypes_userpref()
{
  WM_operatortype_append(PREFERENCES_OT_reset_default_theme);

  WM_operatortype_append(PREFERENCES_OT_autoexec_path_add);
  WM_operatortype_append(PREFERENCES_OT_autoexec_path_remove);

  WM_operatortype_append(PREFERENCES_OT_asset_library_add);
  WM_operatortype_append(PREFERENCES_OT_asset_library_remove);

  WM_operatortype_append(PREFERENCES_OT_extension_repo_add);
  WM_operatortype_append(PREFERENCES_OT_extension_repo_remove);
  WM_operatortype_append(PREFERENCES_OT_extension_url_drop);

  WM_operatortype_append(PREFERENCES_OT_associate_blend);
  WM_operatortype_append(PREFERENCES_OT_unassociate_blend);

  ED_dropbox_drop_extension();
}
