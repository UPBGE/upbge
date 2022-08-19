/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup wm
 *
 * UI List Registry.
 */

#include <stdio.h>
#include <string.h>

#include "BLI_listbase.h"
#include "BLI_sys_types.h"

#include "DNA_space_types.h"
#include "DNA_windowmanager_types.h"

#include "MEM_guardedalloc.h"

#include "UI_interface.h"

#include "BLI_ghash.h"
#include "BLI_string.h"
#include "BLI_utildefines.h"

#include "BKE_main.h"
#include "BKE_screen.h"

#include "WM_api.h"
#include "WM_types.h"

static GHash *uilisttypes_hash = NULL;

uiListType *WM_uilisttype_find(const char *idname, bool quiet)
{
  if (idname[0]) {
    uiListType *ult = BLI_ghash_lookup(uilisttypes_hash, idname);
    if (ult) {
      return ult;
    }
  }

  if (!quiet) {
    printf("search for unknown uilisttype %s\n", idname);
  }

  return NULL;
}

bool WM_uilisttype_add(uiListType *ult)
{
  BLI_ghash_insert(uilisttypes_hash, ult->idname, ult);
  return 1;
}

static void wm_uilisttype_unlink_from_region(const uiListType *ult, ARegion *region)
{
  LISTBASE_FOREACH (uiList *, list, &region->ui_lists) {
    if (list->type == ult) {
      /* Don't delete the list, it's not just runtime data but stored in files. Freeing would make
       * that data get lost. */
      list->type = NULL;
    }
  }
}

static void wm_uilisttype_unlink_from_area(const uiListType *ult, ScrArea *area)
{
  LISTBASE_FOREACH (SpaceLink *, space_link, &area->spacedata) {
    ListBase *regionbase = (space_link == area->spacedata.first) ? &area->regionbase :
                                                                   &space_link->regionbase;
    LISTBASE_FOREACH (ARegion *, region, regionbase) {
      wm_uilisttype_unlink_from_region(ult, region);
    }
  }
}

/**
 * For all lists representing \a ult, clear their `uiListType` pointer. Use when a list-type is
 * deleted, so that the UI doesn't keep references to it.
 *
 * This is a common pattern for unregistering (usually .py defined) types at runtime, e.g. see
 * #WM_gizmomaptype_group_unlink().
 * Note that unlike in some other cases using this pattern, we don't actually free the lists with
 * type \a ult, we just clear the reference to the type. That's because UI-Lists are written to
 * files and we don't want them to get lost together with their (user visible) settings.
 */
static void wm_uilisttype_unlink(Main *bmain, const uiListType *ult)
{
  for (wmWindowManager *wm = bmain->wm.first; wm != NULL; wm = wm->id.next) {
    LISTBASE_FOREACH (wmWindow *, win, &wm->windows) {
      LISTBASE_FOREACH (ScrArea *, global_area, &win->global_areas.areabase) {
        wm_uilisttype_unlink_from_area(ult, global_area);
      }
    }
  }

  for (bScreen *screen = bmain->screens.first; screen != NULL; screen = screen->id.next) {
    LISTBASE_FOREACH (ScrArea *, area, &screen->areabase) {
      wm_uilisttype_unlink_from_area(ult, area);
    }

    LISTBASE_FOREACH (ARegion *, region, &screen->regionbase) {
      wm_uilisttype_unlink_from_region(ult, region);
    }
  }
}

void WM_uilisttype_remove_ptr(Main *bmain, uiListType *ult)
{
  wm_uilisttype_unlink(bmain, ult);

  bool ok = BLI_ghash_remove(uilisttypes_hash, ult->idname, NULL, MEM_freeN);

  BLI_assert(ok);
  UNUSED_VARS_NDEBUG(ok);
}

void WM_uilisttype_init(void)
{
  uilisttypes_hash = BLI_ghash_str_new_ex("uilisttypes_hash gh", 16);
}

void WM_uilisttype_free(void)
{
  GHashIterator gh_iter;
  GHASH_ITER (gh_iter, uilisttypes_hash) {
    uiListType *ult = BLI_ghashIterator_getValue(&gh_iter);
    if (ult->rna_ext.free) {
      ult->rna_ext.free(ult->rna_ext.data);
    }
  }

  BLI_ghash_free(uilisttypes_hash, NULL, MEM_freeN);
  uilisttypes_hash = NULL;
}

void WM_uilisttype_to_full_list_id(const uiListType *ult,
                                   const char *list_id,
                                   char r_full_list_id[/*UI_MAX_NAME_STR*/])
{
  /* We tag the list id with the list type... */
  BLI_snprintf(r_full_list_id, UI_MAX_NAME_STR, "%s_%s", ult->idname, list_id ? list_id : "");
}

const char *WM_uilisttype_list_id_get(const uiListType *ult, uiList *list)
{
  /* Some sanity check for the assumed behavior of #WM_uilisttype_to_full_list_id(). */
  BLI_assert((list->list_id + strlen(ult->idname))[0] == '_');
  /* +1 to skip the '_' */
  return list->list_id + strlen(ult->idname) + 1;
}
