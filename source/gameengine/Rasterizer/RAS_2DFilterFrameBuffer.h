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

/** \file RAS_2DFilterFrameBuffer.h
 *  \ingroup bgerast
 */

#pragma once

#include <memory>

#include "RAS_Rasterizer.h"

class RAS_ICanvas;
class RAS_FrameBuffer;
struct GPUTexture;

/** \brief This class manages an off screen with more than one color textures (sample-able)
 * and an optional depth texture (sample-able), contrary to GPUFrameBuffer.
 * This class is created, owned and unique per RAS_2DFilter to avoid implicit invalidation
 * of filter when the off screen is deleted or using an off screen in multiple filter or
 * different scenes.
 */
class RAS_2DFilterFrameBuffer {
 public:
  enum Flag { RAS_VIEWPORT_SIZE = (1 << 0), RAS_DEPTH = (1 << 1), RAS_MIPMAP = (1 << 2) };

  enum { NUM_COLOR_SLOTS = 8 };

 private:
  const Flag m_flag;
  const unsigned short m_colorSlots;

  unsigned int m_width;
  unsigned int m_height;

  RAS_FrameBuffer *m_frameBuffer;
  GPUTexture *m_colorTextures[NUM_COLOR_SLOTS];
  GPUTexture *m_depthTexture;

  /// Construct the frame buffer and the textures with the current settings.
  void Construct();
  /// Generate mipmap levels for color textures of the off screen.
  void MipmapTexture();

 public:
  RAS_2DFilterFrameBuffer(unsigned short colorSlots,
                          Flag flag,
                          unsigned int width,
                          unsigned int height);
  virtual ~RAS_2DFilterFrameBuffer();

  /** Update the off screen to the new canvas dimensions if allowed.
   * \return True if the off screen is valid.
   */
  bool Update(RAS_ICanvas *canvas);
  /// Bind the off screen and set the viewport before rendering to it.
  void Bind(RAS_Rasterizer *rasty);
  /// Restore the off screen and mipmap textures.
  void Unbind(RAS_Rasterizer *rasty, RAS_ICanvas *canvas);
  /// Return true of the off screen is valid from the OpenGL rules for frame buffers.
  bool GetValid() const;

  int GetColorBindCode(unsigned short index) const;
  int GetDepthBindCode() const;

  unsigned int GetWidth() const;
  unsigned int GetHeight() const;
};
