/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "eevee_game_culling.hh"
#include "eevee_game_instance.hh"

#include "BKE_object.hh"       /* BKE_object_boundbox_get() */
#include "DNA_object_types.h"  /* Object */

#include "BLI_assert.h"
#include "BLI_log.h"           /* BLI_log_warn */

namespace blender::eevee_game {

CullingModule::CullingModule(GameInstance &inst) : inst_(&inst) {}

void CullingModule::init()
{
  /* Pre-allocate SSBOs at max capacity to avoid per-frame realloc stalls.
   * 64k instances @ 128 bytes each = 8 MB for instance_data_sb_.
   * GPU_USAGE_DYNAMIC allows CPU updates via StorageBuffer::update(). */
  instance_data_sb_ = std::make_unique<gpu::StorageBuffer>(
      MAX_GPU_INSTANCES * sizeof(GPUInstanceData), GPU_USAGE_DYNAMIC);

  /* Indices are 4 bytes each; DEVICE_ONLY since the GPU writes and reads them */
  visible_indices_sb_ = std::make_unique<gpu::StorageBuffer>(
      MAX_GPU_INSTANCES * sizeof(uint32_t), GPU_USAGE_DEVICE_ONLY);

  /* One DrawCommand per unique mesh/material bucket - 1000 is a safe upper bound */
  indirect_draw_sb_ = std::make_unique<gpu::StorageBuffer>(
      1000 * sizeof(DrawCommand), GPU_USAGE_DYNAMIC);

  num_draw_buckets_ = 1000;

  /* Zero-initialise all instanceCount fields on first allocation.
   * GPU_USAGE_DEVICE_ONLY buffers would leave OS-provided uninitialised
   * memory; GPU_USAGE_DYNAMIC lets us zero via the staging path here. */
  const Vector<DrawCommand> zero_cmds(num_draw_buckets_, DrawCommand{});
  indirect_draw_sb_->update(zero_cmds.data());

  /* Single uint32 atomic counter used by the culling shader to count visible instances.
   * Must be reset to 0 at the start of each dispatch (done in execute_culling). */
  visible_count_sb_ = std::make_unique<gpu::StorageBuffer>(
      sizeof(uint32_t), GPU_USAGE_DYNAMIC);
}

void CullingModule::begin_sync()
{
  cpu_instance_cache_.clear();

  /* Reset instanceCount to 0 for every DrawCommand bucket.
   * The culling shader (indirect_draw_buf = READ-only) never writes this
   * field. Without this reset, mesh buckets with zero visible instances
   * this frame inherit the previous frame's non-zero count and submit
   * phantom indirect draws — geometry drawn at wrong transforms with
   * undefined instance data. StorageBuffer::update() on GPU_USAGE_DYNAMIC
   * queues a staging transfer; it does not stall the CPU against the GPU
   * provided begin_sync() runs before execute_culling(). */
  const Vector<DrawCommand> zero_cmds(num_draw_buckets_, DrawCommand{});
  indirect_draw_sb_->update(zero_cmds.data());
}

void CullingModule::add_instance(Object *ob, uint32_t resource_id)
{
  GPUInstanceData data;

  /* object_to_world() on the evaluated Object gives the world matrix from the Depsgraph */
  data.model_matrix = ob->object_to_world();

  /* BKE_object_boundbox_get() returns the evaluated AABB.
   * DO NOT cast ob->data to BoundBox* - ob->data is the Mesh/Curve/etc. data block. */
  const BoundBox *bb = BKE_object_boundbox_get(ob);
  if (bb != nullptr) {
    /* vec[0] = min corner (---), vec[6] = max corner (+++) in the BoundBox layout */
    data.bb_min = float3(bb->vec[0]);
    data.bb_max = float3(bb->vec[6]);
  }
  else {
    /* Fallback unit cube; the culling shader will conservatively keep the object */
    data.bb_min = float3(-0.5f);
    data.bb_max = float3(0.5f);
  }

  data.resource_id = resource_id;
  data.flags       = 0;

  /* Guard against exceeding the pre-allocated SSBO capacity.
   * In debug builds, warn loudly so the limit can be raised. In release builds,
   * silently discard excess objects — they become invisible but the engine survives. */
  if (cpu_instance_cache_.size() >= MAX_GPU_INSTANCES) {
    BLI_assert_msg(0, "eevee_game: MAX_GPU_INSTANCES exceeded — raise the limit in eevee_game_defines.hh");
    return;
  }

  cpu_instance_cache_.push_back(data);
}

void CullingModule::execute_culling(View &view)
{
  if (cpu_instance_cache_.empty()) {
    return;
  }

  /* One DMA upload of the entire instance list; avoids per-object GPU calls */
  instance_data_sb_->update(cpu_instance_cache_.data());

  culling_ps_.init();
  culling_ps_.shader_set(inst_->shaders.static_shader_get(SH_CULLING_COMPUTE));

  culling_ps_.bind_ssbo("instance_data_buf",    instance_data_sb_.get());
  culling_ps_.bind_ssbo("visible_indices_buf",   visible_indices_sb_.get());
  culling_ps_.bind_ssbo("indirect_draw_buf",     indirect_draw_sb_.get());
  culling_ps_.bind_ssbo("visible_count_buf",     visible_count_sb_.get());

  /* Hi-Z texture for occlusion culling (built from the prepass depth) */
  culling_ps_.bind_texture("hiz_tx", &inst_->hiz_buffer.front.ref_tx_);

  /* Pass instance count as a push constant so the shader can guard the invocation index */
  const uint32_t num_instances = uint32_t(cpu_instance_cache_.size());
  culling_ps_.push_constant("instance_count", int(num_instances));

  /* Pass the view-projection matrix extracted from the View.
   * Extracting from View (not from uniform_data) ensures the culling matrix
   * matches the exact frustum that will be used for drawing this view,
   * including any jitter applied for FSR temporal accumulation. */
  culling_ps_.push_constant("viewproj", view.persmat());

  /* Reset the visible instance counter before dispatch.
   * 4-byte DMA via staging is cheaper than a 1-thread compute dispatch
   * for a single uint; the transfer is ordered before the compute by the
   * DRW command list so the shader sees zero on the first atomicAdd. */
  constexpr uint32_t zero = 0u;
  visible_count_sb_->update(&zero);

  /* Each workgroup processes 64 instances (local_size_x = 64 in the GLSL). */
  culling_ps_.dispatch(int3(math::divide_ceil(num_instances, 64u), 1u, 1u));

  /* Ensure the SSBO writes from the culling shader are visible before
   * the indirect draw command reads the instanceCount fields. */
  culling_ps_.barrier(GPU_BARRIER_SHADER_STORAGE | GPU_BARRIER_COMMAND_BUFFER);
}

} // namespace blender::eevee_game
