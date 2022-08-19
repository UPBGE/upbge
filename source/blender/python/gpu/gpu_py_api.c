/* SPDX-License-Identifier: GPL-2.0-or-later */

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

#include "BLI_utildefines.h"

#include "gpu_py_capabilities.h"
#include "gpu_py_matrix.h"
#include "gpu_py_platform.h"
#include "gpu_py_select.h"
#include "gpu_py_state.h"
#include "gpu_py_types.h"

#include "gpu_py_api.h" /* Own include. */

/* -------------------------------------------------------------------- */
/** \name GPU Module
 * \{ */

PyDoc_STRVAR(pygpu_doc,
             "This module provides Python wrappers for the GPU implementation in Blender.\n"
             "Some higher level functions can be found in the `gpu_extras` module.");
static struct PyModuleDef pygpu_module_def = {
    PyModuleDef_HEAD_INIT,
    .m_name = "gpu",
    .m_doc = pygpu_doc,
};

PyObject *BPyInit_gpu(void)
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

  return mod;
}

/** \} */
