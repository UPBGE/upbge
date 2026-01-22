/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Minimal wrapper to run the "scatter positions -> corners + normals" compute shader
 * from Python.
 */

#include "gpu_py_mesh_tools.hh"

#include "../generic/python_compat.hh" /* IWYU pragma: keep. */
#include "../intern/bpy_rna.hh"        /* pyrna_id_FromPyObject */

#include "BKE_idtype.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_gpu.hh"

#include "../depsgraph/DEG_depsgraph_query.hh"

#include "../draw/intern/draw_cache_extract.hh"

#include "GPU_context.hh"

#include "../windowmanager/WM_api.hh"

#include "gpu_py_element.hh"
#include "gpu_py_storagebuffer.hh"
#include "gpu_py_uniformbuffer.hh"
#include "gpu_py_vertex_buffer.hh"

using namespace blender;
using namespace blender::bke;
using namespace blender::gpu::shader;
using namespace blender::draw;

PyDoc_STRVAR(
    pygpu_mesh_scatter_doc,
    ".. function:: scatter_positions_to_corners(obj, ssbo_positions, transform=None)\n"
    "\n"
    "   Scatter per-vertex positions (from user SSBO) to per-corner VBOs and recompute\n"
    "   packed normals using the internal compute shader. The mesh VBOs (positions and\n"
    "   normals) will be updated and ready for rendering.\n\n"
    "   NOTE: this function is non-blocking and may request the draw/cache system to\n"
    "   rebuild mesh VBOs asynchronously. If the evaluated mesh currently uses a\n"
    "   3-component vertex format but the draw/cache needs a 4-component (float4)\n"
    "   format, the function will tag the object for a geometry rebuild and return\n"
    "   immediately. The actual VBO population and scatter will then occur on the\n"
    "   next frame.\n\n"
    "   Because the operation can be deferred, callers that require the scatter to be\n"
    "   completed synchronously should re-invoke this function on a later frame (for\n"
    "   example using `bpy.app.timers.register` or from a modal operator) until the\n"
    "   VBOs are populated. This C API does not block or force the draw/cache to\n"
    "   populate VBOs synchronously.\n\n"
    "   Parameters\n"
    "   ----------\n"
    "   obj\n"
    "       Evaluated `bpy.types.Object` owning the mesh (use `obj.evaluated_get(depsgraph)`).\n"
    "   ssbo_positions\n"
    "       `gpu.types.GPUStorageBuf` containing `vec4` per vertex (object-space positions).\n"
    "   transform (optional)\n"
    "       `gpu.types.GPUStorageBuf` containing a `mat4` (used as `transform_mat[0]`). If "
    "omitted\n"
    "       an identity mat4 is used.\n\n"
    "   Accepted buffer types (bindings passed to the high-level API):\n"
    "     - `gpu.types.GPUStorageBuf` (SSBO)\n"
    "     - `gpu.types.GPUVertBuf` (VBO wrapper)\n"
    "     - `gpu.types.GPUUniformBuf` (bound as SSBO via `GPU_uniformbuf_bind_as_ssbo`)\n"
    "     - `gpu.types.GPUIndexBuf` (bound as SSBO via `GPU_indexbuf_bind_as_ssbo`)\n"
    "     - string tokens resolving mesh VBOs (e.g. `'Position'`, `'VBO::Position'`, "
    "`'CornerNormal'`)\n"
    "     - `None`\n\n"
    "   GLSL helpers injected automatically (topology buffer bound as `int topo[]` at binding "
    "15):\n"
    "     int face_offsets(int i);\n"
    "     int corner_to_face(int i);\n"
    "     int corner_verts(int i);\n"
    "     int corner_tri(int tri_idx, int vert_idx);\n"
    "     int corner_tri_face(int i);\n"
    "     int2 edges(int i);\n"
    "     int corner_edges(int i);\n"
    "     int vert_to_face_offsets(int i);\n"
    "     int vert_to_face(int i);\n\n"
    "   Specialization constants added automatically\n"
    "     - `int normals_domain` : 0 = vertex normals, 1 = face normals (derived from mesh)\n"
    "     - `int normals_hq`     : 0/1 high-quality normals flag (from scene perf_flag / "
    "workarounds)\n\n"
    "   Binding indices used by the builtin scatter shader (for reference):\n"
    "     - binding=0 : `positions_out[]` (write, VBO::Position)\n"
    "     - binding=1 : `normals_out[]` (write, VBO::CornerNormal)\n"
    "     - binding=2 : `positions_in[]` (read, vec4 SSBO - provided by caller)\n"
    "     - binding=3 : `transform_mat[]` (read, mat4 SSBO)\n"
    "     - binding=15: `topo[]` (read, int SSBO injected automatically)\n\n"
    "   Returns\n"
    "   -------\n"
    "   None or raises RuntimeError on failure.\n");
PyObject *pygpu_mesh_scatter(PyObject * /*self*/, PyObject *args, PyObject *kwds)
{
  PyObject *py_obj = nullptr;
  BPyGPUStorageBuf *py_ssbo_skinned_pos = nullptr;
  BPyGPUStorageBuf *py_ssbo_transform = nullptr;

  static const char *keywords[] = {"obj", "ssbo", "transform", nullptr};
  if (!PyArg_ParseTupleAndKeywords(args,
                                   kwds,
                                   "OO|O:scatter_positions_to_corners",
                                   (char **)keywords,
                                   &py_obj,
                                   &py_ssbo_skinned_pos,
                                   &py_ssbo_transform))
  {
    return nullptr;
  }

  /* --- 1. Validate Inputs --- */
  if (!GPU_context_active_get()) {
    PyErr_SetString(PyExc_RuntimeError, "No active GPU context");
    return nullptr;
  }
  if (!py_ssbo_skinned_pos || !py_ssbo_skinned_pos->ssbo) {
    PyErr_SetString(PyExc_TypeError, "Expected a GPUStorageBuf as second argument (positions SSBO)");
    return nullptr;
  }

  ID *id_obj = nullptr;
  if (!pyrna_id_FromPyObject(py_obj, &id_obj) || GS(id_obj->name) != ID_OB) {
    PyErr_Format(PyExc_TypeError, "Expected an Object, not %.200s", Py_TYPE(py_obj)->tp_name);
    return nullptr;
  }

  Object *ob_eval = id_cast<Object *>(id_obj);
  if (!DEG_is_evaluated(ob_eval) || ob_eval->type != OB_MESH) {
    PyErr_SetString(PyExc_TypeError, "Expected an evaluated mesh object");
    return nullptr;
  }

  Depsgraph *depsgraph = DEG_get_depsgraph_by_id(ob_eval->id);
  if (!depsgraph) {
    PyErr_SetString(PyExc_RuntimeError, "Object is not owned by a depsgraph");
    return nullptr;
  }

  blender::Mesh *mesh_eval = id_cast<blender::Mesh *>(ob_eval->data);
  if (!mesh_eval || !mesh_eval->runtime || !mesh_eval->runtime->batch_cache) {
    /* Not an error, just not ready. Request a redraw and tell Python to try again later. */
    Object *ob_orig = DEG_get_original(ob_eval);
    if (ob_orig) {
      DEG_id_tag_update(&ob_orig->id, ID_RECALC_GEOMETRY);
      WM_main_add_notifier(NC_WINDOW, nullptr);
    }
    Py_RETURN_NONE;
  }

  /* --- 2. Prepare GPU resources and bindings --- */
  auto *cache = static_cast<MeshBatchCache *>(mesh_eval->runtime->batch_cache);
  auto *vbo_pos = cache->final.buff.vbos.lookup_ptr(VBOType::Position)->get();
  auto *vbo_nor = cache->final.buff.vbos.lookup_ptr(VBOType::CornerNormal)->get();

  Object *ob_orig = DEG_get_original(ob_eval);
  blender::Mesh *mesh_orig = id_cast<blender::Mesh *>(ob_orig->data);
  mesh_orig->is_python_request_gpu = 1;
  mesh_eval->is_python_request_gpu = 1;

  /* Transform SSBO: optional. If not provided, create an identity SSBO and mark it
   * as owned by this function (we will free it unless compute is deferred). */
  blender::gpu::StorageBuf *transform_ssbo = nullptr;
  bool transform_owned = false;

  if (py_ssbo_transform == nullptr) {
    transform_ssbo = GPU_storagebuf_create(sizeof(float) * 16);
    float m[4][4];
    unit_m4(m);
    GPU_storagebuf_update(transform_ssbo, &m[0][0]);
    transform_owned = true;
  }
  else {
    if (!py_ssbo_transform->ssbo) {
      PyErr_SetString(PyExc_TypeError, "transform SSBO is invalid");
      return nullptr;
    }
  }

  using namespace blender::gpu::shader;

  blender::Vector<GpuMeshComputeBinding> bindings = {
      {0, vbo_pos, Qualifier::write, "vec4", "positions_out[]"},
      {1, vbo_nor, Qualifier::write, "uint", "normals_out[]"},
      {2, py_ssbo_skinned_pos->ssbo, Qualifier::read, "vec4", "positions_in[]"},
      {3,
       transform_owned ? transform_ssbo : py_ssbo_transform->ssbo,
       Qualifier::read,
       "mat4",
       "transform_mat[]"},
  };

  /* --- 3. Run Compute Shader via High-Level API --- */
  GpuComputeStatus status = BKE_mesh_gpu_scatter_to_corners(depsgraph,
                                                     ob_eval,
                                                     bindings,
                                                     std::function<void(blender::gpu::shader::ShaderCreateInfo &)>(),
                                                     std::function<void(blender::gpu::Shader *)>(),
                                                     mesh_eval->corner_verts().size());

  if (status == GpuComputeStatus::Error) {
    PyErr_SetString(PyExc_RuntimeError, "Failed to run mesh compute shader");
    return nullptr;
  }

  /* Ready: free locally-created SSBO. */
  if (transform_owned && transform_ssbo) {
    GPU_storagebuf_free(transform_ssbo);
    transform_ssbo = nullptr;
  }
  Py_RETURN_NONE;
}

PyDoc_STRVAR(pygpu_mesh_compute_free_doc,
             ".. function:: free_compute_resources(obj)\n"
             "\n"
             "   Free GPU compute resources (shaders, internal buffers) associated with the mesh\n"
             "   owned by `obj`. This should be called to clean up after using\n"
             "   `gpu.mesh.scatter_positions_to_corners` or `gpu.mesh.run_compute_mesh`.\n"
             "\n"
             "   This also resets internal flags like `mesh.is_using_gpu_deform`.\n"
             "   `obj` may be an evaluated object or an original object (bpy.types.Object).\n");
PyObject *pygpu_mesh_compute_free(PyObject * /*self*/, PyObject *args, PyObject *kwds)
{
  PyObject *py_obj = nullptr;
  static const char *_keywords[] = {"obj", nullptr};
  if (!PyArg_ParseTupleAndKeywords(
          args, kwds, "O:free_compute_resources", (char **)_keywords, &py_obj))
  {
    return nullptr;
  }

  ID *id_obj = nullptr;
  if (!pyrna_id_FromPyObject(py_obj, &id_obj)) {
    PyErr_Format(PyExc_TypeError, "Expected an Object, not %.200s", Py_TYPE(py_obj)->tp_name);
    return nullptr;
  }

  if (GS(id_obj->name) != ID_OB) {
    PyErr_Format(PyExc_TypeError,
                 "Expected an Object, not %.200s",
                 BKE_idtype_idcode_to_name(GS(id_obj->name)));
    return nullptr;
  }

  Object *ob = id_cast<Object *>(id_obj);

  /* Accept evaluated or original object. If evaluated, find original. */
  Object *ob_orig = ob;
  if (DEG_is_evaluated(ob)) {
    /* DEG_get_original returns the original object for an evaluated one. */
    ob_orig = DEG_get_original(ob);
    if (ob_orig == nullptr) {
      PyErr_SetString(PyExc_RuntimeError, "Failed to find original object for evaluated object");
      return nullptr;
    }
  }

  if (ob_orig->type != OB_MESH) {
    PyErr_SetString(PyExc_TypeError, "Object does not own a mesh");
    return nullptr;
  }

  blender::Mesh *mesh_orig = id_cast<blender::Mesh *>(ob_orig->data);
  if (!mesh_orig) {
    PyErr_SetString(PyExc_RuntimeError, "Object mesh data not available");
    return nullptr;
  }

  /* Free GPU resources associated with this mesh (thread-safe internally). */
  BKE_mesh_gpu_free_for_mesh(mesh_orig);
  if (mesh_orig) {
    mesh_orig->is_python_request_gpu = 0;
  }
  Py_RETURN_NONE;
}

static blender::gpu::VertBuf *resolve_vbo_token(MeshBatchCache *cache, const std::string &token)
{
  if (!cache || token.empty()) {
    return nullptr;
  }
  std::string name = token;
  const std::string prefix = "VBO::";
  if (name.rfind(prefix, 0) == 0) {
    name = name.substr(prefix.size());
  }

  if (name == "Position") {
    auto *ptr = cache->final.buff.vbos.lookup_ptr(VBOType::Position);
    return ptr ? ptr->get() : nullptr;
  }
  if (name == "CornerNormal") {
    auto *ptr = cache->final.buff.vbos.lookup_ptr(VBOType::CornerNormal);
    return ptr ? ptr->get() : nullptr;
  }
  if (name == "Tangents") {
    auto *ptr = cache->final.buff.vbos.lookup_ptr(VBOType::Tangents);
    return ptr ? ptr->get() : nullptr;
  }
  return nullptr;
}

PyDoc_STRVAR(
    pygpu_mesh_run_compute_doc,
    "Run a custom compute shader on a mesh.\\n\\n"
    "Signature: run_compute_mesh(obj, shader: str, bindings: Sequence[tuple], "
    "config: callable|None = None, dispatch_count: int = 0)\\n\\n"
    "Bindings: sequence of 5-tuples (binding_index:int, "
    "buffer:GPUStorageBuf|GPUVertBuf|GPUUniformBuf|GPUIndexBuf|str|None, "
    "qualifier:str('read'|'write'|'read_write'), type_name:str, bind_name:str).\\n\\n"
    " - If `buffer` is a string token it is resolved against the mesh batch cache VBOs.\\n"
    "   Supported tokens (examples): 'Position', 'VBO::Position', 'CornerNormal', "
    "'VBO::CornerNormal'.\\n"
    " - Accepted Python buffer wrappers: `gpu.types.GPUStorageBuf`, `gpu.types.GPUVertBuf`,\\n"
    "   `gpu.types.GPUUniformBuf` (bound as SSBO), `gpu.types.GPUIndexBuf` (bound as SSBO), or "
    "`None`.\\n\\n"
    "Config callable: optional callable that returns a Python dict. Two usages are supported:\\n"
    "  * Top-level entries with scalar values (int, float, bool) are treated as specialization\\n"
    "    constants and declared as specialization_constant at shader creation time.\\n"
    "  * A special key 'push_constants' whose value is a dict of uniform names -> value(s).\\n"
    "    Values can be float/int/bool or a sequence of floats/ints for arrays; they are set as\\n"
    "    uniforms immediately before dispatch (via GPU_shader_uniform_*).\\n\\n"
    "Example config callable (Python):\\n"
    "def config():\\n"
    "    return {\\n"
    "        'GRID_W': 128,                # specialization constant (int)\\n"
    "        'GRID_H': 128,                # specialization constant (int)\\n"
    "        'HEIGHT_SCALE': 1.0,          # specialization constant (float)\\n"
    "        'push_constants': {           # uniforms set before dispatch\\n"
    "            'u_time': 1.234,\\n"
    "            'u_spiral_strength': 0.5,\\n"
    "            'u_enabled': True,\\n"
    "            'u_offsets': [0.0, 1.0, 2.0],\\n"
    "        }\\n"
    "    }\\n\\n"
    "Builtins injected automatically (topology accessors bound as `int topo[]` at binding 15):\\n"
    "  int face_offsets(int i);\\n"
    "  int corner_to_face(int i);\\n"
    "  int corner_verts(int i);\\n"
    "  int corner_tri(int tri_idx, int vert_idx);\\n"
    "  int corner_tri_face(int i);\\n"
    "  int2 edges(int i);\\n"
    "  int corner_edges(int i);\\n"
    "  int vert_to_face_offsets(int i);\\n"
    "  int vert_to_face(int i);\\n\\n"
    "Automatic specialization constants added by the runtime:\\n"
    "  - `int normals_domain` (mesh-derived): 0=vertex, 1=face.\\n"
    "  - `int normals_hq` : high-quality normals flag (0/1).\\n\\n"
    "dispatch_count: number of invocations (if 0, defaults to mesh vertex count).\\n\\n"
    "Returns an integer status: 0=Success, 1=NotReady (deferred), 2=Error. The `obj` argument "
    "must be an\\n"
    "evaluated mesh object with a ready batch cache.");
static PyObject *pygpu_mesh_run_compute(PyObject * /*self*/, PyObject *args, PyObject *kwds)
{
  PyObject *py_obj = nullptr;
  PyObject *py_shader = nullptr;          /* str */
  PyObject *py_bindings_seq = nullptr;    /* sequence of tuples */
  PyObject *py_config_callable = nullptr; /* callable returning dict or None */
  int dispatch_count = 0;

  static const char *keywords[] = {
      "obj", "shader", "bindings", "config", "dispatch_count", nullptr};
  if (!PyArg_ParseTupleAndKeywords(args,
                                   kwds,
                                   "OOO|Oi:run_compute_mesh",
                                   (char **)keywords,
                                   &py_obj,
                                   &py_shader,
                                   &py_bindings_seq,
                                   &py_config_callable,
                                   &dispatch_count))
  {
    return nullptr;
  }

  if (!GPU_context_active_get()) {
    PyErr_SetString(PyExc_RuntimeError, "No active GPU context");
    return nullptr;
  }

  /* Get object */
  ID *id_obj = nullptr;
  if (!pyrna_id_FromPyObject(py_obj, &id_obj) || GS(id_obj->name) != ID_OB) {
    PyErr_Format(PyExc_TypeError, "Expected an Object, not %.200s", Py_TYPE(py_obj)->tp_name);
    return nullptr;
  }
  Object *ob_eval = id_cast<Object *>(id_obj);
  if (!DEG_is_evaluated(ob_eval) || ob_eval->type != OB_MESH) {
    PyErr_SetString(PyExc_TypeError, "Expected an evaluated mesh object");
    return nullptr;
  }
  Depsgraph *depsgraph = DEG_get_depsgraph_by_id(ob_eval->id);
  if (!depsgraph) {
    PyErr_SetString(PyExc_RuntimeError, "Object is not owned by a depsgraph");
    return nullptr;
  }

  /* Shader source */
  if (!PyUnicode_Check(py_shader)) {
    PyErr_SetString(PyExc_TypeError, "shader must be a string containing GLSL compute code");
    return nullptr;
  }
  const char *shader_src = PyUnicode_AsUTF8(py_shader);
  if (!shader_src) {
    PyErr_SetString(PyExc_TypeError, "Failed to decode shader string");
    return nullptr;
  }

  /* Convert bindings sequence -> std::vector<GpuMeshComputeBinding>
   *
   * Expected Python binding tuple:
   *   (binding_index:int, buffer:GPUStorageBuf|GPUVertBuf|str_token|None,
   *    qualifier:str('read'|'write'|'read_write'), type_name:str, bind_name:str)
   *
   * If buffer is a string token (e.g. "VBO:Position" or "Position") it will be resolved
   * to the cache's VBO after the MeshBatchCache is obtained.
   */
  std::vector<GpuMeshComputeBinding> local_bindings;
  std::vector<std::string> vbo_tokens;  // aligned with local_bindings
  if (!PySequence_Check(py_bindings_seq)) {
    PyErr_SetString(PyExc_TypeError, "bindings must be a sequence of tuples");
    return nullptr;
  }
  Py_ssize_t n_bind = PySequence_Size(py_bindings_seq);
  local_bindings.reserve(n_bind);
  vbo_tokens.reserve(n_bind);

  for (Py_ssize_t i = 0; i < n_bind; ++i) {
    PyObject *py_item = PySequence_GetItem(py_bindings_seq, i); /* new ref */
    if (!py_item) {
      PyErr_SetString(PyExc_RuntimeError, "Failed to read binding item");
      return nullptr;
    }
    if (!PyTuple_Check(py_item) || PyTuple_Size(py_item) != 5) {
      PyErr_SetString(PyExc_TypeError,
                      "Each binding must be a 5-tuple (index, buffer, qualifier, type, name)");
      Py_DECREF(py_item);
      return nullptr;
    }

    PyObject *py_idx = PyTuple_GetItem(py_item, 0);  /* borrowed */
    PyObject *py_buf = PyTuple_GetItem(py_item, 1);  /* borrowed */
    PyObject *py_qual = PyTuple_GetItem(py_item, 2); /* borrowed */
    PyObject *py_typ = PyTuple_GetItem(py_item, 3);  /* borrowed */
    PyObject *py_name = PyTuple_GetItem(py_item, 4); /* borrowed */

    if (!PyLong_Check(py_idx) || !PyUnicode_Check(py_qual) || !PyUnicode_Check(py_typ) ||
        !PyUnicode_Check(py_name))
    {
      PyErr_SetString(PyExc_TypeError, "Binding tuple types: (int, buffer, str, str, str)");
      Py_DECREF(py_item);
      return nullptr;
    }

    int binding_index = int(PyLong_AsLong(py_idx));
    const char *qual_s = PyUnicode_AsUTF8(py_qual);
    const char *type_name = PyUnicode_AsUTF8(py_typ);
    const char *bind_name = PyUnicode_AsUTF8(py_name);

    /* Resolve qualifier */
    blender::gpu::shader::Qualifier qual = blender::gpu::shader::Qualifier::read;
    if (strcmp(qual_s, "read") == 0) {
      qual = blender::gpu::shader::Qualifier::read;
    }
    else if (strcmp(qual_s, "write") == 0) {
      qual = blender::gpu::shader::Qualifier::write;
    }
    else if (strcmp(qual_s, "read_write") == 0) {
      qual = blender::gpu::shader::Qualifier::read_write;
    }
    else {
      PyErr_SetString(PyExc_ValueError, "qualifier must be 'read', 'write' or 'read_write'");
      Py_DECREF(py_item);
      return nullptr;
    }

    /* Resolve buffer variant: StorageBuf, VertBuf, None, or string token */
    blender::gpu::StorageBuf *sb = nullptr;
    blender::gpu::VertBuf *vb = nullptr;
    blender::gpu::UniformBuf *ub = nullptr;
    blender::gpu::IndexBuf *ib = nullptr;
    std::string token;

    if (py_buf == Py_None) {
      sb = nullptr;
      vb = nullptr;
      ub = nullptr;
      ib = nullptr;
    }
    else if (PyUnicode_Check(py_buf)) {
      const char *s = PyUnicode_AsUTF8(py_buf);
      if (!s) {
        PyErr_SetString(PyExc_TypeError, "Invalid string for buffer token");
        Py_DECREF(py_item);
        return nullptr;
      }
      token = std::string(s);
      // defer resolution until we have the cache
    }
    else {
      BPyGPUStorageBuf *py_sb = reinterpret_cast<BPyGPUStorageBuf *>(py_buf);
      if (py_sb && py_sb->ssbo) {
        sb = py_sb->ssbo;
      }
      BPyGPUVertBuf *py_vb = reinterpret_cast<BPyGPUVertBuf *>(py_buf);
      if (py_vb && py_vb->buf) {
        vb = py_vb->buf;
      }
      BPyGPUUniformBuf *py_ubo = reinterpret_cast<BPyGPUUniformBuf *>(py_buf);
      if (py_ubo && py_ubo->ubo) {
        ub = py_ubo->ubo;
      }
      BPyGPUIndexBuf *py_ibo = reinterpret_cast<BPyGPUIndexBuf *>(py_buf);
      if (py_ibo && py_ibo->elem) {
        ib = py_ibo->elem;
      }
      if (!vb && !sb && !ub && !ib) {
        PyErr_SetString(PyExc_TypeError,
                        "buffer must be a GPUStorageBuf, GPUVertBuf, GPUUniformBuf, "
                        "GPUIndexBuf, a string token or None");
        Py_DECREF(py_item);
        return nullptr;
      }
    }

    GpuMeshComputeBinding b;
    b.binding = binding_index;
    if (sb) {
      b.buffer = sb;
    }
    else if (vb) {
      b.buffer = vb;
    }
    else if (ub) {
      b.buffer = ub;
    }
    else if (ib) {
      b.buffer = ib;
    }
    b.qualifiers = qual;
    b.type_name = strdup(type_name);
    b.bind_name = strdup(bind_name);
    local_bindings.push_back(std::move(b));
    vbo_tokens.push_back(std::move(token));

    Py_DECREF(py_item);
  }

  /* Prepare mesh and validate VBOs like original function */
  blender::Mesh *mesh_eval = id_cast<blender::Mesh *>(ob_eval->data);
  if (!mesh_eval || !mesh_eval->runtime || !mesh_eval->runtime->batch_cache) {
    Object *ob_orig = DEG_get_original(ob_eval);
    if (ob_orig) {
      DEG_id_tag_update(&ob_orig->id, ID_RECALC_GEOMETRY);
      WM_main_add_notifier(NC_WINDOW, nullptr);
    }
    /* free duplicated strings */
    for (auto &b : local_bindings) {
      if (b.type_name)
        free((void *)b.type_name);
      if (b.bind_name)
        free((void *)b.bind_name);
    }
    Py_RETURN_NONE;
  }

  Object *ob_orig = DEG_get_original(ob_eval);
  blender::Mesh *mesh_orig = id_cast<blender::Mesh *>(ob_orig->data);
  mesh_orig->is_python_request_gpu = 1;
  mesh_eval->is_python_request_gpu = 1;

  auto *cache = static_cast<MeshBatchCache *>(mesh_eval->runtime->batch_cache);

  /* Resolve any VBO tokens into actual VertBuf* using the mesh cache. */
  for (size_t i = 0; i < local_bindings.size(); ++i) {
    const std::string &tok = vbo_tokens[i];
    if (tok.empty()) {
      continue;
    }
    blender::gpu::VertBuf *resolved = resolve_vbo_token(cache, tok);
    if (!resolved) {
      /* Cleanup */
      for (auto &b : local_bindings) {
        if (b.type_name)
          free((void *)b.type_name);
        if (b.bind_name)
          free((void *)b.bind_name);
      }
      PyErr_Format(
          PyExc_RuntimeError, "Failed to resolve VBO token '%s' to a mesh VBO", tok.c_str());
      return nullptr;
    }
    local_bindings[i].buffer = resolved;
  }

  /* Prepare config: collect specialization constants and push-constants from py_config_callable.
   */

  std::vector<std::pair<std::string, long>> spec_ints;
  std::vector<std::pair<std::string, double>> spec_floats;
  std::vector<std::pair<std::string, bool>> spec_bools;

  struct PushConst {
    std::string name;
    enum Kind { FLOAT, INT, BOOL, FLOAT_ARRAY } kind;
    std::vector<float> fdata;
    std::vector<int> idata;
    bool bdata = false;
  };
  std::vector<PushConst> push_constants;

  if (py_config_callable && py_config_callable != Py_None && PyCallable_Check(py_config_callable))
  {
    PyObject *py_ret = PyObject_CallObject(py_config_callable, nullptr);
    if (!py_ret) {
      /* cleanup strings already allocated */
      for (auto &b : local_bindings) {
        if (b.type_name)
          free((void *)b.type_name);
        if (b.bind_name)
          free((void *)b.bind_name);
      }
      PyErr_SetString(PyExc_RuntimeError, "config callable raised an exception");
      return nullptr;
    }
    if (PyDict_Check(py_ret)) {
      PyObject *key, *value;
      Py_ssize_t pos = 0;
      while (PyDict_Next(py_ret, &pos, &key, &value)) {
        if (!PyUnicode_Check(key)) {
          continue;
        }
        const char *name = PyUnicode_AsUTF8(key);
        if (STREQ(name, "push_constants") && PyDict_Check(value)) {
          PyObject *k2, *v2;
          Py_ssize_t pos2 = 0;
          while (PyDict_Next(value, &pos2, &k2, &v2)) {
            if (!PyUnicode_Check(k2)) {
              continue;
            }
            const char *pname = PyUnicode_AsUTF8(k2);
            PushConst pc;
            pc.name = pname;
            if (PyFloat_Check(v2) || PyLong_Check(v2)) {
              pc.kind = PushConst::FLOAT;
              pc.fdata.push_back(PyFloat_AsDouble(v2));
            }
            else if (PyBool_Check(v2)) {
              pc.kind = PushConst::BOOL;
              pc.bdata = (v2 == Py_True);
            }
            else if (PySequence_Check(v2)) {
              PyObject *seq = PySequence_Fast(v2, "push_constants array");
              if (seq) {
                Py_ssize_t len = PySequence_Fast_GET_SIZE(seq);
                pc.kind = PushConst::FLOAT_ARRAY;
                pc.fdata.reserve(len);
                for (Py_ssize_t ii = 0; ii < len; ++ii) {
                  PyObject *it = PySequence_Fast_GET_ITEM(seq, ii);
                  pc.fdata.push_back(PyFloat_AsDouble(it));
                }
                Py_DECREF(seq);
              }
            }
            push_constants.push_back(std::move(pc));
          }
        }
        else {
          if (PyLong_Check(value)) {
            spec_ints.emplace_back(std::string(name), PyLong_AsLong(value));
          }
          else if (PyFloat_Check(value)) {
            spec_floats.emplace_back(std::string(name), PyFloat_AsDouble(value));
          }
          else if (PyBool_Check(value)) {
            spec_bools.emplace_back(std::string(name), value == Py_True);
          }
        }
      }
    }
    Py_DECREF(py_ret);
  }

  /* IMPORTANT:
   * We will move `push_constants` into the post_bind_fn (so the lambda owns the data).
   * To still declare the push-constants in the `ShaderCreateInfo` we must keep a copy
   * for the `config_with_specs` lambda. Moving `push_constants` before calling
   * `config_with_specs` would leave it empty and the declarations would not be emitted,
   * causing undefined identifiers at compile-time.
   */
  std::vector<PushConst> push_constants_for_info = push_constants;

  /* Lambda to apply specialization constants and declare push-constants at shader create time. */
  auto config_with_specs = [&](blender::gpu::shader::ShaderCreateInfo &info) {
    for (auto &p : spec_ints) {
      /* scalar specialization -> declare with the actual value provided by Python. */
      info.specialization_constant(
          blender::gpu::shader::Type::int_t, p.first.c_str(), int(p.second));
    }
    for (auto &p : spec_floats) {
      /* Use the float value collected from Python (stored as double). */
      info.specialization_constant(
          blender::gpu::shader::Type::float_t, p.first.c_str(), float(p.second));
    }
    for (auto &p : spec_bools) {
      /* Bool specialization constant must be passed as int (0/1). */
      info.specialization_constant(
          blender::gpu::shader::Type::bool_t, p.first.c_str(), p.second ? 1 : 0);
    }

    /* Declare push-constants provided by the Python config callable.
     * Use the preserved copy `push_constants_for_info` so declarations are present
     * even after we move the original vector into the post_bind_fn.
     */
    for (const PushConst &pc : push_constants_for_info) {
      switch (pc.kind) {
        case PushConst::FLOAT: {
          const int count = int(pc.fdata.size());
          /* scalar -> array_size 0, arrays -> provide size. */
          info.push_constant(
              blender::gpu::shader::Type::float_t, pc.name.c_str(), count > 1 ? count : 0);
          break;
        }
        case PushConst::INT: {
          const int count = int(pc.idata.size());
          info.push_constant(
              blender::gpu::shader::Type::int_t, pc.name.c_str(), count > 1 ? count : 0);
          break;
        }
        case PushConst::BOOL: {
          info.push_constant(blender::gpu::shader::Type::bool_t, pc.name.c_str(), 0);
          break;
        }
        case PushConst::FLOAT_ARRAY: {
          const int count = std::max<int>(1, int(pc.fdata.size()));
          info.push_constant(blender::gpu::shader::Type::float_t, pc.name.c_str(), count);
          break;
        }
      }
    }
  };

  /* Build post_bind_fn that sets push-constants at dispatch time using existing GPU uniform
   * setters. */
  std::function<void(blender::gpu::Shader *)> post_bind_fn = {};
  if (!push_constants.empty()) {
    post_bind_fn = [push_constants = std::move(push_constants)](blender::gpu::Shader *sh) {
      for (const PushConst &pc : push_constants) {
        const int loc = GPU_shader_get_uniform(sh, pc.name.c_str());
        if (loc == -1) {
          continue;
        }
        if (pc.kind == PushConst::FLOAT) {
          GPU_shader_uniform_float_ex(sh, loc, 1, 1, pc.fdata.data());
        }
        else if (pc.kind == PushConst::INT) {
          if (!pc.idata.empty()) {
            GPU_shader_uniform_int_ex(sh, loc, int(pc.idata.size()), 1, pc.idata.data());
          }
        }
        else if (pc.kind == PushConst::BOOL) {
          int v = pc.bdata ? 1 : 0;
          GPU_shader_uniform_int_ex(sh, loc, 1, 1, &v);
        }
        else if (pc.kind == PushConst::FLOAT_ARRAY) {
          GPU_shader_uniform_float_ex(sh, loc, int(pc.fdata.size()), 1, pc.fdata.data());
        }
      }
    };
  }

  const int dispatch = dispatch_count > 0 ? dispatch_count : int(mesh_eval->verts_num);
  blender::bke::GpuComputeStatus status = BKE_mesh_gpu_run_compute(
      DEG_get_depsgraph_by_id(ob_eval->id),
      ob_eval,
      shader_src,
      blender::Span<GpuMeshComputeBinding>(local_bindings.data(), local_bindings.size()),
      config_with_specs,
      post_bind_fn,
      dispatch);

  /* free duplicated strings in local_bindings.type_name/bind_name if any */
  for (auto &b : local_bindings) {
    if (b.type_name)
      free((void *)b.type_name);
    if (b.bind_name)
      free((void *)b.bind_name);
  }

  return PyLong_FromLong(static_cast<int>(status));
}

#ifdef __GNUC__
#  ifdef __clang__
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wcast-function-type"
#  else
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wcast-function-type"
#  endif
#endif

static PyMethodDef pygpu_mesh__tp_methods[] = {
    {"scatter_positions_to_corners",
     (PyCFunction)pygpu_mesh_scatter,
     METH_VARARGS | METH_KEYWORDS,
     pygpu_mesh_scatter_doc},
    {"free_compute_resources",
     (PyCFunction)pygpu_mesh_compute_free,
     METH_VARARGS | METH_KEYWORDS,
     pygpu_mesh_compute_free_doc},
    {"run_compute_mesh",
     (PyCFunction)pygpu_mesh_run_compute,
     METH_VARARGS | METH_KEYWORDS,
     pygpu_mesh_run_compute_doc},
    {nullptr, nullptr, 0, nullptr},
};

static PyModuleDef pygpu_mesh_module_def = {
    PyModuleDef_HEAD_INIT,
    "gpu.mesh",
    "Mesh related GPU helpers.",
    0,
    pygpu_mesh__tp_methods,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
};

#ifdef __GNUC__
#  ifdef __clang__
#    pragma clang diagnostic pop
#  else
#    pragma GCC diagnostic pop
#  endif
#endif

PyObject *bpygpu_mesh_init(void)
{
  PyObject *submodule = PyModule_Create(&pygpu_mesh_module_def);
  return submodule;
}

void bpygpu_mesh_tools_free_all()
{
  BKE_mesh_gpu_free_all_caches();
}
