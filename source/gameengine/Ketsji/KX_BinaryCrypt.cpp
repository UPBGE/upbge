/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file gameengine/Ketsji/KX_BinaryCrypt.cpp
 *  \ingroup ketsji
 *
 * Encrypted Python modules: a finder in front of Python's path finder lets Python search as usual,
 * then looks at the first bytes of the `.py` it found. Only a container is taken over: it is
 * decrypted into one buffer, compiled from memory and the buffer wiped. Its loader neither reads
 * nor writes bytecode, and has no get_source(), so neither tracebacks nor inspect can show its
 * source. Every other module is imported by Python's machinery, untouched: the only cost is one
 * open() per newly imported `.py`, and none at all for games without a key.
 *
 * Data files: bge.logic.decryptFile reads a whole file into a bytes object, decrypting a container
 * straight into it (no intermediate copy but the last partial chunk).
 */

#ifdef WITH_PYTHON

#  include "KX_BinaryCrypt.h"

#  include <cstring>
#  include <fcntl.h>

#  include "BC_binarycrypt.hh"
#  include "BLI_filereader.hh"
#  include "BLI_fileops.hh"
#  include "MEM_guardedalloc.h"

using namespace blender;

/** Bytes BC_file_magic_is_container() needs. */
static constexpr int64_t BC_SNIFF_SIZE = 7;

namespace {

enum class OpenResult { Plain, Container, Error };

/**
 * Open \a path, a container of \a kind when it is one (\a r_reader then yields the plaintext), the
 * file itself otherwise. Errors set a Python exception of \a exc_type (refusals of the container
 * readers included).
 */
OpenResult bc_open(const char *path, const BCKind kind, PyObject *exc_type, FileReader **r_reader)
{
  *r_reader = nullptr;
  const int fd = BLI_open(path, O_BINARY | O_RDONLY, 0);
  if (fd == -1) {
    PyErr_SetFromErrnoWithFilename(PyExc_OSError, path);
    return OpenResult::Error;
  }
  FileReader *raw = BLI_filereader_new_file(fd);
  char magic[BC_SNIFF_SIZE];
  const int64_t n = raw->read(raw, magic, BC_SNIFF_SIZE);
  if (n != BC_SNIFF_SIZE || !BC_file_magic_is_container(magic)) {
    if (n < 0 || raw->seek(raw, 0, SEEK_SET) != 0) {
      raw->close(raw);
      PyErr_SetFromErrnoWithFilename(PyExc_OSError, path);
      return OpenResult::Error;
    }
    *r_reader = raw;
    return OpenResult::Plain;
  }
  const char *error = nullptr;
  FileReader *reader = BC_filereader_open(raw, kind, &error); /* Owns raw from here on. */
  if (reader == nullptr) {
    PyErr_Format(exc_type, "cannot open the encrypted file '%s': %s", path, error);
    return OpenResult::Error;
  }
  *r_reader = reader;
  return OpenResult::Container;
}

/** Size of what \a reader yields, rewound to the start. -1 on failure. */
int64_t bc_size_and_rewind(FileReader *reader)
{
  const off64_t size = reader->seek(reader, 0, SEEK_END);
  if (size < 0 || reader->seek(reader, 0, SEEK_SET) != 0) {
    return -1;
  }
  return int64_t(size);
}

/** _is_container(path): the finder's test, on the file Python's own search found. */
PyObject *bc_py_is_container(PyObject * /*self*/, PyObject *arg)
{
  PyObject *path;
  if (!PyUnicode_FSConverter(arg, &path)) {
    return nullptr;
  }
  bool container = false;
  const int fd = BLI_open(PyBytes_AS_STRING(path), O_BINARY | O_RDONLY, 0);
  Py_DECREF(path);
  if (fd != -1) {
    FileReader *raw = BLI_filereader_new_file(fd);
    char magic[BC_SNIFF_SIZE];
    container = raw->read(raw, magic, BC_SNIFF_SIZE) == BC_SNIFF_SIZE &&
                BC_file_magic_is_container(magic);
    raw->close(raw);
  }
  /* An unreadable file stays Python's: it reports the error as it would without this finder. */
  return PyBool_FromLong(container);
}

/**
 * _exec_module(module, path): run the encrypted module at \a path in the namespace of \a module.
 * The plaintext exists in one buffer, wiped right after the compile.
 */
PyObject *bc_py_exec_module(PyObject * /*self*/, PyObject *args)
{
  PyObject *module, *path_str, *path;
  if (!PyArg_ParseTuple(args, "O!U:_exec_module", &PyModule_Type, &module, &path_str) ||
      !PyUnicode_FSConverter(path_str, &path))
  {
    return nullptr;
  }
  FileReader *reader;
  const OpenResult opened = bc_open(PyBytes_AS_STRING(path), BCKind::Python, PyExc_ImportError, &reader);
  if (opened != OpenResult::Container) {
    if (opened == OpenResult::Plain) {
      /* Replaced between find_spec and here. Never run it as if it were the sealed module. */
      reader->close(reader);
      PyErr_Format(PyExc_ImportError, "'%s' is no longer encrypted", PyBytes_AS_STRING(path));
    }
    Py_DECREF(path);
    return nullptr;
  }
  const int64_t size = bc_size_and_rewind(reader);
  char *source = size >= 0 ? static_cast<char *>(MEM_new_uninitialized(size_t(size) + 1, __func__)) :
                             nullptr;
  const bool read = source != nullptr && reader->read(reader, source, size_t(size)) == size;
  reader->close(reader);
  if (!read) {
    if (source != nullptr) {
      BC_wipe(source, size_t(size));
      MEM_delete_void(static_cast<void *>(source));
    }
    PyErr_Format(PyExc_ImportError,
                 "cannot decrypt '%s' (file corrupted or modified)",
                 PyBytes_AS_STRING(path));
    Py_DECREF(path);
    return nullptr;
  }
  Py_DECREF(path);
  source[size] = '\0';

  PyObject *code = nullptr;
  if (memchr(source, '\0', size_t(size)) != nullptr) {
    /* The compiler reads a C string: a NUL would silently cut the module short. */
    PyErr_SetString(PyExc_SyntaxError, "source code cannot contain null bytes");
  }
  else {
    /* optimize = -1: the interpreter's -O level, as for any other module. */
    code = Py_CompileStringObject(source, path_str, Py_file_input, nullptr, -1);
  }
  BC_wipe(source, size_t(size));
  MEM_delete_void(static_cast<void *>(source));
  if (code == nullptr) {
    return nullptr;
  }

  /* As exec() does for every other module: its globals see the builtins under __builtins__. */
  PyObject *dict = PyModule_GetDict(module);
  if (PyDict_GetItemString(dict, "__builtins__") == nullptr &&
      PyDict_SetItemString(dict, "__builtins__", PyEval_GetBuiltins()) != 0)
  {
    Py_DECREF(code);
    return nullptr;
  }
  PyObject *result = PyEval_EvalCode(code, dict, dict);
  Py_DECREF(code);
  if (result == nullptr) {
    return nullptr;
  }
  Py_DECREF(result);
  Py_RETURN_NONE;
}

PyMethodDef bc_is_container_def = {"_is_container", bc_py_is_container, METH_O, nullptr};
PyMethodDef bc_exec_module_def = {"_exec_module", bc_py_exec_module, METH_VARARGS, nullptr};

/* Runs once per game start in a fresh namespace holding the two functions above. Delegating the
 * search to PathFinder keeps Python's exact precedence (package before module, extension modules,
 * namespace packages, zip entries) and its cached directory listings; a module it found is returned
 * as it is, so PathFinder never searches twice. */
const char *bc_finder_source = R"PY(
from importlib.machinery import PathFinder


class BinaryCryptFinder:
    """Imports the Python modules UPBGE-BinaryCrypt encrypted."""

    _bge_binarycrypt = True

    @staticmethod
    def find_spec(fullname, path=None, target=None):
        spec = PathFinder.find_spec(fullname, path, target)
        if spec is not None and spec.has_location and spec.origin.endswith(".py") and _is_container(spec.origin):
            spec.loader = BinaryCryptFinder
        return spec

    @staticmethod
    def create_module(spec):
        return None

    @staticmethod
    def exec_module(module):
        _exec_module(module, module.__spec__.origin)


def _install(meta_path):
    for finder in meta_path:
        if getattr(finder, "_bge_binarycrypt", False):
            return
    try:
        index = meta_path.index(PathFinder)
    except ValueError:
        index = len(meta_path)
    meta_path.insert(index, BinaryCryptFinder)
)PY";

}  // namespace

PyObject *KX_BinaryCrypt_ReadFile(const char *filepath)
{
  FileReader *reader;
  const OpenResult opened = bc_open(filepath, BCKind::Data, PyExc_OSError, &reader);
  if (opened == OpenResult::Error) {
    return nullptr;
  }
  const int64_t size = bc_size_and_rewind(reader);
  if (size < 0) {
    reader->close(reader);
    return PyErr_SetFromErrnoWithFilename(PyExc_OSError, filepath);
  }
  /* Decrypted straight into the result: whole chunks land there without any copy. */
  PyObject *bytes = PyBytes_FromStringAndSize(nullptr, Py_ssize_t(size));
  if (bytes == nullptr) {
    reader->close(reader);
    return nullptr;
  }
  char *data = PyBytes_AS_STRING(bytes);
  int64_t read;
  /* Disk and cipher time (2.5 ms per MB on a 2.1 GHz Xeon) is not Python's: let its threads run. */
  Py_BEGIN_ALLOW_THREADS;
  read = reader->read(reader, data, size_t(size));
  reader->close(reader);
  Py_END_ALLOW_THREADS;
  if (read != size) {
    BC_wipe(data, size_t(size));
    Py_DECREF(bytes);
    if (opened == OpenResult::Container) {
      PyErr_Format(PyExc_OSError, "cannot decrypt '%s' (file corrupted or modified)", filepath);
      return nullptr;
    }
    return PyErr_SetFromErrnoWithFilename(PyExc_OSError, filepath);
  }
  return bytes;
}

void KX_BinaryCrypt_InstallImporter(PyObject *sys_meta_path)
{
  if (!BC_runtime_has_key() || sys_meta_path == nullptr) {
    return;
  }
  /* Built anew at every game start: Python may have been restarted in between, so no object of
   * an earlier interpreter is kept. */
  PyObject *globals = PyDict_New();
  PyObject *name = PyUnicode_FromString("_bge_binarycrypt");
  PyObject *is_container = PyCFunction_New(&bc_is_container_def, nullptr);
  PyObject *exec_module = PyCFunction_New(&bc_exec_module_def, nullptr);
  bool ok = globals != nullptr && name != nullptr && is_container != nullptr &&
            exec_module != nullptr &&
            PyDict_SetItemString(globals, "__builtins__", PyEval_GetBuiltins()) == 0 &&
            PyDict_SetItemString(globals, "__name__", name) == 0 &&
            PyDict_SetItemString(globals, "_is_container", is_container) == 0 &&
            PyDict_SetItemString(globals, "_exec_module", exec_module) == 0;
  Py_XDECREF(name);
  Py_XDECREF(is_container);
  Py_XDECREF(exec_module);
  if (ok) {
    PyObject *result = PyRun_String(bc_finder_source, Py_file_input, globals, globals);
    PyObject *install = result ? PyDict_GetItemString(globals, "_install") : nullptr;
    PyObject *installed = install ? PyObject_CallOneArg(install, sys_meta_path) : nullptr;
    ok = installed != nullptr;
    Py_XDECREF(result);
    Py_XDECREF(installed);
  }
  Py_XDECREF(globals);
  if (!ok) {
    fprintf(stderr, "BinaryCrypt: cannot install the importer of encrypted Python modules\n");
    PyErr_Print();
  }
}

#endif /* WITH_PYTHON */
