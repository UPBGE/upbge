/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2007 The Zdeno Ash Miklas. */

/** \file VideoBase.h
 *  \ingroup bgevideotex
 */

#pragma once

#include "EXP_PyObjectPlus.h"
#include "Exception.h"
#include "ImageBase.h"

// source states
const int SourceError = -1;
const int SourceEmpty = 0;
const int SourceReady = 1;
const int SourcePlaying = 2;
const int SourceStopped = 3;

// video source formats
enum VideoFormat { None, RGB24, YV12, RGBA32 };

/// base class for video source
class VideoBase : public ImageBase {
 public:
  /// constructor
  VideoBase(void)
      : ImageBase(true), m_format(None), m_status(SourceEmpty), m_repeat(0), m_frameRate(1.0)
  {
    m_orgSize[0] = m_orgSize[1] = 0;
    m_range[0] = m_range[1] = 0.0;
  }

  /// destructor
  virtual ~VideoBase(void)
  {
  }

  /// open video file
  virtual void openFile(char *file)
  {
    m_isFile = true;
    m_status = SourceReady;
  }
  /// open video capture device
  virtual void openCam(char *file, short camIdx)
  {
    m_isFile = false;
    m_status = SourceReady;
  }

  /// play video
  virtual bool play(void)
  {
    if (m_status == SourceReady || m_status == SourceStopped) {
      m_status = SourcePlaying;
      return true;
    }
    return false;
  }
  /// pause video
  virtual bool pause(void)
  {
    if (m_status == SourcePlaying) {
      m_status = SourceStopped;
      return true;
    }
    return false;
  }
  /// stop video
  virtual bool stop(void)
  {
    if (m_status == SourcePlaying) {
      m_status = SourceStopped;
      return true;
    }
    return false;
  }

  // get video status
  int getStatus(void)
  {
    return m_status;
  }

  /// get play range
  const double *getRange(void)
  {
    return m_range;
  }
  /// set play range
  virtual void setRange(double start, double stop)
  {
    if (m_isFile) {
      m_range[0] = start;
      m_range[1] = stop;
    }
  }

  // get video repeat
  int getRepeat(void)
  {
    return m_repeat;
  }
  /// set video repeat
  virtual void setRepeat(int rep)
  {
    if (m_isFile)
      m_repeat = rep;
  }

  /// get frame rate
  float getFrameRate(void)
  {
    return m_frameRate;
  }
  /// set frame rate
  virtual void setFrameRate(float rate)
  {
    if (m_isFile)
      m_frameRate = rate > 0.0f ? rate : 1.0f;
  }

 protected:
  /// video format
  VideoFormat m_format;
  /// original video size
  short m_orgSize[2];

  /// video status
  int m_status;

  /// is source file
  bool m_isFile;

  /// replay range
  double m_range[2];
  /// repeat count
  int m_repeat;
  /// frame rate
  float m_frameRate;

  /// initialize image data
  void init(short width, short height);

  /// process source data
  void process(BYTE *sample);
};

// python fuctions

// cast Image pointer to Video
inline VideoBase *getVideo(PyImage *self)
{
  return static_cast<VideoBase *>(self->m_image);
}

extern ExceptionID SourceVideoCreation;

// object initialization
template<class T> void Video_init(PyImage *self)
{
  // create source video object
  if (self->m_image != nullptr)
    delete self->m_image;
  HRESULT hRslt = S_OK;
  self->m_image = new T(&hRslt);
  CHCKHRSLT(hRslt, SourceVideoCreation);
}

// video functions
void Video_open(VideoBase *self, char *file, short captureID);
PyObject *Video_play(PyImage *self);
PyObject *Video_pause(PyImage *self);
PyObject *Video_stop(PyImage *self);
PyObject *Video_refresh(PyImage *self, PyObject *args);
PyObject *Video_getStatus(PyImage *self, void *closure);
PyObject *Video_getRange(PyImage *self, void *closure);
int Video_setRange(PyImage *self, PyObject *value, void *closure);
PyObject *Video_getRepeat(PyImage *self, void *closure);
int Video_setRepeat(PyImage *self, PyObject *value, void *closure);
PyObject *Video_getFrameRate(PyImage *self, void *closure);
int Video_setFrameRate(PyImage *self, PyObject *value, void *closure);

/* py api */
extern PyTypeObject ImageRenderType;
extern PyTypeObject ImageMirrorType;
extern PyTypeObject ImageViewportType;
