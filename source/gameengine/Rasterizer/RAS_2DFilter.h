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
* Contributor(s): Pierluigi Grassi, Porteries Tristan.
*
* ***** END GPL LICENSE BLOCK *****
*/

#ifndef __RAS_2DFILTER_H__
#define __RAS_2DFILTER_H__

#include "RAS_2DFilterData.h"
#include "RAS_Shader.h"

#include <memory>
#include <unordered_map>

class RAS_2DFilterManager;
class RAS_Rasterizer;
class RAS_ICanvas;
class RAS_OffScreen;
class RAS_2DFilterOffScreen;
class EXP_Value;

class RAS_2DFilter : public virtual RAS_Shader
{
public:
	enum PredefinedUniformType {
		RENDERED_TEXTURE_UNIFORM = 0,
		DATA_TEXTURES_UNIFORM,
		DEPTH_TEXTURE_UNIFORM,
		RENDERED_TEXTURE_WIDTH_UNIFORM,
		RENDERED_TEXTURE_HEIGHT_UNIFORM,
		TEXTURE_COORDINATE_OFFSETS_UNIFORM,
		MAX_PREDEFINED_UNIFORM_TYPE
	};

protected:
	int m_predefinedUniforms[MAX_PREDEFINED_UNIFORM_TYPE];

	std::vector<std::string> m_properties;
	std::vector<unsigned int> m_propertiesLoc;
	EXP_Value *m_gameObject;

	/// True if the uniform locations are updated with the current shader program/script.
	bool m_uniformInitialized;
	/// True if generate mipmap of input color texture.
	bool m_mipmap;

	/** A set of vec2 coordinates that the shaders use to sample nearby pixels from incoming textures.
	The computation should be left to the glsl shader, I keep it for backward compatibility. */
	static const int TEXTURE_OFFSETS_SIZE = 18; //9 vec2 entries
	float m_textureOffsets[TEXTURE_OFFSETS_SIZE];

	std::unordered_map<unsigned short, std::pair<unsigned int, int> > m_textures;

	/// Custom off screen for special datas.
	std::unique_ptr<RAS_2DFilterOffScreen> m_offScreen;

	virtual bool LinkProgram();
	void ParseShaderProgram();
	void BindUniforms(RAS_ICanvas *canvas);
	void BindTextures(RAS_OffScreen *detphofs, RAS_OffScreen *colorofs);
	void UnbindTextures(RAS_OffScreen *detphofs, RAS_OffScreen *colorofs);
	void ComputeTextureOffsets(RAS_ICanvas *canvas);

public:
	RAS_2DFilter(RAS_2DFilterData& data);
	virtual ~RAS_2DFilter();

	bool GetMipmap() const;
	void SetMipmap(bool mipmap);

	RAS_2DFilterOffScreen *GetOffScreen() const;
	void SetOffScreen(RAS_2DFilterOffScreen *offScreen);

	/// Called by the filter manager when it has informations like the display size, a gl context...
	void Initialize(RAS_ICanvas *canvas);

	/** Render the filter.
	 * \param rasty The used rasterizer to call draw commands.
	 * \param canvas The canvas containing screen viewport.
	 * \param detphofs The off screen used only for the depth texture input,
	 * the same for all filters of a scene.
	 * \param colorofs The off screen used only for the color texture input, unique per filters.
	 * \param targetofs The off screen used to draw the filter to.
	 * \return The off screen to use as input for the next filter.
	 */
	RAS_OffScreen *Render(RAS_Rasterizer *rasty, RAS_ICanvas *canvas, RAS_OffScreen *detphofs,
			   RAS_OffScreen *colorofs, RAS_OffScreen *targetofs);
};

#endif // __RAS_2DFILTER_H__

