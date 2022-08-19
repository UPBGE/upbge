/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup wm
 *
 * Menu Registry.
 */

#include <stdio.h>

#include "BLI_sys_types.h"

#include "DNA_windowmanager_types.h"
#include "DNA_workspace_types.h"

#include "MEM_guardedalloc.h"

#include "BLI_ghash.h"
#include "BLI_utildefines.h"

#include "BKE_context.h"
#include "BKE_screen.h"
#include "BKE_workspace.h"

#include "WM_api.h"
#include "WM_types.h"

static GHash *menutypes_hash = NULL;

MenuType *WM_menutype_find(const char *idname, bool quiet)
{
  if (idname[0]) {
    MenuType *mt = BLI_ghash_lookup(menutypes_hash, idname);
    if (mt) {
      return mt;
    }
  }

  if (!quiet) {
    printf("search for unknown menutype %s\n", idname);
  }

  return NULL;
}

void WM_menutype_iter(GHashIterator *ghi)
{
  BLI_ghashIterator_init(ghi, menutypes_hash);
}

bool WM_menutype_add(MenuType *mt)
{
  BLI_assert((mt->description == NULL) || (mt->description[0]));
  BLI_ghash_insert(menutypes_hash, mt->idname, mt);
  return true;
}

void WM_menutype_freelink(MenuType *mt)
{
  bool ok = BLI_ghash_remove(menutypes_hash, mt->idname, NULL, MEM_freeN);

  BLI_assert(ok);
  UNUSED_VARS_NDEBUG(ok);
}

void WM_menutype_init(void)
{
  /* reserve size is set based on blender default setup */
  menutypes_hash = BLI_ghash_str_new_ex("menutypes_hash gh", 512);
}

void WM_menutype_free(void)
{
  GHashIterator gh_iter;

  GHASH_ITER (gh_iter, menutypes_hash) {
    MenuType *mt = BLI_ghashIterator_getValue(&gh_iter);
    if (mt->rna_ext.free) {
      mt->rna_ext.free(mt->rna_ext.data);
    }
  }

  BLI_ghash_free(menutypes_hash, NULL, MEM_freeN);
  menutypes_hash = NULL;
}

bool WM_menutype_poll(bContext *C, MenuType *mt)
{
  /* If we're tagged, only use compatible. */
  if (mt->owner_id[0] != '\0') {
    const WorkSpace *workspace = CTX_wm_workspace(C);
    if (BKE_workspace_owner_id_check(workspace, mt->owner_id) == false) {
      return false;
    }
  }

  if (mt->poll != NULL) {
    return mt->poll(C, mt);
  }
  return true;
}

void WM_menutype_idname_visit_for_search(const bContext *UNUSED(C),
                                         PointerRNA *UNUSED(ptr),
                                         PropertyRNA *UNUSED(prop),
                                         const char *UNUSED(edit_text),
                                         StringPropertySearchVisitFunc visit_fn,
                                         void *visit_user_data)
{
  GHashIterator gh_iter;
  GHASH_ITER (gh_iter, menutypes_hash) {
    MenuType *mt = BLI_ghashIterator_getValue(&gh_iter);

    StringPropertySearchVisitParams visit_params = {NULL};
    visit_params.text = mt->idname;
    visit_params.info = mt->label;
    visit_fn(visit_user_data, &visit_params);
  }
}
