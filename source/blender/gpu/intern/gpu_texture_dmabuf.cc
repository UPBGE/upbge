/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * Dispatcher for DMA-BUF texture creation and update.
 * All real work is in GLTexture::init_internal_dmabuf (EGL/Linux only).
 */

#include "GPU_texture.hh"
#include "gpu_texture_private.hh"

#ifdef __linux__
#ifdef WITH_OPENGL_BACKEND
#    include "epoxy/egl.h"
#    include "epoxy/gl.h"
#    include "opengl/gl_texture.hh"
#  endif /*WITH_OPENGL_BACKEND */
#endif /* __linux__ */

namespace blender {

#ifdef __linux__
#ifdef WITH_OPENGL_BACKEND

gpu::Texture *GPU_texture_create_from_dmabuf(
    const char *name, int w, int h, int stride, int drm_format, gpu::TextureFormat format, int fd)
{
  gpu::Texture *tex = GPU_texture_create_2d(
      name, w, h, 1, format, GPU_TEXTURE_USAGE_SHADER_READ, nullptr);
  if (tex == nullptr) {
    return nullptr;
  }
  auto *gl_tex = static_cast<gpu::GLTexture *>(tex);
  if (!gl_tex->init_internal_dmabuf(fd, stride, drm_format)) {
    GPU_texture_free(tex);
    return nullptr;
  }
  return tex;
}

bool GPU_texture_update_dmabuf(gpu::Texture *texture, int fd, int stride, int drm_format)
{
  return static_cast<gpu::GLTexture *>(texture)->init_internal_dmabuf(fd, stride, drm_format);
}

#endif /* WITH_OPENGL_BACKEND */
#endif   /* __linux__ */

}  // namespace blender
