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

#include "GPU_framebuffer.h"
#include "GPU_texture.h"

RAS_FrameBuffer::RAS_FrameBuffer(unsigned int width,
                                 unsigned int height,
                                 RAS_Rasterizer::FrameBufferType fbtype)
    : m_frameBuffer(nullptr), m_frameBufferType(fbtype)
{
  m_colorAttachment = GPU_texture_create_2d("color_tex", width, height, 1, GPU_RGBA16F, nullptr);
  m_depthAttachment = GPU_texture_create_2d("depth_tex", width, height, 1, GPU_DEPTH24_STENCIL8, nullptr);
  m_frameBuffer = GPU_framebuffer_create("game_fb");
  GPU_framebuffer_texture_attach(m_frameBuffer, m_colorAttachment, 0, 0);
  GPU_framebuffer_texture_attach(m_frameBuffer, m_depthAttachment, 0, 0);
}

RAS_FrameBuffer::~RAS_FrameBuffer()
{
  GPU_framebuffer_free(m_frameBuffer);  // it detaches attachments
  GPU_texture_free(m_colorAttachment);
  GPU_texture_free(m_depthAttachment);
}

GPUFrameBuffer *RAS_FrameBuffer::GetFrameBuffer()
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

GPUTexture *RAS_FrameBuffer::GetColorAttachment()
{
  return m_colorAttachment;
}

GPUTexture *RAS_FrameBuffer::GetDepthAttachment()
{
  return m_depthAttachment;
}

RAS_Rasterizer::FrameBufferType RAS_FrameBuffer::GetType() const
{
  return m_frameBufferType;
}
