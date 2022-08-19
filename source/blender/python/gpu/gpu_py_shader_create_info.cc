/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bpygpu
 *
 * - Use `bpygpu_` for local API.
 * - Use `BPyGPU` for public API.
 */

#include <Python.h>

#include "BLI_utildefines.h"

#include "GPU_shader.h"
#include "intern/gpu_shader_create_info.hh"

#include "../generic/py_capi_utils.h"

#include "gpu_py_shader.h" /* own include */

//#define USE_PYGPU_SHADER_INFO_IMAGE_METHOD

using blender::gpu::shader::DualBlend;
using blender::gpu::shader::Frequency;
using blender::gpu::shader::ImageType;
using blender::gpu::shader::ShaderCreateInfo;
using blender::gpu::shader::StageInterfaceInfo;
using blender::gpu::shader::Type;

#ifdef USE_PYGPU_SHADER_INFO_IMAGE_METHOD
using blender::gpu::shader::Qualifier;

#  define PYDOC_QUALIFIERS \
    "      - ``NO_RESTRICT``\n" \
    "      - ``READ``\n" \
    "      - ``WRITE``\n"
static const struct PyC_FlagSet pygpu_qualifiers[] = {
    {(int)Qualifier::NO_RESTRICT, "NO_RESTRICT"},
    {(int)Qualifier::READ, "READ"},
    {(int)Qualifier::WRITE, "WRITE"},
    {0, nullptr},
};
#endif

#define PYDOC_TYPE_LIST \
  "      - ``FLOAT``\n" \
  "      - ``VEC2``\n" \
  "      - ``VEC3``\n" \
  "      - ``VEC4``\n" \
  "      - ``MAT3``\n" \
  "      - ``MAT4``\n" \
  "      - ``UINT``\n" \
  "      - ``UVEC2``\n" \
  "      - ``UVEC3``\n" \
  "      - ``UVEC4``\n" \
  "      - ``INT``\n" \
  "      - ``IVEC2``\n" \
  "      - ``IVEC3``\n" \
  "      - ``IVEC4``\n" \
  "      - ``BOOL``\n"
static const struct PyC_StringEnumItems pygpu_attrtype_items[] = {
    {(int)Type::FLOAT, "FLOAT"},
    {(int)Type::VEC2, "VEC2"},
    {(int)Type::VEC3, "VEC3"},
    {(int)Type::VEC4, "VEC4"},
    {(int)Type::MAT3, "MAT3"},
    {(int)Type::MAT4, "MAT4"},
    {(int)Type::UINT, "UINT"},
    {(int)Type::UVEC2, "UVEC2"},
    {(int)Type::UVEC3, "UVEC3"},
    {(int)Type::UVEC4, "UVEC4"},
    {(int)Type::INT, "INT"},
    {(int)Type::IVEC2, "IVEC2"},
    {(int)Type::IVEC3, "IVEC3"},
    {(int)Type::IVEC4, "IVEC4"},
    {(int)Type::BOOL, "BOOL"},
    {0, nullptr},
};

#define PYDOC_IMAGE_TYPES \
  "      - ``FLOAT_BUFFER``\n" \
  "      - ``FLOAT_1D``\n" \
  "      - ``FLOAT_1D_ARRAY``\n" \
  "      - ``FLOAT_2D``\n" \
  "      - ``FLOAT_2D_ARRAY``\n" \
  "      - ``FLOAT_3D``\n" \
  "      - ``FLOAT_CUBE``\n" \
  "      - ``FLOAT_CUBE_ARRAY``\n" \
  "      - ``INT_BUFFER``\n" \
  "      - ``INT_1D``\n" \
  "      - ``INT_1D_ARRAY``\n" \
  "      - ``INT_2D``\n" \
  "      - ``INT_2D_ARRAY``\n" \
  "      - ``INT_3D``\n" \
  "      - ``INT_CUBE``\n" \
  "      - ``INT_CUBE_ARRAY``\n" \
  "      - ``UINT_BUFFER``\n" \
  "      - ``UINT_1D``\n" \
  "      - ``UINT_1D_ARRAY``\n" \
  "      - ``UINT_2D``\n" \
  "      - ``UINT_2D_ARRAY``\n" \
  "      - ``UINT_3D``\n" \
  "      - ``UINT_CUBE``\n" \
  "      - ``UINT_CUBE_ARRAY``\n" \
  "      - ``SHADOW_2D``\n" \
  "      - ``SHADOW_2D_ARRAY``\n" \
  "      - ``SHADOW_CUBE``\n" \
  "      - ``SHADOW_CUBE_ARRAY``\n" \
  "      - ``DEPTH_2D``\n" \
  "      - ``DEPTH_2D_ARRAY``\n" \
  "      - ``DEPTH_CUBE``\n" \
  "      - ``DEPTH_CUBE_ARRAY``\n"
static const struct PyC_StringEnumItems pygpu_imagetype_items[] = {
    {(int)ImageType::FLOAT_BUFFER, "FLOAT_BUFFER"},
    {(int)ImageType::FLOAT_1D, "FLOAT_1D"},
    {(int)ImageType::FLOAT_1D_ARRAY, "FLOAT_1D_ARRAY"},
    {(int)ImageType::FLOAT_2D, "FLOAT_2D"},
    {(int)ImageType::FLOAT_2D_ARRAY, "FLOAT"},
    {(int)ImageType::FLOAT_3D, "FLOAT_2D_ARRAY"},
    {(int)ImageType::FLOAT_CUBE, "FLOAT_CUBE"},
    {(int)ImageType::FLOAT_CUBE_ARRAY, "FLOAT_CUBE_ARRAY"},
    {(int)ImageType::INT_BUFFER, "INT_BUFFER"},
    {(int)ImageType::INT_1D, "INT_1D"},
    {(int)ImageType::INT_1D_ARRAY, "INT_1D_ARRAY"},
    {(int)ImageType::INT_2D, "INT_2D"},
    {(int)ImageType::INT_2D_ARRAY, "INT_2D_ARRAY"},
    {(int)ImageType::INT_3D, "INT_3D"},
    {(int)ImageType::INT_CUBE, "INT_CUBE"},
    {(int)ImageType::INT_CUBE_ARRAY, "INT_CUBE_ARRAY"},
    {(int)ImageType::UINT_BUFFER, "UINT_BUFFER"},
    {(int)ImageType::UINT_1D, "UINT_1D"},
    {(int)ImageType::UINT_1D_ARRAY, "UINT_1D_ARRAY"},
    {(int)ImageType::UINT_2D, "UINT_2D"},
    {(int)ImageType::UINT_2D_ARRAY, "UINT_2D_ARRAY"},
    {(int)ImageType::UINT_3D, "UINT_3D"},
    {(int)ImageType::UINT_CUBE, "UINT_CUBE"},
    {(int)ImageType::UINT_CUBE_ARRAY, "UINT_CUBE_ARRAY"},
    {(int)ImageType::SHADOW_2D, "SHADOW_2D"},
    {(int)ImageType::SHADOW_2D_ARRAY, "SHADOW_2D_ARRAY"},
    {(int)ImageType::SHADOW_CUBE, "SHADOW_CUBE"},
    {(int)ImageType::SHADOW_CUBE_ARRAY, "SHADOW_CUBE_ARRAY"},
    {(int)ImageType::DEPTH_2D, "DEPTH_2D"},
    {(int)ImageType::DEPTH_2D_ARRAY, "DEPTH_2D_ARRAY"},
    {(int)ImageType::DEPTH_CUBE, "DEPTH_CUBE"},
    {(int)ImageType::DEPTH_CUBE_ARRAY, "DEPTH_CUBE_ARRAY"},
    {0, nullptr},
};

static const struct PyC_StringEnumItems pygpu_dualblend_items[] = {
    {(int)DualBlend::NONE, "NONE"},
    {(int)DualBlend::SRC_0, "SRC_0"},
    {(int)DualBlend::SRC_1, "SRC_1"},
    {0, nullptr},
};

/* -------------------------------------------------------------------- */
/** \name GPUStageInterfaceInfo Methods
 * \{ */

static bool pygpu_interface_info_get_args(BPyGPUStageInterfaceInfo *self,
                                          PyObject *args,
                                          const char *format,
                                          Type *r_type,
                                          const char **r_name)
{
  struct PyC_StringEnum pygpu_type = {pygpu_attrtype_items};
  PyObject *py_name;

  if (!PyArg_ParseTuple(args, format, PyC_ParseStringEnum, &pygpu_type, &py_name)) {
    return false;
  }

  const char *name = PyUnicode_AsUTF8(py_name);
  if (name == nullptr) {
    return false;
  }

#ifdef USE_GPU_PY_REFERENCES
  PyList_Append(self->references, (PyObject *)py_name);
#endif

  *r_type = (Type)pygpu_type.value_found;
  *r_name = name;
  return true;
}

PyDoc_STRVAR(pygpu_interface_info_smooth_doc,
             ".. method:: smooth(type, name)\n"
             "\n"
             "   Add an attribute with qualifier of type `smooth` to the interface block.\n"
             "\n"
             "   :param type: One of these types:\n"
             "\n" PYDOC_TYPE_LIST
             "\n"
             "   :type type: str\n"
             "   :param name: name of the attribute.\n"
             "   :type name: str\n");
static PyObject *pygpu_interface_info_smooth(BPyGPUStageInterfaceInfo *self, PyObject *args)
{
  Type type;
  const char *name;
  if (!pygpu_interface_info_get_args(self, args, "O&O:smooth", &type, &name)) {
    return nullptr;
  }

  StageInterfaceInfo *interface = reinterpret_cast<StageInterfaceInfo *>(self->interface);
  interface->smooth(type, name);
  Py_RETURN_NONE;
}

PyDoc_STRVAR(pygpu_interface_info_flat_doc,
             ".. method:: flat(type, name)\n"
             "\n"
             "   Add an attribute with qualifier of type `flat` to the interface block.\n"
             "\n"
             "   :param type: One of these types:\n"
             "\n" PYDOC_TYPE_LIST
             "\n"
             "   :type type: str\n"
             "   :param name: name of the attribute.\n"
             "   :type name: str\n");
static PyObject *pygpu_interface_info_flat(BPyGPUStageInterfaceInfo *self, PyObject *args)
{
  Type type;
  const char *name;
  if (!pygpu_interface_info_get_args(self, args, "O&O:flat", &type, &name)) {
    return nullptr;
  }

  StageInterfaceInfo *interface = reinterpret_cast<StageInterfaceInfo *>(self->interface);
  interface->flat(type, name);
  Py_RETURN_NONE;
}

PyDoc_STRVAR(
    pygpu_interface_info_no_perspective_doc,
    ".. method:: no_perspective(type, name)\n"
    "\n"
    "   Add an attribute with qualifier of type `no_perspective` to the interface block.\n"
    "\n"
    "   :param type: One of these types:\n"
    "\n" PYDOC_TYPE_LIST
    "\n"
    "   :type type: str\n"
    "   :param name: name of the attribute.\n"
    "   :type name: str\n");
static PyObject *pygpu_interface_info_no_perspective(BPyGPUStageInterfaceInfo *self,
                                                     PyObject *args)
{
  Type type;
  const char *name;
  if (!pygpu_interface_info_get_args(self, args, "O&O:no_perspective", &type, &name)) {
    return nullptr;
  }

  StageInterfaceInfo *interface = reinterpret_cast<StageInterfaceInfo *>(self->interface);
  interface->no_perspective(type, name);
  Py_RETURN_NONE;
}

static struct PyMethodDef pygpu_interface_info__tp_methods[] = {
    {"smooth",
     (PyCFunction)pygpu_interface_info_smooth,
     METH_VARARGS,
     pygpu_interface_info_smooth_doc},
    {"flat", (PyCFunction)pygpu_interface_info_flat, METH_VARARGS, pygpu_interface_info_flat_doc},
    {"no_perspective",
     (PyCFunction)pygpu_interface_info_no_perspective,
     METH_VARARGS,
     pygpu_interface_info_no_perspective_doc},
    {nullptr, nullptr, 0, nullptr},
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name GPUStageInterfaceInfo Getters and Setters
 * \{ */

PyDoc_STRVAR(pygpu_interface_info_name_doc,
             "Name of the interface block.\n"
             "\n"
             ":type: str");
static PyObject *pygpu_interface_info_name_get(BPyGPUStageInterfaceInfo *self,
                                               void *UNUSED(closure))
{
  StageInterfaceInfo *interface = reinterpret_cast<StageInterfaceInfo *>(self->interface);
  return PyUnicode_FromString(interface->name.c_str());
}

static PyGetSetDef pygpu_interface_info__tp_getseters[] = {
    {"name",
     (getter)pygpu_interface_info_name_get,
     (setter) nullptr,
     pygpu_interface_info_name_doc,
     nullptr},
    {nullptr, nullptr, nullptr, nullptr, nullptr} /* Sentinel */
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name GPUStageInterfaceInfo Type
 * \{ */

static PyObject *pygpu_interface_info__tp_new(PyTypeObject *UNUSED(type),
                                              PyObject *args,
                                              PyObject *kwds)
{
  if (kwds) {
    PyErr_SetString(PyExc_TypeError, "no keywords are expected");
    return nullptr;
  }

  const char *name;
  if (!PyArg_ParseTuple(args, "s:GPUStageInterfaceInfo.__new__*", &name)) {
    return nullptr;
  }

  StageInterfaceInfo *interface = new StageInterfaceInfo(name, "");
  GPUStageInterfaceInfo *interface_info = reinterpret_cast<GPUStageInterfaceInfo *>(interface);

  auto *self = BPyGPUStageInterfaceInfo_CreatePyObject(interface_info);

#ifdef USE_GPU_PY_REFERENCES
  PyObject *py_name = PyTuple_GET_ITEM(args, 0);
  PyList_Append(((BPyGPUStageInterfaceInfo *)self)->references, py_name);
#endif

  return self;
}

#ifdef USE_GPU_PY_REFERENCES

static int pygpu_interface_info__tp_traverse(PyObject *self, visitproc visit, void *arg)
{
  BPyGPUStageInterfaceInfo *py_interface = reinterpret_cast<BPyGPUStageInterfaceInfo *>(self);
  Py_VISIT(py_interface->references);
  return 0;
}

static int pygpu_interface_info__tp_clear(PyObject *self)
{
  BPyGPUStageInterfaceInfo *py_interface = reinterpret_cast<BPyGPUStageInterfaceInfo *>(self);
  Py_CLEAR(py_interface->references);
  return 0;
}

#endif

static void pygpu_interface_info__tp_dealloc(PyObject *self)
{
  BPyGPUStageInterfaceInfo *py_interface = reinterpret_cast<BPyGPUStageInterfaceInfo *>(self);
  StageInterfaceInfo *interface = reinterpret_cast<StageInterfaceInfo *>(py_interface->interface);
  delete interface;

#ifdef USE_GPU_PY_REFERENCES
  PyObject_GC_UnTrack(self);
  if (py_interface->references) {
    pygpu_interface_info__tp_clear(self);
    Py_CLEAR(py_interface->references);
  }
#endif

  Py_TYPE(self)->tp_free((PyObject *)self);
}

PyDoc_STRVAR(pygpu_interface_info__tp_doc,
             ".. class:: GPUStageInterfaceInfo(name)\n"
             "\n"
             "   List of varyings between shader stages.\n\n"
             "\n"
             "   :param name: Name of the interface block.\n"
             "   :type value: str\n");
constexpr PyTypeObject pygpu_interface_info_type()
{
  PyTypeObject pytype = {PyVarObject_HEAD_INIT(nullptr, 0)};
  pytype.tp_name = "GPUStageInterfaceInfo";
  pytype.tp_basicsize = sizeof(BPyGPUStageInterfaceInfo);
  pytype.tp_dealloc = pygpu_interface_info__tp_dealloc;
  pytype.tp_doc = pygpu_interface_info__tp_doc;
#ifdef USE_GPU_PY_REFERENCES
  pytype.tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC;
  pytype.tp_traverse = pygpu_interface_info__tp_traverse;
  pytype.tp_clear = pygpu_interface_info__tp_clear;
#else
  pytype.tp_flags = Py_TPFLAGS_DEFAULT,
#endif
  pytype.tp_methods = pygpu_interface_info__tp_methods;
  pytype.tp_getset = pygpu_interface_info__tp_getseters;
  pytype.tp_new = pygpu_interface_info__tp_new;
  return pytype;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name GPUShaderCreateInfo Methods
 * \{ */

PyDoc_STRVAR(pygpu_shader_info_vertex_in_doc,
             ".. method:: vertex_in(slot, type, name)\n"
             "\n"
             "   Add a vertex shader input attribute.\n"
             "\n"
             "   :param slot: The attribute index.\n"
             "   :type slot: int\n"
             "   :param type: One of these types:\n"
             "\n" PYDOC_TYPE_LIST
             "\n"
             "   :type type: str\n"
             "   :param name: name of the attribute.\n"
             "   :type name: str\n");
static PyObject *pygpu_shader_info_vertex_in(BPyGPUShaderCreateInfo *self, PyObject *args)
{
  int slot;
  struct PyC_StringEnum pygpu_type = {pygpu_attrtype_items};
  const char *param;

  if (!PyArg_ParseTuple(args, "iO&s:vertex_in", &slot, PyC_ParseStringEnum, &pygpu_type, &param)) {
    return nullptr;
  }

#ifdef USE_GPU_PY_REFERENCES
  PyObject *py_name = PyTuple_GET_ITEM(args, 2);
  PyList_Append(self->references, py_name);
#endif

  ShaderCreateInfo *info = reinterpret_cast<ShaderCreateInfo *>(self->info);
  info->vertex_in(slot, (Type)pygpu_type.value_found, param);
  Py_RETURN_NONE;
}

PyDoc_STRVAR(pygpu_shader_info_vertex_out_doc,
             ".. method:: vertex_out(interface)\n"
             "\n"
             "   Add a vertex shader output interface block.\n"
             "\n"
             "   :param interface: Object describing the block.\n"
             "   :type interface: :class:`gpu.types.GPUStageInterfaceInfo`\n");
static PyObject *pygpu_shader_info_vertex_out(BPyGPUShaderCreateInfo *self,
                                              BPyGPUStageInterfaceInfo *o)
{
  if (!BPyGPUStageInterfaceInfo_Check(o)) {
    PyErr_Format(PyExc_TypeError, "Expected a GPUStageInterfaceInfo, got %s", Py_TYPE(o)->tp_name);
    return nullptr;
  }

#ifdef USE_GPU_PY_REFERENCES
  PyList_Append(self->references, (PyObject *)o);
#endif

  ShaderCreateInfo *info = reinterpret_cast<ShaderCreateInfo *>(self->info);
  StageInterfaceInfo *interface = reinterpret_cast<StageInterfaceInfo *>(o->interface);
  info->vertex_out(*interface);

  Py_RETURN_NONE;
}

PyDoc_STRVAR(pygpu_shader_info_fragment_out_doc,
             ".. method:: fragment_out(slot, type, name, blend='NONE')\n"
             "\n"
             "   Specify a fragment output corresponding to a framebuffer target slot.\n"
             "\n"
             "   :param slot: The attribute index.\n"
             "   :type slot: int\n"
             "   :param type: One of these types:\n"
             "\n" PYDOC_TYPE_LIST
             "\n"
             "   :type type: str\n"
             "   :param name: Name of the attribute.\n"
             "   :type name: str\n"
             "   :param blend: Dual Source Blending Index. It can be 'NONE', 'SRC_0' or 'SRC_1'.\n"
             "   :type blend: str\n");
static PyObject *pygpu_shader_info_fragment_out(BPyGPUShaderCreateInfo *self,
                                                PyObject *args,
                                                PyObject *kwds)
{
  int slot;
  struct PyC_StringEnum pygpu_type = {pygpu_attrtype_items};
  const char *name;
  struct PyC_StringEnum blend_type = {pygpu_dualblend_items, (int)DualBlend::NONE};

  static const char *_keywords[] = {"slot", "type", "name", "blend", nullptr};
  static _PyArg_Parser _parser = {
      "i"  /* `slot` */
      "O&" /* `type` */
      "s"  /* `name` */
      "|$" /* Optional keyword only arguments. */
      "O&" /* `blend` */
      ":fragment_out",
      _keywords,
      nullptr,
  };
  if (!_PyArg_ParseTupleAndKeywordsFast(args,
                                        kwds,
                                        &_parser,
                                        &slot,
                                        PyC_ParseStringEnum,
                                        &pygpu_type,
                                        &name,
                                        PyC_ParseStringEnum,
                                        &blend_type)) {
    return nullptr;
  }

#ifdef USE_GPU_PY_REFERENCES
  PyObject *py_name = PyTuple_GET_ITEM(args, 2);
  PyList_Append(self->references, py_name);
#endif

  ShaderCreateInfo *info = reinterpret_cast<ShaderCreateInfo *>(self->info);
  info->fragment_out(slot, (Type)pygpu_type.value_found, name, (DualBlend)blend_type.value_found);

  Py_RETURN_NONE;
}

PyDoc_STRVAR(
    pygpu_shader_info_uniform_buf_doc,
    ".. method:: uniform_buf(slot, type_name, name)\n"
    "\n"
    "   Specify a uniform variable whose type can be one of those declared in `typedef_source`.\n"
    "\n"
    "   :param slot: The uniform variable index.\n"
    "   :type slot: int\n"
    "   :param type_name: Name of the data type. It can be a struct type defined in the source "
    "passed through the :meth:`gpu.types.GPUShaderCreateInfo.typedef_source`.\n"
    "   :type type_name: str\n"
    "   :param name: The uniform variable name.\n"
    "   :type name: str\n");
static PyObject *pygpu_shader_info_uniform_buf(BPyGPUShaderCreateInfo *self, PyObject *args)
{
  int slot;
  const char *type_name;
  const char *name;

  if (!PyArg_ParseTuple(args, "iss:uniform_buf", &slot, &type_name, &name)) {
    return nullptr;
  }

#ifdef USE_GPU_PY_REFERENCES
  PyList_Append(self->references, PyTuple_GET_ITEM(args, 1)); /* type_name */
  PyList_Append(self->references, PyTuple_GET_ITEM(args, 2)); /* name */
#endif

  ShaderCreateInfo *info = reinterpret_cast<ShaderCreateInfo *>(self->info);
  info->uniform_buf(slot, type_name, name);

  Py_RETURN_NONE;
}

#ifdef USE_PYGPU_SHADER_INFO_IMAGE_METHOD
PyDoc_STRVAR(
    pygpu_shader_info_image_doc,
    ".. method:: image(slot, format, type, name, qualifiers={'NO_RESTRICT'})\n"
    "\n"
    "   Specify an image resource used for arbitrary load and store operations.\n"
    "\n"
    "   :param slot: The image resource index.\n"
    "   :type slot: int\n"
    "   :param format: The GPUTexture format that is passed to the shader. Possible values are:\n"
    "" PYDOC_TEX_FORMAT_ITEMS
    "   :type format: str\n"
    "   :param type: The data type describing how the image is to be read in the shader. "
    "Possible values are:\n"
    "\n" PYDOC_IMAGE_TYPES
    "\n"
    "   :type type: str\n"
    "   :param name: The image resource name.\n"
    "   :type name: str\n"
    "   :param qualifiers: Set containing values that describe how the image resource is to be "
    "read or written. Possible values are:\n"
    "" PYDOC_QUALIFIERS
    ""
    "   :type qualifiers: set\n");
static PyObject *pygpu_shader_info_image(BPyGPUShaderCreateInfo *self,
                                         PyObject *args,
                                         PyObject *kwds)
{
  int slot;
  struct PyC_StringEnum pygpu_texformat = {pygpu_textureformat_items};
  struct PyC_StringEnum pygpu_imagetype = {pygpu_imagetype_items};
  const char *name;
  PyObject *py_qualifiers = nullptr;
  Qualifier qualifier = Qualifier::NO_RESTRICT;

  static const char *_keywords[] = {"slot", "format", "type", "name", "qualifiers", nullptr};
  static _PyArg_Parser _parser = {
      "i"  /* `slot` */
      "O&" /* `format` */
      "O&" /* `type` */
      "s"  /* `name` */
      "|$" /* Optional keyword only arguments. */
      "O"  /* `qualifiers` */
      ":image",
      _keywords,
      nullptr,
  };
  if (!_PyArg_ParseTupleAndKeywordsFast(args,
                                        kwds,
                                        &_parser,
                                        &slot,
                                        PyC_ParseStringEnum,
                                        &pygpu_texformat,
                                        PyC_ParseStringEnum,
                                        &pygpu_imagetype,
                                        &name,
                                        &py_qualifiers)) {
    return nullptr;
  }

  if (py_qualifiers &&
      PyC_FlagSet_ToBitfield(
          pygpu_qualifiers, py_qualifiers, (int *)&qualifier, "shader_info.image") == -1) {
    return nullptr;
  }

#  ifdef USE_GPU_PY_REFERENCES
  PyList_Append(self->references, PyTuple_GET_ITEM(args, 3)); /* name */
#  endif

  ShaderCreateInfo *info = reinterpret_cast<ShaderCreateInfo *>(self->info);
  info->image(slot,
              (eGPUTextureFormat)pygpu_texformat.value_found,
              qualifier,
              (ImageType)pygpu_imagetype.value_found,
              name);

  Py_RETURN_NONE;
}
#endif

PyDoc_STRVAR(
    pygpu_shader_info_sampler_doc,
    ".. method:: sampler(slot, type, name)\n"
    "\n"
    "   Specify an image texture sampler.\n"
    "\n"
    "   :param slot: The image texture sampler index.\n"
    "   :type slot: int\n"
    "   :param type: The data type describing the format of each sampler unit. Possible values "
    "are:\n"
    "\n" PYDOC_IMAGE_TYPES
    "\n"
    "   :type type: str\n"
    "   :param name: The image texture sampler name.\n"
    "   :type name: str\n");
static PyObject *pygpu_shader_info_sampler(BPyGPUShaderCreateInfo *self, PyObject *args)
{
  int slot;
  struct PyC_StringEnum pygpu_samplertype = {pygpu_imagetype_items};
  const char *name;

  if (!PyArg_ParseTuple(
          args, "iO&s:sampler", &slot, PyC_ParseStringEnum, &pygpu_samplertype, &name)) {
    return nullptr;
  }

#ifdef USE_GPU_PY_REFERENCES
  PyList_Append(self->references, PyTuple_GET_ITEM(args, 2)); /* name */
#endif

  ShaderCreateInfo *info = reinterpret_cast<ShaderCreateInfo *>(self->info);
  info->sampler(slot, (ImageType)pygpu_samplertype.value_found, name);

  Py_RETURN_NONE;
}

static int constant_type_size(Type type)
{
  switch (type) {
    case Type::BOOL:
    case Type::FLOAT:
    case Type::INT:
    case Type::UINT:
      return 4;
      break;
    case Type::VEC2:
    case Type::UVEC2:
    case Type::IVEC2:
      return 8;
      break;
    case Type::VEC3:
    case Type::UVEC3:
    case Type::IVEC3:
      return 12;
      break;
    case Type::VEC4:
    case Type::UVEC4:
    case Type::IVEC4:
      return 16;
      break;
    case Type::MAT3:
      return 36 + 3 * 4;
    case Type::MAT4:
      return 64;
      break;
  }
  BLI_assert(false);
  return -1;
}

static int constants_calc_size(ShaderCreateInfo *info)
{
  int size_prev = 0;
  int size_last = 0;
  for (const ShaderCreateInfo::PushConst &uniform : info->push_constants_) {
    int pad = 0;
    int size = constant_type_size(uniform.type);
    if (size_last && size_last != size) {
      /* Calc pad. */
      int pack = (size == 8) ? 8 : 16;
      if (size_last < size) {
        pad = pack - (size_last % pack);
      }
      else {
        pad = size_prev % pack;
      }
    }
    else if (size == 12) {
      /* It is still unclear how Vulkan handles padding for `vec3` constants. For now let's follow
       * the rules of the `std140` layout. */
      pad = 4;
    }
    size_prev += pad + size * std::max(1, uniform.array_size);
    size_last = size;
  }
  return size_prev + (size_prev % 16);
}

PyDoc_STRVAR(pygpu_shader_info_push_constant_doc,
             ".. method:: push_constant(type, name, size=0)\n"
             "\n"
             "   Specify a global access constant.\n"
             "\n"
             "   :param type: One of these types:\n"
             "\n" PYDOC_TYPE_LIST
             "\n"
             "   :type type: str\n"
             "   :param name: Name of the constant.\n"
             "   :type name: str\n"
             "   :param size: If not zero, indicates that the constant is an array with the "
             "specified size.\n"
             "   :type size: uint\n");
static PyObject *pygpu_shader_info_push_constant(BPyGPUShaderCreateInfo *self,
                                                 PyObject *args,
                                                 PyObject *kwds)
{
  struct PyC_StringEnum pygpu_type = {pygpu_attrtype_items};
  const char *name = nullptr;
  int array_size = 0;

  static const char *_keywords[] = {"type", "name", "size", nullptr};
  static _PyArg_Parser _parser = {
      "O&" /* `type` */
      "s"  /* `name` */
      "|"  /* Optional arguments. */
      "I"  /* `size` */
      ":push_constant",
      _keywords,
      nullptr,
  };
  if (!_PyArg_ParseTupleAndKeywordsFast(
          args, kwds, &_parser, PyC_ParseStringEnum, &pygpu_type, &name, &array_size)) {
    return nullptr;
  }

#ifdef USE_GPU_PY_REFERENCES
  PyObject *py_name = PyTuple_GET_ITEM(args, 1);
  PyList_Append(self->references, py_name);
#endif

  ShaderCreateInfo *info = reinterpret_cast<ShaderCreateInfo *>(self->info);
  info->push_constant((Type)pygpu_type.value_found, name, array_size);

#define VULKAN_LIMIT 128
  int size = constants_calc_size(info);
  if (size > VULKAN_LIMIT) {
    printf("Push constants have a minimum supported size of "
    STRINGIFY(VULKAN_LIMIT)
    " bytes, however the constants added so far already reach %d bytes. Consider using UBO.\n", size);
  }
#undef VULKAN_LIMIT

  Py_RETURN_NONE;
}

PyDoc_STRVAR(
    pygpu_shader_info_vertex_source_doc,
    ".. method:: vertex_source(source)\n"
    "\n"
    "   Vertex shader source code written in GLSL.\n"
    "\n"
    "   Example:\n"
    "\n"
    "   .. code-block:: python\n"
    "\n"
    "      \"void main {gl_Position = vec4(pos, 1.0);}\"\n"
    "\n"
    "   :param source: The vertex shader source code.\n"
    "   :type source: str\n"
    "\n"
    "   .. seealso:: `GLSL Cross Compilation "
    "<https://wiki.blender.org/wiki/EEVEE_%26_Viewport/GPU_Module/GLSL_Cross_Compilation>`__\n");
static PyObject *pygpu_shader_info_vertex_source(BPyGPUShaderCreateInfo *self, PyObject *o)
{
  const char *vertex_source = PyUnicode_AsUTF8(o);
  if (vertex_source == nullptr) {
    PyErr_Format(PyExc_ValueError, "expected a string, got %s", Py_TYPE(o)->tp_name);
    return nullptr;
  }

#ifdef USE_GPU_PY_REFERENCES
  if (self->vertex_source) {
    Py_DECREF(self->vertex_source);
  }

  self->vertex_source = o;
  Py_INCREF(o);
#endif

  ShaderCreateInfo *info = reinterpret_cast<ShaderCreateInfo *>(self->info);
  info->vertex_source("common_colormanagement_lib.glsl");
  info->vertex_source_generated = vertex_source;

  Py_RETURN_NONE;
}

PyDoc_STRVAR(
    pygpu_shader_info_fragment_source_doc,
    ".. method:: fragment_source(source)\n"
    "\n"
    "   Fragment shader source code written in GLSL.\n"
    "\n"
    "   Example:\n"
    "\n"
    "   .. code-block:: python\n"
    "\n"
    "      \"void main {fragColor = vec4(0.0, 0.0, 0.0, 1.0);}\"\n"
    "\n"
    "   :param source: The fragment shader source code.\n"
    "   :type source: str\n"
    "\n"
    "   .. seealso:: `GLSL Cross Compilation "
    "<https://wiki.blender.org/wiki/EEVEE_%26_Viewport/GPU_Module/GLSL_Cross_Compilation>`__\n");
static PyObject *pygpu_shader_info_fragment_source(BPyGPUShaderCreateInfo *self, PyObject *o)
{
  const char *fragment_source = PyUnicode_AsUTF8(o);
  if (fragment_source == nullptr) {
    PyErr_Format(PyExc_ValueError, "expected a string, got %s", Py_TYPE(o)->tp_name);
    return nullptr;
  }

#ifdef USE_GPU_PY_REFERENCES
  if (self->fragment_source) {
    Py_DECREF(self->fragment_source);
  }

  self->fragment_source = o;
  Py_INCREF(o);
#endif

  ShaderCreateInfo *info = reinterpret_cast<ShaderCreateInfo *>(self->info);
  info->fragment_source("common_colormanagement_lib.glsl");
  info->fragment_source_generated = fragment_source;

  Py_RETURN_NONE;
}

PyDoc_STRVAR(pygpu_shader_info_typedef_source_doc,
             ".. method:: typedef_source(source)\n"
             "\n"
             "   Source code included before resource declaration. "
             "Useful for defining structs used by Uniform Buffers.\n"
             "\n"
             "   Example:\n"
             "\n"
             ".. code-block:: python\n"
             "\n"
             "   \"struct MyType {int foo; float bar;};\"\n"
             "\n"
             "   :param source: The source code defining types.\n"
             "   :type source: str\n");
static PyObject *pygpu_shader_info_typedef_source(BPyGPUShaderCreateInfo *self, PyObject *o)
{
  const char *typedef_source = PyUnicode_AsUTF8(o);
  if (typedef_source == nullptr) {
    PyErr_Format(PyExc_ValueError, "expected a string, got %s", Py_TYPE(o)->tp_name);
    return nullptr;
  }

#ifdef USE_GPU_PY_REFERENCES
  if (self->typedef_source) {
    Py_DECREF(self->typedef_source);
  }

  self->typedef_source = o;
  Py_INCREF(o);
#endif

  ShaderCreateInfo *info = reinterpret_cast<ShaderCreateInfo *>(self->info);
#if 0
  if (info->typedef_sources_.is_empty()) {
    info->typedef_source("GPU_shader_shared_utils.h");
  }
#endif
  info->typedef_source_generated = typedef_source;

  Py_RETURN_NONE;
}

PyDoc_STRVAR(pygpu_shader_info_define_doc,
             ".. method:: define(name, value)\n"
             "\n"
             "   Add a preprocessing define directive. In GLSL it would be something like:\n"
             "\n"
             ".. code-block:: glsl\n"
             "\n"
             "   #define name value\n"
             "\n"
             "   :param name: Token name.\n"
             "   :type name: str\n"
             "   :param value: Text that replaces token occurrences.\n"
             "   :type value: str\n");
static PyObject *pygpu_shader_info_define(BPyGPUShaderCreateInfo *self, PyObject *args)
{
  const char *name;
  const char *value = nullptr;

  if (!PyArg_ParseTuple(args, "s|s:define", &name, &value)) {
    return nullptr;
  }

#ifdef USE_GPU_PY_REFERENCES
  PyList_Append(self->references, PyTuple_GET_ITEM(args, 0)); /* name */
  if (value) {
    PyList_Append(self->references, PyTuple_GET_ITEM(args, 1)); /* value */
  }
#endif

  ShaderCreateInfo *info = reinterpret_cast<ShaderCreateInfo *>(self->info);
  if (value) {
    info->define(name, value);
  }
  else {
    info->define(name);
  }

  Py_RETURN_NONE;
}

static struct PyMethodDef pygpu_shader_info__tp_methods[] = {
    {"vertex_in",
     (PyCFunction)pygpu_shader_info_vertex_in,
     METH_VARARGS,
     pygpu_shader_info_vertex_in_doc},
    {"vertex_out",
     (PyCFunction)pygpu_shader_info_vertex_out,
     METH_O,
     pygpu_shader_info_vertex_out_doc},
    {"fragment_out",
     (PyCFunction)(void *)pygpu_shader_info_fragment_out,
     METH_VARARGS | METH_KEYWORDS,
     pygpu_shader_info_fragment_out_doc},
    {"uniform_buf",
     (PyCFunction)(void *)pygpu_shader_info_uniform_buf,
     METH_VARARGS,
     pygpu_shader_info_uniform_buf_doc},
#ifdef USE_PYGPU_SHADER_INFO_IMAGE_METHOD
    {"image",
     (PyCFunction)(void *)pygpu_shader_info_image,
     METH_VARARGS | METH_KEYWORDS,
     pygpu_shader_info_image_doc},
#endif
    {"sampler",
     (PyCFunction)pygpu_shader_info_sampler,
     METH_VARARGS,
     pygpu_shader_info_sampler_doc},
    {"push_constant",
     (PyCFunction)(void *)pygpu_shader_info_push_constant,
     METH_VARARGS | METH_KEYWORDS,
     pygpu_shader_info_push_constant_doc},
    {"vertex_source",
     (PyCFunction)pygpu_shader_info_vertex_source,
     METH_O,
     pygpu_shader_info_vertex_source_doc},
    {"fragment_source",
     (PyCFunction)pygpu_shader_info_fragment_source,
     METH_O,
     pygpu_shader_info_fragment_source_doc},
    {"typedef_source",
     (PyCFunction)pygpu_shader_info_typedef_source,
     METH_O,
     pygpu_shader_info_typedef_source_doc},
    {"define", (PyCFunction)pygpu_shader_info_define, METH_VARARGS, pygpu_shader_info_define_doc},
    {nullptr, nullptr, 0, nullptr},
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name GPUShaderCreateInfo Init
 * \{ */

static PyObject *pygpu_shader_info__tp_new(PyTypeObject *UNUSED(type),
                                           PyObject *args,
                                           PyObject *kwds)
{
  if (PyTuple_Size(args) || kwds) {
    PyErr_SetString(PyExc_TypeError, "no args or keywords are expected");
    return nullptr;
  }

  ShaderCreateInfo *info = new ShaderCreateInfo("pyGPU_Shader");
  GPUShaderCreateInfo *shader_info = reinterpret_cast<GPUShaderCreateInfo *>(info);

  return BPyGPUShaderCreateInfo_CreatePyObject(shader_info);
}

#ifdef USE_GPU_PY_REFERENCES

static int pygpu_shader_info__tp_traverse(PyObject *self, visitproc visit, void *arg)
{
  BPyGPUShaderCreateInfo *py_info = reinterpret_cast<BPyGPUShaderCreateInfo *>(self);
  Py_VISIT(py_info->vertex_source);
  Py_VISIT(py_info->fragment_source);
  Py_VISIT(py_info->references);
  return 0;
}

static int pygpu_shader_info__tp_clear(PyObject *self)
{
  BPyGPUShaderCreateInfo *py_info = reinterpret_cast<BPyGPUShaderCreateInfo *>(self);
  Py_CLEAR(py_info->vertex_source);
  Py_CLEAR(py_info->fragment_source);
  Py_CLEAR(py_info->references);
  return 0;
}

#endif

static void pygpu_shader_info__tp_dealloc(PyObject *self)
{
  BPyGPUShaderCreateInfo *py_info = reinterpret_cast<BPyGPUShaderCreateInfo *>(self);
  ShaderCreateInfo *info = reinterpret_cast<ShaderCreateInfo *>(py_info->info);
  delete info;

#ifdef USE_GPU_PY_REFERENCES
  PyObject_GC_UnTrack(self);
  if (py_info->references || py_info->vertex_source || py_info->fragment_source) {
    pygpu_shader_info__tp_clear(self);
    Py_XDECREF(py_info->vertex_source);
    Py_XDECREF(py_info->fragment_source);
    Py_XDECREF(py_info->references);
  }
#endif

  Py_TYPE(self)->tp_free((PyObject *)self);
}

PyDoc_STRVAR(pygpu_shader_info__tp_doc,
             ".. class:: GPUShaderCreateInfo()\n"
             "\n"
             "   Stores and describes types and variables that are used in shader sources.\n");
constexpr PyTypeObject pygpu_shader_info_type()
{
  PyTypeObject pytype = {PyVarObject_HEAD_INIT(nullptr, 0)};
  pytype.tp_name = "GPUShaderCreateInfo";
  pytype.tp_basicsize = sizeof(BPyGPUShaderCreateInfo);
  pytype.tp_dealloc = pygpu_shader_info__tp_dealloc;
  pytype.tp_doc = pygpu_shader_info__tp_doc;
#ifdef USE_GPU_PY_REFERENCES
  pytype.tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC;
  pytype.tp_traverse = pygpu_shader_info__tp_traverse;
  pytype.tp_clear = pygpu_shader_info__tp_clear;
#else
  pytype.tp_flags = Py_TPFLAGS_DEFAULT,
#endif
  pytype.tp_methods = pygpu_shader_info__tp_methods;
  pytype.tp_new = pygpu_shader_info__tp_new;
  return pytype;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Public API
 * \{ */

PyTypeObject BPyGPUStageInterfaceInfo_Type = pygpu_interface_info_type();
PyTypeObject BPyGPUShaderCreateInfo_Type = pygpu_shader_info_type();

PyObject *BPyGPUStageInterfaceInfo_CreatePyObject(GPUStageInterfaceInfo *interface)
{
  BPyGPUStageInterfaceInfo *self;

#ifdef USE_GPU_PY_REFERENCES
  self = (BPyGPUStageInterfaceInfo *)_PyObject_GC_New(&BPyGPUStageInterfaceInfo_Type);
  self->references = PyList_New(0);
#else
  self = PyObject_New(BPyGPUStageInterfaceInfo, &BPyGPUStageInterfaceInfo_Type);
#endif

  self->interface = interface;

  return (PyObject *)self;
}

PyObject *BPyGPUShaderCreateInfo_CreatePyObject(GPUShaderCreateInfo *info)
{
  BPyGPUShaderCreateInfo *self;

#ifdef USE_GPU_PY_REFERENCES
  self = (BPyGPUShaderCreateInfo *)_PyObject_GC_New(&BPyGPUShaderCreateInfo_Type);
  self->vertex_source = nullptr;
  self->fragment_source = nullptr;
  self->typedef_source = nullptr;
  self->references = PyList_New(0);
#else
  self = PyObject_New(BPyGPUShaderCreateInfo, &BPyGPUShaderCreateInfo_Type);
#endif

  self->info = info;
  self->constants_total_size = 0;

  return (PyObject *)self;
}

/** \} */
