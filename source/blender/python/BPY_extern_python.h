/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup python
 *
 * Functionality relating to Python setup & tear down.
 */

#pragma once

struct bContext;

#ifdef __cplusplus
extern "C" {
#endif

/* For 'FILE'. */
#include <stdio.h>

/* bpy_interface.c */

/** Call #BPY_context_set first. */
void BPY_python_start(struct bContext *C, int argc, const char **argv);
void BPY_python_end(void);
void BPY_python_reset(struct bContext *C);
void BPY_python_use_system_env(void);
void BPY_python_backtrace(FILE *fp);

#ifdef __cplusplus
} /* extern "C" */
#endif
