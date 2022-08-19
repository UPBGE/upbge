/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "BKE_global.h"

#include "DNA_userdef_types.h"

#include "GPU_batch.h"
#include "GPU_batch_presets.h"
#include "GPU_capabilities.h"
#include "GPU_framebuffer.h"
#include "GPU_platform.h"
#include "GPU_state.h"

#include "mtl_backend.hh"
#include "mtl_common.hh"
#include "mtl_context.hh"
#include "mtl_debug.hh"
#include "mtl_texture.hh"

#include "GHOST_C-api.h"

namespace blender::gpu {

/* -------------------------------------------------------------------- */
/** \name Creation & Deletion
 * \{ */

void gpu::MTLTexture::mtl_texture_init()
{
  BLI_assert(MTLContext::get() != nullptr);

  /* Status. */
  is_baked_ = false;
  is_dirty_ = false;
  resource_mode_ = MTL_TEXTURE_MODE_DEFAULT;
  mtl_max_mips_ = 1;

  /* Metal properties. */
  texture_ = nil;
  texture_buffer_ = nil;
  mip_swizzle_view_ = nil;

  /* Binding information. */
  is_bound_ = false;

  /* VBO. */
  vert_buffer_ = nullptr;
  vert_buffer_mtl_ = nil;
  vert_buffer_offset_ = -1;

  /* Default Swizzle. */
  tex_swizzle_mask_[0] = 'r';
  tex_swizzle_mask_[1] = 'g';
  tex_swizzle_mask_[2] = 'b';
  tex_swizzle_mask_[3] = 'a';
  mtl_swizzle_mask_ = MTLTextureSwizzleChannelsMake(
      MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleAlpha);

  /* TODO(Metal): Find a way of specifying texture usage externally. */
  gpu_image_usage_flags_ = GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_ATTACHMENT;
}

gpu::MTLTexture::MTLTexture(const char *name) : Texture(name)
{
  /* Common Initialization. */
  mtl_texture_init();
}

gpu::MTLTexture::MTLTexture(const char *name,
                            eGPUTextureFormat format,
                            eGPUTextureType type,
                            id<MTLTexture> metal_texture)
    : Texture(name)
{
  /* Common Initialization. */
  mtl_texture_init();

  /* Prep texture from METAL handle. */
  BLI_assert(metal_texture != nil);
  BLI_assert(type == GPU_TEXTURE_2D);
  type_ = type;
  init_2D(metal_texture.width, metal_texture.height, 0, 1, format);

  /* Assign MTLTexture. */
  texture_ = metal_texture;
  [texture_ retain];

  /* Flag as Baked. */
  is_baked_ = true;
  is_dirty_ = false;
  resource_mode_ = MTL_TEXTURE_MODE_EXTERNAL;
}

gpu::MTLTexture::~MTLTexture()
{
  /* Unbind if bound. */
  if (is_bound_) {
    MTLContext *ctx = static_cast<MTLContext *>(unwrap(GPU_context_active_get()));
    if (ctx != nullptr) {
      ctx->state_manager->texture_unbind(this);
    }
  }

  /* Free memory. */
  this->reset();
}

/** \} */

/* -------------------------------------------------------------------- */
void gpu::MTLTexture::bake_mip_swizzle_view()
{
  if (texture_view_dirty_flags_) {
    /* if a texture view was previously created we release it. */
    if (mip_swizzle_view_ != nil) {
      [mip_swizzle_view_ release];
      mip_swizzle_view_ = nil;
    }

    /* Determine num slices */
    int num_slices = 1;
    switch (type_) {
      case GPU_TEXTURE_1D_ARRAY:
        num_slices = h_;
        break;
      case GPU_TEXTURE_2D_ARRAY:
        num_slices = d_;
        break;
      case GPU_TEXTURE_CUBE:
        num_slices = 6;
        break;
      case GPU_TEXTURE_CUBE_ARRAY:
        /* d_ is equal to array levels * 6, including face count. */
        num_slices = d_;
        break;
      default:
        num_slices = 1;
        break;
    }

    int range_len = min_ii((mip_texture_max_level_ - mip_texture_base_level_) + 1,
                           texture_.mipmapLevelCount);
    BLI_assert(range_len > 0);
    BLI_assert(mip_texture_base_level_ < texture_.mipmapLevelCount);
    BLI_assert(mip_texture_base_layer_ < num_slices);
    mip_swizzle_view_ = [texture_
        newTextureViewWithPixelFormat:texture_.pixelFormat
                          textureType:texture_.textureType
                               levels:NSMakeRange(mip_texture_base_level_, range_len)
                               slices:NSMakeRange(mip_texture_base_layer_, num_slices)
                              swizzle:mtl_swizzle_mask_];
    MTL_LOG_INFO(
        "Updating texture view - MIP TEXTURE BASE LEVEL: %d, MAX LEVEL: %d (Range len: %d)\n",
        mip_texture_base_level_,
        min_ii(mip_texture_max_level_, texture_.mipmapLevelCount),
        range_len);
    mip_swizzle_view_.label = [texture_ label];
    texture_view_dirty_flags_ = TEXTURE_VIEW_NOT_DIRTY;
  }
}

/** \name Operations
 * \{ */

id<MTLTexture> gpu::MTLTexture::get_metal_handle()
{

  /* ensure up to date and baked. */
  this->ensure_baked();

  /* Verify VBO texture shares same buffer. */
  if (resource_mode_ == MTL_TEXTURE_MODE_VBO) {
    int r_offset = -1;

    /* TODO(Metal): Fetch buffer from MTLVertBuf when implemented. */
    id<MTLBuffer> buf = nil; /*vert_buffer_->get_metal_buffer(&r_offset);*/
    BLI_assert(vert_buffer_mtl_ != nil);
    BLI_assert(buf == vert_buffer_mtl_ && r_offset == vert_buffer_offset_);

    UNUSED_VARS(buf);
    UNUSED_VARS_NDEBUG(r_offset);
  }

  if (is_baked_) {
    /* For explicit texture views, ensure we always return the texture view. */
    if (resource_mode_ == MTL_TEXTURE_MODE_TEXTURE_VIEW) {
      BLI_assert(mip_swizzle_view_ && "Texture view should always have a valid handle.");
    }

    if (mip_swizzle_view_ != nil || texture_view_dirty_flags_) {
      bake_mip_swizzle_view();
      return mip_swizzle_view_;
    }
    return texture_;
  }
  return nil;
}

id<MTLTexture> gpu::MTLTexture::get_metal_handle_base()
{

  /* ensure up to date and baked. */
  this->ensure_baked();

  /* For explicit texture views, always return the texture view. */
  if (resource_mode_ == MTL_TEXTURE_MODE_TEXTURE_VIEW) {
    BLI_assert(mip_swizzle_view_ && "Texture view should always have a valid handle.");
    if (mip_swizzle_view_ != nil || texture_view_dirty_flags_) {
      bake_mip_swizzle_view();
    }
    return mip_swizzle_view_;
  }

  /* Return base handle. */
  if (is_baked_) {
    return texture_;
  }
  return nil;
}

void gpu::MTLTexture::blit(id<MTLBlitCommandEncoder> blit_encoder,
                           uint src_x_offset,
                           uint src_y_offset,
                           uint src_z_offset,
                           uint src_slice,
                           uint src_mip,
                           gpu::MTLTexture *dest,
                           uint dst_x_offset,
                           uint dst_y_offset,
                           uint dst_z_offset,
                           uint dst_slice,
                           uint dst_mip,
                           uint width,
                           uint height,
                           uint depth)
{

  BLI_assert(this && dest);
  BLI_assert(width > 0 && height > 0 && depth > 0);
  MTLSize src_size = MTLSizeMake(width, height, depth);
  MTLOrigin src_origin = MTLOriginMake(src_x_offset, src_y_offset, src_z_offset);
  MTLOrigin dst_origin = MTLOriginMake(dst_x_offset, dst_y_offset, dst_z_offset);

  if (this->format_get() != dest->format_get()) {
    MTL_LOG_WARNING(
        "[Warning] gpu::MTLTexture: Cannot copy between two textures of different types using a "
        "blit encoder. TODO: Support this operation\n");
    return;
  }

  /* TODO(Metal): Verify if we want to use the one with modified base-level/texture view
   * or not. */
  [blit_encoder copyFromTexture:this->get_metal_handle_base()
                    sourceSlice:src_slice
                    sourceLevel:src_mip
                   sourceOrigin:src_origin
                     sourceSize:src_size
                      toTexture:dest->get_metal_handle_base()
               destinationSlice:dst_slice
               destinationLevel:dst_mip
              destinationOrigin:dst_origin];
}

void gpu::MTLTexture::blit(gpu::MTLTexture *dst,
                           uint src_x_offset,
                           uint src_y_offset,
                           uint dst_x_offset,
                           uint dst_y_offset,
                           uint src_mip,
                           uint dst_mip,
                           uint dst_slice,
                           int width,
                           int height)
{
  BLI_assert(this->type_get() == dst->type_get());

  GPUShader *shader = fullscreen_blit_sh_get();
  BLI_assert(shader != nullptr);
  BLI_assert(GPU_context_active_get());

  /* Fetch restore framebuffer and blit target framebuffer from destination texture. */
  GPUFrameBuffer *restore_fb = GPU_framebuffer_active_get();
  GPUFrameBuffer *blit_target_fb = dst->get_blit_framebuffer(dst_slice, dst_mip);
  BLI_assert(blit_target_fb);
  GPU_framebuffer_bind(blit_target_fb);

  /* Execute graphics draw call to perform the blit. */
  GPUBatch *quad = GPU_batch_preset_quad();

  GPU_batch_set_shader(quad, shader);

  float w = dst->width_get();
  float h = dst->height_get();

  GPU_shader_uniform_2f(shader, "fullscreen", w, h);
  GPU_shader_uniform_2f(shader, "src_offset", src_x_offset, src_y_offset);
  GPU_shader_uniform_2f(shader, "dst_offset", dst_x_offset, dst_y_offset);
  GPU_shader_uniform_2f(shader, "size", width, height);

  GPU_shader_uniform_1i(shader, "mip", src_mip);
  GPU_batch_texture_bind(quad, "imageTexture", wrap(this));

  /* Caching previous pipeline state. */
  bool depth_write_prev = GPU_depth_mask_get();
  uint stencil_mask_prev = GPU_stencil_mask_get();
  eGPUStencilTest stencil_test_prev = GPU_stencil_test_get();
  eGPUFaceCullTest culling_test_prev = GPU_face_culling_get();
  eGPUBlend blend_prev = GPU_blend_get();
  eGPUDepthTest depth_test_prev = GPU_depth_test_get();
  GPU_scissor_test(false);

  /* Apply state for blit draw call. */
  GPU_stencil_write_mask_set(0xFF);
  GPU_stencil_reference_set(0);
  GPU_face_culling(GPU_CULL_NONE);
  GPU_stencil_test(GPU_STENCIL_ALWAYS);
  GPU_depth_mask(false);
  GPU_blend(GPU_BLEND_NONE);
  GPU_depth_test(GPU_DEPTH_ALWAYS);

  GPU_batch_draw(quad);

  /* restoring old pipeline state. */
  GPU_depth_mask(depth_write_prev);
  GPU_stencil_write_mask_set(stencil_mask_prev);
  GPU_stencil_test(stencil_test_prev);
  GPU_face_culling(culling_test_prev);
  GPU_depth_mask(depth_write_prev);
  GPU_blend(blend_prev);
  GPU_depth_test(depth_test_prev);

  if (restore_fb != nullptr) {
    GPU_framebuffer_bind(restore_fb);
  }
  else {
    GPU_framebuffer_restore();
  }
}

GPUFrameBuffer *gpu::MTLTexture::get_blit_framebuffer(uint dst_slice, uint dst_mip)
{

  /* Check if layer has changed. */
  bool update_attachments = false;
  if (!blit_fb_) {
    blit_fb_ = GPU_framebuffer_create("gpu_blit");
    update_attachments = true;
  }

  /* Check if current blit FB has the correct attachment properties. */
  if (blit_fb_) {
    if (blit_fb_slice_ != dst_slice || blit_fb_mip_ != dst_mip) {
      update_attachments = true;
    }
  }

  if (update_attachments) {
    if (format_flag_ & GPU_FORMAT_DEPTH || format_flag_ & GPU_FORMAT_STENCIL) {
      /* DEPTH TEX */
      GPU_framebuffer_ensure_config(
          &blit_fb_,
          {GPU_ATTACHMENT_TEXTURE_LAYER_MIP(wrap(static_cast<Texture *>(this)),
                                            static_cast<int>(dst_slice),
                                            static_cast<int>(dst_mip)),
           GPU_ATTACHMENT_NONE});
    }
    else {
      /* COLOR TEX */
      GPU_framebuffer_ensure_config(
          &blit_fb_,
          {GPU_ATTACHMENT_NONE,
           GPU_ATTACHMENT_TEXTURE_LAYER_MIP(wrap(static_cast<Texture *>(this)),
                                            static_cast<int>(dst_slice),
                                            static_cast<int>(dst_mip))});
    }
    blit_fb_slice_ = dst_slice;
    blit_fb_mip_ = dst_mip;
  }

  BLI_assert(blit_fb_);
  return blit_fb_;
}

MTLSamplerState gpu::MTLTexture::get_sampler_state()
{
  MTLSamplerState sampler_state;
  sampler_state.state = this->sampler_state;
  /* Add more parameters as needed */
  return sampler_state;
}

void gpu::MTLTexture::update_sub(
    int mip, int offset[3], int extent[3], eGPUDataFormat type, const void *data)
{
  /* Fetch active context. */
  MTLContext *ctx = static_cast<MTLContext *>(unwrap(GPU_context_active_get()));
  BLI_assert(ctx);

  /* Do not update texture view. */
  BLI_assert(resource_mode_ != MTL_TEXTURE_MODE_TEXTURE_VIEW);

  /* Ensure mipmaps. */
  this->ensure_mipmaps(mip);

  /* Ensure texture is baked. */
  this->ensure_baked();

  /* Safety checks. */
#if TRUST_NO_ONE
  BLI_assert(mip >= mip_min_ && mip <= mip_max_);
  BLI_assert(mip < texture_.mipmapLevelCount);
  BLI_assert(texture_.mipmapLevelCount >= mip_max_);
#endif

  /* DEPTH FLAG - Depth formats cannot use direct BLIT - pass off to their own routine which will
   * do a depth-only render. */
  bool is_depth_format = (format_flag_ & GPU_FORMAT_DEPTH);
  if (is_depth_format) {
    switch (type_) {

      case GPU_TEXTURE_2D: {
        update_sub_depth_2d(mip, offset, extent, type, data);
        return;
      }
      default:
        MTL_LOG_ERROR(
            "[Error] gpu::MTLTexture::update_sub not yet supported for other depth "
            "configurations\n");
        return;
        return;
    }
  }

  @autoreleasepool {
    /* Determine totalsize of INPUT Data. */
    int num_channels = to_component_len(format_);
    int input_bytes_per_pixel = num_channels * to_bytesize(type);
    int totalsize = 0;

    /* If unpack row length is used, size of input data uses the unpack row length, rather than the
     * image length. */
    int expected_update_w = ((ctx->pipeline_state.unpack_row_length == 0) ?
                                 extent[0] :
                                 ctx->pipeline_state.unpack_row_length);

    /* Ensure calculated total size isn't larger than remaining image data size */
    switch (this->dimensions_count()) {
      case 1:
        totalsize = input_bytes_per_pixel * max_ii(expected_update_w, 1);
        break;
      case 2:
        totalsize = input_bytes_per_pixel * max_ii(expected_update_w, 1) * max_ii(extent[1], 1);
        break;
      case 3:
        totalsize = input_bytes_per_pixel * max_ii(expected_update_w, 1) * max_ii(extent[1], 1) *
                    max_ii(extent[2], 1);
        break;
      default:
        BLI_assert(false);
        break;
    }

    /* When unpack row length is used, provided data does not necessarily contain padding for last
     * row, so we only include up to the end of updated data. */
    if (ctx->pipeline_state.unpack_row_length > 0) {
      BLI_assert(ctx->pipeline_state.unpack_row_length >= extent[0]);
      totalsize -= (ctx->pipeline_state.unpack_row_length - extent[0]) * input_bytes_per_pixel;
    }

    /* Check */
    BLI_assert(totalsize > 0);

    /* Determine expected destination data size. */
    MTLPixelFormat destination_format = gpu_texture_format_to_metal(format_);
    int expected_dst_bytes_per_pixel = get_mtl_format_bytesize(destination_format);
    int destination_num_channels = get_mtl_format_num_components(destination_format);

    /* Prepare specialisation struct (For texture update routine). */
    TextureUpdateRoutineSpecialisation compute_specialisation_kernel = {
        tex_data_format_to_msl_type_str(type),              /* INPUT DATA FORMAT */
        tex_data_format_to_msl_texture_template_type(type), /* TEXTURE DATA FORMAT */
        num_channels,
        destination_num_channels};

    /* Determine whether we can do direct BLIT or not. */
    bool can_use_direct_blit = true;
    if (expected_dst_bytes_per_pixel != input_bytes_per_pixel ||
        num_channels != destination_num_channels) {
      can_use_direct_blit = false;
    }

    if (is_depth_format) {
      if (type_ == GPU_TEXTURE_2D || type_ == GPU_TEXTURE_2D_ARRAY) {
        /* Workaround for crash in validation layer when blitting to depth2D target with
         * dimensions (1, 1, 1); */
        if (extent[0] == 1 && extent[1] == 1 && extent[2] == 1 && totalsize == 4) {
          can_use_direct_blit = false;
        }
      }
    }

    if (format_ == GPU_SRGB8_A8 && !can_use_direct_blit) {
      MTL_LOG_WARNING(
          "SRGB data upload does not work correctly using compute upload. "
          "texname '%s'\n",
          name_);
    }

    /* Safety Checks. */
    if (type == GPU_DATA_UINT_24_8 || type == GPU_DATA_10_11_11_REV) {
      BLI_assert(can_use_direct_blit &&
                 "Special input data type must be a 1-1 mapping with destination texture as it "
                 "cannot easily be split");
    }

    /* Debug and verification. */
    if (!can_use_direct_blit) {
      MTL_LOG_WARNING(
          "gpu::MTLTexture::update_sub supplied bpp is %d bytes (%d components per "
          "pixel), but backing texture bpp is %d bytes (%d components per pixel) "
          "(TODO(Metal): Channel Conversion needed) (w: %d, h: %d, d: %d)\n",
          input_bytes_per_pixel,
          num_channels,
          expected_dst_bytes_per_pixel,
          destination_num_channels,
          w_,
          h_,
          d_);

      /* Check mip compatibility. */
      if (mip != 0) {
        MTL_LOG_ERROR(
            "[Error]: Updating texture layers other than mip=0 when data is mismatched is not "
            "possible in METAL on macOS using texture->write\n");
        return;
      }

      /* Check Format write-ability. */
      if (mtl_format_get_writeable_view_format(destination_format) == MTLPixelFormatInvalid) {
        MTL_LOG_ERROR(
            "[Error]: Updating texture -- destination MTLPixelFormat '%d' does not support write "
            "operations, and no suitable TextureView format exists.\n",
            *(int *)(&destination_format));
        return;
      }
    }

    /* Prepare staging buffer for data. */
    id<MTLBuffer> staging_buffer = nil;
    uint64_t staging_buffer_offset = 0;

    /* Fetch allocation from scratch buffer. */
    MTLTemporaryBuffer allocation =
        ctx->get_scratchbuffer_manager().scratch_buffer_allocate_range_aligned(totalsize, 256);
    memcpy(allocation.data, data, totalsize);
    staging_buffer = allocation.metal_buffer;
    staging_buffer_offset = allocation.buffer_offset;

    /* Common Properties. */
    MTLPixelFormat compatible_write_format = mtl_format_get_writeable_view_format(
        destination_format);

    /* Some texture formats are not writeable so we need to use a texture view. */
    if (compatible_write_format == MTLPixelFormatInvalid) {
      MTL_LOG_ERROR("Cannot use compute update blit with texture-view format: %d\n",
                    *((int *)&compatible_write_format));
      return;
    }
    id<MTLTexture> texture_handle = ((compatible_write_format == destination_format)) ?
                                        texture_ :
                                        [texture_
                                            newTextureViewWithPixelFormat:compatible_write_format];

    /* Prepare command encoders. */
    id<MTLBlitCommandEncoder> blit_encoder = nil;
    id<MTLComputeCommandEncoder> compute_encoder = nil;
    if (can_use_direct_blit) {
      blit_encoder = ctx->main_command_buffer.ensure_begin_blit_encoder();
      BLI_assert(blit_encoder != nil);
    }
    else {
      compute_encoder = ctx->main_command_buffer.ensure_begin_compute_encoder();
      BLI_assert(compute_encoder != nil);
    }

    switch (type_) {

      /* 1D */
      case GPU_TEXTURE_1D:
      case GPU_TEXTURE_1D_ARRAY: {
        if (can_use_direct_blit) {
          /* Use Blit based update. */
          int bytes_per_row = expected_dst_bytes_per_pixel *
                              ((ctx->pipeline_state.unpack_row_length == 0) ?
                                   extent[0] :
                                   ctx->pipeline_state.unpack_row_length);
          int bytes_per_image = bytes_per_row;
          int max_array_index = ((type_ == GPU_TEXTURE_1D_ARRAY) ? extent[1] : 1);
          for (int array_index = 0; array_index < max_array_index; array_index++) {

            int buffer_array_offset = staging_buffer_offset + (bytes_per_image * array_index);
            [blit_encoder
                     copyFromBuffer:staging_buffer
                       sourceOffset:buffer_array_offset
                  sourceBytesPerRow:bytes_per_row
                sourceBytesPerImage:bytes_per_image
                         sourceSize:MTLSizeMake(extent[0], 1, 1)
                          toTexture:texture_handle
                   destinationSlice:((type_ == GPU_TEXTURE_1D_ARRAY) ? (array_index + offset[1]) :
                                                                       0)
                   destinationLevel:mip
                  destinationOrigin:MTLOriginMake(offset[0], 0, 0)];
          }
        }
        else {
          /* Use Compute Based update. */
          if (type_ == GPU_TEXTURE_1D) {
            id<MTLComputePipelineState> pso = texture_update_1d_get_kernel(
                compute_specialisation_kernel);
            TextureUpdateParams params = {mip,
                                          {extent[0], 1, 1},
                                          {offset[0], 0, 0},
                                          ((ctx->pipeline_state.unpack_row_length == 0) ?
                                               extent[0] :
                                               ctx->pipeline_state.unpack_row_length)};
            [compute_encoder setComputePipelineState:pso];
            [compute_encoder setBytes:&params length:sizeof(params) atIndex:0];
            [compute_encoder setBuffer:staging_buffer offset:staging_buffer_offset atIndex:1];
            [compute_encoder setTexture:texture_handle atIndex:0];
            [compute_encoder
                      dispatchThreads:MTLSizeMake(extent[0], 1, 1) /* Width, Height, Layer */
                threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
          }
          else if (type_ == GPU_TEXTURE_1D_ARRAY) {
            id<MTLComputePipelineState> pso = texture_update_1d_array_get_kernel(
                compute_specialisation_kernel);
            TextureUpdateParams params = {mip,
                                          {extent[0], extent[1], 1},
                                          {offset[0], offset[1], 0},
                                          ((ctx->pipeline_state.unpack_row_length == 0) ?
                                               extent[0] :
                                               ctx->pipeline_state.unpack_row_length)};
            [compute_encoder setComputePipelineState:pso];
            [compute_encoder setBytes:&params length:sizeof(params) atIndex:0];
            [compute_encoder setBuffer:staging_buffer offset:staging_buffer_offset atIndex:1];
            [compute_encoder setTexture:texture_handle atIndex:0];
            [compute_encoder
                      dispatchThreads:MTLSizeMake(extent[0], extent[1], 1) /* Width, layers, nil */
                threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
          }
        }
      } break;

      /* 2D */
      case GPU_TEXTURE_2D:
      case GPU_TEXTURE_2D_ARRAY: {
        if (can_use_direct_blit) {
          /* Use Blit encoder update. */
          int bytes_per_row = expected_dst_bytes_per_pixel *
                              ((ctx->pipeline_state.unpack_row_length == 0) ?
                                   extent[0] :
                                   ctx->pipeline_state.unpack_row_length);
          int bytes_per_image = bytes_per_row * extent[1];

          int texture_array_relative_offset = 0;
          int base_slice = (type_ == GPU_TEXTURE_2D_ARRAY) ? offset[2] : 0;
          int final_slice = base_slice + ((type_ == GPU_TEXTURE_2D_ARRAY) ? extent[2] : 1);

          for (int array_slice = base_slice; array_slice < final_slice; array_slice++) {

            if (array_slice > 0) {
              BLI_assert(type_ == GPU_TEXTURE_2D_ARRAY);
              BLI_assert(array_slice < d_);
            }

            [blit_encoder copyFromBuffer:staging_buffer
                            sourceOffset:staging_buffer_offset + texture_array_relative_offset
                       sourceBytesPerRow:bytes_per_row
                     sourceBytesPerImage:bytes_per_image
                              sourceSize:MTLSizeMake(extent[0], extent[1], 1)
                               toTexture:texture_handle
                        destinationSlice:array_slice
                        destinationLevel:mip
                       destinationOrigin:MTLOriginMake(offset[0], offset[1], 0)];

            texture_array_relative_offset += bytes_per_image;
          }
        }
        else {
          /* Use Compute texture update. */
          if (type_ == GPU_TEXTURE_2D) {
            id<MTLComputePipelineState> pso = texture_update_2d_get_kernel(
                compute_specialisation_kernel);
            TextureUpdateParams params = {mip,
                                          {extent[0], extent[1], 1},
                                          {offset[0], offset[1], 0},
                                          ((ctx->pipeline_state.unpack_row_length == 0) ?
                                               extent[0] :
                                               ctx->pipeline_state.unpack_row_length)};
            [compute_encoder setComputePipelineState:pso];
            [compute_encoder setBytes:&params length:sizeof(params) atIndex:0];
            [compute_encoder setBuffer:staging_buffer offset:staging_buffer_offset atIndex:1];
            [compute_encoder setTexture:texture_handle atIndex:0];
            [compute_encoder
                      dispatchThreads:MTLSizeMake(
                                          extent[0], extent[1], 1) /* Width, Height, Layer */
                threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
          }
          else if (type_ == GPU_TEXTURE_2D_ARRAY) {
            id<MTLComputePipelineState> pso = texture_update_2d_array_get_kernel(
                compute_specialisation_kernel);
            TextureUpdateParams params = {mip,
                                          {extent[0], extent[1], extent[2]},
                                          {offset[0], offset[1], offset[2]},
                                          ((ctx->pipeline_state.unpack_row_length == 0) ?
                                               extent[0] :
                                               ctx->pipeline_state.unpack_row_length)};
            [compute_encoder setComputePipelineState:pso];
            [compute_encoder setBytes:&params length:sizeof(params) atIndex:0];
            [compute_encoder setBuffer:staging_buffer offset:staging_buffer_offset atIndex:1];
            [compute_encoder setTexture:texture_handle atIndex:0];
            [compute_encoder dispatchThreads:MTLSizeMake(extent[0],
                                                         extent[1],
                                                         extent[2]) /* Width, Height, Layer */
                       threadsPerThreadgroup:MTLSizeMake(4, 4, 4)];
          }
        }

      } break;

      /* 3D */
      case GPU_TEXTURE_3D: {
        if (can_use_direct_blit) {
          int bytes_per_row = expected_dst_bytes_per_pixel *
                              ((ctx->pipeline_state.unpack_row_length == 0) ?
                                   extent[0] :
                                   ctx->pipeline_state.unpack_row_length);
          int bytes_per_image = bytes_per_row * extent[1];
          [blit_encoder copyFromBuffer:staging_buffer
                          sourceOffset:staging_buffer_offset
                     sourceBytesPerRow:bytes_per_row
                   sourceBytesPerImage:bytes_per_image
                            sourceSize:MTLSizeMake(extent[0], extent[1], extent[2])
                             toTexture:texture_handle
                      destinationSlice:0
                      destinationLevel:mip
                     destinationOrigin:MTLOriginMake(offset[0], offset[1], offset[2])];
        }
        else {
          id<MTLComputePipelineState> pso = texture_update_3d_get_kernel(
              compute_specialisation_kernel);
          TextureUpdateParams params = {mip,
                                        {extent[0], extent[1], extent[2]},
                                        {offset[0], offset[1], offset[2]},
                                        ((ctx->pipeline_state.unpack_row_length == 0) ?
                                             extent[0] :
                                             ctx->pipeline_state.unpack_row_length)};
          [compute_encoder setComputePipelineState:pso];
          [compute_encoder setBytes:&params length:sizeof(params) atIndex:0];
          [compute_encoder setBuffer:staging_buffer offset:staging_buffer_offset atIndex:1];
          [compute_encoder setTexture:texture_handle atIndex:0];
          [compute_encoder
                    dispatchThreads:MTLSizeMake(
                                        extent[0], extent[1], extent[2]) /* Width, Height, Depth */
              threadsPerThreadgroup:MTLSizeMake(4, 4, 4)];
        }
      } break;

      /* CUBE */
      case GPU_TEXTURE_CUBE: {
        if (can_use_direct_blit) {
          int bytes_per_row = expected_dst_bytes_per_pixel *
                              ((ctx->pipeline_state.unpack_row_length == 0) ?
                                   extent[0] :
                                   ctx->pipeline_state.unpack_row_length);
          int bytes_per_image = bytes_per_row * extent[1];

          int texture_array_relative_offset = 0;

          /* Iterate over all cube faces in range (offset[2], offset[2] + extent[2]). */
          for (int i = 0; i < extent[2]; i++) {
            int face_index = offset[2] + i;

            [blit_encoder copyFromBuffer:staging_buffer
                            sourceOffset:staging_buffer_offset + texture_array_relative_offset
                       sourceBytesPerRow:bytes_per_row
                     sourceBytesPerImage:bytes_per_image
                              sourceSize:MTLSizeMake(extent[0], extent[1], 1)
                               toTexture:texture_handle
                        destinationSlice:face_index /* = cubeFace+arrayIndex*6 */
                        destinationLevel:mip
                       destinationOrigin:MTLOriginMake(offset[0], offset[1], 0)];
            texture_array_relative_offset += bytes_per_image;
          }
        }
        else {
          MTL_LOG_ERROR(
              "TODO(Metal): Support compute texture update for GPU_TEXTURE_CUBE %d, %d, %d\n",
              w_,
              h_,
              d_);
        }
      } break;

      case GPU_TEXTURE_CUBE_ARRAY: {
        if (can_use_direct_blit) {

          int bytes_per_row = expected_dst_bytes_per_pixel *
                              ((ctx->pipeline_state.unpack_row_length == 0) ?
                                   extent[0] :
                                   ctx->pipeline_state.unpack_row_length);
          int bytes_per_image = bytes_per_row * extent[1];

          /* Upload to all faces between offset[2] (which is zero in most cases) AND extent[2]. */
          int texture_array_relative_offset = 0;
          for (int i = 0; i < extent[2]; i++) {
            int face_index = offset[2] + i;
            [blit_encoder copyFromBuffer:staging_buffer
                            sourceOffset:staging_buffer_offset + texture_array_relative_offset
                       sourceBytesPerRow:bytes_per_row
                     sourceBytesPerImage:bytes_per_image
                              sourceSize:MTLSizeMake(extent[0], extent[1], 1)
                               toTexture:texture_handle
                        destinationSlice:face_index /* = cubeFace+arrayIndex*6. */
                        destinationLevel:mip
                       destinationOrigin:MTLOriginMake(offset[0], offset[1], 0)];
            texture_array_relative_offset += bytes_per_image;
          }
        }
        else {
          MTL_LOG_ERROR(
              "TODO(Metal): Support compute texture update for GPU_TEXTURE_CUBE_ARRAY %d, %d, "
              "%d\n",
              w_,
              h_,
              d_);
        }
      } break;

      case GPU_TEXTURE_BUFFER: {
        /* TODO(Metal): Support Data upload to TEXTURE BUFFER
         * Data uploads generally happen via GPUVertBuf instead. */
        BLI_assert(false);
      } break;

      case GPU_TEXTURE_ARRAY:
        /* Not an actual format - modifier flag for others. */
        return;
    }

    /* Finalize Blit Encoder. */
    if (can_use_direct_blit) {

      /* Textures which use MTLStorageModeManaged need to have updated contents
       * synced back to CPU to avoid an automatic flush overwriting contents. */
      if (texture_.storageMode == MTLStorageModeManaged) {
        [blit_encoder synchronizeResource:texture_buffer_];
      }
    }
    else {
      /* Textures which use MTLStorageModeManaged need to have updated contents
       * synced back to CPU to avoid an automatic flush overwriting contents. */
      if (texture_.storageMode == MTLStorageModeManaged) {
        blit_encoder = ctx->main_command_buffer.ensure_begin_blit_encoder();
        [blit_encoder synchronizeResource:texture_buffer_];
      }
    }
  }
}

void gpu::MTLTexture::ensure_mipmaps(int miplvl)
{

  /* Do not update texture view. */
  BLI_assert(resource_mode_ != MTL_TEXTURE_MODE_TEXTURE_VIEW);

  /* Clamp level to maximum. */
  int effective_h = (type_ == GPU_TEXTURE_1D_ARRAY) ? 0 : h_;
  int effective_d = (type_ != GPU_TEXTURE_3D) ? 0 : d_;
  int max_dimension = max_iii(w_, effective_h, effective_d);
  int max_miplvl = floor(log2(max_dimension));
  miplvl = min_ii(max_miplvl, miplvl);

  /* Increase mipmap level. */
  if (mipmaps_ < miplvl) {
    mipmaps_ = miplvl;

    /* Check if baked. */
    if (is_baked_ && mipmaps_ > mtl_max_mips_) {
      is_dirty_ = true;
      MTL_LOG_WARNING("Texture requires regenerating due to increase in mip-count\n");
    }
  }
  this->mip_range_set(0, mipmaps_);
}

void gpu::MTLTexture::generate_mipmap()
{
  /* Fetch Active Context. */
  MTLContext *ctx = reinterpret_cast<MTLContext *>(GPU_context_active_get());
  BLI_assert(ctx);

  if (!ctx->device) {
    MTL_LOG_ERROR("Cannot Generate mip-maps -- metal device invalid\n");
    BLI_assert(false);
    return;
  }

  /* Ensure mipmaps. */
  this->ensure_mipmaps(9999);

  /* Ensure texture is baked. */
  this->ensure_baked();
  BLI_assert(is_baked_ && texture_ && "MTLTexture is not valid");

  if (mipmaps_ == 1 || mtl_max_mips_ == 1) {
    MTL_LOG_WARNING("Call to generate mipmaps on texture with 'mipmaps_=1\n'");
    return;
  }

  /* Verify if we can perform mipmap generation. */
  if (format_ == GPU_DEPTH_COMPONENT32F || format_ == GPU_DEPTH_COMPONENT24 ||
      format_ == GPU_DEPTH_COMPONENT16 || format_ == GPU_DEPTH32F_STENCIL8 ||
      format_ == GPU_DEPTH24_STENCIL8) {
    MTL_LOG_WARNING("Cannot generate mipmaps for textures using DEPTH formats\n");
    return;
  }

  @autoreleasepool {

    /* Fetch active BlitCommandEncoder. */
    id<MTLBlitCommandEncoder> enc = ctx->main_command_buffer.ensure_begin_blit_encoder();
    if (G.debug & G_DEBUG_GPU) {
      [enc insertDebugSignpost:@"Generate MipMaps"];
    }
    [enc generateMipmapsForTexture:texture_];
  }
  return;
}

void gpu::MTLTexture::copy_to(Texture *dst)
{
  /* Safety Checks. */
  gpu::MTLTexture *mt_src = this;
  gpu::MTLTexture *mt_dst = static_cast<gpu::MTLTexture *>(dst);
  BLI_assert((mt_dst->w_ == mt_src->w_) && (mt_dst->h_ == mt_src->h_) &&
             (mt_dst->d_ == mt_src->d_));
  BLI_assert(mt_dst->format_ == mt_src->format_);
  BLI_assert(mt_dst->type_ == mt_src->type_);

  UNUSED_VARS_NDEBUG(mt_src);

  /* Fetch active context. */
  MTLContext *ctx = static_cast<MTLContext *>(unwrap(GPU_context_active_get()));
  BLI_assert(ctx);

  /* Ensure texture is baked. */
  this->ensure_baked();

  @autoreleasepool {
    /* Setup blit encoder. */
    id<MTLBlitCommandEncoder> blit_encoder = ctx->main_command_buffer.ensure_begin_blit_encoder();
    BLI_assert(blit_encoder != nil);

    /* TODO(Metal): Consider supporting multiple mip levels IF the GL implementation
     * follows, currently it does not. */
    int mip = 0;

    /* NOTE: mip_size_get() won't override any dimension that is equal to 0. */
    int extent[3] = {1, 1, 1};
    this->mip_size_get(mip, extent);

    switch (mt_dst->type_) {
      case GPU_TEXTURE_2D_ARRAY:
      case GPU_TEXTURE_CUBE_ARRAY:
      case GPU_TEXTURE_3D: {
        /* Do full texture copy for 3D textures */
        BLI_assert(mt_dst->d_ == d_);
        [blit_encoder copyFromTexture:this->get_metal_handle_base()
                            toTexture:mt_dst->get_metal_handle_base()];
      } break;
      default: {
        int slice = 0;
        this->blit(blit_encoder,
                   0,
                   0,
                   0,
                   slice,
                   mip,
                   mt_dst,
                   0,
                   0,
                   0,
                   slice,
                   mip,
                   extent[0],
                   extent[1],
                   extent[2]);
      } break;
    }
  }
}

void gpu::MTLTexture::clear(eGPUDataFormat data_format, const void *data)
{
  /* Ensure texture is baked. */
  this->ensure_baked();

  /* Create clear framebuffer. */
  GPUFrameBuffer *prev_fb = GPU_framebuffer_active_get();
  FrameBuffer *fb = reinterpret_cast<FrameBuffer *>(this->get_blit_framebuffer(0, 0));
  fb->bind(true);
  fb->clear_attachment(this->attachment_type(0), data_format, data);
  GPU_framebuffer_bind(prev_fb);
}

static MTLTextureSwizzle swizzle_to_mtl(const char swizzle)
{
  switch (swizzle) {
    default:
    case 'x':
    case 'r':
      return MTLTextureSwizzleRed;
    case 'y':
    case 'g':
      return MTLTextureSwizzleGreen;
    case 'z':
    case 'b':
      return MTLTextureSwizzleBlue;
    case 'w':
    case 'a':
      return MTLTextureSwizzleAlpha;
    case '0':
      return MTLTextureSwizzleZero;
    case '1':
      return MTLTextureSwizzleOne;
  }
}

void gpu::MTLTexture::swizzle_set(const char swizzle_mask[4])
{
  if (memcmp(tex_swizzle_mask_, swizzle_mask, 4) != 0) {
    memcpy(tex_swizzle_mask_, swizzle_mask, 4);

    /* Creating the swizzle mask and flagging as dirty if changed. */
    MTLTextureSwizzleChannels new_swizzle_mask = MTLTextureSwizzleChannelsMake(
        swizzle_to_mtl(swizzle_mask[0]),
        swizzle_to_mtl(swizzle_mask[1]),
        swizzle_to_mtl(swizzle_mask[2]),
        swizzle_to_mtl(swizzle_mask[3]));

    mtl_swizzle_mask_ = new_swizzle_mask;
    texture_view_dirty_flags_ |= TEXTURE_VIEW_SWIZZLE_DIRTY;
  }
}

void gpu::MTLTexture::mip_range_set(int min, int max)
{
  BLI_assert(min <= max && min >= 0 && max <= mipmaps_);

  /* NOTE:
   * - mip_min_ and mip_max_ are used to Clamp LODs during sampling.
   * - Given functions like Framebuffer::recursive_downsample modifies the mip range
   *   between each layer, we do not want to be re-baking the texture.
   * - For the time being, we are going to just need to generate a FULL mipmap chain
   *   as we do not know ahead of time whether mipmaps will be used.
   *
   *   TODO(Metal): Add texture initialization flag to determine whether mipmaps are used
   *   or not. Will be important for saving memory for big textures. */
  mip_min_ = min;
  mip_max_ = max;

  if ((type_ == GPU_TEXTURE_1D || type_ == GPU_TEXTURE_1D_ARRAY || type_ == GPU_TEXTURE_BUFFER) &&
      max > 1) {

    MTL_LOG_ERROR(
        " MTLTexture of type TEXTURE_1D_ARRAY or TEXTURE_BUFFER cannot have a mipcount "
        "greater than 1\n");
    mip_min_ = 0;
    mip_max_ = 0;
    mipmaps_ = 0;
    BLI_assert(false);
  }

  /* Mip range for texture view. */
  mip_texture_base_level_ = mip_min_;
  mip_texture_max_level_ = mip_max_;
  texture_view_dirty_flags_ |= TEXTURE_VIEW_MIP_DIRTY;
}

void *gpu::MTLTexture::read(int mip, eGPUDataFormat type)
{
  /* Prepare Array for return data. */
  BLI_assert(!(format_flag_ & GPU_FORMAT_COMPRESSED));
  BLI_assert(mip <= mipmaps_);
  BLI_assert(validate_data_format_mtl(format_, type));

  /* NOTE: mip_size_get() won't override any dimension that is equal to 0. */
  int extent[3] = {1, 1, 1};
  this->mip_size_get(mip, extent);

  size_t sample_len = extent[0] * extent[1] * extent[2];
  size_t sample_size = to_bytesize(format_, type);
  size_t texture_size = sample_len * sample_size;
  int num_channels = to_component_len(format_);

  void *data = MEM_mallocN(texture_size + 8, "GPU_texture_read");

  /* Ensure texture is baked. */
  if (is_baked_) {
    this->read_internal(
        mip, 0, 0, 0, extent[0], extent[1], extent[2], type, num_channels, texture_size + 8, data);
  }
  else {
    /* Clear return values? */
    MTL_LOG_WARNING("MTLTexture::read - reading from texture with no image data\n");
  }

  return data;
}

/* Fetch the raw buffer data from a texture and copy to CPU host ptr. */
void gpu::MTLTexture::read_internal(int mip,
                                    int x_off,
                                    int y_off,
                                    int z_off,
                                    int width,
                                    int height,
                                    int depth,
                                    eGPUDataFormat desired_output_format,
                                    int num_output_components,
                                    int debug_data_size,
                                    void *r_data)
{
  /* Verify textures are baked. */
  if (!is_baked_) {
    MTL_LOG_WARNING("gpu::MTLTexture::read_internal - Trying to read from a non-baked texture!\n");
    return;
  }
  /* Fetch active context. */
  MTLContext *ctx = static_cast<MTLContext *>(unwrap(GPU_context_active_get()));
  BLI_assert(ctx);

  /* Calculate Desired output size. */
  int num_channels = to_component_len(format_);
  BLI_assert(num_output_components <= num_channels);
  uint desired_output_bpp = num_output_components * to_bytesize(desired_output_format);

  /* Calculate Metal data output for trivial copy. */
  uint image_bpp = get_mtl_format_bytesize(texture_.pixelFormat);
  uint image_components = get_mtl_format_num_components(texture_.pixelFormat);
  bool is_depth_format = (format_flag_ & GPU_FORMAT_DEPTH);

  /* Verify if we need to use compute read. */
  eGPUDataFormat data_format = to_mtl_internal_data_format(this->format_get());
  bool format_conversion_needed = (data_format != desired_output_format);
  bool can_use_simple_read = (desired_output_bpp == image_bpp) && (!format_conversion_needed) &&
                             (num_output_components == image_components);

  /* Depth must be read using the compute shader -- Some safety checks to verify that params are
   * correct. */
  if (is_depth_format) {
    can_use_simple_read = false;
    /* TODO(Metal): Stencil data write not yet supported, so force components to one. */
    image_components = 1;
    BLI_assert(num_output_components == 1);
    BLI_assert(image_components == 1);
    BLI_assert(data_format == GPU_DATA_FLOAT || data_format == GPU_DATA_UINT_24_8);
    BLI_assert(validate_data_format_mtl(format_, data_format));
  }

  /* SPECIAL Workaround for R11G11B10 textures requesting a read using: GPU_DATA_10_11_11_REV. */
  if (desired_output_format == GPU_DATA_10_11_11_REV) {
    BLI_assert(format_ == GPU_R11F_G11F_B10F);

    /* override parameters - we'll be able to use simple copy, as bpp will match at 4 bytes. */
    image_bpp = sizeof(int);
    image_components = 1;
    desired_output_bpp = sizeof(int);
    num_output_components = 1;

    data_format = GPU_DATA_INT;
    format_conversion_needed = false;
    can_use_simple_read = true;
  }

  /* Determine size of output data. */
  uint bytes_per_row = desired_output_bpp * width;
  uint bytes_per_image = bytes_per_row * height;
  uint total_bytes = bytes_per_image * depth;

  if (can_use_simple_read) {
    /* DEBUG check that if direct copy is being used, then both the expected output size matches
     * the METAL texture size. */
    BLI_assert(
        ((num_output_components * to_bytesize(desired_output_format)) == desired_output_bpp) &&
        (desired_output_bpp == image_bpp));
  }
  /* DEBUG check that the allocated data size matches the bytes we expect. */
  BLI_assert(total_bytes <= debug_data_size);

  /* Fetch allocation from scratch buffer. */
  id<MTLBuffer> destination_buffer = nil;
  uint destination_offset = 0;
  void *destination_buffer_host_ptr = nullptr;

  /* TODO(Metal): Optimize buffer allocation. */
  MTLResourceOptions bufferOptions = MTLResourceStorageModeManaged;
  destination_buffer = [ctx->device newBufferWithLength:max_ii(total_bytes, 256)
                                                options:bufferOptions];
  destination_offset = 0;
  destination_buffer_host_ptr = (void *)((uint8_t *)([destination_buffer contents]) +
                                         destination_offset);

  /* Prepare specialisation struct (For non-trivial texture read routine). */
  int depth_format_mode = 0;
  if (is_depth_format) {
    depth_format_mode = 1;
    switch (desired_output_format) {
      case GPU_DATA_FLOAT:
        depth_format_mode = 1;
        break;
      case GPU_DATA_UINT_24_8:
        depth_format_mode = 2;
        break;
      case GPU_DATA_UINT:
        depth_format_mode = 4;
        break;
      default:
        BLI_assert(false && "Unhandled depth read format case");
        break;
    }
  }

  TextureReadRoutineSpecialisation compute_specialisation_kernel = {
      tex_data_format_to_msl_texture_template_type(data_format), /* TEXTURE DATA TYPE */
      tex_data_format_to_msl_type_str(desired_output_format),    /* OUTPUT DATA TYPE */
      num_channels,                                              /* TEXTURE COMPONENT COUNT */
      num_output_components,                                     /* OUTPUT DATA COMPONENT COUNT */
      depth_format_mode};

  bool copy_successful = false;
  @autoreleasepool {

    /* TODO(Metal): Verify whether we need some form of barrier here to ensure reads
     * happen after work with associated texture is finished. */
    GPU_finish();

    /* Texture View for SRGB special case. */
    id<MTLTexture> read_texture = texture_;
    if (format_ == GPU_SRGB8_A8) {
      read_texture = [texture_ newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm];
    }

    /* Perform per-texture type read. */
    switch (type_) {
      case GPU_TEXTURE_2D: {
        if (can_use_simple_read) {
          /* Use Blit Encoder READ. */
          id<MTLBlitCommandEncoder> enc = ctx->main_command_buffer.ensure_begin_blit_encoder();
          if (G.debug & G_DEBUG_GPU) {
            [enc insertDebugSignpost:@"GPUTextureRead"];
          }
          [enc copyFromTexture:read_texture
                           sourceSlice:0
                           sourceLevel:mip
                          sourceOrigin:MTLOriginMake(x_off, y_off, 0)
                            sourceSize:MTLSizeMake(width, height, 1)
                              toBuffer:destination_buffer
                     destinationOffset:destination_offset
                destinationBytesPerRow:bytes_per_row
              destinationBytesPerImage:bytes_per_image];
          [enc synchronizeResource:destination_buffer];
          copy_successful = true;
        }
        else {

          /* Use Compute READ. */
          id<MTLComputeCommandEncoder> compute_encoder =
              ctx->main_command_buffer.ensure_begin_compute_encoder();
          id<MTLComputePipelineState> pso = texture_read_2d_get_kernel(
              compute_specialisation_kernel);
          TextureReadParams params = {
              mip,
              {width, height, 1},
              {x_off, y_off, 0},
          };
          [compute_encoder setComputePipelineState:pso];
          [compute_encoder setBytes:&params length:sizeof(params) atIndex:0];
          [compute_encoder setBuffer:destination_buffer offset:destination_offset atIndex:1];
          [compute_encoder setTexture:read_texture atIndex:0];
          [compute_encoder dispatchThreads:MTLSizeMake(width, height, 1) /* Width, Height, Layer */
                     threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];

          /* Use Blit encoder to synchronize results back to CPU. */
          id<MTLBlitCommandEncoder> enc = ctx->main_command_buffer.ensure_begin_blit_encoder();
          if (G.debug & G_DEBUG_GPU) {
            [enc insertDebugSignpost:@"GPUTextureRead-syncResource"];
          }
          [enc synchronizeResource:destination_buffer];
          copy_successful = true;
        }
      } break;

      case GPU_TEXTURE_2D_ARRAY: {
        if (can_use_simple_read) {
          /* Use Blit Encoder READ. */
          id<MTLBlitCommandEncoder> enc = ctx->main_command_buffer.ensure_begin_blit_encoder();
          if (G.debug & G_DEBUG_GPU) {
            [enc insertDebugSignpost:@"GPUTextureRead"];
          }
          int base_slice = z_off;
          int final_slice = base_slice + depth;
          int texture_array_relative_offset = 0;

          for (int array_slice = base_slice; array_slice < final_slice; array_slice++) {
            [enc copyFromTexture:read_texture
                             sourceSlice:0
                             sourceLevel:mip
                            sourceOrigin:MTLOriginMake(x_off, y_off, 0)
                              sourceSize:MTLSizeMake(width, height, 1)
                                toBuffer:destination_buffer
                       destinationOffset:destination_offset + texture_array_relative_offset
                  destinationBytesPerRow:bytes_per_row
                destinationBytesPerImage:bytes_per_image];
            [enc synchronizeResource:destination_buffer];

            texture_array_relative_offset += bytes_per_image;
          }
          copy_successful = true;
        }
        else {

          /* Use Compute READ */
          id<MTLComputeCommandEncoder> compute_encoder =
              ctx->main_command_buffer.ensure_begin_compute_encoder();
          id<MTLComputePipelineState> pso = texture_read_2d_array_get_kernel(
              compute_specialisation_kernel);
          TextureReadParams params = {
              mip,
              {width, height, depth},
              {x_off, y_off, z_off},
          };
          [compute_encoder setComputePipelineState:pso];
          [compute_encoder setBytes:&params length:sizeof(params) atIndex:0];
          [compute_encoder setBuffer:destination_buffer offset:destination_offset atIndex:1];
          [compute_encoder setTexture:read_texture atIndex:0];
          [compute_encoder
                    dispatchThreads:MTLSizeMake(width, height, depth) /* Width, Height, Layer */
              threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];

          /* Use Blit encoder to synchronize results back to CPU. */
          id<MTLBlitCommandEncoder> enc = ctx->main_command_buffer.ensure_begin_blit_encoder();
          if (G.debug & G_DEBUG_GPU) {
            [enc insertDebugSignpost:@"GPUTextureRead-syncResource"];
          }
          [enc synchronizeResource:destination_buffer];
          copy_successful = true;
        }
      } break;

      case GPU_TEXTURE_CUBE_ARRAY: {
        if (can_use_simple_read) {
          id<MTLBlitCommandEncoder> enc = ctx->main_command_buffer.ensure_begin_blit_encoder();
          if (G.debug & G_DEBUG_GPU) {
            [enc insertDebugSignpost:@"GPUTextureRead"];
          }
          int base_slice = z_off;
          int final_slice = base_slice + depth;
          int texture_array_relative_offset = 0;

          for (int array_slice = base_slice; array_slice < final_slice; array_slice++) {
            [enc copyFromTexture:read_texture
                             sourceSlice:array_slice
                             sourceLevel:mip
                            sourceOrigin:MTLOriginMake(x_off, y_off, 0)
                              sourceSize:MTLSizeMake(width, height, 1)
                                toBuffer:destination_buffer
                       destinationOffset:destination_offset + texture_array_relative_offset
                  destinationBytesPerRow:bytes_per_row
                destinationBytesPerImage:bytes_per_image];
            [enc synchronizeResource:destination_buffer];

            texture_array_relative_offset += bytes_per_image;
          }
          MTL_LOG_INFO("Copying texture data to buffer GPU_TEXTURE_CUBE_ARRAY\n");
          copy_successful = true;
        }
        else {
          MTL_LOG_ERROR("TODO(Metal): unsupported compute copy of texture cube array");
        }
      } break;

      default:
        MTL_LOG_WARNING(
            "[Warning] gpu::MTLTexture::read_internal simple-copy not yet supported for texture "
            "type: %d\n",
            (int)type_);
        break;
    }

    if (copy_successful) {
      /* Ensure GPU copy commands have completed. */
      GPU_finish();

      /* Copy data from Shared Memory into ptr. */
      memcpy(r_data, destination_buffer_host_ptr, total_bytes);
      MTL_LOG_INFO("gpu::MTLTexture::read_internal success! %d bytes read\n", total_bytes);
    }
    else {
      MTL_LOG_WARNING(
          "[Warning] gpu::MTLTexture::read_internal not yet supported for this config -- data "
          "format different (src %d bytes, dst %d bytes) (src format: %d, dst format: %d), or "
          "varying component counts (src %d, dst %d)\n",
          image_bpp,
          desired_output_bpp,
          (int)data_format,
          (int)desired_output_format,
          image_components,
          num_output_components);
    }
  }
}

/* Remove once no longer required -- will just return 0 for now in MTL path. */
uint gpu::MTLTexture::gl_bindcode_get() const
{
  return 0;
}

bool gpu::MTLTexture::init_internal()
{
  if (format_ == GPU_DEPTH24_STENCIL8) {
    /* Apple Silicon requires GPU_DEPTH32F_STENCIL8 instead of GPU_DEPTH24_STENCIL8. */
    format_ = GPU_DEPTH32F_STENCIL8;
  }

  this->prepare_internal();
  return true;
}

bool gpu::MTLTexture::init_internal(GPUVertBuf *vbo)
{
  /* Zero initialize. */
  this->prepare_internal();

  /* TODO(Metal): Add implementation for GPU Vert buf. */
  return false;
}

bool gpu::MTLTexture::init_internal(const GPUTexture *src, int mip_offset, int layer_offset)
{
  BLI_assert(src);

  /* Zero initialize. */
  this->prepare_internal();

  /* Flag as using texture view. */
  resource_mode_ = MTL_TEXTURE_MODE_TEXTURE_VIEW;
  source_texture_ = src;
  mip_texture_base_level_ = mip_offset;
  mip_texture_base_layer_ = layer_offset;

  /* Assign texture as view. */
  const gpu::MTLTexture *mtltex = static_cast<const gpu::MTLTexture *>(unwrap(src));
  texture_ = mtltex->texture_;
  BLI_assert(texture_);
  [texture_ retain];

  /* Flag texture as baked -- we do not need explicit initialization. */
  is_baked_ = true;
  is_dirty_ = false;

  /* Bake mip swizzle view. */
  bake_mip_swizzle_view();
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name METAL Resource creation and management
 * \{ */

bool gpu::MTLTexture::texture_is_baked()
{
  return is_baked_;
}

/* Prepare texture parameters after initialization, but before baking. */
void gpu::MTLTexture::prepare_internal()
{

  /* Derive implicit usage flags for Depth/Stencil attachments. */
  if (format_flag_ & GPU_FORMAT_DEPTH || format_flag_ & GPU_FORMAT_STENCIL) {
    gpu_image_usage_flags_ |= GPU_TEXTURE_USAGE_ATTACHMENT;
  }

  /* Derive maximum number of mip levels by default.
   * TODO(Metal): This can be removed if max mip counts are specified upfront. */
  if (type_ == GPU_TEXTURE_1D || type_ == GPU_TEXTURE_1D_ARRAY || type_ == GPU_TEXTURE_BUFFER) {
    mtl_max_mips_ = 1;
  }
  else {
    int effective_h = (type_ == GPU_TEXTURE_1D_ARRAY) ? 0 : h_;
    int effective_d = (type_ != GPU_TEXTURE_3D) ? 0 : d_;
    int max_dimension = max_iii(w_, effective_h, effective_d);
    int max_miplvl = max_ii(floor(log2(max_dimension)) + 1, 1);
    mtl_max_mips_ = max_miplvl;
  }
}

void gpu::MTLTexture::ensure_baked()
{

  /* If properties have changed, re-bake. */
  bool copy_previous_contents = false;
  if (is_baked_ && is_dirty_) {
    copy_previous_contents = true;
    id<MTLTexture> previous_texture = texture_;
    [previous_texture retain];

    this->reset();
  }

  if (!is_baked_) {
    MTLContext *ctx = static_cast<MTLContext *>(unwrap(GPU_context_active_get()));
    BLI_assert(ctx);

    /* Ensure texture mode is valid. */
    BLI_assert(resource_mode_ != MTL_TEXTURE_MODE_EXTERNAL);
    BLI_assert(resource_mode_ != MTL_TEXTURE_MODE_TEXTURE_VIEW);
    BLI_assert(resource_mode_ != MTL_TEXTURE_MODE_VBO);

    /* Format and mip levels (TODO(Metal): Optimize mipmaps counts, specify up-front). */
    MTLPixelFormat mtl_format = gpu_texture_format_to_metal(format_);

    /* Create texture descriptor. */
    switch (type_) {

      /* 1D */
      case GPU_TEXTURE_1D:
      case GPU_TEXTURE_1D_ARRAY: {
        BLI_assert(w_ > 0);
        texture_descriptor_ = [[MTLTextureDescriptor alloc] init];
        texture_descriptor_.pixelFormat = mtl_format;
        texture_descriptor_.textureType = (type_ == GPU_TEXTURE_1D_ARRAY) ? MTLTextureType1DArray :
                                                                            MTLTextureType1D;
        texture_descriptor_.width = w_;
        texture_descriptor_.height = 1;
        texture_descriptor_.depth = 1;
        texture_descriptor_.arrayLength = (type_ == GPU_TEXTURE_1D_ARRAY) ? h_ : 1;
        texture_descriptor_.mipmapLevelCount = (mtl_max_mips_ > 0) ? mtl_max_mips_ : 1;
        texture_descriptor_.usage =
            MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite |
            MTLTextureUsagePixelFormatView; /* TODO(Metal): Optimize usage flags. */
        texture_descriptor_.storageMode = MTLStorageModePrivate;
        texture_descriptor_.sampleCount = 1;
        texture_descriptor_.cpuCacheMode = MTLCPUCacheModeDefaultCache;
        texture_descriptor_.hazardTrackingMode = MTLHazardTrackingModeDefault;
      } break;

      /* 2D */
      case GPU_TEXTURE_2D:
      case GPU_TEXTURE_2D_ARRAY: {
        BLI_assert(w_ > 0 && h_ > 0);
        texture_descriptor_ = [[MTLTextureDescriptor alloc] init];
        texture_descriptor_.pixelFormat = mtl_format;
        texture_descriptor_.textureType = (type_ == GPU_TEXTURE_2D_ARRAY) ? MTLTextureType2DArray :
                                                                            MTLTextureType2D;
        texture_descriptor_.width = w_;
        texture_descriptor_.height = h_;
        texture_descriptor_.depth = 1;
        texture_descriptor_.arrayLength = (type_ == GPU_TEXTURE_2D_ARRAY) ? d_ : 1;
        texture_descriptor_.mipmapLevelCount = (mtl_max_mips_ > 0) ? mtl_max_mips_ : 1;
        texture_descriptor_.usage =
            MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite |
            MTLTextureUsagePixelFormatView; /* TODO(Metal): Optimize usage flags. */
        texture_descriptor_.storageMode = MTLStorageModePrivate;
        texture_descriptor_.sampleCount = 1;
        texture_descriptor_.cpuCacheMode = MTLCPUCacheModeDefaultCache;
        texture_descriptor_.hazardTrackingMode = MTLHazardTrackingModeDefault;
      } break;

      /* 3D */
      case GPU_TEXTURE_3D: {
        BLI_assert(w_ > 0 && h_ > 0 && d_ > 0);
        texture_descriptor_ = [[MTLTextureDescriptor alloc] init];
        texture_descriptor_.pixelFormat = mtl_format;
        texture_descriptor_.textureType = MTLTextureType3D;
        texture_descriptor_.width = w_;
        texture_descriptor_.height = h_;
        texture_descriptor_.depth = d_;
        texture_descriptor_.arrayLength = 1;
        texture_descriptor_.mipmapLevelCount = (mtl_max_mips_ > 0) ? mtl_max_mips_ : 1;
        texture_descriptor_.usage =
            MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite |
            MTLTextureUsagePixelFormatView; /* TODO(Metal): Optimize usage flags. */
        texture_descriptor_.storageMode = MTLStorageModePrivate;
        texture_descriptor_.sampleCount = 1;
        texture_descriptor_.cpuCacheMode = MTLCPUCacheModeDefaultCache;
        texture_descriptor_.hazardTrackingMode = MTLHazardTrackingModeDefault;
      } break;

      /* CUBE TEXTURES */
      case GPU_TEXTURE_CUBE:
      case GPU_TEXTURE_CUBE_ARRAY: {
        /* NOTE: For a cube-map 'Texture::d_' refers to total number of faces,
         * not just array slices. */
        BLI_assert(w_ > 0 && h_ > 0);
        texture_descriptor_ = [[MTLTextureDescriptor alloc] init];
        texture_descriptor_.pixelFormat = mtl_format;
        texture_descriptor_.textureType = (type_ == GPU_TEXTURE_CUBE_ARRAY) ?
                                              MTLTextureTypeCubeArray :
                                              MTLTextureTypeCube;
        texture_descriptor_.width = w_;
        texture_descriptor_.height = h_;
        texture_descriptor_.depth = 1;
        texture_descriptor_.arrayLength = (type_ == GPU_TEXTURE_CUBE_ARRAY) ? d_ / 6 : 1;
        texture_descriptor_.mipmapLevelCount = (mtl_max_mips_ > 0) ? mtl_max_mips_ : 1;
        texture_descriptor_.usage =
            MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite |
            MTLTextureUsagePixelFormatView; /* TODO(Metal): Optimize usage flags. */
        texture_descriptor_.storageMode = MTLStorageModePrivate;
        texture_descriptor_.sampleCount = 1;
        texture_descriptor_.cpuCacheMode = MTLCPUCacheModeDefaultCache;
        texture_descriptor_.hazardTrackingMode = MTLHazardTrackingModeDefault;
      } break;

      /* GPU_TEXTURE_BUFFER */
      case GPU_TEXTURE_BUFFER: {
        texture_descriptor_ = [[MTLTextureDescriptor alloc] init];
        texture_descriptor_.pixelFormat = mtl_format;
        texture_descriptor_.textureType = MTLTextureTypeTextureBuffer;
        texture_descriptor_.width = w_;
        texture_descriptor_.height = 1;
        texture_descriptor_.depth = 1;
        texture_descriptor_.arrayLength = 1;
        texture_descriptor_.mipmapLevelCount = (mtl_max_mips_ > 0) ? mtl_max_mips_ : 1;
        texture_descriptor_.usage =
            MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite |
            MTLTextureUsagePixelFormatView; /* TODO(Metal): Optimize usage flags. */
        texture_descriptor_.storageMode = MTLStorageModePrivate;
        texture_descriptor_.sampleCount = 1;
        texture_descriptor_.cpuCacheMode = MTLCPUCacheModeDefaultCache;
        texture_descriptor_.hazardTrackingMode = MTLHazardTrackingModeDefault;
      } break;

      default: {
        MTL_LOG_ERROR("[METAL] Error: Cannot create texture with unknown type: %d\n", type_);
        return;
      } break;
    }

    /* Determine Resource Mode. */
    resource_mode_ = MTL_TEXTURE_MODE_DEFAULT;

    /* Create texture. */
    texture_ = [ctx->device newTextureWithDescriptor:texture_descriptor_];

    [texture_descriptor_ release];
    texture_descriptor_ = nullptr;
    texture_.label = [NSString stringWithUTF8String:this->get_name()];
    BLI_assert(texture_);
    is_baked_ = true;
    is_dirty_ = false;
  }

  /* Re-apply previous contents. */
  if (copy_previous_contents) {
    id<MTLTexture> previous_texture;
    /* TODO(Metal): May need to copy previous contents of texture into new texture. */
    /*[previous_texture release]; */
    UNUSED_VARS(previous_texture);
  }
}

void gpu::MTLTexture::reset()
{

  MTL_LOG_INFO("Texture %s reset. Size %d, %d, %d\n", this->get_name(), w_, h_, d_);
  /* Delete associated METAL resources. */
  if (texture_ != nil) {
    [texture_ release];
    texture_ = nil;
    is_baked_ = false;
    is_dirty_ = true;
  }

  if (mip_swizzle_view_ != nil) {
    [mip_swizzle_view_ release];
    mip_swizzle_view_ = nil;
  }

  if (texture_buffer_ != nil) {
    [texture_buffer_ release];
  }

  /* Blit framebuffer. */
  if (blit_fb_) {
    GPU_framebuffer_free(blit_fb_);
    blit_fb_ = nullptr;
  }

  BLI_assert(texture_ == nil);
  BLI_assert(mip_swizzle_view_ == nil);
}

/** \} */

}  // namespace blender::gpu
