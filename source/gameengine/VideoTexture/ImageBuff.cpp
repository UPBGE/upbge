/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2006 The Zdeno Ash Miklas. */

/** \file gameengine/VideoTexture/ImageBuff.cpp
 *  \ingroup bgevideotex
 */

// implementation

#include "ImageBuff.h"

#include "IMB_imbuf.hh"

#include "Exception.h"
#include "FilterSource.h"

// default filter
FilterRGB24 defFilter;

// forward declaration;
extern PyTypeObject ImageBuffType;

static int ImageBuff_init(PyObject *pySelf, PyObject *args, PyObject *kwds)
{
  short width = -1;
  short height = -1;
  unsigned char color = 0;
  PyObject *py_scale = Py_False;
  ImageBuff *image;

  PyImage *self = reinterpret_cast<PyImage *>(pySelf);
  // create source object
  if (self->m_image != nullptr)
    delete self->m_image;
  image = new ImageBuff();
  self->m_image = image;

  if (PyArg_ParseTuple(
          args, "hh|bO!:ImageBuff", &width, &height, &color, &PyBool_Type, &py_scale)) {
    // initialize image buffer
    image->setScale(py_scale == Py_True);
    image->clear(width, height, color);
  }
  else {
    // check if at least one argument was passed
    if (width != -1 || height != -1)
      // yes and they didn't match => it's an error
      return -1;
    // empty argument list is okay
    PyErr_Clear();
  }
  // initialization succeded
  return 0;
}

ImageBuff::~ImageBuff(void)
{
  if (m_imbuf)
    IMB_freeImBuf(m_imbuf);
}

// load image from buffer
void ImageBuff::load(unsigned char *img, short width, short height)
{
  // loading a new buffer implies to reset the imbuf if any, because the size may change
  if (m_imbuf) {
    IMB_freeImBuf(m_imbuf);
    m_imbuf = nullptr;
  }
  // initialize image buffer
  init(width, height);
  // original size
  short orgSize[2] = {width, height};
  // is filter available
  if (m_pyfilter != nullptr)
    // use it to process image
    convImage(*(m_pyfilter->m_filter), img, orgSize);
  else
    // otherwise use default filter
    convImage(defFilter, img, orgSize);
  // image is available
  m_avail = true;
}

void ImageBuff::clear(short width, short height, unsigned char color)
{
  unsigned char *p;
  int size;

  // loading a new buffer implies to reset the imbuf if any, because the size may change
  if (m_imbuf) {
    IMB_freeImBuf(m_imbuf);
    m_imbuf = nullptr;
  }
  // initialize image buffer
  init(width, height);
  // the width/height may be different due to scaling
  size = (m_size[0] * m_size[1]);
  // initialize memory with color for all channels
  memset(m_image, color, size * 4);
  // and change the alpha channel
  p = &((unsigned char *)m_image)[3];
  for (; size > 0; size--) {
    *p = 0xFF;
    p += 4;
  }
  // image is available
  m_avail = true;
}

// img must point to a array of RGBA data of size width*height
void ImageBuff::plot(unsigned char *img, short width, short height, short x, short y, short mode)
{
  struct ImBuf *tmpbuf;

  if (m_size[0] == 0 || m_size[1] == 0 || width <= 0 || height <= 0)
    return;

  if (!m_imbuf) {
    // allocate most basic imbuf, we will assign the rect buffer on the fly
    m_imbuf = IMB_allocImBuf(m_size[0], m_size[1], 0, 0);
  }

  tmpbuf = IMB_allocImBuf(width, height, 0, 0);

  // assign temporarily our buffer to the ImBuf buffer, we use the same format
  tmpbuf->byte_buffer.data = (uint8_t *)img;
  m_imbuf->byte_buffer.data = (uint8_t *)m_image;
  IMB_rectblend(m_imbuf,
                m_imbuf,
                tmpbuf,
                nullptr,
                nullptr,
                nullptr,
                0,
                x,
                y,
                x,
                y,
                0,
                0,
                width,
                height,
                (IMB_BlendMode)mode,
                false);
  // remove so that MB_freeImBuf will free our buffer
  m_imbuf->byte_buffer.data = nullptr;
  tmpbuf->byte_buffer.data = nullptr;
  IMB_freeImBuf(tmpbuf);
}

void ImageBuff::plot(ImageBuff *img, short x, short y, short mode)
{
  if (m_size[0] == 0 || m_size[1] == 0 || img->m_size[0] == 0 || img->m_size[1] == 0)
    return;

  if (!m_imbuf) {
    // allocate most basic imbuf, we will assign the rect buffer on the fly
    m_imbuf = IMB_allocImBuf(m_size[0], m_size[1], 0, 0);
  }
  if (!img->m_imbuf) {
    // allocate most basic imbuf, we will assign the rect buffer on the fly
    img->m_imbuf = IMB_allocImBuf(img->m_size[0], img->m_size[1], 0, 0);
  }
  // assign temporarily our buffer to the ImBuf buffer, we use the same format
  img->m_imbuf->byte_buffer.data = (uint8_t *)img->m_image;
  m_imbuf->byte_buffer.data = (uint8_t *)m_image;
  IMB_rectblend(m_imbuf,
                m_imbuf,
                img->m_imbuf,
                nullptr,
                nullptr,
                nullptr,
                0,
                x,
                y,
                x,
                y,
                0,
                0,
                img->m_imbuf->x,
                img->m_imbuf->y,
                (IMB_BlendMode)mode,
                false);
  // remove so that MB_freeImBuf will free our buffer
  m_imbuf->byte_buffer.data = nullptr;
  img->m_imbuf->byte_buffer.data = nullptr;
}

// cast Image pointer to ImageBuff
inline ImageBuff *getImageBuff(PyImage *self)
{
  return static_cast<ImageBuff *>(self->m_image);
}

// python methods

static bool testPyBuffer(Py_buffer *buffer, int width, int height, unsigned int pixsize)
{
  if (buffer->itemsize != 1) {
    PyErr_SetString(PyExc_ValueError, "Buffer must be an array of bytes");
    return false;
  }
  if (buffer->len != width * height * pixsize) {
    PyErr_SetString(PyExc_ValueError, "Buffer hasn't the correct size");
    return false;
  }
  // multi dimension are ok as long as there is no hole in the memory
  Py_ssize_t size = buffer->itemsize;
  for (int i = buffer->ndim - 1; i >= 0; i--) {
    if (buffer->suboffsets != nullptr && buffer->suboffsets[i] >= 0) {
      PyErr_SetString(PyExc_ValueError, "Buffer must be of one block");
      return false;
    }
    if (buffer->strides != nullptr && buffer->strides[i] != size) {
      PyErr_SetString(PyExc_ValueError, "Buffer must be of one block");
      return false;
    }
    if (i > 0)
      size *= buffer->shape[i];
  }
  return true;
}

// load image
static PyObject *load(PyImage *self, PyObject *args)
{
  // parameters: string image buffer, its size, width, height
  Py_buffer buffer;
  short width;
  short height;
  unsigned int pixSize;

  // calc proper buffer size
  // use pixel size from filter
  if (self->m_image->getFilter() != nullptr)
    pixSize = self->m_image->getFilter()->m_filter->firstPixelSize();
  else
    pixSize = defFilter.firstPixelSize();

  // parse parameters: only accept Python buffer, width, height
  if (!PyArg_ParseTuple(args, "s*hh:load", &buffer, &width, &height)) {
    // If parsing fails, report error and return
    PyErr_SetString(PyExc_TypeError, "Expected a Python buffer, width, and height as arguments");
    return nullptr;
  }
  // parse parameters: only accept Python buffer, width, height
  if (!PyArg_ParseTuple(args, "s*hh:load", &buffer, &width, &height)) {
    // If parsing fails, report error and return
    PyErr_SetString(PyExc_TypeError, "Expected a Python buffer, width, and height as arguments");
    return nullptr;
  }
  // Check if buffer size is correct
  if (testPyBuffer(&buffer, width, height, pixSize)) {
    try {
      // If correct, load image
      getImageBuff(self)->load((unsigned char *)buffer.buf, width, height);
    }
    catch (Exception &exp) {
      exp.report();
    }
  }
  PyBuffer_Release(&buffer);
  if (PyErr_Occurred())
    return nullptr;
  Py_RETURN_NONE;
}

static PyObject *plot(PyImage *self, PyObject *args)
{
  PyImage *other;
  Py_buffer buffer;
  short width;
  short height;
  short x, y;
  short mode = IMB_BLEND_COPY;

  if (PyArg_ParseTuple(args, "s*hhhh|h:plot", &buffer, &width, &height, &x, &y, &mode)) {
    // correct decoding, verify that buffer size is correct
    // we need a continuous memory buffer
    if (testPyBuffer(&buffer, width, height, 4)) {
      getImageBuff(self)->plot((unsigned char *)buffer.buf, width, height, x, y, mode);
    }
    PyBuffer_Release(&buffer);
    if (PyErr_Occurred())
      return nullptr;
    Py_RETURN_NONE;
  }
  PyErr_Clear();
  // try the other format
  if (PyArg_ParseTuple(args, "O!hh|h:plot", &ImageBuffType, &other, &x, &y, &mode)) {
    getImageBuff(self)->plot(getImageBuff(other), x, y, mode);
    Py_RETURN_NONE;
  }
  PyErr_Clear();
  PyErr_SetString(PyExc_TypeError,
                  "Expecting ImageBuff or Python buffer as first argument; width, height next; "
                  "position x, y and mode as last arguments");
  return nullptr;
}

// methods structure
static PyMethodDef imageBuffMethods[] = {
    {"load", (PyCFunction)load, METH_VARARGS, "Load image from buffer"},
    {"plot", (PyCFunction)plot, METH_VARARGS, "update image buffer"},
    {nullptr}};
// attributes structure
static PyGetSetDef imageBuffGetSets[] = {
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

// define python type
PyTypeObject ImageBuffType = {
    PyVarObject_HEAD_INIT(nullptr, 0) "VideoTexture.ImageBuff", /*tp_name*/
    sizeof(PyImage),                                            /*tp_basicsize*/
    0,                                                          /*tp_itemsize*/
    (destructor)Image_dealloc,                                  /*tp_dealloc*/
    0,                                                          /*tp_print*/
    0,                                                          /*tp_getattr*/
    0,                                                          /*tp_setattr*/
    0,                                                          /*tp_compare*/
    0,                                                          /*tp_repr*/
    0,                                                          /*tp_as_number*/
    0,                                                          /*tp_as_sequence*/
    0,                                                          /*tp_as_mapping*/
    0,                                                          /*tp_hash */
    0,                                                          /*tp_call*/
    0,                                                          /*tp_str*/
    0,                                                          /*tp_getattro*/
    0,                                                          /*tp_setattro*/
    &imageBufferProcs,                                          /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT,                                         /*tp_flags*/
    "Image source from image buffer",                           /* tp_doc */
    0,                                                          /* tp_traverse */
    0,                                                          /* tp_clear */
    0,                                                          /* tp_richcompare */
    0,                                                          /* tp_weaklistoffset */
    0,                                                          /* tp_iter */
    0,                                                          /* tp_iternext */
    imageBuffMethods,                                           /* tp_methods */
    0,                                                          /* tp_members */
    imageBuffGetSets,                                           /* tp_getset */
    0,                                                          /* tp_base */
    0,                                                          /* tp_dict */
    0,                                                          /* tp_descr_get */
    0,                                                          /* tp_descr_set */
    0,                                                          /* tp_dictoffset */
    (initproc)ImageBuff_init,                                   /* tp_init */
    0,                                                          /* tp_alloc */
    Image_allocNew,                                             /* tp_new */
};
