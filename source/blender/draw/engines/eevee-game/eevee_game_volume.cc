/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "eevee_game_volume.hh"
#include "eevee_game_instance.hh"

namespace blender::eevee_game {

VolumeModule::VolumeModule(GameInstance &inst) : inst_(&inst) {}

void VolumeModule::init()
{
  /* Defaults: mild forward-scattering fog.
   * density = 0.1 gives a thin atmospheric haze at 100m depth.
   * anisotropy = 0.7 biases scattering toward the light (forward-scattering medium). */
}

void VolumeModule::sync()
{
  const int2 render_res = inst_->film.render_extent_get();

  /* Froxel grid dimensions derived from render resolution and tile size.
   * XY: screen tiles of tile_size pixels each.
   * Z: exponentially-spaced depth slices (more near, fewer far). */
  const int3 grid_res = int3(
      math::divide_ceil(render_res.x, settings_.tile_size),
      math::divide_ceil(render_res.y, settings_.tile_size),
      settings_.samples_z);

  /* RGBA16F stores: R=scattering R, G=scattering G, B=scattering B, A=extinction.
   * 16F is sufficient for fog density; avoids the 2x bandwidth cost of RGBA32F. */
  if (!volume_grid_tx_) {
    volume_grid_tx_ = std::make_unique<gpu::Texture>("volume_grid");
  }
  volume_grid_tx_->ensure_3d(
      gpu::TextureFormat::RGBA16F,
      grid_res,
      GPU_TEXTURE_USAGE_SHADER_WRITE | GPU_TEXTURE_USAGE_SHADER_READ);

  if (!volume_integrated_tx_) {
    volume_integrated_tx_ = std::make_unique<gpu::Texture>("volume_integrated");
  }
  volume_integrated_tx_->ensure_3d(
      gpu::TextureFormat::RGBA16F,
      grid_res,
      GPU_TEXTURE_USAGE_SHADER_WRITE | GPU_TEXTURE_USAGE_SHADER_READ);

  volume_scatter_ps_.init();
  volume_scatter_ps_.shader_set(inst_->shaders.static_shader_get(SH_VOLUME_SCATTER));

  volume_integration_ps_.init();
  volume_integration_ps_.shader_set(inst_->shaders.static_shader_get(SH_VOLUME_INTEGRATE));

  /* Force the resolve pass to be rebuilt since resolution may have changed. */
  resolve_synced_ = false;
}

void VolumeModule::render(View &view)
{
  if (!settings_.enabled) {
    return;
  }

  GPU_debug_group_begin("Volumetrics");

  /* Pass 1: Light Injection (Scatter)
   * Each froxel samples the Fixed Shadow Atlas to determine how much directional
   * light reaches that point. The Henyey-Greenstein phase function biases
   * scattering toward the light direction (forward-scattering fog). */
  volume_scatter_ps_.bind_texture("shadow_atlas_tx", inst_->shadows.get_atlas());
  volume_scatter_ps_.bind_resources(inst_->lights);
  volume_scatter_ps_.bind_resources(inst_->uniform_data);
  volume_scatter_ps_.bind_image("out_grid_img", volume_grid_tx_.get());
  volume_scatter_ps_.push_constant("vol_density",    settings_.density);
  volume_scatter_ps_.push_constant("vol_anisotropy", settings_.anisotropy);

  const int3 grid_size = volume_grid_tx_->size();
  volume_scatter_ps_.dispatch(math::divide_ceil(grid_size, int3(8, 8, 1)));
  volume_scatter_ps_.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS);

  /* Pass 2: Ray Integration along Z
   * Each thread processes one XY froxel column and accumulates scattering from
   * front to back (near to far). Dispatch only XY: the shader iterates Z internally
   * to preserve cache locality (sequential Z reads from a single thread). */
  volume_integration_ps_.bind_texture("in_grid_tx",       volume_grid_tx_.get());
  volume_integration_ps_.bind_image("out_integrated_img", volume_integrated_tx_.get());

  volume_integration_ps_.dispatch(
      int3(math::divide_ceil(grid_size.x, 8),
           math::divide_ceil(grid_size.y, 8),
           1));
  volume_integration_ps_.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS |
                                  GPU_BARRIER_TEXTURE_FETCH);

  GPU_debug_group_end();
}

void VolumeModule::resolve(gpu::Texture *target_color_tx, gpu::Texture *depth_tx)
{
  if (!settings_.enabled) {
    return;
  }

  GPU_debug_group_begin("Volumetrics.Resolve");

  /* FIX: was using SH_VOLUME_INTEGRATE for the resolve pass — that is the 3D grid
   * integration shader (froxel Z accumulation), not the 2D screen composite shader.
   * Using the wrong shader means the resolve pass reads/writes with the wrong I/O layout
   * and produces garbage or undefined output.
   *
   * SH_VOLUME_RESOLVE maps to "eevee_game_volume_resolve" which is a dedicated
   * full-screen compute shader that:
   *   1. Reads depth_tx to compute view-space ray length per pixel.
   *   2. Converts pixel+depth to froxel UVW in [0,1]^3.
   *   3. Samples volume_integrated_tx (RGBA16F):
   *        RGB = in-scattered radiance.
   *        A   = transmittance (Beer-Lambert: exp(-extinction * length)).
   *   4. Composites: out_color = in_color * transmittance + in_scatter.
   *
   * resolve_synced_ guards against rebuilding the pass on every resolve() call.
   * It is reset to false in sync() when the resolution or froxel grid changes. */
  if (!resolve_synced_) {
    volume_resolve_ps_.init();
    volume_resolve_ps_.shader_set(
        inst_->shaders.static_shader_get(SH_VOLUME_RESOLVE)); /* FIX: was SH_VOLUME_INTEGRATE */
    volume_resolve_ps_.bind_resources(inst_->uniform_data);
    resolve_synced_ = true;
  }

  volume_resolve_ps_.bind_texture("volume_integrated_tx", volume_integrated_tx_.get());
  volume_resolve_ps_.bind_texture("depth_tx",             depth_tx);
  volume_resolve_ps_.bind_image("out_color_img",          target_color_tx);

  /* Push froxel grid dimensions so the shader can reconstruct UVW from screen UV + depth. */
  const int2 render_res = inst_->film.render_extent_get();
  volume_resolve_ps_.push_constant("tile_size",  settings_.tile_size);
  volume_resolve_ps_.push_constant("samples_z",  settings_.samples_z);
  volume_resolve_ps_.push_constant("screen_res", float2(render_res));

  /* One thread per screen pixel. */
  volume_resolve_ps_.dispatch(
      math::divide_ceil(int3(render_res.x, render_res.y, 1), int3(8, 8, 1)));
  volume_resolve_ps_.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH);

  GPU_debug_group_end();
}

void VolumeModule::object_sync(const draw::ObjectHandle & /*ob_handle*/)
{
  /* eevee_game volumes are frustum-wide: the froxel grid covers the full camera
   * frustum unconditionally. Per-object registration is not needed for correctness —
   * the scatter pass samples all volume material sub-passes via the draw manager.
   *
   * When selective volume-object culling is added (e.g. skip objects outside the froxel
   * far plane), store ob_handle.object_key here and filter the voxelisation draw calls. */
}

} // namespace blender::eevee_game
