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

class RAS_2DFilterManager;
class CValue;

class RAS_2DFilter
{
private:
	/// Names of the predefined values available to glsl shaders
	static const char *UNIFORM_NAME_RENDERED_TEXTURE;
	static const char *UNIFORM_NAME_LUMINANCE_TEXTURE;
	static const char *UNIFORM_NAME_DEPTH_TEXTURE;
	static const char *UNIFORM_NAME_RENDERED_TEXTURE_WIDTH;
	static const char *UNIFORM_NAME_RENDERED_TEXTURE_HEIGHT;
	static const char *UNIFORM_NAME_TEXTURE_COORDINATE_OFFSETS;

	RAS_2DFilterManager *m_manager;

	STR_String m_uid;
	STR_String m_fragmentShaderSourceCode;

	unsigned int m_shaderProgramUid;
	unsigned int m_fragmentShaderUid;
	int m_renderedTextureUniformLocation;
	int m_luminanceTextureUniformLocation;
	int m_depthTextureUniformLocation;
	int m_renderedTextureWidthUniformLocation;
	int m_renderedTextureHeightUniformLocation;
	int m_textureOffsetsUniformLocation;
	unsigned int m_renderedTextureUid;
	unsigned int m_luminanceTextureUid;
	unsigned int m_depthTextureUid;

	std::vector<STR_String> m_properties;
	std::vector<unsigned int> m_propertiesLoc;
	CValue *m_gameObject;

	/** A set of vec2 coordinates that the shaders use to sample nearby pixels from incoming textures.
	The computation should be left to the glsl shader, I keep it for backward compatibility. */
	static const int TEXTURE_OFFSETS_SIZE = 18; //9 vec2 entries
	float m_textureOffsets[TEXTURE_OFFSETS_SIZE]; 
	int m_passIndex;
	bool m_enabled;
	bool m_error;
	bool m_initialized;

	void ParseShaderProgram();
	void InitializeShader();
	void InitializeTextures();
	void BindShaderProgram();
	void UnbindShaderProgram();
	void BindUniforms();
	void DrawOverlayPlane();
	void ComputeTextureOffsets();
	void ReleaseTextures();
	void DeleteShader();

public:
	RAS_2DFilter(RAS_2DFilterData& data, RAS_2DFilterManager *manager);
	~RAS_2DFilter();

	/// Called by the filter manager when it has informations like the display size, a gl context...
	void Initialize();

	/// Starts executing the filter.
	void Start();

	/// Finalizes the execution stage of the filter.
	void End();

	/// The pass index determines the precedence of this filter over other filters in the same context.
	int GetPassIndex();

	/// Enables / disables this filter. A disabled filter has no effect on the rendering.
	void SetEnabled(bool enabled);
};

#endif // __RAS_2DFILTER_H__

