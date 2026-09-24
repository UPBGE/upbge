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
#  include <algorithm>

#  include "MEM_guardedalloc.h"

#  include "Exception.h"
#  include "BLI_listbase.hh"
#  include "BLI_string.hh"
#  include "BLI_time.hh"
#  include "BLI_threads.hh"  // for BLI_system_thread_count
#  include "movie_util.hh"

// MovieReader API (Blender imbuf movie module)
#  include "MOV_read.hh"

extern "C" {
#  include <libavutil/imgutils.h>
#  include <libavcodec/avcodec.h>

using namespace blender;
}

// default framerate
const double defFrameRate = 25.0;

// Profiling: enabled via env var BGE_VIDEO_PROFILE=1
static bool g_videoProfileEnabled = false;
static int g_videoProfileFrameCount = 0;
static double g_videoProfileDecodeTotal = 0.0;

static void video_profile_init()
{
  const char *env = getenv("BGE_VIDEO_PROFILE");
  if (env && env[0] == '1') {
    g_videoProfileEnabled = true;
    printf("[VideoFFmpeg] Profiling ENABLED\n");
  }
}

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
      m_movieReader(nullptr),
      m_deinterlace(false),
      m_preseek(0),
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
      m_isStreaming(false)
{
  // set video format
  m_format = RGB24;
  // construction is OK
  *hRslt = S_OK;
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
  if (m_movieReader) {
    MOV_close(m_movieReader);
    m_movieReader = nullptr;
  }
  m_status = SourceStopped;
  m_lastFrame = -1;
  return true;
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
  // For file-based video, use the imbuf MovieReader (Blender's movie_read.cc).
  // This gives us all the benefits of Blender's seek/decode logic:
  // - Smart keyframe seeking with 3-frame offset
  // - VFR double-buffer fallback
  // - VP8/VP9 alpha workaround
  // - MPEGTS generic seek workaround
  // - EOF flush (buffered frames)
  // - WebM variable resolution
  // - OGG album art workaround
  if (inputFormat == nullptr) {
    ImBufFlags flags = m_deinterlace ? ImBufFlags::Deinterlace : ImBufFlags::Zero;
    m_movieReader = MOV_open_file(filename, flags, 0, true, nullptr);
    if (m_movieReader == nullptr) {
      return -1;
    }

    // MOV_open_file triggers probe_video_colorspace which calls anim_getnew,
    // so the reader should be initialized at this point. Verify.
    if (!MOV_is_initialized_and_valid(m_movieReader)) {
      printf("VideoFFmpeg: failed to initialize movie reader for '%s'\n", filename);
      MOV_close(m_movieReader);
      m_movieReader = nullptr;
      return -1;
    }

    m_captWidth = (short)MOV_get_image_width(m_movieReader);
    m_captHeight = (short)MOV_get_image_height(m_movieReader);
    float fps = MOV_get_fps(m_movieReader);
    m_baseFrameRate = (fps > 0.0f) ? (double)fps : defFrameRate;

    // Duration in seconds
    int duration_frames = MOV_get_duration_frames(m_movieReader);
    m_range[0] = 0.0;
    m_range[1] = (m_baseFrameRate > 0.0) ? (double)duration_frames / m_baseFrameRate : 0.0;

    // Always use RGBA32 format for the texture buffer.
    m_format = RGBA32;
    return 0;
  }

  // For camera capture (inputFormat != nullptr), use the imbuf MovieReader in
  // device mode. The reader decodes frames sequentially (no seek, no duration),
  // which is what a live capture stream provides.
  {
    const char *format_name = (inputFormat != nullptr) ? inputFormat->name : nullptr;
    double framerate = (m_captRate > 0.f) ? (double)m_captRate : 0.0;

    ImBufFlags flags = m_deinterlace ? ImBufFlags::Deinterlace : ImBufFlags::Zero;
    m_movieReader = MOV_open_device(filename,
                                    format_name,
                                    m_captWidth,
                                    m_captHeight,
                                    framerate,
                                    flags);
    if (m_movieReader == nullptr) {
      printf("VideoFFmpeg: failed to open capture device '%s'\n", filename);
      return -1;
    }

    m_captWidth = (short)MOV_get_image_width(m_movieReader);
    m_captHeight = (short)MOV_get_image_height(m_movieReader);
    float fps = MOV_get_fps(m_movieReader);
    m_baseFrameRate = (fps > 0.0f) ? (double)fps : defFrameRate;

    /* Live stream: no known duration, the range is left at 0 and never checked
     * in calcImage() because m_isFile is false for capture devices. */
    m_range[0] = 0.0;
    m_range[1] = 0.0;

    /* Always use RGBA32 format for the texture buffer. */
    m_format = RGBA32;
    return 0;
  }
}

// open video file
void VideoFFmpeg::openFile(char *filename)
{
  if (openStream(filename, nullptr, nullptr) != 0)
    return;

  // MovieReader handles seeking internally with smart keyframe logic.
  // No need for preseek calculation.
  m_preseek = 0;

  // open base class
  VideoBase::openFile(filename);

  if (
      // ffmpeg reports that http source are actually non stream
      // but it is really not desirable to seek on http file, so force streaming.
      !strncmp(filename, "http://", 7) || !strncmp(filename, "rtsp://", 7)) {
    // the file is in fact a streaming source, treat as cam to prevent seeking
    m_isFile = false;
    m_isStreaming = true;
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
}

// open video capture device
void VideoFFmpeg::openCam(char *file, short camIdx)
{
  // Open the capture source through the imbuf MovieReader (device mode).
  // The demuxer is selected per platform, exactly like before:
  // - Windows: dshow, device name "video=<camIdx>"
  // - Linux:   v4l2, device name "/dev/video<camIdx>"
  const char *format_name;
  char filename[64];

#  ifdef WIN32
  format_name = "dshow";
  BLI_snprintf(filename, sizeof(filename), "video=%s", (file != nullptr) ? file : "0");
#  else
  format_name = "v4l2";
  /* A full device path can be given explicitly: use it as-is. */
  if (file != nullptr && strncmp(file, "/dev", 4) == 0) {
    BLI_strncpy(filename, file, sizeof(filename));
  }
  else {
    BLI_snprintf(filename, sizeof(filename), "/dev/video%d", camIdx);
  }
#  endif

  if (openStream(filename, av_find_input_format(format_name), nullptr) != 0)
    return;

  // open base class
  VideoBase::openCam(file, camIdx);
}

// play video
bool VideoFFmpeg::play(void)
{
  try {
    // if object is able to play
    if (VideoBase::play()) {
      // set video position
      setPositions();
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
  // Init profiling on first call
  if (!g_videoProfileEnabled && g_videoProfileFrameCount == 0) {
    video_profile_init();
  }

  if (m_status == SourcePlaying) {
    // get actual time
    double startTime = BLI_time_now_seconds();
    double actTime;
    // timestamp passed from audio actuators can sometimes be slightly negative
    if (m_isFile && ts >= -0.5) {
      // allow setting timestamp only when not streaming
      actTime = ts;
    }
    else {
      if (m_lastFrame == -1 && !m_isFile)
        m_startTime = startTime;
      actTime = startTime - m_startTime;
    }
    // if video has ended
    if (m_isFile && actTime * m_frameRate >= m_range[1]) {
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
      double t_decode_start = g_videoProfileEnabled ? BLI_time_now_seconds() : 0.0;

      // Clamp the frame to valid range.
      int duration_frames = MOV_get_duration_frames(m_movieReader);
      int frame_to_decode = (int)actFrame;
      if (frame_to_decode >= duration_frames) {
        frame_to_decode = duration_frames - 1;
      }
      if (frame_to_decode < 0) {
        frame_to_decode = 0;
      }

      // Ensure the image buffer is allocated at the correct size.
      init(m_captWidth, m_captHeight);

      bool decoded = false;
      if (m_pixelsData != nullptr && !m_avail) {
        if (!m_isFile) {
          // Live capture device: decode the next frame in order, no seeking.
          decoded = grabDeviceFrame();
        }
        else {
          // Decode directly into our texture buffer (RGBA, vertical-flipped).
          // This avoids an intermediate buffer + copy through the filter pipeline.
          decoded = MOV_decode_frame_to_buffer(
              m_movieReader, frame_to_decode,
              (uint8_t *)m_pixelsData, m_captWidth, m_captHeight);
        }

        if (decoded) {
          m_avail = true;
        }
      }

      double t_decode_end = g_videoProfileEnabled ? BLI_time_now_seconds() : 0.0;

      if (!m_isFile) {
        // streaming: detect synchronization problem
        double execTime = BLI_time_now_seconds() - startTime;
        if (execTime > 0.005) {
          // exec time is too long, it means that the function was blocking
          // resynchronize the stream from this time
          m_startTime += execTime;
        }
      }

      if (decoded) {
        // save actual frame
        m_lastFrame = actFrame;

        // Profiling output
        if (g_videoProfileEnabled) {
          double decode_ms = (t_decode_end - t_decode_start) * 1000.0;
          g_videoProfileDecodeTotal += decode_ms;
          g_videoProfileFrameCount++;

          // Print every 60 frames or on first frame
          if (g_videoProfileFrameCount == 1 || g_videoProfileFrameCount % 60 == 0) {
            double avg_decode = g_videoProfileDecodeTotal / g_videoProfileFrameCount;
            printf("[VideoFFmpeg] Frame %d: decode=%.2fms | avg decode=%.2fms total_frames=%d\n",
                   g_videoProfileFrameCount, decode_ms, avg_decode, g_videoProfileFrameCount);
          }
        }

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

// decode the next frame from a live capture device into the texture buffer
bool VideoFFmpeg::grabDeviceFrame(void)
{
  // Ensure the image buffer is allocated at the correct size.
  init(m_captWidth, m_captHeight);
  if (m_pixelsData == nullptr) {
    return false;
  }

  // Decode directly into our texture buffer (RGBA, vertical-flipped).
  bool decoded = MOV_decode_next_frame_to_buffer(
      m_movieReader, (uint8_t *)m_pixelsData, m_captWidth, m_captHeight);

  if (!decoded) {
    // No frame right now: a capture device with no signal can return errors.
    // Keep the last good frame displayed (m_avail stays false so the previous
    // texture content is reused by the rasterizer).
    return false;
  }

  m_lastFrame++;
  return true;
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
  }
}

// python methods

// cast blender::Image pointer to VideoFFmpeg
inline VideoFFmpeg *getVideoFFmpeg(PyImage *self)
{
  return static_cast<VideoFFmpeg *>(self->m_imageBase);
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
  if (self->m_imageBase != nullptr) {
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
