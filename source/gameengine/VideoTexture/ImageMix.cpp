/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2007 The Zdeno Ash Miklas. */

/** \file gameengine/VideoTexture/ImageMix.cpp
 *  \ingroup bgevideotex
 */

// implementation

#include "ImageMix.h"

#include "Exception.h"

// cast ImageSource pointer to ImageSourceMix
inline ImageSourceMix *getImageSourceMix(ImageSource *src)
{
  return static_cast<ImageSourceMix *>(src);
}

// get weight
short ImageMix::getWeight(const char *id)
{
  // find source
  ImageSourceList::iterator src = findSource(id);
  // if found, return its weight
  return src != m_sources.end() ? getImageSourceMix(*src)->getWeight() : 0;
}

// set weight
bool ImageMix::setWeight(const char *id, short weight)
{
  // find source
  ImageSourceList::iterator src = findSource(id);
  // if source isn't found, report it
  if (src == m_sources.end())
    return false;
  // set its weight
  getImageSourceMix(*src)->setWeight(weight);
  return true;
}

ExceptionID ImageSizesNotMatch;

ExpDesc ImageSizesNotMatchDesc(ImageSizesNotMatch, "Image sizes of sources are different");

// calculate image from sources and set its availability
void ImageMix::calcImage(unsigned int texId, double ts)
{
  // check source sizes
  if (!checkSourceSizes())
    THRWEXCP(ImageSizesNotMatch, S_OK);
  // set offsets to image buffers
  for (ImageSourceList::iterator it = m_sources.begin(); it != m_sources.end(); ++it)
    // if image buffer is available
    if ((*it)->getImageBuf() != nullptr)
      // set its offset
      getImageSourceMix(*it)->setOffset(m_sources[0]->getImageBuf());
    // otherwise don't calculate image
    else
      return;
  // if there is only single source
  if (m_sources.size() == 1) {
    // use single filter
    FilterBase mixFilt;
    // fiter and convert image
    filterImage(mixFilt, m_sources[0]->getImageBuf(), m_sources[0]->getSize());
  }
  // otherwise use mix filter to merge source images
  else {
    FilterImageMix mixFilt(m_sources);
    // fiter and convert image
    filterImage(mixFilt, m_sources[0]->getImageBuf(), m_sources[0]->getSize());
  }
}

// cast Image pointer to ImageMix
inline ImageMix *getImageMix(PyImage *self)
{
  return static_cast<ImageMix *>(self->m_image);
}

// python methods

// get source weight
static PyObject *getWeight(PyImage *self, PyObject *args)
{
  // weight
  short weight = 0;
  // get arguments
  char *id;
  if (!PyArg_ParseTuple(args, "s:getWeight", &id))
    return nullptr;
  if (self->m_image != nullptr)
    // get weight
    weight = getImageMix(self)->getWeight(id);
  // return weight
  return Py_BuildValue("h", weight);
}

// set source weight
static PyObject *setWeight(PyImage *self, PyObject *args)
{
  // get arguments
  char *id;
  short weight = 0;
  if (!PyArg_ParseTuple(args, "sh:setWeight", &id, &weight))
    return nullptr;
  if (self->m_image != nullptr)
    // set weight
    if (!getImageMix(self)->setWeight(id, weight)) {
      // if not set, report error
      PyErr_SetString(PyExc_RuntimeError, "Invalid id of source");
      return nullptr;
    }
  // return none
  Py_RETURN_NONE;
}

// methods structure
static PyMethodDef imageMixMethods[] = {
    {"getSource", (PyCFunction)Image_getSource, METH_VARARGS, "get image source"},
    {"setSource", (PyCFunction)Image_setSource, METH_VARARGS, "set image source"},
    {"getWeight", (PyCFunction)getWeight, METH_VARARGS, "get image source weight"},
    {"setWeight", (PyCFunction)setWeight, METH_VARARGS, "set image source weight"},
    // methods from ImageBase class
    {"refresh",
     (PyCFunction)Image_refresh,
     METH_VARARGS,
     "Refresh image - invalidate its current content"},
    {nullptr}};
// attributes structure
static PyGetSetDef imageMixGetSets[] = {
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
PyTypeObject ImageMixType = {
    PyVarObject_HEAD_INIT(nullptr, 0) "VideoTexture.ImageMix", /*tp_name*/
    sizeof(PyImage),                                           /*tp_basicsize*/
    0,                                                         /*tp_itemsize*/
    (destructor)Image_dealloc,                                 /*tp_dealloc*/
    0,                                                         /*tp_print*/
    0,                                                         /*tp_getattr*/
    0,                                                         /*tp_setattr*/
    0,                                                         /*tp_compare*/
    0,                                                         /*tp_repr*/
    0,                                                         /*tp_as_number*/
    0,                                                         /*tp_as_sequence*/
    0,                                                         /*tp_as_mapping*/
    0,                                                         /*tp_hash */
    0,                                                         /*tp_call*/
    0,                                                         /*tp_str*/
    0,                                                         /*tp_getattro*/
    0,                                                         /*tp_setattro*/
    &imageBufferProcs,                                         /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT,                                        /*tp_flags*/
    "Image mixer",                                             /* tp_doc */
    0,                                                         /* tp_traverse */
    0,                                                         /* tp_clear */
    0,                                                         /* tp_richcompare */
    0,                                                         /* tp_weaklistoffset */
    0,                                                         /* tp_iter */
    0,                                                         /* tp_iternext */
    imageMixMethods,                                           /* tp_methods */
    0,                                                         /* tp_members */
    imageMixGetSets,                                           /* tp_getset */
    0,                                                         /* tp_base */
    0,                                                         /* tp_dict */
    0,                                                         /* tp_descr_get */
    0,                                                         /* tp_descr_set */
    0,                                                         /* tp_dictoffset */
    (initproc)Image_init<ImageMix>,                            /* tp_init */
    0,                                                         /* tp_alloc */
    Image_allocNew,                                            /* tp_new */
};
