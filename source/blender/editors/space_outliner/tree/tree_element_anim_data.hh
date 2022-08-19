/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 */

#pragma once

#include "tree_element.hh"

struct TreeElement;

namespace blender::ed::outliner {

class TreeElementAnimData final : public AbstractTreeElement {
  AnimData &anim_data_;

 public:
  TreeElementAnimData(TreeElement &legacy_te, AnimData &anim_data);

  void expand(SpaceOutliner &space_outliner) const override;

 private:
  void expand_drivers(SpaceOutliner &space_outliner) const;
  void expand_NLA_tracks(SpaceOutliner &space_outliner) const;
};

}  // namespace blender::ed::outliner
