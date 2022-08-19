/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "BKE_global.h"

#include "BLI_string.h"

#include "gpu_backend.hh"
#include "gpu_context_private.hh"

#include "mtl_backend.hh"
#include "mtl_context.hh"
#include "mtl_debug.hh"
#include "mtl_uniform_buffer.hh"

namespace blender::gpu {

MTLUniformBuf::MTLUniformBuf(size_t size, const char *name) : UniformBuf(size, name)
{
}

MTLUniformBuf::~MTLUniformBuf()
{
  if (metal_buffer_ != nullptr) {
    metal_buffer_->free();
    metal_buffer_ = nullptr;
  }
  has_data_ = false;

  /* Ensure UBO is not bound to active CTX.
   * UBO bindings are reset upon Context-switch so we do not need
   * to check deactivated context's. */
  MTLContext *ctx = MTLContext::get();
  if (ctx) {
    for (int i = 0; i < MTL_MAX_UNIFORM_BUFFER_BINDINGS; i++) {
      MTLUniformBufferBinding &slot = ctx->pipeline_state.ubo_bindings[i];
      if (slot.bound && slot.ubo == this) {
        slot.bound = false;
        slot.ubo = nullptr;
      }
    }
  }
}

void MTLUniformBuf::update(const void *data)
{
  BLI_assert(this);
  BLI_assert(size_in_bytes_ > 0);

  /* Free existing allocation.
   * The previous UBO resource will be tracked by the memory manager,
   * in case dependent GPU work is still executing. */
  if (metal_buffer_ != nullptr) {
    metal_buffer_->free();
    metal_buffer_ = nullptr;
  }

  /* Allocate MTL buffer */
  MTLContext *ctx = static_cast<MTLContext *>(unwrap(GPU_context_active_get()));
  BLI_assert(ctx);
  BLI_assert(ctx->device);
  UNUSED_VARS_NDEBUG(ctx);

  if (data != nullptr) {
    metal_buffer_ = MTLContext::get_global_memory_manager().allocate_with_data(
        size_in_bytes_, true, data);
    has_data_ = true;

    metal_buffer_->set_label(@"Uniform Buffer");
    BLI_assert(metal_buffer_ != nullptr);
    BLI_assert(metal_buffer_->get_metal_buffer() != nil);
  }
  else {
    /* If data is not yet present, no buffer will be allocated and MTLContext will use an empty
     * null buffer, containing zeroes, if the UBO is bound. */
    metal_buffer_ = nullptr;
    has_data_ = false;
  }
}

void MTLUniformBuf::bind(int slot)
{
  if (slot < 0) {
    MTL_LOG_WARNING("Failed to bind UBO %p. uniform location %d invalid.\n", this, slot);
    return;
  }

  BLI_assert(slot < MTL_MAX_UNIFORM_BUFFER_BINDINGS);

  /* Bind current UBO to active context. */
  MTLContext *ctx = MTLContext::get();
  BLI_assert(ctx);

  MTLUniformBufferBinding &ctx_ubo_bind_slot = ctx->pipeline_state.ubo_bindings[slot];
  ctx_ubo_bind_slot.ubo = this;
  ctx_ubo_bind_slot.bound = true;

  bind_slot_ = slot;
  bound_ctx_ = ctx;

  /* Check if we have any deferred data to upload. */
  if (data_ != nullptr) {
    this->update(data_);
    MEM_SAFE_FREE(data_);
  }

  /* Ensure there is at least an empty dummy buffer. */
  if (metal_buffer_ == nullptr) {
    this->update(nullptr);
  }
}

void MTLUniformBuf::unbind()
{
  /* Unbind in debug mode to validate missing binds.
   * Otherwise, only perform a full unbind upon destruction
   * to ensure no lingering references. */
#ifndef NDEBUG
  if (true) {
#else
  if (G.debug & G_DEBUG_GPU) {
#endif
    if (bound_ctx_ != nullptr && bind_slot_ > -1) {
      MTLUniformBufferBinding &ctx_ubo_bind_slot =
          bound_ctx_->pipeline_state.ubo_bindings[bind_slot_];
      if (ctx_ubo_bind_slot.bound && ctx_ubo_bind_slot.ubo == this) {
        ctx_ubo_bind_slot.bound = false;
        ctx_ubo_bind_slot.ubo = nullptr;
      }
    }
  }

  /* Reset bind index. */
  bind_slot_ = -1;
  bound_ctx_ = nullptr;
}

id<MTLBuffer> MTLUniformBuf::get_metal_buffer(int *r_offset)
{
  BLI_assert(this);
  *r_offset = 0;
  if (metal_buffer_ != nullptr && has_data_) {
    *r_offset = 0;
    metal_buffer_->debug_ensure_used();
    return metal_buffer_->get_metal_buffer();
  }
  else {
    *r_offset = 0;
    return nil;
  }
}

int MTLUniformBuf::get_size()
{
  BLI_assert(this);
  return size_in_bytes_;
}

}  // blender::gpu
