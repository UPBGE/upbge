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

extern PyTypeObject DensityF1D_Type;

#define BPy_DensityF1D_Check(v) (PyObject_IsInstance((PyObject *)v, (PyObject *)&DensityF1D_Type))

/*---------------------------Python BPy_DensityF1D structure definition----------*/
typedef struct {
  BPy_UnaryFunction1DDouble py_uf1D_double;
} BPy_DensityF1D;

///////////////////////////////////////////////////////////////////////////////////////////

#ifdef __cplusplus
}
#endif
