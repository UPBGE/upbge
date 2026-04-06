/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "eevee_game_defines.hh"  /* GPUInstanceData is defined here - not redefined below */
#include "GPU_storage_buffer.hh"

struct Object;

namespace blender::eevee_game {

/* Arguments for DrawElementsIndirect - matches the OpenGL/Vulkan spec layout exactly.
 * The Compute Shader writes instanceCount; the CPU fills count/firstIndex/etc. */
struct DrawCommand {
  uint32_t count;          /* Index count per draw */
  uint32_t instanceCount;  /* Filled by the culling Compute Shader */
  uint32_t firstIndex;
  uint32_t baseVertex;
  uint32_t baseInstance;
};

class CullingModule {
 public:
  CullingModule(class GameInstance &inst);
  ~CullingModule() = default; /* unique_ptr members handle GPU resource cleanup */

  void init();

  /* Clear the CPU instance list at the start of each frame */
  void begin_sync();

  /* Append one object to the pending instance list (CPU side) */
  void add_instance(Object *ob, uint32_t resource_id);

  /* Upload instance data and dispatch the GPU culling compute shader */
  void execute_culling(View &view);

  gpu::StorageBuffer *get_instance_buffer()   { return instance_data_sb_.get(); }
  gpu::StorageBuffer *get_visible_idx_buffer() { return visible_indices_sb_.get(); }

 private:
  GameInstance *inst_;

  /* Source: all object transforms/AABBs uploaded to the GPU each frame */
  std::unique_ptr<gpu::StorageBuffer> instance_data_sb_;
  /* Output: resource_ids of objects that passed frustum + Hi-Z culling */
  std::unique_ptr<gpu::StorageBuffer> visible_indices_sb_;
  /* Output: DrawElementsIndirect argument buffer (instanceCount written by GPU) */
  std::unique_ptr<gpu::StorageBuffer> indirect_draw_sb_;
  /* Single uint32 atomic counter — reset to 0 before each dispatch,
   * incremented by the culling shader for each surviving instance. */
  std::unique_ptr<gpu::StorageBuffer> visible_count_sb_;

  /* CPU staging list; cleared each frame, uploaded in one shot before dispatch */
  std::vector<GPUInstanceData> cpu_instance_cache_;

  PassSimple culling_ps_{"Culling.Execute"};
};

} // namespace blender::eevee_game
