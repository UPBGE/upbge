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

extern PyTypeObject PolygonalizationShader_Type;

#define BPy_PolygonalizationShader_Check(v) \
  (PyObject_IsInstance((PyObject *)v, (PyObject *)&PolygonalizationShader_Type))

/*---------------------------Python BPy_PolygonalizationShader structure definition----------*/
typedef struct {
  BPy_StrokeShader py_ss;
} BPy_PolygonalizationShader;

///////////////////////////////////////////////////////////////////////////////////////////

#ifdef __cplusplus
}
#endif
