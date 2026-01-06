/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bpygpu
 *
 * Experimental Python API, not considered public yet (called '_gpu'),
 * we may re-expose as public later.
 *
 * - Use `bpygpu_` for local API.
 * - Use `BPyGPU` for public API.
 */

#include <Python.h>

#include "GPU_context.hh"
#include "GPU_state.hh"

#include "gpu_py_capabilities.hh"
#include "gpu_py_compute.hh"
#include "gpu_py_matrix.hh"
#include "gpu_py_mesh_tools.hh"
#include "gpu_py_ocean.hh"
#include "gpu_py_platform.hh"
#include "gpu_py_select.hh"
#include "gpu_py_state.hh"
#include "gpu_py_types.hh"

#include "gpu_py_api.hh" /* Own include. */

namespace blender {


static void pygpu_module_free(void *m)
{
  (void)m;
  bpygpu_mesh_tools_free_all();
}

/* -------------------------------------------------------------------- */
/** \name GPU Module
 * \{ */

PyDoc_STRVAR(
    /* Wrap. */
    pygpu_doc,
    "This module provides Python wrappers for the GPU implementation in Blender.\n"
    "Some higher level functions can be found in the :mod:`gpu_extras` module.\n");
static PyModuleDef pygpu_module_def = {
    /*m_base*/ PyModuleDef_HEAD_INIT,
    /*m_name*/ "gpu",
    /*m_doc*/ pygpu_doc,
    /*m_size*/ 0,
    /*m_methods*/ nullptr,
    /*m_slots*/ nullptr,
    /*m_traverse*/ nullptr,
    /*m_clear*/ nullptr,
    /*m_free*/ (freefunc)pygpu_module_free,
};

PyObject *BPyInit_gpu()
{
  PyObject *sys_modules = PyImport_GetModuleDict();
  PyObject *submodule;
  PyObject *mod;

  mod = PyModule_Create(&pygpu_module_def);

  PyModule_AddObject(mod, "types", (submodule = bpygpu_types_init()));
  PyDict_SetItem(sys_modules, PyModule_GetNameObject(submodule), submodule);

  PyModule_AddObject(mod, "capabilities", (submodule = bpygpu_capabilities_init()));
  PyDict_SetItem(sys_modules, PyModule_GetNameObject(submodule), submodule);

  PyModule_AddObject(mod, "matrix", (submodule = bpygpu_matrix_init()));
  PyDict_SetItem(sys_modules, PyModule_GetNameObject(submodule), submodule);

  PyModule_AddObject(mod, "platform", (submodule = bpygpu_platform_init()));
  PyDict_SetItem(sys_modules, PyModule_GetNameObject(submodule), submodule);

  PyModule_AddObject(mod, "select", (submodule = bpygpu_select_init()));
  PyDict_SetItem(sys_modules, PyModule_GetNameObject(submodule), submodule);

  PyModule_AddObject(mod, "shader", (submodule = bpygpu_shader_init()));
  PyDict_SetItem(sys_modules, PyModule_GetNameObject(submodule), submodule);

  PyModule_AddObject(mod, "state", (submodule = bpygpu_state_init()));
  PyDict_SetItem(sys_modules, PyModule_GetNameObject(submodule), submodule);

  PyModule_AddObject(mod, "texture", (submodule = bpygpu_texture_init()));
  PyDict_SetItem(sys_modules, PyModule_GetNameObject(submodule), submodule);

  PyModule_AddObject(mod, "mesh", (submodule = bpygpu_mesh_init()));
  PyDict_SetItem(sys_modules, PyModule_GetNameObject(submodule), submodule);

  PyModule_AddObject(mod, "compute", (submodule = bpygpu_compute_init()));
  PyDict_SetItem(sys_modules, PyModule_GetNameObject(submodule), submodule);

  /* ocean helpers */
  PyModule_AddObject(mod, "ocean", (submodule = bpygpu_ocean_init()));
  PyDict_SetItem(sys_modules, PyModule_GetNameObject(submodule), submodule);

  /* Export GPU barrier flags as Python constants (global gpu module). */
  /* Add to root module */
  PyModule_AddIntConstant(mod, "GPU_BARRIER_FRAMEBUFFER", (int)GPU_BARRIER_FRAMEBUFFER);
  PyModule_AddIntConstant(
      mod, "GPU_BARRIER_SHADER_IMAGE_ACCESS", (int)GPU_BARRIER_SHADER_IMAGE_ACCESS);
  PyModule_AddIntConstant(mod, "GPU_BARRIER_TEXTURE_FETCH", (int)GPU_BARRIER_TEXTURE_FETCH);
  PyModule_AddIntConstant(mod, "GPU_BARRIER_TEXTURE_UPDATE", (int)GPU_BARRIER_TEXTURE_UPDATE);
  PyModule_AddIntConstant(mod, "GPU_BARRIER_COMMAND", (int)GPU_BARRIER_COMMAND);
  PyModule_AddIntConstant(mod, "GPU_BARRIER_SHADER_STORAGE", (int)GPU_BARRIER_SHADER_STORAGE);
  PyModule_AddIntConstant(
      mod, "GPU_BARRIER_VERTEX_ATTRIB_ARRAY", (int)GPU_BARRIER_VERTEX_ATTRIB_ARRAY);
  PyModule_AddIntConstant(mod, "GPU_BARRIER_ELEMENT_ARRAY", (int)GPU_BARRIER_ELEMENT_ARRAY);
  PyModule_AddIntConstant(mod, "GPU_BARRIER_UNIFORM", (int)GPU_BARRIER_UNIFORM);
  PyModule_AddIntConstant(mod, "GPU_BARRIER_BUFFER_UPDATE", (int)GPU_BARRIER_BUFFER_UPDATE);

  /* Composite default constant for convenience. */
  PyModule_AddIntConstant(mod,
                          "GPU_BARRIER_DEFAULT",
                          (int)(GPU_BARRIER_TEXTURE_FETCH | GPU_BARRIER_SHADER_IMAGE_ACCESS));

  return mod;
}

/** \} */

}  // namespace blender
