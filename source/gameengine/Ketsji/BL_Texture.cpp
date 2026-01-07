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

#include "GPU_texture.hh"

using namespace blender;

BL_Texture::BL_Texture(blender::Image *ima)
    : EXP_Value(),
      m_isCubeMap(false), m_ima(ima)
{
  m_isCubeMap = false;
  m_name = m_ima->id.name + 2;
  m_gpuTex = nullptr;
  m_textarget = TEXTARGET_2D;
  /* only add support for existing gputextures */
  if (BKE_image_has_opengl_texture(ima)) {
    m_gpuTex = ima->runtime->gputexture[TEXTARGET_2D][0];
  }
}

BL_Texture::~BL_Texture()
{
}

bool BL_Texture::Ok() const
{
  return (m_gpuTex != nullptr);
}

bool BL_Texture::IsCubeMap() const
{
  return m_isCubeMap;
}

blender::Image *BL_Texture::GetImage() const
{
  return m_ima;
}

blender::gpu::Texture *BL_Texture::GetGPUTexture() const
{
  return m_gpuTex;
}

unsigned int BL_Texture::GetTextureType()
{
  return m_textarget;
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
    {nullptr, nullptr}   // Sentinel
};

PyAttributeDef BL_Texture::Attributes[] = {
    EXP_PYATTRIBUTE_NULL // Sentinel
};

#endif  // WITH_PYTHON
