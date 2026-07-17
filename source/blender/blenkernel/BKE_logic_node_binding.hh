/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file BKE_logic_node_binding.hh
 *  \ingroup bke
 */

#pragma once

#include "DNA_listBase.h"

namespace blender {

struct LogicNodeBinding;
struct Object;

LogicNodeBinding *BKE_logic_node_binding_add(Object *ob);
void BKE_logic_node_binding_remove(Object *ob, LogicNodeBinding *binding);
LogicNodeBinding *BKE_logic_node_binding_find_by_tree_name(Object *ob, const char *tree_name);

void BKE_logic_node_binding_copy_list(ListBase *dst, const ListBase *src);
void BKE_logic_node_binding_free_list(ListBase *list);

}  // namespace blender
