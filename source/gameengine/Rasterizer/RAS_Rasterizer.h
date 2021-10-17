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

class RAS_OpenGLRasterizer;
class RAS_FrameBuffer;
class RAS_ICanvas;
struct KX_ClientObjectInfo;
class KX_RayCast;
struct GPUTexture;
struct DRWShadingGroup;

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
    RAS_FRAMEBUFFER_EYE_LEFT0,
    RAS_FRAMEBUFFER_EYE_RIGHT0,
    RAS_FRAMEBUFFER_EYE_LEFT1,
    RAS_FRAMEBUFFER_EYE_RIGHT1,
    RAS_FRAMEBUFFER_BLIT_DEPTH,
    RAS_FRAMEBUFFER_MAX,

    RAS_FRAMEBUFFER_CUSTOM,
  };

  /**
   * Valid SetDepthMask parameters
   */
  enum DepthMask {
    RAS_DEPTHMASK_ENABLED = 1,
    RAS_DEPTHMASK_DISABLED,
  };

  /**
   */
  enum {
    RAS_BACKCULL = 16,  // GEMAT_BACKCULL
  };

  /**
   * Stereo mode types
   */
  enum StereoMode {
    RAS_STEREO_NOSTEREO = 1,
    // WARNING: Not yet supported.
    RAS_STEREO_QUADBUFFERED,
    RAS_STEREO_ABOVEBELOW,
    RAS_STEREO_INTERLACED,
    RAS_STEREO_ANAGLYPH,
    RAS_STEREO_SIDEBYSIDE,
    RAS_STEREO_VINTERLACE,
    RAS_STEREO_3DTVTOPBOTTOM,

    RAS_STEREO_MAXSTEREO
  };

  /**
   * Render pass identifiers for stereo.
   */
  enum StereoEye {
    RAS_STEREO_LEFTEYE = 0,
    RAS_STEREO_RIGHTEYE,
  };

  /**
   * Mipmap options
   */
  enum MipmapOption {
    RAS_MIPMAP_NONE,
    RAS_MIPMAP_NEAREST,
    RAS_MIPMAP_LINEAR,

    RAS_MIPMAP_MAX, /* Should always be last */
  };

  enum ShadowType {
    RAS_SHADOW_NONE,
    RAS_SHADOW_SIMPLE,
    RAS_SHADOW_VARIANCE,
  };

  enum EnableBit {
    RAS_DEPTH_TEST = 0,
    RAS_ALPHA_TEST,
    RAS_SCISSOR_TEST,
    RAS_TEXTURE_2D,
    RAS_TEXTURE_CUBE_MAP,
    RAS_BLEND,
    RAS_COLOR_MATERIAL,
    RAS_CULL_FACE,
    RAS_LIGHTING,
    RAS_MULTISAMPLE,
    RAS_POLYGON_STIPPLE,
    RAS_POLYGON_OFFSET_FILL,
    RAS_POLYGON_OFFSET_LINE
  };

  enum DepthFunc {
    RAS_NEVER = 0,
    RAS_LEQUAL,
    RAS_LESS,
    RAS_ALWAYS,
    RAS_GEQUAL,
    RAS_GREATER,
    RAS_NOTEQUAL,
    RAS_EQUAL
  };

  enum BlendFunc {
    RAS_ZERO = 0,
    RAS_ONE,
    RAS_SRC_COLOR,
    RAS_ONE_MINUS_SRC_COLOR,
    RAS_DST_COLOR,
    RAS_ONE_MINUS_DST_COLOR,
    RAS_SRC_ALPHA,
    RAS_ONE_MINUS_SRC_ALPHA,
    RAS_DST_ALPHA,
    RAS_ONE_MINUS_DST_ALPHA,
    RAS_SRC_ALPHA_SATURATE
  };

  enum MatrixMode { RAS_PROJECTION = 0, RAS_MODELVIEW, RAS_TEXTURE, RAS_MATRIX_MODE_MAX };

  enum ClearBit {
    RAS_COLOR_BUFFER_BIT = 0x2,
    RAS_DEPTH_BUFFER_BIT = 0x4,
    RAS_STENCIL_BUFFER_BIT = 0x8
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
    GPUTexture *m_colorTextureList[RAS_FRAMEBUFFER_MAX];
    GPUTexture *m_depthTextureList[RAS_FRAMEBUFFER_MAX];
    unsigned int m_width;
    unsigned int m_height;
    int m_samples;

   public:
    FrameBuffers();
    ~FrameBuffers();

    void Update(RAS_ICanvas *canvas);
    RAS_FrameBuffer *GetFrameBuffer(FrameBufferType type);
  };

  // All info used to compute the ray cast transform matrix.
  struct RayCastTranform {
    /// The object scale.
    MT_Vector3 scale;
    /// The original object matrix.
    float *origmat;
    /// The output matrix.
    float *mat;
  };

  struct ScreenShaders {
    DRWShadingGroup *normal;
    DRWShadingGroup *anaglyph;
    DRWShadingGroup *interlace;
    DRWShadingGroup *vinterlace;
  } m_screenShaders;

  ViewPortMatrices m_matrices;

  // We store each debug shape by scene.
  RAS_DebugDraw m_debugDraw;

  /* fogging vars */
  bool m_fogenabled;

  double m_time;
  MT_Vector3 m_ambient;
  MT_Matrix4x4 m_viewmatrix;
  MT_Matrix4x4 m_viewinvmatrix;
  MT_Vector3 m_campos;
  bool m_camortho;
  bool m_camnegscale;

  StereoMode m_stereomode;
  StereoEye m_curreye;
  float m_eyeseparation;
  float m_focallength;
  bool m_setfocallength;
  int m_noOfScanlines;

  /* Render tools */
  void *m_clientobject;
  void *m_auxilaryClientInfo;
  int m_lastlightlayer;
  bool m_lastlighting;
  void *m_lastauxinfo;
  unsigned int m_numgllights;

  /// Class used to manage off screens used by the rasterizer.
  FrameBuffers m_frameBuffers;
  ShadowType m_shadowMode;

  bool m_invertFrontFace;
  bool m_last_frontface;

  std::unique_ptr<RAS_OpenGLRasterizer> m_impl;

  void InitScreenShaders();
  void ExitScreenShaders();

 public:
  RAS_Rasterizer();
  virtual ~RAS_Rasterizer();

  /**
   * Enable capability
   * \param bit Enable bit
   */
  void Enable(EnableBit bit);

  /**
   * Disable capability
   * \param bit Enable bit
   */
  void Disable(EnableBit bit);

  /**
   * Set the value for Depth Buffer comparisons
   * \param func Depth comparison function
   */
  void SetDepthFunc(DepthFunc func);

  /**
   * Set the blending equation.
   * \param src The src value.
   * \param dst The destination value.
   */
  void SetBlendFunc(BlendFunc src, BlendFunc dst);

  /**
   * Takes a screenshot
   */
  unsigned int *MakeScreenshot(int x, int y, int width, int height);

  /**
   * SetDepthMask enables or disables writing a fragment's depth value
   * to the Z buffer.
   */
  void SetDepthMask(DepthMask depthmask);

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

  /**
   * Clears a specified set of buffers
   * \param clearbit What buffers to clear (separated by bitwise OR)
   */
  void Clear(int clearbit);

  /**
   * Set background color
   */
  void SetClearColor(float r, float g, float b, float a = 1.0f);

  /**
   * Set background depth
   */
  void SetClearDepth(float d);

  /**
   * Set background color mask.
   */
  void SetColorMask(bool r, bool g, bool b, bool a);

  /**
   * Draw screen overlay plane with basic uv coordinates.
   */
  void DrawOverlayPlane();

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

  /** Draw each stereo off screen to screen.
   * \param canvas The canvas containing the screen viewport.
   * \param lefteyeindex The left off screen index.
   * \param righteyeindex The right off screen index.
   */
  void DrawStereoFrameBuffer(RAS_ICanvas *canvas,
                             RAS_FrameBuffer *leftFb,
                             RAS_FrameBuffer *rightFb);

  /**
   * GetRenderArea computes the render area from the 2d canvas.
   */
  RAS_Rect GetRenderArea(RAS_ICanvas *canvas, StereoEye eye);

  // Stereo Functions
  /**
   * SetStereoMode will set the stereo mode
   */
  void SetStereoMode(const StereoMode stereomode);

  /**
   * Stereo can be used to query if the rasterizer is in stereo mode.
   * \return true if stereo mode is enabled.
   */
  bool Stereo();
  StereoMode GetStereoMode();

  /**
   * Sets which eye buffer subsequent primitives will be rendered to.
   */
  void SetEye(const StereoEye eye);
  StereoEye GetEye();

  /**
   * Sets the distance between eyes for stereo mode.
   */
  void SetEyeSeparation(const float eyeseparation);
  float GetEyeSeparation();

  /**
   * Sets the focal length for stereo mode.
   */
  void SetFocalLength(const float focallength);
  float GetFocalLength();

  /// Get the modelview matrix according to the stereo settings.
  MT_Matrix4x4 GetViewMatrix(StereoEye eye, const MT_Transform &camtrans, bool perspective);
  /**
   * Sets the modelview matrix.
   */
  void SetMatrix(const MT_Matrix4x4 &viewmat,
                 const MT_Matrix4x4 &projmat,
                 const MT_Vector3 &pos,
                 const MT_Vector3 &scale);
  ViewPortMatrices GetAllMatrices();

  /**
   * Get/Set viewport area
   */
  void SetViewport(int x, int y, int width, int height);

  /**
   * Set scissor mask
   */
  void SetScissor(int x, int y, int width, int height);

  /**
   */
  const MT_Vector3 &GetCameraPosition();
  bool GetCameraOrtho();

  /// \param shadowmode = RAS_SHADOW_SIMPLE, RAS_SHADOW_VARIANCE.
  void SetShadowMode(ShadowType shadowmode);

  /// \return the current drawing mode: RAS_SHADOW_SIMPLE, RAS_SHADOW_VARIANCE.
  ShadowType GetShadowMode();

  /**
   * Sets face culling
   */
  void SetCullFace(bool enable);

  /// Set and enable clip plane.
  void EnableClipPlane(int numplanes);
  /// Disable clip plane
  void DisableClipPlane(int numplanes);

  /**
   * Sets wireframe mode.
   */
  void SetLines(bool enable);

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
  MT_Matrix4x4 GetFrustumMatrix(StereoEye eye,
                                float left,
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

  void SetAmbientColor(const MT_Vector3 &color);

  /**
   * Sets a polygon offset.  z depth will be: z1 = mult*z0 + add
   */
  void SetPolygonOffset(float mult, float add);

  RAS_DebugDraw &GetDebugDraw();
  void FlushDebugDraw(RAS_ICanvas *canvas);

  const MT_Matrix4x4 &GetViewMatrix() const;
  const MT_Matrix4x4 &GetViewInvMatrix() const;
  const MT_Matrix4x4 &GetProjMatrix() const;
  const MT_Matrix4x4 &GetProjInvMatrix() const;
  const MT_Matrix4x4 &GetPersMatrix() const;
  const MT_Matrix4x4 &GetPersInvMatrix() const;

  void SetFrontFace(bool ccw);

  void SetInvertFrontFace(bool invert);

  void SetAnisotropicFiltering(short level);
  short GetAnisotropicFiltering();

  void SetMipmapping(MipmapOption val);
  MipmapOption GetMipmapping();

  /// \see KX_RayCast
  bool RayHit(KX_ClientObjectInfo *client, KX_RayCast *result, RayCastTranform *raytransform);
  /// \see KX_RayCast
  bool NeedRayCast(KX_ClientObjectInfo *info, void *data);

  /**
   * Render Tools
   */
  void GetTransform(float *origmat, int objectdrawmode, float mat[16]);

  void DisableForText();

  void SetClientObject(void *obj);

  void SetAuxilaryClientInfo(void *inf);

  /**
   * Prints information about what the hardware supports.
   */
  void PrintHardwareInfo();

  const unsigned char *GetGraphicsCardVendor();
};
