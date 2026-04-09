/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "eevee_game_defines.hh"
#include "GPU_texture.hh"
#include "GPU_framebuffer.hh"
#include "GPU_material.hh"
#include "draw_pass.hh"
#include "draw_texture_pool.hh"
#include "DNA_object_types.h"
#include "DNA_material_types.h"
#include "../eevee/eevee_material_shared.hh"   /* eMaterialPipeline */

namespace blender::eevee_game {

struct AASettings {
  bool   enabled = true;
  AAMode mode    = AAMode::FXAA;
};

/* AAModule: spatial Anti-Aliasing (FXAA and SMAA).
 * Only active when FSR temporal upscaling is OFF.
 * SMAA is a 3-pass rasterisation algorithm; FXAA is a single compute/fragment pass. */
class AAModule {
 public:
  AAModule(class GameInstance *inst) : inst_(inst) {}

  void init();

  /* Rebuild SMAA framebuffer attachments after a resolution change */
  void sync_framebuffers(int2 extent);

  /* Dispatch the configured AA mode. src and dst must be different textures. */
  void apply_aa(gpu::Texture *src, gpu::Texture *dst, const AASettings &settings);

 private:
  void apply_fxaa(gpu::Texture *src, gpu::Texture *dst);
  void apply_smaa(gpu::Texture *src, gpu::Texture *dst);

  GameInstance *inst_;

  PassSimple fxaa_ps_{"AA.FXAA"};
  PassSimple smaa_edge_ps_{"AA.SMAA.Edge"};
  PassSimple smaa_weight_ps_{"AA.SMAA.Weight"};
  PassSimple smaa_blend_ps_{"AA.SMAA.Blend"};

  /* SMAA Area LUT (256x256 RG8) - constant, loaded once in init() */
  std::unique_ptr<gpu::Texture> smaa_area_lut_tx_;

  /* SMAA Search LUT (64x16 RGBA8) - constant, loaded once in init() */
  std::unique_ptr<gpu::Texture> smaa_search_lut_tx_;

  /* Pooled intermediate SMAA targets; acquired/released each apply_smaa() call */
  TextureFromPool smaa_edge_tx_;
  TextureFromPool smaa_weight_tx_;

  /* Framebuffers bound during SMAA passes */
  Framebuffer smaa_edge_fb_;
  Framebuffer smaa_weight_fb_;
  Framebuffer smaa_blend_fb_;
};

/* PipelineModule coordinates the G-Buffer, GTAO, SSGI, Deferred Lighting,
 * Transparent Forward, and FSR mask generation passes. */
class PipelineModule {
 public:
  PipelineModule(class ShaderModule *shaders, class GameInstance *inst);
  ~PipelineModule() = default;

  void sync();

  /* Execute the full opaque + transparent pipeline for one view. */
  void render(View &view, Framebuffer &combined_fb);

  /**
   * Called by MaterialModule to register a new shader into the correct pipeline pass.
   * Returns a PassMain::Sub* used by MaterialModule to further sub-divide per material.
   * Returns nullptr for pipeline types handled per-object (volumes, transparent).
   */
  PassMain::Sub *material_add(Object *ob,
                              blender::Material *blender_mat,
                              GPUMaterial *gpumat,
                              eevee::eMaterialPipeline pipeline_type);

  /* Per-object transparent sub-passes (needed for back-to-front sorting). */
  PassMain::Sub *prepass_transparent_add(Object *ob,
                                         blender::Material *blender_mat,
                                         GPUMaterial *gpumat);
  PassMain::Sub *material_transparent_add(Object *ob,
                                          blender::Material *blender_mat,
                                          GPUMaterial *gpumat);

  AAModule   aa;
  AASettings aa_settings;

 private:
  GameInstance  *inst_;
  ShaderModule  *shaders_;

  /* Opaque deferred: prepass fills depth+velocity, gbuffer_ps fills closure data,
   * lighting_ps reads gbuffer and writes HDR color. */
  struct DeferredLayer {
    PassMain  prepass_ps{"Deferred.Prepass"};
    PassMain  gbuffer_ps{"Deferred.GBuffer"};
    PassSimple lighting_ps_{"Deferred.Lighting"};
  } opaque_layer_;

  /* Forward transparent: sorted back-to-front after deferred lighting. */
  struct ForwardTransparentLayer {
    PassMain prepass_ps{"Forward.Transparent.Prepass"};
    PassMain shading_ps{"Forward.Transparent.Shading"};
  } forward_layer_;

  /* Shadow atlas rendering pass. */
  PassMain shadow_ps_{"Shadow"};

  TextureFromPool opaque_copy_tx_;
  Framebuffer gbuffer_fb_;
};

} // namespace blender::eevee_game
