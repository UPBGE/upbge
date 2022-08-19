/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup pythonintern
 */

#pragma once

struct wmOperatorType;

#ifdef __cplusplus
extern "C" {
#endif

/* these are used for operator methods, used by bpy_operator.c */
PyObject *PYOP_wrap_macro_define(PyObject *self, PyObject *args);

/* exposed to rna/wm api */
/**
 * Generic function used by all Python defined operators
 * it's passed as an argument to #WM_operatortype_append_ptr in for operator registration.
 */
void BPY_RNA_operator_wrapper(struct wmOperatorType *ot, void *userdata);
/**
 * Generic function used by all Python defined macro-operators
 * it's passed as an argument to #WM_operatortype_append_ptr in for operator registration.
 */
void BPY_RNA_operator_macro_wrapper(struct wmOperatorType *ot, void *userdata);

#ifdef __cplusplus
}
#endif
