/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bpygpu
 */

#pragma once

#include <Python.h>

#include "BLI_compiler_attrs.h"

#include "GPU_storage_buffer.hh"

namespace blender {

extern PyTypeObject BPyGPUStorageBuf_Type;

#define BPyGPUStorageBuf_Check(v) (Py_TYPE(v) == &BPyGPUStorageBuf_Type)

struct BPyGPUStorageBuf {
  PyObject_HEAD
  gpu::StorageBuf *ssbo;
};

[[nodiscard]] PyObject *BPyGPUStorageBuf_CreatePyObject(gpu::StorageBuf *ssbo)
    ATTR_NONNULL(1);

}  // namespace blender
