/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2001-2002 NaN Holding BV. All rights reserved. */

/** \file
 * \ingroup bke
 *
 * Contains code specific to the `Library` ID type.
 */

#include "CLG_log.h"

#include "MEM_guardedalloc.h"

/* all types are needed here, in order to do memory operations */
#include "DNA_ID.h"

#include "BLI_utildefines.h"

#include "BLI_blenlib.h"

#include "BLT_translation.h"

#include "BKE_bpath.h"
#include "BKE_idtype.h"
#include "BKE_lib_id.h"
#include "BKE_lib_query.h"
#include "BKE_library.h"
#include "BKE_main.h"
#include "BKE_main_namemap.h"
#include "BKE_packedFile.h"

/* Unused currently. */
// static CLG_LogRef LOG = {.identifier = "bke.library"};

struct BlendWriter;
struct BlendDataReader;

static void library_runtime_reset(Library *lib)
{
  if (lib->runtime.name_map) {
    BKE_main_namemap_destroy(&lib->runtime.name_map);
  }
}

static void library_free_data(ID *id)
{
  Library *library = (Library *)id;
  library_runtime_reset(library);
  if (library->packedfile) {
    BKE_packedfile_free(library->packedfile);
  }
}

static void library_foreach_id(ID *id, LibraryForeachIDData *data)
{
  Library *lib = (Library *)id;
  BKE_LIB_FOREACHID_PROCESS_IDSUPER(data, lib->parent, IDWALK_CB_NEVER_SELF);
}

static void library_foreach_path(ID *id, BPathForeachPathData *bpath_data)
{
  Library *lib = (Library *)id;

  /* FIXME: Find if we should respect #BKE_BPATH_FOREACH_PATH_SKIP_PACKED here, and if not, explain
   * why. */
  if (lib->packedfile !=
      NULL /*&& (bpath_data->flag & BKE_BPATH_FOREACH_PATH_SKIP_PACKED) != 0 */) {
    return;
  }

  if (BKE_bpath_foreach_path_fixed_process(bpath_data, lib->filepath)) {
    BKE_library_filepath_set(bpath_data->bmain, lib, lib->filepath);
  }
}

static void library_blend_read_data(struct BlendDataReader *UNUSED(reader), ID *id)
{
  Library *lib = (Library *)id;
  lib->runtime.name_map = NULL;
}

IDTypeInfo IDType_ID_LI = {
    .id_code = ID_LI,
    .id_filter = FILTER_ID_LI,
    .main_listbase_index = INDEX_ID_LI,
    .struct_size = sizeof(Library),
    .name = "Library",
    .name_plural = "libraries",
    .translation_context = BLT_I18NCONTEXT_ID_LIBRARY,
    .flags = IDTYPE_FLAGS_NO_COPY | IDTYPE_FLAGS_NO_LIBLINKING | IDTYPE_FLAGS_NO_ANIMDATA,
    .asset_type_info = NULL,

    .init_data = NULL,
    .copy_data = NULL,
    .free_data = library_free_data,
    .make_local = NULL,
    .foreach_id = library_foreach_id,
    .foreach_cache = NULL,
    .foreach_path = library_foreach_path,
    .owner_get = NULL,

    .blend_write = NULL,
    .blend_read_data = library_blend_read_data,
    .blend_read_lib = NULL,
    .blend_read_expand = NULL,

    .blend_read_undo_preserve = NULL,

    .lib_override_apply_post = NULL,
};

void BKE_library_filepath_set(Main *bmain, Library *lib, const char *filepath)
{
  /* in some cases this is used to update the absolute path from the
   * relative */
  if (lib->filepath != filepath) {
    BLI_strncpy(lib->filepath, filepath, sizeof(lib->filepath));
  }

  BLI_strncpy(lib->filepath_abs, filepath, sizeof(lib->filepath_abs));

  /* Not essential but set `filepath_abs` is an absolute copy of value which
   * is more useful if its kept in sync. */
  if (BLI_path_is_rel(lib->filepath_abs)) {
    /* NOTE(@campbellbarton): the file may be unsaved, in this case, setting the
     * `filepath_abs` on an indirectly linked path is not allowed from the
     * outliner, and its not really supported but allow from here for now
     * since making local could cause this to be directly linked.
     */
    /* Never make paths relative to parent lib - reading code (blenloader) always set *all*
     * `lib->filepath` relative to current main, not to their parent for indirectly linked ones. */
    const char *blendfile_path = BKE_main_blendfile_path(bmain);
    BLI_path_abs(lib->filepath_abs, blendfile_path);
  }
}
