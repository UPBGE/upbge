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

/** \file gameengine/Rasterizer/RAS_2DFilterFrameBuffer.cpp
 *  \ingroup bgerast
 */

#include "RAS_2DFilterFrameBuffer.h"

#include "GPU_framebuffer.hh"
#include "GPU_state.hh"

#include "RAS_FrameBuffer.h"
#include "RAS_ICanvas.h"

RAS_2DFilterFrameBuffer::RAS_2DFilterFrameBuffer(unsigned short colorSlots,
                                                 Flag flag,
                                                 unsigned int width,
                                                 unsigned int height)
    : m_flag(flag),
      m_colorSlots(colorSlots),
      m_width(width),
      m_height(height),
      m_frameBuffer(nullptr),
      m_depthTexture(nullptr)
{
  for (unsigned short i = 0; i < NUM_COLOR_SLOTS; ++i) {
    m_colorTextures[i] = nullptr;
  }

  if (!(m_flag & RAS_VIEWPORT_SIZE)) {
    Construct();
  }

  if (m_frameBuffer) {
    delete m_frameBuffer;
  }
}

RAS_2DFilterFrameBuffer::~RAS_2DFilterFrameBuffer()
{
  if (m_frameBuffer) {
    GPU_framebuffer_free(m_frameBuffer->GetFrameBuffer());
  }
  for (unsigned short i = 0; i < NUM_COLOR_SLOTS; ++i) {
    GPUTexture *texture = m_colorTextures[i];
    if (texture) {
      GPU_texture_free(texture);
      GPU_texture_free(m_depthTexture);
    }
  }
}

void RAS_2DFilterFrameBuffer::Construct()
{
  m_frameBuffer = new RAS_FrameBuffer(m_width, m_height, RAS_Rasterizer::RAS_FRAMEBUFFER_CUSTOM);
  /* TODO: RESTORE SUPPORT OF MULTIPLE COLOR ATTACHEMENTS IF NEEDED */
  m_colorTextures[0] = m_frameBuffer->GetColorAttachment();
  m_depthTexture = m_frameBuffer->GetDepthAttachment();
}

void RAS_2DFilterFrameBuffer::MipmapTexture()
{
  for (unsigned short i = 0; i < m_colorSlots; ++i) {
    GPUTexture *texture = m_colorTextures[i];
    GPU_texture_bind(texture, 0);
    GPU_apply_state();
    GPU_texture_filter_mode(texture, true);
    GPU_texture_mipmap_mode(texture, true, false);
    GPU_texture_update_mipmap_chain(texture);
    GPU_texture_unbind(texture);
  }
}

bool RAS_2DFilterFrameBuffer::Update(RAS_ICanvas *canvas)
{
  if (m_flag & RAS_VIEWPORT_SIZE) {
    const unsigned int width = canvas->GetWidth() + 1;
    const unsigned int height = canvas->GetHeight() + 1;
    if (m_width != width || m_height != height) {
      m_width = width;
      m_height = height;

      Construct();
    }
  }

  return GetValid();
}

void RAS_2DFilterFrameBuffer::Bind(RAS_Rasterizer *rasty)
{
  GPU_framebuffer_bind(m_frameBuffer->GetFrameBuffer());
  if (!(m_flag & RAS_VIEWPORT_SIZE)) {
    rasty->SetViewport(0, 0, m_width + 1, m_height + 1);
    rasty->Enable(RAS_Rasterizer::RAS_SCISSOR_TEST);
    GPU_scissor_test(true);
    rasty->SetScissor(0, 0, m_width + 1, m_height + 1);
  }
}

void RAS_2DFilterFrameBuffer::Unbind(RAS_Rasterizer *rasty, RAS_ICanvas *canvas)
{
  if (m_flag & RAS_MIPMAP) {
    MipmapTexture();
  }

  if (!(m_flag & RAS_VIEWPORT_SIZE)) {
    const int width = canvas->GetWidth();
    const int height = canvas->GetHeight();
    rasty->SetViewport(0, 0, width + 1, height + 1);
    rasty->Enable(RAS_Rasterizer::RAS_SCISSOR_TEST);
    GPU_scissor_test(true);
    rasty->SetScissor(0, 0, width + 1, height + 1);
  }
}

bool RAS_2DFilterFrameBuffer::GetValid() const
{
  return GPU_framebuffer_check_valid(m_frameBuffer->GetFrameBuffer(), nullptr);
}

int RAS_2DFilterFrameBuffer::GetColorBindCode(unsigned short index) const
{
  if (!m_colorTextures[index]) {
    return -1;
  }

  return GPU_texture_opengl_bindcode(m_colorTextures[index]);
}

int RAS_2DFilterFrameBuffer::GetDepthBindCode() const
{
  if (!m_depthTexture) {
    return -1;
  }

  return GPU_texture_opengl_bindcode(m_depthTexture);
}

GPUTexture *RAS_2DFilterFrameBuffer::GetColorTexture(int slot)
{
  if (!m_colorTextures[slot]) {
    return nullptr;
  }
  GPUTexture *texture = m_colorTextures[slot];
  return texture;
}

GPUTexture *RAS_2DFilterFrameBuffer::GetDepthTexture()
{
  if (!m_depthTexture) {
    return nullptr;
  }
  GPUTexture *texture = m_depthTexture;
  return texture;
}

unsigned int RAS_2DFilterFrameBuffer::GetWidth() const
{
  return m_width;
}

unsigned int RAS_2DFilterFrameBuffer::GetHeight() const
{
  return m_height;
}
