/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_GameStateMaterial.h
 *  \ingroup logicnodes
 *
 * Automatic object-local and shared material-state persistence.
 */

#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "LN_GameState.h"

class KX_GameObject;
class KX_Scene;

namespace blender {
struct Main;
struct Material;
struct Object;
struct bNode;
struct bNodeSocket;
}  // namespace blender

namespace upbge::logic_nodes::game_state {

struct Checkpoint;
struct ObjectState;

bool material_parameter_socket_supports_object_attribute(const blender::bNodeSocket &socket);
std::string material_parameter_attribute_name(const blender::Material *material,
                                              const blender::bNode &node,
                                              const blender::bNodeSocket &socket);
bool read_object_material_parameter_values(const blender::Object *object,
                                           const std::string &property_name,
                                           std::array<float, 4> &r_values);

/** Immutable material assets present when the runtime scene registry is initialized. */
class MaterialAssetCatalog {
 public:
  MaterialAssetCatalog();
  ~MaterialAssetCatalog();
  MaterialAssetCatalog(MaterialAssetCatalog &&) noexcept;
  MaterialAssetCatalog &operator=(MaterialAssetCatalog &&) noexcept;

  MaterialAssetCatalog(const MaterialAssetCatalog &) = delete;
  MaterialAssetCatalog &operator=(const MaterialAssetCatalog &) = delete;

 private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;

  friend LN_GameStateResult build_material_asset_catalog(blender::Main &,
                                                         MaterialAssetCatalog &,
                                                         std::string &);
  friend class MaterialCaptureContext;
  friend LN_GameStateResult prepare_material_restore(
      blender::Main &,
      const MaterialAssetCatalog &,
      const Checkpoint &,
      const std::vector<struct MaterialRestoreTarget> &,
      class PreparedMaterialRestore &,
      std::string &);
};

LN_GameStateResult build_material_asset_catalog(blender::Main &bmain,
                                                MaterialAssetCatalog &r_catalog,
                                                std::string &r_error);

/** Per-save cache: each shared material graph is inspected at most once. */
class MaterialCaptureContext {
 public:
  explicit MaterialCaptureContext(const MaterialAssetCatalog &catalog);
  ~MaterialCaptureContext();
  MaterialCaptureContext(MaterialCaptureContext &&) noexcept;
  MaterialCaptureContext &operator=(MaterialCaptureContext &&) noexcept;

  MaterialCaptureContext(const MaterialCaptureContext &) = delete;
  MaterialCaptureContext &operator=(const MaterialCaptureContext &) = delete;

 private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;

  friend LN_GameStateResult capture_material_state(KX_GameObject &,
                                                   size_t,
                                                   MaterialCaptureContext &,
                                                   ObjectState &,
                                                   size_t &,
                                                   std::string &);
  friend LN_GameStateResult finalize_material_capture(MaterialCaptureContext &,
                                                      Checkpoint &,
                                                      std::string &);
};

LN_GameStateResult capture_material_state(KX_GameObject &object,
                                          size_t expected_slot_count,
                                          MaterialCaptureContext &context,
                                          ObjectState &r_state,
                                          size_t &r_budget,
                                          std::string &r_error);

/** Finalize the deduplicated shared material snapshots after every object has been captured. */
LN_GameStateResult finalize_material_capture(MaterialCaptureContext &context,
                                             Checkpoint &r_checkpoint,
                                             std::string &r_error);

struct MaterialRestoreTarget {
  KX_GameObject *object = nullptr;
  const ObjectState *saved = nullptr;
};

/**
 * Material load preparation may create hidden default properties. Until Commit(), destruction or
 * Rollback() removes every such property, leaving the object state unchanged.
 */
class PreparedMaterialRestore {
 public:
  PreparedMaterialRestore();
  ~PreparedMaterialRestore();
  PreparedMaterialRestore(PreparedMaterialRestore &&) noexcept;
  PreparedMaterialRestore &operator=(PreparedMaterialRestore &&) noexcept;

  PreparedMaterialRestore(const PreparedMaterialRestore &) = delete;
  PreparedMaterialRestore &operator=(const PreparedMaterialRestore &) = delete;

  /** Complete recoverable scene-update allocation before the no-fail load commit. */
  void ReserveCommitUpdates(KX_Scene &scene) const;
  void Commit(KX_Scene &scene);
  void Rollback();

 private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;

  friend LN_GameStateResult prepare_material_restore(
      blender::Main &,
      const MaterialAssetCatalog &,
      const Checkpoint &,
      const std::vector<MaterialRestoreTarget> &,
      PreparedMaterialRestore &,
      std::string &);
};

LN_GameStateResult prepare_material_restore(
    blender::Main &bmain,
    const MaterialAssetCatalog &catalog,
    const Checkpoint &checkpoint,
    const std::vector<MaterialRestoreTarget> &targets,
    PreparedMaterialRestore &r_prepared,
    std::string &r_error);

}  // namespace upbge::logic_nodes::game_state
