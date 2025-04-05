/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Contributor(s): Ulysse Martin, Tristan Porteries.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/Ketsji/KX_2DFilter.cpp
 *  \ingroup ketsji
 */

#include "KX_2DFilter.h"

#include "../python/gpu/gpu_py_texture.hh"

#include "KX_2DFilterFrameBuffer.h"

KX_2DFilter::KX_2DFilter(RAS_2DFilterData &data) : RAS_2DFilter(data)
{
}

KX_2DFilter::~KX_2DFilter()
{
}

bool KX_2DFilter::LinkProgram()
{
  return RAS_2DFilter::LinkProgram();
}

#ifdef WITH_PYTHON

bool KX_2DFilter::SetTextureUniform(BPyGPUTexture *py_texture, const char *samplerName)
{
  if (GetError()) {
    return false;
  }
  if (!py_texture) {
    PyErr_Format(PyExc_ValueError,
                 "KX_2DFilter, no valid GPUTexture found");
    return false;
  }
  GPU_shader_bind(m_shader);
  int slot = GPU_shader_get_sampler_binding(m_shader, samplerName);
  GPU_texture_bind(py_texture->tex, slot);
  GPU_shader_uniform_1i(m_shader, samplerName, slot);

  return true;
}

PyTypeObject KX_2DFilter::Type = {PyVarObject_HEAD_INIT(nullptr, 0) "KX_2DFilter",
                                  sizeof(EXP_PyObjectPlus_Proxy),
                                  0,
                                  py_base_dealloc,
                                  0,
                                  0,
                                  0,
                                  0,
                                  py_base_repr,
                                  0,
                                  0,
                                  0,
                                  0,
                                  0,
                                  0,
                                  0,
                                  0,
                                  0,
                                  Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
                                  0,
                                  0,
                                  0,
                                  0,
                                  0,
                                  0,
                                  0,
                                  Methods,
                                  0,
                                  0,
                                  &BL_Shader::Type,
                                  0,
                                  0,
                                  0,
                                  0,
                                  0,
                                  0,
                                  py_base_new};

PyMethodDef KX_2DFilter::Methods[] = {
    EXP_PYMETHODTABLE(KX_2DFilter, setTexture),
    EXP_PYMETHODTABLE_KEYWORDS(KX_2DFilter, addOffScreen),
    EXP_PYMETHODTABLE_NOARGS(KX_2DFilter, removeOffScreen),
    {nullptr, nullptr}  // Sentinel
};

PyAttributeDef KX_2DFilter::Attributes[] = {
    EXP_PYATTRIBUTE_RW_FUNCTION("mipmap", KX_2DFilter, pyattr_get_mipmap, pyattr_set_mipmap),
    EXP_PYATTRIBUTE_RO_FUNCTION("frameBuffer", KX_2DFilter, pyattr_get_frameBuffer),
    EXP_PYATTRIBUTE_RO_FUNCTION(
        "offScreen",
        KX_2DFilter,
        pyattr_get_frameBuffer),  // Keep offScreen name for background compatibility
    EXP_PYATTRIBUTE_NULL          // Sentinel
};

PyObject *KX_2DFilter::pyattr_get_mipmap(EXP_PyObjectPlus *self_v,
                                         const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_2DFilter *self = static_cast<KX_2DFilter *>(self_v);
  return PyBool_FromLong(self->GetMipmap());
}

int KX_2DFilter::pyattr_set_mipmap(EXP_PyObjectPlus *self_v,
                                   const EXP_PYATTRIBUTE_DEF *attrdef,
                                   PyObject *value)
{
  KX_2DFilter *self = static_cast<KX_2DFilter *>(self_v);
  int param = PyObject_IsTrue(value);
  if (param == -1) {
    PyErr_SetString(PyExc_AttributeError,
                    "shader.enabled = bool: BL_Shader, expected True or False");
    return PY_SET_ATTR_FAIL;
  }

  self->SetMipmap(param);
  return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_2DFilter::pyattr_get_frameBuffer(EXP_PyObjectPlus *self_v,
                                              const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_2DFilter *self = static_cast<KX_2DFilter *>(self_v);
  RAS_2DFilterFrameBuffer *frameBuffer = self->GetFrameBuffer();
  return frameBuffer ? static_cast<KX_2DFilterFrameBuffer *>(frameBuffer)->GetProxy() : Py_None;
}

EXP_PYMETHODDEF_DOC(KX_2DFilter, setTexture, "setTexture(samplerName, gputexture)")
{
  BPyGPUTexture *py_texture;
  char *samplerName = nullptr;

  if (!PyArg_ParseTuple(args, "sO!:setTexture", &samplerName, &BPyGPUTexture_Type, &py_texture)) {
    return nullptr;
  }

  if (!SetTextureUniform(py_texture, samplerName)) {
    return nullptr;
  }

  Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(KX_2DFilter, addOffScreen, " addOffScreen(slots, width, height, mipmap)")
{
  int slots;
  int width = -1;
  int height = -1;
  int mipmap = 0;
  int flag = 0;
  flag |=
      RAS_2DFilterFrameBuffer::RAS_VIEWPORT_SIZE;  // tmp: not viewport size not supported for now

  static const char *kwlist[] = {"slots", "width", "height", "mipmap", nullptr};

  if (!PyArg_ParseTupleAndKeywords(args,
                                   kwds,
                                   "i|iii:addOffScreen",
                                   const_cast<char **>(kwlist),
                                   &slots,
                                   &width,
                                   &height,
                                   &mipmap)) {
    return nullptr;
  }

  if (GetFrameBuffer()) {
    PyErr_SetString(PyExc_TypeError,
                    "filter.addOffScreen(...): KX_2DFilter, custom off screen already exists.");
    return nullptr;
  }

  if (slots < 0 || slots >= 8) {
    PyErr_SetString(
        PyExc_TypeError,
        "filter.addOffScreen(...): KX_2DFilter, slots must be between 0 and 8 excluded.");
    return nullptr;
  }

  if (width < -1 || height < -1 || width == 0 || height == 0) {
    PyErr_SetString(PyExc_TypeError,
                    "filter.addOffScreen(...): KX_2DFilter, invalid size values.");
    return nullptr;
  }

  if (width == -1 || height == -1) {
    flag |= RAS_2DFilterFrameBuffer::RAS_VIEWPORT_SIZE;
  }

  if (mipmap) {
    flag |= RAS_2DFilterFrameBuffer::RAS_MIPMAP;
  }

  KX_2DFilterFrameBuffer *kxFrameBuffer = new KX_2DFilterFrameBuffer(
      slots, (RAS_2DFilterFrameBuffer::Flag)flag, width, height);

  SetOffScreen(kxFrameBuffer);

  return kxFrameBuffer->GetProxy();
}

EXP_PYMETHODDEF_DOC_NOARGS(KX_2DFilter, removeOffScreen, " removeOffScreen()")
{
  SetOffScreen(nullptr);
  Py_RETURN_NONE;
}

#endif  // WITH_PYTHON
