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
#include "GPU_texture.h"
#include "GPU_uniform_buffer.h"

#include "../generic/py_capi_utils.h"
#include "../mathutils/mathutils.h"

#include "gpu_py.h"
#include "gpu_py_texture.h"
#include "gpu_py_uniformbuffer.h"
#include "gpu_py_vertex_format.h"

#include "gpu_py_shader.h" /* own include */

/* -------------------------------------------------------------------- */
/** \name Enum Conversion.
 * \{ */

#define PYDOC_BUILTIN_SHADER_DESCRIPTION \
  "``2D_FLAT_COLOR``\n" \
  "   :Attributes: vec2 pos, vec4 color\n" \
  "   :Uniforms: none\n" \
  "``2D_IMAGE``\n" \
  "   :Attributes: vec2 pos, vec2 texCoord\n" \
  "   :Uniforms: sampler2D image\n" \
  "``2D_SMOOTH_COLOR``\n" \
  "   :Attributes: vec2 pos, vec4 color\n" \
  "   :Uniforms: none\n" \
  "``2D_UNIFORM_COLOR``\n" \
  "   :Attributes: vec2 pos\n" \
  "   :Uniforms: vec4 color\n" \
  "``3D_FLAT_COLOR``\n" \
  "   :Attributes: vec3 pos, vec4 color\n" \
  "   :Uniforms: none\n" \
  "``3D_IMAGE``\n" \
  "   :Attributes: vec3 pos, vec2 texCoord\n" \
  "   :Uniforms: sampler2D image\n" \
  "``3D_SMOOTH_COLOR``\n" \
  "   :Attributes: vec3 pos, vec4 color\n" \
  "   :Uniforms: none\n" \
  "``3D_UNIFORM_COLOR``\n" \
  "   :Attributes: vec3 pos\n" \
  "   :Uniforms: vec4 color\n" \
  "``3D_POLYLINE_FLAT_COLOR``\n" \
  "   :Attributes: vec3 pos, vec4 color\n" \
  "   :Uniforms: vec2 viewportSize, float lineWidth\n" \
  "``3D_POLYLINE_SMOOTH_COLOR``\n" \
  "   :Attributes: vec3 pos, vec4 color\n" \
  "   :Uniforms: vec2 viewportSize, float lineWidth\n" \
  "``3D_POLYLINE_UNIFORM_COLOR``\n" \
  "   :Attributes: vec3 pos\n" \
  "   :Uniforms: vec2 viewportSize, float lineWidth\n"

static const struct PyC_StringEnumItems pygpu_shader_builtin_items[] = {
    {GPU_SHADER_2D_FLAT_COLOR, "2D_FLAT_COLOR"},
    {GPU_SHADER_2D_IMAGE, "2D_IMAGE"},
    {GPU_SHADER_2D_SMOOTH_COLOR, "2D_SMOOTH_COLOR"},
    {GPU_SHADER_2D_UNIFORM_COLOR, "2D_UNIFORM_COLOR"},
    {GPU_SHADER_3D_FLAT_COLOR, "3D_FLAT_COLOR"},
    {GPU_SHADER_3D_IMAGE, "3D_IMAGE"},
    {GPU_SHADER_3D_SMOOTH_COLOR, "3D_SMOOTH_COLOR"},
    {GPU_SHADER_3D_UNIFORM_COLOR, "3D_UNIFORM_COLOR"},
    {GPU_SHADER_3D_POLYLINE_FLAT_COLOR, "3D_POLYLINE_FLAT_COLOR"},
    {GPU_SHADER_3D_POLYLINE_SMOOTH_COLOR, "3D_POLYLINE_SMOOTH_COLOR"},
    {GPU_SHADER_3D_POLYLINE_UNIFORM_COLOR, "3D_POLYLINE_UNIFORM_COLOR"},
    {0, NULL},
};

static const struct PyC_StringEnumItems pygpu_shader_config_items[] = {
    {GPU_SHADER_CFG_DEFAULT, "DEFAULT"},
    {GPU_SHADER_CFG_CLIPPED, "CLIPPED"},
    {0, NULL},
};

static int pygpu_shader_uniform_location_get(GPUShader *shader,
                                             const char *name,
                                             const char *error_prefix)
{
  const int uniform = GPU_shader_get_uniform(shader, name);

  if (uniform == -1) {
    PyErr_Format(PyExc_ValueError, "%s: uniform %.32s not found", error_prefix, name);
  }

  return uniform;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shader Type
 * \{ */

static PyObject *pygpu_shader__tp_new(PyTypeObject *UNUSED(type), PyObject *args, PyObject *kwds)
{
  BPYGPU_IS_INIT_OR_ERROR_OBJ;

  struct {
    const char *vertexcode;
    const char *fragcode;
    const char *geocode;
    const char *libcode;
    const char *defines;
    const char *name;
  } params = {0};

  static const char *_keywords[] = {
      "vertexcode", "fragcode", "geocode", "libcode", "defines", "name", NULL};
  static _PyArg_Parser _parser = {
      "s"  /* `vertexcode` */
      "s"  /* `fragcode` */
      "|$" /* Optional keyword only arguments. */
      "s"  /* `geocode` */
      "s"  /* `libcode` */
      "s"  /* `defines` */
      "s"  /* `name` */
      ":GPUShader.__new__",
      _keywords,
      0,
  };
  if (!_PyArg_ParseTupleAndKeywordsFast(args,
                                        kwds,
                                        &_parser,
                                        &params.vertexcode,
                                        &params.fragcode,
                                        &params.geocode,
                                        &params.libcode,
                                        &params.defines,
                                        &params.name)) {
    return NULL;
  }

  GPUShader *shader = GPU_shader_create_from_python(params.vertexcode,
                                                    params.fragcode,
                                                    params.geocode,
                                                    params.libcode,
                                                    params.defines,
                                                    params.name);

  if (shader == NULL) {
    PyErr_SetString(PyExc_Exception, "Shader Compile Error, see console for more details");
    return NULL;
  }

  return BPyGPUShader_CreatePyObject(shader, false);
}

PyDoc_STRVAR(
    pygpu_shader_bind_doc,
    ".. method:: bind()\n"
    "\n"
    "   Bind the shader object. Required to be able to change uniforms of this shader.\n");
static PyObject *pygpu_shader_bind(BPyGPUShader *self)
{
  GPU_shader_bind(self->shader);
  Py_RETURN_NONE;
}

PyDoc_STRVAR(pygpu_shader_uniform_from_name_doc,
             ".. method:: uniform_from_name(name)\n"
             "\n"
             "   Get uniform location by name.\n"
             "\n"
             "   :param name: Name of the uniform variable whose location is to be queried.\n"
             "   :type name: str\n"
             "   :return: Location of the uniform variable.\n"
             "   :rtype: int\n");
static PyObject *pygpu_shader_uniform_from_name(BPyGPUShader *self, PyObject *arg)
{
  const char *name = PyUnicode_AsUTF8(arg);
  if (name == NULL) {
    return NULL;
  }

  const int uniform = pygpu_shader_uniform_location_get(
      self->shader, name, "GPUShader.get_uniform");

  if (uniform == -1) {
    return NULL;
  }

  return PyLong_FromLong(uniform);
}

PyDoc_STRVAR(
    pygpu_shader_uniform_block_from_name_doc,
    ".. method:: uniform_block_from_name(name)\n"
    "\n"
    "   Get uniform block location by name.\n"
    "\n"
    "   :param name: Name of the uniform block variable whose location is to be queried.\n"
    "   :type name: str\n"
    "   :return: The location of the uniform block variable.\n"
    "   :rtype: int\n");
static PyObject *pygpu_shader_uniform_block_from_name(BPyGPUShader *self, PyObject *arg)
{
  const char *name = PyUnicode_AsUTF8(arg);
  if (name == NULL) {
    return NULL;
  }

  const int uniform = GPU_shader_get_uniform_block(self->shader, name);

  if (uniform == -1) {
    PyErr_Format(PyExc_ValueError, "GPUShader.get_uniform_block: uniform %.32s not found", name);
    return NULL;
  }

  return PyLong_FromLong(uniform);
}

static bool pygpu_shader_uniform_vector_impl(PyObject *args,
                                             int elem_size,
                                             int *r_location,
                                             int *r_length,
                                             int *r_count,
                                             Py_buffer *r_pybuffer)
{
  PyObject *buffer;

  *r_count = 1;
  if (!PyArg_ParseTuple(
          args, "iOi|i:GPUShader.uniform_vector_*", r_location, &buffer, r_length, r_count)) {
    return false;
  }

  if (PyObject_GetBuffer(buffer, r_pybuffer, PyBUF_SIMPLE) == -1) {
    /* PyObject_GetBuffer raise a PyExc_BufferError */
    return false;
  }

  if (r_pybuffer->len < (*r_length * *r_count * elem_size)) {
    PyErr_SetString(PyExc_OverflowError,
                    "GPUShader.uniform_vector_*: buffer size smaller than required.");
    return false;
  }

  return true;
}

PyDoc_STRVAR(pygpu_shader_uniform_vector_float_doc,
             ".. method:: uniform_vector_float(location, buffer, length, count)\n"
             "\n"
             "   Set the buffer to fill the uniform.\n"
             "\n"
             "   :param location: Location of the uniform variable to be modified.\n"
             "   :type location: int\n"
             "   :param buffer:  The data that should be set. Can support the buffer protocol.\n"
             "   :type buffer: sequence of floats\n"
             "   :param length: Size of the uniform data type:\n\n"
             "      - 1: float\n"
             "      - 2: vec2 or float[2]\n"
             "      - 3: vec3 or float[3]\n"
             "      - 4: vec4 or float[4]\n"
             "      - 9: mat3\n"
             "      - 16: mat4\n"
             "   :type length: int\n"
             "   :param count: Specifies the number of elements, vector or matrices that are to "
             "be modified.\n"
             "   :type count: int\n");
static PyObject *pygpu_shader_uniform_vector_float(BPyGPUShader *self, PyObject *args)
{
  int location, length, count;

  Py_buffer pybuffer;

  if (!pygpu_shader_uniform_vector_impl(
          args, sizeof(float), &location, &length, &count, &pybuffer)) {
    return NULL;
  }

  GPU_shader_uniform_vector(self->shader, location, length, count, pybuffer.buf);

  PyBuffer_Release(&pybuffer);

  Py_RETURN_NONE;
}

PyDoc_STRVAR(pygpu_shader_uniform_vector_int_doc,
             ".. method:: uniform_vector_int(location, buffer, length, count)\n"
             "\n"
             "   See GPUShader.uniform_vector_float(...) description.\n");
static PyObject *pygpu_shader_uniform_vector_int(BPyGPUShader *self, PyObject *args)
{
  int location, length, count;

  Py_buffer pybuffer;

  if (!pygpu_shader_uniform_vector_impl(
          args, sizeof(int), &location, &length, &count, &pybuffer)) {
    return NULL;
  }

  GPU_shader_uniform_vector_int(self->shader, location, length, count, pybuffer.buf);

  PyBuffer_Release(&pybuffer);

  Py_RETURN_NONE;
}

PyDoc_STRVAR(pygpu_shader_uniform_bool_doc,
             ".. method:: uniform_bool(name, seq)\n"
             "\n"
             "   Specify the value of a uniform variable for the current program object.\n"
             "\n"
             "   :param name: Name of the uniform variable whose value is to be changed.\n"
             "   :type name: str\n"
             "   :param seq: Value that will be used to update the specified uniform variable.\n"
             "   :type seq: sequence of bools\n");
static PyObject *pygpu_shader_uniform_bool(BPyGPUShader *self, PyObject *args)
{
  const char *error_prefix = "GPUShader.uniform_bool";

  struct {
    const char *id;
    PyObject *seq;
  } params;

  if (!PyArg_ParseTuple(args, "sO:GPUShader.uniform_bool", &params.id, &params.seq)) {
    return NULL;
  }

  int values[4];
  int length;
  int ret;
  {
    PyObject *seq_fast = PySequence_Fast(params.seq, error_prefix);
    if (seq_fast == NULL) {
      PyErr_Format(PyExc_TypeError,
                   "%s: expected a sequence, got %s",
                   error_prefix,
                   Py_TYPE(params.seq)->tp_name);
      ret = -1;
    }
    else {
      length = PySequence_Fast_GET_SIZE(seq_fast);
      if (length == 0 || length > 4) {
        PyErr_Format(PyExc_TypeError,
                     "%s: invalid sequence length. expected 1..4, got %d",
                     error_prefix,
                     length);
        ret = -1;
      }
      else {
        ret = PyC_AsArray_FAST(
            values, sizeof(*values), seq_fast, length, &PyLong_Type, error_prefix);
      }
      Py_DECREF(seq_fast);
    }
  }
  if (ret == -1) {
    return NULL;
  }

  const int location = pygpu_shader_uniform_location_get(self->shader, params.id, error_prefix);

  if (location == -1) {
    return NULL;
  }

  GPU_shader_uniform_vector_int(self->shader, location, length, 1, values);

  Py_RETURN_NONE;
}

PyDoc_STRVAR(pygpu_shader_uniform_float_doc,
             ".. method:: uniform_float(name, value)\n"
             "\n"
             "   Specify the value of a uniform variable for the current program object.\n"
             "\n"
             "   :param name: Name of the uniform variable whose value is to be changed.\n"
             "   :type name: str\n"
             "   :param value: Value that will be used to update the specified uniform variable.\n"
             "   :type value: single number or sequence of numbers\n");
static PyObject *pygpu_shader_uniform_float(BPyGPUShader *self, PyObject *args)
{
  const char *error_prefix = "GPUShader.uniform_float";

  struct {
    const char *id;
    PyObject *seq;
  } params;

  if (!PyArg_ParseTuple(args, "sO:GPUShader.uniform_float", &params.id, &params.seq)) {
    return NULL;
  }

  float values[16];
  int length;

  if (PyFloat_Check(params.seq)) {
    values[0] = (float)PyFloat_AsDouble(params.seq);
    length = 1;
  }
  else if (PyLong_Check(params.seq)) {
    values[0] = (float)PyLong_AsDouble(params.seq);
    length = 1;
  }
  else if (MatrixObject_Check(params.seq)) {
    MatrixObject *mat = (MatrixObject *)params.seq;
    if (BaseMath_ReadCallback(mat) == -1) {
      return NULL;
    }
    if ((mat->row_num != mat->col_num) || !ELEM(mat->row_num, 3, 4)) {
      PyErr_SetString(PyExc_ValueError, "Expected 3x3 or 4x4 matrix");
      return NULL;
    }
    length = mat->row_num * mat->col_num;
    memcpy(values, mat->matrix, sizeof(float) * length);
  }
  else {
    length = mathutils_array_parse(values, 2, 16, params.seq, "");
    if (length == -1) {
      return NULL;
    }
  }

  if (!ELEM(length, 1, 2, 3, 4, 9, 16)) {
    PyErr_SetString(PyExc_TypeError,
                    "Expected a single float or a sequence of floats of length 1..4, 9 or 16.");
    return NULL;
  }

  const int location = pygpu_shader_uniform_location_get(self->shader, params.id, error_prefix);

  if (location == -1) {
    return NULL;
  }

  GPU_shader_uniform_vector(self->shader, location, length, 1, values);

  Py_RETURN_NONE;
}

PyDoc_STRVAR(pygpu_shader_uniform_int_doc,
             ".. method:: uniform_int(name, seq)\n"
             "\n"
             "   Specify the value of a uniform variable for the current program object.\n"
             "\n"
             "   :param name: name of the uniform variable whose value is to be changed.\n"
             "   :type name: str\n"
             "   :param seq: Value that will be used to update the specified uniform variable.\n"
             "   :type seq: sequence of numbers\n");
static PyObject *pygpu_shader_uniform_int(BPyGPUShader *self, PyObject *args)
{
  const char *error_prefix = "GPUShader.uniform_int";

  struct {
    const char *id;
    PyObject *seq;
  } params;

  if (!PyArg_ParseTuple(args, "sO:GPUShader.uniform_int", &params.id, &params.seq)) {
    return NULL;
  }

  int values[4];
  int length;
  int ret;

  if (PyLong_Check(params.seq)) {
    values[0] = PyC_Long_AsI32(params.seq);
    length = 1;
    ret = 0;
  }
  else {
    PyObject *seq_fast = PySequence_Fast(params.seq, error_prefix);
    if (seq_fast == NULL) {
      PyErr_Format(PyExc_TypeError,
                   "%s: expected a sequence, got %s",
                   error_prefix,
                   Py_TYPE(params.seq)->tp_name);
      ret = -1;
    }
    else {
      length = PySequence_Fast_GET_SIZE(seq_fast);
      if (length == 0 || length > 4) {
        PyErr_Format(PyExc_TypeError,
                     "%s: invalid sequence length. expected 1..4, got %d",
                     error_prefix,
                     length);
        ret = -1;
      }
      else {
        ret = PyC_AsArray_FAST(
            values, sizeof(*values), seq_fast, length, &PyLong_Type, error_prefix);
      }
      Py_DECREF(seq_fast);
    }
  }
  if (ret == -1) {
    return NULL;
  }

  const int location = pygpu_shader_uniform_location_get(self->shader, params.id, error_prefix);

  if (location == -1) {
    return NULL;
  }

  GPU_shader_uniform_vector_int(self->shader, location, length, 1, values);

  Py_RETURN_NONE;
}

PyDoc_STRVAR(pygpu_shader_uniform_sampler_doc,
             ".. method:: uniform_sampler(name, texture)\n"
             "\n"
             "   Specify the value of a texture uniform variable for the current GPUShader.\n"
             "\n"
             "   :param name: name of the uniform variable whose texture is to be specified.\n"
             "   :type name: str\n"
             "   :param texture: Texture to attach.\n"
             "   :type texture: :class:`gpu.types.GPUTexture`\n");
static PyObject *pygpu_shader_uniform_sampler(BPyGPUShader *self, PyObject *args)
{
  const char *name;
  BPyGPUTexture *py_texture;
  if (!PyArg_ParseTuple(
          args, "sO!:GPUShader.uniform_sampler", &name, &BPyGPUTexture_Type, &py_texture)) {
    return NULL;
  }

  int slot = GPU_shader_get_texture_binding(self->shader, name);
  GPU_texture_bind(py_texture->tex, slot);
  GPU_shader_uniform_1i(self->shader, name, slot);

  Py_RETURN_NONE;
}

PyDoc_STRVAR(
    pygpu_shader_uniform_block_doc,
    ".. method:: uniform_block(name, ubo)\n"
    "\n"
    "   Specify the value of an uniform buffer object variable for the current GPUShader.\n"
    "\n"
    "   :param name: name of the uniform variable whose UBO is to be specified.\n"
    "   :type name: str\n"
    "   :param ubo: Uniform Buffer to attach.\n"
    "   :type texture: :class:`gpu.types.GPUUniformBuf`\n");
static PyObject *pygpu_shader_uniform_block(BPyGPUShader *self, PyObject *args)
{
  const char *name;
  BPyGPUUniformBuf *py_ubo;
  if (!PyArg_ParseTuple(
          args, "sO!:GPUShader.uniform_block", &name, &BPyGPUUniformBuf_Type, &py_ubo)) {
    return NULL;
  }

  int binding = GPU_shader_get_uniform_block_binding(self->shader, name);
  if (binding == -1) {
    PyErr_SetString(
        PyExc_BufferError,
        "GPUShader.uniform_block: uniform block not found, make sure the name is correct");
    return NULL;
  }

  GPU_uniformbuf_bind(py_ubo->ubo, binding);

  Py_RETURN_NONE;
}

PyDoc_STRVAR(
    pygpu_shader_attr_from_name_doc,
    ".. method:: attr_from_name(name)\n"
    "\n"
    "   Get attribute location by name.\n"
    "\n"
    "   :param name: The name of the attribute variable whose location is to be queried.\n"
    "   :type name: str\n"
    "   :return: The location of an attribute variable.\n"
    "   :rtype: int\n");
static PyObject *pygpu_shader_attr_from_name(BPyGPUShader *self, PyObject *arg)
{
  const char *name = PyUnicode_AsUTF8(arg);
  if (name == NULL) {
    return NULL;
  }

  const int attr = GPU_shader_get_attribute(self->shader, name);

  if (attr == -1) {
    PyErr_Format(PyExc_ValueError, "GPUShader.attr_from_name: attribute %.32s not found", name);
    return NULL;
  }

  return PyLong_FromLong(attr);
}

PyDoc_STRVAR(pygpu_shader_calc_format_doc,
             ".. method:: calc_format()\n"
             "\n"
             "   Build a new format based on the attributes of the shader.\n"
             "\n"
             "   :return: vertex attribute format for the shader\n"
             "   :rtype: :class:`gpu.types.GPUVertFormat`\n");
static PyObject *pygpu_shader_calc_format(BPyGPUShader *self, PyObject *UNUSED(arg))
{
  BPyGPUVertFormat *ret = (BPyGPUVertFormat *)BPyGPUVertFormat_CreatePyObject(NULL);
  GPU_vertformat_from_shader(&ret->fmt, self->shader);
  return (PyObject *)ret;
}

static struct PyMethodDef pygpu_shader__tp_methods[] = {
    {"bind", (PyCFunction)pygpu_shader_bind, METH_NOARGS, pygpu_shader_bind_doc},
    {"uniform_from_name",
     (PyCFunction)pygpu_shader_uniform_from_name,
     METH_O,
     pygpu_shader_uniform_from_name_doc},
    {"uniform_block_from_name",
     (PyCFunction)pygpu_shader_uniform_block_from_name,
     METH_O,
     pygpu_shader_uniform_block_from_name_doc},
    {"uniform_vector_float",
     (PyCFunction)pygpu_shader_uniform_vector_float,
     METH_VARARGS,
     pygpu_shader_uniform_vector_float_doc},
    {"uniform_vector_int",
     (PyCFunction)pygpu_shader_uniform_vector_int,
     METH_VARARGS,
     pygpu_shader_uniform_vector_int_doc},
    {"uniform_bool",
     (PyCFunction)pygpu_shader_uniform_bool,
     METH_VARARGS,
     pygpu_shader_uniform_bool_doc},
    {"uniform_float",
     (PyCFunction)pygpu_shader_uniform_float,
     METH_VARARGS,
     pygpu_shader_uniform_float_doc},
    {"uniform_int",
     (PyCFunction)pygpu_shader_uniform_int,
     METH_VARARGS,
     pygpu_shader_uniform_int_doc},
    {"uniform_sampler",
     (PyCFunction)pygpu_shader_uniform_sampler,
     METH_VARARGS,
     pygpu_shader_uniform_sampler_doc},
    {"uniform_block",
     (PyCFunction)pygpu_shader_uniform_block,
     METH_VARARGS,
     pygpu_shader_uniform_block_doc},
    {"attr_from_name",
     (PyCFunction)pygpu_shader_attr_from_name,
     METH_O,
     pygpu_shader_attr_from_name_doc},
    {"format_calc",
     (PyCFunction)pygpu_shader_calc_format,
     METH_NOARGS,
     pygpu_shader_calc_format_doc},
    {NULL, NULL, 0, NULL},
};

PyDoc_STRVAR(pygpu_shader_name_doc,
             "The name of the shader object for debugging purposes (read-only).\n\n:type: str");
static PyObject *pygpu_shader_name(BPyGPUShader *self)
{
  return PyUnicode_FromString(GPU_shader_get_name(self->shader));
}

PyDoc_STRVAR(
    pygpu_shader_program_doc,
    "The name of the program object for use by the OpenGL API (read-only).\n\n:type: int");
static PyObject *pygpu_shader_program_get(BPyGPUShader *self, void *UNUSED(closure))
{
  return PyLong_FromLong(GPU_shader_get_program(self->shader));
}

static PyGetSetDef pygpu_shader__tp_getseters[] = {
    {"program", (getter)pygpu_shader_program_get, (setter)NULL, pygpu_shader_program_doc, NULL},
    {"name", (getter)pygpu_shader_name, (setter)NULL, pygpu_shader_name_doc, NULL},
    {NULL, NULL, NULL, NULL, NULL} /* Sentinel */
};

static void pygpu_shader__tp_dealloc(BPyGPUShader *self)
{
  if (self->is_builtin == false) {
    GPU_shader_free(self->shader);
  }
  Py_TYPE(self)->tp_free((PyObject *)self);
}

PyDoc_STRVAR(
    pygpu_shader__tp_doc,
    ".. class:: GPUShader(vertexcode, fragcode, geocode=None, libcode=None, defines=None, "
    "name='pyGPUShader')\n"
    "\n"
    "   GPUShader combines multiple GLSL shaders into a program used for drawing.\n"
    "   It must contain at least a vertex and fragment shaders.\n"
    "\n"
    "   The GLSL ``#version`` directive is automatically included at the top of shaders,\n"
    "   and set to 330. Some preprocessor directives are automatically added according to\n"
    "   the Operating System or availability: ``GPU_ATI``, ``GPU_NVIDIA`` and ``GPU_INTEL``.\n"
    "\n"
    "   The following extensions are enabled by default if supported by the GPU:\n"
    "   ``GL_ARB_texture_gather``, ``GL_ARB_texture_cube_map_array``\n"
    "   and ``GL_ARB_shader_draw_parameters``.\n"
    "\n"
    "   For drawing user interface elements and gizmos, use\n"
    "   ``fragOutput = blender_srgb_to_framebuffer_space(fragOutput)``\n"
    "   to transform the output sRGB colors to the frame-buffer color-space.\n"
    "\n"
    "   :param vertexcode: Vertex shader code.\n"
    "   :type vertexcode: str\n"
    "   :param fragcode: Fragment shader code.\n"
    "   :type value: str\n"
    "   :param geocode: Geometry shader code.\n"
    "   :type value: str\n"
    "   :param libcode: Code with functions and presets to be shared between shaders.\n"
    "   :type value: str\n"
    "   :param defines: Preprocessor directives.\n"
    "   :type value: str\n"
    "   :param name: Name of shader code, for debugging purposes.\n"
    "   :type value: str\n");
PyTypeObject BPyGPUShader_Type = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "GPUShader",
    .tp_basicsize = sizeof(BPyGPUShader),
    .tp_dealloc = (destructor)pygpu_shader__tp_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = pygpu_shader__tp_doc,
    .tp_methods = pygpu_shader__tp_methods,
    .tp_getset = pygpu_shader__tp_getseters,
    .tp_new = pygpu_shader__tp_new,
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name gpu.shader Module API
 * \{ */

PyDoc_STRVAR(pygpu_shader_unbind_doc,
             ".. function:: unbind()\n"
             "\n"
             "   Unbind the bound shader object.\n");
static PyObject *pygpu_shader_unbind(BPyGPUShader *UNUSED(self))
{
  GPU_shader_unbind();
  Py_RETURN_NONE;
}

PyDoc_STRVAR(
    pygpu_shader_from_builtin_doc,
    ".. function:: from_builtin(shader_name, config='DEFAULT')\n"
    "\n"
    "   Shaders that are embedded in the blender internal code (see :ref:`built-in-shaders`).\n"
    "   They all read the uniform ``mat4 ModelViewProjectionMatrix``,\n"
    "   which can be edited by the :mod:`gpu.matrix` module.\n"
    "\n"
    "   You can also choose a shader configuration that uses clip_planes by setting the "
    "``CLIPPED`` value to the config parameter. Note that in this case you also need to "
    "manually set the value of ``mat4 ModelMatrix``.\n"
    "\n"
    "   :param shader_name: One of the builtin shader names.\n"
    "   :type shader_name: str\n"
    "   :param config: One of these types of shader configuration:\n"
    "\n"
    "      - ``DEFAULT``\n"
    "      - ``CLIPPED``\n"
    "   :type config: str\n"
    "   :return: Shader object corresponding to the given name.\n"
    "   :rtype: :class:`bpy.types.GPUShader`\n");
static PyObject *pygpu_shader_from_builtin(PyObject *UNUSED(self), PyObject *args, PyObject *kwds)
{
  BPYGPU_IS_INIT_OR_ERROR_OBJ;

  struct PyC_StringEnum pygpu_bultinshader = {pygpu_shader_builtin_items};
  struct PyC_StringEnum pygpu_config = {pygpu_shader_config_items, GPU_SHADER_CFG_DEFAULT};

  static const char *_keywords[] = {"shader_name", "config", NULL};
  static _PyArg_Parser _parser = {
      "O&" /* `shader_name` */
      "|$" /* Optional keyword only arguments. */
      "O&" /* `config` */
      ":from_builtin",
      _keywords,
      0,
  };
  if (!_PyArg_ParseTupleAndKeywordsFast(args,
                                        kwds,
                                        &_parser,
                                        PyC_ParseStringEnum,
                                        &pygpu_bultinshader,
                                        PyC_ParseStringEnum,
                                        &pygpu_config)) {
    return NULL;
  }

  GPUShader *shader = GPU_shader_get_builtin_shader_with_config(pygpu_bultinshader.value_found,
                                                                pygpu_config.value_found);

  return BPyGPUShader_CreatePyObject(shader, true);
}

PyDoc_STRVAR(pygpu_shader_create_from_info_doc,
             ".. function:: create_from_info(shader_info)\n"
             "\n"
             "   Create shader from a GPUShaderCreateInfo.\n"
             "\n"
             "   :param shader_info: GPUShaderCreateInfo\n"
             "   :type shader_info: :class:`bpy.types.GPUShaderCreateInfo`\n"
             "   :return: Shader object corresponding to the given name.\n"
             "   :rtype: :class:`bpy.types.GPUShader`\n");
static PyObject *pygpu_shader_create_from_info(BPyGPUShader *UNUSED(self),
                                               BPyGPUShaderCreateInfo *o)
{
  if (!BPyGPUShaderCreateInfo_Check(o)) {
    PyErr_Format(PyExc_TypeError, "Expected a GPUShaderCreateInfo, got %s", Py_TYPE(o)->tp_name);
    return NULL;
  }

  char error[128];
  if (!GPU_shader_create_info_check_error(o->info, error)) {
    PyErr_SetString(PyExc_Exception, error);
    return NULL;
  }

  GPUShader *shader = GPU_shader_create_from_info(o->info);
  if (!shader) {
    PyErr_SetString(PyExc_Exception, "Shader Compile Error, see console for more details");
    return NULL;
  }

  return BPyGPUShader_CreatePyObject(shader, false);
}

static struct PyMethodDef pygpu_shader_module__tp_methods[] = {
    {"unbind", (PyCFunction)pygpu_shader_unbind, METH_NOARGS, pygpu_shader_unbind_doc},
    {"from_builtin",
     (PyCFunction)pygpu_shader_from_builtin,
     METH_VARARGS | METH_KEYWORDS,
     pygpu_shader_from_builtin_doc},
    {"create_from_info",
     (PyCFunction)pygpu_shader_create_from_info,
     METH_O,
     pygpu_shader_create_from_info_doc},
    {NULL, NULL, 0, NULL},
};

PyDoc_STRVAR(pygpu_shader_module__tp_doc,
             "This module provides access to GPUShader internal functions.\n"
             "\n"
             ".. _built-in-shaders:\n"
             "\n"
             ".. rubric:: Built-in shaders\n"
             "\n"
             "All built-in shaders have the ``mat4 ModelViewProjectionMatrix`` uniform.\n"
             "\n"
             "Its value must be modified using the :class:`gpu.matrix` module.\n"
             "\n" PYDOC_BUILTIN_SHADER_DESCRIPTION);
static PyModuleDef pygpu_shader_module_def = {
    PyModuleDef_HEAD_INIT,
    .m_name = "gpu.shader",
    .m_doc = pygpu_shader_module__tp_doc,
    .m_methods = pygpu_shader_module__tp_methods,
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Public API
 * \{ */

PyObject *BPyGPUShader_CreatePyObject(GPUShader *shader, bool is_builtin)
{
  BPyGPUShader *self;

  self = PyObject_New(BPyGPUShader, &BPyGPUShader_Type);
  self->shader = shader;
  self->is_builtin = is_builtin;

  return (PyObject *)self;
}

PyObject *bpygpu_shader_init(void)
{
  PyObject *submodule;

  submodule = PyModule_Create(&pygpu_shader_module_def);

  return submodule;
}

/** \} */
