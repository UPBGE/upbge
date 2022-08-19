/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup freestyle
 */

#pragma once

#include "../BPy_UnaryFunction1DDouble.h"

#ifdef __cplusplus
extern "C" {
#endif

///////////////////////////////////////////////////////////////////////////////////////////

extern PyTypeObject LocalAverageDepthF1D_Type;

#define BPy_LocalAverageDepthF1D_Check(v) \
  (PyObject_IsInstance((PyObject *)v, (PyObject *)&LocalAverageDepthF1D_Type))

/*---------------------------Python BPy_LocalAverageDepthF1D structure definition----------*/
typedef struct {
  BPy_UnaryFunction1DDouble py_uf1D_double;
} BPy_LocalAverageDepthF1D;

///////////////////////////////////////////////////////////////////////////////////////////

#ifdef __cplusplus
}
#endif
