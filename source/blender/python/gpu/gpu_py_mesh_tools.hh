/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/** \file
 * \ingroup bpygpu
 *
 * Header for the Python wrapper that runs the "scatter positions -> corners + normals"
 * compute shader from Python.
 *
 * The implementation expects a Blender Mesh with an available MeshBatchCache and
 * a user-provided SSBO containing per-vertex vec4 positions.
 */

#pragma once

#include "BLI_compiler_attrs.h"

namespace blender {


/* Forward declarations of Blender types used by the implementation. */
struct Mesh;
typedef struct _object PyObject;

/* -------------------------------------------------------------------- */
/** \name Python API
 *
 * Exposed to the `gpu` Python module.
 * \{ */

/**
 * Python wrapper to dispatch the compute shader that:
 *  - scatters per-vertex positions (from a user SSBO) into per-corner position VBO,
 *  - recomputes packed corner normals.
 *
 * Python signature:
 *   scatter_positions_to_corners(obj, ssbo_positions, transform=None)
 *
 * Requirements and notes:
 *  - `obj` must be convertible to a Blender `Object *` owning mesh data with a ready batch cache.
 *  - `ssbo_positions` must be a `gpu.types.GPUStorageBuf` containing `vec4` per vertex
 *    (size == verts_num * sizeof(vec4)).
 *  - `transform` optional 4x4 matrix for transforming positions during scatter.
 *  - A valid GPU context must be active when calling this function.
 *  - The function binds destination VBOs as SSBOs and dispatches the compute shader,
 *    then performs the required memory barriers.
 */
PyObject *pygpu_mesh_scatter(PyObject *self, PyObject *args, PyObject *kwds);

/**
 * Free GPU resources (shader + SSBO) associated with the mesh owned by obj.
 * Also resets mesh GPU deform flags.
 *
 * Python signature:
 *   free_compute_resources(obj)
 */
PyObject *pygpu_mesh_compute_free(PyObject *self, PyObject *args, PyObject *kwds);

/**
 * Initialize the `gpu.mesh` submodule and add the scatter function.
 * Should be called during the gpu Python module initialization.
 *
 * Returns a borrowed reference to the module object on success, or nullptr on failure.
 */
PyObject *bpygpu_mesh_init() ATTR_WARN_UNUSED_RESULT;

/* Free all scatter resources at python exit if not already done */
void bpygpu_mesh_tools_free_all();

/** \} */

}  // namespace blender
