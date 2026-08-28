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

/** \file gameengine/Rasterizer/RAS_Rasterizer.cpp
 *  \ingroup bgerastogl
 */

#include "RAS_Rasterizer.h"

#include "KX_GameObject.h"
#include "KX_RayCast.h"
#include "RAS_FrameBuffer.h"
#include "RAS_ICanvas.h"
#include "RAS_IPolygonMaterial.h"
#include "RAS_OpenGLRasterizer.h"
#include "RAS_Polygon.h"

#include "BLI_math_geom_c.hh"
#include "BLI_math_matrix_c.hh"
#include "GPU_framebuffer.hh"
#include "GPU_immediate.hh"
#include "GPU_matrix.hh"
#include "GPU_state.hh"

using namespace blender;

RAS_Rasterizer::FrameBuffers::FrameBuffers() : m_width(0), m_height(0), m_samples(0)
{
  for (int i = 0; i < RAS_FRAMEBUFFER_MAX; i++) {
    m_frameBuffers[i] = nullptr;
  }
}

RAS_Rasterizer::FrameBuffers::~FrameBuffers()
{
  /* Free FrameBuffer Textures Attachments */
  for (int i = 0; i < RAS_FRAMEBUFFER_MAX; i++) {
    if (m_frameBuffers[i]) {
      delete m_frameBuffers[i];
    }
  }
}

inline void RAS_Rasterizer::FrameBuffers::Update(RAS_ICanvas *canvas)
{
  const unsigned int width = canvas->GetWidth();
  const unsigned int height = canvas->GetHeight();

  if (width == m_width && height == m_height) {
    // No resize detected.
    return;
  }

  m_width = width;
  m_height = height;
  m_samples = canvas->GetSamples();

  // Destruct all off screens.
  for (unsigned short i = 0; i < RAS_FRAMEBUFFER_MAX; ++i) {
    m_frameBuffers[i] = nullptr;
  }
}

inline RAS_FrameBuffer *RAS_Rasterizer::FrameBuffers::GetFrameBuffer(FrameBufferType fbtype)
{
  if (!m_frameBuffers[fbtype]) {
    // The offscreen need to be created now.

    /* Some GPUs doesn't support high multisample value with GL_RGBA16F or GL_RGBA32F.
     * To avoid crashing we check if the off screen was created and if not decremente
     * the multisample value and try to create the off screen to find a supported value.
     */
    for (int samples = m_samples; samples >= 0; --samples) {

      RAS_FrameBuffer *fb = new RAS_FrameBuffer(m_width, m_height, fbtype);

      if (!fb->GetFrameBuffer()) {
        delete fb;
        continue;
      }
      m_frameBuffers[fbtype] = fb;
      m_samples = samples;
      break;
    }
  }
  return m_frameBuffers[fbtype];
}

RAS_Rasterizer::FrameBufferType RAS_Rasterizer::NextFilterFrameBuffer(FrameBufferType type)
{
  switch (type) {
    case RAS_FRAMEBUFFER_FILTER0: {
      return RAS_FRAMEBUFFER_FILTER1;
    }
    case RAS_FRAMEBUFFER_FILTER1:
    // Passing a non-filter frame buffer is allowed.
    default: {
      return RAS_FRAMEBUFFER_FILTER0;
    }
  }
}

RAS_Rasterizer::FrameBufferType RAS_Rasterizer::NextRenderFrameBuffer(FrameBufferType type)
{
  switch (type) {
    case RAS_FRAMEBUFFER_RENDER0: {
      return RAS_FRAMEBUFFER_RENDER1;
    }
    case RAS_FRAMEBUFFER_RENDER1: {
      return RAS_FRAMEBUFFER_RENDER0;
    }
    // Passing a non-render frame buffer is disallowed.
    default: {
      BLI_assert(false);
      return RAS_FRAMEBUFFER_RENDER0;
    }
  }
}

RAS_Rasterizer::RAS_Rasterizer()
    : m_time(0.0f),
      m_campos(0.0f, 0.0f, 0.0f),
      m_camortho(false),
      m_camnegscale(false),
      m_clientobject(nullptr),
      m_auxilaryClientInfo(nullptr)
{
  m_impl.reset(new RAS_OpenGLRasterizer(this));
}

RAS_Rasterizer::~RAS_Rasterizer()
{
}

void RAS_Rasterizer::Init(RAS_ICanvas *canvas)
{
  GPU_color_mask(true, true, true, true);
  GPU_apply_state();

  /* Here we set RAS_FrameBuffers width and height very early in ge launching process
   * Note that if we want to resize RAS_FrameBuffers, this method must be called
   * But other things would need to be resized too with eevee (blender::GPUViewport and
   * its GPUOffScreen)
   */
  m_frameBuffers.Update(canvas);
}

void RAS_Rasterizer::Exit()
{
  // SetClearDepth(1.0f);
  // SetColorMask(true, true, true, true);
  GPU_color_mask(true, true, true, true);
  GPU_apply_state();

  // SetClearColor(0.0f, 0.0f, 0.0f, 0.0f);

  // Clear(RAS_COLOR_BUFFER_BIT | RAS_DEPTH_BUFFER_BIT);
  GPU_framebuffer_clear_color_depth(GPU_framebuffer_active_get(), {0.0, 0.0, 0.0, 0.0}, 1.0f);
}

void RAS_Rasterizer::BeginFrame(double time)
{
  m_time = time;

  GPU_matrix_reset();

  // SetFrontFace(true);

  m_impl->BeginFrame();

  // Render Tools
  m_clientobject = nullptr;
  m_lastauxinfo = nullptr;
}

void RAS_Rasterizer::EndFrame()
{
  GPU_color_mask(true, true, true, true);
  GPU_apply_state();
}

unsigned int *RAS_Rasterizer::MakeScreenshot(int x, int y, int width, int height)
{
  return m_impl->MakeScreenshot(x, y, width, height);
}

RAS_DebugDraw &RAS_Rasterizer::GetDebugDraw()
{
  return m_debugDraw;
}

void RAS_Rasterizer::FlushDebugDraw(RAS_ICanvas *canvas)
{
  m_debugDraw.Flush(this, canvas);
}

void RAS_Rasterizer::UpdateFrameBuffers(RAS_ICanvas *canvas)
{
  m_frameBuffers.Update(canvas);
}

RAS_FrameBuffer *RAS_Rasterizer::GetFrameBuffer(FrameBufferType type)
{
  return m_frameBuffers.GetFrameBuffer(type);
}

void RAS_Rasterizer::DrawFrameBuffer(RAS_FrameBuffer *srcFrameBuffer,
                                     RAS_FrameBuffer *dstFrameBuffer)
{
  GPUVertFormat *vert_format = immVertexFormat();
  uint pos = GPU_vertformat_attr_add(vert_format, "pos", blender::gpu::VertAttrType::SFLOAT_32_32);
  uint texco = GPU_vertformat_attr_add(
      vert_format, "texCoord", blender::gpu::VertAttrType::SFLOAT_32_32);

  GPU_matrix_reset();

  immBindBuiltinProgram(GPU_SHADER_3D_IMAGE);
  blender::gpu::Texture *tex = GPU_framebuffer_color_texture(srcFrameBuffer->GetFrameBuffer());
  immBindTexture("image", tex);
  float mat[4][4];
  unit_m4(mat);
  immUniformMatrix4fv("ModelViewProjectionMatrix", mat);

  /* Full screen triangle */
  immBegin(GPU_PRIM_TRIS, 3);
  immAttr2f(texco, 0.0f, 0.0f);
  immVertex2f(pos, -1.0f, -1.0f);
  immAttr2f(texco, 2.0f, 0.0f);
  immVertex2f(pos, 3.0f, -1.0f);

  immAttr2f(texco, 0.0f, 2.0f);
  immVertex2f(pos, -1.0f, 3.0f);
  immEnd();

  immUnbindProgram();
  GPU_texture_unbind(tex);
}

void RAS_Rasterizer::DrawFrameBuffer(RAS_ICanvas *canvas, RAS_FrameBuffer *frameBuffer)
{
  GPU_scissor_test(true);
  const RAS_Rect &viewport = canvas->GetViewportArea();
  GPU_viewport(
      viewport.GetLeft(), viewport.GetBottom(), viewport.GetWidth() + 1, viewport.GetHeight() + 1);
  GPU_scissor(
      viewport.GetLeft(), viewport.GetBottom(), viewport.GetWidth() + 1, viewport.GetHeight() + 1);
  GPU_apply_state();

  GPU_framebuffer_restore();
  DrawFrameBuffer(frameBuffer, nullptr);
}

RAS_Rect RAS_Rasterizer::GetRenderArea(RAS_ICanvas *canvas)
{
  RAS_Rect area;
  // every available pixel
  area.SetLeft(0);
  area.SetBottom(0);
  area.SetRight(int(canvas->GetWidth()));
  area.SetTop(int(canvas->GetHeight()));

  return area;
}

const MT_Matrix4x4 &RAS_Rasterizer::GetViewMatrix() const
{
  return m_matrices.view;
}

const MT_Matrix4x4 &RAS_Rasterizer::GetViewInvMatrix() const
{
  return m_matrices.viewinv;
}

const MT_Matrix4x4 &RAS_Rasterizer::GetProjMatrix() const
{
  return m_matrices.proj;
}

const MT_Matrix4x4 &RAS_Rasterizer::GetProjInvMatrix() const
{
  return m_matrices.projinv;
}

const MT_Matrix4x4 &RAS_Rasterizer::GetPersMatrix() const
{
  return m_matrices.pers;
}

const MT_Matrix4x4 &RAS_Rasterizer::GetPersInvMatrix() const
{
  return m_matrices.persinv;
}

MT_Matrix4x4 RAS_Rasterizer::GetFrustumMatrix(float left,
                                              float right,
                                              float bottom,
                                              float top,
                                              float frustnear,
                                              float frustfar,
                                              float focallength,
                                              bool perspective)
{
  float mat[4][4];
  perspective_m4(mat, left, right, bottom, top, frustnear, frustfar);

  return MT_Matrix4x4(&mat[0][0]);
}

MT_Matrix4x4 RAS_Rasterizer::GetOrthoMatrix(
    float left, float right, float bottom, float top, float frustnear, float frustfar)
{
  float mat[4][4];
  orthographic_m4(mat, left, right, bottom, top, frustnear, frustfar);

  return MT_Matrix4x4(&mat[0][0]);
}

// next arguments probably contain redundant info, for later...
MT_Matrix4x4 RAS_Rasterizer::GetViewMatrix(const MT_Transform &camtrans,
                                           bool perspective)
{
  return camtrans.toMatrix();
}

void RAS_Rasterizer::SetMatrix(const MT_Matrix4x4 &viewmat,
                               const MT_Matrix4x4 &projmat,
                               const MT_Vector3 &pos,
                               const MT_Vector3 &scale)
{
  m_matrices.view = viewmat;
  m_matrices.proj = projmat;
  m_matrices.viewinv = m_matrices.view.inverse();
  m_matrices.projinv = m_matrices.proj.inverse();
  m_matrices.pers = m_matrices.proj * m_matrices.view;
  m_matrices.persinv = m_matrices.pers.inverse();

  // Don't making variable negX/negY/negZ allow drastic time saving.
  if (scale[0] < 0.0f || scale[1] < 0.0f || scale[2] < 0.0f) {
    const bool negX = (scale[0] < 0.0f);
    const bool negY = (scale[1] < 0.0f);
    const bool negZ = (scale[2] < 0.0f);
    m_matrices.view.tscale(
        (negX) ? -1.0f : 1.0f, (negY) ? -1.0f : 1.0f, (negZ) ? -1.0f : 1.0f, 1.0f);
    m_camnegscale = negX ^ negY ^ negZ;
  }
  else {
    m_camnegscale = false;
  }

  m_campos = pos;

  m_camortho = (m_matrices.pers[3][3] != 0.0f);
}

ViewPortMatrices RAS_Rasterizer::GetAllMatrices()
{
  return m_matrices;
}

const MT_Vector3 &RAS_Rasterizer::GetCameraPosition()
{
  return m_campos;
}

bool RAS_Rasterizer::GetCameraOrtho()
{
  return m_camortho;
}

double RAS_Rasterizer::GetTime()
{
  return m_time;
}

void RAS_Rasterizer::SetClientObject(void *obj)
{
  m_clientobject = obj;
}

void RAS_Rasterizer::SetAuxilaryClientInfo(void *inf)
{
  m_auxilaryClientInfo = inf;
}

void RAS_Rasterizer::PrintHardwareInfo()
{
  m_impl->PrintHardwareInfo();
}

const unsigned char *RAS_Rasterizer::GetGraphicsCardVendor()
{
  return m_impl->GetGraphicsCardVendor();
}
