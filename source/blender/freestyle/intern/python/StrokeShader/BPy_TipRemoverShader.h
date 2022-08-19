/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup freestyle
 */

#pragma once

#include "../BPy_StrokeShader.h"

#ifdef __cplusplus
extern "C" {
#endif

///////////////////////////////////////////////////////////////////////////////////////////

extern PyTypeObject TipRemoverShader_Type;

#define BPy_TipRemoverShader_Check(v) \
  (PyObject_IsInstance((PyObject *)v, (PyObject *)&TipRemoverShader_Type))

/*---------------------------Python BPy_TipRemoverShader structure definition----------*/
typedef struct {
  BPy_StrokeShader py_ss;
} BPy_TipRemoverShader;

///////////////////////////////////////////////////////////////////////////////////////////

#ifdef __cplusplus
}
#endif
