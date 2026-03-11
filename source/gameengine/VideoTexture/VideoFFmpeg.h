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

#  include <pthread.h>

#  include "BLI_threads.h"

extern "C" {
#  include "ffmpeg_compat.h"
#  include <libavcodec/avcodec.h>
#  include <libavutil/hwcontext.h>
}

#  include "VideoBase.h"

#  define CACHE_FRAME_SIZE 10
#  define CACHE_PACKET_SIZE 50

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
  bool getUseHWDecoding(void)
  {
    return m_useHWDecoding;
  }
  void setUseHWDecoding(bool useHW)
  {
    if (m_useHWDecoding == useHW)
      return;
    m_useHWDecoding = useHW;
    /* Reopen the stream so openStream() picks up the new flag.
     * m_imageName is set by openFile() once the stream is open,
     * so this is a reliable guard against reopening before play(). */
    if (!m_imageName.empty()) {
      std::string name = m_imageName;
      release();
      openFile(const_cast<char *>(name.c_str()));
    }
  }
  bool getLastFrameIsNV12(void) const
  {
    return m_lastFrameIsNV12;
  }
  /* Returns the owned YUV frame copy — valid until clearLastFrameNV12(). */
  const AVFrame *getLastFrameNV12(void) const
  {
    return m_lastFrameYUVCopy;
  }
  void clearLastFrameNV12(void)
  {
    m_lastFrameIsNV12 = false;
    if (m_lastFrameYUVCopy)
      av_frame_unref(m_lastFrameYUVCopy);
  }
  char *getImageName(void)
  {
    return (m_isImage) ? (char *)m_imageName.c_str() : nullptr;
  }

 protected:
  AVFormatContext *m_formatCtx;
  AVCodecContext *m_codecCtx;
  // raw frame extracted from video file
  AVFrame *m_frame;
  // deinterlaced frame if codec requires it
  AVFrame *m_frameDeinterlaced;
  // decoded RGB24 frame if codec requires it
  AVFrame *m_frameRGB;
  // conversion from raw to RGB is done with sws_scale
  struct SwsContext *m_imgConvertCtx;
  // hardware device context (D3D11VA / DXVA2), nullptr if software decoding
  AVBufferRef *m_hwDeviceCtx;
  // pixel format reported by the hardware decoder (e.g. AV_PIX_FMT_D3D11)
  AVPixelFormat m_hwPixFmt;
  // actual SW pixel format after av_hwframe_transfer_data (e.g. NV12 for D3D11VA)
  // AV_PIX_FMT_NONE until the first frame is transferred
  AVPixelFormat m_hwSwFmt;
  // when false, hardware decoding is skipped and the CPU decoder is used
  bool m_useHWDecoding;
  // set by grabFrame when the last frame is a supported YUV format for loadTextureYUV
  bool m_lastFrameIsNV12;
  // owned copy of the last YUV frame (NV12/P010/YUV420P) — safe to read from Pyrefresh
  // without any race against the cache thread. Unref'd by clearLastFrameNV12().
  AVFrame *m_lastFrameYUVCopy;
  // owns the SW frame allocated by the non-threaded grabFrame path for NV12
  AVFrame *m_frameSWNonCached;
  // should the codec be deinterlaced?
  bool m_deinterlace;
  // number of frame of preseek
  int m_preseek;
  // order number of stream holding the video in format context
  int m_videoStream;

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

  /// is image loading done in a separate thread?
  bool m_isThreaded;

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

  /// transfer a hardware-decoded frame to system memory if needed; returns SW frame or nullptr
  /// dst must be a dedicated per-slot AVFrame to avoid data races in the cache thread
  AVFrame *resolveHWFrame(AVFrame *hwFrame, AVFrame *dst);

  /// check if a frame is available and load it in pFrame, return true if a frame could be
  /// retrieved
  AVFrame *grabFrame(long frame);

  /// in case of caching, put the frame back in free queue
  void releaseFrame(AVFrame *frame);

  /// start thread to load the video file/capture/stream
  bool startCache();
  void stopCache();

 private:
  typedef struct {
    blender::Link link;
    long framePosition;
    AVFrame *frame;
    /* Per-slot SW frame used to receive av_hwframe_transfer_data output.
     * Avoids the data-race that arises when a single shared m_frameSW is
     * overwritten by the next resolveHWFrame call before sws_scale has
     * finished reading the previous one. nullptr when HW decode is inactive. */
    AVFrame *frameSW;
  } CacheFrame;
  typedef struct {
    blender::Link link;
    AVPacket packet;
  } CachePacket;

  friend AVPixelFormat getHWFormat(AVCodecContext *ctx, const AVPixelFormat *pix_fmts);

  bool m_stopThread;
  bool m_cacheStarted;
  blender::ListBase m_thread;
  blender::ListBase m_frameCacheBase;  // list of frames that are ready
  blender::ListBase m_frameCacheFree;  // list of frames that are unused
  blender::ListBase m_packetCacheBase;  // list of packets that are ready for decoding
  blender::ListBase m_packetCacheFree;  // list of packets that are unused
  pthread_mutex_t m_cacheMutex;

  AVFrame *allocFrameRGB();
  static void *cacheThread(void *);
};

inline VideoFFmpeg *getFFmpeg(PyImage *self)
{
  return static_cast<VideoFFmpeg *>(self->m_image);
}

#endif /* WITH_FFMPEG */
