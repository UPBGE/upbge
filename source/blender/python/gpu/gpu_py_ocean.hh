/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bpygpu
 *
 * Python bindings helpers for ocean export (declarations).
 */

#pragma once

#include <Python.h>

namespace blender {


/* Create and return the 'gpu.ocean' submodule (registered into sys.modules).
 * This follows the pattern used by other gpu submodules (bpygpu_*_init).
 */
PyObject *bpygpu_ocean_init(void);

}  // namespace blender
