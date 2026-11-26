/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "eevee_game_world.hh"
#include "eevee_game_instance.hh"

#include "../eevee/eevee_camera.hh" /* cubeface_mat */

#include "BLI_math_constants.h"
#include "DEG_depsgraph_query.hh"
#include "DNA_world_types.h"

namespace blender::eevee_game {

void WorldModule::sync()
{
  World *world = inst_.scene->world;

  gpumat_ = nullptr;
  constant_color_ = float3(0.0f);
  if (world != nullptr) {
    constant_color_ = float3(world->horr, world->horg, world->horb);
    if (world->nodetree != nullptr) {
      /* Compiled in the background: the constant color lights the scene
       * until the shader is ready, then the maps are made once. */
      GPUMaterial *gpumat = inst_.shaders.world_shader_get(world, world->nodetree, true);
      if (GPU_material_status(gpumat) == GPU_MAT_SUCCESS) {
        gpumat_ = gpumat;
      }
    }
  }

  const World *world_orig = (world != nullptr) ? DEG_get_original(world) : nullptr;
  const uint64_t world_update = (world != nullptr) ? world->last_update : 0;
  needs_update_ |= (world_orig != captured_world_) || (world_update != captured_update_) ||
                   (gpumat_ != captured_gpumat_) ||
                   (gpumat_ == nullptr && constant_color_ != captured_color_);
  captured_world_ = world_orig;
  captured_update_ = world_update;
  captured_gpumat_ = gpumat_;
  captured_color_ = constant_color_;

  if (gpumat_ != nullptr) {
    /* Background: the world shader per pixel, where the prepass left the clear depth. The
     * full-screen triangle is at the far plane, 0 in reversed depth like the clear. */
    PassSimple &pass = background_ps_;
    pass.init();
    pass.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_DEPTH_EQUAL |
                   DRW_STATE_CLIP_CONTROL_UNIT_RANGE);
    inst_.pipelines.eevee_globals_bind(pass);
    inst_.pipelines.render_pass_outputs_bind(pass);
    pass.material_set(*inst_.manager, gpumat_);
    pass.draw_procedural(GPU_PRIM_TRIS, 1, 3);
  }

  if (!needs_update_) {
    return;
  }

  /* Full chain on the source: the filtering reads it at the footprint of
   * each sample. */
  const int source_mips = int(log2f(float(capture_res_))) + 1;
  radiance_tx_.ensure_cube(gpu::TextureFormat::SFLOAT_16_16_16_16,
                           capture_res_,
                           GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_ATTACHMENT,
                           nullptr,
                           source_mips);
  prefilter_tx_.ensure_cube(gpu::TextureFormat::SFLOAT_16_16_16_16,
                            capture_res_,
                            GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_SHADER_WRITE,
                            nullptr,
                            WORLD_CUBE_MIP_COUNT);
  /* Written as 6 layer arrays, one view per mip. */
  prefilter_tx_.ensure_mip_views(true);

  const GPUSamplerState filter = {GPU_SAMPLER_FILTERING_MIPMAP | GPU_SAMPLER_FILTERING_LINEAR};

  if (gpumat_ != nullptr) {
    PassSimple &pass = capture_ps_;
    pass.init();
    pass.state_set(DRW_STATE_WRITE_COLOR);
    inst_.pipelines.eevee_globals_bind(pass);
    inst_.pipelines.render_pass_outputs_bind(pass);
    pass.material_set(*inst_.manager, gpumat_);
    pass.draw_procedural(GPU_PRIM_TRIS, 1, 3);
  }

  {
    PassSimple &pass = sh_ps_;
    pass.init();
    pass.shader_set(inst_.shaders.static_shader_get(SH_WORLD_SH));
    pass.bind_texture("radiance_tx", &radiance_tx_, filter);
    pass.bind_ssbo("out_sh_buf", &sh_buf_);
    pass.push_constant("sh_lod", log2f(float(capture_res_ / WORLD_SH_RES)));
    pass.dispatch(int3(1));
    pass.barrier(GPU_BARRIER_SHADER_STORAGE);
  }

  {
    PassSimple &pass = prefilter_ps_;
    pass.init();
    pass.shader_set(inst_.shaders.static_shader_get(SH_WORLD_PREFILTER));
    pass.bind_texture("radiance_tx", &radiance_tx_, filter);
    /* 4 pi over the texels of the six faces. */
    pass.push_constant("texel_solid_angle",
                       float(4.0 * M_PI) / float(6 * capture_res_ * capture_res_));
    for (int mip = 0; mip < WORLD_CUBE_MIP_COUNT; mip++) {
      const int size = capture_res_ >> mip;
      pass.bind_image("out_img", prefilter_tx_.mip_view(mip));
      pass.push_constant("roughness", float(mip) / WORLD_CUBE_LOD_MAX);
      pass.dispatch(int3((size + 7) / 8, (size + 7) / 8, 6));
    }
    pass.barrier(GPU_BARRIER_TEXTURE_FETCH);
  }
}

void WorldModule::render_background(View &view)
{
  if (gpumat_ == nullptr) {
    return;
  }
  GPU_debug_group_begin("World.Background");
  inst_.manager->submit(background_ps_, view);
  GPU_debug_group_end();
}

void WorldModule::render()
{
  if (!needs_update_) {
    return;
  }
  needs_update_ = false;

  GPU_debug_group_begin("World.Lighting");
  if (gpumat_ != nullptr) {
    /* The capture lights the scene: a Light Path node sees a reflection ray, as in EEVEE's
     * world probe, not a camera ray. A world that shows a backdrop to the camera and lights
     * the scene with something else otherwise lit it with the backdrop. */
    inst_.pipelines.ray_type_set(eevee::RAY_TYPE_GLOSSY);
    /* The world shader only uses the view rotation. The projection is the 90
     * degree frustum of a cube face; its depth range is irrelevant. */
    const float4x4 winmat = math::projection::perspective(-0.1f, 0.1f, -0.1f, 0.1f, 0.1f, 1.0f);
    for (int face = 0; face < 6; face++) {
      capture_view_.sync(eevee::cubeface_mat(face), winmat);
      capture_fb_.ensure(GPU_ATTACHMENT_NONE,
                         GPU_ATTACHMENT_TEXTURE_CUBEFACE(radiance_tx_, face));
      capture_fb_.bind();
      inst_.manager->submit(capture_ps_, capture_view_);
    }
    inst_.pipelines.ray_type_set(eevee::RAY_TYPE_CAMERA);
  }
  else {
    const float4 color = float4(constant_color_, 1.0f);
    GPU_texture_clear(radiance_tx_, GPU_DATA_FLOAT, &color);
  }
  GPU_texture_update_mipmap_chain(radiance_tx_);
  inst_.manager->submit(sh_ps_);
  inst_.manager->submit(prefilter_ps_);
  GPU_debug_group_end();
}

}  // namespace blender::eevee_game
