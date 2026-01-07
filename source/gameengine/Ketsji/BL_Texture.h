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

/** \file BL_Texture.h
 *  \ingroup ketsji
 */

#pragma once

#include "DNA_image_types.h"

#include "EXP_Value.h"
#include "RAS_Texture.h"

struct GPUMaterialTexture;
namespace blender::gpu {
class Texture;
}  // namespace blender::gpu


class BL_Texture : public EXP_Value, public RAS_Texture {
  Py_Header private : bool m_isCubeMap;
  blender::Image *m_ima;
  blender::gpu::Texture *m_gpuTex;
  blender::eGPUTextureTarget m_textarget;

 public:
  BL_Texture(blender::Image *ima);
  virtual ~BL_Texture();

  // stuff for cvalue related things
  virtual std::string GetName();

  virtual bool Ok() const;
  virtual bool IsCubeMap() const;

  virtual blender::Image *GetImage() const;
  virtual blender::gpu::Texture *GetGPUTexture() const;

  virtual unsigned int GetTextureType();

  enum { MaxUnits = 32 };

#ifdef WITH_PYTHON

#endif  // WITH_PYTHON
};
