/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "eevee_game_instance.hh"

#include "BKE_object.hh"
#include "DEG_depsgraph_query.hh"
#include "DRW_render.hh"
#include "RE_engine.h"
#include "RE_pipeline.h"

#include "draw_view_data.hh" /* DefaultTextureList */

#include "DNA_camera_types.h"

#include "../eevee/eevee_shader.hh"
#include "DNA_scene_types.h"

namespace blender::eevee_game {

/* ================================================================
 * GameInstance constructor
 * ================================================================ */

GameInstance::GameInstance()
    : materials(*this),
      film(*this),
      render_buffers(*this),
      shadows(*this, shadow_data_),
      velocity(*this),
      sync(*this),
      bloom(*this),
      dof(*this),
      gtao(*this),
      ssgi(*this),
      raytrace(this),
      volume(*this),
      world(*this),
      pipelines(&shaders, this),
      upscale(*this),
      main_view_(*this)
{
}

/* ================================================================
 * init
 * ================================================================ */

void GameInstance::init()
{
  const DRWContext *ctx = DRW_context_get();
  DefaultTextureList *dtxl = ctx->viewport_texture_list_get();
  init(int2(GPU_texture_width(dtxl->color), GPU_texture_height(dtxl->color)), nullptr);
}

void GameInstance::init(const int2 &output_res, const rcti *output_rect)
{
  update_eval_members();
  settings_sync();

  film.init(output_res);

  /* Resolution dependent targets. No-op while the resolution is unchanged.
   * A resize is a history discontinuity like a camera cut: SSGI, FSR and
   * occlusion culling must not read what the old buffers held. */
  if (render_buffers.acquire(film.render_extent_get())) {
    frame_state.camera_cut = true;
  }

  if (!modules_initialized_) {
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
    pipelines.aa.init();

    /* Front-load every static shader. Without this each one compiles on
     * first use inside static_shader_get(), i.e. in the middle of a frame. */
    shaders.static_shaders_load(SG_ALL);
    /* The one EEVEE static shader used directly (VelocityModule). */
    eevee::ShaderModule::module_get()->static_shaders_wait_ready(eevee::VERTEX_COPY_SHADERS);

    modules_initialized_ = true;
  }

  /* Upscaling: create the FSR3 resources, and recreate them only when the mode
   * or a resolution changes, not on every redraw. */
  const int4 upscale_key = int4(film.render_extent_get(), film.display_extent_get());
  if (upscale_settings.mode != UpscaleMode::OFF &&
      (!upscale.is_initialized() || upscale_key != upscale_key_))
  {
    upscale.init(film.render_extent_get(), film.display_extent_get());
    upscale_key_ = upscale_key;
  }

  /* Post-processing targets, Hi-Z pyramid and their passes. */
  main_view_.sync();

  (void)output_rect; /* Border render not yet implemented. */
}

/* Anti-aliasing and upscaling from the scene settings. */
void GameInstance::settings_sync()
{
  const SceneEEVEE &eevee = scene->eevee;
  AASettings &aa = pipelines.aa_settings;
  aa.enabled = true;
  upscale_settings.mode = UpscaleMode::OFF;
  upscale_settings.sharpness = math::clamp(eevee.game_upscale_sharpness, 0.0f, 1.0f);
  switch (eevee.game_aa_method) {
    case SCE_EEVEE_GAME_AA_NONE:
      aa.enabled = false;
      break;
    case SCE_EEVEE_GAME_AA_SMAA:
      aa.mode = AAMode::SMAA;
      break;
    case SCE_EEVEE_GAME_AA_FSR3:
      switch (eevee.game_upscale_quality) {
        case SCE_EEVEE_GAME_UPSCALE_NATIVE:
          upscale_settings.mode = UpscaleMode::FSR3_NATIVE_AA;
          break;
        case SCE_EEVEE_GAME_UPSCALE_ULTRA_QUALITY:
          upscale_settings.mode = UpscaleMode::FSR3_ULTRA_QUALITY;
          break;
        case SCE_EEVEE_GAME_UPSCALE_BALANCED:
          upscale_settings.mode = UpscaleMode::FSR3_BALANCED;
          break;
        case SCE_EEVEE_GAME_UPSCALE_PERFORMANCE:
          upscale_settings.mode = UpscaleMode::FSR3_PERFORMANCE;
          break;
        default:
          upscale_settings.mode = UpscaleMode::FSR3_QUALITY;
          break;
      }
      break;
    case SCE_EEVEE_GAME_AA_FXAA:
    default:
      aa.mode = AAMode::FXAA;
      break;
  }
}

/* ================================================================
 * begin_sync
 * ================================================================ */

void GameInstance::begin_sync()
{
  update_eval_members();

  /* Advance frame state. history_valid becomes true after the first complete
   * frame so temporal modules do not blend against uninitialised buffers. */
  frame_state.frame_index++;
  /* Single expression: valid only after the first complete frame AND when
   * no discontinuity occurred. camera_cut is read here by all modules
   * before being cleared at the end of this block. */
  frame_state.history_valid = (frame_state.frame_index > 1) && !frame_state.camera_cut;
  if (frame_state.camera_cut) {
    upscale.notify_camera_cut();
    frame_state.camera_cut = false;  /* cleared after all modules have read it */
  }

  film.sync();

  /* Reset per-frame module state. Materials and pipelines first: object
   * sync fills the passes they reset here. */
  materials.begin_sync();
  pipelines.begin_sync();
  lights.begin_sync();
  shadows.begin_sync();
  velocity.begin_sync();
  volume.begin_sync();
  sync.begin_sync();

  /* Camera matrices.
   *
   * Viewport: the Draw Manager default view already holds what is on screen,
   * whether that is the active camera, a free perspective view or the game
   * camera set by the engine. Using it keeps culling and shading consistent
   * with every other engine drawing into the same viewport.
   *
   * F12: there is no region, so the matrices come from the scene camera. */
  const float2 render_res = float2(film.render_extent_get());

  if (draw_ctx->rv3d != nullptr) {
    const View &default_view = View::default_get();
    uniform_data.viewmat = default_view.viewmat();
    uniform_data.viewinv = default_view.viewinv();
    uniform_data.projectionmat = default_view.winmat();
    /* View space depths of the clip planes, negative in front of the camera: distances here. */
    uniform_data.z_near = -default_view.near_clip();
    uniform_data.z_far = -default_view.far_clip();
    camera.object_to_world = uniform_data.viewinv;
  }
  else {
    if (camera_eval_object != nullptr) {
      camera.sync(camera_eval_object);
    }
    const CameraData &cam = camera.data_get();
    uniform_data.viewinv = camera.object_to_world;
    uniform_data.viewmat = math::invert(camera.object_to_world);
    uniform_data.z_near = cam.clip_near;
    uniform_data.z_far = cam.clip_far;

    /* Frustum half extents on the near plane, horizontal sensor fit:
     * tan(fov_x / 2) = sensor_width / (2 * focal_length). */
    const float aspect = render_res.x / render_res.y;
    const float right = cam.clip_near * cam.sensor_width / (2.0f * cam.focal_length);
    const float top = right / aspect;
    uniform_data.projectionmat = math::projection::perspective(
        -right, right, -top, top, cam.clip_near, cam.clip_far);
  }

  uniform_data.camera_pos = camera.position();
  /* FSR3: advance the jitter sequence NOW so the offset is known before
   * the prepass and G-Buffer are rendered. apply_fsr3() reads the value back
   * later. Non-FSR path: no jitter. */
  uniform_data.jitter = upscale.is_initialized() ? upscale.advance_jitter() : float2(0.0f);
  uniform_data.delta_time = delta_time_ms * 0.001f;
  uniform_data.frame_count = frame_state.frame_index;
  /* The scene exposure is part of the view transform Blender applies when it
   * composites the viewport: applying it here too would apply it twice. Set
   * every frame, the struct is never zero initialized. */
  uniform_data.exposure = 1.0f;

  uniform_data.viewprojmat = uniform_data.projectionmat * uniform_data.viewmat;
  uniform_data.screen_res = render_res;
  uniform_data.screen_res_inv = float2(1.0f) / render_res;
}

void GameInstance::object_sync(ObjectRef &ob_ref, Manager & /*manager*/)
{
  sync.object_sync(ob_ref);
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
   * one full SSBO DMA transfer (~MAX_LIGHTS * sizeof(GameLightData) bytes). */
  /* Routes the shadowed lights to their atlas faces and writes
   * GameLightData::shadow_index, which the upload below packs. */
  shadows.end_sync();

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
  /* Before the pipelines: they bind the world lighting maps it allocates. */
  world.sync();
  pipelines.sync();
}

/* ================================================================
 * render_sample / render_frame
 * ================================================================ */

void GameInstance::render_sample()
{
  main_view_.render();
}

void GameInstance::draw(Manager & /*manager*/)
{
  render_sample();
}

void GameInstance::render_sync()
{
  manager->begin_sync();

  begin_sync();
  DRW_render_object_iter(
      render_engine_, depsgraph, [&](ObjectRef &ob_ref, RenderEngine *, Depsgraph *) {
        sync.object_sync(ob_ref);
      });
  end_sync();

  manager->end_sync();
}

void GameInstance::render_frame(RenderEngine *engine, RenderLayer *layer)
{
  render_engine_ = engine;

  render_sync();
  render_sample();

  RenderPass *rp = RE_pass_find_by_name(layer, RE_PASSNAME_COMBINED, "");
  if (rp == nullptr) {
    return;
  }

  /* Read back from the final post-processed texture, not from a viewport
   * framebuffer: background renders have no viewport at all.
   * display_res_tx_ (FSR3) and aa_out_tx_ (SMAA/FXAA) are both RGBA16F at
   * display resolution, which is what the Combined pass expects.
   *
   * The read flushes the pipeline and stalls; fine for a one-shot F12.
   * The returned buffer is handed to the pass as is, no copy. */
  float *pixels = static_cast<float *>(
      GPU_texture_read(main_view_.get_final_texture(), GPU_DATA_FLOAT, 0));
  RE_pass_set_buffer_data(rp, pixels);
}

/* ================================================================
 * update_eval_members (private)
 * ================================================================ */

void GameInstance::update_eval_members()
{
  draw_ctx = DRW_context_get();
  manager = DRW_manager_get();
  depsgraph = draw_ctx->depsgraph;
  scene = DEG_get_evaluated_scene(depsgraph);
  view_layer = DEG_get_evaluated_view_layer(depsgraph);

  camera_eval_object = (scene->camera != nullptr) ? DEG_get_evaluated(depsgraph, scene->camera) :
                                                    nullptr;
}

/* ================================================================
 * Camera
 * ================================================================ */

void Camera::sync(const Object *cam_ob)
{
  object_to_world = cam_ob->object_to_world();

  /* Any object can be the scene camera; only real cameras carry a lens. */
  if (cam_ob->type == OB_CAMERA) {
    const blender::Camera &cam = DRW_object_get_data_for_drawing<const blender::Camera>(*cam_ob);
    data_.clip_near = cam.clip_start;
    data_.clip_far = cam.clip_end;
    data_.focal_length = cam.lens;
    data_.sensor_width = cam.sensor_x;
  }
}

/* ================================================================
 * HiZBufferPair::ensure
 * ================================================================ */

void HiZBufferPair::ensure(int2 render_res)
{
  /* A minimised window reports a zero extent. */
  render_res = math::max(render_res, int2(1));
  const int2 hiz_extent = math::ceil_to_multiple(render_res, int2(1 << (HIZ_MIP_COUNT - 1)));

  const eGPUTextureUsage usage = GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_SHADER_WRITE;
  if (tx_.ensure_2d(gpu::TextureFormat::SFLOAT_32, hiz_extent, usage, nullptr, HIZ_MIP_COUNT)) {
    /* Fresh storage holds no depth. Occlusion culling stays off until the
     * next update, otherwise a resize would cull against garbage. */
    valid = false;
    tile_counter_.clear_to_zero();
  }
  tx_.ensure_mip_views();
  GPU_texture_mipmap_mode(tx_, true, false);
  for (const int i : IndexRange(HIZ_MIP_COUNT)) {
    mip_ref_[i] = tx_.mip_view(i);
  }

  front.ref_tx_ = tx_;
  uv_scale = float2(render_res) / float2(hiz_extent);
}

/* ================================================================
 * LightModule
 * ================================================================ */

void LightModule::begin_sync()
{
  entries_.clear();
  active_light_count = 0;
}

void LightModule::add(const LightEntry &entry)
{
  entries_.append(entry);
}

void LightModule::upload()
{
  /* Pack the map into the flat array the shaders index. Runs once per frame,
   * after GameInstance::end_sync() stamped shadow_index into every entry. */
  active_light_count = 0;
  for (const LightEntry &entry : entries_) {
    if (active_light_count >= MAX_LIGHTS) {
      break;
    }
    light_buf_[active_light_count++] = entry.data;
  }
  light_buf_.push_update();
}

void LightModule::bind_resources(PassSimple &ps)
{
  ps.bind_ssbo("light_buf", &light_buf_);
}

} // namespace blender::eevee_game
