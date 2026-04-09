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

  /* Allocate render-resolution GPU resources. */
  render_buffers.init(film.render_extent_get());
  hiz_buffer.front.ensure(film.render_extent_get());
  hiz_buffer.back.ensure(film.render_extent_get());

  /* Static modules. */
  culling.init();
  lights.init();
  /* shadows.init() allocates both atlases.
   * Default: DynamicShadowSlots::S16 (16 slots, 2048x2048, ~4 MB).
   * To change at runtime (e.g. from game settings), call:
   *   shadows.init(DynamicShadowSlots::S4 / S16 / S64 / OFF) */
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

  /* FIX: static_shaders_load() was implemented but never called.
   * Without this call every shader compiles synchronously on first use
   * (inside static_shader_get()), stalling the render thread for 50-200ms
   * per shader on cold start and causing a multi-second hitch on the first frame.
   *
   * Calling with SG_ALL here front-loads all compilation cost at engine startup,
   * where a one-time stall is acceptable (it happens during the loading screen
   * in a game, not mid-gameplay). */
  shaders.static_shaders_load(SG_ALL);

  /* FIX: ShadingView::sync() was never called, leaving postfx_tx_, aa_out_tx_, and
   * display_res_tx_ without GPU handles. Any pass that tried to bind them would
   * produce GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT or a null GPU handle crash.
   * sync() allocates all three textures at the correct resolutions and rebuilds
   * the combined framebuffer config. */
  main_view_.sync();

  (void)output_rect; /* Border render not yet implemented. */
}

/* ================================================================
 * begin_sync
 * ================================================================ */

void GameInstance::begin_sync()
{
  update_eval_members();

  film.sync();

  /* Re-sync the view textures if the resolution changed (e.g. window resize
   * or FSR mode toggle). Film::sync() updates render_extent_ / display_extent_
   * before this runs, so the new resolution is already available. */
  main_view_.sync();

  /* Reset per-frame module state. */
  culling.begin_sync();
  lights.begin_sync();
  shadows.begin_sync();
  velocity.begin_sync();
  sync.begin_sync();

  if (camera_eval_object != nullptr) {
    camera.sync(camera_eval_object);
  }

  /* Upload per-frame uniform data. */
  uniform_data.viewmat     = math::invert(camera.object_to_world);
  uniform_data.viewinv     = camera.object_to_world;
  uniform_data.camera_pos  = camera.position();
  /* FSR3: advance the SDK jitter counter NOW so the offset is baked into the
   * projection matrix before the prepass and G-Buffer are rendered.
   * apply_fsr3() reads the value back later — it no longer advances the counter.
   * Non-FSR path: advance_jitter() returns float2(0) without touching the SDK. */
  uniform_data.jitter = (upscale_settings.mode != UpscaleMode::OFF) ?
                         upscale.advance_jitter() :
                         float2(0.0f);
  uniform_data.z_near      = camera.data_get().clip_near;
  uniform_data.z_far       = camera.data_get().clip_far;
  uniform_data.delta_time  = delta_time_ms * 0.001f;
  uniform_data.frame_count = uint32_t(film.frame_index_get());

  const float2 render_res = float2(film.render_extent_get());
  /* Guard against zero height during window creation / minimise.
   * A zero denominator produces Inf in sensor_height and NaN in atan2,
   * which corrupts the projection matrix for the entire frame.
   * Aspect 1:1 is a safe neutral default: produces a valid matrix with
   * no geometry visible, better than a NaN that propagates everywhere. */
  const float  aspect     = (render_res.y > 0.0f) ?
                             render_res.x / render_res.y : 1.0f;
  const float sensor_height = camera.data_get().sensor_width / aspect;
  const float fov_y         = 2.0f * atanf(sensor_height /
                                            (2.0f * camera.data_get().focal_length));

  uniform_data.projectionmat = math::projection::perspective(
      fov_y, aspect,
      camera.data_get().clip_near,
      camera.data_get().clip_far);

  uniform_data.viewprojmat    = uniform_data.projectionmat * uniform_data.viewmat;
  uniform_data.screen_res     = render_res;
  uniform_data.screen_res_inv = float2(1.0f) / render_res;

  sync_scene();
}

/* ================================================================
 * end_sync
 * ================================================================ */

void GameInstance::end_sync()
{
  /* Do NOT call lights.end_sync() here: it packs and uploads the SSBO before
   * shadow_index has been stamped into each LightEntry.  The correct upload
   * happens via lights.upload() below, which calls end_sync() internally after
   * the shadow index writeback loop.  Calling end_sync() twice per frame wastes
   * one full SSBO DMA transfer (~MAX_LIGHTS * sizeof(LightData) bytes). */
  shadows.end_sync();

  /* Shadow index writeback — dual atlas edition.
   *
   * ShadowModule::end_sync() builds static_punctual_ and dynamic_punctual_
   * lists but LightData.shadow_index must be set before the GPU upload.
   *
   * Encoding convention (must match the deferred lighting shader):
   *   shadow_index >= 0                       → static atlas slot index
   *   shadow_index in [DYNAMIC_BASE .. +N-1]  → dynamic atlas slot index
   *   shadow_index == -1                      → no shadow
   *
   * We use STATIC_PUNCTUAL_SLOTS as the base offset for dynamic indices so
   * the shader can distinguish the two atlases with a single integer compare:
   *   if (idx < STATIC_PUNCTUAL_SLOTS) → sample static_atlas
   *   else                             → sample dynamic_atlas at (idx - base)
   */
  constexpr int DYNAMIC_INDEX_BASE = STATIC_PUNCTUAL_SLOTS; /* == 12 */

  int static_slot  = 0;
  int dynamic_slot = 0;

  for (auto &[id, light] : lights.light_map_.items()) {
    if (light.data.type == uint32_t(LightType::SUN)) {
      light.data.shadow_index = 0; /* CSM always uses static atlas cascade row. */
      continue;
    }

    if (light.data.type != uint32_t(LightType::PUNCTUAL_SPOT) &&
        light.data.type != uint32_t(LightType::PUNCTUAL_POINT))
    {
      light.data.shadow_index = -1;
      continue;
    }

    const bool is_dynamic = (light.data.flags & LIGHT_FLAG_SHADOW_DYNAMIC) != 0;

    if (is_dynamic && dynamic_slot < shadows.dynamic_slot_count()) {
      /* Dynamic atlas: encode index above the static range. */
      light.data.shadow_index = DYNAMIC_INDEX_BASE + dynamic_slot++;
    }
    else if (!is_dynamic && static_slot < STATIC_PUNCTUAL_SLOTS) {
      light.data.shadow_index = static_slot++;
    }
    else {
      /* Fallback: dynamic atlas full → try static; static full → no shadow. */
      if (static_slot < STATIC_PUNCTUAL_SLOTS) {
        light.data.shadow_index = static_slot++;
      }
      else {
        light.data.shadow_index = -1;
      }
    }
  }

  lights.upload(); /* Re-pack and push to GPU with updated shadow indices. */

  velocity.geometry_steps_fill();
  velocity.end_sync();
  sync.end_sync();
  materials.end_sync();

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

  /* Flip Y: OpenGL origin is bottom-left, Blender render is top-left. */
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
  draw_ctx       = DRW_context_get();
  scene          = draw_ctx ? draw_ctx->scene      : nullptr;
  view_layer     = draw_ctx ? draw_ctx->view_layer : nullptr;
  depsgraph      = draw_ctx ? draw_ctx->depsgraph  : nullptr;
  manager        = draw_ctx ? draw_ctx->manager    : nullptr;
  render_engine_ = draw_ctx ? draw_ctx->render_engine : nullptr;

  camera_eval_object = nullptr;
  if (scene && scene->camera) {
    camera_eval_object = DEG_get_evaluated_object(depsgraph, scene->camera);
  }
}

/* ================================================================
 * HiZBuffer::ensure
 * ================================================================ */

void HiZBuffer::ensure(int2 render_res)
{
  const int mip_levels = 1 + int(floorf(log2f(float(math::reduce_max(render_res)))));
  ref_tx_.ensure_2d_mip(gpu::TextureFormat::SFLOAT_32, render_res, mip_levels,
                         GPU_TEXTURE_USAGE_SHADER_WRITE | GPU_TEXTURE_USAGE_SHADER_READ);
}

/* ================================================================
 * LightModule
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
  /* Re-pack and re-upload after shadow_index has been stamped by GameInstance::end_sync(). */
  end_sync();
}

void LightModule::bind_resources(PassSimple &ps)
{
  ps.bind_ssbo("light_buf", light_buf_.get());
}

} // namespace blender::eevee_game
