/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "eevee_game_defines.hh"
#include "GPU_texture.hh"
#include "GPU_framebuffer.hh"
#include "GPU_material.hh"
#include "draw_pass.hh"
#include "DRW_gpu_wrapper.hh" /* TextureFromPool */
#include "DNA_object_types.h"
#include "DNA_material_types.h"
#include "../eevee/eevee_material.hh"        /* eMaterialPipeline, eClosureBits */
#include "../eevee/eevee_pipeline.hh"        /* UtilityTexture */
#include "../eevee/eevee_sampling_shared.hh" /* SamplingData */
#include "../eevee/eevee_uniform_shared.hh"  /* UniformData, CameraData */

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

  /* Run the configured AA mode. src and dst must be different textures.
   * Returns the texture holding the result: dst, or src when AA is off and
   * nothing was written. */
  gpu::Texture *apply_aa(gpu::Texture *src, gpu::Texture *dst, const AASettings &settings);

 private:
  void apply_fxaa(gpu::Texture *src, gpu::Texture *dst);
  void apply_smaa(gpu::Texture *src, gpu::Texture *dst);

  GameInstance *inst_;

  PassSimple fxaa_ps_{"AA.FXAA"};
  PassSimple smaa_edge_ps_{"AA.SMAA.Edge"};
  PassSimple smaa_weight_ps_{"AA.SMAA.Weight"};
  PassSimple smaa_blend_ps_{"AA.SMAA.Blend"};

  /* SMAA lookup tables (BLI_smaa_textures.h): area RG8, search R8.
   * Constant, uploaded once in init(). */
  Texture smaa_area_lut_tx_ = {"smaa_area_lut"};
  Texture smaa_search_lut_tx_ = {"smaa_search_lut"};

  /* Intermediate SMAA targets at render resolution. Persistent: they are
   * needed every frame while SMAA is on, and their framebuffers stay valid. */
  Texture smaa_edge_tx_ = {"smaa_edge"};     /* RG8 */
  Texture smaa_weight_tx_ = {"smaa_weight"}; /* RGBA8 */

  Framebuffer smaa_edge_fb_ = {"smaa_edge_fb"};
  Framebuffer smaa_weight_fb_ = {"smaa_weight_fb"};
  /* Output framebuffer shared by FXAA and the SMAA blend stage. */
  Framebuffer output_fb_ = {"aa_output_fb"};
};

/* PipelineModule coordinates the G-Buffer, GTAO, SSGI, Deferred Lighting,
 * Transparent Forward, and FSR mask generation passes. */
class PipelineModule {
 public:
  PipelineModule(class ShaderModule *shaders, class GameInstance *inst);
  ~PipelineModule() = default;

  /* Called from GameInstance::end_sync(). */
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

  /* Called from GameInstance::begin_sync(), before any object is synced:
   * materials add their sub-passes to these passes during object sync. */
  void begin_sync();

  /* Geometry passes other modules submit with their own views. */
  PassMain &prepass_opaque_get()
  {
    return opaque_layer_.prepass_ps;
  }
  /* Casters drawn from the lights only, after the opaque prepass. */
  PassMain &shadow_casters_get()
  {
    return shadow_casters_ps_;
  }
  /* Fixed EEVEE interface of a generated shader drawn outside the geometry
   * passes (world capture, volume properties): globals, sampling and the
   * utility texture. */
  template<typename PassT> void eevee_globals_bind(PassT &pass);
  /* `eevee_game_light_data`: lights, their tile masks, shadows and world
   * lighting, by slot. */
  template<typename PassT> void light_data_bind(PassT &pass);
  /* Render pass and cryptomatte outputs some infos declare; never read back.
   * Their image slots overlap the volume property ones. */
  void render_pass_outputs_bind(PassSimple &pass);
  /* Depth-only bucket of the shadow casters without a camera prepass. */
  PassMain::Sub *shadow_caster_add(blender::Material *blender_mat, GPUMaterial *gpumat);

  /* True when the motion buffer must be cleared to VELOCITY_INVALID and the
   * static pixels resolved after the prepass: FSR is on and the camera moved. */
  bool use_velocity_resolve() const
  {
    return use_velocity_resolve_;
  }
  /* Camera motion of the pixels the velocity prepass did not write. */
  void render_velocity_resolve(View &view);

  AAModule   aa;
  AASettings aa_settings;

 private:
  GameInstance  *inst_;
  ShaderModule  *shaders_;

  /* Opaque deferred: prepass fills depth + velocity, gbuffer_ps fills EEVEE's
   * G-Buffer and writes emission, lighting_ps decodes the G-Buffer and adds
   * the lit radiance. */
  struct DeferredLayer {
    PassMain  prepass_ps{"Deferred.Prepass"};
    PassMain  gbuffer_ps{"Deferred.GBuffer"};
    PassSimple lighting_ps_{"Deferred.Lighting"};
  } opaque_layer_;

  /* Blended surfaces have no opaque prepass to cast shadows with. */
  PassMain shadow_casters_ps_{"Shadow.Casters"};

  /* Pass constants referenced by address from the lighting pass. */
  int use_ao_ = 0;
  int use_ssgi_ = 0;
  int use_ssr_ = 0;

  /* Forward surfaces, shaded by `eevee_game_surf_forward` after the
   * deferred lighting. Opaque ones get their depth from the opaque prepass
   * (they cast shadows and enter the Hi-Z like the deferred ones) and shade
   * with an EQUAL test. Transparent ones are one sub-pass per object, sorted
   * back to front, each preceded by its depth-only overlap mask when the
   * material hides its back faces. */
  struct ForwardLayer {
    PassMain opaque_ps{"Forward.Opaque"};
    PassSortable transparent_ps{"Forward.Transparent"};
  } forward_layer_;

  /* Binds the fixed part of the forward surface interface on a pass. */
  template<typename PassT> void forward_resources_bind(PassT &pass);

  /* `game_lighting_buf`: ambient, light count and tile grid, shared by the
   * light culling, the deferred lighting and the forward surfaces. */
  UniformBuffer<GameLightingData> lighting_buf_ = {"game_lighting_buf"};
  /* Tiled light culling: per screen tile, one bit per light of light_buf. */
  PassSimple light_cull_ps_ = {"Light.Culling"};
  StorageArrayBuffer<uint, 16, true> light_tile_mask_buf_ = {"light_tile_mask_buf"};
  int light_tile_mask_capacity_ = 16;
  /* Ray cast materials declare the object id and prepass normal textures
   * EEVEE's tracing reads. Nothing traces here: 1x1 stand-ins satisfy the
   * bindings. */
  Texture dummy_object_id_tx_ = {"dummy_object_id"};
  Texture dummy_normal_tx_ = {"dummy_prepass_normal"};
  /* Fog grid read by the forward surfaces: the integrated volume, or a
   * 1x1x1 stand-in (clear air) while the fog is off and nothing reads it. */
  Texture dummy_volume_tx_ = {"dummy_volume"};
  Texture dummy_transmittance_tx_ = {"dummy_volume_transmittance"};
  gpu::Texture *volume_tx_ = nullptr;
  gpu::Texture *volume_transmittance_tx_ = nullptr;

  /* Opaque-only color snapshot for the FSR reactive mask. */
  TextureFromPool opaque_copy_tx_ = {"opaque_copy"};
  Framebuffer gbuffer_fb_ = {"gbuffer_fb"};

  /* ---- What EEVEE's material shaders expect to find bound ----
   *
   * Surface shaders are generated by EEVEE's codegen, so their resource
   * interface is EEVEE's, not ours. These are the game-side stand-ins for
   * the pieces of eevee::Instance those shaders read. */

  /* Fill eevee_uniform_buf_ and the camera buffers for this frame. */
  void eevee_resources_update(int normal_layer_count);

  /* `uniform_buf`: camera, film extent, render pass indices, pipeline flags. */
  UniformBuffer<eevee::UniformData> eevee_uniform_buf_ = {"eevee_uniform_buf"};
  /* `sampling_buf`: per-sample random numbers. With no accumulation there is
   * a single sample, so it stays zero: alpha hash and jittered effects get a
   * fixed, temporally stable pattern. */
  StorageBuffer<eevee::SamplingData> eevee_sampling_buf_ = {"eevee_sampling_buf"};
  /* `utility_tx`: blue noise and BSDF lookup tables. */
  eevee::UtilityTexture eevee_utility_tx_;
  /* `camera_prev` / `camera_curr` / `camera_next`: the velocity prepass
   * projects each vertex with these to get its screen-space motion. */
  UniformBuffer<eevee::CameraData> camera_prev_buf_ = {"camera_prev"};
  UniformBuffer<eevee::CameraData> camera_curr_buf_ = {"camera_curr"};
  /* Only FSR reads the motion buffer, and only a moving camera gives the
   * static pixels any motion: otherwise their zero clear is already exact. */
  PassSimple velocity_resolve_ps_ = {"Velocity.Resolve"};
  bool use_velocity_resolve_ = false;
  /* `cryptomatte_object_buf`: never read (see RenderBuffers::cryptomatte_tx). */
  StorageArrayBuffer<float2, 16> cryptomatte_dummy_buf_ = {"cryptomatte_dummy"};

  /* Union of the closures used by the deferred materials synced this frame.
   * Decides how many G-Buffer layers are allocated. */
  eevee::eClosureBits closure_bits_ = eevee::CLOSURE_NONE;
};

} // namespace blender::eevee_game
