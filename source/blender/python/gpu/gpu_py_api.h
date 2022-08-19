/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bpygpu
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Each type object could have a method for free GPU resources.
 * However, it is currently of little use. */
// #define BPYGPU_USE_GPUOBJ_FREE_METHOD

PyObject *BPyInit_gpu(void);

#ifdef __cplusplus
}
#endif
