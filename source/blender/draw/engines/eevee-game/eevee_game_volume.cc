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
  if (!settings_.enabled) {
    return;
  }

  const int2 render_res = inst_->film.render_extent_get();

  /* Froxel grid dimensions derived from render resolution and tile size.
   * XY: screen tiles of tile_size pixels each.
   * Z: exponentially-spaced depth slices (more near, fewer far). */
  const int3 grid_res = int3(math::divide_ceil(render_res, int2(settings_.tile_size)),
                             settings_.samples_z);

  /* RGBA16F stores: RGB = scattering, A = extinction.
   * 16F is sufficient for fog density; avoids the 2x bandwidth cost of RGBA32F. */
  const eGPUTextureUsage usage = GPU_TEXTURE_USAGE_SHADER_WRITE | GPU_TEXTURE_USAGE_SHADER_READ;
  volume_grid_tx_.ensure_3d(gpu::TextureFormat::SFLOAT_16_16_16_16, grid_res, usage);
  volume_integrated_tx_.ensure_3d(gpu::TextureFormat::SFLOAT_16_16_16_16, grid_res, usage);
}

void VolumeModule::render(View & /*view*/)
{
  if (!settings_.enabled) {
    return;
  }

  const UniformData &ud = inst_->uniform_data;
  const int3 grid_size = volume_grid_tx_.size();

  /* Pass 1: Light Injection (Scatter)
   * Each froxel samples the static shadow atlas (sun cascades) to determine
   * how much directional light reaches that point. The Henyey-Greenstein
   * phase function biases scattering toward the light direction. */
  PassSimple &scatter = volume_scatter_ps_;
  scatter.init();
  scatter.shader_set(inst_->shaders.static_shader_get(SH_VOLUME_SCATTER));
  scatter.bind_texture("shadow_atlas", inst_->shadows.get_static_atlas());
  inst_->lights.bind_resources(scatter);
  scatter.bind_image("out_grid_img", volume_grid_tx_);
  scatter.push_constant("tile_size", settings_.tile_size);
  scatter.push_constant("samples_z", settings_.samples_z);
  scatter.push_constant("vol_density", settings_.density);
  /* Clamp g to [-0.99, 0.99]: the Henyey-Greenstein denominator
   *   (1 + g^2 - 2g cos(theta))^(3/2)
   * reaches zero at g = +-1 for theta = 0 (or pi). The resulting Inf would
   * spread through the Beer-Lambert integration of the whole column. */
  scatter.push_constant("vol_anisotropy", math::clamp(settings_.anisotropy, -0.99f, 0.99f));
  scatter.push_constant("light_count", inst_->lights.active_light_count);
  scatter.push_constant("viewmat", ud.viewmat);
  scatter.push_constant("viewprojinv", math::invert(ud.viewprojmat));
  scatter.push_constant("camera_pos", ud.camera_pos);
  scatter.push_constant("z_near", ud.z_near);
  scatter.push_constant("z_far", ud.z_far);
  scatter.push_constant("screen_res", ud.screen_res);
  scatter.push_constant("cascade_viewproj", inst_->shadows.get_data().cascade_viewproj[0]);
  scatter.dispatch(math::divide_ceil(grid_size, int3(8, 8, 1)));
  scatter.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH);
  inst_->manager->submit(scatter);

  /* Pass 2: Ray Integration along Z
   * One thread per XY froxel column, accumulating front to back. Only XY is
   * dispatched: the shader walks Z itself so each thread reads its column
   * sequentially. */
  PassSimple &integrate = volume_integration_ps_;
  integrate.init();
  integrate.shader_set(inst_->shaders.static_shader_get(SH_VOLUME_INTEGRATE));
  integrate.bind_texture("in_grid_tx", volume_grid_tx_);
  integrate.bind_image("out_integrated_img", volume_integrated_tx_);
  integrate.push_constant("z_near", ud.z_near);
  integrate.push_constant("z_far", ud.z_far);
  integrate.push_constant("samples_z", settings_.samples_z);
  integrate.dispatch(int3(math::divide_ceil(grid_size.xy(), int2(8)), 1));
  integrate.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH);
  inst_->manager->submit(integrate);
}

void VolumeModule::resolve(gpu::Texture *target_color_tx, gpu::Texture *depth_tx)
{
  if (!settings_.enabled) {
    return;
  }

  /* Full-screen composite of the integrated froxel grid:
   *   1. Depth gives the view-space ray length per pixel.
   *   2. Pixel + depth map to froxel UVW in [0,1]^3.
   *   3. Sample: RGB = in-scattered radiance, A = transmittance.
   *   4. out_color = in_color * transmittance + in_scatter. */
  const UniformData &ud = inst_->uniform_data;
  const int2 render_res = inst_->film.render_extent_get();

  PassSimple &pass = volume_resolve_ps_;
  pass.init();
  pass.shader_set(inst_->shaders.static_shader_get(SH_VOLUME_RESOLVE));
  pass.bind_texture("volume_integrated_tx", volume_integrated_tx_);
  pass.bind_texture("depth_tx", depth_tx);
  pass.bind_image("out_color_img", target_color_tx);
  pass.push_constant("z_near", ud.z_near);
  pass.push_constant("z_far", ud.z_far);
  pass.push_constant("tile_size", settings_.tile_size);
  pass.push_constant("samples_z", settings_.samples_z);
  pass.push_constant("screen_res", float2(render_res));
  pass.dispatch(math::divide_ceil(render_res, int2(8)));
  pass.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH);
  inst_->manager->submit(pass);
}

void VolumeModule::object_sync(const ObjectHandle & /*ob_handle*/)
{
  /* The froxel grid covers the whole camera frustum with a homogeneous
   * medium, so there is nothing to register per object yet. Kept as the hook
   * for per-object voxelisation of volume materials. */
}

} // namespace blender::eevee_game
