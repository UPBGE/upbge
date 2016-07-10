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

class RAS_2DFilterManager;
class RAS_IRasterizer;
class RAS_ICanvas;
class CValue;

class RAS_2DFilter : public virtual RAS_Shader
{
public:
	enum PredefinedUniformType {
		RENDERED_TEXTURE_UNIFORM = 0,
		DEPTH_TEXTURE_UNIFORM,
		RENDERED_TEXTURE_WIDTH_UNIFORM,
		RENDERED_TEXTURE_HEIGHT_UNIFORM,
		TEXTURE_COORDINATE_OFFSETS_UNIFORM,
		MAX_PREDEFINED_UNIFORM_TYPE
	};

protected:
	int m_predefinedUniforms[MAX_PREDEFINED_UNIFORM_TYPE];

	std::vector<STR_String> m_properties;
	std::vector<unsigned int> m_propertiesLoc;
	CValue *m_gameObject;

	/// True if the uniform locations are updated with the current shader program/script.
	bool m_uniformInitialized;

	/** A set of vec2 coordinates that the shaders use to sample nearby pixels from incoming textures.
	The computation should be left to the glsl shader, I keep it for backward compatibility. */
	static const int TEXTURE_OFFSETS_SIZE = 18; //9 vec2 entries
	float m_textureOffsets[TEXTURE_OFFSETS_SIZE];

	unsigned int m_textures[8];

	virtual bool LinkProgram();
	void ParseShaderProgram();
	void BindUniforms(RAS_ICanvas *canvas);
	void BindTextures(RAS_IRasterizer *rasty, unsigned short depthfbo, unsigned short colorfbo);
	void UnbindTextures(RAS_IRasterizer *rasty, unsigned short depthfbo, unsigned short colorfbo);
	void ComputeTextureOffsets(RAS_ICanvas *canvas);

public:
	RAS_2DFilter(RAS_2DFilterData& data);
	virtual ~RAS_2DFilter();

	/// Called by the filter manager when it has informations like the display size, a gl context...
	void Initialize(RAS_ICanvas *canvas);

	/** Starts executing the filter.
	 * \param rasty The used rasterizer to call draw commands.
	 * \param canvas The canvas containing screen viewport.
	 * \param depthfbo The off screen used only for the depth texture input, 
	 * the same for all filters of a scene.
	 * \param colorfbo The off screen used only for the color texture input, unique per filters.
	 * \param outputfbo The off screen used to draw the filter to.
	 */
	void Start(RAS_IRasterizer *rasty, RAS_ICanvas *canvas, unsigned short depthfbo,
			   unsigned short colorfbo, unsigned short outputfbo);

	/// Finalizes the execution stage of the filter.
	void End();
};

#endif // __RAS_2DFILTER_H__

