/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <stddef.h>
#include <stdlib.h>

#include "RNA_types.h"

#include "BLI_ghash.h"
#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLI_utildefines.h"

#include "BKE_addon.h" /* own include */
#include "BKE_idprop.h"

#include "DNA_listBase.h"
#include "DNA_userdef_types.h"

#include "MEM_guardedalloc.h"

#include "CLG_log.h"

static CLG_LogRef LOG = {"bke.addon"};

/* -------------------------------------------------------------------- */
/** \name Add-on New/Free
 * \{ */

bAddon *BKE_addon_new(void)
{
  bAddon *addon = MEM_callocN(sizeof(bAddon), "bAddon");
  return addon;
}

bAddon *BKE_addon_find(ListBase *addon_list, const char *module)
{
  return BLI_findstring(addon_list, module, offsetof(bAddon, module));
}

bAddon *BKE_addon_ensure(ListBase *addon_list, const char *module)
{
  bAddon *addon = BKE_addon_find(addon_list, module);
  if (addon == NULL) {
    addon = BKE_addon_new();
    BLI_strncpy(addon->module, module, sizeof(addon->module));
    BLI_addtail(addon_list, addon);
  }
  return addon;
}

bool BKE_addon_remove_safe(ListBase *addon_list, const char *module)
{
  bAddon *addon = BLI_findstring(addon_list, module, offsetof(bAddon, module));
  if (addon) {
    BLI_remlink(addon_list, addon);
    BKE_addon_free(addon);
    return true;
  }
  return false;
}

void BKE_addon_free(bAddon *addon)
{
  if (addon->prop) {
    IDP_FreeProperty(addon->prop);
  }
  MEM_freeN(addon);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Add-on Preference API
 * \{ */

static GHash *global_addonpreftype_hash = NULL;

bAddonPrefType *BKE_addon_pref_type_find(const char *idname, bool quiet)
{
  if (idname[0]) {
    bAddonPrefType *apt;

    apt = BLI_ghash_lookup(global_addonpreftype_hash, idname);
    if (apt) {
      return apt;
    }

    if (!quiet) {
      CLOG_WARN(&LOG, "search for unknown addon-pref '%s'", idname);
    }
  }
  else {
    if (!quiet) {
      CLOG_WARN(&LOG, "search for empty addon-pref");
    }
  }

  return NULL;
}

void BKE_addon_pref_type_add(bAddonPrefType *apt)
{
  BLI_ghash_insert(global_addonpreftype_hash, apt->idname, apt);
}

void BKE_addon_pref_type_remove(const bAddonPrefType *apt)
{
  BLI_ghash_remove(global_addonpreftype_hash, apt->idname, NULL, MEM_freeN);
}

void BKE_addon_pref_type_init(void)
{
  BLI_assert(global_addonpreftype_hash == NULL);
  global_addonpreftype_hash = BLI_ghash_str_new(__func__);
}

void BKE_addon_pref_type_free(void)
{
  BLI_ghash_free(global_addonpreftype_hash, NULL, MEM_freeN);
  global_addonpreftype_hash = NULL;
}

/** \} */
