/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2006 The Zdeno Ash Miklas. */

/** \file gameengine/VideoTexture/FilterSource.cpp
 *  \ingroup bgevideotex
 */

// implementation

#include "FilterSource.h"

// FilterRGB24

// define python type
PyTypeObject FilterRGB24Type = {
    PyVarObject_HEAD_INIT(nullptr, 0) "VideoTexture.FilterRGB24", /*tp_name*/
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
    "Source filter RGB24 objects",                                /* tp_doc */
    0,                                                            /* tp_traverse */
    0,                                                            /* tp_clear */
    0,                                                            /* tp_richcompare */
    0,                                                            /* tp_weaklistoffset */
    0,                                                            /* tp_iter */
    0,                                                            /* tp_iternext */
    nullptr,                                                      /* tp_methods */
    0,                                                            /* tp_members */
    nullptr,                                                      /* tp_getset */
    0,                                                            /* tp_base */
    0,                                                            /* tp_dict */
    0,                                                            /* tp_descr_get */
    0,                                                            /* tp_descr_set */
    0,                                                            /* tp_dictoffset */
    (initproc)Filter_init<FilterRGB24>,                           /* tp_init */
    0,                                                            /* tp_alloc */
    Filter_allocNew,                                              /* tp_new */
};

// FilterRGBA32

// define python type
PyTypeObject FilterRGBA32Type = {
    PyVarObject_HEAD_INIT(nullptr, 0) "VideoTexture.FilterRGBA32", /*tp_name*/
    sizeof(PyFilter),                                              /*tp_basicsize*/
    0,                                                             /*tp_itemsize*/
    (destructor)Filter_dealloc,                                    /*tp_dealloc*/
    0,                                                             /*tp_print*/
    0,                                                             /*tp_getattr*/
    0,                                                             /*tp_setattr*/
    0,                                                             /*tp_compare*/
    0,                                                             /*tp_repr*/
    0,                                                             /*tp_as_number*/
    0,                                                             /*tp_as_sequence*/
    0,                                                             /*tp_as_mapping*/
    0,                                                             /*tp_hash */
    0,                                                             /*tp_call*/
    0,                                                             /*tp_str*/
    0,                                                             /*tp_getattro*/
    0,                                                             /*tp_setattro*/
    0,                                                             /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT,                                            /*tp_flags*/
    "Source filter RGBA32 objects",                                /* tp_doc */
    0,                                                             /* tp_traverse */
    0,                                                             /* tp_clear */
    0,                                                             /* tp_richcompare */
    0,                                                             /* tp_weaklistoffset */
    0,                                                             /* tp_iter */
    0,                                                             /* tp_iternext */
    nullptr,                                                       /* tp_methods */
    0,                                                             /* tp_members */
    nullptr,                                                       /* tp_getset */
    0,                                                             /* tp_base */
    0,                                                             /* tp_dict */
    0,                                                             /* tp_descr_get */
    0,                                                             /* tp_descr_set */
    0,                                                             /* tp_dictoffset */
    (initproc)Filter_init<FilterRGBA32>,                           /* tp_init */
    0,                                                             /* tp_alloc */
    Filter_allocNew,                                               /* tp_new */
};

// FilterBGR24

// define python type
PyTypeObject FilterBGR24Type = {
    PyVarObject_HEAD_INIT(nullptr, 0) "VideoTexture.FilterBGR24", /*tp_name*/
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
    "Source filter BGR24 objects",                                /* tp_doc */
    0,                                                            /* tp_traverse */
    0,                                                            /* tp_clear */
    0,                                                            /* tp_richcompare */
    0,                                                            /* tp_weaklistoffset */
    0,                                                            /* tp_iter */
    0,                                                            /* tp_iternext */
    nullptr,                                                      /* tp_methods */
    0,                                                            /* tp_members */
    nullptr,                                                      /* tp_getset */
    0,                                                            /* tp_base */
    0,                                                            /* tp_dict */
    0,                                                            /* tp_descr_get */
    0,                                                            /* tp_descr_set */
    0,                                                            /* tp_dictoffset */
    (initproc)Filter_init<FilterBGR24>,                           /* tp_init */
    0,                                                            /* tp_alloc */
    Filter_allocNew,                                              /* tp_new */
};
