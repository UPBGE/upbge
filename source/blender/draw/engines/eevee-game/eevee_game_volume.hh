/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "eevee_game_defines.hh"
#include "eevee_game_shader_shared.hh"
#include "GPU_batch_utils.hh"
#include "GPU_material.hh"
#include "GPU_texture.hh"

#include "../eevee/eevee_volume_shared.hh"

#include <memory>
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
  ~VolumeModule();

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
   * General path, as EEVEE: the object is rasterized at froxel resolution,
   * first to mark the froxels it holds (occupancy), then to evaluate its
   * volume shader in those and add the result to the property grids. One
   * layer per object, so overlapping objects never race on the additive
   * writes. `geom`: the surface batch of a mesh, the unit cube of a volume
   * object. Returns false when the material reads nothing it can draw with.
   */
  bool object_add(Object *ob,
                  const blender::Material *blender_mat,
                  GPUMaterial *occupancy_mat,
                  GPUMaterial *volume_mat,
                  gpu::Batch *geom,
                  ResourceHandleRange res_handle);

  /* Unit cube drawn for volume objects: their shaders map it to the grids. */
  gpu::Batch *unit_cube_batch_get()
  {
    return cube_batch_;
  }

  /* EEVEE's froxel mapping, as read by the generated volume shaders: same
   * grid and depth slicing as this module. */
  void eevee_data_fill(eevee::VolumesInfoData &data) const;

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

  /* ---- Voxelized media (general path) ---- */

  struct VoxelLayer {
    PassMain ps = {"Volume.Layer"};
  };
  /* Pool reused across frames; the first layer_count_ are this frame's. */
  Vector<std::unique_ptr<VoxelLayer>> layers_;
  int layer_count_ = 0;
  /* A world whose volume is not constant fills every froxel first. */
  PassSimple world_ps_ = {"Volume.World"};
  bool use_world_voxels_ = false;
  /* World or objects wrote the property grids this frame. */
  bool use_voxels_ = false;
  int3 grid_res_ = int3(0);

  /* Properties summed by the volume shaders, EEVEE's formats. */
  Texture prop_scattering_tx_ = {"Volume.Scattering"};
  Texture prop_extinction_tx_ = {"Volume.Extinction"};
  Texture prop_emission_tx_ = {"Volume.Emission"};
  Texture prop_phase_tx_ = {"Volume.Phase"};
  Texture prop_phase_weight_tx_ = {"Volume.PhaseWeight"};
  /* One bit per froxel; hit lists for the accurate intersection method. */
  Texture occupancy_tx_ = {"Volume.Occupancy"};
  Texture hit_depth_tx_ = {"Volume.HitDepth"};
  Texture hit_count_tx_ = {"Volume.HitCount"};
  /* Stencil keeps one material fragment per froxel column. */
  Texture front_depth_tx_ = {"Volume.FrontDepth"};
  Framebuffer occupancy_fb_ = {"Volume.Occupancy"};
  /* Infinite projection: objects past the far plane still voxelize. */
  View volume_view_ = {"Volume.View"};
  gpu::Batch *cube_batch_ = GPU_batch_unit_cube();

  /* What the scatter pass reads: the grids, or a 1^3 zero texture when no
   * shader wrote them (it then skips them anyway). */
  Texture dummy_prop_tx_ = {"Volume.DummyProp"};
  gpu::Texture *scatter_scattering_tx_ = nullptr;
  gpu::Texture *scatter_extinction_tx_ = nullptr;
  gpu::Texture *scatter_emission_tx_ = nullptr;
  gpu::Texture *scatter_phase_tx_ = nullptr;
  gpu::Texture *scatter_phase_weight_tx_ = nullptr;

  template<typename PassT> void properties_bind(PassT &pass);

  /* 3D RGBA16F: scattering (RGB) + extinction (A) per froxel */
  Texture volume_grid_tx_ = {"volume_grid_tx_"};
  /* 3D RGBA16F: integrated transmittance + in-scattered light after Z accumulation */
  Texture volume_integrated_tx_ = {"volume_integrated_tx_"};

  PassSimple volume_scatter_ps_{"Volume.Scatter"};
  PassSimple volume_integration_ps_{"Volume.Integration"};
  PassSimple volume_resolve_ps_{"Volume.Resolve"};
};

} // namespace blender::eevee_game
