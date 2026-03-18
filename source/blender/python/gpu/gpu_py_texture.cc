/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bpygpu
 *
 * This file defines the texture functionalities of the 'gpu' module
 *
 * - Use `bpygpu_` for local API.
 * - Use `BPyGPU` for public API.
 */

#include <Python.h>

#include "BLI_math_base.h"
#include "BLI_string_utf8.h"

#include "DNA_image_types.h"

#include "GPU_context.hh"
#include "GPU_texture.hh"

#include "BKE_image.hh"

#include "../generic/py_capi_utils.hh"
#include "../generic/python_compat.hh" /* IWYU pragma: keep. */

#include "gpu_py.hh"
#include "gpu_py_buffer.hh"

#include "gpu_py_texture.hh" /* own include */

#ifdef WITH_VULKAN_BACKEND
#  include <vulkan/vulkan.h>
#endif

/* Doc-string Literal type for texture formats. */

#define PYDOC_TEX_FORMAT_LITERAL \
  "Literal[" \
  "'RGBA8UI', " \
  "'RGBA8I', " \
  "'RGBA8', " \
  "'RGBA32UI', " \
  "'RGBA32I', " \
  "'RGBA32F', " \
  "'RGBA16UI', " \
  "'RGBA16I', " \
  "'RGBA16F', " \
  "'RGBA16', " \
  "'RG8UI', " \
  "'RG8I', " \
  "'RG8', " \
  "'RG32UI', " \
  "'RG32I', " \
  "'RG32F', " \
  "'RG16UI', " \
  "'RG16I', " \
  "'RG16F', " \
  "'RG16', " \
  "'R8UI', " \
  "'R8I', " \
  "'R8', " \
  "'R32UI', " \
  "'R32I', " \
  "'R32F', " \
  "'R16UI', " \
  "'R16I', " \
  "'R16F', " \
  "'R16', " \
  "'R11F_G11F_B10F', " \
  "'DEPTH32F_STENCIL8', " \
  "'DEPTH24_STENCIL8', " \
  "'SRGB8_A8', " \
  "'RGB16F', " \
  "'SRGB8_A8_DXT1', " \
  "'SRGB8_A8_DXT3', " \
  "'SRGB8_A8_DXT5', " \
  "'RGBA8_DXT1', " \
  "'RGBA8_DXT3', " \
  "'RGBA8_DXT5', " \
  "'DEPTH_COMPONENT32F', " \
  "'DEPTH_COMPONENT24', " \
  "'DEPTH_COMPONENT16']"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name GPUTexture Common Utilities
 * \{ */

const PyC_StringEnumItems pygpu_textureformat_items[] = {
    {int(gpu::TextureFormat::UINT_8_8_8_8), "RGBA8UI"},
    {int(gpu::TextureFormat::SINT_8_8_8_8), "RGBA8I"},
    {int(gpu::TextureFormat::UNORM_8_8_8_8), "RGBA8"},
    {int(gpu::TextureFormat::UINT_32_32_32_32), "RGBA32UI"},
    {int(gpu::TextureFormat::SINT_32_32_32_32), "RGBA32I"},
    {int(gpu::TextureFormat::SFLOAT_32_32_32_32), "RGBA32F"},
    {int(gpu::TextureFormat::UINT_16_16_16_16), "RGBA16UI"},
    {int(gpu::TextureFormat::SINT_16_16_16_16), "RGBA16I"},
    {int(gpu::TextureFormat::SFLOAT_16_16_16_16), "RGBA16F"},
    {int(gpu::TextureFormat::UNORM_16_16_16_16), "RGBA16"},
    {int(gpu::TextureFormat::UINT_8_8), "RG8UI"},
    {int(gpu::TextureFormat::SINT_8_8), "RG8I"},
    {int(gpu::TextureFormat::UNORM_8_8), "RG8"},
    {int(gpu::TextureFormat::UINT_32_32), "RG32UI"},
    {int(gpu::TextureFormat::SINT_32_32), "RG32I"},
    {int(gpu::TextureFormat::SFLOAT_32_32), "RG32F"},
    {int(gpu::TextureFormat::UINT_16_16), "RG16UI"},
    {int(gpu::TextureFormat::SINT_16_16), "RG16I"},
    {int(gpu::TextureFormat::SFLOAT_16_16), "RG16F"},
    {int(gpu::TextureFormat::UNORM_16_16), "RG16"},
    {int(gpu::TextureFormat::UINT_8), "R8UI"},
    {int(gpu::TextureFormat::SINT_8), "R8I"},
    {int(gpu::TextureFormat::UNORM_8), "R8"},
    {int(gpu::TextureFormat::UINT_32), "R32UI"},
    {int(gpu::TextureFormat::SINT_32), "R32I"},
    {int(gpu::TextureFormat::SFLOAT_32), "R32F"},
    {int(gpu::TextureFormat::UINT_16), "R16UI"},
    {int(gpu::TextureFormat::SINT_16), "R16I"},
    {int(gpu::TextureFormat::SFLOAT_16), "R16F"},
    {int(gpu::TextureFormat::UNORM_16), "R16"},
    {int(gpu::TextureFormat::UFLOAT_11_11_10), "R11F_G11F_B10F"},
    {int(gpu::TextureFormat::SFLOAT_32_DEPTH_UINT_8), "DEPTH32F_STENCIL8"},
    {GPU_DEPTH24_STENCIL8_DEPRECATED, "DEPTH24_STENCIL8"},
    {int(gpu::TextureFormat::SRGBA_8_8_8_8), "SRGB8_A8"},
    {int(gpu::TextureFormat::SFLOAT_16_16_16), "RGB16F"},
    {int(gpu::TextureFormat::SRGB_DXT1), "SRGB8_A8_DXT1"},
    {int(gpu::TextureFormat::SRGB_DXT3), "SRGB8_A8_DXT3"},
    {int(gpu::TextureFormat::SRGB_DXT5), "SRGB8_A8_DXT5"},
    {int(gpu::TextureFormat::SNORM_DXT1), "RGBA8_DXT1"},
    {int(gpu::TextureFormat::SNORM_DXT3), "RGBA8_DXT3"},
    {int(gpu::TextureFormat::SNORM_DXT5), "RGBA8_DXT5"},
    {int(gpu::TextureFormat::SFLOAT_32_DEPTH), "DEPTH_COMPONENT32F"},
    {GPU_DEPTH_COMPONENT24_DEPRECATED, "DEPTH_COMPONENT24"},
    {int(gpu::TextureFormat::UNORM_16_DEPTH), "DEPTH_COMPONENT16"},
    {0, nullptr},
};

const PyC_StringEnumItems pygpu_textureextendmode_items[] = {
    {int(GPUSamplerExtendMode::GPU_SAMPLER_EXTEND_MODE_EXTEND), "EXTEND"},
    {int(GPUSamplerExtendMode::GPU_SAMPLER_EXTEND_MODE_REPEAT), "REPEAT"},
    {int(GPUSamplerExtendMode::GPU_SAMPLER_EXTEND_MODE_MIRRORED_REPEAT), "MIRRORED_REPEAT"},
    {int(GPUSamplerExtendMode::GPU_SAMPLER_EXTEND_MODE_CLAMP_TO_BORDER), "CLAMP_TO_BORDER"},
    {0, nullptr},
};

static int pygpu_texture_valid_check(BPyGPUTexture *bpygpu_tex)
{
  if (UNLIKELY(bpygpu_tex->tex == nullptr)) {
    PyErr_SetString(PyExc_ReferenceError,
#ifdef BPYGPU_USE_GPUOBJ_FREE_METHOD
                    "GPU texture was freed, no further access is valid"
#else
                    "GPU texture: internal error"
#endif
    );

    return -1;
  }
  return 0;
}

#define BPYGPU_TEXTURE_CHECK_OBJ(bpygpu) \
  { \
    if (UNLIKELY(pygpu_texture_valid_check(bpygpu) == -1)) { \
      return nullptr; \
    } \
  } \
  ((void)0)

/** \} */

/* -------------------------------------------------------------------- */
/** \name GPUTexture Type
 * \{ */

#define BPYGPU_TEXTURE_EXTEND_MODE_ARG_DOC \
  "   :param extend_mode: the specified extent mode.\n" \
  "   :type extend_mode: Literal['EXTEND', 'REPEAT', 'MIRRORED_REPEAT', 'CLAMP_TO_BORDER']\n";

static PyObject *pygpu_texture__tp_new(PyTypeObject * /*self*/, PyObject *args, PyObject *kwds)
{
  BPYGPU_IS_INIT_OR_ERROR_OBJ;

  PyObject *py_size;
  int size[3] = {1, 1, 1};
  int layers = 0;
  int is_cubemap = false;
  PyC_StringEnum pygpu_textureformat = {pygpu_textureformat_items,
                                        int(gpu::TextureFormat::UNORM_8_8_8_8)};
  BPyGPUBuffer *pybuffer_obj = nullptr;
  PyC_TypeOrNone pybuffer_or_none = PyC_TYPE_OR_NONE_INIT(&BPyGPU_BufferType, &pybuffer_obj);
  char err_out[256] = "unknown error. See console";

  static const char *_keywords[] = {"size", "layers", "is_cubemap", "format", "data", nullptr};
  static _PyArg_Parser _parser = {
      "O"  /* `size` */
      "|$" /* Optional keyword only arguments. */
      "i"  /* `layers` */
      "p"  /* `is_cubemap` */
      "O&" /* `format` */
      "O&" /* `data` */
      ":GPUTexture.__new__",
      _keywords,
      nullptr,
  };
  if (!_PyArg_ParseTupleAndKeywordsFast(args,
                                        kwds,
                                        &_parser,
                                        &py_size,
                                        &layers,
                                        &is_cubemap,
                                        PyC_ParseStringEnum,
                                        &pygpu_textureformat,
                                        PyC_ParseTypeOrNone,
                                        &pybuffer_or_none))
  {
    return nullptr;
  }

  if (pygpu_textureformat.value_found == GPU_DEPTH24_STENCIL8_DEPRECATED) {
    pygpu_textureformat.value_found = int(gpu::TextureFormat::SFLOAT_32_DEPTH_UINT_8);
    PyErr_WarnEx(
        PyExc_DeprecationWarning, "'DEPTH24_STENCIL8' is deprecated. Use 'DEPTH32F_STENCIL8'.", 1);
  }
  if (pygpu_textureformat.value_found == GPU_DEPTH_COMPONENT24_DEPRECATED) {
    pygpu_textureformat.value_found = int(gpu::TextureFormat::SFLOAT_32_DEPTH);
    PyErr_WarnEx(PyExc_DeprecationWarning,
                 "'DEPTH_COMPONENT24' is deprecated. Use 'DEPTH_COMPONENT32F'.",
                 1);
  }

  int len = 1;
  if (PySequence_Check(py_size)) {
    len = PySequence_Size(py_size);
    if ((len < 1) || (len > 3)) {
      PyErr_Format(PyExc_ValueError,
                   "GPUTexture.__new__: \"size\" must be between 1 and 3 in length (got %d)",
                   len);
      return nullptr;
    }
    if (PyC_AsArray(size, sizeof(*size), py_size, len, &PyLong_Type, "GPUTexture.__new__") == -1) {
      return nullptr;
    }
  }
  else if (PyLong_Check(py_size)) {
    size[0] = PyLong_AsLong(py_size);
  }
  else {
    PyErr_SetString(PyExc_ValueError, "GPUTexture.__new__: Expected an int or tuple as first arg");
    return nullptr;
  }

  void *data = nullptr;
  if (pybuffer_obj) {
    if (pybuffer_obj->format != GPU_DATA_FLOAT && pybuffer_obj->format != GPU_DATA_UBYTE) {
      PyErr_SetString(
          PyExc_ValueError,
          "GPUTexture.__new__: Only Buffer of format `FLOAT` or `UBYTE` is supported");
      return nullptr;
    }

    int component_len = GPU_texture_component_len(
        gpu::TextureFormat(pygpu_textureformat.value_found));
    int component_size_expected = (pybuffer_obj->format == GPU_DATA_UBYTE) ? sizeof(uchar) :
                                                                              sizeof(float);
    size_t data_space_expected = size_t(size[0]) * size[1] * size[2] * max_ii(1, layers) *
                                 component_len * component_size_expected;
    if (is_cubemap) {
      data_space_expected *= 6 * size[0];
    }

    if (bpygpu_Buffer_size(pybuffer_obj) < data_space_expected) {
      PyErr_SetString(PyExc_ValueError, "GPUTexture.__new__: Buffer size smaller than requested");
      return nullptr;
    }
    data = pybuffer_obj->buf.as_void;
  }

  /* GPU_texture_create_* always passes GPU_DATA_FLOAT to tex->update() internally.
   * For UBYTE buffers we must NOT pass data inline — instead we allocate without
   * data and upload afterwards with GPU_texture_update(GPU_DATA_UBYTE). */
  const bool is_ubyte = (pybuffer_obj && pybuffer_obj->format == GPU_DATA_UBYTE);
  const float *inline_float_data = is_ubyte ? nullptr : static_cast<const float *>(data);

  gpu::Texture *tex = nullptr;
  if (is_cubemap && len != 1) {
    STRNCPY_UTF8(
        err_out,
        "In cubemaps the same dimension represents height, width and depth. No tuple needed");
  }
  else if (size[0] < 1 || size[1] < 1 || size[2] < 1) {
    STRNCPY_UTF8(err_out, "Values less than 1 are not allowed in dimensions");
  }
  else if (layers && len == 3) {
    STRNCPY_UTF8(err_out, "3D textures have no layers");
  }
  else if (!GPU_context_active_get()) {
    STRNCPY_UTF8(err_out, "No active GPU context found");
  }
  else {
    const char *name = "python_texture";
    eGPUTextureUsage usage = GPU_TEXTURE_USAGE_GENERAL;
    if (is_cubemap) {
      if (layers) {
        tex = GPU_texture_create_cube_array(name,
                                            size[0],
                                            layers,
                                            1,
                                            gpu::TextureFormat(pygpu_textureformat.value_found),
                                            usage,
                                            inline_float_data);
      }
      else {
        tex = GPU_texture_create_cube(name,
                                      size[0],
                                      1,
                                      gpu::TextureFormat(pygpu_textureformat.value_found),
                                      usage,
                                      inline_float_data);
      }
    }
    else if (layers) {
      if (len == 2) {
        tex = GPU_texture_create_2d_array(name,
                                          size[0],
                                          size[1],
                                          layers,
                                          1,
                                          gpu::TextureFormat(pygpu_textureformat.value_found),
                                          usage,
                                          inline_float_data);
      }
      else {
        tex = GPU_texture_create_1d_array(name,
                                          size[0],
                                          layers,
                                          1,
                                          gpu::TextureFormat(pygpu_textureformat.value_found),
                                          usage,
                                          inline_float_data);
      }
    }
    else if (len == 3) {
      tex = GPU_texture_create_3d(name,
                                  size[0],
                                  size[1],
                                  size[2],
                                  1,
                                  gpu::TextureFormat(pygpu_textureformat.value_found),
                                  usage,
                                  is_ubyte ? nullptr : data);
    }
    else if (len == 2) {
      tex = GPU_texture_create_2d(name,
                                  size[0],
                                  size[1],
                                  1,
                                  gpu::TextureFormat(pygpu_textureformat.value_found),
                                  usage,
                                  inline_float_data);
    }
    else {
      tex = GPU_texture_create_1d(name,
                                  size[0],
                                  1,
                                  gpu::TextureFormat(pygpu_textureformat.value_found),
                                  usage,
                                  inline_float_data);
    }

    /* Upload UBYTE data separately with the correct data format. */
    if (tex && is_ubyte && data) {
      GPU_texture_update(tex, GPU_DATA_UBYTE, data);
    }
  }

  if (tex == nullptr) {
    PyErr_Format(PyExc_RuntimeError, "gpu.texture.new(...) failed with '%s'", err_out);
    return nullptr;
  }

  return BPyGPUTexture_CreatePyObject(tex, false);
}

PyDoc_STRVAR(
    /* Wrap. */
    pygpu_texture_width_doc,
    "Width of the texture.\n"
    "\n"
    ":type: int\n");
static PyObject *pygpu_texture_width_get(BPyGPUTexture *self, void * /*type*/)
{
  BPYGPU_TEXTURE_CHECK_OBJ(self);
  return PyLong_FromLong(GPU_texture_width(self->tex));
}

PyDoc_STRVAR(
    /* Wrap. */
    pygpu_texture_height_doc,
    "Height of the texture.\n"
    "\n"
    ":type: int\n");
static PyObject *pygpu_texture_height_get(BPyGPUTexture *self, void * /*type*/)
{
  BPYGPU_TEXTURE_CHECK_OBJ(self);
  return PyLong_FromLong(GPU_texture_height(self->tex));
}

PyDoc_STRVAR(
    /* Wrap. */
    pygpu_texture_format_doc,
    "Format of the texture.\n"
    "\n"
    ":type: str\n");
static PyObject *pygpu_texture_format_get(BPyGPUTexture *self, void * /*type*/)
{
  BPYGPU_TEXTURE_CHECK_OBJ(self);
  gpu::TextureFormat format = GPU_texture_format(self->tex);
  return PyUnicode_FromString(
      PyC_StringEnum_FindIDFromValue(pygpu_textureformat_items, int(format)));
}

PyDoc_STRVAR(
    /* Wrap. */
    pygpu_texture_extend_mode_x_doc,
    ".. method:: extend_mode_x(extend_mode='EXTEND', /)\n"
    "\n"
    "   Set texture sampling method for coordinates outside of the [0..1] uv range along the x "
    "axis.\n"
    "\n" BPYGPU_TEXTURE_EXTEND_MODE_ARG_DOC);
static PyObject *pygpu_texture_extend_mode_x(BPyGPUTexture *self, PyObject *value)
{
  BPYGPU_TEXTURE_CHECK_OBJ(self);

  PyC_StringEnum extend_mode = {pygpu_textureextendmode_items};
  if (!PyC_ParseStringEnum(value, &extend_mode)) {
    return nullptr;
  }

  GPU_texture_extend_mode_x(self->tex, GPUSamplerExtendMode(extend_mode.value_found));
  Py_RETURN_NONE;
}

PyDoc_STRVAR(
    /* Wrap. */
    pygpu_texture_extend_mode_y_doc,
    ".. method:: extend_mode_y(extend_mode='EXTEND', /)\n"
    "\n"
    "   Set texture sampling method for coordinates outside of the [0..1] uv range along the y "
    "axis.\n"
    "\n" BPYGPU_TEXTURE_EXTEND_MODE_ARG_DOC);
static PyObject *pygpu_texture_extend_mode_y(BPyGPUTexture *self, PyObject *value)
{
  BPYGPU_TEXTURE_CHECK_OBJ(self);

  PyC_StringEnum extend_mode = {pygpu_textureextendmode_items};
  if (!PyC_ParseStringEnum(value, &extend_mode)) {
    return nullptr;
  }

  GPU_texture_extend_mode_y(self->tex, GPUSamplerExtendMode(extend_mode.value_found));
  Py_RETURN_NONE;
}

PyDoc_STRVAR(
    /* Wrap. */
    pygpu_texture_extend_mode_doc,
    ".. method:: extend_mode(extend_mode='EXTEND', /)\n"
    "\n"
    "   Set texture sampling method for coordinates outside of the [0..1] uv range along\n"
    "   both the x and y axis.\n"
    "\n" BPYGPU_TEXTURE_EXTEND_MODE_ARG_DOC);
static PyObject *pygpu_texture_extend_mode(BPyGPUTexture *self, PyObject *value)
{
  BPYGPU_TEXTURE_CHECK_OBJ(self);

  PyC_StringEnum extend_mode = {pygpu_textureextendmode_items};
  if (!PyC_ParseStringEnum(value, &extend_mode)) {
    return nullptr;
  }

  GPU_texture_extend_mode(self->tex, GPUSamplerExtendMode(extend_mode.value_found));
  Py_RETURN_NONE;
}

PyDoc_STRVAR(
    /* Wrap. */
    pygpu_texture_filter_mode_doc,
    ".. method:: filter_mode(use_filter)\n"
    "\n"
    "   Set texture filter usage.\n"
    "\n"
    "   :param use_filter: If set to true, the texture will use linear interpolation between "
    "neighboring texels.\n"
    "   :type use_filter: bool\n");
static PyObject *pygpu_texture_filter_mode(BPyGPUTexture *self, PyObject *value)
{
  BPYGPU_TEXTURE_CHECK_OBJ(self);

  bool use_filter;
  if (!PyC_ParseBool(value, &use_filter)) {
    return nullptr;
  }

  GPU_texture_filter_mode(self->tex, use_filter);
  Py_RETURN_NONE;
}

PyDoc_STRVAR(
    /* Wrap. */
    pygpu_texture_mipmap_mode_doc,
    ".. method:: mipmap_mode(use_mipmap=True, use_filter=True)\n"
    "\n"
    "   Set texture filter and mip-map usage.\n"
    "\n"
    "   :param use_mipmap: If set to true, the texture will use mip-mapping as anti-aliasing "
    "method.\n"
    "   :type use_mipmap: bool\n"
    "   :param use_filter: If set to true, the texture will use linear interpolation between "
    "neighboring texels.\n"
    "   :type use_filter: bool\n");
static PyObject *pygpu_texture_mipmap_mode(BPyGPUTexture *self, PyObject *args, PyObject *kwds)
{
  BPYGPU_TEXTURE_CHECK_OBJ(self);

  bool use_mipmap = true;
  bool use_filter = true;
  static const char *_keywords[] = {"use_mipmap", "use_filter"};
  static _PyArg_Parser _parser = {
      "|$" /* Optional keyword only arguments. */
      "b"  /* `use_mipmap` */
      "b"  /* `use_filter` */
      ":mipmap_mode",
      _keywords,
      nullptr,
  };

  if (!_PyArg_ParseTupleAndKeywordsFast(args, kwds, &_parser, &use_mipmap, &use_filter)) {
    return nullptr;
  }

  GPU_texture_mipmap_mode(self->tex, use_mipmap, use_filter);
  Py_RETURN_NONE;
}

PyDoc_STRVAR(
    /* Wrap. */
    pygpu_texture_anisotropic_filter_doc,
    ".. method:: anisotropic_filter(use_anisotropic)\n"
    "\n"
    "   Set anisotropic filter usage. This only has effect if mipmapping is enabled.\n"
    "\n"
    "   :param use_anisotropic: If set to true, the texture will use anisotropic filtering.\n"
    "   :type use_anisotropic: bool\n");
static PyObject *pygpu_texture_anisotropic_filter(BPyGPUTexture *self, PyObject *value)
{
  BPYGPU_TEXTURE_CHECK_OBJ(self);

  bool use_anisotropic;
  if (!PyC_ParseBool(value, &use_anisotropic)) {
    return nullptr;
  }

  GPU_texture_anisotropic_filter(self->tex, use_anisotropic);
  Py_RETURN_NONE;
}

PyDoc_STRVAR(
    /* Wrap. */
    pygpu_texture_clear_doc,
    ".. method:: clear(format='FLOAT', value=(0.0, 0.0, 0.0, 1.0))\n"
    "\n"
    "   Fill texture with specific value.\n"
    "\n"
    "   :param format: The format that describes the content of a single item.\n"
    "      ``UINT_24_8`` is deprecated, use ``FLOAT`` instead.\n"
    "   :type format: " PYDOC_DATAFORMAT_LITERAL
    "\n"
    "   :param value: Sequence each representing the value to fill. Sizes 1..4 are supported.\n"
    "   :type value: Sequence[float] | Sequence[int]\n");
static PyObject *pygpu_texture_clear(BPyGPUTexture *self, PyObject *args, PyObject *kwds)
{
  BPYGPU_TEXTURE_CHECK_OBJ(self);
  PyC_StringEnum pygpu_dataformat = {bpygpu_dataformat_items};
  union {
    int i[4];
    float f[4];
    char c[4];
  } values;

  PyObject *py_values;

  static const char *_keywords[] = {"format", "value", nullptr};
  static _PyArg_Parser _parser = {
      "$"  /* Keyword only arguments. */
      "O&" /* `format` */
      "O"  /* `value` */
      ":clear",
      _keywords,
      nullptr,
  };
  if (!_PyArg_ParseTupleAndKeywordsFast(
          args, kwds, &_parser, PyC_ParseStringEnum, &pygpu_dataformat, &py_values))
  {
    return nullptr;
  }
  if (pygpu_dataformat.value_found == GPU_DATA_UINT_24_8_DEPRECATED) {
    PyErr_WarnEx(PyExc_DeprecationWarning, "`UINT_24_8` is deprecated, use `FLOAT` instead", 1);
  }

  int shape = PySequence_Size(py_values);
  if (shape == -1) {
    return nullptr;
  }

  if (shape > 4) {
    PyErr_SetString(PyExc_AttributeError, "too many dimensions, max is 4");
    return nullptr;
  }

  if (shape != 1 &&
      ELEM(pygpu_dataformat.value_found, GPU_DATA_UINT_24_8_DEPRECATED, GPU_DATA_10_11_11_REV))
  {
    PyErr_SetString(PyExc_AttributeError,
                    "`UINT_24_8` and `10_11_11_REV` only support single values");
    return nullptr;
  }

  memset(&values, 0, sizeof(values));
  if (PyC_AsArray(&values,
                  (pygpu_dataformat.value_found == GPU_DATA_FLOAT) ? sizeof(*values.f) :
                                                                     sizeof(*values.i),
                  py_values,
                  shape,
                  (pygpu_dataformat.value_found == GPU_DATA_FLOAT) ? &PyFloat_Type : &PyLong_Type,
                  "clear") == -1)
  {
    return nullptr;
  }

  if (pygpu_dataformat.value_found == GPU_DATA_UBYTE) {
    /* Convert to byte. */
    values.c[0] = values.i[0];
    values.c[1] = values.i[1];
    values.c[2] = values.i[2];
    values.c[3] = values.i[3];
  }

  GPU_texture_clear(self->tex, eGPUDataFormat(pygpu_dataformat.value_found), &values);
  Py_RETURN_NONE;
}

PyDoc_STRVAR(
    /* Wrap. */
    pygpu_texture_update_doc,
    ".. method:: update(format, data)\n"
    "\n"
    "   Update the entire texture with new pixel data.\n"
    "\n"
    "   :param format: The data format of the pixel data.\n"
    "   :type format: " PYDOC_DATAFORMAT_LITERAL "\n"
    "   :param data: Buffer with pixel data the size of the whole texture.\n"
    "   :type data: :class:`gpu.types.Buffer`\n");
static PyObject *pygpu_texture_update(BPyGPUTexture *self, PyObject *args, PyObject *kwds)
{
  BPYGPU_TEXTURE_CHECK_OBJ(self);

  PyC_StringEnum pygpu_dataformat = {bpygpu_dataformat_items};
  BPyGPUBuffer *pybuffer_obj = nullptr;

  static const char *_keywords[] = {"format", "data", nullptr};
  static _PyArg_Parser _parser = {
      "O&" /* `format` */
      "O!" /* `data` */
      ":update",
      _keywords,
      nullptr,
  };
  if (!_PyArg_ParseTupleAndKeywordsFast(
          args, kwds, &_parser, PyC_ParseStringEnum, &pygpu_dataformat,
          &BPyGPU_BufferType, &pybuffer_obj))
  {
    return nullptr;
  }

  GPU_texture_update(self->tex,
                     eGPUDataFormat(pygpu_dataformat.value_found),
                     pybuffer_obj->buf.as_void);
  Py_RETURN_NONE;
}

PyDoc_STRVAR(
    /* Wrap. */
    pygpu_texture_read_doc,
    ".. method:: read()\n"
    "\n"
    "   Creates a buffer with the value of all pixels.\n"
    "\n"
    "   :return: The Buffer with the read pixels.\n"
    "   :rtype: :class:`gpu.types.Buffer`\n");
static PyObject *pygpu_texture_read(BPyGPUTexture *self)
{
  BPYGPU_TEXTURE_CHECK_OBJ(self);
  gpu::TextureFormat tex_format = GPU_texture_format(self->tex);

  /* #GPU_texture_read is restricted in combining 'data_format' with 'tex_format'.
   * So choose data_format here. */
  eGPUDataFormat best_data_format;
  switch (tex_format) {
    case gpu::TextureFormat::UNORM_16_DEPTH:
    case gpu::TextureFormat::SFLOAT_32_DEPTH:
    case gpu::TextureFormat::SFLOAT_32_DEPTH_UINT_8:
      best_data_format = GPU_DATA_FLOAT;
      break;
    case gpu::TextureFormat::UINT_8:
    case gpu::TextureFormat::UINT_16:
    case gpu::TextureFormat::UINT_16_16:
    case gpu::TextureFormat::UINT_32:
      best_data_format = GPU_DATA_UINT;
      break;
    case gpu::TextureFormat::SINT_16_16:
    case gpu::TextureFormat::SINT_16:
      best_data_format = GPU_DATA_INT;
      break;
    case gpu::TextureFormat::UNORM_8:
    case gpu::TextureFormat::UNORM_8_8:
    case gpu::TextureFormat::UNORM_8_8_8_8:
    case gpu::TextureFormat::UINT_8_8_8_8:
    case gpu::TextureFormat::SRGBA_8_8_8_8:
      best_data_format = GPU_DATA_UBYTE;
      break;
    case gpu::TextureFormat::UFLOAT_11_11_10:
      best_data_format = GPU_DATA_10_11_11_REV;
      break;
    default:
      best_data_format = GPU_DATA_FLOAT;
      break;
  }

  void *buf = GPU_texture_read(self->tex, best_data_format, 0);
  const Py_ssize_t shape[3] = {GPU_texture_height(self->tex),
                               GPU_texture_width(self->tex),
                               Py_ssize_t(GPU_texture_component_len(tex_format))};

  int shape_len = (shape[2] == 1) ? 2 : 3;
  return reinterpret_cast<PyObject *>(
      BPyGPU_Buffer_CreatePyObject(best_data_format, shape, shape_len, buf));
}

#ifdef BPYGPU_USE_GPUOBJ_FREE_METHOD
PyDoc_STRVAR(
    /* Wrap. */
    pygpu_texture_free_doc,
    ".. method:: free()\n"
    "\n"
    "   Free the texture object.\n"
    "   The texture object will no longer be accessible.\n");
static PyObject *pygpu_texture_free(BPyGPUTexture *self)
{
  BPYGPU_TEXTURE_CHECK_OBJ(self);

  GPU_texture_free(self->tex);
  self->tex = nullptr;
  Py_RETURN_NONE;
}
#endif

static void BPyGPUTexture__tp_dealloc(BPyGPUTexture *self)
{
  if (self->tex) {
#ifndef GPU_NO_USE_PY_REFERENCES
    GPU_texture_py_reference_set(self->tex, nullptr);
#endif
    GPU_texture_free(self->tex);
  }
  Py_TYPE(self)->tp_free(reinterpret_cast<PyObject *>(self));
}

static PyGetSetDef pygpu_texture__tp_getseters[] = {
    {"width",
     reinterpret_cast<getter>(pygpu_texture_width_get),
     static_cast<setter>(nullptr),
     pygpu_texture_width_doc,
     nullptr},
    {"height",
     reinterpret_cast<getter>(pygpu_texture_height_get),
     static_cast<setter>(nullptr),
     pygpu_texture_height_doc,
     nullptr},
    {"format",
     reinterpret_cast<getter>(pygpu_texture_format_get),
     static_cast<setter>(nullptr),
     pygpu_texture_format_doc,
     nullptr},
    {nullptr, nullptr, nullptr, nullptr, nullptr} /* Sentinel */
};

#ifdef __GNUC__
#  ifdef __clang__
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wcast-function-type"
#  else
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wcast-function-type"
#  endif
#endif

static PyMethodDef pygpu_texture__tp_methods[] = {
    {"clear",
     reinterpret_cast<PyCFunction>(pygpu_texture_clear),
     METH_VARARGS | METH_KEYWORDS,
     pygpu_texture_clear_doc},
    {"read",
     reinterpret_cast<PyCFunction>(pygpu_texture_read),
     METH_NOARGS,
     pygpu_texture_read_doc},
#ifdef BPYGPU_USE_GPUOBJ_FREE_METHOD
    {"free", (PyCFunction)pygpu_texture_free, METH_NOARGS, pygpu_texture_free_doc},
#endif
    {"extend_mode_x",
     reinterpret_cast<PyCFunction>(pygpu_texture_extend_mode_x),
     METH_O,
     pygpu_texture_extend_mode_x_doc},
    {"extend_mode_y",
     reinterpret_cast<PyCFunction>(pygpu_texture_extend_mode_y),
     METH_O,
     pygpu_texture_extend_mode_y_doc},
    {"extend_mode",
     reinterpret_cast<PyCFunction>(pygpu_texture_extend_mode),
     METH_O,
     pygpu_texture_extend_mode_doc},
    {"filter_mode",
     reinterpret_cast<PyCFunction>(pygpu_texture_filter_mode),
     METH_O,
     pygpu_texture_filter_mode_doc},
    {"mipmap_mode",
     reinterpret_cast<PyCFunction>(pygpu_texture_mipmap_mode),
     METH_VARARGS | METH_KEYWORDS,
     pygpu_texture_mipmap_mode_doc},
    {"anisotropic_filter",
     reinterpret_cast<PyCFunction>(pygpu_texture_anisotropic_filter),
     METH_O,
     pygpu_texture_anisotropic_filter_doc},
    {"update",
     reinterpret_cast<PyCFunction>(pygpu_texture_update),
     METH_VARARGS | METH_KEYWORDS,
     pygpu_texture_update_doc},
    {nullptr, nullptr, 0, nullptr},
};

#ifdef __GNUC__
#  ifdef __clang__
#    pragma clang diagnostic pop
#  else
#    pragma GCC diagnostic pop
#  endif
#endif

PyDoc_STRVAR(
    /* Wrap. */
    pygpu_texture__tp_doc,
    ".. class:: GPUTexture(size, *, layers=0, is_cubemap=False, format='RGBA8', "
    "data=None)\n"
    "\n"
    "   This object gives access to GPU textures.\n"
    "\n"
    "   :param size: Dimensions of the texture 1D, 2D, 3D or cubemap.\n"
    "   :type size: int | Sequence[int]\n"
    "   :param layers: Number of layers in texture array or number of cubemaps in cubemap array\n"
    "   :type layers: int\n"
    "   :param is_cubemap: Indicates the creation of a cubemap texture.\n"
    "   :type is_cubemap: bool\n"
    "   :param format: Internal data format inside GPU memory.\n"
    "      ``DEPTH24_STENCIL8`` is deprecated, use ``DEPTH32F_STENCIL8``.\n"
    "      ``DEPTH_COMPONENT24`` is deprecated, use ``DEPTH_COMPONENT32F``.\n"
    "   :type format: " PYDOC_TEX_FORMAT_LITERAL
    "\n"
    "   :param data: Buffer object to fill the texture.\n"
    "   :type data: :class:`gpu.types.Buffer` | None\n");
PyTypeObject BPyGPUTexture_Type = {
    /*ob_base*/ PyVarObject_HEAD_INIT(nullptr, 0)
    /*tp_name*/ "GPUTexture",
    /*tp_basicsize*/ sizeof(BPyGPUTexture),
    /*tp_itemsize*/ 0,
    /*tp_dealloc*/ reinterpret_cast<destructor>(BPyGPUTexture__tp_dealloc),
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
    /*tp_doc*/ pygpu_texture__tp_doc,
    /*tp_traverse*/ nullptr,
    /*tp_clear*/ nullptr,
    /*tp_richcompare*/ nullptr,
    /*tp_weaklistoffset*/ 0,
    /*tp_iter*/ nullptr,
    /*tp_iternext*/ nullptr,
    /*tp_methods*/ pygpu_texture__tp_methods,
    /*tp_members*/ nullptr,
    /*tp_getset*/ pygpu_texture__tp_getseters,
    /*tp_base*/ nullptr,
    /*tp_dict*/ nullptr,
    /*tp_descr_get*/ nullptr,
    /*tp_descr_set*/ nullptr,
    /*tp_dictoffset*/ 0,
    /*tp_init*/ nullptr,
    /*tp_alloc*/ nullptr,
    /*tp_new*/ pygpu_texture__tp_new,
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
/** \name GPU Texture module
 * \{ */

PyDoc_STRVAR(
    /* Wrap. */
    pygpu_texture_from_image_doc,
    ".. function:: from_image(image)\n"
    "\n"
    "   Get GPUTexture corresponding to an Image data-block. The GPUTexture "
    "memory is "
    "shared with Blender.\n"
    "   Note: Colors read from the texture will be in scene linear color space and have "
    "premultiplied or straight alpha matching the image alpha mode.\n"
    "\n"
    "   :param image: The Image data-block.\n"
    "   :type image: :class:`bpy.types.Image`\n"
    "   :return: The GPUTexture used by the image.\n"
    "   :rtype: :class:`gpu.types.GPUTexture`\n");
static PyObject *pygpu_texture_from_image(PyObject * /*self*/, PyObject *arg)
{
  Image *ima = static_cast<Image *>(PyC_RNA_AsPointer(arg, "Image"));
  if (ima == nullptr) {
    return nullptr;
  }

  ImageUser iuser;
  BKE_imageuser_default(&iuser);
  gpu::Texture *tex = BKE_image_get_gpu_texture(ima, &iuser);

  return BPyGPUTexture_CreatePyObject(tex, true);
}

PyDoc_STRVAR(
    /* Wrap. */
    pygpu_texture_set_image_doc,
    ".. function:: set_image_texture(image, texture)\n"
    "\n"
    "   Override the GPU texture used by a :class:`bpy.types.Image` datablock.\n"
    "   Material Image Texture nodes will sample from ``texture`` instead of\n"
    "   uploading pixel data from CPU memory.\n"
    "   Pass ``None`` to clear the override and restore normal behaviour.\n"
    "\n"
    "   .. warning::\n"
    "      The image does not take ownership of the texture.\n"
    "      You must keep it alive for as long as the image may be rendered,\n"
    "      and free it manually with :meth:`gpu.types.GPUTexture.free`.\n"
    "\n"
    "   :param image: The Image datablock to override.\n"
    "   :type image: :class:`bpy.types.Image`\n"
    "   :param texture: Texture to use, or ``None`` to clear.\n"
    "   :type texture: :class:`gpu.types.GPUTexture` | None\n");
static PyObject *pygpu_texture_set_image(PyObject * /*self*/, PyObject *args)
{
  PyObject *py_image;
  PyObject *py_tex;

  if (!PyArg_ParseTuple(args, "OO:set_image_texture", &py_image, &py_tex)) {
    return nullptr;
  }

  Image *ima = static_cast<Image *>(PyC_RNA_AsPointer(py_image, "Image"));
  if (ima == nullptr) {
    return nullptr;
  }

  gpu::Texture *tex = nullptr;
  if (py_tex != Py_None) {
    if (!BPyGPUTexture_Check(py_tex)) {
      PyErr_SetString(PyExc_TypeError, "set_image_texture: expected a GPUTexture or None");
      return nullptr;
    }
    if (UNLIKELY(pygpu_texture_valid_check(reinterpret_cast<BPyGPUTexture *>(py_tex)) == -1)) {
      return nullptr;
    }
    tex = reinterpret_cast<BPyGPUTexture *>(py_tex)->tex;
  }

  BKE_image_set_gpu_texture_override(ima, tex);
  Py_RETURN_NONE;
}

#ifdef __linux__
#ifdef WITH_OPENGL_BACKEND
PyDoc_STRVAR(
    /* Wrap. */
    pygpu_texture_from_dmabuf_doc,
    ".. function:: from_dmabuf(fd, width, height, stride, drm_format, format)\n"
    "\n"
    "   Create a :class:`gpu.types.GPUTexture` backed by a Linux DMA-BUF file descriptor.\n"
    "   Uses EGL_EXT_image_dma_buf_import for zero-copy GPU import.\n"
    "   Returns ``None`` if EGL or the required extension is unavailable.\n"
    "\n"
    "   :param fd: DMA-BUF file descriptor (not closed by this call).\n"
    "   :type fd: int\n"
    "   :param width: Texture width in pixels.\n"
    "   :type width: int\n"
    "   :param height: Texture height in pixels.\n"
    "   :type height: int\n"
    "   :param stride: Row stride in bytes.\n"
    "   :type stride: int\n"
    "   :param drm_format: DRM fourcc pixel format integer.\n"
    "      Use ``0x34324241`` for RGBA8 (DRM_FORMAT_ABGR8888).\n"
    "   :type drm_format: int\n"
    "   :param format: Internal GPU texture format string.\n"
    "   :type format: " PYDOC_TEX_FORMAT_LITERAL "\n"
    "   :return: The created texture, or None on failure.\n"
    "   :rtype: :class:`gpu.types.GPUTexture` | None\n");
static PyObject *pygpu_texture_from_dmabuf(PyObject * /*self*/, PyObject *args, PyObject *kwds)
{
  int fd, w, h, stride, drm_format;
  PyC_StringEnum pygpu_textureformat = {pygpu_textureformat_items};

  static const char *_keywords[] = {
      "fd", "width", "height", "stride", "drm_format", "format", nullptr};
  static _PyArg_Parser _parser = {
      "iiiii" /* fd, width, height, stride, drm_format */
      "O&"    /* format */
      ":from_dmabuf",
      _keywords,
      nullptr,
  };
  if (!_PyArg_ParseTupleAndKeywordsFast(args,
                                        kwds,
                                        &_parser,
                                        &fd,
                                        &w,
                                        &h,
                                        &stride,
                                        &drm_format,
                                        PyC_ParseStringEnum,
                                        &pygpu_textureformat))
  {
    return nullptr;
  }

  gpu::Texture *tex = GPU_texture_create_from_dmabuf(
      "py_dmabuf",
      w,
      h,
      stride,
      drm_format,
      gpu::TextureFormat(pygpu_textureformat.value_found),
      fd);
  if (tex == nullptr) {
    Py_RETURN_NONE;
  }
  return BPyGPUTexture_CreatePyObject(tex, false);
}
#endif /* WITH_OPENGL_BACKEND */
#endif   /* __linux__ */

#ifdef WITH_VULKAN_BACKEND
PyDoc_STRVAR(
    /* Wrap. */
    pygpu_texture_from_external_memory_doc,
    ".. function:: from_external_memory(handle, handle_type, format, width, height, stride)\n"
    "\n"
    "   Create a :class:`gpu.types.GPUTexture` from an externally-owned GPU memory handle.\n"
    "   Zero-copy Vulkan external memory import. Requires the Vulkan backend and the matching\n"
    "   device extension (VK_KHR_external_memory_fd on Linux,\n"
    "   VK_KHR_external_memory_win32 on Windows).\n"
    "   Returns ``None`` if the extension is unavailable or import fails.\n"
    "\n"
    "   :param handle: DMA-BUF fd (Linux) or Win32 HANDLE (Windows) cast to int.\n"
    "   :type handle: int\n"
    "   :param handle_type: External memory handle type string.\n"
    "      ``'DMA_BUF'``   – ``VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT`` (Linux).\n"
    "      ``'D3D11_KMT'`` – ``VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT`` (Windows).\n"
    "   :type handle_type: str\n"
    "   :param format: Internal GPU texture format string.\n"
    "   :type format: " PYDOC_TEX_FORMAT_LITERAL "\n"
    "   :param width: Texture width in pixels.\n"
    "   :type width: int\n"
    "   :param height: Texture height in pixels.\n"
    "   :type height: int\n"
    "   :param stride: Row stride in bytes.\n"
    "   :type stride: int\n"
    "   :return: The imported texture, or None on failure.\n"
    "   :rtype: :class:`gpu.types.GPUTexture` | None\n");
static PyObject *pygpu_texture_from_external_memory(PyObject * /*self*/,
                                                    PyObject *args,
                                                    PyObject *kwds)
{
  int64_t handle;
  const char *handle_type_str;
  PyC_StringEnum pygpu_textureformat = {pygpu_textureformat_items};
  int w, h, stride;

  static const char *_keywords[] = {
      "handle", "handle_type", "format", "width", "height", "stride", nullptr};
  static _PyArg_Parser _parser = {
      "L"  /* handle */
      "s"  /* handle_type */
      "O&" /* format */
      "iii" /* width, height, stride */
      ":from_external_memory",
      _keywords,
      nullptr,
  };
  if (!_PyArg_ParseTupleAndKeywordsFast(args,
                                        kwds,
                                        &_parser,
                                        &handle,
                                        &handle_type_str,
                                        PyC_ParseStringEnum,
                                        &pygpu_textureformat,
                                        &w,
                                        &h,
                                        &stride))
  {
    return nullptr;
  }

  VkExternalMemoryHandleTypeFlagBits vk_handle_type;
  if (STREQ(handle_type_str, "DMA_BUF")) {
#  if !defined(_WIN32) && !defined(__APPLE__)
    vk_handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
#  else
    PyErr_SetString(PyExc_ValueError, "DMA_BUF handle type is only supported on Linux");
    return nullptr;
#  endif
  }
  else if (STREQ(handle_type_str, "D3D11_KMT")) {
#  ifdef _WIN32
    vk_handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;
#  else
    PyErr_SetString(PyExc_ValueError, "D3D11_KMT handle type is only supported on Windows");
    return nullptr;
#  endif
  }
  else {
    PyErr_Format(PyExc_ValueError,
                 "Unknown handle_type '%s'. Expected 'DMA_BUF' or 'D3D11_KMT'",
                 handle_type_str);
    return nullptr;
  }

  gpu::Texture *tex = GPU_texture_create_from_external_memory(
      "py_ext_mem",
      w,
      h,
      uint32_t(stride),
      gpu::TextureFormat(pygpu_textureformat.value_found),
      uint32_t(vk_handle_type),
      handle);
  if (tex == nullptr) {
    Py_RETURN_NONE;
  }
  return BPyGPUTexture_CreatePyObject(tex, false);
}
#endif /* WITH_VULKAN_BACKEND */

static PyMethodDef pygpu_texture__m_methods[] = {
    {"from_image",
     static_cast<PyCFunction>(pygpu_texture_from_image),
     METH_O,
     pygpu_texture_from_image_doc},
    {"set_image_texture",
     reinterpret_cast<PyCFunction>(pygpu_texture_set_image),
     METH_VARARGS,
     pygpu_texture_set_image_doc},
#ifdef __linux__
#ifdef WITH_OPENGL_BACKEND
    {"from_dmabuf",
     reinterpret_cast<PyCFunction>(pygpu_texture_from_dmabuf),
     METH_VARARGS | METH_KEYWORDS,
     pygpu_texture_from_dmabuf_doc},
#endif
#endif
#ifdef WITH_VULKAN_BACKEND
    {"from_external_memory",
     reinterpret_cast<PyCFunction>(pygpu_texture_from_external_memory),
     METH_VARARGS | METH_KEYWORDS,
     pygpu_texture_from_external_memory_doc},
#endif
    {nullptr, nullptr, 0, nullptr},
};

PyDoc_STRVAR(
    /* Wrap. */
    pygpu_texture__m_doc,
    "This module provides utilities for textures.");
static PyModuleDef pygpu_texture_module_def = {
    /*m_base*/ PyModuleDef_HEAD_INIT,
    /*m_name*/ "gpu.texture",
    /*m_doc*/ pygpu_texture__m_doc,
    /*m_size*/ 0,
    /*m_methods*/ pygpu_texture__m_methods,
    /*m_slots*/ nullptr,
    /*m_traverse*/ nullptr,
    /*m_clear*/ nullptr,
    /*m_free*/ nullptr,
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Local API
 * \{ */

int bpygpu_ParseTexture(PyObject *o, void *p)
{
  if (o == Py_None) {
    *static_cast<gpu::Texture **>(p) = nullptr;
    return 1;
  }

  if (!BPyGPUTexture_Check(o)) {
    PyErr_Format(
        PyExc_ValueError, "expected a texture or None object, got %s", Py_TYPE(o)->tp_name);
    return 0;
  }

  if (UNLIKELY(pygpu_texture_valid_check((BPyGPUTexture *)o) == -1)) {
    return 0;
  }

  *static_cast<gpu::Texture **>(p) = (reinterpret_cast<BPyGPUTexture *>(o))->tex;
  return 1;
}

PyObject *bpygpu_texture_init()
{
  PyObject *submodule;
  submodule = PyModule_Create(&pygpu_texture_module_def);

  return submodule;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Public API
 * \{ */

PyObject *BPyGPUTexture_CreatePyObject(gpu::Texture *tex, bool shared_reference)
{
  BPyGPUTexture *self;

  if (shared_reference) {
#ifndef GPU_NO_USE_PY_REFERENCES
    void **ref = GPU_texture_py_reference_get(tex);
    if (ref) {
      /* Retrieve BPyGPUTexture reference. */
      self = reinterpret_cast<BPyGPUTexture *> POINTER_OFFSET(ref, -offsetof(BPyGPUTexture, tex));
      BLI_assert(self->tex == tex);
      Py_INCREF(self);
      return reinterpret_cast<PyObject *>(self);
    }
#endif

    GPU_texture_ref(tex);
  }

  self = PyObject_New(BPyGPUTexture, &BPyGPUTexture_Type);
  self->tex = tex;

#ifndef GPU_NO_USE_PY_REFERENCES
  BLI_assert(GPU_texture_py_reference_get(tex) == nullptr);
  GPU_texture_py_reference_set(tex, reinterpret_cast<void **>(&self->tex));
#endif

  return reinterpret_cast<PyObject *>(self);
}

/** \} */

#undef BPYGPU_TEXTURE_CHECK_OBJ

}  // namespace blender
