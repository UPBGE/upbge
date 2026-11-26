/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * HWTAA host, see eevee_game_temporal.hh.
 */

#include "BKE_object.hh"

#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "eevee_game_instance.hh"
#include "eevee_game_temporal.hh"

namespace blender::eevee_game {

/* Below this many frames the window is off and the method keeps its history. */
static constexpr int WINDOW_MIN_FRAMES = 4;
/* Window units: output pixels times sqrt(k log2(e)), so that the Gaussian weight of a sample at
 * g is exp2(-|g|^2) (WINDOW_UNIT of the resolve shader). */
static const float WINDOW_UNIT = std::sqrt(TEMPORAL_RECONSTRUCTION_SHARPNESS * 1.4426950408889634f);
/* Jitter phases of a scene at rest, so that a still image keeps converging. */
static constexpr int IDLE_PHASES = 64;
static constexpr float IDLE_MAX_CONFIDENCE = 256.0f;

static float halton(int index, int base)
{
  float f = 1.0f, result = 0.0f;
  for (int i = index; i > 0; i /= base) {
    f /= float(base);
    result += f * float(i % base);
  }
  return result;
}

/* Inverse depth parameters of a projection (inverse_depth() of the shaders): winmat[2][2],
 * winmat[3][2], perspective, and the far distance of an orthographic projection. */
static float4 depth_params_get(const float4x4 &winmat)
{
  const float a = winmat[2][2];
  const float b = winmat[3][2];
  const bool persp = winmat[2][3] != 0.0f;
  return float4(a, b, persp ? 1.0f : 0.0f, persp ? 0.0f : (b - 1.0f) / a);
}

/* Render texel (u, v), inverse depth z of a past frame to this frame's clip space, scaled to
 * window units of the output: the whole reprojection of a static sample in one matrix. */
static float4x4 window_matrix(const float4x4 &reproject,
                              const float4 &depth_params,
                              const float2 &jitter,
                              const int2 render_res,
                              const int2 display_res)
{
  float4x4 texel = float4x4::identity();
  texel[0][0] = 2.0f / float(render_res.x);
  texel[1][1] = 2.0f / float(render_res.y);
  texel[3][0] = (0.5f - jitter.x) * 2.0f / float(render_res.x) - 1.0f;
  texel[3][1] = (0.5f - jitter.y) * 2.0f / float(render_res.y) - 1.0f;
  const bool persp = depth_params.z != 0.0f;
  float4x4 depth = float4x4::identity();
  depth[2][2] = persp ? depth_params.y : -depth_params.x;
  depth[3][2] = persp ? -depth_params.x : depth_params.y;
  float4x4 out = float4x4::identity();
  const float hx = 0.5f * float(display_res.x) * WINDOW_UNIT;
  const float hy = 0.5f * float(display_res.y) * WINDOW_UNIT;
  out[0][0] = out[3][0] = hx;
  out[1][1] = out[3][1] = hy;
  return out * reproject * depth * texel;
}

void TemporalModule::configure(bool enabled, int quality)
{
  enabled_ = enabled;
  if (!enabled_) {
    if (resources_valid_) {
      ring_tx_.free();
      for (int i = 0; i < 2; i++) {
        history_tx_[i].free();
        lock_tx_[i].free();
      }
      object_id_tx_.free();
      opaque_tx_.free();
      reactive_tx_.free();
      output_tx_.free();
      objects_.clear();
      resources_valid_ = false;
    }
    return;
  }
  if (quality == quality_) {
    return;
  }
  quality_ = quality;
  /* Render scale and window length of each mode (HWTAA paper, implementation). Balanced and
   * Performance keep windows no longer than Quality's so that they never cost more. */
  switch (quality) {
    case SCE_EEVEE_GAME_UPSCALE_NATIVE:
      scale_ = 1.0f;
      ring_len_ = 8;
      break;
    case SCE_EEVEE_GAME_UPSCALE_ULTRA_QUALITY:
      scale_ = 1.0f / 1.3f;
      ring_len_ = 14;
      break;
    case SCE_EEVEE_GAME_UPSCALE_BALANCED:
      scale_ = 1.0f / 1.7f;
      ring_len_ = 14;
      break;
    case SCE_EEVEE_GAME_UPSCALE_PERFORMANCE:
      scale_ = 0.5f;
      ring_len_ = 12;
      break;
    case SCE_EEVEE_GAME_UPSCALE_QUALITY:
    default:
      scale_ = 1.0f / 1.5f;
      ring_len_ = 14;
      break;
  }
  const float ratio = 1.0f / scale_;
  phases_ = math::clamp(int(std::ceil(8.0f * ratio * ratio)), 8, 64);
  max_confidence_ = math::clamp(8.0f * ratio * ratio, 8.0f, 32.0f);
  window_volatile_ = scale_ >= 0.6f && ring_len_ >= 8;
  /* While the camera moves the jitter cycle is the window, so every window holds the same
   * sub-pixel pattern. */
  move_phases_ = (scale_ >= 0.6f && ring_len_ >= WINDOW_MIN_FRAMES) ?
                     math::min(ring_len_, phases_) :
                     phases_;
  /* Force a reallocation of the ring. */
  render_res_ = int2(0);
}

int2 TemporalModule::render_res_get(int2 display_res) const
{
  return math::max(int2(1), int2(math::round(float2(display_res) * scale_)));
}

void TemporalModule::init(int2 render_res, int2 display_res)
{
  if (!enabled_ || (resources_valid_ && render_res == render_res_ && display_res == display_res_ &&
                    ring_tx_.depth() == ring_len_))
  {
    return;
  }
  render_res_ = render_res;
  display_res_ = display_res;

  const eGPUTextureUsage rw = GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_SHADER_WRITE;
  ring_tx_.ensure_2d_array(gpu::TextureFormat::UINT_32_32, render_res, ring_len_, rw);
  ring_tx_.clear(uint4(0u));
  for (int i = 0; i < 2; i++) {
    history_tx_[i].ensure_2d(gpu::TextureFormat::SFLOAT_16_16_16_16, display_res, rw);
    history_tx_[i].clear(float4(0.0f));
    lock_tx_[i].ensure_2d(gpu::TextureFormat::UNORM_8_8, display_res, rw);
    lock_tx_[i].clear(float4(0.0f));
  }
  object_id_tx_.ensure_2d(gpu::TextureFormat::UINT_16,
                          render_res,
                          GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_ATTACHMENT);
  opaque_tx_.ensure_2d(gpu::TextureFormat::UNORM_8_8_8_8, render_res, rw);
  reactive_tx_.ensure_2d(gpu::TextureFormat::UNORM_8, render_res, rw);
  reactive_tx_.clear(float4(0.0f));
  output_tx_.ensure_2d(gpu::TextureFormat::SFLOAT_16_16_16_16,
                       display_res,
                       rw | GPU_TEXTURE_USAGE_ATTACHMENT);

  ring_head_ = 0;
  window_frames_ = 1;
  current_ = 0;
  resources_valid_ = false;
}

float2 TemporalModule::advance_jitter()
{
  if (!enabled_) {
    return float2(0.0f);
  }
  frame_++;
  idle_ = !camera_moved_ && !objects_moved_;
  const int phases = idle_ ? IDLE_PHASES : (camera_moved_ ? move_phases_ : phases_);
  const int index = int(frame_ % uint64_t(phases)) + 1;
  jitter_px_ = float2(halton(index, 2), halton(index, 3)) - 0.5f;
  phase_index_ = phases;
  return 2.0f * jitter_px_ / float2(render_res_);
}

void TemporalModule::begin_sync()
{
  synced_.clear();
}

void TemporalModule::object_sync(Object *ob,
                                 const ObjectKey &key,
                                 ResourceHandleRange handle,
                                 bool deformed)
{
  const float4x4 &matrix = ob->object_to_world();
  bool added = false;
  TrackedObject &object = objects_.lookup_or_add_cb(key, [&]() {
    added = true;
    TrackedObject o;
    for (float4x4 &m : o.matrices) {
      m = matrix;
    }
    /* 0 is the background in the ring. */
    o.key = uint(key.hash() ^ (key.hash() >> 16) ^ (key.hash() >> 32)) & TEMPORAL_KEY_MASK;
    o.key = (o.key != 0u) ? o.key : 1u;
    return o;
  });
  /* Instances share their object's matrices only through the key: each one is its own. */
  if (!added && object.last_seen != frame_) {
    /* Not synced for a few frames (culled before the sync): held where it was last seen. */
    const float4x4 last = object.matrices[object.last_seen % TEMPORAL_WINDOW_MAX];
    for (uint64_t f = object.last_seen + 1; f < frame_ && f + TEMPORAL_WINDOW_MAX > frame_; f++) {
      object.matrices[f % TEMPORAL_WINDOW_MAX] = last;
    }
    const float4x4 &prev = object.matrices[(frame_ - 1) % TEMPORAL_WINDOW_MAX];
    if (prev != matrix) {
      object.last_moved = frame_;
    }
  }
  object.matrices[frame_ % TEMPORAL_WINDOW_MAX] = matrix;
  object.last_seen = frame_;
  synced_.append({uint(handle.index()), key, deformed});
}

void TemporalModule::end_sync()
{
  if (!enabled_) {
    return;
  }
  uint resource_len = 1;
  for (const SyncedResource &res : synced_) {
    resource_len = math::max(resource_len, res.resource_id + 1);
  }
  motion_buf_.clear();
  for (uint i = 0; i < resource_len; i++) {
    motion_buf_.append(0u);
  }

  const int ages = math::max(ring_len_ - 1, 1);
  chain_buf_.clear();
  chain_slots_ = 0;
  objects_moved_ = false;
  for (const SyncedResource &res : synced_) {
    TrackedObject &object = objects_.lookup(res.key);
    uint entry = object.key;
    if (res.deformed) {
      entry |= TEMPORAL_DEFORM_BIT;
      objects_moved_ = true;
    }
    else if (object.last_moved != 0 && frame_ - object.last_moved < uint64_t(ring_len_)) {
      /* Moved inside the window: its samples of the past frames were elsewhere. Rows of
       * object_to_world[t - age] * inverse(object_to_world[t]) for every age. */
      objects_moved_ = objects_moved_ || object.last_moved == frame_;
      chain_slots_++;
      const float4x4 inv = math::invert(object.matrices[frame_ % TEMPORAL_WINDOW_MAX]);
      for (int age = 1; age <= ages; age++) {
        const float4x4 m = object.matrices[(frame_ - uint64_t(age)) % TEMPORAL_WINDOW_MAX] * inv;
        for (int row = 0; row < 3; row++) {
          chain_buf_.append(float4(m[0][row], m[1][row], m[2][row], m[3][row]));
        }
      }
      entry |= (uint(chain_slots_) << TEMPORAL_SLOT_SHIFT) | TEMPORAL_MOVED_BIT;
    }
    motion_buf_[res.resource_id] = entry;
  }
  if (chain_buf_.size() == 0) {
    chain_buf_.append(float4(0.0f));
  }
  motion_buf_.push_update();
  chain_buf_.push_update();

  /* Forget the objects that left the scene. */
  objects_.remove_if([&](const auto &item) {
    return frame_ - item.value.last_seen > uint64_t(TEMPORAL_WINDOW_MAX);
  });
}

void TemporalModule::transparency_begin(gpu::Texture *combined_tx)
{
  reactive_written_ = false;
  if (!enabled_) {
    return;
  }
  reactive_ps_.init();
  reactive_ps_.shader_set(inst_.shaders.static_shader_get(SH_TEMPORAL_REACTIVE));
  reactive_ps_.bind_texture("color_tx", combined_tx);
  reactive_ps_.bind_image("opaque_img", opaque_tx_);
  reactive_ps_.bind_image("reactive_img", reactive_tx_);
  reactive_ps_.push_constant("render_size", render_res_);
  reactive_ps_.push_constant("capture", true);
  reactive_ps_.dispatch(int3(math::divide_ceil(render_res_, int2(TEMPORAL_GROUP_SIZE)), 1));
  reactive_ps_.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS);
  inst_.manager->submit(reactive_ps_);
}

void TemporalModule::transparency_end(gpu::Texture *combined_tx)
{
  if (!enabled_) {
    return;
  }
  reactive_ps_.init();
  reactive_ps_.shader_set(inst_.shaders.static_shader_get(SH_TEMPORAL_REACTIVE));
  reactive_ps_.bind_texture("color_tx", combined_tx);
  reactive_ps_.bind_image("opaque_img", opaque_tx_);
  reactive_ps_.bind_image("reactive_img", reactive_tx_);
  reactive_ps_.push_constant("render_size", render_res_);
  reactive_ps_.push_constant("capture", false);
  reactive_ps_.dispatch(int3(math::divide_ceil(render_res_, int2(TEMPORAL_GROUP_SIZE)), 1));
  reactive_ps_.barrier(GPU_BARRIER_TEXTURE_FETCH);
  inst_.manager->submit(reactive_ps_);
  reactive_written_ = true;
}

gpu::Texture *TemporalModule::resolve(gpu::Texture *color_tx)
{
  if (!reactive_written_) {
    /* No transparency pass this frame: nothing is reactive. */
    reactive_tx_.clear(float4(0.0f));
  }
  reactive_written_ = false;

  const UniformData &ud = inst_.uniform_data;
  const bool history_valid = resources_valid_ && inst_.frame_state.history_valid;

  /* This frame's record. */
  ring_head_ = (ring_head_ + 1) % ring_len_;
  window_frames_ = history_valid ? math::min(window_frames_ + 1, ring_len_) : 1;
  FrameRecord &rec = frame_record(frame_);
  rec.viewproj = ud.viewprojmat;
  rec.viewprojinv = math::invert(ud.viewprojmat);
  rec.depth_params = depth_params_get(ud.projectionmat);
  rec.jitter = jitter_px_;
  rec.layer = ring_head_;
  const FrameRecord &prev_rec = (window_frames_ > 1) ? frame_record(frame_ - 1) : rec;
  camera_moved_ = !history_valid || prev_rec.viewproj != rec.viewproj;

  /* Window: one record per past frame, the current one standing for the missing ones. */
  TemporalWindowData &window = window_buf_;
  for (int age = 0; age < TEMPORAL_WINDOW_MAX; age++) {
    if (age >= math::max(window_frames_, 2)) {
      break;
    }
    const FrameRecord &r = (age < window_frames_) ? frame_record(frame_ - uint64_t(age)) : rec;
    const float4x4 reproject = window_matrix(
        rec.viewproj * r.viewprojinv, r.depth_params, r.jitter, render_res_, display_res_);
    float4 *rows = &window.rows[age * TEMPORAL_AGE_STRIDE];
    for (int c = 0; c < 4; c++) {
      rows[TEMPORAL_AGE_REPROJECT + c] = reproject[c];
      rows[TEMPORAL_AGE_VIEWPROJ + c] = r.viewproj[c];
    }
    rows[TEMPORAL_AGE_DEPTH_PARAMS] = r.depth_params;
    rows[TEMPORAL_AGE_JITTER] = float4(r.jitter.x, r.jitter.y, float(r.layer), 0.0f);
  }
  window_buf_.push_update();

  RenderBuffers &rbufs = inst_.render_buffers;
  const GPUSamplerState nearest = GPUSamplerState::default_sampler();
  const GPUSamplerState linear = {GPU_SAMPLER_FILTERING_LINEAR};
  const int motion_len = int(motion_buf_.size());

  /* Store. */
  store_ps_.init();
  store_ps_.shader_set(inst_.shaders.static_shader_get(SH_TEMPORAL_STORE));
  store_ps_.bind_texture("color_tx", color_tx, nearest);
  store_ps_.bind_texture("depth_tx", rbufs.depth_tx, nearest);
  store_ps_.bind_texture("object_id_tx", object_id_tx_, nearest);
  store_ps_.bind_texture("reactive_tx", reactive_tx_, nearest);
  store_ps_.bind_ssbo("motion_buf", motion_buf_);
  store_ps_.bind_image("ring_img", ring_tx_);
  store_ps_.push_constant("depth_params", rec.depth_params);
  store_ps_.push_constant("render_size", render_res_);
  store_ps_.push_constant("motion_len", motion_len);
  store_ps_.push_constant("ring_layer", ring_head_);
  store_ps_.dispatch(int3(math::divide_ceil(render_res_, int2(TEMPORAL_GROUP_SIZE)), 1));
  store_ps_.barrier(GPU_BARRIER_TEXTURE_FETCH);
  inst_.manager->submit(store_ps_);

  /* Resolve. */
  const int prev = 1 - current_;
  const float2 scale = float2(render_res_) / float2(display_res_);
  const float scale_max = math::max(scale.x, scale.y);
  gpu::Shader *sh = inst_.shaders.static_shader_get(SH_TEMPORAL_RESOLVE);
  resolve_ps_.init();
  resolve_ps_.specialize_constant(sh, "window_ortho", rec.depth_params.z == 0.0f);
  resolve_ps_.shader_set(sh);
  resolve_ps_.bind_texture("ring_tx", ring_tx_, nearest);
  resolve_ps_.bind_texture("depth_tx", rbufs.depth_tx, nearest);
  resolve_ps_.bind_texture("object_id_tx", object_id_tx_, nearest);
  resolve_ps_.bind_texture("vector_tx", rbufs.vector_tx, nearest);
  resolve_ps_.bind_texture("history_tx", history_tx_[prev], linear);
  resolve_ps_.bind_texture("reactive_tx", reactive_tx_, linear);
  resolve_ps_.bind_texture("lock_tx", lock_tx_[prev], nearest);
  resolve_ps_.bind_texture("color_tx", color_tx, nearest);
  resolve_ps_.bind_ssbo("motion_buf", motion_buf_);
  resolve_ps_.bind_ssbo("chain_buf", chain_buf_);
  resolve_ps_.bind_ubo("window_data", window_buf_);
  resolve_ps_.bind_image("history_out_img", history_tx_[current_]);
  resolve_ps_.bind_image("lock_out_img", lock_tx_[current_]);
  resolve_ps_.bind_image("out_color_img", output_tx_);
  resolve_ps_.push_constant("curr_viewprojinv", rec.viewprojinv);
  resolve_ps_.push_constant("prev_reproject", prev_rec.viewproj * rec.viewprojinv);
  resolve_ps_.push_constant("curr_depth_params", rec.depth_params);
  resolve_ps_.push_constant("render_scale", scale);
  resolve_ps_.push_constant("jitter_px", jitter_px_);
  resolve_ps_.push_constant("render_size", render_res_);
  resolve_ps_.push_constant("display_size", display_res_);
  resolve_ps_.push_constant("tile_dim", int(std::ceil(7.0f * scale_max)) + 3);
  resolve_ps_.push_constant("motion_len", motion_len);
  resolve_ps_.push_constant("ring_layer", ring_head_);
  resolve_ps_.push_constant("window_frames",
                            (ring_len_ >= WINDOW_MIN_FRAMES) ? window_frames_ : 1);
  resolve_ps_.push_constant("chain_stride", math::max(ring_len_ - 1, 1) * 3);
  resolve_ps_.push_constant("max_confidence", idle_ ? IDLE_MAX_CONFIDENCE : max_confidence_);
  resolve_ps_.push_constant("lock_decay", 1.0f / float(phase_index_));
  resolve_ps_.push_constant("history_valid", history_valid);
  resolve_ps_.push_constant("window_volatile", window_volatile_);
  resolve_ps_.push_constant("frequency_split", idle_ && scale_max <= 0.6f);
  resolve_ps_.dispatch(int3(math::divide_ceil(display_res_, int2(TEMPORAL_GROUP_SIZE)), 1));
  resolve_ps_.barrier(GPU_BARRIER_TEXTURE_FETCH);
  inst_.manager->submit(resolve_ps_);

  current_ = prev;
  resources_valid_ = true;
  return output_tx_;
}

}  // namespace blender::eevee_game
