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
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/Ketsji/BL_Texture.cpp
 *  \ingroup ketsji
 */

#include "BKE_image.hh"

#include "BL_Texture.h"

#include "GPU_material.hh"
#include "GPU_state.hh"

BL_Texture::BL_Texture(GPUMaterialTexture *gpumattex, eGPUTextureTarget textarget)
    : EXP_Value(),
      m_isCubeMap(false),
      m_gpuMatTex(gpumattex),
      m_textarget(textarget),
      m_bindCode(-1)
{
  /* Normally input->textype is Kept in sync with GPU_DATATYPE_STR */
  m_isCubeMap = false; /*(m_gpuTex->type == GPU_TEXCUBE)*/
  m_name = m_gpuMatTex->ima->id.name;

  ImageUser *iuser = m_gpuMatTex->iuser_available ? &m_gpuMatTex->iuser : NULL;
  m_gpuTex = BKE_image_get_gpu_texture(m_gpuMatTex->ima, iuser);

  if (m_gpuTex) {
    m_bindCode = GPU_texture_opengl_bindcode(m_gpuTex);
    m_savedData.bindcode = m_bindCode;
    GPU_texture_ref(m_gpuTex);
  }
}

BL_Texture::~BL_Texture()
{
  if (m_gpuTex) {
    GPU_texture_set_opengl_bindcode(m_gpuTex, m_savedData.bindcode);
    GPU_texture_free(m_gpuTex);
  }
}

void BL_Texture::CheckValidTexture()
{
  if (!m_gpuTex) {
    return;
  }

  /* Test if the gpu texture is the same in the image which own it, if it's not
   * the case then it means that no materials use it anymore and that we have to
   * get a pointer of the updated gpu texture used by materials.
   * The gpu texture in the image can be nullptr or an already different loaded
   * gpu texture. In both cases we call GPU_texture_from_blender.
   */
  int target = TEXTARGET_2D;
  GPUTexture *tex = m_gpuMatTex->ima->gputexture[target][0];
  if (m_gpuTex != tex) {
    // Restore gpu texture original bind cdoe to make sure we will delete the right opengl texture.
    GPU_texture_set_opengl_bindcode(m_gpuTex, m_savedData.bindcode);
    GPU_texture_free(m_gpuTex);
    ImageUser *iuser = m_gpuMatTex->iuser_available ? &m_gpuMatTex->iuser : NULL;
    m_gpuTex = (m_gpuMatTex->ima ? BKE_image_get_gpu_texture(m_gpuMatTex->ima, iuser) :
                    nullptr);

    if (m_gpuTex) {
      int bindCode = GPU_texture_opengl_bindcode(m_gpuTex);
      // If our bind code was the same as the previous gpu texture bind code, then we update it to
      // the new bind code.
      if (m_bindCode == m_savedData.bindcode) {
        m_bindCode = bindCode;
      }
      m_savedData.bindcode = bindCode;
      GPU_texture_ref(m_gpuTex);
    }
  }
}

bool BL_Texture::Ok() const
{
  return (m_gpuTex != nullptr);
}

bool BL_Texture::IsCubeMap() const
{
  return m_isCubeMap;
}

Image *BL_Texture::GetImage() const
{
  return m_gpuMatTex->ima;
}

GPUTexture *BL_Texture::GetGPUTexture() const
{
  return m_gpuTex;
}

unsigned int BL_Texture::GetTextureType()
{
  return m_textarget; /*m_isCubeMap ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;*/
}

void BL_Texture::ActivateTexture(int unit)
{
  /* Since GPUTexture can be shared between material textures (MTex),
   * we should reapply the bindcode in case of VideoTexture owned texture.
   * Without that every material that use this GPUTexture will then use
   * the VideoTexture texture, it's not wanted. */
  GPU_texture_set_opengl_bindcode(m_gpuTex, m_bindCode);
  GPU_texture_bind(m_gpuTex, unit);
  GPU_apply_state();
}

void BL_Texture::DisableTexture()
{
  GPU_texture_unbind(m_gpuTex);
}

int BL_Texture::GetBindCode() const
{
  return m_bindCode;
}

void BL_Texture::SetBindCode(int bindcode)
{
  GPU_texture_set_opengl_bindcode(m_gpuTex, bindcode);
  m_bindCode = bindcode;
}

// stuff for cvalue related things
std::string BL_Texture::GetName()
{
  return RAS_Texture::GetName();
}

#ifdef WITH_PYTHON

PyTypeObject BL_Texture::Type = {PyVarObject_HEAD_INIT(nullptr, 0) "BL_Texture",
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
                                 &EXP_Value::Type,
                                 0,
                                 0,
                                 0,
                                 0,
                                 0,
                                 0,
                                 py_base_new};

PyMethodDef BL_Texture::Methods[] = {
    {nullptr, nullptr}  // Sentinel
};

PyAttributeDef BL_Texture::Attributes[] = {
    EXP_PYATTRIBUTE_RW_FUNCTION(
        "bindCode", BL_Texture, pyattr_get_bind_code, pyattr_set_bind_code),
    EXP_PYATTRIBUTE_NULL  // Sentinel
};

PyObject *BL_Texture::pyattr_get_bind_code(EXP_PyObjectPlus *self_v,
                                           const EXP_PYATTRIBUTE_DEF *attrdef)
{
  BL_Texture *self = static_cast<BL_Texture *>(self_v);
  self->CheckValidTexture();
  return PyLong_FromLong(self->m_bindCode);
}

int BL_Texture::pyattr_set_bind_code(EXP_PyObjectPlus *self_v,
                                     const EXP_PYATTRIBUTE_DEF *attrdef,
                                     PyObject *value)
{
  BL_Texture *self = static_cast<BL_Texture *>(self_v);
  int val = PyLong_AsLong(value);

  if (val < 0 && PyErr_Occurred()) {
    PyErr_Format(PyExc_AttributeError,
                 "texture.%s = int: BL_Texture, expected a unsigned int",
                 attrdef->m_name.c_str());
    return PY_SET_ATTR_FAIL;
  }

  self->SetBindCode(val);
  return PY_SET_ATTR_SUCCESS;
}

#endif  // WITH_PYTHON
