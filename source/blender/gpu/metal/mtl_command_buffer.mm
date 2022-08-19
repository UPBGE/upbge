/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_userdef_types.h"

#include "mtl_backend.hh"
#include "mtl_common.hh"
#include "mtl_context.hh"
#include "mtl_debug.hh"
#include "mtl_framebuffer.hh"

#include <fstream>

using namespace blender;
using namespace blender::gpu;

namespace blender::gpu {

/* Global sync event used across MTLContext's.
 * This resolves flickering artifacts from command buffer
 * dependencies not being honored for work submitted between
 * different GPUContext's. */
id<MTLEvent> MTLCommandBufferManager::sync_event = nil;
uint64_t MTLCommandBufferManager::event_signal_val = 0;

/* Counter for active command buffers. */
int MTLCommandBufferManager::num_active_cmd_bufs = 0;

/* -------------------------------------------------------------------- */
/** \name MTLCommandBuffer initialization and render coordination.
 * \{ */

void MTLCommandBufferManager::prepare(bool supports_render)
{
  render_pass_state_.reset_state();
}

void MTLCommandBufferManager::register_encoder_counters()
{
  encoder_count_++;
  empty_ = false;
}

id<MTLCommandBuffer> MTLCommandBufferManager::ensure_begin()
{
  if (active_command_buffer_ == nil) {

    /* Verify number of active command buffers is below limit.
     * Exceeding this limit will mean we either have a leak/GPU hang
     * or we should increase the command buffer limit during MTLQueue creation */
    BLI_assert(MTLCommandBufferManager::num_active_cmd_bufs < MTL_MAX_COMMAND_BUFFERS);

    if (G.debug & G_DEBUG_GPU) {
      /* Debug: Enable Advanced Errors for GPU work execution. */
      MTLCommandBufferDescriptor *desc = [[MTLCommandBufferDescriptor alloc] init];
      desc.errorOptions = MTLCommandBufferErrorOptionEncoderExecutionStatus;
      desc.retainedReferences = YES;
      active_command_buffer_ = [context_.queue commandBufferWithDescriptor:desc];
    }
    else {
      active_command_buffer_ = [context_.queue commandBuffer];
    }
    [active_command_buffer_ retain];
    MTLCommandBufferManager::num_active_cmd_bufs++;

    /* Ensure command buffers execute in submission order across multiple MTLContext's. */
    if (this->sync_event != nil) {
      [active_command_buffer_ encodeWaitForEvent:this->sync_event value:this->event_signal_val];
    }

    /* Ensure we begin new Scratch Buffer if we are on a new frame. */
    MTLScratchBufferManager &mem = context_.memory_manager;
    mem.ensure_increment_scratch_buffer();

    /* Reset Command buffer heuristics. */
    this->reset_counters();
  }
  BLI_assert(active_command_buffer_ != nil);
  return active_command_buffer_;
}

/* If wait is true, CPU will stall until GPU work has completed. */
bool MTLCommandBufferManager::submit(bool wait)
{
  /* Skip submission if command buffer is empty. */
  if (empty_ || active_command_buffer_ == nil) {
    return false;
  }

  /* Ensure current encoders are finished. */
  this->end_active_command_encoder();
  BLI_assert(active_command_encoder_type_ == MTL_NO_COMMAND_ENCODER);

  /* Flush active ScratchBuffer associated with parent MTLContext. */
  context_.memory_manager.flush_active_scratch_buffer();

  /*** Submit Command Buffer. ***/
  /* Strict ordering ensures command buffers are guaranteed to execute after a previous
   * one has completed. Resolves flickering when command buffers are submitted from
   * different MTLContext's. */
  if (MTLCommandBufferManager::sync_event == nil) {
    MTLCommandBufferManager::sync_event = [context_.device newEvent];
    BLI_assert(MTLCommandBufferManager::sync_event);
    [MTLCommandBufferManager::sync_event retain];
  }
  BLI_assert(MTLCommandBufferManager::sync_event != nil);
  MTLCommandBufferManager::event_signal_val++;

  [active_command_buffer_ encodeSignalEvent:MTLCommandBufferManager::sync_event
                                      value:MTLCommandBufferManager::event_signal_val];

  /* Command buffer lifetime tracking. */
  /* Increment current MTLSafeFreeList reference counter to flag MTLBuffers freed within
   * the current command buffer lifetime as used.
   * This ensures that in-use resources are not prematurely de-referenced and returned to the
   * available buffer pool while they are in-use by the GPU. */
  MTLSafeFreeList *cmd_free_buffer_list =
      MTLContext::get_global_memory_manager().get_current_safe_list();
  BLI_assert(cmd_free_buffer_list);
  cmd_free_buffer_list->increment_reference();

  id<MTLCommandBuffer> cmd_buffer_ref = active_command_buffer_;
  [cmd_buffer_ref retain];

  [cmd_buffer_ref addCompletedHandler:^(id<MTLCommandBuffer> cb) {
    /* Upon command buffer completion, decrement MTLSafeFreeList reference count
     * to allow buffers no longer in use by this CommandBuffer to be freed. */
    cmd_free_buffer_list->decrement_reference();

    /* Release command buffer after completion callback handled. */
    [cmd_buffer_ref release];

    /* Decrement count. */
    MTLCommandBufferManager::num_active_cmd_bufs--;
  }];

  /* Submit command buffer to GPU. */
  [active_command_buffer_ commit];

  if (wait || (G.debug & G_DEBUG_GPU)) {
    /* Wait until current GPU work has finished executing. */
    [active_command_buffer_ waitUntilCompleted];

    /* Command buffer execution debugging can return an error message if
     * execution has failed or encountered GPU-side errors. */
    if (G.debug & G_DEBUG_GPU) {

      NSError *error = [active_command_buffer_ error];
      if (error != nil) {
        NSLog(@"%@", error);
        BLI_assert(false);

        @autoreleasepool {
          const char *stringAsChar = [[NSString stringWithFormat:@"%@", error] UTF8String];

          std::ofstream outfile;
          outfile.open("command_buffer_error.txt", std::fstream::out | std::fstream::app);
          outfile << stringAsChar;
          outfile.close();
        }
      }
    }
  }

  /* Release previous frames command buffer and reset active cmd buffer. */
  if (last_submitted_command_buffer_ != nil) {

    BLI_assert(MTLBackend::get()->is_inside_render_boundary());
    [last_submitted_command_buffer_ autorelease];
    last_submitted_command_buffer_ = nil;
  }
  last_submitted_command_buffer_ = active_command_buffer_;
  active_command_buffer_ = nil;

  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Render Command Encoder Utility and management functions.
 * \{ */

/* Fetch/query current encoder. */
bool MTLCommandBufferManager::is_inside_render_pass()
{
  return (active_command_encoder_type_ == MTL_RENDER_COMMAND_ENCODER);
}

bool MTLCommandBufferManager::is_inside_blit()
{
  return (active_command_encoder_type_ == MTL_BLIT_COMMAND_ENCODER);
}

bool MTLCommandBufferManager::is_inside_compute()
{
  return (active_command_encoder_type_ == MTL_COMPUTE_COMMAND_ENCODER);
}

id<MTLRenderCommandEncoder> MTLCommandBufferManager::get_active_render_command_encoder()
{
  /* Calling code should check if inside render pass. Otherwise nil. */
  return active_render_command_encoder_;
}

id<MTLBlitCommandEncoder> MTLCommandBufferManager::get_active_blit_command_encoder()
{
  /* Calling code should check if inside render pass. Otherwise nil. */
  return active_blit_command_encoder_;
}

id<MTLComputeCommandEncoder> MTLCommandBufferManager::get_active_compute_command_encoder()
{
  /* Calling code should check if inside render pass. Otherwise nil. */
  return active_compute_command_encoder_;
}

MTLFrameBuffer *MTLCommandBufferManager::get_active_framebuffer()
{
  /* If outside of RenderPass, nullptr will be returned. */
  if (this->is_inside_render_pass()) {
    return active_frame_buffer_;
  }
  return nullptr;
}

/* Encoder and Pass management. */
/* End currently active MTLCommandEncoder. */
bool MTLCommandBufferManager::end_active_command_encoder()
{

  /* End active encoder if one is active. */
  if (active_command_encoder_type_ != MTL_NO_COMMAND_ENCODER) {

    switch (active_command_encoder_type_) {
      case MTL_RENDER_COMMAND_ENCODER: {
        /* Verify a RenderCommandEncoder is active and end. */
        BLI_assert(active_render_command_encoder_ != nil);

        /* Complete Encoding. */
        [active_render_command_encoder_ endEncoding];
        [active_render_command_encoder_ release];
        active_render_command_encoder_ = nil;
        active_command_encoder_type_ = MTL_NO_COMMAND_ENCODER;

        /* Reset associated framebuffer flag. */
        active_frame_buffer_ = nullptr;
        active_pass_descriptor_ = nullptr;
        return true;
      }

      case MTL_BLIT_COMMAND_ENCODER: {
        /* Verify a RenderCommandEncoder is active and end. */
        BLI_assert(active_blit_command_encoder_ != nil);
        [active_blit_command_encoder_ endEncoding];
        [active_blit_command_encoder_ release];
        active_blit_command_encoder_ = nil;
        active_command_encoder_type_ = MTL_NO_COMMAND_ENCODER;
        return true;
      }

      case MTL_COMPUTE_COMMAND_ENCODER: {
        /* Verify a RenderCommandEncoder is active and end. */
        BLI_assert(active_compute_command_encoder_ != nil);
        [active_compute_command_encoder_ endEncoding];
        [active_compute_command_encoder_ release];
        active_compute_command_encoder_ = nil;
        active_command_encoder_type_ = MTL_NO_COMMAND_ENCODER;
        return true;
      }

      default: {
        BLI_assert(false && "Invalid command encoder type");
        return false;
      }
    };
  }
  else {
    /* MTL_NO_COMMAND_ENCODER. */
    BLI_assert(active_render_command_encoder_ == nil);
    BLI_assert(active_blit_command_encoder_ == nil);
    BLI_assert(active_compute_command_encoder_ == nil);
    return false;
  }
}

id<MTLRenderCommandEncoder> MTLCommandBufferManager::ensure_begin_render_command_encoder(
    MTLFrameBuffer *ctx_framebuffer, bool force_begin, bool *new_pass)
{
  /* Ensure valid framebuffer. */
  BLI_assert(ctx_framebuffer != nullptr);

  /* Ensure active command buffer. */
  id<MTLCommandBuffer> cmd_buf = this->ensure_begin();
  BLI_assert(cmd_buf);

  /* Begin new command encoder if the currently active one is
   * incompatible or requires updating. */
  if (active_command_encoder_type_ != MTL_RENDER_COMMAND_ENCODER ||
      active_frame_buffer_ != ctx_framebuffer || force_begin) {
    this->end_active_command_encoder();

    /* Determine if this is a re-bind of the same framebuffer. */
    bool is_rebind = (active_frame_buffer_ == ctx_framebuffer);

    /* Generate RenderPassDescriptor from bound framebuffer.  */
    BLI_assert(ctx_framebuffer);
    active_frame_buffer_ = ctx_framebuffer;
    active_pass_descriptor_ = active_frame_buffer_->bake_render_pass_descriptor(
        is_rebind && (!active_frame_buffer_->get_pending_clear()));

    /* Determine if there is a visibility buffer assigned to the context. */
    gpu::MTLBuffer *visibility_buffer = context_.get_visibility_buffer();
    this->active_pass_descriptor_.visibilityResultBuffer =
        (visibility_buffer) ? visibility_buffer->get_metal_buffer() : nil;
    context_.clear_visibility_dirty();

    /* Ensure we have already cleaned up our previous render command encoder. */
    BLI_assert(active_render_command_encoder_ == nil);

    /* Create new RenderCommandEncoder based on descriptor (and begin encoding). */
    active_render_command_encoder_ = [cmd_buf
        renderCommandEncoderWithDescriptor:active_pass_descriptor_];
    [active_render_command_encoder_ retain];
    active_command_encoder_type_ = MTL_RENDER_COMMAND_ENCODER;

    /* Update command buffer encoder heuristics. */
    this->register_encoder_counters();

    /* Apply initial state. */
    /* Update Viewport and Scissor State */
    active_frame_buffer_->apply_state();

    /* FLAG FRAMEBUFFER AS CLEARED -- A clear only lasts as long as one has been specified.
     * After this, resets to Load attachments to parallel GL behavior. */
    active_frame_buffer_->mark_cleared();

    /* Reset RenderPassState to ensure resource bindings are re-applied. */
    render_pass_state_.reset_state();

    /* Return true as new pass started. */
    *new_pass = true;
  }
  else {
    /* No new pass. */
    *new_pass = false;
  }

  BLI_assert(active_render_command_encoder_ != nil);
  return active_render_command_encoder_;
}

id<MTLBlitCommandEncoder> MTLCommandBufferManager::ensure_begin_blit_encoder()
{
  /* Ensure active command buffer. */
  id<MTLCommandBuffer> cmd_buf = this->ensure_begin();
  BLI_assert(cmd_buf);

  /* Ensure no existing command encoder of a different type is active. */
  if (active_command_encoder_type_ != MTL_BLIT_COMMAND_ENCODER) {
    this->end_active_command_encoder();
  }

  /* Begin new Blit Encoder. */
  if (active_blit_command_encoder_ == nil) {
    active_blit_command_encoder_ = [cmd_buf blitCommandEncoder];
    BLI_assert(active_blit_command_encoder_ != nil);
    [active_blit_command_encoder_ retain];
    active_command_encoder_type_ = MTL_BLIT_COMMAND_ENCODER;

    /* Update command buffer encoder heuristics. */
    this->register_encoder_counters();
  }
  BLI_assert(active_blit_command_encoder_ != nil);
  return active_blit_command_encoder_;
}

id<MTLComputeCommandEncoder> MTLCommandBufferManager::ensure_begin_compute_encoder()
{
  /* Ensure active command buffer. */
  id<MTLCommandBuffer> cmd_buf = this->ensure_begin();
  BLI_assert(cmd_buf);

  /* Ensure no existing command encoder of a different type is active. */
  if (active_command_encoder_type_ != MTL_COMPUTE_COMMAND_ENCODER) {
    this->end_active_command_encoder();
  }

  /* Begin new Compute Encoder. */
  if (active_compute_command_encoder_ == nil) {
    active_compute_command_encoder_ = [cmd_buf computeCommandEncoder];
    BLI_assert(active_compute_command_encoder_ != nil);
    [active_compute_command_encoder_ retain];
    active_command_encoder_type_ = MTL_COMPUTE_COMMAND_ENCODER;

    /* Update command buffer encoder heuristics. */
    this->register_encoder_counters();
  }
  BLI_assert(active_compute_command_encoder_ != nil);
  return active_compute_command_encoder_;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Command buffer heuristics.
 * \{ */

/* Rendering Heuristics. */
void MTLCommandBufferManager::register_draw_counters(int vertex_submission)
{
  current_draw_call_count_++;
  vertex_submitted_count_ += vertex_submission;
  empty_ = false;
}

/* Reset workload counters. */
void MTLCommandBufferManager::reset_counters()
{
  empty_ = true;
  current_draw_call_count_ = 0;
  encoder_count_ = 0;
  vertex_submitted_count_ = 0;
}

/* Workload evaluation. */
bool MTLCommandBufferManager::do_break_submission()
{
  /* Skip if no active command buffer. */
  if (active_command_buffer_ == nil) {
    return false;
  }

  /* Use optimized heuristic to split heavy command buffer submissions to better saturate the
   * hardware and also reduce stalling from individual large submissions. */
  if (GPU_type_matches(GPU_DEVICE_INTEL, GPU_OS_ANY, GPU_DRIVER_ANY) ||
      GPU_type_matches(GPU_DEVICE_ATI, GPU_OS_ANY, GPU_DRIVER_ANY)) {
    return ((current_draw_call_count_ > 30000) || (vertex_submitted_count_ > 100000000) ||
            (encoder_count_ > 25));
  }
  else {
    /* Apple Silicon is less efficient if splitting submissions. */
    return false;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Command buffer debugging.
 * \{ */

/* Debug. */
void MTLCommandBufferManager::push_debug_group(const char *name, int index)
{
  id<MTLCommandBuffer> cmd = this->ensure_begin();
  if (cmd != nil) {
    [cmd pushDebugGroup:[NSString stringWithFormat:@"%s_%d", name, index]];
  }
}

void MTLCommandBufferManager::pop_debug_group()
{
  id<MTLCommandBuffer> cmd = this->ensure_begin();
  if (cmd != nil) {
    [cmd popDebugGroup];
  }
}

/* Workload Synchronization. */
bool MTLCommandBufferManager::insert_memory_barrier(eGPUBarrier barrier_bits,
                                                    eGPUStageBarrierBits before_stages,
                                                    eGPUStageBarrierBits after_stages)
{
  /* Only supporting Metal on 10.14 onward anyway - Check required for warnings. */
  if (@available(macOS 10.14, *)) {

    /* Resolve scope. */
    MTLBarrierScope scope = 0;
    if (barrier_bits & GPU_BARRIER_SHADER_IMAGE_ACCESS ||
        barrier_bits & GPU_BARRIER_TEXTURE_FETCH) {
      scope = scope | MTLBarrierScopeTextures | MTLBarrierScopeRenderTargets;
    }
    if (barrier_bits & GPU_BARRIER_SHADER_STORAGE ||
        barrier_bits & GPU_BARRIER_VERTEX_ATTRIB_ARRAY ||
        barrier_bits & GPU_BARRIER_ELEMENT_ARRAY) {
      scope = scope | MTLBarrierScopeBuffers;
    }

    if (scope != 0) {
      /* Issue barrier based on encoder. */
      switch (active_command_encoder_type_) {
        case MTL_NO_COMMAND_ENCODER:
        case MTL_BLIT_COMMAND_ENCODER: {
          /* No barrier to be inserted. */
          return false;
        }

        /* Rendering. */
        case MTL_RENDER_COMMAND_ENCODER: {
          /* Currently flagging both stages -- can use bits above to filter on stage type --
           * though full barrier is safe for now*/
          MTLRenderStages before_stage_flags = 0;
          MTLRenderStages after_stage_flags = 0;
          if (before_stages & GPU_BARRIER_STAGE_VERTEX &&
              !(before_stages & GPU_BARRIER_STAGE_FRAGMENT)) {
            before_stage_flags = before_stage_flags | MTLRenderStageVertex;
          }
          if (before_stages & GPU_BARRIER_STAGE_FRAGMENT) {
            before_stage_flags = before_stage_flags | MTLRenderStageFragment;
          }
          if (after_stages & GPU_BARRIER_STAGE_VERTEX) {
            after_stage_flags = after_stage_flags | MTLRenderStageVertex;
          }
          if (after_stages & GPU_BARRIER_STAGE_FRAGMENT) {
            after_stage_flags = MTLRenderStageFragment;
          }

          id<MTLRenderCommandEncoder> rec = this->get_active_render_command_encoder();
          BLI_assert(rec != nil);
          [rec memoryBarrierWithScope:scope
                          afterStages:after_stage_flags
                         beforeStages:before_stage_flags];
          return true;
        }

        /* Compute. */
        case MTL_COMPUTE_COMMAND_ENCODER: {
          id<MTLComputeCommandEncoder> rec = this->get_active_compute_command_encoder();
          BLI_assert(rec != nil);
          [rec memoryBarrierWithScope:scope];
          return true;
        }
      }
    }
  }
  /* No barrier support. */
  return false;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Render Pass State for active RenderCommandEncoder
 * \{ */
/* Reset binding state when a new RenderCommandEncoder is bound, to ensure
 * pipeline resources are re-applied to the new Encoder.
 * NOTE: In Metal, state is only persistent within an MTLCommandEncoder,
 * not globally. */
void MTLRenderPassState::reset_state()
{
  /* Reset Cached pipeline state. */
  this->bound_pso = nil;
  this->bound_ds_state = nil;

  /* Clear shader binding. */
  this->last_bound_shader_state.set(nullptr, 0);

  /* Other states. */
  MTLFrameBuffer *fb = this->cmd.get_active_framebuffer();
  this->last_used_stencil_ref_value = 0;
  this->last_scissor_rect = {0,
                             0,
                             (uint)((fb != nullptr) ? fb->get_width() : 0),
                             (uint)((fb != nullptr) ? fb->get_height() : 0)};

  /* Reset cached resource binding state */
  for (int ubo = 0; ubo < MTL_MAX_UNIFORM_BUFFER_BINDINGS; ubo++) {
    this->cached_vertex_buffer_bindings[ubo].is_bytes = false;
    this->cached_vertex_buffer_bindings[ubo].metal_buffer = nil;
    this->cached_vertex_buffer_bindings[ubo].offset = -1;

    this->cached_fragment_buffer_bindings[ubo].is_bytes = false;
    this->cached_fragment_buffer_bindings[ubo].metal_buffer = nil;
    this->cached_fragment_buffer_bindings[ubo].offset = -1;
  }

  /* Reset cached texture and sampler state binding state. */
  for (int tex = 0; tex < MTL_MAX_TEXTURE_SLOTS; tex++) {
    this->cached_vertex_texture_bindings[tex].metal_texture = nil;
    this->cached_vertex_sampler_state_bindings[tex].sampler_state = nil;
    this->cached_vertex_sampler_state_bindings[tex].is_arg_buffer_binding = false;

    this->cached_fragment_texture_bindings[tex].metal_texture = nil;
    this->cached_fragment_sampler_state_bindings[tex].sampler_state = nil;
    this->cached_fragment_sampler_state_bindings[tex].is_arg_buffer_binding = false;
  }
}

/* Bind Texture to current RenderCommandEncoder. */
void MTLRenderPassState::bind_vertex_texture(id<MTLTexture> tex, uint slot)
{
  if (this->cached_vertex_texture_bindings[slot].metal_texture != tex) {
    id<MTLRenderCommandEncoder> rec = this->cmd.get_active_render_command_encoder();
    BLI_assert(rec != nil);
    [rec setVertexTexture:tex atIndex:slot];
    this->cached_vertex_texture_bindings[slot].metal_texture = tex;
  }
}

void MTLRenderPassState::bind_fragment_texture(id<MTLTexture> tex, uint slot)
{
  if (this->cached_fragment_texture_bindings[slot].metal_texture != tex) {
    id<MTLRenderCommandEncoder> rec = this->cmd.get_active_render_command_encoder();
    BLI_assert(rec != nil);
    [rec setFragmentTexture:tex atIndex:slot];
    this->cached_fragment_texture_bindings[slot].metal_texture = tex;
  }
}

void MTLRenderPassState::bind_vertex_sampler(MTLSamplerBinding &sampler_binding,
                                             bool use_argument_buffer_for_samplers,
                                             uint slot)
{
  /* TODO(Metal): Implement RenderCommandEncoder vertex sampler binding utility. This will be
   * implemented alongside MTLShader. */
}

void MTLRenderPassState::bind_fragment_sampler(MTLSamplerBinding &sampler_binding,
                                               bool use_argument_buffer_for_samplers,
                                               uint slot)
{
  /* TODO(Metal): Implement RenderCommandEncoder fragment sampler binding utility. This will be
   * implemented alongside MTLShader. */
}

void MTLRenderPassState::bind_vertex_buffer(id<MTLBuffer> buffer, uint buffer_offset, uint index)
{
  /* TODO(Metal): Implement RenderCommandEncoder vertex buffer binding utility. This will be
   * implemented alongside the full MTLMemoryManager. */
}

void MTLRenderPassState::bind_fragment_buffer(id<MTLBuffer> buffer, uint buffer_offset, uint index)
{
  /* TODO(Metal): Implement RenderCommandEncoder fragment buffer binding utility. This will be
   * implemented alongside the full MTLMemoryManager. */
}

void MTLRenderPassState::bind_vertex_bytes(void *bytes, uint length, uint index)
{
  /* TODO(Metal): Implement RenderCommandEncoder vertex bytes binding utility. This will be
   * implemented alongside the full MTLMemoryManager. */
}

void MTLRenderPassState::bind_fragment_bytes(void *bytes, uint length, uint index)
{
  /* TODO(Metal): Implement RenderCommandEncoder fragment bytes binding utility. This will be
   * implemented alongside the full MTLMemoryManager. */
}

/** \} */

}  // blender::gpu
