/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2006 The Zdeno Ash Miklas. */

/** \file gameengine/VideoTexture/FilterColor.cpp
 *  \ingroup bgevideotex
 */

#include "FilterColor.h"

// implementation FilterGray

// attributes structure
static PyGetSetDef filterGrayGetSets[] = {  // attributes from FilterBase class
    {(char *)"previous",
     (getter)Filter_getPrevious,
     (setter)Filter_setPrevious,
     (char *)"previous pixel filter",
     nullptr},
    {nullptr}};

// define python type
PyTypeObject FilterGrayType = {
    PyVarObject_HEAD_INIT(nullptr, 0) "VideoTexture.FilterGray", /*tp_name*/
    sizeof(PyFilter),                                            /*tp_basicsize*/
    0,                                                           /*tp_itemsize*/
    (destructor)Filter_dealloc,                                  /*tp_dealloc*/
    0,                                                           /*tp_print*/
    0,                                                           /*tp_getattr*/
    0,                                                           /*tp_setattr*/
    0,                                                           /*tp_compare*/
    0,                                                           /*tp_repr*/
    0,                                                           /*tp_as_number*/
    0,                                                           /*tp_as_sequence*/
    0,                                                           /*tp_as_mapping*/
    0,                                                           /*tp_hash */
    0,                                                           /*tp_call*/
    0,                                                           /*tp_str*/
    0,                                                           /*tp_getattro*/
    0,                                                           /*tp_setattro*/
    0,                                                           /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT,                                          /*tp_flags*/
    "Filter for grayscale effect",                               /* tp_doc */
    0,                                                           /* tp_traverse */
    0,                                                           /* tp_clear */
    0,                                                           /* tp_richcompare */
    0,                                                           /* tp_weaklistoffset */
    0,                                                           /* tp_iter */
    0,                                                           /* tp_iternext */
    nullptr,                                                     /* tp_methods */
    0,                                                           /* tp_members */
    filterGrayGetSets,                                           /* tp_getset */
    0,                                                           /* tp_base */
    0,                                                           /* tp_dict */
    0,                                                           /* tp_descr_get */
    0,                                                           /* tp_descr_set */
    0,                                                           /* tp_dictoffset */
    (initproc)Filter_init<FilterGray>,                           /* tp_init */
    0,                                                           /* tp_alloc */
    Filter_allocNew,                                             /* tp_new */
};

// implementation FilterColor

// constructor
FilterColor::FilterColor(void)
{
  // reset color matrix to identity
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 5; ++c)
      m_matrix[r][c] = (r == c) ? 256 : 0;
}

// set color matrix
void FilterColor::setMatrix(ColorMatrix &mat)
{
  // copy matrix
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 5; ++c)
      m_matrix[r][c] = mat[r][c];
}

// cast Filter pointer to FilterColor
inline FilterColor *getFilterColor(PyFilter *self)
{
  return static_cast<FilterColor *>(self->m_filter);
}

// python methods and get/sets

// get color matrix
static PyObject *getMatrix(PyFilter *self, void *closure)
{
  ColorMatrix &mat = getFilterColor(self)->getMatrix();
  return Py_BuildValue("((hhhhh)(hhhhh)(hhhhh)(hhhhh))",
                       mat[0][0],
                       mat[0][1],
                       mat[0][2],
                       mat[0][3],
                       mat[0][4],
                       mat[1][0],
                       mat[1][1],
                       mat[1][2],
                       mat[1][3],
                       mat[1][4],
                       mat[2][0],
                       mat[2][1],
                       mat[2][2],
                       mat[2][3],
                       mat[2][4],
                       mat[3][0],
                       mat[3][1],
                       mat[3][2],
                       mat[3][3],
                       mat[3][4]);
}

// set color matrix
static int setMatrix(PyFilter *self, PyObject *value, void *closure)
{
  // matrix to store items
  ColorMatrix mat;
  // check validity of parameter
  bool valid = value != nullptr && PySequence_Check(value) && PySequence_Size(value) == 4;
  // check rows
  for (int r = 0; valid && r < 4; ++r) {
    // get row object
    PyObject *row = PySequence_Fast_GET_ITEM(value, r);
    // check sequence
    valid = PySequence_Check(row) && PySequence_Size(row) == 5;
    // check items
    for (int c = 0; valid && c < 5; ++c) {
      // item must be int
      valid = PyLong_Check(PySequence_Fast_GET_ITEM(row, c));
      // if it is valid, save it in matrix
      if (valid)
        mat[r][c] = short(PyLong_AsLong(PySequence_Fast_GET_ITEM(row, c)));
    }
  }
  // if parameter is not valid, report error
  if (!valid) {
    PyErr_SetString(PyExc_TypeError, "The value must be a matrix [4][5] of ints");
    return -1;
  }
  // set color matrix
  getFilterColor(self)->setMatrix(mat);
  // success
  return 0;
}

// attributes structure
static PyGetSetDef filterColorGetSets[] = {{(char *)"matrix",
                                            (getter)getMatrix,
                                            (setter)setMatrix,
                                            (char *)"matrix [4][5] for color calculation",
                                            nullptr},
                                           // attributes from FilterBase class
                                           {(char *)"previous",
                                            (getter)Filter_getPrevious,
                                            (setter)Filter_setPrevious,
                                            (char *)"previous pixel filter",
                                            nullptr},
                                           {nullptr}};

// define python type
PyTypeObject FilterColorType = {
    PyVarObject_HEAD_INIT(nullptr, 0) "VideoTexture.FilterColor", /*tp_name*/
    sizeof(PyFilter),                                             /*tp_basicsize*/
    0,                                                            /*tp_itemsize*/
    (destructor)Filter_dealloc,                                   /*tp_dealloc*/
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
    0,                                                            /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT,                                           /*tp_flags*/
    "Filter for color calculations",                              /* tp_doc */
    0,                                                            /* tp_traverse */
    0,                                                            /* tp_clear */
    0,                                                            /* tp_richcompare */
    0,                                                            /* tp_weaklistoffset */
    0,                                                            /* tp_iter */
    0,                                                            /* tp_iternext */
    nullptr,                                                      /* tp_methods */
    0,                                                            /* tp_members */
    filterColorGetSets,                                           /* tp_getset */
    0,                                                            /* tp_base */
    0,                                                            /* tp_dict */
    0,                                                            /* tp_descr_get */
    0,                                                            /* tp_descr_set */
    0,                                                            /* tp_dictoffset */
    (initproc)Filter_init<FilterColor>,                           /* tp_init */
    0,                                                            /* tp_alloc */
    Filter_allocNew,                                              /* tp_new */
};

// implementation FilterLevel

// constructor
FilterLevel::FilterLevel(void)
{
  // reset color levels
  for (int r = 0; r < 4; ++r) {
    levels[r][0] = 0;
    levels[r][1] = 0xFF;
    levels[r][2] = 0xFF;
  }
}

// set color levels
void FilterLevel::setLevels(ColorLevel &lev)
{
  // copy levels
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 2; ++c)
      levels[r][c] = lev[r][c];
    levels[r][2] = lev[r][0] < lev[r][1] ? lev[r][1] - lev[r][0] : 1;
  }
}

// cast Filter pointer to FilterLevel
inline FilterLevel *getFilterLevel(PyFilter *self)
{
  return static_cast<FilterLevel *>(self->m_filter);
}

// python methods and get/sets

// get color levels
static PyObject *getLevels(PyFilter *self, void *closure)
{
  ColorLevel &lev = getFilterLevel(self)->getLevels();
  return Py_BuildValue("((HH)(HH)(HH)(HH))",
                       lev[0][0],
                       lev[0][1],
                       lev[1][0],
                       lev[1][1],
                       lev[2][0],
                       lev[2][1],
                       lev[3][0],
                       lev[3][1]);
}

// set color levels
static int setLevels(PyFilter *self, PyObject *value, void *closure)
{
  // matrix to store items
  ColorLevel lev;
  // check validity of parameter
  bool valid = value != nullptr && PySequence_Check(value) && PySequence_Size(value) == 4;
  // check rows
  for (int r = 0; valid && r < 4; ++r) {
    // get row object
    PyObject *row = PySequence_Fast_GET_ITEM(value, r);
    // check sequence
    valid = PySequence_Check(row) && PySequence_Size(row) == 2;
    // check items
    for (int c = 0; valid && c < 2; ++c) {
      // item must be int
      valid = PyLong_Check(PySequence_Fast_GET_ITEM(row, c));
      // if it is valid, save it in matrix
      if (valid)
        lev[r][c] = (unsigned short)(PyLong_AsLong(PySequence_Fast_GET_ITEM(row, c)));
    }
  }
  // if parameter is not valid, report error
  if (!valid) {
    PyErr_SetString(PyExc_TypeError, "The value must be a matrix [4][2] of ints");
    return -1;
  }
  // set color matrix
  getFilterLevel(self)->setLevels(lev);
  // success
  return 0;
}

// attributes structure
static PyGetSetDef filterLevelGetSets[] = {{(char *)"levels",
                                            (getter)getLevels,
                                            (setter)setLevels,
                                            (char *)"levels matrix [4] (min, max)",
                                            nullptr},
                                           // attributes from FilterBase class
                                           {(char *)"previous",
                                            (getter)Filter_getPrevious,
                                            (setter)Filter_setPrevious,
                                            (char *)"previous pixel filter",
                                            nullptr},
                                           {nullptr}};

// define python type
PyTypeObject FilterLevelType = {
    PyVarObject_HEAD_INIT(nullptr, 0) "VideoTexture.FilterLevel", /*tp_name*/
    sizeof(PyFilter),                                             /*tp_basicsize*/
    0,                                                            /*tp_itemsize*/
    (destructor)Filter_dealloc,                                   /*tp_dealloc*/
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
    0,                                                            /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT,                                           /*tp_flags*/
    "Filter for levels calculations",                             /* tp_doc */
    0,                                                            /* tp_traverse */
    0,                                                            /* tp_clear */
    0,                                                            /* tp_richcompare */
    0,                                                            /* tp_weaklistoffset */
    0,                                                            /* tp_iter */
    0,                                                            /* tp_iternext */
    nullptr,                                                      /* tp_methods */
    0,                                                            /* tp_members */
    filterLevelGetSets,                                           /* tp_getset */
    0,                                                            /* tp_base */
    0,                                                            /* tp_dict */
    0,                                                            /* tp_descr_get */
    0,                                                            /* tp_descr_set */
    0,                                                            /* tp_dictoffset */
    (initproc)Filter_init<FilterLevel>,                           /* tp_init */
    0,                                                            /* tp_alloc */
    Filter_allocNew,                                              /* tp_new */
};
