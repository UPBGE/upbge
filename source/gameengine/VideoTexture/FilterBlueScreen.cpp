/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2006 The Zdeno Ash Miklas. */

/** \file gameengine/VideoTexture/FilterBlueScreen.cpp
 *  \ingroup bgevideotex
 */

#include "FilterBlueScreen.h"

// implementation FilterBlueScreen

// constructor
FilterBlueScreen::FilterBlueScreen(void)
{
  // set color to blue
  setColor(0, 0, 255);
  // set limits
  setLimits(64, 64);
}

// set color
void FilterBlueScreen::setColor(unsigned char red, unsigned char green, unsigned char blue)
{
  m_color[0] = red;
  m_color[1] = green;
  m_color[2] = blue;
}

// set limits for color variation
void FilterBlueScreen::setLimits(unsigned short minLimit, unsigned short maxLimit)
{
  m_limits[0] = minLimit;
  m_limits[1] = maxLimit > minLimit ? maxLimit : minLimit;
  // calculate square values
  for (short idx = 0; idx < 2; ++idx)
    m_squareLimits[idx] = m_limits[idx] * m_limits[idx];
  // limits distance
  m_limitDist = m_squareLimits[1] - m_squareLimits[0];
}

// cast Filter pointer to FilterBlueScreen
inline FilterBlueScreen *getFilter(PyFilter *self)
{
  return static_cast<FilterBlueScreen *>(self->m_filter);
}

// python methods and get/sets

// get color
static PyObject *getColor(PyFilter *self, void *closure)
{
  return Py_BuildValue("[BBB]",
                       getFilter(self)->getColor()[0],
                       getFilter(self)->getColor()[1],
                       getFilter(self)->getColor()[2]);
}

// set color
static int setColor(PyFilter *self, PyObject *value, void *closure)
{
  // check validity of parameter
  if (value == nullptr || !(PyTuple_Check(value) || PyList_Check(value)) ||
      PySequence_Fast_GET_SIZE(value) != 3 || !PyLong_Check(PySequence_Fast_GET_ITEM(value, 0)) ||
      !PyLong_Check(PySequence_Fast_GET_ITEM(value, 1)) ||
      !PyLong_Check(PySequence_Fast_GET_ITEM(value, 2))) {
    PyErr_SetString(PyExc_TypeError, "The value must be a sequence of 3 ints");
    return -1;
  }
  // set color
  getFilter(self)->setColor((unsigned char)(PyLong_AsLong(PySequence_Fast_GET_ITEM(value, 0))),
                            (unsigned char)(PyLong_AsLong(PySequence_Fast_GET_ITEM(value, 1))),
                            (unsigned char)(PyLong_AsLong(PySequence_Fast_GET_ITEM(value, 2))));
  // success
  return 0;
}

// get limits
static PyObject *getLimits(PyFilter *self, void *closure)
{
  return Py_BuildValue("[II]", getFilter(self)->getLimits()[0], getFilter(self)->getLimits()[1]);
}

// set limit
static int setLimits(PyFilter *self, PyObject *value, void *closure)
{
  // check validity of parameter
  if (value == nullptr || !(PyTuple_Check(value) || PyList_Check(value)) ||
      PySequence_Fast_GET_SIZE(value) != 2 || !PyLong_Check(PySequence_Fast_GET_ITEM(value, 0)) ||
      !PyLong_Check(PySequence_Fast_GET_ITEM(value, 1))) {
    PyErr_SetString(PyExc_TypeError, "The value must be a sequence of 2 ints");
    return -1;
  }
  // set limits
  getFilter(self)->setLimits((unsigned short)(PyLong_AsLong(PySequence_Fast_GET_ITEM(value, 0))),
                             (unsigned short)(PyLong_AsLong(PySequence_Fast_GET_ITEM(value, 1))));
  // success
  return 0;
}

// attributes structure
static PyGetSetDef filterBSGetSets[] = {
    {(char *)"color", (getter)getColor, (setter)setColor, (char *)"blue screen color", nullptr},
    {(char *)"limits",
     (getter)getLimits,
     (setter)setLimits,
     (char *)"blue screen color limits",
     nullptr},
    // attributes from FilterBase class
    {(char *)"previous",
     (getter)Filter_getPrevious,
     (setter)Filter_setPrevious,
     (char *)"previous pixel filter",
     nullptr},
    {nullptr}};

// define python type
PyTypeObject FilterBlueScreenType = {
    PyVarObject_HEAD_INIT(nullptr, 0) "VideoTexture.FilterBlueScreen", /*tp_name*/
    sizeof(PyFilter),                                                  /*tp_basicsize*/
    0,                                                                 /*tp_itemsize*/
    (destructor)Filter_dealloc,                                        /*tp_dealloc*/
    0,                                                                 /*tp_print*/
    0,                                                                 /*tp_getattr*/
    0,                                                                 /*tp_setattr*/
    0,                                                                 /*tp_compare*/
    0,                                                                 /*tp_repr*/
    0,                                                                 /*tp_as_number*/
    0,                                                                 /*tp_as_sequence*/
    0,                                                                 /*tp_as_mapping*/
    0,                                                                 /*tp_hash */
    0,                                                                 /*tp_call*/
    0,                                                                 /*tp_str*/
    0,                                                                 /*tp_getattro*/
    0,                                                                 /*tp_setattro*/
    0,                                                                 /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT,                                                /*tp_flags*/
    "Filter for Blue Screen objects",                                  /* tp_doc */
    0,                                                                 /* tp_traverse */
    0,                                                                 /* tp_clear */
    0,                                                                 /* tp_richcompare */
    0,                                                                 /* tp_weaklistoffset */
    0,                                                                 /* tp_iter */
    0,                                                                 /* tp_iternext */
    nullptr,                                                           /* tp_methods */
    0,                                                                 /* tp_members */
    filterBSGetSets,                                                   /* tp_getset */
    0,                                                                 /* tp_base */
    0,                                                                 /* tp_dict */
    0,                                                                 /* tp_descr_get */
    0,                                                                 /* tp_descr_set */
    0,                                                                 /* tp_dictoffset */
    (initproc)Filter_init<FilterBlueScreen>,                           /* tp_init */
    0,                                                                 /* tp_alloc */
    Filter_allocNew,                                                   /* tp_new */
};
