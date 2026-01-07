/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Contributor(s): Tristan Porteries.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/Rasterizer/RAS_FrameBuffer.cpp
 *  \ingroup bgerast
 */

#include "RAS_FrameBuffer.h"

#ifdef WITH_PYTHON
#include "../../blender/python/gpu/gpu_py_texture.hh"
#endif

#include "GPU_framebuffer.hh"

using namespace blender;

RAS_FrameBuffer::RAS_FrameBuffer(unsigned int width,
                                 unsigned int height,
                                 RAS_Rasterizer::FrameBufferType fbtype)
    : m_frameBuffer(nullptr), m_frameBufferType(fbtype)
{
  m_colorAttachment = GPU_texture_create_2d("color_tex",
                                            width,
                                            height,
                                            1,
                                            blender::gpu::TextureFormat::SFLOAT_16_16_16_16,
                                            GPU_TEXTURE_USAGE_SHADER_READ |
                                                GPU_TEXTURE_USAGE_ATTACHMENT,
                                            nullptr);
  m_depthAttachment = GPU_texture_create_2d("depth_tex",
                                            width,
                                            height,
                                            1,
                                            blender::gpu::TextureFormat::SFLOAT_32_DEPTH_UINT_8,
                                            GPU_TEXTURE_USAGE_SHADER_READ |
                                                GPU_TEXTURE_USAGE_ATTACHMENT,
                                            nullptr);
  m_frameBuffer = GPU_framebuffer_create("game_fb");
  GPUAttachment config[] = {
      GPU_ATTACHMENT_TEXTURE(m_depthAttachment),
      GPU_ATTACHMENT_TEXTURE(m_colorAttachment)};

  GPU_framebuffer_config_array(
      m_frameBuffer, config, sizeof(config) / sizeof(GPUAttachment));

  m_py_color = nullptr;
  m_py_depth = nullptr;

#ifdef WITH_PYTHON
  m_py_color = BPyGPUTexture_CreatePyObject(m_colorAttachment, false);
  Py_INCREF(m_py_color);
  m_py_depth = BPyGPUTexture_CreatePyObject(m_depthAttachment, false);
  Py_INCREF(m_py_depth);
#endif
}

RAS_FrameBuffer::~RAS_FrameBuffer()
{
  GPU_framebuffer_free(m_frameBuffer);  // it detaches attachments
  GPU_texture_free(m_colorAttachment);
  GPU_texture_free(m_depthAttachment);

#ifdef WITH_PYTHON
  Py_XDECREF(m_py_color);
  m_py_color = nullptr;
  Py_XDECREF(m_py_depth);
  m_py_depth = nullptr;
#endif
}

blender::gpu::FrameBuffer *RAS_FrameBuffer::GetFrameBuffer()
{
  return m_frameBuffer;
}

unsigned int RAS_FrameBuffer::GetWidth() const
{
  return GPU_texture_width(m_colorAttachment);
}

unsigned int RAS_FrameBuffer::GetHeight() const
{
  return GPU_texture_height(m_colorAttachment);
}

blender::gpu::Texture *RAS_FrameBuffer::GetColorAttachment()
{
  return m_colorAttachment;
}

blender::gpu::Texture *RAS_FrameBuffer::GetDepthAttachment()
{
  return m_depthAttachment;
}

void RAS_FrameBuffer::UpdateSize(int width, int height)
{
  if (GPU_texture_width(m_colorAttachment) != width ||
      GPU_texture_height(m_colorAttachment) != height)
  {
    GPU_framebuffer_free(m_frameBuffer);
    GPU_texture_free(m_colorAttachment);
    GPU_texture_free(m_depthAttachment);
    m_colorAttachment = GPU_texture_create_2d("color_tex",
                                              width,
                                              height,
                                              1,
                                              blender::gpu::TextureFormat::SFLOAT_16_16_16_16,
                                              GPU_TEXTURE_USAGE_SHADER_READ |
                                                  GPU_TEXTURE_USAGE_ATTACHMENT,
                                              nullptr);
    m_depthAttachment = GPU_texture_create_2d("depth_tex",
                                              width,
                                              height,
                                              1,
                                              blender::gpu::TextureFormat::SFLOAT_32_DEPTH_UINT_8,
                                              GPU_TEXTURE_USAGE_SHADER_READ |
                                                  GPU_TEXTURE_USAGE_ATTACHMENT,
                                              nullptr);
    m_frameBuffer = GPU_framebuffer_create("game_fb");
    GPUAttachment config[] = {GPU_ATTACHMENT_TEXTURE(m_depthAttachment),
                              GPU_ATTACHMENT_TEXTURE(m_colorAttachment)};

    GPU_framebuffer_config_array(m_frameBuffer, config, sizeof(config) / sizeof(GPUAttachment));

#ifdef WITH_PYTHON
    Py_XDECREF(m_py_color);
    m_py_color = nullptr;
    Py_XDECREF(m_py_depth);
    m_py_depth = nullptr;
    m_py_color = BPyGPUTexture_CreatePyObject(m_colorAttachment, false);
    Py_INCREF(m_py_color);
    m_py_depth = BPyGPUTexture_CreatePyObject(m_depthAttachment, false);
    Py_INCREF(m_py_depth);
#endif
  }
}

RAS_Rasterizer::FrameBufferType RAS_FrameBuffer::GetType() const
{
  return m_frameBufferType;
}
