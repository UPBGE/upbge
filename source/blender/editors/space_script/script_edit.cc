/* SPDX-FileCopyrightText: 2008 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spscript
 */

#include <cstring>

#include "BLI_listbase.h"

#include "BKE_context.hh"
#include "BKE_report.hh"

#include "BLT_translation.hh"

#include "WM_api.hh"
#include "WM_types.hh"
#include "wm_event_system.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "ED_screen.hh"

#include "script_intern.hh" /* own include */

#ifdef WITH_PYTHON
#  include "BPY_extern_run.hh"
#endif

static wmOperatorStatus run_pyfile_exec(bContext *C, wmOperator *op)
{
  char filepath[FILE_MAX];
  RNA_string_get(op->ptr, "filepath", filepath);
#ifdef WITH_PYTHON
  if (BPY_run_filepath(C, filepath, op->reports)) {
    ARegion *region = CTX_wm_region(C);
    if (region != nullptr) {
      ED_region_tag_redraw(region);
    }
    return OPERATOR_FINISHED;
  }
#else
  (void)C; /* unused */
#endif
  return OPERATOR_CANCELLED; /* FAIL */
}

void SCRIPT_OT_python_file_run(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* identifiers */
  ot->name = "Run Python File";
  ot->description = "Run Python file";
  ot->idname = "SCRIPT_OT_python_file_run";

  /* API callbacks. */
  ot->exec = run_pyfile_exec;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_INTERNAL;

  prop = RNA_def_string_file_path(ot->srna, "filepath", nullptr, FILE_MAX, "Path", "");
  RNA_def_property_translation_context(prop, BLT_I18NCONTEXT_EDITOR_FILEBROWSER);
}

#ifdef WITH_PYTHON
static bool script_test_modal_operators(bContext *C)
{
  wmWindowManager *wm = CTX_wm_manager(C);
  LISTBASE_FOREACH (wmWindow *, win, &wm->windows) {
    LISTBASE_FOREACH (wmEventHandler *, handler_base, &win->modalhandlers) {
      if (handler_base->type == WM_HANDLER_TYPE_OP) {
        wmEventHandler_Op *handler = (wmEventHandler_Op *)handler_base;
        if (handler->op != nullptr) {
          wmOperatorType *ot = handler->op->type;
          if (ot->rna_ext.srna) {
            return true;
          }
        }
      }
    }
  }

  return false;
}
#endif

static wmOperatorStatus script_reload_exec(bContext *C, wmOperator *op)
{

#ifdef WITH_PYTHON

  /* clear running operators */
  if (script_test_modal_operators(C)) {
    BKE_report(op->reports, RPT_ERROR, "Cannot reload with running modal operators");
    return OPERATOR_CANCELLED;
  }

  /* TODO(@ideasman42): this crashes on netrender and keying sets, need to look into why
   * disable for now unless running in debug mode. */

  /* It would be nice if we could detect when this is called from the Python
   * only postponing in that case, for now always do it. */
  if (true) {
    /* Postpone when called from Python so this can be called from an operator
     * that might be re-registered, crashing Blender when we try to read from the
     * freed operator type which, see #80694. */
    const char *imports[] = {"bpy", nullptr};
    BPY_run_string_exec(C,
                        imports,
                        "def fn():\n"
                        "    bpy.utils.load_scripts(reload_scripts=True)\n"
                        "    return None\n"
                        "bpy.app.timers.register(fn)");
  }
  else {
    WM_cursor_wait(true);
    const char *imports[] = {"bpy", nullptr};
    BPY_run_string_eval(C, imports, "bpy.utils.load_scripts(reload_scripts=True)");
    WM_cursor_wait(false);
  }

  /* Note that #WM_script_tag_reload is called from `bpy.utils.load_scripts`,
   * any additional updates required by this operator should go there. */

  return OPERATOR_FINISHED;
#else
  UNUSED_VARS(C, op);
  return OPERATOR_CANCELLED;
#endif
}

void SCRIPT_OT_reload(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Reload Scripts";
  ot->description = "Reload scripts";
  ot->idname = "SCRIPT_OT_reload";

  /* API callbacks. */
  ot->exec = script_reload_exec;
}
