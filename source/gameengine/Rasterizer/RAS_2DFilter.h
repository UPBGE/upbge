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

class RAS_2DFilter : public RAS_Shader
{
public:
	enum PredefinedUniformType {
		RENDERED_TEXTURE_UNIFORM = 0,
		LUMINANCE_TEXTURE_UNIFORM,
		DEPTH_TEXTURE_UNIFORM,
		RENDERED_TEXTURE_WIDTH_UNIFORM,
		RENDERED_TEXTURE_HEIGHT_UNIFORM,
		TEXTURE_COORDINATE_OFFSETS_UNIFORM,
		MAX_PREDEFINED_UNIFORM_TYPE
	};

	enum RenderedTextureType {
		RENDERED_TEXTURE = 0,
		LUMINANCE_TEXTURE,
		DEPTH_TEXTURE,
		MAX_RENDERED_TEXTURE_TYPE
	};

private:
	int m_predefinedUniforms[MAX_PREDEFINED_UNIFORM_TYPE];
	unsigned int m_renderedTextures[MAX_RENDERED_TEXTURE_TYPE];

	std::vector<STR_String> m_properties;
	std::vector<unsigned int> m_propertiesLoc;
	CValue *m_gameObject;

	/** A set of vec2 coordinates that the shaders use to sample nearby pixels from incoming textures.
	The computation should be left to the glsl shader, I keep it for backward compatibility. */
	static const int TEXTURE_OFFSETS_SIZE = 18; //9 vec2 entries
	float m_textureOffsets[TEXTURE_OFFSETS_SIZE]; 
	int m_passIndex;

	void ParseShaderProgram();
	void InitializeTextures(RAS_ICanvas *canvas);
	void BindUniforms(RAS_ICanvas *canvas);
	void DrawOverlayPlane(RAS_ICanvas *canvas);
	void ComputeTextureOffsets(RAS_ICanvas *canvas);
	void ReleaseTextures();

public:
	RAS_2DFilter(RAS_2DFilterData& data);
	virtual ~RAS_2DFilter();

	/// Called by the filter manager when it has informations like the display size, a gl context...
	void Initialize(RAS_ICanvas *canvas);

	/// Starts executing the filter.
	void Start(RAS_IRasterizer *rasty, RAS_ICanvas *canvas);

	/// Finalizes the execution stage of the filter.
	void End();

	/// The pass index determines the precedence of this filter over other filters in the same context.
	int GetPassIndex();

	/// Enables / disables this filter. A disabled filter has no effect on the rendering.
	void SetEnabled(bool enabled);
};

#endif // __RAS_2DFILTER_H__

