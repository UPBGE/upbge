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
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file RAS_OpenGLRasterizer.h
 *  \ingroup bgerastogl
 */

#pragma once

#ifdef _MSC_VER
#  pragma warning(disable : 4786)
#endif

#include "RAS_Rasterizer.h"

/**
 * 3D rendering device context.
 */
class RAS_OpenGLRasterizer {
 private:
  class ScreenPlane {
   private:
    unsigned int m_vbo;
    unsigned int m_ibo;
    unsigned int m_vao;

   public:
    ScreenPlane();
    ~ScreenPlane();

    void Render();
  };

  /// Class used to render a screen plane.
  ScreenPlane m_screenPlane;

  RAS_Rasterizer *m_rasterizer;

 public:
  RAS_OpenGLRasterizer(RAS_Rasterizer *rasterizer);
  virtual ~RAS_OpenGLRasterizer();

  unsigned short GetNumLights() const;

  void Enable(RAS_Rasterizer::EnableBit bit);
  void Disable(RAS_Rasterizer::EnableBit bit);

  void SetDepthFunc(RAS_Rasterizer::DepthFunc func);
  void SetDepthMask(RAS_Rasterizer::DepthMask depthmask);

  void SetBlendFunc(RAS_Rasterizer::BlendFunc src, RAS_Rasterizer::BlendFunc dst);

  unsigned int *MakeScreenshot(int x, int y, int width, int height);

  void Init();
  void Exit();
  void DrawOverlayPlane();
  void BeginFrame();
  void Clear(int clearbit);
  void SetClearColor(float r, float g, float b, float a = 1.0f);
  void SetClearDepth(float d);
  void SetColorMask(bool r, bool g, bool b, bool a);
  void EndFrame();

  void SetViewport(int x, int y, int width, int height);
  void SetScissor(int x, int y, int width, int height);

  void SetLines(bool enable);

  void SetAmbient(const MT_Vector3 &amb, float factor);

  void SetPolygonOffset(float mult, float add);

  void EnableClipPlane(int numplanes);
  void DisableClipPlane(int numplanes);

  void SetFrontFace(bool ccw);

  /**
   * Render Tools
   */

  void DisableForText();

  /**
   * Prints information about what the hardware supports.
   */
  void PrintHardwareInfo();

  const unsigned char *GetGraphicsCardVendor();
};
