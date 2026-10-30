/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_GameStateGeometryNodes.h
 *  \ingroup logicnodes
 *
 * Bounded Geometry Nodes modifier persistence. Graph topology remains authored while mutable
 * node values are handled by the shared runtime node-tree provider.
 */

#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "LN_GameState.h"
#include "LN_GameStateInternal.h"

class KX_GameObject;
class KX_Scene;

namespace blender {
struct Main;
struct Object;
struct bNodeTree;
}  // namespace blender

namespace upbge::logic_nodes::game_state {

class RuntimeNodeTreeCapture;

struct ModifierBaseline {
  std::vector<NonGeometryModifierState> non_geometry_modifiers;
  std::vector<GeometryNodesModifierState> geometry_nodes_modifiers;
};

class GeometryNodesAssetCatalog {
 public:
  GeometryNodesAssetCatalog();
  ~GeometryNodesAssetCatalog();
  GeometryNodesAssetCatalog(GeometryNodesAssetCatalog &&) noexcept;
  GeometryNodesAssetCatalog &operator=(GeometryNodesAssetCatalog &&) noexcept;

  GeometryNodesAssetCatalog(const GeometryNodesAssetCatalog &) = delete;
  GeometryNodesAssetCatalog &operator=(const GeometryNodesAssetCatalog &) = delete;

  bool DescribeNodeGroup(const blender::bNodeTree &tree,
                         std::string &r_name,
                         std::string &r_library_path,
                         std::string &r_fingerprint,
                         bool &r_has_simulation) const;
  bool DescribeNodeTree(const blender::bNodeTree &tree,
                        RuntimeNodeTreeType expected_type,
                        std::string &r_name,
                        std::string &r_library_path,
                        std::string &r_fingerprint) const;
  blender::bNodeTree *ResolveNodeGroup(
      blender::Main &bmain, const GeometryNodesModifierState &saved) const;
  blender::bNodeTree *ResolveNodeTree(
      blender::Main &bmain, const RuntimeNodeTreeState &saved) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;

  friend LN_GameStateResult build_geometry_nodes_asset_catalog(
      blender::Main &, GeometryNodesAssetCatalog &, std::string &);
  friend LN_GameStateResult capture_modifier_state(
      KX_GameObject &,
      const ModifierBaseline &,
      const GeometryNodesAssetCatalog &,
      RuntimeNodeTreeCapture &,
      ObjectState &,
      size_t &,
      std::string &);
  friend LN_GameStateResult prepare_modifier_restore(
      blender::Main &,
      const GeometryNodesAssetCatalog &,
      const std::vector<struct ModifierRestoreTarget> &,
      class PreparedModifierRestore &,
      std::string &);
};

LN_GameStateResult build_geometry_nodes_asset_catalog(
    blender::Main &bmain,
    GeometryNodesAssetCatalog &r_catalog,
    std::string &r_error);

LN_GameStateResult capture_modifier_baseline(const blender::Object &object,
                                             const GeometryNodesAssetCatalog &catalog,
                                             ModifierBaseline &r_baseline,
                                             std::string &r_error);

LN_GameStateResult capture_modifier_state(
    KX_GameObject &object,
    const ModifierBaseline &baseline,
    const GeometryNodesAssetCatalog &catalog,
    RuntimeNodeTreeCapture &node_tree_capture,
    ObjectState &r_state,
    size_t &r_budget,
    std::string &r_error);

struct ModifierRestoreTarget {
  KX_GameObject *object = nullptr;
  const ObjectState *saved = nullptr;
  const ModifierBaseline *baseline = nullptr;
};

class PreparedModifierRestore {
 public:
  PreparedModifierRestore();
  ~PreparedModifierRestore();
  PreparedModifierRestore(PreparedModifierRestore &&) noexcept;
  PreparedModifierRestore &operator=(PreparedModifierRestore &&) noexcept;

  PreparedModifierRestore(const PreparedModifierRestore &) = delete;
  PreparedModifierRestore &operator=(const PreparedModifierRestore &) = delete;

  void ReserveCommitUpdates(KX_Scene &scene) const;
  void Commit(KX_Scene &scene);
  void Rollback();

 private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;

  friend LN_GameStateResult prepare_modifier_restore(
      blender::Main &,
      const GeometryNodesAssetCatalog &,
      const std::vector<ModifierRestoreTarget> &,
      PreparedModifierRestore &,
      std::string &);
};

LN_GameStateResult prepare_modifier_restore(
    blender::Main &bmain,
    const GeometryNodesAssetCatalog &catalog,
    const std::vector<ModifierRestoreTarget> &targets,
    PreparedModifierRestore &r_prepared,
    std::string &r_error);

}  // namespace upbge::logic_nodes::game_state
