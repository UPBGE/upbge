/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "eevee_game_defines.hh"
#include "eevee_game_shader_shared.hh"
#include "GPU_texture.hh"

#include <optional>

struct bNodeTree;

namespace blender {
struct Mesh;
}

namespace blender::eevee_game {

struct ObjectHandle; /* eevee_game_sync.hh */

struct VolumeSettings {
  int   tile_size  = 16;    /* Screen-space pixels per froxel XY cell */
  int   samples_z  = 64;    /* Depth slices in the frustum-aligned voxel grid */
};

/* Frustum-aligned voxel (froxel) volumetric fog.
 *
 * Grid layout:
 *   XY: screen tiles of tile_size pixels each
 *   Z:  exponentially-spaced depth slices (more near the camera)
 *
 * Two-pass approach:
 *   Pass 1 (Scatter): inject light from the Fixed Shadow Atlas into each froxel.
 *   Pass 2 (Integrate): accumulate scattering along each Z column into a transmittance map.
 *
 * The integrated 3D texture is then sampled during the deferred lighting pass. */
class VolumeModule {
 public:
  VolumeModule(class GameInstance &inst);

  void init();
  /* Forgets the media of the previous frame. */
  void begin_sync();
  /* Collects the world medium and decides whether the passes run: only when
   * some medium exists, a scene without one pays nothing. */
  void sync();

  /**
   * Medium of a box-shaped mesh whose volume material is constant (see
   * volume_constant_medium()). Evaluated analytically by the scatter pass:
   * no voxelization, no material shader. Returns false when it cannot be
   * handled that way, the object then needs the general path.
   */
  bool box_add(const Object *ob, const Mesh &mesh, bNodeTree *ntree);

  /**
   * Register an object that carries a volume material so the volume pipeline
   * tracks it for froxel voxelisation. Called from SyncModule::sync_mesh()
   * when mat.has_volume == true. Mirrors eevee::VolumeModule::object_sync().
   */
  void object_sync(const ObjectHandle &ob_handle);

  /* Execute both passes; writes to the integrated froxel texture. */
  void render(View &view);

  /* Composite the integrated volume onto the 2D scene color buffer. */
  void resolve(gpu::Texture *target_color_tx, gpu::Texture *depth_tx);

  gpu::Texture *get_volume_texture() { return volume_integrated_tx_; }
  bool enabled() const { return enabled_; }

 private:
  GameInstance *inst_;
  VolumeSettings settings_;
  bool enabled_ = false;

  /* Constant media in boxes; the world medium is a box without bounds. */
  StorageArrayBuffer<VolumeBoxData, VOLUME_BOX_MAX> box_buf_ = {"Volume.Boxes"};
  int box_count_ = 0;

  /* 3D RGBA16F: scattering (RGB) + extinction (A) per froxel */
  Texture volume_grid_tx_ = {"volume_grid_tx_"};
  /* 3D RGBA16F: integrated transmittance + in-scattered light after Z accumulation */
  Texture volume_integrated_tx_ = {"volume_integrated_tx_"};

  PassSimple volume_scatter_ps_{"Volume.Scatter"};
  PassSimple volume_integration_ps_{"Volume.Integration"};
  PassSimple volume_resolve_ps_{"Volume.Resolve"};
};

} // namespace blender::eevee_game
