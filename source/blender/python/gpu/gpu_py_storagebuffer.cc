/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bpygpu
 *
 * Storage buffer Python binding (model from gpu_py_uniformbuffer.cc).
 *
 * - Use `bpygpu_` for local API.
 * - Use `BPyGPU` for public API.
 */

#pragma once

#include "BLI_string_utf8.h"

#include "GPU_context.hh"
#include "GPU_storage_buffer.hh"
#include "../gpu/intern/gpu_storage_buffer_private.hh" /* pour usage_size_get() */

#include "../generic/python_compat.hh" /* IWYU pragma: keep. */

#include "gpu_py.hh"
#include "gpu_py_storagebuffer.hh" /* own include */

namespace blender {

/* -------------------------------------------------------------------- */
/** \name blender::gpu::StorageBuf Common Utilities
 * \{ */

static int pygpu_storagebuffer_valid_check(BPyGPUStorageBuf *bpygpu_sb)
{
  if (UNLIKELY(bpygpu_sb->ssbo == nullptr)) {
    PyErr_SetString(PyExc_ReferenceError,
#ifdef BPYGPU_USE_GPUOBJ_FREE_METHOD
                    "GPU storage buffer was freed, no further access is valid");
#else
                    "GPU storage buffer: internal error");
#endif
    return -1;
  }
  return 0;
}

#define BPYGPU_STORAGEBUF_CHECK_OBJ(bpygpu) \
  { \
    if (UNLIKELY(pygpu_storagebuffer_valid_check(bpygpu) == -1)) { \
      return nullptr; \
    } \
  } \
  ((void)0)

/** \} */

/* -------------------------------------------------------------------- */
/** \name blender::gpu::StorageBuf Type
 * \{ */

/* Helper: pad size to 16 (vec4) */
static size_t pad_to_vec4(size_t len)
{
  return (len + 15u) & ~(size_t)15u;
}

static PyObject *pygpu_storagebuffer__tp_new(PyTypeObject * /*self*/,
                                             PyObject *args,
                                             PyObject *kwds)
{
  BPYGPU_IS_INIT_OR_ERROR_OBJ;

  blender::gpu::StorageBuf *ssbo = nullptr;
  PyObject *pybuffer_obj;
  char err_out[256] = "unknown error. See console";

  static const char *_keywords[] = {"data", nullptr};
  static _PyArg_Parser _parser = {
      PY_ARG_PARSER_HEAD_COMPAT()
      "O" /* `data` */
      ":GPUStorageBuf.__new__",
      _keywords,
      nullptr,
  };
  if (!_PyArg_ParseTupleAndKeywordsFast(args, kwds, &_parser, &pybuffer_obj)) {
    return nullptr;
  }

  if (!GPU_context_active_get()) {
    STRNCPY_UTF8(err_out, "No active GPU context found");
  }
  else {
    Py_buffer pybuffer;
    if (PyObject_GetBuffer(pybuffer_obj, &pybuffer, PyBUF_SIMPLE) == -1) {
      /* PyObject_GetBuffer raise a PyExc_BufferError */
      return nullptr;
    }

    /* In Blender SSBOs require an alignement on vec4 (16 bytes).
     * auto padding if needed. */
    size_t len = (size_t)pybuffer.len;
    size_t padded_len = pad_to_vec4(len);
    void *data_ptr = pybuffer.buf;
    void *tmp = nullptr;

    if (padded_len != len) {
      tmp = PyMem_Malloc(padded_len);
      if (!tmp) {
        PyBuffer_Release(&pybuffer);
        PyErr_NoMemory();
        return nullptr;
      }
      memcpy(tmp, pybuffer.buf, len);
      memset(static_cast<char *>(tmp) + len, 0, padded_len - len);
      data_ptr = tmp;
    }

    ssbo = GPU_storagebuf_create_ex(
        padded_len, data_ptr, GPU_USAGE_DYNAMIC, "python_storagebuffer");

    if (tmp) {
      PyMem_Free(tmp);
    }
    PyBuffer_Release(&pybuffer);
  }

  if (ssbo == nullptr) {
    PyErr_Format(PyExc_RuntimeError, "GPUStorageBuf.__new__(...) failed with '%s'", err_out);
    return nullptr;
  }

  return BPyGPUStorageBuf_CreatePyObject(ssbo);
}

PyDoc_STRVAR(pygpu_storagebuffer_update_doc,
             ".. method:: update(data)\n"
             "\n"
             "   Update the data of the storage buffer object.\n"
             "   Data length will be padded to vec4 (16 bytes) if needed.\n");
static PyObject *pygpu_storagebuffer_update(BPyGPUStorageBuf *self, PyObject *obj)
{
  BPYGPU_STORAGEBUF_CHECK_OBJ(self);

  Py_buffer pybuffer;
  if (PyObject_GetBuffer(obj, &pybuffer, PyBUF_SIMPLE) == -1) {
    /* PyObject_GetBuffer raise a PyExc_BufferError */
    return nullptr;
  }

  size_t len = (size_t)pybuffer.len;
  size_t padded_len = pad_to_vec4(len);
  void *data_ptr = pybuffer.buf;
  void *tmp = nullptr;

  if (padded_len != len) {
    tmp = PyMem_Malloc(padded_len);
    if (!tmp) {
      PyBuffer_Release(&pybuffer);
      PyErr_NoMemory();
      return nullptr;
    }
    memcpy(tmp, pybuffer.buf, len);
    memset(static_cast<char *>(tmp) + len, 0, padded_len - len);
    data_ptr = tmp;
  }

  GPU_storagebuf_update(self->ssbo, data_ptr);

  if (tmp) {
    PyMem_Free(tmp);
  }
  PyBuffer_Release(&pybuffer);
  Py_RETURN_NONE;
}

PyDoc_STRVAR(pygpu_storagebuffer_read_doc,
             ".. method:: read()\n"
             "\n"
             "   Read the full contents of the storage buffer and return a ``bytes`` object.\n"
             "   Slow! Only use for inspection / debugging.\n");
static PyObject *pygpu_storagebuffer_read(BPyGPUStorageBuf *self)
{
  BPYGPU_STORAGEBUF_CHECK_OBJ(self);

  if (!GPU_context_active_get()) {
    PyErr_SetString(PyExc_RuntimeError, "No active GPU context found");
    return nullptr;
  }

  /* Determine expected host-visible size to read. Prefer usage_size if set, else fall back to 0.
   */
  size_t size = 0;
  /* StorageBuf has usage_size_get() exposed in the internal header we included. */
  size = self->ssbo->usage_size_get();
  if (size == 0) {
    /* If usage size not set, try to read zero bytes to indicate empty result. */
    return PyBytes_FromStringAndSize("", 0);
  }

  PyObject *ret = PyBytes_FromStringAndSize(nullptr, (Py_ssize_t)size);
  if (!ret) {
    PyErr_NoMemory();
    return nullptr;
  }
  char *buf = PyBytes_AS_STRING(ret);
  if (buf == nullptr) {
    Py_DECREF(ret);
    PyErr_SetString(PyExc_RuntimeError, "Failed to allocate bytes buffer");
    return nullptr;
  }

  /* Ensure the GPU data is visible to the host and perform the read.
   * GPU_storagebuf_sync_to_host will enqueue a host-visible transfer if supported;
   * GPU_storagebuf_read will block until data is available (backend dependent). */
  GPU_storagebuf_sync_to_host(self->ssbo);
  GPU_storagebuf_read(self->ssbo, buf);

  return ret;
}

#ifdef BPYGPU_USE_GPUOBJ_FREE_METHOD
PyDoc_STRVAR(pygpu_storagebuffer_free_doc,
             ".. method:: free()\n"
             "\n"
             "   Free the storage buffer object.\n"
             "   The storage buffer object will no longer be accessible.\n");
static PyObject *pygpu_storagebuffer_free(BPyGPUStorageBuf *self)
{
  BPYGPU_STORAGEBUF_CHECK_OBJ(self);

  GPU_storagebuf_free(self->ssbo);
  self->ssbo = nullptr;
  Py_RETURN_NONE;
}
#endif

static void BPyGPUStorageBuf__tp_dealloc(BPyGPUStorageBuf *self)
{
  if (self->ssbo) {
    if (GPU_context_active_get()) {
      GPU_storagebuf_free(self->ssbo);
    }
    else {
      /* Contexte GPU déjà détruit : éviter d'appeler l'API GPU qui accéderait à des
       * ressources backend invalides. Log minimal pour debug. */
      printf("PyGPUStorageBuf freed after the GPU context has been destroyed.\n");
    }
    self->ssbo = nullptr;
  }
  Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyGetSetDef pygpu_storagebuffer__tp_getseters[] = {
    {nullptr, nullptr, nullptr, nullptr, nullptr} /* Sentinel */
};

static PyMethodDef pygpu_storagebuffer__tp_methods[] = {
    {"read", (PyCFunction)pygpu_storagebuffer_read, METH_NOARGS, pygpu_storagebuffer_read_doc},
    {"update", (PyCFunction)pygpu_storagebuffer_update, METH_O, pygpu_storagebuffer_update_doc},
#ifdef BPYGPU_USE_GPUOBJ_FREE_METHOD
    {"free", (PyCFunction)pygpu_storagebuffer_free, METH_NOARGS, pygpu_storagebuffer_free_doc},
#endif
    {nullptr, nullptr, 0, nullptr},
};

PyDoc_STRVAR(
    pygpu_storagebuffer__tp_doc,
    ".. class:: GPUStorageBuf(data)\n"
    "\n"
    "   This object gives access to GPU storage buffers (SSBO).\n"
    "\n"
    "   :arg data: Data to fill the buffer. Length will be padded to 16 bytes if required.\n"
    "   :type data: object exposing buffer interface\n");
PyTypeObject BPyGPUStorageBuf_Type = {
    /*ob_base*/ PyVarObject_HEAD_INIT(nullptr, 0)
    /*tp_name*/ "GPUStorageBuf",
    /*tp_basicsize*/ sizeof(BPyGPUStorageBuf),
    /*tp_itemsize*/ 0,
    /*tp_dealloc*/ (destructor)BPyGPUStorageBuf__tp_dealloc,
    /*tp_vectorcall_offset*/ 0,
    /*tp_getattr*/ nullptr,
    /*tp_setattr*/ nullptr,
    /*tp_as_async*/ nullptr,
    /*tp_repr*/ nullptr,
    /*tp_as_number*/ nullptr,
    /*tp_as_sequence*/ nullptr,
    /*tp_as_mapping*/ nullptr,
    /*tp_hash*/ nullptr,
    /*tp_call*/ nullptr,
    /*tp_str*/ nullptr,
    /*tp_getattro*/ nullptr,
    /*tp_setattro*/ nullptr,
    /*tp_as_buffer*/ nullptr,
    /*tp_flags*/ Py_TPFLAGS_DEFAULT,
    /*tp_doc*/ pygpu_storagebuffer__tp_doc,
    /*tp_traverse*/ nullptr,
    /*tp_clear*/ nullptr,
    /*tp_richcompare*/ nullptr,
    /*tp_weaklistoffset*/ 0,
    /*tp_iter*/ nullptr,
    /*tp_iternext*/ nullptr,
    /*tp_methods*/ pygpu_storagebuffer__tp_methods,
    /*tp_members*/ nullptr,
    /*tp_getset*/ pygpu_storagebuffer__tp_getseters,
    /*tp_base*/ nullptr,
    /*tp_dict*/ nullptr,
    /*tp_descr_get*/ nullptr,
    /*tp_descr_set*/ nullptr,
    /*tp_dictoffset*/ 0,
    /*tp_init*/ nullptr,
    /*tp_alloc*/ nullptr,
    /*tp_new*/ pygpu_storagebuffer__tp_new,
    /*tp_free*/ nullptr,
    /*tp_is_gc*/ nullptr,
    /*tp_bases*/ nullptr,
    /*tp_mro*/ nullptr,
    /*tp_cache*/ nullptr,
    /*tp_subclasses*/ nullptr,
    /*tp_weaklist*/ nullptr,
    /*tp_del*/ nullptr,
    /*tp_version_tag*/ 0,
    /*tp_finalize*/ nullptr,
    /*tp_vectorcall*/ nullptr,
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Public API
 * \{ */

PyObject *BPyGPUStorageBuf_CreatePyObject(blender::gpu::StorageBuf *ssbo)
{
  BPyGPUStorageBuf *self;

  self = PyObject_New(BPyGPUStorageBuf, &BPyGPUStorageBuf_Type);
  self->ssbo = ssbo;

  return (PyObject *)self;
}

/** \} */

#undef BPYGPU_STORAGEBUF_CHECK_OBJ

}  // namespace blender
