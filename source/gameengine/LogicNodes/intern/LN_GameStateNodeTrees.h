/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_GameStateNodeTrees.h
 *  \ingroup logicnodes
 *
 * Bounded runtime state for authored Geometry Nodes and compositor node trees.
 */

#pragma once

#include <cstddef>
#include <memory>
#include <string>

#include "LN_GameState.h"
#include "LN_GameStateInternal.h"

class KX_Scene;

namespace blender {
struct Main;
struct Scene;
struct bNodeTree;
}  // namespace blender

namespace upbge::logic_nodes::game_state {

class GeometryNodesAssetCatalog;

class RuntimeNodeTreeCapture {
 public:
  explicit RuntimeNodeTreeCapture(const GeometryNodesAssetCatalog &catalog);
  ~RuntimeNodeTreeCapture();
  RuntimeNodeTreeCapture(RuntimeNodeTreeCapture &&) noexcept;
  RuntimeNodeTreeCapture &operator=(RuntimeNodeTreeCapture &&) noexcept;

  RuntimeNodeTreeCapture(const RuntimeNodeTreeCapture &) = delete;
  RuntimeNodeTreeCapture &operator=(const RuntimeNodeTreeCapture &) = delete;

  LN_GameStateResult CaptureGeometryTree(blender::bNodeTree &tree,
                                         size_t &r_budget,
                                         std::string &r_error);
  LN_GameStateResult CaptureCompositor(blender::Scene &scene,
                                       Checkpoint &r_checkpoint,
                                       size_t &r_budget,
                                       std::string &r_error);
  LN_GameStateResult Finalize(Checkpoint &r_checkpoint, std::string &r_error);

 private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

class PreparedRuntimeNodeTreeRestore {
 public:
  PreparedRuntimeNodeTreeRestore();
  ~PreparedRuntimeNodeTreeRestore();
  PreparedRuntimeNodeTreeRestore(PreparedRuntimeNodeTreeRestore &&) noexcept;
  PreparedRuntimeNodeTreeRestore &operator=(PreparedRuntimeNodeTreeRestore &&) noexcept;

  PreparedRuntimeNodeTreeRestore(const PreparedRuntimeNodeTreeRestore &) = delete;
  PreparedRuntimeNodeTreeRestore &operator=(const PreparedRuntimeNodeTreeRestore &) = delete;

  void ReserveCommitUpdates(KX_Scene &scene) const;
  void Commit(KX_Scene &scene);
  void Rollback() noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;

  friend LN_GameStateResult prepare_runtime_node_tree_restore(
      blender::Main &,
      blender::Scene &,
      const GeometryNodesAssetCatalog &,
      const Checkpoint &,
      PreparedRuntimeNodeTreeRestore &,
      std::string &);
};

LN_GameStateResult prepare_runtime_node_tree_restore(
    blender::Main &bmain,
    blender::Scene &scene,
    const GeometryNodesAssetCatalog &catalog,
    const Checkpoint &checkpoint,
    PreparedRuntimeNodeTreeRestore &r_prepared,
    std::string &r_error);

}  // namespace upbge::logic_nodes::game_state
