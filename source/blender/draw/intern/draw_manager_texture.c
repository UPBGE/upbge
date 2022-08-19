/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2016 Blender Foundation. */

/** \file
 * \ingroup draw
 */

#include "draw_manager.h"
#include "draw_texture_pool.h"

#ifndef NDEBUG
/* Maybe gpu_texture.c is a better place for this. */
static bool drw_texture_format_supports_framebuffer(eGPUTextureFormat format)
{
  /* Some formats do not work with frame-buffers. */
  switch (format) {
    /* Only add formats that are COMPATIBLE with FB.
     * Generally they are multiple of 16bit. */
    case GPU_R8:
    case GPU_R8UI:
    case GPU_R16F:
    case GPU_R16I:
    case GPU_R16UI:
    case GPU_R16:
    case GPU_R32F:
    case GPU_R32UI:
    case GPU_RG8:
    case GPU_RG16:
    case GPU_RG16F:
    case GPU_RG16I:
    case GPU_RG32F:
    case GPU_RGB10_A2:
    case GPU_R11F_G11F_B10F:
    case GPU_RGBA8:
    case GPU_RGBA16:
    case GPU_RGBA16F:
    case GPU_RGBA32F:
    case GPU_DEPTH_COMPONENT16:
    case GPU_DEPTH_COMPONENT24:
    case GPU_DEPTH24_STENCIL8:
    case GPU_DEPTH_COMPONENT32F:
      return true;
    default:
      return false;
  }
}
#endif

void drw_texture_set_parameters(GPUTexture *tex, DRWTextureFlag flags)
{
  if (tex == NULL) {
    return;
  }

  if (flags & DRW_TEX_MIPMAP) {
    GPU_texture_mipmap_mode(tex, true, flags & DRW_TEX_FILTER);
    GPU_texture_generate_mipmap(tex);
  }
  else {
    GPU_texture_filter_mode(tex, flags & DRW_TEX_FILTER);
  }
  GPU_texture_anisotropic_filter(tex, false);
  GPU_texture_wrap_mode(tex, flags & DRW_TEX_WRAP, true);
  GPU_texture_compare_mode(tex, flags & DRW_TEX_COMPARE);
}

GPUTexture *DRW_texture_create_1d(int w,
                                  eGPUTextureFormat format,
                                  DRWTextureFlag flags,
                                  const float *fpixels)
{
  int mip_len = (flags & DRW_TEX_MIPMAP) ? 9999 : 1;
  GPUTexture *tex = GPU_texture_create_1d(__func__, w, mip_len, format, fpixels);
  drw_texture_set_parameters(tex, flags);

  return tex;
}

GPUTexture *DRW_texture_create_2d(
    int w, int h, eGPUTextureFormat format, DRWTextureFlag flags, const float *fpixels)
{
  int mip_len = (flags & DRW_TEX_MIPMAP) ? 9999 : 1;
  GPUTexture *tex = GPU_texture_create_2d(__func__, w, h, mip_len, format, fpixels);
  drw_texture_set_parameters(tex, flags);

  return tex;
}

GPUTexture *DRW_texture_create_2d_array(
    int w, int h, int d, eGPUTextureFormat format, DRWTextureFlag flags, const float *fpixels)
{
  int mip_len = (flags & DRW_TEX_MIPMAP) ? 9999 : 1;
  GPUTexture *tex = GPU_texture_create_2d_array(__func__, w, h, d, mip_len, format, fpixels);
  drw_texture_set_parameters(tex, flags);

  return tex;
}

GPUTexture *DRW_texture_create_3d(
    int w, int h, int d, eGPUTextureFormat format, DRWTextureFlag flags, const float *fpixels)
{
  int mip_len = (flags & DRW_TEX_MIPMAP) ? 9999 : 1;
  GPUTexture *tex = GPU_texture_create_3d(
      __func__, w, h, d, mip_len, format, GPU_DATA_FLOAT, fpixels);
  drw_texture_set_parameters(tex, flags);

  return tex;
}

GPUTexture *DRW_texture_create_cube(int w,
                                    eGPUTextureFormat format,
                                    DRWTextureFlag flags,
                                    const float *fpixels)
{
  int mip_len = (flags & DRW_TEX_MIPMAP) ? 9999 : 1;
  GPUTexture *tex = GPU_texture_create_cube(__func__, w, mip_len, format, fpixels);
  drw_texture_set_parameters(tex, flags);
  return tex;
}

GPUTexture *DRW_texture_create_cube_array(
    int w, int d, eGPUTextureFormat format, DRWTextureFlag flags, const float *fpixels)
{
  int mip_len = (flags & DRW_TEX_MIPMAP) ? 9999 : 1;
  GPUTexture *tex = GPU_texture_create_cube_array(__func__, w, d, mip_len, format, fpixels);
  drw_texture_set_parameters(tex, flags);
  return tex;
}

GPUTexture *DRW_texture_pool_query_2d(int w,
                                      int h,
                                      eGPUTextureFormat format,
                                      DrawEngineType *engine_type)
{
  BLI_assert(drw_texture_format_supports_framebuffer(format));
  GPUTexture *tex = DRW_texture_pool_query(DST.vmempool->texture_pool, w, h, format, engine_type);

  return tex;
}

GPUTexture *DRW_texture_pool_query_fullscreen(eGPUTextureFormat format,
                                              DrawEngineType *engine_type)
{
  const float *size = DRW_viewport_size_get();
  return DRW_texture_pool_query_2d((int)size[0], (int)size[1], format, engine_type);
}

void DRW_texture_ensure_fullscreen_2d(GPUTexture **tex,
                                      eGPUTextureFormat format,
                                      DRWTextureFlag flags)
{
  if (*(tex) == NULL) {
    const float *size = DRW_viewport_size_get();
    *(tex) = DRW_texture_create_2d((int)size[0], (int)size[1], format, flags, NULL);
  }
}

void DRW_texture_ensure_2d(
    GPUTexture **tex, int w, int h, eGPUTextureFormat format, DRWTextureFlag flags)
{
  if (*(tex) == NULL) {
    *(tex) = DRW_texture_create_2d(w, h, format, flags, NULL);
  }
}

void DRW_texture_generate_mipmaps(GPUTexture *tex)
{
  GPU_texture_generate_mipmap(tex);
}

void DRW_texture_free(GPUTexture *tex)
{
  GPU_texture_free(tex);
}
