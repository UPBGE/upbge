/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_BindingSource.cpp
 *  \ingroup logicnodes
 */

#include "LN_BindingSource.h"

#include <utility>

#include "DNA_logic_node_binding_types.h"
#include "DNA_object_types.h"

LN_GamePropertyBindingSource::LN_GamePropertyBindingSource(
    std::vector<LN_GameObjectBindingCandidate> candidates)
    : m_candidates(std::move(candidates))
{
}

void LN_GamePropertyBindingSource::CollectAppliedTrees(
    std::vector<LN_AppliedTreeDesc> &r_applied_trees) const
{
  for (const LN_GameObjectBindingCandidate &candidate : m_candidates) {
    if (candidate.blender_object == nullptr) {
      continue;
    }

    uint32_t applied_tree_index = 0;

    for (const blender::LogicNodeBinding *binding = static_cast<const blender::LogicNodeBinding *>(
             candidate.blender_object->logic_node_bindings.first);
         binding;
         binding = binding->next)
    {
      if (binding->tree_name[0] == '\0') {
        continue;
      }

      LN_AppliedTreeDesc desc;
      desc.game_object = candidate.game_object;
      desc.tree_name = binding->tree_name;
      desc.scene_object_index = candidate.scene_object_index;
      desc.applied_tree_index = applied_tree_index++;
      desc.enabled = binding->enabled != 0;
      desc.runtime_active = candidate.runtime_active;
      r_applied_trees.push_back(desc);
    }
  }
}
