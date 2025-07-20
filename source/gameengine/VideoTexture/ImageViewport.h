/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2007 The Zdeno Ash Miklas. */

/** \file ImageViewport.h
 *  \ingroup bgevideotex
 */

#pragma once

#include "Common.h"
#include "ImageBase.h"

class Texture;

/// class for viewport access
class ImageViewport : public ImageBase {
 public:
  /// constructor
  ImageViewport(unsigned int width, unsigned int height);
  /// Constructor called from python.
  ImageViewport();

  /// destructor
  virtual ~ImageViewport(void);

  /// is whole buffer used
  bool getWhole(void)
  {
    return m_whole;
  }
  /// set whole buffer use
  void setWhole(bool whole);

  /// is alpha channel used
  bool getAlpha(void)
  {
    return m_alpha;
  }
  /// set whole buffer use
  void setAlpha(bool alpha)
  {
    m_alpha = alpha;
  }

  /// get capture size in viewport
  short *getCaptureSize(void)
  {
    return m_capSize;
  }
  /// set capture size in viewport
  void setCaptureSize(short size[2] = nullptr);

  /// get position in viewport
  int *getPosition(void)
  {
    return m_position;
  }
  /// set position in viewport
  void setPosition(int pos[2] = nullptr);

  void createColorToRGBA8Shader();
  void createDepthToRGBA8Shader();
  void createRGBA8Resources();
  void freeRGBA8Resources();
  void convertRGBA16toRGBA8Textures(struct GPUTexture *rgba16f_color, struct GPUTexture *rgba32f_depth);

  /// capture image from viewport to user buffer
  virtual bool loadImage(unsigned int *buffer, unsigned int size, double ts);

 protected:
  unsigned int m_width;
  unsigned int m_height;
  /// frame buffer rectangle
  int m_viewport[4];

  /// size of captured area
  short m_capSize[2];
  /// use whole viewport
  bool m_whole;
  /// use alpha channel
  bool m_alpha;

  /// position of capture rectangle in viewport
  int m_position[2];
  /// upper left point for capturing
  int m_upLeft[2];

  struct GPUFrameBuffer *m_rgba8_color_fb;
  struct GPUTexture *m_rgba8_color_tex;

  struct GPUFrameBuffer *m_rgba8_depth_fb;
  struct GPUTexture *m_rgba8_depth_tex;

  struct GPUShader *m_color_to_rgba8_sh;
  struct GPUShader *m_depth_to_rgba8_sh;

  /// buffer to copy viewport
  BYTE *m_viewportImage;
  /// texture is initialized
  bool m_texInit;

  Texture *m_texture;

  /// capture image from viewport
  virtual void calcImage(unsigned int texid, double ts)
  {
    calcViewport(texid, ts);
  }

  /// capture image from viewport
  virtual void calcViewport(unsigned int texid, double ts);

  /// get viewport size
  int *getViewportSize(void)
  {
    return m_viewport + 2;
  }
};

PyObject *ImageViewport_getCaptureSize(PyImage *self, void *closure);
int ImageViewport_setCaptureSize(PyImage *self, PyObject *value, void *closure);
PyObject *ImageViewport_getWhole(PyImage *self, void *closure);
int ImageViewport_setWhole(PyImage *self, PyObject *value, void *closure);
PyObject *ImageViewport_getAlpha(PyImage *self, void *closure);
int ImageViewport_setAlpha(PyImage *self, PyObject *value, void *closure);
