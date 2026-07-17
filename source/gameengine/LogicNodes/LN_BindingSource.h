/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_BindingSource.h
 *  \ingroup logicnodes
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace blender {
struct Object;
}  // namespace blender

class KX_GameObject;

struct LN_AppliedTreeDesc {
  KX_GameObject *game_object = nullptr;
  std::string tree_name;
  uint32_t scene_object_index = 0;
  uint32_t applied_tree_index = 0;
  bool enabled = true;
  bool runtime_active = true;
};

struct LN_GameObjectBindingCandidate {
  blender::Object *blender_object = nullptr;
  KX_GameObject *game_object = nullptr;
  uint32_t scene_object_index = 0;
  bool runtime_active = true;
};

class LN_BindingSource {
 public:
  virtual ~LN_BindingSource() = default;

  virtual void CollectAppliedTrees(std::vector<LN_AppliedTreeDesc> &r_applied_trees) const = 0;
};

class LN_GamePropertyBindingSource : public LN_BindingSource {
 public:
  explicit LN_GamePropertyBindingSource(
      std::vector<LN_GameObjectBindingCandidate> candidates);

  void CollectAppliedTrees(std::vector<LN_AppliedTreeDesc> &r_applied_trees) const override;

 private:
  std::vector<LN_GameObjectBindingCandidate> m_candidates;
};
