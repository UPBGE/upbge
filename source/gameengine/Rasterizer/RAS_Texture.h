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

/** \file RAS_Texture.h
 *  \ingroup ketsji
 */

#pragma once

#include <array>
#include <string>

struct Image;
struct GPUTexture;

class RAS_Texture {
 protected:
  std::string m_name;

 public:
  RAS_Texture();
  virtual ~RAS_Texture();

  virtual bool Ok() const = 0;
  virtual bool IsCubeMap() const = 0;

  virtual Image *GetImage() const = 0;
  virtual GPUTexture *GetGPUTexture() const = 0;
  std::string &GetName();

  virtual unsigned int GetTextureType() = 0;

  /// Return GL_TEXTURE_2D.
  static int GetCubeMapTextureType();
  /// Return GL_TEXTURE_CUBE_MAP.
  static int GetTexture2DType();
  /// Return all the OpenGL cube map face target, e.g GL_TEXTURE_CUBE_MAP_POSITIVE_Z.
  static const std::array<int, 6> &GetCubeMapTargets();

  enum { MaxUnits = 32 };

  virtual void CheckValidTexture() = 0;
  virtual void ActivateTexture(int unit) = 0;
  virtual void DisableTexture() = 0;

  virtual int GetBindCode() const = 0;
  virtual void SetBindCode(int bindcode) = 0;

  /** Set the current active OpenGL texture to the first texture
   * and bind a null texture in this slot.
   * This function must be used very carfully, normally only after
   * that the user played with glActiveTexture and to make sure that
   * it will not break the render.
   * Only the first slot is affected all texture in greater slot are
   * not affected but just unused as default.
   */
  static void DesactiveTextures();
};
