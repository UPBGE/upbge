/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bpygpu
 */

#pragma once

/* Make sure that there is always a reference count for PyObjects of type String as the strings are
 * passed by reference in the #GPUStageInterfaceInfo and #GPUShaderCreateInfo APIs. */
#define USE_GPU_PY_REFERENCES

/* gpu_py_shader.c */

extern PyTypeObject BPyGPUShader_Type;

#define BPyGPUShader_Check(v) (Py_TYPE(v) == &BPyGPUShader_Type)

typedef struct BPyGPUShader {
  PyObject_VAR_HEAD
  struct GPUShader *shader;
  bool is_builtin;
} BPyGPUShader;

PyObject *BPyGPUShader_CreatePyObject(struct GPUShader *shader, bool is_builtin);
PyObject *bpygpu_shader_init(void);

#ifdef __cplusplus
extern "C" {
#endif

/* gpu_py_shader_create_info.cc */

extern PyTypeObject BPyGPUShaderCreateInfo_Type;
extern PyTypeObject BPyGPUStageInterfaceInfo_Type;

#define BPyGPUShaderCreateInfo_Check(v) (Py_TYPE(v) == &BPyGPUShaderCreateInfo_Type)
#define BPyGPUStageInterfaceInfo_Check(v) (Py_TYPE(v) == &BPyGPUStageInterfaceInfo_Type)

typedef struct BPyGPUStageInterfaceInfo {
  PyObject_VAR_HEAD
  struct GPUStageInterfaceInfo *interface;
#ifdef USE_GPU_PY_REFERENCES
  /* Just to keep a user to prevent freeing buf's we're using. */
  PyObject *references;
#endif
} BPyGPUStageInterfaceInfo;

typedef struct BPyGPUShaderCreateInfo {
  PyObject_VAR_HEAD
  struct GPUShaderCreateInfo *info;
#ifdef USE_GPU_PY_REFERENCES
  /* Just to keep a user to prevent freeing buf's we're using. */
  PyObject *vertex_source;
  PyObject *fragment_source;
  PyObject *typedef_source;
  PyObject *references;
#endif
  size_t constants_total_size;
} BPyGPUShaderCreateInfo;

PyObject *BPyGPUStageInterfaceInfo_CreatePyObject(struct GPUStageInterfaceInfo *interface);
PyObject *BPyGPUShaderCreateInfo_CreatePyObject(struct GPUShaderCreateInfo *info);

#ifdef __cplusplus
}
#endif
