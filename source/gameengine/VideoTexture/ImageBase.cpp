/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2006 The Zdeno Ash Miklas. */

/** \file gameengine/VideoTexture/ImageBase.cpp
 *  \ingroup bgevideotex
 */

#include "ImageBase.h"

#include <epoxy/gl.h>

#include "Exception.h"

#if (defined(WIN32) || defined(WIN64))
#  define strcasecmp _stricmp
#endif

// ImageBase class implementation

ExceptionID ImageHasExports;
ExceptionID InvalidColorChannel;
ExceptionID InvalidImageMode;

ExpDesc ImageHasExportsDesc(ImageHasExports, "Image has exported buffers, cannot resize");
ExpDesc InvalidColorChannelDesc(
    InvalidColorChannel,
    "Invalid or too many color channels specified. At most 4 values within R, G, B, A, 0, 1");
ExpDesc InvalidImageModeDesc(InvalidImageMode,
                             "Invalid image mode, only RGBA and BGRA are supported");

// constructor
ImageBase::ImageBase(bool staticSrc)
    : m_image(nullptr),
      m_imgSize(0),
      m_internalFormat(blender::gpu::TextureFormat::UNORM_8_8_8_8),
      m_avail(false),
      m_scale(false),
      m_scaleChange(false),
      m_flip(false),
      m_zbuff(false),
      m_depth(false),
      m_staticSources(staticSrc),
      m_pyfilter(nullptr)
{
  m_size[0] = m_size[1] = 0;
  m_exports = 0;
}

// destructor
ImageBase::~ImageBase(void)
{
  // release image
  if (m_image)
    MEM_freeN(m_image);
}

// release python objects
bool ImageBase::release(void)
{
  // iterate sources
  for (ImageSourceList::iterator it = m_sources.begin(); it != m_sources.end(); ++it) {
    // release source object
    delete *it;
    *it = nullptr;
  }
  // release filter object
  Py_XDECREF(m_pyfilter);
  m_pyfilter = nullptr;
  return true;
}

// get image
unsigned int *ImageBase::getImage(unsigned int texId, double ts)
{
  // if image is not available
  if (!m_avail) {
    // if there are any sources
    if (!m_sources.empty()) {
      // get images from sources
      for (ImageSourceList::iterator it = m_sources.begin(); it != m_sources.end(); ++it)
        // get source image
        (*it)->getImage(ts);
      // init image
      init(m_sources[0]->getSize()[0], m_sources[0]->getSize()[1]);
    }
    // calculate new image
    calcImage(texId, ts);
  }
  // if image is available, return it, otherwise nullptr
  return m_avail ? m_image : nullptr;
}

bool ImageBase::loadImage(unsigned int *buffer, unsigned int size, double ts)
{
  if (getImage(0, ts) != nullptr && size >= getBuffSize()) {
    memcpy(buffer, m_image, getBuffSize());
    return true;
  }
  return false;
}

// refresh image source
void ImageBase::refresh(void)
{
  // invalidate this image
  m_avail = false;
  // refresh all sources
  for (ImageSourceList::iterator it = m_sources.begin(); it != m_sources.end(); ++it)
    (*it)->refresh();
}

// get source object
PyImage *ImageBase::getSource(const char *id)
{
  // find source
  ImageSourceList::iterator src = findSource(id);
  // return it, if found
  return src != m_sources.end() ? (*src)->getSource() : nullptr;
}

// set source object
bool ImageBase::setSource(const char *id, PyImage *source)
{
  // find source
  ImageSourceList::iterator src = findSource(id);
  // check source loop
  if (source != nullptr && source->m_image->loopDetect(this))
    return false;
  // if found, set new object
  if (src != m_sources.end())
    // if new object is not empty or sources are static
    if (source != nullptr || m_staticSources)
      // replace previous source
      (*src)->setSource(source);
    // otherwise delete source
    else
      m_sources.erase(src);
  // if source is not found and adding is allowed
  else if (!m_staticSources) {
    // create new source
    ImageSource *newSrc = newSource(id);
    newSrc->setSource(source);
    // if source was created, add it to source list
    if (newSrc != nullptr)
      m_sources.push_back(newSrc);
  }
  // otherwise source wasn't set
  else
    return false;
  // source was set
  return true;
}

// set pixel filter
void ImageBase::setFilter(PyFilter *filt)
{
  // reference new filter
  if (filt != nullptr)
    Py_INCREF(filt);
  // release previous filter
  Py_XDECREF(m_pyfilter);
  // set new filter
  m_pyfilter = filt;
}

void ImageBase::swapImageBR()
{
  unsigned int size, v, *s;

  if (m_avail) {
    size = 1 * m_size[0] * m_size[1];
    for (s = m_image; size; size--) {
      v = *s;
      *s++ = VT_SWAPBR(v);
    }
  }
}

// initialize image data
void ImageBase::init(short width, short height)
{
  // if image has to be scaled
  if (m_scale) {
    // recalc sizes of image
    width = calcSize(width);
    height = calcSize(height);
  }
  // if sizes differ
  if (width != m_size[0] || height != m_size[1]) {
    if (m_exports > 0)
      THRWEXCP(ImageHasExports, S_OK);

    // new buffer size
    unsigned int newSize = width * height;
    // if new buffer is larger than previous
    if (newSize > m_imgSize) {
      // set new buffer size
      m_imgSize = newSize;
      // release previous and create new buffer
      if (m_image)
        MEM_freeN(m_image);
      m_image = (unsigned int *)MEM_mallocN(m_imgSize * sizeof(unsigned int), "ImageBase init");
    }
    // new image size
    m_size[0] = width;
    m_size[1] = height;
    // scale was processed
    m_scaleChange = false;
  }
}

// find source
ImageSourceList::iterator ImageBase::findSource(const char *id)
{
  // iterate sources
  ImageSourceList::iterator it;
  for (it = m_sources.begin(); it != m_sources.end(); ++it)
    // if id matches, return iterator
    if ((*it)->is(id))
      return it;
  // source not found
  return it;
}

// check sources sizes
bool ImageBase::checkSourceSizes(void)
{
  // reference size
  short *refSize = nullptr;
  // iterate sources
  for (ImageSourceList::iterator it = m_sources.begin(); it != m_sources.end(); ++it) {
    // get size of current source
    short *curSize = (*it)->getSize();
    // if size is available and is not empty
    if (curSize[0] != 0 && curSize[1] != 0) {
      // if reference size is not set
      if (refSize == nullptr) {
        // set current size as reference
        refSize = curSize;
        // otherwise check with current size
      }
      else if (curSize[0] != refSize[0] || curSize[1] != refSize[1]) {
        // if they don't match, report it
        return false;
      }
    }
  }
  // all sizes match
  return true;
}

// compute nearest power of 2 value
short ImageBase::calcSize(short size)
{
  // while there is more than 1 bit in size value
  while ((size & (size - 1)) != 0)
    // clear last bit
    size = size & (size - 1);
  // return result
  return size;
}

// perform loop detection
bool ImageBase::loopDetect(ImageBase *img)
{
  // if this object is the same as parameter, loop is detected
  if (this == img)
    return true;
  // check all sources
  for (ImageSourceList::iterator it = m_sources.begin(); it != m_sources.end(); ++it)
    // if source detected loop, return this result
    if ((*it)->getSource() != nullptr && (*it)->getSource()->m_image->loopDetect(img))
      return true;
  // no loop detected
  return false;
}

// ImageSource class implementation

// constructor
ImageSource::ImageSource(const char *id) : m_source(nullptr), m_image(nullptr)
{
  // copy id
  int idx;
  for (idx = 0; id[idx] != '\0' && idx < SourceIdSize - 1; ++idx)
    m_id[idx] = id[idx];
  m_id[idx] = '\0';
}

// destructor
ImageSource::~ImageSource(void)
{
  // release source
  setSource(nullptr);
}

// compare id
bool ImageSource::is(const char *id)
{
  for (char *myId = m_id; *myId != '\0'; ++myId, ++id)
    if (*myId != *id)
      return false;
  return *id == '\0';
}

// set source object
void ImageSource::setSource(PyImage *source)
{
  // reference new source
  if (source != nullptr)
    Py_INCREF(source);
  // release previous source
  Py_XDECREF(m_source);
  // set new source
  m_source = source;
}

// get image from source
unsigned int *ImageSource::getImage(double ts)
{
  // if source is available
  if (m_source != nullptr)
    // get image from source
    m_image = m_source->m_image->getImage(0, ts);
  // otherwise reset buffer
  else
    m_image = nullptr;
  // return image
  return m_image;
}

// refresh source
void ImageSource::refresh(void)
{
  // if source is available, refresh it
  if (m_source != nullptr)
    m_source->m_image->refresh();
}

// list of image types
PyTypeList pyImageTypes;

// functions for python interface

// object allocation
PyObject *Image_allocNew(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
  // allocate object
  PyImage *self = reinterpret_cast<PyImage *>(type->tp_alloc(type, 0));
  // initialize object structure
  self->m_image = nullptr;
  // return allocated object
  return reinterpret_cast<PyObject *>(self);
}

// object deallocation
void Image_dealloc(PyImage *self)
{
  // release object attributes
  if (self->m_image != nullptr) {
    if (self->m_image->m_exports > 0) {
      PyErr_SetString(PyExc_SystemError, "deallocated Image object has exported buffers");
      PyErr_Print();
    }
    // if release requires deleting of object, do it
    if (self->m_image->release())
      delete self->m_image;
    self->m_image = nullptr;
  }
  Py_TYPE((PyObject *)self)->tp_free((PyObject *)self);
}

// get image data
PyObject *Image_getImage(PyImage *self, char *mode)
{
  try {
    unsigned int *image = self->m_image->getImage();
    if (image) {
      int dimensions = self->m_image->getBuffSize();
      PyObject *py_buffer = nullptr;

      // Default: return the raw RGBA8 buffer as Python bytes
      if (mode == nullptr || !strcasecmp(mode, "RGBA")) {
        py_buffer = PyBytes_FromStringAndSize((const char *)image, dimensions);
      }
      // "F" mode: return the buffer as float array (for depth, if applicable)
      else if (!strcasecmp(mode, "F")) {
        int float_count = dimensions / sizeof(float);
        py_buffer = PyBytes_FromStringAndSize((const char *)image, float_count * sizeof(float));
      }
      // Custom channel selection (e.g. "R", "RG", "BGA", etc.)
      else {
        int i, c, ncolor = 0, pixels = dimensions / 4;
        int offset[4];
        unsigned char *s, *d;
        // Parse the mode string to determine which channels to extract (max 4)
        for (i = 0; mode[i] != 0 && ncolor < 4; i++) {
          switch (toupper(mode[i])) {
            case 'R':
              offset[ncolor++] = 0;
              break;
            case 'G':
              offset[ncolor++] = 1;
              break;
            case 'B':
              offset[ncolor++] = 2;
              break;
            case 'A':
              offset[ncolor++] = 3;
              break;
            case '0':
              offset[ncolor++] = -1;
              break;
            case '1':
              offset[ncolor++] = -2;
              break;
            default:
              THRWEXCP(InvalidColorChannel, S_OK);
          }
        }
        if (mode[i] != 0) {
          THRWEXCP(InvalidColorChannel, S_OK);
        }
        int out_size = pixels * ncolor;
        py_buffer = PyBytes_FromStringAndSize(nullptr, out_size);
        d = (unsigned char *)PyBytes_AS_STRING(py_buffer);
        s = (unsigned char *)image;
        // Fill the output buffer with the selected channels
        for (i = 0; i < pixels; i++, s += 4, d += ncolor) {
          for (c = 0; c < ncolor; c++) {
            switch (offset[c]) {
              case 0:
                d[c] = s[0];
                break;
              case 1:
                d[c] = s[1];
                break;
              case 2:
                d[c] = s[2];
                break;
              case 3:
                d[c] = s[3];
                break;
              case -1:
                d[c] = 0;
                break;
              case -2:
                d[c] = 0xFF;
                break;
            }
          }
        }
      }
      return py_buffer;
    }
  }
  catch (Exception &exp) {
    exp.report();
    return nullptr;
  }
  Py_RETURN_NONE;
}

// get image size
PyObject *Image_getSize(PyImage *self, void *closure)
{
  return Py_BuildValue("(hh)", self->m_image->getSize()[0], self->m_image->getSize()[1]);
}

// refresh image
PyObject *Image_refresh(PyImage *self, PyObject *args)
{
  Py_buffer buffer;
  bool done = true;
  char *mode = nullptr;
  double ts = -1.0;

  memset(&buffer, 0, sizeof(buffer));
  if (PyArg_ParseTuple(args, "|s*sd:refresh", &buffer, &mode, &ts)) {
    if (buffer.buf) {
      // a target buffer is provided, verify its format
      if (buffer.readonly) {
        PyErr_SetString(PyExc_TypeError, "Buffers passed in argument must be writable");
      }
      else if (!PyBuffer_IsContiguous(&buffer, 'C')) {
        PyErr_SetString(PyExc_TypeError,
                        "Buffers passed in argument must be contiguous in memory");
      }
      else if (((intptr_t)buffer.buf & 3) != 0) {
        PyErr_SetString(PyExc_TypeError,
                        "Buffers passed in argument must be aligned to 4 bytes boundary");
      }
      else {
        // ready to get the image into our buffer
        try {
          done = self->m_image->loadImage((unsigned int *)buffer.buf, buffer.len, ts);
        }
        catch (Exception &exp) {
          exp.report();
        }
      }
      PyBuffer_Release(&buffer);
      if (PyErr_Occurred()) {
        return nullptr;
      }
    }
  }
  else {
    return nullptr;
  }

  self->m_image->refresh();
  if (done)
    Py_RETURN_TRUE;
  Py_RETURN_FALSE;
}

// get scale
PyObject *Image_getScale(PyImage *self, void *closure)
{
  if (self->m_image != nullptr && self->m_image->getScale())
    Py_RETURN_TRUE;
  else
    Py_RETURN_FALSE;
}

// set scale
int Image_setScale(PyImage *self, PyObject *value, void *closure)
{
  // check parameter, report failure
  if (value == nullptr || !PyBool_Check(value)) {
    PyErr_SetString(PyExc_TypeError, "The value must be a bool");
    return -1;
  }
  // set scale
  if (self->m_image != nullptr)
    self->m_image->setScale(value == Py_True);
  // success
  return 0;
}

// get flip
PyObject *Image_getFlip(PyImage *self, void *closure)
{
  if (self->m_image != nullptr && self->m_image->getFlip())
    Py_RETURN_TRUE;
  else
    Py_RETURN_FALSE;
}

// set flip
int Image_setFlip(PyImage *self, PyObject *value, void *closure)
{
  // check parameter, report failure
  if (value == nullptr || !PyBool_Check(value)) {
    PyErr_SetString(PyExc_TypeError, "The value must be a bool");
    return -1;
  }
  // set scale
  if (self->m_image != nullptr)
    self->m_image->setFlip(value == Py_True);
  // success
  return 0;
}

// get zbuff
PyObject *Image_getZbuff(PyImage *self, void *closure)
{
  if (self->m_image != nullptr && self->m_image->getZbuff())
    Py_RETURN_TRUE;
  else
    Py_RETURN_FALSE;
}

// set zbuff
int Image_setZbuff(PyImage *self, PyObject *value, void *closure)
{
  // check parameter, report failure
  if (value == nullptr || !PyBool_Check(value)) {
    PyErr_SetString(PyExc_TypeError, "The value must be a bool");
    return -1;
  }
  // set scale
  if (self->m_image != nullptr)
    self->m_image->setZbuff(value == Py_True);
  // success
  return 0;
}

// get depth
PyObject *Image_getDepth(PyImage *self, void *closure)
{
  if (self->m_image != nullptr && self->m_image->getDepth())
    Py_RETURN_TRUE;
  else
    Py_RETURN_FALSE;
}

// set depth
int Image_setDepth(PyImage *self, PyObject *value, void *closure)
{
  // check parameter, report failure
  if (value == nullptr || !PyBool_Check(value)) {
    PyErr_SetString(PyExc_TypeError, "The value must be a bool");
    return -1;
  }
  // set scale
  if (self->m_image != nullptr)
    self->m_image->setDepth(value == Py_True);
  // success
  return 0;
}

// get filter source object
PyObject *Image_getSource(PyImage *self, PyObject *args)
{
  // get arguments
  char *id;
  if (!PyArg_ParseTuple(args, "s:getSource", &id))
    return nullptr;
  if (self->m_image != nullptr) {
    // get source object
    PyObject *src = reinterpret_cast<PyObject *>(self->m_image->getSource(id));
    // if source is available
    if (src != nullptr) {
      // return source
      Py_INCREF(src);
      return src;
    }
  }
  // source was not found
  Py_RETURN_NONE;
}

// set filter source object
PyObject *Image_setSource(PyImage *self, PyObject *args)
{
  // get arguments
  char *id;
  PyObject *obj;
  if (!PyArg_ParseTuple(args, "sO:setSource", &id, &obj))
    return nullptr;
  if (self->m_image != nullptr) {
    // check type of object
    if (pyImageTypes.in(Py_TYPE(obj))) {
      // convert to image struct
      PyImage *img = reinterpret_cast<PyImage *>(obj);
      // set source
      if (!self->m_image->setSource(id, img)) {
        // if not set, retport error
        PyErr_SetString(PyExc_RuntimeError, "Invalid source or id");
        return nullptr;
      }
    }
    // else report error
    else {
      PyErr_SetString(PyExc_RuntimeError, "Invalid type of object");
      return nullptr;
    }
  }
  // return none
  Py_RETURN_NONE;
}

// get pixel filter object
PyObject *Image_getFilter(PyImage *self, void *closure)
{
  // if image object is available
  if (self->m_image != nullptr) {
    // pixel filter object
    PyObject *filt = reinterpret_cast<PyObject *>(self->m_image->getFilter());
    // if filter is present
    if (filt != nullptr) {
      // return it
      Py_INCREF(filt);
      return filt;
    }
  }
  // otherwise return none
  Py_RETURN_NONE;
}

// set pixel filter object
int Image_setFilter(PyImage *self, PyObject *value, void *closure)
{
  // if image object is available
  if (self->m_image != nullptr) {
    // check new value
    if (value == nullptr || !pyFilterTypes.in(Py_TYPE(value))) {
      // report value error
      PyErr_SetString(PyExc_TypeError, "Invalid type of value");
      return -1;
    }
    // set new value
    self->m_image->setFilter(reinterpret_cast<PyFilter *>(value));
  }
  // return success
  return 0;
}
PyObject *Image_valid(PyImage *self, void *closure)
{
  if (self->m_image->isImageAvailable()) {
    Py_RETURN_TRUE;
  }
  else {
    Py_RETURN_FALSE;
  }
}

static int Image_getbuffer(PyImage *self, Py_buffer *view, int flags)
{
  unsigned int *image;
  int ret;

  try {
    // can throw in case of resize
    image = self->m_image->getImage();
  }
  catch (Exception &exp) {
    exp.report();
    return -1;
  }

  if (!image) {
    PyErr_SetString(PyExc_BufferError, "Image buffer is not available");
    return -1;
  }
  if (view == nullptr) {
    self->m_image->m_exports++;
    return 0;
  }
  ret = PyBuffer_FillInfo(view, (PyObject *)self, image, self->m_image->getBuffSize(), 0, flags);
  if (ret >= 0)
    self->m_image->m_exports++;
  return ret;
}

static void Image_releaseBuffer(PyImage *self, Py_buffer *buffer)
{
  self->m_image->m_exports--;
}

PyBufferProcs imageBufferProcs = {(getbufferproc)Image_getbuffer,
                                  (releasebufferproc)Image_releaseBuffer};
