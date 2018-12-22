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

#ifndef __RAS_TEXTURE_H__
#define __RAS_TEXTURE_H__

#include <string>
#include <array>

struct MTex;
struct Tex;
struct Image;
struct GPUTexture;

class RAS_TextureRenderer;

class RAS_Texture
{
protected:
	int m_bindCode;
	std::string m_name;

	RAS_TextureRenderer *m_renderer;

public:
	RAS_Texture();
	virtual ~RAS_Texture();

	virtual bool Ok() const = 0;
	virtual bool IsCubeMap() const = 0;

	virtual MTex *GetMTex() const = 0;
	virtual Tex *GetTex() const = 0;
	virtual Image *GetImage() const = 0;
	virtual GPUTexture *GetGPUTexture() const = 0;
	std::string& GetName();

	void SetRenderer(RAS_TextureRenderer *renderer);
	RAS_TextureRenderer *GetRenderer() const;

	virtual unsigned int GetTextureType() = 0;

	/// Return GL_TEXTURE_2D.
	static int GetCubeMapTextureType();
	/// Return GL_TEXTURE_CUBE_MAP.
	static int GetTexture2DType();
	/// Return all the OpenGL cube map face target, e.g GL_TEXTURE_CUBE_MAP_POSITIVE_Z_ARB.
	static const std::array<int, 6>& GetCubeMapTargets();

	enum {MaxUnits = 8};

	/** Copy renderer (if available) texture bind code to current texture for next binding.
	 * \param viewportIndex The index of the renderer layer to use for bind code.
	 */
	void ApplyRenderer(unsigned short viewportIndex);
	virtual void UpdateBindCode() = 0;
	virtual void CheckValidTexture() = 0;
	virtual void ActivateTexture(int unit) = 0;
	virtual void DisableTexture() = 0;

	/** Set the current active OpenGL texture to the first texture
	 * and bind a null texture in this slot.
	 * This function must be used very carfully, normally only after
	 * that the user played with glActiveTexture and to make sure that
	 * it will not break the render.
	 * Only the first slot is affected all texture in greater slot are
	 * not affected but just unused as default.
	 */
	static void DesactiveTextures();

	int GetBindCode() const;
	void SetBindCode(int bindcode);
};

#endif // __RAS_TEXTURE_H__
