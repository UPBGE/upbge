/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2016 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup editor/io
 */

#include "MEM_guardedalloc.h"

#include "DNA_cachefile_types.h"
#include "DNA_space_types.h"

#include "BLI_listbase.h"
#include "BLI_path_util.h"
#include "BLI_string.h"

#include "BKE_cachefile.h"
#include "BKE_context.h"
#include "BKE_lib_id.h"
#include "BKE_main.h"
#include "BKE_report.h"

#include "RNA_access.h"
#include "RNA_define.h"

#include "DEG_depsgraph.h"

#include "UI_interface.h"

#include "WM_api.h"
#include "WM_types.h"

#include "io_cache.h"

static void reload_cachefile(bContext *C, CacheFile *cache_file)
{
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  BKE_cachefile_reload(depsgraph, cache_file);
}

static void cachefile_init(bContext *C, wmOperator *op)
{
  PropertyPointerRNA *pprop;

  op->customdata = pprop = MEM_callocN(sizeof(PropertyPointerRNA), "OpenPropertyPointerRNA");
  UI_context_active_but_prop_get_templateID(C, &pprop->ptr, &pprop->prop);
}

static int cachefile_open_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  if (!RNA_struct_property_is_set(op->ptr, "filepath")) {
    char filepath[FILE_MAX];
    Main *bmain = CTX_data_main(C);

    BLI_strncpy(filepath, BKE_main_blendfile_path(bmain), sizeof(filepath));
    BLI_path_extension_replace(filepath, sizeof(filepath), ".abc");
    RNA_string_set(op->ptr, "filepath", filepath);
  }

  cachefile_init(C, op);

  WM_event_add_fileselect(C, op);

  return OPERATOR_RUNNING_MODAL;

  UNUSED_VARS(event);
}

static void open_cancel(bContext *UNUSED(C), wmOperator *op)
{
  MEM_freeN(op->customdata);
  op->customdata = NULL;
}

static int cachefile_open_exec(bContext *C, wmOperator *op)
{
  if (!RNA_struct_property_is_set(op->ptr, "filepath")) {
    BKE_report(op->reports, RPT_ERROR, "No filepath given");
    return OPERATOR_CANCELLED;
  }

  char filepath[FILE_MAX];
  RNA_string_get(op->ptr, "filepath", filepath);

  Main *bmain = CTX_data_main(C);

  CacheFile *cache_file = BKE_libblock_alloc(bmain, ID_CF, BLI_path_basename(filepath), 0);
  BLI_strncpy(cache_file->filepath, filepath, FILE_MAX);
  DEG_id_tag_update(&cache_file->id, ID_RECALC_COPY_ON_WRITE);

  /* Will be set when running invoke, not exec directly. */
  if (op->customdata != NULL) {
    /* hook into UI */
    PropertyPointerRNA *pprop = op->customdata;
    if (pprop->prop) {
      /* When creating new ID blocks, use is already 1, but RNA
       * pointer see also increases user, so this compensates it. */
      id_us_min(&cache_file->id);

      PointerRNA idptr;
      RNA_id_pointer_create(&cache_file->id, &idptr);
      RNA_property_pointer_set(&pprop->ptr, pprop->prop, idptr, NULL);
      RNA_property_update(C, &pprop->ptr, pprop->prop);
    }

    MEM_freeN(op->customdata);
  }

  return OPERATOR_FINISHED;
}

void CACHEFILE_OT_open(wmOperatorType *ot)
{
  ot->name = "Open Cache File";
  ot->description = "Load a cache file";
  ot->idname = "CACHEFILE_OT_open";

  ot->invoke = cachefile_open_invoke;
  ot->exec = cachefile_open_exec;
  ot->cancel = open_cancel;

  WM_operator_properties_filesel(ot,
                                 FILE_TYPE_ALEMBIC | FILE_TYPE_FOLDER,
                                 FILE_BLENDER,
                                 FILE_OPENFILE,
                                 WM_FILESEL_FILEPATH | WM_FILESEL_RELPATH,
                                 FILE_DEFAULTDISPLAY,
                                 FILE_SORT_DEFAULT);
}

/* ***************************** Reload Operator **************************** */

static int cachefile_reload_exec(bContext *C, wmOperator *UNUSED(op))
{
  CacheFile *cache_file = CTX_data_edit_cachefile(C);

  if (!cache_file) {
    return OPERATOR_CANCELLED;
  }

  reload_cachefile(C, cache_file);

  return OPERATOR_FINISHED;
}

void CACHEFILE_OT_reload(wmOperatorType *ot)
{
  ot->name = "Refresh Archive";
  ot->description = "Update objects paths list with new data from the archive";
  ot->idname = "CACHEFILE_OT_reload";

  /* api callbacks */
  ot->exec = cachefile_reload_exec;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/* ***************************** Add Layer Operator **************************** */

static int cachefile_layer_open_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  if (!RNA_struct_property_is_set(op->ptr, "filepath")) {
    char filepath[FILE_MAX];
    Main *bmain = CTX_data_main(C);

    BLI_strncpy(filepath, BKE_main_blendfile_path(bmain), sizeof(filepath));
    BLI_path_extension_replace(filepath, sizeof(filepath), ".abc");
    RNA_string_set(op->ptr, "filepath", filepath);
  }

  /* There is no more CacheFile set when returning from the file selector, so store it here. */
  op->customdata = CTX_data_edit_cachefile(C);

  WM_event_add_fileselect(C, op);

  return OPERATOR_RUNNING_MODAL;

  UNUSED_VARS(event);
}

static int cachefile_layer_add_exec(bContext *C, wmOperator *op)
{
  if (!RNA_struct_property_is_set(op->ptr, "filepath")) {
    BKE_report(op->reports, RPT_ERROR, "No filepath given");
    return OPERATOR_CANCELLED;
  }

  CacheFile *cache_file = op->customdata;

  if (!cache_file) {
    return OPERATOR_CANCELLED;
  }

  char filepath[FILE_MAX];
  RNA_string_get(op->ptr, "filepath", filepath);

  CacheFileLayer *layer = BKE_cachefile_add_layer(cache_file, filepath);

  if (!layer) {
    WM_report(RPT_ERROR, "Could not add a layer to the cache file");
    return OPERATOR_CANCELLED;
  }

  reload_cachefile(C, cache_file);
  WM_main_add_notifier(NC_OBJECT | ND_DRAW, NULL);
  return OPERATOR_FINISHED;
}

void CACHEFILE_OT_layer_add(wmOperatorType *ot)
{
  ot->name = "Add layer";
  ot->description = "Add an override layer to the archive";
  ot->idname = "CACHEFILE_OT_layer_add";

  /* api callbacks */
  ot->invoke = cachefile_layer_open_invoke;
  ot->exec = cachefile_layer_add_exec;

  WM_operator_properties_filesel(ot,
                                 FILE_TYPE_ALEMBIC | FILE_TYPE_FOLDER,
                                 FILE_BLENDER,
                                 FILE_OPENFILE,
                                 WM_FILESEL_FILEPATH | WM_FILESEL_RELPATH,
                                 FILE_DEFAULTDISPLAY,
                                 FILE_SORT_DEFAULT);
}

/* ***************************** Remove Layer Operator **************************** */

static int cachefile_layer_remove_exec(bContext *C, wmOperator *UNUSED(op))
{
  CacheFile *cache_file = CTX_data_edit_cachefile(C);

  if (!cache_file) {
    return OPERATOR_CANCELLED;
  }

  CacheFileLayer *layer = BKE_cachefile_get_active_layer(cache_file);
  BKE_cachefile_remove_layer(cache_file, layer);

  reload_cachefile(C, cache_file);
  WM_main_add_notifier(NC_OBJECT | ND_DRAW, NULL);
  return OPERATOR_FINISHED;
}

void CACHEFILE_OT_layer_remove(wmOperatorType *ot)
{
  ot->name = "Add layer";
  ot->description = "Remove an override layer to the archive";
  ot->idname = "CACHEFILE_OT_layer_remove";

  /* api callbacks */
  ot->exec = cachefile_layer_remove_exec;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/* ***************************** Move Layer Operator **************************** */

static int cachefile_layer_move_exec(bContext *C, wmOperator *op)
{
  CacheFile *cache_file = CTX_data_edit_cachefile(C);

  if (!cache_file) {
    return OPERATOR_CANCELLED;
  }

  CacheFileLayer *layer = BKE_cachefile_get_active_layer(cache_file);

  if (!layer) {
    return OPERATOR_CANCELLED;
  }

  const int dir = RNA_enum_get(op->ptr, "direction");

  if (BLI_listbase_link_move(&cache_file->layers, layer, dir)) {
    cache_file->active_layer = BLI_findindex(&cache_file->layers, layer) + 1;
    /* Only reload if something moved, might be expensive. */
    reload_cachefile(C, cache_file);
    WM_main_add_notifier(NC_OBJECT | ND_DRAW, NULL);
  }

  return OPERATOR_FINISHED;
}

void CACHEFILE_OT_layer_move(wmOperatorType *ot)
{
  static const EnumPropertyItem layer_slot_move[] = {
      {-1, "UP", 0, "Up", ""},
      {1, "DOWN", 0, "Down", ""},
      {0, NULL, 0, NULL, NULL},
  };

  ot->name = "Move layer";
  ot->description =
      "Move layer in the list, layers further down the list will overwrite data from the layers "
      "higher up";
  ot->idname = "CACHEFILE_OT_layer_move";

  /* api callbacks */
  ot->exec = cachefile_layer_move_exec;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_enum(ot->srna,
               "direction",
               layer_slot_move,
               0,
               "Direction",
               "Direction to move the active vertex group towards");
}
