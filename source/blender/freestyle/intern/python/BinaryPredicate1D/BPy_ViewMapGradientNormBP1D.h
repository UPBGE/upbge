/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup freestyle
 */

#pragma once

#include "../BPy_BinaryPredicate1D.h"

///////////////////////////////////////////////////////////////////////////////////////////

extern PyTypeObject ViewMapGradientNormBP1D_Type;

#define BPy_ViewMapGradientNormBP1D_Check(v) \
  (PyObject_IsInstance((PyObject *)v, (PyObject *)&ViewMapGradientNormBP1D_Type))

/*---------------------------Python BPy_ViewMapGradientNormBP1D structure definition----------*/
typedef struct {
  BPy_BinaryPredicate1D py_bp1D;
} BPy_ViewMapGradientNormBP1D;

///////////////////////////////////////////////////////////////////////////////////////////
