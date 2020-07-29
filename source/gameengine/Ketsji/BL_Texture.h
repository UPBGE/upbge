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

#ifndef __BL_TEXTURE_H__
#define __BL_TEXTURE_H__

#include "GPU_draw.h"

#include "EXP_Value.h"
#include "RAS_Texture.h"

struct GPUMaterialTexture;

class BL_Texture : public CValue, public RAS_Texture {
  Py_Header private : bool m_isCubeMap;
  GPUMaterialTexture *m_gpuMatTex;
  GPUTexture *m_gpuTex;
  eGPUTextureTarget m_textarget;

  struct {
    unsigned int bindcode;
  } m_savedData;

 public:
  BL_Texture(GPUMaterialTexture *gpumattex, eGPUTextureTarget textarget);
  virtual ~BL_Texture();

  // stuff for cvalue related things
  virtual std::string GetName();

  virtual bool Ok() const;
  virtual bool IsCubeMap() const;

  virtual Image *GetImage() const;
  virtual GPUTexture *GetGPUTexture() const;

  virtual unsigned int GetTextureType();

  enum { MaxUnits = 32 };

  virtual void CheckValidTexture();
  virtual void ActivateTexture(int unit);
  virtual void DisableTexture();

#ifdef WITH_PYTHON
  static PyObject *pyattr_get_bind_code(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
  static int pyattr_set_bind_code(PyObjectPlus *self_v,
                                  const KX_PYATTRIBUTE_DEF *attrdef,
                                  PyObject *value);

#endif  // WITH_PYTHON
};

#endif  // __BL_TEXTURE_H__
