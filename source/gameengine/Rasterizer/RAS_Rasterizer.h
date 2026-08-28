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

/** \file RAS_Rasterizer.h
 *  \ingroup bgerast
 */

#pragma once

#ifdef _MSC_VER
#  pragma warning(disable : 4786)
#endif

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "MT_Matrix4x4.h"
#include "RAS_DebugDraw.h"
#include "RAS_Rect.h"

namespace blender::gpu {
class Texture;
}

class RAS_OpenGLRasterizer;
class RAS_FrameBuffer;
class RAS_ICanvas;
struct KX_ClientObjectInfo;
class KX_RayCast;

typedef struct ViewPortMatrices {
  MT_Matrix4x4 view;
  MT_Matrix4x4 viewinv;
  MT_Matrix4x4 proj;
  MT_Matrix4x4 projinv;
  MT_Matrix4x4 pers;
  MT_Matrix4x4 persinv;
} ViewPortMatrices;

/**
 * 3D rendering device context interface.
 */
class RAS_Rasterizer {
 public:
  enum FrameBufferType {
    RAS_FRAMEBUFFER_FILTER0 = 0,
    RAS_FRAMEBUFFER_FILTER1,
    RAS_FRAMEBUFFER_RENDER0,
    RAS_FRAMEBUFFER_RENDER1,
    RAS_FRAMEBUFFER_MAX,

    RAS_FRAMEBUFFER_CUSTOM,
  };

  /** Return the output frame buffer normally used for the input frame buffer
   * index in case of filters render.
   * \param index The input frame buffer, can be a non-filter frame buffer.
   * \return The output filter frame buffer.
   */
  static RAS_Rasterizer::FrameBufferType NextFilterFrameBuffer(FrameBufferType index);

  /** Return the output frame buffer normally used for the input frame buffer
   * index in case of simple render.
   * \param index The input render frame buffer, can be a eye frame buffer.
   * \return The output render frame buffer.
   */
  static RAS_Rasterizer::FrameBufferType NextRenderFrameBuffer(FrameBufferType index);

 private:
  class FrameBuffers {
   private:
    RAS_FrameBuffer *m_frameBuffers[RAS_FRAMEBUFFER_MAX];

    /* We need to free all textures at ge exit so we do member variables */
    blender::gpu::Texture *m_colorTextureList[RAS_FRAMEBUFFER_MAX];
    blender::gpu::Texture *m_depthTextureList[RAS_FRAMEBUFFER_MAX];
    unsigned int m_width;
    unsigned int m_height;
    int m_samples;

   public:
    FrameBuffers();
    ~FrameBuffers();

    void Update(RAS_ICanvas *canvas);
    RAS_FrameBuffer *GetFrameBuffer(FrameBufferType type);
  };

  ViewPortMatrices m_matrices;

  // We store each debug shape by scene.
  RAS_DebugDraw m_debugDraw;

  double m_time;
  MT_Matrix4x4 m_viewmatrix;
  MT_Matrix4x4 m_viewinvmatrix;
  MT_Vector3 m_campos;
  bool m_camortho;
  bool m_camnegscale;

  /* Render tools */
  void *m_clientobject;
  void *m_auxilaryClientInfo;
  void *m_lastauxinfo;

  /// Class used to manage off screens used by the rasterizer.
  FrameBuffers m_frameBuffers;

  std::unique_ptr<RAS_OpenGLRasterizer> m_impl;

 public:
  RAS_Rasterizer();
  virtual ~RAS_Rasterizer();

  /**
   * Takes a screenshot
   */
  unsigned int *MakeScreenshot(int x, int y, int width, int height);

  /**
   * Init initializes the renderer.
   */
  void Init(RAS_ICanvas *canvas);

  /**
   * Exit cleans up the renderer.
   */
  void Exit();

  /**
   * BeginFrame is called at the start of each frame.
   */
  void BeginFrame(double time);

  /**
   * EndFrame is called at the end of each frame.
   */
  void EndFrame();

  /// Update dimensions of all off screens.
  void UpdateFrameBuffers(RAS_ICanvas *canvas);

  /** Return the corresponding off screen to off screen type.
   * \param type The off screen type to return.
   */
  RAS_FrameBuffer *GetFrameBuffer(FrameBufferType type);

  /** Draw off screen without set viewport.
   * Used to copy the frame buffer object to another.
   * \param srcindex The input off screen index.
   * \param dstindex The output off screen index.
   */
  void DrawFrameBuffer(RAS_FrameBuffer *srcfb, RAS_FrameBuffer *dstfb);

  /** Draw off screen at the given index to screen.
   * \param canvas The canvas containing the screen viewport.
   * \param index The off screen index to read from.
   */
  void DrawFrameBuffer(RAS_ICanvas *canvas, RAS_FrameBuffer *frameBuffer);

  /**
   * GetRenderArea computes the render area from the 2d canvas.
   */
  RAS_Rect GetRenderArea(RAS_ICanvas *canvas);

  /// Get the modelview matrix according to the stereo settings.
  MT_Matrix4x4 GetViewMatrix(const MT_Transform &camtrans, bool perspective);
  /**
   * Sets the modelview matrix.
   */
  void SetMatrix(const MT_Matrix4x4 &viewmat,
                 const MT_Matrix4x4 &projmat,
                 const MT_Vector3 &pos,
                 const MT_Vector3 &scale);
  ViewPortMatrices GetAllMatrices();

  /**
   */
  const MT_Vector3 &GetCameraPosition();
  bool GetCameraOrtho();

  /**
   */
  double GetTime();

  /**
   * Generates a projection matrix from the specified frustum.
   * \param left the left clipping plane
   * \param right the right clipping plane
   * \param bottom the bottom clipping plane
   * \param top the top clipping plane
   * \param frustnear the near clipping plane
   * \param frustfar the far clipping plane
   * \return a 4x4 matrix representing the projection transform.
   */
  MT_Matrix4x4 GetFrustumMatrix(float left,
                                float right,
                                float bottom,
                                float top,
                                float frustnear,
                                float frustfar,
                                float focallength = 0.0f,
                                bool perspective = true);

  /**
   * Generates a orthographic projection matrix from the specified frustum.
   * \param left the left clipping plane
   * \param right the right clipping plane
   * \param bottom the bottom clipping plane
   * \param top the top clipping plane
   * \param frustnear the near clipping plane
   * \param frustfar the far clipping plane
   * \return a 4x4 matrix representing the projection transform.
   */
  MT_Matrix4x4 GetOrthoMatrix(
      float left, float right, float bottom, float top, float frustnear, float frustfar);

  RAS_DebugDraw &GetDebugDraw();
  void FlushDebugDraw(RAS_ICanvas *canvas);

  const MT_Matrix4x4 &GetViewMatrix() const;
  const MT_Matrix4x4 &GetViewInvMatrix() const;
  const MT_Matrix4x4 &GetProjMatrix() const;
  const MT_Matrix4x4 &GetProjInvMatrix() const;
  const MT_Matrix4x4 &GetPersMatrix() const;
  const MT_Matrix4x4 &GetPersInvMatrix() const;

  /**
   * Render Tools
   */

  void SetClientObject(void *obj);

  void SetAuxilaryClientInfo(void *inf);

  /**
   * Prints information about what the hardware supports.
   */
  void PrintHardwareInfo();

  const unsigned char *GetGraphicsCardVendor();
};
