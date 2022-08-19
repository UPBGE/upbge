/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup wm
 *
 * Operator Registry.
 */

#include "MEM_guardedalloc.h"

#include "CLG_log.h"

#include "DNA_ID.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_userdef_types.h"
#include "DNA_windowmanager_types.h"

#include "BLT_translation.h"

#include "BLI_blenlib.h"
#include "BLI_ghash.h"
#include "BLI_utildefines.h"

#include "BKE_context.h"
#include "BKE_idprop.h"

#include "RNA_access.h"
#include "RNA_define.h"
#include "RNA_enum_types.h"
#include "RNA_prototypes.h"

#include "WM_api.h"
#include "WM_types.h"

#include "wm.h"
#include "wm_event_system.h"

#define UNDOCUMENTED_OPERATOR_TIP N_("(undocumented operator)")

static void wm_operatortype_free_macro(wmOperatorType *ot);

/* -------------------------------------------------------------------- */
/** \name Operator Type Registry
 * \{ */

static GHash *global_ops_hash = NULL;
/** Counter for operator-properties that should not be tagged with #OP_PROP_TAG_ADVANCED. */
static int ot_prop_basic_count = -1;

wmOperatorType *WM_operatortype_find(const char *idname, bool quiet)
{
  if (idname[0]) {
    wmOperatorType *ot;

    /* needed to support python style names without the _OT_ syntax */
    char idname_bl[OP_MAX_TYPENAME];
    WM_operator_bl_idname(idname_bl, idname);

    ot = BLI_ghash_lookup(global_ops_hash, idname_bl);
    if (ot) {
      return ot;
    }

    if (!quiet) {
      CLOG_INFO(
          WM_LOG_OPERATORS, 0, "search for unknown operator '%s', '%s'\n", idname_bl, idname);
    }
  }
  else {
    if (!quiet) {
      CLOG_INFO(WM_LOG_OPERATORS, 0, "search for empty operator");
    }
  }

  return NULL;
}

void WM_operatortype_iter(GHashIterator *ghi)
{
  BLI_ghashIterator_init(ghi, global_ops_hash);
}

/* -------------------------------------------------------------------- */
/** \name Operator Type Append
 * \{ */

static wmOperatorType *wm_operatortype_append__begin(void)
{
  wmOperatorType *ot = MEM_callocN(sizeof(wmOperatorType), "operatortype");

  BLI_assert(ot_prop_basic_count == -1);

  ot->srna = RNA_def_struct_ptr(&BLENDER_RNA, "", &RNA_OperatorProperties);
  RNA_def_struct_property_tags(ot->srna, rna_enum_operator_property_tags);
  /* Set the default i18n context now, so that opfunc can redefine it if needed! */
  RNA_def_struct_translation_context(ot->srna, BLT_I18NCONTEXT_OPERATOR_DEFAULT);
  ot->translation_context = BLT_I18NCONTEXT_OPERATOR_DEFAULT;
  ot->cursor_pending = WM_CURSOR_PICK_AREA;

  return ot;
}
static void wm_operatortype_append__end(wmOperatorType *ot)
{
  if (ot->name == NULL) {
    CLOG_ERROR(WM_LOG_OPERATORS, "Operator '%s' has no name property", ot->idname);
  }
  BLI_assert((ot->description == NULL) || (ot->description[0]));

  /* Allow calling _begin without _end in operatortype creation. */
  WM_operatortype_props_advanced_end(ot);

  /* XXX All ops should have a description but for now allow them not to. */
  RNA_def_struct_ui_text(
      ot->srna, ot->name, ot->description ? ot->description : UNDOCUMENTED_OPERATOR_TIP);
  RNA_def_struct_identifier(&BLENDER_RNA, ot->srna, ot->idname);

  BLI_ghash_insert(global_ops_hash, (void *)ot->idname, ot);
}

/* All ops in 1 list (for time being... needs evaluation later). */

void WM_operatortype_append(void (*opfunc)(wmOperatorType *))
{
  wmOperatorType *ot = wm_operatortype_append__begin();
  opfunc(ot);
  wm_operatortype_append__end(ot);
}

void WM_operatortype_append_ptr(void (*opfunc)(wmOperatorType *, void *), void *userdata)
{
  wmOperatorType *ot = wm_operatortype_append__begin();
  opfunc(ot, userdata);
  wm_operatortype_append__end(ot);
}

/** \} */

void WM_operatortype_remove_ptr(wmOperatorType *ot)
{
  BLI_assert(ot == WM_operatortype_find(ot->idname, false));

  RNA_struct_free(&BLENDER_RNA, ot->srna);

  if (ot->last_properties) {
    IDP_FreeProperty(ot->last_properties);
  }

  if (ot->macro.first) {
    wm_operatortype_free_macro(ot);
  }

  BLI_ghash_remove(global_ops_hash, ot->idname, NULL, NULL);

  WM_keyconfig_update_operatortype();

  MEM_freeN(ot);
}

bool WM_operatortype_remove(const char *idname)
{
  wmOperatorType *ot = WM_operatortype_find(idname, 0);

  if (ot == NULL) {
    return false;
  }

  WM_operatortype_remove_ptr(ot);

  return true;
}

void wm_operatortype_init(void)
{
  /* reserve size is set based on blender default setup */
  global_ops_hash = BLI_ghash_str_new_ex("wm_operatortype_init gh", 2048);
}

static void operatortype_ghash_free_cb(wmOperatorType *ot)
{
  if (ot->last_properties) {
    IDP_FreeProperty(ot->last_properties);
  }

  if (ot->macro.first) {
    wm_operatortype_free_macro(ot);
  }

  if (ot->rna_ext.srna) {
    /* python operator, allocs own string */
    MEM_freeN((void *)ot->idname);
  }

  MEM_freeN(ot);
}

void wm_operatortype_free(void)
{
  BLI_ghash_free(global_ops_hash, NULL, (GHashValFreeFP)operatortype_ghash_free_cb);
  global_ops_hash = NULL;
}

void WM_operatortype_props_advanced_begin(wmOperatorType *ot)
{
  if (ot_prop_basic_count == -1) {
    /* Don't do anything if _begin was called before, but not _end. */
    ot_prop_basic_count = RNA_struct_count_properties(ot->srna);
  }
}

void WM_operatortype_props_advanced_end(wmOperatorType *ot)
{
  PointerRNA struct_ptr;
  int counter = 0;

  if (ot_prop_basic_count == -1) {
    /* WM_operatortype_props_advanced_begin was not called. Don't do anything. */
    return;
  }

  WM_operator_properties_create_ptr(&struct_ptr, ot);

  RNA_STRUCT_BEGIN (&struct_ptr, prop) {
    counter++;
    if (counter > ot_prop_basic_count) {
      WM_operatortype_prop_tag(prop, OP_PROP_TAG_ADVANCED);
    }
  }
  RNA_STRUCT_END;

  ot_prop_basic_count = -1;
}

void WM_operatortype_last_properties_clear_all(void)
{
  GHashIterator iter;

  for (WM_operatortype_iter(&iter); (!BLI_ghashIterator_done(&iter));
       (BLI_ghashIterator_step(&iter))) {
    wmOperatorType *ot = BLI_ghashIterator_getValue(&iter);

    if (ot->last_properties) {
      IDP_FreeProperty(ot->last_properties);
      ot->last_properties = NULL;
    }
  }
}

void WM_operatortype_idname_visit_for_search(const bContext *UNUSED(C),
                                             PointerRNA *UNUSED(ptr),
                                             PropertyRNA *UNUSED(prop),
                                             const char *UNUSED(edit_text),
                                             StringPropertySearchVisitFunc visit_fn,
                                             void *visit_user_data)
{
  GHashIterator gh_iter;
  GHASH_ITER (gh_iter, global_ops_hash) {
    wmOperatorType *ot = BLI_ghashIterator_getValue(&gh_iter);

    char idname_py[OP_MAX_TYPENAME];
    WM_operator_py_idname(idname_py, ot->idname);

    StringPropertySearchVisitParams visit_params = {NULL};
    visit_params.text = idname_py;
    visit_params.info = ot->name;
    visit_fn(visit_user_data, &visit_params);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Operator Macro Type
 * \{ */

typedef struct {
  int retval;
} MacroData;

static void wm_macro_start(wmOperator *op)
{
  if (op->customdata == NULL) {
    op->customdata = MEM_callocN(sizeof(MacroData), "MacroData");
  }
}

static int wm_macro_end(wmOperator *op, int retval)
{
  if (retval & OPERATOR_CANCELLED) {
    MacroData *md = op->customdata;

    if (md->retval & OPERATOR_FINISHED) {
      retval |= OPERATOR_FINISHED;
      retval &= ~OPERATOR_CANCELLED;
    }
  }

  /* if modal is ending, free custom data */
  if (retval & (OPERATOR_FINISHED | OPERATOR_CANCELLED)) {
    if (op->customdata) {
      MEM_freeN(op->customdata);
      op->customdata = NULL;
    }
  }

  return retval;
}

/* macro exec only runs exec calls */
static int wm_macro_exec(bContext *C, wmOperator *op)
{
  int retval = OPERATOR_FINISHED;

  wm_macro_start(op);

  LISTBASE_FOREACH (wmOperator *, opm, &op->macro) {
    if (opm->type->exec) {
      retval = opm->type->exec(C, opm);
      OPERATOR_RETVAL_CHECK(retval);

      if (retval & OPERATOR_FINISHED) {
        MacroData *md = op->customdata;
        md->retval = OPERATOR_FINISHED; /* keep in mind that at least one operator finished */
      }
      else {
        break; /* operator didn't finish, end macro */
      }
    }
    else {
      CLOG_WARN(WM_LOG_OPERATORS, "'%s' can't exec macro", opm->type->idname);
    }
  }

  return wm_macro_end(op, retval);
}

static int wm_macro_invoke_internal(bContext *C,
                                    wmOperator *op,
                                    const wmEvent *event,
                                    wmOperator *opm)
{
  int retval = OPERATOR_FINISHED;

  /* start from operator received as argument */
  for (; opm; opm = opm->next) {
    if (opm->type->invoke) {
      retval = opm->type->invoke(C, opm, event);
    }
    else if (opm->type->exec) {
      retval = opm->type->exec(C, opm);
    }

    OPERATOR_RETVAL_CHECK(retval);

    BLI_movelisttolist(&op->reports->list, &opm->reports->list);

    if (retval & OPERATOR_FINISHED) {
      MacroData *md = op->customdata;
      md->retval = OPERATOR_FINISHED; /* keep in mind that at least one operator finished */
    }
    else {
      break; /* operator didn't finish, end macro */
    }
  }

  return wm_macro_end(op, retval);
}

static int wm_macro_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  wm_macro_start(op);
  return wm_macro_invoke_internal(C, op, event, op->macro.first);
}

static int wm_macro_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  wmOperator *opm = op->opm;
  int retval = OPERATOR_FINISHED;

  if (opm == NULL) {
    CLOG_ERROR(WM_LOG_OPERATORS, "macro error, calling NULL modal()");
  }
  else {
    retval = opm->type->modal(C, opm, event);
    OPERATOR_RETVAL_CHECK(retval);

    /* if we're halfway through using a tool and cancel it, clear the options T37149. */
    if (retval & OPERATOR_CANCELLED) {
      WM_operator_properties_clear(opm->ptr);
    }

    /* if this one is done but it's not the last operator in the macro */
    if ((retval & OPERATOR_FINISHED) && opm->next) {
      MacroData *md = op->customdata;

      md->retval = OPERATOR_FINISHED; /* keep in mind that at least one operator finished */

      retval = wm_macro_invoke_internal(C, op, event, opm->next);

      /* if new operator is modal and also added its own handler */
      if (retval & OPERATOR_RUNNING_MODAL && op->opm != opm) {
        wmWindow *win = CTX_wm_window(C);
        wmEventHandler_Op *handler;

        handler = BLI_findptr(&win->modalhandlers, op, offsetof(wmEventHandler_Op, op));
        if (handler) {
          BLI_remlink(&win->modalhandlers, handler);
          wm_event_free_handler(&handler->head);
        }

        /* If operator is blocking, grab cursor.
         * This may end up grabbing twice, but we don't care. */
        if (op->opm->type->flag & OPTYPE_BLOCKING) {
          int bounds[4] = {-1, -1, -1, -1};
          int wrap = WM_CURSOR_WRAP_NONE;

          if ((op->opm->flag & OP_IS_MODAL_GRAB_CURSOR) ||
              (op->opm->type->flag & OPTYPE_GRAB_CURSOR_XY)) {
            wrap = WM_CURSOR_WRAP_XY;
          }
          else if (op->opm->type->flag & OPTYPE_GRAB_CURSOR_X) {
            wrap = WM_CURSOR_WRAP_X;
          }
          else if (op->opm->type->flag & OPTYPE_GRAB_CURSOR_Y) {
            wrap = WM_CURSOR_WRAP_Y;
          }

          if (wrap) {
            ARegion *region = CTX_wm_region(C);
            if (region) {
              bounds[0] = region->winrct.xmin;
              bounds[1] = region->winrct.ymax;
              bounds[2] = region->winrct.xmax;
              bounds[3] = region->winrct.ymin;
            }
          }

          WM_cursor_grab_enable(win, wrap, false, bounds);
        }
      }
    }
  }

  return wm_macro_end(op, retval);
}

static void wm_macro_cancel(bContext *C, wmOperator *op)
{
  /* call cancel on the current modal operator, if any */
  if (op->opm && op->opm->type->cancel) {
    op->opm->type->cancel(C, op->opm);
  }

  wm_macro_end(op, OPERATOR_CANCELLED);
}

wmOperatorType *WM_operatortype_append_macro(const char *idname,
                                             const char *name,
                                             const char *description,
                                             int flag)
{
  wmOperatorType *ot;
  const char *i18n_context;

  if (WM_operatortype_find(idname, true)) {
    CLOG_ERROR(WM_LOG_OPERATORS, "operator %s exists, cannot create macro", idname);
    return NULL;
  }

  ot = MEM_callocN(sizeof(wmOperatorType), "operatortype");
  ot->srna = RNA_def_struct_ptr(&BLENDER_RNA, "", &RNA_OperatorProperties);

  ot->idname = idname;
  ot->name = name;
  ot->description = description;
  ot->flag = OPTYPE_MACRO | flag;

  ot->exec = wm_macro_exec;
  ot->invoke = wm_macro_invoke;
  ot->modal = wm_macro_modal;
  ot->cancel = wm_macro_cancel;
  ot->poll = NULL;

  /* XXX All ops should have a description but for now allow them not to. */
  BLI_assert((ot->description == NULL) || (ot->description[0]));

  RNA_def_struct_ui_text(
      ot->srna, ot->name, ot->description ? ot->description : UNDOCUMENTED_OPERATOR_TIP);
  RNA_def_struct_identifier(&BLENDER_RNA, ot->srna, ot->idname);
  /* Use i18n context from rna_ext.srna if possible (py operators). */
  i18n_context = ot->rna_ext.srna ? RNA_struct_translation_context(ot->rna_ext.srna) :
                                    BLT_I18NCONTEXT_OPERATOR_DEFAULT;
  RNA_def_struct_translation_context(ot->srna, i18n_context);
  ot->translation_context = i18n_context;

  BLI_ghash_insert(global_ops_hash, (void *)ot->idname, ot);

  return ot;
}

void WM_operatortype_append_macro_ptr(void (*opfunc)(wmOperatorType *, void *), void *userdata)
{
  wmOperatorType *ot;

  ot = MEM_callocN(sizeof(wmOperatorType), "operatortype");
  ot->srna = RNA_def_struct_ptr(&BLENDER_RNA, "", &RNA_OperatorProperties);

  ot->flag = OPTYPE_MACRO;
  ot->exec = wm_macro_exec;
  ot->invoke = wm_macro_invoke;
  ot->modal = wm_macro_modal;
  ot->cancel = wm_macro_cancel;
  ot->poll = NULL;

  /* XXX All ops should have a description but for now allow them not to. */
  BLI_assert((ot->description == NULL) || (ot->description[0]));

  /* Set the default i18n context now, so that opfunc can redefine it if needed! */
  RNA_def_struct_translation_context(ot->srna, BLT_I18NCONTEXT_OPERATOR_DEFAULT);
  ot->translation_context = BLT_I18NCONTEXT_OPERATOR_DEFAULT;
  opfunc(ot, userdata);

  RNA_def_struct_ui_text(
      ot->srna, ot->name, ot->description ? ot->description : UNDOCUMENTED_OPERATOR_TIP);
  RNA_def_struct_identifier(&BLENDER_RNA, ot->srna, ot->idname);

  BLI_ghash_insert(global_ops_hash, (void *)ot->idname, ot);
}

wmOperatorTypeMacro *WM_operatortype_macro_define(wmOperatorType *ot, const char *idname)
{
  wmOperatorTypeMacro *otmacro = MEM_callocN(sizeof(wmOperatorTypeMacro), "wmOperatorTypeMacro");

  BLI_strncpy(otmacro->idname, idname, OP_MAX_TYPENAME);

  /* do this on first use, since operatordefinitions might have been not done yet */
  WM_operator_properties_alloc(&(otmacro->ptr), &(otmacro->properties), idname);
  WM_operator_properties_sanitize(otmacro->ptr, 1);

  BLI_addtail(&ot->macro, otmacro);

  {
    /* operator should always be found but in the event its not. don't segfault */
    wmOperatorType *otsub = WM_operatortype_find(idname, 0);
    if (otsub) {
      RNA_def_pointer_runtime(
          ot->srna, otsub->idname, otsub->srna, otsub->name, otsub->description);
    }
  }

  return otmacro;
}

static void wm_operatortype_free_macro(wmOperatorType *ot)
{
  LISTBASE_FOREACH (wmOperatorTypeMacro *, otmacro, &ot->macro) {
    if (otmacro->ptr) {
      WM_operator_properties_free(otmacro->ptr);
      MEM_freeN(otmacro->ptr);
    }
  }
  BLI_freelistN(&ot->macro);
}

const char *WM_operatortype_name(struct wmOperatorType *ot, struct PointerRNA *properties)
{
  const char *name = NULL;

  if (ot->get_name && properties) {
    name = ot->get_name(ot, properties);
  }

  return (name && name[0]) ? name : RNA_struct_ui_name(ot->srna);
}

char *WM_operatortype_description(struct bContext *C,
                                  struct wmOperatorType *ot,
                                  struct PointerRNA *properties)
{
  if (ot->get_description && properties) {
    char *description = ot->get_description(C, ot, properties);

    if (description) {
      if (description[0]) {
        return description;
      }
      MEM_freeN(description);
    }
  }

  const char *info = RNA_struct_ui_description(ot->srna);
  if (info && info[0]) {
    return BLI_strdup(info);
  }
  return NULL;
}

char *WM_operatortype_description_or_name(struct bContext *C,
                                          struct wmOperatorType *ot,
                                          struct PointerRNA *properties)
{
  char *text = WM_operatortype_description(C, ot, properties);
  if (text == NULL) {
    const char *text_orig = WM_operatortype_name(ot, properties);
    if (text_orig != NULL) {
      text = BLI_strdup(text_orig);
    }
  }
  return text;
}

/** \} */
