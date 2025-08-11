/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bpygpu
 */

#pragma once

#include <Python.h>

#ifndef __cplusplus
#  include "../generic/py_capi_utils.hh"
#endif

namespace blender::gpu {
class Shader;
}  // namespace blender::gpu

struct GPUShaderCreateInfo;
struct GPUStageInterfaceInfo;

/* Make sure that there is always a reference count for PyObjects of type String as the strings are
 * passed by reference in the #GPUStageInterfaceInfo and #GPUShaderCreateInfo APIs. */
#define USE_GPU_PY_REFERENCES

/* `gpu_py_shader.cc` */

extern PyTypeObject BPyGPUShader_Type;

#define BPyGPUShader_Check(v) (Py_TYPE(v) == &BPyGPUShader_Type)

struct BPyGPUShader {
  PyObject_VAR_HEAD
  blender::gpu::Shader *shader;
  bool is_builtin;
};

[[nodiscard]] PyObject *BPyGPUShader_CreatePyObject(blender::gpu::Shader *shader, bool is_builtin);
[[nodiscard]] PyObject *bpygpu_shader_init();

/* gpu_py_shader_create_info.cc */

extern const struct PyC_StringEnumItems pygpu_attrtype_items[];
extern PyTypeObject BPyGPUShaderCreateInfo_Type;
extern PyTypeObject BPyGPUStageInterfaceInfo_Type;

#define BPyGPUShaderCreateInfo_Check(v) (Py_TYPE(v) == &BPyGPUShaderCreateInfo_Type)
#define BPyGPUStageInterfaceInfo_Check(v) (Py_TYPE(v) == &BPyGPUStageInterfaceInfo_Type)

struct BPyGPUStageInterfaceInfo {
  PyObject_VAR_HEAD
  GPUStageInterfaceInfo *interface;
#ifdef USE_GPU_PY_REFERENCES
  /* Just to keep a user to prevent freeing buffers we're using. */
  PyObject *references;
#endif
};

struct BPyGPUShaderCreateInfo {
  PyObject_VAR_HEAD
  GPUShaderCreateInfo *info;
#ifdef USE_GPU_PY_REFERENCES
  /* Just to keep a user to prevent freeing buffers we're using. */
  PyObject *vertex_source;
  PyObject *fragment_source;
  PyObject *compute_source;
  PyObject *typedef_source;
  PyObject *references;
#endif
  size_t constants_total_size;
};

[[nodiscard]] PyObject *BPyGPUStageInterfaceInfo_CreatePyObject(GPUStageInterfaceInfo *interface);
[[nodiscard]] PyObject *BPyGPUShaderCreateInfo_CreatePyObject(GPUShaderCreateInfo *info);
[[nodiscard]] bool bpygpu_shader_is_polyline(blender::gpu::Shader *shader);
