/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/**
 * World lighting.
 *
 * The world shader is drawn into a small cube map, which is then reduced to
 * what the lighting reads per pixel: L1 spherical harmonics for the diffuse
 * lobes, a GGX pre-filtered mip chain for the specular ones. All of it only
 * when the world changes (edit, new shader, compilation finished), so a
 * static world costs one SH dot product and one cube map fetch per closure
 * and nothing per frame on the CPU or in passes.
 */

#include "eevee_game_defines.hh"
#include "eevee_game_shader_shared.hh"

#include "../eevee/eevee_defines.hh"

#include "GPU_material.hh"
#include "GPU_texture.hh"

namespace blender {
struct World;
}

namespace blender::eevee_game {

class GameInstance;

class WorldModule {
 public:
  WorldModule(GameInstance &inst) : inst_(inst) {}

  /* Fetches the world shader and decides whether the lighting maps are stale. */
  void sync();
  /* Re-captures and filters them when sync() found them stale. */
  void render();
  /* Draws the world behind the opaque surfaces of the bound framebuffer, whose depth is the
   * prepass depth. No-op for a world without a shader: the color clear is the background. */
  void render_background(View &view);
  /* Color the frame is cleared to: the background of a world without a shader. */
  float4 background_clear_color() const
  {
    return float4((gpumat_ != nullptr) ? float3(0.0f) : constant_color_, 1.0f);
  }

  /* World lighting inputs of the deferred lighting and forward shaders. */
  template<typename PassT> void bind_resources(PassT &pass)
  {
    const GPUSamplerState filter = {GPU_SAMPLER_FILTERING_MIPMAP | GPU_SAMPLER_FILTERING_LINEAR};
    pass.bind_texture(SPHERE_PROBE_TEX_SLOT, &prefilter_tx_, filter);
    pass.bind_ssbo(IRRADIANCE_BRICK_BUF_SLOT, &sh_buf_);
  }

 private:
  /* Face size of the capture. Mirror-like reflections show its texels. */
  static constexpr int capture_res_ = 128;

  GameInstance &inst_;

  /* Captured radiance with a box filtered mip chain, the filtering source. */
  Texture radiance_tx_ = {"World.Radiance"};
  /* Mip m: radiance pre-filtered for roughness m / WORLD_CUBE_LOD_MAX. */
  Texture prefilter_tx_ = {"World.Prefilter"};
  /* float4(A, B) per color channel: diffuse radiance = A + dot(B, N). */
  StorageArrayBuffer<float4, 4> sh_buf_ = {"World.SH"};

  PassSimple capture_ps_ = {"World.Capture"};
  PassSimple background_ps_ = {"World.Background"};
  PassSimple sh_ps_ = {"World.SH"};
  PassSimple prefilter_ps_ = {"World.Prefilter"};
  View capture_view_ = {"World.Capture"};
  Framebuffer capture_fb_ = {"World.Capture"};

  /* World shader of this frame: null for a world without nodes, or while it
   * compiles. The constant color is then used instead. */
  GPUMaterial *gpumat_ = nullptr;
  float3 constant_color_ = float3(0.0f);

  /* What the maps were made from: any change makes them stale. */
  const World *captured_world_ = nullptr;
  uint64_t captured_update_ = 0;
  GPUMaterial *captured_gpumat_ = nullptr;
  float3 captured_color_ = float3(-1.0f);
  bool needs_update_ = true;
};

}  // namespace blender::eevee_game
