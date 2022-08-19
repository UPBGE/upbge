/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "RNA_types.h"

#include "BLI_ghash.h"
#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLI_utildefines.h"

#include "DNA_listBase.h"
#include "DNA_userdef_types.h"
#include "DNA_windowmanager_types.h"

#include "BKE_idprop.h"
#include "BKE_keyconfig.h" /* own include */

#include "MEM_guardedalloc.h"

/* -------------------------------------------------------------------- */
/** \name Key-Config Preference (UserDef) API
 *
 * \see #BKE_addon_pref_type_init for logic this is bases on.
 * \{ */

wmKeyConfigPref *BKE_keyconfig_pref_ensure(UserDef *userdef, const char *kc_idname)
{
  wmKeyConfigPref *kpt = BLI_findstring(
      &userdef->user_keyconfig_prefs, kc_idname, offsetof(wmKeyConfigPref, idname));
  if (kpt == NULL) {
    kpt = MEM_callocN(sizeof(*kpt), __func__);
    STRNCPY(kpt->idname, kc_idname);
    BLI_addtail(&userdef->user_keyconfig_prefs, kpt);
  }
  if (kpt->prop == NULL) {
    IDPropertyTemplate val = {0};
    kpt->prop = IDP_New(IDP_GROUP, &val, kc_idname); /* name is unimportant. */
  }
  return kpt;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Key-Config Preference (RNA Type) API
 *
 * \see #BKE_addon_pref_type_init for logic this is bases on.
 * \{ */

static GHash *global_keyconfigpreftype_hash = NULL;

wmKeyConfigPrefType_Runtime *BKE_keyconfig_pref_type_find(const char *idname, bool quiet)
{
  if (idname[0]) {
    wmKeyConfigPrefType_Runtime *kpt_rt;

    kpt_rt = BLI_ghash_lookup(global_keyconfigpreftype_hash, idname);
    if (kpt_rt) {
      return kpt_rt;
    }

    if (!quiet) {
      printf("search for unknown keyconfig-pref '%s'\n", idname);
    }
  }
  else {
    if (!quiet) {
      printf("search for empty keyconfig-pref\n");
    }
  }

  return NULL;
}

void BKE_keyconfig_pref_type_add(wmKeyConfigPrefType_Runtime *kpt_rt)
{
  BLI_ghash_insert(global_keyconfigpreftype_hash, kpt_rt->idname, kpt_rt);
}

void BKE_keyconfig_pref_type_remove(const wmKeyConfigPrefType_Runtime *kpt_rt)
{
  BLI_ghash_remove(global_keyconfigpreftype_hash, kpt_rt->idname, NULL, MEM_freeN);
}

void BKE_keyconfig_pref_type_init(void)
{
  BLI_assert(global_keyconfigpreftype_hash == NULL);
  global_keyconfigpreftype_hash = BLI_ghash_str_new(__func__);
}

void BKE_keyconfig_pref_type_free(void)
{
  BLI_ghash_free(global_keyconfigpreftype_hash, NULL, MEM_freeN);
  global_keyconfigpreftype_hash = NULL;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Key-Config Versioning
 * \{ */

void BKE_keyconfig_pref_set_select_mouse(UserDef *userdef, int value, bool override)
{
  wmKeyConfigPref *kpt = BKE_keyconfig_pref_ensure(userdef, WM_KEYCONFIG_STR_DEFAULT);
  IDProperty *idprop = IDP_GetPropertyFromGroup(kpt->prop, "select_mouse");
  if (!idprop) {
    IDPropertyTemplate tmp = {
        .i = value,
    };
    IDP_AddToGroup(kpt->prop, IDP_New(IDP_INT, &tmp, "select_mouse"));
  }
  else if (override) {
    IDP_Int(idprop) = value;
  }
}

static void keymap_item_free(wmKeyMapItem *kmi)
{
  IDP_FreeProperty(kmi->properties);
  if (kmi->ptr) {
    MEM_freeN(kmi->ptr);
  }
  MEM_freeN(kmi);
}

static void keymap_diff_item_free(wmKeyMapDiffItem *kmdi)
{
  if (kmdi->add_item) {
    keymap_item_free(kmdi->add_item);
  }
  if (kmdi->remove_item) {
    keymap_item_free(kmdi->remove_item);
  }
  MEM_freeN(kmdi);
}

void BKE_keyconfig_keymap_filter_item(wmKeyMap *keymap,
                                      const struct wmKeyConfigFilterItemParams *params,
                                      bool (*filter_fn)(wmKeyMapItem *kmi, void *user_data),
                                      void *user_data)
{
  if (params->check_diff_item_add || params->check_diff_item_remove) {
    for (wmKeyMapDiffItem *kmdi = keymap->diff_items.first, *kmdi_next; kmdi; kmdi = kmdi_next) {
      kmdi_next = kmdi->next;
      bool remove = false;

      if (params->check_diff_item_add) {
        if (kmdi->add_item) {
          if (filter_fn(kmdi->add_item, user_data)) {
            remove = true;
          }
        }
      }

      if (!remove && params->check_diff_item_remove) {
        if (kmdi->remove_item) {
          if (filter_fn(kmdi->remove_item, user_data)) {
            remove = true;
          }
        }
      }

      if (remove) {
        BLI_remlink(&keymap->diff_items, kmdi);
        keymap_diff_item_free(kmdi);
      }
    }
  }

  if (params->check_item) {
    for (wmKeyMapItem *kmi = keymap->items.first, *kmi_next; kmi; kmi = kmi_next) {
      kmi_next = kmi->next;
      if (filter_fn(kmi, user_data)) {
        BLI_remlink(&keymap->items, kmi);
        keymap_item_free(kmi);
      }
    }
  }
}

void BKE_keyconfig_pref_filter_items(struct UserDef *userdef,
                                     const struct wmKeyConfigFilterItemParams *params,
                                     bool (*filter_fn)(wmKeyMapItem *kmi, void *user_data),
                                     void *user_data)
{
  LISTBASE_FOREACH (wmKeyMap *, keymap, &userdef->user_keymaps) {
    BKE_keyconfig_keymap_filter_item(keymap, params, filter_fn, user_data);
  }
}

/** \} */
