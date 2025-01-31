/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2007 The Zdeno Ash Miklas. */

/** \file gameengine/VideoTexture/VideoFFmpeg.cpp
 *  \ingroup bgevideotex
 */

#ifdef WITH_FFMPEG

#  include "VideoFFmpeg.h"

// INT64_C fix for some linux machines (C99ism)
#  ifndef __STDC_CONSTANT_MACROS
#    define __STDC_CONSTANT_MACROS
#    ifdef __STDC_CONSTANT_MACROS /* quiet warning */
#    endif
#  endif

#  include <stdint.h>
#  include <string>

#  include "MEM_guardedalloc.h"

#  include "Exception.h"
#  include "BLI_listbase.h"
#  include "BLI_string.h"
#  include "BLI_time.h"
#  include "movie_util.hh"


extern "C" {
#  include <libavutil/imgutils.h>
}

// default framerate
const double defFrameRate = 25.0;

// macro for exception handling and logging
#  define CATCH_EXCP \
    catch (Exception & exp) \
    { \
      exp.report(); \
      m_status = SourceError; \
    }

// class RenderVideo

// constructor
VideoFFmpeg::VideoFFmpeg(HRESULT *hRslt)
    : VideoBase(),
      m_formatCtx(nullptr),
      m_codecCtx(nullptr),
      m_frame(nullptr),
      m_frameDeinterlaced(nullptr),
      m_frameRGB(nullptr),
      m_imgConvertCtx(nullptr),
      m_deinterlace(false),
      m_preseek(0),
      m_videoStream(-1),
      m_baseFrameRate(25.0),
      m_lastFrame(-1),
      m_eof(false),
      m_externTime(false),
      m_curPosition(-1),
      m_startTime(0),
      m_captWidth(0),
      m_captHeight(0),
      m_captRate(0.f),
      m_isImage(false),
      m_isThreaded(false),
      m_isStreaming(false),
      m_stopThread(false),
      m_cacheStarted(false)
{
  // set video format
  m_format = RGB24;
  // force flip because ffmpeg always return the image in the wrong orientation for texture
  setFlip(true);
  // construction is OK
  *hRslt = S_OK;
  BLI_listbase_clear(&m_thread);
  pthread_mutex_init(&m_cacheMutex, nullptr);
  BLI_listbase_clear(&m_frameCacheFree);
  BLI_listbase_clear(&m_frameCacheBase);
  BLI_listbase_clear(&m_packetCacheFree);
  BLI_listbase_clear(&m_packetCacheBase);
}

// destructor
VideoFFmpeg::~VideoFFmpeg()
{
}

void VideoFFmpeg::refresh(void)
{
  // a fixed image will not refresh because it is loaded only once at creation
  if (m_isImage)
    return;
  m_avail = false;
}

// release components
bool VideoFFmpeg::release()
{
  // release
  stopCache();

  if (m_codecCtx) {
    avcodec_free_context(&m_codecCtx);
    m_codecCtx = nullptr;
  }
  if (m_formatCtx) {
    avformat_close_input(&m_formatCtx);
    m_formatCtx = nullptr;
  }
  if (m_frame) {
    av_frame_free(&m_frame);
    m_frame = nullptr;
  }
  if (m_frameDeinterlaced) {
    MEM_freeN(m_frameDeinterlaced->data[0]);
    av_frame_free(&m_frameDeinterlaced);
    m_frameDeinterlaced = nullptr;
  }
  if (m_frameRGB) {
    MEM_freeN(m_frameRGB->data[0]);
    av_frame_free(&m_frameRGB);
    m_frameRGB = nullptr;
  }
  if (m_imgConvertCtx) {
    sws_freeContext(m_imgConvertCtx);
    m_imgConvertCtx = nullptr;
  }
  m_status = SourceStopped;
  m_lastFrame = -1;
  return true;
}

AVFrame *VideoFFmpeg::allocFrameRGB()
{
  AVFrame *frame;
  frame = av_frame_alloc();
  if (m_format == RGBA32) {
    av_image_fill_arrays(
        frame->data,
        frame->linesize,
        (uint8_t *)MEM_callocN(
            av_image_get_buffer_size(AV_PIX_FMT_RGBA, m_codecCtx->width, m_codecCtx->height, 1),
            "ffmpeg rgba"),
        AV_PIX_FMT_RGBA,
        m_codecCtx->width,
        m_codecCtx->height,
        1);
  }
  else {
    av_image_fill_arrays(
        frame->data,
        frame->linesize,
        (uint8_t *)MEM_callocN(
            av_image_get_buffer_size(AV_PIX_FMT_RGB24, m_codecCtx->width, m_codecCtx->height, 1),
            "ffmpeg rgb"),
        AV_PIX_FMT_RGB24,
        m_codecCtx->width,
        m_codecCtx->height,
        1);
  }
  return frame;
}

// set initial parameters
void VideoFFmpeg::initParams(short width, short height, float rate, bool image)
{
  m_captWidth = width;
  m_captHeight = height;
  m_captRate = rate;
  m_isImage = image;
}

int VideoFFmpeg::openStream(const char *filename,
                            const AVInputFormat *inputFormat,
                            AVDictionary **formatParams)
{
  int i, video_stream_index;

  const AVCodec *pCodec;
  AVFormatContext *pFormatCtx = nullptr;
  AVCodecContext *pCodecCtx;
  AVStream *video_stream;

# ifdef FF_API_AVIOFORMAT  // To be removed after ffmpeg5 library update
  if (avformat_open_input(&pFormatCtx, filename, (AVInputFormat *)inputFormat, formatParams) != 0) {
    if (avformat_open_input(&pFormatCtx, filename, (AVInputFormat *)inputFormat, nullptr) != 0) {
# else
  if (avformat_open_input(&pFormatCtx, filename, inputFormat, formatParams) != 0) {
    if (avformat_open_input(&pFormatCtx, filename, inputFormat, nullptr) != 0) {
# endif
      return -1;
    }
    else {
      std::cout << "Camera capture: Format not compatible. Capture in default camera format"
                << std::endl;
    }
  }

  if (avformat_find_stream_info(pFormatCtx, nullptr) < 0) {
    avformat_close_input(&pFormatCtx);
    return -1;
  }

  av_dump_format(pFormatCtx, 0, filename, 0);

  /* Find the video stream */
  video_stream_index = -1;

  for (i = 0; i < pFormatCtx->nb_streams; i++) {
    if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      video_stream_index = i;
      break;
    }
  }

  if (video_stream_index == -1) {
    avformat_close_input(&pFormatCtx);
    return -1;
  }

  video_stream = pFormatCtx->streams[video_stream_index];

  /* Find the decoder for the video stream */
  pCodec = avcodec_find_decoder(video_stream->codecpar->codec_id);
  if (pCodec == nullptr) {
    avformat_close_input(&pFormatCtx);
    return -1;
  }

  pCodecCtx = avcodec_alloc_context3(nullptr);
  avcodec_parameters_to_context(pCodecCtx, video_stream->codecpar);
  pCodecCtx->workaround_bugs = FF_BUG_AUTODETECT;

  if (pCodec->capabilities & AV_CODEC_CAP_OTHER_THREADS) {
    pCodecCtx->thread_count = 0;
  }
  else {
    pCodecCtx->thread_count = BLI_system_thread_count();
  }

  if (pCodec->capabilities & AV_CODEC_CAP_FRAME_THREADS) {
    pCodecCtx->thread_type = FF_THREAD_FRAME;
  }
  else if (pCodec->capabilities & AV_CODEC_CAP_SLICE_THREADS) {
    pCodecCtx->thread_type = FF_THREAD_SLICE;
  }

  if (avcodec_open2(pCodecCtx, pCodec, nullptr) < 0) {
    avformat_close_input(&pFormatCtx);
    return -1;
  }
  if (pCodecCtx->pix_fmt == AV_PIX_FMT_NONE) {
    avcodec_free_context(&pCodecCtx);
    avformat_close_input(&pFormatCtx);
    return -1;
  }
  m_baseFrameRate = av_q2d(av_guess_frame_rate(pFormatCtx, video_stream, nullptr));

  if (m_baseFrameRate <= 0.0) {
    m_baseFrameRate = defFrameRate;
  }

  m_codecCtx = pCodecCtx;
  m_formatCtx = pFormatCtx;
  m_videoStream = video_stream_index;
  m_frame = av_frame_alloc();
  m_frameDeinterlaced = av_frame_alloc();

  // allocate buffer if deinterlacing is required
  av_image_fill_arrays(
      m_frameDeinterlaced->data,
      m_frameDeinterlaced->linesize,
      (uint8_t *)MEM_callocN(
          av_image_get_buffer_size(m_codecCtx->pix_fmt, m_codecCtx->width, m_codecCtx->height, 1),
          "ffmpeg deinterlace"),
      m_codecCtx->pix_fmt,
      m_codecCtx->width,
      m_codecCtx->height,
      1);

  // check if the pixel format supports Alpha
  if (m_codecCtx->pix_fmt == AV_PIX_FMT_RGB32 || m_codecCtx->pix_fmt == AV_PIX_FMT_BGR32 ||
      m_codecCtx->pix_fmt == AV_PIX_FMT_RGB32_1 || m_codecCtx->pix_fmt == AV_PIX_FMT_BGR32_1) {
    // allocate buffer to store final decoded frame
    m_format = RGBA32;
    // allocate sws context
    m_imgConvertCtx = sws_getContext(m_codecCtx->width,
                                     m_codecCtx->height,
                                     m_codecCtx->pix_fmt,
                                     m_codecCtx->width,
                                     m_codecCtx->height,
                                     AV_PIX_FMT_RGBA,
                                     SWS_FAST_BILINEAR,
                                     nullptr,
                                     nullptr,
                                     nullptr);
  }
  else {
    // allocate buffer to store final decoded frame
    m_format = RGB24;
    // allocate sws context
    m_imgConvertCtx = sws_getContext(m_codecCtx->width,
                                     m_codecCtx->height,
                                     m_codecCtx->pix_fmt,
                                     m_codecCtx->width,
                                     m_codecCtx->height,
                                     AV_PIX_FMT_RGB24,
                                     SWS_FAST_BILINEAR,
                                     nullptr,
                                     nullptr,
                                     nullptr);
  }
  m_frameRGB = allocFrameRGB();

  if (!m_imgConvertCtx) {
    avcodec_free_context(&m_codecCtx);
    m_codecCtx = nullptr;
    avformat_close_input(&m_formatCtx);
    m_formatCtx = nullptr;
    av_frame_free(&m_frame);
    m_frame = nullptr;
    MEM_freeN(m_frameDeinterlaced->data[0]);
    av_frame_free(&m_frameDeinterlaced);
    m_frameDeinterlaced = nullptr;
    MEM_freeN(m_frameRGB->data[0]);
    av_frame_free(&m_frameRGB);
    m_frameRGB = nullptr;
    return -1;
  }
  return 0;
}

/*
 * This thread is used to load video frame asynchronously.
 * It provides a frame caching service.
 * The main thread is responsible for positioning the frame pointer in the
 * file correctly before calling startCache() which starts this thread.
 * The cache is organized in two layers: 1) a cache of 20-30 undecoded packets to keep
 * memory and CPU low 2) a cache of 5 decoded frames.
 * If the main thread does not find the frame in the cache (because the video has restarted
 * or because the GE is lagging), it stops the cache with StopCache() (this is a synchronous
 * function: it sends a signal to stop the cache thread and wait for confirmation), then
 * change the position in the stream and restarts the cache thread.
 */
void *VideoFFmpeg::cacheThread(void *data)
{
  VideoFFmpeg *video = (VideoFFmpeg *)data;
  // holds the frame that is being decoded
  CacheFrame *currentFrame = nullptr;
  CachePacket *cachePacket;
  bool endOfFile = false;
  int frameFinished = 0;
  double timeBase = av_q2d(video->m_formatCtx->streams[video->m_videoStream]->time_base);
  int64_t startTs = video->m_formatCtx->streams[video->m_videoStream]->start_time;

  if (startTs == AV_NOPTS_VALUE)
    startTs = 0;

  while (!video->m_stopThread) {
    // packet cache is used solely by this thread, no need to lock
    // In case the stream/file contains other stream than the one we are looking for,
    // allow a bit of cycling to get rid quickly of those frames
    frameFinished = 0;
    while (!endOfFile &&
           (cachePacket = (CachePacket *)video->m_packetCacheFree.first) != nullptr &&
           frameFinished < 25) {
      // free packet => packet cache is not full yet, just read more
      if (av_read_frame(video->m_formatCtx, &cachePacket->packet) >= 0) {
        if (cachePacket->packet.stream_index == video->m_videoStream) {
          // make sure fresh memory is allocated for the packet and move it to queue
          AVPacket newPacket;
          av_packet_ref(&newPacket, &cachePacket->packet);
          cachePacket->packet = newPacket;

          BLI_remlink(&video->m_packetCacheFree, cachePacket);
          BLI_addtail(&video->m_packetCacheBase, cachePacket);
          break;
        }
        else {
          // this is not a good packet for us, just leave it on free queue
          // Note: here we could handle sound packet
          av_packet_unref(&cachePacket->packet);
          frameFinished++;
        }
      }
      else {
        if (video->m_isFile)
          // this mark the end of the file
          endOfFile = true;
        // if we cannot read a packet, no need to continue
        break;
      }
    }
    // frame cache is also used by main thread, lock
    if (currentFrame == nullptr) {
      // no current frame being decoded, take free one
      pthread_mutex_lock(&video->m_cacheMutex);
      if ((currentFrame = (CacheFrame *)video->m_frameCacheFree.first) != nullptr)
        BLI_remlink(&video->m_frameCacheFree, currentFrame);
      pthread_mutex_unlock(&video->m_cacheMutex);
    }
    if (currentFrame != nullptr) {
      // this frame is out of free and busy queue, we can manipulate it without locking
      frameFinished = 0;
      while (!frameFinished &&
             (cachePacket = (CachePacket *)video->m_packetCacheBase.first) != nullptr) {
        BLI_remlink(&video->m_packetCacheBase, cachePacket);
        // use m_frame because when caching, it is not used in main thread
        // we can't use currentFrame directly because we need to convert to RGB first
        avcodec_send_packet(video->m_codecCtx, &cachePacket->packet);
        frameFinished = avcodec_receive_frame(video->m_codecCtx, video->m_frame) == 0;

        if (frameFinished) {
          AVFrame *input = video->m_frame;

          /* This means the data wasnt read properly, this check stops crashing */
          if (input->data[0] != 0 || input->data[1] != 0 || input->data[2] != 0 ||
              input->data[3] != 0) {
            if (video->m_deinterlace) {
              if (ffmpeg_deinterlace((AVFrame *)video->m_frameDeinterlaced,
                                       (const AVFrame *)video->m_frame,
                                       video->m_codecCtx->pix_fmt,
                                       video->m_codecCtx->width,
                                       video->m_codecCtx->height) >= 0) {
                input = video->m_frameDeinterlaced;
              }
            }
            // convert to RGB24
            sws_scale(video->m_imgConvertCtx,
                      input->data,
                      input->linesize,
                      0,
                      video->m_codecCtx->height,
                      currentFrame->frame->data,
                      currentFrame->frame->linesize);
            // move frame to queue, this frame is necessarily the next one
            video->m_curPosition = (long)((cachePacket->packet.dts - startTs) *
                                              (video->m_baseFrameRate * timeBase) +
                                          0.5);
            currentFrame->framePosition = video->m_curPosition;
            pthread_mutex_lock(&video->m_cacheMutex);
            BLI_addtail(&video->m_frameCacheBase, currentFrame);
            pthread_mutex_unlock(&video->m_cacheMutex);
            currentFrame = nullptr;
          }
        }
        av_packet_unref(&cachePacket->packet);
        BLI_addtail(&video->m_packetCacheFree, cachePacket);
      }
      if (currentFrame && endOfFile) {
        // no more packet and end of file => put a special frame that indicates that
        currentFrame->framePosition = -1;
        pthread_mutex_lock(&video->m_cacheMutex);
        BLI_addtail(&video->m_frameCacheBase, currentFrame);
        pthread_mutex_unlock(&video->m_cacheMutex);
        currentFrame = nullptr;
        // no need to stay any longer in this thread
        break;
      }
    }
    // small sleep to avoid unnecessary looping
    BLI_time_sleep_ms(10);
  }
  // before quitting, put back the current frame to queue to allow freeing
  if (currentFrame) {
    pthread_mutex_lock(&video->m_cacheMutex);
    BLI_addtail(&video->m_frameCacheFree, currentFrame);
    pthread_mutex_unlock(&video->m_cacheMutex);
  }
  return 0;
}

// start thread to cache video frame from file/capture/stream
// this function should be called only when the position in the stream is set for the
// first frame to cache
bool VideoFFmpeg::startCache()
{
  if (!m_cacheStarted && m_isThreaded) {
    m_stopThread = false;
    for (int i = 0; i < CACHE_FRAME_SIZE; i++) {
      CacheFrame *frame = new CacheFrame();
      frame->frame = allocFrameRGB();
      BLI_addtail(&m_frameCacheFree, frame);
    }
    for (int i = 0; i < CACHE_PACKET_SIZE; i++) {
      CachePacket *packet = new CachePacket();
      BLI_addtail(&m_packetCacheFree, packet);
    }
    BLI_threadpool_init(&m_thread, cacheThread, 1);
    BLI_threadpool_insert(&m_thread, this);
    m_cacheStarted = true;
  }
  return m_cacheStarted;
}

void VideoFFmpeg::stopCache()
{
  if (m_cacheStarted) {
    m_stopThread = true;
    BLI_threadpool_end(&m_thread);
    // now delete the cache
    CacheFrame *frame;
    CachePacket *packet;
    while ((frame = (CacheFrame *)m_frameCacheBase.first) != nullptr) {
      BLI_remlink(&m_frameCacheBase, frame);
      MEM_freeN(frame->frame->data[0]);
      av_free(frame->frame);
      delete frame;
    }
    while ((frame = (CacheFrame *)m_frameCacheFree.first) != nullptr) {
      BLI_remlink(&m_frameCacheFree, frame);
      MEM_freeN(frame->frame->data[0]);
      av_free(frame->frame);
      delete frame;
    }
    while ((packet = (CachePacket *)m_packetCacheBase.first) != nullptr) {
      BLI_remlink(&m_packetCacheBase, packet);
      av_packet_unref(&packet->packet);
      delete packet;
    }
    while ((packet = (CachePacket *)m_packetCacheFree.first) != nullptr) {
      BLI_remlink(&m_packetCacheFree, packet);
      delete packet;
    }
    m_cacheStarted = false;
  }
}

void VideoFFmpeg::releaseFrame(AVFrame *frame)
{
  if (frame == m_frameRGB) {
    // this is not a frame from the cache, ignore
    return;
  }
  // this frame MUST be the first one of the queue
  pthread_mutex_lock(&m_cacheMutex);
  CacheFrame *cacheFrame = (CacheFrame *)m_frameCacheBase.first;
  assert(cacheFrame != nullptr && cacheFrame->frame == frame);
  BLI_remlink(&m_frameCacheBase, cacheFrame);
  BLI_addtail(&m_frameCacheFree, cacheFrame);
  pthread_mutex_unlock(&m_cacheMutex);
}

// open video file
void VideoFFmpeg::openFile(char *filename)
{
  if (openStream(filename, nullptr, nullptr) != 0)
    return;

  if (m_codecCtx->gop_size)
    m_preseek = (m_codecCtx->gop_size < 25) ? m_codecCtx->gop_size + 1 : 25;
  else if (m_codecCtx->has_b_frames)
    m_preseek = 25;  // should determine gopsize
  else
    m_preseek = 0;

  // get video time range
  m_range[0] = 0.0;
  m_range[1] = (double)m_formatCtx->duration / AV_TIME_BASE;

  // open base class
  VideoBase::openFile(filename);

  if (
      // ffmpeg reports that http source are actually non stream
      // but it is really not desirable to seek on http file, so force streaming.
      // It would be good to find this information from the context but there are no simple
      // indication
      !strncmp(filename, "http://", 7) || !strncmp(filename, "rtsp://", 7) ||
      (m_formatCtx->pb && !m_formatCtx->pb->seekable)) {
    // the file is in fact a streaming source, treat as cam to prevent seeking
    m_isFile = false;
    // but it's not handled exactly like a camera.
    m_isStreaming = true;
    // for streaming it is important to do non blocking read
    m_formatCtx->flags |= AVFMT_FLAG_NONBLOCK;
  }

  if (m_isImage) {
    // the file is to be treated as an image, i.e. load the first frame only
    m_isFile = false;
    // in case of reload, the filename is taken from m_imageName, no need to change it
    if (m_imageName.c_str() != filename)
      m_imageName = filename;
    m_preseek = 0;
    m_avail = false;
    play();
  }
  // check if we should do multi-threading?
  if (!m_isImage && BLI_system_thread_count() > 1) {
    // never thread image: there are no frame to read ahead
    // no need to thread if the system has a single core
    m_isThreaded = true;
  }
}

// open video capture device
void VideoFFmpeg::openCam(char *file, short camIdx)
{
  // open camera source
  const AVInputFormat *inputFormat;
  AVDictionary *formatParams = nullptr;
  char filename[28], rateStr[20];

#  ifdef WIN32
  inputFormat = av_find_input_format("dshow");
  if (!inputFormat)
    // dshow not supported??
    return;
  sprintf(filename, "video=%s", file);
#  else
  // In Linux we support two types of devices: VideoForLinux and DV1394.
  // the user specify it with the filename:
  // [<device_type>][:<standard>]
  // <device_type> : 'v4l' for VideoForLinux, 'dv1394' for DV1394. By default 'v4l'
  // <standard>    : 'pal', 'secam' or 'ntsc'. By default 'ntsc'
  // The driver name is constructed automatically from the device type:
  // v4l   : /dev/video<camIdx>
  // dv1394: /dev/dv1394/<camIdx>
  // If you have different driver name, you can specify the driver name explicitly
  // instead of device type. Examples of valid filename:
  //    /dev/v4l/video0:pal
  //    /dev/ieee1394/1:ntsc
  //    dv1394:secam
  //    v4l:pal
  char *p;

  if (file && strstr(file, "1394") != nullptr) {
    // the user specifies a driver, check if it is v4l or d41394
    inputFormat = av_find_input_format("dv1394");
    sprintf(filename, "/dev/dv1394/%d", camIdx);
  }
  else {
    const char *formats[] = {"video4linux2,v4l2", "video4linux2", "video4linux"};
    int i, formatsCount = sizeof(formats) / sizeof(char *);
    for (i = 0; i < formatsCount; i++) {
      inputFormat = av_find_input_format(formats[i]);
      if (inputFormat)
        break;
    }
    sprintf(filename, "/dev/video%d", camIdx);
  }
  if (!inputFormat)
    // these format should be supported, check ffmpeg compilation
    return;
  if (file && strncmp(file, "/dev", 4) == 0) {
    // user does not specify a driver
    strncpy(filename, file, sizeof(filename));
    filename[sizeof(filename) - 1] = 0;
    if ((p = strchr(filename, ':')) != 0)
      *p = 0;
  }
  if (file && (p = strchr(file, ':')) != nullptr) {
    av_dict_set(&formatParams, "standard", p + 1, 0);
  }
#  endif
  // frame rate
  if (m_captRate <= 0.f)
    m_captRate = defFrameRate;
  sprintf(rateStr, "%f", m_captRate);

  av_dict_set(&formatParams, "framerate", rateStr, 0);

  if (m_captWidth > 0 && m_captHeight > 0) {
    char video_size[64];
    BLI_snprintf(video_size, sizeof(video_size), "%dx%d", m_captWidth, m_captHeight);
    av_dict_set(&formatParams, "video_size", video_size, 0);
  }

  if (openStream(filename, inputFormat, &formatParams) != 0)
    return;

  // for video capture it is important to do non blocking read
  m_formatCtx->flags |= AVFMT_FLAG_NONBLOCK;
  // open base class
  VideoBase::openCam(file, camIdx);
  // check if we should do multi-threading?
  if (BLI_system_thread_count() > 1) {
    // no need to thread if the system has a single core
    m_isThreaded = true;
  }

  av_dict_free(&formatParams);
}

// play video
bool VideoFFmpeg::play(void)
{
  try {
    // if object is able to play
    if (VideoBase::play()) {
      // set video position
      setPositions();

      if (m_isStreaming) {
        av_read_play(m_formatCtx);
      }

      // return success
      return true;
    }
  }
  CATCH_EXCP;
  return false;
}

// pause video
bool VideoFFmpeg::pause(void)
{
  try {
    if (VideoBase::pause()) {
      if (m_isStreaming) {
        av_read_pause(m_formatCtx);
      }
      return true;
    }
  }
  CATCH_EXCP;
  return false;
}

// stop video
bool VideoFFmpeg::stop(void)
{
  try {
    VideoBase::stop();
    // force restart when play
    m_lastFrame = -1;
    return true;
  }
  CATCH_EXCP;
  return false;
}

// set video range
void VideoFFmpeg::setRange(double start, double stop)
{
  try {
    // set range
    if (m_isFile) {
      VideoBase::setRange(start, stop);
      // set range for video
      setPositions();
    }
  }
  CATCH_EXCP;
}

// set framerate
void VideoFFmpeg::setFrameRate(float rate)
{
  VideoBase::setFrameRate(rate);
}

// image calculation
// load frame from video
void VideoFFmpeg::calcImage(unsigned int texId, double ts)
{
  if (m_status == SourcePlaying) {
    // get actual time
    double startTime = BLI_time_now_seconds();
    double actTime;
    // timestamp passed from audio actuators can sometimes be slightly negative
    if (m_isFile && ts >= -0.5) {
      // allow setting timestamp only when not streaming
      actTime = ts;
      if (actTime * actFrameRate() < m_lastFrame) {
        // user is asking to rewind, force a cache clear to make sure we will do a seek
        // note that this does not decrement m_repeat if ts didn't reach m_range[1]
        stopCache();
      }
    }
    else {
      if (m_lastFrame == -1 && !m_isFile)
        m_startTime = startTime;
      actTime = startTime - m_startTime;
    }
    // if video has ended
    if (m_isFile && actTime * m_frameRate >= m_range[1]) {
      // in any case, this resets the cache
      stopCache();
      // if repeats are set, decrease them
      if (m_repeat > 0)
        --m_repeat;
      // if video has to be replayed
      if (m_repeat != 0) {
        // reset its position
        actTime -= (m_range[1] - m_range[0]) / m_frameRate;
        m_startTime += (m_range[1] - m_range[0]) / m_frameRate;
      }
      // if video has to be stopped, stop it
      else {
        m_status = SourceStopped;
        return;
      }
    }
    // actual frame
    long actFrame = (m_isImage) ? m_lastFrame + 1 : long(actTime * actFrameRate());
    // if actual frame differs from last frame
    if (actFrame != m_lastFrame) {
      AVFrame *frame;
      // get image
      if ((frame = grabFrame(actFrame)) != nullptr) {
        if (!m_isFile && !m_cacheStarted) {
          // streaming without cache: detect synchronization problem
          double execTime = BLI_time_now_seconds() - startTime;
          if (execTime > 0.005) {
            // exec time is too long, it means that the function was blocking
            // resynchronize the stream from this time
            m_startTime += execTime;
          }
        }
        // save actual frame
        m_lastFrame = actFrame;
        // init image, if needed
        init(short(m_codecCtx->width), short(m_codecCtx->height));
        // process image
        process((BYTE *)(frame->data[0]));
        // finished with the frame, release it so that cache can reuse it
        releaseFrame(frame);
        // in case it is an image, automatically stop reading it
        if (m_isImage) {
          m_status = SourceStopped;
          // close the file as we don't need it anymore
          release();
        }
      }
      else if (m_isStreaming) {
        // we didn't get a frame and we are streaming, this may be due to
        // a delay in the network or because we are getting the frame too fast.
        // In the later case, shift time by a small amount to compensate for a drift
        m_startTime += 0.001;
      }
    }
  }
}

// set actual position
void VideoFFmpeg::setPositions(void)
{
  // set video start time
  m_startTime = BLI_time_now_seconds();
  // if file is played and actual position is before end position
  if (!m_eof && m_lastFrame >= 0 && (!m_isFile || m_lastFrame < m_range[1] * actFrameRate()))
    // continue from actual position
    m_startTime -= double(m_lastFrame) / actFrameRate();
  else {
    m_startTime -= m_range[0];
    // start from beginning, stop cache just in case
    stopCache();
  }
}

// position pointer in file, position in second
AVFrame *VideoFFmpeg::grabFrame(long position)
{
  AVPacket packet;
  int frameFinished;
  int posFound = 1;
  bool frameLoaded = false;
  int64_t targetTs = 0;
  CacheFrame *frame;
  int64_t dts = 0;

  if (m_cacheStarted) {
    // when cache is active, we must not read the file directly
    do {
      pthread_mutex_lock(&m_cacheMutex);
      frame = (CacheFrame *)m_frameCacheBase.first;
      pthread_mutex_unlock(&m_cacheMutex);
      // no need to remove the frame from the queue: the cache thread does not touch the head, only
      // the tail
      if (frame == nullptr) {
        // no frame in cache, in case of file it is an abnormal situation
        if (m_isFile) {
          // go back to no threaded reading
          stopCache();
          break;
        }
        return nullptr;
      }
      if (frame->framePosition == -1) {
        // this frame mark the end of the file (only used for file)
        // leave in cache to make sure we don't miss it
        m_eof = true;
        return nullptr;
      }
      // for streaming, always return the next frame,
      // that's what grabFrame does in non cache mode anyway.
      if (m_isStreaming || frame->framePosition == position) {
        return frame->frame;
      }
      // for cam, skip old frames to keep image realtime.
      // There should be no risk of clock drift since it all happens on the same CPU
      if (frame->framePosition > position) {
        // this can happen after rewind if the seek didn't find the first frame
        // the frame in the buffer is ahead of time, just leave it there
        return nullptr;
      }
      // this frame is not useful, release it
      pthread_mutex_lock(&m_cacheMutex);
      BLI_remlink(&m_frameCacheBase, frame);
      BLI_addtail(&m_frameCacheFree, frame);
      pthread_mutex_unlock(&m_cacheMutex);
    } while (true);
  }
  double timeBase = av_q2d(m_formatCtx->streams[m_videoStream]->time_base);
  int64_t startTs = m_formatCtx->streams[m_videoStream]->start_time;
  if (startTs == AV_NOPTS_VALUE)
    startTs = 0;

  // come here when there is no cache or cache has been stopped
  // locate the frame, by seeking if necessary (seeking is only possible for files)
  if (m_isFile) {
    // first check if the position that we are looking for is in the preseek range
    // if so, just read the frame until we get there
    if (position > m_curPosition + 1 && m_preseek && position - (m_curPosition + 1) < m_preseek) {
      while (av_read_frame(m_formatCtx, &packet) >= 0) {
        if (packet.stream_index == m_videoStream) {
          avcodec_send_packet(m_codecCtx, &packet);
          frameFinished = avcodec_receive_frame(m_codecCtx, m_frame) == 0;

          if (frameFinished) {
            m_curPosition = (long)((packet.dts - startTs) * (m_baseFrameRate * timeBase) + 0.5);
          }
        }
        av_packet_unref(&packet);
        if (position == m_curPosition + 1)
          break;
      }
    }
    // if the position is not in preseek, do a direct jump
    if (position != m_curPosition + 1) {
      int64_t pos = (int64_t)((position - m_preseek) / (m_baseFrameRate * timeBase));

      if (pos < 0)
        pos = 0;

      pos += startTs;

      if (position <= m_curPosition || !m_eof) {
#  if 0
				// Tried to make this work but couldn't: seeking on byte is ignored by the
				// format plugin and it will generally continue to read from last timestamp.
				// Too bad because frame seek is not always able to get the first frame
				// of the file.
				if (position <= m_preseek)
				{
					// we can safely go the beginning of the file
					if (av_seek_frame(m_formatCtx, m_videoStream, 0, AVSEEK_FLAG_BYTE) >= 0)
					{
						// binary seek does not reset the timestamp, must do it now
						av_update_cur_dts(m_formatCtx, m_formatCtx->streams[m_videoStream], startTs);
						m_curPosition = 0;
					}
				}
				else
#  endif
        {
          // current position is now lost, guess a value.
          if (av_seek_frame(m_formatCtx, m_videoStream, pos, AVSEEK_FLAG_BACKWARD) >= 0) {
            // current position is now lost, guess a value.
            // It's not important because it will be set at this end of this function
            m_curPosition = position - m_preseek - 1;
          }
        }
      }
      // this is the timestamp of the frame we're looking for
      targetTs = (int64_t)(position / (m_baseFrameRate * timeBase)) + startTs;

      posFound = 0;
      avcodec_flush_buffers(m_codecCtx);
    }
  }
  else if (m_isThreaded) {
    // cache is not started but threading is possible
    // better not read the stream => make take some time, better start caching
    if (startCache())
      return nullptr;
    // Abnormal!!! could not start cache, fall back on direct read
    m_isThreaded = false;
  }

  // find the correct frame, in case of streaming and no cache, it means just
  // return the next frame. This is not quite correct, may need more work
  while (av_read_frame(m_formatCtx, &packet) >= 0) {
    if (packet.stream_index == m_videoStream) {
      AVFrame *input = m_frame;
      short counter = 0;

      /* If m_isImage, while the data is not read properly (png, tiffs, etc formats may need
       * several pass), else don't need while loop*/
      do {
        avcodec_send_packet(m_codecCtx, &packet);
        frameFinished = avcodec_receive_frame(m_codecCtx, m_frame) == 0;

        counter++;
      } while ((input->data[0] == 0 && input->data[1] == 0 && input->data[2] == 0 &&
                input->data[3] == 0) &&
               counter < 10 && m_isImage);

      // remember dts to compute exact frame number
      dts = packet.dts;
      if (frameFinished && !posFound) {
        if (dts >= targetTs) {
          posFound = 1;
        }
      }

      if (frameFinished && posFound == 1) {
        AVFrame *input = m_frame;

        /* This means the data wasnt read properly,
         * this check stops crashing */
        if (input->data[0] == 0 && input->data[1] == 0 && input->data[2] == 0 &&
            input->data[3] == 0) {
          av_packet_unref(&packet);
          break;
        }

        if (m_deinterlace) {
          if (ffmpeg_deinterlace((AVFrame *)m_frameDeinterlaced,
                                   (const AVFrame *)m_frame,
                                   m_codecCtx->pix_fmt,
                                   m_codecCtx->width,
                                   m_codecCtx->height) >= 0) {
            input = m_frameDeinterlaced;
          }
        }
        // convert to RGB24
        sws_scale(m_imgConvertCtx,
                  input->data,
                  input->linesize,
                  0,
                  m_codecCtx->height,
                  m_frameRGB->data,
                  m_frameRGB->linesize);
        av_packet_unref(&packet);
        frameLoaded = true;
        break;
      }
    }
    av_packet_unref(&packet);
  }
  m_eof = m_isFile && !frameLoaded;
  if (frameLoaded) {
    m_curPosition = (long)((dts - startTs) * (m_baseFrameRate * timeBase) + 0.5);
    if (m_isThreaded) {
      // normal case for file: first locate, then start cache
      if (!startCache()) {
        // Abnormal!! could not start cache, return to non-cache mode
        m_isThreaded = false;
      }
    }
    return m_frameRGB;
  }
  return nullptr;
}

// python methods

// cast Image pointer to VideoFFmpeg
inline VideoFFmpeg *getVideoFFmpeg(PyImage *self)
{
  return static_cast<VideoFFmpeg *>(self->m_image);
}

// object initialization
static int VideoFFmpeg_init(PyObject *pySelf, PyObject *args, PyObject *kwds)
{
  PyImage *self = reinterpret_cast<PyImage *>(pySelf);
  // parameters - video source
  // file name or format type for capture (only for Linux: video4linux or dv1394)
  char *file = nullptr;
  // capture device number
  short capt = -1;
  // capture width, only if capt is >= 0
  short width = 0;
  // capture height, only if capt is >= 0
  short height = 0;
  // capture rate, only if capt is >= 0
  float rate = 25.f;

  static const char *kwlist[] = {"file", "capture", "rate", "width", "height", nullptr};

  // get parameters
  if (!PyArg_ParseTupleAndKeywords(args,
                                   kwds,
                                   "s|hfhh",
                                   const_cast<char **>(kwlist),
                                   &file,
                                   &capt,
                                   &rate,
                                   &width,
                                   &height)) {
    return -1;
  }

  try {
    // create video object
    Video_init<VideoFFmpeg>(self);

    // set thread usage
    getVideoFFmpeg(self)->initParams(width, height, rate);

    // open video source
    Video_open(getVideo(self), file, capt);
  }
  catch (Exception &exp) {
    exp.report();
    return -1;
  }
  // initialization succeded
  return 0;
}

static PyObject *VideoFFmpeg_getPreseek(PyImage *self, void *closure)
{
  return Py_BuildValue("h", getFFmpeg(self)->getPreseek());
}

// set range
static int VideoFFmpeg_setPreseek(PyImage *self, PyObject *value, void *closure)
{
  // check validity of parameter
  if (value == nullptr || !PyLong_Check(value)) {
    PyErr_SetString(PyExc_TypeError, "The value must be an integer");
    return -1;
  }
  // set preseek
  getFFmpeg(self)->setPreseek(PyLong_AsLong(value));
  // success
  return 0;
}

// get deinterlace
static PyObject *VideoFFmpeg_getDeinterlace(PyImage *self, void *closure)
{
  if (getFFmpeg(self)->getDeinterlace())
    Py_RETURN_TRUE;
  else
    Py_RETURN_FALSE;
}

// set flip
static int VideoFFmpeg_setDeinterlace(PyImage *self, PyObject *value, void *closure)
{
  // check parameter, report failure
  if (value == nullptr || !PyBool_Check(value)) {
    PyErr_SetString(PyExc_TypeError, "The value must be a bool");
    return -1;
  }
  // set deinterlace
  getFFmpeg(self)->setDeinterlace(value == Py_True);
  // success
  return 0;
}

// methods structure
static PyMethodDef videoMethods[] = {  // methods from VideoBase class
    {"play", (PyCFunction)Video_play, METH_NOARGS, "Play (restart) video"},
    {"pause", (PyCFunction)Video_pause, METH_NOARGS, "pause video"},
    {"stop", (PyCFunction)Video_stop, METH_NOARGS, "stop video (play will replay it from start)"},
    {"refresh", (PyCFunction)Video_refresh, METH_VARARGS, "Refresh video - get its status"},
    {nullptr}};
// attributes structure
static PyGetSetDef videoGetSets[] = {  // methods from VideoBase class
    {(char *)"status", (getter)Video_getStatus, nullptr, (char *)"video status", nullptr},
    {(char *)"range",
     (getter)Video_getRange,
     (setter)Video_setRange,
     (char *)"replay range",
     nullptr},
    {(char *)"repeat",
     (getter)Video_getRepeat,
     (setter)Video_setRepeat,
     (char *)"repeat count, -1 for infinite repeat",
     nullptr},
    {(char *)"framerate",
     (getter)Video_getFrameRate,
     (setter)Video_setFrameRate,
     (char *)"frame rate",
     nullptr},
    // attributes from ImageBase class
    {(char *)"valid",
     (getter)Image_valid,
     nullptr,
     (char *)"bool to tell if an image is available",
     nullptr},
    {(char *)"image", (getter)Image_getImage, nullptr, (char *)"image data", nullptr},
    {(char *)"size", (getter)Image_getSize, nullptr, (char *)"image size", nullptr},
    {(char *)"scale",
     (getter)Image_getScale,
     (setter)Image_setScale,
     (char *)"fast scale of image (near neighbor)",
     nullptr},
    {(char *)"flip",
     (getter)Image_getFlip,
     (setter)Image_setFlip,
     (char *)"flip image vertically",
     nullptr},
    {(char *)"filter",
     (getter)Image_getFilter,
     (setter)Image_setFilter,
     (char *)"pixel filter",
     nullptr},
    {(char *)"preseek",
     (getter)VideoFFmpeg_getPreseek,
     (setter)VideoFFmpeg_setPreseek,
     (char *)"nb of frames of preseek",
     nullptr},
    {(char *)"deinterlace",
     (getter)VideoFFmpeg_getDeinterlace,
     (setter)VideoFFmpeg_setDeinterlace,
     (char *)"deinterlace image",
     nullptr},
    {nullptr}};

// python type declaration
PyTypeObject VideoFFmpegType = {
    PyVarObject_HEAD_INIT(nullptr, 0) "VideoTexture.VideoFFmpeg", /*tp_name*/
    sizeof(PyImage),                                              /*tp_basicsize*/
    0,                                                            /*tp_itemsize*/
    (destructor)Image_dealloc,                                    /*tp_dealloc*/
    0,                                                            /*tp_print*/
    0,                                                            /*tp_getattr*/
    0,                                                            /*tp_setattr*/
    0,                                                            /*tp_compare*/
    0,                                                            /*tp_repr*/
    0,                                                            /*tp_as_number*/
    0,                                                            /*tp_as_sequence*/
    0,                                                            /*tp_as_mapping*/
    0,                                                            /*tp_hash */
    0,                                                            /*tp_call*/
    0,                                                            /*tp_str*/
    0,                                                            /*tp_getattro*/
    0,                                                            /*tp_setattro*/
    &imageBufferProcs,                                            /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT,                                           /*tp_flags*/
    "FFmpeg video source",                                        /* tp_doc */
    0,                                                            /* tp_traverse */
    0,                                                            /* tp_clear */
    0,                                                            /* tp_richcompare */
    0,                                                            /* tp_weaklistoffset */
    0,                                                            /* tp_iter */
    0,                                                            /* tp_iternext */
    videoMethods,                                                 /* tp_methods */
    0,                                                            /* tp_members */
    videoGetSets,                                                 /* tp_getset */
    0,                                                            /* tp_base */
    0,                                                            /* tp_dict */
    0,                                                            /* tp_descr_get */
    0,                                                            /* tp_descr_set */
    0,                                                            /* tp_dictoffset */
    (initproc)VideoFFmpeg_init,                                   /* tp_init */
    0,                                                            /* tp_alloc */
    Image_allocNew,                                               /* tp_new */
};

// object initialization
static int ImageFFmpeg_init(PyObject *pySelf, PyObject *args, PyObject *kwds)
{
  PyImage *self = reinterpret_cast<PyImage *>(pySelf);
  // parameters - video source
  // file name or format type for capture (only for Linux: video4linux or dv1394)
  char *file = nullptr;

  // get parameters
  if (!PyArg_ParseTuple(args, "s:ImageFFmpeg", &file))
    return -1;

  try {
    // create video object
    Video_init<VideoFFmpeg>(self);

    getVideoFFmpeg(self)->initParams(0, 0, 1.0, true);

    // open video source
    Video_open(getVideo(self), file, -1);
  }
  catch (Exception &exp) {
    exp.report();
    return -1;
  }
  // initialization succeded
  return 0;
}

static PyObject *Image_reload(PyImage *self, PyObject *args)
{
  char *newname = nullptr;
  if (!PyArg_ParseTuple(args, "|s:reload", &newname))
    return nullptr;
  if (self->m_image != nullptr) {
    VideoFFmpeg *video = getFFmpeg(self);
    // check type of object
    if (!newname)
      newname = video->getImageName();
    if (!newname) {
      // if not set, retport error
      PyErr_SetString(PyExc_RuntimeError, "No image file name given");
      return nullptr;
    }
    // make sure the previous file is cleared
    video->release();
    // open the new file
    video->openFile(newname);
  }
  Py_RETURN_NONE;
}

// methods structure
static PyMethodDef imageMethods[] = {  // methods from VideoBase class
    {"refresh", (PyCFunction)Video_refresh, METH_VARARGS, "Refresh image, i.e. load it"},
    {"reload", (PyCFunction)Image_reload, METH_VARARGS, "Reload image, i.e. reopen it"},
    {nullptr}};
// attributes structure
static PyGetSetDef imageGetSets[] = {  // methods from VideoBase class
    {(char *)"status", (getter)Video_getStatus, nullptr, (char *)"video status", nullptr},
    // attributes from ImageBase class
    {(char *)"valid",
     (getter)Image_valid,
     nullptr,
     (char *)"bool to tell if an image is available",
     nullptr},
    {(char *)"image", (getter)Image_getImage, nullptr, (char *)"image data", nullptr},
    {(char *)"size", (getter)Image_getSize, nullptr, (char *)"image size", nullptr},
    {(char *)"scale",
     (getter)Image_getScale,
     (setter)Image_setScale,
     (char *)"fast scale of image (near neighbor)",
     nullptr},
    {(char *)"flip",
     (getter)Image_getFlip,
     (setter)Image_setFlip,
     (char *)"flip image vertically",
     nullptr},
    {(char *)"filter",
     (getter)Image_getFilter,
     (setter)Image_setFilter,
     (char *)"pixel filter",
     nullptr},
    {nullptr}};

// python type declaration
PyTypeObject ImageFFmpegType = {
    PyVarObject_HEAD_INIT(nullptr, 0) "VideoTexture.ImageFFmpeg", /*tp_name*/
    sizeof(PyImage),                                              /*tp_basicsize*/
    0,                                                            /*tp_itemsize*/
    (destructor)Image_dealloc,                                    /*tp_dealloc*/
    0,                                                            /*tp_print*/
    0,                                                            /*tp_getattr*/
    0,                                                            /*tp_setattr*/
    0,                                                            /*tp_compare*/
    0,                                                            /*tp_repr*/
    0,                                                            /*tp_as_number*/
    0,                                                            /*tp_as_sequence*/
    0,                                                            /*tp_as_mapping*/
    0,                                                            /*tp_hash */
    0,                                                            /*tp_call*/
    0,                                                            /*tp_str*/
    0,                                                            /*tp_getattro*/
    0,                                                            /*tp_setattro*/
    &imageBufferProcs,                                            /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT,                                           /*tp_flags*/
    "FFmpeg image source",                                        /* tp_doc */
    0,                                                            /* tp_traverse */
    0,                                                            /* tp_clear */
    0,                                                            /* tp_richcompare */
    0,                                                            /* tp_weaklistoffset */
    0,                                                            /* tp_iter */
    0,                                                            /* tp_iternext */
    imageMethods,                                                 /* tp_methods */
    0,                                                            /* tp_members */
    imageGetSets,                                                 /* tp_getset */
    0,                                                            /* tp_base */
    0,                                                            /* tp_dict */
    0,                                                            /* tp_descr_get */
    0,                                                            /* tp_descr_set */
    0,                                                            /* tp_dictoffset */
    (initproc)ImageFFmpeg_init,                                   /* tp_init */
    0,                                                            /* tp_alloc */
    Image_allocNew,                                               /* tp_new */
};

#endif  // WITH_FFMPEG
