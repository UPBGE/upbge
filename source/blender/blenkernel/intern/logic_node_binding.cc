/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file logic_node_binding.cc
 *  \ingroup bke
 */

#include "BKE_logic_node_binding.hh"

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_string.h"

#include "BKE_lib_id.hh"

#include "DNA_logic_node_binding_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"

namespace blender {

static LogicNodeBinding *logic_node_binding_copy(const LogicNodeBinding *src)
{
  LogicNodeBinding *dst = MEM_new_zeroed<LogicNodeBinding>(__func__);
  if (src != nullptr) {
    *dst = *src;
    dst->next = dst->prev = nullptr;
  }
  return dst;
}

LogicNodeBinding *BKE_logic_node_binding_add(Object *ob)
{
  if (ob == nullptr) {
    return nullptr;
  }

  LogicNodeBinding *binding = MEM_new_zeroed<LogicNodeBinding>(__func__);
  binding->enabled = 1;
  BLI_addtail(&ob->logic_node_bindings, binding);
  return binding;
}

void BKE_logic_node_binding_remove(Object *ob, LogicNodeBinding *binding)
{
  if (ob == nullptr || binding == nullptr) {
    return;
  }

  BKE_logic_node_binding_set_tree(binding, nullptr);
  BLI_remlink(&ob->logic_node_bindings, binding);
  MEM_delete(binding);
}

void BKE_logic_node_binding_set_tree(LogicNodeBinding *binding, bNodeTree *tree)
{
  if (binding == nullptr) {
    return;
  }

  BLI_assert(tree == nullptr || tree->type == NTREE_LOGIC);
  if (tree != nullptr && tree->type != NTREE_LOGIC) {
    return;
  }

  if (binding->tree != tree) {
    if (binding->tree != nullptr) {
      id_us_min(&binding->tree->id);
    }
    if (tree != nullptr) {
      id_us_plus(&tree->id);
    }
    binding->tree = tree;
  }

  binding->flag &= ~LOGIC_NODE_BINDING_LEGACY_NAME_FALLBACK;
  if (tree != nullptr) {
    STRNCPY(binding->tree_name, tree->id.name + 2);
  }
  else {
    binding->tree_name[0] = '\0';
  }
}

const char *BKE_logic_node_binding_tree_name(const LogicNodeBinding *binding)
{
  if (binding == nullptr) {
    return "";
  }
  if (binding->tree != nullptr) {
    return binding->tree->id.name + 2;
  }
  if (binding->flag & LOGIC_NODE_BINDING_LEGACY_NAME_FALLBACK) {
    return binding->tree_name;
  }
  return "";
}

LogicNodeBinding *BKE_logic_node_binding_find_by_tree(Object *ob, const bNodeTree *tree)
{
  if (ob == nullptr || tree == nullptr) {
    return nullptr;
  }

  for (LogicNodeBinding *binding = static_cast<LogicNodeBinding *>(ob->logic_node_bindings.first);
       binding;
       binding = binding->next)
  {
    if (binding->tree == tree) {
      return binding;
    }
  }
  return nullptr;
}

LogicNodeBinding *BKE_logic_node_binding_find_by_tree_name(Object *ob, const char *tree_name)
{
  if (ob == nullptr || tree_name == nullptr || tree_name[0] == '\0') {
    return nullptr;
  }

  for (LogicNodeBinding *binding = static_cast<LogicNodeBinding *>(ob->logic_node_bindings.first);
       binding;
       binding = binding->next)
  {
    if (STREQ(BKE_logic_node_binding_tree_name(binding), tree_name)) {
      return binding;
    }
  }
  return nullptr;
}

void BKE_logic_node_binding_copy_list(ListBase *dst, const ListBase *src)
{
  /* Do not free existing dst entries: during ID duplication dst may contain shallow-copied
   * pointers from src (see object_copy_data). Caller must BLI_listbase_clear(dst) first. */
  dst->first = dst->last = nullptr;

  for (const LogicNodeBinding *binding = static_cast<const LogicNodeBinding *>(src->first);
       binding;
       binding = binding->next)
  {
    BLI_addtail(dst, logic_node_binding_copy(binding));
  }
}

void BKE_logic_node_binding_free_list(ListBase *list, const bool do_id_user)
{
  LogicNodeBinding *binding;
  while ((binding = static_cast<LogicNodeBinding *>(BLI_pophead(list)))) {
    if (do_id_user) {
      BKE_logic_node_binding_set_tree(binding, nullptr);
    }
    MEM_delete(binding);
  }
}

}  // namespace blender
