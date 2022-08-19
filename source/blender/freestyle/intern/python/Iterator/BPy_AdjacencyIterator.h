/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup freestyle
 */

#pragma once

#include "../BPy_Iterator.h"

#include "../../stroke/ChainingIterators.h"

#ifdef __cplusplus
extern "C" {
#endif

///////////////////////////////////////////////////////////////////////////////////////////

extern PyTypeObject AdjacencyIterator_Type;

#define BPy_AdjacencyIterator_Check(v) \
  (PyObject_IsInstance((PyObject *)v, (PyObject *)&AdjacencyIterator_Type))

/*---------------------------Python BPy_AdjacencyIterator structure definition----------*/
typedef struct {
  BPy_Iterator py_it;
  Freestyle::AdjacencyIterator *a_it;
  bool at_start;
} BPy_AdjacencyIterator;

///////////////////////////////////////////////////////////////////////////////////////////

#ifdef __cplusplus
}
#endif
