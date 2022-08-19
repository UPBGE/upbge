/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup wm
 *
 * Panel Registry.
 *
 * \note Unlike menu, and other registries, this doesn't *own* the PanelType.
 *
 * For popups/popovers only, regions handle panel types by including them in local lists.
 */

#include <stdio.h>

#include "BLI_sys_types.h"

#include "DNA_windowmanager_types.h"

#include "BLI_ghash.h"
#include "BLI_utildefines.h"

#include "BKE_screen.h"

#include "WM_api.h"

static GHash *g_paneltypes_hash = NULL;

PanelType *WM_paneltype_find(const char *idname, bool quiet)
{
  if (idname[0]) {
    PanelType *pt = BLI_ghash_lookup(g_paneltypes_hash, idname);
    if (pt) {
      return pt;
    }
  }

  if (!quiet) {
    printf("search for unknown paneltype %s\n", idname);
  }

  return NULL;
}

bool WM_paneltype_add(PanelType *pt)
{
  BLI_ghash_insert(g_paneltypes_hash, pt->idname, pt);
  return true;
}

void WM_paneltype_remove(PanelType *pt)
{
  const bool ok = BLI_ghash_remove(g_paneltypes_hash, pt->idname, NULL, NULL);

  BLI_assert(ok);
  UNUSED_VARS_NDEBUG(ok);
}

void WM_paneltype_init(void)
{
  /* reserve size is set based on blender default setup */
  g_paneltypes_hash = BLI_ghash_str_new_ex("g_paneltypes_hash gh", 512);
}

void WM_paneltype_clear(void)
{
  BLI_ghash_free(g_paneltypes_hash, NULL, NULL);
}

void WM_paneltype_idname_visit_for_search(const bContext *UNUSED(C),
                                          PointerRNA *UNUSED(ptr),
                                          PropertyRNA *UNUSED(prop),
                                          const char *UNUSED(edit_text),
                                          StringPropertySearchVisitFunc visit_fn,
                                          void *visit_user_data)
{
  GHashIterator gh_iter;
  GHASH_ITER (gh_iter, g_paneltypes_hash) {
    PanelType *pt = BLI_ghashIterator_getValue(&gh_iter);

    StringPropertySearchVisitParams visit_params = {NULL};
    visit_params.text = pt->idname;
    visit_params.info = pt->label;
    visit_fn(visit_user_data, &visit_params);
  }
}
