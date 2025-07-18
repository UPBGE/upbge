/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2007 The Zdeno Ash Miklas. */

/** \file ImageRender.h
 *  \ingroup bgevideotex
 */

#pragma once

#include "DNA_screen_types.h"

#include "Common.h"
#include "ImageViewport.h"
#include "KX_Camera.h"
#include "KX_Scene.h"
#include "RAS_ICanvas.h"
#include "RAS_Rasterizer.h"

struct GPUFrameBuffer;
class Texture;

/// class for render 3d scene
class ImageRender : public ImageViewport {
 public:
  /// constructors
  ImageRender(KX_Scene *scene,
              KX_Camera *camera,
              unsigned int width,
              unsigned int height,
              unsigned short samples);
  ImageRender(KX_Scene *scene,
              KX_GameObject *observer,
              KX_GameObject *mirror,
              RAS_IPolyMaterial *mat,
              unsigned int width,
              unsigned int height,
              unsigned short samples);

  /// destructor
  virtual ~ImageRender(void);

  /// clipping distance
  float getClip(void)
  {
    return m_clip;
  }
  /// set whole buffer use
  void setClip(float clip)
  {
    m_clip = clip;
  }
  /// render status
  bool isDone()
  {
    return m_done;
  }

  KX_Camera *GetCamera();

  /// render frame (public so that it is accessible from python)
  bool Render();
  /// in case fbo is used, method to unbind
  void Unbind();

  void RunPreDrawCallbacks();
  void RunPostDrawCallbacks();

  void SetTexture(Texture *tex);

#ifdef WITH_PYTHON
  PyObject *m_preDrawCallbacks;
  PyObject *m_postDrawCallbacks;
#endif

 protected:
  /// true if ready to render
  bool m_render;
  /// is render done already?
  bool m_done;
  /// rendered scene
  KX_Scene *m_scene;
  /// camera for render
  KX_Camera *m_camera;
  /// number of render passes
  unsigned short m_samples;
  /// do we own the camera?
  bool m_owncamera;

  /// Used to make color and depth textures availables in ImageViewport::calcViewport
  GPUFrameBuffer *m_targetfb;

  /// for mirror operation
  KX_GameObject *m_observer;
  KX_GameObject *m_mirror;
  float m_clip;              // clipping distance
  float m_mirrorHalfWidth;   // mirror width in mirror space
  float m_mirrorHalfHeight;  // mirror height in mirror space
  MT_Vector3 m_mirrorPos;    // mirror center position in local space
  MT_Vector3 m_mirrorZ;      // mirror Z axis in local space
  MT_Vector3 m_mirrorY;      // mirror Y axis in local space
  MT_Vector3 m_mirrorX;      // mirror X axis in local space
  /// canvas
  RAS_ICanvas *m_canvas;
  /// rasterizer
  RAS_Rasterizer *m_rasterizer;
  /// engine
  KX_KetsjiEngine *m_engine;

  /// render 3d scene to image
  virtual void calcImage(unsigned int texId, double ts)
  {
    calcViewport(texId, ts);
  }

  /// render 3d scene to image
  virtual void calcViewport(unsigned int texId, double ts);
};
