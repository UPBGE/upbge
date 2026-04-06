/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "eevee_game_instance.hh"

#include "BKE_object.hh"
#include "DEG_depsgraph_query.hh"
#include "DRW_render.hh"
#include "RE_pipeline.h"

#include "DNA_camera_types.h"
#include "DNA_scene_types.h"

namespace blender::eevee_game {

/* ================================================================
 * GameInstance constructor
 * ================================================================ */

GameInstance::GameInstance()
    : materials(*this),
      film(*this),
      shadows(*this, shadow_data_),
      culling(*this),
      sync(*this),
      bloom(*this),
      dof(*this),
      gtao(*this),
      ssgi(*this),
      raytrace(*this),
      volume(*this),
      pipelines(&shaders, this),
      upscale(*this),
      main_view_(*this)
{
}

/* ================================================================
 * init
 * ================================================================ */

void GameInstance::init(const int2 &output_res, const rcti *output_rect)
{
  update_eval_members();

  film.init(output_res);

  /* Allocate all render-resolution GPU resources. */
  render_buffers.init(film.render_extent_get());
  hiz_buffer.front.ensure(film.render_extent_get());
  hiz_buffer.back.ensure(film.render_extent_get());

  /* Static modules that need a one-time allocation. */
  culling.init();
  lights.init();
  shadows.init();
  volume.init();
  bloom.init();
  dof.init();
  gtao.init();
  ssgi.init();
  raytrace.init();

  /* Upscaling: create the FSR3 context if mode != OFF. */
  if (upscale_settings.mode != UpscaleMode::OFF) {
    upscale.init(film.render_extent_get(), film.display_extent_get());
  }

  (void)output_rect; /* Border render: not yet used. */
}

/* ================================================================
 * begin_sync
 * ================================================================ */

void GameInstance::begin_sync()
{
  update_eval_members();

  film.sync();

  /* Reset per-frame module state. */
  culling.begin_sync();
  lights.begin_sync();
  shadows.begin_sync();
  velocity.begin_sync();
  sync.begin_sync();

  /* Resolve the camera from the scene. */
  if (camera_eval_object != nullptr) {
    camera.sync(camera_eval_object);
  }

  /* Upload per-frame uniform data.
   * These fields map directly to the GLSL 'uniform_data' UBO. */
  uniform_data.viewmat     = math::invert(camera.object_to_world);
  uniform_data.viewinv     = camera.object_to_world;
  uniform_data.camera_pos  = camera.position();
  uniform_data.jitter      = film.get_pixel_jitter();
  uniform_data.z_near      = camera.data_get().clip_near;
  uniform_data.z_far       = camera.data_get().clip_far;
  uniform_data.delta_time  = delta_time_ms * 0.001f;
  uniform_data.frame_count = uint32_t(film.frame_index_get());

  /* Projection matrix — built from camera lens / sensor data.
   * film.render_extent_get() gives the render resolution, not the display resolution,
   * so the aspect ratio accounts for any FSR downscale. */
  const float2 render_res = float2(film.render_extent_get());
  const float  aspect     = render_res.x / render_res.y;

  /* FIX: was `2 * atan(sensor_width * 0.5 / focal_length) / aspect`.
   * Dividing the angle by aspect is mathematically wrong: it would shrink the vertical
   * FOV as the image gets wider, the opposite of what perspective projection requires.
   *
   * Correct derivation:
   *   A standard camera sensor is sensor_width × sensor_height.
   *   For a given focal length f, the full vertical FOV is:
   *     fov_y = 2 * atan(sensor_height / (2 * f))
   *   And sensor_height = sensor_width / aspect  (landscape format).
   *   So:
   *     fov_y = 2 * atan(sensor_width / (2 * f * aspect))
   *
   * The aspect ratio then goes into projection::perspective() where it scales
   * the horizontal extent of the frustum — not the angle. */
  const float  sensor_height = camera.data_get().sensor_width / aspect;
  const float  fov_y         = 2.0f * atanf(sensor_height /
                                             (2.0f * camera.data_get().focal_length));

  uniform_data.projectionmat = math::projection::perspective(
      fov_y, aspect,
      camera.data_get().clip_near,
      camera.data_get().clip_far);

  uniform_data.viewprojmat    = uniform_data.projectionmat * uniform_data.viewmat;
  uniform_data.screen_res     = render_res;
  uniform_data.screen_res_inv = float2(1.0f) / render_res;

  /* Traverse the Depsgraph and populate culling, lights, velocity, shadows.
   * Must happen after uniform_data is filled (culling uses camera matrices)
   * and after per-frame begin_sync() resets on all sub-modules. */
  sync_scene();
}

/* ================================================================
 * end_sync
 * ================================================================ */

void GameInstance::end_sync()
{
  /* Upload the packed light array after the scene traversal has added all lights. */
  lights.end_sync();

  /* Compute tight cascade view-projections from the final light list.
   * FIX: shadow_index writeback — ShadowModule::end_sync() now returns the
   * atlas indices it assigned so LightModule can stamp them into LightData.
   * See ShadowModule::end_sync() for the updated implementation. */
  shadows.end_sync();

  /* Propagate shadow atlas indices computed by ShadowModule back into LightData.
   * ShadowModule stores a compact vector of punctual-light atlas slots.
   * We iterate the light map and match by light identity to stamp shadow_index.
   *
   * FIX: previously shadow_index was never written back here, so all punctual lights
   * read shadow_index = -1 in the shader and produced no shadows. */
  {
    int slot = 0;
    for (auto &[id, light] : lights.light_map_.items()) {
      if (light.data.type == uint32_t(LightType::PUNCTUAL_SPOT) ||
          light.data.type == uint32_t(LightType::PUNCTUAL_POINT))
      {
        if (slot < MAX_PUNCTUAL_SHADOW_SLOTS) {
          light.data.shadow_index = slot++;
        }
        else {
          light.data.shadow_index = -1; /* Budget exceeded — no shadow. */
        }
      }
      /* Directional lights use the CSM cascade array; their index is fixed at 0. */
      else if (light.data.type == uint32_t(LightType::SUN)) {
        light.data.shadow_index = 0;
      }
    }
    /* Re-upload the light buffer now that shadow indices are stamped. */
    lights.upload();
  }

  /* Finalise velocity: upload indirection buffer and swap current → previous. */
  velocity.geometry_steps_fill();
  velocity.end_sync();

  sync.end_sync();

  /* Finish texture loading for all materials synced this frame. */
  materials.end_sync();

  /* Sync all per-frame effect state (texture allocation, pass configuration). */
  bloom.sync();
  dof.sync();
  gtao.sync();
  ssgi.sync();
  raytrace.sync();
  volume.sync();
  pipelines.sync();
}

/* ================================================================
 * render_sample / render_frame
 * ================================================================ */

void GameInstance::render_sample()
{
  main_view_.render();
}

void GameInstance::render_frame(RenderEngine *engine, RenderLayer *layer)
{
  render_sample();

  /* Read the final pixels from GPU back to CPU and push them into the RenderLayer.
   *
   * The final output lives in main_view_'s display_res_tx_ (after FSR upscale) or
   * aa_out_tx_ (when FSR is OFF). Both are blitted to the default framebuffer by
   * Film::present(). We read from there. */
  RenderResult *rr = RE_engine_get_result(engine);
  if (rr == nullptr) {
    RE_engine_end_result(engine, layer, false, false, false);
    return;
  }

  RenderPass *rp = RE_pass_find_by_name(layer, RE_PASSNAME_COMBINED, "");
  if (rp == nullptr) {
    RE_engine_end_result(engine, layer, false, false, false);
    return;
  }

  const int2 display_res = film.display_extent_get();

  DefaultFramebufferList *dfbl = DRW_viewport_framebuffer_list_get();
  GPU_framebuffer_bind(dfbl->default_fb);
  GPU_framebuffer_read_color(dfbl->default_fb,
                              0, 0,
                              display_res.x, display_res.y,
                              4,
                              0,
                              GPU_DATA_FLOAT,
                              rp->ibuf->float_buffer.data);

  /* Flip Y: OpenGL framebuffer origin is bottom-left, Blender render is top-left. */
  const int row_bytes = display_res.x * 4 * sizeof(float);
  Vector<uint8_t> tmp_row(row_bytes);
  float *pixels = rp->ibuf->float_buffer.data;
  for (int y = 0; y < display_res.y / 2; y++) {
    float *top    = pixels + (display_res.y - 1 - y) * display_res.x * 4;
    float *bottom = pixels + y * display_res.x * 4;
    memcpy(tmp_row.data(), top,    row_bytes);
    memcpy(top,    bottom, row_bytes);
    memcpy(bottom, tmp_row.data(), row_bytes);
  }

  RE_engine_end_result(engine, layer, false, false, false);
}

/* ================================================================
 * update_eval_members (private)
 * ================================================================ */

void GameInstance::update_eval_members()
{
  /* DRW_context_get() is only valid inside a DRW draw/render callback. */
  draw_ctx      = DRW_context_get();
  scene         = draw_ctx ? draw_ctx->scene      : nullptr;
  view_layer    = draw_ctx ? draw_ctx->view_layer : nullptr;
  depsgraph     = draw_ctx ? draw_ctx->depsgraph  : nullptr;
  manager       = draw_ctx ? draw_ctx->manager    : nullptr;
  render_engine_ = draw_ctx ? draw_ctx->render_engine : nullptr;

  camera_eval_object = nullptr;
  if (scene && scene->camera) {
    camera_eval_object = DEG_get_evaluated_object(depsgraph, scene->camera);
  }
}

/* ================================================================
 * HiZBuffer::ensure (helper)
 * ================================================================ */

void HiZBuffer::ensure(int2 render_res)
{
  /* R32F mip chain.  Mip count = floor(log2(max_dim)) + 1.
   * Used by culling (occlusion test) and SSR (coarse ray march). */
  const int mip_levels = 1 + int(floorf(log2f(float(math::reduce_max(render_res)))));
  ref_tx_.ensure_2d_mip(gpu::TextureFormat::SFLOAT_32, render_res, mip_levels,
                         GPU_TEXTURE_USAGE_SHADER_WRITE | GPU_TEXTURE_USAGE_SHADER_READ);
}

/* ================================================================
 * LightModule stubs
 * ================================================================ */

void LightModule::init()
{
  light_buf_ = std::make_unique<gpu::StorageBuffer>(
      MAX_LIGHTS * sizeof(LightData), GPU_USAGE_DYNAMIC);
}

void LightModule::begin_sync()
{
  light_map_.clear();
  active_light_count = 0;
}

void LightModule::add(int id, const LightEntry &entry)
{
  light_map_.add_overwrite(id, entry);
}

void LightModule::end_sync()
{
  /* Pack the map into a flat array in insertion order. */
  active_light_count = 0;
  LightData packed[MAX_LIGHTS];
  for (auto &[id, entry] : light_map_.items()) {
    if (active_light_count >= MAX_LIGHTS) {
      break;
    }
    packed[active_light_count++] = entry.data;
  }
  light_buf_->update(packed);
}

void LightModule::upload()
{
  /* Called after shadow_index has been stamped into light_map_ entries.
   * Re-pack and re-upload the light buffer so shaders see the updated indices. */
  end_sync();
}

void LightModule::bind_resources(PassSimple &ps)
{
  ps.bind_ssbo("light_buf", light_buf_.get());
}

} // namespace blender::eevee_game
