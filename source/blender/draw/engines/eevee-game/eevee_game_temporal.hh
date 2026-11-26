/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * HWTAA: temporal anti-aliasing and upscaling with a window of exact samples.
 *
 * The store pass writes every frame's render samples (color, 3x3 median luma, stable object
 * key, inverse depth, flags) into one layer of a ring of N layers. The resolve reconstructs each
 * output pixel: still pixels blend a recursive history (identity validation, variance clip,
 * locks on thin features), moving pixels gather the samples of the last N - 1 frames from the
 * ring, each moved exactly to this frame. See the HWTAA paper for the method.
 *
 * Host side, per frame:
 * - begin_sync(): jitter of this frame (Halton 2, 3);
 * - object_sync(): stable key, motion and deformation of each mesh, by draw manager resource;
 * - end_sync(): motion table and motion chains (object motion over the window);
 * - transparency_begin() / transparency_end(): reactive mask around the transparent surfaces;
 * - resolve(): window matrices, store and resolve passes.
 */

#include "BLI_map.hh"

#include "DRW_gpu_wrapper.hh"
#include "draw_handle.hh"
#include "draw_pass.hh"

#include "eevee_game_temporal_shared.hh"

struct Object;

namespace blender::eevee_game {

class GameInstance;

class TemporalModule {
 public:
  TemporalModule(GameInstance &inst) : inst_(inst) {}

  /* Settings of this frame. quality: #eEEVEEGameUpscaleQuality. */
  void configure(bool enabled, int quality);

  bool enabled() const
  {
    return enabled_;
  }

  /* Render resolution for an output resolution. */
  int2 render_res_get(int2 display_res) const;

  /* (Re)creates the resources when the resolutions or the window length changed. */
  void init(int2 render_res, int2 display_res);

  /* Jitter of this frame, NDC offset. Before the views are synced. */
  float2 advance_jitter();

  void begin_sync();
  void object_sync(Object *ob, const ObjectKey &key, ResourceHandleRange handle, bool deformed);
  void end_sync();

  /* Attached to the prepass as the object id output of the depth shaders. */
  gpu::Texture *object_id_tx()
  {
    return object_id_tx_;
  }

  /* Reactive mask: the opaque color before the transparent surfaces, then the mask after. */
  void transparency_begin(gpu::Texture *combined_tx);
  void transparency_end(gpu::Texture *combined_tx);

  /* Store and resolve. Returns the output, at display resolution. */
  gpu::Texture *resolve(gpu::Texture *color_tx);

 private:
  struct FrameRecord {
    float4x4 viewproj = float4x4::identity();
    float4x4 viewprojinv = float4x4::identity();
    float4 depth_params = float4(0.0f);
    float2 jitter = float2(0.0f);
    int layer = 0;
  };

  struct TrackedObject {
    /* Object to world matrix of each of the last frames, by frame index modulo the window. */
    float4x4 matrices[TEMPORAL_WINDOW_MAX];
    uint64_t last_moved = 0;
    uint64_t last_seen = 0;
    uint key = 0;
  };

  struct SyncedResource {
    uint resource_id;
    /* Not a pointer: the map moves its values when it grows during the sync. */
    ObjectKey key;
    bool deformed;
  };

  GameInstance &inst_;

  bool enabled_ = false;
  int quality_ = -1;
  float scale_ = 1.0f;
  /* Ring layers = frames of the window. */
  int ring_len_ = 8;
  int phases_ = 8;
  int move_phases_ = 8;
  float max_confidence_ = 8.0f;
  bool window_volatile_ = false;

  int2 render_res_ = int2(0);
  int2 display_res_ = int2(0);
  bool resources_valid_ = false;

  /* Frame counter of the module, the index of every per frame record. */
  uint64_t frame_ = 0;
  int ring_head_ = 0;
  int window_frames_ = 1;
  float2 jitter_px_ = float2(0.0f);
  int phase_index_ = 0;
  /* Last resolves: the camera or an object moved (one frame late, as the GPU sees it). */
  bool camera_moved_ = true;
  bool objects_moved_ = true;
  bool idle_ = false;
  int current_ = 0;
  FrameRecord frames_[TEMPORAL_WINDOW_MAX];

  Map<ObjectKey, TrackedObject> objects_;
  Vector<SyncedResource> synced_;
  int chain_slots_ = 0;

  Texture ring_tx_ = {"hwtaa_ring"};
  Texture history_tx_[2] = {{"hwtaa_history_a"}, {"hwtaa_history_b"}};
  Texture lock_tx_[2] = {{"hwtaa_lock_a"}, {"hwtaa_lock_b"}};
  Texture object_id_tx_ = {"hwtaa_object_id"};
  Texture opaque_tx_ = {"hwtaa_opaque"};
  Texture reactive_tx_ = {"hwtaa_reactive"};
  Texture output_tx_ = {"hwtaa_output"};

  StorageVectorBuffer<uint, 128> motion_buf_ = {"hwtaa_motion"};
  StorageVectorBuffer<float4, 64> chain_buf_ = {"hwtaa_chain"};
  UniformBuffer<TemporalWindowData> window_buf_;

  PassSimple reactive_ps_ = {"HWTAA.Reactive"};
  PassSimple store_ps_ = {"HWTAA.Store"};
  PassSimple resolve_ps_ = {"HWTAA.Resolve"};
  bool reactive_written_ = false;

  FrameRecord &frame_record(uint64_t frame)
  {
    return frames_[frame % TEMPORAL_WINDOW_MAX];
  }
};

}  // namespace blender::eevee_game
