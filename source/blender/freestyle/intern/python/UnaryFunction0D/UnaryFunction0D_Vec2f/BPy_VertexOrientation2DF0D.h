/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup freestyle
 */

#pragma once

#include "../BPy_UnaryFunction0DVec2f.h"

#ifdef __cplusplus
extern "C" {
#endif

///////////////////////////////////////////////////////////////////////////////////////////

extern PyTypeObject VertexOrientation2DF0D_Type;

#define BPy_VertexOrientation2DF0D_Check(v) \
  (PyObject_IsInstance((PyObject *)v, (PyObject *)&VertexOrientation2DF0D_Type))

/*---------------------------Python BPy_VertexOrientation2DF0D structure definition----------*/
typedef struct {
  BPy_UnaryFunction0DVec2f py_uf0D_vec2f;
} BPy_VertexOrientation2DF0D;

///////////////////////////////////////////////////////////////////////////////////////////

#ifdef __cplusplus
}
#endif
