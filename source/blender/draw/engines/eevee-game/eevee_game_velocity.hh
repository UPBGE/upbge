/* SPDX-FileCopyrightText: 2021 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_map.hh"
#include "DRW_gpu_wrapper.hh"
#include "DRW_render.hh"
#include "draw_handle.hh"

/* eevee_velocity_shared.hh defines eVelocityStep, VelocityIndex, VelocityObjectIndex, etc.
 * We reuse these shared structs directly from the EEVEE module because they match the
 * GLSL shader layout exactly and we would use the same shaders. */
#include "../eevee/eevee_velocity_shared.hh"
#include "../eevee/eevee_defines.hh"

namespace blender::eevee_game {

using namespace draw;

/* Reuse the EEVEE ObjectKey type for stable object identity tracking */
using ObjectKey = blender::draw::ObjectKey;
using eevee::eVelocityStep;
using eevee::VelocityIndex;

/* GPU buffer types - same layout as EEVEE velocity buffers */
using VelocityObjectBuf   = draw::StorageArrayBuffer<float4x4, 16, false>;
using VelocityGeometryBuf = draw::StorageArrayBuffer<float4, 16, true>;
using VelocityIndexBuf    = draw::StorageArrayBuffer<VelocityIndex, 16>;

/**
 * VelocityModule for eevee_game.
 *
 * Simplified from the EEVEE version: game mode is always viewport-mode,
 * so we only track STEP_PREVIOUS and STEP_CURRENT. STEP_NEXT is unused.
 *
 * The motion vectors written to vector_tx are consumed by:
 *   - FSR3 temporal reconstruction (motionVectors input)
 *   - The prepass velocity shader that writes vector_tx
 */
class VelocityModule {
 public:
  /* Per-object velocity tracking data. Persistent across frames. */
  struct VelocityObjectData : public VelocityIndex {
    /** Evaluated-data ID hash for geometry deform matching. */
    uint64_t id = 0;
  };

  /* Per-geometry tracking data for deforming objects. */
  struct VelocityGeometryData {
    /* The batch is requested during sync but only extracted at the end of
     * it, so the position buffer must not be dereferenced before then. */
    gpu::Batch *batch = nullptr;
    int ofs = 0;
    int len = 0;

    gpu::VertBuf *pos_buf_get() const
    {
      /* Surface batches keep positions in the second vertex buffer slot. */
      return (batch != nullptr) ? batch->verts_(1) : nullptr;
    }
  };

  /** Persistent map: ObjectKey -> velocity indices for previous/current step. */
  Map<ObjectKey, VelocityObjectData> velocity_map;
  /** Transient per-frame geometry data (deforming meshes). Cleared after geometry_steps_fill(). */
  Map<uint64_t, VelocityGeometryData> geometry_map;

  /** Object world matrices for previous and current frame. */
  std::array<VelocityObjectBuf *, 3> object_steps;
  /** Geometry (vertex positions) for deforming objects. */
  std::array<VelocityGeometryBuf *, 3> geometry_steps;
  /** Occupied slot counts per step. */
  int3 object_steps_usage = int3(0);
  /** Indirection buffer indexed by draw manager resource_id. */
  VelocityIndexBuf indirection_buf;
  /** Frame time at which each step was evaluated. */
  float3 step_time = float3(0.0f);

 private:
  class GameInstance &inst_;
  eVelocityStep step_ = eevee::STEP_CURRENT;

 public:
  VelocityModule(GameInstance &inst) : inst_(inst)
  {
    for (VelocityObjectBuf *&buf : object_steps) {
      buf = new VelocityObjectBuf();
    }
    for (VelocityGeometryBuf *&buf : geometry_steps) {
      buf = new VelocityGeometryBuf();
    }
  }

  ~VelocityModule()
  {
    for (VelocityObjectBuf *buf : object_steps)   { delete buf; }
    for (VelocityGeometryBuf *buf : geometry_steps) { delete buf; }
  }

  /** Reset per-frame counters. Called at the start of begin_sync(). */
  void begin_sync();

  /**
   * Register one object for velocity tracking.
   * Returns true if the object has motion this frame.
   * @param resource_handle  The DRW resource handle for this object (indexes indirection_buf).
   */
  bool step_object_sync(ObjectKey &object_key,
                        const ObjectRef &object_ref,
                        int recalc,
                        ResourceHandleRange resource_handle);

  /**
   * Copy geometry vertex buffers into the geometry step buffer.
   * Must be called after all batch caches are extracted (end of sync).
   */
  void geometry_steps_fill();

  /**
   * Finalise the indirection buffer and upload all steps to GPU.
   * Swaps current -> previous for the next frame.
   */
  void end_sync();

  /** Bind velocity SSBOs to a render pass (prepass, shadow, etc.). */
  template<typename PassType> void bind_resources(PassType &pass)
  {
    pass.bind_ssbo(VELOCITY_OBJ_PREV_BUF_SLOT,   &(*object_steps[eevee::STEP_PREVIOUS]));
    pass.bind_ssbo(VELOCITY_OBJ_NEXT_BUF_SLOT,   &(*object_steps[eevee::STEP_PREVIOUS])); /* next=prev in game mode */
    pass.bind_ssbo(VELOCITY_GEO_PREV_BUF_SLOT,   &(*geometry_steps[eevee::STEP_PREVIOUS]));
    pass.bind_ssbo(VELOCITY_GEO_NEXT_BUF_SLOT,   &(*geometry_steps[eevee::STEP_PREVIOUS]));
    pass.bind_ssbo(VELOCITY_INDIRECTION_BUF_SLOT, &indirection_buf);
  }

 private:
  bool object_has_velocity(const Object *ob);
  bool object_is_deform(const Object *ob);
};

} // namespace blender::eevee_game
