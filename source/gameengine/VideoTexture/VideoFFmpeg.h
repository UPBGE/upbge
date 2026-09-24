/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2007 The Zdeno Ash Miklas. */

/** \file VideoFFmpeg.h
 *  \ingroup bgevideotex
 */

#pragma once

#ifdef WITH_FFMPEG
/* this needs to be parsed with __cplusplus defined before included through ffmpeg_compat.h */
#  if defined(__FreeBSD__)
#    include <inttypes.h>
#  endif

extern "C" {
#  include "ffmpeg_compat.h"
#  include <libavcodec/avcodec.h>
}

// MovieReader is defined in the imbuf movie module (used for video decoding).
namespace blender {
struct MovieReader;
}

#  include "VideoBase.h"

// type VideoFFmpeg declaration
class VideoFFmpeg : public VideoBase {
 public:
  /// constructor
  VideoFFmpeg(HRESULT *hRslt);
  /// destructor
  virtual ~VideoFFmpeg();

  /// set initial parameters
  void initParams(short width, short height, float rate, bool image = false);
  /// open video/image file
  virtual void openFile(char *file);
  /// open video capture device
  virtual void openCam(char *driver, short camIdx);

  /// release video source
  virtual bool release(void);
  /// overwrite base refresh to handle fixed image
  virtual void refresh(void);
  /// play video
  virtual bool play(void);
  /// pause video
  virtual bool pause(void);
  /// stop video
  virtual bool stop(void);
  /// set play range
  virtual void setRange(double start, double stop);
  /// set frame rate
  virtual void setFrameRate(float rate);
  // some specific getters and setters
  int getPreseek(void)
  {
    return m_preseek;
  }
  void setPreseek(int preseek)
  {
    if (preseek >= 0)
      m_preseek = preseek;
  }
  bool getDeinterlace(void)
  {
    return m_deinterlace;
  }
  void setDeinterlace(bool deinterlace)
  {
    m_deinterlace = deinterlace;
  }
  char *getImageName(void)
  {
    return (m_isImage) ? (char *)m_imageName.c_str() : nullptr;
  }

 protected:
  // Video decoding is now handled by the imbuf MovieReader (Blender's movie_read.cc).
  blender::MovieReader *m_movieReader;
  // should the codec be deinterlaced?
  bool m_deinterlace;
  // number of frame of preseek
  int m_preseek;

  // the actual frame rate
  double m_baseFrameRate;

  /// last displayed frame
  long m_lastFrame;

  /// end of file reached
  bool m_eof;

  /// flag to indicate that time is coming from application
  bool m_externTime;

  /// current file pointer position in file expressed in frame number
  long m_curPosition;

  /// time of video play start
  double m_startTime;

  /// width of capture in pixel
  short m_captWidth;

  /// height of capture in pixel
  short m_captHeight;

  /// frame rate of capture in frames per seconds
  float m_captRate;

  /// is file an image?
  bool m_isImage;

  /// is streaming or camera?
  bool m_isStreaming;

  /// keep last image name
  std::string m_imageName;

  /// image calculation
  virtual void calcImage(unsigned int texId, double ts);

  /// set actual position
  void setPositions(void);

  /// get actual framerate
  double actFrameRate(void)
  {
    return m_frameRate * m_baseFrameRate;
  }

  /// common function to video file and capture
  int openStream(const char *filename, const AVInputFormat *inputFormat, AVDictionary **formatParams);

  /// decode the next frame from a live capture device into the texture buffer.
  bool grabDeviceFrame(void);

 private:
};

inline VideoFFmpeg *getFFmpeg(PyImage *self)
{
  return static_cast<VideoFFmpeg *>(self->m_imageBase);
}

#endif /* WITH_FFMPEG */
