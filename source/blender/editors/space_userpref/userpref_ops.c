/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2009 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup spuserpref
 */

#include <string.h>

#include "DNA_screen_types.h"
#include "DNA_space_types.h"

#include "BLI_listbase.h"
#ifdef WIN32
#  include "BLI_winstuff.h"
#endif
#include "BLI_path_util.h"

#include "BKE_context.h"
#include "BKE_main.h"
#include "BKE_preferences.h"

#include "BKE_report.h"

#include "RNA_access.h"
#include "RNA_define.h"
#include "RNA_types.h"

#include "UI_interface.h"

#include "WM_api.h"
#include "WM_types.h"

#include "ED_userpref.h"

#include "MEM_guardedalloc.h"

/* -------------------------------------------------------------------- */
/** \name Reset Default Theme Operator
 * \{ */

static int preferences_reset_default_theme_exec(bContext *C, wmOperator *UNUSED(op))
{
  Main *bmain = CTX_data_main(C);
  UI_theme_init_default();
  UI_style_init_default();
  WM_reinit_gizmomap_all(bmain);
  WM_event_add_notifier(C, NC_WINDOW, NULL);
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

static int preferences_autoexec_add_exec(bContext *UNUSED(C), wmOperator *UNUSED(op))
{
  bPathCompare *path_cmp = MEM_callocN(sizeof(bPathCompare), "bPathCompare");
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

static int preferences_autoexec_remove_exec(bContext *UNUSED(C), wmOperator *op)
{
  const int index = RNA_int_get(op->ptr, "index");
  bPathCompare *path_cmp = BLI_findlink(&U.autoexec_paths, index);
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

static int preferences_asset_library_add_exec(bContext *UNUSED(C), wmOperator *op)
{
  char *path = RNA_string_get_alloc(op->ptr, "directory", NULL, 0, NULL);
  char dirname[FILE_MAXFILE];

  BLI_path_slash_rstrip(path);
  BLI_split_file_part(path, dirname, sizeof(dirname));

  /* NULL is a valid directory path here. A library without path will be created then. */
  BKE_preferences_asset_library_add(&U, dirname, path);
  U.runtime.is_dirty = true;

  /* There's no dedicated notifier for the Preferences. */
  WM_main_add_notifier(NC_WINDOW, NULL);

  MEM_freeN(path);
  return OPERATOR_FINISHED;
}

static int preferences_asset_library_add_invoke(bContext *C,
                                                wmOperator *op,
                                                const wmEvent *UNUSED(event))
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

static int preferences_asset_library_remove_exec(bContext *UNUSED(C), wmOperator *op)
{
  const int index = RNA_int_get(op->ptr, "index");
  bUserAssetLibrary *library = BLI_findlink(&U.asset_libraries, index);
  if (library) {
    BKE_preferences_asset_library_remove(&U, library);
    U.runtime.is_dirty = true;
    /* Trigger refresh for the Asset Browser. */
    WM_main_add_notifier(NC_SPACE | ND_SPACE_ASSET_PARAMS, NULL);
  }
  return OPERATOR_FINISHED;
}

static void PREFERENCES_OT_asset_library_remove(wmOperatorType *ot)
{
  ot->name = "Remove Asset Library";
  ot->idname = "PREFERENCES_OT_asset_library_remove";
  ot->description =
      "Remove a path to a .blend file, so the Asset Browser will not attempt to show it anymore";

  ot->exec = preferences_asset_library_remove_exec;

  ot->flag = OPTYPE_INTERNAL;

  RNA_def_int(ot->srna, "index", 0, 0, INT_MAX, "Index", "", 0, 1000);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Associate File Type Operator (Windows only)
 * \{ */

static bool associate_blend_poll(bContext *C)
{
#ifdef WIN32
  UNUSED_VARS(C);
  return true;
#else
  CTX_wm_operator_poll_msg_set(C, "Windows-only operator");
  return false;
#endif
}

static int associate_blend_exec(bContext *UNUSED(C), wmOperator *op)
{
#ifdef WIN32
  WM_cursor_wait(true);
  if (BLI_windows_register_blend_extension(true)) {
    BKE_report(op->reports, RPT_INFO, "File association registered");
    WM_cursor_wait(false);
    return OPERATOR_FINISHED;
  }
  else {
    BKE_report(op->reports, RPT_ERROR, "Unable to register file association");
    WM_cursor_wait(false);
    return OPERATOR_CANCELLED;
  }
#else
  UNUSED_VARS(op);
  BLI_assert_unreachable();
  return OPERATOR_CANCELLED;
#endif
}

static void PREFERENCES_OT_associate_blend(struct wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Register File Association";
  ot->description = "Use this installation for .blend files and to display thumbnails";
  ot->idname = "PREFERENCES_OT_associate_blend";

  /* api callbacks */
  ot->exec = associate_blend_exec;
  ot->poll = associate_blend_poll;
}

/** \} */

void ED_operatortypes_userpref(void)
{
  WM_operatortype_append(PREFERENCES_OT_reset_default_theme);

  WM_operatortype_append(PREFERENCES_OT_autoexec_path_add);
  WM_operatortype_append(PREFERENCES_OT_autoexec_path_remove);

  WM_operatortype_append(PREFERENCES_OT_asset_library_add);
  WM_operatortype_append(PREFERENCES_OT_asset_library_remove);

  WM_operatortype_append(PREFERENCES_OT_associate_blend);
}
