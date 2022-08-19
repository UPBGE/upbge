/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup freestyle
 */

#pragma once

#include "../BPy_UnaryFunction1D.h"

#include "../../geometry/Geom.h"

#ifdef __cplusplus
extern "C" {
#endif

///////////////////////////////////////////////////////////////////////////////////////////

extern PyTypeObject UnaryFunction1DVec2f_Type;

#define BPy_UnaryFunction1DVec2f_Check(v) \
  (PyObject_IsInstance((PyObject *)v, (PyObject *)&UnaryFunction1DVec2f_Type))

/*---------------------------Python BPy_UnaryFunction1DVec2f structure definition----------*/
typedef struct {
  BPy_UnaryFunction1D py_uf1D;
  Freestyle::UnaryFunction1D<Freestyle::Geometry::Vec2f> *uf1D_vec2f;
} BPy_UnaryFunction1DVec2f;

/*---------------------------Python BPy_UnaryFunction1DVec2f visible prototypes-----------*/
int UnaryFunction1DVec2f_Init(PyObject *module);

///////////////////////////////////////////////////////////////////////////////////////////

#ifdef __cplusplus
}
#endif
