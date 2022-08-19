/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2014 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup bke
 */

#include <stdlib.h>

#include "DNA_anim_types.h"

#include "BLI_ghash.h"
#include "BLI_linklist_stack.h"
#include "BLI_listbase.h"
#include "BLI_utildefines.h"

#include "BKE_anim_data.h"
#include "BKE_idprop.h"
#include "BKE_idtype.h"
#include "BKE_lib_id.h"
#include "BKE_lib_query.h"
#include "BKE_main.h"
#include "BKE_node.h"

/* status */
enum {
  IDWALK_STOP = 1 << 0,
};

typedef struct LibraryForeachIDData {
  Main *bmain;
  /**
   * 'Real' ID, the one that might be in `bmain`, only differs from self_id when the later is a
   * private one.
   */
  ID *owner_id;
  /**
   * ID from which the current ID pointer is being processed. It may be an embedded ID like master
   * collection or root node tree.
   */
  ID *self_id;

  /** Flags controlling the behavior of the 'foreach id' looping code. */
  int flag;
  /** Generic flags to be passed to all callback calls for current processed data. */
  int cb_flag;
  /** Callback flags that are forbidden for all callback calls for current processed data. */
  int cb_flag_clear;

  /* Function to call for every ID pointers of current processed data, and its opaque user data
   * pointer. */
  LibraryIDLinkCallback callback;
  void *user_data;
  /** Store the returned value from the callback, to decide how to continue the processing of ID
   * pointers for current data. */
  int status;

  /* To handle recursion. */
  GSet *ids_handled; /* All IDs that are either already done, or still in ids_todo stack. */
  BLI_LINKSTACK_DECLARE(ids_todo, ID *);
} LibraryForeachIDData;

bool BKE_lib_query_foreachid_iter_stop(LibraryForeachIDData *data)
{
  return (data->status & IDWALK_STOP) != 0;
}

void BKE_lib_query_foreachid_process(LibraryForeachIDData *data, ID **id_pp, int cb_flag)
{
  if (BKE_lib_query_foreachid_iter_stop(data)) {
    return;
  }

  const int flag = data->flag;
  ID *old_id = *id_pp;

  /* Update the callback flags with the ones defined (or forbidden) in `data` by the generic
   * caller code. */
  cb_flag = ((cb_flag | data->cb_flag) & ~data->cb_flag_clear);

  /* Update the callback flags with some extra information regarding overrides: all 'loopback',
   * 'internal', 'embedded' etc. ID pointers are never overridable. */
  if (cb_flag & (IDWALK_CB_INTERNAL | IDWALK_CB_LOOPBACK | IDWALK_CB_OVERRIDE_LIBRARY_REFERENCE)) {
    cb_flag |= IDWALK_CB_OVERRIDE_LIBRARY_NOT_OVERRIDABLE;
  }

  const int callback_return = data->callback(
      &(struct LibraryIDLinkCallbackData){.user_data = data->user_data,
                                          .bmain = data->bmain,
                                          .id_owner = data->owner_id,
                                          .id_self = data->self_id,
                                          .id_pointer = id_pp,
                                          .cb_flag = cb_flag});
  if (flag & IDWALK_READONLY) {
    BLI_assert(*(id_pp) == old_id);
  }
  if (old_id && (flag & IDWALK_RECURSE)) {
    if (BLI_gset_add((data)->ids_handled, old_id)) {
      if (!(callback_return & IDWALK_RET_STOP_RECURSION)) {
        BLI_LINKSTACK_PUSH(data->ids_todo, old_id);
      }
    }
  }
  if (callback_return & IDWALK_RET_STOP_ITER) {
    data->status |= IDWALK_STOP;
  }
}

int BKE_lib_query_foreachid_process_flags_get(LibraryForeachIDData *data)
{
  return data->flag;
}

int BKE_lib_query_foreachid_process_callback_flag_override(LibraryForeachIDData *data,
                                                           const int cb_flag,
                                                           const bool do_replace)
{
  const int cb_flag_backup = data->cb_flag;
  if (do_replace) {
    data->cb_flag = cb_flag;
  }
  else {
    data->cb_flag |= cb_flag;
  }
  return cb_flag_backup;
}

static bool library_foreach_ID_link(Main *bmain,
                                    ID *id_owner,
                                    ID *id,
                                    LibraryIDLinkCallback callback,
                                    void *user_data,
                                    int flag,
                                    LibraryForeachIDData *inherit_data);

void BKE_lib_query_idpropertiesForeachIDLink_callback(IDProperty *id_prop, void *user_data)
{
  BLI_assert(id_prop->type == IDP_ID);

  LibraryForeachIDData *data = (LibraryForeachIDData *)user_data;
  const int cb_flag = IDWALK_CB_USER | ((id_prop->flag & IDP_FLAG_OVERRIDABLE_LIBRARY) ?
                                            0 :
                                            IDWALK_CB_OVERRIDE_LIBRARY_NOT_OVERRIDABLE);
  BKE_LIB_FOREACHID_PROCESS_ID(data, id_prop->data.pointer, cb_flag);
}

void BKE_library_foreach_ID_embedded(LibraryForeachIDData *data, ID **id_pp)
{
  /* Needed e.g. for callbacks handling relationships. This call shall be absolutely read-only. */
  ID *id = *id_pp;
  const int flag = data->flag;

  BKE_lib_query_foreachid_process(data, id_pp, IDWALK_CB_EMBEDDED);
  if (BKE_lib_query_foreachid_iter_stop(data)) {
    return;
  }
  BLI_assert(id == *id_pp);

  if (id == NULL) {
    return;
  }

  if (flag & IDWALK_IGNORE_EMBEDDED_ID) {
    /* Do Nothing. */
  }
  else if (flag & IDWALK_RECURSE) {
    /* Defer handling into main loop, recursively calling BKE_library_foreach_ID_link in
     * IDWALK_RECURSE case is troublesome, see T49553. */
    /* XXX note that this breaks the 'owner id' thing now, we likely want to handle that
     * differently at some point, but for now it should not be a problem in practice. */
    if (BLI_gset_add(data->ids_handled, id)) {
      BLI_LINKSTACK_PUSH(data->ids_todo, id);
    }
  }
  else {
    if (!library_foreach_ID_link(
            data->bmain, data->owner_id, id, data->callback, data->user_data, data->flag, data)) {
      data->status |= IDWALK_STOP;
      return;
    }
  }
}

static void library_foreach_ID_data_cleanup(LibraryForeachIDData *data)
{
  if (data->ids_handled != NULL) {
    BLI_gset_free(data->ids_handled, NULL);
    BLI_LINKSTACK_FREE(data->ids_todo);
  }
}

/** \return false in case iteration over ID pointers must be stopped, true otherwise. */
static bool library_foreach_ID_link(Main *bmain,
                                    ID *id_owner,
                                    ID *id,
                                    LibraryIDLinkCallback callback,
                                    void *user_data,
                                    int flag,
                                    LibraryForeachIDData *inherit_data)
{
  LibraryForeachIDData data = {.bmain = bmain};

  BLI_assert(inherit_data == NULL || data.bmain == inherit_data->bmain);

  if (flag & IDWALK_RECURSE) {
    /* For now, recursion implies read-only, and no internal pointers. */
    flag |= IDWALK_READONLY;
    flag &= ~IDWALK_DO_INTERNAL_RUNTIME_POINTERS;

    /* NOTE: This function itself should never be called recursively when IDWALK_RECURSE is set,
     * see also comments in #BKE_library_foreach_ID_embedded.
     * This is why we can always create this data here, and do not need to try and re-use it from
     * `inherit_data`. */
    data.ids_handled = BLI_gset_new(BLI_ghashutil_ptrhash, BLI_ghashutil_ptrcmp, __func__);
    BLI_LINKSTACK_INIT(data.ids_todo);

    BLI_gset_add(data.ids_handled, id);
  }
  else {
    data.ids_handled = NULL;
  }
  data.flag = flag;
  data.status = 0;
  data.callback = callback;
  data.user_data = user_data;

#define CALLBACK_INVOKE_ID(check_id, cb_flag) \
  { \
    CHECK_TYPE_ANY((check_id), ID *, void *); \
    BKE_lib_query_foreachid_process(&data, (ID **)&(check_id), (cb_flag)); \
    if (BKE_lib_query_foreachid_iter_stop(&data)) { \
      library_foreach_ID_data_cleanup(&data); \
      return false; \
    } \
  } \
  ((void)0)

#define CALLBACK_INVOKE(check_id_super, cb_flag) \
  { \
    CHECK_TYPE(&((check_id_super)->id), ID *); \
    BKE_lib_query_foreachid_process(&data, (ID **)&(check_id_super), (cb_flag)); \
    if (BKE_lib_query_foreachid_iter_stop(&data)) { \
      library_foreach_ID_data_cleanup(&data); \
      return false; \
    } \
  } \
  ((void)0)

  for (; id != NULL; id = (flag & IDWALK_RECURSE) ? BLI_LINKSTACK_POP(data.ids_todo) : NULL) {
    data.self_id = id;
    /* Note that we may call this functions sometime directly on an embedded ID, without any
     * knowledge of the owner ID then.
     * While not great, and that should be probably sanitized at some point, we cal live with it
     * for now. */
    data.owner_id = ((id->flag & LIB_EMBEDDED_DATA) != 0 && id_owner != NULL) ? id_owner :
                                                                                data.self_id;

    /* inherit_data is non-NULL when this function is called for some sub-data ID
     * (like root node-tree of a material).
     * In that case, we do not want to generate those 'generic flags' from our current sub-data ID
     * (the node tree), but re-use those generated for the 'owner' ID (the material). */
    if (inherit_data == NULL) {
      data.cb_flag = ID_IS_LINKED(id) ? IDWALK_CB_INDIRECT_USAGE : 0;
      /* When an ID is defined as not refcounting its ID usages, it should never do it. */
      data.cb_flag_clear = (id->tag & LIB_TAG_NO_USER_REFCOUNT) ?
                               IDWALK_CB_USER | IDWALK_CB_USER_ONE :
                               0;
    }
    else {
      data.cb_flag = inherit_data->cb_flag;
      data.cb_flag_clear = inherit_data->cb_flag_clear;
    }

    if (bmain != NULL && bmain->relations != NULL && (flag & IDWALK_READONLY) &&
        (flag & IDWALK_DO_INTERNAL_RUNTIME_POINTERS) == 0 &&
        (((bmain->relations->flag & MAINIDRELATIONS_INCLUDE_UI) == 0) ==
         ((data.flag & IDWALK_INCLUDE_UI) == 0))) {
      /* Note that this is minor optimization, even in worst cases (like id being an object with
       * lots of drivers and constraints and modifiers, or material etc. with huge node tree),
       * but we might as well use it (Main->relations is always assumed valid,
       * it's responsibility of code creating it to free it,
       * especially if/when it starts modifying Main database). */
      MainIDRelationsEntry *entry = BLI_ghash_lookup(bmain->relations->relations_from_pointers,
                                                     id);
      for (MainIDRelationsEntryItem *to_id_entry = entry->to_ids; to_id_entry != NULL;
           to_id_entry = to_id_entry->next) {
        BKE_lib_query_foreachid_process(
            &data, to_id_entry->id_pointer.to, to_id_entry->usage_flag);
        if (BKE_lib_query_foreachid_iter_stop(&data)) {
          library_foreach_ID_data_cleanup(&data);
          return false;
        }
      }
      continue;
    }

    /* NOTE: ID.lib pointer is purposefully fully ignored here...
     * We may want to add it at some point? */

    if (flag & IDWALK_DO_INTERNAL_RUNTIME_POINTERS) {
      CALLBACK_INVOKE_ID(id->newid, IDWALK_CB_INTERNAL);
      CALLBACK_INVOKE_ID(id->orig_id, IDWALK_CB_INTERNAL);
    }

    if (id->override_library != NULL) {
      CALLBACK_INVOKE_ID(id->override_library->reference,
                         IDWALK_CB_USER | IDWALK_CB_OVERRIDE_LIBRARY_REFERENCE);
      CALLBACK_INVOKE_ID(id->override_library->storage,
                         IDWALK_CB_USER | IDWALK_CB_OVERRIDE_LIBRARY_REFERENCE);

      CALLBACK_INVOKE_ID(id->override_library->hierarchy_root, IDWALK_CB_LOOPBACK);
    }

    IDP_foreach_property(id->properties,
                         IDP_TYPE_FILTER_ID,
                         BKE_lib_query_idpropertiesForeachIDLink_callback,
                         &data);
    if (BKE_lib_query_foreachid_iter_stop(&data)) {
      library_foreach_ID_data_cleanup(&data);
      return false;
    }

    AnimData *adt = BKE_animdata_from_id(id);
    if (adt) {
      BKE_animdata_foreach_id(adt, &data);
      if (BKE_lib_query_foreachid_iter_stop(&data)) {
        library_foreach_ID_data_cleanup(&data);
        return false;
      }
    }

    const IDTypeInfo *id_type = BKE_idtype_get_info_from_id(id);
    if (id_type->foreach_id != NULL) {
      id_type->foreach_id(id, &data);

      if (BKE_lib_query_foreachid_iter_stop(&data)) {
        library_foreach_ID_data_cleanup(&data);
        return false;
      }
    }
  }

  library_foreach_ID_data_cleanup(&data);
  return true;

#undef CALLBACK_INVOKE_ID
#undef CALLBACK_INVOKE
}

void BKE_library_foreach_ID_link(
    Main *bmain, ID *id, LibraryIDLinkCallback callback, void *user_data, int flag)
{
  library_foreach_ID_link(bmain, NULL, id, callback, user_data, flag, NULL);
}

void BKE_library_update_ID_link_user(ID *id_dst, ID *id_src, const int cb_flag)
{
  if (cb_flag & IDWALK_CB_USER) {
    id_us_min(id_src);
    id_us_plus(id_dst);
  }
  else if (cb_flag & IDWALK_CB_USER_ONE) {
    id_us_ensure_real(id_dst);
  }
}

uint64_t BKE_library_id_can_use_filter_id(const ID *id_owner)
{
  /* any type of ID can be used in custom props. */
  if (id_owner->properties) {
    return FILTER_ID_ALL;
  }
  const short id_type_owner = GS(id_owner->name);

  /* IDProps of armature bones and nodes, and bNode->id can use virtually any type of ID. */
  if (ELEM(id_type_owner, ID_NT, ID_AR)) {
    return FILTER_ID_ALL;
  }

  /* Casting to non const.
   * TODO(jbakker): We should introduce a ntree_id_has_tree function as we are actually not
   * interested in the result. */
  if (ntreeFromID((ID *)id_owner)) {
    return FILTER_ID_ALL;
  }

  if (BKE_animdata_from_id(id_owner)) {
    /* AnimationData can use virtually any kind of data-blocks, through drivers especially. */
    return FILTER_ID_ALL;
  }

  switch ((ID_Type)id_type_owner) {
    case ID_LI:
      return FILTER_ID_LI;
    case ID_SCE:
      return FILTER_ID_OB | FILTER_ID_WO | FILTER_ID_SCE | FILTER_ID_MC | FILTER_ID_MA |
             FILTER_ID_GR | FILTER_ID_TXT | FILTER_ID_LS | FILTER_ID_MSK | FILTER_ID_SO |
             FILTER_ID_GD | FILTER_ID_BR | FILTER_ID_PAL | FILTER_ID_IM | FILTER_ID_NT;
    case ID_OB:
      /* Could be more specific, but simpler to just always say 'yes' here. */
      return FILTER_ID_ALL;
    case ID_ME:
      return FILTER_ID_ME | FILTER_ID_MA | FILTER_ID_IM;
    case ID_CU_LEGACY:
      return FILTER_ID_OB | FILTER_ID_MA | FILTER_ID_VF;
    case ID_MB:
      return FILTER_ID_MA;
    case ID_MA:
      return FILTER_ID_TE | FILTER_ID_GR;
    case ID_TE:
      return FILTER_ID_IM | FILTER_ID_OB;
    case ID_LT:
      return 0;
    case ID_LA:
      return FILTER_ID_TE;
    case ID_CA:
      return FILTER_ID_OB | FILTER_ID_IM;
    case ID_KE:
      /* Warning! key->from, could be more types in future? */
      return FILTER_ID_ME | FILTER_ID_CU_LEGACY | FILTER_ID_LT;
    case ID_SCR:
      return FILTER_ID_SCE;
    case ID_WO:
      return FILTER_ID_TE;
    case ID_SPK:
      return FILTER_ID_SO;
    case ID_GR:
      return FILTER_ID_OB | FILTER_ID_GR;
    case ID_NT:
      /* Could be more specific, but node.id has no type restriction... */
      return FILTER_ID_ALL;
    case ID_BR:
      return FILTER_ID_BR | FILTER_ID_IM | FILTER_ID_PC | FILTER_ID_TE | FILTER_ID_MA;
    case ID_PA:
      return FILTER_ID_OB | FILTER_ID_GR | FILTER_ID_TE;
    case ID_MC:
      return FILTER_ID_GD | FILTER_ID_IM;
    case ID_MSK:
      /* WARNING! mask->parent.id, not typed. */
      return FILTER_ID_MC;
    case ID_LS:
      return FILTER_ID_TE | FILTER_ID_OB;
    case ID_LP:
      return FILTER_ID_IM;
    case ID_GD:
      return FILTER_ID_MA;
    case ID_WS:
      return FILTER_ID_SCE;
    case ID_CV:
      return FILTER_ID_MA | FILTER_ID_OB;
    case ID_PT:
      return FILTER_ID_MA;
    case ID_VO:
      return FILTER_ID_MA;
    case ID_SIM:
      return FILTER_ID_OB | FILTER_ID_IM;
    case ID_WM:
      return FILTER_ID_SCE | FILTER_ID_WS;
    case ID_IM:
    case ID_VF:
    case ID_TXT:
    case ID_SO:
    case ID_AR:
    case ID_AC:
    case ID_PAL:
    case ID_PC:
    case ID_CF:
      /* Those types never use/reference other IDs... */
      return 0;
    case ID_IP:
      /* Deprecated... */
      return 0;
  }

  BLI_assert_unreachable();
  return 0;
}

bool BKE_library_id_can_use_idtype(ID *id_owner, const short id_type_used)
{
  /* any type of ID can be used in custom props. */
  if (id_owner->properties) {
    return true;
  }

  const short id_type_owner = GS(id_owner->name);
  /* Exception for ID_LI as they don't exist as a filter. */
  if (id_type_used == ID_LI) {
    return id_type_owner == ID_LI;
  }

  /* Exception: ID_KE aren't available as filter_id. */
  if (id_type_used == ID_KE) {
    return ELEM(id_type_owner, ID_ME, ID_CU_LEGACY, ID_LT);
  }

  /* Exception: ID_SCR aren't available as filter_id. */
  if (id_type_used == ID_SCR) {
    return ELEM(id_type_owner, ID_WS);
  }

  const uint64_t filter_id_type_used = BKE_idtype_idcode_to_idfilter(id_type_used);
  const uint64_t can_be_used = BKE_library_id_can_use_filter_id(id_owner);
  return (can_be_used & filter_id_type_used) != 0;
}

/* ***** ID users iterator. ***** */
typedef struct IDUsersIter {
  ID *id;

  ListBase *lb_array[INDEX_ID_MAX];
  int lb_idx;

  ID *curr_id;
  int count_direct, count_indirect; /* Set by callback. */
} IDUsersIter;

static int foreach_libblock_id_users_callback(LibraryIDLinkCallbackData *cb_data)
{
  ID **id_p = cb_data->id_pointer;
  const int cb_flag = cb_data->cb_flag;
  IDUsersIter *iter = cb_data->user_data;

  if (*id_p) {
    /* 'Loopback' ID pointers (the ugly 'from' ones, like Key->from).
     * Those are not actually ID usage, we can ignore them here.
     */
    if (cb_flag & IDWALK_CB_LOOPBACK) {
      return IDWALK_RET_NOP;
    }

    if (*id_p == iter->id) {
#if 0
      printf(
          "%s uses %s (refcounted: %d, userone: %d, used_one: %d, used_one_active: %d, "
          "indirect_usage: %d)\n",
          iter->curr_id->name,
          iter->id->name,
          (cb_flag & IDWALK_USER) ? 1 : 0,
          (cb_flag & IDWALK_USER_ONE) ? 1 : 0,
          (iter->id->tag & LIB_TAG_EXTRAUSER) ? 1 : 0,
          (iter->id->tag & LIB_TAG_EXTRAUSER_SET) ? 1 : 0,
          (cb_flag & IDWALK_INDIRECT_USAGE) ? 1 : 0);
#endif
      if (cb_flag & IDWALK_CB_INDIRECT_USAGE) {
        iter->count_indirect++;
      }
      else {
        iter->count_direct++;
      }
    }
  }

  return IDWALK_RET_NOP;
}

int BKE_library_ID_use_ID(ID *id_user, ID *id_used)
{
  IDUsersIter iter;

  /* We do not care about iter.lb_array/lb_idx here... */
  iter.id = id_used;
  iter.curr_id = id_user;
  iter.count_direct = iter.count_indirect = 0;

  BKE_library_foreach_ID_link(
      NULL, iter.curr_id, foreach_libblock_id_users_callback, (void *)&iter, IDWALK_READONLY);

  return iter.count_direct + iter.count_indirect;
}

static bool library_ID_is_used(Main *bmain, void *idv, const bool check_linked)
{
  IDUsersIter iter;
  ListBase *lb_array[INDEX_ID_MAX];
  ID *id = idv;
  int i = set_listbasepointers(bmain, lb_array);
  bool is_defined = false;

  iter.id = id;
  iter.count_direct = iter.count_indirect = 0;
  while (i-- && !is_defined) {
    ID *id_curr = lb_array[i]->first;

    if (!id_curr || !BKE_library_id_can_use_idtype(id_curr, GS(id->name))) {
      continue;
    }

    for (; id_curr && !is_defined; id_curr = id_curr->next) {
      if (id_curr == id) {
        /* We are not interested in self-usages (mostly from drivers or bone constraints...). */
        continue;
      }
      iter.curr_id = id_curr;
      BKE_library_foreach_ID_link(
          bmain, id_curr, foreach_libblock_id_users_callback, &iter, IDWALK_READONLY);

      is_defined = ((check_linked ? iter.count_indirect : iter.count_direct) != 0);
    }
  }

  return is_defined;
}

bool BKE_library_ID_is_locally_used(Main *bmain, void *idv)
{
  return library_ID_is_used(bmain, idv, false);
}

bool BKE_library_ID_is_indirectly_used(Main *bmain, void *idv)
{
  return library_ID_is_used(bmain, idv, true);
}

void BKE_library_ID_test_usages(Main *bmain, void *idv, bool *is_used_local, bool *is_used_linked)
{
  IDUsersIter iter;
  ListBase *lb_array[INDEX_ID_MAX];
  ID *id = idv;
  int i = set_listbasepointers(bmain, lb_array);
  bool is_defined = false;

  iter.id = id;
  iter.count_direct = iter.count_indirect = 0;
  while (i-- && !is_defined) {
    ID *id_curr = lb_array[i]->first;

    if (!id_curr || !BKE_library_id_can_use_idtype(id_curr, GS(id->name))) {
      continue;
    }

    for (; id_curr && !is_defined; id_curr = id_curr->next) {
      if (id_curr == id) {
        /* We are not interested in self-usages (mostly from drivers or bone constraints...). */
        continue;
      }
      iter.curr_id = id_curr;
      BKE_library_foreach_ID_link(
          bmain, id_curr, foreach_libblock_id_users_callback, &iter, IDWALK_READONLY);

      is_defined = (iter.count_direct != 0 && iter.count_indirect != 0);
    }
  }

  *is_used_local = (iter.count_direct != 0);
  *is_used_linked = (iter.count_indirect != 0);
}

/* ***** IDs usages.checking/tagging. ***** */
static void lib_query_unused_ids_tag_recurse(Main *bmain,
                                             const int tag,
                                             const bool do_local_ids,
                                             const bool do_linked_ids,
                                             ID *id,
                                             int *r_num_tagged)
{
  /* We should never deal with embedded, not-in-main IDs here. */
  BLI_assert((id->flag & LIB_EMBEDDED_DATA) == 0);

  if ((!do_linked_ids && ID_IS_LINKED(id)) || (!do_local_ids && !ID_IS_LINKED(id))) {
    return;
  }

  MainIDRelationsEntry *id_relations = BLI_ghash_lookup(bmain->relations->relations_from_pointers,
                                                        id);
  if ((id_relations->tags & MAINIDRELATIONS_ENTRY_TAGS_PROCESSED) != 0) {
    return;
  }
  id_relations->tags |= MAINIDRELATIONS_ENTRY_TAGS_PROCESSED;

  if ((id->tag & tag) != 0) {
    return;
  }

  if ((id->flag & LIB_FAKEUSER) != 0) {
    /* This ID is forcefully kept around, and therefore never unused, no need to check it further.
     */
    return;
  }

  if (ELEM(GS(id->name), ID_WM, ID_WS, ID_SCE, ID_SCR, ID_LI)) {
    /* Some 'root' ID types are never unused (even though they may not have actual users), unless
     * their actual user-count is set to 0. */
    return;
  }

  /* An ID user is 'valid' (i.e. may affect the 'used'/'not used' status of the ID it uses) if it
   * does not match `ignored_usages`, and does match `required_usages`. */
  const int ignored_usages = (IDWALK_CB_LOOPBACK | IDWALK_CB_EMBEDDED);
  const int required_usages = (IDWALK_CB_USER | IDWALK_CB_USER_ONE);

  /* This ID may be tagged as unused if none of its users are 'valid', as defined above.
   *
   * First recursively check all its valid users, if all of them can be tagged as
   * unused, then we can tag this ID as such too. */
  bool has_valid_from_users = false;
  /* Preemptively consider this ID as unused. That way if there is a loop of dependency leading
   * back to it, it won't create a fake 'valid user' detection.
   * NOTE: This can only be done for a subset of IDs, some types are never 'indirectly unused',
   * same for IDs with a fake user. */
  if ((id->flag & LIB_FAKEUSER) == 0 && !ELEM(GS(id->name), ID_SCE, ID_WM, ID_SCR, ID_WS, ID_LI)) {
    id->tag |= tag;
  }
  for (MainIDRelationsEntryItem *id_from_item = id_relations->from_ids; id_from_item != NULL;
       id_from_item = id_from_item->next) {
    if ((id_from_item->usage_flag & ignored_usages) != 0 ||
        (id_from_item->usage_flag & required_usages) == 0) {
      continue;
    }

    ID *id_from = id_from_item->id_pointer.from;
    if ((id_from->flag & LIB_EMBEDDED_DATA) != 0) {
      /* Directly 'by-pass' to actual real ID owner. */
      const IDTypeInfo *type_info_from = BKE_idtype_get_info_from_id(id_from);
      BLI_assert(type_info_from->owner_get != NULL);
      id_from = type_info_from->owner_get(bmain, id_from, NULL);
    }

    lib_query_unused_ids_tag_recurse(
        bmain, tag, do_local_ids, do_linked_ids, id_from, r_num_tagged);
    if ((id_from->tag & tag) == 0) {
      has_valid_from_users = true;
      break;
    }
  }
  if (has_valid_from_users) {
    /* This ID has 'valid' users, clear the 'tag as unused' preemptively set above. */
    id->tag &= ~tag;
  }
  else {
    /* This ID has no 'valid' users, tag it as unused. */
    id->tag |= tag;
    if (r_num_tagged != NULL) {
      r_num_tagged[INDEX_ID_NULL]++;
      r_num_tagged[BKE_idtype_idcode_to_index(GS(id->name))]++;
    }
  }
}

void BKE_lib_query_unused_ids_tag(Main *bmain,
                                  const int tag,
                                  const bool do_local_ids,
                                  const bool do_linked_ids,
                                  const bool do_tag_recursive,
                                  int *r_num_tagged)
{
  /* First loop, to only check for immediately unused IDs (those with 0 user count).
   * NOTE: It also takes care of clearing given tag for used IDs. */
  ID *id;
  FOREACH_MAIN_ID_BEGIN (bmain, id) {
    if ((!do_linked_ids && ID_IS_LINKED(id)) || (!do_local_ids && !ID_IS_LINKED(id))) {
      id->tag &= ~tag;
    }
    else if (id->us == 0) {
      id->tag |= tag;
      if (r_num_tagged != NULL) {
        r_num_tagged[INDEX_ID_NULL]++;
        r_num_tagged[BKE_idtype_idcode_to_index(GS(id->name))]++;
      }
    }
    else {
      id->tag &= ~tag;
    }
  }
  FOREACH_MAIN_ID_END;

  if (!do_tag_recursive) {
    return;
  }

  BKE_main_relations_create(bmain, 0);
  FOREACH_MAIN_ID_BEGIN (bmain, id) {
    lib_query_unused_ids_tag_recurse(bmain, tag, do_local_ids, do_linked_ids, id, r_num_tagged);
  }
  FOREACH_MAIN_ID_END;
  BKE_main_relations_free(bmain);
}

static int foreach_libblock_used_linked_data_tag_clear_cb(LibraryIDLinkCallbackData *cb_data)
{
  ID *self_id = cb_data->id_self;
  ID **id_p = cb_data->id_pointer;
  const int cb_flag = cb_data->cb_flag;
  bool *is_changed = cb_data->user_data;

  if (*id_p) {
    /* The infamous 'from' pointers (Key.from, ...).
     * those are not actually ID usage, so we ignore them here. */
    if (cb_flag & IDWALK_CB_LOOPBACK) {
      return IDWALK_RET_NOP;
    }

    /* If checked id is used by an assumed used ID,
     * then it is also used and not part of any linked archipelago. */
    if (!(self_id->tag & LIB_TAG_DOIT) && ((*id_p)->tag & LIB_TAG_DOIT)) {
      (*id_p)->tag &= ~LIB_TAG_DOIT;
      *is_changed = true;
    }
  }

  return IDWALK_RET_NOP;
}

void BKE_library_unused_linked_data_set_tag(Main *bmain, const bool do_init_tag)
{
  ID *id;

  if (do_init_tag) {
    FOREACH_MAIN_ID_BEGIN (bmain, id) {
      if (id->lib && (id->tag & LIB_TAG_INDIRECT) != 0) {
        id->tag |= LIB_TAG_DOIT;
      }
      else {
        id->tag &= ~LIB_TAG_DOIT;
      }
    }
    FOREACH_MAIN_ID_END;
  }

  for (bool do_loop = true; do_loop;) {
    do_loop = false;
    FOREACH_MAIN_ID_BEGIN (bmain, id) {
      /* We only want to check that ID if it is currently known as used... */
      if ((id->tag & LIB_TAG_DOIT) == 0) {
        BKE_library_foreach_ID_link(
            bmain, id, foreach_libblock_used_linked_data_tag_clear_cb, &do_loop, IDWALK_READONLY);
      }
    }
    FOREACH_MAIN_ID_END;
  }
}

void BKE_library_indirectly_used_data_tag_clear(Main *bmain)
{
  ListBase *lb_array[INDEX_ID_MAX];

  bool do_loop = true;
  while (do_loop) {
    int i = set_listbasepointers(bmain, lb_array);
    do_loop = false;

    while (i--) {
      LISTBASE_FOREACH (ID *, id, lb_array[i]) {
        if (!ID_IS_LINKED(id) || id->tag & LIB_TAG_DOIT) {
          /* Local or non-indirectly-used ID (so far), no need to check it further. */
          continue;
        }
        BKE_library_foreach_ID_link(
            bmain, id, foreach_libblock_used_linked_data_tag_clear_cb, &do_loop, IDWALK_READONLY);
      }
    }
  }
}
